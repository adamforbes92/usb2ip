// IgnitronUSB gauge UI — WebSocket-driven dark dashboard.
// Channel keys match the firmware TelemetryStore JSON ("rpm", "map", ...).

const CH = {
  rpm:       { label: "RPM",        unit: "",     min: 0,   max: 8000 },
  map:       { label: "MAP",        unit: "kPa",  min: 0,   max: 250 },
  tps:       { label: "Throttle",   unit: "%",    min: 0,   max: 100 },
  tpp:       { label: "Pedal",      unit: "%",    min: 0,   max: 100 },
  clt:       { label: "Coolant",    unit: "\u00B0C", min: -40, max: 130, warn: 105, alarm: 115 },
  lambda:    { label: "Lambda",     unit: "\u03BB", min: 0.6, max: 1.4, dp: 2 },
  iat:       { label: "Intake Air", unit: "\u00B0C", min: -40, max: 100 },
  batt:      { label: "Battery",    unit: "V",    min: 8,   max: 16, dp: 1, alarmLow: 11.5 },
  inj_duty:  { label: "Inj Duty",   unit: "%",    min: 0,   max: 100, warn: 85 },
  ign_adv:   { label: "Ign Adv",    unit: "\u00B0", min: -10, max: 45 },
  oil_press: { label: "Oil Press",  unit: "Bar",  min: 0,   max: 8, dp: 2, alarmLow: 0.8 },
  fuel_press:{ label: "Fuel Press", unit: "Bar",  min: 0,   max: 6, dp: 1 },
  vss:       { label: "Speed",      unit: "km/h", min: 0,   max: 300 },
};

const ACCENT = "#24d1c4", ACCENT2 = "#ffb02e", DANGER = "#ff4d5e",
      MUTED = "#7d8ea0", TRACK = "#24303b", TEXT = "#e6edf3";

// Smoothed display values so needles glide instead of jumping.
const shown = {};

function fmt(key, v) {
  const c = CH[key];
  const dp = c && c.dp != null ? c.dp : 0;
  return v.toFixed(dp);
}

function colorFor(key, v) {
  const c = CH[key];
  if (!c) return ACCENT;
  if (c.alarm != null && v >= c.alarm) return DANGER;
  if (c.alarmLow != null && v <= c.alarmLow) return DANGER;
  if (c.warn != null && v >= c.warn) return ACCENT2;
  return ACCENT;
}

// --- radial gauge --------------------------------------------------------
// Wrap a label onto at most `maxLines` lines that fit `maxW`, ellipsising the
// last one if the channel name is still too long (Ignitron's names run to
// "Variable valve timing camshaft #1 adjustment solenoid duty cycle").
function wrapLabel(ctx, text, maxW, maxLines) {
  const words = String(text || "").split(" ");
  const lines = [];
  let cur = "";
  for (const w of words) {
    const trial = cur ? cur + " " + w : w;
    if (!cur || ctx.measureText(trial).width <= maxW) cur = trial;
    else { lines.push(cur); cur = w; }
  }
  if (cur) lines.push(cur);
  if (lines.length > maxLines) { lines.length = maxLines; lines[maxLines - 1] += "\u2026"; }
  return lines.map((l) => {
    while (l.length > 1 && ctx.measureText(l).width > maxW) l = l.slice(0, -2) + "\u2026";
    return l;
  });
}

class Gauge {
  constructor(canvas, key) {
    this.canvas = canvas;
    this.key = key;
    this.big = canvas.classList.contains("gauge-lg");
    this.ctx = canvas.getContext("2d");
    this.resize();
    window.addEventListener("resize", () => this.resize());
  }
  resize() {
    const dpr = window.devicePixelRatio || 1;
    const r = this.canvas.getBoundingClientRect();
    this.canvas.width = r.width * dpr;
    this.canvas.height = r.height * dpr;
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.w = r.width; this.h = r.height;
    this.draw(shown[this.key]);   // undefined until data (or a sweep) arrives -> drawn empty
  }
  // v == null draws the empty dial: track, ticks, "--". A gauge with no data
  // must never be drawn at 0 - on a -40..140 coolant dial that is a third of
  // the way round and looks like a reading.
  draw(v) {
    const c = CH[this.key] || { label: this.key, unit: "", min: 0, max: 100 };
    const ctx = this.ctx, big = this.big;
    const cx = this.w / 2, cy = this.h * 0.60;
    const rad = Math.min(this.w, this.h) * (big ? 0.42 : 0.40);
    const A0 = Math.PI * 0.75, A1 = Math.PI * 2.25; // 270 deg sweep, open at the bottom
    const has = v != null && isFinite(v);
    const t = has ? Math.max(0, Math.min(1, (v - c.min) / (c.max - c.min))) : 0;
    const col = has ? colorFor(this.key, v) : MUTED;

    ctx.clearRect(0, 0, this.w, this.h);

    // track
    ctx.lineCap = "round";
    ctx.lineWidth = big ? 16 : 12;
    ctx.strokeStyle = TRACK;
    ctx.beginPath(); ctx.arc(cx, cy, rad, A0, A1); ctx.stroke();

    // value arc
    if (has && t > 0) {
      ctx.strokeStyle = col;
      ctx.shadowColor = col; ctx.shadowBlur = 12;
      ctx.beginPath(); ctx.arc(cx, cy, rad, A0, A0 + (A1 - A0) * t); ctx.stroke();
      ctx.shadowBlur = 0;
    }

    // ticks
    ctx.strokeStyle = MUTED; ctx.lineWidth = 2;
    for (let i = 0; i <= 10; i++) {
      const a = A0 + (A1 - A0) * (i / 10);
      const r1 = rad - (big ? 22 : 16), r2 = rad - (big ? 30 : 22);
      ctx.beginPath();
      ctx.moveTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1);
      ctx.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2);
      ctx.stroke();
    }

    // label: along the top, where there is room for the full channel name
    ctx.textAlign = "center"; ctx.textBaseline = "top";
    ctx.fillStyle = MUTED;
    ctx.font = `${big ? 15 : 12}px Segoe UI, system-ui, sans-serif`;
    const lh = big ? 18 : 14;
    wrapLabel(ctx, c.label, this.w - 20, 2).forEach((ln, i) => ctx.fillText(ln, cx, 8 + i * lh));

    // value in the middle, unit below it
    ctx.textBaseline = "middle";
    ctx.fillStyle = has ? TEXT : MUTED;
    ctx.font = `700 ${big ? 54 : 30}px Segoe UI, system-ui, sans-serif`;
    ctx.fillText(has ? fmt(this.key, v) : "--", cx, cy - (big ? 4 : 2));
    if (c.unit) {
      ctx.fillStyle = MUTED;
      ctx.font = `${big ? 16 : 12}px Segoe UI, system-ui, sans-serif`;
      ctx.fillText(c.unit, cx, cy + (big ? 40 : 26));
    }
  }
}

// --- build UI ------------------------------------------------------------
// The dial grid is built from the gauge registry (/api/gauges) rather than
// hardcoded: whatever is ticked on the Gauges tab shows up here, optionally
// split into Basic / Ignition / Knock / Injection / Advanced sections.
let gauges = [];
let catalog = null;                       // last /api/gauges response
const heroGauge = new Gauge(document.getElementById("g_rpm"), "rpm");
const gaugeHost = document.getElementById("gaugeHost");

function heroKey() {
  const on = catalog ? catalog.gauges.filter((g) => g.on) : [];
  return (catalog && catalog.hero && on.some((g) => g.key === catalog.hero)) ? catalog.hero : "rpm";
}

function rebuildDashboard() {
  if (!catalog || !gaugeHost) return;
  const hero = heroKey();
  heroGauge.key = hero;
  heroGauge._drawn = null;
  gauges = [heroGauge];
  gaugeHost.innerHTML = "";

  const made = [];
  const addDial = (g, parent) => {
    const cv = document.createElement("canvas");
    cv.className = "gauge";
    cv.dataset.ch = g.key;
    parent.appendChild(cv);
    made.push([cv, g.key]);
  };
  const newGrid = (parent) => {
    const grid = document.createElement("div");
    grid.className = "grid";
    parent.appendChild(grid);
    return grid;
  };

  // The top dial's gauge never gets a small one too.
  const on = catalog.gauges.filter((g) => g.on && g.key !== hero);

  if (catalog.arrangeByType) {
    catalog.types.forEach((tname, ti) => {
      const items = on.filter((g) => g.type === ti);
      if (!items.length) return;
      const sec = document.createElement("section");
      sec.className = "gsection";
      const h = document.createElement("h3");
      h.textContent = tname;
      sec.appendChild(h);
      gaugeHost.appendChild(sec);
      const grid = newGrid(sec);
      items.forEach((g) => addDial(g, grid));
    });
  } else {
    // Flat grid: the user's drag order first, anything new in catalogue order.
    const order = catalog.order || [];
    const rank = (k) => { const i = order.indexOf(k); return i < 0 ? 1e9 : i; };
    on.sort((a, b) => rank(a.key) - rank(b.key));
    const grid = newGrid(gaugeHost);
    grid.classList.add("sortable");
    on.forEach((g) => addDial(g, grid));
    enableDragReorder(grid);
  }
  if (!on.length) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = "No gauges selected \u2014 pick some on the Gauges tab.";
    gaugeHost.appendChild(p);
  }
  // Only now, with everything attached to the document, create the Gauge
  // objects: their constructor measures the canvas, and a canvas that is not
  // in the page yet measures 0x0 and stays blank until the next resize.
  made.forEach(([cv, key]) => gauges.push(new Gauge(cv, key)));
  heroGauge.resize();
}

// Drag a dial to a new slot (flat layout only). The dial is not dragged
// visually; it simply swaps into whichever slot the pointer is over, and the
// resulting order is saved on the board. With a mouse the drag starts on the
// first real movement; with a finger it needs a short hold first, so the page
// can still be scrolled by swiping across the dials.
function enableDragReorder(grid) {
  let drag = null;
  const finish = async () => {
    if (!drag) return;
    const d = drag; drag = null;
    clearTimeout(d.timer);
    if (!d.active) return;
    d.cv.classList.remove("dragging");
    const order = [...grid.querySelectorAll(".gauge")].map((c) => c.dataset.ch);
    catalog.order = order;
    try {
      await fetch(`/api/gauges?order=${encodeURIComponent(order.join(","))}`, { method: "POST" });
    } catch (e) { /* best effort */ }
  };
  grid.addEventListener("pointerdown", (e) => {
    const cv = e.target.closest(".gauge");
    if (!cv || (e.pointerType === "mouse" && e.button !== 0)) return;
    drag = { cv, x: e.clientX, y: e.clientY, id: e.pointerId, active: false };
    const start = () => {
      if (!drag) return;
      drag.active = true;
      cv.classList.add("dragging");
      try { cv.setPointerCapture(drag.id); } catch (_) { /* ignore */ }
    };
    if (e.pointerType === "mouse") drag.start = start;
    else drag.timer = setTimeout(start, 350);
  });
  grid.addEventListener("pointermove", (e) => {
    if (!drag) return;
    if (!drag.active) {
      if (Math.hypot(e.clientX - drag.x, e.clientY - drag.y) < 8) return;
      if (drag.start) drag.start();
      else { clearTimeout(drag.timer); drag = null; return; }   // finger moved before the hold: it's a scroll
    }
    e.preventDefault();
    const hit = document.elementFromPoint(e.clientX, e.clientY);
    const over = hit && hit.closest ? hit.closest(".gauge") : null;
    if (!over || over === drag.cv || over.parentElement !== grid) return;
    const r = over.getBoundingClientRect();
    const before = (e.clientX - r.left) / r.width + (e.clientY - r.top) / r.height < 1;
    grid.insertBefore(drag.cv, before ? over : over.nextSibling);
  });
  grid.addEventListener("pointerup", finish);
  grid.addEventListener("pointercancel", finish);
  grid.addEventListener("touchmove", (e) => { if (drag && drag.active) e.preventDefault(); }, { passive: false });
}

