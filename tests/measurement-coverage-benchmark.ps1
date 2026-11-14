param(
    [Parameter(Mandatory)] [ValidateSet('Backend','Runtime')] [string]$Mode,
    [Parameter(Mandatory)] [string]$Candidate,
    [string]$Baseline,
    [Parameter(Mandatory)] [string]$Probe,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [ValidateRange(2,30)] [int]$Processes = 6,
    [ValidateRange(2,16)] [int]$Rounds = 4,
    [ValidateRange(1000,10000)] [int]$CpuMilliseconds = 1000
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Mode -eq 'Backend' -and -not $Baseline) { throw 'Backend mode requires an immutable baseline DLL.' }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new benchmark output directory.' }
$Candidate = (Resolve-Path -LiteralPath $Candidate).Path
$Probe = (Resolve-Path -LiteralPath $Probe).Path
if ($Baseline) { $Baseline = (Resolve-Path -LiteralPath $Baseline).Path }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$utf8 = [Text.UTF8Encoding]::new($false)
$binaries = @(@($Candidate,$Baseline,$Probe) | Where-Object { $_ } | Sort-Object -Unique | ForEach-Object {
    $item = Get-Item -LiteralPath $_
    [ordered]@{ path=$item.FullName; sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash; size=$item.Length }
})
$rows = [Collections.Generic.List[object]]::new()
$started = [DateTime]::UtcNow.ToString('o')

function Assert-Binaries {
    foreach ($binary in $binaries) {
        if ((Get-FileHash -LiteralPath $binary.path -Algorithm SHA256).Hash -cne $binary.sha256) {
            throw "Binary changed during benchmark: $($binary.path)"
        }
    }
}

function Invoke-Probe([string]$Name, [string[]]$Arguments) {
    Assert-Binaries
    $directory = Join-Path $OutputDirectory $Name
    New-Item -ItemType Directory -Path $directory | Out-Null
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Probe
    foreach ($argument in $Arguments) { [void]$start.ArgumentList.Add($argument) }
    $start.WorkingDirectory = $directory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment['DXVK_CONFIG'] = 'd3d9.gtaSaCompat = True; d3d9.gtaSaCompatDiagnostics = False; dxvk.hud = 0'
    $start.Environment['DXVK_HUD'] = '0'
    $start.Environment['DXVK_LOG_PATH'] = $directory
    [void]$start.Environment.Remove('DXVK_CONFIG_FILE')
    [void]$start.Environment.Remove('DXVK_STATE_CACHE_PATH')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $limit = [Math]::Max(120000, $Rounds * ($CpuMilliseconds + 1500) * 4)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $process.WaitForExit(1000)) {
        if ($timer.ElapsedMilliseconds -gt $limit) {
            $process.Kill($true)
            $process.WaitForExit()
            [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'), $stdout.GetAwaiter().GetResult(), $utf8)
            [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'), $stderr.GetAwaiter().GetResult(), $utf8)
            throw "$Name timed out; evidence retained in $directory"
        }
    }
    $text = $stdout.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'), $text, $utf8)
    [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'), $stderr.GetAwaiter().GetResult(), $utf8)
    $exitCode = $process.ExitCode
    $process.Dispose()
    if ($exitCode -ne 0) { throw "$Name failed with exit $exitCode; see $directory" }
    Assert-Binaries
    @($text -split '\r?\n' | Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json })
}

function Add-Row($Row) {
    $rows.Add($Row)
    [IO.File]::AppendAllText((Join-Path $OutputDirectory 'samples.jsonl'),
        (($Row | ConvertTo-Json -Depth 8 -Compress) + "`n"), $utf8)
}

$metadata = [ordered]@{
    mode=$Mode; startedUtc=$started; binaries=$binaries; processes=$Processes; rounds=$Rounds
    cpuMilliseconds=$CpuMilliseconds; scheduling='unmodified OS scheduling; runs sequentially'
    ordering='AB/BA alternates; A/A control uses identical disabled collection'
    interpretation='Actual Vulkan fixture overhead only; not a claim about game FPS'
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'metadata.json'), ($metadata | ConvertTo-Json -Depth 8), $utf8)

