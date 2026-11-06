# Shared by the process-level tests.

function Assert-True($condition, [string]$message) {
  if (-not $condition) { throw "FAIL $message" }
}

function Get-RunningCopies([string]$exe) {
  $target = [IO.Path]::GetFullPath($exe)
  @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue |
      Where-Object { $_.Path -eq $target })
}

function Assert-NoneRunning([string]$exe, [string]$when) {
  $deadline = (Get-Date).AddSeconds(5)
  while ((Get-RunningCopies $exe).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
  Assert-True ((Get-RunningCopies $exe).Count -eq 0) "A process was left running $when"
}

# Runs the app, capturing what it prints. Stops it and fails if it does not finish in time.
function Invoke-App([string]$exe, [string[]]$arguments, [int]$timeoutSeconds = 60) {
  $folder = Join-Path ([IO.Path]::GetTempPath()) "rq-test-$PID"
  New-Item -ItemType Directory -Force $folder | Out-Null
  $outFile = Join-Path $folder 'out.txt'
  try {
    $process = Start-Process $exe -ArgumentList $arguments -PassThru -RedirectStandardOutput $outFile -RedirectStandardError (Join-Path $folder 'err.txt')
    $null = $process.Handle  # PowerShell loses ExitCode unless the handle is read while running.
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
      $process.Kill()
      throw "FAIL $exe $arguments did not finish within $timeoutSeconds seconds"
    }
    $process.WaitForExit()
    [pscustomobject]@{ ExitCode = $process.ExitCode; Output = (Get-Content $outFile -Raw -ErrorAction SilentlyContinue) }
  } finally {
    Remove-Item $folder -Recurse -Force -ErrorAction SilentlyContinue
  }
}
