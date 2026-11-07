"""Tests the live feed logic on the private Python, with a fake socket and fake provider. No network is used."""

import json
import sys
import threading
import time
import types
import unittest
from pathlib import Path
from unittest import mock

NATIVE = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(NATIVE / "python"), str(NATIVE.parent)]
sys.modules["_rq_host"] = types.SimpleNamespace(report_progress=lambda *_: None, is_cancelled=lambda: False, publish_event=lambda _: None)

from crypto_feed import ProviderError  # noqa: E402
from rq_engine import live  # noqa: E402
from rq_engine.market_history import history  # noqa: E402

STEP = 60
T0 = 1_800_000_000 - 1_800_000_000 % STEP


def bar(time_s=T0, opened=10.0, high=12.0, low=9.0, close=11.0, volume=5.0):
    return {"time": time_s, "open": opened, "high": high, "low": low, "close": close, "volume": volume}


def tick(symbol="BTC-USD", price=100.0, time_ms=None, size=1.0, trade=1):
    message = {"type": "ct", "id": symbol, "price": price, "time": time_ms if time_ms is not None else T0 * 1000, "open": 90.0, "bid": price - 1, "ask": price + 1}
    if trade is not None:
        message.update(size=size, trade=trade, side="buy")
    return message


def status(connected=True):
    return {"type": "cs", "connected": connected, "lastMessage": 1, "retryIn": 0, "reconnects": 0, "rejected": 0}


class ApplyTrade(unittest.TestCase):
    def test_a_trade_in_the_current_bar_extends_it(self):
        updated = live.apply_trade(bar(), (T0 + 5) * 1000, 13.0, 2.0, STEP)
        self.assertEqual(updated, bar(high=13.0, close=13.0, volume=7.0))
        updated = live.apply_trade(bar(), (T0 + 5) * 1000, 8.0, 1.0, STEP)
        self.assertEqual(updated["low"], 8.0)
        self.assertEqual(updated["high"], 12.0)

    def test_a_new_bar_opens_at_its_first_trade_and_not_the_last_close(self):
        updated = live.apply_trade(bar(close=11.0), (T0 + STEP + 1) * 1000, 50.0, 2.0, STEP)
        self.assertEqual(updated, bar(T0 + STEP, 50.0, 50.0, 50.0, 50.0, 2.0))

    def test_an_older_trade_is_ignored(self):
        self.assertIsNone(live.apply_trade(bar(), (T0 - 1) * 1000, 50.0, 2.0, STEP))

    def test_volume_already_in_the_history_is_not_counted_twice(self):
        updated = live.apply_trade(bar(), T0 * 1000, 11.5, 2.0, STEP, skip_volume=True)
        self.assertEqual(updated["volume"], 5.0)
        self.assertEqual(updated["close"], 11.5)

    def test_volume_has_no_rounding_noise(self):
        updated = bar(volume=0.1)
        for _ in range(2):
            updated = live.apply_trade(updated, T0 * 1000, 11.0, 0.1, STEP)
        self.assertEqual(updated["volume"], 0.3)

    def test_the_input_bar_is_left_unchanged(self):
        original = bar()
        live.apply_trade(original, T0 * 1000, 99.0, 1.0, STEP)
        self.assertEqual(original, bar())


class FakeRest:
    def __init__(self, bars=None, error=None, hook=None):
        self.bars = bars if bars is not None else [bar(T0 - STEP), bar(T0)]
        self.error = error
        self.hook = hook
        self.urls = []

    def candles(self, _symbol, _step):
        if self.hook:
            self.hook()
        if self.error:
            raise self.error
        return [dict(b) for b in self.bars]

    def get(self, url):
        self.urls.append(url)
        return {}


class FakeFeed:
    instances = []

    def __init__(self, publish, stop):
        self.publish = publish
        self.stop_event = stop
        self.watch = None
        FakeFeed.instances.append(self)

    def set_watch(self, symbols):
        self.watch = symbols

    def run(self):
        self.stop_event.wait()


