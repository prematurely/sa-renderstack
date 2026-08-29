param(
    [switch]$Help,
    [string]$Version = '0.1.0-alpha.1',
    [string]$Configuration = 'Release',
    [string]$Architecture = 'x86',
    [string]$GameRoot
)

if ($Help) {
    @'
Usage: pwsh -NoProfile -File tools/release-gate.ps1 [-Help]
       [-Version 0.1.0-alpha.1] [-Configuration Release] [-Architecture x86]
       [-GameRoot <GTA San Andreas directory>]

Runs the clean local release gate in a fixed order and writes
out/reports/phase-1-release-gate.md. The gate never deploys files to the game.
'@ | Write-Output
    exit 0
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

. (Join-Path $PSScriptRoot 'lib/process-runner.ps1')

$root = Get-RenderStackRepositoryRoot -CallerScriptPath $PSCommandPath
$outRoot = Join-Path $root 'out'
$reportsRoot = Join-Path $outRoot 'reports'
$logsRoot = Join-Path $outRoot 'logs/release-gate'
$reportPath = Join-Path $reportsRoot 'phase-1-release-gate.md'
$startedUtc = [DateTime]::UtcNow
$firstExitCode = 0
$overallStatus = 'passed'
$failureMessage = $null
$repositoryCommit = 'unavailable'
$steps = [Collections.Generic.List[object]]::new()

function Get-RelativePath {
    param([Parameter(Mandatory)] [string]$Path)
    return [IO.Path]::GetRelativePath($root, (Get-RenderStackFullPath -Path $Path)).Replace('\', '/')
}

function Add-Step {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Status,
        [Parameter(Mandatory)] [string]$Details,
        [string]$Command = '',
        [string]$LogPath = '',
        [AllowNull()] [Nullable[int]]$ExitCode = $null,
        [AllowNull()] [Nullable[double]]$DurationMs = $null
    )
    [void]$steps.Add([ordered]@{
        name = $Name
        status = $Status
        details = $Details
        command = $Command
        logPath = $LogPath
        exitCode = $ExitCode
        durationMs = $DurationMs
    })
    if ($Status -eq 'FAIL') {
        $script:overallStatus = 'failed'
        if ($script:firstExitCode -eq 0) { $script:firstExitCode = if ($null -ne $ExitCode -and $ExitCode -ne 0) { $ExitCode } else { 1 } }
    }
}

function New-StepLogPaths {
    param([Parameter(Mandatory)] [string]$Name)
    New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null
    $safeName = $Name -replace '[^A-Za-z0-9_.-]', '-'
    $directory = Join-Path $logsRoot (New-RenderStackArtifactName -Prefix $safeName -Extension '.dir')
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    return [ordered]@{
        stdout = Join-Path $directory 'stdout.log'
        stderr = Join-Path $directory 'stderr.log'
        combined = Join-Path $directory 'combined.log'
    }
}

function Invoke-ReleaseProcess {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $root,
        [hashtable]$EnvironmentOverrides = @{},
        [string[]]$EnvironmentRemovals = @()
    )
    $paths = New-StepLogPaths -Name $Name
    foreach ($path in $paths.Values) {
        [IO.File]::WriteAllText($path, '', [Text.UTF8Encoding]::new($false))
    }
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    try {
        $result = Invoke-RenderStackProcess -FilePath $FilePath -ArgumentList $Arguments `
            -WorkingDirectory $WorkingDirectory -EnvironmentOverrides $EnvironmentOverrides `
            -EnvironmentRemovals $EnvironmentRemovals -StandardOutputLogPath $paths.stdout `
            -StandardErrorLogPath $paths.stderr -CombinedLogPath $paths.combined -Label $Name
        $stopwatch.Stop()
        $status = if ($result.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }
        $commandDetails = if ($status -eq 'PASS') { 'Command exited successfully' } else { 'Command failed' }
        Add-Step -Name $Name -Status $status -Details $commandDetails `
            -Command $result.Command -LogPath (Get-RelativePath -Path $paths.combined) `
            -ExitCode $result.ExitCode -DurationMs $stopwatch.Elapsed.TotalMilliseconds
        return $result
    } catch {
        $stopwatch.Stop()
        Add-RenderStackLogText -Path $paths.stderr -Text ($_.Exception.Message + [Environment]::NewLine)
        Add-Step -Name $Name -Status 'FAIL' -Details $_.Exception.Message `
            -Command ((@($FilePath) + @($Arguments)) -join ' ') `
            -LogPath (Get-RelativePath -Path $paths.combined) -ExitCode 1 `
            -DurationMs $stopwatch.Elapsed.TotalMilliseconds
        return $null
    }
}

