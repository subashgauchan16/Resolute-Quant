"""Backend-owned crypto and sentiment data for Resolute Quant.

Coinbase Exchange public data and the alternative.me Fear & Greed index reach the page only
through this module. It holds no credentials and never places orders.

Everything pushed to the page is validated first. Bad values are dropped and counted, never
repaired or invented.
"""
from __future__ import annotations

import json
import math
import random
import re
import threading
import time
import urllib.error
import urllib.request
from bisect import bisect_left, insort
from datetime import datetime

from websockets.sync.client import connect as ws_connect

REST = "https://api.exchange.coinbase.com"
WS = "wss://ws-feed.exchange.coinbase.com"
FEAR_GREED = "https://api.alternative.me/fng/?limit=1"
HEADERS = {"User-Agent": "ResoluteQuant/1.1", "Accept": "application/json"}

PAIR = re.compile(r"^[A-Z0-9]{1,12}-[A-Z0-9]{2,6}$")
GRANULARITIES = {60, 300, 900, 3600, 21600, 86400}  # the only ones Coinbase serves
MIN_REST_GAP = 0.13        # polite gap between Coinbase REST requests
MAX_WATCH = 40
BOOK_LEVELS = 10
BOOK_PUSH_SECONDS = 0.25   # at most 4 book updates a second
STATUS_SECONDS = 2
SILENCE_SECONDS = 12       # replace the socket after this long without a message
MAX_FUTURE_SECONDS = 60    # reject timestamps further ahead than clock drift explains
HEARTBEAT_SYMBOL = "BTC-USD"


class ProviderError(Exception):
    """A provider refused or failed. `status` is the HTTP status to hand to the page."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def finite(value):
    """Return a finite float, or None for anything else."""
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def positive(value):
    number = finite(value)
    return number if number is not None and number > 0 else None


def parse_time_ms(text, now=None):
    """Coinbase ISO timestamp to epoch ms. None if unparsable or implausibly in the future."""
    if not isinstance(text, str):
        return None
    trimmed = re.sub(r"(\.\d{6})\d*", r"\1", text).replace("Z", "+00:00")
    try:
        stamp = datetime.fromisoformat(trimmed)
    except ValueError:
        return None
    if stamp.tzinfo is None:
        return None
    ms = int(stamp.timestamp() * 1000)
    now = time.time() if now is None else now
    return ms if ms <= (now + MAX_FUTURE_SECONDS) * 1000 else None


def normalise_ticker(raw, now=None):
    """Coinbase ticker message to the page's `ct` message. None if a required field is bad."""
    symbol, price, stamp = raw.get("product_id"), positive(raw.get("price")), parse_time_ms(raw.get("time"), now)
    if not isinstance(symbol, str) or not PAIR.match(symbol) or price is None or stamp is None:
        return None
    message = {"type": "ct", "id": symbol, "price": price, "time": stamp,
               "open": positive(raw.get("open_24h")), "high": positive(raw.get("high_24h")),
               "low": positive(raw.get("low_24h")), "vol": finite(raw.get("volume_24h")),
               "bid": positive(raw.get("best_bid")), "ask": positive(raw.get("best_ask"))}
    size, trade, side = positive(raw.get("last_size")), raw.get("trade_id"), raw.get("side")
    if size is not None and trade is not None and side in ("buy", "sell"):
        message.update(size=size, trade=trade, side=side)
    return message


def normalise_candles(rows, now=None):
    """Coinbase [time, low, high, open, close, volume] rows to ascending bars.

    Returns (bars, rejected). Rows failing a check are dropped, and duplicates keep the first.
    """
    now = time.time() if now is None else now
    bars, seen, rejected = [], set(), 0
    for row in rows if isinstance(rows, list) else []:
        try:
            stamp, low, high, opened, close, volume = row
        except (TypeError, ValueError):
            rejected += 1
            continue
        stamp = finite(stamp)
        low, high, opened, close = positive(low), positive(high), positive(opened), positive(close)
        volume = finite(volume)
        valid = (stamp is not None and stamp <= now + MAX_FUTURE_SECONDS and None not in (low, high, opened, close)
                 and volume is not None and volume >= 0 and high >= max(opened, close) and low <= min(opened, close))
        if not valid or stamp in seen:
            rejected += 1
            continue
        seen.add(stamp)
        bars.append({"time": int(stamp), "open": opened, "high": high, "low": low, "close": close, "volume": volume})
    bars.sort(key=lambda bar: bar["time"])
    return bars, rejected


