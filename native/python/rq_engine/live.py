"""Live Coinbase prices for the native app, reusing the browser terminal's feed and validation.

The feed runs on its own Python thread. Every trade updates the forming bar, so highs and lows are
exact, but the window is told at most five times a second. The bar rules match the browser terminal:
a new bar opens at its first trade, not at the last close, and bars are reloaded after a reconnect.
"""

import json
import math
import random
import threading
import time

import _rq_host
import history
from crypto_feed import MAX_WATCH, PAIR, REST, CoinbaseFeed, ProviderError

from . import market_history

FLUSH_SECONDS = 0.2
STATUS_SECONDS = 2.0
STOP_TIMEOUT_SECONDS = 2.0


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


class Live:
    def __init__(self, emit, rest, feed_class):
        self.emit = emit
        self.rest = rest
        self.feed_class = feed_class
        self.lock = threading.Lock()
        self.stop_event = None
        self.feed = None
        self.threads = []
        self._reset()

    def _reset(self):
        self.tickers = {}
        self.changed = set()
        self.status = None
        self.status_sent = 0.0
        self.was_connected = False
        self.restored = False
        self.chart = None

    @property
    def running(self):
        return self.stop_event is not None

    def start(self, symbols):
        symbols = _validated_symbols(symbols)
        if self.running:
            self.feed.set_watch(symbols)
            return
        self._reset()
        self.stop_event = threading.Event()
        self.feed = self.feed_class(self.on_message, self.stop_event)
        self.feed.set_watch(symbols)
        self.threads = [threading.Thread(target=self.feed.run, name="rq-feed", daemon=True),
                        threading.Thread(target=self._flush_loop, name="rq-flush", daemon=True)]
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
        if self.running:
            self.feed.set_watch(symbols)
        return symbols

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

    def _chart_trade(self, chart, trade, skip_volume):
        updated = apply_trade(chart["last"], trade[0], trade[1], trade[2], chart["step"], skip_volume)
        if updated is not None:
            chart["last"] = updated
            chart["changed"] = True

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
            elif kind == "ct":
                self.tickers[message["id"]] = message
                self.changed.add(message["id"])
                chart = self.chart
                if chart is not None and message["id"] == chart["symbol"] and "trade" in message:
                    trade = (message["time"], message["price"], message["size"])
                    if chart["last"] is None:
                        chart["pending"].append(trade)
                    else:
                        self._chart_trade(chart, trade, False)

    def flush(self):
        """Sends what changed since the last call, as one message. Returns it, for tests."""
        now = time.monotonic()
        with self.lock:
            message = {"type": "live"}
            if self.changed:
                message["tickers"] = [self.tickers[symbol] for symbol in sorted(self.changed)]
                self.changed = set()
            chart = self.chart
            if chart is not None and chart["last"] is not None and chart["changed"]:
                message["bar"] = dict(chart["last"], symbol=chart["symbol"], interval=chart["interval"])
                chart["changed"] = False
            if self.restored:
                message["restored"] = True
                self.restored = False
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


class _FixtureFeed:
    """Synthetic messages for tests, with no network. The UI has no way to request this feed."""

    instance = None

    def __init__(self, publish, stop):
        self.publish = publish
        self.stop_event = stop
        self.watch = []
        self.lock = threading.Lock()
        self.drop_requested = False
        self.trade = 0
        _FixtureFeed.instance = self

    def set_watch(self, symbols):
        with self.lock:
            self.watch = list(symbols)

    def _status(self, connected):
        return {"type": "cs", "connected": connected, "lastMessage": int(time.time() * 1000), "retryIn": 0 if connected else 1, "reconnects": 0, "rejected": 0}

    def run(self):
        connected = True
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
                    symbols = list(self.watch)
                for symbol in symbols:
                    price = 100.0 + self.trade % 7
                    self.publish({"type": "ct", "id": symbol, "price": price, "time": int(time.time() * 1000), "open": 100.0, "high": 110.0, "low": 90.0,
                                  "vol": 5.0, "bid": price - 0.5, "ask": price + 0.5, "size": 1.0, "trade": self.trade, "side": "buy"})
                if self.trade % 25 == 0:
                    self.publish(self._status(connected))
        except FeedStopped:
            pass


class _FixtureRest:
    calls = 0

    def get(self, url):
        if url.endswith("NOPE-USD"):
            raise ProviderError(404, "Coinbase does not list that symbol.")
        return {}

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


def _emit(text):
    _rq_host.publish_event(text)


_live = Live(_emit, market_history._rest, StoppableFeed)


def start(args, _host):
    _live.start(args.get("symbols", []))
    return {"running": True}


def start_fixture_for_test(args, _host):
    global _live
    _live.stop()
    _live = Live(_emit, _FixtureRest(), _FixtureFeed)
    _live.start(args.get("symbols", []))
    return {"running": True}


def drop_fixture_for_test(_args, _host):
    _FixtureFeed.instance.drop_requested = True
    return {}


def fixture_rest_calls_for_test(_args, _host):
    return {"calls": _FixtureRest.calls}


def stop(_args, _host):
    _live.stop()
    return {"running": False}


def watch(args, _host):
    return {"symbols": _live.set_watch(args.get("symbols", []))}


def add_symbol(args, _host):
    """Checks a pair exists before it joins the watchlist, so a bad one can't wait forever for a price."""
    symbols = _validated_symbols(list(args.get("symbols", [])) + [args.get("symbol", "")])
    added = _validated_symbols([args.get("symbol", "")])[0]
    _live.rest.get(f"{REST}/products/{added}")
    return {"symbols": _live.set_watch(symbols), "added": added}


def chart(args, _host):
    return _live.set_chart(args.get("symbol", ""), str(args.get("interval", "")).upper())


def shutdown():
    """Called just before the interpreter closes."""
    _live.stop()
