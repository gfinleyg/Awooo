<#
Builds AntWorkoutQt and puts everything it needs to run into dist\AntWorkout\ (dist\AntWorkout-Debug\ for Debug):
the exes, antnative.dll, Qt (windeployqt), the C++ runtime, and the fonts, sounds, images, themes and workouts.

  .\build_windows.ps1                       Release, with the tests
  .\build_windows.ps1 -Configuration Debug
  .\build_windows.ps1 -SkipTests

Runs from any PowerShell: the VS 2022 x64 build environment is loaded if cl isn't already on PATH. CMake and Ninja
are the ones that come with Qt (C:\Qt\Tools). ANT_LIB is built first if its .lib for the configuration is missing.

Nothing in dist\ is deleted: files there newer than the build's (settings, an edited theme or workout) are kept.
#>
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$QtRoot = "C:\Qt\6.7.0\msvc2019_64",
    [string]$QtTools = "C:\Qt\Tools",
    # The ANT SDK, beside the Awooo checkout by default (as for AntNative)
    [string]$ANTLibRoot = "..\..\antlib\ANT_LIB",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

function Resolve-AbsolutePath([string]$PathValue) {
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $PathValue))
}

function Invoke-Checked([string]$What, [scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)." }
}

# robocopy's exit codes 0-7 are success (what was copied), 8 and up are failures
function Copy-Tree([string]$From, [string]$To, [string[]]$Options) {
    & robocopy $From $To @Options /NJH /NJS /NDL /NP /NFL | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying $From to $To failed (robocopy exit $LASTEXITCODE)." }
    $global:LASTEXITCODE = 0
}

$ANTLibRoot = Resolve-AbsolutePath $ANTLibRoot
$cmake = Join-Path $QtTools "CMake_64\bin\cmake.exe"
$ctest = Join-Path $QtTools "CMake_64\bin\ctest.exe"
$ninja = Join-Path $QtTools "Ninja\ninja.exe"
$windeployqt = Join-Path $QtRoot "bin\windeployqt.exe"
foreach ($tool in @($cmake, $ninja, $windeployqt)) {
    if (-not (Test-Path $tool)) { throw "$tool was not found. Pass -QtRoot / -QtTools." }
}

# The VS 2022 x64 environment (cl, link, msbuild, and VCToolsRedistDir for the C++ runtime below)
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "Visual Studio 2022 with the C++ workload was not found." }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    # run vcvars64 in cmd and take the environment it leaves behind (it looks for vswhere on PATH itself; its
    # complaints go to stderr, which Windows PowerShell would otherwise turn into an error)
    $env:PATH = "$(Split-Path $vswhere -Parent);$env:PATH"
    foreach ($line in (& cmd.exe /c "`"$vcvars`" >nul 2>&1 && set")) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

Write-Host "Qt:      $QtRoot"
Write-Host "ANT SDK: $ANTLibRoot"

# ANT_LIB is a static library, built with msbuild (AntNative's build_windows.ps1 builds it the same way)
if (-not (Test-Path (Join-Path $ANTLibRoot "x64\$Configuration\ANT_LIB.lib"))) {
    Invoke-Checked "ANT_LIB build" {
        msbuild (Join-Path $ANTLibRoot "ANT_Lib.vcxproj") /m /p:Configuration=$Configuration /p:Platform=x64 /p:SolutionDir="$ANTLibRoot\"
    }
}

$build = Join-Path $PSScriptRoot "build\$Configuration"
Invoke-Checked "CMake configure" {
    & $cmake -S $PSScriptRoot -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_BUILD_TYPE=$Configuration" `
        "-DCMAKE_PREFIX_PATH=$QtRoot" "-DANT_LIB_ROOT=$ANTLibRoot"
}
Invoke-Checked "Build" { & $cmake --build $build }
if (-not $SkipTests) {
    Invoke-Checked "Tests" { & $ctest --test-dir $build --output-on-failure }
}

# dist\: the build's bin folder less the tests, then Qt and the C++ runtime beside the exes
$distName = if ($Configuration -eq "Debug") { "AntWorkout-Debug" } else { "AntWorkout" }
$dist = Join-Path $PSScriptRoot "dist\$distName"
Copy-Tree (Join-Path $build "bin") $dist @("/E", "/XO", "/XF", "tst_*.exe", "*.ilk")

$deployMode = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
# (no compiler runtime from windeployqt: Qt's would be older than AntNative needs, the right one is copied below;
# no network or shader compilers either, which the app never uses)
Invoke-Checked "windeployqt" {
    & $windeployqt $deployMode --no-compiler-runtime --no-translations --no-system-d3d-compiler --no-opengl-sw `
        --no-system-dxc-compiler --no-network --verbose 0 (Join-Path $dist "AntWorkout.exe")
}

# The C++ runtime the build was compiled against, next to the exes so it's the one loaded (anything older crashes
# AntNative, see tests\tst_ant_sticks.cpp). Debug builds use the debug runtime installed with Visual Studio.
if ($Configuration -eq "Release") {
    $crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
    if (-not $crt) { throw "The C++ runtime was not found under $env:VCToolsRedistDir." }
    Copy-Item (Join-Path $crt.FullName "msvcp140*.dll"), (Join-Path $crt.FullName "vcruntime140*.dll") $dist -Force
}

Write-Host "Built $dist"
