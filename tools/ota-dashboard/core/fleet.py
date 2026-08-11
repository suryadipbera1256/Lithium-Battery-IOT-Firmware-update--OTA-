"""Fleet discovery with a fast path and a guaranteed fallback.

Fast path  : iot.search_index -- ONE paginated call returns thing names AND
             live connectivity, so pre-flight validation needs no extra API
             traffic. Requires fleet indexing (see README).
Fallback   : iot.list_things / list_things_in_thing_group -- always available,
             but yields no connectivity, so the UI degrades honestly instead
             of pretending every node is online.

Both paths are O(n) over things with pagination handled by botocore, and the
whole result is memoised for `fleet_ttl_seconds`, so flipping between pages
or toggling checkboxes costs zero API calls.
"""
from __future__ import annotations

import time
from dataclasses import dataclass

import streamlit as st
from botocore.exceptions import ClientError

from core.aws import AWS_ERRORS, explain, iot
from core.settings import settings

# Fleet-index errors that legitimately mean "fall back to the registry",
# as opposed to a credential/permission problem worth surfacing.
_FALLBACK_CODES = frozenset({
    "IndexNotReadyException", "InvalidRequestException",
    "ResourceNotFoundException", "AccessDeniedException",
})


@dataclass(frozen=True, slots=True)
class Node:
    name: str
    connected: bool | None      # None => unknown (index disabled)
    thing_type: str
    version: str                # from the registry attribute, if maintained

    @property
    def status(self) -> str:
        if self.connected is None:
            return "unknown"
        return "online" if self.connected else "offline"


@dataclass(frozen=True, slots=True)
class Fleet:
    nodes: tuple[Node, ...]
    source: str                 # "fleet-index" | "registry" | "unavailable"
    note: str
    error: str = ""             # non-empty => discovery failed outright

    @property
    def names(self) -> tuple[str, ...]:
        return tuple(n.name for n in self.nodes)

    @property
    def online(self) -> int:
        return sum(1 for n in self.nodes if n.connected)

    @property
    def unknown(self) -> int:
        return sum(1 for n in self.nodes if n.connected is None)

    def index(self) -> dict[str, Node]:
        """name -> Node, so selection lookups are O(1) not O(n)."""
        return {n.name: n for n in self.nodes}


def _via_index(query: str) -> list[Node]:
    """SearchIndex has NO botocore paginator (get_paginator raises
    OperationNotPageableError), so the nextToken loop is hand-rolled.
    maxResults is capped at 250 by the API."""
    client = iot()
    out: list[Node] = []
    token: str | None = None
    while True:
        kwargs: dict = {"queryString": query, "maxResults": 250}
        if token:
            kwargs["nextToken"] = token
        resp = client.search_index(**kwargs)
        for doc in resp.get("things", []):
            conn = doc.get("connectivity") or {}
            attrs = doc.get("attributes") or {}
            out.append(
                Node(
                    name=doc["thingName"],
                    connected=bool(conn.get("connected")) if conn else None,
                    thing_type=doc.get("thingTypeName", "") or "",
                    version=attrs.get("fw_version", "") or attrs.get("version", "") or "",
                )
            )
        token = resp.get("nextToken")
        if not token:
            return out


def _via_registry(group: str, thing_type: str) -> list[Node]:
    client = iot()
    if group:
        pages = client.get_paginator("list_things_in_thing_group").paginate(
            thingGroupName=group
        )
        names = [n for page in pages for n in page.get("things", [])]
        return [Node(name=n, connected=None, thing_type="", version="") for n in names]

    kwargs = {"thingTypeName": thing_type} if thing_type else {}
    pages = client.get_paginator("list_things").paginate(**kwargs)
    return [
        Node(
            name=t["thingName"],
            connected=None,
            thing_type=t.get("thingTypeName", "") or "",
            version=(t.get("attributes") or {}).get("fw_version", ""),
        )
        for page in pages
        for t in page.get("things", [])
    ]


@st.cache_data(show_spinner="Discovering fleet...")
def _discover(group: str, thing_type: str, _ttl_bucket: int) -> Fleet:
    query_parts = ["thingName:*"]
    if group:
        query_parts = [f"thingGroupNames:{group}"]
    if thing_type:
        query_parts.append(f"thingTypeName:{thing_type}")
    query = " AND ".join(query_parts)

    try:
        nodes = _via_index(query)
        if nodes:
            return Fleet(tuple(sorted(nodes, key=lambda n: n.name)),
                         "fleet-index",
                         "Live connectivity from the AWS IoT fleet index.")
    except ClientError as exc:
        if exc.response.get("Error", {}).get("Code", "") not in _FALLBACK_CODES:
            return Fleet((), "unavailable", "", explain(exc))
    except AWS_ERRORS as exc:
        # Credential / region / endpoint failure: the registry path would fail
        # identically, so report instead of retrying.
        return Fleet((), "unavailable", "", explain(exc))

    try:
        nodes = _via_registry(group, thing_type)
    except AWS_ERRORS as exc:
        return Fleet((), "unavailable", "", explain(exc))

    return Fleet(
        tuple(sorted(nodes, key=lambda n: n.name)),
        "registry",
        "Fleet indexing unavailable -- connectivity unknown. Enable it with: "
        "aws iot update-indexing-configuration "
        "--thing-indexing-configuration thingIndexingMode=REGISTRY_AND_SHADOW,"
        "thingConnectivityIndexingMode=STATUS",
    )


def discover(refresh: bool = False) -> Fleet:
    cfg = settings()
    if refresh:
        _discover.clear()
    # A coarse time bucket gives cache_data a TTL we control from secrets
    # without hard-coding it in the decorator (ttl= must be a literal).
    bucket = int(time.time() // max(cfg.fleet_ttl_seconds, 1))
    return _discover(cfg.thing_group, cfg.thing_type, bucket)


# ---------------------------------------------------------------- pre-flight

@dataclass(frozen=True, slots=True)
class PreFlight:
    blocking: tuple[str, ...]
    advisory: tuple[str, ...]

    @property
    def clear(self) -> bool:
        return not self.blocking


def preflight(selected: tuple[str, ...], fleet: Fleet, max_per_minute: int) -> PreFlight:
    """Validate a target set before CreateJob. O(len(selected))."""
    blocking: list[str] = []
    advisory: list[str] = []
    idx = fleet.index()

    if not selected:
        blocking.append("No target nodes selected.")

    missing = [n for n in selected if n not in idx]
    if missing:
        blocking.append(f"{len(missing)} selected node(s) are not in the registry: "
                        f"{', '.join(missing[:5])}"
                        + (" ..." if len(missing) > 5 else ""))

    offline = [n for n in selected if idx.get(n) and idx[n].connected is False]
    if offline:
        advisory.append(
            f"{len(offline)} node(s) currently offline. The job stays QUEUED and "
            "is delivered on their next connect -- no action needed."
        )

    unknown = [n for n in selected if idx.get(n) and idx[n].connected is None]
    if unknown:
        advisory.append(f"Connectivity unknown for {len(unknown)} node(s) "
                        "(fleet indexing off).")

    if selected and len(selected) == len(fleet.nodes) and len(selected) > 1:
        advisory.append(
            f"This targets the ENTIRE fleet ({len(selected)} nodes). Rollout is "
            f"rate-limited to {max_per_minute}/min, but consider a canary batch first."
        )

    if max_per_minute <= 0:
        blocking.append("Rollout rate must be at least 1 per minute.")

    return PreFlight(tuple(blocking), tuple(advisory))
