param([Parameter(Mandatory)][string]$Baseline,[Parameter(Mandatory)][string]$Candidate,
  [Parameter(Mandatory)][string]$Probe,[Parameter(Mandatory)][string]$OutputDirectory,
  [ValidateRange(2,30)][int]$Pairs=12,[switch]$DisableCompat,[switch]$Observe,[switch]$ObserverPair,[string]$Cases='all')
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a fresh benchmark directory'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$Baseline=(Resolve-Path -LiteralPath $Baseline).Path;$Candidate=(Resolve-Path -LiteralPath $Candidate).Path;$Probe=(Resolve-Path -LiteralPath $Probe).Path
$hashes=@($Baseline,$Candidate,$Probe|Get-FileHash|Select-Object Path,Hash)
$metadata=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');pairs=$Pairs;rounds=5;disableCompat=[bool]$DisableCompat;observe=[bool]$Observe;observerPair=[bool]$ObserverPair;cases=$Cases;hashes=$hashes;scope='real native D3D9 device, no game geometry; submit and completed wall times; alternating process order and rotated cases; fixture main thread pinned to permitted logical CPU, workers unchanged'}
$metadata|ConvertTo-Json -Depth 5|Set-Content (Join-Path $OutputDirectory 'metadata.json')
$rows=[Collections.Generic.List[object]]::new()
$env:DXVK_CONFIG="d3d9.gtaSaCompat = $(!$DisableCompat); d3d9.gtaSaStateBlockPrefilter = True; d3d9.gtaSaCompatDiagnostics = False; dxvk.hud = 0"
$env:DXVK_HUD=''
$mode=if($Observe){'bench'}else{'bench-off'}
Push-Location $OutputDirectory
try{
  # Run functional acceptance against the exact bytes about to be timed.
  # Always exercise journals here, even for compat-off performance controls.
  $performanceConfig=$env:DXVK_CONFIG
  $env:DXVK_CONFIG='d3d9.gtaSaCompat=True;d3d9.gtaSaStateBlockPrefilter=True;dxvk.hud=0'
  foreach($variant in @('baseline','candidate')){
    $path=if($variant -eq 'baseline'){$Baseline}else{$Candidate}
    $checkMode=if($variant -eq 'baseline'){'check-optional-stats'}else{'check'}
    $env:DXVK_LOG_PATH=$OutputDirectory
    & $Probe $path $checkMode > "preflight-$variant.log" 2> "preflight-$variant.err"
    if($LASTEXITCODE){throw "Functional preflight failed: $variant; no performance result is accepted"}
  }
  $env:DXVK_CONFIG=$performanceConfig
  for($pair=0;$pair -lt $Pairs;$pair++){
    $order=if($pair%2){@('candidate','baseline')}else{@('baseline','candidate')}
    foreach($variant in $order){
      $path=if($variant -eq 'baseline'){$Baseline}else{$Candidate}
      if($ObserverPair){$mode=if($variant -eq 'baseline'){'bench-off'}else{'bench'}}
      $log=Join-Path $OutputDirectory "$pair-$variant.log";$env:DXVK_LOG_PATH=$OutputDirectory
      & $Probe $path $mode $pair $Cases > $log 2> "$log.err"
      if($LASTEXITCODE){throw "Benchmark process failed: $pair/$variant"}
      $records=@(Get-Content -LiteralPath $log)
      if(!($records -match '^checks=\d+ failures=0$')){throw 'Missing successful process footer'}
      $expected=if($Cases -ne 'all'){@($Cases.Split(',')).Count*5}elseif($DisableCompat){15}else{45}
      $bench=@($records|Where-Object {$_ -match '^bench '});$complete=@($records|Where-Object {$_ -match '^complete '})
      if($bench.Count -ne $expected -or $complete.Count -ne $expected){throw 'Missing/duplicate benchmark rows'}
      foreach($line in $bench){
        if($line -notmatch '^bench case=(\S+) round=(\d+) count=(\d+) us=(\d+) observer=(\d+)$'){throw 'Invalid benchmark record'}
        $case=$Matches[1];$round=[int]$Matches[2];$count=[int]$Matches[3];$us=[double]$Matches[4]
        $completed=@($complete|Where-Object {$_ -match "^complete case=$case round=$round us=(\d+)$"})
        if($completed.Count -ne 1){throw 'Ambiguous completion record'}
        $completed[0] -match 'us=(\d+)$'|Out-Null
        $rows.Add([pscustomobject]@{pair=$pair;variant=$variant;case=$case;round=$round;count=$count;submitNs=$us*1000/$count;completeNs=[double]$Matches[1]*1000/$count})
      }
    }
    Write-Output "Completed pair $($pair+1)/$Pairs"
  }
}finally{Pop-Location}
function Median($v){$s=@($v|Sort-Object);if($s.Count%2){$s[[int][Math]::Floor($s.Count/2)]}else{($s[$s.Count/2-1]+$s[$s.Count/2])/2}}
$summary=foreach($case in @($rows.case|Sort-Object -Unique)){
  foreach($metric in @('submitNs','completeNs')){
    $changes=for($pair=0;$pair -lt $Pairs;$pair++){
      $ratios=for($round=0;$round -lt 5;$round++){
        $a=@($rows|Where-Object {$_.pair -eq $pair -and $_.round -eq $round -and $_.case -eq $case -and $_.variant -eq 'baseline'})
        $b=@($rows|Where-Object {$_.pair -eq $pair -and $_.round -eq $round -and $_.case -eq $case -and $_.variant -eq 'candidate'})
        if($a.Count -ne 1 -or $b.Count -ne 1 -or $a[0].$metric -le 0){throw 'Invalid matched pair'}
        100*($b[0].$metric/$a[0].$metric-1)
      };Median $ratios
    }
    [pscustomobject]@{case=$case;metric=$metric;baselineMedianNs=Median @($rows|Where-Object {$_.case -eq $case -and $_.variant -eq 'baseline'}|ForEach-Object {$_.$metric});candidateMedianNs=Median @($rows|Where-Object {$_.case -eq $case -and $_.variant -eq 'candidate'}|ForEach-Object {$_.$metric});pairedMedianChangePercent=Median $changes;fasterPairs=@($changes|Where-Object {$_ -lt 0}).Count;pairs=$Pairs;pairChanges=@($changes)}
  }
}
$rows|ConvertTo-Json -Depth 4|Set-Content (Join-Path $OutputDirectory 'rows.json')
$summary|ConvertTo-Json -Depth 5|Set-Content (Join-Path $OutputDirectory 'summary.json')
if(@(Compare-Object $hashes @($Baseline,$Candidate,$Probe|Get-FileHash|Select-Object Path,Hash) -Property Path,Hash).Count){throw 'Binary hashes changed during benchmark'}
$summary|Format-Table case,metric,pairedMedianChangePercent,fasterPairs,pairs
