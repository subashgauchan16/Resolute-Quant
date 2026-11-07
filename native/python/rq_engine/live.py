"""Live Coinbase data for the terminal, reusing the browser terminal's feed and validation.

The feed runs on its own Python thread. Every trade updates the forming bar and the session figures, so
highs, lows and flow are exact, but the window is told at most five times a second. The bar rules match
the browser terminal: a new bar opens at its first trade, and bars are reloaded after a reconnect.
"""

import json
import math
import random
import threading
import time
from collections import deque

import _rq_host
import history
from crypto_feed import MAX_WATCH, PAIR, CoinbaseFeed, ProviderError

from . import market_history, markets

FLUSH_SECONDS = 0.2
STATUS_SECONDS = 2.0
RATE_SECONDS = 1.0
# Short, because closing a connection politely can take seconds, and the app shouldn't wait on it to quit.
# The threads are daemons, so any still finishing can't hold up the interpreter's shutdown.
STOP_TIMEOUT_SECONDS = 1.0
RESEED_SECONDS = 1800
FEAR_GREED_SECONDS = 3600
TAPE_KEPT = 60
TAPE_SENT = 40
BOOK_LEVELS = 10
SPARK_GRANULARITY = 3600
SPARK_POINTS = 24


class FeedStopped(Exception):
    """Raised inside the feed's publish call, which is how its endless session loop is left."""


class StoppableFeed(CoinbaseFeed):
    """CoinbaseFeed.run never returns, which suits a server but not an app that can stop streaming."""

    def __init__(self, publish, stop):
        super().__init__(publish)
        self.stop_event = stop

    def run(self):
        attempt = 0
        try:
            while not self.stop_event.is_set():
                started = time.time()
                try:
                    self._session()
                except FeedStopped:
                    raise
                except Exception:
                    pass  # The status message tells the window it dropped.
                self.connected = False
                with self.lock:
                    self.book = None
                attempt = 0 if time.time() - started > 60 else attempt + 1
                delay = min(30, 2 ** attempt) + random.random() * 0.4
                self.reconnects += 1
                self.retry_at = time.time() + delay
                self.publish(self.status())
                self.stop_event.wait(delay)
        except FeedStopped:
            pass
        finally:
            self.connected = False


def apply_trade(bar, time_ms, price, size, step, skip_volume=False):
    """Returns the bar after a trade, or None if the trade belongs to an earlier bar."""
    bucket = math.floor(time_ms / 1000 / step) * step
    if bucket < bar["time"]:
        return None
    if bucket > bar["time"]:
        # The previous close can be minutes old, so the first trade seen opens the new bar.
        bar = {"time": bucket, "open": price, "high": price, "low": price, "close": price, "volume": 0.0}
    else:
        bar = dict(bar, high=max(bar["high"], price), low=min(bar["low"], price), close=price)
    if not skip_volume:
        # Coinbase sizes have 8 decimal places. Rounding removes the noise that adding decimals leaves.
        bar["volume"] = round((bar["volume"] or 0.0) + size, 8)
    return bar


def _validated_symbols(symbols):
    cleaned = []
    for symbol in symbols:
        symbol = str(symbol).strip().upper()
        if not PAIR.match(symbol):
            raise history.BadRequest(f"{symbol or 'That'} is not a Coinbase pair. Pairs are written like BTC-USD.")
        if symbol not in cleaned:
            cleaned.append(symbol)
    if len(cleaned) > MAX_WATCH:
        raise history.BadRequest(f"The watchlist can hold up to {MAX_WATCH} pairs.")
    return cleaned


def _new_session():
    return {"buy": 0.0, "sell": 0.0, "pv": 0.0, "v": 0.0, "n": 0, "times": deque(), "tape": deque(maxlen=TAPE_KEPT), "since": int(time.time() * 1000), "last": None}


