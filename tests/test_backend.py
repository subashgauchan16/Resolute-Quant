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
import history as hs  # noqa: E402
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

    def test_market_mode_keeps_negative_prices_and_missing_volume(self):
        rows = [[100, -40, -30, -38, -35, 9], [200, 1, 4, 2, 3, None]]
        bars, rejected = cf.normalise_candles(rows, now=NOW, market=True)
        self.assertEqual((rejected, bars[0]["low"], bars[1]["volume"]), (0, -40.0, None))

    def test_market_mode_still_rejects_bad_rows(self):
        rows = [[100, 3, 2, 2, 2, 1], [200, 0, 4, 2, 3, 1], [300, 1, 4, 2, None, 1], [400, 1, 4, 2, 3, -1], [500, 1, 2, 3, 2, 1]]
        self.assertEqual(cf.normalise_candles(rows, now=NOW, market=True), ([], 5))

    def test_exchange_mode_is_unchanged(self):
        self.assertEqual(cf.normalise_candles([[100, -40, -30, -38, -35, 9], [200, 1, 4, 2, 3, None]], now=NOW), ([], 2))

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
            with mock.patch.object(srv, "identify", return_value=(True, "this terminal", {"codeId": srv.RUNNING_CODE})):
                self.assertEqual(srv.bind_server(port, "default"), (None, f"http://127.0.0.1:{port}"))

    def test_different_version_is_refused_with_instructions(self):
        for body in ({}, {"codeId": "something-older"}):  # {} is a terminal from before this check existed
            with socket.socket() as blocker:
                blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                blocker.bind(("127.0.0.1", 0))
                blocker.listen(1)
                with mock.patch.object(srv, "identify", return_value=(True, "this terminal", body)), self.assertRaises(SystemExit) as stopped:
                    srv.bind_server(blocker.getsockname()[1], "default")
                self.assertIn("Close its black window", str(stopped.exception))


class CodeFingerprint(unittest.TestCase):
    def test_same_files_mean_no_restart(self):
        self.assertEqual(srv.code_id(), srv.RUNNING_CODE)
        self.assertFalse(srv.restart_needed())

    def test_dependency_changes_count_as_an_update(self):
        self.assertIn("requirements.txt", srv.BACKEND_FILES)

    def test_changed_files_need_a_restart(self):
        with mock.patch.object(srv, "RUNNING_CODE", "older"):
            self.assertTrue(srv.restart_needed())

    def test_unreadable_file_waits_for_the_next_check(self):
        with mock.patch.object(srv, "code_id", side_effect=OSError("being replaced")):
            self.assertFalse(srv.restart_needed())


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


DAY0 = 1_700_000_000 - 1_700_000_000 % 86400  # a UTC midnight safely in the past


def bar(t, close=100.5, volume=2.5):
    return {"time": t, "open": 100.0, "high": 101.0, "low": 99.0, "close": close, "volume": volume}


