/* Shared session state: the watchlists, the open symbol and per-symbol quotes. */
import { store } from "./localStorage.js";
import { dp, num } from "../chart/helper.js";

const DEFAULTS = ["BTC-USD", "ETH-USD", "SOL-USD", "XRP-USD", "DOGE-USD", "ADA-USD", "LINK-USD", "AVAX-USD", "LTC-USD", "BTC-GBP"];
const MDEFAULTS = ["^GSPC", "^IXIC", "^FTSE", "AAPL", "NVDA", "MSFT", "TSLA", "HSBA.L", "GBPUSD=X", "GC=F"];
const GRANS = [["1M", 60], ["5M", 300], ["15M", 900], ["1H", 3600], ["6H", 21600], ["1D", 86400]];
const YGRANS = [["1M", 60], ["5M", 300], ["15M", 900], ["1H", 3600], ["4H", 14400], ["1D", 86400], ["1W", 604800], ["1MO", 2592000]];

/* Commands and the API use 1M for one minute and 1MO for one month. Buttons show the difference. */
export const granLabel = (name) => name.replace(/^(\d+)M$/, "$1m");

export const FRIENDLY = { "^GSPC": "S&P 500", "^IXIC": "NASDAQ", "^FTSE": "FTSE 100", "^DJI": "DOW", "GBPUSD=X": "GBP/USD", "EURUSD=X": "EUR/USD", "GC=F": "GOLD", "CL=F": "OIL" };
export const LEVELS = 10;
export const MAX_WATCH = 40; /* per group; the data program enforces the same limit */
export const BIG_TRADE_USD = 50000;

export const validY = (p) => /^[A-Z0-9.^=\-]{1,15}$/.test(p);
export const validPair = (p) => /^[A-Z0-9]{1,12}-[A-Z0-9]{2,6}$/.test(p);

export const S = {
  y: { up: false, stream: false, limited: false },
  mlist: store.get("mlist", MDEFAULTS).filter(validY),
  list: store.get("list", DEFAULTS).filter(validPair),
  sel: store.get("sel", "BTC-USD"),
  gran: store.get("gran", 300),
  feedUp: null, live: false, lastMsg: 0, count: 0, rate: 0, lag: null, retryAt: 0,
  q: {},
  alerts: store.get("alerts", []),
  started: Date.now(),
};

export const base = (p) => p.split("-")[0];
export const quoteCcy = (p) => p.split("-")[1];
export const isY = (id) => S.mlist.includes(id);
export const label = (id) => (isY(id) ? (FRIENDLY[id] || id) : base(id));
export const grans = () => (isY(S.sel) ? YGRANS : GRANS);

if (!S.list.length) S.list = [...DEFAULTS];
if (!S.list.includes(S.sel) && !S.mlist.includes(S.sel)) S.sel = S.list[0];
if (!grans().some((g) => g[1] === S.gran)) S.gran = 300;

export function Q(p) {
  return S.q[p] || (S.q[p] = { last: null, open: null, high: null, low: null, vol: null, bid: null, ask: null, spark: [], tape: [], buy: 0, sell: 0, pv: 0, v: 0, n: 0, times: [], since: Date.now(), dirty: true, dir: 0 });
}

/* Decimal places depend on the asset: stocks use cents, FX and crypto scale with price. */
export const dpFor = (id, v) => (isY(id) && !/=X$/.test(id) ? (Math.abs(v) >= 1 ? 2 : 4) : dp(Math.abs(v)));
export const pxs = (v) => (v == null ? "-" : num(v, dpFor(S.sel, v)));
