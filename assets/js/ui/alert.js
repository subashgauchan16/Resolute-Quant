/* Price alerts: saved per symbol, fired when the price crosses them. */
import { drawLines } from "../chart/ohlc.js";
import { $, hms, px } from "../chart/helper.js";
import { store } from "../store/localStorage.js";
import { MAX_WATCH, S } from "../store/state.js";

function saveAndDraw() {
  store.set("alerts", S.alerts);
  drawLines();
}

export function addAlert(p, price) {
  S.alerts.push({ p, px: price });
  saveAndDraw();
}

export function clearAlerts(p) {
  S.alerts = S.alerts.filter((a) => a.p !== p);
  saveAndDraw();
}

export function checkAlerts(p, before, after) {
  if (before == null || !S.alerts.length) return;
  const hit = S.alerts.filter((x) => x.p === p && ((before < x.px && after >= x.px) || (before > x.px && after <= x.px)));
  if (!hit.length) return;
  S.alerts = S.alerts.filter((x) => !hit.includes(x));
  saveAndDraw();
  $("alerttext").textContent = `${p} crossed ${px(hit[0].px)} at ${hms(Date.now())}. Last ${px(after)}.`;
  $("alertbar").hidden = false;
}

export function showWatchDropped(symbols) {
  $("alerttext").textContent = `These symbols are getting no data, because they are invalid or over the limit of ${MAX_WATCH}: ${symbols.join(", ")}.`;
  $("alertbar").hidden = false;
}

export function initAlerts() {
  $("alertclose").onclick = () => { $("alertbar").hidden = true; };
}
