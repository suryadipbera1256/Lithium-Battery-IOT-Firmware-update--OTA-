"""Shared test scaffolding. No pytest dependency -- these run as plain scripts
so the dashboard's requirements.txt stays production-only."""
from __future__ import annotations

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# AppTest.from_file() resolves relative paths against the CALLING file, not the
# CWD, so tests must pass this absolute path.
APP = str(ROOT / "app.py")

# st.secrets and the app's own relative paths resolve against the CWD.
os.chdir(ROOT)
sys.path.insert(0, str(ROOT))

_state = {"fails": 0, "total": 0}


def check(name: str, cond: bool, extra: str = "") -> bool:
    _state["total"] += 1
    if not cond:
        _state["fails"] += 1
    print(("  PASS  " if cond else "  FAIL  ") + name + (f"   {extra}" if extra else ""))
    return bool(cond)


def section(title: str) -> None:
    print(f"\n=== {title} ===")


def finish() -> None:
    f, t = _state["fails"], _state["total"]
    print(f"\n{t - f}/{t} checks passed" + (f"  ({f} FAILED)" if f else ""))
    raise SystemExit(1 if f else 0)


def require_secrets() -> None:
    p = ROOT / ".streamlit" / "secrets.toml"
    if not p.exists():
        print("SKIP: .streamlit/secrets.toml is required for AppTest runs.\n"
              "      Copy secrets.toml.example (values may be dummies).")
        raise SystemExit(0)
