"""Resolute Quant data program.

Runs on your own PC only. It uses yfinance to read stocks, indices, FX, company
data and news from Yahoo Finance, then passes them to the terminal page.

Data from Yahoo through yfinance is for personal use. It can be delayed or stop
working without notice. This program never places orders.
"""
from __future__ import annotations

import argparse
import json
import logging
import math
import os
import queue
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
import warnings
import webbrowser
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

import yfinance as yf

import history
import research_models
from crypto_feed import GRANULARITIES, MAX_WATCH, PAIR, CoinbaseFeed, ProviderError, Rest, normalise_candles

try:
    from yfinance.exceptions import YFRateLimitError
except Exception:  # older yfinance versions
    class YFRateLimitError(Exception):
        pass

HOST, DEFAULT_PORT = "127.0.0.1", 8765
FALLBACK_PORTS = 10        # with no configured port, try this many ports after the default
CONFIG_FILE = Path(__file__).with_name("terminal.config.json")
SERVICE = "resolute-quant"
PAGE =Path(__file__).with_name("terminal.html")
ASSETS = Path(__file__).with_name("assets").resolve()
ASSET_TYPES = {".css": "text/css; charset=utf-8", ".js": "text/javascript; charset=utf-8",
               ".svg": "image/svg+xml", ".png": "image/png", ".ico": "image/x-icon",
               ".webp": "image/webp"}
