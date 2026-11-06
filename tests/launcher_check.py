"""Launcher and port-conflict test using the real Start Terminal.bat on a fresh copy of the project.

    python tests/launcher_check.py <empty-work-folder> <output.json> [busy_port]

Each scenario starts a new cmd.exe session. The BROWSER variable points at a small logger, so no
window opens and the exact URL the launcher chose is recorded. The first scenario builds .venv
from nothing. Only processes started here are stopped. The busy port's owner is never touched.
"""
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
work, out = Path(sys.argv[1]), Path(sys.argv[2])
BUSY = int(sys.argv[3]) if len(sys.argv) > 3 else 8765
copy = work / "Subash Terminal"
log = work / "browser.log"
results = []


def health(port):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/health", timeout=2) as response:
            return json.load(response)
    except Exception:
        return None


def http_status(port):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=3) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code
    except Exception as error:
        return type(error).__name__


def launch(args="", env=None):
    if log.exists():
        log.unlink()
    logger = work / "log_browser.py"
    browser = f"{sys.executable} {logger} %s".replace("\\", "/")
    full_env = {k: v for k, v in os.environ.items() if k != "RESOLUTE_QUANT_PORT"}
    full_env.update(BROWSER=browser, PYTHONUTF8="1", PYTHONUNBUFFERED="1", PYTHONDONTWRITEBYTECODE="1", **(env or {}))
    output = (work / "console.txt").open("w", encoding="utf-8", errors="replace")
    process = subprocess.Popen(f'cmd.exe /c ""{copy / "Start Terminal.bat"}" {args}"', cwd=copy, env=full_env, stdin=subprocess.DEVNULL,
                               stdout=output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    return process, output


def stop(process, output):
    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
    process.wait(timeout=20)
    output.close()


def wait_port(port, seconds, instance_only=True):
    deadline = time.time() + seconds
    while time.time() < deadline:
        body = health(port)
        if body:
            return body
        time.sleep(1)
    return None


def browser_urls(seconds=4):
    time.sleep(seconds)
    return log.read_text(encoding="utf-8").split() if log.exists() else []


def console():
    return (work / "console.txt").read_text(encoding="utf-8", errors="replace")


def record(name, passed, **detail):
    results.append({"scenario": name, "pass": bool(passed), **detail})
    print("PASS" if passed else "FAIL", name, json.dumps(detail)[:300])
    out.write_text(json.dumps({"busy_port": BUSY, "busy_port_status": http_status(BUSY), "results": results}, indent=1), encoding="utf-8")


def main():
    work.mkdir(parents=True, exist_ok=True)
    shutil.copytree(PROJECT, copy, ignore=shutil.ignore_patterns(".venv", "__pycache__", "docs", "research"))
    (work / "log_browser.py").write_text("import sys\nopen(sys.argv[0].replace('log_browser.py','browser.log'),'a').write(sys.argv[1]+'\\n')\n", encoding="utf-8")
    before = http_status(BUSY)
    fallback = BUSY + 1

    # 1. Fresh session, no .venv, default port busy: announced fallback, browser opens this terminal.
    process, output = launch()
    started = time.time()
    body = wait_port(fallback, 300)
    urls = browser_urls()
    text = console()
    record("fresh install; default port busy; deterministic fallback", body and body["port"] == fallback and body["fallback"]
           and urls == [f"http://127.0.0.1:{fallback}"] and "has not been touched" in text and f"USING PORT {fallback}" in text,
           seconds_to_ready=round(time.time() - started), browser=urls, source=body and body["portSource"], venv_created=(copy / ".venv").exists())

    # 2. Second launch while running: reuses the instance, opens it, starts no duplicate.
    first_instance = body and body["instance"]
    second, second_output = launch()
    time.sleep(8)
    urls = browser_urls(2)
    again = health(fallback)
    second_code = second.poll()
    stop(second, second_output)
    record("second launch reuses the running terminal", urls == [f"http://127.0.0.1:{fallback}"] and again and again["instance"] == first_instance,
           browser=urls, same_instance=bool(again and again["instance"] == first_instance), bat_exit=second_code)
    stop(process, output)

    # 3. Configured busy port: no fallback, clear message, nothing opened.
    process, output = launch(env={"RESOLUTE_QUANT_PORT": str(BUSY)})
    time.sleep(10)
    urls = browser_urls(1)
    text = console()
    stop(process, output)
    record("configured busy port does not fall back", not urls and health(fallback) is None and "no other port was tried" in text and "has not been touched" in text,
           browser=urls, message=[line for line in text.splitlines() if "no other port" in line])

    # 4. RESOLUTE_QUANT_PORT, config file and --port, in priority order.
    process, output = launch(env={"RESOLUTE_QUANT_PORT": str(BUSY + 7)})
    body = wait_port(BUSY + 7, 40)
    urls = browser_urls()
    stop(process, output)
    record("RESOLUTE_QUANT_PORT honoured", body and body["portSource"] == "RESOLUTE_QUANT_PORT" and urls == [f"http://127.0.0.1:{BUSY + 7}"], browser=urls)

    (copy / "terminal.config.json").write_text(json.dumps({"port": BUSY + 8}), encoding="utf-8")
    process, output = launch()
    body = wait_port(BUSY + 8, 40)
    urls = browser_urls()
    stop(process, output)
    record("terminal.config.json honoured", body and body["portSource"] == "terminal.config.json" and urls == [f"http://127.0.0.1:{BUSY + 8}"], browser=urls)

    process, output = launch(args=f"--port {BUSY + 9}", env={"RESOLUTE_QUANT_PORT": str(BUSY + 7)})
    body = wait_port(BUSY + 9, 40)
    urls = browser_urls()
    stop(process, output)
    record("--port beats RESOLUTE_QUANT_PORT and config", body and body["portSource"] == "--port" and urls == [f"http://127.0.0.1:{BUSY + 9}"], browser=urls)
    (copy / "terminal.config.json").unlink()

    # 5. A bad setting stops with a message.
    process, output = launch(env={"RESOLUTE_QUANT_PORT": "not-a-port"})
    time.sleep(8)
    text = console()
    urls = browser_urls(1)
    stop(process, output)
    record("bad port setting stops with a message", "must be a port number" in text and not urls, browser=urls)

    # 6. The busy port's owner is unaffected.
    record("other service on the busy port unchanged", http_status(BUSY) == before, before=before, after=http_status(BUSY))


if __name__ == "__main__":
    main()
