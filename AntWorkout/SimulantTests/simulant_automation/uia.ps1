# UI Automation helpers for driving SimulANT+ (dot-source this): finds SimulANT+'s controls by their automation
# IDs and clicks/selects them, so the hardware tests can run without anyone at the SimulANT+ window. See README.md.
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

function Get-SimWindows {
    $proc = Get-Process -Name "SimulANT+" -ErrorAction Stop | Select-Object -First 1
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)
    return $AE::RootElement.FindAll($TS::Children, $cond)
}

function Find-El($root, [string]$id = $null, [string]$name = $null, [string]$class = $null, [int]$timeout = 5) {
    $deadline = (Get-Date).AddSeconds($timeout)
    do {
        $conds = @()
        if ($id) { $conds += New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id) }
        if ($name) { $conds += New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $name) }
        if ($class) { $conds += New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, $class) }
        $cond = if ($conds.Count -eq 1) { $conds[0] } else { New-Object System.Windows.Automation.AndCondition($conds) }
        $roots = if ($root) { @($root) } else { @(Get-SimWindows) }
        foreach ($r in $roots) {
            $el = $r.FindFirst($TS::Descendants, $cond)
            if ($el) { return $el }
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    throw "Not found: id=$id name=$name class=$class"
}

function Invoke-El($el) {
    $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Select-El($el) {
    $el.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
}

function Expand-El($el) {
    $el.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
}

function Dump-El($el, [int]$depth = 0, [int]$max = 14) {
    if ($depth -gt $max) { return }
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $c = $el.Current
    $type = $c.ControlType.ProgrammaticName -replace "ControlType\.", ""
    $line = ("  " * $depth) + "$type '$($c.Name)' id=$($c.AutomationId) class=$($c.ClassName)"
    if (-not $c.IsEnabled) { $line += " [disabled]" }
    Write-Output $line
    $child = $walker.GetFirstChild($el)
    while ($null -ne $child) {
        Dump-El $child ($depth + 1) $max
        $child = $walker.GetNextSibling($child)
    }
}

function Dump-Sim { foreach ($w in Get-SimWindows) { Dump-El $w } }

function Get-SimTexts {
    # the simulator entries' names in the device tree, e.g. "Heart Rate Sensor (Ch1)"
    $tree = Find-El -id "listDevices"
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, "textName")
    return @($tree.FindAll($TS::Descendants, $cond) | Where-Object { $_.Current.Name -match "\(Ch\d+\)$" })
}

function Select-Sim([string]$simProfile) {
    $text = Get-SimTexts | Where-Object { $_.Current.Name -like "$simProfile (Ch*)" } | Select-Object -Last 1
    if (-not $text) { throw "No simulator '$simProfile'" }
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $item = $walker.GetParent($walker.GetParent($text))
    Select-El $item
    Start-Sleep -Milliseconds 800
    return $text.Current.Name
}

function Get-Events([int]$last = 8) {
    $events = Find-El -id "textWindow"
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $names = @(); $c = $walker.GetFirstChild($events); while ($c) { $names += $c.Current.Name; $c = $walker.GetNextSibling($c) }
    return $names | Select-Object -Last $last
}

function Run-SimScript([string]$simProfile, [string]$folder, [string]$script) {
    # select the simulator, pick its script by full path (only that profile's scripts are listed), Run
    $name = Select-Sim $simProfile
    $path = "$env:USERPROFILE\Documents\SimulANT+\Scripts\$folder\$script"
    $combo = Find-El -id "comboScripts"
    Expand-El $combo
    Start-Sleep -Milliseconds 600
    Select-El (Find-El -root $combo -name $path -class "ListBoxItem" -timeout 3)
    Start-Sleep -Milliseconds 300
    $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse()
    $selected = $combo.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern).Current.GetSelection()[0].Current.Name
    if ($selected -ne $path) { throw "Script selection is '$selected', not $path" }
    Invoke-El (Find-El -id "buttonRun")
    Start-Sleep -Seconds 2
    return "$name running $script"
}

function Add-Sim([string]$simProfile) {
    Invoke-El (Find-El -id "buttonAddSimulator")
    $combo = Find-El -id "comboProfile"
    Expand-El $combo
    Start-Sleep -Milliseconds 600
    Select-El (Find-El -root $combo -name $simProfile -class "ListBoxItem")
    Start-Sleep -Milliseconds 300
    Invoke-El (Find-El -id "buttonOK")
    Start-Sleep -Seconds 2
}

function Stop-SimScript([string]$simProfile) {
    Select-Sim $simProfile | Out-Null
    $stop = Find-El -id "buttonStop"
    if ($stop.Current.IsEnabled) { Invoke-El $stop; Start-Sleep -Seconds 1 }
}

function TurnOff-Sim([string]$simProfile) {
    # the simulator's own TurnOff method (its Properties and Methods panel): it stops transmitting
    Select-Sim $simProfile | Out-Null
    $panel = Find-El -class "PropertiesAndMethodsPanel"
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, "TurnOff")),
        (New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, "Button")))
    Invoke-El ($panel.FindFirst($TS::Descendants, $cond))
    Start-Sleep -Milliseconds 500
}

function Test-SimScriptRunning([string]$simProfile) {
    Select-Sim $simProfile | Out-Null
    return (Find-El -id "buttonStop").Current.IsEnabled
}

function Close-Sim {
    $win = Get-SimWindows | Where-Object { $_.Current.Name -eq "SimulANT+" } | Select-Object -First 1
    if ($win) { $win.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close() }
}
