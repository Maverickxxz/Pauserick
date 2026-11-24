# Measures break reminders while they sit idle, as in the README comparison.
# Each app counts as its whole process tree (the process and every process it started,
# e.g. Electron's GPU and renderer processes), summed, sampled every few seconds.
# Nothing is opened or injected: the numbers come from WMI, as Task Manager shows them.
#
#   powershell -ExecutionPolicy Bypass -File tools\measure-idle.ps1 -Out idle.csv -Seconds 300 Pauserick=1234 Stretchly=5678
#
# Then take the median of each column, and the CPU used over the run (last cpu_ms - first).
# wsprivate_kb is the "Memory" column of Task Manager (private working set), private_kb is
# private bytes (commit), ws_kb the full working set including shared pages.
param(
    [Parameter(Mandatory)] [string]$Out,
    [int]$Seconds = 300,
    [int]$Interval = 5,
    [Parameter(Mandatory, ValueFromRemainingArguments)] [string[]]$Apps  # name=pid of the main process
)

function Get-Tree([uint32]$root, $all) {
    $byId = @{}; foreach ($p in $all) { $byId[[uint32]$p.ProcessId] = $p }
    $set = @{ $root = $true }
    do {
        $added = $false
        foreach ($p in $all) {
            $id = [uint32]$p.ProcessId; $parent = $byId[[uint32]$p.ParentProcessId]
            # a parent id can be reused: the child must be younger than its parent
            if (-not $set.ContainsKey($id) -and $parent -and $set.ContainsKey([uint32]$parent.ProcessId) -and
                $p.CreationDate -ge $parent.CreationDate) { $set[$id] = $true; $added = $true }
        }
    } while ($added)
    $all | Where-Object { $set.ContainsKey([uint32]$_.ProcessId) }
}

$roots = [ordered]@{}
foreach ($a in $Apps) { $k, $v = $a.Split('='); $roots[$k] = [uint32]$v }
"time,app,procs,threads,ws_kb,wsprivate_kb,private_kb,cpu_ms" | Set-Content -Encoding ascii $Out
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    $all = Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, CreationDate, WorkingSetSize, PrivatePageCount, ThreadCount, KernelModeTime, UserModeTime
    $wsPrivate = @{}
    Get-CimInstance Win32_PerfRawData_PerfProc_Process -Property IDProcess, WorkingSetPrivate |
        ForEach-Object { $wsPrivate[[uint32]$_.IDProcess] = [double]$_.WorkingSetPrivate }
    foreach ($name in $roots.Keys) {
        $tree = @(Get-Tree $roots[$name] $all)
        if (-not $tree.Count) { Write-Warning "$name is not running"; continue }
        $sum = { param($prop) ($tree | Measure-Object $prop -Sum).Sum }
        $wsp = 0; foreach ($p in $tree) { $wsp += $wsPrivate[[uint32]$p.ProcessId] }
        $cpuMs = ((& $sum KernelModeTime) + (& $sum UserModeTime)) / 10000
        "{0},{1},{2},{3},{4},{5},{6},{7}" -f (Get-Date -Format 'HH:mm:ss'), $name, $tree.Count, (& $sum ThreadCount),
            [int]((& $sum WorkingSetSize) / 1KB), [int]($wsp / 1KB), [int]((& $sum PrivatePageCount) / 1KB), [int64]$cpuMs |
            Add-Content -Encoding ascii $Out
    }
    Start-Sleep $Interval
}