// --- animation -----------------------------------------------------------
// Needles glide to their target, but we only repaint a gauge while it is
// actually moving — once it settles the canvas is left alone (no flicker).
let sweeping = false;
function animate() {
  if (sweeping) { requestAnimationFrame(animate); return; }
  gauges.forEach((g) => {
    const target = latest[g.key];
    if (target == null) return;
    const c = CH[g.key];
    const cur = shown[g.key] == null ? target : shown[g.key];
    let next = cur + (target - cur) * 0.2;
    if (Math.abs(target - next) < (c.max - c.min) * 0.0015) next = target; // snap when close
    shown[g.key] = next;
    const eps = (c.max - c.min) * 0.0015;
    if (g._drawn == null || Math.abs(next - g._drawn) >= eps) {
      g.draw(next);
      g._drawn = next;
    }
  });
  requestAnimationFrame(animate);
}

// Boot-up "sweep" — every needle runs min -> max -> min once, purely for show.
// Bypasses the normal target-seeking animate() loop (via the `sweeping` flag)
// so it isn't immediately overridden by real data on the next 100ms poll.
function sweepGauges() {
  sweeping = true;
  const t0 = performance.now();
  const dur = 900;
  function step(now) {
    const p = Math.min((now - t0) / dur, 1);
    const tri = p < 0.5 ? p * 2 : (1 - p) * 2;   // 0 -> 1 -> 0
    gauges.forEach((g) => {
      const c = CH[g.key];
      const v = c.min + (c.max - c.min) * tri;
      shown[g.key] = v;
      g.draw(v);
      g._drawn = v;
    });
    if (p < 1) { requestAnimationFrame(step); return; }
    sweeping = false;
    // Settle on the live value, or back to empty if nothing has arrived yet -
    // never leave a dial parked at its minimum looking like a reading.
    gauges.forEach((g) => {
      const v = latest[g.key] == null ? null : latest[g.key];
      shown[g.key] = v == null ? undefined : v;
      g.draw(v);
      g._drawn = v;
    });
  }
  requestAnimationFrame(step);
}

// --- Launch / Unlaunch Gauges ---------------------------------------------
// "Launch" replays the captured session-start handshake, then starts a
// continuous bulk-IN poll (/api/launch) so the ECU streams telemetry without
// a real USB/IP client attached. "Unlaunch" (/api/unlaunch) stops that poll
// — it deliberately does NOT cut ECU power or the WiFi bridge, since that
// would drop the very connection serving this page.
const launchBtn = document.getElementById("launchGauges");
const launchMsg = document.getElementById("launchMsg");
let launched = false;
let launchBusy = false;
let launchMsgTimer = null;
function setLaunchedUI(on) {
  launched = on;
  launchBtn.textContent = on ? "Unlaunch Gauges" : "Launch Gauges";
  launchBtn.classList.toggle("active", on);
}
// Feedback beside the button, so it is obvious the board is talking to the
// ECU (or failing to) rather than the click having done nothing.
function setLaunchMsg(text, cls, fadeMs) {
  if (!launchMsg) return;
  clearTimeout(launchMsgTimer);
  launchMsg.textContent = text || "";
  launchMsg.className = "msg " + (cls || "");
  if (fadeMs) launchMsgTimer = setTimeout(() => { launchMsg.textContent = ""; }, fadeMs);
}
async function launchGauges(auto) {
  if (launchBusy || launched) return;
  launchBusy = true;
  launchBtn.disabled = true;
  launchBtn.textContent = "Launching\u2026";
  setLaunchMsg((auto ? "Auto-connect: " : "") + "contacting the ECU\u2026", "");
  try {
    const r = await fetch("/api/launch", { method: "POST" });
    const d = await r.json();
    if (d.ok) {
      setLaunchedUI(true);
      if (!catalog || catalog.sweep !== false) sweepGauges();
      setLaunchMsg("ECU responded \u2014 handshake OK, streaming", "ok", 5000);
      logUsb("Gauges launched (handshake replay ok)", "ok");
    } else {
      setLaunchedUI(false);
      const why = d.err || "no ECU / handshake step rejected";
      setLaunchMsg("Launch failed: " + why, "err", 12000);
      logUsb("Launch failed \u2014 " + why, "warn");
    }
  } catch (e) {
    setLaunchedUI(false);
    setLaunchMsg("The board didn't answer", "err", 8000);
    logUsb("Launch failed \u2014 request error", "warn");
  } finally {
    launchBtn.disabled = false;
    launchBusy = false;
  }
}
if (launchBtn) launchBtn.addEventListener("click", async () => {
  if (!launched) { launchGauges(false); return; }
  launchBusy = true;
  launchBtn.disabled = true;
  launchBtn.textContent = "Stopping\u2026";
  try { await fetch("/api/unlaunch", { method: "POST" }); } catch (e) { /* best effort */ }
  setLaunchedUI(false);
  setLaunchMsg("Gauge poll stopped", "", 4000);
  logUsb("Gauges unlaunched", "warn");
  launchBtn.disabled = false;
  launchBusy = false;
});

// --- Gauges tab ------------------------------------------------------------
// Six dropdowns (2x3), one per registry Group, each an alphabetical checkbox
// list. Four rows are listed but disabled: Ignitron's PC software computes
// them into its logs (AFR, USB timing, timestamp) and the ECU never sends
// them, so ticking one would do nothing.
const gaugeGroupsRoot = document.getElementById("gaugeGroups");
const arrangeToggle = document.getElementById("arrangeByType");
const sweepToggle = document.getElementById("sweepOnLaunch");
const autoLaunchToggle = document.getElementById("autoLaunch");
const heroSelect = document.getElementById("heroSelect");
const hideUnmapped = document.getElementById("hideUnmapped");
const gaugeMsg = document.getElementById("gaugeMsg");
const gaugeCount = document.getElementById("gaugeCount");

function setGaugeMsg(text) {
  if (!gaugeMsg) return;
  gaugeMsg.textContent = text || "";
  gaugeMsg.className = text ? "msg err" : "msg";
}

function renderGaugesTab() {
  if (!catalog || !gaugeGroupsRoot) return;
  const hide = hideUnmapped ? hideUnmapped.checked : true;
  const nOnTotal = catalog.gauges.filter((g) => g.on).length;
  const max = catalog.maxEnabled || 16;
  if (gaugeCount)
    gaugeCount.textContent = `${nOnTotal} of ${max} shown · ${catalog.mapped || 0} of ${catalog.total || catalog.gauges.length} channels mapped`;
  // Re-rendering rebuilds the <details>, which would snap every dropdown shut
  // on each tick; remember which ones were open and reopen them.
  const openGroups = new Set([...gaugeGroupsRoot.querySelectorAll("details[open]")].map((d) => +d.dataset.gi));
  gaugeGroupsRoot.innerHTML = "";
  catalog.groups.forEach((gname, gi) => {
    const items = catalog.gauges
      .filter((g) => g.group === gi && (!hide || g.mapped))
      .sort((a, b) => a.label.localeCompare(b.label));
    const det = document.createElement("details");
    det.className = "gpicker";
    det.dataset.gi = gi;
    det.open = openGroups.has(gi);
    const nOn = items.filter((g) => g.on).length;
    const sum = document.createElement("summary");
    sum.innerHTML = `${gname} <span class="count">${nOn}/${items.length}</span>`;
    det.appendChild(sum);
    items.forEach((g) => {
      const row = document.createElement("label");
      row.className = "gitem" + (g.mapped ? "" : " unmapped");
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.checked = !!g.on;
      cb.disabled = !g.mapped;
      cb.addEventListener("change", async () => {
        const want = cb.checked;
        try {
          const r = await fetch(
            `/api/gauges?key=${encodeURIComponent(g.key)}&on=${want ? 1 : 0}`,
            { method: "POST" });
          const d = await r.json();
          if (!d.ok) {
            // Refused (dashboard full, or unmapped) — put the tick back.
            cb.checked = !want;
            setGaugeMsg(d.err === "limit reached"
              ? `Limit reached — ${d.maxEnabled} gauges max. Turn one off first.`
              : `Can't enable ${g.label}: ${d.err}.`);
            return;
          }
          g.on = want;
          setGaugeMsg("");
          if (typeof d.enabled === "number") catalog.enabled = d.enabled;
        } catch (e) {
          cb.checked = !want;
          setGaugeMsg("Couldn't reach the board.");
          return;
        }
        renderGaugesTab();
        rebuildDashboard();
      });
      row.appendChild(cb);
      const txt = document.createElement("span");
      txt.textContent = g.label + (g.unit ? ` (${g.unit})` : "");
      row.appendChild(txt);
      if (!g.mapped) {
        const tag = document.createElement("em");
        tag.textContent = "not sent by the ECU";
        row.appendChild(tag);
      }
      det.appendChild(row);
    });
    gaugeGroupsRoot.appendChild(det);
  });
  if (arrangeToggle) arrangeToggle.checked = !!catalog.arrangeByType;
  if (sweepToggle) sweepToggle.checked = catalog.sweep !== false;
  if (autoLaunchToggle) autoLaunchToggle.checked = !!catalog.autoLaunch;
  if (heroSelect) {
    const hero = heroKey();
    heroSelect.innerHTML = "";
    // RPM is always offered: it is the fallback when the chosen gauge is off.
    catalog.gauges.filter((g) => g.on || g.key === "rpm").sort((a, b) => a.label.localeCompare(b.label)).forEach((g) => {
      const o = document.createElement("option");
      o.value = g.key;
      o.textContent = g.label + (g.unit ? ` (${g.unit})` : "");
      o.selected = g.key === hero;
      heroSelect.appendChild(o);
    });
  }
}

