# Resolute Quant native app

A Windows desktop foundation: C++20, Qt 6 Widgets, and an embedded private Python. It proves the window and Python work together. **It has no market data, models, or trading functions, and it opens no network ports.** The browser terminal at the repository root is still the working product.

## What it does

- A window with sidebar navigation and a status bar.
- **Python Health page:** Python version and location, whether isolation is active, and the NumPy version.
- **A real calculation:** type numbers, and NumPy returns the mean and sample standard deviation.
- **Error display:** type a word in the list, and the Python error and traceback appear in the window.
- **A long job:** Python stays busy for 5 seconds with progress and Cancel, while the window keeps updating.

## Build it

Paths must stay short, because Windows long paths are off by default. These commands run from the repository root in PowerShell.

### 1. Install the tools once

These change your system, and their licence terms are yours to accept.

```bash
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

```bash
winget install --id Kitware.CMake --exact
```

```bash
winget install --id Ninja-build.Ninja --exact
```

Then install Qt to `C:\Qt` with the [Qt Online Installer](https://www.qt.io/download-qt-installer-oss), which needs a free Qt account. You need **Qt → Qt 6.12.0 → MSVC 2022 64-bit**, not the entries under AI tools. If Qt is already installed, this adds only that kit:

```bash
C:\Qt\MaintenanceTool.exe --root C:\Qt --accept-licenses --default-answer --confirm-command --no-default-installations install qt.qt6.6120.win64_msvc2022_64
```

That command accepts Qt's licence terms for the component.

### 2. Create the private Python once

This downloads Python 3.13.17 and NumPy 2.5.4, checks their checksums, and puts them in `native\runtime\`. Nothing is installed system-wide.

```bash
powershell -ExecutionPolicy Bypass -File native\tools\setup_runtime.ps1
```

### 3. Build and test

```bash
powershell -ExecutionPolicy Bypass -File native\tools\build.ps1 -Clean -Test
```

The first build downloads pybind11, again with a checksum. The tests briefly open a real window.

### 4. Run it

This makes a self-contained folder at `native\dist\ResoluteQuant\`, of about 150 MB.

```bash
powershell -ExecutionPolicy Bypass -File native\tools\deploy.ps1
```

```bash
native\dist\ResoluteQuant\ResoluteQuant.exe
```

### Starting again

`native\build`, `native\runtime` and `native\dist` are all ignored by Git and safe to delete. Delete them, and run steps 2 to 4 again.

## How it fits together

```text
Window (UI thread)  --submit, cancel-->  PythonEngine (one background thread)
                    <--signals: progress, result, error--
                                               |
                                 rq_python_host.dll  (the only code that links Python)
                                               |
                         Embedded Python + python/rq_engine + NumPy
```

- **`src/app/`** is the window. It never touches Python.
- **`src/bridge/PythonEngine`** runs one job at a time on one thread, and only passes plain C++ results back.
- **`src/bridge/PythonHost`** is the only file that includes Python. The app loads this DLL at runtime, after checking the private Python exists, so a missing runtime gives a message in the window rather than a Windows loader error.
- **`python/rq_engine/`** holds the jobs. To add one, write a function taking `(args, host)` that returns a JSON-friendly dictionary, and register it in `runner.py`.

## Known limits

- **A native crash closes the app.** Python exceptions are caught and shown, but a crash inside a native library, such as NumPy, can't be caught when Python runs in the same process. Risky future work should run in a child process launched from Python, behind `PythonEngine`, without changing any screens.
- **Cancel is cooperative.** A job must call `host.raise_if_cancelled()`. A job stuck in a native call can't be interrupted, so on exit the app waits 3 seconds, logs it, and ends the process with exit code 4.
- **One job at a time.** Other jobs wait in the queue.
- **Python can't restart within a running app.** If it fails to start, close and reopen the app.
- **Not ready to give to other people.** The deployed folder relies on the Microsoft C++ runtime being installed, and has no installer, code signing or Qt licence notices. It has only been run on the PC it was built on.
- **Release builds only.** The private Python has no debug libraries.

## Logs

Warnings and errors go to `%LOCALAPPDATA%\ResoluteQuant\native.log`, which restarts after 1 MB. It holds job names and error text. Error text can quote what was typed into a calculation. Never add credentials to it.

## Alongside the rest of the project

| Component | Effect |
| --- | --- |
| Browser terminal, `Start Terminal.bat`, `server.py` | Unchanged, and not used by this app. Both can run at once |
| GitHub Pages site | Unchanged |
| Render deployment draft | Unaffected. This is a desktop program and can't deploy to Render. A hosted version would stay browser-based, with the provider-terms questions already raised on that draft |
| Ports and credentials | The app opens no ports, and stores and needs no credentials |

## Where later work plugs in

None of this is built. Each still has open questions about data sources and their terms.

| Future work | Where it goes |
| --- | --- |
| Settings and provenance storage | SQLite through Qt's SQL module, which is the proposed next task |
| Market data | An `rq_engine` module reusing `history.py` and the validation in `crypto_feed.py`. Provider terms must be checked first, and running alongside the browser terminal could open two Coinbase connections |
| Reports, disclosures, and the company and supplier graph | Background `rq_engine` jobs, with new screens beside the Python Health page |
| Predictive models | An `rq_engine` module, ideally in a child process. Blocked on permitted data and agreed thresholds |
| Enterprise and procurement integrations | A new service layer, with credentials kept in Windows Credential Manager |

## Licences

Exact versions and checksums are in [dependencies.lock.md](dependencies.lock.md). Free Qt is used under LGPLv3 and linked dynamically. Building and running on your own PC doesn't trigger distribution duties, but giving the app to others does, including providing the LGPL text and notices and allowing relinking. Get legal advice before distributing it.
