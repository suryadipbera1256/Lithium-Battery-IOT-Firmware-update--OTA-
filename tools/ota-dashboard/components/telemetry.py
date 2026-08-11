"""Live telemetry widget: browser -> AWS IoT Core, direct.

Python's only job is to mint a short-lived SigV4-signed WSS URL (core/wss.py)
and hand it to the iframe. From then on the socket is owned by the browser
tab: closing the tab closes the connection, there is no server-side thread,
no queue, no session_state race, and no orphaned MQTT session. Streamlit's
stateless rerun model is simply not in the data path.

PAYLOAD CONTRACT -- mirrors buildTelemetryPayload() in src/aws_iot_core.cpp:

    { thing_name, fw_version,
      health   : { uptime_s, reset, heap_free, heap_largest, reconnects, probation },
      location : { latitude, longitude, source, speed_kmh, satellites },
      sensors  : { mq2_gas_raw, mq8_gas_raw, bme680:{ temp_c, humidity_pct,
                                                      pressure_hpa, gas_res_kohm } },
      telemetry: { rssi, imei, total_odometer },
      bms      : { age_s, valid, voltage, current, soc, residual_cap, full_cap,
                   cycles, balance, protection, chg_mos, dsg_mos, temps[6],
                   cell_count, min_cell, max_cell, avg_cell, delta_cell, cells[] } }

Verified at 1038 B worst case against the firmware's 2560 B buffer.

RENDERING COMPLEXITY. One card per thing, held in a Map and mutated in place:
field updates are direct textContent writes against cached element handles, so
a message costs O(fields) with no reflow of other cards and no re-parse of
history. The raw log remains a bounded ring (oldest node dropped on overflow)
with a rAF-batched flush, so a burst cannot thrash layout.

SAFETY SIGNALS are deliberately loud, because a battery dashboard that renders
stale data as live is worse than one that renders nothing:
  * bms.valid == 0  -> the whole BMS block greys out and shows STALE with its age
  * health.probation -> amber PROBATION badge (image not yet confirmed post-OTA)
  * reset reason in {panic,task_wdt,int_wdt,wdt,brownout} -> red
  * protection flags non-zero -> red
  * heap_largest/heap_free ratio -> fragmentation bar
"""
from __future__ import annotations

import json

import streamlit.components.v1 as components

MQTT_CDN = "https://unpkg.com/mqtt@5.10.1/dist/mqtt.min.js"

