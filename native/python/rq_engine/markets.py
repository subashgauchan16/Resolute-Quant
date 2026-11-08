"""Yahoo quotes, profiles, news and chart bars for the terminal's markets side.

The Yahoo functions are short copies from server.py, which the app doesn't load. A test keeps them
equal. Yahoo's tick stream isn't ported yet, so quotes refresh every 90 seconds, and the terminal's
status badge says so.
"""

import json
import threading
import time

from crypto_feed import ProviderError

from .market_history import YAHOO_INTERVALS, _clean, yahoo_call, yahoo_limited

SNAPSHOT_SECONDS = 90
META_SECONDS = 300
STOP_TIMEOUT_SECONDS = 0.5
# Name: (period to request). The same ranges as server.INTERVALS.
PERIODS = {"1M": "5d", "5M": "1mo", "15M": "1mo", "1H": "1y", "4H": "1y", "1D": "5y", "1W": "10y", "1MO": "max"}


def _yf():
    import yfinance  # Slow to import, so only loaded when first needed.

    return yfinance


def fast_quote(symbol):
    from yfinance.exceptions import YFRateLimitError

    info = _yf().Ticker(symbol).fast_info

    def field(name):
        try:
            return _clean(info.get(name))
        except YFRateLimitError:
            raise
        except Exception:
            return None

    out = {"prev": field("previousClose"), "high": field("dayHigh"), "low": field("dayLow"),
           "vol": field("lastVolume"), "yearHigh": field("yearHigh"), "yearLow": field("yearLow"),
           "cap": field("marketCap")}
    try:
        out["cur"] = info.get("currency")
    except YFRateLimitError:
        raise
    except Exception:
        pass
    last = field("lastPrice")
    if last is None:
        raise ValueError("no price")
    return last, out


def meta(symbol):
    yf = _yf()
    info = yf.Ticker(symbol).info or {}
    closes = yf.Ticker(symbol).history(period="5d", interval="60m", auto_adjust=False)["Close"]
    spark = [round(v, 6) for v in (_clean(x) for x in closes.tolist()[-24:]) if v is not None]
    return {
        "name": info.get("longName") or info.get("shortName"),
        "state": info.get("marketState"),
        "delay": info.get("exchangeDataDelayedBy"),
        "exchange": info.get("fullExchangeName") or info.get("exchange"),
        "spark": spark,
    }


def candles(symbol, name):
    frame = _yf().Ticker(symbol).history(period=PERIODS[name], interval=YAHOO_INTERVALS[name], auto_adjust=False)
    bars = []
    for stamp, row in frame.tail(320).iterrows():
        o, h, l, c = (_clean(row[k]) for k in ("Open", "High", "Low", "Close"))
        if None in (o, h, l, c):
            continue
        bars.append({"time": int(stamp.timestamp()), "open": o, "high": h, "low": l, "close": c,
                     "volume": _clean(row.get("Volume")) or 0})
    if not bars:
        raise ValueError("no bars")
    return bars


def profile(symbol):
    info = _yf().Ticker(symbol).info or {}
    keep = ("sector", "industry", "country", "website", "currency", "quoteType")
    out = {k: info.get(k) for k in keep}
    out.update({
        "name": info.get("longName") or info.get("shortName"),
        "exchange": info.get("fullExchangeName") or info.get("exchange"),
        "pe": _clean(info.get("trailingPE")), "forwardPe": _clean(info.get("forwardPE")),
        "eps": _clean(info.get("trailingEps")), "dividend": _clean(info.get("dividendYield")),
        "beta": _clean(info.get("beta")), "avgVolume": _clean(info.get("averageVolume")),
        "cap": _clean(info.get("marketCap")),
        "yearHigh": _clean(info.get("fiftyTwoWeekHigh")), "yearLow": _clean(info.get("fiftyTwoWeekLow")),
    })
    return out


def news(symbol):
    items = []
    for raw in _yf().Ticker(symbol).news or []:
        item = raw.get("content", raw) or {}
        link = (item.get("clickThroughUrl") or {}).get("url") or (item.get("canonicalUrl") or {}).get("url")
        title = item.get("title")
        if not title or not isinstance(link, str) or not link.startswith("https://"):
            continue
        items.append({"title": title, "url": link, "time": item.get("pubDate"),
                      "publisher": (item.get("provider") or {}).get("displayName") or "Yahoo Finance"})
    return items[:12]


class Provider:
    """The real Yahoo calls, each passing through the shared throttle."""

    quote = staticmethod(lambda symbol: yahoo_call(lambda: fast_quote(symbol)))
    meta = staticmethod(lambda symbol: yahoo_call(lambda: meta(symbol)))
    candles = staticmethod(lambda symbol, name: yahoo_call(lambda: candles(symbol, name)))
    profile = staticmethod(lambda symbol: yahoo_call(lambda: profile(symbol)))
    news = staticmethod(lambda symbol: yahoo_call(lambda: news(symbol)))
    limited = staticmethod(yahoo_limited)


class Markets:
    """Refreshes the watched symbols in the background and sends each change to the window."""

    def __init__(self, emit, provider=Provider):
        self.emit = emit
        self.provider = provider
        self.lock = threading.Lock()
        self.watch = []
        self.stop_event = None
        self.wake = threading.Event()
        self.thread = None
        self.meta_at = {}

    @property
    def running(self):
        return self.stop_event is not None

    def set_watch(self, symbols):
        with self.lock:
            self.watch = list(symbols)
        self.wake.set()

    def start(self, symbols):
        self.set_watch(symbols)
        if self.running:
            return
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._loop, args=(self.stop_event,), name="rq-markets", daemon=True)
        self.thread.start()

    def stop(self):
        if not self.running:
            return
        self.stop_event.set()
        self.wake.set()
        self.thread.join(STOP_TIMEOUT_SECONDS)
        self.stop_event = None
        self.thread = None

    def _send(self, **fields):
        self.emit(json.dumps(dict(fields, type="markets"), allow_nan=False))

    def refresh(self, symbol, stop=None):
        """Fetches one symbol's quote, and its slower details when they are due."""
        last, fields = self.provider.quote(symbol)
        self._send(quote=dict({k: v for k, v in fields.items() if v is not None}, id=symbol, price=last, time=int(time.time() * 1000)))
        if stop is not None and stop.is_set():
            return
        if time.time() - self.meta_at.get(symbol, 0) > META_SECONDS:
            details = self.provider.meta(symbol)
            self.meta_at[symbol] = time.time()
            self._send(quote=dict({k: v for k, v in details.items() if v is not None}, id=symbol))

    def cycle(self, stop=None):
        with self.lock:
            symbols = list(self.watch)
        for symbol in symbols:
            if stop is not None and stop.is_set():
                return
            try:
                self.refresh(symbol, stop)
            except ProviderError:
                break  # Yahoo asked to slow down, so wait for the next round.
            except Exception:
                continue  # One symbol's failure must not stop the others.
        self._send(status={"limited": bool(self.provider.limited())})

    def _loop(self, stop):
        self._send(status={"limited": False})
        while not stop.is_set():
            self.wake.clear()
            started = time.time()
            self.cycle(stop)
            self.wake.wait(max(1, SNAPSHOT_SECONDS - (time.time() - started)))
