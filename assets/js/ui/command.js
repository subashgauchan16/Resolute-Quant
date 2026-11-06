/* The command bar, watchlist editing and keyboard shortcuts. */
import { $, css, px } from "../chart/helper.js";
import { fetchStats, pushCryptoWatch, seed } from "../data/crypto.js";
import { pushWatch, resolveSymbol } from "../data/markets.js";
import { store } from "../store/localStorage.js";
import { FRIENDLY, grans, isY, S, validPair, validY } from "../store/state.js";
import { addAlert, clearAlerts } from "./alert.js";
import { select, setGran } from "./selection.js";
import { addRow, removeRow } from "./watchlist.js";

const PLACEHOLDER = "Type BTC, 5M, ADD PEPE, ALERT 77000 or HELP. Press / to focus.";

const norm = (t) => (t.includes("-") ? t : t + "-USD");

function toast(message, bad) {
  const input = $("cmd");
  input.value = "";
  input.placeholder = message;
  input.parentElement.style.borderColor = bad ? css("--down") : "";
  setTimeout(() => {
    input.placeholder = PLACEHOLDER;
    input.parentElement.style.borderColor = "";
  }, 3500);
}

async function addMarket(p, quiet) {
  if (!validY(p)) {
    if (!quiet) toast(`${p} is not a symbol.`, true);
    return false;
  }
  if (isY(p)) {
    select(p);
    return true;
  }
  try {
    await resolveSymbol(p);
  } catch (e) {
    if (!quiet) toast(e.status === 429 ? e.message : `Yahoo has no price for ${p}.`, true);
    return false;
  }
  S.mlist.push(p);
  store.set("mlist", S.mlist);
  addRow(p, true);
  await pushWatch();
  select(p);
  return true;
}

async function addPair(p) {
  if (!validPair(p)) return toast(`${p} is not a pair name. Try ADD PEPE or ADD BTC-GBP.`, true);
  if (S.list.includes(p)) return select(p);
  try {
    await fetchStats(p);
  } catch (e) {
    return toast(e.status === 404 ? `Coinbase does not list ${p}.` : `Could not check ${p}. Check your connection.`, true);
  }
  S.list.push(p);
  store.set("list", S.list);
  addRow(p, false);
  pushCryptoWatch();
  seed(p);
  return select(p);
}

async function addAuto(t) {
  const p = norm(t);
  if (validPair(p)) {
    try {
      await fetchStats(p);
      return addPair(p);
    } catch { /* not on Coinbase, so try Yahoo */ }
  }
  if (!await addMarket(t, true)) toast(`Neither Coinbase nor Yahoo lists ${t}.`, true);
  return undefined;
}

function removeSymbol(p) {
  if (!S.list.includes(p) && !isY(p)) return toast(`${p} is not on your watchlist.`, true);
  const wasOpen = S.sel === p;
  const y = isY(p);
  if (S.list.length + S.mlist.length === 1) return toast("Keep at least one symbol on the list.", true);
  if (wasOpen) select([...S.list, ...S.mlist].find((x) => x !== p));
  removeRow(p);
  if (y) {
    S.mlist = S.mlist.filter((x) => x !== p);
    store.set("mlist", S.mlist);
    pushWatch();
    return undefined;
  }
  S.list = S.list.filter((x) => x !== p);
  store.set("list", S.list);
  pushCryptoWatch();
  return undefined;
}

function openSymbol(t) {
  const named = Object.keys(FRIENDLY).find((k) => FRIENDLY[k].replace(/\W/g, "") === t.replace(/\W/g, ""));
  if (named && isY(named)) return select(named);
  if (isY(t)) return select(t);
  if (S.list.includes(norm(t))) return select(norm(t));
  return addAuto(t);
}

const showHelp = () => { $("help").hidden = false; };

function run(raw) {
  const words = raw.trim().toUpperCase().split(/\s+/);
  if (!words[0]) return undefined;
  const [a, b] = words;
  const gran = grans().find((x) => x[0] === a);
  if (gran) return setGran(gran[1]);
  if (a === "HELP") return showHelp();
  if (a === "STK" && b) return addMarket(b);
  if (a === "ADD" && b) return openSymbol(b);
  if ((a === "DEL" || a === "REMOVE") && b) return removeSymbol(isY(b) ? b : norm(b));
  if (a === "CLEAR") {
    clearAlerts(S.sel);
    return toast(`Alerts cleared on ${S.sel}.`);
  }
  if (a === "ALERT") {
    const price = +String(b).replace(/,/g, "");
    if (!(price > 0)) return toast("Give a price, for example ALERT 77000.", true);
    addAlert(S.sel, price);
    return toast(`Alert set on ${S.sel} at ${px(price)}.`);
  }
  return openSymbol(a);
}

function fullscreen() {
  const d = document;
  (d.fullscreenElement ? d.exitFullscreen() : d.documentElement.requestFullscreen())?.catch?.(() => {});
}

function onGlobalKey(e) {
  if (e.key === "Escape") $("help").hidden = true;
  if (e.target.tagName === "INPUT") return;
  if (e.key === "/") {
    e.preventDefault();
    $("cmd").focus();
  }
  if (e.key === "f" || e.key === "F") fullscreen();
  if (e.key === "ArrowDown" || e.key === "ArrowUp") {
    e.preventDefault();
    const all = [...S.mlist, ...S.list];
    const i = all.indexOf(S.sel);
    select(all[(i + (e.key === "ArrowDown" ? 1 : -1) + all.length) % all.length]);
  }
}

export function initCommands() {
  $("cmd").addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      const value = e.target.value;
      e.target.value = "";
      run(value);
    }
    if (e.key === "Escape") {
      e.target.value = "";
      e.target.blur();
    }
  });
  $("fs").onclick = fullscreen;
  $("helpbtn").onclick = showHelp;
  $("help").onclick = () => { $("help").hidden = true; };
  addEventListener("keydown", onGlobalKey);
}
