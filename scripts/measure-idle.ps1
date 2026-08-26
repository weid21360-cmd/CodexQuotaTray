param(
    [string]$ProcessName = 'CodexQuotaTray',
    [ValidateRange(30, 86400)]
    [int]$DurationSeconds = 600,
    [ValidateRange(1, 60)]
    [int]$SampleSeconds = 5
)

$ErrorActionPreference = 'Stop'
$process = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$logicalProcessors = [Environment]::ProcessorCount
$samples = [System.Collections.Generic.List[object]]::new()
$previousCpu = $process.CPU
$previousTime = Get-Date
$deadline = $previousTime.AddSeconds($DurationSeconds)

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $SampleSeconds
    $process.Refresh()
    if ($process.HasExited) { throw "$ProcessName exited during measurement." }
    $now = Get-Date
    $cpuSeconds = $process.CPU
    $elapsed = ($now - $previousTime).TotalSeconds
    $cpuPercent = (($cpuSeconds - $previousCpu) / $elapsed / $logicalProcessors) * 100
    $samples.Add([PSCustomObject]@{
        Time = $now
        CpuPercent = [math]::Max(0, $cpuPercent)
        PrivateMB = $process.PrivateMemorySize64 / 1MB
        WorkingSetMB = $process.WorkingSet64 / 1MB
        Handles = $process.HandleCount
    })
    $previousCpu = $cpuSeconds
    $previousTime = $now
}

$cpu = $samples | Measure-Object CpuPercent -Average -Maximum
$private = $samples | Measure-Object PrivateMB -Average -Maximum
$working = $samples | Measure-Object WorkingSetMB -Average -Maximum
$handles = $samples | Measure-Object Handles -Minimum -Maximum

[PSCustomObject]@{
    Samples = $samples.Count
    AverageCpuPercent = [math]::Round($cpu.Average, 3)
    MaximumCpuPercent = [math]::Round($cpu.Maximum, 3)
    AveragePrivateMB = [math]::Round($private.Average, 2)
    MaximumPrivateMB = [math]::Round($private.Maximum, 2)
    AverageWorkingSetMB = [math]::Round($working.Average, 2)
    MaximumWorkingSetMB = [math]::Round($working.Maximum, 2)
    MinimumHandles = $handles.Minimum
    MaximumHandles = $handles.Maximum
    HandleGrowth = $handles.Maximum - $handles.Minimum
}