class Live:
    def __init__(self, emit, rest, feed_class, seed=True):
        self.emit = emit
        self.rest = rest
        self.feed_class = feed_class
        self.seed_enabled = seed
        self.lock = threading.Lock()
        self.stop_event = None
        self.feed = None
        self.threads = []
        self.watch = []
        self._reset()

    def _reset(self):
        self.tickers = {}
        self.changed = set()
        self.status = None
        self.status_sent = 0.0
        self.was_connected = False
        self.restored = False
        self.chart = None
        self.selected = None
        self.sessions = {}
        self.session_changed = False
        self.book = None
        self.book_changed = False
        self.count = 0
        self.lag = None
        self.rate_sent = time.monotonic()
        self.seed_queue = deque()

    @property
    def running(self):
        return self.stop_event is not None

    def start(self, symbols):
        symbols = _validated_symbols(symbols)
        if self.running:
            self.set_watch(symbols)
            return
        self._reset()
        self.watch = symbols
        self.stop_event = threading.Event()
        self.feed = self.feed_class(self.on_message, self.stop_event)
        self.feed.set_watch(symbols)
        self.seed_queue.extend(symbols)
        self.threads = [threading.Thread(target=self.feed.run, name="rq-feed", daemon=True),
                        threading.Thread(target=self._flush_loop, name="rq-flush", daemon=True)]
        if self.seed_enabled:
            self.threads.append(threading.Thread(target=self._seed_loop, name="rq-seed", daemon=True))
        for thread in self.threads:
            thread.start()

    def stop(self):
        if not self.running:
            return
        self.stop_event.set()
        deadline = time.monotonic() + STOP_TIMEOUT_SECONDS
        for thread in self.threads:
            thread.join(max(0.0, deadline - time.monotonic()))
        self.stop_event = None
        self.feed = None
        self.threads = []
        self.emit(json.dumps({"type": "live", "stopped": True}))

    def set_watch(self, symbols):
        symbols = _validated_symbols(symbols)
        with self.lock:
            self.seed_queue.extend(s for s in symbols if s not in self.watch)
            self.watch = symbols
        if self.running:
            self.feed.set_watch(symbols)
        return symbols

    def follow(self, symbol):
        """Chooses the pair whose book, tape and session figures reach the window. None for a market symbol."""
        with self.lock:
            self.selected = symbol
            self.book = None
            self.book_changed = False
            self.session_changed = symbol is not None and symbol in self.sessions
        if self.running:
            self.feed.set_book(symbol)

    def set_chart(self, symbol, interval):
        """Loads bars for the chart. Trades arriving meanwhile are kept, then replayed onto them."""
        symbol = _validated_symbols([symbol])[0]
        step = history.COINBASE_STEPS.get(interval)
        if step is None:
            raise history.BadRequest("Coinbase does not offer that timeframe.")
        with self.lock:
            self.chart = {"symbol": symbol, "interval": interval, "step": step, "last": None, "pending": [], "fetched_ms": 0, "changed": False}
        bars = self.rest.candles(symbol, step)
        with self.lock:
            chart = self.chart
            chart["last"] = bars[-1]
            chart["fetched_ms"] = time.time() * 1000
            for trade in chart.pop("pending"):
                self._chart_trade(chart, trade, trade[0] <= chart["fetched_ms"])
            # The result carries any replayed trades, so the window starts from the forming bar.
            bars[-1:] = [chart["last"]] if chart["last"]["time"] == bars[-1]["time"] else [bars[-1], chart["last"]]
            chart["changed"] = False
        return {"symbol": symbol, "interval": interval, "bars": bars}

    def clear_chart(self):
        with self.lock:
            self.chart = None

    def _chart_trade(self, chart, trade, skip_volume):
        updated = apply_trade(chart["last"], trade[0], trade[1], trade[2], chart["step"], skip_volume)
        if updated is not None:
            chart["last"] = updated
            chart["changed"] = True

    def _record_trade(self, message):
        session = self.sessions.setdefault(message["id"], _new_session())
        size, price = message["size"], message["price"]
        session["buy" if message["side"] == "buy" else "sell"] += size
        session["pv"] += price * size
        session["v"] += size
        session["n"] += 1
        session["times"].append(message["time"])
        session["tape"].appendleft({"t": message["time"], "p": price, "s": size, "side": message["side"]})
        if message["id"] == self.selected:
            self.session_changed = True

    def on_message(self, message):
        """Called on the feed thread for every message."""
        stop = self.stop_event
        if stop is None or stop.is_set():
            raise FeedStopped()
        with self.lock:
            kind = message.get("type")
            if kind == "cs":
                if message["connected"] and not self.was_connected and self.status is not None:
                    self.restored = True
                self.was_connected = message["connected"]
                self.status = message
                self.status_sent = 0.0
                return
            self.count += 1
            if kind == "cb":
                if message["id"] == self.selected:
                    self.book = message
                    self.book_changed = True
            elif kind == "ct":
                self.tickers[message["id"]] = message
                self.changed.add(message["id"])
                self.lag = max(0, time.time() * 1000 - message["time"])
                self.sessions.setdefault(message["id"], _new_session())["last"] = message["price"]
                if "trade" in message:
                    self._record_trade(message)
                chart = self.chart
                if chart is not None and message["id"] == chart["symbol"] and "trade" in message:
                    trade = (message["time"], message["price"], message["size"])
                    if chart["last"] is None:
                        chart["pending"].append(trade)
                    else:
                        self._chart_trade(chart, trade, False)

    def _session_summary(self, symbol):
        session = self.sessions[symbol]
        cutoff = time.time() * 1000 - 60000
        while session["times"] and session["times"][0] < cutoff:
            session["times"].popleft()
        total = session["buy"] + session["sell"]
        return {"id": symbol, "vwap": session["pv"] / session["v"] if session["v"] else None, "last": session["last"],
                "buyPercent": session["buy"] / total * 100 if total else None, "tradesPerMinute": len(session["times"]),
                "trades": session["n"], "since": session["since"], "tape": list(session["tape"])[:TAPE_SENT]}

    def flush(self):
        """Sends what changed since the last call, as one message. Returns it, for tests."""
        now = time.monotonic()
        with self.lock:
            message = {"type": "live"}
            if self.changed:
                message["tickers"] = [self.tickers[symbol] for symbol in sorted(self.changed)]
                self.changed = set()
            chart = self.chart
            if chart is not None and chart.get("last") is not None and chart["changed"]:
                message["bar"] = dict(chart["last"], symbol=chart["symbol"], interval=chart["interval"])
                chart["changed"] = False
            if self.restored:
                message["restored"] = True
                self.restored = False
            if self.book_changed and self.book is not None:
                top = dict(self.book, bids=self.book["bids"][:BOOK_LEVELS], asks=self.book["asks"][:BOOK_LEVELS])
                message["book"] = top
                self.book_changed = False
            if self.selected is not None and self.selected in self.sessions and (self.session_changed or now - self.status_sent >= STATUS_SECONDS):
                message["session"] = self._session_summary(self.selected)
                self.session_changed = False
            if now - self.rate_sent >= RATE_SECONDS:
                message["rate"] = {"perSecond": round(self.count / (now - self.rate_sent)), "lag": self.lag}
                self.count = 0
                self.rate_sent = now
            if self.status is not None and (len(message) > 1 or now - self.status_sent >= STATUS_SECONDS):
                message["status"] = self.status
                self.status_sent = now
        if len(message) == 1:
            return None
        self.emit(json.dumps(message, allow_nan=False))
        return message

    def _flush_loop(self):
        stop = self.stop_event
        while not stop.wait(FLUSH_SECONDS):
            self.flush()

    def seed_one(self, symbol):
        """Sends a pair's 24-hour figures and sparkline, which the live ticker doesn't carry at first."""
        seed = {"id": symbol}
        try:
            seed.update(self.rest.stats(symbol))
        except Exception:
            pass  # The live ticker fills these in.
        try:
            seed["spark"] = [bar["close"] for bar in self.rest.candles(symbol, SPARK_GRANULARITY)[-SPARK_POINTS:]]
        except Exception:
            pass  # The sparkline stays empty until the next round.
        if len(seed) > 1:
            self.emit(json.dumps({"type": "live", "seed": seed}, allow_nan=False))

    def seed_fear_greed(self):
        try:
            self.emit(json.dumps({"type": "live", "fearGreed": self.rest.fear_greed()}, allow_nan=False))
        except Exception:
            self.emit(json.dumps({"type": "live", "fearGreed": None}))

    def _seed_loop(self):
        stop = self.stop_event
        next_all = time.monotonic() + RESEED_SECONDS
        next_fear_greed = 0.0
        while not stop.is_set():
            now = time.monotonic()
            if now >= next_fear_greed:
                self.seed_fear_greed()
                next_fear_greed = now + FEAR_GREED_SECONDS
            if now >= next_all:
                with self.lock:
                    self.seed_queue.extend(self.watch)
                next_all = now + RESEED_SECONDS
            with self.lock:
                symbol = self.seed_queue.popleft() if self.seed_queue else None
            if symbol is None:
                stop.wait(0.5)
            else:
                self.seed_one(symbol)


