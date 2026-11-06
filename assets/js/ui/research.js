/* Live research panel: fetches forecasts when Yahoo candle history refreshes. */
import { api } from "../data/http.js";
import { $, pxs } from "../chart/helper.js";
import { isY, S } from "../store/state.js";

let lastKey = "";
let lastRequest = 0;
let pending = false;

const el = (id) => document.getElementById(id);
const price = (value) => Number.isFinite(Number(value))
  ? Number(value).toLocaleString(undefined, { maximumFractionDigits: 4 })
  : "—";
const pct = (value) => Number.isFinite(Number(value))
  ? `${Number(value) > 0 ? "+" : ""}${Number(value).toFixed(3)}%`
  : "—";

function status(text, kind = "wait") {
  const node = el("rq-model-status");
  if (!node) return;
  node.textContent = text;
  node.dataset.state = kind;
}

function pathFor(fan, highKey, lowKey, width = 600, height = 94, pad = 8) {
  if (!fan?.length) return "";
  const values = fan.flatMap((point) => [point[highKey], point[lowKey]]).filter(Number.isFinite);
  if (!values.length) return "";
  let low = Math.min(...values), high = Math.max(...values);
  if (high === low) { high += 1; low -= 1; }
  const y = (value) => pad + (high - value) / (high - low) * (height - pad * 2);
  const x = (index) => pad + index * (width - pad * 2) / Math.max(1, fan.length - 1);
  const upper = fan.map((p, i) => `${x(i)},${y(p[highKey])}`);
  const lower = fan.map((p, i) => `${x(i)},${y(p[lowKey])}`).reverse();
  return `M ${upper.join(" L ")} L ${lower.join(" L ")} Z`;
}

function lineFor(fan, key, width = 600, height = 94, pad = 8) {
  if (!fan?.length) return "";
  const values = fan.flatMap((point) => [point.p05, point.p95]).filter(Number.isFinite);
  if (!values.length) return "";
  let low = Math.min(...values), high = Math.max(...values);
  if (high === low) { high += 1; low -= 1; }
  const y = (value) => pad + (high - value) / (high - low) * (height - pad * 2);
  const x = (index) => pad + index * (width - pad * 2) / Math.max(1, fan.length - 1);
  return fan.map((p, i) => `${i ? "L" : "M"} ${x(i)} ${y(p[key])}`).join(" ");
}

function drawFan(fan) {
  const outer = el("rq-fan-outer");
  const inner = el("rq-fan-inner");
  const median = el("rq-fan-median");
  if (!outer || !inner || !median) return;
  outer.setAttribute("d", pathFor(fan, "p95", "p05"));
  inner.setAttribute("d", pathFor(fan, "p75", "p25"));
  median.setAttribute("d", lineFor(fan, "p50"));
}

function paint(result, stale) {
  el("rq-last-price").textContent = price(result.last_price);
  el("rq-ridge").textContent = pct(result.next_bar?.ridge_return_pct);
  el("rq-sgd").textContent = pct(result.next_bar?.sgd_return_pct);
  el("rq-ridge-price").textContent = price(result.next_bar?.ridge_price);
  el("rq-sgd-price").textContent = price(result.next_bar?.sgd_price);
  el("rq-rmse").textContent = result.validation?.ridge?.rmse_pct == null ? "—" : `${result.validation.ridge.rmse_pct.toFixed(4)}%`;
  el("rq-baseline-rmse").textContent = result.validation?.zero_return?.rmse_pct == null ? "—" : `${result.validation.zero_return.rmse_pct.toFixed(4)}%`;
  el("rq-validation").textContent = `${result.validation_rows} holdout rows · ${result.bar_count} bars`;
  el("rq-disclaimer").textContent = result.disclaimer || "Research estimate only.";
  drawFan(result.simulation?.fan || []);
  status(stale ? "READY · SOURCE DATA STALE" : "LIVE DATA · MODEL READY", stale ? "wait" : "live");
  lastKey = `${S.sel}|${S.gran}`;
}

export async function refreshResearch(force = false) {
  const key = `${S.sel}|${S.gran}`;
  if (!el("research-panel") || pending) return;
  if (!isY(S.sel)) {
    status("DISABLED · CRYPTO MODEL RESEARCH NEEDS PROVIDER PERMISSION", "wait");
    el("rq-disclaimer").textContent = "Coinbase terms in this repository prohibit using its data to build or test models without written permission. No Coinbase candles are sent to the model engine.";
    el("rq-validation").textContent = "Select a Yahoo Finance market symbol to run research.";
    return;
  }
  if (!force && key === lastKey && Date.now() - lastRequest < 60000) return;
  pending = true;
  lastRequest = Date.now();
  status("READING MARKET DATA · RUNNING MODELS", "wait");
  try {
    const params = new URLSearchParams({ symbol: S.sel, interval: ["1M","5M","15M","1H","4H","1D","1W","1MO"].find((name) => {
      const seconds = { "1M":60, "5M":300, "15M":900, "1H":3600, "4H":14400, "1D":86400, "1W":604800, "1MO":2592000 }[name];
      return seconds === S.gran;
    }) || "1D" });
    const result = await api(`/api/research/forecast?${params.toString()}`);
    if (`${S.sel}|${S.gran}` !== key) return;
    paint(result, result.data_stale === true);
  } catch (error) {
    lastKey = "";
    status("MODEL UNAVAILABLE", "down");
    el("rq-validation").textContent = error.message || "Forecast request failed.";
  } finally {
    pending = false;
  }
}

export function initResearch() {
  const button = el("rq-refresh");
  if (button) button.addEventListener("click", () => refreshResearch(true));
  refreshResearch(true);
}