async function postGaugePref(query) {
  try { await fetch(`/api/gauges?${query}`, { method: "POST" }); } catch (e) { /* best effort */ }
}
if (sweepToggle) sweepToggle.addEventListener("change", () => {
  if (catalog) catalog.sweep = sweepToggle.checked;
  postGaugePref(`sweep=${sweepToggle.checked ? 1 : 0}`);
});
if (autoLaunchToggle) autoLaunchToggle.addEventListener("change", () => {
  if (catalog) catalog.autoLaunch = autoLaunchToggle.checked;
  postGaugePref(`autoLaunch=${autoLaunchToggle.checked ? 1 : 0}`);
});
if (heroSelect) heroSelect.addEventListener("change", () => {
  if (catalog) catalog.hero = heroSelect.value;
  postGaugePref(`hero=${encodeURIComponent(heroSelect.value)}`);
  rebuildDashboard();
});

if (hideUnmapped) hideUnmapped.addEventListener("change", renderGaugesTab);

if (arrangeToggle) arrangeToggle.addEventListener("change", async () => {
  if (catalog) catalog.arrangeByType = arrangeToggle.checked;
  try {
    await fetch(`/api/gauges?arrangeByType=${arrangeToggle.checked ? 1 : 0}`, { method: "POST" });
  } catch (e) { /* best effort */ }
  rebuildDashboard();
});

// The catalogue is ~384 rows, so it arrives a group at a time (see the
// /api/gauges comment in ignitron_wifi.cpp) and is stitched together here.
async function loadGauges() {
  try {
    const meta = await (await fetch("/api/gauges")).json();
    const all = [];
    for (let gi = 0; gi < meta.groups.length; gi++) {
      const part = await (await fetch(`/api/gauges?group=${gi}`)).json();
      if (part.gauges) all.push(...part.gauges);
    }
    catalog = Object.assign({}, meta, { gauges: all });
    // The dial renderer reads label/unit/min/max/dp from CH, so fold the
    // registry in alongside the legacy entries. dp is the decimals Ignitron's
    // own viewer prints for that channel.
    all.forEach((g) => {
      CH[g.key] = Object.assign({}, CH[g.key], {
        label: g.label, unit: g.unit, min: g.min, max: g.max, dp: g.dp,
      });
    });
    renderGaugesTab();
    rebuildDashboard();
  } catch (e) {
    console.warn("gauge catalogue unavailable", e);
  }
}

const identRoot = document.getElementById("identgrid");
function renderIdent(ident) {
  if (!identRoot) return;
  const v = (u, k) => (u && u[k]) ? "V" + u[k] : "--";
  const com = ident && ident.com, dsp = ident && ident.dsp, cpu = ident && ident.cpu;
  setHTML(identRoot,
    chip("CPU firmware",   v(cpu, "fw")) +
    chip("DSP firmware",   v(dsp, "fw")) +
    chip("COM firmware",   v(com, "fw")) +
    chip("CPU bootloader", v(cpu, "bl")) +
    chip("DSP bootloader", v(dsp, "bl")) +
    chip("COM bootloader", v(com, "bl")));
}

// --- data (poll /api/status, Can2Cluster style) --------------------------
let latest = {};
const dot = document.getElementById("link");
const linkText = document.getElementById("linkText");
const metaEl = document.getElementById("meta");

// --- system panel --------------------------------------------------------
const sysRoot = document.getElementById("sysgrid");
const diagRoot = document.getElementById("diaggrid");
const usbRoot = document.getElementById("usbgrid");
const setDisWifi = document.getElementById("setDisWifi");
const setResWifi = document.getElementById("setResWifi");
const setIdleOff = document.getElementById("setIdleOff");
let settingsLoaded = false;

// Only touch the DOM when the rendered markup actually changes — stops the
// diagnostics chips from repainting (and flickering) on every 10 Hz poll.
function setHTML(el, html) {
  if (el && el._html !== html) { el.innerHTML = html; el._html = html; }
}

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text;
}

function chip(label, value, cls) {
  return `<div class="chip ${cls || ""}"><div class="k">${label}</div><div class="v">${value}</div></div>`;
}

