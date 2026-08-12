"""Happy-path render test with stubbed AWS clients (no network, no creds).

Patches the `iot`/`s3` names inside each core module (they hold direct
references from `from core.aws import ...`), then drives the real app through
AppTest so every widget, the job-document preview, the pre-flight verdicts, and
the tracking fragment are exercised against realistic API shapes.
"""
from __future__ import annotations

import time
from datetime import datetime, timedelta, timezone

from _bootstrap import APP, check, finish, require_secrets, section  # noqa: E402

require_secrets()

import core.aws as aws  # noqa: E402
import core.firmware as fwmod  # noqa: E402
import core.fleet as flmod  # noqa: E402
import core.jobs as jobsmod  # noqa: E402
import core.settings as settingsmod  # noqa: E402
import core.wss as wssmod  # noqa: E402
from streamlit.testing.v1 import AppTest  # noqa: E402

NOW = datetime(2026, 8, 6, 12, 0, tzinfo=timezone.utc)
THINGS = [
    {"thingName": f"BAT-{i:03d}", "connectivity": {"connected": i % 3 != 0},
     "thingTypeName": "BatteryPack", "attributes": {"fw_version": "1.0.1"}}
    for i in range(1, 13)
]
EXEC_STATUSES = (["SUCCEEDED"] * 5 + ["IN_PROGRESS"] * 2 + ["QUEUED"] * 3
                 + ["FAILED"] * 2)
created: list[dict] = []


class Paginator:
    def __init__(self, pages): self._pages = pages
    def paginate(self, **kw): return iter(self._pages)


class FakeIot:
    def search_index(self, queryString, maxResults=250, nextToken=None):
        # Exercise the hand-rolled nextToken loop with two real pages.
        if nextToken is None:
            return {"things": THINGS[:8], "nextToken": "page2"}
        return {"things": THINGS[8:]}

    def list_jobs(self, maxResults=25):
        return {"jobs": [
            {"jobId": "as-ota-1-0-3-20260806-090000", "status": "IN_PROGRESS"},
            {"jobId": "as-ota-1-0-2-20260805-113000", "status": "COMPLETED"},
        ]}

    def describe_job(self, jobId):
        return {"job": {"jobId": jobId, "status": "IN_PROGRESS"}}

    def get_paginator(self, name):
        if name == "list_job_executions_for_job":
            return Paginator([{ "executionSummaries": [
                {"thingArn": f"arn:aws:iot:ap-south-1:123456789012:thing/BAT-{i + 1:03d}",
                 "jobExecutionSummary": {
                     "status": EXEC_STATUSES[i],
                     "queuedAt": NOW, "startedAt": NOW + timedelta(minutes=1),
                     "lastUpdatedAt": NOW + timedelta(minutes=4),
                     "retryAttempt": 1 if EXEC_STATUSES[i] == "FAILED" else 0,
                 }} for i in range(len(EXEC_STATUSES))]}])
        raise AssertionError(f"unexpected paginator {name}")

    def describe_job_execution(self, jobId, thingName):
        return {"execution": {"statusDetails": {
            "detailsMap": {"reason": "weak_signal"}}}}

    def create_job(self, **kw):
        created.append(kw)
        return {"jobId": kw["jobId"],
                "jobArn": f"arn:aws:iot:ap-south-1:123456789012:job/{kw['jobId']}"}

    def cancel_job(self, **kw):
        return {}


class FakeS3:
    def get_paginator(self, name):
        return Paginator([{"Contents": [
            {"Key": "firmware/1.0.3/firmware.bin", "Size": 1103456,
             "LastModified": NOW, "ETag": '"abc123"'},
            {"Key": "firmware/1.0.2/firmware.bin", "Size": 1098000,
             "LastModified": NOW - timedelta(days=1), "ETag": '"def456"'},
        ]}])

    def head_object(self, Bucket, Key):
        return {"ContentLength": 1103456, "LastModified": NOW}


class FakeCreds:
    access_key = "AKIAIOSFODNN7EXAMPLE"
    secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
    token = None


_iot, _s3 = FakeIot(), FakeS3()
for mod in (aws, flmod, jobsmod, fwmod):
    if hasattr(mod, "iot"):
        mod.iot = lambda: _iot
    if hasattr(mod, "s3"):
        mod.s3 = lambda: _s3
