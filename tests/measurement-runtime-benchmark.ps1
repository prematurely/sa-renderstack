param(
    [Parameter(Mandatory)] [string]$Backend,
    [ValidateRange(1, 64)] [int]$Processes = 6,
    [ValidateRange(4, 64)] [int]$Rounds = 4,
    [ValidateRange(32, 4096)] [int]$Iterations = 256,
    [string]$EvidenceName = 'runtime-overhead',
    [switch]$SkipActiveCapture,
    [switch]$NoBuild,
    [string]$MSBuild,
    [switch]$AllowNonV18MsBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) {
    $buildParameters = @{ BuildOnly = $true; Backend = $Backend; EvidenceName = 'benchmark-prepare' }
    if ($MSBuild) { $buildParameters.MSBuild = $MSBuild }
    if ($AllowNonV18MsBuild) { $buildParameters.AllowNonV18MsBuild = $true }
    & (Join-Path $PSScriptRoot 'measurement-runtime-test.ps1') @buildParameters
}
$backendItem = Get-Item -LiteralPath $Backend
$probeItem = Get-Item -LiteralPath (Join-Path $root 'out/perf/20261027-unified-measurement/runtime/bin/MeasurementRuntimeTests.exe')
$backendHash = (Get-FileHash -LiteralPath $backendItem.FullName -Algorithm SHA256).Hash
$probeHash = (Get-FileHash -LiteralPath $probeItem.FullName -Algorithm SHA256).Hash
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff')
$out = Join-Path $root "out/perf/20261027-unified-measurement/runtime/benchmarks/$EvidenceName-$stamp-$PID"
New-Item -ItemType Directory -Path $out -Force | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$metadata = [ordered]@{
    backend = $backendItem.FullName; backendSha256 = $backendHash; backendSize = $backendItem.Length
    probe = $probeItem.FullName; probeSha256 = $probeHash; probeSize = $probeItem.Length
    processes = $Processes; rounds = $Rounds; iterations = $Iterations; warmupPerVariant = 64
    hud = 'renderstack,fps,frametimes'; present = 'immediate'; gpuFence = 'outside-timed-region'
    backBufferWidth = 640; backBufferHeight = 480; viewportCheck = 'native GetBackBuffer/GetDesc must match before timing'
    window = 'visible, shown without activation; messages pumped outside timed variants'; visibleHudLines = 8
    measurement = 'wall-clear-draw-present-runtime'; units = 'nanoseconds-per-successful-Present'
    variants = @{ A = 'runtime disabled'; B = 'runtime enabled, idle, RenderStack HUD hidden'; C = 'runtime enabled, idle, RenderStack HUD visible' }
    ordering = 'AB/BA and AC/CA alternate by round within each independent process'
    activeCapture = 'separate observer-stage illustration in the first process; not a matched A/B'
}
[IO.File]::WriteAllText((Join-Path $out 'metadata.json'), ($metadata | ConvertTo-Json -Depth 5), $utf8)
$rows = [Collections.Generic.List[object]]::new()
for ($index = 0; $index -lt $Processes; ++$index) {
    if ((Get-FileHash -LiteralPath $backendItem.FullName -Algorithm SHA256).Hash -cne $backendHash -or
        (Get-FileHash -LiteralPath $probeItem.FullName -Algorithm SHA256).Hash -cne $probeHash) {
        throw 'Backend or probe identity changed between independent benchmark processes'
    }
    $directory = Join-Path $out "process-$index"
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $probeItem.FullName
    foreach ($argument in @('--benchmark', $backendItem.FullName, [string]$Rounds, [string]$Iterations)) {
        [void]$start.ArgumentList.Add($argument)
    }
    $includeActive = $index -eq 0 -and -not $SkipActiveCapture
    if ($includeActive) { [void]$start.ArgumentList.Add('--active') }
    $start.WorkingDirectory = $directory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment['DXVK_HUD'] = 'renderstack,fps,frametimes'
    $start.Environment['DXVK_LOG_PATH'] = $directory
    $start.Environment['MEASUREMENT_BENCH_BACKEND_SHA256'] = $backendHash
    $start.Environment['MEASUREMENT_BENCH_PROBE_SHA256'] = $probeHash
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    Write-Output "Benchmark process $($index + 1)/${Processes}: $Rounds alternating rounds, $Iterations Presents per variant"
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(120000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw "Benchmark process $index exceeded 120 seconds"
    }
    $outputText = $stdout.GetAwaiter().GetResult()
    $errorText = $stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'), $outputText, $utf8)
    [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'), $errorText, $utf8)
    if ($process.ExitCode -ne 0) { throw "Benchmark process $index failed: $($process.ExitCode); see $directory" }
    $processRows = @($outputText -split '\r?\n' | Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json })
    $idleRows = @($processRows | Where-Object type -eq 'idle-round')
    if ($idleRows.Count -ne $Rounds * 4) { throw "Expected $($Rounds * 4) idle rows, found $($idleRows.Count) in $directory" }
    foreach ($row in $processRows) {
        $row | Add-Member -NotePropertyName processIndex -NotePropertyValue $index
        if ($row.type -eq 'idle-round' -and ($row.failedPresents -ne 0 -or $row.workloadFailures -ne 0 -or
                $row.successfulPresents -ne $Iterations -or -not $row.idle -or -not $row.gpuCompleted -or
                $row.backendSha256 -cne $backendHash -or $row.probeSha256 -cne $probeHash)) {
            throw "Invalid timed row in $directory"
        }
        $rows.Add($row)
        [IO.File]::AppendAllText((Join-Path $out 'per-round.jsonl'), (($row | ConvertTo-Json -Compress -Depth 5) + "`n"), $utf8)
    }
    $activeRows = @($processRows | Where-Object type -eq 'active-observer-stage')
    if ($activeRows.Count -ne $(if ($includeActive) { 3 } else { 0 })) {
        throw "Unexpected active observer-stage row count in $directory"
    }
    $process.Dispose()
}
$paired = @($rows | Where-Object type -eq 'idle-round' | Group-Object processIndex, round, pair | ForEach-Object {
    $group = $_.Group
    $baseline = @($group | Where-Object variant -eq 'A')
    $observed = @($group | Where-Object variant -ne 'A')
    if ($baseline.Count -ne 1 -or $observed.Count -ne 1) { throw "Unpaired benchmark rows: $($_.Name)" }
    [ordered]@{ processIndex = $baseline[0].processIndex; pid = $baseline[0].pid; round = $baseline[0].round
        pair = $baseline[0].pair; order = $baseline[0].order; variant = $observed[0].variant
        disabledNsPerPresent = $baseline[0].wallNsPerSuccessfulPresent
        observedNsPerPresent = $observed[0].wallNsPerSuccessfulPresent
        deltaNsPerPresent = $observed[0].wallNsPerSuccessfulPresent - $baseline[0].wallNsPerSuccessfulPresent
        ratio = $observed[0].wallNsPerSuccessfulPresent / $baseline[0].wallNsPerSuccessfulPresent }
})
[IO.File]::WriteAllText((Join-Path $out 'paired.json'), (ConvertTo-Json -InputObject @($paired) -Depth 4), $utf8)
$overlay = @($paired | Group-Object processIndex, round | ForEach-Object {
    $hidden = @($_.Group | Where-Object pair -eq 'AB')[0]
    $visible = @($_.Group | Where-Object pair -eq 'AC')[0]
    [ordered]@{ processIndex = $hidden.processIndex; pid = $hidden.pid; round = $hidden.round
        hiddenNsPerPresent = $hidden.observedNsPerPresent; visibleNsPerPresent = $visible.observedNsPerPresent
        visibleMinusHiddenNsPerPresent = $visible.observedNsPerPresent - $hidden.observedNsPerPresent
        differenceOfPairedDeltasNsPerPresent = $visible.deltaNsPerPresent - $hidden.deltaNsPerPresent
        visibleToHiddenRatio = $visible.observedNsPerPresent / $hidden.observedNsPerPresent }
})
[IO.File]::WriteAllText((Join-Path $out 'overlay-paired.json'), (ConvertTo-Json -InputObject @($overlay) -Depth 4), $utf8)
Write-Output "Benchmark artifacts: $out"
