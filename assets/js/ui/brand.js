/* The Resolute Quant logo intro. It opens once a session and loops until closed, and the brand reopens it. */
import { $ } from "../chart/helper.js";

const SEEN_KEY = "rq.introSeen";
const AUTO_CLOSE_MS = 3600;
const CLOSE_FADE_MS = 450;
const DUST_COUNT = 16;
const TILT_DEGREES = 4;

let autoClose;

function seenThisSession() {
  try {
    if (sessionStorage.getItem(SEEN_KEY)) return true;
    sessionStorage.setItem(SEEN_KEY, "1");
  } catch { /* storage blocked: show it, which is harmless */ }
  return false;
}

function scatterDust(intro) {
  for (let i = 0; i < DUST_COUNT; i++) {
    const speck = document.createElement("i");
    speck.className = "rq-dust";
    speck.style.left = `${Math.random() * 100}%`;
    speck.style.animationDuration = `${7 + Math.random() * 8}s`;
    speck.style.animationDelay = `${-Math.random() * 12}s`;
    intro.append(speck);
  }
}

function close() {
  const intro = $("intro");
  if (intro.hidden || intro.classList.contains("closing")) return;
  clearTimeout(autoClose);
  intro.classList.add("closing");
  setTimeout(() => {
    intro.hidden = true;
    intro.classList.remove("closing");
    intro.querySelectorAll(".rq-dust").forEach((speck) => speck.remove());
  }, CLOSE_FADE_MS);
}

function open(closesItself) {
  const intro = $("intro");
  clearTimeout(autoClose);
  scatterDust(intro);
  intro.hidden = false;
  if (closesItself) autoClose = setTimeout(close, AUTO_CLOSE_MS);
}

/* The logo leans gently towards the pointer. */
function tilt(event) {
  const stage = $("rqstage");
  stage.style.setProperty("--rq-tilt-y", `${(event.clientX / innerWidth - 0.5) * TILT_DEGREES}deg`);
  stage.style.setProperty("--rq-tilt-x", `${(0.5 - event.clientY / innerHeight) * TILT_DEGREES}deg`);
}

export function initBrand() {
  const intro = $("intro");
  $("brand").onclick = () => open(false);
  intro.onclick = close;
  intro.addEventListener("pointermove", tilt);
  /* Capture so the key that closes the intro does not also run a shortcut. */
  addEventListener("keydown", (event) => {
    if (intro.hidden) return;
    event.stopImmediatePropagation();
    event.preventDefault();
    close();
  }, true);
  if (!seenThisSession()) open(true);
}