aws.identity = lambda: {"account": "123456789012", "arn": "arn:aws:iam::123456789012:user/ota-dash"}
aws.frozen_credentials = lambda: FakeCreds()
wssmod.frozen_credentials = lambda: FakeCreds()
jobsmod._account_id = lambda: "123456789012"

ARTIFACT = {
    "key": "firmware/1.0.3/firmware.bin", "version": "1.0.3", "size": 1_103_456,
    "sha256": "9f2c1ab34de5f607aa11bb22cc33dd44ee55ff66778899aabbccddeeff001122",
    "embedded": "1.0.3", "etag": "abc123", "published": time.time(),
}


def app(page, **state):
    at = AppTest.from_file(APP, default_timeout=60)
    at.session_state["auth_role"] = "operator"
    for k, v in state.items():
        at.session_state[k] = v
    at.run()
    at.sidebar.radio[0].set_value(page).run()
    return at


def text_of(at):
    parts = [str(m.value) for m in at.markdown]
    parts += [str(c.value) for c in at.get("code")]
    parts += [str(c.value) for c in at.caption]
    return "\n".join(parts)


section("Firmware Deploy (12 nodes, 8 selected)")
sel = [f"BAT-{i:03d}" for i in range(1, 9)]
at = app("Firmware Deploy", artifact=ARTIFACT, sel_nodes=sel)
check("no exception", not at.exception,
      at.exception[0].value.splitlines()[0] if at.exception else "")
body = text_of(at)
check("nextToken loop returned all 12 things", "12" in body, "fleet size card")
check("online/unknown counted", "Online" in body and "Unknown" in body)
check("job document rendered", "firmwareUrl" in body and "minCsq" in body)
bars = list(at.get("progress"))
check("URC budget meter rendered", len(bars) == 1 and 0 < bars[0].value <= 100,
      f"{[b.value for b in bars]}")
check("URC budget explained to operator",
      "1536 B URC buffer" in body and "firmwareUrl` is emitted first" in body)
check("pre-flight cleared", any("cleared pre-flight" in str(s.value) for s in at.success),
      str([s.value for s in at.success])[:120])
check("gate sliders present", len(at.slider) >= 5, f"{len(at.slider)} sliders")
check("review & deploy enabled",
      any("Review & deploy" in str(b.label) and not b.disabled for b in at.button))
check("no blocking errors", not at.error, str([e.value[:80] for e in at.error]))

section("conditional gates flow into the document")
csq = next(s for s in at.slider if "CSQ" in str(s.label))
at2 = csq.set_value(27).run()
doc = "\n".join(str(c.value) for c in at2.get("code"))
check("minCsq 27 injected", '"minCsq":27' in doc, doc[:150])
kbps = next(s for s in at2.slider if "downlink" in str(s.label))
at3 = kbps.set_value(200).run()
doc3 = "\n".join(str(c.value) for c in at3.get("code"))
check("minKbps 200 injected", '"minKbps":200' in doc3)
check("firmwareUrl still first key", doc3.index("firmwareUrl") < doc3.index("minCsq"))

section("scheduling widgets")
at4 = next(r for r in at3.radio if "Start" in str(r.label)).set_value("Scheduled window").run()
check("no exception on schedule mode", not at4.exception,
      at4.exception[0].value.splitlines()[0] if at4.exception else "")
check("date picker rendered", len(at4.date_input) >= 1)
check("time picker rendered", len(at4.time_input) >= 1)
check("timezone selector rendered",
      any("Timezone" in str(s.label) for s in at4.selectbox))
sched_ok = any("Rollout begins" in str(s.value) for s in at4.success)
sched_err = any("30 minutes" in str(e.value) for e in at4.error)
check("schedule validated (accepted or 30-min rule enforced)", sched_ok or sched_err,
      f"ok={sched_ok} lead_rule={sched_err}")

section("CreateJob payload shape")
created.clear()
plan = jobsmod.RolloutPlan(max_per_minute=5, timeout_minutes=30,
                           start_utc=NOW + timedelta(hours=3))
res = jobsmod.create("as-ota-1-0-3-test", ("BAT-001", "BAT-002"), '{"a":1}',
                     "desc", plan)
