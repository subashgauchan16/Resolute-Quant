/* Stocks, indices, FX and gold: everything from the local data program (server.py). No DOM. */
import { onCryptoMessage, syncCrypto } from "./crypto.js";
import { api } from "./http.js";
import { emit } from "../store/events.js";
import { isY, Q, S } from "../store/state.js";

const param = (symbol) => encodeURIComponent(symbol);

export const fetchProfile = (symbol) => api(`/api/profile?symbol=${param(symbol)}`);
export const fetchNews = (symbol) => api(`/api/news?symbol=${param(symbol)}`);
export const resolveSymbol = (symbol) => api(`/api/resolve?symbol=${param(symbol)}`);

export async function fetchCandles(symbol, interval) {
  return (await api(`/api/candles?symbol=${param(symbol)}&interval=${interval}`)).bars;
}

function onQuote(m) {
  if (!isY(m.id)) return;
  const q = Q(m.id);
  const prev = q.last;
  if (m.price != null) {
    q.last = m.price;
    q.dir = prev == null || m.price === prev ? 0 : m.price > prev ? 1 : -1;
    q.flash = q.dir || q.flash;
  }
  q.open = m.prev ?? q.open;
  q.high = m.high ?? q.high;
  q.low = m.low ?? q.low;
  q.vol = m.vol ?? q.vol;
  q.cur = m.cur ?? q.cur;
  q.state = m.state ?? q.state;
  q.delay = m.delay ?? q.delay;
  q.name = m.name ?? q.name;
  q.asOf = m.time ?? q.asOf;
  q.cap = m.cap ?? q.cap;
  q.yearHigh = m.yearHigh ?? q.yearHigh;
  q.yearLow = m.yearLow ?? q.yearLow;
  if (m.spark && m.spark.length) {
    q.spark = m.spark;
    q.sparkDirty = true;
  }
  q.dirty = true;
  if (m.tick && m.price != null) {
    if (m.id === S.sel && m.regular) emit("trade", { t: m.time, p: m.price, s: 0 }, true);
    emit("price", m.id, prev, m.price);
  }
}

function onStatus(m) {
  S.y.stream = m.stream;
  S.y.limited = m.limited;
}

/* Tell the data program which symbols to follow, and take the quotes it already holds. */
export async function pushWatch() {
  try {
    const r = await api(`/api/watch?symbols=${param(S.mlist.join(","))}`);
    onStatus(r.status);
    r.quotes.forEach(onQuote);
  } catch { /* the stream's onerror already shows the program as offline */ }
}

export function connectMarkets() {
  const es = new EventSource("/api/stream");
  es.onopen = () => {
    S.y.up = true;
    emit("markets-link", true);
    pushWatch();
    syncCrypto();
  };
  es.onerror = () => {
    S.y.up = false;
    S.y.stream = false;
    S.live = false;
    S.feedUp = false;
    emit("markets-link", false);
  };
  es.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.type === "q") onQuote(m);
    else if (m.type === "s") onStatus(m);
    else onCryptoMessage(m);
  };
}
