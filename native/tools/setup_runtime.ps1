# Creates or updates the private Python used by the native app in native\runtime\python.
# Nothing is installed system-wide. Delete native\runtime to remove it.
# Usage: powershell -ExecutionPolicy Bypass -File native\tools\setup_runtime.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$version = '3.13.17'
$sha256 = 'DCE409744B088F94C6E37BA41F8100C384C27E029E843237364C602D30D475E5'
$url = "https://www.nuget.org/api/v2/package/python/$version"

$native = Split-Path $PSScriptRoot -Parent
$runtime = Join-Path $native 'runtime'
$target = Join-Path $runtime 'python'
$python = Join-Path $target 'python.exe'
$lock = Join-Path $native 'python\requirements.lock'
$marker = Join-Path $runtime 'installed-lock.sha256'
$lockHash = (Get-FileHash $lock -Algorithm SHA256).Hash

if ($Force -and (Test-Path $target)) {
  Remove-Item $target -Recurse -Force
  Remove-Item $marker -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $runtime | Out-Null

if (-not (Test-Path $target)) {
  $work = Join-Path $runtime "download-$PID"
  New-Item -ItemType Directory $work | Out-Null
  try {
    $package = Join-Path $work 'python.zip'
    "Downloading Python $version"
    Invoke-WebRequest $url -OutFile $package -UseBasicParsing
    $actual = (Get-FileHash $package -Algorithm SHA256).Hash
    if ($actual -ne $sha256) { throw "Checksum mismatch for Python ${version}: got $actual" }
    Expand-Archive $package -DestinationPath (Join-Path $work 'unpacked')
    Move-Item (Join-Path $work 'unpacked\tools') $target
  } finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  }
}

# Python 3.13.17 ships SQLite 3.50.4, which has a documented WAL bug. This replaces it with a fixed release.
$sqliteVersion = '3.54.1'
$sqliteSha256 = 'F436F53AD8F965AF43F3817B42C99C0689E6787F6B7ACE208FE38DAB7FFC4971'
$sqliteUrl = 'https://www.sqlite.org/2026/sqlite-dll-win-x64-3540100.zip'
$current = (& $python -I -B -c "import sqlite3; print(sqlite3.sqlite_version)").Trim()
if ($current -ne $sqliteVersion) {
  $work = Join-Path $runtime "sqlite-$PID"
  New-Item -ItemType Directory $work | Out-Null
  try {
    "Installing SQLite $sqliteVersion in place of $current"
    $package = Join-Path $work 'sqlite.zip'
    Invoke-WebRequest $sqliteUrl -OutFile $package -UseBasicParsing
    $actual = (Get-FileHash $package -Algorithm SHA256).Hash
    if ($actual -ne $sqliteSha256) { throw "Checksum mismatch for SQLite ${sqliteVersion}: got $actual" }
    Expand-Archive $package -DestinationPath (Join-Path $work 'unpacked')
    Copy-Item (Join-Path $work 'unpacked\sqlite3.dll') (Join-Path $target 'DLLs\sqlite3.dll') -Force
  } finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  }
}

$installed = if (Test-Path $marker) { (Get-Content $marker -Raw).Trim() } else { '' }
if ($installed -ne $lockHash) {
  "Installing pinned packages"
  # pip reports progress on stderr, which must not count as a failure here.
  $ErrorActionPreference = 'Continue'
  & $python -I -m pip install --disable-pip-version-check --no-warn-script-location --no-deps --only-binary ':all:' --require-hashes -r $lock 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 2
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($code -ne 0) { throw "pip failed with exit code $code" }
  Set-Content $marker $lockHash -Encoding ascii
} else {
  'Pinned packages are already installed.'
}

& $python -I -c "import sys, sqlite3, numpy, pandas, yfinance, websockets; print('Python', sys.version.split()[0], 'SQLite', sqlite3.sqlite_version, 'NumPy', numpy.__version__, 'pandas', pandas.__version__, 'yfinance', yfinance.__version__, 'websockets', websockets.__version__)"
if ($LASTEXITCODE -ne 0) { throw 'The runtime failed its import check' }