function fmtCountdown(ms) {
  if (!ms) return "—";
  const s = Math.round(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

const MUX_LABEL = { esp: "ESP (WiFi)", laptop: "Laptop (USB-C)", off: "Disconnected" };
const ROUTE_LABEL = { auto: "Auto (VBUS)", usbc: "Forced USB-C", wifi: "Forced WiFi" };

function renderSys(sys) {
  if (!sys) return;
  const powerState = sys.asleep ? "Asleep" : sys.ecuPwr ? "On" : "Off";
  setHTML(sysRoot,
    chip("USB route", MUX_LABEL[sys.mux] || sys.mux, sys.mux === "off" ? "warn" : "ok") +
    chip("Override", ROUTE_LABEL[sys.route] || sys.route, sys.route && sys.route !== "auto" ? "ok" : "") +
    chip("USB-C", sys.usbc ? "Connected" : "—", sys.usbc ? "ok" : "") +
    chip("ECU 5V", powerState, sys.asleep ? "warn" : sys.ecuPwr ? "ok" : "") +
    chip("WiFi bridge", sys.wifiOn ? (sys.wifiUse ? "In use" : "Idle") : "Off",
         sys.wifiOn ? "ok" : "") +
    chip("Auto-off in",
         (sys.idleOff === false || sys.usbc || (sys.route && sys.route !== "auto"))
           ? "held" : fmtCountdown(sys.idleMs),
         (sys.idleOff !== false && !sys.usbc && sys.idleMs && sys.idleMs < 60000 && sys.route === "auto") ? "warn" : ""));

  if (sys.route) setRouteActive(sys.route);

  if (!settingsLoaded) {
    setDisWifi.checked = !!sys.cDisWifi;
    setResWifi.checked = !!sys.cResWifi;
    setIdleOff.checked = sys.idleOff !== false;
    settingsLoaded = true;
  }
}

function renderDiag(data) {
  if (!diagRoot) return;
  const sys = data.sys || {};
  setHTML(diagRoot,
    chip("Firmware", data.fw || "—") +
    chip("Free heap", data.heap != null ? (data.heap / 1024).toFixed(0) + " KB" : "—") +
    chip("ECU link", data.link ? "Active" : "Inactive", data.link ? "ok" : "warn") +
    chip("Frames", data.frames != null ? data.frames : 0, data.link ? "ok" : "") +
    chip("AP SSID", sys.ssid || "—") +
    chip("AP IP", sys.ip || "—"));
  renderUsb(data.usb, sys.route);
}

// --- USB connectivity trace (Diag) ---------------------------------------
const usbTrace = document.getElementById("usbTrace");
const usbRateEl = document.getElementById("usbRate");
const usbLog = document.getElementById("usbLog");
const usbCtx = usbTrace ? usbTrace.getContext("2d") : null;
const HIST = 150;
const inHist = [], outHist = [];
let usbPrev = null;      // { inBytes, outBytes, t }
const usbState = {};     // last-seen connectivity flags, for the event log

function logUsb(msg, cls) {
  if (!usbLog) return;
  const t = new Date().toLocaleTimeString();
  const row = document.createElement("div");
  row.className = "row";
  row.innerHTML = `<span class="t">${t}</span><span class="m ${cls || ""}">${msg}</span>`;
  usbLog.insertBefore(row, usbLog.firstChild);
  while (usbLog.childElementCount > 60) usbLog.removeChild(usbLog.lastChild);
}

function noteChange(key, val, onMsg, offMsg) {
  if (usbState[key] === val) return;
  if (usbState[key] !== undefined) logUsb(val ? onMsg : offMsg, val ? "ok" : "warn");
  usbState[key] = val;
}

function renderUsb(u, route) {
  if (!usbRoot) return;
  u = u || {};
  const now = performance.now();
  let inRate = 0, outRate = 0;
  if (usbPrev) {
    const dt = (now - usbPrev.t) / 1000;
    if (dt > 0) {
      inRate = Math.max(0, ((u.inBytes || 0) - usbPrev.inBytes) / dt);
      outRate = Math.max(0, ((u.outBytes || 0) - usbPrev.outBytes) / dt);
    }
  }
  usbPrev = { inBytes: u.inBytes || 0, outBytes: u.outBytes || 0, t: now };
  inHist.push(inRate); outHist.push(outRate);
  if (inHist.length > HIST) { inHist.shift(); outHist.shift(); }

  const dev = u.dev || {};
  setHTML(usbRoot,
    chip("USB device", usbDevText(dev), dev.present ? (dev.ready ? "ok" : "warn") : "") +
    chip("USB/IP client", u.ipClient ? "Attached" : "—", u.ipClient ? "ok" : "") +
    chip("Capture client", u.capClient ? (u.capBrowser ? "Browser" : "tapcap (TCP)") : "—", u.capClient ? "ok" : "") +
    chip("IN frames", u.inFrames != null ? u.inFrames : 0) +
    chip("OUT frames", u.outFrames != null ? u.outFrames : 0) +
    chip("Dropped", u.dropped != null ? u.dropped : 0, u.dropped ? "warn" : "") +
    chip("Route", ROUTE_LABEL[route] || route || "—", route && route !== "auto" ? "ok" : ""));

  if (usbRateEl)
    usbRateEl.textContent = `▲ ${fmtRate(inRate)}  ▼ ${fmtRate(outRate)}`;

  noteChange("dev", dev.present ? usbDevKey(dev) : "",
             `USB device connected — ${usbDevText(dev)}`, "USB device disconnected");
  noteChange("ip", !!u.ipClient, "USB/IP client attached", "USB/IP client detached");
  noteChange("cap", !!u.capClient, "Capture client attached", "Capture client detached");
  noteChange("route", route || "auto",
             `Route → ${ROUTE_LABEL[route] || route}`, `Route → ${ROUTE_LABEL[route] || route}`);
  drawTrace();
}

// ---------------------------------------------------------------------------
// USB capture (Diagnostics). The board streams tap records over
// ws://<board>/ws/capture as binary frames; we keep every chunk in memory and
// hand back one Blob on Download. The file is byte-identical to what
// tools/tapcap.py writes, so ilf_match / gauge_lab / label_by_notes read it
// unchanged. Text frames sent up are notes: the board stamps them into the
// stream as 0xFF marker records with the board's own timestamp, so they line
// up with the frames around them regardless of WiFi latency.
const capGrid = document.getElementById("capgrid");
const capStart = document.getElementById("capStart");
const capStop = document.getElementById("capStop");
const capDownload = document.getElementById("capDownload");
const capMsg = document.getElementById("capMsg");
const capNote = document.getElementById("capNote");
const capNoteAdd = document.getElementById("capNoteAdd");
const capLog = document.getElementById("capLog");
const cap = { ws: null, chunks: [], bytes: 0, records: 0, notes: 0, t0: 0, tEnd: 0, timer: null, blob: null };

function logCap(msg, cls) {
  if (!capLog) return;
  const t = new Date().toLocaleTimeString();
  const row = document.createElement("div");
  row.className = "row";
  row.innerHTML = `<span class="t">${t}</span><span class="m ${cls || ""}">${msg}</span>`;
  capLog.insertBefore(row, capLog.firstChild);
  while (capLog.childElementCount > 40) capLog.removeChild(capLog.lastChild);
}

function setCapMsg(text, cls) {
  if (!capMsg) return;
  capMsg.textContent = text || "";
  capMsg.className = "msg " + (cls || "");
}

function fmtBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1048576) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1048576).toFixed(2)} MB`;
}

function renderCap() {
  if (!capGrid) return;
  const running = !!cap.ws && cap.ws.readyState === WebSocket.OPEN;
  const secs = cap.t0 ? Math.floor(((running ? performance.now() : cap.tEnd || performance.now()) - cap.t0) / 1000) : 0;
  const mm = String(Math.floor(secs / 60)).padStart(2, "0"), ss = String(secs % 60).padStart(2, "0");
  setHTML(capGrid,
    chip("State", running ? "Recording" : (cap.bytes ? "Stopped" : "Idle"), running ? "ok" : "") +
    chip("Duration", `${mm}:${ss}`) +
    chip("Size", fmtBytes(cap.bytes)) +
    chip("Records", cap.records) +
    chip("Notes", cap.notes));
}

// Count records in a chunk stream without copying: records are
// [0xEC dir ep flags t_ms(4) len(2) payload]. Frames from the board can split
// a record, so carry the leftover across chunks.
let capCarry = new Uint8Array(0);
function countRecords(buf) {
  let b = new Uint8Array(buf);
  if (capCarry.length) {
    const j = new Uint8Array(capCarry.length + b.length);
    j.set(capCarry); j.set(b, capCarry.length);
    b = j;
  }
  let i = 0;
  while (i + 10 <= b.length) {
    if (b[i] !== 0xEC) { i++; continue; }   // resync (shouldn't happen)
    const len = b[i + 8] | (b[i + 9] << 8);
    if (i + 10 + len > b.length) break;
    cap.records++;
    if (b[i + 1] === 0xFF) cap.notes++;
    i += 10 + len;
  }
  capCarry = b.slice(i);
}

function capSetButtons(running) {
  if (capStart) capStart.disabled = running;
  if (capStop) capStop.disabled = !running;
  if (capNote) capNote.disabled = !running;
  if (capNoteAdd) capNoteAdd.disabled = !running;
  if (capDownload) capDownload.disabled = running || cap.bytes === 0;
}

function capStartFn() {
  if (cap.ws) return;
  if (!("WebSocket" in window)) { setCapMsg("This browser has no WebSocket support", "err"); return; }
  cap.chunks = []; cap.bytes = 0; cap.records = 0; cap.notes = 0; cap.blob = null;
  cap.t0 = performance.now(); cap.tEnd = 0; capCarry = new Uint8Array(0);
  const url = `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws/capture`;
  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  cap.ws = ws;
  setCapMsg("Connecting…");
  ws.onopen = () => {
    capSetButtons(true);
    setCapMsg("Recording — keep this page open", "ok");
    logCap("Capture started", "ok");
    cap.timer = setInterval(renderCap, 1000);
    renderCap();
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data === "string") {
      // JSON status from the board (ok / refused)
      try {
        const j = JSON.parse(ev.data);
        if (j.err) { setCapMsg(j.err, "err"); logCap(j.err, "warn"); }
      } catch (_) { /* ignore */ }
      return;
    }
    cap.chunks.push(ev.data);
    cap.bytes += ev.data.byteLength;
    countRecords(ev.data);
  };
  ws.onerror = () => { setCapMsg("Connection error", "err"); };
  ws.onclose = (ev) => {
    const wasRunning = cap.ws === ws;
    cap.ws = null;
    cap.tEnd = performance.now();
    if (cap.timer) { clearInterval(cap.timer); cap.timer = null; }
    capSetButtons(false);
    if (wasRunning) {
      if (cap.bytes) {
        logCap(`Capture stopped — ${fmtBytes(cap.bytes)}, ${cap.records} records, ${cap.notes} notes`, "ok");
        setCapMsg(ev.wasClean ? "Stopped — download below" : "Connection lost — partial capture kept", ev.wasClean ? "ok" : "err");
      } else if (!capMsg.classList.contains("err")) {
        setCapMsg("Capture ended with no data", "err");
      }
    }
    renderCap();
  };
}

function capStopFn() {
  if (!cap.ws) return;
  setCapMsg("Stopping…");
  cap.ws.close(1000, "user");
}

function capAddNote() {
  if (!cap.ws || cap.ws.readyState !== WebSocket.OPEN) return;
  const text = (capNote.value || "").trim();
  if (!text) return;
  cap.ws.send(text);            // board stamps it as a 0xFF record
  logCap(`Note: ${text}`);
  capNote.value = "";
}

function capDownloadFn() {
  if (!cap.bytes || cap.ws) return;
  if (!cap.blob) cap.blob = new Blob(cap.chunks, { type: "application/octet-stream" });
  const d = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  const name = `ignitron_capture_${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.bin`;
  const a = document.createElement("a");
  a.href = URL.createObjectURL(cap.blob);
  a.download = name;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 2000);
  logCap(`Saved ${name}`, "ok");
}

if (capStart) {
  capStart.addEventListener("click", capStartFn);
  capStop.addEventListener("click", capStopFn);
  capDownload.addEventListener("click", capDownloadFn);
  capNoteAdd.addEventListener("click", capAddNote);
  capNote.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); capAddNote(); } });
  // Losing the page loses the recording — warn before navigating away mid-capture.
  window.addEventListener("beforeunload", (e) => {
    if (cap.ws) { e.preventDefault(); e.returnValue = ""; }
  });
  renderCap();
}

// USB base-class codes → friendly names (bDeviceClass / bInterfaceClass).
const USB_CLASS = {
  1: "Audio", 2: "Comms", 3: "HID", 7: "Printer", 8: "Mass storage",
  9: "Hub", 10: "CDC data", 11: "Smart card", 224: "Wireless", 255: "Vendor",
};

function hex4(n) { return (n >>> 0).toString(16).toUpperCase().padStart(4, "0"); }

function usbDevKey(dev) {
  return `${dev.vid || 0}:${dev.pid || 0}:${dev.class || 0}`;
}

function usbDevText(dev) {
  if (!dev || !dev.present) return "—";
  const id = `${hex4(dev.vid || 0)}:${hex4(dev.pid || 0)}`;
  if (!dev.ready) return `Detecting… (${id})`;
  const kind = USB_CLASS[dev.class] || `Class ${dev.class || 0}`;
  return `${kind} (${id})`;
}

function fmtRate(bps) {
  if (bps >= 1024 * 1024) return (bps / 1048576).toFixed(1) + " MB/s";
  if (bps >= 1024) return (bps / 1024).toFixed(1) + " KB/s";
  return Math.round(bps) + " B/s";
}

function drawTrace() {
  if (!usbCtx) return;
  const dpr = window.devicePixelRatio || 1;
  const r = usbTrace.getBoundingClientRect();
  if (usbTrace.width !== r.width * dpr || usbTrace.height !== r.height * dpr) {
    usbTrace.width = r.width * dpr;
    usbTrace.height = r.height * dpr;
    usbCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  const w = r.width, h = r.height, ctx = usbCtx;
  ctx.clearRect(0, 0, w, h);
  const peak = Math.max(64, ...inHist, ...outHist);
  const plot = (hist, color, fill) => {
    ctx.beginPath();
    for (let i = 0; i < hist.length; i++) {
      const x = (i / (HIST - 1)) * w;
      const y = h - (hist[i] / peak) * (h - 6) - 3;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.stroke();
    if (fill) {
      ctx.lineTo((hist.length - 1) / (HIST - 1) * w, h);
      ctx.lineTo(0, h); ctx.closePath();
      ctx.fillStyle = fill; ctx.fill();
    }
  };
  plot(outHist, ACCENT2, "rgba(255,176,46,.10)");
  plot(inHist, ACCENT, "rgba(36,209,196,.14)");
}

function postSettings() {
  const q = `usbcDisablesWifi=${setDisWifi.checked ? 1 : 0}` +
            `&restoreWifiOnUnplug=${setResWifi.checked ? 1 : 0}` +
            `&idleAutoOff=${setIdleOff.checked ? 1 : 0}`;
  fetch(`/api/settings?${q}`, { method: "POST" }).catch(() => {});
}
setDisWifi.addEventListener("change", postSettings);
setResWifi.addEventListener("change", postSettings);
setIdleOff.addEventListener("change", postSettings);

// --- USB route override (segmented control) ------------------------------
const routeSeg = document.getElementById("routeSeg");
const routeMsg = document.getElementById("routeMsg");
let routeCurrent = null;

function setRouteActive(route) {
  if (!routeSeg) return;
  routeCurrent = route;
  routeSeg.querySelectorAll(".seg-btn").forEach((b) =>
    b.classList.toggle("active", b.dataset.route === route));
}

if (routeSeg) routeSeg.addEventListener("click", async (e) => {
  const btn = e.target.closest(".seg-btn");
  if (!btn) return;
  const mode = btn.dataset.route;
  setRouteActive(mode);
  routeMsg.textContent = "Switching…"; routeMsg.className = "msg";
  try {
    const r = await (await fetch(`/api/route?mode=${mode}`, { method: "POST" })).json();
    routeMsg.textContent = r.ok ? "" : (r.err || "Error");
    routeMsg.className = r.ok ? "msg" : "msg err";
  } catch (err) {
    routeMsg.textContent = "Failed"; routeMsg.className = "msg err";
  }
});

// --- fault codes / limp mode ---------------------------------------------
// Fields come straight out of the live block (see /api/status "fault"); the
// names come from data/faults.js (Ignitron.exe's own table, P-code keyed).
// Shown only on the Logging tab's "Fault codes & limp mode" card - the
// dashboard stays gauges-only.
const faultRoot = document.getElementById("faultgrid");
let faultPrev = null;

function renderFault(f, link) {
  const have = f && f.valid && link;
  const total = have ? (f.cpu | 0) + (f.dsp | 0) : 0;
  const last = have && f.last ? faultLabel(f.last) : null;

  if (faultRoot) {
    const html = have
      ? chip("Limp mode", f.limp ? "ACTIVE" : "no", f.limp ? "err" : "ok") +
        chip("Faults (CPU)", f.cpu, f.cpu ? "warn" : "ok") +
        chip("Faults (DSP)", f.dsp, f.dsp ? "warn" : "ok") +
        chip("Last fault", last || "none", last ? "warn" : "ok") +
        chip("Clear-limp switch", f.clearSw ? "on" : "off", "")
      : chip("Fault data", "no ECU session", "");
    if (html !== faultPrev) { setHTML(faultRoot, html); faultPrev = html; }
  }
  // Log transitions so they show up in the USB event log with a timestamp.
  if (have && typeof logUsb === "function") {
    if (renderFault.limp !== !!f.limp) {
      renderFault.limp = !!f.limp;
      if (f.limp) logUsb("ECU entered LIMP MODE" + (last ? " — " + last : ""), "warn");
    }
    if (renderFault.last !== f.last && f.last) {
      renderFault.last = f.last;
      logUsb("Last fault: " + last, "warn");
    }
  }
}

// --- fault memory: read / clear (Logging tab) ------------------------------
const faultReadBtn = document.getElementById("faultRead");
const faultClearBtn = document.getElementById("faultClear");
const faultMsgEl = document.getElementById("faultMsg");
const faultListEl = document.getElementById("faultList");

function setFaultMsg(text, cls) {
  if (!faultMsgEl) return;
  faultMsgEl.textContent = text || "";
  faultMsgEl.className = "msg " + (cls || "");
}

// Ignitron's own wording for the occurrence count (strings 176..179).
function faultFrequency(n) {
  if (n >= 16) return "Constant";
  if (n >= 8) return "Frequent";
  if (n >= 3) return "Sporadic";
  return "Intermittent";
}

function renderFaultList(d) {
  if (!faultListEl) return;
  if (!d || !d.faults) { faultListEl.innerHTML = ""; return; }
  if (!d.faults.length) {
    faultListEl.innerHTML = `<div class="empty">No faults stored (DSP counter ${d.dsp ? d.dsp.counter : "?"}, CPU counter ${d.cpu ? d.cpu.counter : "?"}).</div>`;
    return;
  }
  const rows = d.faults.map((f) => {
    const na = (v) => (v === 9999 || v === 32767 || v === 65535) ? "\u2014" : String(v);
    return `<tr class="${f.pageOk ? "" : "bad"}">
      <td class="code">P${String(f.code).padStart(4, "0")}</td>
      <td class="desc">${faultLabel(f.code)}${f.pageOk ? "" : " (page checksum error)"}</td>
      <td>${f.src}</td>
      <td class="num">${na(f.rpm)}</td>
      <td class="num">${f.load === 65535 ? "\u2014" : (f.load / 10).toFixed(1) + " %"}</td>
      <td class="num">${na(f.value)}</td>
      <td class="num">${f.count}\u00d7 ${faultFrequency(f.count)}</td>
      <td class="num">${f.duration}</td>
    </tr>`;
  }).join("");
  faultListEl.innerHTML = `<table><thead><tr>
      <th>Code</th><th>Description</th><th>Unit</th><th>RPM</th><th>Load</th><th>Value</th><th>Frequency</th><th>Duration</th>
    </tr></thead><tbody>${rows}</tbody></table>`;
}

async function readFaultMemory() {
  if (!faultReadBtn) return;
  faultReadBtn.disabled = true; faultClearBtn.disabled = true;
  setFaultMsg("Reading fault memory\u2026", "");
  try {
    const r = await fetch("/api/faults");
    const d = await r.json();
    if (d.ok) {
      renderFaultList(d);
      const n = d.faults.length;
      setFaultMsg(`${n} fault${n === 1 ? "" : "s"} found.` +
                  ((d.dsp && !d.dsp.csum) || (d.cpu && !d.cpu.csum) ? " EEPROM checksum error in a page." : ""),
                  n ? "err" : "ok");
    } else {
      setFaultMsg("Failed to retrieve fault codes: " + (d.err || "?"), "err");
    }
  } catch (e) {
    setFaultMsg("The board didn't answer", "err");
  } finally {
    faultReadBtn.disabled = false; faultClearBtn.disabled = false;
  }
}

if (faultReadBtn) faultReadBtn.addEventListener("click", readFaultMemory);
if (faultClearBtn) faultClearBtn.addEventListener("click", async () => {
  if (!confirm("Clear the ECU's fault memory? This is what Ignitron's Clear button does; the list cannot be recovered afterwards.")) return;
  faultReadBtn.disabled = true; faultClearBtn.disabled = true;
  setFaultMsg("Clearing fault memory\u2026", "");
  try {
    const r = await fetch("/api/faults/clear", { method: "POST" });
    const d = await r.json();
    if (d.ok) {
      setFaultMsg("Fault codes have been successfully cleared.", "ok");
      logUsb("ECU fault memory cleared", "ok");
      await readFaultMemory();
    } else {
      setFaultMsg("Failed to clear fault codes: " + (d.err || "?") + " \u2014 please try again.", "err");
    }
  } catch (e) {
    setFaultMsg("The board didn't answer", "err");
  } finally {
    faultReadBtn.disabled = false; faultClearBtn.disabled = false;
  }
});

async function fetchStatus() {
  try {
    const res = await fetch("/api/status");
    const data = await res.json();
    if (data.ch) latest = Object.assign(latest, data.ch);
    if (data.g) latest = Object.assign(latest, data.g);   // registry gauges
    dot.className = "dot " + (data.link ? "live" : "idle");
    linkText.textContent = data.link ? "ECU Active" : "ECU Inactive";
    linkText.className = data.link ? "ok" : "";
    metaEl.textContent = `${data.frames || 0} frames`;
    metaEl.className = "meta " + (data.link ? "ok" : "");
    renderSys(data.sys);
    renderDiag(data);
    renderIdent(data.ident);
    renderFault(data.fault, data.link);
    if (launchBtn && !launchBusy && data.sys && !!data.sys.gaugePoll !== launched)
      setLaunchedUI(!!data.sys.gaugePoll);
  } catch (e) {
    // The poll itself is the ping: no answer means the board, not the ECU.
    dot.className = "dot";
    linkText.textContent = "Board offline";
    linkText.className = "err";
    metaEl.className = "meta";
  }
}

// --- WiFi settings -------------------------------------------------------
const wifiSsid = document.getElementById("wifiSsid");
const wifiPass = document.getElementById("wifiPass");
const wifiSave = document.getElementById("wifiSave");
const wifiMsg = document.getElementById("wifiMsg");
const secBadge = document.getElementById("secBadge");
const showPass = document.getElementById("showPass");
const factoryBtn = document.getElementById("factoryBtn");

async function loadWifi() {
  try {
    const d = await (await fetch("/api/wifi")).json();
    wifiSsid.value = d.ssid || "";
    secBadge.textContent = d.secured ? "(currently secured)" : "(currently open)";
    if (d.maxChannel) setChanMax(d.maxChannel);
    if (d.channel) setChanCurrent(d.channel);
  } catch (e) { /* ignore */ }
}

showPass.addEventListener("change", () => {
  wifiPass.type = showPass.checked ? "text" : "password";
});

wifiSave.addEventListener("click", async () => {
  const ssid = wifiSsid.value.trim();
  const pass = wifiPass.value;
  if (!ssid) {
    wifiMsg.textContent = "SSID is required"; wifiMsg.className = "msg err"; return;
  }
  if (pass && pass.length < 8) {
    wifiMsg.textContent = "Password must be blank or \u2265 8 characters";
    wifiMsg.className = "msg err"; return;
  }
  wifiMsg.textContent = "Saving\u2026"; wifiMsg.className = "msg";
  const q = `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(pass)}`;
  try {
    const r = await (await fetch(`/api/wifi?${q}`, { method: "POST" })).json();
    if (r.ok) {
      wifiMsg.textContent = `Rebooting \u2014 reconnect to "${ssid}"${pass ? "" : " (open)"}\u2026`;
      wifiMsg.className = "msg ok";
    } else {
      wifiMsg.textContent = r.err || "Error"; wifiMsg.className = "msg err";
    }
  } catch (e) {
    // Connection drops as the device reboots — treat as success.
    wifiMsg.textContent = "Rebooting\u2026"; wifiMsg.className = "msg ok";
  }
});

factoryBtn.addEventListener("click", async () => {
  if (!confirm("Restore factory defaults? The device will reboot as an open AP " +
               "\"IgnitronUSB\" with no password.")) return;
  wifiMsg.textContent = "Factory reset \u2014 rebooting\u2026"; wifiMsg.className = "msg ok";
  try { await fetch("/api/factory", { method: "POST" }); } catch (e) { /* reboot drops it */ }
});

// --- WiFi channel picker + band scan --------------------------------------
// Congestion per channel = sum over scanned APs of overlap(Δch) × strength(RSSI).
// 2.4 GHz channels are 5 MHz apart but 20 MHz wide, so an AP on channel N
// loads N±1..±4 too (triangular weight, zero at |Δ| ≥ 5).
const chanChart = document.getElementById("chanChart");
const chanLegend = document.getElementById("chanLegend");
const chanSeg = document.getElementById("chanSeg");
const chanScan = document.getElementById("chanScan");
const chanApply = document.getElementById("chanApply");
const chanMsg = document.getElementById("chanMsg");
const CHANNELS = 13;            // chart always shows the full 2.4 GHz band
let chanMax = 13;               // selectable range, from the device (regulatory domain)
let chanCurrent = 0, chanPending = 0, chanNets = null, chanLoad = null, chanPoll = null;

function buildChanSeg() {
  if (!chanSeg) return;
  chanSeg.innerHTML = "";
  for (let c = 1; c <= CHANNELS; c++) {
    const b = document.createElement("button");
    b.className = "seg-btn"; b.dataset.ch = c; b.textContent = c;
    if (c > chanMax) { b.disabled = true; b.title = "Not permitted in this regulatory domain"; }
    chanSeg.appendChild(b);
  }
}
buildChanSeg();
function setChanMax(m) {
  if (!m || m === chanMax) return;
  chanMax = m; buildChanSeg(); paintChanSeg();
}

function chanStrength(rssi) {            // -95 dBm → 0 … -35 dBm → 1
  return Math.max(0, Math.min(1, (rssi + 95) / 60));
}
function computeChanLoad(nets) {
  const load = new Array(CHANNELS + 1).fill(0);
  const count = new Array(CHANNELS + 1).fill(0);
  nets.forEach((n) => {
    if (n.ch < 1 || n.ch > CHANNELS) return;
    count[n.ch]++;
    const s = chanStrength(n.rssi);
    for (let c = 1; c <= CHANNELS; c++) {
      const d = Math.abs(c - n.ch);
      if (d < 5) load[c] += s * (1 - d / 5);
    }
  });
  return { load, count };
}

function paintChanSeg() {
  if (!chanSeg) return;
  chanSeg.querySelectorAll(".seg-btn").forEach((b) => {
    const c = +b.dataset.ch;
    b.classList.toggle("active", c === chanCurrent);
    b.classList.toggle("pending", chanPending && c === chanPending && c !== chanCurrent);
    b.classList.remove("quiet", "busy");
    if (chanLoad) {
      const max = Math.max(...chanLoad.load.slice(1), 0.01);
      const rel = chanLoad.load[c] / max;
      if (rel < 0.25) b.classList.add("quiet");
      else if (rel > 0.75) b.classList.add("busy");
    }
  });
  if (chanApply) chanApply.disabled = !(chanPending && chanPending !== chanCurrent);
}

function drawChanChart() {
  if (!chanChart) return;
  const dpr = window.devicePixelRatio || 1;
  const r = chanChart.getBoundingClientRect();
  if (r.width === 0) return;
  chanChart.width = r.width * dpr; chanChart.height = 150 * dpr;
  const ctx = chanChart.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const w = r.width, h = 150, padL = 8, padR = 8, padT = 10, padB = 22;
  ctx.clearRect(0, 0, w, h);
  const slot = (w - padL - padR) / CHANNELS;
  const barW = slot * 0.62;
  const max = chanLoad ? Math.max(...chanLoad.load.slice(1), 0.01) : 1;

  for (let c = 1; c <= CHANNELS; c++) {
    const x0 = padL + (c - 1) * slot + (slot - barW) / 2;
    // channel column background: current = accent, pending = amber
    if (c === chanCurrent || (chanPending && c === chanPending)) {
      ctx.fillStyle = c === chanCurrent ? "rgba(36,209,196,.10)" : "rgba(255,176,46,.12)";
      ctx.fillRect(padL + (c - 1) * slot, padT, slot, h - padT - padB);
    }
    if (chanLoad) {
      const v = chanLoad.load[c] / max;
      const bh = v * (h - padT - padB);
      const col = v < 0.25 ? "#3ddc84" : v > 0.75 ? DANGER : ACCENT2;
      ctx.fillStyle = col;
      ctx.globalAlpha = 0.85;
      ctx.fillRect(x0, h - padB - bh, barW, bh);
      ctx.globalAlpha = 1;
      if (chanLoad.count[c]) {
        ctx.fillStyle = "#c8d2dc"; ctx.font = "11px system-ui, sans-serif"; ctx.textAlign = "center";
        ctx.fillText(chanLoad.count[c], x0 + barW / 2, Math.max(padT + 9, h - padB - bh - 4));
      }
    }
    // axis label
    ctx.fillStyle = (c === chanCurrent) ? ACCENT : "#7d8a96";
    ctx.font = (c === 1 || c === 6 || c === 11 ? "bold " : "") + "12px system-ui, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText(c, x0 + barW / 2, h - 6);
  }
  ctx.strokeStyle = "rgba(255,255,255,.08)";
  ctx.beginPath(); ctx.moveTo(padL, h - padB + 0.5); ctx.lineTo(w - padR, h - padB + 0.5); ctx.stroke();
}

function renderChanLegend() {
  if (!chanLegend) return;
  if (!chanLoad) {
    chanLegend.innerHTML = `Current channel <strong>${chanCurrent || "?"}</strong>. Press <strong>Scan</strong> to see what's on the air.`;
    return;
  }
  const best = [1, 6, 11].reduce((a, b) => chanLoad.load[a] <= chanLoad.load[b] ? a : b);
  const total = chanNets.length;
  const here = chanLoad.count[chanCurrent] || 0;
  const strongest = chanNets.slice().sort((a, b) => b.rssi - a.rssi).slice(0, 3)
    .map((n) => `${n.ssid} (ch ${n.ch}, ${n.rssi} dBm)`).join(", ");
  chanLegend.innerHTML =
    `${total} network${total === 1 ? "" : "s"} heard; ${here} on your channel <strong>${chanCurrent}</strong>. ` +
    `Quietest of 1/6/11: <strong>${best}</strong>.` +
    (strongest ? `<br>Loudest: ${strongest}` : "");
}

