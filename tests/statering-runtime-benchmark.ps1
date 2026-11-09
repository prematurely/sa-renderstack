param([Parameter(Mandatory)][string]$Backend,[int]$Processes=12,[int]$Rounds=4,[int]$Iterations=512,[string]$EvidenceName='paired-final')
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if($Processes -lt 2 -or $Rounds -lt 2 -or $Iterations -lt 64){throw 'Repeated benchmark requires processes>=2, rounds>=2 and iterations>=64'}
$root=Split-Path -Parent $PSScriptRoot
$base=Join-Path $root 'out/perf/20261027-statering-cache'
$exe=@{A=Join-Path $base 'bench-baseline/bin/StateRingRuntimeTests.exe';B=Join-Path $base 'bench-candidate/bin/StateRingRuntimeTests.exe'}
$out=Join-Path $base ($EvidenceName+'-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff'))
New-Item -ItemType Directory -Path $out|Out-Null
$Backend=(Resolve-Path -LiteralPath $Backend).Path
$identities=@($Backend,$exe.A,$exe.B)|Get-FileHash -Algorithm SHA256|Select-Object Path,Hash
[ordered]@{identities=$identities;processPairs=$Processes;rounds=$Rounds;iterations=$Iterations;order='AB/BA alternating process pairs; native modes reverse by round';measurement='wall-ns-per-guard-plus-owner-ring-poll';inputs='real D3D9/Vulkan device; controlled GTA API addresses and key/clock; no physical input';limits='source fixture, not game FPS; forwarding VirtualQuery observer shared by both versions'}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $out 'metadata.json')
$rows=[Collections.Generic.List[object]]::new()
for($p=0;$p -lt $Processes;++$p){
    $order=if($p%2){'B','A'}else{'A','B'}
    foreach($variant in $order){
        foreach($id in $identities){if((Get-FileHash -LiteralPath $id.Path -Algorithm SHA256).Hash -cne $id.Hash){throw 'Benchmark input changed'}}
        $wd=Join-Path $out "$p-$variant";New-Item -ItemType Directory -Path $wd|Out-Null
        Push-Location $wd
        try {& $exe[$variant] --benchmark $Backend $Rounds $Iterations *> 'raw.log';$code=$LASTEXITCODE}
        finally {Pop-Location}
        if($code){throw "Native benchmark failed: $p-$variant"}
        $data=@(Get-Content (Join-Path $wd 'raw.log')|Where-Object {$_.StartsWith('{')}|ForEach-Object {$_|ConvertFrom-Json})
        if($data.Count -ne $Rounds*3){throw 'Incomplete benchmark rows'}
        foreach($row in $data){
            $row|Add-Member -NotePropertyName processPair -NotePropertyValue $p
            $row|Add-Member -NotePropertyName variant -NotePropertyValue $variant
            $rows.Add($row)
        }
    }
    Write-Output "completed pair $($p+1)/$Processes"
}
$rows|ConvertTo-Json -Depth 4|Set-Content (Join-Path $out 'rows.json')
$paired=@($rows|Group-Object processPair,round,mode|ForEach-Object {$a=@($_.Group|Where-Object variant -eq A)[0];$b=@($_.Group|Where-Object variant -eq B)[0];[pscustomobject]@{processPair=$a.processPair;round=$a.round;mode=$a.mode;baselineNs=$a.wallNsPerCall;candidateNs=$b.wallNsPerCall;ratio=$b.wallNsPerCall/$a.wallNsPerCall;baselineQueries=$a.virtualQueries;candidateQueries=$b.virtualQueries;baselineSnapshots=$a.snapshotAttempts;candidateSnapshots=$b.snapshotAttempts}})
$paired|ConvertTo-Json -Depth 4|Set-Content (Join-Path $out 'paired.json')
Write-Output "Artifacts: $out"