class _FixtureFeed:
    """Synthetic messages for tests, with no network. The UI has no way to request this feed."""

    instance = None

    def __init__(self, publish, stop):
        self.publish = publish
        self.stop_event = stop
        self.watch = []
        self.book_symbol = None
        self.lock = threading.Lock()
        self.drop_requested = False
        self.trade = 0
        _FixtureFeed.instance = self

    def set_watch(self, symbols):
        with self.lock:
            self.watch = list(symbols)

    def set_book(self, symbol):
        with self.lock:
            self.book_symbol = symbol

    def _status(self, connected):
        return {"type": "cs", "connected": connected, "lastMessage": int(time.time() * 1000), "retryIn": 0 if connected else 1, "reconnects": 0, "rejected": 0}

    def run(self):
        try:
            self.publish(self._status(True))
            while not self.stop_event.wait(0.02):
                if self.drop_requested:
                    self.drop_requested = False
                    self.publish(self._status(False))
                    self.stop_event.wait(0.2)
                    self.publish(self._status(True))
                    continue
                self.trade += 1
                with self.lock:
                    symbols, book = list(self.watch), self.book_symbol
                price = 100.0 + self.trade % 7
                for symbol in symbols:
                    self.publish({"type": "ct", "id": symbol, "price": price, "time": int(time.time() * 1000), "open": 100.0, "high": 110.0, "low": 90.0,
                                  "vol": 5.0, "bid": price - 0.5, "ask": price + 0.5, "size": 1.0, "trade": self.trade, "side": "buy" if self.trade % 3 else "sell"})
                if book:
                    self.publish({"type": "cb", "id": book, "depth": 40, "crossed": False, "time": int(time.time() * 1000),
                                  "bids": [[price - 0.5 - i, 2.0 + i] for i in range(12)], "asks": [[price + 0.5 + i, 1.0 + i] for i in range(12)]})
                if self.trade % 25 == 0:
                    self.publish(self._status(True))
        except FeedStopped:
            pass


