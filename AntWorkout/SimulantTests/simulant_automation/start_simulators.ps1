# Sets SimulANT+ up for a SimulANT+ test scenario: starts SimulANT+ if it isn't running, adds the scenario's
# simulators and runs each one's awo_*.py script - what README.md's step 3 does by hand. Start the harness
# (run_simulant_test.py / run_app_test.py) first: it takes the first free stick and installs the scripts.
#
#   .\start_simulators.ps1 trainer
#   .\start_simulators.ps1 sensors
#   .\start_simulators.ps1 -Stop      stops every script and closes SimulANT+
param(
    [ValidateSet("trainer", "sensors")]
    [string]$Scenario,
    [switch]$Stop,
    [string]$SimulantExe = "$env:USERPROFILE\Documents\SimulANT+\SimulANT+.exe"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "uia.ps1")

# SimulANT+ profile name, its folder under Documents\SimulANT+\Scripts, and the test's script
$Simulators = @{
    trainer = @("Bike Trainer (FE-C)", "TrainerControlDevice", "awo_trainer.py")
    heartrate = @("Heart Rate Sensor", "HeartRateSensor", "awo_heartrate.py")
    power = @("Bike Power Sensor (Power-Only)", "BikePowerOnlySensor", "awo_power.py")
    speedcadence = @("Bike Speed & Cadence Sensor", "BikeSpeedCadenceSensor", "awo_speedcadence.py")
}
# Matches the scenarios' simulator table in ..\README.md
$Scenarios = @{
    trainer = @("trainer", "heartrate")
    sensors = @("power", "speedcadence", "heartrate")
}

if ($Stop) {
    if (Get-Process -Name "SimulANT+" -ErrorAction SilentlyContinue) {
        foreach ($sim in Get-SimTexts) {
            Stop-SimScript ($sim.Current.Name -replace " \(Ch\d+\)$", "")
        }
        Close-Sim
        Start-Sleep -Seconds 3
    }
    if (Get-Process -Name "SimulANT+" -ErrorAction SilentlyContinue) { throw "SimulANT+ is still running" }
    "SimulANT+ closed"
    return
}
if (-not $Scenario) { throw "Pass a scenario (trainer or sensors), or -Stop" }

if (-not (Get-Process -Name "SimulANT+" -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $SimulantExe -WorkingDirectory (Split-Path $SimulantExe)
    # its main window, with the free stick's device listed
    Find-El -id "buttonAddSimulator" -timeout 60 | Out-Null
    Start-Sleep -Seconds 3
}

$existing = @(Get-SimTexts | ForEach-Object { $_.Current.Name -replace " \(Ch\d+\)$", "" })
foreach ($key in $Scenarios[$Scenario]) {
    $simProfile, $folder, $script = $Simulators[$key]
    if ($existing -notcontains $simProfile) {
        Add-Sim $simProfile
    } elseif (Test-SimScriptRunning $simProfile) {
        # left running: it follows each new run by itself (the harness's control.txt), and restarting it mid-run
        # would make the device go quiet
        "$simProfile already running its script"
        continue
    } else {
        # added but stopped: pick up the scripts the harness just installed
        Invoke-El (Find-El -id "buttonReload")
        Start-Sleep -Seconds 3
    }
    Run-SimScript $simProfile $folder $script | Out-Null
    if (-not (Test-SimScriptRunning $simProfile)) { throw "$simProfile's script isn't running: $(Get-Events 4)" }
    "$simProfile running $script"
}
