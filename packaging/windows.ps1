# Windows: a modjuke folder with Qt, libopenmpt and the Visual C++ runtime,
# zipped.
#
#   pwsh packaging/windows.ps1 [-BuildDir build] [-Version v1.0]
#
# Needs a Release build made with MSVC (BuildDir\modjuke.exe), Qt's bin folder
# on PATH (windeployqt), and a Visual Studio developer shell (for the runtime
# DLLs). Writes dist\modjuke-VERSION-windows-x64.zip.
param(
    [string]$BuildDir = "build",
    [string]$Version = ""
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$Root = (Get-Location).Path
if (-not $Version) {
    $Version = git describe --tags --always 2>$null
    if (-not $Version) { $Version = "dev" }
}
$Dist = Join-Path $Root "dist"
$Out = Join-Path $Dist "modjuke"
$Exe = Join-Path $BuildDir "modjuke.exe"
if (-not (Test-Path $Exe)) { throw "no ${Exe}: build modjuke first" }
if (-not $env:VCToolsRedistDir) { throw "VCToolsRedistDir is not set: run from a Visual Studio developer shell" }

# The official libopenmpt DLLs (checksum-verified)
$LibVersion = "0.8.9"
$Url = "https://lib.openmpt.org/files/libopenmpt/dev/libopenmpt-$LibVersion+release.dev.windows.vs2022.zip"
$Sha256 = "a23e375e576e3a6997ffeeb6cf34488e9020e2a5d8226856b02628b6a87c5e8e"
New-Item -ItemType Directory -Force $Dist | Out-Null
$Zip = Join-Path $Dist "libopenmpt-dev.zip"
if (-not (Test-Path $Zip)) { Invoke-WebRequest $Url -OutFile $Zip }
if ((Get-FileHash $Zip -Algorithm SHA256).Hash -ne $Sha256) {
    Remove-Item $Zip
    throw "checksum mismatch: $Url"
}
$Lib = Join-Path $Dist "libopenmpt-dev"
if (Test-Path $Lib) { Remove-Item -Recurse -Force $Lib }
Expand-Archive $Zip $Lib

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out, (Join-Path $Out "licenses") | Out-Null
Copy-Item $Exe $Out
# libopenmpt.dll and its openmpt-*.dll next to modjuke.exe, where it looks first
Copy-Item (Join-Path $Lib "bin\amd64\*.dll") $Out
Copy-Item (Join-Path $Lib "LICENSE.txt") (Join-Path $Out "licenses\libopenmpt.txt")
Copy-Item (Join-Path $Lib "Licenses\*") (Join-Path $Out "licenses")
Copy-Item (Join-Path $Root "packaging\THIRD-PARTY.txt") $Out

windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $Out "modjuke.exe")
if ($LASTEXITCODE) { throw "windeployqt failed" }

# The Visual C++ runtime, app-local, for Qt and libopenmpt
$Crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" |
    Select-Object -First 1
if (-not $Crt) { throw "Visual C++ runtime DLLs not found under $env:VCToolsRedistDir" }
Copy-Item (Join-Path $Crt.FullName "*.dll") $Out

$Package = Join-Path $Dist "modjuke-$Version-windows-x64.zip"
if (Test-Path $Package) { Remove-Item $Package }
Compress-Archive -Path $Out -DestinationPath $Package
Write-Host "OK: $Package"
