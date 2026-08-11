"""AWS IoT Jobs: creation, precision scheduling, and batched status polling.

Polling cost is the thing that matters here. The naive design calls
DescribeJobExecution once per device per refresh tick -- O(devices) API calls
every 2 seconds, which throttles at fleet scale and burns latency.

This module uses ListJobExecutionsForJob: ONE paginated call returns every
execution for the job, and we aggregate in a single pass:
    O(ceil(n/250)) API calls, O(n) local work, per refresh tick.
Results are memoised for `poll_ttl_seconds`, so a fragment refreshing every
2 s against a 10 s TTL makes ~1 call per 10 s regardless of tick rate.
"""
from __future__ import annotations

import re
import time
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone

import streamlit as st

from core.aws import AWS_ERRORS, explain, iot
from core.settings import settings

TERMINAL = frozenset({"SUCCEEDED", "FAILED", "REJECTED", "REMOVED",
                      "CANCELED", "TIMED_OUT"})
ACTIVE = frozenset({"QUEUED", "IN_PROGRESS"})
# Lifecycle order -- used for the status-count pills, which read as a pipeline.
ORDER = ("QUEUED", "IN_PROGRESS", "SUCCEEDED", "FAILED", "TIMED_OUT",
         "REJECTED", "CANCELED", "REMOVED")
# Triage order -- used to sort the execution table. Failures first: the operator
# needs to see what broke without scrolling past a page of SUCCEEDED rows.
TABLE_ORDER = ("FAILED", "TIMED_OUT", "REJECTED", "IN_PROGRESS", "QUEUED",
               "SUCCEEDED", "CANCELED", "REMOVED")

_SAFE_ID = re.compile(r"[^A-Za-z0-9_-]")
# AWS requires a scheduled job's startTime to be at least this far out.
MIN_SCHEDULE_LEAD = timedelta(minutes=30)


# ------------------------------------------------------------------ job id

def make_job_id(prefix: str, version: str, now: datetime | None = None) -> str:
    stamp = (now or datetime.now(timezone.utc)).strftime("%Y%m%d-%H%M%S")
    body = _SAFE_ID.sub("-", f"{prefix}-{version}-{stamp}")
    return body[:64]


# ------------------------------------------------------------------ create

@dataclass(frozen=True, slots=True)
class RolloutPlan:
    max_per_minute: int
    timeout_minutes: int
    abort_failure_pct: float = 25.0
    abort_min_executed: int = 5
    target_selection: str = "SNAPSHOT"     # SNAPSHOT | CONTINUOUS
    start_utc: datetime | None = None      # None => start immediately
    end_utc: datetime | None = None


def validate_schedule(start_utc: datetime | None,
                      end_utc: datetime | None) -> tuple[str, ...]:
    errs: list[str] = []
    if start_utc is None:
        return ()
    now = datetime.now(timezone.utc)
    if start_utc <= now:
        errs.append("Scheduled start is in the past.")
    elif start_utc - now < MIN_SCHEDULE_LEAD:
        errs.append(
            "AWS IoT requires a scheduled start at least 30 minutes ahead. "
            f"Chosen start is {int((start_utc - now).total_seconds() // 60)} min out."
        )
    if end_utc is not None and end_utc <= start_utc:
        errs.append("Maintenance window end must be after its start.")
    return tuple(errs)


def _thing_arns(names: tuple[str, ...]) -> list[str]:
    cfg = settings()
    account = st.session_state.get("_aws_account") or _account_id()
    return [f"arn:aws:iot:{cfg.region}:{account}:thing/{n}" for n in names]


def _account_id() -> str:
    from core.aws import identity
    acct = identity()["account"]
    st.session_state["_aws_account"] = acct
    return acct