def normalise_stats(raw):
    last = positive(raw.get("last")) if isinstance(raw, dict) else None
    if last is None:
        raise ProviderError(502, "Coinbase returned no price for that symbol.")
    return {"last": last, "open": positive(raw.get("open")), "high": positive(raw.get("high")),
            "low": positive(raw.get("low")), "volume": finite(raw.get("volume"))}


def normalise_fear_greed(raw):
    try:
        item = raw["data"][0]
        value, label = int(item["value"]), item["value_classification"]
    except (KeyError, IndexError, TypeError, ValueError) as error:
        raise ProviderError(502, "The Fear & Greed index returned an unexpected answer.") from error
    if not 0 <= value <= 100 or not isinstance(label, str):
        raise ProviderError(502, "The Fear & Greed index returned an unexpected answer.")
    return {"value": value, "classification": label, "time": int(finite(item.get("timestamp")) or 0)}


class Book:
    """Full level 2 book for one symbol. Prices stay sorted, so reads of the top are cheap."""

    def __init__(self, symbol, snapshot):
        self.symbol = symbol
        self.bids, self.asks = {}, {}
        for side, levels in ((self.bids, snapshot.get("bids", [])), (self.asks, snapshot.get("asks", []))):
            for level in levels:
                price, size = positive(level[0]), positive(level[1])
                if price is not None and size is not None:
                    side[price] = size
        self.bid_prices = sorted(self.bids)   # ascending, best bid last
        self.ask_prices = sorted(self.asks)   # ascending, best ask first

    def apply(self, side, price, size):
        """Apply one level change. Returns False if the values are not usable."""
        price, size = positive(price), finite(size)
        if price is None or size is None or size < 0 or side not in ("buy", "sell"):
            return False
        levels, prices = (self.bids, self.bid_prices) if side == "buy" else (self.asks, self.ask_prices)
        if size == 0:
            if price in levels:
                del levels[price]
                del prices[bisect_left(prices, price)]
        else:
            if price not in levels:
                insort(prices, price)
            levels[price] = size
        return True

    def message(self, levels=BOOK_LEVELS):
        bids = [[price, self.bids[price]] for price in reversed(self.bid_prices[-levels:])]
        asks = [[price, self.asks[price]] for price in self.ask_prices[:levels]]
        return {"type": "cb", "id": self.symbol, "bids": bids, "asks": asks,
                "depth": len(self.bids) + len(self.asks),
                "crossed": bool(bids and asks and bids[0][0] >= asks[0][0]), "time": int(time.time() * 1000)}


class Rest:
    """Coinbase REST and Fear & Greed requests, one at a time, with a polite gap."""

    def __init__(self):
        self.lock = threading.Lock()
        self.last = 0.0
        self.rejected = 0

    def get(self, url):
        with self.lock:
            gap = MIN_REST_GAP - (time.time() - self.last)
            if gap > 0:
                time.sleep(gap)
            self.last = time.time()
            request = urllib.request.Request(url, headers=HEADERS)
            try:
                with urllib.request.urlopen(request, timeout=10) as response:
                    return json.load(response)
            except urllib.error.HTTPError as error:
                if error.code == 404:
                    raise ProviderError(404, "Coinbase does not list that symbol.") from error
                if error.code == 429:
                    raise ProviderError(429, "The provider is rate limiting this PC. It will retry shortly.") from error
                raise ProviderError(502, f"The provider answered {error.code}.") from error
            except (urllib.error.URLError, TimeoutError, OSError, ValueError) as error:
                raise ProviderError(502, "Could not reach the provider.") from error

    def candles(self, symbol, granularity):
        bars, rejected = normalise_candles(self.get(f"{REST}/products/{symbol}/candles?granularity={granularity}"))
        self.rejected += rejected
        if not bars:
            raise ProviderError(502, "Coinbase returned no candles for that request.")
        return bars

    def stats(self, symbol):
        return normalise_stats(self.get(f"{REST}/products/{symbol}/stats"))

    def fear_greed(self):
        return normalise_fear_greed(self.get(FEAR_GREED))


