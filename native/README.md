# Resolute Quant native app

The Windows desktop version of Resolute Quant: C++20, Qt 6 Widgets, and an embedded private Python. It rebuilds the browser terminal natively, with the same layout, colours, fonts and logo. **It has no models or trading functions, it never places orders, and it opens no network ports.**

## What it does

- **The terminal screen:** the browser terminal's grid, with the animated logo, command box, status badges, watchlist with sparklines, chart, order book, time and sales, stats strip and footer. It connects to Coinbase and Yahoo when it opens.
  - **Live chart:** candles, volume, EMA 20, last-price line, alert lines and crosshair. Every trade counts towards the forming bar, and a new bar opens at its first trade. The window updates at most about 5 times a second.
  - **Commands:** the same as the browser. Type `HELP` to see them, plus `HEALTH` for the Python Health page. Press `/` to focus the command box.
  - **Saved on this PC:** watchlists, open symbol, timeframe and alerts.
  - **Market symbols:** show a profile, news and market statistics in place of the book and tape.
  - **Honest state:** badges show LIVE, RECONNECTING, NO DATA or CONNECTING, and prices dim when they aren't current. It reconnects by itself, then reloads the chart, because trades were missed.
- **History page:** press **HISTORY**, choose Coinbase or Yahoo Finance, a symbol, a timeframe and dates, then read the bars as a candlestick chart and a table, and export them as CSV.
  - Scroll over the chart to zoom, and drag to pan. The arrow keys, `+`, `-` and Home also work once the chart has focus.
  - Missing bars are reported and never filled. Unfinished bars are left out and counted. If any page of a request fails, nothing is shown, so a partial range can't pass as complete.
  - Export writes exactly the bars on screen, without downloading them again.
- **Python Health page:** type `HEALTH`. Python version and location, whether isolation is active, and the NumPy version.
- **A real calculation:** type numbers, and NumPy returns the mean and sample standard deviation.
- **Error display:** errors appear as a plain message, with the Python traceback behind **Show details**.
- **A long job:** Python stays busy for 5 seconds with progress and Cancel, while the window keeps updating.

## Build it

Paths must stay short, because Windows long paths are off by default. These commands run from the repository root in PowerShell.

### 1. Install the tools once

These change your system, and their licence terms are yours to accept.

```bash
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

```bash
winget install --id Kitware.CMake --exact
```

```bash
winget install --id Ninja-build.Ninja --exact
```

Then install Qt to `C:\Qt` with the [Qt Online Installer](https://www.qt.io/download-qt-installer-oss), which needs a free Qt account. You need **Qt → Qt 6.12.0 → MSVC 2022 64-bit**, not the entries under AI tools. If Qt is already installed, this adds only that kit:

```bash
C:\Qt\MaintenanceTool.exe --root C:\Qt --accept-licenses --default-answer --confirm-command --no-default-installations install qt.qt6.6120.win64_msvc2022_64
```

That command accepts Qt's licence terms for the component.

### 2. Create the private Python once

This downloads Python 3.13.17 and the terminal's pinned packages, checks their checksums, and puts them in `native\runtime\`. Nothing is installed system-wide. Run it again after `native\python\requirements.lock` changes, and it installs only what changed.

```bash
powershell -ExecutionPolicy Bypass -File native\tools\setup_runtime.ps1
```

### 3. Build and test

```bash
powershell -ExecutionPolicy Bypass -File native\tools\build.ps1 -Clean -Test
```

The first build downloads pybind11, again with a checksum. The tests briefly open a real window. They use synthetic bars and a fake provider, so they never contact Coinbase or Yahoo.

To check the real providers, run this separate program. It loads real history from both, prints only counts and timings, and keeps no market data.

```bash
powershell -Command "$env:Path='C:\Qt\6.12.0\msvc2022_64\bin;'+$env:Path; native\build\release\live_history_check.exe"
```

To check the real Coinbase feed, this streams for 30 seconds and reports counts, timings and memory. Pass a different number of seconds to run longer.

```bash
powershell -Command "$env:Path='C:\Qt\6.12.0\msvc2022_64\bin;'+$env:Path; native\build\release\live_feed_check.exe 30"
```

### 4. Run it

This makes a self-contained folder at `native\dist\ResoluteQuant\`. Close the app first if it's open from that folder, because the folder is replaced.

```bash
powershell -ExecutionPolicy Bypass -File native\tools\deploy.ps1
```

```bash
native\dist\ResoluteQuant\ResoluteQuant.exe
```

Add `--offline` to open it without contacting any provider.

### How close it is to the browser terminal

The colours, sizes, fonts, layout, wording, number formats and logo come straight from the browser terminal's own files. It isn't pixel-identical, because Qt draws text and shapes differently from a browser, the chart is hand-built rather than Lightweight Charts, and the logo's shine uses a different blend that gives a similar look. It also doesn't rearrange itself for narrow windows, as the page does.

### Starting again

`native\build`, `native\runtime` and `native\dist` are all ignored by Git and safe to delete. Delete them, and run steps 2 to 4 again.

## How it fits together

```text
Window (UI thread)  --submit, cancel-->  PythonEngine (one background thread)
                    <--signals: progress, result, error--
                                               |
                                 rq_python_host.dll  (the only code that links Python)
                                               |
                         Embedded Python + python/rq_engine + NumPy
