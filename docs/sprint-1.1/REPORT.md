# Resolute Quant (formerly Subash Terminal): Stage 1.1 and Research Sprint 1 report

**Date:** 6 November 2026. **Scope:** terminal stabilisation, historical data audit, and research specification.

This report does not claim that any model or strategy is profitable, and it does not claim production readiness. No model was trained and no backtest was run. Live order execution does not exist in this project and was not added.

Evidence labels: **[V]** verified on this machine, **[T]** test result, **[A]** assumption, **[R]** recommendation, **[Q]** unresolved question. Raw results are in `evidence/`.

## 1. Executive summary

**Terminal.** The terminal now gets all market data through its local backend. An audit of every request made by the page during a full test run found only the local server and Google Fonts, with no provider connections and no WebSockets. The launcher handles this PC's real port conflict without touching the other service.

**Final acceptance run: 53 of 55 checks passed, with 2 failures left open.**

- **Frame smoothness fails and is unexplained.** Identical code gave a 67 ms worst frame gap in one run and 1,783 ms in the last, while other programs were busy on this PC. The cause is not established, so the check stays failed.
- **The heap limit fails, but no sustained leak was seen.** The heap grew 12.6% in the 10-minute run. A separate 33-minute trend rose during warm-up, peaked at 3.61 MB, then settled near 3.2 MB. That trend is suggestive and is not proof over longer periods.

**Data.** The project has **no historical dataset**, so no data audit has passed. What exists is a small provider probe, which shows that Coinbase can supply the history needed and that Yahoo data has quality problems.

**The finding that matters most for research: costs rule out short-horizon direction trading at retail fee tiers.** Coinbase's published lowest-tier taker fee is 60 bps, or about 120 bps for a round trip, while the sampled median BTC move over 15 minutes is about 10 bps. The 15-minute direction target proposed in the coordinator's documents would need moves about twelve times the typical size just to cover fees.

**Recommended first target:** 24-hour-ahead realised volatility of BTC-USD, sampled hourly, compared with an EWMA benchmark. It needs no tradable edge, and it tests the pipeline honestly. Every later strategy can also use it for sizing. Direction research would follow at horizons of 5 days or longer.

**Recommendation: GO for a small, approved next sprint, and NO-GO for any live trading or complex model.** Section 10 lists the decisions needed.

## 2. Verified baseline and outstanding issues

The full baseline, written before implementation, is in `00-baseline.md`. Main points:

- [V] The project had no version control: its Git root was the home folder, with no commits. No repository was initialised there. The owner has since named the public repository `subashgauchan16/Resolute-Quant` as the project's home. This work is proposed there on the review branch `stage-1.1`, **not merged into `main`**, with the original program as its first commit so that every change can be reviewed as a diff.
- [V] Before this sprint there were no tests.
- [V] Another agent's documents audit a separate, unmodified copy of the original program. Those documents contain claims that do not match the code, listed in the baseline. They were not edited.
- [V] **That agent stayed active during this sprint.** It initialised a Git repository in the `Downloads` folder and made 5 commits there, the last at 01:16 UTC. Those commits track no file of this project. A `.venv` also appeared in this project's folder at 01:28 UTC, during the final test run, from a source not established.
- [V] The Phase 1 report's claims were retested here, not assumed. Retesting found three defects it had missed: the invalid live-bar open, the unmeasured chart update rate, and the unbounded per-symbol buffer.

**Outstanding issues**

| # | Issue | Status |
| --- | --- | --- |
| 1 | Port 8765 is held by an unidentified Windows HTTP service | Handled by fallback. The owner of that service is unknown [Q] |
| 2 | Two tabs on different symbols compete for the single watchlist and book | Known limit, inherited design |
| 3 | Coinbase's newest REST candle lags by up to about 5 minutes, so a load can show a short gap before live bars | Documented, not fixed. Fixing it needs a reconciliation rule [R] |
| 4 | Yahoo history is cut to the newest 320 bars, so a US stock's `1H` chart shows about 46 trading days rather than the 1 year requested | Documented, inherited |
| 5 | 429 handling in the page is a retry message rather than the prompt's gentle warning everywhere | Phase 4 work |
| 6 | Multi-chart layout, panels and indicators from the original brief | Not started; this sprint did not cover them |
| 7 | **Frame gaps over 250 ms**, intermittent | **Open.** Failed in 2 of 3 runs on the final code. Cause not established |
| 8 | **Heap limit** exceeded in the 10-minute run | **Open.** No sustained growth in a 33-minute trend |
| 9 | The page waits for the Google Fonts stylesheet before it starts | Inherited. The intro appeared about 3.3 s after navigation on a fresh profile. Vendoring the fonts would fix this |
| 10 | The licence names a different canonical repository from its source line | Owner to check. The licence was not edited |

