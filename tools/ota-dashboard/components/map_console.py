"""AS AI — Fleet Map Console (Spatial Tracking, Route Playback, Geofencing)
=============================================================================
Isolated component for fleet geospatial rendering. Owns nothing outside this
module: Firmware Registry, Deploy, Job Tracking and Live Telemetry are untouched.

LAYOUT CONTRACT
    Block A  Fleet status metrics          full width, top
    Block B  Filtering & battery selection  control panel
    Block C  Route playback (historical)    control panel
    Block D  Geofencing & zone management   control panel
    Map + fleet grid                        right pane

ADDRESSES, NOT COORDINATES
    The panel shows human-readable places (reverse-geocoded via Nominatim).
    Exact lat/lng stays in the map popup/tooltip, where precision is actually
    useful and does not compete with the rest of the UI for attention.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import random
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from functools import lru_cache
from typing import Any, Dict, List, Literal, Optional, Tuple
from urllib.parse import quote

import pandas as pd
import pydeck as pdk
import streamlit as st

from components.ui import header, stat, stat_row
from core import fleet as fl

try:
    import folium
    from folium.plugins import Draw
    from streamlit_folium import st_folium
    HAS_FOLIUM = True
except ImportError:                                    # pragma: no cover
    HAS_FOLIUM = False

try:
    from geopy.geocoders import Nominatim
    HAS_GEOPY = True
except ImportError:                                    # pragma: no cover
    HAS_GEOPY = False


# ==============================================================================
# 1. TYPES & CONSTANTS
# ==============================================================================

FleetState = Literal["Moving", "Stopped", "Idle", "Towing/Theft", "Offline"]

# (RGBA for PyDeck, hex for Folium/HTML) — single source for both renderers.
STATE_COLORS: Dict[FleetState, Tuple[Tuple[int, int, int, int], str]] = {
    "Moving":       ((0, 224, 164, 220), "#00e0a4"),
    "Stopped":      ((255, 93, 108, 220), "#ff5d6c"),
    "Idle":         ((255, 179, 64, 220), "#ffb340"),
    "Towing/Theft": ((170, 80, 240, 220), "#aa50f0"),
    "Offline":      ((139, 152, 171, 180), "#8b98ab"),
}
ALL_STATES: List[FleetState] = list(STATE_COLORS)

HUB_PRESETS: Dict[str, Dict[str, Any]] = {
    "Kolkata R&D Hub":    {"lat": 22.7525975, "lng": 88.3912191, "radius_m": 3000.0,
                           "polygon": [[88.3712, 22.7726], [88.4112, 22.7726],
                                       [88.4112, 22.7326], [88.3712, 22.7326]]},
    "Mumbai Port Hub":    {"lat": 18.9553, "lng": 72.8465, "radius_m": 3500.0,
                           "polygon": [[72.8300, 18.9700], [72.8650, 18.9700],
                                       [72.8650, 18.9350], [72.8300, 18.9350]]},
    "Bengaluru Tech Depot": {"lat": 12.9716, "lng": 77.5946, "radius_m": 4500.0,
                           "polygon": [[77.5700, 12.9900], [77.6200, 12.9900],
                                       [77.6200, 12.9500], [77.5700, 12.9500]]},
    "Delhi-NCR Corridor": {"lat": 28.6139, "lng": 77.2090, "radius_m": 6000.0,
                           "polygon": [[77.1700, 28.6400], [77.2400, 28.6400],
                                       [77.2400, 28.5800], [77.1700, 28.5800]]},
}

DEFAULT_CENTER = (22.7525975, 88.3912191)
STALE_AFTER_S = 120.0

# Viewport clamp: [[S, W], [N, E]] around the Indian subcontinent. Panning is
# bounded to this box and zoom-out is floored, so the browser never requests the
# global tile pyramid — that request set is what dominates first-paint bandwidth
# and tile-cache RAM.
INDIA_BOUNDS = [[6.5546079, 68.1113787], [35.6745457, 97.395561]]
MIN_ZOOM = 5      # z4 and below is whole-hemisphere tiles nobody here needs
MAX_ZOOM = 18

# session_state keys, declared once so nothing drifts
K_CUSTOM_HUBS = "custom_hubs"        # name -> zone dict (drawn zones)
K_SEEN_SHAPES = "map_seen_shapes"    # fingerprints already turned into zones
K_ACTIVE_ZONE = "map_active_zone"    # selectbox value, so a new zone can preselect
K_ZONE_CENTRED = "map_zone_centred"   # last zone the map flew to (edge trigger)
K_VIEW = "map_view"                   # sticky (lat, lng, zoom) the camera holds
K_LAST_FOCUS = "map_last_focus"       # target unit, to detect a change
K_DRAW_REPORTED = "map_draw_reported"  # component has returned all_drawings once

# One definition for every geofence outline, so preset and custom zones cannot
# drift apart. `opacity` (the STROKE alpha) is set explicitly: leaving it to
# Leaflet's default meant overlapping zones compounded their fills and edges
# until the stack rendered near-black.
ZONE_STYLE: Dict[str, Dict[str, Any]] = {
    "active":   {"color": "#3388ff", "weight": 2.5, "opacity": 0.95,
                 "fill_color": "#3388ff", "fill_opacity": 0.20},
    "inactive": {"color": "#8b98ab", "weight": 1.5, "opacity": 0.50,
                 "fill_color": "#8b98ab", "fill_opacity": 0.05},
}


@dataclass(slots=True)
class SpatialTelemetryNode:
    """Spatial state for one vehicle, with its operating state derived once."""

    thing_name: str
    lat: float
    lng: float
    speed_kmh: float
    bms_current_a: float
    bms_voltage_v: float
    soc_pct: float
    last_seen_ts: float
    is_online: bool
    state: FleetState = "Offline"
    color_rgba: Tuple[int, int, int, int] = field(default=(139, 152, 171, 180))
    color_hex: str = "#8b98ab"

    def __post_init__(self) -> None:
        self.evaluate_state()

    def evaluate_state(self) -> FleetState:
        """Moving: speed>0. Idle: static, current flowing. Stopped: static, no
        current. Towing/Theft: moving with NO current — i.e. the pack is being
        transported, not driven. Offline: stale or never seen."""
        age_s = time.time() - self.last_seen_ts if self.last_seen_ts > 0 else 1e9

        if not self.is_online or age_s > STALE_AFTER_S or not self.has_fix:
            self.state = "Offline"
        elif self.speed_kmh > 0.5 and abs(self.bms_current_a) < 0.1:
            self.state = "Towing/Theft"
        elif self.speed_kmh > 0.5:
            self.state = "Moving"
        elif abs(self.bms_current_a) >= 0.1:
            self.state = "Idle"
        else:
            self.state = "Stopped"

        self.color_rgba, self.color_hex = STATE_COLORS[self.state]
        return self.state

    @property
    def has_fix(self) -> bool:
        return self.lat != 0.0 and self.lng != 0.0

    @property
    def last_seen_str(self) -> str:
        if self.last_seen_ts <= 0:
            return "Never"
        return datetime.fromtimestamp(self.last_seen_ts, tz=timezone.utc) \
                       .strftime("%Y-%m-%d %H:%M:%S UTC")


# ==============================================================================
# 2. GEOSPATIAL ENGINE
# ==============================================================================

def haversine_distance_m(lat1: float, lng1: float, lat2: float, lng2: float) -> float:
    """Great-circle distance in metres."""
    r_earth = 6371000.0
    dlat = math.radians(lat2 - lat1)
    dlng = math.radians(lng2 - lng1)
    a = (math.sin(dlat / 2.0) ** 2
         + math.cos(math.radians(lat1)) * math.cos(math.radians(lat2))
         * math.sin(dlng / 2.0) ** 2)
    return r_earth * 2.0 * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))


def point_in_polygon(lat: float, lng: float, polygon_lng_lat: List[List[float]]) -> bool:
    """Ray casting. `polygon_lng_lat` is [[lng, lat], ...] (GeoJSON order)."""
    n = len(polygon_lng_lat)
    if n < 3:
        return False
    inside = False
    p1lng, p1lat = polygon_lng_lat[0]
    for i in range(1, n + 1):
        p2lng, p2lat = polygon_lng_lat[i % n]
        if min(p1lat, p2lat) < lat <= max(p1lat, p2lat) and lng <= max(p1lng, p2lng):
            if p1lat != p2lat:
                xinters = (lat - p1lat) * (p2lng - p1lng) / (p2lat - p1lat) + p1lng
            else:
                xinters = p1lng
            if p1lng == p2lng or lng <= xinters:
                inside = not inside
        p1lng, p1lat = p2lng, p2lat
    return inside


def zone_contains(zone: Dict[str, Any], lat: float, lng: float) -> bool:
    """Polygon takes precedence over radius — the same precedence the renderer
    uses, so what is drawn is exactly what is evaluated."""
    poly = zone.get("polygon") or []
    if len(poly) >= 3:
        return point_in_polygon(lat, lng, poly)
    return haversine_distance_m(lat, lng, zone.get("lat", 0.0),
                                zone.get("lng", 0.0)) <= zone.get("radius_m", 0.0)


def _stable_seed(text: str) -> int:
    """Deterministic across processes.

    Python salts str.__hash__ per interpreter (PYTHONHASHSEED), so seeding an
    RNG with hash(name) produced a different synthetic fleet on every restart
    despite the docstring promising determinism.
    """
    return int.from_bytes(hashlib.sha256(text.encode()).digest()[:8], "big")


# ==============================================================================
# 3. REVERSE GEOCODING
# ==============================================================================

_GEO_PRECISION = 4          # ~11 m — GPS jitter must not mint new cache entries


@st.cache_data(ttl=86_400, max_entries=1024, show_spinner=False)
def reverse_geocode(lat_r: float, lng_r: float) -> str:
    """Coordinates -> "Locality, Region". Empty string when unavailable.

    Cached on ROUNDED coordinates and for a full day. Nominatim's usage policy
    allows ~1 request/second, so callers must also keep the number of distinct
    points small — see `resolve_place()`, which geocodes only the focused unit
    rather than every marker on the map.
    """
    if not HAS_GEOPY:
        return ""
    try:
        geocoder = Nominatim(user_agent="as-ai-fleet-ota-console/1.0", timeout=5)
        loc = geocoder.reverse((lat_r, lng_r), exactly_one=True, language="en", zoom=14)
        if not loc:
            return ""
        addr = getattr(loc, "raw", {}).get("address", {}) or {}
        locality = next(
            (addr[k] for k in ("suburb", "neighbourhood", "village", "town",
                               "city_district", "city", "municipality", "county")
             if addr.get(k)),
            "",
        )
        region = addr.get("state") or addr.get("state_district") or ""
        parts = [p for p in (locality, region) if p]
        if parts:
            return ", ".join(parts)
        return str(loc.address).split(",")[0] if loc.address else ""
    except Exception:
        # Network hiccup / rate limit / DNS: never break the page for a label.
        return ""


def resolve_place(lat: float, lng: float) -> str:
    """UI-facing wrapper: rounds, geocodes, and degrades gracefully."""
    if lat == 0.0 and lng == 0.0:
        return "No GPS fix"
    place = reverse_geocode(round(lat, _GEO_PRECISION), round(lng, _GEO_PRECISION))
    if place:
        return place
    return "Address unavailable" if HAS_GEOPY else "Geocoding disabled"


# ==============================================================================
# 4. SYNTHETIC DATA (until a telemetry history store exists)
# ==============================================================================

@st.cache_data(ttl=600, show_spinner=False)
def generate_synthetic_spatial_fleet(node_names: Tuple[str, ...]) -> Dict[str, Dict[str, Any]]:
    """Plausible spatial snapshots for nodes with no live GPS in session state."""
    preset_keys = list(HUB_PRESETS)
    spatial_db: Dict[str, Dict[str, Any]] = {}
    now = time.time()

    for idx, name in enumerate(node_names):
        hub = HUB_PRESETS[preset_keys[idx % len(preset_keys)]]
        rng = random.Random(_stable_seed(name))
        roll = rng.random()

        if roll < 0.35:                      # Moving
            speed, current = rng.uniform(15.0, 65.0), rng.uniform(5.0, 35.0)
        elif roll < 0.65:                    # Stopped
            speed, current = 0.0, 0.0
        elif roll < 0.85:                    # Idle
            speed, current = 0.0, rng.uniform(0.5, 4.0)
        else:                                # Towing/Theft
            speed, current = rng.uniform(20.0, 45.0), 0.0

        spatial_db[name] = {
            "lat": hub["lat"] + rng.uniform(-0.025, 0.025),
            "lng": hub["lng"] + rng.uniform(-0.025, 0.025),
            "speed_kmh": round(speed, 2),
            "bms_current_a": round(current, 2),
            "bms_voltage_v": round(rng.uniform(48.0, 54.6), 2),
            "soc_pct": round(rng.uniform(20.0, 98.0), 1),
            "last_seen_ts": now - rng.uniform(5.0, 90.0),
        }
    return spatial_db


@st.cache_data(ttl=3600, show_spinner=False)
def generate_historical_route(
    thing_name: str, start_lat: float, start_lng: float,
    ts_from: datetime, ts_to: datetime, num_points: int = 48,
) -> pd.DataFrame:
    """Breadcrumb trail spanning the requested window.

    Synthetic. Swap the body for a Timestream/S3 query when history is stored;
    the return shape (lat, lng, speed_kmh, soc_pct, timestamp) is the contract
    the renderer depends on.

    num_points is a FIXED sample budget, not a function of the window: the real
    query behind this must down-sample server-side, or a 24 h window on 30 s
    telemetry returns 2 880 rows per device and the cost scales with the slider.
    """
    rng = random.Random(_stable_seed(f"{thing_name}{ts_from}{ts_to}"))
    span_s = max((ts_to - ts_from).total_seconds(), 60.0)
    t0 = ts_from.timestamp()
    step = span_s / max(num_points - 1, 1)

    rows: List[Dict[str, Any]] = []
    lat, lng = start_lat - 0.03, start_lng - 0.04
    for i in range(num_points):
        lat += rng.uniform(0.0008, 0.0030)
        lng += rng.uniform(0.0008, 0.0030)
        moving = i not in (0, num_points - 1)
        rows.append({
            "thing_name": thing_name,
            "lat": lat,
            "lng": lng,
            "speed_kmh": round(rng.uniform(10.0, 55.0) if moving else 0.0, 1),
            "soc_pct": round(max(15.0, 95.0 - i * (80.0 / num_points)), 1),
            "timestamp": datetime.fromtimestamp(t0 + i * step, tz=timezone.utc)
                                 .strftime("%d %b %H:%M"),
        })
    return pd.DataFrame(rows)


# ==============================================================================
# 5. GEOFENCE CAPTURE
# ==============================================================================

def geojson_to_zone(feature: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Leaflet-Draw GeoJSON -> the internal zone dict.

    Circles arrive as a Point with `properties.radius` (metres); polygons and
    rectangles as a Polygon whose first ring is already [lng, lat].
    """
    try:
        geom = feature.get("geometry", {}) or {}
        props = feature.get("properties", {}) or {}
        gtype = geom.get("type")
        coords = geom.get("coordinates")

        if gtype == "Point" and coords:
            lng, lat = float(coords[0]), float(coords[1])
            has_r = props.get("radius") is not None
            # `radius_explicit` matters on the round trip: L.Circle.toGeoJSON()
            # emits a bare Point and DROPS the radius, so a saved circle coming
            # back from the map would otherwise be rewritten to the 1000 m
            # default. sync_drawn_zones() keeps the stored radius when this is
            # False.
            return {"lat": lat, "lng": lng,
                    "radius_m": float(props["radius"]) if has_r else 1000.0,
                    "polygon": [], "radius_explicit": has_r}

        if gtype == "Polygon" and coords:
            ring = [[float(p[0]), float(p[1])] for p in coords[0]]
            if len(ring) >= 3:
                # Leaflet closes the ring; drop the duplicate final vertex so the
                # ray-casting wrap-around does not count that edge twice.
                if ring[0] == ring[-1]:
                    ring = ring[:-1]
                lat_c = sum(p[1] for p in ring) / len(ring)
                lng_c = sum(p[0] for p in ring) / len(ring)
                return {"lat": lat_c, "lng": lng_c, "radius_m": 0.0,
                        "polygon": ring, "radius_explicit": True}
    except (TypeError, ValueError, IndexError, KeyError):
        pass
    return None