kw = created[0]
check("job id passed", res["jobId"] == "as-ota-1-0-3-test")
check("targets are thing ARNs",
      all(t.startswith("arn:aws:iot:ap-south-1:123456789012:thing/") for t in kw["targets"]),
      str(kw["targets"]))
check("rollout rate limited", kw["jobExecutionsRolloutConfig"]["maximumPerMinute"] == 5)
check("exponential ramp configured",
      "exponentialRate" in kw["jobExecutionsRolloutConfig"])
check("abort config on FAILED and TIMED_OUT",
      {c["failureType"] for c in kw["abortConfig"]["criteriaList"]} == {"FAILED", "TIMED_OUT"})
check("timeout config set", kw["timeoutConfig"]["inProgressTimeoutInMinutes"] == 30)
check("schedulingConfig ISO-8601 UTC",
      kw["schedulingConfig"]["startTime"] == "2026-08-06T15:00:00Z",
      kw["schedulingConfig"]["startTime"])
check("no endTime when unset", "endTime" not in kw["schedulingConfig"])

created.clear()
jobsmod.create("j2", ("BAT-001",), "{}", "d",
               jobsmod.RolloutPlan(3, 20, start_utc=NOW + timedelta(hours=3),
                                   end_utc=NOW + timedelta(hours=9)))
check("endTime + STOP_ROLLOUT when window closes",
      created[0]["schedulingConfig"]["endTime"] == "2026-08-06T21:00:00Z"
      and created[0]["schedulingConfig"]["endBehavior"] == "STOP_ROLLOUT")

created.clear()
jobsmod.create("j3", ("BAT-001",), "{}", "d", jobsmod.RolloutPlan(3, 20))
check("no schedulingConfig for immediate start",
      "schedulingConfig" not in created[0])

section("Job Tracking")
at5 = app("Job Tracking", tracked_job="as-ota-1-0-3-20260806-090000")
check("no exception", not at5.exception,
      at5.exception[0].value.splitlines()[0] if at5.exception else "")
tbody = text_of(at5)
check("status counts rendered", "SUCCEEDED" in tbody and "QUEUED" in tbody)
check("progress bar present", len(list(at5.get("progress"))) >= 1)
check("execution dataframe rendered", len(at5.dataframe) >= 1)
if at5.dataframe:
    df = at5.dataframe[0].value
    check("all 12 executions listed", len(df) == 12, str(len(df)))
    check("failures sorted to the top", df.iloc[0]["Status"] == "FAILED",
          str(df["Status"].tolist()[:5]))
    check("succeeded sorted last", df.iloc[-1]["Status"] == "SUCCEEDED",
          str(df["Status"].tolist()[-3:]))
    check("retry column populated", "Retries" in df.columns)
snap = jobsmod.poll("as-ota-1-0-3-20260806-090000", 0)
check("aggregate counts correct",
      (snap.succeeded, snap.failed, snap.total) == (5, 2, 12),
      f"{snap.succeeded}/{snap.failed}/{snap.total}")
check("progress = terminal/total", abs(snap.progress - 7 / 12) < 1e-9, str(snap.progress))
check("api calls bounded (1 describe + 1 page)", snap.api_calls == 2, str(snap.api_calls))

section("Live Telemetry")
at6 = app("Live Telemetry", sel_nodes=["BAT-001", "BAT-002"])
check("no exception", not at6.exception,
      at6.exception[0].value.splitlines()[0] if at6.exception else "")
check("no error shown", not at6.error, str([e.value[:90] for e in at6.error]))
url = wssmod.signed_url()
# Assert STRUCTURALLY against the configured endpoint. This previously hardcoded
# "smoketest-ats.iot.ap-south-1", the endpoint of a throwaway secrets.toml used
# during development, so it broke the moment real secrets were installed. The
# test must verify the signing, not the operator's account.
_cfg = settingsmod.settings()
check("signed wss url minted",
      url.startswith(f"wss://{_cfg.endpoint}/mqtt?") and
      "X-Amz-Algorithm=AWS4-HMAC-SHA256" in url and
      "X-Amz-Signature=" in url,
      url[:90])
check("IAM hint mentions scoped topicfilter",
      "topicfilter/bms/data" in text_of(at6))

finish()
