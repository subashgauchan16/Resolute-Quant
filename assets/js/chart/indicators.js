/* Indicator maths. Pure functions only: no DOM, chart or network access. */
export const EMA_PERIOD = 20;

export function emaStep(value, prev, period = EMA_PERIOD) {
  const k = 2 / (period + 1);
  return value * k + prev * (1 - k);
}

/* Returns the line plus the last two values, so a live bar can be re-stepped from `prev`. */
export function emaSeries(bars, period = EMA_PERIOD) {
  let cur = bars[0].close;
  let prev = cur;
  const points = bars.map((bar, i) => {
    prev = cur;
    if (i) cur = emaStep(bar.close, cur, period);
    return { time: bar.time, value: cur };
  });
  return { points, prev, cur };
}
