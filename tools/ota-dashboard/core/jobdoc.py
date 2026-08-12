"""AWS IoT Job document builder, written against the ACTUAL firmware parser.

Two hard constraints come from include/EC200U_AWS_OTA.h and must not be
violated by anything this dashboard emits:

1. otaCheckDownlink() locates the download URL with
       strstr(urcLine, "https://")
   i.e. the FIRST "https://" in the whole URC line wins. Therefore
   `firmwareUrl` MUST be the first key in the document, and no other field
   may ever contain a URL. We enforce both here.

2. The URC is read into a 1536 B buffer (`_otaUrl[1536]`, main.cpp raised its
   read buffer to 1536 for exactly this reason). The presigned URL expands to
   roughly 550-900 B at delivery time, so the document has a real byte budget.
   estimate_delivered_size() models the post-expansion length and the UI
   blocks a push that would overflow the device buffer.

Conditional-OTA gate fields (minCsq / minKbps / maxRetries) are additive and
backward-compatible: an older image simply ignores them.
See docs/firmware-contract.md for the ~15-line firmware patch that consumes
them.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass

_URL_RE = re.compile(r"https?://", re.IGNORECASE)
# Signature block appended by AWS when the placeholder is expanded:
# X-Amz-Algorithm/Credential/Date/Expires/SignedHeaders/Signature/Security-Token
PRESIGN_EXPANSION_BYTES = 620
_PLACEHOLDER = "${{aws:iot:s3-presigned-url:{url}}}"


@dataclass(frozen=True, slots=True)
class NetworkGate:
    """Conditional-OTA preconditions evaluated on the edge device."""
    min_csq: int          # AT+CSQ RSSI units, 0-31 (99 = unknown)
    min_kbps: int         # estimated downlink throughput floor
    max_retries: int      # device-side retries before reporting FAILED
    require_charging: bool = False


@dataclass(frozen=True, slots=True)
class BuiltDoc:
    document: str
    delivered_estimate: int
    budget: int
    base_url: str
    errors: tuple[str, ...]

    @property
    def within_budget(self) -> bool:
        return self.delivered_estimate <= self.budget

    @property
    def ok(self) -> bool:
        return not self.errors and self.within_budget


def s3_https_url(region: str, bucket: str, key: str) -> str:
    """Path-style S3 URL -- the form the presigned-URL placeholder expects."""
    return f"https://s3.{region}.amazonaws.com/{bucket}/{key.lstrip('/')}"


def build(
    *,
    region: str,
    bucket: str,
    key: str,
    version: str,
    gate: NetworkGate,
    sha256: str | None,
    size: int | None,
    urc_budget: int,
) -> BuiltDoc:
    base = s3_https_url(region, bucket, key)

    # firmwareUrl FIRST -- constraint (1).
    doc: dict[str, object] = {
        "firmwareUrl": _PLACEHOLDER.format(url=base),
        "version": version,
        "minCsq": int(gate.min_csq),
        "minKbps": int(gate.min_kbps),
        "maxRetries": int(gate.max_retries),
    }
    if size:
        doc["size"] = int(size)
    if sha256:
        doc["sha256"] = sha256[:16]  # 64 bits is ample for integrity signalling
    if gate.require_charging:
        doc["requireCharging"] = True

    # separators=(",", ":") -- every byte counts against the URC budget.
    document = json.dumps(doc, separators=(",", ":"))

    errors: list[str] = []
    if not version:
        errors.append("Version string is required.")

    # Constraint (1) guard: the first "https://" in the document must be the
    # one inside firmwareUrl, or the device flashes the wrong thing.
    if any(_URL_RE.search(str(v)) for k, v in doc.items() if k != "firmwareUrl"):
        errors.append("A non-firmwareUrl field contains a URL; the device "
                      "parser would latch onto the wrong one.")
    elif document.find("https://") != document.find(base):
        errors.append("firmwareUrl is not the first URL in the document.")

    delivered = len(document) + PRESIGN_EXPANSION_BYTES
    return BuiltDoc(
        document=document,
        delivered_estimate=delivered,
        budget=urc_budget,
        base_url=base,
        errors=tuple(errors),
    )