## 3. Workstream A: architecture and acceptance

### Architecture now

```
Coinbase REST + WebSocket ─┐
alternative.me ────────────┼─> crypto_feed.py ─┐
Yahoo (yfinance) ──────────────> server.py ────┴─> HTTP + SSE on 127.0.0.1 ─> browser page
```

Validation in the backend:

- Prices must be finite and positive.
- OHLC bars must be self-consistent.
- Timestamps need a timezone and may not be more than 60 seconds ahead of the clock.
- Symbols and granularities are checked against an allowlist.
- Bad values are **dropped and counted, never repaired**. Counts appear in `/api/health`.
- When a provider refuses, the last good copy is served with `stale: true`.

The dependency change is one line: `websockets` is now declared directly. `yfinance` already required it.

### Port policy

Order: `--port`, then `RESOLUTE_QUANT_PORT`, then `terminal.config.json`, then the default 8765.

- A **configured** port never falls back. If it is busy, the program explains and stops.
- The **default** falls back through 8766 to 8775, in order. It announces this in the console and in the page footer.
- A terminal already running is reused, and `/api/health` confirms that it really is this terminal.
- The browser opens only after the port answers with this instance's ID.
- The server now requests exclusive use of the port. [V] Python's default on Windows allowed a second listener on a busy port, so two terminals could have shared one.
- **Storage:** [T] saved watchlists and alerts do not carry across ports. They are not migrated. Because the fallback is deterministic, this PC should settle on 8766 as long as that port stays free.

**Decision needed:** this policy assumes an announced fallback is acceptable [A].

### `/api/health` contract

Purpose: launcher identity check and the page's port notice.

Fields: `service`, `status` (`ok` or `degraded`), `timestamp`, `instance`, `started`, `port`, `preferredPort`, `portSource`, `fallback`, `clients`, and `providers`, covering Yahoo stream and limit state and Coinbase connection, last message, retry time, reconnects and rejected counts.

### Timeframes (A4)

Tested against the providers. **Native** means the provider serves it directly.

| Timeframe | Coinbase (crypto) | Yahoo (stocks, FX, indices) |
| --- | --- | --- |
| `1m`, `5m`, `15m`, `1H`, `1D` | Native | Native |
| `6H` | Native | Not offered |
| `4H` | **Not available** (HTTP 400) | Native, **added** |
| `1W` | **Not available** (HTTP 400) | Native |
| `1MO` | Not offered by the API | Native, **added** |

- `1M` means one minute in commands and the API, and `1MO` means one month. Buttons show `1m` and `1MO`. [T] Measured bar steps are 60 seconds and about 31 days.
- Crypto `4H`, `1W` and `1MO` are **not offered**. No candles are invented. They could be aggregated from `1H` or `1D` in UTC-anchored buckets, but Coinbase's 350-row limit gives only about 87 `4H` bars or about 50 `1W` bars from one request. **Proposal for approval.**
- Yahoo `4H` bars are anchored to the session open, not to UTC. For AAPL that gives two bars a day of unequal length.

### Chart library (A4)

- [V] Vendored: Lightweight Charts 4.2.0, Apache 2.0, local file, works offline. The attribution logo stays on, and the notice is now in `README.md`.
- [V] Upstream typings: v4.2.0 has no panes, and v5.0.0 has `addPane`. v5 replaces `addCandlestickSeries` with a unified `addSeries`, so upgrading changes code. Latest is 5.2.1, Apache 2.0.
- **Not upgraded.** This sprint has no panel requirement to demonstrate. [R] Decide in Phase 2 with a prototype: v5 panes or one v4 chart per panel. Both remain open.
- The coordinator's note describing the licence as dual "Commercial + OSS" does not match the vendored file, which says Apache 2.0 only.

### Acceptance matrix

**Unit tests:** 31 of 31 pass [T], with a fake local upstream covering bad data and reconnects. All provider payloads in them are labelled fixtures.

**Launcher:** 8 of 8 pass [T] on the rebranded code, using the real `Start Terminal.bat`, a fresh copy, a newly built `.venv` and the real conflict on 8765. A logger stood in for the browser, so the exact URL was recorded.

