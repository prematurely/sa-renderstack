param(
    [switch]$Help,
    [string]$Configuration = 'Release',
    [string]$Architecture = 'x86',
    [string]$DxvkSource,
    [string]$BridgeSource,
    [string]$ActiveBridgeConfig,
    [string]$ActiveDxvkConfig,
    [string]$MSBuild,
    [string]$Python,
    [string]$LlvmMingwBin,
    [string]$Ninja,
    [string]$Glslang,
    [switch]$AllowNonV18MsBuild,
    [switch]$SkipLocalBridgeEvidence
)

if ($Help) {
    @'
Usage: test.ps1 [-Help] [-Configuration Release] [-Architecture x86]
       [-DxvkSource <path>] [-BridgeSource <path>]
       [-ActiveBridgeConfig <path>] [-ActiveDxvkConfig <path>]
       [-MSBuild <path>] [-Python <path>] [-LlvmMingwBin <path>]
       [-Ninja <path>] [-Glslang <path>] [-AllowNonV18MsBuild]
       [-SkipLocalBridgeEvidence]

Runs the reproducible source, build, runtime, Bridge, and export gates and
publishes out/test-results.json with durable per-test logs.
'@ | Write-Output
    exit 0
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

if ($Configuration -cne 'Release') {
    Write-Error "Unsupported configuration '$Configuration'; only Release is accepted"
    exit 2
}
if ($Architecture -cne 'x86') {
    Write-Error "Unsupported architecture '$Architecture'; only x86 is accepted"
    exit 2
}

. (Join-Path $PSScriptRoot 'lib/process-runner.ps1')
. (Join-Path $PSScriptRoot 'lib/toolchain-discovery.ps1')

$root = Get-RenderStackRepositoryRoot -CallerScriptPath $PSCommandPath
$logsRoot = Join-Path $root 'out/logs/tests'
$runParent = Join-Path $root 'out/run'
$runRoot = Join-Path $root 'out/run/task-6'
$reportPath = Join-Path $root 'out/test-results.json'
$startedUtc = [DateTime]::UtcNow
$firstExitCode = 0
$overallStatus = 'passed'
$results = [Collections.Generic.List[object]]::new()
$temporaryDirectories = [Collections.Generic.List[string]]::new()

function Get-RelativePath {
    param([Parameter(Mandatory)] [string]$Path)
    return [IO.Path]::GetRelativePath($root, (Get-RenderStackFullPath -Path $Path)).Replace('\', '/')
}

function New-TestLogPaths {
    param([Parameter(Mandatory)] [string]$Name)
    $safeName = ($Name -replace '[^A-Za-z0-9_.-]', '-')
    $directory = Join-Path $logsRoot (New-RenderStackArtifactName -Prefix $safeName -Extension '.dir')
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    return [ordered]@{
        stdout = Join-Path $directory 'stdout.log'
        stderr = Join-Path $directory 'stderr.log'
        combined = Join-Path $directory 'combined.log'
    }
}

function New-Result {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Category,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [string[]]$Command,
        [Parameter(Mandatory)] [bool]$Required,
        [Parameter(Mandatory)] [string]$Status,
        [AllowNull()] [string]$SkipReason,
        [AllowNull()] [Nullable[int64]]$ExitCode,
        [Parameter(Mandatory)] [string]$StartedUtc,
        [Parameter(Mandatory)] [string]$EndedUtc,
        [Parameter(Mandatory)] [double]$DurationMs,
        [Parameter(Mandatory)] [string]$StdoutPath,
        [Parameter(Mandatory)] [string]$StderrPath,
        [Parameter(Mandatory)] [string]$CombinedPath
    )
    return [ordered]@{
        name = $Name
        category = $Category
        command = @($Command)
        required = $Required
        status = $Status
        skipReason = $SkipReason
        exitCode = $ExitCode
        startedUtc = $StartedUtc
        endedUtc = $EndedUtc
        durationMs = [double]$DurationMs
        stdoutLogPath = Get-RelativePath -Path $StdoutPath
        stderrLogPath = Get-RelativePath -Path $StderrPath
        combinedLogPath = Get-RelativePath -Path $CombinedPath
    }
}

function Invoke-Gate {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Category,
        [Parameter(Mandatory)] [string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)] [bool]$Required,
        [string]$WorkingDirectory = $root,
        [hashtable]$EnvironmentOverrides = @{},
        [string[]]$EnvironmentRemovals = @(),
        [ValidateRange(1, 5)] [int]$MaxAttempts = 1
    )
    $paths = New-TestLogPaths -Name $Name
    foreach ($path in $paths.Values) {
        [IO.File]::WriteAllText($path, '', [Text.UTF8Encoding]::new($false))
    }
    $start = [DateTime]::UtcNow
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $nativeExitCode = $null
    $firstFailureExitCode = $null
    $status = 'failed'
    $skipReason = $null
    try {
        for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
            if ($attempt -gt 1) {
                Add-RenderStackLogText -Path $paths.combined -Text (
                    "[$Name] retry attempt=$attempt/$MaxAttempts" + [Environment]::NewLine)
            }
            try {
                $process = Invoke-RenderStackProcess -FilePath $FilePath -ArgumentList $Arguments `
                    -WorkingDirectory $WorkingDirectory -EnvironmentOverrides $EnvironmentOverrides `
                    -EnvironmentRemovals $EnvironmentRemovals -StandardOutputLogPath $paths.stdout `
                    -StandardErrorLogPath $paths.stderr -CombinedLogPath $paths.combined -Label $Name
                if ($process.ExitCode -eq 0) {
                    $nativeExitCode = 0
                    $status = 'passed'
                    break
                }
                if ($null -eq $firstFailureExitCode) {
                    $firstFailureExitCode = [int64]$process.ExitCode
                }
            } catch {
                Add-RenderStackLogText -Path $paths.stderr -Text ($_.Exception.Message + [Environment]::NewLine)
            }
            if ($attempt -lt $MaxAttempts) {
                Start-Sleep -Milliseconds 50
            }
        }
    } catch {
        $status = 'failed'
        Add-RenderStackLogText -Path $paths.stderr -Text $_.Exception.Message
    } finally {
        $stopwatch.Stop()
    }
    if ($status -eq 'failed' -and $null -ne $firstFailureExitCode) {
        $nativeExitCode = $firstFailureExitCode
    }
    if ($status -eq 'failed' -and $script:firstExitCode -eq 0) {
        $script:firstExitCode = if ($null -ne $nativeExitCode -and $nativeExitCode -ne 0) {
            $nativeExitCode
        } else { 1 }
    }
    if ($status -eq 'failed') { $script:overallStatus = 'failed' }
    $ended = [DateTime]::UtcNow
    $record = New-Result -Name $Name -Category $Category -Command (@($FilePath) + @($Arguments)) `
        -Required $Required -Status $status -SkipReason $skipReason -ExitCode $nativeExitCode `
        -StartedUtc $start.ToString('o') -EndedUtc $ended.ToString('o') `
        -DurationMs $stopwatch.Elapsed.TotalMilliseconds -StdoutPath $paths.stdout `
        -StderrPath $paths.stderr -CombinedPath $paths.combined
    [void]$results.Add($record)
    return $record
}

function Add-SkippedGate {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Category,
        [Parameter(Mandatory)] [bool]$Required,
        [Parameter(Mandatory)] [string]$Reason
    )
    $paths = New-TestLogPaths -Name $Name
    foreach ($path in $paths.Values) {
        [IO.File]::WriteAllText($path, "Skipped: $Reason`n", [Text.UTF8Encoding]::new($false))
    }
    $now = [DateTime]::UtcNow.ToString('o')
    $record = New-Result -Name $Name -Category $Category -Command @() -Required $Required `
        -Status 'skipped' -SkipReason $Reason -ExitCode $null -StartedUtc $now -EndedUtc $now `
        -DurationMs 0.0 -StdoutPath $paths.stdout -StderrPath $paths.stderr -CombinedPath $paths.combined
    [void]$results.Add($record)
    if ($Required) { $script:overallStatus = 'failed'; if ($script:firstExitCode -eq 0) { $script:firstExitCode = 1 } }
    return $record
}

