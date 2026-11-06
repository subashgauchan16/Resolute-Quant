/* The history panel: choose a timeframe and dates, read the bars newest first, and export them. */
import { $, esc, num } from "../chart/helper.js";
import { csvDifference, fetchHistory, fetchHistoryCsv, saveCsv } from "../data/history.js";
import { dpFor, granLabel, grans, isY, S } from "../store/state.js";

const DEFAULT_DAYS = 30;
const dateText = (ms) => new Date(ms).toISOString().slice(0, 10);

let view = null;
let request = null;
let token = 0;

const show = (id, text, kind = "") => {
  const el = $(id);
  el.textContent = text;
  el.dataset.kind = kind;
};

function bodyRows(bars, symbol) {
  return bars.map((b) => {
    const d = dpFor(symbol, b.close);
    return `<tr><th scope="row">${esc(new Date(b.time * 1000).toISOString().slice(0, 16).replace("T", " "))}</th><td>${num(b.open, d)}</td><td>${num(b.high, d)}</td><td>${num(b.low, d)}</td><td>${num(b.close, d)}</td><td>${num(b.volume, 2)}</td></tr>`;
  }).join("");
}

/* Plain statements of what this view does and does not contain. */
function coverage(v) {
  const notes = [];
  if (v.stale) notes.push("This is a saved copy, because the provider did not answer. It may be out of date.");
  if (v.truncated) notes.push(`Only the newest ${num(v.limit, 0)} bars are shown, so the start of your range is not covered. Choose a shorter range to see older bars.`);
  if (v.gaps.scope === "within_trading_days") {
    if (v.gaps.count) notes.push(`${num(v.gaps.count, 0)} gaps inside trading days, about ${num(v.gaps.missing_bars, 0)} bars with no data. They are not filled in. Closures between days are not counted, so a whole missing day would not show here.`);
  } else if (v.gaps.count) {
    notes.push(`${num(v.gaps.count, 0)} gaps, about ${num(v.gaps.missing_bars, 0)} bars with no data. They are not filled in. Exchanges publish no bar when nothing traded.`);
  }
  if (v.bars.some((b) => b.volume == null)) notes.push("Volume is blank where the provider gave none. It is not set to zero.");
  if (v.rejected) notes.push(`${num(v.rejected, 0)} invalid rows from the provider were left out, not repaired.`);
  if (v.unfinished_excluded) notes.push("The bar still in progress is left out, because its values can change.");
  return notes.join(" ");
}

function render(v) {
  view = v;
  $("hbody").innerHTML = bodyRows(v.bars, v.symbol);
  $("htable").hidden = !v.count;
  $("hexport").disabled = !v.count;
  $("hempty").hidden = v.count > 0;
  $("hempty").textContent = v.count ? "" : `The provider returned no finished bars for ${v.symbol} in this range. It may be closed then, or older than the provider keeps at this timeframe. Try a larger timeframe or a more recent range.`;
  show("hsummary", v.count ? `${num(v.count, 0)} bars, newest first, ${v.first_utc} to ${v.last_utc}. Source: ${v.source}. Downloaded ${v.downloaded_at_utc}.` : "");
  show("hnotes", coverage(v), v.stale || v.truncated ? "warn" : "");
}

function clearView() {
  view = null;
  $("hbody").innerHTML = "";
  $("htable").hidden = true;
  $("hempty").hidden = true;
  $("hexport").disabled = true;
  show("hsummary", "");
  show("hnotes", "");
  show("hresult", "");
}

function readableError(error) {
  if (error.status === 429) return `${error.message} Try again in a minute.`;
  if (error.status) return String(error.message);
  return "Data program offline. Start Start Terminal.bat.";
}

async function load() {
  const mine = ++token;
  clearView();
  request = {
    symbol: S.sel, interval: $("hinterval").value, source: isY(S.sel) ? "yahoo" : "coinbase",
    start: $("hstart").value, end: $("hend").value,
  };
  $("hload").disabled = true;
  show("hstatus", `Loading ${request.symbol} history.`);
  try {
    const answer = await fetchHistory(request);
    if (mine !== token) return;
    show("hstatus", "");
    render(answer);
  } catch (error) {
    if (mine === token) show("hstatus", `History did not load. ${readableError(error)}`, "bad");
  } finally {
    if (mine === token) $("hload").disabled = false;
  }
}

async function exportCsv() {
  if (!view) return;
  const shown = view.count;
  const name = `${view.symbol.replace(/[^A-Za-z0-9.-]/g, "")}_${view.interval}_${view.start}_to_${view.end}.csv`;
  show("hresult", "Exporting.");
  $("hexport").disabled = true;
  try {
    const outcome = await saveCsv(name, async () => {
      const text = await fetchHistoryCsv(request);
      const difference = csvDifference(text, view);
      if (difference) throw new Error(`the data changed since it was loaded (${difference}). Load the history again, then export`);
      return text;
    });
    if (outcome === "saved") show("hresult", `Saved ${name} with ${num(shown, 0)} rows.`, "good");
    else if (outcome === "cancelled") show("hresult", "Export cancelled. No file was saved.");
    else show("hresult", `Download of ${name} started with ${num(shown, 0)} rows. Check your downloads folder to confirm it arrived.`);
  } catch (error) {
    show("hresult", `Export failed: ${error.status ? readableError(error) : error.message}. No file was confirmed saved.`, "bad");
  } finally {
    $("hexport").disabled = !view;
  }
}

export function closeHistory() {
  $("history").hidden = true;
}

export function openHistory() {
  const box = $("hinterval");
  const previous = box.value;
  box.innerHTML = grans().map(([name]) => `<option value="${esc(name)}">${esc(granLabel(name))}</option>`).join("");
  box.value = grans().some(([name]) => name === previous) ? previous : "1D";
  $("hsymbol").textContent = S.sel;
  const now = Date.now();
  $("hend").max = $("hstart").max = dateText(now);
  $("hend").value = dateText(now);
  $("hstart").value = dateText(now - DEFAULT_DAYS * 86400000);
  $("history").hidden = false;
  load();
}

export function initHistory() {
  $("historybtn").onclick = openHistory;
  $("hclose").onclick = closeHistory;
  $("hload").onclick = load;
  $("hexport").onclick = exportCsv;
  $("hform").addEventListener("submit", (event) => {
    event.preventDefault();
    load();
  });
}
