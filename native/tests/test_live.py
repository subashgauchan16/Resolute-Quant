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

import pandas as pd  # noqa: E402
import server  # noqa: E402  The browser terminal's backend, to check both give the same answer.
from crypto_feed import ProviderError  # noqa: E402
from rq_engine import live, market_history as mh, markets  # noqa: E402
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

    def stats(self, symbol):
        self.urls.append(f"stats/{symbol}")
        return {"last": 10.0, "open": 9.0, "high": 11.0, "low": 8.0, "volume": 5.0}

    def fear_greed(self):
        return {"value": 50, "classification": "Neutral", "time": 0}


class FakeFeed:
    instances = []

    def __init__(self, publish, stop):
        self.publish = publish
        self.stop_event = stop
        self.watch = None
        self.book = None
        FakeFeed.instances.append(self)

    def set_watch(self, symbols):
        self.watch = symbols

    def set_book(self, symbol):
        self.book = symbol

    def run(self):
        self.stop_event.wait()


class Base(unittest.TestCase):
    def make(self, rest=None):
        self.sent = []
        self.live = live.Live(lambda text: self.sent.append(json.loads(text)), rest or FakeRest(), FakeFeed, seed=False)
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

    def test_resolve_checks_coinbase_first_then_yahoo(self):
        rest = FakeRest()
        self.make(rest)
        provider = mock.Mock()
        provider.quote.return_value = (1.0, {})
        markets_stub = types.SimpleNamespace(provider=provider)
        with mock.patch.object(live, "_live", self.live), mock.patch.object(live, "_markets", markets_stub):
            self.assertEqual(live.resolve({"symbol": "pepe"}, None), {"kind": "crypto", "symbol": "PEPE-USD"})
            self.assertEqual(rest.urls[-1], "stats/PEPE-USD")
            provider.quote.assert_not_called()

            rest.stats = mock.Mock(side_effect=ProviderError(404, "Coinbase does not list that symbol."))
            self.assertEqual(live.resolve({"symbol": "aapl"}, None), {"kind": "market", "symbol": "AAPL"})
            self.assertEqual(live.resolve({"symbol": "BTC", "kind": "market"}, None), {"kind": "market", "symbol": "BTC"})
            with self.assertRaises(ProviderError):
                live.resolve({"symbol": "NOPE", "kind": "crypto"}, None)

            provider.quote.side_effect = ValueError("no price")
            with self.assertRaisesRegex(ProviderError, "Neither Coinbase nor Yahoo"):
                live.resolve({"symbol": "NOPE"}, None)
            with self.assertRaisesRegex(ProviderError, "Yahoo has no price"):
                live.resolve({"symbol": "NOPE", "kind": "market"}, None)
            with self.assertRaises(history.BadRequest):
                live.resolve({"symbol": "not a symbol"}, None)


def trade_tick(symbol="BTC-USD", price=100.0, size=2.0, side="buy", trade=1):
    return dict(tick(symbol, price, int(time.time() * 1000), size, trade), side=side)


