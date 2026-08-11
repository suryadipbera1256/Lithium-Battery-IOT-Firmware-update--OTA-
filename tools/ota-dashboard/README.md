# AS AI — Fleet OTA & Diagnostics Dashboard

Streamlit operator console for the ESP32 + Quectel EC200U-CN battery fleet:
firmware publication, conditional/scheduled AWS IoT Job rollout, batched job
tracking, and live browser→IoT Core telemetry.

---

## 1. Workspace isolation

This dashboard lives entirely under `tools/ota-dashboard/` and shares **nothing**
with the PlatformIO C++ project:

| Concern | Firmware project | This dashboard |
|---|---|---|
| Python env | `/.venv` (pyserial, python-dotenv — used by `scripts/apply_env.py`) | `tools/ota-dashboard/.venv` |
| Dependencies | `/requirements.txt` | `tools/ota-dashboard/requirements.txt` |
| Secrets | `/.env` (compile-time macros) | `tools/ota-dashboard/.streamlit/secrets.toml` |
| Config | `/platformio.ini` | `tools/ota-dashboard/.streamlit/config.toml` |

Why collision is structurally impossible, not just conventional:

* **PlatformIO never sees these files.** `platformio.ini` sets
  `build_src_filter` over `src/` only, and the Library Dependency Finder scans
  `lib/` and `include/`. `tools/` is outside both trees, so `pio run` cannot
  compile, scan, or copy anything here.
* **No shared interpreter.** `run.ps1` / `run.sh` create and invoke
  `tools/ota-dashboard/.venv` by absolute path. `streamlit` and `boto3` are
  never installed into the root `.venv`, so the PlatformIO pre-build script's
  dependency set stays exactly as it is.
* **No shared secrets file.** The dashboard does not read `/.env`. Firmware
  identity comes from compile-time macros; dashboard identity comes from
  `st.secrets`.
* **Independent ignore rules.** A nested `.gitignore` excludes `.venv/` and
  `secrets.toml` here regardless of the root file.

```
tools/ota-dashboard/
├── app.py                      # router + all four pages
├── core/
│   ├── settings.py             # frozen dataclass over st.secrets, parsed once
│   ├── auth.py                 # password gate, operator/viewer roles
│   ├── aws.py                  # cache_resource boto3 session + clients
│   ├── firmware.py             # .bin inspection, checksum, S3 publish
│   ├── fleet.py                # discovery (fleet index → registry fallback)
│   ├── jobdoc.py               # job document builder + firmware-parser guards
│   ├── jobs.py                 # CreateJob, scheduling, batched polling
│   └── wss.py                  # SigV4 presigned MQTT-over-WSS URL
├── components/
│   ├── ui.py                   # stat cards, pills, kv blocks
│   └── telemetry.py            # embedded browser→IoT Core MQTT widget
├── static/theme.css
├── tests/                      # 114 checks, no pytest needed
├── iam/ecs-task-role-policy.json
├── docs/firmware-contract.md   # job-document ⇄ EC200U_AWS_OTA.h contract
├── .streamlit/{config.toml, secrets.toml.example}
├── requirements.txt
└── run.ps1 / run.sh
```

---

## 2. Setup

```powershell
cd tools\ota-dashboard
copy .streamlit\secrets.toml.example .streamlit\secrets.toml
notepad .streamlit\secrets.toml
.\run.ps1
```

`run.ps1` creates `.venv`, installs `requirements.txt`, and starts Streamlit on
`127.0.0.1:8501`. Subsequent runs reuse the venv.

Get your ATS endpoint for `[iot].endpoint`:

```bash
aws iot describe-endpoint --endpoint-type iot:Data-ATS --region ap-south-1
```

Enable fleet indexing so the dashboard can show live connectivity (optional —
it falls back to `ListThings` and says so):

```bash
aws iot update-indexing-configuration --thing-indexing-configuration thingIndexingMode=REGISTRY_AND_SHADOW,thingConnectivityIndexingMode=STATUS --region ap-south-1
```

---

## 3. Modules

**Security gate** — `st.secrets` passwords with `hmac.compare_digest`, per-session
lockout, and two roles: `operator` (upload + CreateJob + CancelJob) and `viewer`
(fleet, tracking, telemetry). The role gate disables the write paths in the UI.

