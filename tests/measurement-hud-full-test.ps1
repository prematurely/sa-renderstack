param([string]$Backend, [string]$LlvmMingwBin, [string]$Python, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$pythonInfo = Find-RenderStackPython -PythonPath $Python -RepoRoot $root
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'out/perf/20261110-full-hud' }
$captures = @{}
foreach ($case in @(
    @{ Name='reference-wide'; Items='renderstack'; Width=2560; Height=480 },
    @{ Name='reference-narrow'; Items='renderstack'; Width=720; Height=480 },
    @{ Name='full-2560'; Items='full'; Width=2560; Height=1600 },
    @{ Name='full-1280'; Items='full'; Width=1280; Height=720 }
)) {
    $directory = Join-Path $OutputDirectory $case.Name
    & (Join-Path $PSScriptRoot 'measurement-backend-test.ps1') -Backend $Backend `
        -LlvmMingwBin $LlvmMingwBin -FullHud -HudItems $case.Items `
        -Width $case.Width -Height $case.Height -OutputDirectory $directory
    if ($LASTEXITCODE -ne 0) { throw "Full HUD render failed: $($case.Name)" }
    $captures[$case.Name] = Join-Path $directory 'hud-published.bmp'
}
foreach ($pair in @(@('reference-wide','full-2560'), @('reference-narrow','full-1280'))) {
    & $pythonInfo.Path (Join-Path $PSScriptRoot 'measurement-hud-full-test.py') $captures[$pair[0]] $captures[$pair[1]]
    if ($LASTEXITCODE -ne 0) { throw 'Full HUD clips or overlaps the eight-line measurement block' }
}
