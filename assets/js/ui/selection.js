/* Switching the open symbol and candle size, and which panels that symbol's type shows. */
import { C, loadChart } from "../chart/ohlc.js";
import { $ } from "../chart/helper.js";
import { followBook } from "../data/crypto.js";
import { store } from "../store/localStorage.js";
import { granLabel, grans, isY, label, Q, S } from "../store/state.js";
import { loadDetails } from "./details.js";
import { refreshResearch } from "./research.js";
import { highlight } from "./watchlist.js";

export function paintGrans() {
  const box = $("intervals");
  box.textContent = "";
  grans().forEach(([name, gran]) => {
    const b = document.createElement("button");
    b.textContent = granLabel(name);
    b.onclick = () => setGran(gran);
    b.classList.toggle("on", gran === S.gran);
    box.append(b);
  });
}

export function setGran(gran) {
  S.gran = gran;
  store.set("gran", gran);
  paintGrans();
  loadChart();
}

export function applyMode() {
  const y = isY(S.sel);
  $("paneBook").hidden = $("paneTape").hidden = $("statsC").hidden = y;
  $("paneCo").hidden = $("paneNews").hidden = $("statsY").hidden = !y;
  $("app").classList.toggle("sel-y", y);
  $("app").classList.toggle("sel-c", !y);
  if (!grans().some((g) => g[1] === S.gran)) S.gran = 300;
  paintGrans();
}

export function paintHeader() {
  $("pair").textContent = isY(S.sel) ? label(S.sel) : S.sel;
}

export function select(p) {
  if (p === S.sel && !C.loading) return;
  S.sel = p;
  store.set("sel", p);
  followBook(p);
  S.tapeDirty = true;
  Q(p).dirty = true;
  S.prof = null;
  applyMode();
  paintHeader();
  highlight();
  loadChart();
  refreshResearch(true);
  loadDetails();
}
