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
from datetime import date, datetime, timedelta, timezone
from typing import Any, Dict, List, Literal, Optional, Tuple

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

STATE_ICON = {
    "Moving": "🟢", "Stopped": "🔴", "Idle": "🟡",
    "Towing/Theft": "🟣", "Offline": "⚪",
}

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

# session_state keys, declared once so nothing drifts
K_CUSTOM_HUBS = "custom_hubs"        # name -> zone dict (drawn zones)
K_SEEN_SHAPES = "map_seen_shapes"    # fingerprints already turned into zones
K_ACTIVE_ZONE = "map_active_zone"    # selectbox value, so a new zone can preselect


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
    day_from: date, day_to: date, num_points: int = 48,
) -> pd.DataFrame:
    """Breadcrumb trail spanning the requested window.

    Synthetic. Swap the body for a Timestream/S3 query when history is stored;
    the return shape (lat, lng, speed_kmh, soc_pct, timestamp) is the contract
    the renderer depends on.
    """
    rng = random.Random(_stable_seed(f"{thing_name}{day_from}{day_to}"))
    span_s = max((day_to - day_from).days + 1, 1) * 86_400
    t0 = datetime.combine(day_from, datetime.min.time(), tzinfo=timezone.utc).timestamp()
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
            radius = float(props.get("radius") or 1000.0)
            return {"lat": lat, "lng": lng, "radius_m": radius, "polygon": []}

        if gtype == "Polygon" and coords:
            ring = [[float(p[0]), float(p[1])] for p in coords[0]]
            if len(ring) >= 3:
                # Leaflet closes the ring; drop the duplicate final vertex so the
                # ray-casting wrap-around does not count that edge twice.
                if ring[0] == ring[-1]:
                    ring = ring[:-1]
                lat_c = sum(p[1] for p in ring) / len(ring)
                lng_c = sum(p[0] for p in ring) / len(ring)
                return {"lat": lat_c, "lng": lng_c, "radius_m": 0.0, "polygon": ring}
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


def sync_drawn_zones(drawings: Optional[List[Dict[str, Any]]]) -> List[str]:
    """Import newly drawn shapes into `custom_hubs`. Returns the names created.

    Idempotent by fingerprint, which is what keeps this free of rerun loops: the
    first pass creates the zone and records the fingerprint, every later pass
    sees a known fingerprint and does nothing, so the caller's `st.rerun()` fires
    exactly once per drawn shape.

    Additions only. A shape removed with the map's trash tool leaves its saved
    zone intact — deletion is an explicit action in the Manage tab, so a stray
    click on the map cannot silently drop a monitoring zone.
    """
    if not drawings:
        return []

    custom: Dict[str, Dict[str, Any]] = st.session_state.setdefault(K_CUSTOM_HUBS, {})
    seen: set = st.session_state.setdefault(K_SEEN_SHAPES, set())
    created: List[str] = []

    for feature in drawings:
        fp = shape_fingerprint(feature)
        if fp in seen:
            continue
        zone = geojson_to_zone(feature)
        seen.add(fp)                       # mark even on failure: never retry a bad shape
        if not zone:
            continue
        n = len(custom) + 1
        while f"Custom Zone {n}" in custom:
            n += 1
        name = f"Custom Zone {n}"
        zone["fingerprint"] = fp
        custom[name] = zone
        created.append(name)

    return created


# ==============================================================================
# 6. MAP RENDERERS
# ==============================================================================

