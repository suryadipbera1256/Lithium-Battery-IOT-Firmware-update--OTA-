"""Phase H contract tests: S3 layout, job document, payload shape.

Run: .venv/Scripts/python.exe -m pytest tests/test_phase_h.py -q
     (or plain `python tests/test_phase_h.py` for a dependency-free check)

These assert the two contracts that cannot be verified by running the app:
the agreed S3 key layout, and that the job document the dashboard emits is
still parseable by the firmware's actual parser in EC200U_AWS_OTA.h.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from core import firmware as fw
from core import jobdoc

REGION = "ap-south-1"
BUCKET = "as-ota-firmware"
FW_PREFIX = "firmwares/"
JOBS_PREFIX = "jobs/"


# ---------------------------------------------------------------- S3 layout

def test_version_slug():
    assert fw.version_slug("1.0.5") == "v1_0_5"
    assert fw.version_slug("v1.0.5") == "v1_0_5"
    assert fw.version_slug(" 1.0.10 ") == "v1_0_10"


def test_firmware_key_matches_agreed_layout():
    assert fw.s3_key(FW_PREFIX, "1.0.5") == "firmwares/firmware_v1_0_5.bin"
    # A trailing slash in config must not double up.
    assert fw.s3_key("firmwares", "1.0.5") == "firmwares/firmware_v1_0_5.bin"


def test_job_doc_key_matches_agreed_layout():
    assert fw.job_doc_key(JOBS_PREFIX, "1.0.5") == "jobs/ota_job_v1_0_5.json"


def test_no_bucket_per_version_anywhere():
    """The retired pattern must not reappear in a key."""
    key = fw.s3_key(FW_PREFIX, "1.0.5")
    assert "as-ota-firmware-" not in key


def test_app_recovers_version_from_flat_key():
    """app.py's regex must round-trip the key back to a version string."""
    key = fw.s3_key(FW_PREFIX, "1.0.5")
    m = re.search(r"firmware_v?([0-9]+(?:[._][0-9]+)*)\.bin$", key)
    assert m and m.group(1).replace("_", ".") == "1.0.5"


# ------------------------------------------------------------ job document

def _build(version="1.0.5", csq=18, kbps=384, retries=2):
    return jobdoc.build(
        region=REGION, bucket=BUCKET,
        key=fw.s3_key(FW_PREFIX, version),
        version=version,
        gate=jobdoc.NetworkGate(csq, kbps, retries),
        sha256="9f2c1ab34de5f607" + "0" * 48,
        size=375381,
        urc_budget=1400,
    )


def test_document_is_valid_json_and_ok():
    b = _build()
    assert b.ok, b.errors
    json.loads(b.document)


def test_firmware_url_is_first_key():
    """otaCheckDownlink() takes the FIRST https:// in the URC line."""
    b = _build()
    doc = b.document
    assert doc.startswith('{"firmwareUrl":')
    assert doc.find("https://") == doc.find(b.base_url)


def test_url_points_at_new_layout():
    b = _build()
    assert b.base_url == (
        f"https://s3.{REGION}.amazonaws.com/{BUCKET}/firmwares/firmware_v1_0_5.bin"
    )


def test_gates_present_and_parseable_by_firmware_parser():
    """Mirror of _otaParseInt(): bare key name, separators skipped.

    The firmware searches for the BARE key so it survives both literal and
    backslash-escaped URC payloads. Verify both forms, because a gate that
    fails open would silently ignore an operator threshold.
    """
    doc = _build(csq=18, kbps=384, retries=2).document

    def ota_parse_int(line, key, fallback):
        i = line.find(key)
        if i < 0:
            return fallback
        p = i + len(key)
        while p < len(line) and line[p] in ' :"\\':
            p += 1
        j = p
        while j < len(line) and line[j].isdigit():
            j += 1
        return int(line[p:j]) if j > p else fallback

    for label, line in (("literal", doc), ("escaped", doc.replace('"', '\\"'))):
        assert ota_parse_int(line, "minCsq", 10) == 18, label
        assert ota_parse_int(line, "minKbps", 0) == 384, label
        assert ota_parse_int(line, "maxRetries", 0) == 2, label


def test_legacy_document_falls_back_to_compile_time_default():
    """A doc with no gates must leave the firmware on MIN_OTA_RSSI (10)."""
    line = ('{"firmwareUrl":"https://s3.x/y.bin","version":"1.0.2"}')

    def ota_parse_int(l, key, fallback):
        return fallback if l.find(key) < 0 else -1

    assert ota_parse_int(line, "minCsq", 10) == 10


