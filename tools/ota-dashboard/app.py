"""AS AI — Fleet OTA & Diagnostics Dashboard
=============================================
ESP32 + Quectel EC200U-CN  ·  AWS IoT Core / Jobs  ·  ap-south-1

Run from tools/ota-dashboard with the LOCAL venv:
    .\\.venv\\Scripts\\streamlit.exe run app.py        (PowerShell)
    ./.venv/bin/streamlit run app.py                  (bash)

Design notes
------------
* Every AWS read is memoised (cache_data + secrets-driven TTL); every AWS
  client is memoised for the session (cache_resource). A rerun costs 0 calls.
* Job polling is batched: 1 + ceil(n/250) calls per TTL window for the whole
  fleet, not one call per device per tick.
* Auto-refresh is scoped to the tracking pane with st.fragment(run_every=),
  so upload widgets, fleet selection, and the telemetry socket are untouched.
* Live telemetry never enters Python: the browser holds the MQTT/WSS socket.
"""
from __future__ import annotations

import re
import time
import uuid
from datetime import datetime, time as dtime, timedelta, timezone
from zoneinfo import ZoneInfo

import pandas as pd
import streamlit as st

from components import map_console as mc
from components import telemetry as tw
from components.ui import header, inject_css, kv, pill, stat, stat_row
from core import firmware as fw
from core import fleet as fl
from core import jobdoc, jobs, wss
from core.auth import is_operator, logout, require_login, role
from core.aws import identity
from core.settings import settings

st.set_page_config(
    page_title="AS AI · Fleet OTA Console",
    page_icon="⚡",
    layout="wide",
    initial_sidebar_state="expanded",
)
inject_css()
require_login()

CFG = settings()
TZ_CHOICES = ("Asia/Kolkata", "UTC")


def fleet_or_stop() -> fl.Fleet:
    """Fleet discovery is the first AWS call every page makes, so it is also
    where a credential/region/permission misconfiguration surfaces. Render it
    as an actionable message instead of a traceback."""
    f = fl.discover()
    if f.error:
        st.error(f.error, icon="⛔")
        st.caption("Fix the credentials in `.streamlit/secrets.toml`, then use "
                   "**Refresh** in the sidebar.")
        st.stop()
    return f


# ============================================================== sidebar

def sidebar() -> str:
    with st.sidebar:
        st.markdown(
            '<div style="display:flex;align-items:center;gap:.6rem;margin-bottom:1rem">'
            '<div class="gate-mark" style="width:34px;height:34px;font-size:13px;'
            'border-radius:9px;margin:0">AS</div>'
            '<div><div style="font-weight:650;font-size:.95rem">Fleet OTA Console</div>'
            '<div style="color:#8b98ab;font-size:.68rem;letter-spacing:.06em">'
            'ESP32 · EC200U-CN</div></div></div>',
            unsafe_allow_html=True,
        )

        page = st.radio(
            "Section",
            ("Firmware Registry", "Fleet & Deploy", "Job Tracking", "Live Telemetry"),
            label_visibility="collapsed",
        )

        st.divider()
        ident = identity()
        r = role() or "-"
        st.markdown(
            f'<div style="font-size:.72rem;line-height:1.75;color:#8b98ab">'
            f'ROLE <span style="color:#00e0a4">{r.upper()}</span><br>'
            f'REGION <span style="color:#e4e9f0">{CFG.region}</span><br>'
            f'ACCOUNT <span style="color:#e4e9f0">{ident["account"]}</span><br>'
            f'BUCKET <span style="color:#e4e9f0">{CFG.bucket or "unset"}</span></div>',
            unsafe_allow_html=True,
        )
        if not is_operator():
            st.caption("Read-only role. Firmware upload and CreateJob are disabled.")

        st.divider()
        c1, c2 = st.columns(2)
        if c1.button("Refresh", use_container_width=True):
            st.cache_data.clear()
            st.rerun()
        if c2.button("Sign out", use_container_width=True):
            logout()

        if not CFG.configured:
            st.error("Incomplete secrets: set [aws].region, [iot].endpoint, [s3].bucket.")
    return page


# ==================================================== 1 · firmware registry

