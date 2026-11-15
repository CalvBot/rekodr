// Shared helpers for the clipback pages.

async function api(path, opts = {}) {
  const init = { method: opts.method || (opts.body !== undefined ? "POST" : "GET") };
  if (opts.body !== undefined) {
    init.body = typeof opts.body === "string" ? opts.body : JSON.stringify(opts.body);
    init.headers = { "Content-Type": "application/json" };
  }
  const r = await fetch(path, init);
  const data = await r.json().catch(() => ({}));
  if (!r.ok || data.error) throw new Error(data.error || r.statusText);
  return data;
}

const enc = encodeURIComponent;

function fmtTime(s, frac = false) {
  if (!isFinite(s)) s = 0;
  const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60;
  const sec = frac ? (s % 60).toFixed(1).padStart(4, "0") : String(Math.floor(s % 60)).padStart(2, "0");
  return (h ? h + ":" + String(m).padStart(2, "0") : m) + ":" + sec;
}

function fmtSize(b) {
  if (b > 1e9) return (b / 1e9).toFixed(1) + " GB";
  if (b > 1e6) return Math.round(b / 1e6) + " MB";
  return Math.round(b / 1e3) + " KB";
}

function ago(ms) {
  const s = (Date.now() - ms) / 1000;
  if (s < 60) return "just now";
  if (s < 3600) return Math.floor(s / 60) + " min ago";
  const d = new Date(ms);
  const t = d.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
  if (s < 86400 && d.getDate() === new Date().getDate()) return "today " + t;
  return d.toLocaleDateString([], { day: "numeric", month: "short" }) + " " + t;
}

let toastTimer;
function toast(msg, err = false) {
  let el = document.getElementById("toast");
  if (!el) {
    el = document.createElement("div");
    el.id = "toast";
    document.body.append(el);
  }
  el.textContent = msg;
  el.className = "show" + (err ? " err" : "");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (el.className = ""), err ? 5000 : 2200);
}

// Every page title ends with this; clipback finds its own window by it (see server.rs).
const TITLE_END = " · clipback";

// An icon from icons.js (Lucide), drawn in the text colour.
function icon(name) {
  const t = document.createElement("template");
  t.innerHTML = `<svg class="i" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name] || ""}</svg>`;
  return t.content.firstChild;
}
// Sets a button's icon and text together.
function setLabel(el, name, text) {
  el.replaceChildren(icon(name), ...(text ? [text] : []));
}
// <button data-icon="settings">Options</button> → icon before the text.
document.addEventListener("DOMContentLoaded", () => {
  for (const el of document.querySelectorAll("[data-icon]")) el.prepend(icon(el.dataset.icon));
});

function h(tag, attrs = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "icon") el.append(icon(v));
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k === "class") el.className = v;
    else if (k === "style") el.style.cssText = v;
    else if (v !== false && v != null) el.setAttribute(k, v === true ? "" : v);
  }
  for (const kid of kids.flat()) if (kid != null) el.append(kid);
  return el;
}

// Recording status in the header: dot, text and the pause button.
async function statusBar(onUpdate) {
  const dot = document.getElementById("dot"), text = document.getElementById("status"), btn = document.getElementById("pause");
  async function tick() {
    try {
      const s = await api("/api/status");
      dot.className = "dot" + (!s.paused && s.live ? " rec" : "");
      const apps = s.audio.filter(a => a.exe !== "mic").map(a => a.exe.replace(/\.exe$/, ""));
      const span = s.oldest_us ? fmtTime((s.now_us - s.oldest_us) / 1e6) : "0:00";
      text.textContent = s.paused ? "Paused" : !s.live ? "Waiting for the screen…" :
        `Recording · ${span} buffered` + (s.encode && s.encode[1] ? ` · ${s.encode[0]} Mbps for ${s.encode[1].replace(/\.exe$/, "")}` : "") +
        (apps.length ? " · " + apps.join(", ") : "");
      text.title = `${fmtSize(s.ring_bytes)} of ${fmtSize(s.ring_cap)}`;
      if (btn) setLabel(btn, s.paused ? "play" : "pause", s.paused ? "Resume" : "Pause");
      if (btn) btn.dataset.paused = s.paused;
      const badge = document.getElementById("newApps");
      if (badge) {
        badge.hidden = !s.new_apps;
        badge.textContent = s.new_apps;
        badge.title = `${s.new_apps} new app${s.new_apps === 1 ? "" : "s"} to sort in Options`;
      }
      onUpdate && onUpdate(s);
    } catch {
      dot.className = "dot";
      text.textContent = "clipback isn't running";
    }
  }
  if (btn) btn.onclick = async () => { await api(btn.dataset.paused === "true" ? "/api/resume" : "/api/pause", { method: "POST" }); tick(); };
  tick();
  setInterval(() => document.visibilityState === "visible" && tick(), 2000);
}

// Same easing as the exported video: smoothstep between keyframes.
function keyValue(keys, t, field) {
  if (!keys.length) return field === "z" ? 1 : 0.5;
  if (t <= keys[0].t) return keys[0][field];
  for (let i = 0; i < keys.length - 1; i++) {
    const a = keys[i], b = keys[i + 1];
    if (t < b.t) {
      const u = Math.min(1, Math.max(0, (t - a.t) / Math.max(1e-3, b.t - a.t)));
      return a[field] + (b[field] - a[field]) * u * u * (3 - 2 * u);
    }
  }
  return keys[keys.length - 1][field];
}
