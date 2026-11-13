"use strict";
// APRS hunter GUI. Polls the JSON API and draws with plain DOM and SVG, so
// it works offline with no libraries.

const REFRESH_MS = 5000;
const LIVE_MS = 30000;
const STALE_MS = 5 * 60 * 1000;
const DEMODULATOR_FLOOR_DB = { 6: -5, 7: -7.5, 8: -10, 9: -12.5, 10: -15, 11: -17.5, 12: -20 };
const WEATHER_UNITS = {
  temperature: "°C", humidity: "%", pressure: "hPa", wind_speed: "m/s", wind_gust: "m/s",
  wind_direction: "°", rain_1h: "mm", rain_24h: "mm", rain_since_midnight: "mm", luminosity: "W/m²",
};

const state = { hours: 24, sortKey: "last_utc", sortDir: -1, data: null, timer: null };

// ---- Formatting ----

const $ = (selector) => document.querySelector(selector);

function esc(value) {
  return String(value ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function fmt(value, digits = 1) {
  return value === null || value === undefined || Number.isNaN(value) ? "–" : Number(value).toFixed(digits);
}

function signed(value, digits = 0) {
  if (value === null || value === undefined) return "–";
  return (value > 0 ? "+" : "") + Number(value).toFixed(digits);
}

function ms(iso) {
  return iso ? Date.parse(iso) : NaN;
}

function age(iso) {
  const t = ms(iso);
  if (Number.isNaN(t)) return "–";
  const s = Math.max(0, Math.round((Date.now() - t) / 1000));
  if (s < 60) return s + " s";
  if (s < 3600) return Math.round(s / 60) + " min";
  if (s < 86400) return Math.round(s / 3600) + " h";
  return Math.round(s / 86400) + " d";
}

function clock(iso) {
  const t = ms(iso);
  return Number.isNaN(t) ? "–" : new Date(t).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

function dateTime(iso) {
  const t = ms(iso);
  return Number.isNaN(t) ? "–" : new Date(t).toLocaleString();
}

function osmLink(lat, lon, text) {
  if (lat === null || lat === undefined || lon === null || lon === undefined) return "–";
  const url = `https://www.openstreetmap.org/?mlat=${lat}&mlon=${lon}#map=13/${lat}/${lon}`;
  return `<a href="${esc(url)}" target="_blank" rel="noopener">${esc(text ?? `${fmt(lat, 5)}, ${fmt(lon, 5)}`)}</a>`;
}

// ---- Scales and SVG ----

function linear(d0, d1, r0, r1) {
  const k = d1 === d0 ? 0 : (r1 - r0) / (d1 - d0);
  return (v) => r0 + (v - d0) * k;
}

function niceStep(span, count) {
  const raw = (span || 1) / count;
  const magnitude = 10 ** Math.floor(Math.log10(raw));
  const n = raw / magnitude;
  return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * magnitude;
}

function ticks(min, max, count) {
  const step = niceStep(max - min, count);
  const out = [];
  for (let v = Math.ceil(min / step) * step; v <= max + step * 1e-9; v += step) out.push(Number(v.toFixed(10)));
  return out;
}

function svg(width, height, body) {
  return `<svg viewBox="0 0 ${width} ${height}" role="img" xmlns="http://www.w3.org/2000/svg">${body}</svg>`;
}

// Draw at the box's real width, so text keeps its size on a phone.
function chartWidth(target, fallback) {
  return Math.max(300, Math.round(target.clientWidth || fallback));
}

function empty(text) {
  return `<p class="empty">${esc(text)}</p>`;
}

function yAxis(values, y, x0, x1, label = (v) => v) {
  return values.map((v) =>
    `<line class="grid" x1="${x0}" x2="${x1}" y1="${y(v)}" y2="${y(v)}"/>` +
    `<text x="${x0 - 4}" y="${y(v) + 4}" text-anchor="end">${esc(label(v))}</text>`).join("");
}

function xAxis(values, x, y0, y1, label = (v) => v) {
  return values.map((v) =>
    `<line class="grid" x1="${x(v)}" x2="${x(v)}" y1="${y0}" y2="${y1}"/>` +
    `<text x="${x(v)}" y="${y1 + 14}" text-anchor="middle">${esc(label(v))}</text>`).join("");
}

function timeTicks(t0, t1, count) {
  const span = t1 - t0;
  const steps = [60e3, 5 * 60e3, 15 * 60e3, 30 * 60e3, 3600e3, 3 * 3600e3, 6 * 3600e3, 12 * 3600e3, 86400e3, 7 * 86400e3];
  const step = steps.find((s) => span / s <= count) || steps[steps.length - 1];
  const out = [];
  for (let t = Math.ceil(t0 / step) * step; t <= t1; t += step) out.push(t);
  return out;
}

function timeLabel(t, span) {
  const d = new Date(t);
  if (span > 2 * 86400e3) return d.toLocaleDateString([], { month: "short", day: "numeric" });
  return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

// ---- Data ----

function sinceIso() {
  return state.hours ? new Date(Date.now() - state.hours * 3600e3).toISOString() : "";
}

async function api(route, params = {}) {
  const query = new URLSearchParams(params).toString();
  const response = await fetch(`/api/${route}${query ? "?" + query : ""}`, { cache: "no-store" });
  if (!response.ok) throw new Error(`${route}: HTTP ${response.status}`);
  return response.json();
}

async function refresh() {
  clearTimeout(state.timer);
  const since = sinceIso();
  const params = since ? { since } : {};
  try {
    const [summary, packets, stations, noise, hourly, events] = await Promise.all([
      api("summary", params), api("packets", params), api("stations", params),
      api("noise", params), api("hourly", params), api("events", params),
    ]);
    state.data = { summary, packets, stations, noise, hourly, events };
    render();
    showStatus("");
  } catch (error) {
    showStatus(`Cannot reach the logger (${error.message}). Retrying.`);
  } finally {
    state.timer = setTimeout(refresh, REFRESH_MS);
  }
}

function showStatus(text) {
  const status = $("#status");
  status.textContent = text;
  status.hidden = !text;
}

function goodPackets() {
  return state.data.packets.filter((p) => p.crc_ok);
}

function spreadingFactor() {
  return state.data.summary.last_boot?.sf || 12;
}

// ---- Rendering ----

function render() {
  renderLive();
  renderTiles();
  renderNoise();
  renderCompass();
  renderDistance();
  renderSnr();
  renderHourly();
  renderFrequencyError();
  renderStations();
  renderPackets();
  renderEvents();
}

function renderLive() {
  const last = state.data.summary.last_record_utc;
  const elapsed = Date.now() - ms(last);
  const dot = $("#live");
  dot.className = "dot" + (elapsed < LIVE_MS ? " live" : elapsed < STALE_MS ? " stale" : "");
  dot.title = last ? `Last record ${age(last)} ago` : "No data yet";
}

function tile(label, value, sub) {
  return `<div class="tile"><div class="label">${esc(label)}</div><div class="value">${value}</div><div class="sub">${sub}</div></div>`;
}

// What the RSSI read check found in the range. Firmware 0.3.0 and later drop
// torn reads; older firmware kept them, and they show as glitch windows.
function readCheckParts(s) {
  const counted = (n, one, many) => `${esc(n)} ${n === 1 ? one : many}`;
  const parts = [];
  if (s.noise_read_mismatches !== null) parts.push(counted(s.noise_read_mismatches, "RSSI read dropped", "RSSI reads dropped"));
  if (s.noise_skipped_samples) parts.push(counted(s.noise_skipped_samples, "sample skipped", "samples skipped"));
  if (s.noise_glitches) parts.push(counted(s.noise_glitches, "glitch window", "glitch windows") + " from firmware before 0.3.0");
  return parts;
}

function renderTiles() {
  const s = state.data.summary;
  const failRate = s.packets ? ` (${fmt((100 * s.crc_fail) / s.packets, 0)}% fail)` : "";
  const last = s.last_packet;
  const noise = s.last_noise;
  const rx = s.receiver;
  const boot = s.last_boot;
  const far = s.farthest;

  $("#tiles").innerHTML = [
    tile("Packets", esc(s.packets), `${esc(s.ok)} ok · ${esc(s.crc_fail)} CRC fail${failRate}`),
    tile("Stations", esc(s.stations), "with at least one good packet"),
    tile("Farthest", far ? `${fmt(far.distance_km, 1)} km` : "–",
      far ? `${esc(far.src)} at ${fmt(far.bearing_deg, 0)}°` : "needs station positions and a receiver position"),
    tile("Last packet", last ? esc(age(last.host_utc)) : "–",
      last ? `${esc(last.src || "?")} · ${fmt(last.rssi_dbm)} dBm · SNR ${fmt(last.snr_db)} dB${last.crc_ok ? "" : " · CRC fail"}` : "none in this range"),
    tile("Noise now", noise ? `${fmt(noise.mean_dbm)} dBm` : "–",
      noise ? [`min ${fmt(noise.min_dbm, 0)}${noise.min_is_glitch ? " (glitch)" : ""}`, `max ${fmt(noise.max_dbm, 0)}`,
        `${esc(noise.samples)} samples`, ...readCheckParts(s)].join(" · ") : "no noise records"),
    tile("Receiver", rx && rx.rx_source ? esc(rx.rx_source) : "none",
      rx && rx.rx_lat !== null ? `${osmLink(rx.rx_lat, rx.rx_lon)}${rx.rx_sats ? ` · ${esc(rx.rx_sats)} sats` : ""}${rx.rx_hdop ? ` · HDOP ${fmt(rx.rx_hdop)}` : ""}` : "set --home or --gps-port"),
    tile("GPS clock", rx && rx.gps_minus_host_s !== null && rx.gps_minus_host_s !== undefined ? `${signed(rx.gps_minus_host_s, 2)} s` : "–",
      rx && rx.gps_utc ? "GPS time minus computer time" : "no GPS time"),
    tile("Board", boot ? `fw ${esc(boot.fw)}` : "–",
      boot ? `SF${esc(boot.sf)} · ${fmt(boot.freq_hz / 1e6, 3)} MHz · ${esc(boot.bw_hz / 1000)} kHz · ${esc(s.boots)} boots · ${esc(s.errors)} errors` : "no boot record"),
  ].join("");
}

function renderNoise() {
  const { noise, packets } = state.data;
  const target = $("#chart-noise");
  const points = noise.filter((n) => n.mean_dbm !== null);
  if (!points.length && !packets.length) {
    target.innerHTML = empty("No noise records or packets in this range.");
    return;
  }
  const W = chartWidth(target, 960), H = 240, L = 44, R = 10, T = 10, B = 24;
  const times = points.map((n) => ms(n.host_utc)).concat(packets.map((p) => ms(p.host_utc)));
  // Start at the first record inside the range, so a short log fills the width.
  const t1 = Date.now();
  const first = Math.min(...times);
  let t0 = state.hours ? Math.max(t1 - state.hours * 3600e3, first) : first;
  if (t1 - t0 < 60e3) t0 = t1 - 60e3;
  const values = [];
  points.forEach((n) => { values.push(n.mean_dbm, n.max_dbm); if (n.min_dbm !== null) values.push(n.min_dbm); });
  packets.forEach((p) => { if (p.rssi_dbm !== null) values.push(p.rssi_dbm); });
  const lo = Math.max(-140, Math.floor(Math.min(...values) - 3));
  const hi = Math.min(-20, Math.ceil(Math.max(...values) + 3));
  const x = linear(t0, t1, L, W - R);
  const y = linear(lo, hi, H - B, T);
  const clampY = (v) => y(Math.max(lo, Math.min(hi, v)));

  let body = yAxis(ticks(lo, hi, 5), y, L, W - R, (v) => v);
  body += xAxis(timeTicks(t0, t1, 8), x, T, H - B, (t) => timeLabel(t, t1 - t0));
  if (points.length) {
    const top = points.map((n) => `${x(ms(n.host_utc))},${clampY(n.max_dbm)}`);
    const bottom = points.slice().reverse().map((n) => `${x(ms(n.host_utc))},${clampY(n.min_dbm ?? n.mean_dbm)}`);
    body += `<polygon class="band" points="${top.concat(bottom).join(" ")}"/>`;
    body += `<polyline class="mean" points="${points.map((n) => `${x(ms(n.host_utc))},${clampY(n.mean_dbm)}`).join(" ")}"/>`;
    body += points.filter((n) => n.glitches > 0).map((n) =>
      `<line class="glitch" x1="${x(ms(n.host_utc))}" x2="${x(ms(n.host_utc))}" y1="${H - B}" y2="${H - B - 6}"><title>${esc(n.glitches)} glitch reading(s) from firmware before 0.3.0 near ${esc(clock(n.host_utc))}</title></line>`).join("");
  }
  body += packets.filter((p) => p.rssi_dbm !== null).map((p) =>
    `<circle data-packet="${p.id}" class="${p.crc_ok ? "ok" : "fail"}" cx="${x(ms(p.host_utc))}" cy="${clampY(p.rssi_dbm)}" r="3.5">` +
    `<title>${esc(p.src || "?")} · ${fmt(p.rssi_dbm)} dBm · SNR ${fmt(p.snr_db)} dB · ${esc(clock(p.host_utc))}</title></circle>`).join("");
  target.innerHTML = svg(W, H, body);
}

function renderCompass() {
  const target = $("#chart-compass");
  const stations = state.data.stations.filter((s) => s.distance_km !== null && s.bearing_deg !== null);
  if (!stations.length) {
    target.innerHTML = empty("No station positions yet. Stations that send a position appear here once the receiver has a position (--home or --gps-port).");
    return;
  }
  const S = Math.min(chartWidth(target, 340), 440), C = S / 2, R = C - 26;
  const maxD = Math.max(...stations.map((s) => s.distance_km)) || 1;
  const rings = ticks(0, maxD, 3).filter((d) => d > 0);
  const outer = Math.max(maxD, rings[rings.length - 1] || maxD);
  const r = linear(0, outer, 0, R);
  // Ring labels sit east-south-east, away from the cardinal lines.
  const labelAngle = (112 * Math.PI) / 180;
  let body = rings.map((d) => `<circle class="grid" cx="${C}" cy="${C}" r="${r(d)}" fill="none"/>` +
    `<text x="${C + r(d) * Math.sin(labelAngle) + 3}" y="${C - r(d) * Math.cos(labelAngle) + 4}">${esc(d)} km</text>`).join("");
  body += `<line class="grid" x1="${C}" x2="${C}" y1="${C - R}" y2="${C + R}"/><line class="grid" x1="${C - R}" x2="${C + R}" y1="${C}" y2="${C}"/>`;
  body += [["N", C, C - R - 8], ["E", C + R + 10, C + 4], ["S", C, C + R + 16], ["W", C - R - 10, C + 4]]
    .map(([t, tx, ty]) => `<text x="${tx}" y="${ty}" text-anchor="middle">${t}</text>`).join("");
  body += `<circle class="ok" cx="${C}" cy="${C}" r="2.5"/>`;
  body += stations.map((s) => {
    const a = (s.bearing_deg * Math.PI) / 180;
    const px = C + r(s.distance_km) * Math.sin(a);
    const py = C - r(s.distance_km) * Math.cos(a);
    const size = 3 + Math.min(5, Math.log2(s.packets));
    return `<circle class="${s.direct ? "ok" : "hollow"}" cx="${px}" cy="${py}" r="${size}">` +
      `<title>${esc(s.src)} · ${fmt(s.distance_km)} km at ${fmt(s.bearing_deg, 0)}° · ${esc(s.packets)} packets</title></circle>` +
      `<text class="label" x="${px + size + 3}" y="${py + 4}">${esc(s.src)}</text>`;
  }).join("");
  target.innerHTML = svg(S, S, body);
}

function renderDistance() {
  const target = $("#chart-distance");
  const points = goodPackets().filter((p) => p.distance_km !== null && p.rssi_dbm !== null);
  if (!points.length) {
    target.innerHTML = empty("Needs packets with a station position and a receiver position.");
    return;
  }
  const W = chartWidth(target, 420), H = 260, L = 44, R = 10, T = 10, B = 26;
  const maxD = Math.max(...points.map((p) => p.distance_km)) * 1.05 || 1;
  const rssi = points.map((p) => p.rssi_dbm);
  const lo = Math.floor(Math.min(...rssi) - 3), hi = Math.ceil(Math.max(...rssi) + 3);
  const x = linear(0, maxD, L, W - R), y = linear(lo, hi, H - B, T);
  let body = yAxis(ticks(lo, hi, 5), y, L, W - R);
  body += xAxis(ticks(0, maxD, 5), x, T, H - B, (v) => `${v} km`);
  body += points.map((p) =>
    `<circle data-packet="${p.id}" class="${p.heard_via === "direct" ? "ok" : "hollow"}" cx="${x(p.distance_km)}" cy="${y(p.rssi_dbm)}" r="3.5">` +
    `<title>${esc(p.src)} · ${fmt(p.distance_km)} km · ${fmt(p.rssi_dbm)} dBm${p.heard_via === "direct" ? "" : " · via " + esc(p.heard_via)}</title></circle>`).join("");
  target.innerHTML = svg(W, H, body);
}

function renderSnr() {
  const target = $("#chart-snr");
  const snrs = goodPackets().map((p) => p.snr_db).filter((v) => v !== null);
  if (!snrs.length) {
    target.innerHTML = empty("No good packets in this range.");
    return;
  }
  const floor = DEMODULATOR_FLOOR_DB[spreadingFactor()] ?? -20;
  const lo = Math.floor(Math.min(floor - 4, ...snrs) / 2) * 2;
  const hi = Math.ceil(Math.max(12, ...snrs) / 2) * 2;
  const bins = [];
  for (let b = lo; b < hi; b += 2) bins.push({ from: b, count: snrs.filter((v) => v >= b && v < b + 2).length });
  const W = chartWidth(target, 420), H = 260, L = 34, R = 10, T = 10, B = 26;
  const maxCount = Math.max(...bins.map((b) => b.count));
  const x = linear(lo, hi, L, W - R), y = linear(0, maxCount, H - B, T);
  let body = yAxis(ticks(0, maxCount, Math.min(4, maxCount)), y, L, W - R);
  body += xAxis(ticks(lo, hi, 6), x, T, H - B, (v) => `${v} dB`);
  body += bins.filter((b) => b.count).map((b) =>
    `<rect class="bar" x="${x(b.from) + 1}" y="${y(b.count)}" width="${x(b.from + 2) - x(b.from) - 2}" height="${H - B - y(b.count)}"><title>${b.from} to ${b.from + 2} dB: ${b.count}</title></rect>`).join("");
  body += `<line class="limit" x1="${x(floor)}" x2="${x(floor)}" y1="${T}" y2="${H - B}"/><text x="${x(floor) + 4}" y="${T + 10}">SF${spreadingFactor()} limit ${floor} dB</text>`;
  target.innerHTML = svg(W, H, body);
}

function renderHourly() {
  const target = $("#chart-hourly");
  const hours = state.data.hourly;
  if (!hours.length) {
    target.innerHTML = empty("No packets in this range.");
    return;
  }
  const W = chartWidth(target, 420), H = 260, L = 34, R = 10, T = 10, B = 26;
  const maxCount = Math.max(...hours.map((h) => h.ok + h.fail));
  const band = (W - L - R) / hours.length;
  const y = linear(0, maxCount, H - B, T);
  const every = Math.ceil(hours.length / 8);
  let body = yAxis(ticks(0, maxCount, Math.min(4, maxCount)), y, L, W - R);
  body += hours.map((h, i) => {
    const x0 = L + i * band + 1, w = Math.max(1, band - 2);
    const label = i % every === 0 ? `<text x="${x0 + w / 2}" y="${H - B + 14}" text-anchor="middle">${esc(new Date(ms(h.hour + ":00:00Z")).toLocaleTimeString([], { hour: "2-digit" }))}</text>` : "";
    return `<rect class="bar" x="${x0}" y="${y(h.ok)}" width="${w}" height="${H - B - y(h.ok)}"><title>${esc(h.hour)}:00 UTC · ${h.ok} ok · ${h.fail} CRC fail</title></rect>` +
      `<rect class="bar-fail" x="${x0}" y="${y(h.ok + h.fail)}" width="${w}" height="${y(h.ok) - y(h.ok + h.fail)}"/>` + label;
  }).join("");
  target.innerHTML = svg(W, H, body);
}

function renderFrequencyError() {
  const target = $("#chart-ferr");
  const byStation = {};
  goodPackets().forEach((p) => {
    if (p.src && p.freq_err_hz !== null) (byStation[p.src] = byStation[p.src] || []).push(p);
  });
  const stations = state.data.stations.filter((s) => byStation[s.src]).sort((a, b) => b.packets - a.packets).slice(0, 12);
  if (!stations.length) {
    target.innerHTML = empty("No good packets in this range.");
    return;
  }
  const all = stations.flatMap((s) => byStation[s.src].map((p) => p.freq_err_hz));
  const pad = Math.max(50, (Math.max(...all) - Math.min(...all)) * 0.1);
  const lo = Math.min(...all) - pad, hi = Math.max(...all) + pad;
  const W = chartWidth(target, 420), L = 90, R = 12, T = 8, row = 20, B = 26;
  const H = T + row * stations.length + B;
  const x = linear(lo, hi, L, W - R);
  let body = xAxis(ticks(lo, hi, 4), x, T, H - B, (v) => `${signed(v)} Hz`);
  body += stations.map((s, i) => {
    const cy = T + row * i + row / 2;
    let line = `<text class="label" x="${L - 6}" y="${cy + 4}" text-anchor="end">${esc(s.src)}</text>`;
    if (s.ferr_sd !== null) {
      line += `<line class="whisker" x1="${x(s.ferr_mean - s.ferr_sd)}" x2="${x(s.ferr_mean + s.ferr_sd)}" y1="${cy}" y2="${cy}"/>`;
    }
    line += `<line class="whisker" x1="${x(s.ferr_mean)}" x2="${x(s.ferr_mean)}" y1="${cy - 6}" y2="${cy + 6}"><title>${esc(s.src)}: mean ${signed(s.ferr_mean)} Hz (${signed(s.ppm_mean, 2)} ppm), sd ${fmt(s.ferr_sd, 0)} Hz</title></line>`;
    line += byStation[s.src].map((p) => `<circle data-packet="${p.id}" class="hollow" cx="${x(p.freq_err_hz)}" cy="${cy}" r="2.5"><title>${signed(p.freq_err_hz)} Hz</title></circle>`).join("");
    return line;
  }).join("");
  target.innerHTML = svg(W, H, body);
}

const STATION_COLUMNS = [
  { key: "src", label: "Station" },
  { key: "packets", label: "Pkts", num: true },
  { key: "crc_fail", label: "CRC fail", num: true },
  { key: "last_utc", label: "Last heard", show: (s) => age(s.last_utc) },
  { key: "rssi_mean", label: "RSSI dBm", num: true, show: (s) => `${fmt(s.rssi_mean)} <span class="hint">${fmt(s.rssi_min, 0)}…${fmt(s.rssi_max, 0)}</span>` },
  { key: "snr_mean", label: "SNR dB", num: true, show: (s) => `${fmt(s.snr_mean)} <span class="hint">${fmt(s.snr_min, 0)}…${fmt(s.snr_max, 0)}</span>` },
  { key: "margin_mean", label: "Margin dB", num: true, show: (s) => fmt(s.margin_mean) },
  { key: "ferr_mean", label: "Freq err Hz", num: true, show: (s) => `${signed(s.ferr_mean)}${s.ferr_sd !== null ? ` <span class="hint">± ${fmt(s.ferr_sd, 0)}</span>` : ""}` },
  { key: "ppm_mean", label: "ppm", num: true, show: (s) => signed(s.ppm_mean, 2) },
  { key: "distance_km", label: "Km", num: true, show: (s) => fmt(s.distance_km) },
  { key: "bearing_deg", label: "Bearing", num: true, show: (s) => (s.bearing_deg === null ? "–" : `${fmt(s.bearing_deg, 0)}°`) },
  { key: "direct", label: "Heard", show: (s) => esc([s.direct ? `direct ${s.direct}` : ""].concat(Object.entries(s.via).map(([d, n]) => `via ${d} ${n}`)).filter(Boolean).join(", ") || "–") },
  { key: "aprs_format", label: "Format", show: (s) => esc(s.aprs_format || "–") },
  { key: "comment", label: "Comment", wrap: true, show: (s) => esc(s.comment || "") },
];

function renderStations() {
  const stations = state.data.stations.slice().sort((a, b) => {
    const va = a[state.sortKey], vb = b[state.sortKey];
    if (va === vb) return 0;
    if (va === null || va === undefined) return 1;
    if (vb === null || vb === undefined) return -1;
    return (va < vb ? -1 : 1) * state.sortDir;
  });
  const head = STATION_COLUMNS.map((c) =>
    `<th data-sort="${c.key}" class="${c.num ? "num" : ""}">${esc(c.label)}${state.sortKey === c.key ? (state.sortDir > 0 ? " ▲" : " ▼") : ""}</th>`).join("");
  const rows = stations.map((s) => `<tr>${STATION_COLUMNS.map((c) =>
    `<td class="${c.num ? "num" : ""}${c.wrap ? " wrap" : ""}">${c.show ? c.show(s) : esc(s[c.key])}</td>`).join("")}</tr>`).join("");
  $("#stations").innerHTML = `<thead><tr>${head}</tr></thead><tbody>${rows || `<tr><td colspan="${STATION_COLUMNS.length}">No stations in this range.</td></tr>`}</tbody>`;
}

function renderPackets() {
  const head = ["Time", "#", "Station", "Heard", "Format", "RSSI", "SNR", "Margin", "Freq err", "CRC", "CR", "Len", "ToA ms", "Km", "Bearing", "Receiver", "Info"];
  const numeric = new Set(["#", "RSSI", "SNR", "Margin", "Freq err", "Len", "ToA ms", "Km", "Bearing"]);
  const rows = state.data.packets.slice(0, 200).map((p) => `<tr data-packet="${p.id}" class="${p.crc_ok ? "" : "crc-fail"}">` + [
    clock(p.host_utc), p.seq, p.src || "?", p.heard_via, p.aprs_format || p.decode_error || "–",
    fmt(p.rssi_dbm), fmt(p.snr_db, 2), fmt(p.link_margin_db), signed(p.freq_err_hz), p.crc_ok ? "ok" : "fail",
    p.rx_cr ? `4/${p.rx_cr}` : "–", p.len, fmt(p.toa_us / 1000, 0), fmt(p.distance_km), p.bearing_deg === null ? "–" : `${fmt(p.bearing_deg, 0)}°`,
    p.rx_source || "–",
  ].map((v, i) => `<td class="${numeric.has(head[i]) ? "num" : ""}">${esc(v)}</td>`).join("") +
    `<td class="wrap">${esc((p.info || "").slice(0, 80))}</td></tr>`).join("");
  $("#packets").innerHTML = `<thead><tr>${head.map((h) => `<th class="${numeric.has(h) ? "num" : ""}">${h}</th>`).join("")}</tr></thead>` +
    `<tbody>${rows || `<tr><td colspan="${head.length}">No packets in this range. The board logs noise while it waits.</td></tr>`}</tbody>`;
}

function renderEvents() {
  const { boots, errors } = state.data.events;
  const lines = boots.map((b) => ({ t: b.host_utc, html: `${esc(dateTime(b.host_utc))} · boot · fw ${esc(b.fw)} · radio status ${esc(b.radio_status)} · reset reason ${esc(b.reset_reason)} · SF${esc(b.sf)} ${fmt(b.freq_hz / 1e6, 3)} MHz` }))
    .concat(errors.map((e) => ({ t: e.host_utc, html: `<span class="error">${esc(dateTime(e.host_utc))} · error · ${esc(e.step)} returned ${esc(e.code)}</span>` })))
    .sort((a, b) => (a.t < b.t ? 1 : -1));
  $("#events").innerHTML = lines.map((l) => `<div>${l.html}</div>`).join("") || empty("No boots or errors in this range.");
}

// ---- Packet detail ----

function kv(pairs) {
  return `<dl class="kv">${pairs.filter(([, v]) => v !== undefined).map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v}</dd>`).join("")}</dl>`;
}

async function openPacket(id) {
  let p;
  try {
    p = await api("packet", { id });
  } catch (error) {
    showStatus(`Cannot load packet ${id}: ${error.message}`);
    return;
  }
  const floor = DEMODULATOR_FLOOR_DB[p.sf] ?? -20;
  const noise = p.noise_near;
  const weather = p.weather ? Object.entries(p.weather).map(([k, v]) => [k.replace(/_/g, " "), `${esc(v)} ${esc(WEATHER_UNITS[k] || "")}`]) : [];

  $("#detail-title").textContent = `${p.src || "?"} · #${p.seq}`;
  $("#detail-body").innerHTML =
    `<h3>Signal</h3>` + kv([
      ["RSSI", `${fmt(p.rssi_dbm)} dBm`],
      ["SNR", `${fmt(p.snr_db, 2)} dB`],
      ["Link margin", `${fmt(p.link_margin_db)} dB above the SF${esc(p.sf)} limit (${floor} dB)`],
      ["Frequency error", `${signed(p.freq_err_hz)} Hz (${signed(p.freq_err_ppm, 3)} ppm)`],
      ["CRC", p.crc_ok ? "ok" : "<b>failed</b>"],
      ["Coding rate", p.rx_cr ? `4/${esc(p.rx_cr)} (from the received header)` : "–"],
      ["Length", `${esc(p.len)} bytes`],
      ["Time on air", `${fmt(p.toa_us / 1000, 1)} ms`],
      ["Heard", p.heard_via === "direct" ? "direct" : `via digipeater ${esc(p.heard_via)}: RSSI and SNR belong to that digipeater`],
    ]) +
    `<h3>Radio registers</h3>` + kv([
      ["IRQ flags", `0x${Number(p.irq ?? 0).toString(16).padStart(2, "0")} · ${esc(p.irq_flags.join(", ") || "none")}`],
      ["Modem status", `0x${Number(p.modem_stat ?? 0).toString(16).padStart(2, "0")} · ${esc(p.modem.flags.join(", ") || "none")}`],
      ["Noise near packet", noise ? `mean ${fmt(noise.mean_dbm)} dBm · min ${fmt(noise.min_dbm, 0)}${noise.min_is_glitch ? " (glitch)" : ""} · max ${fmt(noise.max_dbm, 0)} · ${signed(noise.seconds_from_packet, 0)} s from the packet` : "–"],
      ["RSSI minus noise", noise ? `${signed(p.rssi_dbm - noise.mean_dbm, 1)} dB${p.rssi_dbm < noise.mean_dbm ? " (below the noise: LoRa can decode there)" : ""}` : "–"],
    ]) +
    `<h3>APRS</h3>` + kv([
      ["Source", esc(p.src || "?")],
      ["Destination", esc(p.dst || "–")],
      ["Path", esc(p.path || "–")],
      ["Format", esc(p.aprs_format || "–")],
      ["Position", osmLink(p.latitude, p.longitude)],
      ["Distance", p.distance_km === null ? "–" : `${fmt(p.distance_km, 2)} km at ${fmt(p.bearing_deg, 0)}°`],
      ["Altitude", p.altitude_m === null ? "–" : `${fmt(p.altitude_m, 0)} m`],
      ["Speed / course", p.speed_kmh === null ? "–" : `${fmt(p.speed_kmh)} km/h · ${fmt(p.course_deg, 0)}°`],
      ["Symbol", esc(`${p.symbol_table || ""}${p.symbol || ""}` || "–")],
      ["Comment", esc(p.comment || "–")],
      ["Decode error", p.decode_error ? esc(p.decode_error) : undefined],
    ]) +
    (weather.length ? `<h3>Weather</h3>` + kv(weather) : "") +
    `<h3>Receiver</h3>` + kv([
      ["Position", osmLink(p.rx_lat, p.rx_lon)],
      ["Source", esc(p.rx_source || "none")],
      ["Altitude", p.rx_alt_m === null ? "–" : `${fmt(p.rx_alt_m, 0)} m`],
      ["Satellites / HDOP", p.rx_sats === null ? "–" : `${esc(p.rx_sats)} · ${fmt(p.rx_hdop)}`],
      ["Fix time", esc(p.rx_fix_utc || "–")],
    ]) +
    `<h3>Time</h3>` + kv([
      ["Computer (host_utc)", esc(p.host_utc)],
      ["GPS (gps_utc)", esc(p.gps_utc || "–")],
      ["GPS minus computer", p.gps_minus_host_s === null ? "–" : `${signed(p.gps_minus_host_s, 3)} s`],
      ["Board uptime", `${fmt(p.uptime_ms / 1000, 1)} s`],
      ["Firmware", p.boot ? `${esc(p.boot.fw)} · boot at ${esc(dateTime(p.boot.host_utc))}` : "–"],
    ]) +
    `<h3>Info field</h3><div class="hexdump">${esc(p.info || "–")}</div>` +
    `<h3>Raw bytes</h3><div class="hexdump">${esc(p.hexdump.join("\n"))}</div>`;
  $("#detail").hidden = false;
}

function closeDetail() {
  $("#detail").hidden = true;
}

// ---- Events ----

document.addEventListener("click", (event) => {
  const range = event.target.closest("#range button");
  if (range) {
    state.hours = Number(range.dataset.hours);
    document.querySelectorAll("#range button").forEach((b) => b.classList.toggle("on", b === range));
    refresh();
    return;
  }
  const sort = event.target.closest("#stations th[data-sort]");
  if (sort) {
    const key = sort.dataset.sort;
    state.sortDir = state.sortKey === key ? -state.sortDir : -1;
    state.sortKey = key;
    renderStations();
    return;
  }
  const packet = event.target.closest("[data-packet]");
  if (packet) openPacket(packet.dataset.packet);
});

$("#detail-close").addEventListener("click", closeDetail);
document.addEventListener("keydown", (event) => { if (event.key === "Escape") closeDetail(); });

refresh();
