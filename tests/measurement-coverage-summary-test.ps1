$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$directory = Join-Path $root ('out/perf/20261112-measurement-coverage/summary-fixture-{0}-{1}' -f $PID,[DateTime]::UtcNow.Ticks)
New-Item -ItemType Directory -Path $directory | Out-Null
[IO.File]::WriteAllText((Join-Path $directory 'metadata.json'), '{"mode":"Runtime"}')
$rows = @(
    foreach ($variant in @('A','B')) {
        $sampledPresents = if ($variant -eq 'A') { 5 } else { 10 }
        $cycles = if ($variant -eq 'A') { 200 } else { 400 }
        $report = Join-Path $directory ($variant+'.log')
        [IO.File]::WriteAllText($report,
            "cpu captureId=1 samples=2 samplingElapsedNs=1000 sampledPresents=$sampledPresents`n")
        foreach ($phase in @('baseline','cpu')) {
            [ordered]@{ suite='active'; processIndex=0; round=0; variant=$variant; phase=$phase;
                elapsedNs=2000; presents=20; mainCpuNs=0; csCpuNs=0; mainThreadCycles=$cycles;
                mainSamples=2; reportPath=$report }
        }
    }
)
$rows | ForEach-Object { $_ | ConvertTo-Json -Compress } | Set-Content -LiteralPath (Join-Path $directory 'samples.jsonl') -Encoding utf8
& (Join-Path $PSScriptRoot 'measurement-coverage-summary.ps1') -InputDirectory $directory
$result = Get-Content -LiteralPath (Join-Path $directory 'summary.json') -Raw | ConvertFrom-Json
$cycle = @($result.summary | Where-Object comparison -eq 'active, cpu, mainCyclesPerSampledPresent')
if ($cycle.Count -ne 1 -or $cycle[0].aMedian -ne 40 -or $cycle[0].bMedian -ne 40 -or
    $cycle[0].medianProcessChangePercent -ne 0) { throw 'Sampler cycles were not normalized with the same-window Presents.' }
$cpu = @($result.summary | Where-Object { $_.comparison -like '*CpuNsPerPresent' })
if ($cpu.Count -ne 4 -or @($cpu | Where-Object { $_.pairs -ne 1 -or $_.zeroAPairs -ne 1 -or
        $_.zeroBPairs -ne 1 -or $null -ne $_.medianProcessChangePercent }).Count) {
    throw 'Zero CPU accounting pairs were lost or assigned a fabricated relative percentage.'
}
if ($result.reports.Count -ne 2) { throw 'Same-window source reports and hashes are missing.' }
Write-Output 'PASS same-window cycle normalization and retained zero CPU accounting pairs'

# A mismatched report must fail, rather than quietly normalizing a different capture.
[IO.File]::WriteAllText((Join-Path $directory 'B.log'),
    "cpu captureId=1 samples=99 samplingElapsedNs=1000 sampledPresents=10`n")
$rejected = $false
try { & (Join-Path $PSScriptRoot 'measurement-coverage-summary.ps1') -InputDirectory $directory | Out-Null }
catch { $rejected = $_.Exception.Message -like '*sample count differs*' }
if (-not $rejected) { throw 'The summary accepted a mismatched source report.' }
Write-Output 'PASS mismatched capture evidence is rejected'