def page_firmware() -> None:
    header(
        "Firmware Registry",
        "Local validation against partitions_ota.csv, then a checksum-verified "
        "PutObject to S3. Nothing reaches a device until it passes here.",
        right=f"s3://{CFG.bucket}/{CFG.firmware_prefix}",
    )

    staged = st.session_state.get("artifact")
    if staged:
        stat_row([
            stat("Staged version", staged["version"], tone="ok"),
            stat("Size", f"{staged['size'] / 1024:.1f} KB",
                 f"{staged['size'] / CFG.app_slot_bytes:.0%} of app slot"),
            stat("SHA-256", staged["sha256"][:12] + "…"),
            stat("S3 key", staged["key"].rsplit("/", 2)[-2] + "/" + staged["key"].rsplit("/", 1)[-1]),
        ])

    tab_new, tab_existing = st.tabs(["Upload new image", "Use published artefact"])

    # ------------------------------------------------------------ upload
    with tab_new:
        up = st.file_uploader(
            "ESP32 application image (.bin)", type=["bin"],
            help="Produced by: pio run -e prod  →  .pio/build/prod/firmware.bin",
            disabled=not is_operator(),
        )
        if up is None:
            st.info("Select a `.bin`. It is parsed in-browser-session before any "
                    "upload: image magic, esp_app_desc_t version, and app-slot fit.")
            return

        data = up.getvalue()
        meta = fw.inspect(up.name, data, CFG.app_slot_bytes)

        left, right = st.columns([1.15, 1])
        with left:
            kv([
                ("File", meta.filename),
                ("Size", f"{meta.size:,} B  ({meta.size_kb:.1f} KB)"),
                ("App-slot use", f"{meta.size / CFG.app_slot_bytes:.1%} of "
                                 f"{CFG.app_slot_bytes:,} B"),
                ("Image magic", "0xE9 OK" if meta.image_magic_ok else "INVALID"),
                ("Embedded version", meta.version or "—"),
                ("Project", meta.project_name or "—"),
                ("IDF / core", meta.idf_version or "—"),
                ("Built", meta.built or "—"),
                ("SHA-256", meta.sha256),
            ])
        with right:
            for e in meta.errors:
                st.error(e, icon="⛔")
            for w in meta.warnings:
                st.warning(w, icon="⚠️")
            if meta.deployable and not meta.warnings:
                st.success("Image validated. Safe to publish.", icon="✅")

            version = st.text_input(
                "Release version", value=meta.version or "",
                help="Used for the S3 key, the job document `version` field, and "
                     "the job id.",
                disabled=not is_operator(),
            ).strip()

            if meta.version and version and version != meta.version:
                st.warning(
                    f"Declared `{version}` ≠ embedded `{meta.version}`. The device "
                    "reports its own FW_VERSION on SUCCEEDED, so this mismatch "
                    "will show up in job status details.",
                    icon="⚠️",
                )

            key = fw.s3_key(CFG.firmware_prefix, version or "unversioned")
            st.caption(f"Target key → `{key}`")

            existing = fw.head(CFG.bucket, key) if (version and CFG.bucket) else None
            if existing:
                st.warning(
                    f"Key already exists ({existing['ContentLength']:,} B, "
                    f"{existing['LastModified']:%Y-%m-%d %H:%M} UTC). Publishing "
                    "overwrites it — devices pinned to this URL get the new bytes.",
                    icon="⚠️",
                )

            can = bool(meta.deployable and version and CFG.bucket and is_operator())
            if st.button("Publish to S3", type="primary", disabled=not can,
                         use_container_width=True):
                with st.spinner("Uploading…"):
                    try:
                        res = fw.upload(CFG.bucket, key, data, meta, version)
                    except Exception as exc:  # noqa: BLE001
                        st.error(f"Upload failed: {exc}")
                        return
                st.session_state["artifact"] = {
                    "key": key, "version": version, "size": meta.size,
                    "sha256": meta.sha256, "embedded": meta.version,
                    "etag": res["etag"], "published": time.time(),
                }
                st.success(f"Published. ETag `{res['etag']}`", icon="✅")
                st.rerun()

    # -------------------------------------------------- existing artefact
    with tab_existing:
        objs = fw.list_artifacts(CFG.bucket, CFG.firmware_prefix) if CFG.bucket else ()
        if not objs:
            st.info("No `.bin` objects found under the configured prefix.")
            return
        labels = {
            f"{o['Key']}  ·  {o['Size'] / 1024:.0f} KB  ·  "
            f"{o['LastModified']:%Y-%m-%d %H:%M} UTC": o
            for o in objs
        }
        chosen = st.selectbox("Published artefacts", tuple(labels))
        obj = labels[chosen]
        # Key layout is firmwares/firmware_v1_0_5.bin -- recover "1.0.5" from
        # the filename, since the version no longer has its own folder.
        m = re.search(r"firmware_v?([0-9]+(?:[._][0-9]+)*)\.bin$", obj["Key"])
        guessed = m.group(1).replace("_", ".") if m else ""
        version = st.text_input("Version for the job document", value=guessed).strip()
        if st.button("Stage this artefact", disabled=not (version and is_operator())):
            st.session_state["artifact"] = {
                "key": obj["Key"], "version": version, "size": obj["Size"],
                "sha256": "", "embedded": "", "etag": obj.get("ETag", "").strip('"'),
                "published": obj["LastModified"].timestamp(),
            }
            st.success("Staged.", icon="✅")
            st.rerun()


