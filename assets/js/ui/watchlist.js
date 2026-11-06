/* The watchlist column: rows, sparklines and live prices. */
import { $, cls, css, esc, num, pct, pctTxt, STATE } from "../chart/helper.js";
import { emit, on } from "../store/events.js";
import { base, dpFor, FRIENDLY, isY, Q, quoteCcy, S } from "../store/state.js";

const rows = {};
let colourUp;
let colourDown;

export function addRow(p, y) {
  const b = document.createElement("button");
  b.className = "row";
  b.dataset.p = p;
  b.innerHTML = `<span class="sym"></span><svg viewBox="0 0 52 24" preserveAspectRatio="none" aria-hidden="true"><path fill="none" stroke-width="1.2"/></svg><span class="px">-</span><span class="chg flat">-</span>`;
  b.querySelector(".sym").innerHTML = `${esc(y ? (FRIENDLY[p] || p) : base(p))}<i>${y ? "-" : quoteCcy(p)}</i>`;
  b.onclick = () => emit("select", p);
  $(y ? "listM" : "listC").append(b);
  rows[p] = { el: b, px: b.querySelector(".px"), chg: b.querySelector(".chg"), path: b.querySelector("path"), tag: b.querySelector(".sym i") };
}

export function removeRow(p) {
  rows[p].el.remove();
  delete rows[p];
}

export function highlight() {
  document.querySelectorAll(".row").forEach((r) => {
    const selected = r.dataset.p === S.sel;
    r.classList.toggle("on", selected);
    if (selected) r.scrollIntoView({ block: "nearest" });
  });
}

function spark(p, q) {
  const pts = q.spark.concat(q.last ?? []);
  if (pts.length < 2) return;
  const lo = Math.min(...pts);
  const r = Math.max(...pts) - lo || 1;
  const n = pts.length - 1;
  rows[p].path.setAttribute("d", pts.map((v, i) => `${i ? "L" : "M"}${(i / n * 52).toFixed(1)} ${(22 - (v - lo) / r * 20).toFixed(1)}`).join(""));
  rows[p].path.setAttribute("stroke", pts.at(-1) >= pts[0] ? colourUp : colourDown);
}

export function paintWatchlist(now) {
  for (const p of [...S.mlist, ...S.list]) {
    const q = Q(p);
    const r = rows[p];
    if (!r || !q.dirty) continue;
    q.dirty = false;
    r.px.textContent = q.last == null ? "-" : num(q.last, dpFor(p, q.last));
    if (isY(p)) r.tag.textContent = (STATE[q.state] || "-").replace("AFTER HOURS", "AFTER").replace("PRE-MARKET", "PRE") + (q.delay > 0 ? ` ${q.delay}M` : "");
    const c = pct(q);
    r.chg.textContent = pctTxt(c);
    r.chg.className = "chg " + cls(c);
    if (q.flash) {
      r.px.classList.remove("flash-up", "flash-down");
      void r.px.offsetWidth;
      r.px.classList.add(q.flash > 0 ? "flash-up" : "flash-down");
      q.flash = 0;
    }
    if (q.sparkDirty || !q.sparked || now - q.sparked > 5000) {
      spark(p, q);
      q.sparked = now;
      q.sparkDirty = false;
    }
  }
}

export function initWatchlist() {
  colourUp = css("--up");
  colourDown = css("--down");
  S.mlist.forEach((p) => addRow(p, true));
  S.list.forEach((p) => addRow(p, false));
  on("markets-link", (up) => { $("ynote").hidden = up; });
}
