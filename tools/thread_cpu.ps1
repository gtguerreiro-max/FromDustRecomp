# Samples per-thread CPU usage of a running FromDust.exe.
# Usage: .\tools\thread_cpu.ps1 [-Interval 5]
param([int]$Interval = 5)
$p = Get-Process FromDust -ErrorAction Stop
$before = @{}
foreach ($t in $p.Threads) { try { $before[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep -Seconds $Interval
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
    try {
        $ms = $t.TotalProcessorTime.TotalMilliseconds - ($before[$t.Id] | ForEach-Object { $_ })
        [pscustomobject]@{ Tid = $t.Id; CpuPct = [math]::Round(100 * $ms / ($Interval * 1000), 1) }
    } catch {}
}
$rows | Sort-Object CpuPct -Descending | Select-Object -First 12
"total: $([math]::Round(($rows | Measure-Object CpuPct -Sum).Sum, 1))% of one core"
