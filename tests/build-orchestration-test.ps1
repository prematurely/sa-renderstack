param(
    [string]$Python,
    [string]$LlvmMingwBin
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
    $scripts = @(
        (Join-Path $root 'tools/build.ps1'),
        (Join-Path $root 'tools/prepare-meson.ps1'),
        (Join-Path $root 'tools/test.ps1'),
        (Join-Path $root 'tools/verify-exports.ps1'),
        (Join-Path $root 'tools/release-gate.ps1')
)
$outside = Join-Path ([IO.Path]::GetTempPath()) "sa-renderstack-help-$([Guid]::NewGuid().ToString('N'))"

function Get-OutFileSnapshot {
    $out = Join-Path $root 'out'
    if (-not (Test-Path -LiteralPath $out -PathType Container)) {
        return @()
    }
    return @(
        Get-ChildItem -LiteralPath $out -File -Force -Recurse |
            Sort-Object FullName |
            ForEach-Object {
                $relative = [IO.Path]::GetRelativePath($out, $_.FullName).Replace('\', '/')
                "$relative|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)|$((Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash)"
            }
    )
}

function Invoke-Captured {
    param([Parameter(Mandatory)] [string]$Script)

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = (Get-Command pwsh).Source
    $startInfo.WorkingDirectory = $outside
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in @('-NoProfile', '-File', $Script, '-Help')) {
        [void]$startInfo.ArgumentList.Add($argument)
    }
    $startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw "Unable to start Help process: $Script" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = $stdoutTask.GetAwaiter().GetResult() + $stderrTask.GetAwaiter().GetResult()
        }
    } finally {
        $process.Dispose()
    }
}

try {
    [IO.Directory]::CreateDirectory($outside) | Out-Null
    foreach ($script in $scripts) {
        if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
            throw "Required orchestration script is missing: $script"
        }
    }
    $before = Get-OutFileSnapshot
    foreach ($script in $scripts) {
        $result = Invoke-Captured -Script $script
        if ($result.ExitCode -ne 0 -or $result.Output -notmatch '(?i)usage:') {
            throw "Help failed for $script with exit $($result.ExitCode): $($result.Output)"
        }
    }
    $after = Get-OutFileSnapshot
    if (@(Compare-Object $before $after).Count -ne 0) {
        throw 'Help changed repository out files'
    }

    $metadataPath = Join-Path $root 'out/build-metadata.json'
    if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        throw "Build metadata is missing: $metadataPath"
    }
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    if ($metadata.schema.GetType() -ne [Int64] -or $metadata.schema -ne 1 -or
        $metadata.configuration -cne 'Release' -or $metadata.architecture -cne 'x86' -or
        $metadata.component -cne 'All' -or $metadata.exitCode.GetType() -ne [Int64] -or
        $metadata.exitCode -ne 0) {
        throw 'Build metadata top-level schema differs'
    }
    foreach ($output in @($metadata.outputs)) {
        $path = Join-Path $root ([string]$output.path)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Build metadata output is missing: $($output.path)"
        }
        $item = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToUpperInvariant()
        if ($output.size.GetType() -ne [Int64] -or $output.size -ne $item.Length -or
            $output.sha256 -cne $hash) {
            throw "Build metadata output hash/size differs: $($output.path)"
        }
    }

    $expectedTargets = @(
        'renderstack-d3d9-batch-audit-test',
        'renderstack-d3d9-deferred-shader-binding-test',
        'renderstack-dxvk-state-dedup-test',
        'renderstack-stateblock-prefilter-probe',
        'renderstack-gta-sa-compat-probe'
    )
    $expectedTests = $expectedTargets[0..2]
    $targets = @(Get-Content -LiteralPath (Join-Path $root 'out/build/dxvk-x86/meson-info/intro-targets.json') -Raw |
        ConvertFrom-Json | Where-Object { $_.name -like 'renderstack-*' } | ForEach-Object { $_.name } | Sort-Object)
    $tests = @(Get-Content -LiteralPath (Join-Path $root 'out/build/dxvk-x86/meson-info/intro-tests.json') -Raw |
        ConvertFrom-Json | ForEach-Object { $_.name } | Sort-Object)
    if (@(Compare-Object ($expectedTargets | Sort-Object) $targets).Count -ne 0) {
        throw "Meson target registration differs: $($targets -join ', ')"
    }
    if (@(Compare-Object ($expectedTests | Sort-Object) $tests).Count -ne 0) {
        throw "Meson test registration differs: $($tests -join ', ')"
    }
} finally {
    if (Test-Path -LiteralPath $outside) {
        Remove-Item -LiteralPath $outside -Recurse -Force
    }
}

Write-Output 'PASS build and Help orchestration contracts'
exit 0