if ($Mode -eq 'Backend') {
    for ($pair=0; $pair -lt $Processes; ++$pair) {
        # Rotate suite order as well as binary order to reduce warm-up and thermal bias.
        $suites = @('inactive','active','aa')
        for ($slot=0; $slot -lt $suites.Count; ++$slot) {
            $suite = $suites[($slot+$pair)%$suites.Count]
            $order = if ($pair%2) { @('B','A') } else { @('A','B') }
            foreach ($variant in $order) {
                $dll = if ($suite -eq 'inactive' -and $variant -eq 'A') { $Baseline } else { $Candidate }
                $collection = if ($suite -eq 'active' -and $variant -eq 'B') { 'on' } elseif ($suite -eq 'aa' -and $variant -eq 'B') { 'aa' } else { 'off' }
                $name = '{0}-{1:D2}-{2}' -f $suite,$pair,$variant
                $parsed = @(Invoke-Probe $name @($dll,'--benchmark',$collection))
                if ($parsed.Count -ne 1) { throw "$name expected one benchmark row, found $($parsed.Count)." }
                $row = $parsed[0]
                foreach ($field in @('wall_ns_per_iteration','thread_cycles_per_iteration')) {
                    if (-not $row.PSObject.Properties[$field] -or -not ([double]$row.$field -gt 0) -or
                        [double]::IsInfinity([double]$row.$field)) { throw "$name invalid $field" }
                }
                if ($row.failures -ne 0 -or $row.iterations -lt 1 -or $row.successful_presents -ne $row.iterations) {
                    throw "$name failed real workload or successful-Present checks."
                }
                if ($row.dropped -ne 0 -or $row.inflight -ne 0 -or
                    ($collection -eq 'on' -and ($row.supported -ne 1 -or $row.events -le 0))) {
                    throw "$name incomplete telemetry evidence."
                }
                $row | Add-Member -NotePropertyName suite -NotePropertyValue $suite
                $row | Add-Member -NotePropertyName pair -NotePropertyValue $pair
                $row | Add-Member -NotePropertyName label -NotePropertyValue $variant
                $row | Add-Member -NotePropertyName order -NotePropertyValue ($order -join '')
                Add-Row $row
                Write-Output "$name complete"
            }
        }
    }
} else {
    for ($process=0; $process -lt $Processes; ++$process) {
        $suites = if ($process%2) { @('aa','active') } else { @('active','aa') }
        foreach ($suite in $suites) {
            $arguments = @('--coverage-benchmark',$Candidate,[string]$Rounds,[string]$CpuMilliseconds)
            if ($suite -eq 'aa') { $arguments += '--aa' }
            $name = '{0}-{1:D2}' -f $suite,$process
            $parsed = @(Invoke-Probe $name $arguments | Where-Object type -eq 'coverage-benchmark')
            if ($parsed.Count -ne $Rounds*2*3) { throw "$name expected $($Rounds*6) phase rows, found $($parsed.Count)." }
            $seen = [Collections.Generic.HashSet[string]]::new()
            foreach ($row in $parsed) {
                # The fixture truthfully labels both A/A configurations A.
                # Keep that value, and use a separate comparison label by position.
                $row | Add-Member -NotePropertyName configuredVariant -NotePropertyValue $row.variant
                if ($suite -eq 'aa') {
                    if ($row.order -cne 'AA' -or $row.variant -cne 'A' -or $row.position -notin @(0,1) -or $row.collectionEnabled) {
                        throw "$name invalid A/A configuration"
                    }
                    $row.variant = if ($row.position -eq 0) { 'A' } else { 'B' }
                } elseif ([bool]$row.collectionEnabled -ne ($row.variant -eq 'B')) {
                    throw "$name collection configuration differs from its comparison label"
                }
                $key = '{0}/{1}/{2}' -f $row.round,$row.variant,$row.phase
                if (-not $seen.Add($key) -or $row.round -lt 0 -or $row.round -ge $Rounds -or
                    $row.variant -notin @('A','B') -or $row.phase -notin @('baseline','cpu','attribution') -or
                    -not ([double]$row.elapsedNs -gt 0) -or -not ([double]$row.presents -gt 0)) {
                    throw "$name invalid or repeated phase row $key"
                }
                if (-not $row.valid -or $row.failedPresents -ne 0 -or -not $row.cpuTimeAvailable) {
                    throw "$name incomplete real-workload or thread-CPU evidence: $key"
                }
                foreach ($field in @('workPresentTotalNs','presentTotalNs','maxWorkPresentNs','maxPresentNs',
                    'completionPresentNs','completionWorkNs','cpuFinishingNs','cpuFinishingObserved','reportWaitNs','reportWasPending')) {
                    if (-not $row.PSObject.Properties[$field]) { throw "$name missing capture-tail field: $field" }
                }
                if ($row.iterations -le 0 -or $row.workPresentTotalNs -le 0 -or $row.presentTotalNs -le 0 -or
                    $row.mainSamples -le 0 -or $row.mainDropped -ne 0 -or $row.csDropped -ne 0 -or
                    $row.dropped -ne 0 -or $row.frameTraceDropped -ne 0) { throw "$name incomplete capture counts: $key" }
                if ($row.collectionEnabled -and ($row.csSamples -le 0 -or $row.events -le 0 -or $row.memorySnapshots -lt 2)) {
                    throw "$name enabled coverage did not reach the report: $key"
                }
                if (-not $row.collectionEnabled -and ($row.csSamples -ne 0 -or $row.events -ne 0 -or $row.memorySnapshots -ne 0)) {
                    throw "$name disabled control collected extra coverage: $key"
                }
                $row | Add-Member -NotePropertyName suite -NotePropertyValue $suite
                $row | Add-Member -NotePropertyName processIndex -NotePropertyValue $process
                Add-Row $row
            }
            Write-Output "$name complete: $($parsed.Count) phase rows"
        }
    }
}
Assert-Binaries
$metadata.endedUtc = [DateTime]::UtcNow.ToString('o')
$metadata.rows = $rows.Count
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'metadata.json'), ($metadata | ConvertTo-Json -Depth 8), $utf8)
& (Join-Path $PSScriptRoot 'measurement-coverage-summary.ps1') -InputDirectory $OutputDirectory
Write-Output "Benchmark artifacts: $OutputDirectory"
