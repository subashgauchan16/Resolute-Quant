import math
import unittest

import numpy as np

from research_models import MIN_BARS, run_research


def sample_bars(n=240):
    bars = []
    price = 100.0
    for i in range(n):
        # Deterministic, non-constant series; test data only.
        price *= math.exp(0.0002 + 0.006 * math.sin(i / 7) + 0.002 * math.cos(i / 3))
        bars.append({"time": 1_700_000_000 + i * 86400, "close": price})
    return bars


class ResearchModelTests(unittest.TestCase):
    def test_returns_forecast_metrics_and_simulation_fan(self):
        result = run_research(sample_bars())
        self.assertEqual(result["model_status"], "ready")
        self.assertEqual(result["bar_count"], 240)
        self.assertGreater(result["validation_rows"], 0)
        self.assertIn("ridge", result["validation"])
        self.assertIn("sgd", result["validation"])
        for model_name in ("zero_return", "ridge", "sgd"):
            for metric_name in ("mae_pct", "rmse_pct", "directional_accuracy_pct"):
                value = result["validation"][model_name][metric_name]
                self.assertIsNotNone(value)
                self.assertTrue(math.isfinite(value), (model_name, metric_name, value))
        for value in result["next_bar"].values():
            self.assertTrue(math.isfinite(value), value)
        self.assertEqual(len(result["simulation"]["fan"]), 24)
        self.assertEqual(result["simulation"]["paths"], 500)
        for point in result["simulation"]["fan"]:
            self.assertLessEqual(point["p05"], point["p25"])
            self.assertLessEqual(point["p25"], point["p50"])
            self.assertLessEqual(point["p50"], point["p75"])
            self.assertLessEqual(point["p75"], point["p95"])

    def test_requires_sufficient_data(self):
        with self.assertRaises(ValueError):
            run_research(sample_bars(MIN_BARS - 1))

    def test_deduplicates_and_drops_invalid_candles(self):
        bars = sample_bars()
        bars.extend([bars[-1], {"time": "bad", "close": 1}, {"time": 9999999999, "close": float("nan")}])
        result = run_research(bars)
        self.assertEqual(result["bar_count"], 240)

    def test_repeatable_for_same_final_bar(self):
        bars = sample_bars()
        self.assertEqual(run_research(bars)["simulation"]["fan"], run_research(bars)["simulation"]["fan"])


if __name__ == "__main__":
    unittest.main()
