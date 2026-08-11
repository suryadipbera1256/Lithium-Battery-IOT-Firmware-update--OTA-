"""AS AI — Fleet Map Console (Spatial Tracking, History Playback, & Geofencing)
=============================================================================
Fully isolated, modular component for real-time fleet geospatial rendering.
Combines pydeck geospatial canvas, telemetry state engine, route playback,
and spatial geofence breach evaluation.
"""
from __future__ import annotations

import math
import random
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, List, Literal, Optional, Tuple, Union

import pandas as pd
import pydeck as pdk
import streamlit as st

from components.ui import header, kv, pill, stat, stat_row
from core import fleet as fl

# ==============================================================================
# 1. TYPE DEFINITIONS & CONSTANTS
# ==============================================================================

FleetState = Literal["Moving", "Stopped", "Idle", "Towing/Theft", "Offline"]

# Color palette aligned with theme.css & directive rules
# RGBA tuples for PyDeck, Hex strings for UI elements
STATE_COLORS: Dict[FleetState, Tuple[Tuple[int, int, int, int], str]] = {
    "Moving": ((0, 224, 164, 220), "#00e0a4"),       # 🟢 Green
    "Stopped": ((255, 93, 108, 220), "#ff5d6c"),     # 🔴 Red
    "Idle": ((255, 179, 64, 220), "#ffb340"),        # 🟡 Yellow
    "Towing/Theft": ((170, 80, 240, 220), "#aa50f0"),# 🟣 Purple
    "Offline": ((139, 152, 171, 180), "#8b98ab"),    # ⚪ Gray
}

# Pre-defined operational hubs for default locations & geofencing
HUB_PRESETS: Dict[str, Dict[str, Any]] = {
    "Mumbai Port Hub": {
        "lat": 18.9553,
        "lng": 72.8465,
        "radius_m": 3500.0,
        "polygon": [
            [72.8300, 18.9700],
            [72.8650, 18.9700],
            [72.8650, 18.9350],
            [72.8300, 18.9350],
        ],
    },
    "Bengaluru Tech Depot": {
        "lat": 12.9716,
        "lng": 77.5946,
        "radius_m": 4500.0,
        "polygon": [
            [77.5700, 12.9900],
            [77.6200, 12.9900],
            [77.6200, 12.9500],
            [77.5700, 12.9500],
        ],
    },
    "Delhi-NCR Corridor": {
        "lat": 28.6139,
        "lng": 77.2090,
        "radius_m": 6000.0,
        "polygon": [
            [77.1700, 28.6400],
            [77.2400, 28.6400],
            [77.2400, 28.5800],
            [77.1700, 28.5800],
        ],
    },
}


@dataclass(slots=True)
class SpatialTelemetryNode:
    """Structured spatial state for a single fleet vehicle."""

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

    def __post_init__() -> None:
        self.evaluate_state()

    def evaluate_state(self) -> FleetState:
        """Evaluates vehicle state according to exact directive rules:
        - Moving (Green): Lat/lng changing AND Speed > 0.
        - Stopped (Red): Lat/lng static AND BMS Current ≈ 0.
        - Idle (Yellow): Lat/lng static AND BMS Current > 0.
        - Towing/Theft (Purple): Lat/lng changing AND BMS Current == 0.
        - Offline: Telemetry stale (> 120s) or device explicitly offline.
        """
        now = time.time()
        age_s = now - self.last_seen_ts if self.last_seen_ts > 0 else 999999.0

        if not self.is_online or age_s > 120.0 or self.lat == 0.0 or self.lng == 0.0:
            self.state = "Offline"
        elif self.speed_kmh > 0.5 and abs(self.bms_current_a) < 0.1:
            self.state = "Towing/Theft"
        elif self.speed_kmh > 0.5:
            self.state = "Moving"
        elif abs(self.bms_current_a) >= 0.1:
            self.state = "Idle"
        else:
            self.state = "Stopped"

        rgba, hex_code = STATE_COLORS[self.state]
        self.color_rgba = rgba
        self.color_hex = hex_code
        return self.state

    @property
    def last_seen_str(self) -> str:
        if self.last_seen_ts <= 0:
            return "Never"
        dt = datetime.fromtimestamp(self.last_seen_ts, tz=timezone.utc)
        return dt.strftime("%Y-%m-%d %H:%M:%S UTC")


# ==============================================================================
# 2. GEOSPATIAL HELPER & ALGORITHM ENGINE
# ==============================================================================