_HTML = """
<div id="tw">
  <div class="bar">
    <span class="dot" id="dot"></span>
    <span id="state">connecting</span>
    <span class="sep"></span>
    <span class="lbl">NODES</span><span class="num" id="nodes">0</span>
    <span class="lbl">MSGS</span><span class="num" id="cnt">0</span>
    <span class="lbl">RATE</span><span class="num" id="rate">0.0/s</span>
    <span class="lbl">LAST</span><span class="num" id="age">--</span>
    <span class="grow"></span>
    <input id="flt" placeholder="filter node / text" spellcheck="false"/>
    <button id="vCards" class="act">CARDS</button>
    <button id="vJson">JSON</button>
    <button id="vHex">HEX</button>
    <button id="pause">PAUSE</button>
    <button id="clr">CLEAR</button>
  </div>
  <div id="topics"></div>
  <div id="cards"><div class="empty">Waiting for telemetry on __NTOPICS__ topic(s)&hellip;</div></div>
  <div id="log" class="hide"><div class="empty">Raw frames appear here&hellip;</div></div>
</div>

<style>
  #tw { font-family:"JetBrains Mono","Cascadia Code",ui-monospace,Menlo,monospace;
        color:#e4e9f0; background:#0e131b; border:1px solid #222d3d;
        border-radius:12px; overflow:hidden; }
  .bar { display:flex; align-items:center; gap:.5rem; padding:.5rem .7rem;
         background:#151d29; border-bottom:1px solid #222d3d; font-size:11px;
         flex-wrap:wrap; }
  .grow { flex:1; }
  .sep { width:1px; height:14px; background:#222d3d; }
  .lbl { color:#8b98ab; letter-spacing:.09em; }
  .num { color:#00e0a4; font-variant-numeric:tabular-nums; min-width:34px; }
  .dot { width:8px; height:8px; border-radius:50%; background:#ffb340;
         box-shadow:0 0 8px currentColor; }
  .dot.on { background:#00e0a4; } .dot.off { background:#ff5d6c; }
  #state { color:#8b98ab; text-transform:uppercase; letter-spacing:.08em; }
  #tw input { background:#0a0e14; border:1px solid #222d3d; color:#e4e9f0;
              border-radius:6px; padding:.22rem .45rem; font:inherit; font-size:11px;
              width:150px; outline:none; }
  #tw input:focus { border-color:#35a9ff; }
  #tw button { background:#0a0e14; border:1px solid #222d3d; color:#8b98ab;
               border-radius:6px; padding:.24rem .55rem; font:inherit; font-size:10px;
               letter-spacing:.08em; cursor:pointer; }
  #tw button:hover { color:#e4e9f0; border-color:#35a9ff; }
  #tw button.act { color:#0a0e14; background:#00e0a4; border-color:#00e0a4; }
  #topics { padding:.4rem .7rem; font-size:10px; color:#8b98ab;
            border-bottom:1px solid #222d3d; background:#111823;
            white-space:nowrap; overflow-x:auto; }
  #topics b { color:#35a9ff; font-weight:500; }
  .hide { display:none !important; }

  /* ---------- card grid ---------- */
  #cards { height:__HEIGHT__px; overflow-y:auto; padding:.55rem;
           display:grid; grid-template-columns:repeat(auto-fill,minmax(340px,1fr));
           gap:.55rem; align-content:start; scrollbar-width:thin; }
  .empty { color:#8b98ab; padding:.9rem .8rem; grid-column:1/-1; }
  .card { background:#111823; border:1px solid #222d3d; border-radius:10px;
          padding:.55rem .65rem; font-size:11px; }
  .card.stale { border-color:#5a3a1e; }
  .chd { display:flex; align-items:center; gap:.4rem; margin-bottom:.45rem;
         padding-bottom:.4rem; border-bottom:1px solid #1b2532; flex-wrap:wrap; }
  .cname { color:#35a9ff; font-weight:600; letter-spacing:.02em; }
  .cfw { color:#8b98ab; }
  .badge { font-size:9px; letter-spacing:.07em; padding:.1rem .32rem;
           border-radius:4px; border:1px solid currentColor; }
  .b-ok    { color:#00e0a4; } .b-warn { color:#ffb340; }
  .b-crit  { color:#ff5d6c; } .b-dim  { color:#5f6c80; }
  .sect { color:#5f6c80; font-size:9px; letter-spacing:.11em; margin:.45rem 0 .22rem; }
  .kv { display:grid; grid-template-columns:repeat(auto-fit,minmax(88px,1fr));
        gap:.15rem .5rem; }
  .kv div { display:flex; justify-content:space-between; gap:.3rem; }
  .kv span:first-child { color:#8b98ab; }
  .kv span:last-child  { color:#e4e9f0; font-variant-numeric:tabular-nums; }
  .kv .hi { color:#00e0a4; } .kv .wr { color:#ffb340; } .kv .cr { color:#ff5d6c; }
  .card.stale .bmsBlk { opacity:.42; }

  /* SOC + fragmentation meters */
  .meter { height:5px; background:#0a0e14; border-radius:3px; overflow:hidden;
           margin:.2rem 0 .3rem; }
  .meter i { display:block; height:100%; background:#00e0a4; transition:width .3s; }
  .meter i.wr { background:#ffb340; } .meter i.cr { background:#ff5d6c; }

  /* per-cell bars */
  .cells { display:flex; align-items:flex-end; gap:2px; height:34px;
           padding:.2rem .1rem; background:#0a0e14; border-radius:5px; }
  .cells i { flex:1; min-width:3px; background:#2b7fd4; border-radius:1px 1px 0 0; }
  .cells i.lo { background:#ff5d6c; } .cells i.hiC { background:#00e0a4; }
  .temps { display:flex; gap:.3rem; flex-wrap:wrap; }
  .temps b { font-weight:400; color:#e4e9f0; background:#0a0e14;
             border-radius:4px; padding:.06rem .3rem; }

  /* ---------- raw log ---------- */
  #log { height:__HEIGHT__px; overflow-y:auto; padding:.35rem 0; font-size:11px;
         line-height:1.5; scrollbar-width:thin; }
  .row { padding:.16rem .75rem; border-bottom:1px solid #161f2b;
         display:grid; grid-template-columns:64px 1fr; gap:.6rem; align-items:baseline; }
  .row:hover { background:#151d29; }
  .t { color:#5f6c80; }
  .p { white-space:pre-wrap; word-break:break-all; }
  .n { color:#35a9ff; }  .k { color:#8b98ab; } .v { color:#00e0a4; }
  .hex { color:#ffb340; letter-spacing:.04em; }
  .err { color:#ff5d6c; padding:.5rem .8rem; }
</style>

<script src="__CDN__"></script>
<script>
(function () {
  const URL_ = "__URL__", CID = "__CID__", TOPICS = __TOPICS__, MAX = __MAX__;
  const STALE_AFTER = __STALE__;              // bms.age_s beyond this reads stale
  const cardsEl = document.getElementById("cards");
  const log     = document.getElementById("log");
  const dot     = document.getElementById("dot");
  const stEl    = document.getElementById("state");
  const cntEl   = document.getElementById("cnt");
  const nodesEl = document.getElementById("nodes");
  const rateEl  = document.getElementById("rate");
  const ageEl   = document.getElementById("age");
  const flt     = document.getElementById("flt");

  document.getElementById("topics").innerHTML =
    TOPICS.map(t => "<b>&#9656;</b> " + t).join("   ");

  let n = 0, paused = false, view = "cards", last = 0, win = [];
  const pending = [];
  let scheduled = false, cleared = false, emptied = false;
  const cards = new Map();                    // thing_name -> {root, refs...}

  const pad = v => String(v).padStart(2, "0");
  const clock = d => pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
  const esc = s => String(s).replace(/[&<>]/g, c => ({ "&":"&amp;","<":"&lt;",">":"&gt;" }[c]));
  const num = (v, d) => (v === undefined || v === null || isNaN(v)) ? "--" : Number(v).toFixed(d);

  const BAD_RESET = new Set(["panic","task_wdt","int_wdt","wdt","brownout"]);

  function dur(s) {
    s = Math.max(0, Math.floor(s || 0));
    const d = Math.floor(s/86400), h = Math.floor(s%86400/3600), m = Math.floor(s%3600/60);
    if (d) return d + "d" + pad(h) + "h";
    if (h) return h + "h" + pad(m) + "m";
    return m + "m" + pad(s % 60) + "s";
  }

  function hexdump(bytes) {
    let out = "";
    for (let i = 0; i < bytes.length; i += 16) {
      const slice = bytes.subarray(i, i + 16);
      let hex = "", asc = "";
      for (let j = 0; j < slice.length; j++) {
        hex += slice[j].toString(16).padStart(2, "0") + (j === 7 ? "  " : " ");
        asc += slice[j] >= 32 && slice[j] < 127 ? String.fromCharCode(slice[j]) : ".";
      }
      out += i.toString(16).padStart(4, "0") + "  " + hex.padEnd(50) + " |" + asc + "|\\n";
    }
    return out.trimEnd();
  }

  function colorJSON(obj) {
    return JSON.stringify(obj, null, 1)
      .replace(/[&<>]/g, c => ({ "&":"&amp;","<":"&lt;",">":"&gt;" }[c]))
      .replace(/"([^"]+)":/g, '<span class="k">"$1"</span>:')
      .replace(/: (-?\\d+\\.?\\d*)/g, ': <span class="n">$1</span>')
      .replace(/: "([^"]*)"/g, ': <span class="v">"$1"</span>');
  }

  // ---------- card construction (once per node) ----------
  const CARD_HTML =
    '<div class="chd">' +
      '<span class="cname"></span><span class="cfw"></span>' +
      '<span class="grow"></span>' +
      '<span class="badge b-dim bStale"></span>' +
      '<span class="badge b-warn bProb hide">PROBATION</span>' +
      '<span class="badge b-dim bRst"></span>' +
    '</div>' +
    '<div class="sect">PACK</div>' +
    '<div class="meter"><i class="mSoc"></i></div>' +
    '<div class="kv bmsBlk">' +
      '<div><span>SOC</span><span class="vSoc">--</span></div>' +
      '<div><span>Volts</span><span class="vV">--</span></div>' +
      '<div><span>Amps</span><span class="vA">--</span></div>' +
      '<div><span>Cycles</span><span class="vCyc">--</span></div>' +
      '<div><span>Cap</span><span class="vCap">--</span></div>' +
      '<div><span>Protect</span><span class="vProt">--</span></div>' +
      '<div><span>CHG MOS</span><span class="vChg">--</span></div>' +
      '<div><span>DSG MOS</span><span class="vDsg">--</span></div>' +
    '</div>' +
    '<div class="sect">CELLS <span class="cCnt"></span></div>' +
    '<div class="cells bmsBlk"></div>' +
    '<div class="kv bmsBlk">' +
      '<div><span>min</span><span class="vMin">--</span></div>' +
      '<div><span>max</span><span class="vMax">--</span></div>' +
      '<div><span>avg</span><span class="vAvg">--</span></div>' +
      '<div><span>delta</span><span class="vDlt">--</span></div>' +
    '</div>' +
    '<div class="sect">TEMPS</div>' +
    '<div class="temps bmsBlk"></div>' +
    '<div class="sect">HEALTH</div>' +
    '<div class="meter"><i class="mFrag"></i></div>' +
    '<div class="kv">' +
      '<div><span>Uptime</span><span class="vUp">--</span></div>' +
      '<div><span>Heap</span><span class="vHeap">--</span></div>' +
      '<div><span>Largest</span><span class="vLarge">--</span></div>' +
      '<div><span>Reconn</span><span class="vRec">--</span></div>' +
      '<div><span>RSSI</span><span class="vRssi">--</span></div>' +
      '<div><span>Sats</span><span class="vSat">--</span></div>' +
    '</div>' +
    '<div class="sect">LOCATION / SENSORS</div>' +
    '<div class="kv">' +
      '<div><span>Lat</span><span class="vLat">--</span></div>' +
      '<div><span>Lon</span><span class="vLon">--</span></div>' +
      '<div><span>Src</span><span class="vSrc">--</span></div>' +
      '<div><span>Speed</span><span class="vSpd">--</span></div>' +
      '<div><span>Odo</span><span class="vOdo">--</span></div>' +
      '<div><span>BME</span><span class="vBme">--</span></div>' +
      '<div><span>MQ2</span><span class="vMq2">--</span></div>' +
      '<div><span>MQ8</span><span class="vMq8">--</span></div>' +
    '</div>';

  function makeCard(name) {
    const root = document.createElement("div");
    root.className = "card";
    root.innerHTML = CARD_HTML;
    const q = s => root.querySelector(s);
    const c = {
      root, name: q(".cname"), fw: q(".cfw"),
      bStale: q(".bStale"), bProb: q(".bProb"), bRst: q(".bRst"),
      mSoc: q(".mSoc"), soc: q(".vSoc"), v: q(".vV"), a: q(".vA"),
      cyc: q(".vCyc"), cap: q(".vCap"), prot: q(".vProt"),
      chg: q(".vChg"), dsg: q(".vDsg"),
      cCnt: q(".cCnt"), cells: q(".cells"),
      min: q(".vMin"), max: q(".vMax"), avg: q(".vAvg"), dlt: q(".vDlt"),
      temps: q(".temps"),
      mFrag: q(".mFrag"), up: q(".vUp"), heap: q(".vHeap"), large: q(".vLarge"),
      rec: q(".vRec"), rssi: q(".vRssi"), sat: q(".vSat"),
      lat: q(".vLat"), lon: q(".vLon"), src: q(".vSrc"), spd: q(".vSpd"),
      odo: q(".vOdo"), bme: q(".vBme"), mq2: q(".vMq2"), mq8: q(".vMq8"),
      bars: [],
    };
    c.name.textContent = name;
    cardsEl.appendChild(root);
    cards.set(name, c);
    nodesEl.textContent = cards.size;
    return c;
  }

  // ---------- card update: O(fields), no reflow of siblings ----------
  function updateCard(d) {
    const name = d.thing_name || "unknown";
    let c = cards.get(name);
    if (!c) {
      if (!emptied) { cardsEl.innerHTML = ""; emptied = true; }
      c = makeCard(name);
    }
    c.fw.textContent = "v" + (d.fw_version || "?");

    const h = d.health || {}, b = d.bms || {}, l = d.location || {},
          s = d.sensors || {}, bme = s.bme680 || {}, t = d.telemetry || {};

    /* --- staleness: the safety-critical signal. readBMS() only stamps
       lastUpdate on a sweep that parsed a frame, so age_s is trustworthy;
       -1 means the pack has never been read at all. --- */
    const age = (b.age_s === undefined) ? -1 : Number(b.age_s);
    const stale = (b.valid === 0) || age < 0 || age > STALE_AFTER;
    c.root.classList.toggle("stale", stale);
    c.bStale.className = "badge bStale " + (stale ? "b-warn" : "b-ok");
    c.bStale.textContent = age < 0 ? "NO CAN" : (stale ? "STALE " + age + "s" : "LIVE " + age + "s");

    c.bProb.classList.toggle("hide", !h.probation);

    const rst = h.reset || "?";
    c.bRst.className = "badge bRst " + (BAD_RESET.has(rst) ? "b-crit" : "b-dim");
    c.bRst.textContent = rst;

    // pack
    const soc = Number(b.soc || 0);
    c.mSoc.style.width = Math.max(0, Math.min(100, soc)) + "%";
    c.mSoc.className = "mSoc" + (soc < 15 ? " cr" : soc < 30 ? " wr" : "");
    c.soc.textContent = soc + "%";
    c.v.textContent = num(b.voltage, 2) + "V";
    c.a.textContent = num(b.current, 2) + "A";
    c.a.className = "vA " + (Number(b.current) < 0 ? "wr" : "hi");
    c.cyc.textContent = b.cycles === undefined ? "--" : b.cycles;
    c.cap.textContent = num(b.residual_cap, 1) + "/" + num(b.full_cap, 1);
    const prot = Number(b.protection || 0);
    c.prot.textContent = prot ? "0x" + prot.toString(16).toUpperCase() : "none";
    c.prot.className = "vProt " + (prot ? "cr" : "hi");
    c.chg.textContent = b.chg_mos ? "ON" : "OFF";
    c.chg.className = "vChg " + (b.chg_mos ? "hi" : "wr");
    c.dsg.textContent = b.dsg_mos ? "ON" : "OFF";
    c.dsg.className = "vDsg " + (b.dsg_mos ? "hi" : "wr");

    // cells -- reuse bar nodes; only rebuild when the count changes
    const cv = Array.isArray(b.cells) ? b.cells : [];
    c.cCnt.textContent = cv.length ? "(" + cv.length + ")" : "";
    if (c.bars.length !== cv.length) {
      c.cells.innerHTML = "";
      c.bars = cv.map(() => c.cells.appendChild(document.createElement("i")));
    }
    if (cv.length) {
      const mn = Number(b.min_cell), mx = Number(b.max_cell);
      const span = (mx - mn) || 0.001;
      for (let i = 0; i < cv.length; i++) {
        const val = Number(cv[i]);
        // Height encodes position within the pack's own spread, which is what
        // makes an outlier visible -- absolute 3.9-4.2 V would look flat.
        c.bars[i].style.height = (18 + 82 * ((val - mn) / span)) + "%";
        c.bars[i].className = val === mn ? "lo" : val === mx ? "hiC" : "";
        c.bars[i].title = "cell " + (i + 1) + ": " + val.toFixed(3) + " V";
      }
    }
    c.min.textContent = num(b.min_cell, 3);
    c.max.textContent = num(b.max_cell, 3);
    c.avg.textContent = num(b.avg_cell, 3);
    const dlt = Number(b.delta_cell || 0);
    c.dlt.textContent = num(dlt, 3);
    c.dlt.className = "vDlt " + (dlt > 0.1 ? "cr" : dlt > 0.05 ? "wr" : "hi");

    const tv = Array.isArray(b.temps) ? b.temps : [];
    c.temps.innerHTML = tv.length
      ? tv.map(x => "<b>" + num(x, 1) + "&deg;</b>").join("")
      : '<span style="color:#5f6c80">--</span>';

    // health -- largest/free IS the fragmentation metric
    const free = Number(h.heap_free || 0), large = Number(h.heap_largest || 0);
    const fragPct = free ? (large / free) * 100 : 0;
    c.mFrag.style.width = Math.max(0, Math.min(100, fragPct)) + "%";
    c.mFrag.className = "mFrag" + (fragPct < 40 ? " cr" : fragPct < 65 ? " wr" : "");
    c.mFrag.parentElement.title =
      "largest allocatable / total free = " + fragPct.toFixed(0) + "%  (low = fragmented)";
    c.up.textContent    = dur(h.uptime_s);
    c.heap.textContent  = (free / 1024).toFixed(1) + "k";
    c.large.textContent = (large / 1024).toFixed(1) + "k";
    c.rec.textContent   = h.reconnects === undefined ? "--" : h.reconnects;
    c.rssi.textContent  = t.rssi === undefined ? "--" : t.rssi;
    c.sat.textContent   = l.satellites === undefined ? "--" : l.satellites;

    c.lat.textContent = num(l.latitude, 5);
    c.lon.textContent = num(l.longitude, 5);
    c.src.textContent = l.source || "--";
    c.src.className = "vSrc " + (l.source === "GPS" ? "hi" : "wr");
    c.spd.textContent = num(l.speed_kmh, 1);
    c.odo.textContent = num(t.total_odometer, 1);
    c.bme.textContent = num(bme.temp_c, 1) + "&deg;/" + num(bme.humidity_pct, 0) + "%";
    c.bme.innerHTML   = num(bme.temp_c, 1) + "&deg; " + num(bme.humidity_pct, 0) + "%";
    c.mq2.textContent = s.mq2_gas_raw === undefined ? "--" : s.mq2_gas_raw;
    c.mq8.textContent = s.mq8_gas_raw === undefined ? "--" : s.mq8_gas_raw;
  }

  function flush() {
    scheduled = false;
    if (cleared) { log.innerHTML = ""; cleared = false; }
    const frag = document.createDocumentFragment();
    for (const html of pending) {
      const el = document.createElement("div");
      el.className = "row";
      el.innerHTML = html;
      frag.appendChild(el);
    }
    pending.length = 0;
    const stick = log.scrollTop + log.clientHeight >= log.scrollHeight - 40;
    log.appendChild(frag);
    while (log.childElementCount > MAX) log.removeChild(log.firstElementChild);
    if (stick) log.scrollTop = log.scrollHeight;
  }

  function push(topic, payload) {
    n++; last = Date.now(); win.push(last);
    if (paused) return;

    let obj = null, text = "", cls = "p";
    const raw = new TextDecoder().decode(payload);
    try { obj = JSON.parse(raw); } catch (e) { obj = null; }

    // Cards always stay current, even while the raw log is the visible pane.
    if (obj) { try { updateCard(obj); } catch (e) { /* never break the socket */ } }

    const needle = flt.value.trim();
    if (view === "hex" || !obj) { text = esc(hexdump(payload)); cls = "p hex"; }
    else { text = colorJSON(obj); }
    if (needle && !text.includes(needle) && !topic.includes(needle)) return;

    const node = (obj && obj.thing_name) || topic.split("/")[2] || topic;
    pending.push('<span class="t">' + clock(new Date(last)) + '</span>' +
                 '<span class="' + cls + '"><span class="n">' + esc(node) +
                 '</span>  ' + text + '</span>');
    if (!scheduled) { scheduled = true; requestAnimationFrame(flush); }
  }

  setInterval(function () {
    const now = Date.now();
    while (win.length && now - win[0] > 10000) win.shift();
    cntEl.textContent = n;
    rateEl.textContent = (win.length / 10).toFixed(1) + "/s";
    ageEl.textContent = last ? Math.round((now - last) / 1000) + "s" : "--";
  }, 1000);

  function setView(v) {
    view = v;
    cardsEl.classList.toggle("hide", v !== "cards");
    log.classList.toggle("hide", v === "cards");
    for (const [id, name] of [["vCards","cards"],["vJson","json"],["vHex","hex"]])
      document.getElementById(id).classList.toggle("act", v === name);
  }
  document.getElementById("vCards").onclick = () => setView("cards");
  document.getElementById("vJson").onclick  = () => setView("json");
  document.getElementById("vHex").onclick   = () => setView("hex");

  document.getElementById("pause").onclick = function () {
    paused = !paused; this.classList.toggle("act", paused);
    this.textContent = paused ? "RESUME" : "PAUSE";
  };
  document.getElementById("clr").onclick = function () {
    log.innerHTML = ""; pending.length = 0; n = 0; win = []; cntEl.textContent = "0";
    cards.clear(); cardsEl.innerHTML = ""; emptied = false; nodesEl.textContent = "0";
  };

  if (typeof mqtt === "undefined") {
    cardsEl.innerHTML = '<div class="err">mqtt.js failed to load from the CDN. ' +
                        'Vendor it locally if this host has no egress.</div>';
    dot.className = "dot off"; stEl.textContent = "no client";
    return;
  }

  const client = mqtt.connect(URL_, {
    clientId: CID, protocolVersion: 5, clean: true,
    keepalive: 30, reconnectPeriod: 4000, connectTimeout: 8000,
  });

  client.on("connect", function () {
    dot.className = "dot on"; stEl.textContent = "live";
    cleared = true;
    TOPICS.forEach(t => client.subscribe(t, { qos: 0 }, function (err) {
      if (err) { stEl.textContent = "subscribe denied"; dot.className = "dot off"; }
    }));
  });
  client.on("message", (t, p) => push(t, p));
  client.on("reconnect", () => { dot.className = "dot"; stEl.textContent = "reconnecting"; });
  client.on("close",    () => { dot.className = "dot off"; stEl.textContent = "closed"; });
  client.on("error", function (e) {
    dot.className = "dot off";
    stEl.textContent = "error";
    log.classList.remove("hide");
    log.innerHTML += '<div class="err">' + (e && e.message ? e.message : e) +
      ' &mdash; a 403 here means the signed URL expired (rerun the page) or the IAM ' +
      'policy lacks iot:Connect / iot:Subscribe on these topics.</div>';
  });
  window.addEventListener("beforeunload", () => client.end(true));
})();
</script>
"""


def render(signed: str, topics: list[str], client_id: str,
           height: int = 430, max_rows: int = 300,
           stale_after_s: int = 15) -> None:
    """Embed the widget.

    stale_after_s should mirror the firmware's own validity window:
    bmsReadInterval * 3 == 15 s (see buildTelemetryPayload). Passing the same
    number keeps the dashboard's STALE badge and the device's `valid` flag from
    disagreeing.
    """
    html = (
        _HTML
        .replace("__CDN__", MQTT_CDN)
        .replace("__URL__", signed)
        .replace("__CID__", client_id)
        .replace("__TOPICS__", json.dumps(topics))
        .replace("__NTOPICS__", str(len(topics)))
        .replace("__MAX__", str(max_rows))
        .replace("__HEIGHT__", str(height))
        .replace("__STALE__", str(int(stale_after_s)))
    )
    components.html(html, height=height + 132, scrolling=False)