class CoinbaseFeed:
    """One upstream WebSocket, with reconnects, pushing normalised messages through `publish`."""

    def __init__(self, publish):
        self.publish = publish
        self.lock = threading.Lock()
        self.watch = set()
        self.book_symbol = None
        self.book = None
        self.connected = False
        self.last_message = 0.0
        self.retry_at = 0.0
        self.reconnects = 0
        self.rejected = 0
        self.last_trade = {}

    def set_watch(self, symbols):
        with self.lock:
            self.watch = {s for s in symbols if PAIR.match(s)}
            return sorted(self.watch)

    def set_book(self, symbol):
        with self.lock:
            self.book_symbol = symbol if symbol and PAIR.match(symbol) else None
            if self.book and self.book.symbol != self.book_symbol:
                self.book = None

    def status(self):
        now = time.time()
        return {"type": "cs", "connected": self.connected, "lastMessage": int(self.last_message * 1000),
                "retryIn": max(0, round(self.retry_at - now, 1)), "reconnects": self.reconnects, "rejected": self.rejected}

    def _channels(self, wanted_watch, wanted_book, subscribed_watch, subscribed_book, socket):
        """Bring the upstream subscriptions in line with what the page asked for."""
        added, removed = wanted_watch - subscribed_watch, subscribed_watch - wanted_watch
        if added:
            socket.send(json.dumps({"type": "subscribe", "channels": [{"name": "ticker", "product_ids": sorted(added)}]}))
        if removed:
            socket.send(json.dumps({"type": "unsubscribe", "channels": [{"name": "ticker", "product_ids": sorted(removed)}]}))
        if wanted_book != subscribed_book:
            if subscribed_book:
                socket.send(json.dumps({"type": "unsubscribe", "channels": [{"name": "level2_batch", "product_ids": [subscribed_book]}]}))
            if wanted_book:
                socket.send(json.dumps({"type": "subscribe", "channels": [{"name": "level2_batch", "product_ids": [wanted_book]}]}))

    def _handle(self, raw):
        kind = raw.get("type")
        if kind == "ticker":
            message = normalise_ticker(raw)
            if message is None:
                self.rejected += 1
                return
            trade = message.get("trade")
            if trade is not None and self.last_trade.get(message["id"]) == trade:
                for field in ("trade", "size", "side"):
                    message.pop(field, None)  # same trade repeated by the feed
            elif trade is not None:
                self.last_trade[message["id"]] = trade
            self.publish(message)
        elif kind == "snapshot":
            with self.lock:
                if raw.get("product_id") == self.book_symbol:
                    self.book = Book(self.book_symbol, raw)
        elif kind == "l2update":
            with self.lock:
                if self.book and raw.get("product_id") == self.book.symbol:
                    for change in raw.get("changes", []):
                        if len(change) != 3 or not self.book.apply(change[0], change[1], change[2]):
                            self.rejected += 1
        elif kind == "error":
            print(f"Coinbase feed message: {str(raw.get('message'))[:120]}")

    def _session(self):
        with ws_connect(WS, open_timeout=10, close_timeout=2, max_size=64 * 2 ** 20) as socket:
            socket.send(json.dumps({"type": "subscribe", "channels": [{"name": "heartbeat", "product_ids": [HEARTBEAT_SYMBOL]}]}))
            self.connected, self.last_message = True, time.time()
            self.publish(self.status())
            watched, booked, pushed, status_at = set(), None, 0.0, 0.0
            while True:
                with self.lock:
                    wanted_watch, wanted_book = set(self.watch), self.book_symbol
                if wanted_watch != watched or wanted_book != booked:
                    self._channels(wanted_watch, wanted_book, watched, booked, socket)
                    watched, booked = wanted_watch, wanted_book
                try:
                    data = socket.recv(timeout=0.25)
                    self.last_message = time.time()
                    self._handle(json.loads(data))
                except TimeoutError:
                    pass
                now = time.time()
                if now - self.last_message > SILENCE_SECONDS:
                    raise TimeoutError("feed silent")
                if now - pushed >= BOOK_PUSH_SECONDS:
                    pushed = now
                    with self.lock:
                        message = self.book.message() if self.book else None
                    if message:
                        self.publish(message)
                if now - status_at >= STATUS_SECONDS:
                    status_at = now
                    self.publish(self.status())

    def run(self):
        """Stay connected forever, backing off up to 30 seconds with jitter between attempts."""
        attempt = 0
        while True:
            started = time.time()
            try:
                self._session()
            except Exception as error:
                print(f"Coinbase feed dropped ({type(error).__name__}).")
            self.connected = False
            with self.lock:
                self.book = None
            attempt = 0 if time.time() - started > 60 else attempt + 1
            delay = min(30, 2 ** attempt) + random.random() * 0.4
            self.reconnects += 1
            self.retry_at = time.time() + delay
            self.publish(self.status())
            time.sleep(delay)