def zone_summary(zone: Dict[str, Any]) -> str:
    poly = zone.get("polygon") or []
    if len(poly) >= 3:
        return f"Polygon · {len(poly)} vertices"
    return f"Circle · {int(zone.get('radius_m', 0))} m radius"


def shape_fingerprint(feature: Dict[str, Any]) -> str:
    """Stable identity for a drawn shape.

    st_folium replays `all_drawings` on EVERY rerun, so identity is what stops a
    shape from being re-imported forever. Derived from the geometry itself (plus
    the circle radius, which lives in properties and is the only thing
    distinguishing two concentric circles) — never from list position, because
    deleting one shape on the map would renumber the rest and re-import them all.
    """
    geom = feature.get("geometry", {}) or {}
    radius = (feature.get("properties", {}) or {}).get("radius")
    payload = json.dumps(
        {"g": geom, "r": round(float(radius), 3) if radius is not None else None},
        sort_keys=True, separators=(",", ":"),
    )
    return hashlib.sha1(payload.encode()).hexdigest()[:16]


EDIT_MATCH_M = 400.0     # centroid drift still counted as "the same zone edited"


def _match_existing_zone(custom: Dict[str, Dict[str, Any]],
                         zone: Dict[str, Any]) -> Optional[str]:
    """Name of the saved zone this geometry is an edited version of, else None.

    Saved zones are re-rendered into the editable FeatureGroup every run, so they
    come back in `all_drawings` with a FRESH fingerprint whenever the operator
    nudges them. Fingerprint identity alone would therefore read every edit as a
    brand-new zone. Centroid proximity is what ties the edited geometry back to
    the zone it came from.

    O(n) over custom zones, which is a handful — the loop is not the cost here.
    """
    best, best_d = None, EDIT_MATCH_M
    for name, z in custom.items():
        d = haversine_distance_m(zone["lat"], zone["lng"], z["lat"], z["lng"])
        if d < best_d:
            best, best_d = name, d
    return best


