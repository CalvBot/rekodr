// Key art for the clipback keys: 144x144 SVGs, a dark card with a glow of the key's colour at the top and a
// coloured bar along the bottom. Stream Deck draws SVG with a basic renderer, so this sticks to shapes,
// gradients, text and transforms (no filters, masks or CSS).

export const C = {
  bg: "#121419", text: "#e8eaee", dim: "#6b7280",
  red: "#ff4b5c", amber: "#ffb938", green: "#4ade80", grey: "#9aa3b2",
};

const FONT = "Bahnschrift, Segoe UI, sans-serif";

export const esc = s => String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

export function text(x, y, s, size, fill, extra = "") {
  return `<text x="${x}" y="${y}" text-anchor="middle" font-family="${FONT}" font-size="${size}" fill="${fill}" ${extra}>${esc(s)}</text>`;
}

export function card({ accent = C.grey, glow = 0.3, body = "", label = "", bar = 1 }) {
  return `<svg xmlns="http://www.w3.org/2000/svg" width="144" height="144" viewBox="0 0 144 144">` +
    `<defs><radialGradient id="g" cx="72" cy="0" r="110" gradientUnits="userSpaceOnUse">` +
    `<stop offset="0" stop-color="${accent}" stop-opacity="${glow}"/><stop offset="1" stop-color="${accent}" stop-opacity="0"/>` +
    `</radialGradient></defs>` +
    `<rect width="144" height="144" fill="${C.bg}"/><rect width="144" height="144" fill="url(#g)"/>` +
    body +
    (label ? text(72, 118, label, label.length > 9 ? 15 : 18, C.text) : "") +
    `<rect y="138" width="144" height="6" fill="${accent}" fill-opacity="${bar}"/>` +
    `</svg>`;
}

// A clock inside an arrow that turns back: "the last so long".
export function history(color, cx = 72, cy = 26, r = 12) {
  const p = a => [cx + r * Math.cos(a), cy + r * Math.sin(a)].map(v => v.toFixed(1)).join(" ");
  const from = p(Math.PI * 1.15), to = p(Math.PI * 0.85); // an open circle with its gap on the left
  const [fx, fy] = from.split(" ").map(Number);
  return `<g fill="none" stroke="${color}" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round">` +
    `<path d="M ${from} A ${r} ${r} 0 1 1 ${to}"/>` +
    `<path d="M ${(fx - 4.5).toFixed(1)} ${(fy - 6.5).toFixed(1)} L ${fx} ${fy} L ${(fx + 7).toFixed(1)} ${(fy - 1.5).toFixed(1)}"/>` +
    `<path d="M ${cx} ${cy - r * 0.55} L ${cx} ${cy} L ${cx + r * 0.42} ${cy + r * 0.3}"/></g>`;
}

export function tick(color, cx = 72, cy = 52) {
  return `<path d="M ${cx - 20} ${cy + 1} L ${cx - 6} ${cy + 15} L ${cx + 21} ${cy - 14}" fill="none" stroke="${color}" ` +
    `stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>`;
}

export function cross(color, cx = 72, cy = 52) {
  return `<path d="M ${cx - 16} ${cy - 16} L ${cx + 16} ${cy + 16} M ${cx + 16} ${cy - 16} L ${cx - 16} ${cy + 16}" fill="none" ` +
    `stroke="${color}" stroke-width="8" stroke-linecap="round"/>`;
}

// A strip of film: the clips window.
export function film(color, cx = 72, cy = 52) {
  return `<g fill="none" stroke="${color}" stroke-width="4.5">` +
    `<rect x="${cx - 23}" y="${cy - 23}" width="46" height="46" rx="10"/>` +
    `<g stroke-width="4" stroke-linecap="round" stroke-dasharray="3.5 6.5">` +
    `<path d="M ${cx - 12} ${cy - 14} L ${cx - 12} ${cy + 16}"/><path d="M ${cx + 12} ${cy - 14} L ${cx + 12} ${cy + 16}"/></g></g>`;
}

// The timeline: a track with a playhead.
export function track(color, cx = 72, cy = 52) {
  return `<g fill="none" stroke="${color}" stroke-width="4.5" stroke-linecap="round">` +
    `<path d="M ${cx - 26} ${cy - 10} L ${cx + 26} ${cy - 10}"/><path d="M ${cx - 26} ${cy + 4} L ${cx + 8} ${cy + 4}"/>` +
    `<path d="M ${cx - 26} ${cy + 18} L ${cx + 18} ${cy + 18}"/><path d="M ${cx + 2} ${cy - 22} L ${cx + 2} ${cy + 26}" stroke="${C.text}"/></g>`;
}

// Options: three sliders.
export function sliders(color, cx = 72, cy = 52) {
  const row = (y, knob) => `<path d="M ${cx - 24} ${y} L ${cx + 24} ${y}"/><circle cx="${cx + knob}" cy="${y}" r="6" fill="${C.bg}"/>`;
  return `<g fill="none" stroke="${color}" stroke-width="4.5" stroke-linecap="round">${row(cy - 16, -8)}${row(cy, 10)}${row(cy + 16, -2)}</g>`;
}

// Recording: a filled dot in a ring. Paused: two bars. Off: an empty ring.
export function dot(color, cx = 72, cy = 52) {
  return `<circle cx="${cx}" cy="${cy}" r="26" fill="none" stroke="${color}" stroke-width="4" stroke-opacity="0.45"/>` +
    `<circle cx="${cx}" cy="${cy}" r="15" fill="${color}"/>`;
}
export function bars(color, cx = 72, cy = 52) {
  return `<g fill="${color}"><rect x="${cx - 17}" y="${cy - 22}" width="12" height="44" rx="3"/>` +
    `<rect x="${cx + 5}" y="${cy - 22}" width="12" height="44" rx="3"/></g>`;
}
export function ring(color, cx = 72, cy = 52) {
  return `<circle cx="${cx}" cy="${cy}" r="24" fill="none" stroke="${color}" stroke-width="4.5"/>`;
}

// The Save key: the history arrow, the length in big type, and a line under it.
export function saveKey(length, label, accent, glow = 0.3) {
  const size = length.length > 3 ? 44 : 56;
  return card({
    accent, glow, label,
    body: history(accent) + text(72, 84, length, size, "#ffffff", `font-weight="600"`),
  });
}
