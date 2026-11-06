/* Profile and news panels for market symbols. */
import { $, big, esc, num } from "../chart/helper.js";
import { fetchNews, fetchProfile } from "../data/markets.js";
import { isY, label, S } from "../store/state.js";

const kv = (k, v) => (v == null || v === "" ? "" : `<div class="kv"><span>${k}</span><b>${esc(v)}</b></div>`);

function ago(t) {
  const m = (Date.now() - Date.parse(t)) / 6e4;
  if (!isFinite(m)) return "";
  if (m < 60) return Math.max(1, Math.round(m)) + "m ago";
  return m < 1440 ? Math.round(m / 60) + "h ago" : Math.round(m / 1440) + "d ago";
}

async function loadProfile(id) {
  try {
    const p = await fetchProfile(id);
    if (id !== S.sel) return;
    S.prof = p;
    const pc = (v) => (v == null ? null : num(v, 2) + "%");
    $("co").innerHTML = `<h3>${esc(p.name || label(id))}</h3><p>${esc(id)}${p.exchange ? " · " + esc(p.exchange) : ""}</p>`
      + kv("Sector", p.sector) + kv("Industry", p.industry) + kv("Country", p.country)
      + kv("Forward P/E", p.forwardPe == null ? null : num(p.forwardPe, 1))
      + kv("EPS", p.eps == null ? null : num(p.eps, 2))
      + kv("Dividend yield", pc(p.dividend))
      + kv("Beta", p.beta == null ? null : num(p.beta, 2))
      + kv("Average volume", p.avgVolume == null ? null : big(p.avgVolume))
      + (p.stale ? `<p>Showing the last saved profile.</p>` : "");
  } catch (e) {
    if (id === S.sel) $("co").innerHTML = `<p>${esc(e.message)}</p>`;
  }
}

async function loadNews(id) {
  try {
    const n = await fetchNews(id);
    if (id !== S.sel) return;
    $("newssrc").textContent = n.stale ? "SAVED COPY" : "YAHOO FINANCE";
    $("news").innerHTML = n.items.length
      ? n.items.map((a) => `<a href="${esc(a.url)}" target="_blank" rel="noopener noreferrer">${esc(a.title)}<small>${esc(a.publisher)} · ${ago(a.time)}</small></a>`).join("")
      : `<div class="empty">No recent stories for ${esc(label(id))}.</div>`;
  } catch (e) {
    if (id === S.sel) $("news").innerHTML = `<div class="empty">${esc(e.message)}</div>`;
  }
}

export async function loadDetails() {
  const id = S.sel;
  if (!isY(id)) return;
  $("co").innerHTML = `<p>Loading ${esc(label(id))}.</p>`;
  $("news").innerHTML = "";
  await loadProfile(id);
  if (id === S.sel) await loadNews(id);
}