```

- **`src/app/`** is the window. It never touches Python.
- **`src/bridge/PythonEngine`** runs one job at a time on one thread, and only passes plain C++ results back.
- **`src/bridge/PythonHost`** is the only file that includes Python. The app loads this DLL at runtime, after checking the private Python exists, so a missing runtime gives a message in the window rather than a Windows loader error.
- **`python/rq_engine/`** holds the jobs. To add one, write a function taking `(args, host)` that returns a JSON-friendly dictionary, and register it in `runner.py`.
- **The live feed** runs on its own Python threads and reaches the window through `PythonEngine::pythonEvent`, so jobs such as a history load can still run while it streams. `live.py` subclasses `CoinbaseFeed` only to add a way to stop.
- **`history.py` and `crypto_feed.py`** at the repository root are shared with the browser terminal, and the build copies them in. Fetching from Yahoo is in `server.py`, which the app doesn't load, so `market_history.py` and `markets.py` have short copies. Tests check that both give the same answer.
- **`assets/`** holds the terminal's fonts, under the SIL Open Font License in `assets/fonts/OFL.txt`, and PNG copies of its logos, because this Qt has no WebP plugin.

## Known limits

- **A native crash closes the app.** Python exceptions are caught and shown, but a crash inside a native library, such as NumPy, can't be caught when Python runs in the same process. Risky future work should run in a child process launched from Python, behind `PythonEngine`, without changing any screens.
- **Cancel is cooperative.** A job must call `host.raise_if_cancelled()`. A job stuck in a native call can't be interrupted, so on exit the app waits 3 seconds, logs it, and ends the process with exit code 4.
- **One job at a time.** Other jobs wait in the queue.
- **A provider request can't be interrupted.** Cancel takes effect once the current request ends, or between requests. Closing the app during one can end the process after 3 seconds, as above.
- **The forming bar's volume is partial.** It counts only trades seen since the chart loaded, on top of what Coinbase had reported by then.
- **Yahoo prices refresh every 90 seconds.** The browser terminal also uses Yahoo's tick stream, which isn't ported yet. The badge shows YAHOO 90S, as the page does when that stream is down.
- **Alerts only fire on data the app receives,** so a market symbol's alert is only checked every 90 seconds.
- **The chart is deliberately simple.** It has no indicators or drawing tools. Bars sit side by side, so a gap shows as a jump in the date axis.
- **Yahoo gaps are counted only within trading days.** A whole missing day isn't detected, because trading calendars aren't known here.
- **Python can't restart within a running app.** If it fails to start, close and reopen the app.
- **Not ready to give to other people.** The deployed folder relies on the Microsoft C++ runtime being installed, and has no installer, code signing or Qt licence notices. It has only been run on the PC it was built on.
- **Release builds only.** The private Python has no debug libraries.

## Logs

Warnings and errors go to `%LOCALAPPDATA%\ResoluteQuant\native.log`, which restarts after 1 MB. It holds job names and error text. Error text can quote what was typed into a calculation. Never add credentials to it.

## Alongside the rest of the project

| Component | Effect |
| --- | --- |
| Browser terminal, `Start Terminal.bat`, `server.py` | Unchanged, and `server.py` isn't used by this app. Both can run at once |
| `history.py`, `crypto_feed.py` | Shared, not changed. A change to either now affects both programs, and the tests of both should be run |
| GitHub Pages site | Unchanged |
| Render deployment draft | Unaffected. This is a desktop program and can't deploy to Render. A hosted version would stay browser-based, with the provider-terms questions already raised on that draft |
| Ports and credentials | The app opens no ports, and stores and needs no credentials |
| Network | Unless started with `--offline`, the app contacts Coinbase, Yahoo and alternative.me (Fear & Greed) from when it opens. While both run, the app and a browser terminal each hold their own Coinbase connection, and each makes its own Yahoo requests, which adds to the chance of Yahoo slowing them down |
| Market data | Provider data is for personal research. Exported CSV files and screenshots of real data stay out of this public repository, and the data must not be shared or used for model work where a provider's terms forbid it |

## Where later work plugs in

None of this is built. Each still has open questions about data sources and their terms.

| Future work | Where it goes |
| --- | --- |
| Settings and provenance storage | SQLite through Qt's SQL module, which is the proposed next task |
| Yahoo's tick stream | A background thread in `markets.py`, using `yf.WebSocket` as `server.py` does |
| Reports, disclosures, and the company and supplier graph | Background `rq_engine` jobs, with new screens beside the Python Health page |
| Predictive models | An `rq_engine` module, ideally in a child process. Blocked on permitted data and agreed thresholds |
| Enterprise and procurement integrations | A new service layer, with credentials kept in Windows Credential Manager |

## Licences

Exact versions and checksums are in [dependencies.lock.md](dependencies.lock.md) and [python/requirements.lock](python/requirements.lock). Each Python package has its own licence, which should be reviewed before distributing. Free Qt is used under LGPLv3 and linked dynamically. Building and running on your own PC doesn't trigger distribution duties, but giving the app to others does, including providing the LGPL text and notices and allowing relinking. Get legal advice before distributing it.
