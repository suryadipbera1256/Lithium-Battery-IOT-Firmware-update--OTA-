"""Degraded-path render test: every page must show an actionable message
instead of a traceback when AWS credentials are absent or wrong.

Regression guard for two bugs this caught:
  * iot.get_paginator("search_index") -> OperationNotPageableError (SearchIndex
    has no botocore paginator), which crashed three of the four pages.
  * NoCredentialsError escaping to the UI as a raw traceback.
"""
import os
import time

from _bootstrap import APP, check, finish, require_secrets, section  # noqa: E402
from streamlit.testing.v1 import AppTest  # noqa: E402

require_secrets()

# No AWS credentials on purpose: this tests the degraded path an operator with
# a half-configured machine will actually hit.
for k in ("AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN",
          "AWS_PROFILE", "AWS_DEFAULT_PROFILE"):
    os.environ.pop(k, None)
os.environ["AWS_EC2_METADATA_DISABLED"] = "true"
os.environ["AWS_CONFIG_FILE"] = "nonexistent"
os.environ["AWS_SHARED_CREDENTIALS_FILE"] = "nonexistent"

PAGES = ("Firmware Registry", "Fleet & Deploy", "Job Tracking", "Live Telemetry")
ARTIFACT = {
    "key": "firmware/1.0.3/firmware.bin", "version": "1.0.3", "size": 1_103_456,
    "sha256": "9f2c1ab34de5f607aa11bb22cc33dd44ee55ff66778899aabbccddeeff001122",
    "embedded": "1.0.3", "etag": "deadbeef", "published": time.time(),
}
def launch(page, role="operator", artifact=ARTIFACT, **state):
    at = AppTest.from_file(APP, default_timeout=45)
    at.session_state["auth_role"] = role
    if artifact:
        at.session_state["artifact"] = artifact
    for k, v in state.items():
        at.session_state[k] = v
    at.run()
    if at.sidebar.radio:
        at.sidebar.radio[0].set_value(page).run()
    return at


def first_exc(at):
    return at.exception[0].value.splitlines()[0] if at.exception else ""


section("every page renders without a traceback, no AWS credentials")
for p in PAGES:
    at = launch(p)
    ok = check(f"{p}: no unhandled exception", not at.exception, first_exc(at))
    if ok and p != "Firmware Registry":
        msgs = " ".join(str(e.value) for e in at.error) + \
               " ".join(str(w.value) for w in at.warning)
        check(f"{p}: credential problem explained to the operator",
              "credential" in msgs.lower(), msgs[:100])

section("deploy page with no artefact staged")
at = launch("Fleet & Deploy", artifact=None)
check("no exception", not at.exception, first_exc(at))
check("tells the operator to publish firmware first",
      any("Firmware Registry" in str(i.value) for i in at.info))

section("viewer role must not get write controls")
at = launch("Firmware Registry", role="viewer")
check("no exception", not at.exception, first_exc(at))
uploaders = list(at.get("file_uploader"))
check("upload widget disabled for viewer",
      all(getattr(u, "disabled", True) for u in uploaders) if uploaders else True)
check("read-only role is stated in the sidebar",
      any("Read-only role" in str(c.value) for c in at.sidebar.caption))

section("unauthenticated access")
at = AppTest.from_file(APP, default_timeout=45)
at.run()
check("gate form is shown", any("Authenticate" in str(b.label) for b in at.button))
check("navigation does not leak before login", not at.sidebar.radio)

finish()
