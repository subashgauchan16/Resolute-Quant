/* Creates chart instances with the terminal's look. Phase 2 grows this into the multi-panel host. */
import { css } from "./helper.js";

export function createTerminalChart(container) {
  const mute = css("--mute");
  const amber = css("--amber");
  const line = "#16181c";
  const border = "#23262c";
  return window.LightweightCharts.createChart(container, {
    autoSize: true,
    layout: { background: { type: "solid", color: "#000000" }, textColor: mute, fontFamily: css("--mono"), fontSize: 11, attributionLogo: true },
    grid: { vertLines: { color: line }, horzLines: { color: line } },
    rightPriceScale: { borderColor: border, scaleMargins: { top: 0.06, bottom: 0.22 } },
    timeScale: {
      borderColor: border, timeVisible: true, secondsVisible: false, rightOffset: 6, barSpacing: 8,
      tickMarkFormatter: (t, kind) => {
        const d = new Date(t * 1000);
        if (kind <= 1) return d.toLocaleDateString("en-GB", { month: "short", year: "2-digit" });
        if (kind === 2) return d.toLocaleDateString("en-GB", { day: "2-digit", month: "short" });
        return d.toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" });
      },
    },
    crosshair: { mode: 0, vertLine: { color: amber, labelBackgroundColor: border }, horzLine: { color: amber, labelBackgroundColor: border } },
    localization: { timeFormatter: (t) => new Date(t * 1000).toLocaleString("en-GB", { day: "2-digit", month: "short", hour: "2-digit", minute: "2-digit" }) },
  });
}
