![Resolute Quant](assets/img/resolute-quant-logo.webp)

# Resolute Quant

© 2026 RESOLUTE QUANT | SG | subashgauchan16. See [LICENSE](LICENSE). This is proprietary, restricted-use software: public visibility grants no right to use it.

**Project site:** https://subashgauchan16.github.io/Resolute-Quant/ is a summary page. The terminal itself runs on your own computer, and the site shows no market data.

> **Prototype.** A working single-chart research terminal with backend-only market data. The multi-chart dashboard, indicators and resizable panels are not built yet, and no predictive model exists. It places no orders. The logo's word "trade" is branding; this program has no execution layer.

A local research and monitoring terminal for crypto, stocks, indices, FX and gold.
It places no orders and connects to no broker or exchange account. See `README.txt` for
the short user guide.

## Run it

Double-click `Start Terminal.bat`, then use the page it opens. Keep the black window open, and
close it to stop. The page must come from the data program: opening `terminal.html` directly
cannot load its modules.

## Choosing the port

The first of these that is set wins:

1. `Start Terminal.bat --port 8770`
2. The `RESOLUTE_QUANT_PORT` environment variable
3. `terminal.config.json` beside `server.py`, containing `{"port": 8770}`
4. The default, 8765

- **A configured port (1 to 3) never falls back.** If it is busy, the program says so and stops.
- **The default may fall back.** If 8765 is busy, the program tries 8766 to 8775 in order, and
  says which port it chose in the black window and the page footer.
- **Another program on the port is never touched.** The program reports it and suggests how to identify it.
- **A terminal already running is reused**, not duplicated. `/api/health` confirms its identity.
- **Saved settings belong to the exact address.** Watchlists and alerts on one port do not appear
  on another. Keep to one port to keep your settings. They are not migrated automatically.

## Layout

| Layer | Files | Rule |
| --- | --- | --- |
| Data program | `server.py`, `crypto_feed.py` | The only code that talks to providers. Validates data, and serves HTTP, Server-Sent Events and the page's files |
| Data layer | `assets/js/data/` | Calls the data program only. No DOM. Reports news through `store/events.js` |
| State | `assets/js/store/` | Session state, event bus, `localStorage` access, saved layout |
| Model layer | `assets/js/chart/indicators.js` | Pure indicator maths |
| Charts | `assets/js/chart/` | Chart creation, the price panel, order book and tape painting, formatting helpers |
| UI | `assets/js/ui/`, `assets/css/style.css`, `terminal.html` | Panels, command bar, alerts, badges |
| Bootstrap | `assets/js/main.js` | Connects the layers and runs the 200 ms repaint loop |

There is no execution layer. Google Fonts is the only other internet request the page makes.

Lightweight Charts 4.2.0 is vendored in `assets/lib/`. It is Apache 2.0, copyright TradingView, Inc.
(https://www.tradingview.com/). The chart's attribution logo is kept on.

## API

`GET` only, and local requests only. Quote and tick `time` is epoch milliseconds, and candle `time` is epoch seconds.

| Route | Answer |
| --- | --- |
| `/api/health` | `{service, status, timestamp, instance, started, port, preferredPort, portSource, fallback, clients, providers}` |
| `/api/stream` | Server-Sent Events: `s` Yahoo status, `q` Yahoo quote, `ct` crypto tick, `cb` top of book, `cs` crypto feed status |
| `/api/watch?symbols=` | Set the Yahoo watchlist. `{quotes, status}` |
| `/api/candles?symbol=&interval=` | Yahoo bars. `1M 5M 15M 1H 4H 1D 1W 1MO`, where `1M` is one minute and `1MO` one month. `{bars, stale}` |
| `/api/profile`, `/api/news`, `/api/resolve` | Yahoo company data |
| `/api/crypto/watch?symbols=` | Set the crypto tickers. `{watch, status}` |
| `/api/crypto/book?symbol=` | Choose the one book to follow; empty stops it. `{symbol}` |
| `/api/crypto/candles?symbol=&granularity=` | Coinbase bars at 60, 300, 900, 3600, 21600 or 86400 seconds. `{bars, stale}` |
| `/api/crypto/stats?symbol=` | `{last, open, high, low, volume, stale}` |
| `/api/sentiment/feargreed` | `{value, classification, time, stale}` |
| `/api/history?symbol=&interval=&source=&start=&end=` | Past bars, newest first, with coverage facts. `source` is `coinbase` or `yahoo`, and dates are UTC `YYYY-MM-DD`. Add `&format=csv` for a CSV file |

`stale: true` means the provider refused and this is the last good copy. Errors are `{error}` with
status 400, 403, 404, 429 or 502. Bad provider values are dropped and counted, never repaired.

## Tests

```bash
python -m unittest discover -s tests
```

```bash
node tests/acceptance.mjs --python .venv/Scripts/python.exe --port 8771 --idle-minutes 10 --out result.json
```

```bash
python tests/launcher_check.py <empty-folder> <output.json> 8801
```

The last two use live providers. They need Edge and an internet connection, and they exit with a failure code if any check fails. The port `8801` keeps the launcher test away from a terminal you already have running.

Dependencies are pinned in `requirements.txt` for repeatable installs, tested on Python 3.13. The packages the project uses directly are listed in `requirements.in`. To update, install from `requirements.in` into a clean environment, retest, then freeze the versions into `requirements.txt`.

## History and CSV export

Press **HISTORY**, or type `HISTORY`, to see past bars for the open symbol. Choose a timeframe and UTC dates, and read the bars newest first. **Export CSV** saves the same rows.

CSV columns: `time_utc, symbol, interval, open, high, low, close, volume, source, downloaded_at_utc`. Times are UTC. Yahoo prices are unadjusted, and a Yahoo daily bar's time is midnight in the exchange's own timezone, so it often shows a non-zero UTC hour.

- Up to 5,000 bars per request. If a range holds more, the newest are returned and the page says so.
- Missing bars are reported and never filled. Invalid rows and the bar still in progress are left out and counted.
- History is fetched from the provider when requested and cached briefly. Only the files you export are kept.
- Exports are for your own use. Do not share them or publish anything derived from them. Coinbase's terms also bar using its data to build or test models without written consent. See `docs/architecture/phase-0-audit.md`.

## Saved in the browser

`localStorage` keys start with `subash.`: `mlist` and `list` (watchlists), `sel`, `gran`,
`alerts` and `layout`.

## Known limits

- One watchlist and one followed book per data program, so two tabs on different symbols compete.
- Each watchlist group holds up to 40 symbols. Adding more is refused with a message.
- Coinbase's newest candle can lag by minutes, so a page load can show a short gap before the live bars.
  A live bar opens at the first trade seen, so its open may differ from the exchange's.
- Yahoo's history is cut to the newest 320 bars per request, and Yahoo data is for personal use and may be delayed.

Saved settings keep the internal `subash.` key prefix from the project's earlier name, so existing watchlists survive the rename.

Stage 1.1 evidence and the research specification are in `docs/sprint-1.1/`. The project site's source is in `site/`, and only that folder is published.
