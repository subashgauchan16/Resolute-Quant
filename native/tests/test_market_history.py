"""Tests the native history code on the private Python, against a fake provider. No network is used."""

import csv
import os
import sys
import tempfile
import time
import types
import unittest
from pathlib import Path
from unittest import mock

import pandas as pd

NATIVE = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(NATIVE / "python"), str(NATIVE.parent)]

# The real rq_engine.runner imports this module, which only exists inside the app.
sys.modules["_rq_host"] = types.SimpleNamespace(report_progress=lambda *_: None, is_cancelled=lambda: False)

import server  # noqa: E402  The browser terminal's backend, to check both give the same answer.
from crypto_feed import ProviderError  # noqa: E402
from rq_engine import market_history as mh  # noqa: E402
from rq_engine import runner  # noqa: E402

HOST = runner.Host()
START, END = "2026-01-05", "2026-01-09"


def frame():
    """Five Yahoo-style rows, including a NaN row, a missing volume and an intraday gap."""
    index = pd.DatetimeIndex(
        ["2026-01-05 09:30", "2026-01-05 09:31", "2026-01-05 09:34", "2026-01-05 09:35", "2026-01-06 09:30"],
        tz="America/New_York",
    )
    return pd.DataFrame(
        {
            "Open": [10.0, 11.0, float("nan"), 12.5, -1.5],
            "High": [11.0, 12.0, 13.0, 13.25, -1.0],
            "Low": [9.5, 10.5, 11.0, 12.0, -2.0],
            "Close": [10.5, 11.5, 12.0, 13.0, -1.25],
            "Volume": [100.0, float("nan"), 300.0, 400.0, 500.0],
        },
        index=index,
    )


class FakeTicker:
    calls = 0
    error = None

    def __init__(self, _symbol):
        pass

    def history(self, **_kwargs):
        FakeTicker.calls += 1
        if FakeTicker.error is not None:
            raise FakeTicker.error
        return frame()


class Base(unittest.TestCase):
    def setUp(self):
        mh._cache.clear()
        mh._last_view = None
        mh._yahoo_blocked_until = 0.0
        mh._yahoo_strikes = 0
        FakeTicker.calls = 0
        FakeTicker.error = None


class YahooParity(Base):
    def test_native_copy_gives_the_same_answer_as_the_browser_backend(self):
        start, end = 1767571200, 1767916800
        with mock.patch("yfinance.Ticker", FakeTicker), mock.patch.object(server.yf, "Ticker", FakeTicker), mock.patch("time.time", return_value=1800000000):
            for interval in ("1M", "1D"):
                ours = mh.fetch_yahoo("AAPL", interval, start, end)
                theirs = server.yahoo_history("AAPL", interval, start, end)
                self.assertEqual(ours, theirs)
        self.assertEqual(ours["rejected"], 1)
        self.assertEqual(len(ours["bars"]), 4)


