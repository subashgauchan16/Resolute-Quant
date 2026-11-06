"""Market-price history: date ranges, paging, coverage reporting and CSV.

No network access and no web framework, so this logic can be tested on its own and reused by a
future desktop interface. Missing bars are reported and never filled. Bars that have not finished
are left out and counted, because their values can still change.
"""
from __future__ import annotations

import csv
import io
import time
from datetime import datetime, timezone

MAX_BARS = 5000
COINBASE_PAGE = 300  # the documented maximum candles per request
DAY = 86400
WIDEST_UTC_OFFSET = 14 * 3600  # a month bar starts at local midnight, up to 14 hours before UTC midnight

# Seconds per bar. None marks a calendar month, which has no fixed length.
COINBASE_STEPS = {"1M": 60, "5M": 300, "15M": 900, "1H": 3600, "6H": 21600, "1D": DAY}
YAHOO_STEPS = {"1M": 60, "5M": 300, "15M": 900, "1H": 3600, "4H": 14400, "1D": DAY, "1W": 7 * DAY, "1MO": None}

COLUMNS = ("time_utc", "symbol", "interval", "open", "high", "low", "close", "volume", "source", "downloaded_at_utc")


class BadRequest(ValueError):
    """A request the caller can correct. The message is safe to show on the page."""


def iso_utc(seconds):
    return datetime.fromtimestamp(seconds, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_day(text, name):
    try:
        day = datetime.strptime(text or "", "%Y-%m-%d")
    except ValueError as error:
        raise BadRequest(f"{name} must be a date written as YYYY-MM-DD.") from error
    return int(day.replace(tzinfo=timezone.utc).timestamp())


def parse_range(start_text, end_text, now=None):
    """Whole UTC days, both inclusive. Returns epoch seconds with an exclusive end, capped at now."""
    now = int(time.time() if now is None else now)
    start = parse_day(start_text, "The start date")
    end = parse_day(end_text, "The end date") + DAY
    if start >= end:
        raise BadRequest("The start date must not be after the end date.")
    if start >= now:
        raise BadRequest("The start date is in the future, so there is no history yet.")
    return start, min(end, now)


def page_windows(start, end, step, max_bars=MAX_BARS, page=COINBASE_PAGE):
    """Provider request windows from newest to oldest. Returns (windows, truncated)."""
    first = max(start, end - max_bars * step)
    windows, stop = [], end
    while stop > first:
        begin = max(first, stop - page * step)
        windows.append((begin, stop))
        stop = begin
    return windows, first > start


def complete_only(bars, step, now=None):
    """Split ascending bars into finished bars and a count of unfinished ones."""
    now = int(time.time() if now is None else now)
    if step is None:
        current = datetime.fromtimestamp(now, timezone.utc).strftime("%Y-%m")
        done = [b for b in bars if datetime.fromtimestamp(b["time"] + WIDEST_UTC_OFFSET, timezone.utc).strftime("%Y-%m") < current]
    else:
        done = [b for b in bars if b["time"] + step <= now]
    return done, len(bars) - len(done)


def gap_summary(bars, step):
    """Count missing steps between ascending bars. Monthly bars have no fixed step, so are not counted."""
    if step is None:
        return {"count": 0, "missing_bars": 0, "largest_seconds": 0, "scope": "all"}
    gaps = [b["time"] - a["time"] for a, b in zip(bars, bars[1:]) if b["time"] - a["time"] > step]
    return {"count": len(gaps), "missing_bars": sum(g // step - 1 for g in gaps), "largest_seconds": max(gaps, default=0), "scope": "all"}


def session_gaps(bars, step, day_of):
    """Gaps inside one exchange-local day only, because closures between days are normal.

    Trading calendars are not known here, so a whole missing day is not detected.
    day_of maps a bar's time to its exchange-local date.
    """
    if step is None or step >= DAY:
        return {"count": 0, "missing_bars": 0, "largest_seconds": 0, "scope": "within_trading_days"}
    gaps = [b["time"] - a["time"] for a, b in zip(bars, bars[1:]) if b["time"] - a["time"] > step and day_of.get(a["time"]) == day_of.get(b["time"])]
    return {"count": len(gaps), "missing_bars": sum(g // step - 1 for g in gaps), "largest_seconds": max(gaps, default=0), "scope": "within_trading_days"}


def number(value):
    """The shortest text that reads back as exactly the same number. Missing stays blank."""
    return "" if value is None else repr(float(value))


def build_view(fetched, symbol, interval, source, step, start_text, end_text, stale=False, now=None):
    """The answer for the page, newest first, from a provider result and its coverage facts."""
    done, unfinished = complete_only(fetched["bars"], step, now)
    return {
        "symbol": symbol, "interval": interval, "source": source,
        "start": start_text, "end": end_text,
        "bars": list(reversed(done)), "count": len(done),
        "first_utc": iso_utc(done[0]["time"]) if done else None,
        "last_utc": iso_utc(done[-1]["time"]) if done else None,
        "gaps": fetched.get("gaps") or gap_summary(done, step), "rejected": fetched["rejected"], "unfinished_excluded": unfinished,
        "truncated": fetched["truncated"], "limit": MAX_BARS, "stale": stale,
        "downloaded_at_utc": iso_utc(fetched["downloaded_at"]),
    }


def to_csv(view):
    """UTF-8-ready CSV text of a view: a header row, then one row per bar in the view's order."""
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\r\n")
    writer.writerow(COLUMNS)
    for bar in view["bars"]:
        writer.writerow((iso_utc(bar["time"]), view["symbol"], view["interval"], number(bar["open"]), number(bar["high"]),
                         number(bar["low"]), number(bar["close"]), number(bar["volume"]), view["source"], view["downloaded_at_utc"]))
    return out.getvalue()


def export_name(view):
    safe = "".join(c for c in view["symbol"] if c.isalnum() or c in ".-") or "symbol"
    return f"{safe}_{view['interval']}_{view['start']}_to_{view['end']}.csv"
