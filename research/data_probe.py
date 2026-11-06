"""Small, read-only provider probe for the Stage 1.1 data audit.

Measures what Coinbase and Yahoo actually return for the terminal's timeframes, and how
far back Coinbase candles go. It makes a few dozen requests, downloads no large dataset
and stores summary statistics only, never raw provider data.

Usage: python research/data_probe.py [output.json]
"""
from __future__ import annotations

import json
import math
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter
from datetime import datetime, timedelta, timezone

import yfinance as yf

COINBASE = "https://api.exchange.coinbase.com"
HEADERS = {"User-Agent": "ResoluteQuant-research/1.0", "Accept": "application/json"}
CRYPTO = ["BTC-USD", "ETH-USD"]
CRYPTO_GRANS = {"1m": 60, "5m": 300, "15m": 900, "1H": 3600, "4H-native?": 14400, "6H": 21600, "1D": 86400, "1W-native?": 604800}
HISTORY_POINTS = {"2y": 730, "5y": 1825, "8y": 2920}
YAHOO = ["AAPL", "^GSPC", "HSBA.L", "GBPUSD=X", "BTC-USD"]
YAHOO_INTERVALS = {"1m": ("1m", "5d"), "5m": ("5m", "1mo"), "15m": ("15m", "1mo"), "1H": ("60m", "1y"),
                   "4H-native?": ("4h", "1y"), "1D": ("1d", "5y"), "1W": ("1wk", "10y"), "1MO": ("1mo", "max")}


def get_json(path):
    request = urllib.request.Request(COINBASE + path, headers=HEADERS)
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode(errors="replace")[:160]
    finally:
        time.sleep(0.2)


def iso(seconds):
    return datetime.fromtimestamp(seconds, timezone.utc).isoformat() if seconds is not None else None


def summarise(bars, step=None):
    """bars: list of (time_seconds, open, high, low, close, volume) in provider order."""
    if not bars:
        return {"rows": 0}
    times = [b[0] for b in bars]
    ordered = sorted(set(times))
    diffs = Counter(b - a for a, b in zip(ordered, ordered[1:]))
    modal = diffs.most_common(1)[0][0] if diffs else None
    expected = step or modal
    off_step = {str(k): v for k, v in diffs.items() if k != expected}
    missing = sum((k // expected - 1) * v for k, v in diffs.items() if expected and k > expected)
    invalid = sum(1 for _, o, h, l, c, _v in bars if not all(isinstance(x, (int, float)) and math.isfinite(x) and x > 0 for x in (o, h, l, c)) or h < max(o, c) - 1e-9 or l > min(o, c) + 1e-9)
    return {
        "rows": len(bars), "first_utc": iso(ordered[0]), "last_utc": iso(ordered[-1]),
        "last_bar_age_seconds": round(time.time() - ordered[-1]),
        "duplicates": len(times) - len(ordered), "modal_step_seconds": modal,
        "missing_steps_between_rows": missing, "distinct_off_step_gaps": len(off_step),
        "largest_gap_seconds": max(diffs) if diffs else None,
        "ohlc_invalid_rows": invalid, "zero_volume_rows": sum(1 for b in bars if not b[5]),
        "provider_order": "newest_first" if times[0] > times[-1] else "oldest_first",
    }


def probe_coinbase():
    out = {"candles": {}, "history_depth": {}, "trades_sample": {}}
    for symbol in CRYPTO:
        for name, gran in CRYPTO_GRANS.items():
            status, body = get_json(f"/products/{symbol}/candles?granularity={gran}")
            if status == 200:
                bars = [(r[0], r[3], r[2], r[1], r[4], r[5]) for r in body]
                out["candles"][f"{symbol} {name}"] = summarise(bars, gran)
            else:
                out["candles"][f"{symbol} {name}"] = {"http_status": status, "body": body}
    now = datetime.now(timezone.utc)
    for label, days in HISTORY_POINTS.items():
        start = now - timedelta(days=days)
        query = urllib.parse.urlencode({"granularity": 60, "start": start.isoformat(), "end": (start + timedelta(hours=5)).isoformat()})
        status, body = get_json(f"/products/BTC-USD/candles?{query}")
        out["history_depth"][f"BTC-USD 1m, {label} ago"] = {"http_status": status, "rows_in_5h_window": len(body) if status == 200 else None}
    status, body = get_json("/products/BTC-USD/trades?limit=100")
    if status == 200:
        out["trades_sample"] = {"rows": len(body), "fields": sorted(body[0]), "newest": body[0]["time"], "oldest": body[-1]["time"]}
    status, body = get_json("/products/BTC-USD")
    if status == 200:
        out["product_fields"] = {k: body.get(k) for k in ("quote_increment", "base_increment", "status", "trading_disabled", "cancel_only", "post_only", "limit_only")}
    return out


def probe_yahoo():
    out = {}
    for symbol in YAHOO:
        for name, (interval, period) in YAHOO_INTERVALS.items():
            key = f"{symbol} {name}"
            try:
                frame = yf.Ticker(symbol).history(period=period, interval=interval, auto_adjust=False)
                bars = [(int(i.timestamp()), float(r["Open"]), float(r["High"]), float(r["Low"]), float(r["Close"]), float(r.get("Volume") or 0))
                        for i, r in frame.iterrows() if not any(math.isnan(float(r[k])) for k in ("Open", "High", "Low", "Close"))]
                row = summarise(bars)
                row["nan_rows_dropped"] = len(frame) - len(bars)
                row["columns"] = list(frame.columns)
                row["index_timezone"] = str(frame.index.tz)
                out[key] = row
            except Exception as error:  # record the provider's refusal as evidence
                out[key] = {"error": f"{type(error).__name__}: {str(error)[:140]}"}
            time.sleep(0.4)
    return out


def main():
    result = {"generated_utc": datetime.now(timezone.utc).isoformat(), "yfinance_version": yf.__version__,
              "coinbase": probe_coinbase(), "yahoo": probe_yahoo()}
    target = sys.argv[1] if len(sys.argv) > 1 else "data_probe.json"
    with open(target, "w", encoding="utf-8") as handle:
        json.dump(result, handle, indent=1)
    print("wrote", target)


if __name__ == "__main__":
    main()