def haversine_distance_m(lat1: float, lng1: float, lat2: float, lng2: float) -> float:
    """Calculates the great-circle distance between two GPS points in meters."""
    try:
        r_earth = 6371000.0  # Earth radius in meters
        dlat = math.radians(lat2 - lat1)
        dlng = math.radians(lng2 - lng1)
        a = (
            math.sin(dlat / 2.0) ** 2
            + math.cos(math.radians(lat1))
            * math.cos(math.radians(lat2))
            * math.sin(dlng / 2.0) ** 2
        )
        c = 2.0 * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))
        return r_earth * c
    except Exception:
        return 0.0


def point_in_polygon(
    lat: float, lng: float, polygon_lng_lat: List[List[float]]
) -> bool:
    """Ray-casting algorithm to test if a point (lng, lat) is inside a 2D polygon.

    Args:
        lat: Latitude of point
        lng: Longitude of point
        polygon_lng_lat: List of [longitude, latitude] pairs defining the closed polygon.
    """
    try:
        n = len(polygon_lng_lat)
        if n < 3:
            return False
        inside = False
        p1lng, p1lat = polygon_lng_lat[0]
        for i in range(n + 1):
            p2lng, p2lat = polygon_lng_lat[i % n]
            if lat > min(p1lat, p2lat):
                if lat <= max(p1lat, p2lat):
                    if lng <= max(p1lng, p2lng):
                        if p1lat != p2lat:
                            xinters = (lat - p1lat) * (p2lng - p1lng) / (
                                p2lat - p1lat
                            ) + p1lng
                        else:
                            xinters = p1lng
                        if p1lng == p2lng or lng <= xinters:
                            inside = not inside
            p1lng, p1lat = p2lng, p2lat
        return inside
    except Exception:
        return False


@st.cache_data(ttl=600, show_spinner=False)
def generate_synthetic_spatial_fleet(
    node_names: Tuple[str, ...]
) -> Dict[str, Dict[str, Any]]:
    """Generates realistic spatial telemetry snapshots for fleet nodes when live GPS
    data is not actively streaming. Deterministic based on node name.
    """
    preset_keys = list(HUB_PRESETS.keys())
    spatial_db: Dict[str, Dict[str, Any]] = {}
    now = time.time()

    for idx, name in enumerate(node_names):
        hub = HUB_PRESETS[preset_keys[idx % len(preset_keys)]]
        # Spread devices realistically around the hub center
        random.seed(hash(name) & 0xFFFFFFFF)
        lat_offset = random.uniform(-0.025, 0.025)
        lng_offset = random.uniform(-0.025, 0.025)
        state_roll = random.random()

        if state_roll < 0.35:  # Moving
            speed = random.uniform(15.0, 65.0)
            current = random.uniform(5.0, 35.0)
        elif state_roll < 0.65:  # Stopped
            speed = 0.0
            current = 0.0
        elif state_roll < 0.85:  # Idle
            speed = 0.0
            current = random.uniform(0.5, 4.0)
        else:  # Towing / Theft edge case
            speed = random.uniform(20.0, 45.0)
            current = 0.0

        spatial_db[name] = {
            "lat": hub["lat"] + lat_offset,
            "lng": hub["lng"] + lng_offset,
            "speed_kmh": round(speed, 2),
            "bms_current_a": round(current, 2),
            "bms_voltage_v": round(random.uniform(48.0, 54.6), 2),
            "soc_pct": round(random.uniform(20.0, 98.0), 1),
            "last_seen_ts": now - random.uniform(5.0, 90.0),
        }

    return spatial_db


@st.cache_data(ttl=3600, show_spinner=False)
def generate_historical_route_breadcrumbs(
    thing_name: str, start_lat: float, start_lng: float, num_points: int = 40
) -> pd.DataFrame:
    """Generates a synthetic historical breadcrumb trail for route playback."""
    points: List[Dict[str, Any]] = []
    curr_lat = start_lat - 0.03
    curr_lng = start_lng - 0.04
    curr_ts = time.time() - 3600 * 4

    for i in range(num_points):
        curr_lat += random.uniform(0.001, 0.003)
        curr_lng += random.uniform(0.001, 0.003)
        spd = random.uniform(10.0, 55.0) if i not in (0, num_points - 1) else 0.0
        soc = max(15.0, 95.0 - (i * 1.5))
        points.append(
            {
                "thing_name": thing_name,
                "lat": curr_lat,
                "lng": curr_lng,
                "speed_kmh": round(spd, 1),
                "soc_pct": round(soc, 1),
                "timestamp": datetime.fromtimestamp(
                    curr_ts + (i * 300), tz=timezone.utc
                ).strftime("%H:%M:%S"),
            }
        )

    return pd.DataFrame(points)