def test_document_fits_device_urc_budget():
    b = _build()
    assert b.within_budget, (b.delivered_estimate, b.budget)


def test_url_in_another_field_is_rejected():
    """Guard against the parser latching onto the wrong URL."""
    b = jobdoc.build(
        region=REGION, bucket=BUCKET, key=fw.s3_key(FW_PREFIX, "1.0.5"),
        version="https://evil.example/x",           # URL smuggled into version
        gate=jobdoc.NetworkGate(12, 0, 1),
        sha256=None, size=None, urc_budget=1400,
    )
    assert not b.ok
    assert any("URL" in e for e in b.errors)


# ------------------------------------------------- widget payload contract

PAYLOAD_KEYS = {
    "top": ["thing_name", "fw_version", "health", "location",
            "sensors", "telemetry", "bms"],
    "health": ["uptime_s", "reset", "heap_free", "heap_largest",
               "reconnects", "probation"],
    "bms": ["age_s", "valid", "voltage", "current", "soc", "residual_cap",
            "full_cap", "cycles", "balance", "protection", "chg_mos",
            "dsg_mos", "temps", "cell_count", "min_cell", "max_cell",
            "avg_cell", "delta_cell", "cells"],
}


def test_widget_reads_every_documented_field():
    """Every field the firmware emits must be referenced by the JS widget.

    Catches the drift that matters: firmware adds a field, dashboard never
    shows it, and nobody notices until it is needed during an incident.
    """
    js = (Path(__file__).resolve().parent.parent
          / "components" / "telemetry.py").read_text(encoding="utf-8")
    missing = []
    for group, keys in PAYLOAD_KEYS.items():
        for k in keys:
            if k not in js:
                missing.append(f"{group}.{k}")
    assert not missing, f"widget never reads: {missing}"


# ------------------------------------------------------ settings contract

def test_every_CFG_attribute_referenced_by_app_exists():
    """Static guard against the AttributeError class of bug.

    A renamed settings field only shows up at runtime, on the one page that
    reads it — health checks and unit tests both sail past it. Resolve every
    `CFG.<attr>` in app.py against the dataclass instead.
    """
    from dataclasses import fields as dc_fields

    from core.settings import Settings

    app = (Path(__file__).resolve().parent.parent / "app.py").read_text(encoding="utf-8")
    referenced = set(re.findall(r"\bCFG\.([A-Za-z_][A-Za-z0-9_]*)", app))
    available = {f.name for f in dc_fields(Settings)} | {
        n for n in dir(Settings) if not n.startswith("_")
    }
    missing = sorted(referenced - available)
    assert not missing, f"app.py reads CFG attributes that do not exist: {missing}"


def test_schema_key_changes_when_fields_change():
    """The cache key must actually track the field list."""
    from core.settings import _schema_key

    k1 = _schema_key()
    assert len(k1) == 12 and k1 == _schema_key()   # stable within a definition


# --------------------------------------------- presigned-URL guard (Step 3)

def test_create_job_configures_presigned_url_expansion():
    """A job whose document needs signing MUST NOT be created role-less.

    Without presignedUrlConfig.roleArn, AWS IoT delivers the placeholder
    verbatim and every device 403s at AT+QHTTPGET. Failing at CreateJob is
    strictly better: a doomed job still burns a device execution attempt and,
    with OTA_REPORT_JOB_STATUS=1, can leave a non-terminal execution that
    blocks every later job.
    """
    src = (Path(__file__).resolve().parent.parent
           / "core" / "jobs.py").read_text(encoding="utf-8")
    assert "presignedUrlConfig" in src, "CreateJob never sets presignedUrlConfig"
    assert "presign_role_arn" in src, "presign role is not consulted"
    assert "raise ValueError" in src, "guard must raise, not warn"


def test_presign_settings_exist():
    from dataclasses import fields as dc_fields

    from core.settings import Settings

    names = {f.name for f in dc_fields(Settings)}
    assert {"presign_role_arn", "presign_expires_sec"} <= names


if __name__ == "__main__":
    fails = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"PASS  {name}")
            except AssertionError as exc:
                fails += 1
                print(f"FAIL  {name}: {exc}")
    print(f"\n{fails} failure(s)")
    sys.exit(1 if fails else 0)
