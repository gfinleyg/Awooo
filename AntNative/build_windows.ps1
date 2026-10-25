param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    # The ANT SDK, beside the Awooo checkout by default
    [string]$ANTLibRoot = "..\..\antlib\ANT_LIB",
    # One or more Python installs to build for (each gets its own ant_native.cpXYZ-win_amd64.pyd).
    # Default: every Python 3.10 or newer the py launcher knows about.
    [string[]]$PythonHome = @(),
    # pybind11 is header only, so one copy serves every Python. Default: the
    # first of the PythonHome installs that has it (pip install pybind11).
    [string]$Pybind11Dir = "",
    # Where AntWorkout lives. Every build copies fresh ant_native.*.pyd files
    # and the ant/ python package into <AntWorkoutRoot>\AntNative\, so
    # AntWorkout never needs this checkout present to run. Defaults to
    # the sibling folder layout (..\AntNative and ..\AntWorkout under
    # the same parent).
    [string]$AntWorkoutRoot = "..\AntWorkout"
)

$ErrorActionPreference = "Stop"

function Resolve-AbsolutePath([string]$PathValue) {
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $PathValue))
}

$ANTLibRoot = Resolve-AbsolutePath $ANTLibRoot
$AntWorkoutRoot = Resolve-AbsolutePath $AntWorkoutRoot
$project = Join-Path $PSScriptRoot "ant_native.vcxproj"

if (-not (Test-Path (Join-Path $ANTLibRoot "ANT_Lib.vcxproj"))) {
    throw "ANT_Lib.vcxproj was not found under '$ANTLibRoot'. Pass -ANTLibRoot 'C:\path\to\ANT_LIB'."
}
if ($PythonHome.Count -eq 0) {
    # py -0p lines look like " -V:3.12 *        C:\...\python.exe"
    $PythonHome = @(& py -0p | ForEach-Object {
        if ($_ -match '-V:3\.(\d+)\s+\*?\s*(\S.*python\.exe)\s*$' -and [int]$Matches[1] -ge 10) {
            Split-Path $Matches[2] -Parent
        }
    })
    if ($PythonHome.Count -eq 0) { throw "No Python 3.10+ found by 'py -0p'. Pass -PythonHome 'C:\path\to\Python'." }
}
$pythons = foreach ($pythonDir in $PythonHome) {
    $exe = Join-Path $pythonDir "python.exe"
    if (-not (Test-Path $exe)) {
        throw "Python was not found under '$pythonDir'. Pass -PythonHome 'C:\path\to\Python'."
    }
    # "312" for Python 3.12: the pythonXYZ.lib to link and the cpXYZ in the module's name
    $version = & $exe -c "import sys; print(f'{sys.version_info[0]}{sys.version_info[1]}')"
    [pscustomobject]@{ Home = $pythonDir; Exe = $exe; Version = $version }
}
if ([string]::IsNullOrWhiteSpace($Pybind11Dir)) {
    foreach ($python in $pythons) {
        #prints nothing (rather than failing, which Stop would turn into an exception) without pybind11
        $include = & $python.Exe -c "import importlib.util; print(__import__('pybind11').get_include() if importlib.util.find_spec('pybind11') else '')"
        if ($include) {
            # get_include() returns the directory that directly contains pybind11\pybind11.h.
            # The vcxproj expects the pybind11 package root, so remove the final \include.
            $Pybind11Dir = Split-Path $include -Parent
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($Pybind11Dir)) { throw "pybind11 is not installed in any of: $($PythonHome -join ', ')." }
}
$Pybind11Dir = (Resolve-Path $Pybind11Dir).Path

Write-Host "ANT SDK: $ANTLibRoot"
foreach ($python in $pythons) { Write-Host "Python:  $($python.Home) (cp$($python.Version))" }
Write-Host "pybind11: $Pybind11Dir"
Write-Host "AntWorkout: $AntWorkoutRoot"
if (-not (Test-Path $AntWorkoutRoot)) {
    Write-Host "  (does not exist yet - the vendored copy will be created there anyway)"
}

# ANT_LIB is a static library. Build it first so its Release/Debug .lib exists
# at the location used by the wrapper linker.
& msbuild (Join-Path $ANTLibRoot "ANT_Lib.vcxproj") /m /p:Configuration=$Configuration /p:Platform=x64 /p:SolutionDir="$ANTLibRoot\"
if ($LASTEXITCODE -ne 0) { throw "ANT_LIB build failed." }

# antnative.dll, the C/C++ library (include\antnative.h, antnative.hpp): one build serves every program
& msbuild (Join-Path $PSScriptRoot "antnative.vcxproj") /m /p:Configuration=$Configuration /p:Platform=x64 /p:ANTLibRoot="$ANTLibRoot"
if ($LASTEXITCODE -ne 0) { throw "antnative.dll build failed." }

foreach ($python in $pythons) {
    & msbuild $project /m /p:Configuration=$Configuration /p:Platform=x64 /p:ANTLibRoot="$ANTLibRoot" /p:PythonHome="$($python.Home)" /p:PythonVersion=$($python.Version) /p:Pybind11Dir="$Pybind11Dir" /p:AntWorkoutRoot="$AntWorkoutRoot"
    if ($LASTEXITCODE -ne 0) { throw "AntNative build failed for Python $($python.Version)." }
}

Write-Host "Build complete."
