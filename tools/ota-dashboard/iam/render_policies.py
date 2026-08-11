#!/usr/bin/env python3
"""Strip documentation keys from the annotated IAM policies so AWS accepts them.

The checked-in policy files carry `Comment` / `_README` / `_NOTES` keys that
explain *why* each statement exists. IAM's policy grammar is strict: any key in a
Statement other than Sid/Effect/Action/NotAction/Resource/NotResource/Principal/
NotPrincipal/Condition is rejected with MalformedPolicyDocument.

Rather than keep an annotated copy and a clean copy in sync (they would drift),
the annotated file stays the single source of truth and the apply-ready JSON is
derived from it here.

    python iam/render_policies.py out/

Writes:
    dashboard-policy.json      <- ecs-task-role-policy.json
    exec-policy.json           <- ecs-task-execution-role-policy.json
    presign-trust.json         <- iot-presign-role.json .trust_policy
    presign-perms.json         <- iot-presign-role.json .permissions_policy
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

STATEMENT_KEYS = {
    "Sid", "Effect", "Action", "NotAction", "Resource", "NotResource",
    "Principal", "NotPrincipal", "Condition",
}
HERE = Path(__file__).resolve().parent


def clean(doc: dict) -> dict:
    """Keep Version + Statement; drop every annotation key, recursively."""
    return {
        "Version": doc.get("Version", "2012-10-17"),
        "Statement": [
            {k: v for k, v in stmt.items() if k in STATEMENT_KEYS}
            for stmt in doc["Statement"]
        ],
    }


def main(outdir: str = "out") -> int:
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)

    simple = {
        "dashboard-policy.json": "ecs-task-role-policy.json",
        "exec-policy.json": "ecs-task-execution-role-policy.json",
    }
    for dst, src in simple.items():
        doc = json.loads((HERE / src).read_text(encoding="utf-8"))
        (out / dst).write_text(json.dumps(clean(doc), indent=2), encoding="utf-8")
        print(f"{dst:<22} <- {src}")

    presign = json.loads((HERE / "iot-presign-role.json").read_text(encoding="utf-8"))
    for dst, key in (("presign-trust.json", "trust_policy"),
                     ("presign-perms.json", "permissions_policy")):
        (out / dst).write_text(json.dumps(clean(presign[key]), indent=2), encoding="utf-8")
        print(f"{dst:<22} <- iot-presign-role.json .{key}")

    # Placeholders left unsubstituted are the single most common cause of a
    # policy that applies cleanly but never matches anything at runtime.
    for f in sorted(out.glob("*.json")):
        body = f.read_text(encoding="utf-8")
        for token in ("ACCOUNT_ID", "REGION", "CMK_KEY_ID"):
            if token in body:
                print(f"  WARNING: {f.name} still contains {token}")
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
