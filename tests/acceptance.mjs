/* End-to-end acceptance run: real server.py + real providers + headless Edge with a throwaway profile.
 *
 *   node tests/acceptance.mjs --python <python.exe> --port 8771 --idle-minutes 10 --out result.json
 *
 * Starts the server, audits every network request, drives features through the page's real
 * handlers, stops and restarts the server, then runs an uninterrupted idle measurement.
 * Results are recorded as pass or fail; nothing here retries a failed check.
 */
import { spawn, spawnSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const PYTHON = arg("python", "python");
const PORT = Number(arg("port", 8771));
const IDLE_MINUTES = Number(arg("idle-minutes", 0));
const OUT = arg("out", "acceptance.json");
const EDGE = arg("browser", "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe");
const DEBUG_PORT = Number(arg("debug-port", 9333));
const PROJECT = join(dirname(fileURLToPath(import.meta.url)), "..");
const BASE = `http://127.0.0.1:${PORT}`;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const result = { started: new Date().toISOString(), base: BASE, checks: [], requests: {}, webSockets: [], console: [], exceptions: [], serverOutput: [], idle: null };
const check = (name, pass, detail) => {
  result.checks.push({ name, pass: Boolean(pass), detail });
  console.log(`${pass ? "PASS" : "FAIL"}  ${name}${detail === undefined ? "" : "  " + JSON.stringify(detail)}`.slice(0, 260));
};
const save = () => writeFileSync(OUT, JSON.stringify(result, null, 1));

/* Edge's launcher exits early, so only its unique profile path identifies every process it started. */
function killBrowser(profile) {
  const pattern = profile.replace(/'/g, "''");
  spawnSync("powershell", ["-NoProfile", "-Command", `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -like '*${pattern}*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }`]);
}

let serverProcess;
function startServer() {
  serverProcess = spawn(PYTHON, ["server.py", "--port", String(PORT), "--no-browser"], {
    cwd: PROJECT, env: { ...process.env, PYTHONDONTWRITEBYTECODE: "1", PYTHONUNBUFFERED: "1", PYTHONUTF8: "1" }, stdio: ["ignore", "pipe", "pipe"],
  });
  for (const stream of [serverProcess.stdout, serverProcess.stderr]) {
    stream.on("data", (chunk) => {
      for (const line of chunk.toString().split(/\r?\n/)) if (line.trim()) result.serverOutput.push(`${new Date().toISOString().slice(11, 19)} ${line.slice(0, 200)}`);
    });
  }
}
async function waitForServer() {
  for (let i = 0; i < 60; i++) {
    try {
      const response = await fetch(`${BASE}/api/health`);
      if (response.ok) return await response.json();
    } catch { /* not up yet */ }
    await sleep(500);
  }
  throw new Error("server did not start");
}
function stopServer() {
  return new Promise((resolve) => {
    serverProcess.once("exit", resolve);
    serverProcess.kill();
  });
}

/* Minimal Chrome DevTools Protocol client using Node's built-in WebSocket. */
class Page {
  constructor(ws) {
    this.ws = ws;
    this.id = 0;
    this.pending = new Map();
    this.handlers = {};
    ws.onmessage = (event) => {
      const m = JSON.parse(event.data);
      if (m.id && this.pending.has(m.id)) {
        const { resolve, reject } = this.pending.get(m.id);
        this.pending.delete(m.id);
        if (m.error) reject(new Error(m.error.message));
        else resolve(m.result);
      } else if (m.method && this.handlers[m.method]) {
        this.handlers[m.method](m.params);
      }
    };
  }
  send(method, params = {}) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }
  async eval(expression) {
    const r = await this.send("Runtime.evaluate", { expression: `(async()=>{${expression}})()`, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result.value;
  }
}

const HELPERS = `
const el=id=>document.getElementById(id),t=id=>el(id)?.textContent,sleep=ms=>new Promise(r=>setTimeout(r,ms));
const cmd=async(v,ms=2500)=>{const i=el('cmd');i.value=v;i.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}));await sleep(ms)};
const key=k=>document.body.dispatchEvent(new KeyboardEvent('keydown',{key:k,bubbles:true}));
const on=()=>document.querySelector('#intervals .on')?.textContent;
`;

/* The rebrand: name, notice, logo file, the intro's life cycle and the looping header animation. */
async function brand(page) {
  const r = await page.eval(`${HELPERS}
    const intro=el('intro'),closedByTimer=intro.hidden;
    const running=[...document.querySelectorAll('.rq-mark-shine,.rq-word')].every(x=>x.getAnimations().some(a=>a.playState==='running'));
    const logo=await fetch('assets/img/resolute-quant-logo.webp');
    const page=document.documentElement.outerHTML;
    el('brand').click();await sleep(600);
    const reopened=!intro.hidden,img=document.querySelector('.rq-logo');
    const shineRunning=document.querySelector('.rq-shine').getAnimations()[0]?.playState==='running';
    await sleep(4500);const staysOpen=!intro.hidden;
    const before=t('pair');key('ArrowDown');await sleep(800);const closedByKey=intro.classList.contains('closing')||intro.hidden;const keyEaten=t('pair')===before;
    await sleep(700);
    return {title:document.title,brand:el('brand').getAttribute('aria-label'),copy:document.querySelector('.rq-copy').textContent,href:document.querySelector('.rq-copy').href,noOldName:!/subash terminal/i.test(page),logoType:logo.headers.get('content-type'),logoLoaded:img.naturalWidth,closedByTimer,running,reopened,shineRunning,staysOpen,closedByKey,keyEaten,hiddenAfter:intro.hidden}`);
  check("name is Resolute Quant, with no old name on the page", r.title.includes("Resolute Quant") && r.brand.startsWith("Resolute Quant") && r.noOldName, { title: r.title });
  check("copyright notice and repository link", r.copy === "© 2026 RESOLUTE QUANT | SG | subashgauchan16" && r.href === "https://github.com/subashgauchan16/Resolute-Quant", r.copy);
  check("logo file served and displayed", r.logoType === "image/webp" && r.logoLoaded === 1672, { type: r.logoType, width: r.logoLoaded });
  check("header animation is running in a loop", r.running, r.running);
  check("brand reopens the intro, which loops until closed", r.reopened && r.shineRunning && r.staysOpen, { reopened: r.reopened, shine: r.shineRunning, staysOpen: r.staysOpen });
  check("any key closes the intro without also running a shortcut", r.closedByKey && r.keyEaten && r.hiddenAfter, { closed: r.closedByKey, keyEaten: r.keyEaten });
}

async function regression(page) {
  const crypto = await page.eval(`${HELPERS} await cmd('BTC',7000);return {status:t('statustext'),y:t('ystatustext'),big:t('bigpx'),rate:t('rate'),book:t('booksrc'),asks:el('asks').childElementCount,bids:el('bids').childElementCount,tape:el('tape').childElementCount,vwap:t('s-vwap'),flow:t('s-flow'),imb:t('s-imb'),fng:t('s-fng'),fngd:t('s-fngd'),range:t('s-range'),vol:t('s-vol'),rows:document.querySelectorAll('.row').length,grans:t('intervals'),legend:t('legend'),canvases:document.querySelectorAll('#chart canvas').length,msg:el('chartmsg').hidden}`);
  check("crypto badge LIVE through the backend", crypto.status === "LIVE", crypto.status);
  check("crypto quote and chart render", crypto.big.startsWith("$") && crypto.legend.includes("EMA 20") && crypto.canvases > 0 && crypto.msg, { big: crypto.big, legend: crypto.legend });
  check("order book renders ten levels a side", crypto.asks === 10 && crypto.bids === 10 && crypto.book.startsWith("LIVE, DEPTH"), crypto.book);
  check("trade tape fills", crypto.tape > 0, crypto.tape);
  check("session stats compute", crypto.vwap !== "-" && crypto.flow !== "-" && crypto.imb !== "-" && crypto.range !== "-" && crypto.vol !== "-", crypto);
  check("fear and greed through the backend", /^\d+ \/ 100$/.test(crypto.fng), { fng: crypto.fng, detail: crypto.fngd });
  check("watchlist has all rows", crypto.rows === 20, crypto.rows);
  check("crypto timeframes offered are the ones Coinbase serves", crypto.grans === "1m5m15m1H6H1D", crypto.grans);

  for (const name of ["1M", "15M", "1H", "6H", "1D", "5M"]) {
    const r = await page.eval(`${HELPERS} await cmd('${name}',2500);return {on:on(),legend:t('legend'),msg:el('chartmsg').hidden}`);
    check(`crypto timeframe ${name}`, r.on === name.replace(/^(\d+)M$/, "$1m") && r.msg && r.legend.includes("EMA"), r.on);
  }

  const liveBar = await page.eval(`${HELPERS} await cmd('1M',66000);const {C}=await import('/assets/js/chart/ohlc.js');const c=C.last;await cmd('5M',1500);return {...c,age:Math.round(Date.now()/1000-c.time)}`);
  check("live 1m bar exists and its OHLC is self-consistent", liveBar.age < 120 && liveBar.high >= Math.max(liveBar.open, liveBar.close) && liveBar.low <= Math.min(liveBar.open, liveBar.close), liveBar);

  const alert = await page.eval(`${HELPERS} await cmd('ALERT 90000',600);const set=localStorage.getItem('subash.alerts');const placeholder=el('cmd').placeholder;
    const ev=await import('/assets/js/store/events.js');await cmd('CLEAR',400);const cleared=localStorage.getItem('subash.alerts');
    await cmd('ALERT 500',300);ev.emit('price',t('pair'),499,501);const fired=!el('alertbar').hidden,text=t('alerttext');el('alertclose').click();await cmd('CLEAR',300);await sleep(3600);
    return {set,placeholder,cleared,fired,text,left:localStorage.getItem('subash.alerts'),dismissed:el('alertbar').hidden}`);
  check("alert set, clear, fire, dismiss (firing uses an injected crossing)", alert.set.includes("90000") && alert.cleared === "[]" && alert.fired && alert.left === "[]" && alert.dismissed, alert.text);

  const stock = await page.eval(`${HELPERS} await cmd('AAPL',7000);return {pair:t('pair'),big:t('bigpx'),asof:t('asof'),legend:t('legend'),bookHidden:el('paneBook').hidden,co:t('co').slice(0,60),news:el('news').childElementCount,state:t('y-state'),cap:t('y-cap'),grans:t('intervals'),msg:el('chartmsg').hidden}`);
  check("stock view: quote, chart, profile, news, stats", stock.pair === "AAPL" && stock.big !== "-" && stock.bookHidden && stock.co.includes("Apple") && stock.news > 0 && stock.cap !== "-" && stock.msg, stock);
  check("stock timeframes include 4H and 1MO", stock.grans === "1m5m15m1H4H1D1W1MO", stock.grans);
  for (const name of ["1MO", "4H", "1W", "1M", "1D"]) {
    const r = await page.eval(`${HELPERS} const before=t('legend');await cmd('${name}',4000);return {on:on(),legend:t('legend'),msg:el('chartmsg').hidden}`);
    check(`stock timeframe ${name}`, r.on === name.replace(/^(\d+)M$/, "$1m") && r.msg, { on: r.on, legend: r.legend });
  }
  const minuteVsMonth = await page.eval(`const a=await (await fetch('/api/candles?symbol=AAPL&interval=1M')).json(),b=await (await fetch('/api/candles?symbol=AAPL&interval=1MO')).json();const step=x=>x.bars.at(-2).time?x.bars.at(-1).time-x.bars.at(-2).time:null;return {minuteStep:step(a),monthStep:step(b),minuteRows:a.bars.length,monthRows:b.bars.length}`);
  check("1M is one minute and 1MO is one month", minuteVsMonth.minuteStep <= 86400 && minuteVsMonth.monthStep > 27 * 86400, minuteVsMonth);

  const edit = await page.eval(`${HELPERS} await cmd('ETH',3500);const eth={pair:t('pair'),book:t('booksrc'),asks:el('asks').childElementCount};
    await cmd('ADD PEPE',4500);const pepe={pair:t('pair'),row:!!document.querySelector('.row[data-p="PEPE-USD"]'),saved:localStorage.getItem('subash.list')?.includes('PEPE')};
    await cmd('DEL PEPE',1500);const gone={row:!!document.querySelector('.row[data-p="PEPE-USD"]'),saved:localStorage.getItem('subash.list')?.includes('PEPE')};
    await cmd('NOSUCHSYM1',4500);const bad=el('cmd').placeholder;await sleep(3600);
    await cmd('HELP',200);const help=!el('help').hidden;key('Escape');const closed=el('help').hidden;
    const before=t('pair');key('ArrowDown');await sleep(1500);const arrow=[before,t('pair')];key('/');const slash=document.activeElement.id;document.activeElement.blur();
    return {eth,pepe,gone,bad,help,closed,arrow,slash}`);
  check("switching crypto symbol moves the book", edit.eth.pair === "ETH-USD" && edit.eth.asks === 10, edit.eth);
  check("ADD and DEL through the backend check", edit.pepe.pair === "PEPE-USD" && edit.pepe.row && edit.pepe.saved && !edit.gone.row && !edit.gone.saved, { pepe: edit.pepe, gone: edit.gone });
  check("unknown symbol explained", edit.bad.includes("Neither Coinbase nor Yahoo"), edit.bad);
  check("HELP, Esc, arrow, slash", edit.help && edit.closed && edit.arrow[0] !== edit.arrow[1] && edit.slash === "cmd", edit.arrow);

  const persisted = await page.eval(`${HELPERS} await cmd('SOL',1500);await cmd('15M',1500);return {sel:localStorage.getItem('subash.sel'),gran:localStorage.getItem('subash.gran')}`);
  await page.send("Page.reload");
  await sleep(5000);
  const reloaded = await page.eval(`${HELPERS} return {pair:t('pair'),on:on(),status:t('statustext')}`);
  check("open symbol and timeframe survive a refresh", reloaded.pair === "SOL-USD" && reloaded.on === "15m", { persisted, reloaded });

  const layout = await page.eval(`const l=await import('/assets/js/store/layout.js');l.saveLayout({...l.defaultLayout(),hidden:['paneNews'],sizes:{watch:300}});const saved=l.loadLayout();localStorage.setItem('subash.layout','{"version":1,"hidden":"x"}');const bad=l.loadLayout();localStorage.removeItem('subash.layout');return {saved,bad}`);
  check("layout store saves and rejects damage (no panel controls use it yet)", layout.saved.sizes.watch === 300 && Array.isArray(layout.bad.hidden), layout.saved);
}

/* A provider failure must never show one symbol's candles under another's name, or old bars as current. */
async function dataHonesty(page) {
  const failed = await page.eval(`${HELPERS}
    const real=window.fetch,{C}=await import('/assets/js/chart/ohlc.js');
    await cmd('BTC',3000);const before=!!C.last;
    window.fetch=(u,o)=>String(u).includes('/api/crypto/candles')&&String(u).includes('ETH-USD')?Promise.resolve(new Response('{"error":"fixture"}',{status:502})):real(u,o);
    await cmd('ETH',2000);
    const during={pair:t('pair'),before,hasBars:!!C.last,legend:t('legend'),msg:el('chartmsg').hidden?null:t('chartmsg')};
    window.fetch=real;await sleep(16500);
    return {...during,recovered:!!C.last&&C.key.startsWith('ETH-USD'),msgAfter:el('chartmsg').hidden}`);
  check("failed load clears the previous symbol's candles and names the symbol", failed.before && failed.pair === "ETH-USD" && !failed.hasBars && failed.legend === "" && failed.msg?.includes("ETH-USD"), failed.msg);
  check("chart recovers by itself after a failed load", failed.recovered && failed.msgAfter, failed.recovered);
  const stale = await page.eval(`${HELPERS}
    const real=window.fetch;
    window.fetch=async(u,o)=>{const r=await real(u,o);if(!String(u).includes('/api/crypto/candles')||!String(u).includes('SOL-USD'))return r;const j=await r.json();j.stale=true;return new Response(JSON.stringify(j),{status:200,headers:{'Content-Type':'application/json'}})};
    await cmd('SOL',3000);const shown=el('chartmsg').hidden?null:t('chartmsg'),legend=t('legend');window.fetch=real;
    await cmd('BTC',3000);return {shown,hasLegend:legend!=='',clearedOnNext:el('chartmsg').hidden}`);
  check("a saved copy of candles is labelled as possibly out of date", stale.shown?.startsWith("Saved copy of SOL-USD") && stale.hasLegend && stale.clearedOnNext, stale.shown);
  const limit = await page.eval(`${HELPERS}
    const {S}=await import('/assets/js/store/state.js'),saved=S.mlist;
    S.mlist=[...saved,...Array.from({length:40-saved.length},(_,i)=>'ZZ'+i)];
    await cmd('STK NEWSYM',800);const text=el('cmd').placeholder,added=S.mlist.includes('NEWSYM');S.mlist=saved;await sleep(3600);return {text,added}`);
  check("a full watchlist refuses a new symbol and says why", limit.text.includes("full at 40") && !limit.added, limit.text);
}

/* History reads real provider bars. Failure and save paths are forced inside the page. */
async function historyPanel(page) {
  const live = await page.eval(`${HELPERS}
    await cmd('BTC',1500);await cmd('HISTORY',200);
    const visible=!el('history').hidden,wait=async(f,ms=30000)=>{const s=Date.now();while(Date.now()-s<ms&&!f())await sleep(150)};
    await wait(()=>el('hsummary').textContent||el('hstatus').dataset.kind==='bad');
    const times=[...el('hbody').querySelectorAll('tr th')].map(x=>x.textContent),rows=times.length;
    const newestFirst=times.every((x,i)=>!i||times[i-1]>x);
    const req=new URLSearchParams({symbol:'BTC-USD',interval:el('hinterval').value,source:'coinbase',start:el('hstart').value,end:el('hend').value});
    const csv=await (await fetch('/api/history?'+req+'&format=csv')).text(),lines=csv.trim().split(String.fromCharCode(10));
    let written=null;
    window.showSaveFilePicker=async()=>({createWritable:async()=>({write:async b=>{written=await b.text()},close:async()=>{},abort:async()=>{}})});
    el('hexport').click();await wait(()=>el('hresult').dataset.kind==='good'||el('hresult').dataset.kind==='bad',15000);
    const saved=el('hresult').textContent,savedKind=el('hresult').dataset.kind,writtenRows=written?written.trim().split(String.fromCharCode(10)).length-1:0;
    window.showSaveFilePicker=async()=>({createWritable:async()=>({write:async()=>{throw new Error('disk full')},close:async()=>{},abort:async()=>{}})});
    el('hexport').click();await wait(()=>el('hresult').dataset.kind==='bad',15000);
    const failed=el('hresult').textContent;
    window.showSaveFilePicker=async()=>{throw new DOMException('no','AbortError')};
    el('hexport').click();await wait(()=>el('hresult').textContent.startsWith('Export cancelled'),5000);
    const cancelled=el('hresult').textContent;window.showSaveFilePicker=undefined;let downloaded=null;const clickReal=HTMLAnchorElement.prototype.click;HTMLAnchorElement.prototype.click=function(){downloaded=this.download};el('hexport').click();await wait(()=>el('hresult').textContent.startsWith('Download of'),15000);const fallback=el('hresult').textContent;HTMLAnchorElement.prototype.click=clickReal;
    return {visible,rows,newestFirst,statusKind:el('hstatus').dataset.kind,header:lines[0].trim(),csvRows:lines.length-1,written:writtenRows,savedKind,saved,failed,cancelled,fallback,downloaded}`);
  check("history opens and shows real bars, newest first", live.visible && live.rows > 0 && live.newestFirst, { rows: live.rows, newestFirst: live.newestFirst });
  check("CSV has the documented header and the same rows as the table", live.header === "time_utc,symbol,interval,open,high,low,close,volume,source,downloaded_at_utc" && live.csvRows === live.rows, { csv: live.csvRows, table: live.rows });
  check("export reports saved only after the file is written", live.savedKind === "good" && live.written === live.rows && live.saved.startsWith("Saved"), live.saved);
  check("a failed write is reported as a failure", live.failed.startsWith("Export failed") && live.failed.includes("disk full"), live.failed);
  check("a cancelled save dialog is reported as cancelled", live.cancelled.startsWith("Export cancelled"), live.cancelled);
  check("without a save dialog, export says the download started and does not claim it saved", live.fallback.startsWith("Download of") && !live.fallback.includes("Saved") && live.downloaded?.endsWith(".csv"), live.fallback);
  const states = await page.eval(`${HELPERS}
    const wait=async(f,ms=30000)=>{const s=Date.now();while(Date.now()-s<ms&&!f())await sleep(150)};
    el('hstart').value=el('hend').value='2000-01-03';el('hload').click();
    await wait(()=>!el('hempty').hidden||el('hstatus').dataset.kind==='bad');
    const empty={shown:!el('hempty').hidden,exportDisabled:el('hexport').disabled,tableHidden:el('htable').hidden,status:el('hstatus').textContent};
    const real=window.fetch;window.fetch=(u,o)=>String(u).includes('/api/history')?Promise.resolve(new Response(JSON.stringify({error:'fixture failure'}),{status:502})):real(u,o);
    el('hload').click();await wait(()=>el('hstatus').dataset.kind==='bad',5000);
    const error={text:el('hstatus').textContent,exportDisabled:el('hexport').disabled};window.fetch=real;
    key('Escape');await sleep(200);const closed=el('history').hidden,pairBefore=t('pair');
    return {empty,error,closed,pairBefore}`);
  check("empty history explains itself and disables export", states.empty.shown && states.empty.exportDisabled && states.empty.tableHidden, states.empty.status);
  check("a history error is shown and disables export", states.error.text.includes("fixture failure") && states.error.exportDisabled, states.error.text);
  check("Esc closes history", states.closed, states.closed);
}

/* After an update, the page must say to restart. Old-backend answers are simulated inside the page. */
async function restartNotice(page) {
  const r = await page.eval(`${HELPERS}
    const {checkBackend}=await import('/assets/js/ui/status.js'),real=window.fetch;
    const bar=()=>el('alertbar').hidden?null:t('alerttext');
    await checkBackend();const current=bar();
    window.fetch=(u,o)=>String(u).includes('/api/health')?Promise.resolve(new Response('{"service":"resolute-quant"}',{status:200})):real(u,o);
    await checkBackend();const older=bar();
    window.fetch=(u,o)=>String(u).includes('/api/health')?real(u,o).then(async x=>{const j=await x.json();j.restartNeeded=true;return new Response(JSON.stringify(j),{status:200})}):real(u,o);
    await checkBackend();const changed=bar();
    window.fetch=real;await checkBackend();const restarted=bar();
    window.fetch=(u,o)=>String(u).includes('/api/history')?Promise.resolve(new Response('{"error":"Not found."}',{status:404})):real(u,o);
    await cmd('HISTORY',1500);const historyMessage=t('hstatus');window.fetch=real;key('Escape');await sleep(200);
    return {current,older,changed,restarted,historyMessage}`);
  check("the page says nothing when the backend is current", r.current === null, r.current);
  check("the page says to restart for a backend from before this check", r.older?.includes("Close the black window"), r.older);
  check("the page says to restart when files changed after startup", r.changed?.includes("Close the black window"), r.changed);
  check("the restart message clears after a restart", r.restarted === null, r.restarted);
  check("history explains an out-of-date backend instead of saying not found", r.historyMessage.includes("Close the black window") && !r.historyMessage.includes("Not found"), r.historyMessage);
}

/* Browser storage belongs to the exact address, so a different port starts with no saved settings. */
async function storagePerPort(page) {
  const other = spawn(PYTHON, ["-m", "http.server", String(PORT + 1), "--bind", "127.0.0.1", "--directory", join(PROJECT, "assets", "img-none-" + Date.now())], { stdio: "ignore" });
  await sleep(1500);
  const here = await page.eval(`return Object.keys(localStorage).filter(k=>k.startsWith('subash.')).sort()`);
  await page.send("Page.navigate", { url: `http://127.0.0.1:${PORT + 1}/` });
  await sleep(1500);
  const there = await page.eval(`return Object.keys(localStorage).filter(k=>k.startsWith('subash.')).sort()`);
  other.kill();
  await page.send("Page.navigate", { url: BASE });
  await sleep(5000);
  check("saved settings do not carry across ports (documented limit)", here.length > 0 && there.length === 0, { here, there });
}

async function restart(page) {
  await page.eval(`${HELPERS} await cmd('BTC',3000);return 1`);
  await stopServer();
  await sleep(5000);
  const off = await page.eval(`${HELPERS} return {y:t('ystatustext'),crypto:t('statustext'),note:el('ynote').hidden?null:t('ynote'),rows:document.querySelectorAll('.row').length,last:t('bigpx')}`);
  check("backend stopped: offline message shown and page stays up", off.note === "Data program offline. Start Start Terminal.bat." && off.y === "STOCKS OFF" && off.crypto === "CRYPTO OFF" && off.rows === 20, off);
  const stillResponds = await page.eval(`${HELPERS} key('ArrowDown');await sleep(800);return t('pair')`);
  check("backend stopped: controls still respond, without error", stillResponds !== "BTC-USD", stillResponds);
  await page.eval(`${HELPERS} key('ArrowUp');await sleep(500);return 1`);
  startServer();
  await waitForServer();
  await sleep(12000);
  const back = await page.eval(`${HELPERS} return {y:t('ystatustext'),crypto:t('statustext'),noteHidden:el('ynote').hidden,rate:t('rate'),book:t('booksrc')}`);
  check("backend restarted: page recovers without a reload", back.noteHidden && back.crypto === "LIVE" && back.y !== "STOCKS OFF" && back.book.startsWith("LIVE"), back);
}

async function idle(page, minutes) {
  await page.eval(`${HELPERS}
    const events=await import('/assets/js/store/events.js');
    const {C}=await import('/assets/js/chart/ohlc.js');
    const p=globalThis.__probe={start:Date.now(),heap:[],dom:[],frames:0,maxGap:0,over100:0,over250:0,last:performance.now(),trades:0,statusSamples:{},visible:document.visibilityState,tick:0,flushStart:C.flushes,gapTimes:[],C};
    events.on('trade',()=>p.trades++);
    const f=now=>{const g=now-p.last;p.last=now;p.frames++;if(g>p.maxGap)p.maxGap=g;if(g>100)p.over100++;if(g>250){p.over250++;p.gapTimes.push([Math.round((Date.now()-p.start)/1000),Math.round(g)])}requestAnimationFrame(f)};requestAnimationFrame(f);
    setInterval(()=>{p.tick++;const s=t('statustext');p.statusSamples[s]=(p.statusSamples[s]||0)+1},1000);
    return 1`);
  const heapSeries = [];
  const totalSeconds = minutes * 60;
  const startedAt = Date.now();
  /* Forced garbage collection can itself stall frames, so it runs only before and after the measured window. */
  const collectedHeap = async () => {
    await page.send("HeapProfiler.collectGarbage");
    return +((await page.send("Runtime.getHeapUsage")).usedSize / 1048576).toFixed(2);
  };
  const heapStart = await collectedHeap();
  await page.eval(`const p=globalThis.__probe;p.start=Date.now();p.frames=0;p.maxGap=0;p.over100=0;p.over250=0;p.gapTimes=[];p.trades=0;p.flushStart=p.C.flushes;p.statusSamples={};return 1`);
  while ((Date.now() - startedAt) / 1000 < totalSeconds) {
    await sleep(30000);
    const metrics = await page.send("Runtime.getHeapUsage");
    const dom = await page.eval(`return document.getElementsByTagName('*').length`);
    heapSeries.push({ minute: +((Date.now() - startedAt) / 60000).toFixed(1), heapMB: +(metrics.usedSize / 1048576).toFixed(2), domNodes: dom });
    result.idle = { minutes, heapSeries };
    save();
  }
  const probe = await page.eval(`const p=globalThis.__probe;const s=(Date.now()-p.start)/1000;return {elapsedSeconds:s,frames:p.frames,maxGapMs:Math.round(p.maxGap),gapsOver100ms:p.over100,gapsOver250ms:p.over250,gapTimes:p.gapTimes,tradesToChart:p.trades,chartUpdates:p.C.flushes-p.flushStart,chartUpdatesPerSecond:+((p.C.flushes-p.flushStart)/s).toFixed(2),statusSamples:p.statusSamples}`);
  const heapEnd = await collectedHeap();
  const health = await (await fetch(`${BASE}/api/health`)).json();
  result.idle = { minutes, heapSeries, heapStart, heapEnd, probe, health };
  check(`idle ${minutes} min: completed without interruption`, probe.elapsedSeconds >= totalSeconds - 5, Math.round(probe.elapsedSeconds));
  check("idle: heap after GC grew by no more than 10% over the run", heapEnd <= heapStart * 1.1, { heapStart, heapEnd });
  check("idle: chart updates at no more than 5 per second", probe.chartUpdatesPerSecond <= 5, { updates: probe.chartUpdates, perSecond: probe.chartUpdatesPerSecond, trades: probe.tradesToChart });
  check("idle: DOM size stable (within 5%)", Math.max(...heapSeries.map((s) => s.domNodes)) <= Math.min(...heapSeries.map((s) => s.domNodes)) * 1.05, [Math.min(...heapSeries.map((s) => s.domNodes)), Math.max(...heapSeries.map((s) => s.domNodes))]);
  check("idle: feed stayed LIVE in at least 99% of samples", (probe.statusSamples.LIVE || 0) >= 0.99 * Object.values(probe.statusSamples).reduce((a, b) => a + b, 0), probe.statusSamples);
  check("idle: no frame gap over 250 ms", probe.gapsOver250ms === 0, { maxGapMs: probe.maxGapMs, gapsOver100ms: probe.gapsOver100ms, gapsOver250ms: probe.gapsOver250ms });
}

function networkAudit() {
  const hosts = {};
  for (const url of Object.keys(result.requests)) {
    const host = new URL(url).host;
    hosts[host] = (hosts[host] || 0) + result.requests[url];
  }
  result.hosts = hosts;
  const providers = Object.keys(hosts).filter((host) => !host.startsWith("127.0.0.1") && !/fonts\.(googleapis|gstatic)\.com$/.test(host));
  check("network audit: page requested only the local server and Google Fonts", providers.length === 0, hosts);
  check("network audit: page opened no WebSocket", result.webSockets.length === 0, result.webSockets);
}

async function main() {
  startServer();
  const health = await waitForServer();
  check("health identifies the service", health.service === "resolute-quant", { port: health.port, source: health.portSource, status: health.status });

  /* A browser left on the debug port would be silently reused, so its page and profile would not be fresh. */
  const stale = await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/version`).then(() => true, () => false);
  if (stale) throw new Error(`debug port ${DEBUG_PORT} is already in use`);
  const profile = mkdtempSync(join(tmpdir(), "subash-edge-"));
  const browser = spawn(EDGE, ["--headless=new", `--remote-debugging-port=${DEBUG_PORT}`, `--user-data-dir=${profile}`, "--no-first-run", "--disable-extensions", "--window-size=1600,1000", "about:blank"], { stdio: "ignore" });
  try {
    let target;
    for (let i = 0; i < 40 && !target; i++) {
      await sleep(500);
      try {
        target = (await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json()).find((x) => x.type === "page");
      } catch { /* browser starting */ }
    }
    if (!target) throw new Error("browser did not start");
    const ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    const page = new Page(ws);
    page.handlers["Network.requestWillBeSent"] = (p) => {
      const clean = p.request.url.split("?")[0];
      if (clean.startsWith("http")) result.requests[clean] = (result.requests[clean] || 0) + 1;
    };
    page.handlers["Network.webSocketCreated"] = (p) => result.webSockets.push(p.url);
    page.handlers["Runtime.exceptionThrown"] = (p) => result.exceptions.push(p.exceptionDetails.exception?.description || p.exceptionDetails.text);
    page.handlers["Log.entryAdded"] = (p) => { if (["error", "warning"].includes(p.entry.level)) result.console.push(`${new Date().toISOString().slice(11, 19)} ${p.entry.level} ${p.entry.text} ${p.entry.url || ""}`.slice(0, 200)); };
    page.handlers["Runtime.consoleAPICalled"] = (p) => { if (["error", "warning"].includes(p.type)) result.console.push(`${p.type} ${p.args.map((a) => a.value ?? a.description).join(" ")}`.slice(0, 200)); };
    for (const domain of ["Page", "Runtime", "Network", "Log"]) await page.send(`${domain}.enable`);
    await page.send("Page.navigate", { url: BASE });
    /* Timing is polled: on a fresh profile the page waits for the font stylesheet before it starts. */
    const navigated = Date.now();
    const waitIntro = async (hidden, limitMs) => {
      const start = Date.now();
      while (Date.now() - start < limitMs) {
        const state = await page.eval(`const i=document.getElementById('intro');return i?i.hidden:null`);
        if (state === hidden) return Date.now() - start;
        await sleep(100);
      }
      return null;
    };
    const opened = await waitIntro(false, 20000);
    check("first visit in a fresh profile opens the logo intro", opened !== null, { msAfterNavigate: Date.now() - navigated });
    const closed = await waitIntro(true, 6000);
    check("first-visit intro closes by itself", closed !== null, { msToClose: closed });
    await brand(page);

    await regression(page);
    await dataHonesty(page);
    await historyPanel(page);
    await restartNotice(page);
    await storagePerPort(page);
    const beforeRestartConsole = result.console.length;
    await restart(page);
    result.consoleBeforeRestart = beforeRestartConsole;
    networkAudit();
    save();
    if (IDLE_MINUTES > 0) {
      await page.send("Page.reload");
      await sleep(6000);
      const idleConsoleStart = result.console.length;
      await page.send("HeapProfiler.enable");
      await idle(page, IDLE_MINUTES);
      result.consoleDuringIdle = result.console.slice(idleConsoleStart);
      check("idle: no console errors", result.consoleDuringIdle.length === 0, result.consoleDuringIdle.slice(0, 5));
      networkAudit();
    }
    check("no uncaught page exceptions in the whole run", result.exceptions.length === 0, result.exceptions.slice(0, 3));
    ws.close();
  } finally {
    killBrowser(profile);
    await stopServer().catch(() => {});
    await sleep(1500);
    try { rmSync(profile, { recursive: true, force: true }); } catch { /* the browser may still hold a file */ }
  }
  result.finished = new Date().toISOString();
  result.summary = { pass: result.checks.filter((c) => c.pass).length, fail: result.checks.filter((c) => !c.pass).length };
  save();
  console.log(JSON.stringify(result.summary));
  process.exit(result.summary.fail ? 1 : 0);
}

main().catch((error) => {
  console.error(error);
  result.fatal = String(error);
  save();
  try { serverProcess.kill(); } catch { /* already gone */ }
  process.exit(1);
});