class _FixtureRest:
    calls = 0

    def get(self, url):
        return {}

    def stats(self, symbol):
        if symbol == "NOPE-USD":
            raise ProviderError(404, "Coinbase does not list that symbol.")
        return {"last": 100.0, "open": 98.0, "high": 110.0, "low": 90.0, "volume": 5.0}

    def fear_greed(self):
        return {"value": 42, "classification": "Fear", "time": 0}

    def candles(self, symbol, step):
        _FixtureRest.calls += 1
        if symbol == "FAIL-USD":
            raise ProviderError(502, "Could not reach the provider.")
        newest = math.floor(time.time() / step) * step
        bars = []
        for index in range(50, -1, -1):
            value = 100.0 + (index % 5)
            bars.append({"time": newest - index * step, "open": value, "high": value + 2.0, "low": value - 2.0, "close": value + 1.0, "volume": 10.0})
        return bars


class _FixtureProvider:
    """Synthetic Yahoo answers for tests."""

    @staticmethod
    def quote(symbol):
        if symbol == "NOPE":
            raise ValueError("no price")
        return 200.0, {"prev": 190.0, "high": 205.0, "low": 185.0, "vol": 1000.0, "yearHigh": 300.0, "yearLow": 100.0, "cap": 5e9, "cur": "USD"}

    @staticmethod
    def meta(_symbol):
        return {"name": "Test Company", "state": "REGULAR", "delay": 15, "exchange": "Test Exchange", "spark": [1.0, 3.0, 2.0, 4.0]}

    @staticmethod
    def candles(symbol, _name):
        return _FixtureRest().candles(symbol, 3600)

    @staticmethod
    def profile(_symbol):
        return {"name": "Test Company", "exchange": "Test Exchange", "sector": "Testing", "forwardPe": 12.5, "pe": 20.0, "beta": 1.5, "avgVolume": 2000.0, "eps": 3.0, "dividend": 1.2}

    @staticmethod
    def news(_symbol):
        return [{"title": "Test <story>", "url": "https://example.com/story", "time": "2026-01-01T00:00:00Z", "publisher": "Test News"}]

    @staticmethod
    def limited():
        return False


