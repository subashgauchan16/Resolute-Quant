# Creates the private Python used by the native app in native\runtime\python.
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

if ((Test-Path $target) -and -not $Force) {
  "Runtime already exists: $target. Use -Force to recreate it."
  & $python -I -c "import sys, numpy; print('Python', sys.version.split()[0], 'NumPy', numpy.__version__)"
  exit $LASTEXITCODE
}
if (Test-Path $target) { Remove-Item $target -Recurse -Force }
New-Item -ItemType Directory -Force $runtime | Out-Null

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

  "Installing pinned packages"
  & $python -I -m pip install --disable-pip-version-check --no-warn-script-location --no-deps --only-binary ':all:' --require-hashes -r (Join-Path $native 'python\requirements.lock')
  if ($LASTEXITCODE -ne 0) { throw "pip failed with exit code $LASTEXITCODE" }

  & $python -I -c "import sys, numpy; print('Python', sys.version.split()[0], 'NumPy', numpy.__version__)"
  if ($LASTEXITCODE -ne 0) { throw 'The new runtime failed its import check' }
} finally {
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
