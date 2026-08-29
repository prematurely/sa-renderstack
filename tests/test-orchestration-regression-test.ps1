param(
    [ValidateSet('All', 'Help', 'Runner', 'Schema', 'Gate')]
    [string]$Case = 'All'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
$testScript = Join-Path $root 'tools/test.ps1'
$exportScript = Join-Path $root 'tools/verify-exports.ps1'
$runnerScript = Join-Path $root 'tools/lib/process-runner.ps1'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) "sa-renderstack-test-orchestration-$([Guid]::NewGuid().ToString('N'))"

if (-not (Test-Path -LiteralPath $testScript -PathType Leaf)) {
    throw "Required test orchestrator is missing: $testScript"
}
if (-not (Test-Path -LiteralPath $exportScript -PathType Leaf)) {
    throw "Required export verifier is missing: $exportScript"
}

function Invoke-Captured {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [hashtable]$Environment = @{}
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$startInfo.ArgumentList.Add($argument) }
    foreach ($entry in $Environment.GetEnumerator()) {
        $startInfo.Environment[[string]$entry.Key] = [string]$entry.Value
    }
    $startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw "Unable to start $FilePath" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            StdOut = $stdoutTask.GetAwaiter().GetResult()
            StdErr = $stderrTask.GetAwaiter().GetResult()
        }
    } finally {
        $process.Dispose()
    }
}

function Assert-Help {
    $outside = Join-Path $fixtureRoot 'outside'
    [IO.Directory]::CreateDirectory($outside) | Out-Null
    foreach ($script in @($testScript, $exportScript)) {
        $result = Invoke-Captured -FilePath (Get-Command pwsh).Source `
            -Arguments @('-NoProfile', '-File', $script, '-Help') -WorkingDirectory $outside
        if ($result.ExitCode -ne 0 -or ($result.StdOut + $result.StdErr) -notmatch '(?i)usage:') {
            throw "Help contract failed: $script"
        }
    }
    Write-Output 'PASS test/export Help contracts'
}

function Assert-Runner {
    . $runnerScript
    $fixture = Join-Path $fixtureRoot 'runner'
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    $script = Join-Path $fixture 'child.ps1'
    [IO.File]::WriteAllText($script, @'
param([string]$First, [string]$Second)
$payload = [ordered]@{
  argv = @($First, $Second)
  cwd = [Environment]::CurrentDirectory
  removed = [Environment]::GetEnvironmentVariable('REMOVE_ME', 'Process')
}
[Console]::Out.WriteLine(('x' * 262144))
[Console]::Error.Write(('y' * 262144))
[Console]::Out.WriteLine(($payload | ConvertTo-Json -Compress))
'@)
    $env:REMOVE_ME = 'present'
    try {
        $result = Invoke-RenderStackProcess -FilePath (Get-Command pwsh).Source `
            -ArgumentList @('-NoProfile', '-File', $script, 'argument with spaces', 'literal"quote') `
            -WorkingDirectory $fixture -EnvironmentRemovals @('REMOVE_ME')
    } finally {
        Remove-Item Env:REMOVE_ME -ErrorAction SilentlyContinue
    }
    if ($result.ExitCode -ne 0 -or $result.StandardOutput.Length -lt 262144 -or
        $result.StandardError.Length -lt 262144) {
        throw 'Shared runner did not drain both streams'
    }
    $jsonLine = @($result.StandardOutput -split '\r?\n' | Where-Object { $_ -like '{*' })[-1]
    $payload = $jsonLine | ConvertFrom-Json
    if ($payload.argv[0] -cne 'argument with spaces' -or $payload.argv[1] -cne 'literal"quote' -or
        $payload.cwd -cne $fixture -or $null -ne $payload.removed) {
        throw 'Shared runner argument, working-directory, or environment-removal contract differs'
    }
    Write-Output 'PASS shared process runner stream and argument contracts'
}

function Assert-Schema {
    $source = Get-Content -LiteralPath $testScript -Raw
    $releaseSource = Get-Content -LiteralPath (Join-Path $root 'tools/release-gate.ps1') -Raw
    $buildSource = Get-Content -LiteralPath (Join-Path $root 'tools/build.ps1') -Raw
    foreach ($token in @('schemaVersion', 'repositoryCommit', 'overallStatus', 'skipReason',
            'stdoutLogPath', 'stderrLogPath', 'combinedLogPath', 'Write-RenderStackAtomicJson')) {
        if (-not $source.Contains($token, [StringComparison]::Ordinal)) {
            throw "Test result schema token is missing: $token"
        }
    }
    foreach ($token in @('DXVK_CONFIG_FILE', 'renderstack-gta-sa-compat-probe',
            'renderstack-stateblock-prefilter-probe', '--force', '--no-rebuild', 'intro-targets.json',
            "'status', '--porcelain', '--untracked-files=all'", 'hosted-ci-boundary-regression',
            'SkipGpuRuntimeProbes', 'SkipEnvironmentSensitiveBridgeTests')) {
        if (-not $source.Contains($token, [StringComparison]::Ordinal)) {
            throw "Runtime orchestration token is missing: $token"
        }
    }
    foreach ($token in @('out/build/bridge/d3d9.dll', 'out/build/dxvk-x86/src/d3d9/d3d9.dll',
            'SA-RenderStack-v$Version-sdk.zip', 'SA-RenderStack-v$Version-symbols.zip')) {
        if (-not $releaseSource.Contains($token, [StringComparison]::Ordinal)) {
            throw "Release evidence token is missing: $token"
        }
    }
    if (-not $buildSource.Contains('PYTHONDONTWRITEBYTECODE', [StringComparison]::Ordinal)) {
        throw 'Build orchestration does not disable Python bytecode generation'
    }
    if (-not $source.Contains('PYTHONDONTWRITEBYTECODE', [StringComparison]::Ordinal)) {
        throw 'Test orchestration does not disable Python bytecode generation'
    }
    foreach ($token in @('$mesonTestDirectory', '-WorkingDirectory $mesonTestDirectory', 'MaxAttempts',
            '$exportArguments', 'LlvmMingwBin', 'Ninja', 'Glslang', 'Reset-TestRunRoot',
            '$normalizedArguments = [Collections.Generic.List[string]]::new()')) {
        if (-not $source.Contains($token, [StringComparison]::Ordinal)) {
            throw "Test isolation/retry token is missing: $token"
        }
    }
    if ($source -notmatch '(?s)Add-SkippedGate\s+-Name ''build-refresh''.*?-Required \$false') {
        throw 'Verified build-cache reuse must not be recorded as a required skip'
    }
    foreach ($token in @('DXVK_CONFIG_FILE', 'metadata.tools.llvmMingw',
            'metadata.tools.ninja', 'metadata.tools.glslang', '$repositoryCommit',
            "`$overallStatus = 'failed'")) {
        $owner = if ($token -eq '$repositoryCommit') { $releaseSource } else { $source }
        if (-not $owner.Contains($token, [StringComparison]::Ordinal)) {
            throw "Test orchestration integrity token is missing: $token"
        }
    }
    foreach ($token in @('$commandDetails = if', '$releaseVerdict = if', '$missing = @(')) {
        if (-not $releaseSource.Contains($token, [StringComparison]::Ordinal)) {
            throw "Release gate expression token is missing: $token"
        }
    }
    Write-Output 'PASS test result and runtime orchestration schema'
}

