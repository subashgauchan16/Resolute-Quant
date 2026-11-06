"""Subash Terminal data program.

Runs on your own PC only. It uses yfinance to read stocks, indices, FX, company
data and news from Yahoo Finance, then passes them to the terminal page.

Data from Yahoo through yfinance is for personal use. It can be delayed or stop
working without notice. This program never places orders.
"""
from __future__ import annotations

import json
import logging
import math
import queue
import re
import threading
import time
import warnings
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import yfinance as yf

try:
    from yfinance.exceptions import YFRateLimitError
except Exception:  # older yfinance versions
    class YFRateLimitError(Exception):
        pass

HOST, PORT = "127.0.0.1", 8765
PAGE = Path(__file__).with_name("terminal.html")
SYMBOL = re.compile(r"^[A-Z0-9.^=\-]{1,15}$")
SNAPSHOT_SECONDS = 90      # gap between price refreshes for the whole watchlist
META_SECONDS = 300         # gap between market-state refreshes
MIN_CALL_GAP = 0.3         # polite gap between Yahoo requests
INTERVALS = {              # name: (yfinance interval, period, cache seconds)
    "1M": ("1m", "5d", 20), "5M": ("5m", "1mo", 45), "15M": ("15m", "1mo", 90),
    "1H": ("60m", "1y", 300), "1D": ("1d", "5y", 900), "1W": ("1wk", "10y", 1800),
}

logging.getLogger("yfinance").setLevel(logging.CRITICAL)
warnings.filterwarnings("ignore", category=DeprecationWarning)


def clean(value):
    """Turn NaN, infinity and numpy numbers into values JSON can carry."""
    if value is None:
        return None
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


class Yahoo:
    """All Yahoo requests pass through here, one at a time, with backoff."""

    def __init__(self):
        self.lock = threading.Lock()
        self.last = 0.0
        self.blocked_until = 0.0
        self.strikes = 0

    @property
    def limited(self):
        return time.time() < self.blocked_until

    def call(self, fn):
        if self.limited:
            raise YFRateLimitError()
        with self.lock:
            gap = MIN_CALL_GAP - (time.time() - self.last)
            if gap > 0:
                time.sleep(gap)
            self.last = time.time()
            try:
                result = fn()
                self.strikes = 0
                return result
            except YFRateLimitError:
                self.strikes += 1
                self.blocked_until = time.time() + min(900, 60 * 2 ** (self.strikes - 1))
                print(f"Yahoo asked us to slow down. Pausing requests for "
                      f"{int(self.blocked_until - time.time())} seconds.")
                raise


yahoo = Yahoo()


class Cache:
    def __init__(self):
        self.lock = threading.Lock()
        self.items = {}

    def get(self, key, ttl, load):
        """Serve fresh data if we can, and the last good copy if Yahoo refuses."""
        with self.lock:
            hit = self.items.get(key)
        if hit and time.time() - hit[0] < ttl:
            return hit[1], False
        try:
            value = load()
        except Exception:
            if hit:
                return hit[1], True
            raise
        with self.lock:
            self.items[key] = (time.time(), value)
        return value, False


cache = Cache()


class Hub:
    """Holds the latest quotes and pushes changes to any open page."""

    def __init__(self):
        self.lock = threading.Lock()
        self.quotes = {}
        self.watch = []
        self.clients = set()
        self.stream_up = False
        self.last_snapshot = 0.0
        self.changed = threading.Event()

    def set_watch(self, symbols):
        with self.lock:
            self.watch = symbols
            for s in symbols:
                self.quotes.setdefault(s, {"id": s})
        self.changed.set()

    def merge(self, symbol, fields):
        with self.lock:
            quote = self.quotes.setdefault(symbol, {"id": symbol})
            quote.update({k: v for k, v in fields.items() if v is not None})
            message = dict(quote, type="q")
        self.publish(message)

    def status(self):
        return {"type": "s", "stream": self.stream_up, "limited": yahoo.limited,
                "snapshot": self.last_snapshot}

    def publish(self, message):
        data = json.dumps(message, allow_nan=False)
        for client in list(self.clients):
            try:
                client.put_nowait(data)
            except queue.Full:
                self.clients.discard(client)


hub = Hub()


def fast_quote(symbol):
    info = yf.Ticker(symbol).fast_info

    def field(name):
        try:
            return clean(info.get(name))
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
    info = yf.Ticker(symbol).info or {}
    closes = yf.Ticker(symbol).history(period="5d", interval="60m", auto_adjust=False)["Close"]
    spark = [round(v, 6) for v in (clean(x) for x in closes.tolist()[-24:]) if v is not None]
    return {
        "name": info.get("longName") or info.get("shortName"),
        "state": info.get("marketState"),
        "delay": info.get("exchangeDataDelayedBy"),
        "exchange": info.get("fullExchangeName") or info.get("exchange"),
        "spark": spark,
    }


