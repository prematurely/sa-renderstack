param(
    [Parameter(Mandatory)] [string]$Baseline,
    [Parameter(Mandatory)] [string]$Candidate,
    [Parameter(Mandatory)] [string]$Probe,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [ValidateRange(3,30)] [int]$Pairs = 9
)
$ErrorActionPreference='Stop'
$variants=@{baseline=(Resolve-Path -LiteralPath $Baseline).Path;candidate=(Resolve-Path -LiteralPath $Candidate).Path}
$probe=(Resolve-Path -LiteralPath $Probe).Path
if(Test-Path -LiteralPath $OutputDirectory){throw 'Evidence directory already exists'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$output=(Resolve-Path -LiteralPath $OutputDirectory).Path
# Separate processes alternate AA, AB and BA. No affinity or priority changes.
$before=@($variants.baseline,$variants.candidate,$probe|Get-FileHash|Select-Object Path,Hash)
$rows=[Collections.Generic.List[object]]::new()
foreach($pair in 0..($Pairs-1)){
  $order=if($pair%3 -eq 0){@('baseline','baseline')}elseif($pair%3 -eq 1){@('baseline','candidate')}else{@('candidate','baseline')}
  foreach($mode in 'present','observer') { foreach($slot in 0..1){
    if(Get-Process gta_sa -ErrorAction SilentlyContinue){throw 'Game is running'}
    $variant=$order[$slot]
    $directory=Join-Path $output "$pair-$mode-$slot-$variant"
    New-Item -ItemType Directory -Path $directory|Out-Null
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$probe; $start.WorkingDirectory=$directory
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.ArgumentList.Add($variants[$variant]);$start.ArgumentList.Add("--bench-$mode")
    $start.Environment['DXVK_HUD']=''
    $start.Environment['DXVK_CONFIG']='d3d9.gtaSaCompat=True;d3d9.presentInterval=0;dxvk.hud='
    $start.Environment['DXVK_LOG_PATH']=$directory
    $process=[Diagnostics.Process]::Start($start)
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if(!$process.WaitForExit(60000)){ $process.Kill();throw 'Present control timed out' }
    $raw=$stdout.GetAwaiter().GetResult()
    [IO.File]::WriteAllText("$directory/stdout.log",$raw)
    [IO.File]::WriteAllText("$directory/stderr.log",$stderr.GetAwaiter().GetResult())
    if($process.ExitCode){throw "Probe failed: $directory"}
    $parsed=@($raw -split '\r?\n'|Where-Object {$_ -like '{*'}|ForEach-Object {$_|ConvertFrom-Json})
    if($parsed.Count -ne 1){throw 'Expected one timing record'}
    $row=$parsed[0]
    if($row.failures -ne 0 -or $row.failedPresents -ne 0 -or $row.iterations -ne 2000 -or $row.ns -le 0 -or $row.cycles -le 0 -or $row.callbacks -ne $(if($mode -eq 'observer'){2128}else{0})){throw 'Invalid behavior/timing record'}
    $row|Add-Member -NotePropertyMembers @{pair=$pair;slot=$slot;variant=$variant;mode=$mode;control=($pair%3 -eq 0)}
    $rows.Add($row);$process.Dispose()
  }}
  "Present pair $pair complete"
}
$after=@($variants.baseline,$variants.candidate,$probe|Get-FileHash|Select-Object Path,Hash)
if(@(Compare-Object $before $after -Property Path,Hash).Count){throw 'Benchmark binaries changed'}
@{binaries=$before;rows=@($rows);affinity='normal';warmup=128;iterations=2000}|ConvertTo-Json -Depth 8|Set-Content "$output/results.json"
function Median($values){$v=@($values|Sort-Object);if($v.Count%2){return $v[[int][Math]::Floor($v.Count/2)]};return ($v[$v.Count/2-1]+$v[$v.Count/2])/2}
$summary=foreach($mode in 'present','observer'){foreach($control in $false,$true){foreach($metric in 'ns','cycles'){
  $selected=@($rows|Where-Object {$_.mode -eq $mode -and $_.control -eq $control})
  $changes=foreach($pair in @($selected.pair|Sort-Object -Unique)){
    $members=@($selected|Where-Object pair -eq $pair)
    $a=if($control){$members[0]}else{$members|Where-Object variant -eq 'baseline'}
    $b=if($control){$members[1]}else{$members|Where-Object variant -eq 'candidate'}
    100*($b.$metric/$a.$metric-1)
  }
  [pscustomobject]@{mode=$mode;control=$control;metric=$metric;changePercent=(Median $changes);pairs=$changes.Count;changes=@($changes)}
}}}
$summary|ConvertTo-Json -Depth 5|Set-Content "$output/summary.json"
$summary|Select-Object -ExcludeProperty changes|Format-Table