function Invoke-ScriptGate {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$ScriptPath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)] [string]$Category = 'script',
        [bool]$Required = $true,
        [string]$WorkingDirectory = $root,
        [hashtable]$EnvironmentOverrides = @{},
        [string[]]$EnvironmentRemovals = @()
    )
    return Invoke-Gate -Name $Name -Category $Category -FilePath (Get-Command pwsh).Source `
        -Arguments (@('-NoProfile', '-File', $ScriptPath) + @($Arguments)) -Required $Required `
        -WorkingDirectory $WorkingDirectory `
        -EnvironmentOverrides $EnvironmentOverrides -EnvironmentRemovals $EnvironmentRemovals
}

function Get-OutputPath {
    param([Parameter(Mandatory)] [string]$RelativePath)
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required build output is missing: $RelativePath" }
    return $path
}

function Test-BuildIsCurrent {
    param([Parameter(Mandatory)] $Toolchain)
    $metadataPath = Join-Path $root 'out/build-metadata.json'
    if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) { return $false }
    try {
        $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
        $gitStatus = Invoke-RenderStackProcess -FilePath (Get-Command git.exe).Source `
            -ArgumentList @('status', '--porcelain', '--untracked-files=all') `
            -WorkingDirectory $root -Label 'metadata-worktree-status'
        if ($gitStatus.ExitCode -ne 0 -or -not [string]::IsNullOrWhiteSpace($gitStatus.StandardOutput)) {
            return $false
        }
        $commit = (Invoke-RenderStackProcess -FilePath (Get-Command git.exe).Source -ArgumentList @('rev-parse', 'HEAD') `
            -WorkingDirectory $root -Label 'metadata-commit').StandardOutput.Trim()
        if ($metadata.schema -ne 1 -or $metadata.commit -cne $commit -or
            $metadata.configuration -cne $Configuration -or $metadata.architecture -cne $Architecture -or
            $metadata.component -cne 'All' -or $metadata.exitCode -ne 0) { return $false }
        foreach ($pair in @(
                [pscustomobject]@{ Actual = $Toolchain.Python; Recorded = $metadata.tools.python; PathProperty = 'Path'; Name = 'python' },
                [pscustomobject]@{ Actual = $Toolchain.LlvmMingw; Recorded = $metadata.tools.llvmMingw; PathProperty = 'BinPath'; Name = 'llvmMingw' },
                [pscustomobject]@{ Actual = $Toolchain.Ninja; Recorded = $metadata.tools.ninja; PathProperty = 'Path'; Name = 'ninja' },
                [pscustomobject]@{ Actual = $Toolchain.Glslang; Recorded = $metadata.tools.glslang; PathProperty = 'Path'; Name = 'glslang' })) {
            if ($null -eq $pair.Actual -or $null -eq $pair.Recorded -or
                [string]$pair.Actual.Version -cne [string]$pair.Recorded.Version -or
                [string]$pair.Actual.($pair.PathProperty) -cne [string]$pair.Recorded.($pair.PathProperty)) {
                return $false
            }
        }
        if ($null -eq $metadata.tools.meson -or
            [string]$metadata.tools.meson.version -cne '1.11.1' -or
            -not (Test-Path -LiteralPath ([string]$metadata.tools.meson.moduleDirectory) -PathType Container)) {
            return $false
        }
        foreach ($output in @($metadata.outputs)) {
            $path = Join-Path $root ([string]$output.path)
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
            $item = Get-Item -LiteralPath $path
            if ([int64]$output.size -ne $item.Length -or
                $output.sha256 -cne (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToUpperInvariant()) { return $false }
        }
        return $true
    } catch { return $false }
}

