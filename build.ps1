# Builds bin\DarkPicturesRT.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# MSVC is required: ReShade is built with it, and MinGW returns API structs
# differently, which crashes inside ReShade.
# Usage: .\build.ps1 [-Install] [-Package] [-GameBin <folder of the game's shipping exe>]
#   -Package  makes bin\release\DarkPicturesRT-<version>.zip: the add-on, README, LICENSE and
#             THIRD_PARTY_NOTICES.txt (no patched shaders: those are made from the game's own)
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameBin = "E:\SteamLibrary\steamapps\common\The Dark Pictures Anthology - The Devil In Me\SMG028\Binaries\Win64"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$name = "DarkPicturesRT"
$addon = "$root\bin\$name.addon64"
$exes = "ManOfMedan-Win64-Shipping.exe", "LittleHope-Win64-Shipping.exe", "HouseOfAshes-Win64-Shipping.exe",
        "TheDevilInMe-Win64-Shipping.exe"  # matches kGames in src\probe.cpp

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    "rc /nologo /i `"$root\src`" /fo `"$obj\version.res`" `"$root\src\version.rc`"",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" " +
     "/Fo`"$obj\\`" `"$root\src\probe.cpp`" `"$obj\version.res`" " +
     "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\$name.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "add-on build failed" }
Write-Host "Built $addon ($name $((Get-Item $addon).VersionInfo.FileVersion))"

# tools\dxilasm.exe: assembles and signs patched DXIL with the Windows SDK's DXC.
$sdkInclude = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include\*\um\dxcapi.h" -ErrorAction SilentlyContinue |
    Sort-Object FullName | Select-Object -Last 1
if (-not $sdkInclude) { throw "dxcapi.h not found (Windows 10/11 SDK)." }
$toolCmd = @(
    "call `"$vcvars`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE " +
     "/I `"$($sdkInclude.DirectoryName)`" /Fo`"$obj\\`" `"$root\tools\dxilasm.cpp`" " +
     "/link /OUT:`"$root\bin\dxilasm.exe`" ole32.lib")
) -join " && "
cmd /c $toolCmd
if ($LASTEXITCODE) { throw "dxilasm build failed" }
Write-Host "Built $root\bin\dxilasm.exe"

# tools\d3d12probe.exe: prints D3D12Core's CreateStateObject prologue for the detour.
$probeCmd = @(
    "call `"$vcvars`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE " +
     "/Fo`"$obj\\`" `"$root\tools\d3d12probe.cpp`" /link /OUT:`"$root\bin\d3d12probe.exe`"")
) -join " && "
cmd /c $probeCmd
if ($LASTEXITCODE) { throw "d3d12probe build failed" }
Write-Host "Built $root\bin\d3d12probe.exe"

if ($Install) {
    $exe = $exes | Where-Object { Test-Path "$GameBin\$_" } | Select-Object -First 1
    if (-not $exe) { throw "None of $($exes -join ', ') found in $GameBin (pass -GameBin)" }
    if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon $GameBin -Force
    Write-Host "Installed $name to $GameBin"
}

if ($Package) {
    $version = (Get-Item $addon).VersionInfo.FileVersion  # DPRT_VERSION from src\version.h
    $stage = "$root\bin\release\$name-$version"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item $addon, "$root\README.md", "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.txt" $stage
    $zip = "$root\bin\release\$name-$version.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Write-Host "Packaged $zip"
    Get-ChildItem $stage -File | ForEach-Object { "{0,-32} {1,-10} {2,10:N0}" -f $_.Name, $_.VersionInfo.FileVersion, $_.Length }
}
