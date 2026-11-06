/* Paints the order book and the time & sales tape. The book's data lives in data/orderbook.js. */
import { $, cls, dp, hms, num, px, sz } from "./helper.js";
import { B, bview } from "../data/orderbook.js";
import { BIG_TRADE_USD, LEVELS, Q, S } from "../store/state.js";

function bookRows(el, side, max) {
  const levels = bview(side, LEVELS);
  if (side === "sell") levels.reverse();
  while (levels.length < LEVELS) {
    if (side === "sell") levels.unshift(null);
    else levels.push(null);
  }
  el.innerHTML = levels.map((l) => (l
    ? `<div class="lv ${side === "sell" ? "ask" : "bid"}"><i style="width:${(l.c / max * 100).toFixed(1)}%"></i><span>${px(l.p)}</span><span>${sz(l.s)}</span><span>${sz(l.c)}</span></div>`
    : `<div class="lv"></div>`)).join("");
}

export function paintBook(stale) {
  if (!(B.ready && B.product === S.sel)) {
    $("booksrc").textContent = "LOADING";
    if ($("asks").childElementCount) {
      $("asks").innerHTML = "";
      $("bids").innerHTML = "";
    }
    return;
  }
  const a = bview("sell", LEVELS);
  const b = bview("buy", LEVELS);
  const max = Math.max(a.at(-1)?.c || 1, b.at(-1)?.c || 1);
  bookRows($("asks"), "sell", max);
  bookRows($("bids"), "buy", max);
  const ba = a[0]?.p;
  const bb = b[0]?.p;
  if (ba && bb) {
    const m = (ba + bb) / 2;
    $("spread").textContent = `${num(ba - bb, dp(m))} (${num((ba - bb) / m * 1e4, 2)} bp)`;
    $("mid").textContent = px(m);
  }
  const tb = b.at(-1)?.c || 0;
  const ta = a.at(-1)?.c || 0;
  const imb = (tb - ta) / ((tb + ta) || 1) * 100;
  const e = $("s-imb");
  e.textContent = `${imb >= 0 ? "+" : ""}${num(imb, 1)}%`;
  e.className = cls(imb);
  $("booksrc").textContent = stale ? "STALE" : "LIVE, DEPTH " + num(B.depth, 0);
}

export function paintTape() {
  if (!S.tapeDirty) return;
  S.tapeDirty = false;
  $("tape").innerHTML = Q(S.sel).tape.slice(0, 40).map((t) => {
    const large = t.p * t.s >= BIG_TRADE_USD;
    return `<div class="tr${large ? " big" : ""}"><span class="flat">${hms(t.t)}</span><span class="${t.side === "buy" ? "up" : "down"}">${px(t.p)}</span><span>${sz(t.s)}</span></div>`;
  }).join("");
}
