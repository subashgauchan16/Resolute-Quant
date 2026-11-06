"""The numerical and long-running demonstrations. These prove the bridge. They are not models."""

import math
import time

import numpy as np


def _parse_numbers(text):
    parts = text.replace(",", " ").split()
    numbers = []
    for part in parts:
        try:
            number = float(part)
        except ValueError:
            raise ValueError(f"'{part}' is not a number.") from None
        if not math.isfinite(number):
            raise ValueError(f"'{part}' is not a finite number.")
        numbers.append(number)
    if len(numbers) < 2:
        raise ValueError("Enter at least two numbers.")
    return np.array(numbers, dtype=np.float64)


def summary_statistics(args, _host):
    values = _parse_numbers(args.get("text", ""))
    return {
        "count": int(values.size),
        "mean": float(np.mean(values)),
        "sample_std_dev": float(np.std(values, ddof=1)),
    }


def long_demo(args, host):
    """Keeps the CPU busy in Python for a set time, reporting progress and honouring cancel."""
    seconds = float(args.get("seconds", 5.0))
    steps = 50
    data = np.arange(200_000, dtype=np.float64)
    for step in range(steps):
        host.raise_if_cancelled()
        step_end = time.monotonic() + seconds / steps
        while time.monotonic() < step_end:
            np.sqrt(data).sum()
        host.progress((step + 1) / steps, f"Step {step + 1} of {steps}")
    return {"steps_completed": steps}


def stuck_for_test(args, _host):
    """Ignores cancel, like a native call that cannot be interrupted. Used by the shutdown test."""
    time.sleep(float(args.get("seconds", 30.0)))
    return {}


def exit_for_test(_args, _host):
    """A job calling sys.exit must fail that job, not close the app."""
    raise SystemExit(3)
