/* Top-bar connection badges, clock and message-rate meter. */
import { $, hms } from "../chart/helper.js";
import { api } from "../data/http.js";
import { S } from "../store/state.js";

/* Returns true while the crypto feed is stale, so other panels can dim themselves. */
export function paintStatus(now) {
  const age = now - S.lastMsg;
  const open = S.y.up && S.feedUp === true;
  const stale = !(S.live && open);
  $("app").classList.toggle("stale", stale && S.everLive === true);

  const yBadge = $("ystatus");
  const yText = $("ystatustext");
  $("app").classList.toggle("ystale", !S.y.up);
  if (!S.y.up) {
    yBadge.dataset.s = "down";
    yText.textContent = "STOCKS OFF";
  } else if (S.y.limited) {
    yBadge.dataset.s = "wait";
    yText.textContent = "YAHOO PAUSED";
  } else if (S.y.stream) {
    yBadge.dataset.s = "live";
    yText.textContent = "YAHOO STREAM";
  } else {
    yBadge.dataset.s = "wait";
    yText.textContent = "YAHOO 90S";
  }

  const badge = $("status");
  const label = $("statustext");
  if (!stale) {
    badge.dataset.s = "live";
    label.textContent = "LIVE";
  } else if (!S.y.up) {
    badge.dataset.s = "down";
    label.textContent = "CRYPTO OFF";
  } else if (S.retryAt > now) {
    badge.dataset.s = "down";
    label.textContent = `RECONNECTING ${Math.ceil((S.retryAt - now) / 1000)}S`;
  } else if (open) {
    badge.dataset.s = "wait";
    label.textContent = `NO DATA ${Math.floor(age / 1000)}S`;
  } else {
    badge.dataset.s = "wait";
    label.textContent = "CONNECTING";
  }

  const zone = new Intl.DateTimeFormat("en-GB", { timeZoneName: "short" }).formatToParts(now).find((x) => x.type === "timeZoneName").value;
  $("clock").textContent = hms(now) + " " + zone;
  return stale;
}

/* Say so on the page when the data program had to use a port other than the preferred one. */
export async function paintPortNotice() {
  try {
    const health = await api("/api/health");
    if (health.fallback) $("portnote").textContent = `Port ${health.port}: ${health.preferredPort} was busy. Saved settings are per port. `;
  } catch { /* the offline note already covers this */ }
}

export function paintMeter() {
  S.rate = S.count;
  S.count = 0;
  $("rate").textContent = S.rate;
  $("lag").textContent = S.lag == null ? "-" : `${Math.max(0, Math.round(S.lag))} ms`;
}