class Base(unittest.TestCase):
    def make(self, rest=None):
        self.sent = []
        self.live = live.Live(lambda text: self.sent.append(json.loads(text)), rest or FakeRest(), FakeFeed)
        self.addCleanup(self.live.stop)
        return self.live


class Flow(Base):
    def setUp(self):
        self.make()
        patcher = mock.patch.object(live, "FLUSH_SECONDS", 3600)  # Tests call flush() themselves.
        patcher.start()
        self.addCleanup(patcher.stop)
        self.live.start(["btc-usd", "ETH-USD", "BTC-USD"])
        self.feed = FakeFeed.instances[-1]

    def test_start_cleans_the_watchlist(self):
        self.assertEqual(self.feed.watch, ["BTC-USD", "ETH-USD"])

    def test_second_start_does_not_open_a_second_connection(self):
        count = len(FakeFeed.instances)
        self.live.start(["SOL-USD"])
        self.assertEqual(len(FakeFeed.instances), count)
        self.assertEqual(self.feed.watch, ["SOL-USD"])

    def test_many_ticks_become_one_message_with_the_latest_price(self):
        for price in (1.0, 2.0, 3.0):
            self.live.on_message(tick(price=price))
        message = self.live.flush()
        self.assertEqual([t["price"] for t in message["tickers"]], [3.0])
        self.assertIsNone(self.live.flush())

    def test_every_trade_counts_towards_the_bar_between_messages(self):
        self.live.set_chart("BTC-USD", "1M")
        for price, trade in ((30.0, 1), (5.0, 2), (11.0, 3)):
            self.live.on_message(tick(price=price, trade=trade))
        message = self.live.flush()
        self.assertEqual(message["bar"]["high"], 30.0)
        self.assertEqual(message["bar"]["low"], 5.0)
        self.assertEqual(message["bar"]["close"], 11.0)
        self.assertEqual(message["bar"]["volume"], 8.0)
        self.assertEqual((message["bar"]["symbol"], message["bar"]["interval"]), ("BTC-USD", "1M"))

    def test_ticks_for_other_pairs_and_ticks_without_a_trade_leave_the_bar_alone(self):
        self.live.set_chart("BTC-USD", "1M")
        self.live.on_message(tick("ETH-USD", price=500.0))
        self.live.on_message(tick(price=500.0, trade=None))
        self.assertNotIn("bar", self.live.flush())

    def test_trades_during_a_chart_load_are_replayed_without_double_counting_volume(self):
        rest = FakeRest(hook=lambda: self.live.on_message(tick(price=99.0, time_ms=int(time.time() * 1000) - 1000)))
        self.live.rest = rest
        rest.bars = [bar(int(time.time()) - int(time.time()) % STEP)]
        result = self.live.set_chart("BTC-USD", "1M")
        newest = result["bars"][-1]
        self.assertEqual(newest["close"], 99.0)
        self.assertEqual(newest["high"], 99.0)
        self.assertEqual(newest["volume"], 5.0)
        self.assertNotIn("bar", self.live.flush() or {})

    def test_a_replayed_trade_in_a_new_bar_is_added_after_the_history(self):
        rest = FakeRest(hook=lambda: self.live.on_message(tick(price=99.0, time_ms=(T0 + STEP) * 1000)))
        self.live.rest = rest
        result = self.live.set_chart("BTC-USD", "1M")
        self.assertEqual([b["time"] for b in result["bars"]], [T0 - STEP, T0, T0 + STEP])
        self.assertEqual(result["bars"][-1]["open"], 99.0)

    def test_failed_chart_load_is_reported_and_later_trades_are_safe(self):
        self.live.rest = FakeRest(error=ProviderError(502, "Could not reach the provider."))
        with self.assertRaises(ProviderError):
            self.live.set_chart("BTC-USD", "1M")
        self.live.on_message(tick())
        self.assertNotIn("bar", self.live.flush())

    def test_a_trade_for_the_old_chart_cant_reach_the_new_one(self):
        self.live.set_chart("BTC-USD", "1M")
        self.live.set_chart("ETH-USD", "5M")
        self.live.on_message(tick("BTC-USD", price=500.0))
        self.assertNotIn("bar", self.live.flush())

    def test_status_is_sent_when_it_changes_and_repeated_only_every_few_seconds(self):
        self.live.on_message(status())
        self.assertTrue(self.live.flush()["status"]["connected"])
        self.assertIsNone(self.live.flush())
        self.live.status_sent = time.monotonic() - live.STATUS_SECONDS - 1
        self.assertIn("status", self.live.flush())

    def test_reconnect_is_flagged_once_so_the_chart_reloads(self):
        self.live.on_message(status(True))
        self.assertNotIn("restored", self.live.flush())  # The first connection isn't a reconnect.
        self.live.on_message(status(False))
        self.assertNotIn("restored", self.live.flush())
        self.live.on_message(status(True))
        self.assertTrue(self.live.flush()["restored"])
        self.live.on_message(status(True))
        self.assertNotIn("restored", self.live.flush())

    def test_stop_ends_threads_and_later_feed_messages_raise(self):
        threads = list(self.live.threads)
        self.live.stop()
        for thread in threads:
            self.assertFalse(thread.is_alive())
        self.assertFalse(self.live.running)
        self.assertTrue(self.sent[-1]["stopped"])
        with self.assertRaises(live.FeedStopped):
            self.live.on_message(tick())
        self.live.stop()  # Stopping twice is harmless.

    def test_restart_after_stop_starts_clean(self):
        self.live.on_message(tick())
        self.live.stop()
        self.live.start(["ETH-USD"])
        self.assertEqual(self.live.tickers, {})
        self.assertIsNot(FakeFeed.instances[-1], self.feed)


