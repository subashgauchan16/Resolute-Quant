"""Transparent research models for Yahoo Finance equity/market candles.

This module does not place orders. Coinbase-sourced candles are intentionally excluded
until written provider permission is available for model research.
"""
from __future__ import annotations

import math
from typing import Iterable

import numpy as np

FEATURE_LOOKBACK = 20
MIN_BARS = 80
RIDGE_ALPHA = 1.0
SIMULATION_PATHS = 500
SIMULATION_HORIZON = 24


def _metrics(actual: np.ndarray, predicted: np.ndarray) -> dict:
    if actual.size == 0:
        return {"mae_pct": None, "rmse_pct": None, "directional_accuracy_pct": None}
    errors = predicted - actual
    return {
        "mae_pct": round(float(np.mean(np.abs(errors)) * 100), 5),
        "rmse_pct": round(float(np.sqrt(np.mean(errors ** 2)) * 100), 5),
        "directional_accuracy_pct": round(float(np.mean(np.sign(predicted) == np.sign(actual)) * 100), 2),
    }


def _features(returns: np.ndarray, end: int) -> np.ndarray:
    """Features available immediately before the target return at index end."""
    past = returns[:end]
    return np.array([
        past[-1], past[-2], past[-5], past[-10],
        float(np.std(past[-5:], ddof=1)),
        float(np.std(past[-20:], ddof=1)),
        float(np.mean(past[-5:])),
        float(np.mean(past[-20:])),
    ], dtype=float)


