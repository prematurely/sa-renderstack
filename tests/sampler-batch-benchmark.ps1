param(
    [Parameter(Mandatory)] [string]$Baseline,
    [Parameter(Mandatory)] [string]$Candidate,
    [Parameter(Mandatory)] [string]$Probe,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [ValidateRange(1,30)] [int]$Processes = 6,
    [ValidateRange(1,10)] [int]$Rounds = 4
)
$ErrorActionPreference = 'Stop'
$Baseline = (Resolve-Path -LiteralPath $Baseline).Path
$Candidate = (Resolve-Path -LiteralPath $Candidate).Path
$Probe = (Resolve-Path -LiteralPath $Probe).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new benchmark output directory.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$before = @($Baseline,$Candidate,$Probe | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
$env:DXVK_CONFIG = 'd3d9.gtaSaCompat = True; d3d9.gtaSaStateBlockPrefilter = True; d3d9.gtaSaCompatDiagnostics = False; dxvk.hud = 0'
$env:DXVK_HUD = ''; $env:DXVK_LOG_PATH = $OutputDirectory
$cases = @('sampler-repeat','sampler-change','batch-repeat','batch-change','stateblock','draw-control','draw-repeat','draw-change')
$sameModule = [int]($Baseline -ieq $Candidate)
$samples = [Collections.Generic.List[object]]::new()
$processEvidence = [Collections.Generic.List[object]]::new()
$started = [DateTime]::UtcNow.ToString('o')
Push-Location $OutputDirectory
try {
    for ($process=0; $process -lt $Processes; ++$process) {
        $stem = 'pair-{0:D2}' -f $process
        & $Probe pair $Baseline $Candidate $process $Rounds > "$stem.jsonl" 2> "$stem.err"
        if ($LASTEXITCODE) { throw "$stem failed with exit $LASTEXITCODE" }
        $rows = @(Get-Content -LiteralPath "$stem.jsonl" | ForEach-Object { $_ | ConvertFrom-Json })
        $result = @($rows | Where-Object kind -eq 'result')
        if ($result.Count -ne 1 -or $result[0].failures -ne 0 -or $result[0].checks -lt 40000) {
            throw "$stem did not complete real-device behavior checks"
        }
        $metadata = @($rows | Where-Object kind -eq 'metadata')
        if ($metadata.Count -ne 2 -or @($metadata.protected | Sort-Object -Unique).Count -ne 2) {
            throw "$stem has invalid device metadata"
        }
        foreach ($m in $metadata) {
            if ($m.protected -notin 0,1 -or $m.sameModule -ne $sameModule -or $m.seed -ne $process -or $m.rounds -ne $Rounds) {
                throw "$stem metadata mismatch"
            }
        }
        $seen = [Collections.Generic.HashSet[string]]::new()
        foreach ($row in @($rows | Where-Object kind -eq 'sample')) {
            if ($row.case -notin $cases -or $row.protected -notin 0,1 -or $row.variant -notin 'baseline','candidate' -or
                $row.round -lt 0 -or $row.round -ge $Rounds) { throw "$stem invalid sample key" }
            $expected = if ($row.case.StartsWith('draw')) {2048} elseif ($row.case.StartsWith('batch') -or $row.case -eq 'stateblock') {20000} else {1000000}
            if ($row.iterations -ne $expected) { throw "$stem iteration mismatch" }
            foreach ($field in 'ns','cycles','completeNs') {
                $value = [double]$row.$field
                if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -le 0) { throw "$stem invalid $field" }
            }
            $key = '{0}/{1}/{2}/{3}' -f $row.protected,$row.case,$row.round,$row.variant
            if (!$seen.Add($key)) { throw "$stem duplicate $key" }
            $row | Add-Member -NotePropertyName process -NotePropertyValue $process
            $samples.Add($row)
        }
        if ($seen.Count -ne 2*$cases.Count*$Rounds*2 -or $rows.Count -ne $seen.Count+3) { throw "$stem missing or unexpected rows" }
        $processEvidence.Add([pscustomobject]@{ process=$process; checks=$result[0].checks; metadata=$metadata })
        Write-Output "$stem complete: $($result[0].checks) checks, $($seen.Count) timing samples"
    }
} finally { Pop-Location }
function Median([double[]]$values) {
    $v = @($values | Sort-Object)
    if ($v.Count%2) { return $v[[int][Math]::Floor($v.Count/2)] }
    return ($v[$v.Count/2-1]+$v[$v.Count/2])/2
}
$summary = foreach ($protected in 0,1) { foreach ($case in $cases) { foreach ($metric in 'ns','cycles','completeNs') {
    $selected = @($samples | Where-Object { $_.protected -eq $protected -and $_.case -eq $case })
    $changes = @(for ($process=0;$process -lt $Processes;++$process) {
        $paired = @(for ($round=0;$round -lt $Rounds;++$round) {
            $a = $selected | Where-Object { $_.process -eq $process -and $_.round -eq $round -and $_.variant -eq 'baseline' }
            $b = $selected | Where-Object { $_.process -eq $process -and $_.round -eq $round -and $_.variant -eq 'candidate' }
            100*([double]$b.$metric/[double]$a.$metric-1)
        })
        Median $paired
    })
    [pscustomobject]@{ protected=$protected; case=$case; metric=$metric;
        baselineMedian=(Median @($selected | Where-Object variant -eq 'baseline' | ForEach-Object { $_.$metric }));
        candidateMedian=(Median @($selected | Where-Object variant -eq 'candidate' | ForEach-Object { $_.$metric }));
        changePercent=(Median $changes); fasterProcesses=@($changes | Where-Object {$_ -lt 0}).Count;
        processes=$Processes; processChanges=$changes }
} } }
$after = @($Baseline,$Candidate,$Probe | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
if (@(Compare-Object $before $after -Property Path,Hash).Count) { throw 'Benchmark binaries changed.' }
[ordered]@{ startedUtc=$started; endedUtc=[DateTime]::UtcNow.ToString('o'); processes=$Processes; rounds=$Rounds;
    binaries=$before; evidence=@($processEvidence); samples=@($samples); summary=@($summary) } |
    ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Encoding utf8
$summary | Select-Object -ExcludeProperty processChanges | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'summary.csv') -NoTypeInformation
