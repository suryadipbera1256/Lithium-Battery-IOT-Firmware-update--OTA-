"""Presentation primitives. CSS is injected once per session."""
from __future__ import annotations

from pathlib import Path

import streamlit as st

_CSS = Path(__file__).resolve().parent.parent / "static" / "theme.css"

_PILL = {
    "SUCCEEDED": ("ok", "SUCCEEDED"),
    "IN_PROGRESS": ("run", "IN PROGRESS"),
    "QUEUED": ("wait", "QUEUED"),
    "SCHEDULED": ("wait", "SCHEDULED"),
    "FAILED": ("bad", "FAILED"),
    "TIMED_OUT": ("bad", "TIMED OUT"),
    "REJECTED": ("bad", "REJECTED"),
    "CANCELED": ("mute", "CANCELED"),
    "REMOVED": ("mute", "REMOVED"),
    "COMPLETED": ("ok", "COMPLETED"),
    "online": ("ok", "ONLINE"),
    "offline": ("mute", "OFFLINE"),
    "unknown": ("wait", "UNKNOWN"),
}


@st.cache_data(show_spinner=False)
def _read_sheet(_mtime: float) -> str:
    """Disk read is cached; the <style> ELEMENT is not."""
    try:
        return _CSS.read_text(encoding="utf-8")
    except OSError:
        return ""


def _sheet() -> str:
    # Cache key includes mtime, so editing theme.css hot-reloads on the next
    # rerun instead of requiring a server restart.
    try:
        mtime = _CSS.stat().st_mtime
    except OSError:
        return ""
    return _read_sheet(mtime)


def inject_css() -> None:
    """Emit the stylesheet on EVERY run.

    Do not "optimise" this with a session_state once-guard: Streamlit
    reconciles the element tree positionally on each rerun, so an element that
    is not re-emitted is removed from the DOM. Skipping it after the first run
    silently drops all styling from the second run onward (verified in-browser:
    .sec-head fell back to display:block with default heading sizes). The read
    is cached, so the recurring cost is just re-sending the string.
    """
    sheet = _sheet()
    if sheet:
        st.markdown(f"<style>{sheet}</style>", unsafe_allow_html=True)


def pill(status: str) -> str:
    cls, label = _PILL.get(status, ("mute", status))
    return f'<span class="pill pill-{cls}">{label}</span>'


def header(title: str, subtitle: str = "", right: str = "") -> None:
    sub = f'<p class="sec-sub">{subtitle}</p>' if subtitle else ""
    st.markdown(
        f'<div class="sec-head"><div><h2 class="sec-title">{title}</h2>{sub}</div>'
        f'<div class="sec-right">{right}</div></div>',
        unsafe_allow_html=True,
    )


def stat(label: str, value: str, sub: str = "", tone: str = "") -> str:
    tail = f'<div class="stat-s">{sub}</div>' if sub else ""
    return (
        f'<div class="stat {tone}"><div class="stat-l">{label}</div>'
        f'<div class="stat-v">{value}</div>{tail}</div>'
    )


def stat_row(cards: list[str]) -> None:
    st.markdown(f'<div class="stat-row">{"".join(cards)}</div>',
                unsafe_allow_html=True)


def kv(rows: list[tuple[str, str]]) -> None:
    body = "".join(
        f'<div class="kv-r"><span class="kv-k">{k}</span>'
        f'<span class="kv-v">{v}</span></div>'
        for k, v in rows
    )
    st.markdown(f'<div class="kv">{body}</div>', unsafe_allow_html=True)
