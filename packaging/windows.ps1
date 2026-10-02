# Windows: a modjuke folder with Qt, libopenmpt and the Visual C++ runtime,
# zipped.
#
#   pwsh packaging/windows.ps1 [-BuildDir build]
#
# Needs a Release build made with MSVC (BuildDir\modjuke.exe), Qt's bin folder
# on PATH (windeployqt), and a Visual Studio developer shell (runtime DLLs,
# dumpbin). Writes dist\modjuke.zip.
param(
    [string]$BuildDir = "build"
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$Root = (Get-Location).Path
$Dist = Join-Path $Root "dist"
$Work = Join-Path $Dist "work"
$Out = Join-Path $Dist "modjuke"
$Exe = Join-Path $BuildDir "modjuke.exe"
if (-not (Test-Path $Exe)) { throw "no ${Exe}: build modjuke first" }
if (-not $env:VCToolsRedistDir) { throw "VCToolsRedistDir is not set: run from a Visual Studio developer shell" }

# The official libopenmpt DLLs (checksum-verified)
$LibVersion = "0.8.9"
$Url = "https://lib.openmpt.org/files/libopenmpt/dev/libopenmpt-$LibVersion+release.dev.windows.vs2022.zip"
$Sha256 = "a23e375e576e3a6997ffeeb6cf34488e9020e2a5d8226856b02628b6a87c5e8e"
New-Item -ItemType Directory -Force $Work | Out-Null
$Zip = Join-Path $Work "libopenmpt-dev.zip"
if (-not (Test-Path $Zip)) { Invoke-WebRequest $Url -OutFile $Zip }
if ((Get-FileHash $Zip -Algorithm SHA256).Hash -ne $Sha256) {
    Remove-Item $Zip
    throw "checksum mismatch: $Url"
}
$Lib = Join-Path $Work "libopenmpt-dev"
if (Test-Path $Lib) { Remove-Item -Recurse -Force $Lib }
Expand-Archive $Zip $Lib

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
$Licenses = Join-Path $Out "licenses"
New-Item -ItemType Directory -Force $Out, $Licenses | Out-Null
Copy-Item $Exe $Out
# libopenmpt.dll and the openmpt-*.dll it imports (MP3/Vorbis samples,
# compressed formats), next to modjuke.exe, where modjuke looks first
Copy-Item (Join-Path $Lib "bin\amd64\*.dll") $Out
Copy-Item (Join-Path $Root "LICENSE") (Join-Path $Out "LICENSE.txt")
Copy-Item (Join-Path $Root "packaging\THIRD-PARTY.txt") $Out
Copy-Item (Join-Path $Root "packaging\licenses\*") $Licenses
Copy-Item (Join-Path $Lib "LICENSE.txt") (Join-Path $Licenses "libopenmpt.txt")
Copy-Item (Join-Path $Lib "Licenses\*") $Licenses

# Qt's DLLs and plugins. No compiler runtime: windeployqt would add the
# 25 MB vc_redist installer; the DLLs are copied below instead.
windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime `
    (Join-Path $Out "modjuke.exe")
if ($LASTEXITCODE) { throw "windeployqt failed" }

# windeployqt deploys everything Qt Multimedia, Gui and Network could use.
# modjuke only needs audio output and PNG images, so these go:
#  - FFmpeg and its plugin (video/audio file decoding; the Windows media
#    plugin stays as the multimedia backend),
#  - the DirectX shader compiler (Qt Quick / 3D only),
#  - TLS, network information, touch, JPEG/GIF/SVG/ICO and SVG icon plugins.
$Drop = @("avcodec-*.dll", "avformat-*.dll", "avutil-*.dll", "swresample-*.dll", "swscale-*.dll",
          "multimedia\ffmpegmediaplugin.dll", "dxcompiler.dll", "dxil.dll", "Qt6Svg.dll",
          "tls", "networkinformation", "generic", "imageformats", "iconengines")
foreach ($pattern in $Drop) {
    Get-Item (Join-Path $Out $pattern) -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
}

# The Visual C++ runtime DLLs that Qt and libopenmpt import, app-local
$Crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" |
    Select-Object -First 1
if (-not $Crt) { throw "Visual C++ runtime DLLs not found under $env:VCToolsRedistDir" }
foreach ($dll in "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    Copy-Item (Join-Path $Crt.FullName $dll) $Out
}

# Every DLL that a file in the package imports must be in the package or
# part of Windows; a missing one would only show on the user's machine.
$System32 = Join-Path $env:WINDIR "System32"
$missing = @()
foreach ($file in Get-ChildItem $Out -Recurse -Include *.exe, *.dll) {
    $deps = dumpbin /nologo /dependents $file.FullName |
        Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] }
    foreach ($dep in $deps) {
        if ($dep -like "api-ms-win-*" -or $dep -like "ext-ms-*") { continue }
        if (Test-Path (Join-Path $Out $dep)) { continue }
        # the build machine has the VC++ runtime installed, users may not
        $vcRuntime = $dep -match '^(msvcp|vcruntime|concrt|vccorlib)'
        if (-not $vcRuntime -and (Test-Path (Join-Path $System32 $dep))) { continue }
        $missing += "$($file.Name) -> $dep"
    }
}
if ($missing) { throw "missing DLLs:`n$($missing -join "`n")" }

$Package = Join-Path $Dist "modjuke.zip"
if (Test-Path $Package) { Remove-Item $Package }
Compress-Archive -Path $Out -DestinationPath $Package
Write-Host "OK: $Package ($([math]::Round((Get-Item $Package).Length / 1MB, 1)) MB)"