**End-to-end:** final run on the rebranded code, using headless Edge with a throwaway profile, real providers and a `.venv` built by the launcher test.

| Criterion | Result | Evidence |
| --- | --- | --- |
| Fresh install, default port busy, announced fallback | **Pass** | Ready in 125 s, opened `:8766` |
| Second launch reuses the terminal | **Pass** | Same instance ID |
| Configured busy port does not fall back | **Pass** | Clear message, no browser |
| `--port`, `RESOLUTE_QUANT_PORT`, config file priority | **Pass** | Each honoured |
| Other service on the busy port unaffected | **Pass** | 401 before and after |
| Backend-only data, whole run | **Pass** | Hosts: local server, Google Fonts, and the storage check's spare local port. No WebSockets |
| Name, copyright notice and repository link | **Pass** | `© 2026 RESOLUTE QUANT \| SG \| subashgauchan16` |
| Logo intro opens on first visit, closes itself, reopens and loops; header animation loops | **Pass** | Looks and alignment were checked by eye at desktop and phone widths |
| Crypto: quote, chart, book, tape, stats, Fear & Greed | **Pass** | Live through the backend |
| Stocks: quote, chart, profile, news, stats | **Pass** | |
| All timeframes offered, both markets | **Pass** | |
| Alerts: set, clear, fire, dismiss | **Pass** | Firing used an injected crossing |
| `ADD`, `DEL`, invalid symbol, `HELP`, keys | **Pass** | |
| Persistence across a refresh | **Pass** | |
| Backend stop: offline state, page usable | **Pass** | Exact message shown |
| Backend restart: recovers without reload | **Pass** | |
| 10-minute uninterrupted idle | **Pass** | 608 s, `LIVE` in 606 of 606 samples, no reconnects, DOM constant at 642 nodes |
| No console errors during idle | **Pass** | Empty |
| No frame gap over 250 ms | **Fail** | 97 gaps, worst 1,783 ms, about 43 frames a second on average. See the run history |
| Chart updates at or below 5 per second | **Pass** | 2.65/s from 4,562 trades. About 6.8/s before the fix |
| Heap after GC grows at most 10% | **Fail** | 3.01 to 3.39 MB (+12.6%). See below |

**Run history.** Every run is kept in `evidence/`, including the failures.

| Run | Code | Browser | Checks | Worst frame gap | Gaps over 250 ms | Heap start to end |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Before fixes | Fresh | 45 of 46 | 733 ms | 11 | Not comparable; GC ran every 30 s |
| 2 | Fixes, before rebrand | **Reused** | 46 of 47 | 150 ms | 0 | 2.89 to 3.76 MB |
| 3 | Rebrand | **Reused** | 53 of 54 | 67 ms | 0 | 2.76 to 3.31 MB |
| 4 | Rebrand | Fresh | 51 of 55 | 486 ms | 12 | 2.76 to 3.30 MB |
| 5 (final) | Rebrand | Fresh | 53 of 55 | 1,783 ms | 97 | 3.01 to 3.39 MB |

- **Reused browser:** a mistake in my test harness left the previous Edge alive on Windows, so runs 2 and 3 attached to the old page and profile. Their "fresh profile" description would have been false, and they had the first-visit intro hidden from proper testing. The harness now kills browsers by profile path and refuses to start if the debug port is busy.
- **Run 4's two intro failures were test timing:** a fixed wait, on a profile with no cached fonts. The test now polls.
- **Frame gaps:** runs 3 to 5 used the same app code, so the app alone does not explain the range. Run 5's gaps followed the appearance of an unrelated `.venv`, with CPU at 44% before the run began. This fits outside load, but **it is a correlation and not a demonstrated cause**. Headless timing may also differ from a visible window. [R] Test on a quiet machine with a CPU trace before relying on this.

**Heap trend** (`evidence/heap_trend.json`, on pre-rebrand code, GC before each reading):

| Minute | 3 | 8 | 13 | 18 | 23 | 28 | 33 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Heap, MB | 2.82 | 3.59 | 3.61 | 3.12 | 3.14 | 3.15 | 3.21 |

Readings from minute 18 still rise slowly, about 0.006 MB a minute, which is too short a period to tell apart from a small leak or noise. The check's threshold was not changed.

**Console errors outside idle, all explained:**

- Deliberate invalid symbol: 404 and 502.
- Deliberate storage check against a port with no page: 404.
- Deliberate server stop: connection reset and refused.