function Assert-GateNullArguments {
    $gateRoot = $root
    $gateLogs = Join-Path $fixtureRoot 'gate-logs'
    $child = Join-Path $fixtureRoot 'gate-child.ps1'
    [IO.File]::WriteAllText($child, "Write-Output 'PASS gate child'`n")

    $script:root = $gateRoot
    $script:logsRoot = $gateLogs
    $script:results = [Collections.Generic.List[object]]::new()
    $script:firstExitCode = 0
    $script:overallStatus = 'passed'
    . (Join-Path $root 'tools/lib/process-runner.ps1')
    $tokens = $null
    $parseErrors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $root 'tools/test.ps1'), [ref]$tokens, [ref]$parseErrors)
    foreach ($functionName in @('Get-RelativePath', 'New-TestLogPaths', 'New-Result', 'Invoke-Gate', 'Invoke-ScriptGate')) {
        $function = $ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -ceq $functionName
            }, $true) | Select-Object -First 1
        if ($null -eq $function) { throw "Could not load test gate function: $functionName" }
        Invoke-Expression $function.Extent.Text
    }

    try {
        $record = Invoke-ScriptGate -Name 'null-arguments' -ScriptPath $child `
            -Arguments $null -Category 'regression'
        if ($record.status -cne 'passed' -or @($record.command | Where-Object { $null -eq $_ -or $_ -eq '' }).Count -ne 0) {
            throw 'Invoke-Gate did not normalize a null argument list into a valid command record'
        }
    } finally {
        if (Test-Path -LiteralPath $gateLogs) {
            Remove-Item -LiteralPath $gateLogs -Recurse -Force
        }
    }
    Write-Output 'PASS test gate null-argument contract'
}

try {
    [IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
    if ($Case -in @('All', 'Help')) { Assert-Help }
    if ($Case -in @('All', 'Runner')) { Assert-Runner }
    if ($Case -in @('All', 'Schema')) { Assert-Schema }
    if ($Case -in @('All', 'Gate')) { Assert-GateNullArguments }
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}

Write-Output 'PASS test orchestration regression suite'
exit 0