**Firmware Registry** — the `.bin` is parsed **before** upload: image magic
`0xE9`, `esp_app_desc_t` (embedded version / project / IDF / build timestamp),
SHA-256, and fit against the 1 920 KB app slot from `partitions_ota.csv`. Upload
is a single `PutObject` with `Content-MD5` so S3 rejects a corrupted transfer
server-side; version and SHA-256 go into object metadata. Overwriting an
existing key warns first.

**Fleet Targeting** — multiselect with canary / select-all / clear, an
online-only filter, and pre-flight validation that separates *blocking* errors
(unknown things, empty selection) from *advisory* ones (offline nodes — which is
fine, the job waits; whole-fleet selection — which gets an explicit warning).

**Conditional OTA gates** — `minCsq`, `minKbps`, `maxRetries`,
`requireCharging` are injected into the job document. `core/jobdoc.py` enforces
the two hard constraints of the real device parser: `firmwareUrl` must be the
first URL in the document (`otaCheckDownlink()` takes the first `https://`), and
the delivered size must fit the 1536 B URC buffer. The UI shows live budget
consumption and blocks a push that would overflow it.
The running firmware ignores these fields today —
[`docs/firmware-contract.md`](docs/firmware-contract.md) has the ~20-line patch
that makes them authoritative.

**Precision scheduling** — date + time pickers in `Asia/Kolkata` or UTC,
converted to UTC and emitted as `schedulingConfig.startTime` / `endTime` with
`endBehavior: STOP_ROLLOUT`. AWS's 30-minute minimum lead time is validated
client-side before `CreateJob`.

**Staged rollout** — every job carries `jobExecutionsRolloutConfig`
(`maximumPerMinute` + exponential ramp gated on succeeded things), `abortConfig`
(auto-CANCEL past a failure-rate threshold), and `timeoutConfig`. There is no
code path that pushes to the whole fleet at once.

**Smart tracking** — `ListJobExecutionsForJob` (one paginated call for the whole
job) instead of `DescribeJobExecution` per device per tick:
**1 + ⌈n/250⌉ calls per TTL window**, aggregated in a single `Counter` pass.
`@st.cache_data` with a secrets-driven TTL means a 2 s refresh tick against a
10 s TTL still makes ~1 call per 10 s. `st.fragment(run_every=)` scopes the
auto-refresh to the tracking pane alone. `statusDetails` (the `_otaFail()`
reason strings) are fetched on demand for a capped set of failed nodes, never in
the poll loop.

**Live telemetry (hybrid WSS)** — Python mints a short-lived SigV4-presigned
`wss://…/mqtt` URL and hands it to an embedded widget; the browser holds the
MQTT socket to IoT Core directly. No background thread, no queue, no
`session_state` race, no orphaned connection — the tab owns the lifecycle.
The widget does JSON/hex-dump toggling, substring filtering, pause, a
rAF-batched bounded DOM ring (300 rows, O(1) per message), and a 10 s rolling
message-rate meter.

> **Why not the AWS IoT Device SDK v2 for JavaScript?** Its browser build
> requires a bundler (there is no drop-in CDN artefact) and Cognito Identity
> Pool credentials — a second identity to provision and scope. Server-side
> presigning reuses the dashboard's already-scoped IAM identity, needs no build
> step, and keeps credentials out of the page: the browser only ever receives a
> subscribe-scoped, ~4-minute-lived signed URL. The data path is identical.

---

## 4. Tests

```powershell
.\.venv\Scripts\python.exe tests\run_all.py
```

No pytest, no extra dependencies — plain scripts, 114 checks:

| Suite | Covers |
|---|---|
| `tests/test_logic.py` | `esp_app_desc_t` parsing, app-slot fit, job-document invariants (firmwareUrl-first, URC budget, compact JSON), job-id sanitising, the 30-minute scheduling rule, pre-flight blocking vs advisory, SigV4 WSS presigning |
| `tests/test_pages_degraded.py` | Every page renders an actionable message — not a traceback — with absent/bad credentials; viewer role cannot upload; the gate does not leak navigation |
| `tests/test_pages_stubbed.py` | Full happy path against stubbed AWS: fleet pagination, gate values reaching the document, scheduling widgets, `CreateJob` payload shape, tracking aggregation and API-call count |