def sync_drawn_zones(drawings: Optional[List[Dict[str, Any]]]) -> List[str]:
    """Reconcile map shapes into `custom_hubs`. Returns names created or updated.

    Idempotent by fingerprint, which is what keeps this free of rerun loops: a
    known fingerprint is skipped, so the caller's `st.rerun()` fires exactly once
    per real change and the next pass finds nothing to do.

    Saved zones are rendered INTO the editable FeatureGroup, so `all_drawings` is
    a complete snapshot of what is on the map — which makes deletion a set-diff
    rather than an event. streamlit-folium exposes no `deleted_features` /
    `edited_features` channel (verified against its source); `all_drawings` is
    the only drawing state it returns.

    `drawings is None` means the component has not reported yet and is ignored.
    An empty LIST is honoured only after at least one real report, so a
    first-paint blank cannot wipe live monitoring zones.
    """
    if drawings is None:
        return []
    st.session_state[K_DRAW_REPORTED] = True

    custom: Dict[str, Dict[str, Any]] = st.session_state.setdefault(K_CUSTOM_HUBS, {})
    seen: set = st.session_state.setdefault(K_SEEN_SHAPES, set())
    touched: List[str] = []

    # PHASE 1 — adds and edits. This MUST run before removals: an edit changes
    # the geometry and therefore the fingerprint, so a removal pass running first
    # would see the old fingerprint missing, delete the zone, and let phase 1
    # re-create it under a fresh default name — silently discarding any rename
    # and resetting the active-zone selection on every boundary tweak.
    for feature in drawings:
        fp = shape_fingerprint(feature)
        if fp in seen:
            continue
        zone = geojson_to_zone(feature)
        seen.add(fp)                       # mark even on failure: never retry a bad shape
        if not zone:
            continue

        match = _match_existing_zone(custom, zone)
        if match:
            prior = custom[match]
            # L.Circle.toGeoJSON() drops the radius, so a re-rendered circle
            # returns without one. Keep the stored value rather than silently
            # resizing the zone to the 1000 m default.
            if not zone.pop("radius_explicit", False) and not zone["polygon"]:
                zone["radius_m"] = prior.get("radius_m", zone["radius_m"])
            zone["fingerprint"] = fp
            custom[match] = zone
            touched.append(match)
            continue

        # A Point with NO properties.radius cannot be a user-drawn circle —
        # leaflet-draw always attaches the radius to circles it creates. Reaching
        # here means it is a marker artefact (a popup anchor, a geocoder pin),
        # and creating a zone from it is the ghost-circle failure. Matched
        # radius-less Points are fine: that is a saved circle round-tripping
        # through L.Circle.toGeoJSON(), which drops the radius.
        if not zone.pop("radius_explicit", False) and not zone["polygon"]:
            continue

        n = len(custom) + 1
        while f"Zone {n} (Custom)" in custom:
            n += 1
        name = f"Zone {n} (Custom)"
        zone["fingerprint"] = fp
        custom[name] = zone
        touched.append(name)

    # PHASE 2 — removals, against fingerprints as they stand AFTER phase 1, so a
    # zone that was merely edited is no longer a candidate for deletion.
    live_fps = {shape_fingerprint(f) for f in drawings}
    for name in [n for n, z in custom.items()
                 if z.get("fingerprint") and z["fingerprint"] not in live_fps]:
        custom.pop(name, None)
        if st.session_state.get(K_ACTIVE_ZONE) == name:
            st.session_state[K_ACTIVE_ZONE] = next(iter(HUB_PRESETS))
        touched.append(name)

    return touched


