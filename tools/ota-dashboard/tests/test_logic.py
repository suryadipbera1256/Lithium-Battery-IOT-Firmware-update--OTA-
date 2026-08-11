"""Pure-logic unit tests: image parsing, job-document invariants, scheduling
rules, pre-flight, and SigV4 presigning. No AWS calls, no Streamlit runtime."""
import json
import struct
from datetime import datetime, timedelta, timezone

from _bootstrap import check, finish  # noqa: E402  (also sets cwd + sys.path)

from core import firmware as fw, fleet as fl, jobdoc, jobs, wss

# ---------------------------------------------- 1. firmware image inspection
def synth(version="1.0.3", project="as-ai-fw", size=1_100_000, magic=0xE9,
          desc_magic=0xABCD5432):
    b = bytearray(size)
    b[0] = magic
    struct.pack_into("<II8s32s32s16s16s32s", b, 0x20, desc_magic, 0, b"",
                     version.encode(), project.encode(), b"12:00:00",
                     b"Aug  6 2026", b"v4.4.7")
    return bytes(b)

m = fw.inspect("firmware.bin", synth(), 1_966_080)
check("image magic detected", m.image_magic_ok)
check("esp_app_desc_t parsed", m.app_desc_ok)
check("embedded version extracted", m.version == "1.0.3", m.version)
check("project name extracted", m.project_name == "as-ai-fw", m.project_name)
check("idf version extracted", m.idf_version == "v4.4.7", m.idf_version)
check("built stamp extracted", m.built == "Aug  6 2026 12:00:00", m.built)
check("deployable", m.deployable, str(m.errors))
check("sha256 length", len(m.sha256) == 64)
check("md5 b64 present", bool(m.md5_b64))

bad = fw.inspect("x.bin", synth(magic=0x00), 1_966_080)
check("non-ESP32 image rejected", not bad.deployable and not bad.image_magic_ok)

too_big = fw.inspect("x.bin", synth(size=2_000_000), 1_966_080)
check("oversize image rejected", not too_big.deployable,
      str(too_big.errors[:1]))

tight = fw.inspect("x.bin", synth(size=1_900_000), 1_966_080)
check("near-full slot warns but deploys", tight.deployable and bool(tight.warnings))

nodesc = fw.inspect("x.bin", synth(desc_magic=0x12345678), 1_966_080)
check("missing app_desc warns, still deployable",
      nodesc.deployable and not nodesc.app_desc_ok and bool(nodesc.warnings))

# S3 key layout is asserted in test_phase_h.py, which owns the bucket-layout
# contract (s3_key, job_doc_key, version_slug, and the version round-trip used by
# app.py). Duplicating it here left a stale copy asserting the retired
# "firmware/<version>/firmware.bin" scheme, which silently contradicted the
# agreed "firmwares/firmware_v1_0_5.bin" layout.

# ---------------------------------------------------- 2. job document build
gate = jobdoc.NetworkGate(min_csq=12, min_kbps=40, max_retries=2)
d = jobdoc.build(region="ap-south-1", bucket="as-ota-firmware",
                 key="firmware/1.0.3/firmware.bin", version="1.0.3", gate=gate,
                 sha256="9f2c1ab34de5f607aa11bb22cc33dd44ee55ff66", size=1_100_000,
                 urc_budget=1400)
doc = json.loads(d.document)
check("firmwareUrl is first key", next(iter(doc)) == "firmwareUrl")
check("presigned placeholder form",
      doc["firmwareUrl"].startswith("${aws:iot:s3-presigned-url:https://s3.ap-south-1"))
check("gates injected",
      (doc["minCsq"], doc["minKbps"], doc["maxRetries"]) == (12, 40, 2))
check("compact json (no spaces)", ", " not in d.document and '": ' not in d.document)
check("sha truncated to 16", len(doc["sha256"]) == 16)
check("no errors", not d.errors, str(d.errors))
check("within URC budget", d.within_budget,
      f"{d.delivered_estimate}/{d.budget}")
check("first https is firmwareUrl's",
      d.document.find("https://") == d.document.find(d.base_url))

longkey = jobdoc.build(region="ap-south-1", bucket="a" * 300,
                       key=("b" * 400) + "/firmware.bin", version="1.0.3",
                       gate=gate, sha256=None, size=None, urc_budget=1400)
check("oversized doc flagged over budget", not longkey.within_budget,
      f"{longkey.delivered_estimate}/{longkey.budget}")
check("oversized doc not ok", not longkey.ok)

