"""Unit and fixture tests for the data program. Run from the project folder:

    python -m unittest discover -s tests -v

Every "provider" payload here is a hand-written fixture for testing validation. None of it is
market data. The feed test talks to a fake WebSocket server on this PC, never to Coinbase.
"""
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from websockets.sync.server import serve  # noqa: E402

import crypto_feed as cf  # noqa: E402
import server as srv  # noqa: E402

NOW = 1_800_000_000
ISO_NOW = "2027-01-15T08:00:00.123456789Z"
ISO_NOW_SECONDS = 1_800_000_000.123


class Tickers(unittest.TestCase):
    def good(self, **change):
        raw = {"type": "ticker", "product_id": "BTC-USD", "price": "100.5", "time": ISO_NOW, "open_24h": "90",
               "high_24h": "110", "low_24h": "80", "volume_24h": "12.5", "best_bid": "100", "best_ask": "101",
               "last_size": "0.5", "trade_id": 7, "side": "buy"}
        raw.update(change)
        return raw

    def test_good_message_is_normalised_with_millisecond_time(self):
        m = cf.normalise_ticker(self.good(), now=ISO_NOW_SECONDS)
        self.assertEqual((m["type"], m["id"], m["price"], m["size"], m["side"]), ("ct", "BTC-USD", 100.5, 0.5, "buy"))
        self.assertAlmostEqual(m["time"], ISO_NOW_SECONDS * 1000, delta=1)

    def test_required_field_problems_reject_the_message(self):
        for change in ({"price": "abc"}, {"price": "NaN"}, {"price": "-1"}, {"price": None}, {"time": "yesterday"},
                       {"time": None}, {"product_id": "bad symbol"}, {"product_id": None}):
            self.assertIsNone(cf.normalise_ticker(self.good(**change), now=ISO_NOW_SECONDS), change)

    def test_future_timestamp_rejected_but_clock_drift_allowed(self):
        self.assertIsNone(cf.normalise_ticker(self.good(), now=ISO_NOW_SECONDS - 3600))
        self.assertIsNotNone(cf.normalise_ticker(self.good(), now=ISO_NOW_SECONDS - 30))

    def test_timestamp_without_timezone_rejected(self):
        self.assertIsNone(cf.parse_time_ms("2027-01-15T08:00:00", now=ISO_NOW_SECONDS))

    def test_optional_fields_stay_missing_rather_than_invented(self):
        m = cf.normalise_ticker(self.good(open_24h=None, best_bid="x"), now=ISO_NOW_SECONDS)
        self.assertIsNone(m["open"])
        self.assertIsNone(m["bid"])

    def test_message_without_usable_trade_has_no_trade_fields(self):
        m = cf.normalise_ticker(self.good(side="unknown"), now=ISO_NOW_SECONDS)
        self.assertNotIn("trade", m)


class Candles(unittest.TestCase):
    def test_sorted_ascending_and_field_order_mapped(self):
        bars, rejected = cf.normalise_candles([[200, 1, 4, 2, 3, 9], [100, 5, 8, 6, 7, 1]], now=NOW)
        self.assertEqual(rejected, 0)
        self.assertEqual([b["time"] for b in bars], [100, 200])
        self.assertEqual(bars[1], {"time": 200, "open": 2.0, "high": 4.0, "low": 1.0, "close": 3.0, "volume": 9.0})

    def test_invalid_rows_dropped_and_counted_not_repaired(self):
        rows = [
            [100, 1, 4, 2, 3, 9],       # good
            [100, 1, 4, 2, 3, 9],       # duplicate time
            [200, 3, 2, 2, 2, 1],       # high below low
            [300, 1, 2, 3, 2, 1],       # open above high
            [400, 0, 4, 2, 3, 1],       # zero low
            [500, 1, 4, 2, 3, -1],      # negative volume
            [600, 1, 4, 2, None, 1],    # missing close
            [NOW + 3600, 1, 4, 2, 3, 1],  # future bar
            ["bad"],                    # wrong shape
        ]
        bars, rejected = cf.normalise_candles(rows, now=NOW)
        self.assertEqual([b["time"] for b in bars], [100])
        self.assertEqual(rejected, 8)

    def test_non_list_answer_gives_no_bars(self):
        self.assertEqual(cf.normalise_candles({"message": "error"}, now=NOW), ([], 0))


