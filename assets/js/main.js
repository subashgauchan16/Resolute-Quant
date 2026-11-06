/* Bootstrap: wires the data layer to the UI and runs the repaint loop. */
import { paintBook, paintTape } from "./chart/book.js";
import { $, hms } from "./chart/helper.js";
import { chartTrade, flushChart, initPriceChart, legend, loadChart, refreshIfDue } from "./chart/ohlc.js";
import { checkFeed, seedAll } from "./data/crypto.js";
import { connectMarkets } from "./data/markets.js";
import { on } from "./store/events.js";
import { isY, S } from "./store/state.js";
import { checkAlerts, initAlerts } from "./ui/alert.js";
import { initBrand } from "./ui/brand.js";
import { initCommands } from "./ui/command.js";
import { loadDetails } from "./ui/details.js";
import { applyMode, paintHeader, select } from "./ui/selection.js";
import { paintCryptoStats, paintFearGreed, paintMarketStats, paintQuote } from "./ui/stats.js";
import { paintMeter, paintPortNotice, paintStatus } from "./ui/status.js";
import { highlight, initWatchlist, paintWatchlist } from "./ui/watchlist.js";

const RENDER_MS = 200;
const METER_MS = 1000;
const SEED_MS = 300000;
const FEAR_GREED_MS = 3600000;

function render() {
  const now = Date.now();
  flushChart();
  checkFeed(now);
  const stale = paintStatus(now);
  paintWatchlist(now);
  paintQuote();
  if (isY(S.sel)) {
    paintMarketStats();
    refreshIfDue(now);
    return;
  }
  paintCryptoStats(now);
  paintBook(stale);
  paintTape();
}

function boot() {
  on("trade", chartTrade);
  on("price", checkAlerts);
  on("feed-restored", loadChart);
  on("select", select);

  initBrand();
  initPriceChart();
  initWatchlist();
  initAlerts();
  initCommands();
  applyMode();
  paintHeader();
  highlight();
  legend();
  loadDetails();
  $("session").textContent = `Session started ${hms(S.started).slice(0, 5)}`;

  connectMarkets();
  loadChart();
  seedAll();
  paintFearGreed();
  paintPortNotice();

  setInterval(render, RENDER_MS);
  setInterval(paintMeter, METER_MS);
  setInterval(seedAll, SEED_MS);
  setInterval(paintFearGreed, FEAR_GREED_MS);
}

boot();