# ==============================================================================
# 6. MAP RENDERERS
# ==============================================================================

# ---------------------------------------------------------------- marker art
# One SVG, recoloured per state. An inline DivIcon is used rather than a CDN
# PNG because the marker must take the state colour at render time (a raster
# cannot), it needs no network fetch that a locked-down network could block,
# and it stays crisp at every zoom level.
_VEHICLE_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" '
    'width="{size}" height="{size}">'
    '<g filter="url(#s)">'
    '<path d="M16 31C16 31 27 21.8 27 13.6A11 11 0 1 0 5 13.6C5 21.8 16 31 16 31Z" '
    'fill="{fill}" stroke="#ffffff" stroke-width="{stroke}"/>'
    # battery body + terminal + bolt: reads as "battery asset" at 28-34 px
    '<rect x="9.5" y="9" width="11" height="9" rx="1.6" fill="#0d1420" opacity=".92"/>'
    '<rect x="20.6" y="11.4" width="1.9" height="4.2" rx=".8" fill="#0d1420" opacity=".92"/>'
    '<path d="M15.6 10.2 12.4 14.4h2.5l-1.1 3.4 3.4-4.4h-2.5z" fill="{fill}"/>'
    '</g>'
    '<defs><filter id="s" x="-30%" y="-30%" width="160%" height="160%">'
    '<feDropShadow dx="0" dy="1.2" stdDeviation="1.1" flood-opacity=".45"/>'
    '</filter></defs></svg>'
)

# Popup chrome. Injected ONCE into the map header instead of inlined on every
# marker: Leaflet's own .leaflet-popup-content-wrapper is opaque white with its
# own radius, so inline styles alone cannot stop the popup looking washed out —
# the wrapper and the tip have to be overridden. Doing it here also drops ~600 B
# of duplicated inline CSS per marker from the payload.
_POPUP_CSS = """
<style>
.leaflet-popup-content-wrapper{
  background:#0d1420 !important; color:#f2f5f9 !important;
  border:1px solid #24324a !important; border-radius:10px !important;
  box-shadow:0 14px 38px rgba(0,0,0,.6) !important; opacity:1 !important;
}
.leaflet-popup-content{ margin:0 !important; padding:0 !important; width:auto !important; }
.leaflet-popup-tip{ background:#0d1420 !important; border:1px solid #24324a !important;
                    box-shadow:none !important; }
.leaflet-popup-close-button{ color:#8b98ab !important; font-size:17px !important;
                             padding:6px 8px 0 0 !important; }
.leaflet-popup-close-button:hover{ color:#f2f5f9 !important; }
.vp{ font-family:'JetBrains Mono',ui-monospace,SFMono-Regular,Menlo,monospace;
     padding:.65rem .8rem .7rem; min-width:236px; }
.vp-h{ font-size:13px; font-weight:700; color:#4fb3ff; letter-spacing:.02em;
       padding-bottom:.4rem; margin-bottom:.45rem; border-bottom:1px solid #24324a; }
.vp table{ border-collapse:collapse; width:100%; }
.vp td{ padding:2.5px 0; vertical-align:baseline; }
/* Keys recede, values dominate: 600-weight pure-white values at full opacity
   are what makes the block legible over a bright basemap. */
.vp-k{ color:#93a2b8; font-weight:400; font-size:10.5px; letter-spacing:.07em;
       text-transform:uppercase; padding-right:14px !important; white-space:nowrap; }
.vp-v{ color:#ffffff; font-weight:600; font-size:12px; opacity:1; text-align:right;
       font-variant-numeric:tabular-nums; }
.vp-v.acc{ color:#00e0a4; } .vp-v.warn{ color:#ffb340; } .vp-v.crit{ color:#ff5d6c; }
.vp-v.dim{ color:#c8d2e0; font-weight:500; }
</style>
"""


def zone_anchor(zone: Dict[str, Any]) -> List[float]:
    """Where a zone's popup should hang: its NORTH edge, not its centre.

    Anchoring at the centre parks the popup on top of exactly the vehicles the
    zone contains — the markers the operator opened it to look at. One degree of
    latitude is ~111.32 km everywhere (no cos(lat) term, unlike longitude), so
    the circle case is a straight metres->degrees conversion.
    """
    poly = zone.get("polygon") or []
    if len(poly) >= 3:
        top = max(poly, key=lambda p: p[1])          # northernmost vertex
        return [top[1], top[0]]
    lat_off = zone.get("radius_m", 0.0) / 111_320.0
    return [zone.get("lat", 0.0) + lat_off, zone.get("lng", 0.0)]


@lru_cache(maxsize=64)
def _zone_popup(name: str, summary: str) -> str:
    """Click-popup for a geofence boundary.

    Cached: a zone's chrome is a pure function of (name, summary) and both change
    only when the operator edits the zone, so this is built once per zone for the
    life of the process rather than on every telemetry-driven rerun.
    """
    return (
        f'<div class="vp"><div class="vp-h">{name}</div><table>'
        f'<tr><td class="vp-k">Type</td><td class="vp-v">{summary}</td></tr>'
        f'<tr><td class="vp-k">Status</td>'
        f'<td class="vp-v acc">Active monitoring zone</td></tr>'
        f"</table></div>"
    )


@lru_cache(maxsize=16)
def _pin_data_uri(color_hex: str) -> str:
    """Same pin as a data URI for deck.gl's IconLayer.

    Cached: there are only ever five state colours, and re-encoding the SVG per
    row per rerun is pure waste.
    """
    svg = _VEHICLE_SVG.format(size=64, fill=color_hex, stroke=1.6)
    return "data:image/svg+xml;charset=utf-8," + quote(svg, safe="")