class OtherPayloads(unittest.TestCase):
    def test_stats_needs_a_price(self):
        with self.assertRaises(cf.ProviderError):
            cf.normalise_stats({"open": "1"})
        self.assertEqual(cf.normalise_stats({"last": "5", "volume": "x"})["volume"], None)

    def test_fear_greed_range_and_shape(self):
        good = {"data": [{"value": "50", "value_classification": "Neutral", "timestamp": "100"}]}
        self.assertEqual(cf.normalise_fear_greed(good), {"value": 50, "classification": "Neutral", "time": 100})
        for bad in ({}, {"data": []}, {"data": [{"value": "101", "value_classification": "x"}]},
                    {"data": [{"value": "ten", "value_classification": "x"}]}):
            with self.assertRaises(cf.ProviderError):
                cf.normalise_fear_greed(bad)


class Books(unittest.TestCase):
    def setUp(self):
        self.book = cf.Book("BTC-USD", {"bids": [["100", "1"], ["99", "2"], ["bad", "1"]], "asks": [["101", "3"], ["102", "4"]]})

    def test_snapshot_skips_bad_levels_and_orders_best_first(self):
        m = self.book.message()
        self.assertEqual(m["bids"], [[100.0, 1.0], [99.0, 2.0]])
        self.assertEqual(m["asks"], [[101.0, 3.0], [102.0, 4.0]])
        self.assertEqual(m["depth"], 4)
        self.assertFalse(m["crossed"])

    def test_updates_insert_change_and_delete(self):
        self.assertTrue(self.book.apply("buy", "100.5", "7"))
        self.assertTrue(self.book.apply("sell", "101", "0"))
        self.assertTrue(self.book.apply("sell", "500", "0"))  # deleting an absent level is harmless
        m = self.book.message()
        self.assertEqual(m["bids"][0], [100.5, 7.0])
        self.assertEqual(m["asks"][0], [102.0, 4.0])

    def test_bad_updates_refused(self):
        self.assertFalse(self.book.apply("buy", "x", "1"))
        self.assertFalse(self.book.apply("buy", "100", "-1"))
        self.assertFalse(self.book.apply("hold", "100", "1"))

    def test_crossed_book_is_flagged_not_hidden(self):
        self.book.apply("buy", "150", "1")
        self.assertTrue(self.book.message()["crossed"])

    def test_top_levels_only(self):
        for i in range(30):
            self.book.apply("buy", str(50 + i), "1")
        self.assertEqual(len(self.book.message()["bids"]), cf.BOOK_LEVELS)


