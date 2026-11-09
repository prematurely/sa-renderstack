param(
    [Parameter(Mandatory)] [string]$Baseline,
    [Parameter(Mandatory)] [string]$Candidate,
    [Parameter(Mandatory)] [string]$Probe,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [ValidateRange(1,50)] [int]$Processes = 12,
    [ValidateRange(1,16)] [int]$Rounds = 4,
    [ValidateSet('on','off')] [string]$Profile = 'on',
    [ValidateSet('controls','all','textures')] [string]$Cases = 'controls',
    [string]$Cpu = 'any'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib/control-path-results.ps1')
$Baseline = (Resolve-Path -LiteralPath $Baseline).Path
$Candidate = (Resolve-Path -LiteralPath $Candidate).Path
$Probe = (Resolve-Path -LiteralPath $Probe).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$expected = [Collections.Generic.HashSet[string]]::new()
foreach ($stage in @('vs','ps')) {
    foreach ($kind in @('float','int')) {
        $limit = if ($kind -eq 'int') {16} elseif ($stage -eq 'ps') {224} else {256}
        foreach ($count in @(1,4,16,64,224,256)) {
            if ($count -gt $limit) { continue }
            foreach ($pattern in @('same','first','tail','all')) {
                $key = "setter,$stage,$kind,$count,$pattern"
                if ($Cases -eq 'all' -or ($Cases -ne 'textures' -and $key -in @('setter,vs,float,4,same','setter,ps,float,16,same',
                        'setter,ps,int,16,same','setter,vs,int,16,same','setter,ps,float,64,all'))) {
                    [void]$expected.Add($key)
                }
            }
        }
    }
}
foreach ($kind in @('draw','texture-draw','sampler-draw','bind-draw','multi-bind-draw',
        'texture-hit-draw','texture-multi-hit-draw','texture-mixed-draw','texture-srgb-draw')) {
    if ($Cases -eq 'controls' -and $kind -notin @('draw','texture-draw')) {continue}
    if ($Cases -eq 'all' -and $kind -in @('texture-hit-draw','texture-multi-hit-draw','texture-mixed-draw','texture-srgb-draw')) {continue}
    if ($Cases -eq 'textures' -and $kind -in @('draw','sampler-draw','bind-draw')) {continue}
    [void]$expected.Add("$kind,vs+ps,float,4,tail")
    [void]$expected.Add("$kind-complete,vs+ps,float,4,tail")
}
# Refuse a reused profile/CPU namespace before any file can be overwritten.
if (@(Get-ChildItem -LiteralPath $OutputDirectory -Filter "$Profile-$Cpu-*").Count -or
    (Test-Path -LiteralPath (Join-Path $OutputDirectory "results-$Profile-$Cpu.json")) -or
    (Test-Path -LiteralPath (Join-Path $OutputDirectory "summary-$Profile-$Cpu.csv"))) {
    throw 'Output namespace is occupied; use a fresh output directory'
}
$binaryHashes = @($Baseline,$Candidate,$Probe | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
$started = [DateTime]::UtcNow.ToString('o')
$samples = [Collections.Generic.List[object]]::new()
$metadata = [Collections.Generic.List[object]]::new()
Push-Location $OutputDirectory
try {
    for ($process = 0; $process -lt $Processes; ++$process) {
        $stem = '{0}-{1}-{2:D2}' -f $Profile,$Cpu,$process
        & $Probe pair $Baseline $Candidate $Profile $Rounds $process $Cpu $Cases > "$stem.jsonl" 2> "$stem.err"
        if ($LASTEXITCODE -ne 0) {throw "$stem failed: $LASTEXITCODE"}
        $parsed = Read-ControlPathSamples -Path "$stem.jsonl" -Expected $expected -Rounds $Rounds `
            -Seed $process -Profile $Profile -Cpu $Cpu -SharedDevice ($Baseline -ieq $Candidate)
        $metadata.Add($parsed.metadata)
        foreach ($row in $parsed.samples) {
            $row | Add-Member -NotePropertyName process -NotePropertyValue $process
            $samples.Add($row)
        }
        Write-Output "completed process $($process+1)/$Processes profile=$Profile cpu=$Cpu checks=$($parsed.checks) samples=$($parsed.samples.Count)"
    }
} finally {Pop-Location}
function Median([double[]]$values) {
    $v = @($values | Sort-Object)
    if ($v.Count % 2) {return $v[[int][Math]::Floor($v.Count/2)]}
    return ($v[$v.Count/2-1]+$v[$v.Count/2])/2
}
$summary = foreach ($case in $expected | Sort-Object) {
    $rows = @($samples | Where-Object case -eq $case)
    $changes = foreach ($process in 0..($Processes-1)) {
        $paired = foreach ($round in 0..($Rounds-1)) {
            $a = $rows | Where-Object {$_.process -eq $process -and $_.round -eq $round -and $_.variant -eq 'baseline'}
            $b = $rows | Where-Object {$_.process -eq $process -and $_.round -eq $round -and $_.variant -eq 'candidate'}
            100 * ($b.ns/$a.ns-1)
        }
        Median $paired
    }
    [pscustomobject]@{
        case=$case
        baselineMedianNs=Median @($rows | Where-Object variant -eq baseline | ForEach-Object ns)
        candidateMedianNs=Median @($rows | Where-Object variant -eq candidate | ForEach-Object ns)
        processMedianChangePercent=Median $changes
        fasterProcesses=@($changes | Where-Object {$_ -lt 0}).Count
        processes=$Processes
        processChanges=@($changes)
    }
}
$finalHashes = @($Baseline,$Candidate,$Probe | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
if (@(Compare-Object $binaryHashes $finalHashes -Property Path,Hash).Count) {
    throw 'Benchmark binaries changed during the run'
}
$report = [ordered]@{
    startedUtc=$started;endedUtc=[DateTime]::UtcNow.ToString('o');profile=$Profile;cpu=$Cpu;cases=$Cases
    processes=$Processes;rounds=$Rounds
    binaries=$binaryHashes
    metadata=@($metadata);samples=@($samples);summary=@($summary)
}
$report | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $OutputDirectory "results-$Profile-$Cpu.json")
$summary | Select-Object -ExcludeProperty processChanges | Export-Csv (Join-Path $OutputDirectory "summary-$Profile-$Cpu.csv") -NoTypeInformation
