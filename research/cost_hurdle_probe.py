"""Short read-only sample: quoted spread plus typical move size by horizon, for BTC-USD and ETH-USD.

Takes one minute. The sample is small and recent, so it gives a sense of scale for choosing a
horizon. It is not a backtest and says nothing about whether any signal works.

Usage: python research/cost_hurdle_probe.py [output.json]
"""
import json
import math
import statistics
import sys
import time
import urllib.request
from datetime import datetime, timezone

BASE = "https://api.exchange.coinbase.com"
HEADERS = {"User-Agent": "ResoluteQuant-research/1.0"}


def get(path):
    with urllib.request.urlopen(urllib.request.Request(BASE + path, headers=HEADERS), timeout=15) as response:
        return json.load(response)


def spreads(symbol, samples=30, gap=2):
    values = []
    for _ in range(samples):
        book = get(f"/products/{symbol}/book?level=1")
        bid, ask = float(book["bids"][0][0]), float(book["asks"][0][0])
        values.append((ask - bid) / ((ask + bid) / 2) * 1e4)
        time.sleep(gap)
    ordered = sorted(values)
    return {"samples": samples, "seconds_between": gap, "median_bps": round(statistics.median(values), 3),
            "p90_bps": round(ordered[int(0.9 * (samples - 1))], 3), "max_bps": round(max(values), 3)}


def moves(symbol, granularity, horizons):
    rows = sorted(get(f"/products/{symbol}/candles?granularity={granularity}"), key=lambda r: r[0])
    closes = [r[4] for r in rows]
    out = {"bars": len(closes), "first_utc": datetime.fromtimestamp(rows[0][0], timezone.utc).isoformat(),
           "last_utc": datetime.fromtimestamp(rows[-1][0], timezone.utc).isoformat(), "horizons": {}}
    for name, h in horizons.items():
        returns = [abs(math.log(closes[i + h] / closes[i])) * 1e4 for i in range(len(closes) - h)]
        out["horizons"][name] = {"overlapping_observations": len(returns), "median_abs_move_bps": round(statistics.median(returns), 1)}
    time.sleep(0.2)
    return out


def main():
    result = {"generated_utc": datetime.now(timezone.utc).isoformat(), "spread": {}, "moves": {}}
    for symbol in ("BTC-USD", "ETH-USD"):
        result["spread"][symbol] = spreads(symbol)
        result["moves"][symbol] = {
            "15m bars": moves(symbol, 900, {"15m": 1, "1H": 4, "4H": 16}),
            "1H bars": moves(symbol, 3600, {"1H": 1, "4H": 4, "24H": 24}),
            "1D bars": moves(symbol, 86400, {"1D": 1, "5D": 5, "20D": 20}),
        }
    target = sys.argv[1] if len(sys.argv) > 1 else "cost_hurdle_probe.json"
    with open(target, "w", encoding="utf-8") as handle:
        json.dump(result, handle, indent=1)
    print(json.dumps(result, indent=None)[:3000])


if __name__ == "__main__":
    main()
