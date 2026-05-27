param(
  [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildDir = Join-Path $repoRoot "build-libredwg"
$releaseDir = Join-Path $buildDir $Configuration
$distDir = Join-Path $repoRoot "dist\dgm2xyz-win64"

cmake --preset vs2022-libredwg
if ($LASTEXITCODE -ne 0) {
  throw "CMake configure failed with exit code $LASTEXITCODE"
}

cmake --build --preset release-libredwg
if ($LASTEXITCODE -ne 0) {
  throw "CMake build failed with exit code $LASTEXITCODE"
}

ctest --preset release-libredwg
if ($LASTEXITCODE -ne 0) {
  throw "CTest failed with exit code $LASTEXITCODE"
}

$exe = Join-Path $releaseDir "dgm2xyz.exe"
$libredwg = Join-Path $releaseDir "libredwg.dll"

if (!(Test-Path $exe)) {
  throw "Missing dgm2xyz.exe at $exe"
}

if (!(Test-Path $libredwg)) {
  throw "Missing libredwg.dll at $libredwg. The release build is not ready to ship DWG support."
}

New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Copy-Item -Force $exe $distDir
Copy-Item -Force $libredwg $distDir

Write-Host "Packaged ready-to-run app:"
Write-Host $distDir
