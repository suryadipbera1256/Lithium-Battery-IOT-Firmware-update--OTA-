"""Run every dashboard test. No pytest, no extra dependencies.

    .venv\\Scripts\\python.exe tests\\run_all.py      (PowerShell)
    ./.venv/bin/python tests/run_all.py               (bash)

test_logic.py needs nothing. The two AppTest suites need a
.streamlit/secrets.toml to exist (dummy values are fine) and skip cleanly
without one.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SUITES = ("test_logic.py", "test_pages_degraded.py", "test_pages_stubbed.py")

failed: list[str] = []
for name in SUITES:
    # flush=True: the child inherits stdout, so an unflushed parent buffer
    # would print all the banners after all the output.
    print(f"\n{'=' * 62}\n  {name}\n{'=' * 62}", flush=True)
    rc = subprocess.run([sys.executable, str(HERE / name)]).returncode
    if rc != 0:
        failed.append(name)

print(f"\n{'=' * 62}", flush=True)
if failed:
    print("FAILED: " + ", ".join(failed))
    raise SystemExit(1)
print(f"All {len(SUITES)} suites passed.")
