/* Separates warm-up from a leak: garbage-collected heap readings at intervals, after a warm-up wait.
 *
 *   node tests/heap_trend.mjs --python <python.exe> --port 8781 --warmup 3 --samples 7 --every 5 --out heap.json
 */
import { spawn, spawnSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const PORT = arg("port", "8781");
const WARMUP = Number(arg("warmup", 3));
const SAMPLES = Number(arg("samples", 7));
const EVERY = Number(arg("every", 5));
const OUT = arg("out", "heap.json");
const DEBUG_PORT = arg("debug-port", "9334");
const EDGE = arg("browser", "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe");
const PROJECT = join(dirname(fileURLToPath(import.meta.url)), "..");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const server = spawn(arg("python", "python"), ["server.py", "--port", PORT, "--no-browser"], {
  cwd: PROJECT, env: { ...process.env, PYTHONDONTWRITEBYTECODE: "1" }, stdio: "ignore",
});
const profile = mkdtempSync(join(tmpdir(), "subash-heap-"));
const browser = spawn(EDGE, ["--headless=new", `--remote-debugging-port=${DEBUG_PORT}`, `--user-data-dir=${profile}`, "--no-first-run", "--disable-extensions", "about:blank"], { stdio: "ignore" });

/* Edge's launcher exits early, so only its unique profile path identifies every process it started. */
function killBrowser(profile) {
  const pattern = profile.replace(/'/g, "''");
  spawnSync("powershell", ["-NoProfile", "-Command", `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -like '*${pattern}*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }`]);
}

let id = 0;
const pending = new Map();
let ws;
const send = (method, params = {}) => new Promise((resolve) => {
  const key = ++id;
  pending.set(key, resolve);
  ws.send(JSON.stringify({ id: key, method, params }));
});

async function main() {
  await sleep(4000);
  const target = (await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json()).find((x) => x.type === "page");
  ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve) => { ws.onopen = resolve; });
  ws.onmessage = (event) => {
    const m = JSON.parse(event.data);
    if (m.id && pending.has(m.id)) {
      pending.get(m.id)(m.result);
      pending.delete(m.id);
    }
  };
  await send("Runtime.enable");
  await send("HeapProfiler.enable");
  await send("Page.navigate", { url: `http://127.0.0.1:${PORT}/` });
  await sleep(WARMUP * 60000);
  const readings = [];
  for (let i = 0; i < SAMPLES; i++) {
    await send("HeapProfiler.collectGarbage");
    const usage = await send("Runtime.getHeapUsage");
    const status = await send("Runtime.evaluate", { expression: "document.getElementById('statustext').textContent", returnByValue: true });
    readings.push({ minute: WARMUP + i * EVERY, heapMB: +(usage.usedSize / 1048576).toFixed(3), status: status.result.value });
    writeFileSync(OUT, JSON.stringify({ warmupMinutes: WARMUP, readings }, null, 1));
    console.log(JSON.stringify(readings.at(-1)));
    if (i < SAMPLES - 1) await sleep(EVERY * 60000);
  }
}

main().catch((error) => console.error(error)).finally(async () => {
  killBrowser(profile);
  server.kill();
  await sleep(1500);
  try { rmSync(profile, { recursive: true, force: true }); } catch { /* the browser may still hold a file */ }
  process.exit(0);
});
