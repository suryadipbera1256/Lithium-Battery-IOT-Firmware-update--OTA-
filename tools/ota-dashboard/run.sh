#!/usr/bin/env bash
# =========================================================
# AS AI — OTA Dashboard launcher (bash / Git Bash / Linux)
# Uses a venv INSIDE this folder only; never the repo-root .venv.
# =========================================================
set -euo pipefail
cd "$(dirname "$0")"

VENV=".venv"
PY="$VENV/bin/python"
[ -x "$PY" ] || PY="$VENV/Scripts/python.exe"   # Git Bash on Windows

if [ ! -x "$PY" ]; then
  echo "[setup] Creating isolated venv at $VENV"
  python -m venv "$VENV"
  PY="$VENV/bin/python"; [ -x "$PY" ] || PY="$VENV/Scripts/python.exe"
  "$PY" -m pip install --upgrade pip --quiet
  "$PY" -m pip install -r requirements.txt
fi

if [ ! -f ".streamlit/secrets.toml" ]; then
  echo "[error] .streamlit/secrets.toml is missing."
  echo "        cp .streamlit/secrets.toml.example .streamlit/secrets.toml"
  exit 1
fi

exec "$PY" -m streamlit run app.py --server.address 127.0.0.1 --server.port 8501
