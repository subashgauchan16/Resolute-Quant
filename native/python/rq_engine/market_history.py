"""Market-price history for the native app, reusing the browser terminal's history and validation code.

Missing bars are reported and never filled. Unfinished bars are left out and counted. A failed page
fails the whole request, so a partial range is never presented as complete.
"""

import math
import os
import re
import threading
import time
from datetime import datetime, timezone

import history
from crypto_feed import PAIR, ProviderError, Rest, normalise_candles

CACHE_SECONDS = 300
SYMBOL = re.compile(r"^[A-Z0-9.^=\-]{1,15}$")
SOURCES = {"coinbase": history.COINBASE_STEPS, "yahoo": history.YAHOO_STEPS}
YAHOO_INTERVALS = {"1M": "1m", "5M": "5m", "15M": "15m", "1H": "60m", "4H": "4h", "1D": "1d", "1W": "1wk", "1MO": "1mo"}

_rest = Rest()
_cache = {}  # key: (time stored, provider result)
_last_view = None
_yahoo_blocked_until = 0.0
_yahoo_strikes = 0
_yahoo_last = 0.0
_yahoo_lock = threading.Lock()
MIN_YAHOO_GAP = 0.3  # The same polite gap as the browser terminal.


def options(_args, _host):
    return {
        "sources": [
            {"id": "coinbase", "label": "Coinbase", "example": "BTC-USD", "intervals": list(history.COINBASE_STEPS)},
            {"id": "yahoo", "label": "Yahoo Finance", "example": "AAPL", "intervals": list(history.YAHOO_STEPS)},
        ]
    }


def _clean(value):
    """NaN, infinity and missing values become None, so validation can see them."""
    if value is None:
        return None
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def _yahoo_rows(frame):
    rows = [[int(stamp.timestamp()), _clean(row["Low"]), _clean(row["High"]), _clean(row["Open"]), _clean(row["Close"]), _clean(row.get("Volume"))]
            for stamp, row in frame.iterrows()]
    day_of = {int(stamp.timestamp()): stamp.strftime("%Y-%m-%d") for stamp in frame.index}  # Yahoo's index is exchange time
    return rows, day_of


def yahoo_limited():
    return _yahoo_blocked_until > time.time()


def yahoo_call(fn):
    """Every Yahoo request passes through here, one at a time, with a polite gap and backoff."""
    global _yahoo_blocked_until, _yahoo_strikes, _yahoo_last
    wait = _yahoo_blocked_until - time.time()
    if wait > 0:
        raise ProviderError(429, f"Yahoo asked this PC to slow down. Try again in {math.ceil(wait)} seconds.")

    from yfinance.exceptions import YFRateLimitError  # Slow to import, so only loaded when first needed.

    with _yahoo_lock:
        gap = MIN_YAHOO_GAP - (time.time() - _yahoo_last)
        if gap > 0:
            time.sleep(gap)
        _yahoo_last = time.time()
        try:
            result = fn()
        except YFRateLimitError as error:
            _yahoo_strikes += 1
            pause = min(900, 60 * 2 ** (_yahoo_strikes - 1))
            _yahoo_blocked_until = time.time() + pause
            raise ProviderError(429, f"Yahoo asked this PC to slow down. Try again in {pause} seconds.") from error
        _yahoo_strikes = 0
        return result


def fetch_yahoo(symbol, interval, start, end):
    """Unadjusted Yahoo bars for a date range. A copy of server.yahoo_history, kept equal by a test."""
    import yfinance as yf

    begin = datetime.fromtimestamp(start, timezone.utc)
    finish = datetime.fromtimestamp(end, timezone.utc)
    frame = yahoo_call(lambda: yf.Ticker(symbol).history(start=begin, end=finish, interval=YAHOO_INTERVALS[interval], auto_adjust=False))
    rows, day_of = _yahoo_rows(frame)
    bars, rejected = normalise_candles(rows, market=True)
    kept = bars[-history.MAX_BARS:]
    return {"bars": kept, "rejected": rejected, "truncated": len(bars) > history.MAX_BARS, "downloaded_at": int(time.time()),
            "gaps": history.session_gaps(kept, history.YAHOO_STEPS[interval], day_of)}