def _emit(text):
    _rq_host.publish_event(text)


_live = Live(_emit, market_history._rest, StoppableFeed)
_markets = markets.Markets(_emit)


def start(args, _host):
    _markets.start(args.get("markets", []))
    _live.start(args.get("crypto", []))
    _live.follow(args.get("selected") or None)
    return {"running": True}


def start_fixture_for_test(args, host):
    global _live, _markets
    shutdown()
    _live = Live(_emit, _FixtureRest(), _FixtureFeed)
    _markets = markets.Markets(_emit, _FixtureProvider)
    return start(args, host)


def drop_fixture_for_test(_args, _host):
    _FixtureFeed.instance.drop_requested = True
    return {}


def fixture_rest_calls_for_test(_args, _host):
    return {"calls": _FixtureRest.calls}


def select(args, _host):
    _live.follow(args.get("crypto") or None)
    if not args.get("crypto"):
        _live.clear_chart()
    return {}


def watch(args, _host):
    _markets.set_watch(args.get("markets", []))
    return {"crypto": _live.set_watch(args.get("crypto", []))}


def resolve(args, _host):
    """Checks a symbol exists before it joins the watchlist, so a bad one can't wait forever for a price."""
    symbol = str(args.get("symbol", "")).strip().upper()
    kind = args.get("kind", "auto")
    pair = symbol if "-" in symbol else symbol + "-USD"
    if kind in ("auto", "crypto") and PAIR.match(pair):
        try:
            _live.rest.stats(pair)
            return {"kind": "crypto", "symbol": pair}
        except ProviderError:
            if kind == "crypto":
                raise
    if kind == "crypto" or not market_history.SYMBOL.match(symbol):
        raise history.BadRequest(f"{symbol or 'That'} is not a symbol.")
    try:
        _markets.provider.quote(symbol)
    except ProviderError:
        raise
    except Exception as error:
        raise ProviderError(404, f"Neither Coinbase nor Yahoo lists {symbol}." if kind == "auto" else f"Yahoo has no price for {symbol}.") from error
    return {"kind": "market", "symbol": symbol}


def chart(args, _host):
    return _live.set_chart(args.get("symbol", ""), str(args.get("interval", "")).upper())


def market_chart(args, _host):
    symbol, interval = args.get("symbol", ""), str(args.get("interval", "")).upper()
    if interval not in markets.PERIODS:
        raise history.BadRequest("Yahoo does not offer that timeframe.")
    return {"symbol": symbol, "interval": interval, "bars": _markets.provider.candles(symbol, interval)}


def market_profile(args, _host):
    return {"symbol": args.get("symbol", ""), "profile": _markets.provider.profile(args.get("symbol", ""))}


def market_news(args, _host):
    return {"symbol": args.get("symbol", ""), "items": _markets.provider.news(args.get("symbol", ""))}


def shutdown(*_):
    """Called just before the interpreter closes, and whenever background work must stop."""
    _markets.stop()
    _live.stop()
    return {}