**Blocked or not run**

| Item | Status | Why |
| --- | --- | --- |
| Real Coinbase outage or real provider 429 | **Not run** | Covered by a fake upstream and fixtures only |
| A real visible double-click, using the default browser | **Not run** | Logger used. The owner should do this once |
| Default port 8765 when free | **Blocked** | Port is held on this PC; the logic is covered by unit tests |
| Phone-width layout | **Partly checked** | No sideways scroll and the intro fits at 375 px. Not automated, and the last small header change was not rechecked |
| Opening `terminal.html` directly | **Not run** | Guard message never exercised |
| Full-screen control | **Not run** | |
| Soak beyond 33 minutes | **Not run** | |
| The intro on real GPUs and with reduced-motion set | **Not run** | Reduced-motion styles exist but were never exercised |

**Regression, with cause:** crypto now stops if the data program stops, because the browser no longer connects to Coinbase itself. Phase 1 kept crypto running in that state. This follows directly from the backend-only rule and is shown as `CRYPTO OFF`. Prices freeze and dim; they are not hidden or simulated.

**Multi-tab behaviour changed slightly:** the book is now a single backend subscription, so a second tab on another symbol takes it over.

## 4. Historical data inventory and quality

**Finding: there is no historical dataset to audit, and no audit has passed.**

| Asset examined | What it is | Usable as research data? |
| --- | --- | --- |
| Project folder | No data files | No |
| `trading_intelligence/` | Harness with a synthetic generator whose docstring says it is built to give positive expected value. A later commit added a crypto downloader script, which was not reviewed. A check on 6 November found no data files there | **No.** Must never support market conclusions |
| `server.py` `/api/candles` | Live view, newest 320 bars, not stored | No, not reproducible |
| Unrelated forecasting script | Reads `daily_long_clean.csv`, which was not found | No |

### Provider probe (`research/data_probe.py`)

This measures what the providers return; it is **a sample and not a dataset**. Raw data was not stored. All timestamps were converted to UTC.

**Coinbase Exchange, BTC-USD and ETH-USD spot**

| Attribute | Finding |
| --- | --- |
| Acquisition | Public REST, 350 rows per request, newest first |
| Fields | time, low, high, open, close, volume |
| Native sizes | 60, 300, 900, 3600, 21600, 86400 seconds |
| Sample quality | At every size and on both symbols: no duplicates, no missing steps, no invalid OHLC, no zero-volume rows |
| Depth | BTC-USD 1m windows returned full rows 2, 5 and 8 years ago. The earliest date was not established |
| Freshness | Newest 1m bar about 4.7 minutes old at probe time |
| Trades | About 100 recent trades, price, side, size, time and ID. Deep history was not tested |
| Order book, funding | No historical book found through this route. Spot has no funding |
| Licence | **Not verified** [Q] |

These clean results cover only about 350 bars per size. They say nothing about the history as a whole.

**Yahoo Finance via yfinance**

| Instrument | Finding |
| --- | --- |
| AAPL, ^GSPC, HSBA.L | Session gaps are expected. AAPL `1D` has 1 invalid OHLC row and 1 NaN row. HSBA.L `1D` has 1 invalid row. ^GSPC has hundreds of zero-volume `1H` rows |
| GBPUSD=X | **Volume is zero in every row at every interval. `1D` has 27 invalid OHLC rows** |
| BTC-USD via Yahoo | **77% of 1m rows have zero volume**. `1H` is missing 189 steps, with one gap of about 4.2 days |
| Depth | `1m` 5 days, `15m` 1 month, `1H` 1 year (for the requested periods), `1MO` back to 1985 for AAPL |
| Licence | yfinance is for personal use, per the project's README |

[R] **Do not use Yahoo for research data in this stage.** Its crypto is clearly unsuitable. Its equities might serve as a low-frequency comparison later.

### Smallest useful acquisition plan (needs approval)

- **What:** Coinbase BTC-USD and ETH-USD at `1H` and `1D`.
- **Size:** about 125 requests per symbol per 5 years at `1H`. Under 2 MB per symbol, estimated at about 40 bytes per row.
- **Not:** the `1m`, 2-year-plus download in the coordinator's plan, which needs thousands of requests and is not needed for the recommended target.
- **Storage:** dated, immutable files outside version control, with a SHA-256 manifest. **The repository is public, and the licence is unverified, so raw data must not be committed.**
- **First step:** find each symbol's real start date, then run the audit in section 8.

## 5. Proposed first prediction target