function Assert-StepPassed {
    param([Parameter(Mandatory)] [object]$Result, [Parameter(Mandatory)] [string]$Name)
    if ($null -eq $Result -or $Result.ExitCode -ne 0) {
        throw "$Name failed"
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory)] [string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file: $Path"
    }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToUpperInvariant()
}

function Write-ReleaseReport {
    param([Parameter(Mandatory)] [string]$Verdict)
    New-Item -ItemType Directory -Path $reportsRoot -Force | Out-Null
    $lines = [Collections.Generic.List[string]]::new()
    [void]$lines.Add('# SA RenderStack Phase 1 Release Gate')
    [void]$lines.Add('')
    [void]$lines.Add("- Version: $Version")
    [void]$lines.Add("- Configuration: $Configuration")
    [void]$lines.Add("- Architecture: $Architecture")
    [void]$lines.Add("- Repository: $root")
    [void]$lines.Add("- Repository commit: $repositoryCommit")
    [void]$lines.Add("- Game root: $GameRoot")
    [void]$lines.Add("- Started UTC: $($startedUtc.ToString('o'))")
    [void]$lines.Add("- Ended UTC: $([DateTime]::UtcNow.ToString('o'))")
    [void]$lines.Add('')
    [void]$lines.Add('| Gate | Status | Exit | Details | Log |')
    [void]$lines.Add('| --- | --- | ---: | --- | --- |')
    foreach ($step in $steps) {
        $details = ([string]$step.details).Replace('|', '\|').Replace("`r", ' ').Replace("`n", ' ')
        $log = if ([string]::IsNullOrWhiteSpace([string]$step.logPath)) { '' } else { "[$($step.logPath)]($($step.logPath))" }
        $exit = if ($null -eq $step.exitCode) { '' } else { [string]$step.exitCode }
        [void]$lines.Add("| $($step.name) | $($step.status) | $exit | $details | $log |")
    }
    [void]$lines.Add('')
    [void]$lines.Add("Verdict: $Verdict")
    $temporary = Join-Path $reportsRoot (New-RenderStackArtifactName -Prefix 'release-gate' -Extension '.tmp')
    [IO.File]::WriteAllText($temporary, (($lines -join [Environment]::NewLine) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary, $reportPath, $true)
}

try {
    if ($Configuration -cne 'Release') {
        throw "Unsupported configuration '$Configuration'; only Release is accepted"
    }
    if ($Architecture -cne 'x86') {
        throw "Unsupported architecture '$Architecture'; only x86 is accepted"
    }
    if ([string]::IsNullOrWhiteSpace($Version) -or $Version -match '[\\/:\x00]') {
        throw "Invalid release version '$Version'"
    }
    $versionFile = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
    if ($versionFile -cne $Version) {
        throw "Release version differs from VERSION: expected '$versionFile', requested '$Version'"
    }
    if ([string]::IsNullOrWhiteSpace($GameRoot)) {
        $GameRoot = Split-Path -Parent (Split-Path -Parent $root)
    }
    $GameRoot = Get-RenderStackFullPath -Path $GameRoot
    if (-not (Test-Path -LiteralPath $GameRoot -PathType Container)) {
        throw "Game root does not exist: $GameRoot"
    }

    $identity = Invoke-RenderStackProcess -FilePath (Get-Command git.exe -ErrorAction Stop).Source `
        -ArgumentList @('rev-parse', 'HEAD') -WorkingDirectory $root -Label 'release-identity'
    if ($identity.ExitCode -ne 0) {
        throw "Unable to resolve repository commit: $($identity.StandardError)"
    }
    $repositoryCommit = $identity.StandardOutput.Trim()
    if ($repositoryCommit -notmatch '^[0-9a-f]{40}$') {
        throw "Git returned an invalid repository commit: $repositoryCommit"
    }
    Add-Step -Name 'release-identity' -Status 'PASS' -Details "Repository commit $repositoryCommit" `
        -Command $identity.Command -ExitCode 0 -DurationMs $identity.DurationMilliseconds

    $packageScript = Join-Path $root 'tools/package.ps1'
    $packageTest = Join-Path $root 'tests/package-layout-test.ps1'
    $missing = @($packageScript, $packageTest) | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }
    if ($missing.Count -ne 0) {
        Add-Step -Name 'package-preflight' -Status 'FAIL' `
            -Details "Task 7 dependency missing; no build deletion performed: $($missing -join ', ')"
        throw 'Release gate preflight failed'
    }
    Add-Step -Name 'package-preflight' -Status 'PASS' -Details 'Task 7 packaging dependencies are present'

    $pwsh = (Get-Command pwsh -ErrorAction Stop).Source
    $build = Invoke-ReleaseProcess -Name 'clean-build' -FilePath $pwsh -Arguments @(
        '-NoProfile', '-File', (Join-Path $root 'tools/build.ps1'),
        '-Configuration', $Configuration,
        '-Architecture', $Architecture,
        '-Component', 'All', '-Clean')
    Assert-StepPassed -Result $build -Name 'clean-build'

    $tests = Invoke-ReleaseProcess -Name 'full-test' -FilePath $pwsh -Arguments @(
        '-NoProfile', '-File', (Join-Path $root 'tools/test.ps1'),
        '-Configuration', $Configuration, '-Architecture', $Architecture)
    Assert-StepPassed -Result $tests -Name 'full-test'

    $package = Invoke-ReleaseProcess -Name 'package' -FilePath $pwsh -Arguments @(
        '-NoProfile', '-File', $packageScript,
        '-Version', $Version, '-Configuration', $Configuration)
    Assert-StepPassed -Result $package -Name 'package'

    $packageLayout = Invoke-ReleaseProcess -Name 'package-layout' -FilePath $pwsh -Arguments @(
        '-NoProfile', '-File', $packageTest,
        '-Version', $Version, '-Configuration', $Configuration)
    Assert-StepPassed -Result $packageLayout -Name 'package-layout'

    $hygiene = Invoke-ReleaseProcess -Name 'source-hygiene' -FilePath $pwsh -Arguments @(
        '-NoProfile', '-File', (Join-Path $root 'tools/verify-source-tree.ps1'))
    Assert-StepPassed -Result $hygiene -Name 'source-hygiene'

    $diffCheck = Invoke-ReleaseProcess -Name 'git-diff-check' -FilePath (Get-Command git.exe).Source `
        -Arguments @('diff', '--check')
    Assert-StepPassed -Result $diffCheck -Name 'git-diff-check'

    $tracked = Invoke-ReleaseProcess -Name 'tracked-artifact-policy' -FilePath (Get-Command git.exe).Source `
        -Arguments @('ls-files')
    Assert-StepPassed -Result $tracked -Name 'tracked-artifact-policy'
    $forbiddenTracked = @($tracked.StandardOutput -split '\r?\n' | Where-Object {
        $_ -match '(?i)(?:^|/)(?:[^/]+\.)?(?:bak|log|exe|dll|pdb|obj|o)$' -or
        $_ -match '(?i)(?:^|/)\.wraplock$'
    })
    if ($forbiddenTracked.Count -ne 0) {
        Add-Step -Name 'tracked-artifact-policy-result' -Status 'FAIL' `
            -Details "Forbidden tracked artifacts: $($forbiddenTracked -join ', ')"
        throw 'Tracked artifact policy failed'
    }
    Add-Step -Name 'tracked-artifact-policy-result' -Status 'PASS' -Details 'No forbidden build/runtime artifacts are tracked'

    $rollback = @(
        [pscustomobject]@{ Name = 'game-root-bridge'; Path = Join-Path $GameRoot 'd3d9.dll'; Expected = 'B0BBE0C98B132EA0B8E6BA0FA2C978D82C48063A50C2324BE3A8F7141FE7E9FF' },
        [pscustomobject]@{ Name = 'game-root-dxvk'; Path = Join-Path $GameRoot 'dxvk-3.0.1-merged/d3d9.dll'; Expected = '69454C02480981686731B7975EDEA5452E64F02425624BEA410C3A432933FF5F' }
    )
    foreach ($item in $rollback) {
        $actual = Get-FileSha256 -Path $item.Path
        if ($actual -cne $item.Expected) {
            Add-Step -Name $item.Name -Status 'FAIL' -Details "Rollback hash mismatch: expected $($item.Expected), got $actual"
            throw "$($item.Name) rollback hash mismatch"
        }
        Add-Step -Name $item.Name -Status 'PASS' -Details "Rollback hash unchanged: $actual"
    }

    $artifactSpecs = @(
        [pscustomobject]@{ Name = 'artifact-bridge-dll'; Path = Join-Path $root 'out/build/bridge/d3d9.dll' },
        [pscustomobject]@{ Name = 'artifact-dxvk-dll'; Path = Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll' },
        [pscustomobject]@{ Name = 'artifact-split-archive'; Path = Join-Path $root "out/packages/SA-RenderStack-v$Version-split.zip" },
        [pscustomobject]@{ Name = 'artifact-sdk-archive'; Path = Join-Path $root "out/packages/SA-RenderStack-v$Version-sdk.zip" },
        [pscustomobject]@{ Name = 'artifact-symbols-archive'; Path = Join-Path $root "out/packages/SA-RenderStack-v$Version-symbols.zip" },
        [pscustomobject]@{ Name = 'artifact-source-manifest'; Path = Join-Path $root "out/packages/SA-RenderStack-v$Version-source-manifest.json" },
        [pscustomobject]@{ Name = 'artifact-split-manifest'; Path = Join-Path $root 'out/stage/split/manifest.json' }
    )
    foreach ($artifact in $artifactSpecs) {
        if (-not (Test-Path -LiteralPath $artifact.Path -PathType Leaf)) {
            throw "Release artifact is missing: $($artifact.Path)"
        }
        Add-Step -Name $artifact.Name -Status 'PASS' `
            -Details "SHA-256 $((Get-FileSha256 -Path $artifact.Path))"
    }
} catch {
    $failureMessage = $_.Exception.Message
    if ($overallStatus -eq 'passed') { $overallStatus = 'failed' }
    if ($firstExitCode -eq 0) { $firstExitCode = 1 }
    if ($steps.Count -eq 0 -or $steps[$steps.Count - 1].Status -ne 'FAIL') {
        Add-Step -Name 'release-gate-abort' -Status 'FAIL' -Details $failureMessage
    }
} finally {
    try {
        $releaseVerdict = if ($overallStatus -eq 'passed') { 'PASS' } else { 'FAIL' }
        Write-ReleaseReport -Verdict $releaseVerdict
    } catch {
        if ($firstExitCode -eq 0) { $firstExitCode = 1 }
        $overallStatus = 'failed'
        Write-Error "Release report publication failed: $($_.Exception.Message)"
    }
}

if ($overallStatus -ne 'passed') {
    Write-Error "Release gate failed: $reportPath"
    exit $firstExitCode
}
Write-Output "Release gate report: $reportPath"
exit 0
