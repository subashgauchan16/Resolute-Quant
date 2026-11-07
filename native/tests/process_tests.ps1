# Starts the real exe as a separate process and checks its exit codes, output and leftovers.
param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$QtBin,
  [int]$Launches = 5
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process_helpers.ps1')

$env:Path = "$QtBin;$env:SystemRoot\System32;$env:SystemRoot"
$env:PYTHONHOME = 'C:\bogus-python-home'
$env:PYTHONPATH = 'C:\bogus-python-path'
Assert-NoneRunning $Exe 'before the tests'

$env:QT_QPA_PLATFORM = 'offscreen'
foreach ($launch in 1..$Launches) {
  $run = Invoke-App $Exe @('--smoke-test', '--offline')
  Assert-True ($run.ExitCode -eq 0) "Launch $launch exited with $($run.ExitCode). Output: $($run.Output)"
  $report = $run.Output | ConvertFrom-Json
  Assert-True ($report.ok -eq $true) "Launch $launch reported failure: $($run.Output)"
  Assert-True ($report.isolated -eq $true) "Launch $launch was not isolated"
  Assert-NoneRunning $Exe "after launch $launch"
}
"PASS $Launches launches each ran Python and exited with code 0, leaving no process behind"

$run = Invoke-App $Exe @('--smoke-test', '--offline', '--python-runtime', 'C:\rq-no-such-runtime')
Assert-True ($run.ExitCode -eq 2) "Missing runtime exited with $($run.ExitCode). Output: $($run.Output)"
Assert-True ($run.Output -match 'rq-no-such-runtime') "Missing runtime message does not name the folder: $($run.Output)"
Assert-NoneRunning $Exe 'after the missing-runtime test'
"PASS A missing runtime gives exit code 2 and names the folder"

$clock = [Diagnostics.Stopwatch]::StartNew()
$run = Invoke-App $Exe @('--test-stuck-shutdown', '--offline')
Assert-True ($run.ExitCode -eq 4) "Stuck shutdown exited with $($run.ExitCode)"
Assert-True ($clock.Elapsed.TotalSeconds -lt 15) "Stuck shutdown took $($clock.Elapsed.TotalSeconds) seconds"
Assert-NoneRunning $Exe 'after the stuck-shutdown test'
"PASS A job stuck in a native call ends the process with code 4 in $([math]::Round($clock.Elapsed.TotalSeconds, 1)) s rather than hanging"

Remove-Item Env:QT_QPA_PLATFORM
$process = Start-Process $Exe -ArgumentList '--offline' -PassThru
$null = $process.Handle  # PowerShell loses ExitCode unless the handle is read while running.
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and $process.MainWindowHandle -eq 0) { Start-Sleep -Milliseconds 100; $process.Refresh() }
Assert-True ($process.MainWindowHandle -ne 0) 'The real window did not open'
Start-Sleep -Seconds 2
[void]$process.CloseMainWindow()
Assert-True $process.WaitForExit(10000) 'The window closed but the process did not exit'
Assert-True ($process.ExitCode -eq 0) "Closing the window exited with $($process.ExitCode)"
Assert-NoneRunning $Exe 'after closing the window'
"PASS Closing the real window exits with code 0"