def fetch_provider(source, symbol, interval, start, end):
    if source == "coinbase":
        return _rest.candles_between(symbol, history.COINBASE_STEPS[interval], start, end)
    return fetch_yahoo(symbol, interval, start, end)


def _cached_fetch(fetch, source, symbol, interval, start, end):
    key = (fetch, source, symbol, interval, start // 60, end // 60)
    stored = _cache.get(key)
    if stored is not None and time.time() - stored[0] < CACHE_SECONDS:
        return stored[1]
    result = fetch(source, symbol, interval, start, end)
    _cache[key] = (time.time(), result)
    return result


def _build(args, host, fetch):
    global _last_view
    _last_view = None
    source = args.get("source", "")
    interval = str(args.get("interval", "")).upper()
    symbol = str(args.get("symbol", "")).strip().upper()
    steps = SOURCES.get(source)
    if steps is None:
        raise history.BadRequest("The source must be coinbase or yahoo.")
    if interval not in steps:
        raise history.BadRequest(f"{source} does not offer that timeframe. Choose from {', '.join(steps)}.")
    if not SYMBOL.match(symbol):
        raise history.BadRequest("Enter a symbol, such as BTC-USD or AAPL.")
    if source == "coinbase" and not PAIR.match(symbol):
        raise history.BadRequest("Coinbase needs a pair written like BTC-USD.")
    if symbol[0] in "=-":  # A spreadsheet would read these as formulas.
        raise history.BadRequest("That is not a valid symbol.")
    start_text, end_text = args.get("start", ""), args.get("end", "")
    start, end = history.parse_range(start_text, end_text)

    host.raise_if_cancelled()
    fetched = _cached_fetch(fetch, source, symbol, interval, start, end)
    host.raise_if_cancelled()

    view = history.build_view(fetched, symbol, interval, source, steps[interval], start_text, end_text)
    view["export_name"] = history.export_name(view)
    _last_view = view
    return view


def load(args, host):
    return _build(args, host, fetch_provider)


def export_csv(args, _host):
    """Writes exactly the bars last shown in the window. Nothing is downloaded again."""
    if _last_view is None:
        raise history.BadRequest("Load some history before exporting.")
    path = args["path"]
    text = history.to_csv(_last_view)
    temporary = f"{path}.{threading.get_ident()}.tmp"
    try:
        with open(temporary, "w", encoding="utf-8", newline="") as file:
            file.write(text)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)
    return {"path": path, "rows": len(_last_view["bars"])}


def _fixture_fetch(_source, _symbol, _interval, _start, end):
    """Synthetic bars for tests, which never touch a provider. The UI has no way to request these."""
    step = history.DAY
    first = end - end % step - 41 * step
    bars = []
    for index in range(40):
        if index == 20:  # Leaves one missing bar, for the gap report.
            continue
        rising = index % 3 != 0
        low, high = 100.0 + index, 110.5 + index
        opened, close = (102.0 + index, 108.25 + index) if rising else (108.25 + index, 102.0 + index)
        bars.append({"time": first + index * step, "open": opened, "high": high, "low": low, "close": close,
                     "volume": None if index == 5 else 1000.0 + index})
    return {"bars": bars, "rejected": 2, "truncated": False, "downloaded_at": int(time.time())}


def fixture_for_test(args, host):
    return _build(args, host, _fixture_fetch)


def import_for_test(_args, _host):
    """Loads every native library the provider code needs, to prove they work embedded."""
    import curl_cffi
    import lxml.etree
    import pandas
    import websockets
    import yfinance

    return {"yfinance": yfinance.__version__, "pandas": pandas.__version__, "curl_cffi": curl_cffi.__version__,
            "lxml": lxml.etree.__version__, "websockets": websockets.__version__}