def _marker_popup(node: SpatialTelemetryNode, place: str) -> str:
    """All precise technical data lives here, not in the control panel."""
    rows = [
        ("State", f"<span style='color:{node.color_hex};font-weight:700'>"
                  f"{STATE_ICON[node.state]} {node.state}</span>"),
        ("Location", place or "—"),
        ("Latitude", f"{node.lat:.7f}"),
        ("Longitude", f"{node.lng:.7f}"),
        ("Speed", f"{node.speed_kmh:.1f} km/h"),
        ("SOC", f"{node.soc_pct:.1f} %"),
        ("Voltage", f"{node.bms_voltage_v:.2f} V"),
        ("Current", f"{node.bms_current_a:.2f} A"),
        ("Last seen", node.last_seen_str),
    ]
    body = "".join(
        f"<tr><td style='color:#8b98ab;padding:2px 10px 2px 0;white-space:nowrap'>{k}</td>"
        f"<td style='color:#e4e9f0;font-weight:600'>{v}</td></tr>"
        for k, v in rows
    )
    return (
        "<div style=\"font-family:'JetBrains Mono',ui-monospace,monospace;font-size:11.5px;"
        "min-width:230px\">"
        f"<div style='font-size:13px;font-weight:700;color:#35a9ff;margin-bottom:6px'>"
        f"{node.thing_name}</div><table style='border-collapse:collapse'>{body}</table></div>"
    )


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
    fmap = folium.Map(location=list(center), zoom_start=int(zoom),
                      tiles="cartodbpositron", control_scale=True)

    gkey = _google_maps_key()
    if gkey:
        folium.TileLayer(
            tiles=f"https://mt1.google.com/vt/lyrs=m&x={{x}}&y={{y}}&z={{z}}&key={gkey}",
            attr="Google Maps", name="Google Roadmap", overlay=False, control=True,
        ).add_to(fmap)
    folium.TileLayer("OpenStreetMap", name="OpenStreetMap").add_to(fmap)

    if active_zone:
        poly = active_zone.get("polygon") or []
        if len(poly) >= 3:
            folium.Polygon(
                locations=[[p[1], p[0]] for p in poly],
                color="#35a9ff", fill=True, fill_color="#35a9ff",
                fill_opacity=0.18, weight=2.5,
                tooltip=f"Zone: {active_zone_name} ({zone_summary(active_zone)})",
            ).add_to(fmap)
        elif active_zone.get("radius_m"):
            folium.Circle(
                location=[active_zone["lat"], active_zone["lng"]],
                radius=active_zone["radius_m"],
                color="#35a9ff", fill=True, fill_color="#35a9ff",
                fill_opacity=0.15, weight=2.5,
                tooltip=f"Zone: {active_zone_name} ({zone_summary(active_zone)})",
            ).add_to(fmap)

    if route is not None and not route.empty:
        folium.PolyLine(route[["lat", "lng"]].values.tolist(),
                        color="#ffb340", weight=4, opacity=0.9,
                        tooltip=f"Route · {focus_name}").add_to(fmap)
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
        folium.CircleMarker(
            location=[node.lat, node.lng],
            radius=9 if node.thing_name == focus_name else 7,
            color="#ffffff", weight=2.0 if node.thing_name == focus_name else 1.2,
            fill=True, fill_color=node.color_hex, fill_opacity=0.92,
            popup=folium.Popup(_marker_popup(node, place), max_width=300),
            tooltip=f"{STATE_ICON[node.state]} {node.thing_name} · {node.state}",
        ).add_to(fmap)

    Draw(
        export=False, position="topleft",
        draw_options={"polyline": False, "polygon": True, "circle": True,
                      "rectangle": True, "marker": False, "circlemarker": False},
        edit_options={"edit": True, "remove": True},
    ).add_to(fmap)
    folium.LayerControl(collapsed=True).add_to(fmap)

    # `center`/`zoom` are passed as parameters rather than baked into the key, so
    # re-centering on a new target does not discard the user's drawn shapes.
    return st_folium(
        fmap, key="fleet_map", height=560, use_container_width=True,
        center=list(center), zoom=int(zoom),
        returned_objects=["all_drawings", "last_active_drawing"],
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
        layers.append(pdk.Layer(
            "ScatterplotLayer", data=frame, get_position=["lng", "lat"],
            get_fill_color="color_rgba", get_line_color=[255, 255, 255, 200],
            get_radius=140, radius_min_pixels=7, radius_max_pixels=18,
            line_width_min_pixels=1.5, pickable=True, auto_highlight=True))

    st.pydeck_chart(pdk.Deck(
        layers=layers,
        initial_view_state=pdk.ViewState(latitude=center[0], longitude=center[1],
                                         zoom=zoom, pitch=0),
        map_style="https://basemaps.cartocdn.com/gl/positron-gl-style/style.json",
        tooltip={
            "html": ("<b>{thing_name}</b><br/>{state}<br/>"
                     "Lat {lat} · Lng {lng}<br/>"
                     "SOC {soc}% · {voltage} V · {current} A<br/>{speed} km/h"),
            "style": {"backgroundColor": "#121821", "color": "#e4e9f0",
                      "fontSize": "12px", "borderRadius": "8px",
                      "fontFamily": "JetBrains Mono, monospace"},
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
    st.markdown("#### 🔎 Filtering & Selection")
    visible = st.multiselect(
        "Filter by state",
        options=ALL_STATES, default=ALL_STATES,
        format_func=lambda s: f"{STATE_ICON[s]}  {s}  ({counts.get(s, 0)})",
        help="Hides markers whose derived state is not selected.",
    )
    focus = st.selectbox(
        "Select target battery",
        options=["All Fleet Vehicles", *node_names],
        help="Locks the map onto one unit and resolves its street address.",
    )
    return (visible or ALL_STATES), focus


def _block_c_playback(focus: str) -> Tuple[bool, date, date]:
    st.markdown("#### 🛣️ Route Playback")
    if focus == "All Fleet Vehicles":
        st.caption("Select a single battery in **Filtering & Selection** to enable "
                   "historical playback.")
        return False, date.today(), date.today()

    enabled = st.toggle("Draw historical route", value=False,
                        help="Overlays the breadcrumb trail for the window below.")
    today = date.today()
    span = st.date_input(
        "Date range", value=(today - timedelta(days=1), today),
        max_value=today, disabled=not enabled,
    )
    if isinstance(span, tuple) and len(span) == 2:
        d_from, d_to = span
    else:                                   # mid-edit: only one date picked yet
        d_from = d_to = span if isinstance(span, date) else today
    return enabled, d_from, d_to


def _block_d_geofencing() -> Tuple[str, Dict[str, Any]]:
    """Active-zone selection + management.

    There is no "add" form. Zones are created by drawing on the map and are
    imported by `sync_drawn_zones()`; both widgets below read straight from
    session state, so a shape drawn on the map appears here on the same rerun.
    """
    st.markdown("#### 🛡️ Geofencing & Zones")
    custom: Dict[str, Dict[str, Any]] = st.session_state.setdefault(K_CUSTOM_HUBS, {})
    all_zones = {**HUB_PRESETS, **custom}

    labels = {f"📌 {n}": n for n in HUB_PRESETS}
    labels.update({f"⚡ {n}": n for n in custom})
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
        st.caption("✏️ Draw a circle, polygon or rectangle with the map toolbar to "
                   "add a zone — it is saved and selectable immediately.")

    with tab_manage:
        if not custom:
            st.caption("No drawn zones yet. Presets are built in and cannot be removed.")
        else:
            for name, z in list(custom.items()):
                c1, c2 = st.columns([3, 1.1], vertical_alignment="center")
                c1.markdown(
                    f"**⚡ {name}**<br><span style='color:#8b98ab;font-size:.72rem'>"
                    f"{zone_summary(z)}</span>", unsafe_allow_html=True)
                if c2.button("Delete", key=f"delzone_{name}",
                             use_container_width=True):
                    custom.pop(name, None)
                    # Drop the fingerprint too, otherwise the shape still on the
                    # map would never be re-importable after deletion.
                    fp = z.get("fingerprint")
                    if fp:
                        st.session_state.setdefault(K_SEEN_SHAPES, set()).discard(fp)
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
        st.error(f"Fleet discovery failed: {fleet_info.error}", icon="⛔")
        return
    if not fleet_info.names:
        st.info("No nodes registered. Provision a device to enable spatial tracking.",
                icon="📡")
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
        play_on, day_from, day_to = _block_c_playback(focus)
        st.divider()
        zone_name, active_zone = _block_d_geofencing()

    target = nodes.get(focus) if focus != "All Fleet Vehicles" else None
    frame = nodes_to_frame(nodes, visible_states)

    if target and target.has_fix:
        center, zoom = (target.lat, target.lng), 14
    elif not frame.empty:
        center, zoom = (float(frame["lat"].mean()), float(frame["lng"].mean())), 11
    else:
        center, zoom = DEFAULT_CENTER, 11

    focus_place = resolve_place(target.lat, target.lng) if target else ""

    route = None
    if play_on and target and target.has_fix:
        route = generate_historical_route(target.thing_name, target.lat, target.lng,
                                          day_from, day_to)

    with canvas:
        # ---------- focused unit summary: places, not coordinates ----------
        if target:
            compact_metrics([
                ("UNIT", target.thing_name, ""),
                ("LOCATION", focus_place, ""),
                ("STATE", f"{STATE_ICON[target.state]} {target.state}",
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
            st.caption(f"🛣️ Route playback · **{len(route)} points** · "
                       f"{day_from:%d %b} → {day_to:%d %b} · *synthetic trail until a "
                       f"telemetry history store is wired*")

        if HAS_FOLIUM:
            out = render_folium_map(nodes, visible_states, center, zoom, active_zone,
                                    zone_name, route, focus_place,
                                    target.thing_name if target else "")
            # Auto-import drawn shapes. `all_drawings` (not last_active_drawing)
            # so shapes drawn while the script was mid-run are not missed.
            created = sync_drawn_zones((out or {}).get("all_drawings"))
            if created:
                st.session_state[K_ACTIVE_ZONE] = created[-1]
                st.toast(f"Zone saved: {created[-1]}", icon="🛡️")
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
        st.warning(f"⚠️ **{len(breaches)}** vehicle(s) outside **{zone_name}**.")
        with st.expander("Breach detail"):
            st.dataframe(pd.DataFrame(breaches), use_container_width=True,
                         hide_index=True)

    # ---------- fleet grid ----------
    st.divider()
    st.markdown("#### 📋 Fleet Detail")
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
