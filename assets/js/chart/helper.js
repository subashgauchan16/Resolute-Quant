/* Formatting and small DOM utilities shared by the UI and chart modules. */
export const $ = (id) => document.getElementById(id);

export const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

export const dp = (p) => (p >= 1000 ? 2 : p >= 100 ? 2 : p >= 1 ? 4 : p >= 0.01 ? 5 : 7);
export const num = (v, d) => (v == null || !isFinite(v) ? "-" : v.toLocaleString("en-GB", { minimumFractionDigits: d, maximumFractionDigits: d }));
export const px = (v) => (v == null ? "-" : num(v, dp(Math.abs(v))));
export const sz = (v) => (v >= 1000 ? num(v, 0) : v >= 10 ? num(v, 2) : v >= 0.01 ? num(v, 4) : num(v, 6));
export const big = (v) => (v == null ? "-" : v >= 1e12 ? num(v / 1e12, 2) + "T" : v >= 1e9 ? num(v / 1e9, 2) + "B" : v >= 1e6 ? num(v / 1e6, 2) + "M" : v >= 1e3 ? num(v / 1e3, 1) + "K" : num(v, 2));
export const ccySymbol = (c) => ({ USD: "$", GBP: "£", EUR: "€" }[c] || "");
export const hms = (ms) => new Date(ms).toLocaleTimeString("en-GB");
export const unit = (c) => (c === "GBp" ? ["", "p"] : [ccySymbol(c), ""]);
export const esc = (t) => String(t ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

export const STATE = { REGULAR: "OPEN", PRE: "PRE-MARKET", PREPRE: "CLOSED", POST: "AFTER HOURS", POSTPOST: "CLOSED", CLOSED: "CLOSED" };

export const pct = (q) => (q.last != null && q.open ? (q.last / q.open - 1) * 100 : null);
export const cls = (v) => (v == null || v === 0 ? "flat" : v > 0 ? "up" : "down");
export const pctTxt = (v) => (v == null ? "-" : (v > 0 ? "▲ " : v < 0 ? "▼ " : "") + num(Math.abs(v), 2) + "%");