class Session(Base):
    def setUp(self):
        self.make()
        patcher = mock.patch.object(live, "FLUSH_SECONDS", 3600)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.live.start(["BTC-USD", "ETH-USD"])
        self.feed = FakeFeed.instances[-1]
        self.live.follow("BTC-USD")

    def test_follow_chooses_the_book_and_clears_the_last_one(self):
        self.assertEqual(self.feed.book, "BTC-USD")
        self.live.on_message({"type": "cb", "id": "BTC-USD", "bids": [[1.0, 2.0]], "asks": [[2.0, 3.0]], "depth": 2})
        self.assertEqual(self.live.flush()["book"]["id"], "BTC-USD")
        self.live.on_message({"type": "cb", "id": "BTC-USD", "bids": [], "asks": [], "depth": 0})
        self.live.follow("ETH-USD")
        self.assertEqual(self.feed.book, "ETH-USD")
        self.assertNotIn("book", self.live.flush() or {})
        self.live.follow(None)
        self.assertIsNone(self.feed.book)

    def test_a_book_for_another_pair_is_ignored(self):
        self.live.on_message({"type": "cb", "id": "ETH-USD", "bids": [], "asks": [], "depth": 0})
        self.assertNotIn("book", self.live.flush() or {})

    def test_the_book_is_cut_to_ten_levels(self):
        levels = [[float(i), 1.0] for i in range(1, 15)]
        self.live.on_message({"type": "cb", "id": "BTC-USD", "bids": levels, "asks": levels, "depth": 28})
        message = self.live.flush()["book"]
        self.assertEqual((len(message["bids"]), len(message["asks"]), message["depth"]), (10, 10, 28))

    def test_session_figures_match_a_hand_calculation(self):
        self.live.on_message(trade_tick(price=100.0, size=2.0, side="buy", trade=1))
        self.live.on_message(trade_tick(price=110.0, size=1.0, side="sell", trade=2))
        self.live.on_message(trade_tick(price=90.0, size=1.0, side="buy", trade=3))
        session = self.live.flush()["session"]
        self.assertEqual(session["vwap"], (200.0 + 110.0 + 90.0) / 4.0)
        self.assertEqual(session["buyPercent"], 75.0)
        self.assertEqual(session["trades"], 3)
        self.assertEqual(session["tradesPerMinute"], 3)
        self.assertEqual(session["last"], 90.0)
        self.assertEqual([t["p"] for t in session["tape"]], [90.0, 110.0, 100.0])  # Newest first.

    def test_a_tick_without_a_trade_adds_nothing_to_the_session(self):
        self.live.on_message(tick(trade=None))
        session = self.live._session_summary("BTC-USD")
        self.assertIsNone(session["vwap"])
        self.assertEqual(session["trades"], 0)
        self.assertEqual(session["tape"], [])

    def test_old_trades_leave_the_per_minute_count_and_the_tape_is_capped(self):
        old = int(time.time() * 1000) - 120000
        for trade in range(1, 80):
            self.live.on_message(dict(trade_tick(trade=trade), time=old))
        session = self.live._session_summary("BTC-USD")
        self.assertEqual(session["tradesPerMinute"], 0)
        self.assertEqual(session["trades"], 79)
        self.assertEqual(len(session["tape"]), live.TAPE_SENT)
        self.assertEqual(len(self.live.sessions["BTC-USD"]["tape"]), live.TAPE_KEPT)

    def test_session_is_only_sent_for_the_selected_pair(self):
        self.live.on_message(trade_tick("ETH-USD"))
        self.assertNotIn("session", self.live.flush() or {})

    def test_rate_reports_messages_per_second(self):
        for trade in range(1, 11):
            self.live.on_message(trade_tick(trade=trade))
        self.live.rate_sent = time.monotonic() - 2.0
        rate = self.live.flush()["rate"]
        self.assertEqual(rate["perSecond"], 5)
        self.assertIsNotNone(rate["lag"])

    def test_seed_sends_stats_and_a_sparkline(self):
        self.live.seed_one("BTC-USD")
        seed = self.sent[-1]["seed"]
        self.assertEqual(seed["id"], "BTC-USD")
        self.assertEqual(seed["open"], 9.0)
        self.assertEqual(seed["spark"], [11.0, 11.0])

    def test_a_failed_seed_sends_only_what_worked(self):
        self.live.rest.stats = mock.Mock(side_effect=ProviderError(502, "No."))
        self.live.seed_one("BTC-USD")
        self.assertNotIn("open", self.sent[-1]["seed"])
        count = len(self.sent)
        self.live.rest.candles = mock.Mock(side_effect=ProviderError(502, "No."))
        self.live.seed_one("BTC-USD")
        self.assertEqual(len(self.sent), count)  # Nothing to report at all.

    def test_fear_and_greed_failure_is_reported_as_none(self):
        self.live.seed_fear_greed()
        self.assertEqual(self.sent[-1]["fearGreed"]["value"], 50)
        self.live.rest.fear_greed = mock.Mock(side_effect=ProviderError(502, "No."))
        self.live.seed_fear_greed()
        self.assertIsNone(self.sent[-1]["fearGreed"])

    def test_new_pairs_are_queued_for_seeding(self):
        self.live.seed_queue.clear()
        self.live.set_watch(["BTC-USD", "ETH-USD", "LTC-USD"])
        self.assertEqual(list(self.live.seed_queue), ["LTC-USD"])
        self.assertEqual(self.feed.watch, ["BTC-USD", "ETH-USD", "LTC-USD"])


class FakeTicker:
    def __init__(self, _symbol):
        self.fast_info = {"previousClose": 90.0, "dayHigh": 105.0, "dayLow": float("nan"), "lastVolume": 1000.0, "yearHigh": 200.0,
                          "yearLow": 50.0, "marketCap": 5e9, "currency": "USD", "lastPrice": 100.0}
        self.info = {"longName": "A Company", "marketState": "POST", "exchangeDataDelayedBy": 15, "fullExchangeName": "NASDAQ", "sector": "Tech",
                     "industry": "Software", "country": "US", "website": "https://example.com", "currency": "USD", "quoteType": "EQUITY", "trailingPE": 20.0,
                     "forwardPE": 18.0, "trailingEps": 3.0, "dividendYield": 1.2, "beta": 1.1, "averageVolume": 5000.0, "marketCap": 5e9,
                     "fiftyTwoWeekHigh": 200.0, "fiftyTwoWeekLow": float("inf")}
        self.news = [{"content": {"title": "Kept", "clickThroughUrl": {"url": "https://example.com/a"}, "pubDate": "2026-01-01T00:00:00Z", "provider": {"displayName": "Wire"}}},
                     {"title": "Not secure", "canonicalUrl": {"url": "http://example.com/b"}},
                     {"content": {"title": "", "clickThroughUrl": {"url": "https://example.com/c"}}}]

    def history(self, **_kwargs):
        index = pd.date_range("2026-01-05 09:30", periods=30, freq="h", tz="America/New_York")
        values = [100.0 + i for i in range(30)]
        values[3] = float("nan")
        return pd.DataFrame({"Open": values, "High": [v + 1 for v in values], "Low": [v - 1 for v in values], "Close": values, "Volume": [10.0] * 29 + [float("nan")]}, index=index)