function setChanCurrent(c) {
  if (!c || c === chanCurrent) return;
  chanCurrent = c;
  paintChanSeg(); drawChanChart(); renderChanLegend();
}

if (chanSeg) chanSeg.addEventListener("click", (e) => {
  const b = e.target.closest(".seg-btn"); if (!b || b.disabled) return;
  chanPending = +b.dataset.ch;
  chanMsg.textContent = chanPending === chanCurrent ? "" : `Channel ${chanPending} selected — apply to reboot onto it.`;
  chanMsg.className = "msg";
  paintChanSeg(); drawChanChart();
});

async function pollScan() {
  try {
    const d = await (await fetch("/api/scan")).json();
    if (d.maxChannel) setChanMax(d.maxChannel);
    if (d.channel) setChanCurrent(d.channel);
    if (d.state === "scanning") return false;
    if (d.state === "done") {
      chanNets = d.networks || [];
      chanLoad = computeChanLoad(chanNets);
      paintChanSeg(); drawChanChart(); renderChanLegend();
    }
    return true;
  } catch (e) { return true; }
}

if (chanScan) chanScan.addEventListener("click", async () => {
  chanScan.disabled = true; chanMsg.textContent = "Scanning…"; chanMsg.className = "msg";
  try {
    const r = await (await fetch("/api/scan", { method: "POST" })).json();
    if (!r.ok) { chanMsg.textContent = r.err || "Scan failed"; chanMsg.className = "msg err"; chanScan.disabled = false; return; }
  } catch (e) { chanMsg.textContent = "Scan failed"; chanMsg.className = "msg err"; chanScan.disabled = false; return; }
  clearInterval(chanPoll);
  const t0 = Date.now();
  chanPoll = setInterval(async () => {
    const done = await pollScan();
    if (done || Date.now() - t0 > 15000) {
      clearInterval(chanPoll); chanScan.disabled = false;
      chanMsg.textContent = done && chanLoad ? "" : "Scan timed out"; chanMsg.className = done ? "msg" : "msg err";
    }
  }, 500);
});

