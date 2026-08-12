"""Security gate: st.secrets-backed password login with role separation.

Two roles so the destructive capability is separable from read access:
  operator -> may CreateJob / CancelJob / upload firmware
  viewer   -> fleet discovery, job tracking, live telemetry only

Constant-time comparison (hmac.compare_digest) and a per-session attempt
counter.  This is a gate, not a perimeter -- see README for putting the
process behind Cognito/ALB or a VPN, which is the real control.
"""
from __future__ import annotations

import hmac
from typing import Literal

import streamlit as st

from core.settings import secrets_health, settings

Role = Literal["operator", "viewer"]
_KEY = "auth_role"


def role() -> Role | None:
    return st.session_state.get(_KEY)


def is_operator() -> bool:
    return st.session_state.get(_KEY) == "operator"


def _match(candidate: str) -> Role | None:
    cfg = settings()
    # Evaluate both branches; no early return, so timing does not leak which
    # password matched.
    op = bool(cfg.operator_password) and hmac.compare_digest(
        candidate, cfg.operator_password
    )
    vw = bool(cfg.viewer_password) and hmac.compare_digest(
        candidate, cfg.viewer_password
    )
    if op:
        return "operator"
    if vw:
        return "viewer"
    return None


def require_login() -> Role:
    """Render the gate and halt the script until authenticated."""
    existing = role()
    if existing:
        return existing

    cfg = settings()
    attempts = st.session_state.setdefault("auth_attempts", 0)

    st.markdown(
        """
        <div class="gate-wrap">
          <div class="gate-mark">AS</div>
          <h1 class="gate-title">Fleet OTA &amp; Diagnostics</h1>
          <!-- h1/p classes are targeted by over-specified CSS rules; see theme.css -->
          <p class="gate-sub">ESP32 &middot; EC200U-CN &middot; AWS IoT Core &middot; ap-south-1</p>
        </div>
        """,
        unsafe_allow_html=True,
    )

    if not (cfg.operator_password or cfg.viewer_password):
        st.error("No passwords configured — cannot authenticate.", icon="⛔")
        st.info(secrets_health(), icon="🔎")
        st.stop()

    if attempts >= cfg.max_attempts:
        st.error("Locked out. Restart the browser session to retry.")
        st.stop()

    with st.form("gate", border=True):
        pwd = st.text_input("Access key", type="password", label_visibility="collapsed",
                            placeholder="Operator or viewer password")
        ok = st.form_submit_button("Authenticate", use_container_width=True, type="primary")

    if ok:
        matched = _match(pwd)
        if matched:
            st.session_state[_KEY] = matched
            st.session_state["auth_attempts"] = 0
            st.rerun()
        st.session_state["auth_attempts"] = attempts + 1
        st.error(f"Rejected. {cfg.max_attempts - attempts - 1} attempt(s) left.")

    st.stop()


def logout() -> None:
    st.session_state.pop(_KEY, None)
    st.rerun()