function Get-MesonEnvironment {
    param([Parameter(Mandatory)] $Toolchain, [Parameter(Mandatory)] [string]$PythonPath, [string]$ModuleDirectory)
    $directories = @(
        $Toolchain.LlvmMingw.BinPath,
        (Split-Path -Parent $Toolchain.Ninja.Path),
        (Split-Path -Parent $Toolchain.Glslang.Path),
        (Split-Path -Parent $PythonPath),
        (Split-Path -Parent (Get-Command git.exe).Source),
        (Join-Path $env:SystemRoot 'System32'),
        (Join-Path $env:SystemRoot 'System32/Wbem')
    ) | Select-Object -Unique
    return @{
        PATH = ($directories -join [IO.Path]::PathSeparator)
        PYTHONPATH = $ModuleDirectory
        PYTHONDONTWRITEBYTECODE = '1'
    }
}

function New-TestRunDirectory {
    param([Parameter(Mandatory)] [string]$Name)

    $directory = Join-Path $runRoot $Name
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    [void]$temporaryDirectories.Add($directory)
    return $directory
}

function Reset-TestRunRoot {
    $safeRunParent = Assert-RenderStackPathUnder -Path $runParent -Base (Join-Path $root 'out') `
        -Description 'Task 6 run parent'
    Assert-RenderStackNoReparsePath -Path $safeRunParent -Anchor (Join-Path $root 'out') `
        -Description 'Task 6 run parent' | Out-Null
    if (-not (Test-Path -LiteralPath $safeRunParent -PathType Container)) {
        New-Item -ItemType Directory -Path $safeRunParent -Force | Out-Null
    }
    Assert-RenderStackNoReparsePath -Path $safeRunParent -Anchor (Join-Path $root 'out') `
        -Description 'Task 6 run parent' | Out-Null

    $state = Get-RenderStackPathState -Path $runRoot
    if ($state.Exists) {
        if (($state.Attributes -band [IO.FileAttributes]::Directory) -eq 0) {
            throw "Task 6 run root is not a directory: $runRoot"
        }
        Assert-RenderStackNoReparseTree -Path $runRoot -Anchor $safeRunParent `
            -Description 'Task 6 run root' | Out-Null
        Remove-Item -LiteralPath $runRoot -Recurse -Force -ErrorAction Stop
    }
    New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
    Assert-RenderStackNoReparsePath -Path $runRoot -Anchor $safeRunParent `
        -Description 'Task 6 run root' | Out-Null
}

function Add-TargetAndRegistrationGates {
    param([Parameter(Mandatory)] $Toolchain, [Parameter(Mandatory)] [string]$PythonPath,
        [Parameter(Mandatory)] [hashtable]$MesonEnvironment)
    $build = Join-Path $root 'out/build/dxvk-x86'
    $targetInfo = Join-Path $build 'meson-info/intro-targets.json'
    $testInfo = Join-Path $build 'meson-info/intro-tests.json'
    foreach ($path in @($targetInfo, $testInfo)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Meson introspection file is missing: $path" } }
    $targets = @(Get-Content -LiteralPath $targetInfo -Raw | ConvertFrom-Json)
    $expectedTargets = @('renderstack-d3d9-batch-audit-test', 'renderstack-d3d9-deferred-shader-binding-test',
        'renderstack-dxvk-state-dedup-test', 'renderstack-stateblock-prefilter-probe', 'renderstack-gta-sa-compat-probe')
    $actualTargets = @($targets | Where-Object { $_.name -in $expectedTargets } | ForEach-Object { $_.name })
    if (@(Compare-Object ($expectedTargets | Sort-Object) ($actualTargets | Sort-Object)).Count -ne 0 -or @($actualTargets).Count -ne 5) {
        throw 'Meson target introspection did not resolve exactly five required probe targets'
    }
    foreach ($target in $targets | Where-Object { $_.name -in $expectedTargets }) {
        if ($target.type -cne 'executable' -or @($target.filename).Count -ne 1) { throw "Invalid introspection record for $($target.name)" }
        $path = Get-RenderStackFullPath -Path $target.filename[0]
        if (-not $path.StartsWith((Get-RenderStackFullPath -Path $build) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Introspection output is outside build directory: $path"
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Introspected target output is missing: $path" }
    }
    $registered = @(Get-Content -LiteralPath $testInfo -Raw | ConvertFrom-Json)
    $expectedTests = $expectedTargets[0..2]
    $actualTests = @($registered | ForEach-Object { $_.name })
    if (@(Compare-Object ($expectedTests | Sort-Object) ($actualTests | Sort-Object)).Count -ne 0 -or @($actualTests).Count -ne 3) {
        throw 'Meson introspection did not report exactly three registered tests'
    }
    foreach ($test in $registered) {
        if (@($test.cmd).Count -lt 1 -or -not (Test-Path -LiteralPath (Get-RenderStackFullPath -Path $test.cmd[0]) -PathType Leaf)) {
            throw "Invalid registered Meson test command: $($test.name)"
        }
    }
    $mesonTestDirectory = New-TestRunDirectory -Name 'meson-test'
    [void](Invoke-Gate -Name 'meson-test' -Category 'meson' -FilePath $PythonPath `
        -Arguments @('-m', 'mesonbuild.mesonmain', 'test', '-C', $build, '--no-rebuild', '--print-errorlogs') `
        -Required $true -WorkingDirectory $mesonTestDirectory -EnvironmentOverrides $MesonEnvironment)
}

function Add-RuntimeProbeGates {
    param([Parameter(Mandatory)] [string]$Backend, [Parameter(Mandatory)] [string]$CompatProbe,
        [Parameter(Mandatory)] [string]$StateProbe)
    $compatDir = Join-Path $runRoot 'no-dxvk.conf-compat'
    $stateDir = Join-Path $runRoot 'no-dxvk.conf-stateblock'
    foreach ($directory in @($compatDir, $stateDir)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        [void]$temporaryDirectories.Add($directory)
    }
    $stateConfig = Join-Path $stateDir 'stateblock.conf'
    [IO.File]::WriteAllText($stateConfig, "d3d9.gtaSaCompat = True`nd3d9.gtaSaStateBlockPrefilter = True`nd3d9.presentInterval = 0`ndxgi.syncInterval = 0`n", [Text.UTF8Encoding]::new($false))
    [void](Invoke-Gate -Name 'runtime-compatibility-probe' -Category 'runtime' -FilePath $CompatProbe `
        -Arguments @($Backend, '--force') -Required $true -WorkingDirectory $compatDir `
        -EnvironmentRemovals @('DXVK_CONFIG', 'DXVK_CONFIG_FILE'))
    [void](Invoke-Gate -Name 'runtime-stateblock-prefilter-probe' -Category 'runtime' -FilePath $StateProbe `
        -Arguments @($Backend) -Required $true -EnvironmentOverrides @{ DXVK_CONFIG_FILE = $stateConfig } `
        -WorkingDirectory $stateDir -EnvironmentRemovals @('DXVK_CONFIG'))
}

function Add-BridgeRuntimeGates {
    param([Parameter(Mandatory)] [string]$Backend)
    $bridgeTests = Join-Path $root 'out/build/bridge-tests'
    $runs = @(
        @{ Name = 'bridge-statejournal-local'; File = 'ProperShadersStateJournalTests.exe'; Args = @('--local', $Backend) },
        @{ Name = 'bridge-statejournal-native'; File = 'ProperShadersStateJournalTests.exe'; Args = @($Backend) },
        @{ Name = 'bridge-batch-policy'; File = 'ProperShadersBatchPolicyTests.exe'; Args = @() },
        @{ Name = 'bridge-adapter-config'; File = 'PerformanceAdapterConfigTests.exe'; Args = @() },
        @{ Name = 'bridge-adapters'; File = 'PerformanceAdaptersTests.exe'; Args = @() },
        @{ Name = 'bridge-api-smoke'; File = 'GtaSaCompatApi3Smoke.exe'; Args = @($Backend) }
    )
    foreach ($run in $runs) {
        $runDirectory = New-TestRunDirectory -Name $run.Name
        $result = Invoke-Gate -Name $run.Name -Category 'bridge-runtime' `
            -FilePath (Join-Path $bridgeTests $run.File) -Arguments $run.Args -Required $true `
            -WorkingDirectory $runDirectory `
            -EnvironmentRemovals @('DXVK_CONFIG', 'DXVK_CONFIG_FILE')
        if ($run.Name -eq 'bridge-api-smoke' -and $result.status -eq 'passed') {
            $log = Get-Content -LiteralPath (Join-Path $root $result.combinedLogPath) -Raw
            if ($log -notmatch 'api=7 flags=0x00000FFF') {
                $result.status = 'failed'
                $script:overallStatus = 'failed'
                if ($script:firstExitCode -eq 0) { $script:firstExitCode = 1 }
                throw 'Bridge API smoke did not report API v7 and flags 0x00000FFF'
            }
        }
    }
}