### Scale evidence (`research/cost_hurdle_probe.py`)

A small, recent, overlapping sample. It shows scale only, and says nothing about whether a signal exists.

| Horizon | BTC median move | ETH median move | Compared with ~120 bps taker round trip |
| --- | --- | --- | --- |
| 15 minutes | 10.4 bps | 14.0 bps | About 12 times the move |
| 1 hour | 16.1 bps | 21.9 bps | About 5 to 7 times |
| 24 hours | 87.3 bps | 118.6 bps | About 1 to 1.4 times |
| 5 days | 255.5 bps | 290.7 bps | About 0.4 to 0.5 times |
| 20 days | 553.6 bps | 815.4 bps | About 0.15 to 0.2 times |

Median quoted spread over 60 seconds was about 0.001 bps for BTC and 0.47 bps for ETH (p90 0.22 and 0.85). **Fees dominate spread at this tier.**

**Fees.** The Coinbase Exchange help page lists 40 bps maker and 60 bps taker at the $0 to $10K tier, and Coinbase's Advanced pages list 50 and 90 bps at entry. Fees fall with volume. **The owner's actual tier is unknown [Q]**. The `trading_intelligence` config uses a 0.05 bps fee, which is roughly 1,000 times too low.

Sources: [Coinbase Exchange fees](https://help.coinbase.com/en/exchange/trading-and-funding/exchange-fees), [Coinbase Advanced fee announcement](https://www.coinbase.com/blog/were-lowering-fees-for-many-active-traders-on-coinbase-advanced), [Advanced VIP](https://www.coinbase.com/advanced-vip).

### Candidates

Notation: `C(t)` is the close of the bar ending at `t`, in UTC. `r(t) = ln(C(t) / C(t-1))`.

**Timing for all candidates.** The bar's end alone is not a safe decision time, because REST candles were seen up to about 5 minutes late. Decision time is **bar end + 10 minutes** [A]. Candles may be revised after first publication, so a live system must use the value as seen at decision time, and that must be tested [Q].

| | T1: Direction | T2: Forward return | **T3: Volatility (recommended)** |
| --- | --- | --- | --- |
| Label | `1 if ln(C(t+h)/C(t)) > 0, else 0` | `ln(C(t+h)/C(t))` | `ln( sqrt( sum[i=1..24] r(t+i)^2 ) )` |
| Horizon | 5 days | 5 days | 24 hours |
| Sampling | Daily, 00:00 UTC | Daily, 00:00 UTC | Hourly |
| Data needed | `1D` | `1D` | `1H` |
| Label overlap | 4 days | 4 days | 23 hours |
| Purge + embargo | 5 + 5 days | 5 + 5 days | 24 + 24 hours |
| Independent samples, 5 years | About 365 | About 365 | About 1,825 |
| Metrics | Brier, log loss, calibration, AUC | MAE, sign accuracy, out-of-sample R² | QLIKE, MSE of log RV, Mincer-Zarnowitz |
| Benchmark | Base rate | Zero and historical mean | EWMA and previous-day RV |
| Weakness | Few samples, drift-driven base rate | Very noisy | Not a trading signal |

### Why T3 first [R]

1. **It suits the data available:** `1H` Coinbase candles are small to acquire, and there are more independent samples than for T1.
2. **Failure is informative:** volatility clustering is widely documented, so failing to beat EWMA would point to a pipeline problem. A failed direction model says very little.
3. **It avoids an early false positive:** it can't be mistaken for a profitable strategy.
4. **It is reused:** T1 and every strategy in section 7 need volatility for sizing.

**Instrument:** BTC-USD on Coinbase spot, with ETH-USD as the out-of-universe check.

**Assumptions to confirm [A]:** crypto first, research only. The owner's trading capital, tier and preferred markets are unknown, and any of them could change this recommendation.

## 6. Feature dictionary outline and leakage

All features use data up to bar `t`, and are available at decision time (bar end + 10 minutes). Missing input bars make the feature missing; nothing is forward-filled across a gap larger than one bar. Scaling is fitted on each training fold only.

| Feature | Formula | Source | Target | Live? | Main risk |
| --- | --- | --- | --- | --- | --- |
| `rv_24h` | `sqrt(sum r(t-i)^2, i=0..23)` | `1H` close | T3, T1 | Yes | None if trailing |
| `rv_7d`, `rv_30d` | Same, 168 and 720 bars | `1H` close | T3, T1 | Yes | Warm-up rows |
| `ewma_vol` | `s(t)^2 = λ s(t-1)^2 + (1-λ) r(t)^2` | `1H` close | T3 benchmark | Yes | λ must be fixed before testing |
| `range_pk` | Parkinson on trailing high/low | `1H` OHLC | T3 | Yes | Revised bars |
| `abs_ret_1h` | `|r(t)|` | `1H` close | T3 | Yes | None |
| `volume_ratio` | `V(t) / mean(V, trailing 168)` | `1H` volume | T3 | Yes | Venue share shifts |
| `hour_of_day`, `weekday` | UTC calendar | Timestamp | T3 | Yes | Overfitting |
| `mom_5d`, `mom_20d` | `ln(C(t)/C(t-k))` | `1D` close | T1 | Yes | Selection across horizons |
| `ma_gap_20d` | `C(t)/mean(C, trailing 20) - 1` | `1D` close | T1 | Yes | **Must be trailing** |
| `eth_btc_ret` | ETH return minus BTC return | `1D` close | T1 | Yes | Timestamp alignment |

**Leakage risks to test explicitly**

- **Centred windows.** [T] The `trading_intelligence` moving average uses `np.convolve(mode="same")`, which reproduced a 9-bar look-ahead with a 20-bar window.
- **Scaler or imputer fitted on all data.**
- **Overlapping labels** crossing a fold boundary, which is why purge and embargo apply.
- **Using the unfinished bar.** The newest REST row may still be in progress.
- **Revised candles:** history downloaded later may differ from what was seen live.
- **Survivorship:** choosing BTC and ETH today is itself hindsight, which limits how far results generalise. Delisted pairs are not represented.
- **Research reuse:** the sample in section 5 is recent data already seen. It should fall in the final holdout only if no further choices depend on it.

**Required test:** corrupt all data after `t`, and confirm that every feature at `t` is unchanged.

**Order flow and book features** are out of scope: no historical data exists. Collecting it live from the terminal's feed would take months to become usable.

## 7. Model and strategy comparison plan

### Model ladder

Each step must beat the one before it out of sample, or the work stops there.

| Step | For T3 | For T1 (later) | Condition to run |
| --- | --- | --- | --- |
| 1. Naive | Previous-day RV; historical mean | Base rate | Data audit passed |
| 2. Transparent rule | EWMA, λ fixed in advance | 20-day momentum sign | Step 1 |
| 3. Interpretable | HAR-style linear regression on log RV | Regularised logistic | Beats step 2 on validation |
| 4. Tree-based | Gradient boosting | Same | Step 3 leaves structured residuals, **and new dependency approved** |
| 5. Volatility model | GARCH only if HAR falls short | Not applicable | Dependency approved |
| 6. Ensemble | Only from models that each add value | Same | Independent validated value |

**Nothing above step 3 is proposed for the next sprint.**

### Existing `trading_intelligence` harness

**Do not reuse it as it stands [R].** It was not run end to end. These findings are for the version on disk at about 23:20 on 5 November. The other agent has committed to that folder since, and those changes were not re-reviewed:

| Defect | How established |
| --- | --- |
| Centred moving average, a look-ahead leak | **Reproduced** in isolation |
| NaN assigned to an integer target raises an error | **Reproduced** in isolation |
| `np.errstate(invalid="omit")` raises an error | **Reproduced** in isolation |
| Horizon divided by the string `"1m"` raises an error | **Reproduced** in isolation |
| Class index (0, 1, 2) used directly as position, so "down" is flat and "flat" is long | Read in code |
| Predictions are a placeholder, always class 1, so the model is never used | Read in code |
| `baselines` called but imported as `baselines_mod` | Read in code |
| Gaps fail the audit even for session markets; the timestamp check always passes | Read in code |
| Fee default about 1,000 times too low | Compared with published fees |

The specifications in the coordinator's documents are a reasonable checklist, once the target and costs are corrected.

### Strategy hypotheses

These are hypotheses to test later and are **not recommendations to trade**. Each needs T3 first for sizing. Exact parameters must be fixed before the test.

| Hypothesis | Entry and exit | Holding | Sizing and limits | Turnover risk |
| --- | --- | --- | --- | --- |
| **Trend** | Long when 20-day return is above 0, checked daily; flat otherwise. No shorts | Days to weeks | Volatility target, 100% maximum notional | Low to medium |
| **Mean reversion** | Long after a daily close 2 sigma below the 20-day mean; exit at the mean or after 5 days | 1 to 5 days | Half of trend size | **High; most exposed to cost** |
| **Breakout** | Long on a close above the 20-day high with rising volatility; exit on a close below the 10-day low | Days to weeks | Volatility target | Low to medium |
| **Order flow** | **Not testable** | | No historical data | |
| **Regime selection** | Only if it adds value over the best single rule, using trailing data only | | Same limits | Adds parameters |

**Shared rules:**

- Spot long or flat only, with no leverage.
- Stop conditions: daily loss or drawdown limit moves to flat, and stale data or a feed outage blocks new entries.
- Signals are computed at decision time and filled no earlier than the next bar's open.
- Results before costs are never reported alone.

## 8. Backtesting and risk-control specification

### Data audit, before any modelling

Must report for each file:

- Rows, first and last time, timezone, and SHA-256.
- Duplicates, gaps against expected steps, and the longest gap.
- Invalid OHLC, non-positive values, zero-volume rows, and outlier returns reviewed by hand.

Rules: nothing is silently repaired. Every exclusion is logged.

### Splits

Chronological only, and random splits are never used.

- Expanding-window walk-forward: at least 24 months of training, 3-month test folds, stepping 3 months.
- **Final holdout:** the newest 6 months, used once after the model is frozen.
- Purge and embargo as in section 5.
- Tuning uses inner folds inside the training window only.

### Cost model

| Component | Base | Stress | Status |
| --- | --- | --- | --- |
| Fee per side | 60 bps taker | 90 bps | Published. **Replace with owner's tier** |
| Maker alternative | 40 bps | Fill not assumed | Published; fill risk unmodelled |
| Half spread per side | 0.25 bps BTC, 0.5 bps ETH | 1 and 2 bps | Short sample; recalibrate |
| Slippage per side | 5 bps | 15 bps | **Assumption [A]**, not measured |
| Latency | Fill at next bar open | Next bar open + adverse 5 bps | Assumption |
| Funding, borrow | None for spot long-only | | Not applicable |

### Required reporting

For every candidate and its benchmark, on each fold, subperiod and pooled:

- **Returns:** gross and net, with dates and cost scenario.
- **Drawdown:** maximum depth, duration, time to recovery.
- **Risk-adjusted:** Sharpe and Sortino, stating frequency, annualisation (365 days for crypto), risk-free rate and sample size. Report confidence intervals.
- **Trades:** count, hit rate, average win and loss, expectancy, tail losses.
- **Turnover:** annual turnover, plus net returns at 0.5x, 1x and 2x costs.
- **Stability:** per-fold results, parameter neighbourhood, and number of variants tried, so that selection bias is visible.
- **Calibration:** Brier and reliability plot for probabilities; Mincer-Zarnowitz for volatility.

### Proposed thresholds, for owner sign-off **before** any evaluation

These are proposals only. They become binding only once approved, and must not be changed after seeing results.

| Gate | Proposed pass condition |
| --- | --- |
| Data | Zero unexplained duplicates or invalid rows; every gap over 1 bar is explained |
| T3 forecast | QLIKE at least 5% lower than EWMA, pooled; better in at least 70% of folds; Diebold-Mariano p below 0.05 with HAC errors; repeated on the holdout |
| T1 forecast | Brier better than base rate pooled and in at least 60% of folds |
| Strategy economics | Net Sharpe exceeds buy-and-hold by at least 0.3 in base costs; net return above benchmark at 2x costs |
| Strategy risk | Maximum drawdown no worse than buy-and-hold; at least 100 trades, or the sample is reported as inconclusive |
| Robustness | Still passes with parameters moved 25% either way, and in at least 2 of 3 subperiods |

A failure rejects the candidate. Passing permits **paper observation only**. Live execution stays out of scope, and needs separate authorisation, limits, monitoring and a tested kill switch.

## 9. File changes and test evidence

Compared with the pre-sprint state.

**Added:** `.gitignore`, `assets/img/` (logo, header mark, icon), `assets/css/brand.css`, `assets/js/ui/brand.js`, `crypto_feed.py`, `assets/js/data/crypto.js`, `tests/test_backend.py`, `tests/acceptance.mjs`, `tests/launcher_check.py`, `tests/heap_trend.mjs`, `research/data_probe.py`, `research/cost_hurdle_probe.py`, `docs/sprint-1.1/`.

**Modified:**

- `server.py`: additive routes, port policy, health, and exclusive bind. It also ends SSE clients that fell behind, instead of leaving them silently without data.
- `requirements.txt`, `Start Terminal.bat` (passes arguments through), `terminal.html`, `README.md`, `README.txt`.
- `assets/js/`: `main.js`; `chart/book.js`, `chart/ohlc.js`; `data/feargreed.js`, `data/http.js`, `data/markets.js`, `data/orderbook.js`; `store/state.js`; `ui/command.js`, `ui/selection.js`, `ui/stats.js`, `ui/status.js`.

**Renamed:** the product is now Resolute Quant. `SUBASH_PORT` became `RESOLUTE_QUANT_PORT`, and the health service name became `resolute-quant`. Browser storage keeps its internal `subash.` prefix so saved watchlists survive. `Start Terminal.bat` keeps its filename.

**Removed:** `assets/js/data/coinbase.js`, replaced by `crypto.js`.

**Existing routes keep their meaning.** New stream message types are additive, and `/api/candles` gains two intervals.

**Bugs fixed that predate this sprint:**

| Bug | Effect |
| --- | --- |
| Live bar opened at a stale REST close | Bars with an open above the high (seen: 75,789 open, 75,696 high) |
| Per-minute trade buffer pruned only for the open symbol | Unbounded growth for other symbols |
| A chart update on every trade | About 6.8 per second, above the 5 limit |
| A favicon request returned 400 | A console error on each load |

**Evidence files:** `evidence/acceptance.json` (final), `evidence/acceptance-run*.json` (earlier runs), `evidence/launcher.json`, `evidence/heap_trend.json`, `evidence/data_probe.json`, `evidence/cost_hurdle_probe.json`.

The launcher test ran on the rebranded code. After it, only test scripts and `.gitignore` changed. The unit tests were rerun last, and 31 of 31 passed.

## 10. Unresolved decisions, risks and next actions

**Decisions for the owner**

| # | Decision | Recommendation |
| --- | --- | --- |
| 1 | Review and merge pull request `stage-1.1`, and confirm the licence's canonical repository and attribution name | Review the two open failures first |
| 2 | Which copy is canonical: this folder or `subash_extract`? | This folder |
| 3 | Accept the announced port fallback, or pin a port? | Pin 8766 once the 8765 service is identified or left as is |
| 4 | Approve first target T3 on BTC-USD? | Yes |
| 5 | Approve the small Coinbase `1H` and `1D` download? | Yes, after checking the terms |
| 6 | What is your real fee tier, capital range and market focus? | Needed; these can change the plan |
| 7 | Sign the thresholds in section 8 | Before any results |
| 8 | Crypto `4H`, `1W`, `1MO` by aggregation? | Yes, labelled as aggregated, in Phase 2 |
| 9 | Ownership of `server.py` and research between the agents | Confirm one owner per area, with only one active at a time. Moving the other agent's work into this repository would need a review first |

**Risks**

- **Two agents on one project:** the other agent's documents already describe a different copy, and its activity may have affected the performance results here.
- **Provider dependence:** Coinbase and Yahoo can change without notice, and terms are unverified.
- **Public repository:** anything pushed is permanent in practice.
- **Small sample size:** even 5 years gives about 365 independent 5-day periods, too few to support complex direction models.
- **The most likely research outcome is that a tradable edge is not found.** That would be a valid result.

**Next actions if approved**

1. Owner reviews the pull request and does one real double-click launch.
2. Investigate the frame gaps on a quiet machine, with a CPU trace, before multi-chart work adds load.
3. Write the data downloader and audit, and download the small dataset.
4. Implement the T3 benchmarks and HAR model in a new, tested harness.
5. In parallel, resume terminal Phase 2 (multi-chart), including the chart library decision.

## Go or no-go for the next sprint

| Area | Recommendation | Conditions |
| --- | --- | --- |
| **Research: data audit and T3 benchmarks** | **GO** | Approve decisions 4 to 7, and check Coinbase's data terms |
| **Terminal: Phase 2 multi-chart work** | **GO, with a condition** | Investigate the frame gaps first, because more charts add load |
| **Tree models, ensembles or direction models** | **NO-GO for now** | Only after the ladder in section 7 justifies them |
| **Paper trading** | **NO-GO for now** | Only after a candidate passes pre-agreed gates |
| **Live trading or order execution** | **NO-GO** | Not in scope. Needs separate authorisation |
| **Calling the terminal production-ready** | **NO-GO** | Two acceptance checks remain open |

No profitability claim is made, and no evidence exists on which to make one.
