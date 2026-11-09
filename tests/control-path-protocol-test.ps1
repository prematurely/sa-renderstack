param([Parameter(Mandatory)] [string]$InputPath)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib/control-path-results.ps1')
# Mutate a successful real-device capture, so protocol tests stay tied to the
# producer's actual records and terminal behavioral-check count.
$lines = @(Get-Content -LiteralPath $InputPath)
$meta = $lines[0] | ConvertFrom-Json
$expected = [Collections.Generic.HashSet[string]]::new()
foreach ($line in $lines[1..($lines.Count-2)]) {[void]$expected.Add(($line | ConvertFrom-Json).case)}
if ($expected.Count -notin @(9,12,78)) {throw 'Not a complete benchmark capture'}
$cpu = if ($meta.pinned) { [string](($lines[1] | ConvertFrom-Json).cpuStart) } else {'any'}
$argsForParser = @{Expected=$expected;Rounds=$meta.rounds;Seed=$meta.seed;Profile=$meta.profile;Cpu=$cpu;SharedDevice=$meta.sharedDevice}
[void](Read-ControlPathSamples -Path $InputPath @argsForParser)
$temp = Join-Path ([IO.Path]::GetDirectoryName((Resolve-Path -LiteralPath $InputPath).Path)) ('protocol-' + [guid]::NewGuid().ToString('N') + '.jsonl')
$cases = [ordered]@{
    'zero checks' = {param($a) $a[-1]='checks=0 failures=0'; $a}
    'missing completion' = {param($a) $a[0..($a.Count-2)]}
    'duplicate completion' = {param($a) $a + $a[-1]}
    'missing sample' = {param($a) $a[0] ; $a[2..($a.Count-1)]}
    'duplicate sample' = {param($a) $a[0];$a[1];$a[1..($a.Count-1)]}
    'wrong workload' = {param($a) $r=$a[1]|ConvertFrom-Json;$r.iterations=1;$a[1]=$r|ConvertTo-Json -Compress;$a}
    'nonfinite cycles' = {param($a) $r=$a[1]|ConvertFrom-Json;$r.cycles='Infinity';$a[1]=$r|ConvertTo-Json -Compress;$a}
    'wrong shared device' = {param($a) $r=$a[0]|ConvertFrom-Json;$r.sharedDevice=!$r.sharedDevice;$a[0]=$r|ConvertTo-Json -Compress;$a}
    'wrong affinity metadata' = {param($a) $r=$a[0]|ConvertFrom-Json;$r.pinned=!$r.pinned;$a[0]=$r|ConvertTo-Json -Compress;$a}
    'zero module' = {param($a) $r=$a[0]|ConvertFrom-Json;$r.baselineBase='0';$a[0]=$r|ConvertTo-Json -Compress;$a}
    'wrong module topology' = {param($a) $r=$a[0]|ConvertFrom-Json;$r.candidateBase=if($r.sharedDevice){'1'}else{$r.baselineBase};$a[0]=$r|ConvertTo-Json -Compress;$a}
}
try {
    foreach ($entry in $cases.GetEnumerator()) {
        & $entry.Value $lines.Clone() | Set-Content -LiteralPath $temp
        $rejected=$false
        try {[void](Read-ControlPathSamples -Path $temp @argsForParser)} catch {$rejected=$true}
        if (!$rejected) {throw "Accepted invalid capture: $($entry.Key)"}
    }
} finally {if (Test-Path -LiteralPath $temp) {Remove-Item -LiteralPath $temp}}
Write-Output "PASS real capture accepted; $($cases.Count) invalid capture mutations rejected"