These are regression guards for four bugs found during development, all of
which are the kind that only appear at runtime:

1. `iot.get_paginator("search_index")` raises `OperationNotPageableError` —
   SearchIndex has no botocore paginator, so the fleet fast path failed
   *unconditionally* and crashed three pages. Now a hand-rolled `nextToken` loop.
2. Credential errors escaped as raw tracebacks instead of guidance.
3. The "inject CSS once per session" optimisation dropped the `<style>`
   element on every rerun (Streamlit reconciles the element tree positionally),
   so all styling vanished after the first render. Verified fixed in-browser.
4. The execution table sorted `QUEUED` first, burying failures under a page of
   `SUCCEEDED` rows. Now sorted failures-first for triage.

---

## 5. Security posture

`iam/ecs-task-role-policy.json` is the single source of truth for the dashboard's
permissions — attach it to a plain IAM user for local Docker, or to the ECS task
role on Fargate. Only the trust relationship differs, so it is deliberately not
duplicated per deployment target. Scoped to: `s3:PutObject` on
`BUCKET/PREFIX/*` only; `iot:CreateJob` on `job/as-ota-*`; `iot:Connect` on
`client/as-dash-*`; `iot:Subscribe`/`iot:Receive` on
`topicfilter/bms/data/*/telemetry` — **not** `#`; plus explicit `Deny` on
`iot:Publish` (the dashboard is an observer and must not be able to forge
telemetry or fake a SUCCEEDED job execution) and on firmware deletion.

### Applying the policies

The checked-in JSON carries `Comment` / `_README` keys explaining each statement.
**IAM rejects those** — its Statement grammar allows only Sid/Effect/Action/
Resource/Principal/Condition, so a raw `aws iam create-policy` fails with
`MalformedPolicyDocument`. Derive apply-ready copies instead of keeping a second
hand-maintained set in sync:

```bash
python iam/render_policies.py out/
aws iam create-policy --policy-name AsOtaDashboardPolicy \
  --policy-document file://out/dashboard-policy.json
aws iam create-role --role-name AsOtaIotPresignRole \
  --assume-role-policy-document file://out/presign-trust.json
aws iam put-role-policy --role-name AsOtaIotPresignRole \
  --policy-name SignFirmwareObjects --policy-document file://out/presign-perms.json
```

The renderer also warns on any `ACCOUNT_ID` / `REGION` / `CMK_KEY_ID` placeholder
left unsubstituted — the usual cause of a policy that applies cleanly but silently
matches nothing at runtime.

**The password gate is not a perimeter.** These are live battery packs with
MOSFET control, so before this reaches more than one tester put the process
behind an ALB with Cognito/OIDC, or restrict it to the office VPN, and keep
`server.address = 127.0.0.1` locally. That is the one gap here with hardware
safety consequences rather than engineering debt.

---

## 6. Contracts read from the firmware

Everything below was taken from the source, not assumed:

| Value | Source |
|---|---|
| `bms/data/{thing}/telemetry` | `src/main.cpp:362` |
| `$aws/things/{thing}/jobs/notify-next` | `src/main.cpp:220` |
| `$aws/things/{thing}/jobs/{jobId}/update` | `EC200U_AWS_OTA.h` `_otaReportStatus()` |
| First-`https://` URL parse | `EC200U_AWS_OTA.h` `otaCheckDownlink()` |
| 1536 B URC / `_otaUrl` buffer | `src/main.cpp:231`, `EC200U_AWS_OTA.h` |
| App slot `0x1E0000` = 1 966 080 B | `partitions_ota.csv` |
| Failure reason strings | `EC200U_AWS_OTA.h` `_otaFail()` call sites |
| `SUCCEEDED` reported post-reboot | `EC200U_AWS_OTA.h` `otaBootReport()` |

The last row matters when reading the tracking table: a device sits in
`IN_PROGRESS` across the flash **and** the reboot, and only flips to
`SUCCEEDED` once the new image boots, self-validates, and reconnects MQTT. A
long `IN_PROGRESS` is expected behaviour, not a stall.