def _samples(returns: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    xs, ys = [], []
    for target_index in range(FEATURE_LOOKBACK, len(returns)):
        xs.append(_features(returns, target_index))
        ys.append(returns[target_index])
    return np.asarray(xs, dtype=float), np.asarray(ys, dtype=float)


def _prepare(x_train: np.ndarray, x_other: np.ndarray):
    mean = x_train.mean(axis=0)
    scale = x_train.std(axis=0)
    scale[scale < 1e-12] = 1.0
    return (x_train - mean) / scale, (x_other - mean) / scale, mean, scale


def _fit_ridge(x: np.ndarray, y: np.ndarray, alpha: float = RIDGE_ALPHA):
    """Ridge with an unpenalised intercept, solved with stable least squares."""
    design = np.column_stack([np.ones(len(x)), x])
    penalty = np.eye(design.shape[1]) * alpha
    penalty[0, 0] = 0.0
    return np.linalg.solve(design.T @ design + penalty, design.T @ y)


def _predict(weights: np.ndarray, x: np.ndarray) -> np.ndarray:
    return np.column_stack([np.ones(len(x)), x]) @ weights


def _fit_sgd(x: np.ndarray, y: np.ndarray, epochs: int = 500, learning_rate: float = 0.01):
    """Deterministic full-pass SGD on standardised targets; intercept is not penalised."""
    y_scale = float(np.std(y))
    if y_scale < 1e-12:
        y_scale = 1.0
    target = y / y_scale
    design = np.column_stack([np.ones(len(x)), x])
    weights = np.zeros(design.shape[1], dtype=float)
    for _ in range(epochs):
        for row, actual in zip(design, target):
            error = float(row @ weights - actual)
            weights[0] -= learning_rate * error
            weights[1:] -= learning_rate * (error * row[1:] + RIDGE_ALPHA * weights[1:] / max(1, len(x)))
    return weights, y_scale


def _bars_to_returns(bars: Iterable[dict]) -> tuple[list[dict], np.ndarray]:
    clean = []
    for bar in bars:
        try:
            stamp, close = int(bar["time"]), float(bar["close"])
        except (KeyError, TypeError, ValueError, OverflowError):
            continue
        if stamp < 0 or not math.isfinite(close) or close <= 0:
            continue
        clean.append({"time": stamp, "close": close})
    clean.sort(key=lambda item: item["time"])
    deduped = []
    for item in clean:
        if deduped and item["time"] == deduped[-1]["time"]:
            deduped[-1] = item
        else:
            deduped.append(item)
    if len(deduped) < MIN_BARS:
        raise ValueError(f"Need at least {MIN_BARS} valid bars; received {len(deduped)}.")
    closes = np.asarray([item["close"] for item in deduped], dtype=float)
    returns = np.diff(np.log(closes))
    if not np.all(np.isfinite(returns)) or len(returns) < FEATURE_LOOKBACK + 10:
        raise ValueError("Not enough valid returns to create model features.")
    return deduped, returns


def run_research(bars: Iterable[dict]) -> dict:
    """Validate candles, evaluate baselines chronologically, refit and project 24 bars."""
    clean_bars, returns = _bars_to_returns(bars)
    x, y = _samples(returns)
    if len(y) < 40:
        raise ValueError("Not enough feature/target rows for a chronological holdout.")

    split = max(30, int(len(y) * 0.8))
    split = min(split, len(y) - 10)
    x_train_raw, x_test_raw = x[:split], x[split:]
    y_train, y_test = y[:split], y[split:]
    x_train, x_test, _, _ = _prepare(x_train_raw, x_test_raw)

    ridge_weights = _fit_ridge(x_train, y_train)
    ridge_test = _predict(ridge_weights, x_test)
    sgd_weights, sgd_y_scale = _fit_sgd(x_train, y_train)
    sgd_test = _predict(sgd_weights, x_test) * sgd_y_scale

    # Refit only after the chronological evaluation has been measured.
    x_all, _, x_mean, x_scale = _prepare(x, x)
    ridge_final = _fit_ridge(x_all, y)
    sgd_final, sgd_final_scale = _fit_sgd(x_all, y)
    latest_features = _features(returns, len(returns))
    latest_scaled = ((latest_features - x_mean) / x_scale).reshape(1, -1)
    ridge_return = float(_predict(ridge_final, latest_scaled)[0])
    sgd_return = float(_predict(sgd_final, latest_scaled)[0] * sgd_final_scale)

    last_price = float(clean_bars[-1]["close"])
    observed_mean = float(np.mean(returns))
    observed_vol = float(np.std(returns, ddof=1))
    if not math.isfinite(observed_vol):
        observed_vol = 0.0

    # A seeded simulation is repeatable for a given final candle timestamp.
    seed = int(clean_bars[-1]["time"]) % (2 ** 32)
    rng = np.random.default_rng(seed)
    shocks = rng.normal(observed_mean, observed_vol, size=(SIMULATION_PATHS, SIMULATION_HORIZON))
    paths = last_price * np.exp(np.cumsum(shocks, axis=1))
    fan = []
    for step in range(SIMULATION_HORIZON):
        column = paths[:, step]
        q05, q25, q50, q75, q95 = np.quantile(column, [0.05, 0.25, 0.5, 0.75, 0.95])
        fan.append({
            "step": step + 1,
            "p05": round(float(q05), 8),
            "p25": round(float(q25), 8),
            "p50": round(float(q50), 8),
            "p75": round(float(q75), 8),
            "p95": round(float(q95), 8),
        })

    return {
        "model_status": "ready",
        "model_scope": "Yahoo Finance market candles only",
        "bar_count": len(clean_bars),
        "validation_rows": len(y_test),
        "as_of": clean_bars[-1]["time"],
        "last_price": last_price,
        "next_bar": {
            "ridge_return_pct": round(ridge_return * 100, 5),
            "ridge_price": round(last_price * math.exp(ridge_return), 8),
            "sgd_return_pct": round(sgd_return * 100, 5),
            "sgd_price": round(last_price * math.exp(sgd_return), 8),
        },
        "validation": {
            "zero_return": _metrics(y_test, np.zeros_like(y_test)),
            "ridge": _metrics(y_test, ridge_test),
            "sgd": _metrics(y_test, sgd_test),
            "method": "Chronological 80/20 holdout; final forecasts refit on all available feature rows.",
        },
        "simulation": {
            "paths": SIMULATION_PATHS,
            "horizon_bars": SIMULATION_HORIZON,
            "historical_mean_return_pct": round(observed_mean * 100, 5),
            "historical_volatility_pct": round(observed_vol * 100, 5),
            "fan": fan,
            "method": "Geometric Brownian motion approximation using observed log-return mean and volatility.",
        },
        "disclaimer": "Research estimate, not a guarantee or investment recommendation. Simulation bands are conditional on historical-return assumptions.",
    }