check("missing version errors",
      bool(jobdoc.build(region="ap-south-1", bucket="b", key="k", version="",
                        gate=gate, sha256=None, size=None,
                        urc_budget=1400).errors))

opt = jobdoc.build(region="ap-south-1", bucket="b", key="k", version="1.0.3",
                   gate=jobdoc.NetworkGate(20, 0, 0, True), sha256=None,
                   size=None, urc_budget=1400)
check("requireCharging opt-in", json.loads(opt.document).get("requireCharging") is True)
check("size/sha omitted when absent",
      "size" not in opt.document and "sha256" not in opt.document)

# --------------------------------------------------------- 3. job id + schedule
jid = jobs.make_job_id("as-ota", "1.0.3", datetime(2026, 8, 6, 9, 30, 0, tzinfo=timezone.utc))
check("job id sanitised & stamped", jid == "as-ota-1-0-3-20260806-093000", jid)
check("job id <= 64 chars", len(jobs.make_job_id("as-ota", "x" * 200)) <= 64)

now = datetime.now(timezone.utc)
check("immediate start needs no validation", jobs.validate_schedule(None, None) == ())
check("past start rejected", bool(jobs.validate_schedule(now - timedelta(hours=1), None)))
check("sub-30min lead rejected", bool(jobs.validate_schedule(now + timedelta(minutes=10), None)))
check("valid lead accepted", jobs.validate_schedule(now + timedelta(hours=2), None) == ())
check("end before start rejected",
      bool(jobs.validate_schedule(now + timedelta(hours=2), now + timedelta(hours=1))))

# ------------------------------------------------------------- 4. pre-flight
N = fl.Node
f = fl.Fleet((N("BAT-001", True, "", ""), N("BAT-002", False, "", ""),
             N("BAT-003", None, "", "")), "fleet-index", "")
check("fleet index O(1) lookup", set(f.index()) == {"BAT-001", "BAT-002", "BAT-003"})
check("online count", f.online == 1)
check("unknown count", f.unknown == 1)
check("empty selection blocks", not fl.preflight((), f, 5).clear)
check("unknown thing blocks", not fl.preflight(("GHOST",), f, 5).clear)
check("zero rate blocks", not fl.preflight(("BAT-001",), f, 0).clear)
p = fl.preflight(("BAT-001", "BAT-002", "BAT-003"), f, 5)
check("whole-fleet select passes with advisories", p.clear and len(p.advisory) >= 2,
      str(p.advisory))
check("offline node is advisory not blocking", fl.preflight(("BAT-002",), f, 5).clear)
check("node status labels",
      (f.nodes[0].status, f.nodes[1].status, f.nodes[2].status)
      == ("online", "offline", "unknown"))

# ----------------------------------------------------- 5. SigV4 WSS presign
url = wss._presign("abc123-ats.iot.ap-south-1.amazonaws.com", "ap-south-1",
                   "AKIAIOSFODNN7EXAMPLE",
                   "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "TOKEN/with+chars=")
check("wss scheme + /mqtt path", url.startswith("wss://abc123-ats.iot.ap-south-1.amazonaws.com/mqtt?"))
for p_ in ("X-Amz-Algorithm=AWS4-HMAC-SHA256", "X-Amz-Credential=",
           "X-Amz-Date=", "X-Amz-SignedHeaders=host", "X-Amz-Signature="):
    check(f"presign param {p_.rstrip('=')}", p_ in url)
check("iotdevicegateway in credential scope", "iotdevicegateway" in url)
check("signature is 64 hex", len(url.split("X-Amz-Signature=")[1].split("&")[0]) == 64)
check("token appended AFTER signature",
      url.index("X-Amz-Signature=") < url.index("X-Amz-Security-Token="))
check("token url-encoded", "TOKEN%2Fwith%2Bchars%3D" in url)
check("no token param when creds are long-lived",
      "X-Amz-Security-Token" not in wss._presign("h", "r", "a", "s", None))

# deterministic-signature sanity: same inputs within the same second -> same sig
u1 = wss._presign("h.example", "ap-south-1", "AK", "SK", None)
u2 = wss._presign("h.example", "ap-south-1", "AK", "SK", None)
check("signing is deterministic for identical inputs", u1.split("X-Amz-Date=")[1] == u2.split("X-Amz-Date=")[1])

# --------------------------------------------------------- 6. jobs constants
check("TERMINAL/ACTIVE disjoint", not (jobs.TERMINAL & jobs.ACTIVE))
check("ORDER covers all statuses", set(jobs.ORDER) == jobs.TERMINAL | jobs.ACTIVE)

finish()
