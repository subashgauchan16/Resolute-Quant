/* The quote header and the session analytics row. */
import { $, big, ccySymbol, cls, hms, num, pct, pctTxt, px, STATE, unit } from "../chart/helper.js";
import { fetchFearGreed } from "../data/feargreed.js";
import { base, isY, label, pxs, Q, quoteCcy, S } from "../store/state.js";

const text = (id, value) => { $(id).textContent = value; };

export function paintQuote() {
  const y = isY(S.sel);
  const q = Q(S.sel);
  const c = pct(q);
  const [pre, suf] = y ? unit(q.cur) : [ccySymbol(quoteCcy(S.sel)), ""];
  text("bigpx", q.last == null ? "-" : pre + pxs(q.last) + suf);
  const change = $("bigchg");
  change.textContent = c == null ? "-" : `${pctTxt(c)} ${y ? "TODAY" : "24H"}`;
  change.className = cls(c);
  text("asof", y && q.asOf ? `${STATE[q.state] || ""}${q.delay > 0 ? `, Yahoo delay ${q.delay}m` : ""}, as of ${hms(q.asOf).slice(0, 8)}` : "");
  document.title = q.last == null ? "Resolute Quant" : `${label(S.sel)} ${pxs(q.last)} ${c >= 0 ? "▲" : "▼"} Resolute Quant`;
}

export function paintMarketStats() {
  const q = Q(S.sel);
  const p = S.prof || {};
  const bar = (id, lo, hi) => {
    if (lo != null && hi != null && q.last != null) $(id).style.left = Math.max(0, Math.min(100, (q.last - lo) / ((hi - lo) || 1) * 100)) + "%";
  };
  text("y-range", q.high == null ? "-" : `${pxs(q.low)} to ${pxs(q.high)}`);
  bar("y-rangepos", q.low, q.high);
  const yh = p.yearHigh ?? q.yearHigh;
  const yl = p.yearLow ?? q.yearLow;
  text("y-52", yh == null ? "-" : `${pxs(yl)} to ${pxs(yh)}`);
  bar("y-52pos", yl, yh);
  text("y-prev", pxs(q.open));
  text("y-prevd", q.open && q.last != null ? `change ${q.last >= q.open ? "+" : "-"}${pxs(Math.abs(q.last - q.open))}` : "-");
  text("y-vol", q.vol ? big(q.vol) : "-");
  text("y-avol", p.avgVolume ? `average ${big(p.avgVolume)}` : "not reported");
  text("y-cap", q.cap || p.cap ? big(q.cap || p.cap) : "-");
  text("y-cur", q.cur ? `quoted in ${q.cur}` : "-");
  text("y-pe", p.pe == null ? "-" : num(p.pe, 1));
  text("y-state", STATE[q.state] || "-");
  text("y-delay", q.delay > 0 ? `Yahoo lists a ${q.delay} min delay` : "no delay listed by Yahoo");
}

export function paintCryptoStats(now) {
  const q = Q(S.sel);
  if (q.high != null) {
    text("s-range", `${px(q.low)} to ${px(q.high)}`);
    $("s-rangepos").style.left = Math.max(0, Math.min(100, (q.last - q.low) / ((q.high - q.low) || 1) * 100)) + "%";
  }
  if (q.vol != null && q.last != null) {
    text("s-vol", ccySymbol(quoteCcy(S.sel)) + big(q.vol * q.last));
    text("s-volbase", `${big(q.vol)} ${base(S.sel)}`);
  }
  if (q.v) {
    const vwap = q.pv / q.v;
    const d = (q.last / vwap - 1) * 100;
    text("s-vwap", px(vwap));
    text("s-vwapd", `last ${d >= 0 ? "+" : ""}${num(d, 2)}% vs VWAP`);
  } else {
    text("s-vwap", "-");
    text("s-vwapd", "Waiting for the first trade");
  }
  const total = q.buy + q.sell;
  if (total) {
    const b = q.buy / total * 100;
    text("s-flow", `${num(b, 0)}% / ${num(100 - b, 0)}%`);
    $("s-flowbar").style.width = b + "%";
  } else {
    text("s-flow", "-");
    $("s-flowbar").style.width = "50%";
  }
  q.times = q.times.filter((t) => now - t < 60000);
  text("s-tpm", num(q.times.length, 0));
  text("s-count", `${num(q.n, 0)} trades since ${hms(q.since).slice(0, 5)}`);
}

export async function paintFearGreed() {
  try {
    const d = await fetchFearGreed();
    text("s-fng", d.value + " / 100");
    text("s-fngd", d.classification + (d.stale ? ", saved copy" : ", daily"));
  } catch {
    text("s-fngd", "Could not load. Retries hourly.");
  }
}
