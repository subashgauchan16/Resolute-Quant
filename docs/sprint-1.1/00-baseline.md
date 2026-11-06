# Stage 1.1 baseline (written before implementation)

Captured 2026-11-05, about 23:25 local time. Labels: **[V]** verified on this machine, **[A]** assumption, **[Q]** unresolved.

## Repository

- [V] Project root: a `Subash Terminal` folder inside `Downloads\SubashTerminal`.
- [V] Git root: the user's home folder. Branch `master`, no commits, no remote. The project is untracked (`?? ./`), so there is no commit hash and no history. No Git repository was initialised, as instructed.
- [V] Rollback copy of the pre-sprint tree: taken in the session scratchpad. `SubashTerminal.zip` remains the only copy of the original program.
- [V] Tests: none exist. `research/data_probe.py` is new in this sprint and was added before this document.

## Files changed by Phase 1 (hash comparison with `SubashTerminal.zip`)

- Unchanged: `README.txt`, `Start Terminal.bat`, `requirements.txt`.
- Modified: `server.py`, `terminal.html`.
- Added: `README.md`, `assets/css/style.css`, `assets/lib/lightweight-charts.standalone.production.js`, and 22 modules in `assets/js/`.

Phase 1 results came from one author in one session and had no independent review. This sprint retests them.

## Runtime

- [V] Python 3.13.14 on PATH; `py` launcher not found by the Git Bash shell. Node 24.18.1. Edge present; Chrome absent.
- [V] Launch: `Start Terminal.bat` makes `.venv`, installs `requirements.txt`, runs `server.py`, and opens the browser at the fixed `127.0.0.1:8765`.
- [V] `requirements.txt` declares only `yfinance>=1.8,<2`. `yfinance` 1.8.0 already requires `websockets` (17.4 installed), `pandas`, `numpy` and `requests`.

## Routes in `server.py`

`/`, `/terminal.html`, `/assets/*`, `/api/stream`, `/api/watch`, `/api/candles`, `/api/profile`, `/api/news`, `/api/resolve`. All are GET. `/api/health` does **not** exist; a request returns 400 because it falls through to symbol validation.

`/api/candles` intervals: `1M 5M 15M 1H 1D 1W`, where `1M` means one minute. Output is cut to the newest 320 bars.

## Market-data connections

| Connection | Where | Provider |
| --- | --- | --- |
| `wss://ws-feed.exchange.coinbase.com` | Browser, `data/coinbase.js:62` | Coinbase ticker, level2_batch, heartbeat |
| `/products/{p}/stats` | Browser, `data/coinbase.js:12` | Coinbase |
| `/products/{p}/candles` | Browser, `data/coinbase.js:15,32` | Coinbase |
| `api.alternative.me/fng` | Browser, `data/feargreed.js:5` | Fear & Greed |
| `yf.Ticker`, `yf.WebSocket` | `server.py` | Yahoo (already backend-only) |
| `fonts.googleapis.com` | `terminal.html:7-8` | Fonts, not market data |

## Known failures found during inspection

1. [V] **Port 8765 is unavailable on this PC.** It is held by HTTP.sys under PID 4. It answers `401` with `Server: Microsoft-HTTPAPI/2.0`. A bind attempt fails with WinError 10013. The current code then opens the browser at that service. The owning application is unidentified [Q].
2. [V] **`http.server` uses `SO_REUSEADDR`, which on Windows allows a second listener on a port already in use.** Reproduced on a test port. Two terminals could share a port silently, so "port in use" detection is unreliable for the terminal's own instances.
3. [V] When the SSE client's queue overflows, `Hub.publish` discards it, but its handler loop keeps running on status messages only, so it stops receiving data with no error.
4. [V] Coinbase REST candles: 350 rows per request, and the newest bar lagged by up to about 5 minutes at 1m in the probe.
5. [V] The Phase 1 10-minute idle test did not complete. The earlier console contained unexplained Coinbase WebSocket failures.

## Ownership conflict (reported before editing)

Files that I did not write appeared in `Downloads` during the session:

- `deliverables/*.md`: written by a coordinator named "Hari". The audit covers a **different copy**, `Downloads/subash_extract/Subash Terminal/`, which is the unmodified original (same hashes as the zip for `server.py` 16,297 B and `terminal.html` 208,672 B).
- `trading_intelligence/`: a research harness, 23:01 to 23:17.
- `Subash_Terminal_Trading_Intelligence_Roadmap_and_Hari_Work_Report.pdf`: a planning report that says it is not a repository audit.

Actions:

- None of these files were or will be edited.
- `subash_extract` and this folder have diverged. **[Q] The owner should name one canonical copy.** This sprint works on the folder named above.
- The ownership matrix assigns `server.py` to the owner and says the UI agent must not "alter backend semantics". This sprint brief explicitly directs provider connections into the backend, so changes to `server.py` are additive and existing routes keep their meaning.
- `websocket_conflict_resolution.md` §5 keeps the browser's direct Coinbase WebSocket as a fallback. That contradicts its own §3 and this brief, so no fallback is built.

Claims in those documents that do not match the code:

- `api_contract_checklist.md` and `baseline_inspection_report.md` list a Yahoo `6H` interval; no such interval exists (`6H` is a Coinbase-only granularity).
- `api_contract_checklist.md` §2.3 says `time` is both milliseconds and seconds. Quote `time` and candle `time` do differ, but it lists `open` as a Yahoo prior close, whereas the Yahoo field is `prev`.
- `chart_library_decision_note.md` calls the licence dual "Commercial + OSS"; the vendored file's header says Apache 2.0 only.
- `tradable_universe_definition.md` says `server.py` consumes the Coinbase feed; it does not.

## Historical data

- [V] No market dataset exists in the project, `trading_intelligence/`, or the searched parts of `Downloads`, `Documents` and `Desktop`.
- [V] An unrelated forecasting script in the home folder reads `daily_long_clean.csv`; no such file was found.
- [V] The only CSV files in the home folder are unrelated business files, judged by name. They were not opened.
- [V] `trading_intelligence` runs on a synthetic generator only.

## Assumptions for this sprint

- [A] A deterministic port fallback is acceptable if it is announced. It can be disabled by configuring a port explicitly.
- [A] One user with one terminal tab at a time, matching the existing single-watchlist design in `Hub`.
