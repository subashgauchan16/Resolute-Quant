/* Crypto data, all from the local data program, which owns the Coinbase connection. No DOM. */
import { api } from "./http.js";
import { bookReset, setBook } from "./orderbook.js";
import { emit } from "../store/events.js";
import { isY, Q, S } from "../store/state.js";

const STALE_MS = 6000;
const SPARK_GRANULARITY = 3600;
const param = (value) => encodeURIComponent(value);

export const fetchStats = (p) => api(`/api/crypto/stats?symbol=${param(p)}`);

export async function fetchCandles(p, gran) {
  return (await api(`/api/crypto/candles?symbol=${param(p)}&granularity=${gran}`)).bars;
}

export async function seed(p) {
  const q = Q(p);
  try {
    const s = await fetchStats(p);
    q.open ??= s.open;
    q.high = s.high ?? q.high;
    q.low = s.low ?? q.low;
    q.vol = s.volume ?? q.vol;
    q.last ??= s.last;
    q.dirty = true;
  } catch { /* the live ticker fills this in */ }
  try {
    q.spark = (await fetchCandles(p, SPARK_GRANULARITY)).slice(-24).map((bar) => bar.close);
    q.dirty = true;
    q.sparkDirty = true;
  } catch { /* the sparkline stays empty until the next seed */ }
}

export async function seedAll() {
  for (const p of [...S.list]) await seed(p);
}

/* Tell the data program what this page follows. Safe to repeat. */
export async function pushCryptoWatch() {
  try {
    await api(`/api/crypto/watch?symbols=${param(S.list.join(","))}`);
  } catch { /* the stream's onerror already shows the program as offline */ }
}

export async function followBook(p) {
  bookReset();
  try {
    await api(`/api/crypto/book?symbol=${param(isY(p) ? "" : p)}`);
  } catch { /* retried when the stream reconnects */ }
}

export async function syncCrypto() {
  await pushCryptoWatch();
  await followBook(S.sel);
}

/* Repaint check: mark silence as stale. Reconnecting upstream is the data program's job. */
export function checkFeed(now) {
  if (S.live && now - S.lastMsg > STALE_MS) S.live = false;
}

function onStatus(m) {
  const wasConnected = S.feedUp;
  S.feedUp = m.connected;
  S.retryAt = Date.now() + (m.retryIn || 0) * 1000;
  if (!m.connected) S.live = false;
  if (m.connected && wasConnected === false && !isY(S.sel)) emit("feed-restored");
}

function onTick(m) {
  const p = m.id;
  const q = Q(p);
  const prev = q.last;
  q.open = m.open ?? q.open;
  q.high = m.high ?? q.high;
  q.low = m.low ?? q.low;
  q.vol = m.vol ?? q.vol;
  q.bid = m.bid ?? q.bid;
  q.ask = m.ask ?? q.ask;
  q.last = m.price;
  q.dirty = true;
  q.dir = prev == null || m.price === prev ? 0 : m.price > prev ? 1 : -1;
  q.flash = q.dir || q.flash;
  S.lag = Date.now() - m.time;
  if (m.trade == null) return;
  const trade = { t: m.time, p: m.price, s: m.size, side: m.side, id: m.trade };
  q.tape.unshift(trade);
  if (q.tape.length > 60) q.tape.pop();
  q[m.side === "buy" ? "buy" : "sell"] += m.size;
  q.pv += m.price * m.size;
  q.v += m.size;
  q.n++;
  q.times.push(m.time);
  while (q.times[0] < m.time - 60000) q.times.shift(); /* only the last minute is used */
  if (p === S.sel) {
    S.tapeDirty = true;
    emit("trade", trade);
  }
  emit("price", p, prev, m.price);
}

/* Messages from /api/stream: ct = tick, cb = book, cs = feed status. */
export function onCryptoMessage(m) {
  if (m.type === "cs") {
    onStatus(m);
    return;
  }
  S.lastMsg = Date.now();
  S.count++;
  S.live = true;
  S.everLive = true;
  if (m.type === "ct") onTick(m);
  else if (m.type === "cb" && m.id === S.sel) setBook(m);
}