def snapshot_loop():
    meta_at = {}
    while True:
        started = time.time()
        with hub.lock:
            symbols = list(hub.watch)
        for symbol in symbols:
            with hub.lock:
                recent_tick = time.time() - hub.quotes.get(symbol, {}).get("tickAt", 0) < 120
            try:
                last, fields = yahoo.call(lambda: fast_quote(symbol))
                if not recent_tick:  # the stream knows better while it is ticking
                    fields.update(price=last, time=int(time.time() * 1000), tick=False)
                hub.merge(symbol, fields)
                if time.time() - meta_at.get(symbol, 0) > META_SECONDS:
                    hub.merge(symbol, yahoo.call(lambda: meta(symbol)))
                    meta_at[symbol] = time.time()
            except YFRateLimitError:
                break
            except Exception as error:
                print(f"Could not refresh {symbol}: {error}")
        hub.last_snapshot = time.time()
        hub.publish(hub.status())
        hub.changed.clear()
        hub.changed.wait(max(1, SNAPSHOT_SECONDS - (time.time() - started)))


def on_tick(message):
    symbol, price = message.get("id"), clean(message.get("price"))
    if not symbol or price is None:
        return
    stamp = clean(message.get("time")) or time.time() * 1000
    with hub.lock:
        state = hub.quotes.get(symbol, {}).get("state")
    # The stream's own session flag is unreliable for indices, so use Yahoo's marketState.
    hub.merge(symbol, {
        "price": price, "time": int(stamp), "tick": True, "tickAt": time.time(),
        "regular": message.get("market_hours") == 1 and state == "REGULAR",
        "high": clean(message.get("day_high")), "low": clean(message.get("day_low")),
    })


def stream_loop():
    wait = 2
    while True:
        with hub.lock:
            symbols = list(hub.watch)
        if not symbols:
            time.sleep(1)
            continue
        subscribed = set(symbols)
        try:
            with yf.WebSocket(verbose=False) as socket:
                socket.subscribe(symbols)
                hub.stream_up = True
                hub.publish(hub.status())
                wait = 2

                def follow_watchlist():
                    while hub.stream_up:
                        hub.changed.wait(5)
                        with hub.lock:
                            new = [s for s in hub.watch if s not in subscribed]
                        if new:
                            subscribed.update(new)
                            try:
                                socket.subscribe(new)
                            except Exception:
                                return
                        time.sleep(1)

                threading.Thread(target=follow_watchlist, daemon=True).start()
                socket.listen(on_tick)
        except Exception as error:
            print(f"Yahoo stream dropped ({type(error).__name__}). Retrying in {wait}s.")
        hub.stream_up = False
        hub.publish(hub.status())
        time.sleep(wait)
        wait = min(60, wait * 2)


def candles(symbol, name):
    interval, period, _ = INTERVALS[name]
    frame = yf.Ticker(symbol).history(period=period, interval=interval, auto_adjust=False)
    bars = []
    for stamp, row in frame.tail(320).iterrows():
        o, h, l, c = (clean(row[k]) for k in ("Open", "High", "Low", "Close"))
        if None in (o, h, l, c):
            continue
        bars.append({"time": int(stamp.timestamp()), "open": o, "high": h, "low": l, "close": c,
                     "volume": clean(row.get("Volume")) or 0})
    if not bars:
        raise ValueError("no bars")
    return bars


def profile(symbol):
    info = yf.Ticker(symbol).info or {}
    keep = ("sector", "industry", "country", "website", "currency", "quoteType")
    out = {k: info.get(k) for k in keep}
    out.update({
        "name": info.get("longName") or info.get("shortName"),
        "exchange": info.get("fullExchangeName") or info.get("exchange"),
        "pe": clean(info.get("trailingPE")), "forwardPe": clean(info.get("forwardPE")),
        "eps": clean(info.get("trailingEps")), "dividend": clean(info.get("dividendYield")),
        "beta": clean(info.get("beta")), "avgVolume": clean(info.get("averageVolume")),
        "cap": clean(info.get("marketCap")),
        "yearHigh": clean(info.get("fiftyTwoWeekHigh")), "yearLow": clean(info.get("fiftyTwoWeekLow")),
    })
    return out