def _marker_icon(color_hex: str, focused: bool):
    """DivIcon carrying the recoloured pin. Anchored at the tip, not the centre."""
    size = 36 if focused else 28
    svg = _VEHICLE_SVG.format(size=size, fill=color_hex,
                              stroke=2.0 if focused else 1.5)
    return folium.DivIcon(
        html=f'<div style="width:{size}px;height:{size}px">{svg}</div>',
        icon_size=(size, size),
        icon_anchor=(size // 2, size),          # tip touches the coordinate
        class_name="veh-pin",
    )


def _soc_class(soc: float) -> str:
    return "crit" if soc < 15 else "warn" if soc < 30 else "acc"


def _marker_popup(node: SpatialTelemetryNode, place: str) -> str:
    """The single source of detail for a vehicle. No hover tooltip competes.

    Emits class names only — every style lives in _POPUP_CSS.
    """
    rows = [
        ("State", node.state, ""),
        ("Location", place or "—", "dim"),
        ("SOC", f"{node.soc_pct:.1f} %", _soc_class(node.soc_pct)),
        ("Voltage", f"{node.bms_voltage_v:.2f} V", ""),
        ("Current", f"{node.bms_current_a:.2f} A",
         "warn" if node.bms_current_a < 0 else ""),
        ("Speed", f"{node.speed_kmh:.1f} km/h", ""),
        ("Latitude", f"{node.lat:.7f}", "dim"),
        ("Longitude", f"{node.lng:.7f}", "dim"),
        ("Last seen", node.last_seen_str, "dim"),
    ]
    body = "".join(
        f'<tr><td class="vp-k">{k}</td>'
        f'<td class="vp-v {cls}"'
        + (f' style="color:{node.color_hex}"' if k == "State" else "")
        + f">{v}</td></tr>"
        for k, v, cls in rows
    )
    return (f'<div class="vp"><div class="vp-h">{node.thing_name}</div>'
            f"<table>{body}</table></div>")


def _draw_zone(shape_parent, name: str, zone: Dict[str, Any], active: bool,
               anchor_parent=None) -> None:
    """Render one geofence. Single definition for presets and custom zones.

    TWO PARENTS, deliberately. `shape_parent` is the editable FeatureGroup that
    Draw's edit/trash toolbar owns; `anchor_parent` is the bare map.

    The popup hangs off an invisible marker at the zone's north edge so it never
    covers the vehicles inside. That marker must NOT live in the editable group:
    everything in that group is serialised back through `all_drawings`, a Marker
    serialises as a Point, and geojson_to_zone() turns any Point into a circle
    with the 1000 m default radius. The anchor therefore reappeared as a phantom
    zone on the next rerun — the "ghost circle after delete". It also sits a full
    radius north of the centroid, so it never matched EDIT_MATCH_M and was always
    imported as new rather than recognised.

    popup= only, never tooltip=: a zone covers a large slice of the viewport, so
    a hover binding fired constantly while the operator aimed at a vehicle.
    """
    style = ZONE_STYLE["active" if active else "inactive"]
    poly = zone.get("polygon") or []

    if len(poly) >= 3:
        shape = folium.Polygon(locations=[[p[1], p[0]] for p in poly],
                               fill=True, **style)
    elif zone.get("radius_m"):
        shape = folium.Circle(location=[zone["lat"], zone["lng"]],
                              radius=zone["radius_m"], fill=True, **style)
    else:
        return
    shape.add_to(shape_parent)

    folium.Marker(
        location=zone_anchor(zone),
        icon=folium.DivIcon(html="<div style='width:1px;height:1px'></div>",
                            icon_size=(1, 1)),
        popup=folium.Popup(_zone_popup(name, zone_summary(zone)), max_width=210),
    ).add_to(anchor_parent if anchor_parent is not None else shape_parent)


def render_folium_map(
    nodes: Dict[str, SpatialTelemetryNode],
    visible_states: List[str],
    center: Tuple[float, float],
    zoom: int,
    active_zone: Optional[Dict[str, Any]],
    active_zone_name: str,
    route: Optional[pd.DataFrame],
    focus_place: str,
    focus_name: str,
) -> Dict[str, Any]:
    """Interactive basemap with draw tools, zone overlay and route playback."""
    # Positron carries deep street geometry inside India and thins out to
    # country/major-city labels elsewhere, which is exactly the requested
    # detail gradient without paying for a bespoke style.
    fmap = folium.Map(
        location=list(center), zoom_start=int(zoom),
        tiles="cartodbpositron", control_scale=True,
        min_zoom=MIN_ZOOM, max_zoom=MAX_ZOOM,
        max_bounds=True,
    )
    # Explicit viewport clamp. folium's `max_bounds=True` only derives bounds
    # from the features already added, so the box is set on the Leaflet options
    # directly. Viscosity 1.0 makes the edge hard — at <1.0 the user can drag
    # past it and it springs back, which reads as a broken map.
    fmap.options["maxBounds"] = INDIA_BOUNDS
    fmap.options["maxBoundsViscosity"] = 1.0
    # Popup chrome, injected once for the whole map.
    fmap.get_root().header.add_child(folium.Element(_POPUP_CSS))

    gkey = _google_maps_key()
    if gkey:
        folium.TileLayer(
            tiles=f"https://mt1.google.com/vt/lyrs=m&x={{x}}&y={{y}}&z={{z}}&key={gkey}",
            attr="Google Maps", name="Google Roadmap", overlay=False, control=True,
            min_zoom=MIN_ZOOM, max_zoom=MAX_ZOOM,
        ).add_to(fmap)
    folium.TileLayer("OpenStreetMap", name="OpenStreetMap",
                     min_zoom=MIN_ZOOM, max_zoom=MAX_ZOOM).add_to(fmap)

    # EDITABLE LAYER. Draw's edit/trash toolbar only ever operates on the
    # FeatureGroup handed to `options.edit.featureGroup`. Without passing one,
    # folium builds a fresh empty group, so shapes added straight to the map are
    # invisible to the toolbar — that is why edit/trash appeared to do nothing on
    # saved zones. Everything editable goes in here.
    editable = folium.FeatureGroup(name="Custom zones", control=False)

    custom_zones: Dict[str, Dict[str, Any]] = st.session_state.get(K_CUSTOM_HUBS, {})
    for zname, z in custom_zones.items():
        # Shape into the editable group (so edit/trash can grab it); popup anchor
        # onto the map, where Draw cannot serialise it back as a phantom shape.
        _draw_zone(editable, zname, z, active=(zname == active_zone_name),
                   anchor_parent=fmap)
    editable.add_to(fmap)

    # Presets stay OUTSIDE the editable group: they are code-defined, so exposing
    # them to the trash tool would offer a delete that cannot persist.
    if active_zone and active_zone_name not in custom_zones:
        _draw_zone(fmap, active_zone_name, active_zone, active=True)

    if route is not None and not route.empty:
        # No tooltip: the line spans the whole viewport, so hovering anywhere
        # near it fired constantly, and it only repeated the unit name already
        # shown in the caption above the map. The breadcrumb dots below keep
        # theirs — they are small, precise, and carry per-point data nothing
        # else shows.
        folium.PolyLine(route[["lat", "lng"]].values.tolist(),
                        color="#ffb340", weight=4, opacity=0.9).add_to(fmap)
        for _, r in route.iloc[::6].iterrows():
            folium.CircleMarker(
                location=[r["lat"], r["lng"]], radius=3.5,
                color="#ffb340", fill=True, fill_color="#0e131b", fill_opacity=1.0,
                tooltip=f"{r['timestamp']} · {r['speed_kmh']} km/h · SOC {r['soc_pct']}%",
            ).add_to(fmap)

    for node in nodes.values():
        if node.state not in visible_states or not node.has_fix:
            continue
        # Geocode only the focused unit: every marker would mean N Nominatim
        # calls per rerun, far past its 1 req/s policy.
        place = focus_place if node.thing_name == focus_name else ""
        # NO tooltip= on purpose. A hover tooltip and a click popup on the same
        # marker fight each other: the tooltip covers the pin the user is aiming
        # at and repeats what the popup already says. The popup is the single
        # source of detail.
        folium.Marker(
            location=[node.lat, node.lng],
            icon=_marker_icon(node.color_hex, node.thing_name == focus_name),
            popup=folium.Popup(_marker_popup(node, place), max_width=320),
        ).add_to(fmap)

    Draw(
        export=False, position="topleft",
        feature_group=editable,        # <- binds edit/trash to the saved zones
        # folium defaults this to True, which attaches `alert(coords)` to every
        # drawn layer. A blocking modal on each click is the "freeze" symptom.
        show_geometry_on_click=False,
        draw_options={"polyline": False, "polygon": True, "circle": True,
                      "rectangle": True, "marker": False, "circlemarker": False},
        edit_options={"edit": True, "remove": True},
    ).add_to(fmap)
    folium.LayerControl(collapsed=True).add_to(fmap)

    # PERFORMANCE BINDING.
    #  * `key` is CONSTANT: st_folium remounts the whole Leaflet instance when the
    #    key changes, so keying on centre/zoom/target would rebuild every tile,
    #    marker and layer on each selection — the CPU spike this is avoiding.
    #  * `center`/`zoom` are parameters instead, which pans the existing map.
    #  * `returned_objects` is exactly one entry. Every name listed here is
    #    serialised browser->Python on EVERY map interaction, including pans and
    #    zooms; `last_active_drawing` duplicated data already in `all_drawings`
    #    and was never read.
    return st_folium(
        fmap, key="fleet_map", height=560, use_container_width=True,
        center=list(center), zoom=int(zoom),
        returned_objects=["all_drawings"],
    )


def _google_maps_key() -> str:
    try:
        key = st.secrets.get("gmaps", {}).get("api_key", "")
    except Exception:
        key = ""
    return (key or os.getenv("GOOGLE_MAPS_API_KEY", "")).strip()


def render_pydeck_map(
    frame: pd.DataFrame, center: Tuple[float, float], zoom: float,
    active_zone: Optional[Dict[str, Any]], route: Optional[pd.DataFrame],
) -> None:
    """Fallback canvas when folium/streamlit-folium are unavailable."""
    layers: List[pdk.Layer] = []

    if active_zone:
        poly = active_zone.get("polygon") or []
        if len(poly) >= 3:
            layers.append(pdk.Layer(
                "PolygonLayer", data=[{"polygon": poly}], get_polygon="polygon",
                get_fill_color=[53, 169, 255, 40], get_line_color=[53, 169, 255, 220],
                line_width_min_pixels=2, pickable=False))
        elif active_zone.get("radius_m"):
            layers.append(pdk.Layer(
                "ScatterplotLayer",
                data=[{"lat": active_zone["lat"], "lng": active_zone["lng"],
                       "radius": active_zone["radius_m"]}],
                get_position=["lng", "lat"], get_fill_color=[53, 169, 255, 45],
                get_radius="radius", pickable=False))

    if route is not None and not route.empty:
        layers.append(pdk.Layer(
            "PathLayer", data=[{"path": route[["lng", "lat"]].values.tolist()}],
            get_path="path", get_color=[255, 179, 64, 230], width_min_pixels=4))

    if not frame.empty:
        # IconLayer, same pin art as Folium. The icon spec is per-row because
        # deck.gl bakes the image into the accessor, which is also how each
        # vehicle gets its own state colour without a sprite atlas per state.
        icon_frame = frame.copy()
        icon_frame["icon"] = [
            {"url": _pin_data_uri(hex_), "width": 64, "height": 64,
             "anchorY": 64, "mask": False}
            for hex_ in icon_frame["state"].map(
                lambda s: STATE_COLORS.get(s, STATE_COLORS["Offline"])[1])
        ]
        layers.append(pdk.Layer(
            "IconLayer", data=icon_frame, get_icon="icon",
            get_position=["lng", "lat"], get_size=3.4, size_scale=10,
            size_min_pixels=22, size_max_pixels=44,
            pickable=True, auto_highlight=True))

    st.pydeck_chart(pdk.Deck(
        layers=layers,
        initial_view_state=pdk.ViewState(latitude=center[0], longitude=center[1],
                                         zoom=zoom, pitch=0),
        map_style="https://basemaps.cartocdn.com/gl/positron-gl-style/style.json",
        # deck.gl has no click-popup primitive, so this hover card IS the single
        # detail surface here — it is not a duplicate of anything.
        tooltip={
            "html": (
                "<div style='font-family:JetBrains Mono,monospace;min-width:210px'>"
                "<div style='font-size:13px;font-weight:700;color:#4fb3ff;"
                "padding-bottom:4px;margin-bottom:5px;border-bottom:1px solid #24324a'>"
                "{thing_name}</div>"
                "<span style='color:#93a2b8;font-size:10.5px'>STATE</span> "
                "<b style='color:#fff'>{state}</b><br/>"
                "<span style='color:#93a2b8;font-size:10.5px'>SOC</span> "
                "<b style='color:#00e0a4'>{soc}%</b>&nbsp;&nbsp;"
                "<span style='color:#93a2b8;font-size:10.5px'>V</span> "
                "<b style='color:#fff'>{voltage}</b>&nbsp;&nbsp;"
                "<span style='color:#93a2b8;font-size:10.5px'>A</span> "
                "<b style='color:#fff'>{current}</b><br/>"
                "<span style='color:#93a2b8;font-size:10.5px'>SPEED</span> "
                "<b style='color:#fff'>{speed} km/h</b><br/>"
                "<span style='color:#93a2b8;font-size:10.5px'>LAT/LNG</span> "
                "<b style='color:#c8d2e0'>{lat}, {lng}</b></div>"),
            "style": {"backgroundColor": "#0d1420", "border": "1px solid #24324a",
                      "borderRadius": "10px", "padding": "10px 12px",
                      "boxShadow": "0 14px 38px rgba(0,0,0,.6)"},
        },
    ), use_container_width=True)


# ==============================================================================
# 7. STATE ASSEMBLY
# ==============================================================================

def build_spatial_state(fleet_info: Any) -> Dict[str, SpatialTelemetryNode]:
    """Live telemetry from session_state when present, synthetic otherwise."""
    synth = generate_synthetic_spatial_fleet(fleet_info.names)
    out: Dict[str, SpatialTelemetryNode] = {}

    for node in fleet_info.nodes:
        raw = st.session_state.get(f"telemetry_last_{node.name}") or synth.get(node.name)
        if isinstance(raw, dict):
            out[node.name] = SpatialTelemetryNode(
                thing_name=node.name,
                lat=float(raw.get("lat") or raw.get("latitude") or 0.0),
                lng=float(raw.get("lng") or raw.get("longitude") or 0.0),
                speed_kmh=float(raw.get("speed_kmh") or raw.get("speed") or 0.0),
                bms_current_a=float(raw.get("bms_current_a") or raw.get("current") or 0.0),
                bms_voltage_v=float(raw.get("bms_voltage_v") or raw.get("voltage") or 0.0),
                soc_pct=float(raw.get("soc_pct") or raw.get("soc") or 0.0),
                last_seen_ts=float(raw.get("last_seen_ts") or raw.get("timestamp") or time.time()),
                is_online=(node.connected is not False),
            )
        else:
            out[node.name] = SpatialTelemetryNode(
                thing_name=node.name, lat=0.0, lng=0.0, speed_kmh=0.0,
                bms_current_a=0.0, bms_voltage_v=0.0, soc_pct=0.0,
                last_seen_ts=0.0, is_online=False)
    return out


def nodes_to_frame(nodes: Dict[str, SpatialTelemetryNode],
                   visible_states: List[str]) -> pd.DataFrame:
    return pd.DataFrame([
        {"thing_name": n.thing_name, "lat": round(n.lat, 7), "lng": round(n.lng, 7),
         "speed": n.speed_kmh, "current": n.bms_current_a, "voltage": n.bms_voltage_v,
         "soc": n.soc_pct, "state": n.state, "color_rgba": list(n.color_rgba),
         "last_seen_str": n.last_seen_str}
        for n in nodes.values()
        if n.state in visible_states and n.has_fix
    ])


# ==============================================================================
# 8. CONTROL PANEL BLOCKS
# ==============================================================================

def compact_metrics(items: List[Tuple[str, str, str]]) -> None:
    """Small label/value strip.

    Replaces st.metric here rather than shrinking it with CSS: overriding
    stMetricValue/stMetricLabel would also shrink the metrics on Job Tracking and
    Live Telemetry, which are out of scope and look correct as they are.
    `items` is (label, value, colour-or-empty).
    """
    cells = "".join(
        f'<div class="cm-cell"><div class="cm-l">{label}</div>'
        f'<div class="cm-v"{f" style=color:{tone}" if tone else ""}>{value}</div></div>'
        for label, value, tone in items
    )
    st.markdown(f'<div class="cm-row">{cells}</div>', unsafe_allow_html=True)


def _block_b_filters(node_names: Tuple[str, ...],
                     counts: Dict[str, int]) -> Tuple[List[str], str]:
    st.markdown("####  Filtering & Selection")
    visible = st.multiselect(
        "Filter by state",
        options=ALL_STATES, default=ALL_STATES,
        format_func=lambda s: f"{s}  ({counts.get(s, 0)})",
        help="Hides markers whose derived state is not selected.",
    )
    focus = st.selectbox(
        "Select target battery",
        options=["All Fleet Vehicles", *node_names],
        help="Locks the map onto one unit and resolves its street address.",
    )
    return (visible or ALL_STATES), focus


def _block_c_playback(focus: str) -> Tuple[bool, datetime, datetime]:
    """Playback window, hard-capped at 24 h.

    A datetime range SLIDER rather than a date_input pair: the slider's whole
    track is exactly [now-24h, now], so an over-long window is not merely
    validated-and-rejected, it is unreachable. A bounded date_input still allows
    picking yesterday AND today, which is up to 48 h of rows.
    """
    st.markdown("#### Route Playback")
    now = datetime.now().replace(second=0, microsecond=0)
    floor = now - timedelta(days=1)

    if focus == "All Fleet Vehicles":
        st.caption("Select a single battery in **Filtering & Selection** to enable "
                   "historical playback.")
        return False, floor, now

    enabled = st.toggle("Draw historical route", value=False,
                        help="Overlays the breadcrumb trail for the window below.")
    ts_from, ts_to = st.slider(
        "Playback window",
        min_value=floor,
        max_value=now,
        value=(now - timedelta(hours=6), now),
        step=timedelta(minutes=15),
        format="DD MMM HH:mm",
        disabled=not enabled,
    )
    st.caption("Historical routes are restricted to the last 24 hours to ensure "
               "optimal performance.")
    return enabled, ts_from, ts_to


def _rename_zone(custom: Dict[str, Dict[str, Any]], old: str, new: str) -> str:
    """Re-key a custom zone in place. Returns "" on success, else the reason.

    Rebuilds the dict rather than pop-then-insert so the zone keeps its position
    in the Active dropdown — a renamed zone jumping to the bottom of the list
    reads as "a different zone appeared".

    Validated against BOTH dicts: a custom zone shadowing a preset name would
    make `{**HUB_PRESETS, **custom}` silently drop the preset.
    """
    new = new.strip()
    if not new or new == old:
        return ""
    if new in HUB_PRESETS:
        return f"“{new}” is a preset name. Choose another."
    if new in custom:
        return f"“{new}” already exists."

    for key in list(custom):                       # preserve insertion order
        value = custom.pop(key)
        custom[new if key == old else key] = value

    if st.session_state.get(K_ACTIVE_ZONE) == old:
        st.session_state[K_ACTIVE_ZONE] = new      # keep the selection alive
    return ""


def _block_d_geofencing() -> Tuple[str, Dict[str, Any]]:
    """Active-zone selection + management.

    There is no "add" form. Zones are created by drawing on the map and are
    imported by `sync_drawn_zones()`; both widgets below read straight from
    session state, so a shape drawn on the map appears here on the same rerun.
    """
    st.markdown("####  Geofencing & Zones")
    custom: Dict[str, Dict[str, Any]] = st.session_state.setdefault(K_CUSTOM_HUBS, {})
    all_zones = {**HUB_PRESETS, **custom}

    # Custom zone names already end in "(Custom)", so appending a second marker
    # produced "Zone 1 (Custom)  (custom)". Only presets need the suffix.
    labels = {f"{n}  (preset)": n for n in HUB_PRESETS}
    labels.update({n: n for n in custom})
    label_of = {v: k for k, v in labels.items()}

    tab_active, tab_manage = st.tabs(["Active", f"Manage ({len(custom)})"])

    with tab_active:
        # A freshly drawn zone preselects itself; the widget key keeps the choice
        # stable across the reruns that map interaction triggers.
        wanted = st.session_state.get(K_ACTIVE_ZONE)
        options = list(labels)
        index = options.index(label_of[wanted]) if wanted in label_of else 0
        picked = st.selectbox(
            "Active monitoring zone", options=options, index=index,
            help="Vehicles outside this zone raise a breach alert.",
        )
        zone_name = labels[picked]
        st.session_state[K_ACTIVE_ZONE] = zone_name
        zone = all_zones[zone_name]
        st.caption(f"{zone_summary(zone)} · centre {resolve_place(zone['lat'], zone['lng'])}")
        st.caption(" Draw a circle, polygon or rectangle with the map toolbar to "
                   "add a zone — it is saved and selectable immediately.")

    with tab_manage:
        st.caption("Boundaries are edited on the map: pick the edit or trash tool "
                   "in the toolbar, adjust the shape, then save. Changes are "
                   "captured automatically.")

        if not custom:
            st.caption("No drawn zones yet. Presets are built in and cannot be removed.")
        else:
            for name, z in list(custom.items()):
                # [name | menu] — one popover per row keeps the list scannable
                # instead of three competing buttons per zone.
                c_name, c_menu = st.columns([3, 1], vertical_alignment="center")
                c_name.markdown(
                    f"<div class='zn-row'><span class='zn-name'>{name}</span>"
                    f"<span class='zn-meta'>{zone_summary(z)}</span></div>",
                    unsafe_allow_html=True)

                with c_menu.popover("⋮", use_container_width=True):
                    st.caption(name)

                    new_name = st.text_input("Rename", value=name, key=f"rn_{name}")
                    if st.button("Save name", key=f"btnrn_{name}",
                                 use_container_width=True):
                        err = _rename_zone(custom, name, new_name)
                        if err:
                            st.warning(err)
                        else:
                            st.rerun()

                    st.divider()
                    if st.button("Delete zone", key=f"delzone_{name}",
                                 type="primary", use_container_width=True):
                        custom.pop(name, None)
                        # Drop the fingerprint too, otherwise the shape still on
                        # the map would never be re-importable after deletion.
                        # Keep the fingerprint as a TOMBSTONE. Saved zones are
                        # rendered into the editable group, so the shape is still
                        # present in the all_drawings snapshot for this cycle —
                        # discarding the fingerprint here made sync_drawn_zones
                        # re-import it instantly, which is the "deleted zone
                        # comes back" bug.
                        if st.session_state.get(K_ACTIVE_ZONE) == name:
                            st.session_state[K_ACTIVE_ZONE] = next(iter(HUB_PRESETS))
                        st.rerun()

            st.caption("Deleting a zone here does not erase the shape from the map; "
                       "clear it with the map's trash tool if you want it gone visually.")

    return zone_name, zone


# ==============================================================================
# 9. PAGE
# ==============================================================================

def render_map_console() -> None:
    header(
        "Fleet Map Console",
        "Live spatial tracking, historical route playback and geofence breach "
        "detection. Addresses are reverse-geocoded; exact coordinates live in the "
        "map popups.",
        right="Folium · OSM/Carto" if HAS_FOLIUM else "PyDeck fallback",
    )

    fleet_info = fl.discover()
    if fleet_info.error:
        st.error(f"Fleet discovery failed: {fleet_info.error}")
        return
    if not fleet_info.names:
        st.info("No nodes registered. Provision a device to enable spatial tracking.")
        return

    nodes = build_spatial_state(fleet_info)
    counts: Dict[str, int] = {s: 0 for s in ALL_STATES}
    for n in nodes.values():
        counts[n.state] += 1

    # ---------- Block A ----------
    stat_row([
        stat("Total Fleet", str(len(nodes)), "registered units", tone="info"),
        stat("Moving", str(counts["Moving"]), "under power", tone="ok"),
        stat("Stopped", str(counts["Stopped"]), "parked, no current", tone="bad"),
        stat("Idle", str(counts["Idle"]), "static, drawing current", tone="warn"),
        stat("Towing / Theft", str(counts["Towing/Theft"]), "moving, no current",
             tone="bad" if counts["Towing/Theft"] else "mute"),
        stat("Offline", str(counts["Offline"]), "no recent telemetry",
             tone="warn" if counts["Offline"] else "mute"),
    ])
    st.divider()

    panel, canvas = st.columns([1, 2.35], gap="large")

    with panel:
        visible_states, focus = _block_b_filters(fleet_info.names, counts)
        st.divider()
        play_on, ts_from, ts_to = _block_c_playback(focus)
        st.divider()
        zone_name, active_zone = _block_d_geofencing()

    target = nodes.get(focus) if focus != "All Fleet Vehicles" else None
    frame = nodes_to_frame(nodes, visible_states)

    # ---------------------------------------------------------------- camera
    # STICKY view held in session state, recomputed only on an actual event.
    #
    # The previous version derived the centre from a precedence chain on every
    # run. Selecting a zone centred it for exactly one rerun, then the next
    # rerun (st_folium fires one on any map interaction) saw zone_changed==False
    # and fell through to the fleet centroid — the map snapped straight back,
    # which is why dropdown centring looked broken.
    zone_changed = st.session_state.get(K_ZONE_CENTRED) != zone_name
    st.session_state[K_ZONE_CENTRED] = zone_name
    focus_changed = st.session_state.get(K_LAST_FOCUS) != focus
    st.session_state[K_LAST_FOCUS] = focus

    view = st.session_state.get(K_VIEW)
    if zone_changed and active_zone:
        view = (active_zone["lat"], active_zone["lng"], 14)
    elif focus_changed and target and target.has_fix:
        view = (target.lat, target.lng, 14)
    elif view is None:                       # first paint only
        if target and target.has_fix:
            view = (target.lat, target.lng, 14)
        elif not frame.empty:
            view = (float(frame["lat"].mean()), float(frame["lng"].mean()), 11)
        else:
            view = (*DEFAULT_CENTER, 11)
    st.session_state[K_VIEW] = view

    center, zoom = (view[0], view[1]), view[2]

    focus_place = resolve_place(target.lat, target.lng) if target else ""

    route = None
    if play_on and target and target.has_fix:
        route = generate_historical_route(target.thing_name, target.lat, target.lng,
                                          ts_from, ts_to)

    with canvas:
        # ---------- focused unit summary: places, not coordinates ----------
        if target:
            compact_metrics([
                ("UNIT", target.thing_name, ""),
                ("LOCATION", focus_place, ""),
                ("STATE", target.state,
                 target.color_hex),
                ("SPEED", f"{target.speed_kmh:.1f} km/h", ""),
            ])
        else:
            compact_metrics([
                ("SCOPE", "Entire fleet", ""),
                ("MAP CENTRE", resolve_place(*center), ""),
                ("PLOTTED", f"{len(frame)} / {len(nodes)}", ""),
            ])

        if route is not None and not route.empty:
            st.caption(f" Route playback · **{len(route)} points** · "
                       f"{ts_from:%d %b %H:%M} → {ts_to:%H:%M} · *synthetic trail until a "
                       f"telemetry history store is wired*")

        if HAS_FOLIUM:
            out = render_folium_map(nodes, visible_states, center, zoom, active_zone,
                                    zone_name, route, focus_place,
                                    target.thing_name if target else "")
            # Auto-import drawn shapes. `all_drawings` (not last_active_drawing)
            # so shapes drawn while the script was mid-run are not missed.
            # `.get` returns None when the component has not reported yet, which
            # sync_drawn_zones treats as "no information" rather than "no shapes".
            created = sync_drawn_zones((out or {}).get("all_drawings"))
            if created:
                if created[-1] in st.session_state.get(K_CUSTOM_HUBS, {}):
                    st.session_state[K_ACTIVE_ZONE] = created[-1]
                    st.toast(f"Zone saved: {created[-1]}")
                else:
                    st.toast(f"Zone removed: {created[-1]}")
                # Fires once per shape: the fingerprint is now recorded, so the
                # next pass finds nothing new and does not rerun again.
                st.rerun()
        else:
            st.caption("`folium` / `streamlit-folium` not installed — drawing tools "
                       "unavailable. Install them to add custom zones.")
            render_pydeck_map(frame, center, zoom, active_zone, route)

    # ---------- breach detection ----------
    breaches = [
        {"Device": n.thing_name, "State": n.state,
         "Distance from centre (km)": round(
             haversine_distance_m(n.lat, n.lng, active_zone["lat"],
                                  active_zone["lng"]) / 1000.0, 2)}
        for n in nodes.values()
        if n.has_fix and not zone_contains(active_zone, n.lat, n.lng)
    ]
    if breaches:
        st.warning(f" **{len(breaches)}** vehicle(s) outside **{zone_name}**.")
        with st.expander("Breach detail"):
            st.dataframe(pd.DataFrame(breaches), use_container_width=True,
                         hide_index=True)

    # ---------- fleet grid ----------
    st.divider()
    st.markdown("####  Fleet Detail")
    if frame.empty:
        st.caption("No vehicles match the current state filter.")
        return
    st.dataframe(
        frame[["thing_name", "state", "speed", "soc", "voltage", "current",
               "lat", "lng", "last_seen_str"]].rename(columns={
                   "thing_name": "Device", "state": "State", "speed": "Speed (km/h)",
                   "soc": "SOC (%)", "voltage": "Voltage (V)", "current": "Current (A)",
                   "lat": "Latitude", "lng": "Longitude", "last_seen_str": "Last Seen"}),
        use_container_width=True, hide_index=True, height=260,
    )
