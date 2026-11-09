param(
    [Parameter(Mandatory)] [string]$Baseline,
    [Parameter(Mandatory)] [string]$Candidate,
    [Parameter(Mandatory)] [string]$Probe,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [ValidateRange(4, 50)] [int]$Pairs = 10,
    [ValidateSet('on', 'off')] [string]$Dedup = 'on'
)
$ErrorActionPreference = 'Stop'
$Baseline = (Resolve-Path -LiteralPath $Baseline).Path
$Candidate = (Resolve-Path -LiteralPath $Candidate).Path
$Probe = (Resolve-Path -LiteralPath $Probe).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$samples = [Collections.Generic.List[object]]::new()
$expected = @{}
foreach ($stage in @('vs','ps')) {
    foreach ($kind in @('float','int')) {
        $limit = if ($kind -eq 'int') { 16 } elseif ($stage -eq 'ps') { 224 } else { 256 }
        foreach ($count in @(1,4,16,64,224,256)) {
            if ($count -gt $limit) { continue }
            foreach ($pattern in @('same','first','tail','all')) {
                $expected["setter,$stage,$kind,$count,$pattern"] = 300000
            }
        }
    }
}
foreach ($kind in @('draw','texture-draw','sampler-draw','bind-draw','multi-bind-draw')) {
    $expected["$kind,vs+ps,float,4,tail"] = 16000
    $expected["$kind-complete,vs+ps,float,4,tail"] = 16000
}
$started = [DateTime]::UtcNow.ToString('o')
Push-Location $OutputDirectory
try {
    for ($pair = 0; $pair -lt $Pairs; $pair++) {
        # Balanced AB/BA order limits a fixed ordering bias. No affinity,
        # priority, power-plan or driver configuration is changed.
        $order = if ($pair % 2) { @('candidate','baseline') } else { @('baseline','candidate') }
        foreach ($variant in $order) {
            $dll = if ($variant -eq 'baseline') { $Baseline } else { $Candidate }
            $stem = '{0}-{1:D2}-{2}' -f $Dedup, $pair, $variant
            & $Probe $dll $Dedup bench > "$stem.csv" 2> "$stem.err"
            if ($LASTEXITCODE -ne 0) { throw "$stem failed with exit $LASTEXITCODE" }
            $seen = [Collections.Generic.HashSet[string]]::new()
            foreach ($line in Get-Content "$stem.csv") {
                if ($line -notmatch '^(setter|[a-z-]*draw(?:-complete)?),') { continue }
                $fields = $line.Split(',')
                if ($fields.Count -ne 7) { throw "$stem invalid row: $line" }
                $case = $fields[0..4] -join ','
                if (!$expected.ContainsKey($case) -or !$seen.Add($case)) { throw "$stem unexpected/duplicate case: $case" }
                if ([int]$fields[5] -ne $expected[$case]) { throw "$stem wrong iteration count: $case" }
                $elapsed = [double]::Parse($fields[6], [Globalization.CultureInfo]::InvariantCulture)
                if ([double]::IsNaN($elapsed) -or [double]::IsInfinity($elapsed) -or $elapsed -le 0) {
                    throw "$stem invalid time: $case"
                }
                $samples.Add([pscustomobject]@{
                    pair = $pair; variant = $variant
                    case = $case; iterations = [int]$fields[5]; ns = $elapsed
                })
            }
            if ($seen.Count -ne $expected.Count) { throw "$stem missing benchmark cases ($($seen.Count)/$($expected.Count))" }
        }
        Write-Output "completed pair $($pair + 1)/$Pairs dedup=$Dedup"
    }
} finally { Pop-Location }

function Median([double[]]$values) {
    $v = @($values | Sort-Object)
    if ($v.Count % 2) { return $v[[int][Math]::Floor($v.Count / 2)] }
    return ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2
}
$summary = foreach ($group in $samples | Group-Object case) {
    $a = @($group.Group | Where-Object variant -eq baseline | ForEach-Object ns)
    $b = @($group.Group | Where-Object variant -eq candidate | ForEach-Object ns)
    $ma = Median $a; $mb = Median $b
    $pairChanges = foreach ($n in 0..($Pairs - 1)) {
        $pa = $group.Group | Where-Object { $_.pair -eq $n -and $_.variant -eq 'baseline' }
        $pb = $group.Group | Where-Object { $_.pair -eq $n -and $_.variant -eq 'candidate' }
        if (!$pa -or !$pb) { throw "missing sample for $($group.Name) pair=$n" }
        100 * ($pb.ns / $pa.ns - 1)
    }
    [pscustomobject]@{
        case = $group.Name; baselineMedianNs = $ma; candidateMedianNs = $mb
        changePercent = 100 * ($mb / $ma - 1)
        pairedMedianChangePercent = Median $pairChanges
        candidateFasterPairs = @($pairChanges | Where-Object { $_ -lt 0 }).Count
        pairs = $Pairs
    }
}
$report = [ordered]@{
    startedUtc = $started; endedUtc = [DateTime]::UtcNow.ToString('o')
    dedup = $Dedup; pairs = $Pairs
    binaries = @($Baseline, $Candidate, $Probe | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
    samples = @($samples); summary = @($summary)
    limits = 'Synthetic public API and small real draws; does not establish GTA scene FPS.'
}
$report | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDirectory "results-$Dedup.json")
$summary | Export-Csv (Join-Path $OutputDirectory "summary-$Dedup.csv") -NoTypeInformation
