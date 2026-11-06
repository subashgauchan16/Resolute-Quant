/* The main price panel: candles, volume, EMA and alert lines, kept current from live trades. */
import { createTerminalChart } from "./chart-engine.js";
import { $, big, css } from "./helper.js";
import { emaSeries, emaStep } from "./indicators.js";
import { dpFor, grans, isY, pxs, S } from "../store/state.js";
import * as crypto from "../data/crypto.js";
import * as markets from "../data/markets.js";

const REFRESH_MS = 60000;
const RETRY_MS = 15000;

export const C = { token: 0, loading: true, last: null, pending: [], fetchedAt: 0, emaPrev: null, emaCur: null, lines: [], nextRefresh: 0, dirty: false, flushes: 0 };
let sCandle;
let sEma;
let sVol;
let amber;

const volBar = (c) => ({ time: c.time, value: c.volume, color: c.close >= c.open ? "rgba(43,217,138,.38)" : "rgba(255,90,82,.38)" });

export function initPriceChart() {
  const up = css("--up");
  const down = css("--down");
  amber = css("--amber");
  const chart = createTerminalChart($("chart"));
  sCandle = chart.addCandlestickSeries({ upColor: up, downColor: down, borderVisible: false, wickUpColor: up, wickDownColor: down, priceLineColor: amber });
  sEma = chart.addLineSeries({ color: amber, lineWidth: 1, priceLineVisible: false, lastValueVisible: false, crosshairMarkerVisible: false });
  sVol = chart.addHistogramSeries({ priceFormat: { type: "volume" }, priceScaleId: "", lastValueVisible: false, priceLineVisible: false });
  sVol.priceScale().applyOptions({ scaleMargins: { top: 0.84, bottom: 0 } });
  chart.subscribeCrosshairMove((e) => {
    const d = e.seriesData && e.seriesData.get(sCandle);
    S.hover = !!d;
    if (d) {
      const v = e.seriesData.get(sVol);
      legend({ ...d, volume: v ? v.value : 0 });
    } else {
      legend();
    }
  });
}

async function fetchBars(p) {
  if (!isY(p)) return crypto.fetchCandles(p, S.gran);
  const name = grans().find((g) => g[1] === S.gran)[0];
  const bars = await markets.fetchCandles(p, name);
  C.nextRefresh = Date.now() + REFRESH_MS;
  return bars;
}

export async function loadChart() {
  const token = ++C.token;
  const p = S.sel;
  C.loading = true;
  C.pending = [];
  $("chartmsg").hidden = true;
  try {
    const bars = await fetchBars(p);
    if (token !== C.token) return;
    if (!bars.length) throw new Error("empty");
    const d = dpFor(p, bars.at(-1).close);
    sCandle.applyOptions({ priceFormat: { type: "price", precision: d, minMove: 10 ** -d } });
    sCandle.setData(bars);
    sVol.setData(bars.map(volBar));
    const ema = emaSeries(bars);
    C.emaPrev = ema.prev;
    C.emaCur = ema.cur;
    sEma.setData(ema.points);
    C.last = bars.at(-1);
    C.dirty = false;
    C.fetchedAt = Date.now();
    C.loading = false;
    for (const tr of C.pending.reverse()) chartTrade(tr, tr.t <= C.fetchedAt);
    C.pending = [];
    drawLines();
    legend();
  } catch {
    if (token !== C.token) return;
    const m = $("chartmsg");
    m.textContent = "Chart history did not load. Retrying in 15 seconds.";
    m.hidden = false;
    setTimeout(() => { if (token === C.token) loadChart(); }, RETRY_MS);
  }
}

export function chartTrade(tr, skipVol) {
  if (C.loading) {
    C.pending.unshift(tr);
    return;
  }
  let c = C.last;
  if (!c) return;
  /* market bars start at the session open, so count from the last bar; daily and weekly bars come from Yahoo */
  const sec = tr.t / 1000;
  const y = isY(S.sel);
  const b = y
    ? (S.gran >= 86400 ? c.time : c.time + Math.floor((sec - c.time) / S.gran) * S.gran)
    : Math.floor(sec / S.gran) * S.gran;
  if (b < c.time) return;
  if (b > c.time) {
    flushChart(); /* the finished bar must show its last trades */
    C.emaPrev = C.emaCur;
    /* The newest history bar can be minutes old, so its close is not this bar's open: use the first trade seen. */
    c = C.last = { time: b, open: tr.p, high: tr.p, low: tr.p, close: tr.p, volume: 0 };
  }
  c.high = Math.max(c.high, tr.p);
  c.low = Math.min(c.low, tr.p);
  c.close = tr.p;
  if (!skipVol) c.volume += tr.s;
  C.emaCur = emaStep(c.close, C.emaPrev);
  C.dirty = true;
}

/* Trades only change the bar's numbers. The repaint loop draws them, which caps chart updates at 5 a second. */
export function flushChart() {
  if (!C.dirty || C.loading || !C.last) return;
  C.dirty = false;
  C.flushes++;
  const c = C.last;
  sCandle.update({ ...c });
  sVol.update(volBar(c));
  sEma.update({ time: c.time, value: C.emaCur });
  if (!S.hover) legend();
}

export function legend(bar) {
  const c = bar || C.last;
  if (!c) return;
  const k = (n, v) => `<span>${n} <b>${v}</b></span>`;
  $("legend").innerHTML = k("O", pxs(c.open)) + k("H", pxs(c.high)) + k("L", pxs(c.low)) + k("C", pxs(c.close)) + k("VOL", big(c.volume)) + `<span style="color:var(--amber)">EMA 20</span>`;
}

export function drawLines() {
  C.lines.forEach((l) => sCandle.removePriceLine(l));
  C.lines = S.alerts.filter((a) => a.p === S.sel).map((a) => sCandle.createPriceLine({ price: a.px, color: amber, lineWidth: 1, lineStyle: 2, axisLabelVisible: true, title: "ALERT" }));
}

/* Market candles do not stream their history, so reload them about once a minute. */
export function refreshIfDue(now) {
  if (C.nextRefresh && now > C.nextRefresh && !C.loading) {
    C.nextRefresh = now + REFRESH_MS;
    loadChart();
  }
}