function Remove-TemporaryRunDirectories {
    foreach ($directory in @($temporaryDirectories | Sort-Object Length -Descending)) {
        try {
            if (Test-Path -LiteralPath $directory) {
                Assert-RenderStackNoReparseTree -Path $directory -Anchor $runRoot -Description 'Task 6 temporary run directory' | Out-Null
                Remove-Item -LiteralPath $directory -Recurse -Force
            }
        } catch {
            if ($script:firstExitCode -eq 0) { $script:firstExitCode = 1 }
            $script:overallStatus = 'failed'
            Write-Error "Temporary run cleanup failed: $($_.Exception.Message)"
        }
    }
    try {
        if (Test-Path -LiteralPath $runRoot) {
            Assert-RenderStackNoReparseTree -Path $runRoot -Anchor $runParent `
                -Description 'Task 6 run root cleanup' | Out-Null
            Remove-Item -LiteralPath $runRoot -Recurse -Force -ErrorAction Stop
        }
    } catch {
        if ($script:firstExitCode -eq 0) { $script:firstExitCode = 1 }
        $script:overallStatus = 'failed'
        Write-Error "Task 6 run root cleanup failed: $($_.Exception.Message)"
    }
}

try {
    New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null
    Reset-TestRunRoot

    $scriptsRoot = Join-Path $root 'tests'
    $layout = Invoke-ScriptGate -Name 'repository-layout' -ScriptPath (Join-Path $scriptsRoot 'repository-layout-test.ps1') -Category 'layout'
    [void](Invoke-ScriptGate -Name 'windows-ci-workflow' -ScriptPath (Join-Path $scriptsRoot 'windows-ci-workflow-test.ps1') -Category 'automation')
    $msbuildCompatibilityArguments = if ($AllowNonV18MsBuild) { @('-AllowNonV18MsBuild') } else { @() }
    [void](Invoke-ScriptGate -Name 'msbuild-compatibility' -ScriptPath (Join-Path $scriptsRoot 'msbuild-compatibility-regression-test.ps1') `
        -Arguments $msbuildCompatibilityArguments -Category 'build')
    $hygiene = Invoke-ScriptGate -Name 'source-hygiene-regression' -ScriptPath (Join-Path $scriptsRoot 'source-hygiene-regression-test.ps1') -Category 'hygiene'
    [void](Invoke-ScriptGate -Name 'dxvk-provenance' -ScriptPath (Join-Path $scriptsRoot 'dxvk-provenance-test.ps1') -Category 'provenance')
    [void](Invoke-ScriptGate -Name 'dxvk-dependency-provenance' -ScriptPath (Join-Path $scriptsRoot 'dxvk-dependency-provenance-test.ps1') -Category 'provenance')
    [void](Invoke-ScriptGate -Name 'historical-dxvk-overlay-manifest' -ScriptPath (Join-Path $scriptsRoot 'dxvk-overlay-manifest-test.ps1') -Category 'historical')
    [void](Invoke-ScriptGate -Name 'historical-dxvk-overlay-regression' -ScriptPath (Join-Path $scriptsRoot 'dxvk-overlay-regression-test.ps1') -Category 'historical')
    [void](Invoke-ScriptGate -Name 'historical-bridge-manifest' -ScriptPath (Join-Path $scriptsRoot 'bridge-migration-manifest-test.ps1') -Category 'historical')
    [void](Invoke-ScriptGate -Name 'historical-bridge-evidence' -ScriptPath (Join-Path $scriptsRoot 'bridge-build-evidence-test.ps1') -Category 'historical')
    if ($PSBoundParameters.ContainsKey('DxvkSource')) {
        [void](Invoke-ScriptGate -Name 'live-dxvk-overlay-manifest' -ScriptPath (Join-Path $scriptsRoot 'dxvk-overlay-manifest-test.ps1') -Arguments @('-Source', $DxvkSource) -Category 'live' -Required $false)
        [void](Invoke-ScriptGate -Name 'live-dxvk-overlay-regression' -ScriptPath (Join-Path $scriptsRoot 'dxvk-overlay-regression-test.ps1') -Arguments @('-Source', $DxvkSource) -Category 'live' -Required $false)
    } else {
        [void](Add-SkippedGate -Name 'live-dxvk-audit' -Category 'live' -Required $false -Reason 'External DXVK source was not supplied')
    }
    $liveBridge = $PSBoundParameters.ContainsKey('BridgeSource') -or $PSBoundParameters.ContainsKey('ActiveBridgeConfig') -or $PSBoundParameters.ContainsKey('ActiveDxvkConfig')
    if ($liveBridge -and -not ($PSBoundParameters.ContainsKey('BridgeSource') -and $PSBoundParameters.ContainsKey('ActiveBridgeConfig') -and $PSBoundParameters.ContainsKey('ActiveDxvkConfig'))) {
        throw 'Live Bridge audit requires -BridgeSource, -ActiveBridgeConfig, and -ActiveDxvkConfig together'
    }
    if ($liveBridge) {
        [void](Invoke-ScriptGate -Name 'live-bridge-config-audit' -ScriptPath (Join-Path $scriptsRoot 'bridge-migration-manifest-test.ps1') `
            -Arguments @('-BridgeSource', $BridgeSource, '-ActiveBridgeConfig', $ActiveBridgeConfig, '-ActiveDxvkConfig', $ActiveDxvkConfig) -Category 'live' -Required $false)
    } else {
        [void](Add-SkippedGate -Name 'live-bridge-config-audit' -Category 'live' -Required $false -Reason 'External Bridge and config paths were not supplied')
    }
    [void](Invoke-ScriptGate -Name 'historical-baseline-regression' -ScriptPath (Join-Path $scriptsRoot 'historical-baseline-regression-test.ps1') -Category 'regression')
    [void](Invoke-ScriptGate -Name 'dxvk-overlay-regression-focused' -ScriptPath (Join-Path $scriptsRoot 'dxvk-overlay-regression-test.ps1') -Category 'regression')
    [void](Invoke-ScriptGate -Name 'bridge-importer-path-safety' -ScriptPath (Join-Path $scriptsRoot 'bridge-importer-path-safety-test.ps1') -Category 'bridge-regression')
    [void](Invoke-ScriptGate -Name 'bridge-config-regression' -ScriptPath (Join-Path $scriptsRoot 'import-audited-config-regression-test.ps1') -Category 'bridge-regression')
    [void](Invoke-ScriptGate -Name 'd3dx-preparation-regression' -ScriptPath (Join-Path $scriptsRoot 'prepare-dxsdk-regression-test.ps1') -Category 'bridge-regression')
    [void](Invoke-ScriptGate -Name 'bridge-self-contained' -ScriptPath (Join-Path $scriptsRoot 'bridge-self-contained-source-test.ps1') -Category 'bridge-regression')
    $currentEvidence = Join-Path $root 'out/reports/task-6'
    New-Item -ItemType Directory -Path $currentEvidence -Force | Out-Null
    if ($SkipLocalBridgeEvidence) {
        [void](Add-SkippedGate -Name 'current-bridge-evidence' -Category 'evidence' -Required $false `
            -Reason 'Local GTA game-root Bridge reference was not supplied')
        [void](Add-SkippedGate -Name 'bridge-evidence-types-regression' -Category 'bridge-regression' -Required $false `
            -Reason 'Current Bridge evidence is unavailable without the local GTA game-root reference')
    } else {
        [void](Invoke-ScriptGate -Name 'current-bridge-evidence' -ScriptPath (Join-Path $root 'tools/migration/verify-bridge-baseline.ps1') `
            -Arguments @('-Reference', (Join-Path (Split-Path -Parent (Split-Path -Parent $root)) 'd3d9.dll'), '-Candidate', (Get-OutputPath -RelativePath 'out/build/bridge/d3d9.dll'),
                '-ExportsOut', (Join-Path $currentEvidence 'bridge-exports.txt'), '-EvidenceOut', (Join-Path $currentEvidence 'bridge-build-evidence.json')) -Category 'evidence')
        [void](Invoke-ScriptGate -Name 'bridge-evidence-types-regression' -ScriptPath (Join-Path $scriptsRoot 'bridge-build-evidence-types-regression-test.ps1') `
            -Arguments @('-ExportsPath', (Join-Path $currentEvidence 'bridge-exports.txt'),
                '-EvidencePath', (Join-Path $currentEvidence 'bridge-build-evidence.json'),
                '-Candidate', (Get-OutputPath -RelativePath 'out/build/bridge/d3d9.dll')) -Category 'bridge-regression')
    }
    [void](Invoke-ScriptGate -Name 'backend-api-source' -ScriptPath (Join-Path $scriptsRoot 'backend-api-source-test.ps1') -Category 'api')

    $toolchain = Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -MSBuildPath $MSBuild -PythonPath $Python `
        -LlvmMingwBin $LlvmMingwBin -NinjaPath $Ninja -GlslangPath $Glslang
    $pythonPath = $toolchain.Python.Path
    $buildCurrent = Test-BuildIsCurrent -Toolchain $toolchain
    if (-not $buildCurrent) {
        $buildArguments = [Collections.Generic.List[string]]::new()
        foreach ($entry in @(
                [pscustomobject]@{ Name = '-MSBuild'; Value = $MSBuild },
                [pscustomobject]@{ Name = '-Python'; Value = $Python },
                [pscustomobject]@{ Name = '-LlvmMingwBin'; Value = $LlvmMingwBin },
                [pscustomobject]@{ Name = '-Ninja'; Value = $Ninja },
                [pscustomobject]@{ Name = '-Glslang'; Value = $Glslang })) {
            if (-not [string]::IsNullOrWhiteSpace($entry.Value)) {
                [void]$buildArguments.Add($entry.Name)
                [void]$buildArguments.Add($entry.Value)
            }
        }
        [void](Invoke-Gate -Name 'build-refresh' -Category 'build' -FilePath (Get-Command pwsh).Source `
            -Arguments (@('-NoProfile', '-File', (Join-Path $root 'tools/build.ps1'), '-Configuration', $Configuration,
                '-Architecture', $Architecture, '-Component', 'All') +
                $(if ($AllowNonV18MsBuild) { @('-AllowNonV18MsBuild') } else { @() }) +
                $buildArguments.ToArray()) `
            -Required $true)
    } else {
        [void](Add-SkippedGate -Name 'build-refresh' -Category 'build' -Required $false -Reason 'Current Task 6A build metadata and output hashes are valid')
    }
    $toolchain = Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -MSBuildPath $MSBuild -PythonPath $Python `
        -LlvmMingwBin $LlvmMingwBin -NinjaPath $Ninja -GlslangPath $Glslang `
        -AllowNonV18MsBuild:$AllowNonV18MsBuild
    $metadata = Get-Content -LiteralPath (Join-Path $root 'out/build-metadata.json') -Raw | ConvertFrom-Json
    $moduleDirectory = [string]$metadata.tools.meson.moduleDirectory
    $mesonEnvironment = Get-MesonEnvironment -Toolchain $toolchain -PythonPath $pythonPath -ModuleDirectory $moduleDirectory
    Add-TargetAndRegistrationGates -Toolchain $toolchain -PythonPath $pythonPath -MesonEnvironment $mesonEnvironment
    [void](Invoke-Gate -Name 'backend-api-msvc-executable' -Category 'api' -FilePath (Get-OutputPath -RelativePath 'out/build/sdk-tests/backend-api-layout-test.exe') -Required $true)
    $llvmSyntaxDirectory = New-TestRunDirectory -Name 'backend-api-llvm-syntax'
    [void](Invoke-Gate -Name 'backend-api-llvm-syntax' -Category 'api' -FilePath $toolchain.LlvmMingw.CompilerPath `
        -Arguments @('-std=c++17', '-fsyntax-only', (Join-Path $root 'tests/backend-api-layout-test.cpp'), '-I', (Join-Path $root 'sdk/include'), '-I', (Join-Path $root 'backend/dxvk/include/vulkan/include')) `
        -Required $true -WorkingDirectory $llvmSyntaxDirectory -MaxAttempts 3)

    $compatProbe = (Get-Content -LiteralPath (Join-Path $root 'out/build/dxvk-x86/meson-info/intro-targets.json') -Raw | ConvertFrom-Json | Where-Object name -eq 'renderstack-gta-sa-compat-probe').filename[0]
    $stateProbe = (Get-Content -LiteralPath (Join-Path $root 'out/build/dxvk-x86/meson-info/intro-targets.json') -Raw | ConvertFrom-Json | Where-Object name -eq 'renderstack-stateblock-prefilter-probe').filename[0]
    Add-RuntimeProbeGates -Backend (Get-OutputPath -RelativePath 'out/build/dxvk-x86/src/d3d9/d3d9.dll') -CompatProbe $compatProbe -StateProbe $stateProbe
    Add-BridgeRuntimeGates -Backend (Get-OutputPath -RelativePath 'out/build/dxvk-x86/src/d3d9/d3d9.dll')

    $exportArguments = [Collections.Generic.List[string]]::new()
    foreach ($argument in @(
            '-Configuration', $Configuration,
            '-Architecture', $Architecture,
            '-BridgePath', (Get-OutputPath -RelativePath 'out/build/bridge/d3d9.dll'),
            '-DxvkPath', (Get-OutputPath -RelativePath 'out/build/dxvk-x86/src/d3d9/d3d9.dll'))) {
        [void]$exportArguments.Add([string]$argument)
    }
    if (-not [string]::IsNullOrWhiteSpace($LlvmMingwBin)) {
        [void]$exportArguments.Add('-LlvmMingwBin')
        [void]$exportArguments.Add($LlvmMingwBin)
    }
    [void](Invoke-ScriptGate -Name 'export-verification' -ScriptPath (Join-Path $root 'tools/verify-exports.ps1') `
        -Arguments $exportArguments.ToArray() -Category 'exports')
    [void](Invoke-ScriptGate -Name 'final-source-hygiene' -ScriptPath (Join-Path $root 'tools/verify-source-tree.ps1') -Category 'final')
} catch {
    $overallStatus = 'failed'
    if ($firstExitCode -eq 0) { $firstExitCode = 1 }
    Write-Error $_.Exception.Message
} finally {
    try { Remove-TemporaryRunDirectories } catch { $overallStatus = 'failed'; if ($firstExitCode -eq 0) { $firstExitCode = 1 } }
    $endedUtc = [DateTime]::UtcNow
    $publishedCommit = 'unavailable'
    try {
        $publishedCommit = (Invoke-RenderStackProcess -FilePath (Get-Command git.exe).Source -ArgumentList @('rev-parse', 'HEAD') `
            -WorkingDirectory $root -Label 'result-commit').StandardOutput.Trim()
    } catch { }
    $report = [ordered]@{
        schemaVersion = 1
        repositoryCommit = $publishedCommit
        configuration = $Configuration
        architecture = $Architecture
        startedUtc = $startedUtc.ToString('o')
        endedUtc = $endedUtc.ToString('o')
        overallStatus = $overallStatus
        results = @($results)
    }
    try { Write-RenderStackAtomicJson -Path $reportPath -Value $report } catch {
        $overallStatus = 'failed'
        if ($firstExitCode -eq 0) { $firstExitCode = 1 }
        Write-Error "Test result publication failed: $($_.Exception.Message)"
    }
}

if ($overallStatus -eq 'failed') {
    Write-Error "Test orchestration failed; results: $reportPath"
    exit $firstExitCode
}
Write-Output "Test results: $reportPath"
exit 0