if (chanApply) chanApply.addEventListener("click", async () => {
  if (!chanPending || chanPending === chanCurrent) return;
  chanMsg.textContent = "Saving…"; chanMsg.className = "msg";
  try {
    const r = await (await fetch(`/api/channel?channel=${chanPending}`, { method: "POST" })).json();
    if (r.ok) { chanMsg.textContent = `Rebooting onto channel ${chanPending} — reconnect in a few seconds…`; chanMsg.className = "msg ok"; }
    else { chanMsg.textContent = r.err || "Error"; chanMsg.className = "msg err"; }
  } catch (e) { chanMsg.textContent = "Rebooting…"; chanMsg.className = "msg ok"; }
});

window.addEventListener("resize", drawChanChart);

// --- Logging tab -----------------------------------------------------------
// Channel selection lives on the board (/api/log/config) and is independent
// of the dashboard's enabled set. Device mode: the board records to flash and
// we just show status + files. Browser mode: we poll /api/log/sample at the
// chosen rate and keep rows here, streaming to a file via the File System
// Access API when available, else holding them for a Save CSV download.
const logMode = document.getElementById("logMode");
const logRate = document.getElementById("logRate");
const logStartBtn = document.getElementById("logStart");
const logBrowserSave = document.getElementById("logBrowserSave");
const logMsg = document.getElementById("logMsg");
const logName = document.getElementById("logName");
const logNameField = document.getElementById("logNameField");
const flashBar = document.getElementById("flashBar");
const flashFill = document.getElementById("flashFill");
const flashLbl = document.getElementById("flashLbl");
const logEstimate = document.getElementById("logEstimate");
const logClearFlashBtn = document.getElementById("logClearFlash");
const logFilesMsg = document.getElementById("logFilesMsg");
let flashFree = 0, flashTotal = 0;
const logStatusRoot = document.getElementById("logStatus");
const logCount = document.getElementById("logCount");
const logSearch = document.getElementById("logSearch");
const logClearBtn = document.getElementById("logClear");
const logGroupsRoot = document.getElementById("logGroups");
const logFilesRoot = document.getElementById("logFiles");
const paramsImportBtn = document.getElementById("paramsImportBtn");
const paramsImport = document.getElementById("paramsImport");
const paramsMsg = document.getElementById("paramsMsg");

const logSel = new Set();          // selected ECU channel indices
let logMax = 384;
let logDeviceRunning = false;
let logStatusTimer = null;
let logStatusPrev = "";
let logFilesPrev = "";
let logConfigLoaded = false;
// browser-mode state
let bLog = null;                   // {timer, rows, header, writable, t0}