class Validation(Base):
    def test_bad_symbols_are_correctable_messages(self):
        self.make()
        for symbols in (["AAPL"], ["="], [""], [f"A{i}-USD" for i in range(live.MAX_WATCH + 1)]):
            with self.subTest(symbols), self.assertRaises(history.BadRequest):
                self.live.start(symbols)
        self.assertFalse(self.live.running)

    def test_bad_timeframe_is_a_correctable_message(self):
        self.make()
        with self.assertRaises(history.BadRequest):
            self.live.set_chart("BTC-USD", "7H")

    def test_an_unlisted_pair_is_refused_and_a_listed_one_added(self):
        rest = FakeRest()
        self.make(rest)
        with mock.patch.object(live, "_live", self.live):
            result = live.add_symbol({"symbols": ["BTC-USD"], "symbol": "ltc-usd"}, None)
            self.assertEqual(result["symbols"], ["BTC-USD", "LTC-USD"])
            self.assertTrue(rest.urls[-1].endswith("/products/LTC-USD"))
            rest.get = mock.Mock(side_effect=ProviderError(404, "Coinbase does not list that symbol."))
            with self.assertRaises(ProviderError):
                live.add_symbol({"symbols": ["BTC-USD"], "symbol": "NOPE-USD"}, None)


class StoppableFeedTests(unittest.TestCase):
    def test_stop_during_a_session_ends_run(self):
        stop = threading.Event()

        def publish(_message):
            if stop.is_set():
                raise live.FeedStopped()

        feed = live.StoppableFeed(publish, stop)
        sessions = []

        def session():
            sessions.append(1)
            stop.set()
            feed.publish(feed.status())

        feed._session = session
        feed.run()
        self.assertEqual(sessions, [1])
        self.assertFalse(feed.connected)

    def test_a_dropped_session_reports_status_and_retries(self):
        stop = threading.Event()
        statuses = []

        def publish(message):
            statuses.append(message)

        feed = live.StoppableFeed(publish, stop)
        calls = []

        def session():
            calls.append(1)
            if len(calls) == 2:
                stop.set()
            raise OSError("dropped")

        feed._session = session
        with mock.patch.object(stop, "wait", return_value=False):
            feed.run()
        self.assertEqual(len(calls), 2)
        self.assertEqual(feed.reconnects, 2)
        self.assertFalse(statuses[-1]["connected"])
        self.assertGreater(statuses[0]["retryIn"], 0)


if __name__ == "__main__":
    unittest.main()