SYMBOL =re.compile(r"^[A-Z0-9.^=\-]{1,15}$")
SNAPSHOT_SECONDS = 90      # gap between price refreshes for the whole watchlist
META_SECONDS = 300         # gap between market-state refreshes
MIN_CALL_GAP = 0.3         # polite gap between Yahoo requests
INTERVALS = {              # name: (yfinance interval, period, cache seconds)
    "1M": ("1m", "5d", 20), "5M": ("5m", "1mo", 45), "15M": ("15m", "1mo", 90),
    "1H": ("60m", "1y", 300), "4H": ("4h", "1y", 600), "1D": ("1d", "5y", 900),
    "1W": ("1wk", "10y", 1800), "1MO": ("1mo", "max", 3600),  # 1M is one minute, 1MO one month
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
    MAX_ITEMS = 256

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
            for old in sorted(self.items, key=lambda k: self.items[k][0])[:max(0, len(self.items) - self.MAX_ITEMS)]:
                del self.items[old]
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
coinbase = CoinbaseFeed(hub.publish)
rest = Rest()
instance = {"id": uuid.uuid4().hex, "started": int(time.time() * 1000), "port": None,
            "preferred": DEFAULT_PORT, "source": "default"}


def health():
    """Contract for GET /api/health. The launcher uses `service` and `instance` to confirm that
    a port is really this terminal, and the page uses the port fields to report a fallback."""
    feed = coinbase.status()
    fine = feed["connected"] and not yahoo.limited
    return {"service": SERVICE, "status": "ok" if fine else "degraded",
            "timestamp": int(time.time() * 1000), "instance": instance["id"], "started": instance["started"],
            "port": instance["port"], "preferredPort": instance["preferred"], "portSource": instance["source"],
            "fallback": instance["port"] != instance["preferred"], "clients": len(hub.clients),
            "providers": {
                "yahoo": {"stream": hub.stream_up, "limited": yahoo.limited, "snapshot": hub.last_snapshot},
                "coinbase": {"connected": feed["connected"], "lastMessage": feed["lastMessage"], "retryIn": feed["retryIn"],
                             "reconnects": feed["reconnects"], "rejected": feed["rejected"], "restRejected": rest.rejected},
            }}


def split_watch(raw, valid):
    """Keep valid symbols up to the limit, and name the rest so the page can say so."""
    wanted = [s for s in raw if s]
    kept = [s for s in wanted if valid.match(s)][:MAX_WATCH]
    return kept, [s for s in wanted if s not in kept]


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


def yahoo_history(symbol, name, start, end):
    """Unadjusted Yahoo bars for a date range. Rows failing validation are dropped and counted."""
    begin = datetime.fromtimestamp(start, timezone.utc)
    finish = datetime.fromtimestamp(end, timezone.utc)
    frame = yf.Ticker(symbol).history(start=begin, end=finish, interval=INTERVALS[name][0], auto_adjust=False)
    rows = [[int(stamp.timestamp()), clean(row["Low"]), clean(row["High"]), clean(row["Open"]), clean(row["Close"]), clean(row.get("Volume"))]
            for stamp, row in frame.iterrows()]
    bars, rejected = normalise_candles(rows)
    return {"bars": bars[-history.MAX_BARS:], "rejected": rejected, "truncated": len(bars) > history.MAX_BARS,
            "downloaded_at": int(time.time())}


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
    server_version = "ResoluteQuant"

    def log_message(self, *args):
        pass

    def reply(self, status, body, kind="application/json; charset=utf-8", extra=None):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        for header, value in (extra or {}).items():
            self.send_header(header, value)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value, status=200):
        self.reply(status, json.dumps(value, allow_nan=False).encode())

    def symbol(self, query):
        value = (query.get("symbol") or [""])[0].upper()
        return value if SYMBOL.match(value) else None

    def send_asset(self, path):
        """Serve the page's CSS, JS modules and vendored chart library from assets/.

        Added for the modular frontend. Only known file types inside assets/ are served.
        """
        target = (ASSETS / unquote(path[len("/assets/"):])).resolve()
        kind = ASSET_TYPES.get(target.suffix.lower())
        if kind is None or not target.is_relative_to(ASSETS) or not target.is_file():
            return self.send_json({"error": "Not found."}, 404)
        return self.reply(200, target.read_bytes(), kind)

    def do_GET(self):
        # Only answer pages addressed to this PC, not other websites.
        if self.headers.get("Host", "").split(":")[0] not in ("127.0.0.1", "localhost"):
            return self.send_json({"error": "Local use only."}, 403)
        url = urlparse(self.path)
        query = parse_qs(url.query)
        try:
            if url.path in ("/", "/terminal.html"):
                return self.reply(200, PAGE.read_bytes(), "text/html; charset=utf-8")
            if url.path.startswith("/assets/"):
                return self.send_asset(url.path)
            if url.path == "/api/stream":
                return self.stream()
            if url.path == "/api/health":
                return self.send_json(health())
            if url.path == "/api/sentiment/feargreed":
                data, stale = cache.get(("fng",), 3600, rest.fear_greed)
                return self.send_json(dict(data, stale=stale))
            if url.path == "/api/crypto/watch":
                raw = (query.get("symbols") or [""])[0].upper().split(",")
                kept, dropped = split_watch(raw, PAIR)
                return self.send_json({"watch": coinbase.set_watch(kept), "dropped": dropped, "status": coinbase.status()})
            if url.path == "/api/crypto/book":  # one book at a time; an empty symbol stops it
                value = (query.get("symbol") or [""])[0].upper()
                if value and not PAIR.match(value):
                    return self.send_json({"error": "That is not a valid pair."}, 400)
                coinbase.set_book(value)
                return self.send_json({"symbol": value or None})
            if url.path in ("/api/crypto/candles", "/api/crypto/stats"):
                pair = (query.get("symbol") or [""])[0].upper()
                if not PAIR.match(pair):
                    return self.send_json({"error": "That is not a valid pair."}, 400)
                if url.path.endswith("stats"):
                    data, stale = cache.get(("cstats", pair), 20, lambda: rest.stats(pair))
                    return self.send_json(dict(data, stale=stale))
                try:
                    granularity = int((query.get("granularity") or [""])[0])
                except ValueError:
                    granularity = 0
                if granularity not in GRANULARITIES:
                    return self.send_json({"error": "Coinbase does not serve that candle size."}, 400)
                bars, stale = cache.get(("cbars", pair, granularity), min(granularity // 6, 60),
                                        lambda: rest.candles(pair, granularity))
                return self.send_json({"bars": bars, "stale": stale})
            if url.path == "/api/watch":
                raw = (query.get("symbols") or [""])[0].upper().split(",")
                kept, dropped = split_watch(raw, SYMBOL)
                hub.set_watch(kept)
                with hub.lock:
                    quotes = [dict(hub.quotes[s], type="q") for s in hub.watch]
                return self.send_json({"quotes": quotes, "dropped": dropped, "status": hub.status()})
            symbol = self.symbol(query)
            if not symbol:
                return self.send_json({"error": "That is not a valid symbol."}, 400)
            if url.path == "/api/research/forecast":
                # Research is restricted to Yahoo candles. Coinbase candles are not used for model training.
                if PAIR.match(symbol):
                    return self.send_json({"error": "Coinbase data is not used for model research without written provider permission."}, 403)
                name = (query.get("interval") or ["1D"])[0].upper()
                if name not in INTERVALS:
                    return self.send_json({"error": "Unknown interval."}, 400)
                bars, stale = cache.get(("bars", symbol, name), INTERVALS[name][2],
                                        lambda: yahoo.call(lambda: candles(symbol, name)))
                try:
                    model, _ = cache.get(("research", symbol, name, bars[-1]["time"] if bars else 0), 60,
                                         lambda: research_models.run_research(bars))
                except ValueError as error:
                    return self.send_json({"error": str(error), "model_status": "insufficient_data"}, 422)
                return self.send_json(dict(model, data_stale=stale))
            if url.path == "/api/history":
                return self.history(symbol, query)
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
        except ProviderError as error:
            return self.send_json({"error": str(error)}, error.status)
        except history.BadRequest as error:
            return self.send_json({"error": str(error)}, 400)
        except (BrokenPipeError, ConnectionError):
            return None
        except Exception as error:
            print(f"{url.path} failed: {error}")
            return self.send_json({"error": "Yahoo returned no data for that request."}, 502)

    def history(self, symbol, query):
        """Market-price history for the history panel, as JSON or as a CSV file."""
        field = lambda name: (query.get(name) or [""])[0]
        source, name, wants = field("source"), field("interval").upper(), field("format") or "json"
        steps = {"coinbase": history.COINBASE_STEPS, "yahoo": history.YAHOO_STEPS}.get(source)
        if steps is None:
            raise history.BadRequest("The source must be coinbase or yahoo.")
        if name not in steps:
            raise history.BadRequest(f"{source} does not offer that timeframe. Choose from {', '.join(steps)}.")
        if wants not in ("json", "csv"):
            raise history.BadRequest("The format must be json or csv.")
        if source == "coinbase" and not PAIR.match(symbol):
            raise history.BadRequest("That is not a valid pair.")
        if symbol[0] in "=-":  # a spreadsheet would read these as formulas
            raise history.BadRequest("That is not a valid symbol.")
        start, end = history.parse_range(field("start"), field("end"))
        step = steps[name]
        if source == "coinbase":
            load = lambda: rest.candles_between(symbol, step, start, end)
        else:
            load = lambda: yahoo.call(lambda: yahoo_history(symbol, name, start, end))
        fetched, stale = cache.get(("history", source, symbol, name, start // 60, end // 60), 300, load)
        view = history.build_view(fetched, symbol, name, source, step, field("start"), field("end"), stale=stale)
        if wants == "json":
            return self.send_json(view)
        disposition = f'attachment; filename="{history.export_name(view)}"'
        return self.reply(200, history.to_csv(view).encode("utf-8"), "text/csv; charset=utf-8", {"Content-Disposition": disposition})

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
            client.put_nowait(json.dumps(coinbase.status()))
            while client in hub.clients:  # a client that fell too far behind reconnects for a clean start
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
    # On Windows SO_REUSEADDR lets a second listener share a busy port, so ask for sole use instead.
    allow_reuse_address = os.name != "nt"

    def server_bind(self):
        if os.name == "nt":
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def valid_port(value):
    try:
        port = int(value)
    except (TypeError, ValueError):
        return None
    return port if 1024 <= port <= 65535 else None


def configured_port(cli_port, env, config_file):
    """Port selection order: --port, then RESOLUTE_QUANT_PORT, then terminal.config.json, then the default.

    Returns (port, source). A configured port never falls back. Bad settings stop the launch.
    """
    settings = [("--port", cli_port), ("RESOLUTE_QUANT_PORT", env.get("RESOLUTE_QUANT_PORT"))]
    if config_file.exists():
        try:
            settings.append((config_file.name, json.loads(config_file.read_text(encoding="utf-8")).get("port")))
        except (OSError, ValueError, AttributeError):
            raise SystemExit(f"{config_file.name} could not be read. Fix it or delete it.")
    for source, value in settings:
        if value is not None:
            port = valid_port(value)
            if port is None:
                raise SystemExit(f"{source} must be a port number from 1024 to 65535, not {value!r}.")
            return port, source
    return DEFAULT_PORT, "default"


def identify(port):
    """Ask whoever holds a port what it is. Sends one GET and changes nothing.

    Returns (is_terminal, description, health).
    """
    request = urllib.request.Request(f"http://{HOST}:{port}/api/health", headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=2) as response:
            body = json.load(response)
            if isinstance(body, dict) and body.get("service") == SERVICE:
                return True, "this terminal", body
            return False, f"HTTP {response.status}, server {response.headers.get('Server') or 'unnamed'}", None
    except urllib.error.HTTPError as error:
        return False, f"HTTP {error.code}, server {error.headers.get('Server') or 'unnamed'}", None
    except (urllib.error.URLError, OSError, ValueError):
        return False, "it did not answer as a web server", None


def busy_message(port, description):
    return (f"Port {port} is held by another program ({description}). It has not been touched.\n"
            f"  To see what holds it, run: netstat -ano | findstr :{port}\n"
            f"  and, for Windows HTTP services: netsh http show servicestate")


def bind_server(port, source):
    """Bind the chosen port, or a fallback if none was configured.

    Returns (server, None) when bound, (None, url) when this terminal is already running.
    """
    candidates = [port] if source != "default" else [port + i for i in range(FALLBACK_PORTS + 1)]
    for candidate in candidates:
        try:
            return Server((HOST, candidate), Handler), None
        except OSError:
            is_terminal, description, _ = identify(candidate)
            if is_terminal:
                return None, f"http://{HOST}:{candidate}"
            print(busy_message(candidate, description))
    if source != "default":
        raise SystemExit(f"\nPort {port} was set by {source}, so no other port was tried. Choose a free port or free this one.")
    raise SystemExit(f"\nPorts {candidates[0]} to {candidates[-1]} are all busy. Set a free port with RESOLUTE_QUANT_PORT or terminal.config.json.")


def open_when_confirmed(url, port, attempts=10, wait=1.0):
    """Open the browser once the port answers as this very instance. A busy PC can be slow to answer."""
    description = "it did not answer"
    for attempt in range(attempts):
        is_terminal, description, body = identify(port)
        if is_terminal and body.get("instance") == instance["id"]:
            webbrowser.open(url)
            return True
        if attempt < attempts - 1:
            time.sleep(wait)
    print(f"Not opening the browser: port {port} did not answer as this terminal ({description}).")
    print(f"If the terminal is running, open {url} yourself.")
    return False


def main():
    parser = argparse.ArgumentParser(description="Resolute Quant data program")
    parser.add_argument("--port", help="port to use; overrides RESOLUTE_QUANT_PORT and terminal.config.json")
    parser.add_argument("--no-browser", action="store_true", help="do not open a browser")
    options = parser.parse_args()
    preferred, source = configured_port(options.port, os.environ, CONFIG_FILE)
    server, existing = bind_server(preferred, source)
    if server is None:
        print(f"The terminal is already running at {existing}. Opening it.")
        if not options.no_browser:
            webbrowser.open(existing)
        return
    port = server.server_address[1]
    instance.update(port=port, preferred=preferred, source=source)
    url = f"http://{HOST}:{port}"
    threading.Thread(target=snapshot_loop, daemon=True).start()
    threading.Thread(target=stream_loop, daemon=True).start()
    threading.Thread(target=coinbase.run, daemon=True).start()
    if port != preferred:
        print(f"\nUSING PORT {port} INSTEAD OF {preferred}. Watchlists and alerts are saved per address,\n"
              f"so this address has its own saved settings. To choose the port, set RESOLUTE_QUANT_PORT or terminal.config.json.\n")
    print(f"Resolute Quant is running at {url}")
    print("Keep this window open. Close it to stop the terminal.")
    if not options.no_browser:
        threading.Timer(0.8, lambda: open_when_confirmed(url, port)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