# ======================================================= 2 · fleet & deploy

@st.dialog("Confirm OTA rollout")
def _confirm(job_id: str, targets: tuple[str, ...], art: dict,
             gate: jobdoc.NetworkGate, plan: jobs.RolloutPlan, document: str) -> None:
    st.markdown(f"**{len(targets)}** device(s) will receive firmware "
                f"**{art['version']}** ({art['size'] / 1024:.1f} KB).")
    when = ("immediately on next device connect" if plan.start_utc is None
            else f"from {plan.start_utc:%Y-%m-%d %H:%M} UTC")
    kv([
        ("Job id", job_id),
        ("Rollout", f"{plan.max_per_minute}/min, exponential, starts {when}"),
        ("Abort if", f"≥{plan.abort_failure_pct:.0f}% of ≥{plan.abort_min_executed} "
                     "executions fail"),
        ("Exec timeout", f"{plan.timeout_minutes} min"),
        ("Network gate", f"CSQ ≥ {gate.min_csq}, ≥ {gate.min_kbps} kbps, "
                         f"retries ≤ {gate.max_retries}"),
        ("Targets", ", ".join(targets[:6]) + (" …" if len(targets) > 6 else "")),
    ])
    st.code(document, language="json")
    st.warning("These are live battery packs with MOSFET control. A failed flash "
               "leaves the device on its current bank, but a *bad image that "
               "boots* does not.", icon="⚠️")

    c1, c2 = st.columns(2)
    if c1.button("Cancel", use_container_width=True):
        st.rerun()
    if c2.button("Create job", type="primary", use_container_width=True):
        try:
            res = jobs.create(job_id, targets, document,
                              # ASCII only: this string is stored on the AWS job
                              # and printed by `aws iot describe-job`, which dies
                              # with a charmap UnicodeEncodeError on a cp1252
                              # Windows console if it contains e.g. an en-dash.
                              f"OTA {art['version']} to {len(targets)} node(s)", plan)
        except Exception as exc:  # noqa: BLE001
            st.error(f"CreateJob rejected: {exc}")
            return

        # Archive AFTER the job exists, never before: a document sitting in
        # jobs/ that no job ever used is misleading during an audit.
        try:
            fw.archive_job_doc(
                CFG.bucket,
                fw.job_doc_key(CFG.jobs_prefix, art["version"]),
                document, res["jobId"],
            )
        except Exception as exc:  # noqa: BLE001
            # The job is already live; a failed archive must not read as failure.
            st.warning(f"Job created, but archiving the document to "
                       f"s3://{CFG.bucket}/{CFG.jobs_prefix} failed: {exc}", icon="📄")

        st.session_state["tracked_job"] = res["jobId"]
        st.session_state["just_created"] = res["jobId"]
        st.rerun()