def create(job_id: str, targets: tuple[str, ...], document: str,
           description: str, plan: RolloutPlan) -> dict:
    """CreateJob with rate-limited rollout, abort criteria, and optional
    scheduling. Every safety knob is populated -- there is no 'blast' path."""
    kwargs: dict = {
        "jobId": job_id,
        "targets": _thing_arns(targets),
        "document": document,
        "description": description[:2028],
        "targetSelection": plan.target_selection,
        "jobExecutionsRolloutConfig": {
            "maximumPerMinute": int(plan.max_per_minute),
            "exponentialRate": {
                "baseRatePerMinute": max(1, min(int(plan.max_per_minute), 5)),
                "incrementFactor": 2.0,
                "rateIncreaseCriteria": {"numberOfSucceededThings": 5},
            },
        },
        "abortConfig": {
            "criteriaList": [
                {
                    "failureType": "FAILED",
                    "action": "CANCEL",
                    "thresholdPercentage": float(plan.abort_failure_pct),
                    "minNumberOfExecutedThings": int(plan.abort_min_executed),
                },
                {
                    "failureType": "TIMED_OUT",
                    "action": "CANCEL",
                    "thresholdPercentage": float(plan.abort_failure_pct),
                    "minNumberOfExecutedThings": int(plan.abort_min_executed),
                },
            ]
        },
        "timeoutConfig": {"inProgressTimeoutInMinutes": int(plan.timeout_minutes)},
    }

    """PRESIGNED URL EXPANSION.

    ${aws:iot:s3-presigned-url:...} is substituted by AWS IoT only when
    presignedUrlConfig.roleArn is supplied; IoT assumes that role to sign the S3
    object. Omit it and the device receives the placeholder VERBATIM, its
    strstr(urcLine, "https://") latches onto the unsigned URL inside the braces,
    and AT+QHTTPGET returns HTTP 403 -- reported as `get_len` on the console.

    Refuse rather than ship a job that cannot possibly succeed: a failed
    CreateJob costs nothing, whereas a queued-but-doomed job consumes a device
    execution attempt and, with OTA_REPORT_JOB_STATUS=1, can leave a
    non-terminal execution that blocks every later job.
    """
    cfg = settings()
    if "${aws:iot:s3-presigned-url:" in document:
        if not cfg.presign_role_arn:
            raise ValueError(
                "The job document uses a pre-signed URL placeholder but "
                "[iot].presign_role_arn is not configured. AWS IoT would deliver "
                "the placeholder unexpanded and every device would fail with "
                "HTTP 403. Set it to the ARN of the role IoT assumes to sign S3 "
                "URLs (see iam/iot-presign-role.json)."
            )
        kwargs["presignedUrlConfig"] = {
            "roleArn": cfg.presign_role_arn,
            # 60-3600 s. Signed when the device FETCHES the document, not at
            # CreateJob, so this only has to outlast one download.
            "expiresInSec": max(60, min(3600, int(cfg.presign_expires_sec))),
        }

    if plan.start_utc is not None:
        sched: dict = {"startTime": plan.start_utc.strftime("%Y-%m-%dT%H:%M:%SZ")}
        if plan.end_utc is not None:
            sched["endTime"] = plan.end_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
            sched["endBehavior"] = "STOP_ROLLOUT"
        kwargs["schedulingConfig"] = sched

    resp = iot().create_job(**kwargs)
    poll.clear()
    recent.clear()
    return {"jobId": resp.get("jobId", job_id), "jobArn": resp.get("jobArn", "")}


def cancel(job_id: str, comment: str = "Cancelled from dashboard") -> None:
    iot().cancel_job(jobId=job_id, comment=comment[:2028], force=False)
    poll.clear()
    recent.clear()


# ------------------------------------------------------------------- poll

@dataclass(frozen=True, slots=True)
class Execution:
    thing: str
    status: str
    queued_at: datetime | None
    started_at: datetime | None
    updated_at: datetime | None
    retry_count: int


@dataclass(slots=True)
class JobSnapshot:
    job_id: str
    status: str
    counts: Counter = field(default_factory=Counter)
    executions: tuple[Execution, ...] = ()
    api_calls: int = 0
    fetched_at: float = 0.0
    error: str = ""

    @property
    def total(self) -> int:
        return sum(self.counts.values())

    @property
    def done(self) -> int:
        return sum(v for k, v in self.counts.items() if k in TERMINAL)

    @property
    def progress(self) -> float:
        return (self.done / self.total) if self.total else 0.0

    @property
    def succeeded(self) -> int:
        return self.counts.get("SUCCEEDED", 0)

    @property
    def failed(self) -> int:
        return (self.counts.get("FAILED", 0) + self.counts.get("TIMED_OUT", 0)
                + self.counts.get("REJECTED", 0))


