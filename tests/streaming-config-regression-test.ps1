param([string]$LlvmMingwBin)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
# This standalone consumer probe does not invoke Ninja, glslang, or Python.
$llvm = Find-RenderStackLlvmMingw -RepoRoot $root -LlvmMingwBin $LlvmMingwBin
$fixture = Join-Path $root ('out/test-fixtures/streaming-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$probe = Join-Path $fixture 'streaming-interval-probe.exe'
& $llvm.CompilerPath '-std=c++17' '-O2' '-msse2' '-static' `
    (Join-Path $PSScriptRoot 'streaming-interval-probe.cpp') '-o' $probe
if ($LASTEXITCODE -ne 0) { throw "Native consumer probe compile failed: $LASTEXITCODE" }
& $probe --self-test
if ($LASTEXITCODE -ne 0) { throw 'Native decision fragment checks failed' }
$tool = Join-Path $root 'tools/prepare-streaming-config.ps1'
$utf8 = [Text.UTF8Encoding]::new($false)
$checks = 0
function Assert-Equal($Actual, $Expected, [string]$Name) {
    if ($Actual -cne $Expected) { throw "FAIL $Name expected=[$Expected] actual=[$Actual]" }
    $script:checks++
}
function Read-Consumer([string]$Path) {
    $result = & $probe $Path | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Consumer probe failed: $Path" }
    return $result | ConvertFrom-Json
}
function Prepare([string]$Source, [string]$Output) {
    $messages = & (Get-Command pwsh).Source -NoProfile -File $tool -Source $Source -OutputDirectory $Output 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Messages = $messages }
}
$baseline = "[Settings]`r`nStreamMemoryForced = 2000`r`nDoubleStreamingMemoryLimit = 1`r`nMaxRAM = 3500`r`nPreLoadLODs = 1`r`n; RemoveUnusedIntervalMs = 99`r`nRemoveUnusedIntervalMs = 750 ; retain this comment`r`n[Range7]`r`nRemoveUnusedIntervalMs = 25`r`nKeepLoaded = 1`r`n"
$source = Join-Path $fixture 'baseline.ini'
[IO.File]::WriteAllText($source, $baseline, $utf8)
$sourceHash = (Get-FileHash -LiteralPath $source).Hash
$before = Read-Consumer $source
Assert-Equal $before.intervalMs 60 'original consumer falls back to default'
Assert-Equal $before.streamMemory 2000 'native stream budget reading'
Assert-Equal $before.doubleLimit 1 'native doubled accounting reading'
Assert-Equal $before.maxRam 3500 'native RAM cap reading'
Assert-Equal $before.preloadLods 1 'native LOD preload reading'
$output = Join-Path $fixture 'prepared'
$prepared = Prepare $source $output
Assert-Equal $prepared.ExitCode 0 'preparation succeeds'
$candidate = Join-Path $output 'ImprovedStreaming.ini'
$after = Read-Consumer $candidate
Assert-Equal $after.intervalMs 750 'candidate consumer reads intended interval'
Assert-Equal (Get-FileHash -LiteralPath $source).Hash $sourceHash 'source unchanged'
Assert-Equal ([IO.File]::ReadAllText($candidate)) ($baseline.Replace('RemoveUnusedIntervalMs = 750', 'RemoveUnusedInterval = 750')) 'only target key bytes change'
foreach ($key in @('streamMemory','doubleLimit','maxRam','preloadLods')) {
    Assert-Equal $after.$key $before.$key "consumer preserves $key"
}
$manifest = Get-Content -LiteralPath (Join-Path $output 'preparation.json') -Raw | ConvertFrom-Json
Assert-Equal $manifest.applied $false 'manifest says not applied'
Assert-Equal $manifest.sourceSha256 $sourceHash 'manifest source identity'
Assert-Equal $manifest.candidateSha256 (Get-FileHash -LiteralPath $candidate).Hash 'manifest candidate identity'
Assert-Equal $manifest.intervalMs 750 'manifest effective interval'
Assert-Equal $manifest.changed $true 'manifest reports rename'

# Idempotence and output-collision checks exercise the public preparation command.
$again = Join-Path $fixture 'idempotent'
Assert-Equal (Prepare $candidate $again).ExitCode 0 'canonical input accepted'
Assert-Equal (Get-FileHash -LiteralPath (Join-Path $again 'ImprovedStreaming.ini')).Hash `
    (Get-FileHash -LiteralPath $candidate).Hash 'idempotent bytes'
Assert-Equal ((Get-Content (Join-Path $again 'preparation.json') -Raw | ConvertFrom-Json).changed) $false 'idempotent manifest'
Assert-Equal ((Prepare $source $output).ExitCode -ne 0) $true 'existing output refused'
Assert-Equal (Get-FileHash -LiteralPath $candidate).Hash $manifest.candidateSha256 'collision leaves candidate unchanged'

$invalid = [ordered]@{
    missing = "[Settings]`nMaxRAM=3500`n"
    duplicate = "[Settings]`nRemoveUnusedIntervalMs=750`nRemoveUnusedIntervalMs=500`n"
    conflict = "[Settings]`nRemoveUnusedIntervalMs=750`nRemoveUnusedInterval=60`n"
    malformed = "[Settings]`nRemoveUnusedIntervalMs=750junk`n"
    negative = "[Settings]`nRemoveUnusedIntervalMs=-1`n"
    overflow = "[Settings]`nRemoveUnusedIntervalMs=2147483648`n"
    wrongSection = "[Other]`nRemoveUnusedIntervalMs=750`n"
    wrongCase = "[Settings]`nremoveunusedintervalms=750`n"
    duplicateSection = "[Settings]`nRemoveUnusedIntervalMs=750`n[Settings]`nOther=1`n"
    unicodeKeySpace = "[Settings]`n$([char]0x00A0)RemoveUnusedIntervalMs=750`n"
    unicodeSectionSpace = "[$([char]0x2003)Settings]`nRemoveUnusedIntervalMs=750`n"
    unicodeValueSpace = "[Settings]`nRemoveUnusedIntervalMs=$([char]0x00A0)750`n"
}
foreach ($entry in $invalid.GetEnumerator()) {
    $path = Join-Path $fixture ($entry.Key + '.ini')
    $destination = Join-Path $fixture ($entry.Key + '-output')
    [IO.File]::WriteAllText($path, $entry.Value, $utf8)
    $hash = (Get-FileHash -LiteralPath $path).Hash
    Assert-Equal ((Prepare $path $destination).ExitCode -ne 0) $true "reject $($entry.Key)"
    Assert-Equal (Test-Path -LiteralPath $destination) $false "no output on $($entry.Key)"
    Assert-Equal (Get-FileHash -LiteralPath $path).Hash $hash "input preserved on $($entry.Key)"
}
foreach ($value in @('0','0x2EE')) {
    $path = Join-Path $fixture ('value-' + $value + '.ini')
    [IO.File]::WriteAllText($path, "[Settings]`nRemoveUnusedIntervalMs=$value`n", $utf8)
    $destination = Join-Path $fixture ('value-' + $value)
    Assert-Equal (Prepare $path $destination).ExitCode 0 "accept $value"
    $expected = if ($value -ceq '0') {0} else {750}
    Assert-Equal (Read-Consumer (Join-Path $destination 'ImprovedStreaming.ini')).intervalMs $expected "effective $value"
}
$bomSource = Join-Path $fixture 'bom.ini'
[IO.File]::WriteAllText($bomSource, $baseline.Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($true))
$bomOutput = Join-Path $fixture 'bom-output'
Assert-Equal (Prepare $bomSource $bomOutput).ExitCode 0 'UTF8 BOM accepted'
Assert-Equal ([Convert]::ToHexString([IO.File]::ReadAllBytes((Join-Path $bomOutput 'ImprovedStreaming.ini')))) `
    ([Convert]::ToHexString([Text.UTF8Encoding]::new($true).GetPreamble()) + [Convert]::ToHexString($utf8.GetBytes($baseline.Replace("`r`n", "`n").Replace('RemoveUnusedIntervalMs = 750','RemoveUnusedInterval = 750')))) 'BOM and LF preserved'
Assert-Equal (Read-Consumer (Join-Path $bomOutput 'ImprovedStreaming.ini')).intervalMs 750 'BOM native consumer'
$utf16Source = Join-Path $fixture 'utf16.ini'
[IO.File]::WriteAllText($utf16Source, $baseline, [Text.Encoding]::Unicode)
Assert-Equal ((Prepare $utf16Source (Join-Path $fixture 'utf16-output')).ExitCode -ne 0) $true 'UTF16 refused'
$badUtf8 = Join-Path $fixture 'invalid-utf8.ini'
[IO.File]::WriteAllBytes($badUtf8, [byte[]]@(0xC3,0x28))
Assert-Equal ((Prepare $badUtf8 (Join-Path $fixture 'invalid-utf8-output')).ExitCode -ne 0) $true 'invalid UTF8 refused'
Assert-Equal (Test-Path -LiteralPath (Join-Path $fixture 'invalid-utf8-output')) $false 'no output for invalid UTF8'
Assert-Equal ((Prepare $source (Join-Path $root ('streaming-forbidden-' + [guid]::NewGuid().ToString('N')))).ExitCode -ne 0) $true 'output outside out refused'
Assert-Equal ((Prepare $source $fixture).ExitCode -ne 0) $true 'source directory refused'
Assert-Equal (Get-FileHash -LiteralPath $source).Hash $sourceHash 'source unchanged after all cases'

# Repeated AB/BA replay through the real INI reader and relocated native gate.
# These are deterministic policy counts, deliberately not a CPU/FPS benchmark.
$samples = [Collections.Generic.List[object]]::new()
for ($round = 0; $round -lt 12; $round++) {
    $order = if ($round % 2 -eq 0) {@('A','B')} else {@('B','A')}
    foreach ($variant in $order) {
        $path = if ($variant -ceq 'A') {$source} else {$candidate}
        $sample = Read-Consumer $path
        $expected = if ($variant -ceq 'A') {156} else {13}
        Assert-Equal $sample.pressurePermits $expected "round $round $variant pressure permits"
        Assert-Equal $sample.ramPermits 625 "round $round $variant emergency permits"
        $samples.Add([ordered]@{round=$round; variant=$variant; intervalMs=$sample.intervalMs;
            pressurePermits=$sample.pressurePermits; ramPermits=$sample.ramPermits})
    }
}
$report = [ordered]@{schemaVersion=1; kind='offline-policy-replay'; gameTest=$false;
    fixture=$fixture; checks=$checks; rounds=12; simulatedDurationMs=10000; callbackStepMs=16;
    sourceSha256=$sourceHash; candidateSha256=$manifest.candidateSha256;
    nativeProbeSha256=(Get-FileHash -LiteralPath $probe).Hash; samples=@($samples.ToArray())}
[IO.File]::WriteAllText((Join-Path $fixture 'results.json'), ($report | ConvertTo-Json -Depth 6), $utf8)
Write-Output "PASS $checks preparation/consumer/replay assertions; 12 alternating A/B rounds"
Write-Output "Retained fixture and report: $fixture"