class MarketsParity(unittest.TestCase):
    """markets.py holds short copies of functions in server.py, so both must give the same answer."""

    def test_copies_match_the_browser_backend(self):
        with mock.patch("yfinance.Ticker", FakeTicker):
            self.assertEqual(markets.fast_quote("A"), server.fast_quote("A"))
            self.assertEqual(markets.meta("A"), server.meta("A"))
            self.assertEqual(markets.profile("A"), server.profile("A"))
            self.assertEqual(markets.news("A"), server.news("A"))
            self.assertEqual(len(markets.news("A")), 1)
            for name in server.INTERVALS:
                self.assertEqual(markets.candles("A", name), server.candles("A", name))

    def test_periods_match_the_browser_backend(self):
        self.assertEqual({k: v[1] for k, v in server.INTERVALS.items()}, markets.PERIODS)
        self.assertEqual({k: v[0] for k, v in server.INTERVALS.items()}, mh.YAHOO_INTERVALS)


class MarketsLoop(unittest.TestCase):
    def provider(self):
        provider = mock.Mock()
        provider.quote.side_effect = lambda symbol: (100.0, {"prev": 90.0, "high": None})
        provider.meta.return_value = {"state": "REGULAR", "name": None, "spark": [1.0, 2.0]}
        provider.limited.return_value = False
        return provider

    def make(self, provider):
        self.sent = []
        return markets.Markets(lambda text: self.sent.append(json.loads(text)), provider)

    def quotes(self):
        return [m["quote"] for m in self.sent if "quote" in m]

    def test_a_cycle_sends_quotes_then_details_and_status(self):
        loop = self.make(self.provider())
        loop.set_watch(["A"])
        loop.cycle()
        first, second = self.quotes()
        self.assertEqual((first["id"], first["price"], first["prev"]), ("A", 100.0, 90.0))
        self.assertNotIn("high", first)  # Missing values aren't sent as nulls.
        self.assertEqual((second["id"], second["state"], second["spark"]), ("A", "REGULAR", [1.0, 2.0]))
        self.assertEqual(self.sent[-1]["status"], {"limited": False})

    def test_details_are_only_fetched_when_due(self):
        provider = self.provider()
        loop = self.make(provider)
        loop.set_watch(["A"])
        loop.cycle()
        loop.cycle()
        self.assertEqual(provider.quote.call_count, 2)
        self.assertEqual(provider.meta.call_count, 1)

    def test_one_bad_symbol_does_not_stop_the_others(self):
        provider = self.provider()
        provider.quote.side_effect = lambda symbol: (_ for _ in ()).throw(ValueError("no price")) if symbol == "BAD" else (100.0, {})
        loop = self.make(provider)
        loop.set_watch(["BAD", "GOOD"])
        loop.cycle()
        self.assertEqual({q["id"] for q in self.quotes()}, {"GOOD"})

    def test_a_rate_limit_ends_the_round_and_is_reported(self):
        provider = self.provider()
        provider.quote.side_effect = ProviderError(429, "Slow down.")
        provider.limited.return_value = True
        loop = self.make(provider)
        loop.set_watch(["A", "B"])
        loop.cycle()
        self.assertEqual(provider.quote.call_count, 1)
        self.assertEqual(self.sent[-1]["status"], {"limited": True})

    def test_stop_ends_the_thread(self):
        loop = self.make(self.provider())
        loop.start(["A"])
        thread = loop.thread
        loop.stop()
        self.assertFalse(thread.is_alive())
        self.assertFalse(loop.running)
        loop.stop()  # Stopping twice is harmless.

    def test_throttle_pauses_after_a_rate_limit(self):
        from yfinance.exceptions import YFRateLimitError

        mh._yahoo_blocked_until = 0.0
        mh._yahoo_strikes = 0
        self.addCleanup(lambda: setattr(mh, "_yahoo_blocked_until", 0.0))
        with self.assertRaisesRegex(ProviderError, "slow down"):
            mh.yahoo_call(mock.Mock(side_effect=YFRateLimitError()))
        self.assertTrue(mh.yahoo_limited())
        waiting = mock.Mock()
        with self.assertRaisesRegex(ProviderError, "slow down"):
            mh.yahoo_call(waiting)
        waiting.assert_not_called()


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