@st.cache_data(show_spinner=False, ttl=300, max_entries=64)
def poll(job_id: str, _ttl_bucket: int) -> JobSnapshot:
    """One paginated ListJobExecutionsForJob + one DescribeJob. Single pass.

    `ttl`/`max_entries` are backstops, not the freshness mechanism -- that is the
    `_ttl_bucket` argument. They exist because the cache key includes job_id, so
    tracking many jobs over a long session would otherwise grow the cache without
    bound.
    """
    client = iot()
    snap = JobSnapshot(job_id=job_id, status="UNKNOWN", fetched_at=time.time())
    try:
        snap.status = client.describe_job(jobId=job_id)["job"].get("status", "UNKNOWN")
        snap.api_calls = 1

        rows: list[Execution] = []
        counts: Counter = Counter()
        pages = client.get_paginator("list_job_executions_for_job").paginate(
            jobId=job_id, PaginationConfig={"PageSize": 250}
        )
        for page in pages:
            snap.api_calls += 1
            for ex in page.get("executionSummaries", []):
                s = ex.get("jobExecutionSummary", {})
                status = s.get("status", "UNKNOWN")
                counts[status] += 1
                rows.append(
                    Execution(
                        thing=ex.get("thingArn", "").rsplit("/", 1)[-1],
                        status=status,
                        queued_at=s.get("queuedAt"),
                        started_at=s.get("startedAt"),
                        updated_at=s.get("lastUpdatedAt"),
                        retry_count=int(s.get("retryAttempt", 0) or 0),
                    )
                )
        snap.counts = counts
        snap.executions = tuple(rows)
    except AWS_ERRORS as exc:
        snap.error = explain(exc)
    return snap


def snapshot(job_id: str) -> JobSnapshot:
    """Freshness comes from the bucket argument; failures are never retained.

    poll() returns its error INSIDE the snapshot, so without this a transient
    AccessDeniedException is memoised exactly like a success. Because fixing the
    IAM grant does not change the bucket, the operator then keeps seeing a denial
    that no longer applies -- which is precisely what happened with iot:ListJobs.
    Dropping the entry means the next rerun re-asks AWS instead of replaying it.
    """
    ttl = max(settings().poll_ttl_seconds, 1)
    snap = poll(job_id, int(time.time() // ttl))
    if snap.error:
        poll.clear()
    return snap


@st.cache_data(show_spinner=False, ttl=300, max_entries=8)
def recent(_ttl_bucket: int, limit: int = 25) -> tuple[tuple[dict, ...], str]:
    """Recent jobs, newest first. One call, no per-job describes.
    Returns (jobs, error) so the caller can distinguish 'none exist' from
    'could not ask'."""
    try:
        resp = iot().list_jobs(maxResults=min(limit, 250))
        return tuple(resp.get("jobs", [])), ""
    except AWS_ERRORS as exc:
        return (), explain(exc)


def recent_jobs(limit: int = 25) -> tuple[tuple[dict, ...], str]:
    """See snapshot() -- same reason for not retaining a failed lookup."""
    jobs_, err = recent(int(time.time() // 30), limit)
    if err:
        recent.clear()
    return jobs_, err


def failure_reasons(job_id: str, things: tuple[str, ...],
                    cap: int = 8) -> dict[str, str]:
    """statusDetails are only in DescribeJobExecution, so this is deliberately
    called on demand for a capped set of FAILED devices -- never in the poll
    loop. Reasons come from _otaFail() in EC200U_AWS_OTA.h."""
    client = iot()
    out: dict[str, str] = {}
    for name in things[:cap]:
        try:
            ex = client.describe_job_execution(jobId=job_id, thingName=name)
            details = ex.get("execution", {}).get("statusDetails", {}) or {}
            detail_map = details.get("detailsMap", details) or {}
            out[name] = detail_map.get("reason") or detail_map.get("fw_version") or "-"
        except AWS_ERRORS as exc:
            out[name] = explain(exc)
    return out
