"""Cached boto3 session + clients.

Clients are expensive to construct (endpoint resolution, credential
resolution, botocore model loading ~100ms each) and are thread-safe once
built, so they live in @st.cache_resource and survive every rerun.
"""
from __future__ import annotations

import boto3
import streamlit as st
from botocore.config import Config
from botocore.exceptions import (BotoCoreError, ClientError, EndpointConnectionError,
                                 NoCredentialsError, NoRegionError,
                                 ProfileNotFound)

from core.settings import settings

# Everything botocore can raise that we want to render as a clean message
# rather than a traceback. ClientError is API-level; BotoCoreError covers
# credential/endpoint/config failures, which are the common local misconfigs.
AWS_ERRORS = (ClientError, BotoCoreError)

_CFG = Config(
    retries={"max_attempts": 4, "mode": "adaptive"},
    connect_timeout=5,
    read_timeout=20,
    tcp_keepalive=True,
    user_agent_extra="as-ai-ota-dashboard/1.0",
)


@st.cache_resource(show_spinner=False)
def session() -> boto3.Session:
    cfg = settings()
    if cfg.access_key_id and cfg.secret_access_key:
        return boto3.Session(
            aws_access_key_id=cfg.access_key_id,
            aws_secret_access_key=cfg.secret_access_key,
            aws_session_token=cfg.session_token or None,
            region_name=cfg.region,
        )
    if cfg.profile:
        return boto3.Session(profile_name=cfg.profile, region_name=cfg.region)
    return boto3.Session(region_name=cfg.region)  # default credential chain


@st.cache_resource(show_spinner=False)
def iot():
    return session().client("iot", config=_CFG)


@st.cache_resource(show_spinner=False)
def s3():
    return session().client("s3", config=_CFG)


@st.cache_resource(show_spinner=False)
def sts():
    return session().client("sts", config=_CFG)


@st.cache_data(ttl=900, show_spinner=False)
def identity() -> dict:
    """Who am I -- shown in the sidebar so the operator can see which
    credentials are live before pushing firmware."""
    try:
        d = sts().get_caller_identity()
        return {"account": d.get("Account", "?"), "arn": d.get("Arn", "?")}
    except Exception as exc:  # noqa: BLE001 - surfaced in UI, never fatal
        return {"account": "-", "arn": f"unavailable ({type(exc).__name__})"}


def explain(exc: Exception) -> str:
    """Turn a botocore failure into something an operator can act on."""
    if isinstance(exc, NoCredentialsError):
        return ("No AWS credentials resolved. Set `[aws].access_key_id` / "
                "`secret_access_key` (or `profile`) in "
                "`.streamlit/secrets.toml`, or configure the default chain.")
    if isinstance(exc, ProfileNotFound):
        return f"AWS profile not found: {exc}"
    if isinstance(exc, NoRegionError):
        return "No AWS region configured. Set `[aws].region` in secrets.toml."
    if isinstance(exc, EndpointConnectionError):
        return f"Cannot reach the AWS endpoint — check network/proxy. ({exc})"
    if isinstance(exc, ClientError):
        err = exc.response.get("Error", {})
        code = err.get("Code", "ClientError")
        msg = err.get("Message", str(exc))
        if code in ("AccessDeniedException", "AccessDenied",
                    "UnauthorizedOperation", "NotAuthorized"):
            return (f"Access denied ({code}): {msg}  →  compare the current "
                    "identity against iam/ecs-task-role-policy.json.")
        if code in ("ExpiredTokenException", "ExpiredToken",
                    "InvalidClientTokenId", "SignatureDoesNotMatch"):
            return f"Credentials rejected ({code}): {msg}"
        return f"{code}: {msg}"
    return f"{type(exc).__name__}: {exc}"


def frozen_credentials():
    """Resolved credentials for SigV4 WSS presigning."""
    creds = session().get_credentials()
    if creds is None:
        raise RuntimeError("No AWS credentials resolved.")
    return creds.get_frozen_credentials()
