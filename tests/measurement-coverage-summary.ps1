param([Parameter(Mandatory)] [string]$InputDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$directory = (Resolve-Path -LiteralPath $InputDirectory).Path
$metadata = Get-Content -LiteralPath (Join-Path $directory 'metadata.json') -Raw | ConvertFrom-Json
$rows = @(Get-Content -LiteralPath (Join-Path $directory 'samples.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
$utf8 = [Text.UTF8Encoding]::new($false)
$paired = [Collections.Generic.List[object]]::new()
$reports = [Collections.Generic.Dictionary[string,object]]::new()
function Median([object[]]$Values) {
    $sorted = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ } | Sort-Object)
    if (-not $sorted.Count) { return $null }
    $mid = [int][Math]::Floor($sorted.Count/2)
    if ($sorted.Count%2) { return $sorted[$mid] }
    return ($sorted[$mid-1]+$sorted[$mid])/2
}
function Read-SamplerWindow($Row) {
    if ($reports.ContainsKey($Row.reportPath)) { return $reports[$Row.reportPath] }
    $found = $null
    $reader = [IO.File]::OpenText($Row.reportPath)
    try { while ($null -ne ($line = $reader.ReadLine())) {
        if (-not $line.StartsWith('cpu captureId=')) { continue }
        $presents = [regex]::Match($line, '\bsampledPresents=(\d+)\b')
        $elapsed = [regex]::Match($line, '\bsamplingElapsedNs=(\d+)\b')
        $samples = [regex]::Match($line, '\bsamples=(\d+)\b')
        if (-not $presents.Success -or -not $elapsed.Success -or -not $samples.Success) {
            throw "Missing sampler-window fields: $($Row.reportPath)"
        }
        if ([uint64]$samples.Groups[1].Value -ne [uint64]$Row.mainSamples) { throw 'Report sample count differs from benchmark row.' }
        $found = [pscustomobject]@{ path=$Row.reportPath;
            sha256=(Get-FileHash -LiteralPath $Row.reportPath -Algorithm SHA256).Hash;
            presents=[uint64]$presents.Groups[1].Value; elapsedNs=[uint64]$elapsed.Groups[1].Value }
        break
    } } finally { $reader.Dispose() }
    if (-not $found -or $found.presents -eq 0 -or $found.elapsedNs -eq 0) {
        throw "No valid sampler-window evidence: $($Row.reportPath)"
    }
    $reports.Add($Row.reportPath,$found)
    $found
}
function Add-Pair($A,$B,[string]$Metric,[double]$Av,[double]$Bv,[string]$Phase='',
    [bool]$AObserved=$true,[bool]$BObserved=$true) {
    if ($Av -lt 0 -or $Bv -lt 0 -or [double]::IsNaN($Av) -or [double]::IsNaN($Bv) -or
        [double]::IsInfinity($Av) -or [double]::IsInfinity($Bv)) { throw "Invalid $Metric value" }
    $relative = if ($AObserved -and $BObserved -and $Av -gt 0) { 100*($Bv/$Av-1) } else { $null }
    $process = if ($metadata.mode -eq 'Backend') { $A.pair } else { $A.processIndex }
    $round = if ($metadata.mode -eq 'Backend') { 0 } else { $A.round }
    $paired.Add([pscustomobject]@{ suite=$A.suite; process=$process; round=$round; phase=$Phase;
        metric=$Metric; a=$(if ($AObserved) { $Av } else { $null }); b=$(if ($BObserved) { $Bv } else { $null });
        delta=$(if ($AObserved -and $BObserved) { $Bv-$Av } else { $null }); changePercent=$relative;
        observedA=$AObserved; observedB=$BObserved;
        zeroA=($AObserved -and $Av -eq 0); zeroB=($BObserved -and $Bv -eq 0) })
}
if ($metadata.mode -eq 'Backend') {
    foreach ($group in ($rows | Group-Object suite,pair)) {
        $a = @($group.Group | Where-Object label -eq 'A'); $b = @($group.Group | Where-Object label -eq 'B')
        if ($a.Count -ne 1 -or $b.Count -ne 1 -or $a[0].iterations -ne $b[0].iterations) { throw "Unpaired backend rows: $($group.Name)" }
        foreach ($metric in @('wall_ns_per_iteration','thread_cycles_per_iteration')) {
            Add-Pair $a[0] $b[0] $metric ([double]$a[0].$metric) ([double]$b[0].$metric)
        }
    }
} elseif ($metadata.mode -eq 'Runtime') {
    foreach ($group in ($rows | Group-Object suite,processIndex,round,phase)) {
        $a = @($group.Group | Where-Object variant -eq 'A'); $b = @($group.Group | Where-Object variant -eq 'B')
        if ($a.Count -ne 1 -or $b.Count -ne 1) { throw "Unpaired runtime rows: $($group.Name)" }
        foreach ($metric in @('elapsedNs','mainCpuNs','csCpuNs')) {
            Add-Pair $a[0] $b[0] ($metric+'PerPresent') ([double]$a[0].$metric/[double]$a[0].presents) `
                ([double]$b[0].$metric/[double]$b[0].presents) $a[0].phase
        }
        if ($a[0].phase -eq 'cpu') {
            $aw = Read-SamplerWindow $a[0]; $bw = Read-SamplerWindow $b[0]
            Add-Pair $a[0] $b[0] 'mainCyclesPerSampledPresent' `
                ([double]$a[0].mainThreadCycles/[double]$aw.presents) `
                ([double]$b[0].mainThreadCycles/[double]$bw.presents) 'cpu'
            foreach ($metric in @('maxWorkPresentNs','maxPresentNs','completionPresentNs','completionWorkNs','cpuFinishingNs','reportWaitNs')) {
                if ($a[0].PSObject.Properties[$metric] -and $b[0].PSObject.Properties[$metric]) {
                    $observedA = $metric -ne 'cpuFinishingNs' -or $a[0].cpuFinishingObserved
                    $observedB = $metric -ne 'cpuFinishingNs' -or $b[0].cpuFinishingObserved
                    Add-Pair $a[0] $b[0] $metric ([double]$a[0].$metric) ([double]$b[0].$metric) 'capture' $observedA $observedB
                }
            }
            foreach ($metric in @('workPresentTotalNs','presentTotalNs')) {
                if ($a[0].PSObject.Properties[$metric] -and $b[0].PSObject.Properties[$metric]) {
                    if ($a[0].iterations -le 0 -or $b[0].iterations -le 0) { throw 'Missing timed work/Present count.' }
                    Add-Pair $a[0] $b[0] ($metric+'PerIteration') `
                        ([double]$a[0].$metric/[double]$a[0].iterations) `
                        ([double]$b[0].$metric/[double]$b[0].iterations) 'capture'
                }
            }
        }
    }
} else { throw "Unknown mode: $($metadata.mode)" }
$summary = @(
    foreach ($group in ($paired | Group-Object suite,phase,metric)) {
        $changes = @($group.Group | Group-Object process | ForEach-Object {
            $valid = @($_.Group | Where-Object { $null -ne $_.changePercent })
            [pscustomobject]@{ process=$_.Name; pairs=$_.Count; relativePairs=$valid.Count;
                deltaMedian=(Median @($_.Group | ForEach-Object delta));
                changePercent=(Median @($valid | ForEach-Object changePercent)) }
        })
        $allRelative = @($group.Group | Where-Object { $null -eq $_.changePercent }).Count -eq 0
        $cpuTime = $group.Group[0].metric -in @('mainCpuNsPerPresent','csCpuNsPerPresent')
        [pscustomobject]@{ comparison=$group.Name; pairs=$group.Count; processes=$changes.Count;
            aMedian=(Median @($group.Group | ForEach-Object a)); bMedian=(Median @($group.Group | ForEach-Object b));
            deltaMedian=(Median @($group.Group | ForEach-Object delta));
            medianProcessChangePercent=$(if ($allRelative) { Median @($changes | ForEach-Object changePercent) } else { $null });
            zeroAPairs=@($group.Group | Where-Object zeroA).Count; zeroBPairs=@($group.Group | Where-Object zeroB).Count;
            unobservedAPairs=@($group.Group | Where-Object { -not $_.observedA }).Count;
            unobservedBPairs=@($group.Group | Where-Object { -not $_.observedB }).Count;
            cpuAccountingQuantized=$cpuTime; processChanges=$changes }
    }
)
$result = [ordered]@{ mode=$metadata.mode; pairs=$paired.Count;
    timingSemantics='Phase CPU time retains zeros and finite OS accounting resolution. Cycles are normalized only by Presents from the same CPU sampler window. Inclusive/parallel thread costs are not additive.';
    reports=@($reports.Values); summary=$summary }
[IO.File]::WriteAllText((Join-Path $directory 'paired.json'), (ConvertTo-Json -InputObject @($paired.ToArray()) -Depth 8), $utf8)
[IO.File]::WriteAllText((Join-Path $directory 'summary.json'), (ConvertTo-Json -InputObject $result -Depth 10), $utf8)
Write-Output "Summary: $(Join-Path $directory 'summary.json')"