def page_deploy() -> None:
    art = st.session_state.get("artifact")
    header(
        "Fleet Targeting & Rollout",
        "Conditional gates are injected into the job document; the device "
        "evaluates them before it touches flash.",
        right=(f"artifact {art['version']}" if art else "no artefact staged"),
    )

    if not art:
        st.info("Publish or stage a firmware artefact in **Firmware Registry** first.",
                icon="📦")
        return

    fleet = fleet_or_stop()
    if fleet.source == "registry":
        st.caption(f"⚠️ {fleet.note}")
    if not fleet.nodes:
        st.warning("No things found in this account/region. Provision a device "
                   "first (scripts/provision_device.py), or scope "
                   "`[iot].thing_group` / `thing_type` correctly.", icon="📡")
        return

    stat_row([
        stat("Fleet size", str(len(fleet.nodes)), fleet.source, tone="info"),
        stat("Online", str(fleet.online), "fleet index", tone="ok"),
        stat("Unknown", str(fleet.unknown), "no connectivity data",
             tone="warn" if fleet.unknown else ""),
        stat("Artefact", art["version"], f"{art['size'] / 1024:.0f} KB"),
    ])

    # ------------------------------------------------------- target picker
    st.markdown("##### Target selection")
    c1, c2 = st.columns([3, 1])
    only_online = c2.toggle("Online only", value=False,
                            help="Offline nodes still receive the job on their "
                                 "next connect; filtering is a convenience, "
                                 "not a requirement.")
    pool = tuple(n.name for n in fleet.nodes
                 if not only_online or n.connected is not False)
    selected = tuple(c1.multiselect(
        f"Nodes ({len(pool)} available)", pool,
        default=st.session_state.get("sel_nodes", ()),
        placeholder="Select target nodes…",
    ))
    st.session_state["sel_nodes"] = list(selected)

    b1, b2, b3 = st.columns(3)
    if b1.button("Select all", use_container_width=True):
        st.session_state["sel_nodes"] = list(pool)
        st.rerun()
    if b2.button(f"Canary (first {min(3, len(pool))})", use_container_width=True):
        st.session_state["sel_nodes"] = list(pool[:3])
        st.rerun()
    if b3.button("Clear", use_container_width=True):
        st.session_state["sel_nodes"] = []
        st.rerun()

    if selected:
        idx = fleet.index()
        st.markdown(
            " ".join(
                f'<span class="pill pill-{"ok" if idx[n].connected else ("wait" if idx[n].connected is None else "mute")}">{n}</span>'
                for n in selected[:24] if n in idx
            ) + (f' <span class="pill pill-mute">+{len(selected) - 24}</span>'
                 if len(selected) > 24 else ""),
            unsafe_allow_html=True,
        )

    st.divider()

    # ------------------------------------------- conditional OTA + rollout
    gcol, scol = st.columns(2)

    with gcol:
        st.markdown("##### Conditional OTA gates")
        st.caption("Injected as `minCsq` / `minKbps` / `maxRetries`. The device "
                   "checks these before disconnecting MQTT and staging to UFS.")
        min_csq = st.slider(
            "Minimum CSQ (AT+CSQ RSSI, 0–31)", 5, 31, CFG.default_min_csq,
            help="EC200U_AWS_OTA.h currently compares against the compile-time "
                 "MIN_OTA_RSSI (10). See docs/firmware-contract.md for the patch "
                 "that reads this field instead.",
        )
        min_kbps = st.slider("Minimum downlink (kbps)", 0, 500,
                             CFG.default_min_kbps, step=10)
        max_retries = st.number_input("Device-side retries before FAILED", 0, 5, 2)
        require_charging = st.toggle(
            "Require pack on charge", value=False,
            help="Adds `requireCharging:true`. Only meaningful once the firmware "
                 "reads it — see the contract doc.",
        )
        gate = jobdoc.NetworkGate(int(min_csq), int(min_kbps), int(max_retries),
                                  bool(require_charging))

    with scol:
        st.markdown("##### Rollout & schedule")
        max_per_min = st.slider("Max devices per minute", 1, 50,
                                CFG.default_max_per_minute,
                                help="Staged rollout. Protects the carrier link "
                                     "and prevents a fleet-wide simultaneous flash.")
        timeout_min = st.slider("Execution timeout (min)", 5, 120,
                                CFG.job_timeout_minutes, step=5)
        abort_pct = st.slider("Auto-abort at failure rate (%)", 5, 100, 25, step=5)
        target_sel = st.radio(
            "Target selection", ("SNAPSHOT", "CONTINUOUS"), horizontal=True,
            help="CONTINUOUS also targets nodes added to the group later.",
        )

        mode = st.radio("Start", ("Immediately", "Scheduled window"),
                        horizontal=True)
        start_utc = end_utc = None
        if mode == "Scheduled window":
            tzname = st.selectbox("Timezone", TZ_CHOICES)
            tz = ZoneInfo(tzname)
            now_local = datetime.now(tz) + timedelta(minutes=45)
            d1, t1 = st.columns(2)
            sd = d1.date_input("Start date", value=now_local.date(),
                               min_value=datetime.now(tz).date())
            stime = t1.time_input("Start time", value=dtime(now_local.hour,
                                                            (now_local.minute // 5) * 5))
            start_utc = datetime.combine(sd, stime, tzinfo=tz).astimezone(timezone.utc)

            if st.toggle("Close the window at a fixed time", value=False):
                d2, t2 = st.columns(2)
                ed = d2.date_input("End date", value=sd + timedelta(days=1))
                etime = t2.time_input("End time", value=dtime(6, 0))
                end_utc = datetime.combine(ed, etime, tzinfo=tz).astimezone(timezone.utc)

            sched_errs = jobs.validate_schedule(start_utc, end_utc)
            if sched_errs:
                for e in sched_errs:
                    st.error(e, icon="⛔")
            else:
                st.success(f"Rollout begins {start_utc:%Y-%m-%d %H:%M} UTC "
                           f"({start_utc.astimezone(tz):%H:%M} {tzname}).", icon="🕒")

    plan = jobs.RolloutPlan(
        max_per_minute=int(max_per_min),
        timeout_minutes=int(timeout_min),
        abort_failure_pct=float(abort_pct),
        target_selection=target_sel,
        start_utc=start_utc,
        end_utc=end_utc,
    )

    # ------------------------------------------------------- job document
    st.divider()
    st.markdown("##### Pre-flight")

    built = jobdoc.build(
        region=CFG.region, bucket=CFG.bucket, key=art["key"],
        version=art["version"], gate=gate, sha256=art.get("sha256") or None,
        size=art["size"], urc_budget=CFG.urc_budget_bytes,
    )
    pf = fl.preflight(selected, fleet, plan.max_per_minute)
    sched_errs = jobs.validate_schedule(plan.start_utc, plan.end_utc)

    dcol, icol = st.columns([1.1, 1])
    with dcol:
        st.code(built.document, language="json")
        used = built.delivered_estimate / max(built.budget, 1)
        st.progress(min(used, 1.0),
                    text=f"Estimated delivered URC size "
                         f"{built.delivered_estimate} B / {built.budget} B budget "
                         f"({used:.0%})")
        st.caption(
            "Budget derives from the 1536 B URC buffer in main.cpp and "
            "`_otaUrl[1536]` in EC200U_AWS_OTA.h. `firmwareUrl` is emitted first "
            "because otaCheckDownlink() takes the FIRST `https://` in the line."
        )
    with icol:
        blocking = list(pf.blocking) + list(built.errors) + list(sched_errs)
        if not built.within_budget:
            blocking.append(
                f"Job document would deliver ≈{built.delivered_estimate} B, over "
                f"the {built.budget} B device budget. Shorten the bucket/key, or "
                "raise the URC buffer in firmware."
            )
        for b in blocking:
            st.error(b, icon="⛔")
        for a in pf.advisory:
            st.info(a, icon="ℹ️")
        if not blocking:
            st.success(f"{len(selected)} target(s) cleared pre-flight.", icon="✅")

        job_id = jobs.make_job_id(CFG.job_id_prefix, art["version"])
        st.caption(f"Job id → `{job_id}`")

        ready = not blocking and is_operator()
        if st.button("Review & deploy", type="primary", disabled=not ready,
                     use_container_width=True):
            _confirm(job_id, selected, art, gate, plan, built.document)
        if not is_operator():
            st.caption("Viewer role cannot create jobs.")


# ======================================================== 3 · job tracking

def _exec_frame(snap: jobs.JobSnapshot) -> pd.DataFrame:
    # Triage order, not lifecycle order — failures at the top.
    rank = {s: i for i, s in enumerate(jobs.TABLE_ORDER)}
    rows = [
        {
            "Node": e.thing,
            "Status": e.status,
            "Retries": e.retry_count,
            "Queued": e.queued_at,
            "Started": e.started_at,
            "Updated": e.updated_at,
        }
        for e in snap.executions
    ]
    df = pd.DataFrame(rows)
    if df.empty:
        return df
    df["_r"] = df["Status"].map(lambda s: rank.get(s, 99))
    return df.sort_values(["_r", "Node"]).drop(columns="_r").reset_index(drop=True)


def page_tracking() -> None:
    created = st.session_state.pop("just_created", None)
    if created:
        st.toast(f"Job {created} created.", icon="🚀")

    recent, err = jobs.recent_jobs(40)
    header(
        "Job Tracking",
        "One ListJobExecutionsForJob call per TTL window covers the whole fleet — "
        "no per-device DescribeJobExecution polling.",
        right=f"poll TTL {CFG.poll_ttl_seconds}s",
    )

    if err:
        st.error(err, icon="⛔")
        return
    if not recent:
        st.info("No IoT jobs found in this account/region.")
        return

    labels = {f"{j['jobId']}  ·  {j.get('status', '?')}": j["jobId"] for j in recent}
    tracked = st.session_state.get("tracked_job")
    default = 0
    if tracked:
        for i, jid in enumerate(labels.values()):
            if jid == tracked:
                default = i
                break

    c1, c2, c3 = st.columns([3, 1, 1])
    chosen = c1.selectbox("Job", tuple(labels), index=default,
                          label_visibility="collapsed")
    job_id = labels[chosen]
    st.session_state["tracked_job"] = job_id
    live = c2.toggle("Auto-refresh", value=True)
    every = c3.selectbox("Interval", (2, 5, 10, 30), index=1,
                         format_func=lambda s: f"{s}s",
                         label_visibility="collapsed")

    # The fragment is the whole point: only this pane re-executes, so the
    # selectbox above, the sidebar, and any other pane's state are untouched.
    @st.fragment(run_every=(f"{every}s" if live else None))
    def pane() -> None:
        snap = jobs.snapshot(job_id)
        if snap.error:
            st.error(snap.error, icon="⛔")
            return

        stat_row([
            stat("Job state", snap.status,
                 tone="ok" if snap.status == "COMPLETED" else "info"),
            stat("Targets", str(snap.total)),
            stat("Succeeded", str(snap.succeeded), tone="ok"),
            stat("In progress", str(snap.counts.get("IN_PROGRESS", 0)), tone="info"),
            stat("Queued", str(snap.counts.get("QUEUED", 0)), tone="warn"),
            stat("Failed", str(snap.failed),
                 tone="bad" if snap.failed else ""),
        ])
        st.progress(snap.progress,
                    text=f"{snap.done}/{snap.total} terminal "
                         f"({snap.progress:.0%})  ·  {snap.api_calls} API call(s) "
                         f"this fetch  ·  refreshed "
                         f"{datetime.fromtimestamp(snap.fetched_at):%H:%M:%S}")

        st.markdown(
            " ".join(pill(s) + f' <span style="color:#8b98ab;font-size:.78rem">'
                               f'{snap.counts[s]}</span>'
                     for s in jobs.ORDER if snap.counts.get(s)),
            unsafe_allow_html=True,
        )

        df = _exec_frame(snap)
        if df.empty:
            st.info("No executions yet — the job is still being rolled out.")
        else:
            st.dataframe(
                df, use_container_width=True, hide_index=True, height=330,
                column_config={
                    "Queued": st.column_config.DatetimeColumn(format="DD MMM HH:mm:ss"),
                    "Started": st.column_config.DatetimeColumn(format="DD MMM HH:mm:ss"),
                    "Updated": st.column_config.DatetimeColumn(format="DD MMM HH:mm:ss"),
                },
            )

        failed = tuple(e.thing for e in snap.executions
                       if e.status in ("FAILED", "TIMED_OUT", "REJECTED"))
        if failed:
            with st.expander(f"Failure detail · {len(failed)} node(s)"):
                st.caption("Reasons come from `_otaFail()` in EC200U_AWS_OTA.h: "
                           "weak_signal · qhttpurl_connect · get_len · readfile · "
                           "update_begin · qfread_short · flash_write · finalize.")
                if st.button("Fetch statusDetails", key=f"fd-{job_id}"):
                    reasons = jobs.failure_reasons(job_id, failed)
                    st.dataframe(
                        pd.DataFrame(
                            [{"Node": k, "Reason": v} for k, v in reasons.items()]),
                        use_container_width=True, hide_index=True,
                    )
                    if len(failed) > 8:
                        st.caption(f"Showing 8 of {len(failed)} — capped to keep "
                                   "the call count bounded.")

    pane()

    if is_operator() and st.button("Cancel this job"):
        try:
            jobs.cancel(job_id)
            st.success("Cancel requested.")
        except Exception as exc:  # noqa: BLE001
            st.error(str(exc))


# ======================================================= 4 · live telemetry

def page_telemetry() -> None:
    header(
        "Live Telemetry · Hybrid WSS",
        "The browser holds the MQTT/WSS socket to IoT Core directly. Python only "
        "mints a short-lived SigV4 URL and stays out of the data path.",
        right=f"{CFG.endpoint or 'endpoint unset'}",
    )

    # Telemetry only needs registry access to POPULATE the node picker, so a
    # discovery failure degrades to manual topic entry rather than blocking the
    # stream — an operator with subscribe-only IAM can still watch traffic.
    fleet = fl.discover()
    if fleet.error:
        st.warning(f"Fleet discovery unavailable — {fleet.error} "
                   "Enter a topic filter manually.", icon="⚠️")
        fleet = fl.Fleet((), "unavailable", "")

    c1, c2 = st.columns([3, 1])
    opts = (("Selected nodes", "Whole fleet", "Custom topic") if fleet.nodes
            else ("Whole fleet", "Custom topic"))
    scope = c2.radio("Scope", opts)

    if scope == "Selected nodes":
        pre = tuple(st.session_state.get("sel_nodes", ()))
        nodes = tuple(c1.multiselect("Nodes", fleet.names, default=pre,
                                     placeholder="Select nodes to stream…"))
        topics = [CFG.telemetry_topic(n) for n in nodes]
    elif scope == "Whole fleet":
        # Deliberately a scoped wildcard on the telemetry prefix, never "#".
        topics = [CFG.telemetry_topic("+")]
        c1.text_input("Topic filter", value=topics[0], disabled=True)
    else:
        raw = c1.text_input("Topic filter", value=CFG.telemetry_topic("+"))
        topics = [t.strip() for t in raw.split(",") if t.strip()]

    if not topics:
        st.info("Pick at least one node, or switch scope to the fleet wildcard.")
        return
    if len(topics) > 8:
        st.warning(f"{len(topics)} explicit topics. Consider the fleet wildcard "
                   f"`{CFG.telemetry_topic('+')}` instead of many subscriptions.",
                   icon="⚠️")

    if not CFG.endpoint:
        st.error("Set `[iot].endpoint` in secrets.toml (the iot:Data-ATS endpoint).")
        return

    try:
        signed = wss.signed_url()
    except Exception as exc:  # noqa: BLE001
        st.error(f"Could not presign the WSS URL: {exc}")
        return

    cid = st.session_state.setdefault("wss_cid", f"as-dash-{uuid.uuid4().hex[:12]}")
    # stale_after_s mirrors the firmware's own validity window
    # (bmsReadInterval * 3 == 15 s in buildTelemetryPayload), so the STALE badge
    # and the device's `valid` flag can never disagree.
    tw.render(signed, topics, cid, height=440, stale_after_s=15)

    st.caption(
        f"Client id `{cid}` · signature valid ~{wss.PRESIGN_TTL}s and re-minted on "
        "rerun · QoS 0, clean session, no persistent subscription left behind. "
        "The tab owns the socket lifecycle: closing it disconnects."
    )

    with st.expander("Required IAM permissions for this widget"):
        st.code(
            f"""iot:Connect    arn:aws:iot:{CFG.region}:<account>:client/as-dash-*
iot:Subscribe  arn:aws:iot:{CFG.region}:<account>:topicfilter/{CFG.telemetry_topic('*')}
iot:Receive    arn:aws:iot:{CFG.region}:<account>:topic/{CFG.telemetry_topic('*')}""",
            language="text",
        )
        st.caption("Scoped to the telemetry prefix only — never `topic/*` or `#`. "
                   "Full policy: iam/ecs-task-role-policy.json")


# ================================================================== router

PAGES = {
    "Firmware Registry": page_firmware,
    "Fleet & Deploy": page_deploy,
    "Job Tracking": page_tracking,
    "Live Telemetry": page_telemetry,
}

PAGES[sidebar()]()