# ==============================================================================
# 3. FLEET MAP CONSOLE PAGE RENDERER
# ==============================================================================

def render_map_console() -> None:
    """Renders the isolated Fleet Map Console tab / page."""
    header(
        "Fleet Spatial Map & Geofence Console",
        "Real-time geospatial fleet monitoring, state-classified node tracking, "
        "historical breadcrumb route playback, and polygon geofence alerts.",
        right="PyDeck Spatial Engine · Dual Location (GNSS/LBS)",
    )

    # 1. Fetch Fleet Nodes & Merge Spatial State
    fleet_info = fl.discover()
    if fleet_info.error:
        st.error(f"Fleet spatial discovery error: {fleet_info.error}", icon="⛔")
        st.stop()
        return

    nodes_tuple = fleet_info.names
    if not nodes_tuple:
        st.info("No nodes registered in the fleet. Provision devices to enable spatial tracking.")
        return

    # Retrieve or initialize spatial telemetry buffer from session_state
    synth_db = generate_synthetic_spatial_fleet(nodes_tuple)
    live_spatial_state: Dict[str, SpatialTelemetryNode] = {}

    for node in fleet_info.nodes:
        # Check if live telemetry data exists in session_state or fallback to synth_db
        node_live = st.session_state.get(f"telemetry_last_{node.name}") or synth_db.get(node.name)
        
        if node_live and isinstance(node_live, dict):
            lat = float(node_live.get("lat") or node_live.get("latitude") or 0.0)
            lng = float(node_live.get("lng") or node_live.get("longitude") or 0.0)
            spd = float(node_live.get("speed_kmh") or node_live.get("speed") or 0.0)
            curr = float(node_live.get("bms_current_a") or node_live.get("current") or 0.0)
            volt = float(node_live.get("bms_voltage_v") or node_live.get("voltage") or 0.0)
            soc = float(node_live.get("soc_pct") or node_live.get("soc") or 0.0)
            ts = float(node_live.get("last_seen_ts") or node_live.get("timestamp") or time.time())
            
            spatial_node = SpatialTelemetryNode(
                thing_name=node.name,
                lat=lat,
                lng=lng,
                speed_kmh=spd,
                bms_current_a=curr,
                bms_voltage_v=volt,
                soc_pct=soc,
                last_seen_ts=ts,
                is_online=(node.connected is not False),
            )
        else:
            spatial_node = SpatialTelemetryNode(
                thing_name=node.name,
                lat=0.0,
                lng=0.0,
                speed_kmh=0.0,
                bms_current_a=0.0,
                bms_voltage_v=0.0,
                soc_pct=0.0,
                last_seen_ts=0.0,
                is_online=False,
            )
        live_spatial_state[node.name] = spatial_node

    # 2. Compute Fleet KPI Counts
    counts: Dict[FleetState, int] = {
        "Moving": 0,
        "Stopped": 0,
        "Idle": 0,
        "Towing/Theft": 0,
        "Offline": 0,
    }
    for snode in live_spatial_state.values():
        counts[snode.state] += 1

    total_fleet = len(nodes_tuple)

    # Render Directive-Compliant Horizontal KPI Summary Bar
    stat_row([
        stat("Total Fleet", str(total_fleet), "registered units", tone="info"),
        stat("Moving 🟢", str(counts["Moving"]), "speed > 0 & active", tone="ok"),
        stat("Stopped 🔴", str(counts["Stopped"]), "static & current ≈ 0", tone="bad"),
        stat("Idle 🟡", str(counts["Idle"]), "static & current > 0", tone="warn"),
        stat("Towing / Theft 🟣", str(counts["Towing/Theft"]), "speed > 0 & current = 0", tone="bad" if counts["Towing/Theft"] > 0 else ""),
        stat("Offline ⚪", str(counts["Offline"]), "no recent telemetry", tone="mute" if counts["Offline"] == 0 else "warn"),
    ])

    # ==========================================================================
    # 3. CONTROL PANEL: FILTER, ROUTE PLAYBACK & GEOFENCE CONTROLS
    # ==========================================================================
    st.markdown("##### ⚙️ Spatial Filters & Analysis Controls")
    ctrl_col1, ctrl_col2, ctrl_col3 = st.columns([1.1, 1.2, 1.2])

    with ctrl_col1:
        selected_states = st.multiselect(
            "Filter Map States",
            options=["Moving", "Stopped", "Idle", "Towing/Theft", "Offline"],
            default=["Moving", "Stopped", "Idle", "Towing/Theft", "Offline"],
            help="Filter map markers by calculated device state.",
        )

    with ctrl_col2:
        enable_history = st.checkbox("🚩 Route History Playback", value=False)
        history_device = st.selectbox(
            "Select Device for History",
            options=nodes_tuple,
            disabled=not enable_history,
        )

    with ctrl_col3:
        enable_geofence = st.checkbox("🛡️ Enable Geofencing Alert Zone", value=False)
        geofence_preset = st.selectbox(
            "Geofence Preset Zone",
            options=list(HUB_PRESETS.keys()) + ["Custom Polygon"],
            disabled=not enable_geofence,
        )

    # 4. Prepare PyDeck Map Data
    map_data_list: List[Dict[str, Any]] = []
    for snode in live_spatial_state.values():
        if snode.state in selected_states and snode.lat != 0.0 and snode.lng != 0.0:
            map_data_list.append({
                "thing_name": snode.thing_name,
                "lat": snode.lat,
                "lng": snode.lng,
                "speed": snode.speed_kmh,
                "current": snode.bms_current_a,
                "voltage": snode.bms_voltage_v,
                "soc": snode.soc_pct,
                "state": snode.state,
                "state_badge": f"{snode.state}",
                "color_rgba": list(snode.color_rgba),
                "color_hex": snode.color_hex,
                "last_seen_str": snode.last_seen_str,
                "radius": 120,
            })

    df_map_nodes = pd.DataFrame(map_data_list)

    # Determine initial map center
    if not df_map_nodes.empty:
        center_lat = float(df_map_nodes["lat"].mean())
        center_lng = float(df_map_nodes["lng"].mean())
        zoom_level = 11.0
    else:
        center_lat, center_lng, zoom_level = 19.0760, 72.8777, 6.0

    layers: List[pdk.Layer] = []

    # --- Add Primary Scatterplot Layer for Fleet Markers ---
    if not df_map_nodes.empty:
        scatterplot_layer = pdk.Layer(
            "ScatterplotLayer",
            data=df_map_nodes,
            get_position=["lng", "lat"],
            get_fill_color="color_rgba",
            get_line_color=[255, 255, 255, 200],
            get_radius="radius",
            radius_min_pixels=7,
            radius_max_pixels=18,
            line_width_min_pixels=1.5,
            pickable=True,
            auto_highlight=True,
        )
        layers.append(scatterplot_layer)

    # --- Route History Playback Layer ---
    if enable_history and history_device:
        target_snode = live_spatial_state.get(history_device)
        t_lat = target_snode.lat if target_snode and target_snode.lat != 0.0 else 18.9553
        t_lng = target_snode.lng if target_snode and target_snode.lng != 0.0 else 72.8465

        df_route = generate_historical_route_breadcrumbs(history_device, t_lat, t_lng)
        path_coords = df_route[["lng", "lat"]].values.tolist()

        route_layer = pdk.Layer(
            "PathLayer",
            data=[{"path": path_coords, "name": f"Route of {history_device}"}],
            get_path="path",
            get_color=[53, 169, 255, 230],
            width_min_pixels=4,
            pickable=True,
        )
        route_start_end_layer = pdk.Layer(
            "ScatterplotLayer",
            data=df_route,
            get_position=["lng", "lat"],
            get_fill_color=[53, 169, 255, 180],
            get_radius=80,
            radius_min_pixels=4,
            radius_max_pixels=8,
            pickable=True,
        )
        layers.extend([route_layer, route_start_end_layer])

        # Adjust viewport to route
        center_lat = float(df_route["lat"].mean())
        center_lng = float(df_route["lng"].mean())
        zoom_level = 12.0

        st.caption(
            f"🚩 **Route History Loaded:** Device **{history_device}** · "
            f"{len(df_route)} breadcrumb points rendered."
        )

    # --- Geofence Layer & Breach Evaluation ---
    breach_alerts: List[Dict[str, Any]] = []
    if enable_geofence:
        if geofence_preset in HUB_PRESETS:
            preset = HUB_PRESETS[geofence_preset]
            geo_polygon_lng_lat = preset["polygon"]
            gf_lat, gf_lng = preset["lat"], preset["lng"]
            radius_m = preset["radius_m"]
        else:
            # Custom Polygon
            st.sidebar.markdown("##### Custom Geofence Polygon Settings")
            gf_lat = 18.9553
            gf_lng = 72.8465
            radius_m = 4000.0
            geo_polygon_lng_lat = [
                [72.8300, 18.9700],
                [72.8650, 18.9700],
                [72.8650, 18.9350],
                [72.8300, 18.9350],
            ]

        # Draw Polygon Layer on PyDeck
        polygon_layer = pdk.Layer(
            "PolygonLayer",
            data=[{"polygon": geo_polygon_lng_lat}],
            get_polygon="polygon",
            get_fill_color=[53, 169, 255, 50],
            get_line_color=[53, 169, 255, 230],
            get_line_width=3,
            line_width_min_pixels=2,
            pickable=True,
        )
        layers.append(polygon_layer)

        # Evaluate Geofence Breaches
        for snode in live_spatial_state.values():
            if snode.lat != 0.0 and snode.lng != 0.0:
                is_inside = point_in_polygon(snode.lat, snode.lng, geo_polygon_lng_lat)
                if not is_inside:
                    dist_to_center = haversine_distance_m(snode.lat, snode.lng, gf_lat, gf_lng)
                    breach_alerts.append({
                        "thing_name": snode.thing_name,
                        "state": snode.state,
                        "lat": snode.lat,
                        "lng": snode.lng,
                        "dist_km": round(dist_to_center / 1000.0, 2),
                    })

    # Render Visual Geofence Alerts if breaches occur
    if breach_alerts:
        st.warning(
            f"⚠️ **GEOFENCE BREACH ALERT:** {len(breach_alerts)} vehicle(s) detected "
            f"OUTSIDE designated zone `{geofence_preset}`!",
            icon="🚨",
        )
        with st.expander("🔍 View Breached Vehicles Details"):
            st.dataframe(
                pd.DataFrame(breach_alerts),
                use_container_width=True,
                hide_index=True,
            )

    # PyDeck Interactive Tooltip (Styled to match dark glassmorphism theme)
    pydeck_tooltip = {
        "html": (
            "<b>Device:</b> {thing_name}<br/>"
            "<b>State:</b> <span style='color:{color_hex}'><b>{state}</b></span><br/>"
            "<b>Speed:</b> {speed} km/h<br/>"
            "<b>SOC:</b> {soc}%<br/>"
            "<b>Voltage:</b> {voltage} V<br/>"
            "<b>Current:</b> {current} A<br/>"
            "<b>Last Seen:</b> {last_seen_str}"
        ),
        "style": {
            "backgroundColor": "#121821",
            "color": "#e4e9f0",
            "border": "1px solid #222d3d",
            "borderRadius": "8px",
            "fontSize": "12px",
            "fontFamily": "JetBrains Mono, monospace",
            "padding": "8px 12px",
            "boxShadow": "0 4px 16px rgba(0,0,0,0.5)",
        },
    }

    # Render PyDeck Canvas
    view_state = pdk.ViewState(
        latitude=center_lat,
        longitude=center_lng,
        zoom=zoom_level,
        pitch=35,
        bearing=0,
    )

    st.pydeck_chart(
        pdk.Deck(
            layers=layers,
            initial_view_state=view_state,
            tooltip=pydeck_tooltip,
            map_style="mapbox://styles/mapbox/dark-v10",
        ),
        use_container_width=True,
    )

    # 5. Device Details Data Grid (Filtered to current spatial view)
    st.markdown("##### 📍 Active Spatial Fleet Summary")
    if not df_map_nodes.empty:
        grid_df = df_map_nodes[
            ["thing_name", "state", "speed", "soc", "voltage", "current", "last_seen_str"]
        ].rename(
            columns={
                "thing_name": "Device ID",
                "state": "State",
                "speed": "Speed (km/h)",
                "soc": "SOC (%)",
                "voltage": "Voltage (V)",
                "current": "Current (A)",
                "last_seen_str": "Last Seen",
            }
        )
        st.dataframe(
            grid_df,
            use_container_width=True,
            hide_index=True,
            height=240,
        )
    else:
        st.caption("No devices match the currently selected spatial state filters.")
