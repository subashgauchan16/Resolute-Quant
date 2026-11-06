# Configures, builds and optionally tests the native app inside the MSVC x64 environment.
# Usage: powershell -ExecutionPolicy Bypass -File native\tools\build.ps1 [-Clean] [-Test]
param([switch]$Clean, [switch]$Test)
$ErrorActionPreference = 'Stop'

$native = Split-Path $PSScriptRoot -Parent
$machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$user = [Environment]::GetEnvironmentVariable('Path', 'User')
$env:Path = "$machine;$user"

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools 2022 were not found. See native\README.md.' }
$vs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath
if (-not $vs) { throw 'No Visual Studio instance with the C++ tools was found. See native\README.md.' }

if ($Clean) { Remove-Item (Join-Path $native 'build\release') -Recurse -Force -ErrorAction SilentlyContinue }

$steps = @(
  'cmake --preset release',
  'cmake --build --preset release'
)
if ($Test) { $steps += 'ctest --preset release' }

$batch = Join-Path ([IO.Path]::GetTempPath()) "rq-build-$PID.bat"
$lines = @('@echo off', "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul", 'if errorlevel 1 exit /b 1', "cd /d `"$native`"")
foreach ($step in $steps) { $lines += $step; $lines += 'if errorlevel 1 exit /b 1' }
$lines | Set-Content $batch -Encoding ascii
try {
  & cmd.exe /c $batch
  $code = $LASTEXITCODE
} finally {
  Remove-Item $batch -ErrorAction SilentlyContinue
}
exit $code