class HistoryLogic(unittest.TestCase):
    def test_range_is_whole_utc_days_inclusive_and_capped_at_now(self):
        self.assertEqual(hs.parse_range("2027-01-14", "2027-01-14", now=hs.parse_day("2027-06-01", "x")), (hs.parse_day("2027-01-14", "x"), hs.parse_day("2027-01-15", "x")))
        start, end = hs.parse_range("2027-01-14", "2099-01-01", now=hs.parse_day("2027-01-15", "x") + 5)
        self.assertEqual(end, hs.parse_day("2027-01-15", "x") + 5)

    def test_bad_ranges_are_refused_with_a_reason(self):
        for start, end in (("", "2027-01-01"), ("2027-13-01", "2027-01-01"), ("2027-01-02", "2027-01-01"), ("2099-01-01", "2099-01-02")):
            with self.assertRaises(hs.BadRequest):
                hs.parse_range(start, end, now=hs.parse_day("2027-06-01", "x"))

    def test_pages_cover_the_range_without_gaps_or_overlap(self):
        windows, truncated = hs.page_windows(0, 3600 * 650, 3600)
        self.assertFalse(truncated)
        self.assertEqual(windows[0][1], 3600 * 650)
        self.assertEqual(windows[-1][0], 0)
        for newer, older in zip(windows, windows[1:]):
            self.assertEqual(newer[0], older[1])
        self.assertTrue(all((b - a) // 3600 <= hs.COINBASE_PAGE for a, b in windows))

    def test_oversized_range_keeps_the_newest_and_says_so(self):
        windows, truncated = hs.page_windows(0, 60 * 20000, 60)
        self.assertTrue(truncated)
        self.assertEqual(windows[-1][0], 60 * (20000 - hs.MAX_BARS))

    def test_unfinished_bars_are_left_out_and_counted(self):
        done, unfinished = hs.complete_only([bar(0), bar(60), bar(120)], 60, now=125)
        self.assertEqual([b["time"] for b in done], [0, 60])
        self.assertEqual(unfinished, 1)

    def test_current_month_bar_is_unfinished(self):
        june, july = hs.parse_day("2027-06-01", "x"), hs.parse_day("2027-07-01", "x")
        done, unfinished = hs.complete_only([bar(june), bar(july)], None, now=july + 100)
        self.assertEqual([b["time"] for b in done], [june])
        self.assertEqual(unfinished, 1)

    def test_month_bar_of_an_exchange_east_of_utc_is_unfinished_in_its_own_month(self):
        november = hs.parse_day("2027-11-01", "x")
        tokyo_november_bar = november - 9 * 3600  # 1 November local time, still October in UTC
        tokyo_october_bar = hs.parse_day("2027-10-01", "x") - 9 * 3600
        done, unfinished = hs.complete_only([bar(tokyo_october_bar), bar(tokyo_november_bar)], None, now=november + 3600)
        self.assertEqual([b["time"] for b in done], [tokyo_october_bar])
        self.assertEqual(unfinished, 1)

    def test_month_bar_west_of_utc_still_finishes_correctly(self):
        october, november = hs.parse_day("2027-10-01", "x") + 4 * 3600, hs.parse_day("2027-11-01", "x") + 4 * 3600
        done, _ = hs.complete_only([bar(october), bar(november)], None, now=november + 3600)
        self.assertEqual([b["time"] for b in done], [october])

    def test_market_closures_between_days_are_not_gaps(self):
        monday, friday = 0, 3600 * 72
        bars = [bar(monday), bar(monday + 3600), bar(monday + 3 * 3600), bar(friday)]
        days = {monday: "mon", monday + 3600: "mon", monday + 3 * 3600: "mon", friday: "fri"}
        self.assertEqual(hs.session_gaps(bars, 3600, days), {"count": 1, "missing_bars": 1, "largest_seconds": 7200, "scope": "within_trading_days"})

    def test_daily_and_monthly_market_bars_report_no_session_gaps(self):
        bars = [bar(0), bar(4 * hs.DAY)]
        self.assertEqual(hs.session_gaps(bars, hs.DAY, {})["count"], 0)
        self.assertEqual(hs.session_gaps(bars, None, {})["count"], 0)

    def test_view_prefers_the_providers_session_gaps(self):
        fetched = {"bars": [bar(DAY0), bar(DAY0 + 7200)], "rejected": 0, "truncated": False, "downloaded_at": DAY0, "gaps": {"count": 0, "missing_bars": 0, "largest_seconds": 0, "scope": "within_trading_days"}}
        view = hs.build_view(fetched, "AAPL", "1H", "yahoo", 3600, "a", "b", now=DAY0 + 10**7)
        self.assertEqual(view["gaps"]["scope"], "within_trading_days")

    def test_missing_volume_is_blank_in_csv_and_never_zero(self):
        view = self.view([bar(DAY0, volume=None)])
        row = hs.to_csv(view).strip().split(chr(13) + chr(10))[1].split(",")
        self.assertEqual(row[hs.COLUMNS.index("volume")], "")

    def test_gaps_are_counted_and_never_filled(self):
        bars = [bar(0), bar(60), bar(300)]
        self.assertEqual(hs.gap_summary(bars, 60), {"count": 1, "missing_bars": 3, "largest_seconds": 240, "scope": "all"})
        self.assertEqual(len(bars), 3)
        self.assertEqual(hs.gap_summary(bars, None)["count"], 0)

    def view(self, bars, **change):
        fetched = {"bars": bars, "rejected": 0, "truncated": False, "downloaded_at": DAY0}
        arguments = {"symbol": "BTC-USD", "interval": "1H", "source": "coinbase", "step": 3600, "start_text": "2027-01-14", "end_text": "2027-01-15", "now": DAY0 + 10**7}
        arguments.update(change)
        return hs.build_view(fetched, **arguments)

    def test_view_is_newest_first_with_coverage(self):
        view = self.view([bar(DAY0), bar(DAY0 + 3600)])
        self.assertEqual([b["time"] for b in view["bars"]], [DAY0 + 3600, DAY0])
        self.assertEqual((view["count"], view["first_utc"]), (2, hs.iso_utc(DAY0)))

    def test_empty_view_and_csv_have_a_header_only(self):
        view = self.view([])
        self.assertEqual((view["bars"], view["count"], view["first_utc"]), ([], 0, None))
        self.assertEqual(hs.to_csv(view), ",".join(hs.COLUMNS) + "\r\n")

    def test_csv_header_order_row_count_and_values(self):
        import csv
        import io
        view = self.view([bar(DAY0, close=0.1 + 0.2, volume=1e-07), bar(DAY0 + 3600)])
        rows = list(csv.reader(io.StringIO(hs.to_csv(view))))
        self.assertEqual(tuple(rows[0]), hs.COLUMNS)
        self.assertEqual(len(rows), 3)
        oldest = dict(zip(rows[0], rows[2]))
        self.assertEqual(float(oldest["close"]), 0.1 + 0.2, "numbers must read back exactly")
        self.assertEqual(float(oldest["volume"]), 1e-07)
        self.assertEqual(oldest["time_utc"], hs.iso_utc(DAY0))
        self.assertTrue(oldest["time_utc"].endswith("Z"))

    def test_csv_escapes_commas_quotes_and_line_breaks(self):
        import csv
        import io
        for awkward in ("A,B", "say " + chr(34) + "hi" + chr(34), "one" + chr(10) + "two", "caf" + chr(233)):
            view = self.view([bar(DAY0)], symbol=awkward)
            text = hs.to_csv(view)
            self.assertEqual(list(csv.reader(io.StringIO(text, newline="")))[1][1], awkward)
            self.assertEqual(text.encode("utf-8").decode("utf-8"), text)
        self.assertIn('"A,B"', hs.to_csv(self.view([bar(DAY0)], symbol="A,B")))
        self.assertIn(chr(34) + "say " + chr(34) * 2 + "hi" + chr(34) * 3, hs.to_csv(self.view([bar(DAY0)], symbol="say " + chr(34) + "hi" + chr(34))))

    def test_export_name_is_safe(self):
        self.assertEqual(hs.export_name({"symbol": "^GSPC", "interval": "1D", "start": "2027-01-01", "end": "2027-01-02"}), "GSPC_1D_2027-01-01_to_2027-01-02.csv")


class CoinbasePaging(unittest.TestCase):
    def test_all_pages_are_requested_merged_and_validated(self):
        rest = cf.Rest()
        step = 3600
        pages = [[[DAY0 + 301 * step, 1, 4, 2, 3, 9], [DAY0 + 300 * step, 3, 2, 2, 2, 1]], [[DAY0, 1, 4, 2, 3, 9]]]
        with mock.patch.object(rest, "get", side_effect=pages) as get:
            out = rest.candles_between("BTC-USD", step, DAY0, DAY0 + 350 * step)
        self.assertEqual(get.call_count, 2)
        self.assertIn("granularity=3600", get.call_args_list[0].args[0])
        self.assertEqual([b["time"] for b in out["bars"]], [DAY0, DAY0 + 301 * step])
        self.assertEqual((out["rejected"], out["truncated"]), (1, False))

    def test_a_failed_page_fails_the_whole_request(self):
        rest = cf.Rest()
        with mock.patch.object(rest, "get", side_effect=[[], cf.ProviderError(502, "fixture")]), self.assertRaises(cf.ProviderError):
            rest.candles_between("BTC-USD", 3600, DAY0, DAY0 + 350 * 3600)


class CacheLimit(unittest.TestCase):
    def test_oldest_entries_are_removed_at_the_limit(self):
        cache = srv.Cache()
        with mock.patch.object(srv.Cache, "MAX_ITEMS", 3), mock.patch.object(srv.time, "time", side_effect=[float(i) for i in range(100)]):
            for key in "abcde":
                cache.get(key, 1000, lambda: key)
        self.assertEqual(sorted(cache.items), ["c", "d", "e"])


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
        self.assertEqual((body["codeId"], body["restartNeeded"]), (srv.RUNNING_CODE, False))
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

    def history_url(self, **change):
        query = {"symbol": "BTC-USD", "interval": "1H", "source": "coinbase", "start": "2020-01-01", "end": "2020-01-02"}
        query.update(change)
        return "/api/history?" + "&".join(f"{k}={v}" for k, v in query.items())

    def fetched(self, bars=(), **change):
        return dict({"bars": list(bars), "rejected": 0, "truncated": False, "downloaded_at": DAY0}, **change)

    def test_history_json_is_newest_first(self):
        first = hs.parse_day("2020-01-01", "x")
        with mock.patch.object(srv.rest, "candles_between", return_value=self.fetched([bar(first), bar(first + 3600)])):
            status, body = self.get(self.history_url())
        self.assertEqual(status, 200)
        self.assertEqual([b["time"] for b in body["bars"]], [first + 3600, first])
        self.assertEqual((body["count"], body["source"], body["stale"]), (2, "coinbase", False))

    def test_history_csv_matches_the_json_view(self):
        import http.client
        first = hs.parse_day("2020-01-01", "x")
        with mock.patch.object(srv.rest, "candles_between", return_value=self.fetched([bar(first), bar(first + 3600)])):
            conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
            conn.request("GET", self.history_url(format="csv"))
            response = conn.getresponse()
            text = response.read().decode("utf-8")
            conn.close()
        self.assertEqual(response.status, 200)
        self.assertIn("text/csv", response.getheader("Content-Type"))
        self.assertIn("BTC-USD_1H_2020-01-01_to_2020-01-02.csv", response.getheader("Content-Disposition"))
        lines = text.strip().split("\r\n")
        self.assertEqual(lines[0], ",".join(hs.COLUMNS))
        self.assertEqual(len(lines), 3)
        self.assertTrue(lines[1].startswith(hs.iso_utc(first + 3600)))

    def test_history_empty_range_is_an_empty_answer(self):
        with mock.patch.object(srv.rest, "candles_between", return_value=self.fetched()):
            status, body = self.get(self.history_url())
        self.assertEqual((status, body["bars"], body["count"]), (200, [], 0))

    def test_history_input_errors_explain_themselves(self):
        for change in ({"source": "other"}, {"interval": "4H"}, {"interval": "7X"}, {"start": "bad"}, {"start": "2020-01-03"}, {"format": "xml"},
                       {"symbol": "AAPL"}, {"symbol": "=A1", "source": "yahoo", "interval": "1D"}):
            status, body = self.get(self.history_url(**change))
            self.assertEqual(status, 400, change)
            self.assertTrue(body["error"], change)

    def test_history_provider_errors_keep_their_status(self):
        for code in (404, 429, 502):
            srv.cache.items.clear()
            with mock.patch.object(srv.rest, "candles_between", side_effect=cf.ProviderError(code, "fixture")):
                self.assertEqual(self.get(self.history_url()), (code, {"error": "fixture"}))

    def test_history_last_good_copy_is_flagged_stale(self):
        first = hs.parse_day("2020-01-01", "x")
        with mock.patch.object(srv.rest, "candles_between", return_value=self.fetched([bar(first)])):
            self.get(self.history_url())
        for key in list(srv.cache.items):
            srv.cache.items[key] = (0, srv.cache.items[key][1])
        with mock.patch.object(srv.rest, "candles_between", side_effect=cf.ProviderError(502, "fixture")):
            status, body = self.get(self.history_url())
        self.assertEqual((status, body["stale"], body["count"]), (200, True, 1))

    def test_yahoo_history_validates_and_keeps_what_the_provider_sent(self):
        import pandas as pd
        first = hs.parse_day("2020-01-01", "x")
        index = pd.to_datetime([first, first + 86400, first + 2 * 86400], unit="s", utc=True)
        frame = pd.DataFrame({"Open": [2.0, 2.0, 2.0], "High": [4.0, 1.0, 4.0], "Low": [1.0, 1.0, 1.0], "Close": [3.0, 3.0, 3.0], "Volume": [0.0, 5.0, 7.0]}, index=index)
        with mock.patch.object(srv.yf, "Ticker") as ticker:
            ticker.return_value.history.return_value = frame
            out = srv.yahoo_history("AAPL", "1D", first, first + 3 * 86400)
        self.assertEqual([b["time"] for b in out["bars"]], [first, first + 2 * 86400])
        self.assertEqual(out["bars"][0]["volume"], 0.0)
        self.assertEqual(out["rejected"], 1)
        self.assertFalse(ticker.return_value.history.call_args.kwargs["auto_adjust"])

    def test_yahoo_history_counts_gaps_inside_a_trading_day_only(self):
        import pandas as pd
        monday = pd.Timestamp("2020-01-06 09:30", tz="America/New_York")
        index = pd.DatetimeIndex([monday, monday + pd.Timedelta(hours=1), monday + pd.Timedelta(hours=3), monday + pd.Timedelta(days=1)])
        frame = pd.DataFrame({"Open": [2.0] * 4, "High": [4.0] * 4, "Low": [1.0] * 4, "Close": [3.0] * 4, "Volume": [1.0, float("nan"), 1.0, 1.0]}, index=index)
        with mock.patch.object(srv.yf, "Ticker") as ticker:
            ticker.return_value.history.return_value = frame
            out = srv.yahoo_history("CL=F", "1H", 0, 10**10)
        self.assertEqual((out["gaps"]["count"], out["gaps"]["scope"]), (1, "within_trading_days"))
        self.assertEqual(out["rejected"], 0)
        self.assertIsNone(out["bars"][1]["volume"])

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
