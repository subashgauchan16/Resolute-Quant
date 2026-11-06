# Copies the built app, Qt, the Python sources and the private Python into one self-contained folder.
# Run after a build. Usage: powershell -ExecutionPolicy Bypass -File native\tools\deploy.ps1 [-Out folder]
# The result is for this PC. Qt's LGPL duties apply before giving it to anyone else: see native\README.md.
param([string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\ResoluteQuant'))
$ErrorActionPreference = 'Stop'

$native = Split-Path $PSScriptRoot -Parent
$build = Join-Path $native 'build\release'
$qtBin = 'C:\Qt\6.12.0\msvc2022_64\bin'
$runtime = Join-Path $native 'runtime\python'
foreach ($required in (Join-Path $build 'ResoluteQuant.exe'), (Join-Path $qtBin 'windeployqt.exe'), $runtime) {
  if (-not (Test-Path $required)) { throw "Missing: $required. See native\README.md." }
}

if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null

Copy-Item (Join-Path $build 'ResoluteQuant.exe'), (Join-Path $build 'rq_python_host.dll') $Out
New-Item -ItemType Directory (Join-Path $Out 'python') | Out-Null
Copy-Item (Join-Path $native 'python\rq_engine') (Join-Path $Out 'python\rq_engine') -Recurse
# Shared with the browser terminal, from the repository root.
Copy-Item (Join-Path $native '..\history.py'), (Join-Path $native '..\crypto_feed.py') (Join-Path $Out 'python')
New-Item -ItemType Directory (Join-Path $Out 'runtime') | Out-Null
Copy-Item $runtime (Join-Path $Out 'runtime\python') -Recurse

$arguments = '--release', '--no-translations', '--no-system-d3d-compiler', '--no-opengl-sw', '--no-compiler-runtime', '--no-network', (Join-Path $Out 'ResoluteQuant.exe')
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$output = & (Join-Path $qtBin 'windeployqt.exe') @arguments 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($code -ne 0) { $output; throw "windeployqt failed with exit code $code" }

$restricted = Get-ChildItem $Out -Filter 'Qt6*.dll' | Where-Object { $_.Name -match '^Qt6(Charts|Graphs|DataVisualization|Quick3D|Grpc|HttpServer|Qml|Quick)' }
if ($restricted) { throw "Unexpected Qt modules in the deployment: $($restricted.Name -join ', ')" }

"Deployed to $Out"
"Qt modules: $((Get-ChildItem $Out -Filter 'Qt6*.dll').Name -join ', ')"
"Size: $([math]::Round(((Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB, 0)) MB"
