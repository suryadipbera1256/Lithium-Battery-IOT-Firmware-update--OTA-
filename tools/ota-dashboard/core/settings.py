"""Immutable, single-parse settings facade over st.secrets.

st.secrets lookups are dict traversals; we collapse them into one frozen
dataclass built once per session (cache_resource) so no page-render path
re-walks the secrets tree.  O(1) attribute access everywhere downstream.
"""
from __future__ import annotations

import hashlib
import tomllib
from dataclasses import dataclass, fields
from pathlib import Path
from typing import Any

import streamlit as st

SECRETS_PATH = Path(__file__).resolve().parent.parent / ".streamlit" / "secrets.toml"


def _get(section: str, key: str, default: Any = "") -> Any:
    try:
        return st.secrets[section][key]
    except (KeyError, FileNotFoundError):
        return default


def secrets_health() -> str:
    """Explain WHY secrets are empty. Streamlit swallows a TOML parse error and
    hands back an empty mapping, which is indistinguishable from a missing file
    -- and a BOM is the single most common cause on Windows, because Notepad
    writes one by default and `Out-File -Encoding utf8` does too under
    PowerShell 5.1."""
    if not SECRETS_PATH.exists():
        return (f"`{SECRETS_PATH.name}` not found at `{SECRETS_PATH.parent}`. "
                "Copy `secrets.toml.example` next to it and fill it in.")
    raw = SECRETS_PATH.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        return (
            "`secrets.toml` starts with a UTF-8 BOM, so TOML parsing fails and "
            "Streamlit reports no secrets at all. Re-save it as UTF-8 **without** "
            "BOM (VS Code: 'Save with Encoding → UTF-8'; PowerShell: "
            "`Set-Content -Encoding utf8NoBOM`, or `[IO.File]::WriteAllText()`)."
        )
    try:
        parsed = tomllib.loads(raw.decode("utf-8"))
    except UnicodeDecodeError:
        return "`secrets.toml` is not valid UTF-8. Re-save it as UTF-8 without BOM."
    except tomllib.TOMLDecodeError as exc:
        return f"`secrets.toml` is not valid TOML: {exc}"
    missing = [s for s in ("auth", "aws", "iot", "s3") if s not in parsed]
    if missing:
        return (f"`secrets.toml` parses but is missing section(s): "
                f"{', '.join('[' + m + ']' for m in missing)}.")
    return ("`secrets.toml` parses and has an `[auth]` section, but neither "
            "`operator_password` nor `viewer_password` is set to a non-empty value.")


@dataclass(frozen=True, slots=True)
class Settings:
    # auth
    operator_password: str
    viewer_password: str
    max_attempts: int
    # aws
    region: str
    access_key_id: str
    secret_access_key: str
    session_token: str
    profile: str
    # iot
    endpoint: str
    telemetry_topic_template: str
    thing_group: str
    thing_type: str
    job_id_prefix: str
    # IAM role AWS IoT assumes to SIGN the S3 URL. Without it the
    # ${aws:iot:s3-presigned-url:...} placeholder is delivered unexpanded and
    # every device fails at AT+QHTTPGET with HTTP 403.
    presign_role_arn: str
    presign_expires_sec: int
    # s3 -- single bucket, two prefixes. Bucket-per-version is discarded:
    # `as-ota-firmware/firmwares/...` and `as-ota-firmware/jobs/...`
    bucket: str
    firmware_prefix: str
    jobs_prefix: str
    # ota
    app_slot_bytes: int
    urc_budget_bytes: int
    default_min_csq: int
    default_min_kbps: int
    default_max_per_minute: int
    job_timeout_minutes: int
    poll_ttl_seconds: int
    fleet_ttl_seconds: int

    def telemetry_topic(self, thing: str) -> str:
        return self.telemetry_topic_template.format(thing=thing)

    @property
    def configured(self) -> bool:
        return bool(self.region and self.endpoint and self.bucket)


def _schema_key() -> str:
    """Fingerprint of the Settings field list.

    st.cache_resource holds the built object for the life of the PROCESS. It does
    not re-execute when the source changes, so editing this dataclass while the
    server is running leaves a cached instance of the OLD class in place — every
    read of a newly added field then raises AttributeError, and the traceback
    points at the caller rather than at the stale cache. Renaming `prefix` to
    `firmware_prefix` did exactly that.

    Passing this fingerprint as a cache-key argument makes any field add, rename
    or removal invalidate the entry automatically, so a hot edit self-heals
    instead of needing a manual restart or a Clear-cache click.
    """
    names = ",".join(f.name for f in fields(Settings))
    return hashlib.sha1(names.encode("utf-8")).hexdigest()[:12]


def settings() -> Settings:
    return _settings_cached(_schema_key())


@st.cache_resource(show_spinner=False)
def _settings_cached(schema_key: str) -> Settings:  # noqa: ARG001 (cache key)
    return Settings(
        operator_password=str(_get("auth", "operator_password")),
        viewer_password=str(_get("auth", "viewer_password")),
        max_attempts=int(_get("auth", "max_attempts", 5)),
        region=str(_get("aws", "region", "ap-south-1")),
        access_key_id=str(_get("aws", "access_key_id")),
        secret_access_key=str(_get("aws", "secret_access_key")),
        session_token=str(_get("aws", "session_token")),
        profile=str(_get("aws", "profile")),
        endpoint=str(_get("iot", "endpoint")),
        telemetry_topic_template=str(
            _get("iot", "telemetry_topic_template", "bms/data/{thing}/telemetry")
        ),
        thing_group=str(_get("iot", "thing_group")),
        thing_type=str(_get("iot", "thing_type")),
        job_id_prefix=str(_get("iot", "job_id_prefix", "as-ota")),
        presign_role_arn=str(_get("iot", "presign_role_arn")),
        presign_expires_sec=int(_get("iot", "presign_expires_sec", 3600)),
        bucket=str(_get("s3", "bucket", "as-ota-firmware")),
        firmware_prefix=str(_get("s3", "firmware_prefix", "firmwares/")),
        jobs_prefix=str(_get("s3", "jobs_prefix", "jobs/")),
        app_slot_bytes=int(_get("ota", "app_slot_bytes", 1966080)),
        urc_budget_bytes=int(_get("ota", "urc_budget_bytes", 1400)),
        default_min_csq=int(_get("ota", "default_min_csq", 12)),
        default_min_kbps=int(_get("ota", "default_min_kbps", 40)),
        default_max_per_minute=int(_get("ota", "default_max_per_minute", 5)),
        job_timeout_minutes=int(_get("ota", "job_timeout_minutes", 30)),
        poll_ttl_seconds=int(_get("ota", "poll_ttl_seconds", 10)),
        fleet_ttl_seconds=int(_get("ota", "fleet_ttl_seconds", 300)),
    )
