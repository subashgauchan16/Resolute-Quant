/* Market-price history from the local data program. No DOM. */
import { api, apiText } from "./http.js";

const query = ({ symbol, interval, source, start, end }) => new URLSearchParams({ symbol, interval, source, start, end }).toString();

export const fetchHistory = (request) => api(`/api/history?${query(request)}`);
export const fetchHistoryCsv = (request) => apiText(`/api/history?${query(request)}&format=csv`);

const isoTime = (seconds) => new Date(seconds * 1000).toISOString().replace(".000Z", "Z");
const sameNumber = (field, value) => (value == null ? field === "" : field !== "" && Number(field) === value);

/* Compares every row of CSV text from this program with the rows on screen. Its fields never
   contain commas or line breaks, because symbols are validated. Answers null, or what differs. */
export function csvDifference(text, view) {
  const rows = text.trim().split(/\r?\n/).slice(1).filter(Boolean);
  if (rows.length !== view.bars.length) return `${rows.length} rows now, ${view.bars.length} shown`;
  for (const [i, bar] of view.bars.entries()) {
    const [time, , , open, high, low, close, volume] = rows[i].split(",");
    const same = time === isoTime(bar.time) && sameNumber(open, bar.open) && sameNumber(high, bar.high)
      && sameNumber(low, bar.low) && sameNumber(close, bar.close) && sameNumber(volume, bar.volume);
    if (!same) return `the row for ${isoTime(bar.time)} differs`;
  }
  return null;
}

/* Writes the file. Answers "saved", "cancelled" or "download-started", and throws if writing fails. */
export async function saveCsv(fileName, getText) {
  if (!window.showSaveFilePicker) {
    const link = document.createElement("a");
    link.href = URL.createObjectURL(new Blob([await getText()], { type: "text/csv;charset=utf-8" }));
    link.download = fileName;
    link.click();
    URL.revokeObjectURL(link.href);
    return "download-started";
  }
  let handle;
  try {
    handle = await window.showSaveFilePicker({ suggestedName: fileName, types: [{ description: "CSV file", accept: { "text/csv": [".csv"] } }] });
  } catch (error) {
    if (error.name === "AbortError") return "cancelled";
    throw error;
  }
  const text = await getText();
  const writable = await handle.createWritable();
  try {
    await writable.write(new Blob([text], { type: "text/csv;charset=utf-8" }));
    await writable.close();
  } catch (error) {
    await writable.abort?.().catch(() => {});
    throw error;
  }
  return "saved";
}
