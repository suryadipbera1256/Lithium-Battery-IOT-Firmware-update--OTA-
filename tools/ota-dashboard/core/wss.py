"""SigV4-presigned MQTT-over-WebSocket URL for the browser telemetry widget.

Why presign server-side instead of shipping the AWS IoT SDK v2 into the page:
the SDK's browser build requires a bundler (webpack/rollup) -- there is no
drop-in CDN artefact -- and it would need Cognito Identity Pool credentials,
i.e. a second identity to provision and scope. Presigning here reuses the
dashboard's already-scoped IAM identity, needs no build step, and keeps the
credentials out of the page: the browser only ever sees a short-lived,
subscribe-scoped signed URL.

The data path is still browser -> IoT Core directly. Python signs a URL and
then leaves the path entirely: no background thread, no queue, no
session_state race, and the browser tab owns the connection lifecycle.

Signing spec: service "iotdevicegateway", canonical URI "/mqtt", empty
payload, host-only signed headers, security token appended AFTER signing.
"""
from __future__ import annotations

import hashlib
import hmac
import time
from datetime import datetime, timezone
from urllib.parse import quote

import streamlit as st

from core.aws import frozen_credentials
from core.settings import settings

SERVICE = "iotdevicegateway"
_ALGO = "AWS4-HMAC-SHA256"
_EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
# Presigned WSS URLs are accepted within a 5-minute signing window; we
# re-mint at 4 minutes so an open widget never races the boundary.
PRESIGN_TTL = 240


def _sign(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def _signing_key(secret: str, datestamp: str, region: str) -> bytes:
    k = _sign(f"AWS4{secret}".encode("utf-8"), datestamp)
    k = _sign(k, region)
    k = _sign(k, SERVICE)
    return _sign(k, "aws4_request")


def _presign(endpoint: str, region: str, access_key: str, secret_key: str,
             token: str | None) -> str:
    now = datetime.now(timezone.utc)
    amzdate = now.strftime("%Y%m%dT%H%M%SZ")
    datestamp = now.strftime("%Y%m%d")
    scope = f"{datestamp}/{region}/{SERVICE}/aws4_request"

    # Query params must be in sorted order; these already are.
    qs = (
        f"X-Amz-Algorithm={_ALGO}"
        f"&X-Amz-Credential={quote(f'{access_key}/{scope}', safe='')}"
        f"&X-Amz-Date={amzdate}"
        f"&X-Amz-SignedHeaders=host"
    )
    canonical = "\n".join([
        "GET", "/mqtt", qs, f"host:{endpoint}", "", "host", _EMPTY_SHA256,
    ])
    to_sign = "\n".join([
        _ALGO, amzdate, scope, hashlib.sha256(canonical.encode()).hexdigest()
    ])
    signature = hmac.new(
        _signing_key(secret_key, datestamp, region), to_sign.encode(), hashlib.sha256
    ).hexdigest()

    url = f"wss://{endpoint}/mqtt?{qs}&X-Amz-Signature={signature}"
    if token:
        # Session tokens are appended AFTER signing -- part of the spec, not
        # an omission.
        url += f"&X-Amz-Security-Token={quote(token, safe='')}"
    return url


@st.cache_data(ttl=PRESIGN_TTL, show_spinner=False)
def _cached(endpoint: str, region: str, _bucket: int) -> str:
    creds = frozen_credentials()
    return _presign(endpoint, region, creds.access_key, creds.secret_key,
                    creds.token)


def signed_url() -> str:
    cfg = settings()
    if not cfg.endpoint:
        raise RuntimeError("[iot].endpoint is not set in secrets.toml")
    return _cached(cfg.endpoint, cfg.region, int(time.time() // PRESIGN_TTL))