function setLogMsg(text, cls) { if (logMsg) { logMsg.textContent = text || ""; logMsg.className = "msg " + (cls || ""); } }
function fmtBytes(b) { return b >= 1048576 ? (b / 1048576).toFixed(2) + " MB" : b >= 1024 ? (b / 1024).toFixed(1) + " KB" : b + " B"; }
function fmtDur(ms) { const s = Math.floor(ms / 1000); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`; }

async function loadLogConfig() {
  try {
    const d = await (await fetch("/api/log/config")).json();
    logSel.clear();
    (d.channels || []).forEach((c) => logSel.add(typeof c === "number" ? c : c.ch));
    if (d.rate && logRate) logRate.value = String(d.rate);
    if (d.max) logMax = d.max;
    logConfigLoaded = true;
    renderLogPicker();
    renderEstimate();
  } catch (e) { /* board offline */ }
}

let logSaveTimer = null;
function scheduleLogConfigSave() {
  clearTimeout(logSaveTimer);
  logSaveTimer = setTimeout(async () => {
    const q = `channels=${[...logSel].sort((a, b) => a - b).join(",")}&rate=${logRate ? logRate.value : 10}`;
    try {
      const r = await (await fetch(`/api/log/config?${q}`, { method: "POST" })).json();
      if (!r.ok) setLogMsg(r.err || "Couldn't save selection", "err");
    } catch (e) { setLogMsg("Couldn't reach the board", "err"); }
  }, 400);
}

function renderLogPicker() {
  if (!catalog || !logGroupsRoot) return;
  const filter = (logSearch && logSearch.value.trim().toLowerCase()) || "";
  if (logCount) logCount.textContent = `${logSel.size} of ${logMax} channels selected`;
  const openGroups = new Set([...logGroupsRoot.querySelectorAll("details[open]")].map((d) => +d.dataset.gi));
  logGroupsRoot.innerHTML = "";
  const busy = logDeviceRunning || !!bLog;
  catalog.groups.forEach((gname, gi) => {
    const items = catalog.gauges
      .filter((g) => g.group === gi && g.mapped && typeof g.ch === "number")
      .sort((a, b) => a.label.localeCompare(b.label));
    const shown = filter ? items.filter((g) => (g.label + " " + g.unit).toLowerCase().includes(filter)) : items;
    if (filter && shown.length === 0) return;
    const det = document.createElement("details");
    det.className = "gpicker"; det.dataset.gi = gi;
    det.open = filter ? true : openGroups.has(gi);
    const nOn = items.filter((g) => logSel.has(g.ch)).length;
    const sum = document.createElement("summary");
    sum.innerHTML = `${gname} <span class="count">${nOn}/${items.length}</span>`;
    det.appendChild(sum);
    shown.forEach((g) => {
      const row = document.createElement("label");
      row.className = "gitem";
      const cb = document.createElement("input");
      cb.type = "checkbox"; cb.checked = logSel.has(g.ch); cb.disabled = busy;
      cb.addEventListener("change", () => {
        if (cb.checked) {
          if (logSel.size >= logMax) { cb.checked = false; setLogMsg(`Limit is ${logMax} channels per log.`, "err"); return; }
          logSel.add(g.ch);
        } else logSel.delete(g.ch);
        setLogMsg("");
        scheduleLogConfigSave();
        renderLogPicker();
        renderEstimate();
      });
      row.appendChild(cb);
      const txt = document.createElement("span");
      txt.textContent = g.label + (g.unit ? ` (${g.unit})` : "");
      row.appendChild(txt);
      det.appendChild(row);
    });
    logGroupsRoot.appendChild(det);
  });
}
if (logSearch) logSearch.addEventListener("input", renderLogPicker);
if (logClearBtn) logClearBtn.addEventListener("click", () => {
  if (logDeviceRunning || bLog) return;
  logSel.clear(); scheduleLogConfigSave(); renderLogPicker();
});
// "Select all": every channel the ECU sends. "Select main": the same minus the
// status-bit words (switch states, protection flags, mode words — see
// BITFIELD_CHANNELS in faults.js), which are what you'd read on the Diag tab
// rather than plot.
function logSelectSet(skipBits) {
  if (logDeviceRunning || bLog || !catalog) return;
  logSel.clear();
  catalog.gauges.forEach((g) => {
    if (!g.mapped || typeof g.ch !== "number") return;
    if (skipBits && typeof BITFIELD_CHANNELS !== "undefined" && BITFIELD_CHANNELS.has(g.ch)) return;
    if (logSel.size < logMax) logSel.add(g.ch);
  });
  setLogMsg(""); scheduleLogConfigSave(); renderLogPicker(); renderEstimate();
}
const logSelectAllBtn = document.getElementById("logSelectAll");
const logSelectMainBtn = document.getElementById("logSelectMain");
if (logSelectAllBtn) logSelectAllBtn.addEventListener("click", () => logSelectSet(false));
if (logSelectMainBtn) logSelectMainBtn.addEventListener("click", () => logSelectSet(true));
if (logRate) logRate.addEventListener("change", () => { scheduleLogConfigSave(); renderEstimate(); });
if (logMode) logMode.addEventListener("change", () => { if (logNameField) logNameField.hidden = logMode.value !== "device"; });
if (logClearFlashBtn) logClearFlashBtn.addEventListener("click", async () => {
  if (!confirm("Delete every log stored on the board? Export anything you want to keep first.")) return;
  try {
    const r = await (await fetch("/api/log/clear", { method: "POST" })).json();
    logFilesMsg.textContent = `Removed ${r.removed} log${r.removed === 1 ? "" : "s"}.`; logFilesMsg.className = "msg ok";
  } catch (e) { logFilesMsg.textContent = "Couldn't reach the board."; logFilesMsg.className = "msg err"; }
  logFilesPrev = ""; pollLogStatus();
});

function renderLogStatus(d) {
  if (!logStatusRoot) return;
  let html;
  if (bLog) {
    html = chip("Mode", "browser", "ok") + chip("Rows", bLog.rows.length + bLog.flushed, "ok") +
           chip("Elapsed", fmtDur(performance.now() - bLog.t0), "ok") +
           chip("Saving", bLog.writable ? "streaming to file" : "in memory", bLog.writable ? "ok" : "warn");
  } else if (d) {
    const free = d.fsTotal ? d.fsTotal - d.fsUsed : 0;
    html = chip("Mode", d.running ? "board · recording" : "idle", d.running ? "ok" : "") +
           (d.running ? chip("File", d.file, "ok") + chip("Rows", d.rows, "ok") +
                        chip("Elapsed", fmtDur(d.elapsedMs), "ok") + chip("Size", fmtBytes(d.bytes), "ok") : "") +
           chip("Flash free", fmtBytes(free), free < 262144 ? "warn" : "") +
           (d.dropped ? chip("Dropped rows", d.dropped, "warn") : "");
  } else html = chip("Logger", "--", "");
  if (html !== logStatusPrev) { setHTML(logStatusRoot, html); logStatusPrev = html; }
}

function renderFlash(d) {
  if (d && d.fsTotal) { flashTotal = d.fsTotal; flashFree = d.fsTotal - d.fsUsed; }
  if (flashBar && flashTotal) {
    const used = flashTotal - flashFree, pct = Math.min(100, Math.round(used / flashTotal * 100));
    flashFill.style.width = pct + "%";
    flashLbl.textContent = `flash: ${fmtBytes(used)} used of ${fmtBytes(flashTotal)} · ${fmtBytes(flashFree)} free`;
    flashBar.className = "flashbar" + (flashFree < 131072 ? " full" : flashFree < 524288 ? " warn" : "");
  }
  renderEstimate();
}

// Bytes per second for the current selection/rate, and how long the free
// flash lasts at that rate — the number people actually need before pressing
// Start on a long drive.
function renderEstimate() {
  if (!logEstimate) return;
  const n = logSel.size, hz = +(logRate ? logRate.value : 10) || 10;
  if (!n) { logEstimate.textContent = "Select channels to see the storage estimate."; logEstimate.className = "hint"; return; }
  const bps = (4 + 2 * n) * hz;
  const perMin = bps * 60;
  let txt = `${n} channel${n === 1 ? "" : "s"} at ${hz} Hz ≈ ${fmtBytes(perMin)}/min on the board (CSV is ~4× larger).`;
  let cls = "hint";
  if (flashFree) {
    const secs = flashFree / bps;
    const dur = secs >= 3600 ? `${(secs / 3600).toFixed(1)} h` : secs >= 60 ? `${Math.round(secs / 60)} min` : `${Math.round(secs)} s`;
    txt += ` Free flash lasts about ${dur}.`;
    if (secs < 600) { cls += " err"; txt += " Lower the rate or the channel count for a longer log."; }
    else if (secs < 1800) cls += " warn";
  }
  logEstimate.textContent = txt; logEstimate.className = cls;
}

function renderLogFiles(d) {
  if (!logFilesRoot || !d) return;
  const files = (d.files || []).slice().sort((a, b) => b.name.localeCompare(a.name));
  const key = JSON.stringify(files) + (d.running ? d.file : "");
  if (key === logFilesPrev) return;
  logFilesPrev = key;
  logFilesRoot.innerHTML = "";
  if (!files.length) { logFilesRoot.innerHTML = '<span class="hint">No logs on the board yet.</span>'; return; }
  files.forEach((f) => {
    const row = document.createElement("div");
    const active = d.running && d.file === f.name;
    row.className = "logfile" + (active ? " active" : "");
    row.innerHTML = `<span class="name">${f.name}</span><span class="size">${fmtBytes(f.bytes)}${active ? " · recording" : ""}</span><span class="spacer"></span>`;
    const csv = document.createElement("a"); csv.className = "btn small"; csv.textContent = "CSV";
    csv.href = `/api/log/download?name=${encodeURIComponent(f.name)}&fmt=csv`; csv.setAttribute("download", f.name.replace(".ilg", ".csv"));
    const raw = document.createElement("a"); raw.className = "btn small"; raw.textContent = "Raw";
    raw.href = `/api/log/download?name=${encodeURIComponent(f.name)}&fmt=raw`; raw.setAttribute("download", f.name);
    const del = document.createElement("button"); del.className = "btn small danger"; del.textContent = "Delete"; del.disabled = active;
    del.addEventListener("click", async () => {
      if (!confirm(`Delete ${f.name}?`)) return;
      await fetch(`/api/log/delete?name=${encodeURIComponent(f.name)}`, { method: "POST" }).catch(() => {});
      logFilesPrev = ""; pollLogStatus();
    });
    row.append(csv, raw, del);
    logFilesRoot.appendChild(row);
  });
}

async function pollLogStatus() {
  try {
    const d = await (await fetch("/api/log/status")).json();
    const was = logDeviceRunning;
    logDeviceRunning = !!d.running;
    if (was !== logDeviceRunning) { renderLogPicker(); updateLogButtons(); }
    renderLogStatus(d);
    renderFlash(d);
    renderLogFiles(d);
  } catch (e) { renderLogStatus(null); }
}

function updateLogButtons() {
  if (!logStartBtn) return;
  const running = logDeviceRunning || !!bLog;
  logStartBtn.textContent = running ? "Stop logging" : "Start logging";
  logStartBtn.classList.toggle("active", running);
  if (logMode) logMode.disabled = running;
  if (logRate) logRate.disabled = running;
  if (logBrowserSave) logBrowserSave.hidden = !(bLog && !bLog.writable && bLog.rows.length);
}

// ---- browser-mode recording -------------------------------------------------
function csvEscape(s) {
  const needs = s.indexOf(",") >= 0 || s.indexOf('"') >= 0 || s.indexOf("\n") >= 0;
  return needs ? '"' + s.split('"').join('""') + '"' : s;
}

async function startBrowserLog() {
  if (logSel.size === 0) { setLogMsg("Select at least one channel first.", "err"); return; }
  const chs = [...logSel].sort((a, b) => a - b);
  const meta = chs.map((ch) => catalog.gauges.find((g) => g.ch === ch) || { label: "ch" + ch, unit: "", dp: 0 });
  const header = "time_s," + meta.map((m) => csvEscape(m.label + (m.unit ? ` (${m.unit})` : ""))).join(",") + "\n";
  let writable = null;
  if (window.showSaveFilePicker) {
    try {
      const h = await window.showSaveFilePicker({ suggestedName: `ignitron_${new Date().toISOString().replace(/[:.]/g, "-")}.csv`,
        types: [{ description: "CSV", accept: { "text/csv": [".csv"] } }] });
      writable = await h.createWritable();
      await writable.write(header);
    } catch (e) { writable = null; /* cancelled or unsupported: fall back to memory */ }
  }
  bLog = { chs, meta, header, rows: [], flushed: 0, writable, t0: performance.now(), timer: null, pending: "" };
  const period = 1000 / (+logRate.value || 10);
  bLog.timer = setInterval(async () => {
    if (!bLog) return;
    try {
      const d = await (await fetch("/api/log/sample")).json();
      if (!d.link) return;                       // no ECU data: don't log garbage
      const t = ((performance.now() - bLog.t0) / 1000).toFixed(3);
      const line = t + "," + (d.v || []).map((v, i) => v == null ? "" : Number(v).toFixed(bLog.meta[i].dp || 0)).join(",") + "\n";
      if (bLog.writable) {
        bLog.pending += line;
        if (bLog.pending.length > 4096) { const p = bLog.pending; bLog.pending = ""; await bLog.writable.write(p); }
        bLog.flushed++;
      } else bLog.rows.push(line);
      renderLogStatus(null);
      if (logBrowserSave) logBrowserSave.hidden = !!bLog.writable;
    } catch (e) { /* skip this sample */ }
  }, period);
  updateLogButtons(); renderLogPicker();
  setLogMsg(writable ? "Streaming to your file." : "Recording in this page — press Save CSV when done.", "ok");
}

async function stopBrowserLog(save) {
  if (!bLog) return;
  clearInterval(bLog.timer);
  const b = bLog; bLog = null;
  if (b.writable) {
    try { if (b.pending) await b.writable.write(b.pending); await b.writable.close(); } catch (e) { /* ignore */ }
    setLogMsg(`Saved ${b.flushed} rows to your file.`, "ok");
  } else if (save && b.rows.length) {
    const blob = new Blob([b.header, ...b.rows], { type: "text/csv" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = `ignitron_${new Date().toISOString().replace(/[:.]/g, "-")}.csv`;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    setLogMsg(`Saved ${b.rows.length} rows.`, "ok");
  } else if (b.rows.length) {
    // keep rows around for a later Save
    bLog = null; pendingBrowserRows = b;
  }
  updateLogButtons(); renderLogPicker(); renderLogStatus(null);
}
let pendingBrowserRows = null;
if (logBrowserSave) logBrowserSave.addEventListener("click", () => {
  const b = bLog || pendingBrowserRows;
  if (!b || !b.rows.length) return;
  const blob = new Blob([b.header, ...b.rows], { type: "text/csv" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = `ignitron_${new Date().toISOString().replace(/[:.]/g, "-")}.csv`;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  setLogMsg(`Saved ${b.rows.length} rows.`, "ok");
  if (!bLog) { pendingBrowserRows = null; logBrowserSave.hidden = true; }
});

if (logStartBtn) logStartBtn.addEventListener("click", async () => {
  if (bLog) { await stopBrowserLog(true); return; }
  if (logDeviceRunning) {
    try { await fetch("/api/log/stop", { method: "POST" }); } catch (e) {}
    setLogMsg("Stopped.", "ok"); await pollLogStatus(); return;
  }
  if (logSel.size === 0) { setLogMsg("Select at least one channel first.", "err"); return; }
  if (logMode && logMode.value === "browser") { await startBrowserLog(); return; }
  try {
    const nm = logName ? logName.value.trim() : "";
    const r = await (await fetch(`/api/log/start${nm ? "?name=" + encodeURIComponent(nm) : ""}`, { method: "POST" })).json();
    if (r.ok) { setLogMsg(`Recording to ${r.file}.`, "ok"); if (logName) logName.value = ""; }
    else setLogMsg(r.err || "Couldn't start", "err");
  } catch (e) { setLogMsg("Couldn't reach the board.", "err"); }
  await pollLogStatus();
});

// ---- parameters import / export --------------------------------------------
if (paramsImportBtn && paramsImport) {
  paramsImportBtn.addEventListener("click", () => paramsImport.click());
  paramsImport.addEventListener("change", async () => {
    const file = paramsImport.files && paramsImport.files[0];
    paramsImport.value = "";
    if (!file) return;
    paramsMsg.textContent = "Importing…"; paramsMsg.className = "msg";
    try {
      const text = await file.text();
      JSON.parse(text);   // fail fast on a non-JSON file
      const r = await (await fetch("/api/params", { method: "POST", headers: { "Content-Type": "application/json" }, body: text })).json();
      if (r.ok) {
        paramsMsg.textContent = `Applied — ${r.applied} gauge${r.applied === 1 ? "" : "s"} enabled${r.skipped ? `, ${r.skipped} unknown skipped` : ""}.`;
        paramsMsg.className = "msg ok";
        await loadGauges(); await loadLogConfig();
      } else { paramsMsg.textContent = r.err || "Import failed"; paramsMsg.className = "msg err"; }
    } catch (e) { paramsMsg.textContent = "Not a valid parameters file"; paramsMsg.className = "msg err"; }
  });
}

// --- tabs ----------------------------------------------------------------
// --- OTA --------------------------------------------------------------------
// Two-stage by design (see ota_manager.h): the filesystem image carries the web
// UI and does NOT reboot, so the firmware upload follows it and reboots once at
// the end — that way the UI and the application that serves it land together.
// Completed steps are remembered briefly in localStorage so the step indicator
// survives the reboot the firmware stage triggers.
let otaFile = null;
const OTA_STEPS_KEY = "ign_ota_steps";

function otaLoadDone() {
  try {
    const raw = JSON.parse(localStorage.getItem(OTA_STEPS_KEY) || "{}");
    if (!raw.ts || Date.now() - raw.ts > 15 * 60 * 1000) return [];
    return Array.isArray(raw.done) ? raw.done : [];
  } catch (e) { return []; }
}
function otaMarkDone(step) {
  const done = otaLoadDone();
  if (!done.includes(step)) done.push(step);
  try { localStorage.setItem(OTA_STEPS_KEY, JSON.stringify({ done, ts: Date.now() })); }
  catch (e) { /* private mode: the indicator just won't persist */ }
  renderOtaSteps();
}
function renderOtaSteps() {
  const sel = document.getElementById("otaType");
  const cur = sel ? sel.value : "filesystem";
  const done = otaLoadDone();
  document.querySelectorAll("#otaSteps .ota-step").forEach((el) => {
    const s = el.dataset.step;
    el.classList.toggle("done", done.includes(s));
    el.classList.toggle("active", s === cur && !done.includes(s));
  });
}
function setOtaStatus(msg, cls) {
  const el = document.getElementById("otaStatus");
  if (!el) return;
  el.textContent = msg || "";
  el.className = "ota-status" + (cls ? " " + cls : "");
}

function startOtaUpload() {
  if (!otaFile) { setOtaStatus("Select a .bin file first", "error"); return; }
  const type = (document.getElementById("otaType") || {}).value || "firmware";
  const isFs = type === "filesystem";
  const btn = document.getElementById("otaUploadBtn");
  const wrap = document.getElementById("otaProgressWrap");
  const bar = document.getElementById("otaProgressBar");
  const lbl = document.getElementById("otaProgressLabel");

  const fd = new FormData();
  fd.append("update", otaFile, otaFile.name);
  const xhr = new XMLHttpRequest();
  xhr.open("POST", isFs ? "/api/ota/fs" : "/api/ota");

  if (btn) btn.disabled = true;
  if (wrap) wrap.style.display = "block";
  if (bar) bar.style.width = "0%";
  if (lbl) lbl.textContent = "0%";
  setOtaStatus("Uploading…", "");

  xhr.upload.addEventListener("progress", (e) => {
    if (!e.lengthComputable) return;
    const pct = Math.round((e.loaded / e.total) * 100);
    if (bar) bar.style.width = pct + "%";
    if (lbl) lbl.textContent = pct + "%";
  });

  xhr.addEventListener("load", () => {
    let ok = false;
    try { ok = JSON.parse(xhr.responseText).success === true; }
    catch (e) { ok = xhr.status === 200; }
    if (!ok) {
      setOtaStatus("Update failed. Please try again.", "error");
      if (wrap) wrap.style.display = "none";
      if (btn) btn.disabled = false;
      return;
    }
    if (bar) bar.style.width = "100%";
    if (lbl) lbl.textContent = "100%";
    otaMarkDone(type);
    if (isFs) {
      // No reboot after the filesystem stage — move the user on to step 2.
      const sel = document.getElementById("otaType");
      if (sel) sel.value = "firmware";
      renderOtaSteps();
      setOtaStatus("Filesystem updated. Now upload the firmware.", "success");
      otaFile = null;
      const fi = document.getElementById("otaFile");
      if (fi) fi.value = "";
      setText("otaFileName", "");
      if (wrap) wrap.style.display = "none";
      if (btn) btn.disabled = true;
    } else {
      setOtaStatus("Update complete. Rebooting…", "success");
    }
  });
  xhr.addEventListener("error", () => {
    setOtaStatus("Upload failed. Check the connection and retry.", "error");
    if (wrap) wrap.style.display = "none";
    if (btn) btn.disabled = false;
  });
  xhr.send(fd);
}

function initOta() {
  const fi = document.getElementById("otaFile");
  const btn = document.getElementById("otaUploadBtn");
  if (!fi || !btn) return;

  fetch("/api/ota/info").then((r) => r.json()).then((info) => {
    setHTML(document.getElementById("otaInfo"),
      chip("Firmware", info.version || "—") +
      chip("Board", info.board || "—") +
      chip("Hardware", info.hardware || "—"));
  }).catch(() => { /* offline: leave it empty */ });

  fi.addEventListener("change", () => {
    const f = fi.files && fi.files[0];
    if (!f) return;
    if (!f.name.toLowerCase().endsWith(".bin")) {
      setOtaStatus("Please choose a .bin file", "error");
      btn.disabled = true;
      return;
    }
    otaFile = f;
    btn.disabled = false;
    setText("otaFileName", `${f.name} (${(f.size / 1024).toFixed(0)} KB)`);
    setOtaStatus("", "");
  });

  btn.addEventListener("click", startOtaUpload);

  const sel = document.getElementById("otaType");
  if (sel) {
    // Resume mid-sequence: if the filesystem stage is already done, start on 2.
    const done = otaLoadDone();
    if (done.includes("filesystem") && !done.includes("firmware")) sel.value = "firmware";
    sel.addEventListener("change", renderOtaSteps);
  }
  renderOtaSteps();
}

const tabButtons = document.querySelectorAll(".tab");
const tabPanels = document.querySelectorAll(".tabpanel");
tabButtons.forEach((btn) => btn.addEventListener("click", () => {
  tabButtons.forEach((b) => b.classList.remove("active"));
  tabPanels.forEach((p) => p.classList.remove("active"));
  btn.classList.add("active");
  document.getElementById("tab-" + btn.dataset.tab).classList.add("active");
  if (btn.dataset.tab === "dashboard") gauges.forEach((g) => g.resize());
  if (btn.dataset.tab === "settings") { loadWifi(); setTimeout(drawChanChart, 0); }
  if (btn.dataset.tab === "logging") {
    if (!logConfigLoaded) loadLogConfig(); else renderLogPicker();
    pollLogStatus();
    clearInterval(logStatusTimer); logStatusTimer = setInterval(pollLogStatus, 1000);
  } else { clearInterval(logStatusTimer); logStatusTimer = null; }
}));

async function initApp() {
  loadWifi();
  initOta();
  await loadGauges();    // catalogue first: it builds the dial grid
  loadLogConfig();
  await fetchStatus();   // learns whether a gauge poll is already running
  setInterval(fetchStatus, 100); // 10 Hz live data
  requestAnimationFrame(animate);
  if (catalog && catalog.autoLaunch && !launched) launchGauges(true);
}

if (document.readyState === "loading")
  document.addEventListener("DOMContentLoaded", initApp);
else
  initApp();