def news(symbol):
    items = []
    for raw in yf.Ticker(symbol).news or []:
        item = raw.get("content", raw) or {}
        link = (item.get("clickThroughUrl") or {}).get("url") or (item.get("canonicalUrl") or {}).get("url")
        title = item.get("title")
        if not title or not isinstance(link, str) or not link.startswith("https://"):
            continue
        items.append({"title": title, "url": link, "time": item.get("pubDate"),
                      "publisher": (item.get("provider") or {}).get("displayName") or "Yahoo Finance"})
    return items[:12]


def resolve(symbol):
    last, fields = fast_quote(symbol)
    hub.merge(symbol, dict(fields, price=last, time=int(time.time() * 1000), tick=False))
    return {"symbol": symbol, "price": last, "cur": fields.get("cur")}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "SubashTerminal"

    def log_message(self, *args):
        pass

    def reply(self, status, body, kind="application/json; charset=utf-8"):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value, status=200):
        self.reply(status, json.dumps(value, allow_nan=False).encode())

    def symbol(self, query):
        value = (query.get("symbol") or [""])[0].upper()
        return value if SYMBOL.match(value) else None

    def do_GET(self):
        # Only answer pages addressed to this PC, not other websites.
        if self.headers.get("Host", "").split(":")[0] not in ("127.0.0.1", "localhost"):
            return self.send_json({"error": "Local use only."}, 403)
        url = urlparse(self.path)
        query = parse_qs(url.query)
        try:
            if url.path in ("/", "/terminal.html"):
                return self.reply(200, PAGE.read_bytes(), "text/html; charset=utf-8")
            if url.path == "/api/stream":
                return self.stream()
            if url.path == "/api/watch":
                raw = (query.get("symbols") or [""])[0].upper().split(",")
                hub.set_watch([s for s in raw if SYMBOL.match(s)][:40])
                with hub.lock:
                    quotes = [dict(hub.quotes[s], type="q") for s in hub.watch]
                return self.send_json({"quotes": quotes, "status": hub.status()})
            symbol = self.symbol(query)
            if not symbol:
                return self.send_json({"error": "That is not a valid symbol."}, 400)
            if url.path == "/api/candles":
                name = (query.get("interval") or ["5M"])[0].upper()
                if name not in INTERVALS:
                    return self.send_json({"error": "Unknown interval."}, 400)
                bars, stale = cache.get(("bars", symbol, name), INTERVALS[name][2],
                                        lambda: yahoo.call(lambda: candles(symbol, name)))
                return self.send_json({"bars": bars, "stale": stale})
            if url.path == "/api/profile":
                data, stale = cache.get(("profile", symbol), 3600, lambda: yahoo.call(lambda: profile(symbol)))
                return self.send_json(dict(data, stale=stale))
            if url.path == "/api/news":
                data, stale = cache.get(("news", symbol), 300, lambda: yahoo.call(lambda: news(symbol)))
                return self.send_json({"items": data, "stale": stale})
            if url.path == "/api/resolve":
                data, _ = cache.get(("resolve", symbol), 300, lambda: yahoo.call(lambda: resolve(symbol)))
                return self.send_json(data)
            return self.send_json({"error": "Not found."}, 404)
        except YFRateLimitError:
            return self.send_json({"error": "Yahoo is rate limiting this PC. It will retry shortly."}, 429)
        except (BrokenPipeError, ConnectionError):
            return None
        except Exception as error:
            print(f"{url.path} failed: {error}")
            return self.send_json({"error": "Yahoo returned no data for that request."}, 502)

    def stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        client = queue.Queue(maxsize=500)
        hub.clients.add(client)
        try:
            client.put_nowait(json.dumps(hub.status()))
            while True:
                try:
                    data = client.get(timeout=10)
                    self.wfile.write(f"data: {data}\n\n".encode())
                except queue.Empty:
                    self.wfile.write(f"data: {json.dumps(hub.status())}\n\n".encode())
                self.wfile.flush()
        except (BrokenPipeError, ConnectionError, OSError):
            pass
        finally:
            hub.clients.discard(client)


class Server(ThreadingHTTPServer):
    daemon_threads = True


def main():
    try:
        server = Server((HOST, PORT), Handler)
    except OSError:
        print(f"Port {PORT} is already in use. The terminal may already be running.")
        print(f"Opening http://{HOST}:{PORT}")
        webbrowser.open(f"http://{HOST}:{PORT}")
        return
    threading.Thread(target=snapshot_loop, daemon=True).start()
    threading.Thread(target=stream_loop, daemon=True).start()
    print(f"Subash Terminal is running at http://{HOST}:{PORT}")
    print("Keep this window open. Close it to stop the terminal.")
    threading.Timer(0.8, lambda: webbrowser.open(f"http://{HOST}:{PORT}")).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