class Load(Base):
    def load(self, **overrides):
        args = {"source": "yahoo", "symbol": "aapl", "interval": "1M", "start": START, "end": END}
        args.update(overrides)
        return mh.load(args, HOST)

    def test_view_is_newest_first_and_honest(self):
        with mock.patch("yfinance.Ticker", FakeTicker):
            view = self.load()
        self.assertEqual(view["symbol"], "AAPL")
        times = [bar["time"] for bar in view["bars"]]
        self.assertEqual(times, sorted(times, reverse=True))
        self.assertEqual(view["rejected"], 1)
        self.assertEqual(view["gaps"]["scope"], "within_trading_days")
        self.assertEqual(view["export_name"], f"AAPL_1M_{START}_to_{END}.csv")
        volumes = {bar["time"]: bar["volume"] for bar in view["bars"]}
        self.assertIn(None, volumes.values())
        self.assertNotIn(0, volumes.values())
        self.assertLess(min(bar["low"] for bar in view["bars"]), 0)  # Negative prices are kept.

    def test_repeat_request_uses_the_cache(self):
        with mock.patch("yfinance.Ticker", FakeTicker):
            self.load()
            self.load()
            self.assertEqual(FakeTicker.calls, 1)
            self.load(interval="1D")
            self.assertEqual(FakeTicker.calls, 2)

    def test_expired_cache_fetches_again(self):
        with mock.patch("yfinance.Ticker", FakeTicker):
            self.load()
            key = next(iter(mh._cache))
            mh._cache[key] = (time.time() - mh.CACHE_SECONDS - 1, mh._cache[key][1])
            self.load()
        self.assertEqual(FakeTicker.calls, 2)

    def test_rate_limit_gives_a_plain_message_and_pauses_requests(self):
        from yfinance.exceptions import YFRateLimitError

        FakeTicker.error = YFRateLimitError()
        with mock.patch("yfinance.Ticker", FakeTicker):
            with self.assertRaisesRegex(ProviderError, "slow down"):
                self.load()
            with self.assertRaisesRegex(ProviderError, "slow down"):
                self.load()
        self.assertEqual(FakeTicker.calls, 1)

    def test_failed_load_leaves_nothing_to_export(self):
        with mock.patch("yfinance.Ticker", FakeTicker):
            self.load()
        self.assertIsNotNone(mh._last_view)
        with self.assertRaises(mh.history.BadRequest):
            self.load(symbol="")
        self.assertIsNone(mh._last_view)

    def test_invalid_requests_are_correctable_messages(self):
        bad = [
            {"source": "elsewhere"},
            {"interval": "7H"},
            {"symbol": "not a symbol"},
            {"symbol": "=CMD"},
            {"symbol": "-1"},
            {"source": "coinbase", "symbol": "AAPL", "interval": "1D"},
            {"start": "yesterday"},
            {"start": END, "end": START},
            {"start": "2999-01-01", "end": "2999-01-02"},
        ]
        for overrides in bad:
            with self.subTest(overrides), self.assertRaises(mh.history.BadRequest):
                self.load(**overrides)
        self.assertEqual(FakeTicker.calls, 0)

    def test_cancel_before_the_request_sends_nothing(self):
        cancelled = types.SimpleNamespace(raise_if_cancelled=mock.Mock(side_effect=runner.JobCancelled()))
        with mock.patch("yfinance.Ticker", FakeTicker), self.assertRaises(runner.JobCancelled):
            mh.load({"source": "yahoo", "symbol": "AAPL", "interval": "1D", "start": START, "end": END}, cancelled)
        self.assertEqual(FakeTicker.calls, 0)

    def test_coinbase_failure_on_any_page_fails_the_whole_request(self):
        pages = [[[1767657600, 90.0, 110.0, 100.0, 105.0, 5.0]], ProviderError(502, "Could not reach the provider.")]
        with mock.patch.object(mh._rest, "get", side_effect=pages), self.assertRaisesRegex(ProviderError, "Could not reach"):
            mh.load({"source": "coinbase", "symbol": "BTC-USD", "interval": "1M", "start": "2026-01-05", "end": "2026-01-06"}, HOST)
        self.assertIsNone(mh._last_view)

    def test_options_match_the_shared_timeframes(self):
        sources = {s["id"]: s for s in mh.options({}, HOST)["sources"]}
        self.assertEqual(sources["coinbase"]["intervals"], list(mh.history.COINBASE_STEPS))
        self.assertEqual(sources["yahoo"]["intervals"], list(mh.history.YAHOO_STEPS))
        self.assertEqual(set(mh.YAHOO_INTERVALS), set(mh.history.YAHOO_STEPS))
        self.assertEqual({k: v[0] for k, v in server.INTERVALS.items()}, mh.YAHOO_INTERVALS)


class Export(Base):
    def test_export_is_exactly_the_loaded_view_without_downloading_again(self):
        with mock.patch("yfinance.Ticker", FakeTicker):
            view = mh.load({"source": "yahoo", "symbol": "AAPL", "interval": "1M", "start": START, "end": END}, HOST)
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "out.csv")
            result = mh.export_csv({"path": path}, HOST)
            with open(path, encoding="utf-8", newline="") as file:
                rows = list(csv.DictReader(file))
            self.assertEqual(os.listdir(folder), ["out.csv"])
        self.assertEqual(FakeTicker.calls, 1)
        self.assertEqual(result["rows"], len(view["bars"]))
        self.assertEqual(len(rows), len(view["bars"]))
        for row, bar in zip(rows, view["bars"]):
            self.assertEqual(float(row["close"]), bar["close"])
            self.assertEqual(row["volume"], "" if bar["volume"] is None else repr(float(bar["volume"])))

    def test_failed_write_leaves_no_files_and_keeps_an_existing_file(self):
        mh._last_view = {"bars": [], "symbol": "A", "interval": "1D", "source": "yahoo", "downloaded_at_utc": ""}
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "out.csv")
            Path(path).write_text("keep me", encoding="utf-8")
            with mock.patch("os.replace", side_effect=PermissionError("in use")), self.assertRaises(PermissionError):
                mh.export_csv({"path": path}, HOST)
            self.assertEqual(os.listdir(folder), ["out.csv"])
            self.assertEqual(Path(path).read_text(encoding="utf-8"), "keep me")

    def test_export_before_loading_is_a_plain_message(self):
        with self.assertRaises(mh.history.BadRequest):
            mh.export_csv({"path": "unused.csv"}, HOST)


if __name__ == "__main__":
    unittest.main()
