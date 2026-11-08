# Deploys the app to a temporary folder outside the repository, then runs it with Qt and every
# Python removed from PATH, to prove it depends only on what is inside that folder.
param([Parameter(Mandatory)][string]$Deploy)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process_helpers.ps1')

$folder = Join-Path ([IO.Path]::GetTempPath()) "rq-clean-$PID"
try {
  & powershell -NoProfile -ExecutionPolicy Bypass -File $Deploy -Out $folder
  Assert-True ($LASTEXITCODE -eq 0) "Deployment failed with exit code $LASTEXITCODE"
  $exe = Join-Path $folder 'ResoluteQuant.exe'
  Assert-True (-not (Test-Path (Join-Path $folder 'dev_runtime_path.txt'))) 'The development runtime pointer was deployed'

  $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
  $env:PYTHONHOME = 'C:\bogus-python-home'
  $env:PYTHONPATH = 'C:\bogus-python-path'
  Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue

  $run = Invoke-App $exe @('--smoke-test', '--offline')
  Assert-True ($run.ExitCode -eq 0) "The deployed app exited with $($run.ExitCode). Output: $($run.Output)"
  $report = $run.Output | ConvertFrom-Json
  Assert-True ($report.ok -eq $true) "The deployed app reported failure: $($run.Output)"
  $expected = [IO.Path]::GetFullPath((Join-Path $folder 'runtime\python'))
  Assert-True ($report.prefix -ieq $expected) "The deployed app used Python from $($report.prefix)"
  Assert-True ($report.storage.ok -eq $true) "The deployed app's storage self-test failed: $($run.Output)"
  Assert-True ($report.storage.sqlite_version -eq '3.54.1') "The deployed app uses SQLite $($report.storage.sqlite_version)"
  Assert-NoneRunning $exe 'after the deployed run'
  "PASS The deployed app ran from $folder using its own Qt and Python, in the real windowing mode"

  # With no python313.dll anywhere, a directly linked exe could not even start.
  Rename-Item (Join-Path $folder 'runtime') 'runtime-hidden'
  $run = Invoke-App $exe @('--smoke-test', '--offline')
  Assert-True ($run.ExitCode -eq 2) "With no runtime the deployed app exited with $($run.ExitCode). Output: $($run.Output)"
  Assert-True ($run.Output -match 'was not found') "With no runtime the message was: $($run.Output)"
  Assert-NoneRunning $exe 'after the no-runtime run'
  "PASS With its runtime removed, the deployed app still starts and reports the problem"
} finally {
  Get-RunningCopies (Join-Path $folder 'ResoluteQuant.exe') | Stop-Process -Force -ErrorAction SilentlyContinue
  Remove-Item $folder -Recurse -Force -ErrorAction SilentlyContinue
}