class Ports(unittest.TestCase):
    def config(self, text):
        folder = tempfile.mkdtemp()
        path = Path(folder) / "terminal.config.json"
        path.write_text(text, encoding="utf-8")
        return path

    def missing(self):
        return Path(tempfile.mkdtemp()) / "none.json"

    def test_order_is_cli_then_env_then_file_then_default(self):
        path = self.config('{"port": 9003}')
        self.assertEqual(srv.configured_port("9001", {"RESOLUTE_QUANT_PORT": "9002"}, path), (9001, "--port"))
        self.assertEqual(srv.configured_port(None, {"RESOLUTE_QUANT_PORT": "9002"}, path), (9002, "RESOLUTE_QUANT_PORT"))
        self.assertEqual(srv.configured_port(None, {}, path), (9003, "terminal.config.json"))
        self.assertEqual(srv.configured_port(None, {}, self.missing()), (srv.DEFAULT_PORT, "default"))

    def test_bad_settings_stop_rather_than_guess(self):
        for args in (("abc", {}, self.missing()), (None, {"RESOLUTE_QUANT_PORT": "80"}, self.missing()),
                     (None, {"RESOLUTE_QUANT_PORT": "70000"}, self.missing()), (None, {}, self.config("not json")),
                     (None, {}, self.config('{"port": "x"}'))):
            with self.assertRaises(SystemExit):
                srv.configured_port(*args)

    def free_port(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    def test_second_listener_cannot_share_a_port_on_windows(self):
        first = srv.Server(("127.0.0.1", self.free_port()), srv.Handler)
        try:
            with self.assertRaises(OSError):
                srv.Server(("127.0.0.1", first.server_address[1]), srv.Handler)
        finally:
            first.server_close()

    def test_configured_port_never_falls_back(self):
        with socket.socket() as blocker:
            blocker.bind(("127.0.0.1", 0))
            blocker.listen(1)
            with mock.patch("builtins.print"), self.assertRaises(SystemExit):
                srv.bind_server(blocker.getsockname()[1], "RESOLUTE_QUANT_PORT")

    def test_default_port_falls_back_to_next_free_in_order(self):
        with socket.socket() as blocker:
            blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            blocker.bind(("127.0.0.1", 0))
            blocker.listen(1)
            port = blocker.getsockname()[1]
            with mock.patch("builtins.print") as printed, mock.patch.object(srv, "identify", return_value=(False, "fixture", None)):
                server, existing = srv.bind_server(port, "default")
            try:
                self.assertIsNone(existing)
                self.assertGreater(server.server_address[1], port)
                self.assertIn("has not been touched", printed.call_args_list[0].args[0])
            finally:
                server.server_close()

    def test_running_terminal_is_reused_not_duplicated(self):
        with socket.socket() as blocker:
            blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            blocker.bind(("127.0.0.1", 0))
            blocker.listen(1)
            port = blocker.getsockname()[1]
            with mock.patch.object(srv, "identify", return_value=(True, "this terminal", {})):
                self.assertEqual(srv.bind_server(port, "default"), (None, f"http://127.0.0.1:{port}"))


class FeedReconnect(unittest.TestCase):
    """The feed against a fake upstream that sends good and bad messages, then hangs up."""

    def test_reconnects_rejects_bad_data_and_keeps_book_rules(self):
        connections = []

        def upstream(ws):
            connections.append(1)
            ws.recv()  # heartbeat subscription
            ws.send(json.dumps({"type": "ticker", "product_id": "BTC-USD", "price": "not a number", "time": "2020-01-01T00:00:00Z"}))
            ws.send(json.dumps({"type": "ticker", "product_id": "BTC-USD", "price": "100", "time": "2020-01-01T00:00:00Z",
                                "last_size": "1", "trade_id": 1, "side": "buy"}))
            ws.send(json.dumps({"type": "ticker", "product_id": "BTC-USD", "price": "100", "time": "2020-01-01T00:00:00Z",
                                "last_size": "1", "trade_id": 1, "side": "buy"}))
            ws.send(json.dumps({"type": "snapshot", "product_id": "BTC-USD", "bids": [["99", "1"]], "asks": [["101", "1"]]}))
            ws.send(json.dumps({"type": "l2update", "product_id": "BTC-USD", "changes": [["buy", "x", "1"]]}))
            time.sleep(0.6)  # long enough for one book push

        messages = []
        with serve(upstream, "127.0.0.1", 0) as fake:
            threading.Thread(target=fake.serve_forever, daemon=True).start()
            port = fake.socket.getsockname()[1]
            with mock.patch.object(cf, "WS", f"ws://127.0.0.1:{port}"):
                feed = cf.CoinbaseFeed(messages.append)
                feed.set_book("BTC-USD")
                threading.Thread(target=feed.run, daemon=True).start()
                deadline = time.time() + 15
                while time.time() < deadline and len(connections) < 2:
                    time.sleep(0.1)
            fake.shutdown()

        self.assertGreaterEqual(len(connections), 2, "feed should reconnect after the upstream hangs up")
        self.assertGreaterEqual(feed.reconnects, 1)
        self.assertGreaterEqual(feed.rejected, 2, "bad ticker and bad level update should be counted")
        ticks = [m for m in messages if m["type"] == "ct"]
        self.assertTrue(ticks)
        self.assertTrue(all(m["price"] == 100.0 for m in ticks))
        first_session = ticks[:2]
        self.assertEqual(sum("trade" in m for m in first_session), 1, "a repeated trade id should be sent once")
        self.assertTrue(any(m["type"] == "cb" and m["bids"] == [[99.0, 1.0]] for m in messages))
        states = [m["connected"] for m in messages if m["type"] == "cs"]
        self.assertIn(True, states)
        self.assertIn(False, states)


class BrowserOpening(unittest.TestCase):
    def test_retries_until_this_instance_answers(self):
        answers = [(False, "slow", None), (False, "slow", None), (True, "this terminal", {"instance": srv.instance["id"]})]
        with mock.patch.object(srv, "identify", side_effect=answers) as identify, mock.patch.object(srv.webbrowser, "open") as opened:
            self.assertTrue(srv.open_when_confirmed("http://x", 1, wait=0))
        self.assertEqual(identify.call_count, 3)
        opened.assert_called_once_with("http://x")

    def test_never_opens_another_instance_and_explains(self):
        other = (True, "this terminal", {"instance": "someone-else"})
        with mock.patch.object(srv, "identify", return_value=other), mock.patch.object(srv.webbrowser, "open") as opened, mock.patch("builtins.print") as printed:
            self.assertFalse(srv.open_when_confirmed("http://x", 1, attempts=3, wait=0))
        opened.assert_not_called()
        self.assertIn("open http://x yourself", printed.call_args_list[-1].args[0])


class WatchLimit(unittest.TestCase):
    def test_keeps_up_to_the_limit_and_names_the_rest(self):
        symbols = [f"S{i}-USD" for i in range(srv.MAX_WATCH + 2)]
        kept, dropped = srv.split_watch(symbols + ["bad!", ""], srv.PAIR)
        self.assertEqual(kept, symbols[:srv.MAX_WATCH])
        self.assertEqual(dropped, symbols[srv.MAX_WATCH:] + ["bad!"])

    def test_nothing_dropped_when_all_fit(self):
        self.assertEqual(srv.split_watch(["AAPL", "MSFT"], srv.SYMBOL), (["AAPL", "MSFT"], []))


class Routes(unittest.TestCase):
    """The real HTTP handler on a spare port, with the provider layer stubbed by fixtures."""

    @classmethod
    def setUpClass(cls):
        cls.server = srv.Server(("127.0.0.1", 0), srv.Handler)
        cls.port = cls.server.server_address[1]
        srv.instance.update(port=cls.port, preferred=cls.port, source="--port")
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def get(self, path, host=None):
        import http.client
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        conn.request("GET", path, headers={"Host": host} if host else {})
        response = conn.getresponse()
        body = json.loads(response.read() or b"null")
        conn.close()
        return response.status, body

    def setUp(self):
        srv.cache.items.clear()

    def test_health_contract(self):
        status, body = self.get("/api/health")
        self.assertEqual(status, 200)
        self.assertEqual(body["service"], "resolute-quant")
        self.assertEqual(body["instance"], srv.instance["id"])
        for field in ("status", "timestamp", "port", "preferredPort", "portSource", "fallback", "clients", "providers"):
            self.assertIn(field, body)
        self.assertEqual(set(body["providers"]), {"yahoo", "coinbase"})
        self.assertIn(body["status"], ("ok", "degraded"))

    def test_existing_validation_is_unchanged(self):
        self.assertEqual(self.get("/api/candles?symbol=!!")[0], 400)
        self.assertEqual(self.get("/api/candles?symbol=AAPL&interval=7X")[0], 400)
        self.assertEqual(self.get("/nope")[0], 400)
        self.assertEqual(self.get("/api/health", host="evil.example")[0], 403)

    def test_month_and_four_hour_are_known_intervals_and_minute_is_distinct(self):
        self.assertEqual(srv.INTERVALS["1M"][0], "1m")
        self.assertEqual(srv.INTERVALS["1MO"][0], "1mo")
        self.assertEqual(srv.INTERVALS["4H"][0], "4h")

    def test_crypto_input_validation(self):
        self.assertEqual(self.get("/api/crypto/stats?symbol=AAPL")[0], 400)
        self.assertEqual(self.get("/api/crypto/candles?symbol=BTC-USD&granularity=14400")[0], 400)
        self.assertEqual(self.get("/api/crypto/candles?symbol=BTC-USD&granularity=x")[0], 400)
        self.assertEqual(self.get("/api/crypto/book?symbol=bad!")[0], 400)

    def test_provider_errors_reach_the_page_with_their_status(self):
        for code in (404, 429, 502):
            srv.cache.items.clear()
            with mock.patch.object(srv.rest, "stats", side_effect=cf.ProviderError(code, "fixture")):
                self.assertEqual(self.get("/api/crypto/stats?symbol=BTC-USD"), (code, {"error": "fixture"}))

    def test_last_good_copy_is_flagged_stale_when_provider_fails(self):
        bars = [{"time": 1, "open": 1, "high": 1, "low": 1, "close": 1, "volume": 0}]
        with mock.patch.object(srv.rest, "candles", return_value=bars):
            self.assertFalse(self.get("/api/crypto/candles?symbol=BTC-USD&granularity=60")[1]["stale"])
        key = ("cbars", "BTC-USD", 60)
        srv.cache.items[key] = (0, bars)
        with mock.patch.object(srv.rest, "candles", side_effect=cf.ProviderError(502, "fixture")):
            self.assertTrue(self.get("/api/crypto/candles?symbol=BTC-USD&granularity=60")[1]["stale"])

    def test_watch_and_book_set_the_feed(self):
        status, body = self.get("/api/crypto/watch?symbols=BTC-USD,bad!,ETH-USD")
        self.assertEqual(body["watch"], ["BTC-USD", "ETH-USD"])
        self.assertEqual(body["dropped"], ["BAD!"])
        self.assertEqual(self.get("/api/crypto/book?symbol=ETH-USD")[1], {"symbol": "ETH-USD"})
        self.assertEqual(srv.coinbase.book_symbol, "ETH-USD")
        self.assertEqual(self.get("/api/crypto/book?symbol=")[1], {"symbol": None})
        self.assertIsNone(srv.coinbase.book_symbol)

    def test_assets_cannot_leave_their_folder(self):
        for path in ("/assets/../server.py", "/assets/..%2fserver.py", "/assets/%2e%2e/%2e%2e/x.py", "/assets/none.js"):
            self.assertEqual(self.get(path)[0], 404, path)


if __name__ == "__main__":
    unittest.main()
