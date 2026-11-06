/* Market-price history from the local data program. No DOM. */
import { api, apiText } from "./http.js";

const query = ({ symbol, interval, source, start, end }) => new URLSearchParams({ symbol, interval, source, start, end }).toString();

export const fetchHistory = (request) => api(`/api/history?${query(request)}`);
export const fetchHistoryCsv = (request) => apiText(`/api/history?${query(request)}&format=csv`);

/* Data rows in CSV text from this program: fields never contain line breaks, so each line is a row. */
export const csvRowCount = (text) => Math.max(0, text.trim().split(/\r?\n/).length - 1);

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
