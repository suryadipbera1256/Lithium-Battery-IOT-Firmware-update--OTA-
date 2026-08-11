# Job-document contract: dashboard ⇄ `EC200U_AWS_OTA.h`

The dashboard emits this document (keys in this exact order):

```json
{"firmwareUrl":"${aws:iot:s3-presigned-url:https://s3.ap-south-1.amazonaws.com/BUCKET/firmware/1.0.3/firmware.bin}","version":"1.0.3","minCsq":12,"minKbps":40,"maxRetries":2,"size":1103456,"sha256":"9f2c1ab34de5f607"}
```

## Invariants the dashboard already enforces

| Invariant | Why | Enforced in |
|---|---|---|
| `firmwareUrl` is the **first** key, and no other field contains a URL | `otaCheckDownlink()` uses `strstr(urcLine, "https://")` — the first match wins | `core/jobdoc.py` (`build()` rejects violations) |
| Delivered document + presign expansion ≤ 1400 B | URC read buffer is 1536 B (`main.cpp`), `_otaUrl[1536]` | `core/jobdoc.py` (`delivered_estimate`) |
| Image ≤ `0x1E0000` (1 966 080 B) | app0/app1 slot size in `partitions_ota.csv` | `core/firmware.py` (`inspect()`) |
| First byte is `0xE9`, `esp_app_desc_t` magic `0xABCD5432` | rejects non-ESP32 / truncated images before upload | `core/firmware.py` |
| Compact JSON (`separators=(",",":")`) | every byte counts against the URC budget | `core/jobdoc.py` |

## Firmware side: gates are LIVE

`minCsq`, `minKbps` and `maxRetries` are parsed by `otaCheckDownlink()` and
enforced by `_isNetworkStable()` in `include/EC200U_AWS_OTA.h`. They are still
additive and backward-compatible: a document omitting them falls back to the
compile-time `MIN_OTA_RSSI` (10), so a legacy two-field job document behaves
exactly as it always did.

`requireCharging` is emitted by `core/jobdoc.py` when set but is **not** read by
the firmware. It is inert today.

### How the firmware reads them

`_otaParseInt()` searches for the **bare** key name, then skips any of
`space : " \` before the digits. It is deliberately not anchored on a leading
quote: whether `AT+QMTRECV` delivers the payload with literal or
backslash-escaped quotes is firmware-dependent, and matching only `"minCsq"`
would silently miss the escaped form — falling back to `MIN_OTA_RSSI` and
quietly bypassing an operator-set threshold. A safety gate must not fail open on
a formatting difference.

### `minKbps` is a proxy, not a measurement

The EC200U cannot measure throughput before the transfer starts, so the gate maps
the radio access technology from `AT+QNWINFO` to a conservative floor:

| RAT | Assumed floor |
|---|---:|
| LTE | 2000 kbps |
| HSPA | 384 |
| WCDMA / EDGE | 128 |
| GPRS | 40 |

An LTE device on a congested cell still passes. This answers "is the radio fast
enough", not "is there bandwidth right now".

### `maxRetries` is parsed but unused

`_gateRetries` is populated and logged, deliberately not acted on. The retry loop
belongs at the `otaRun()` call site in `main.cpp`; re-entering the AT bus from
inside `otaRun()` while `Update` is mid-flash would corrupt the download. AWS IoT
Jobs' own retry configuration covers this instead.

## Sequencing note

The gate check happens at **Step 1 of `otaRun()`, after** `IN_PROGRESS` is
published and the jobId is persisted to NVS. A gate rejection is therefore
reported as `FAILED` with `reason: weak_signal` rather than the job silently
staying `QUEUED`. That is deliberate — the operator sees the refusal in
**Job Tracking → Failure detail** — but it means a gated device consumes one
execution attempt per notify-next delivery.
