param(
    [ValidateSet('All', 'Help', 'Validation', 'Discovery', 'SafeClean', 'MesonCache', 'MesonConcurrency')]
    [string]$Case = 'All',
    [string]$ValidWheelPath,
    [string]$PythonPath,
    [string]$MSBuildPath,
    [string]$LlvmMingwBin
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
$prepareMesonScript = Join-Path $root 'tools/prepare-meson.ps1'
$buildScript = Join-Path $root 'tools/build.ps1'
$processRunnerScript = Join-Path $root 'tools/lib/process-runner.ps1'
$toolchainDiscoveryScript = Join-Path $root 'tools/lib/toolchain-discovery.ps1'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) "sa-renderstack-orchestration-$([Guid]::NewGuid().ToString('N'))"
$expectedWheelSha256 = '9B3A023657E393DBC5335B95C561337D49B7A458F5541E47EC44F2CC566E0D80'

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$ArgumentList,
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [hashtable]$Environment = @{}
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    foreach ($argument in $ArgumentList) {
        [void]$startInfo.ArgumentList.Add($argument)
    }
    $startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
    foreach ($entry in $Environment.GetEnumerator()) {
        $startInfo.Environment[[string]$entry.Key] = [string]$entry.Value
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw "Failed to start process: $FilePath"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            StdOut = $stdout
            StdErr = $stderr
            Output = ($stdout + $stderr)
        }
    } finally {
        $process.Dispose()
    }
}

function Get-OutSnapshot {
    $out = Join-Path $root 'out'
    if (-not (Test-Path -LiteralPath $out)) {
        return '<absent>'
    }

    return (@(Get-ChildItem -LiteralPath $out -Force -Recurse | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($out.Length)
        if ($_.PSIsContainer) {
            "directory|$relative"
        } else {
            "file|$relative|$($_.LastWriteTimeUtc.Ticks)|$($_.Length)"
        }
    }) -join "`n")
}

function Assert-HelpCase {
    foreach ($script in @($prepareMesonScript, $buildScript)) {
        if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
            throw "Required orchestration script is missing: $script"
        }
    }

    $outside = Join-Path $fixtureRoot 'outside-help'
    New-Item -ItemType Directory -Path $outside -Force | Out-Null
    $before = Get-OutSnapshot
    foreach ($script in @($prepareMesonScript, $buildScript)) {
        $result = Invoke-CapturedProcess -FilePath (Get-Command pwsh).Source `
            -ArgumentList @('-NoProfile', '-File', $script, '-Help') -WorkingDirectory $outside
        if ($result.ExitCode -ne 0 -or $result.Output -notmatch '(?i)usage:') {
            throw "Help failed for $script with exit $($result.ExitCode): $($result.Output)"
        }
    }
    $after = Get-OutSnapshot
    if ($after -cne $before) {
        $difference = Compare-Object ($before -split "`n") ($after -split "`n") |
            Format-Table -AutoSize | Out-String
        throw "Help changed the repository out tree:`n$difference"
    }
    Write-Output 'PASS orchestration help is side-effect free outside the repository'
}

function Assert-ValidationCase {
    $outside = Join-Path $fixtureRoot 'outside-validation'
    New-Item -ItemType Directory -Path $outside -Force | Out-Null
    $missing = Join-Path $fixtureRoot 'must-not-be-probed.exe'
    $environment = @{
        SA_RENDERSTACK_MSBUILD = $missing
        SA_RENDERSTACK_PYTHON = $missing
        SA_RENDERSTACK_LLVM_MINGW_BIN = $missing
        SA_RENDERSTACK_NINJA = $missing
        SA_RENDERSTACK_GLSLANG = $missing
    }
    $cases = @(
        @{ Arguments = @('-Configuration', 'Debug'); Expected = 'Release' },
        @{ Arguments = @('-Architecture', 'x64'); Expected = 'x86' },
        @{ Arguments = @('-Component', 'Invalid'); Expected = 'All|Bridge|Dxvk' }
    )
    $before = Get-OutSnapshot
    foreach ($validationCase in $cases) {
        $result = Invoke-CapturedProcess -FilePath (Get-Command pwsh).Source `
            -ArgumentList (@('-NoProfile', '-File', $buildScript) + $validationCase.Arguments) `
            -WorkingDirectory $outside -Environment $environment
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch $validationCase.Expected) {
            throw "Invalid build arguments were not rejected correctly: $($result.Output)"
        }
        if ($result.Output -match [regex]::Escape($missing)) {
            throw "Tool discovery ran before argument rejection: $($result.Output)"
        }
    }
    if ((Get-OutSnapshot) -cne $before) {
        throw 'Rejected build arguments changed the repository out tree'
    }
    Write-Output 'PASS invalid configuration, architecture, and component fail before discovery'
}

function Get-KnownToolPath {
    param([Parameter(Mandatory)] [string[]]$Candidates)

    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    throw "Required regression-test tool is unavailable: $($Candidates -join ', ')"
}

function Assert-DiscoveryCase {
    . $processRunnerScript
    . $toolchainDiscoveryScript

    $msbuildInfo = Find-RenderStackMSBuild -MsBuildPath $MSBuildPath -RepoRoot $root
    $msbuild = $msbuildInfo.Path
    $python = if (-not [string]::IsNullOrWhiteSpace($PythonPath)) {
        Get-KnownToolPath -Candidates @($PythonPath)
    } else {
        (Find-RenderStackPython -RepoRoot $root).Path
    }
    $llvmInfo = Find-RenderStackLlvmMingw -LlvmMingwBin $LlvmMingwBin -RepoRoot $root
    $llvmBin = $llvmInfo.BinPath
    $ninja = Get-KnownToolPath -Candidates @('C:\msys64\mingw32\bin\ninja.exe')
    $glslang = Get-KnownToolPath -Candidates @('C:\msys64\mingw64\bin\glslangValidator.exe')
    if ($msbuildInfo.ProductMajor -ne 18 -or $msbuildInfo.HostArchitecture -cne 'amd64' -or
        $msbuildInfo.Path -notmatch '(?i)[\\/]MSBuild[\\/]Current[\\/]Bin[\\/]amd64[\\/]MSBuild\.exe$') {
        throw "MSBuild discovery did not prove VS18 HostX64: $($msbuildInfo | ConvertTo-Json -Compress)"
    }
    if ($llvmInfo.Source -notin @('parameter', 'environment:SA_RENDERSTACK_LLVM_MINGW_BIN', 'local-workspace-fallback')) {
        throw "LLVM-MinGW discovery returned an unexpected source: $($llvmInfo.Source)"
    }
    $relocatedRepository = Join-Path $fixtureRoot 'relocated-workspace/repository'
    $relocatedToolchains = Join-Path $relocatedRepository 'toolchains'
    New-Item -ItemType Directory -Path $relocatedToolchains -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'toolchains/llvm-mingw-i686.ini') `
        -Destination (Join-Path $relocatedToolchains 'llvm-mingw-i686.ini')
    $relocatedLlvm = Find-RenderStackLlvmMingw -LlvmMingwBin $llvmBin -RepoRoot $relocatedRepository
    if ($relocatedLlvm.BinPath -cne $llvmBin -or $relocatedLlvm.Source -cne 'parameter') {
        throw 'Relocated repository did not honor the explicit LLVM-MinGW override'
    }

    $missing = Join-Path $fixtureRoot 'unrelated-tool-must-not-be-read'
    $saved = @{}
    foreach ($name in @('SA_RENDERSTACK_MSBUILD', 'SA_RENDERSTACK_PYTHON',
            'SA_RENDERSTACK_LLVM_MINGW_BIN', 'SA_RENDERSTACK_NINJA', 'SA_RENDERSTACK_GLSLANG')) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        $env:SA_RENDERSTACK_MSBUILD = $missing
        $dxvk = Get-RenderStackToolchain -RepoRoot $root -Component Dxvk `
            -PythonPath $python -LlvmMingwBin $llvmBin -NinjaPath $ninja -GlslangPath $glslang
        if ($null -ne $dxvk.MSBuild -or $dxvk.Python.Path -cne $python) {
            throw 'DXVK discovery probed or returned an unrelated Bridge tool'
        }

        $env:SA_RENDERSTACK_PYTHON = $missing
        $env:SA_RENDERSTACK_LLVM_MINGW_BIN = $missing
        $env:SA_RENDERSTACK_NINJA = $missing
        $env:SA_RENDERSTACK_GLSLANG = $missing
        $bridge = Get-RenderStackToolchain -RepoRoot $root -Component Bridge -MsBuildPath $msbuild
        if ($bridge.MSBuild.Path -cne $msbuild -or $null -ne $bridge.Python -or $null -ne $bridge.LlvmMingw) {
            throw 'Bridge discovery probed or returned unrelated DXVK tools'
        }

        $nonAmd64 = Join-Path (Split-Path -Parent (Split-Path -Parent $msbuild)) 'MSBuild.exe'
        if (Test-Path -LiteralPath $nonAmd64 -PathType Leaf) {
            $rejected = $false
            try {
                Find-RenderStackMSBuild -MsBuildPath $nonAmd64 -RepoRoot $root | Out-Null
            } catch {
                $rejected = $_.Exception.Message -match 'amd64|HostX64|Visual Studio 18 BuildTools'
            }
            if (-not $rejected) {
                throw "Non-amd64 MSBuild override was accepted: $nonAmd64"
            }
        }

        $fakeVs17 = Join-Path $fixtureRoot 'Microsoft Visual Studio/17/BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe'
        New-Item -ItemType Directory -Path (Split-Path -Parent $fakeVs17) -Force | Out-Null
        [IO.File]::WriteAllText($fakeVs17, 'not-msbuild')
        $vs17Rejected = $false
        try {
            Find-RenderStackMSBuild -MsBuildPath $fakeVs17 -RepoRoot $root | Out-Null
        } catch {
            $vs17Rejected = $_.Exception.Message -match 'Visual Studio 18 BuildTools|product major 18|amd64'
        }
        if (-not $vs17Rejected) {
            throw 'VS17-shaped MSBuild override was accepted'
        }

        $missingLlvm = Join-Path $fixtureRoot 'missing-llvm/bin'
        $missingRejected = $false
        try {
            Find-RenderStackLlvmMingw -LlvmMingwBin $missingLlvm -RepoRoot $root | Out-Null
        } catch {
            $missingRejected = $_.Exception.Message -match 'does not exist'
        }
        if (-not $missingRejected) {
            throw 'Missing LLVM-MinGW override was accepted'
        }

        $invalidLlvm = Join-Path $fixtureRoot 'invalid-llvm/bin'
        New-Item -ItemType Directory -Path $invalidLlvm -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $invalidLlvm 'i686-w64-mingw32-clang++.exe'), 'incomplete')
        $invalidRejected = $false
        try {
            Find-RenderStackLlvmMingw -LlvmMingwBin $invalidLlvm -RepoRoot $root | Out-Null
        } catch {
            $invalidRejected = $_.Exception.Message -match 'incomplete'
        }
        if (-not $invalidRejected) {
            throw 'Incomplete LLVM-MinGW override was accepted'
        }
    } finally {
        foreach ($name in $saved.Keys) {
            [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
        }
    }
    Write-Output 'PASS discovery enforces VS18 HostX64, portable LLVM overrides, and component isolation'
}

function Assert-SafeCleanCase {
    . $processRunnerScript

    $repository = Join-Path $fixtureRoot 'safe-clean'
    $buildRoot = Join-Path $repository 'out/build'
    $selected = Join-Path $buildRoot 'dxvk-x86'
    $outside = Join-Path $repository 'outside'
    $sentinel = Join-Path $outside 'sentinel.txt'
    New-Item -ItemType Directory -Path $selected -Force | Out-Null
    New-Item -ItemType Directory -Path $outside -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $selected 'remove.txt'), 'remove')
    [IO.File]::WriteAllText($sentinel, 'keep')

    Remove-RenderStackBuildPath -Path $selected -RepoRoot $repository -BuildRoot $buildRoot -AllowedName 'dxvk-x86'
    if (Test-Path -LiteralPath $selected) {
        throw 'Selected clean directory remains'
    }

    $outsideRejected = $false
    try {
        Remove-RenderStackBuildPath -Path $outside -RepoRoot $repository -BuildRoot $buildRoot -AllowedName 'dxvk-x86'
    } catch {
        $outsideRejected = $_.Exception.Message -match 'outside|allowlist'
    }
    if (-not $outsideRejected -or [IO.File]::ReadAllText($sentinel) -cne 'keep') {
        throw 'Safe clean did not preserve an outside sentinel'
    }

    $junctionTarget = Join-Path $repository 'junction-target'
    $junction = Join-Path $buildRoot 'dxvk-x86'
    New-Item -ItemType Directory -Path $junctionTarget -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $junctionTarget 'sentinel.txt'), 'keep-junction')
    New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
    New-Item -ItemType Junction -Path $junction -Target $junctionTarget | Out-Null
    try {
        $junctionRejected = $false
        try {
            Remove-RenderStackBuildPath -Path $junction -RepoRoot $repository -BuildRoot $buildRoot -AllowedName 'dxvk-x86'
        } catch {
            $junctionRejected = $_.Exception.Message -match 'reparse'
        }
        if (-not $junctionRejected -or [IO.File]::ReadAllText((Join-Path $junctionTarget 'sentinel.txt')) -cne 'keep-junction') {
            throw 'Safe clean traversed a junction'
        }
    } finally {
        if (Test-Path -LiteralPath $junction) {
            [IO.Directory]::Delete($junction)
        }
    }

    foreach ($ancestorCase in @('out', 'out-build')) {
        $caseRepository = Join-Path $fixtureRoot "safe-clean-$ancestorCase"
        $caseOutside = Join-Path $fixtureRoot "safe-clean-$ancestorCase-outside"
        $caseSentinel = Join-Path $caseOutside 'sentinel.txt'
        New-Item -ItemType Directory -Path $caseRepository -Force | Out-Null
        New-Item -ItemType Directory -Path $caseOutside -Force | Out-Null
        [IO.File]::WriteAllText($caseSentinel, "keep-$ancestorCase")
        if ($ancestorCase -ceq 'out') {
            $caseJunction = Join-Path $caseRepository 'out'
            New-Item -ItemType Junction -Path $caseJunction -Target $caseOutside | Out-Null
            $caseBuildRoot = Join-Path $caseJunction 'build'
        } else {
            $caseOut = Join-Path $caseRepository 'out'
            New-Item -ItemType Directory -Path $caseOut -Force | Out-Null
            $caseJunction = Join-Path $caseOut 'build'
            New-Item -ItemType Junction -Path $caseJunction -Target $caseOutside | Out-Null
            $caseBuildRoot = $caseJunction
        }
        try {
            $ancestorRejected = $false
            try {
                Remove-RenderStackBuildPath -Path (Join-Path $caseBuildRoot 'dxvk-x86') `
                    -RepoRoot $caseRepository -BuildRoot $caseBuildRoot -AllowedName 'dxvk-x86'
            } catch {
                $ancestorRejected = $_.Exception.Message -match 'reparse'
            }
            if (-not $ancestorRejected -or [IO.File]::ReadAllText($caseSentinel) -cne "keep-$ancestorCase") {
                throw "Safe clean did not reject the $ancestorCase ancestor junction"
            }
        } finally {
            if (Test-Path -LiteralPath $caseJunction) {
                [IO.Directory]::Delete($caseJunction)
            }
        }
    }
    Write-Output 'PASS safe clean validates repository, out, build, and selected-child reparse boundaries'
}

function New-IsolatedRepository {
    param([Parameter(Mandatory)] [string]$Name)

    $repository = Join-Path $fixtureRoot $Name
    foreach ($directory in @('tools/lib', 'backend/dxvk', 'src/bridge/legacy')) {
        New-Item -ItemType Directory -Path (Join-Path $repository $directory) -Force | Out-Null
    }
    [IO.File]::WriteAllText((Join-Path $repository 'VERSION'), 'fixture')
    Copy-Item -LiteralPath $prepareMesonScript -Destination (Join-Path $repository 'tools/prepare-meson.ps1')
    Copy-Item -LiteralPath $processRunnerScript -Destination (Join-Path $repository 'tools/lib/process-runner.ps1')
    Copy-Item -LiteralPath $toolchainDiscoveryScript -Destination (Join-Path $repository 'tools/lib/toolchain-discovery.ps1')
    return $repository
}

function Invoke-IsolatedMesonPrepare {
    param(
        [Parameter(Mandatory)] [string]$Repository,
        [Parameter(Mandatory)] [string]$Python,
        [Parameter(Mandatory)] [string]$PackageSource
    )

    return Invoke-CapturedProcess -FilePath (Get-Command pwsh).Source -ArgumentList @(
        '-NoProfile', '-File', (Join-Path $Repository 'tools/prepare-meson.ps1'),
        '-Python', $Python, '-PackageUrl', $PackageSource
    ) -WorkingDirectory $fixtureRoot
}

function Assert-NoMesonTemporaryPublication {
    param([Parameter(Mandatory)] [string]$Cache)

    $temporary = @(Get-ChildItem -LiteralPath $Cache -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^meson-(download|extract)-' })
    if ($temporary.Count -ne 0) {
        throw "Meson preparation left temporary entries: $($temporary.Name -join ', ')"
    }
}

function Assert-MesonCacheCase {
    . $processRunnerScript
    . $toolchainDiscoveryScript
    $python = if (-not [string]::IsNullOrWhiteSpace($PythonPath)) {
        Get-KnownToolPath -Candidates @($PythonPath)
    } else {
        (Find-RenderStackPython -RepoRoot $root).Path
    }
    if ([string]::IsNullOrWhiteSpace($ValidWheelPath) -or
        -not (Test-Path -LiteralPath $ValidWheelPath -PathType Leaf)) {
        throw 'MesonCache requires -ValidWheelPath pointing to the pinned wheel'
    }
    $ValidWheelPath = [IO.Path]::GetFullPath($ValidWheelPath)
    $wheelHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ValidWheelPath).Hash.ToUpperInvariant()
    if ($wheelHash -cne $expectedWheelSha256) {
        throw "Meson wheel fixture hash differs: $wheelHash"
    }

    $corruptWheel = Join-Path $fixtureRoot 'corrupt.whl'
    [IO.File]::WriteAllBytes($corruptWheel, [byte[]](0..31))

    $corruptRepository = New-IsolatedRepository -Name 'meson-corrupt'
    $corruptResult = Invoke-IsolatedMesonPrepare -Repository $corruptRepository -Python $python -PackageSource $corruptWheel
    if ($corruptResult.ExitCode -eq 0 -or $corruptResult.Output -notmatch 'SHA-256 mismatch') {
        throw "Corrupt Meson wheel was not rejected: $($corruptResult.Output)"
    }
    $corruptCache = Join-Path $corruptRepository 'out/deps/meson/1.11.1'
    Assert-NoMesonTemporaryPublication -Cache $corruptCache

    $invalidRepository = New-IsolatedRepository -Name 'meson-invalid-published'
    $invalidModule = Join-Path $invalidRepository 'out/deps/meson/1.11.1/site-packages'
    New-Item -ItemType Directory -Path $invalidModule -Force | Out-Null
    $invalidSentinel = Join-Path $invalidModule 'keep.invalid'
    [IO.File]::WriteAllText($invalidSentinel, 'unchanged')
    $invalidTimestamp = (Get-Item -LiteralPath $invalidModule).LastWriteTimeUtc
    $invalidResult = Invoke-IsolatedMesonPrepare -Repository $invalidRepository -Python $python -PackageSource $ValidWheelPath
    if ($invalidResult.ExitCode -eq 0 -or $invalidResult.Output -notmatch 'Published Meson cache is invalid') {
        throw "Invalid published Meson cache did not fail closed: $($invalidResult.Output)"
    }
    if ([IO.File]::ReadAllText($invalidSentinel) -cne 'unchanged' -or
        (Get-Item -LiteralPath $invalidModule).LastWriteTimeUtc -ne $invalidTimestamp) {
        throw 'Invalid published Meson cache changed'
    }

    $junctionRepository = New-IsolatedRepository -Name 'meson-junction'
    $junctionOutside = Join-Path $fixtureRoot 'meson-junction-outside'
    $junctionSentinel = Join-Path $junctionOutside 'sentinel.txt'
    New-Item -ItemType Directory -Path $junctionOutside -Force | Out-Null
    [IO.File]::WriteAllText($junctionSentinel, 'outside')
    $junction = Join-Path $junctionRepository 'out'
    New-Item -ItemType Junction -Path $junction -Target $junctionOutside | Out-Null
    try {
        $junctionResult = Invoke-IsolatedMesonPrepare -Repository $junctionRepository -Python $python -PackageSource $ValidWheelPath
        if ($junctionResult.ExitCode -eq 0 -or $junctionResult.Output -notmatch 'reparse point') {
            throw "Meson junction escape was not rejected: $($junctionResult.Output)"
        }
        if ([IO.File]::ReadAllText($junctionSentinel) -cne 'outside') {
            throw 'Meson preparation changed an outside junction sentinel'
        }
    } finally {
        if (Test-Path -LiteralPath $junction) {
            [IO.Directory]::Delete($junction)
        }
    }

    $validRepository = New-IsolatedRepository -Name 'meson-valid-reuse'
    $first = Invoke-IsolatedMesonPrepare -Repository $validRepository -Python $python -PackageSource $ValidWheelPath
    if ($first.ExitCode -ne 0) {
        throw "Initial pinned Meson publication failed: $($first.Output)"
    }
    $moduleDirectory = Join-Path $validRepository 'out/deps/meson/1.11.1/site-packages'
    if ($first.StdOut.TrimEnd() -notmatch [regex]::Escape($moduleDirectory) + '$') {
        throw "Meson prepare did not end with the module directory: $($first.Output)"
    }
    $moduleTimestamp = (Get-Item -LiteralPath $moduleDirectory).LastWriteTimeUtc
    $archive = Join-Path $validRepository 'out/deps/meson/1.11.1/meson-1.11.1-py3-none-any.whl'
    $second = Invoke-IsolatedMesonPrepare -Repository $validRepository -Python $python -PackageSource $corruptWheel
    if ($second.ExitCode -ne 0 -or $second.Output -notmatch 'Using verified published Meson') {
        throw "Valid Meson publication was not reused: $($second.Output)"
    }
    $moduleChanged = (Get-Item -LiteralPath $moduleDirectory).LastWriteTimeUtc -ne $moduleTimestamp
    if ($moduleChanged -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToUpperInvariant() -cne $expectedWheelSha256) {
        throw 'Meson published cache reuse modified the authenticated cache'
    }
    Assert-NoMesonTemporaryPublication -Cache (Split-Path -Parent $moduleDirectory)

    $corruptionCases = @(
        @{ Name = 'mesonmain-corrupt'; Relative = 'mesonbuild/mesonmain.py'; Action = 'corrupt' },
        @{ Name = 'module-corrupt'; Relative = 'mesonbuild/build.py'; Action = 'corrupt' },
        @{ Name = 'module-extra'; Relative = 'mesonbuild/extra.py'; Action = 'extra' },
        @{ Name = 'module-missing'; Relative = 'mesonbuild/build.py'; Action = 'missing' }
    )
    foreach ($corruptionCase in $corruptionCases) {
        $repository = New-IsolatedRepository -Name $corruptionCase.Name
        $initial = Invoke-IsolatedMesonPrepare -Repository $repository -Python $python -PackageSource $ValidWheelPath
        if ($initial.ExitCode -ne 0) {
            throw "Meson integrity fixture publication failed: $($initial.Output)"
        }
        $published = Join-Path $repository 'out/deps/meson/1.11.1/site-packages'
        $target = Join-Path $published $corruptionCase.Relative
        switch ($corruptionCase.Action) {
            'corrupt' { [IO.File]::AppendAllText($target, "`n# corrupted") }
            'extra' { [IO.File]::WriteAllText($target, 'extra') }
            'missing' { [IO.File]::Delete($target) }
        }
        $result = Invoke-IsolatedMesonPrepare -Repository $repository -Python $python -PackageSource $ValidWheelPath
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch 'Published Meson cache is invalid') {
            throw "Published Meson $($corruptionCase.Name) fixture was not rejected: $($result.Output)"
        }
    }
    Write-Output 'PASS Meson cache authenticates RECORD hashes, sizes, and exact file set on reuse'
}

function Start-CapturedProcess {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$ArgumentList,
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [hashtable]$Environment = @{}
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    foreach ($argument in $ArgumentList) { [void]$startInfo.ArgumentList.Add($argument) }
    $startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
    foreach ($entry in $Environment.GetEnumerator()) {
        $startInfo.Environment[[string]$entry.Key] = [string]$entry.Value
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "Failed to start process: $FilePath" }
    return [pscustomobject]@{
        Process = $process
        StdOutTask = $process.StandardOutput.ReadToEndAsync()
        StdErrTask = $process.StandardError.ReadToEndAsync()
    }
}

function Complete-CapturedProcess {
    param([Parameter(Mandatory)] [object]$Capture)

    try {
        $Capture.Process.WaitForExit()
        $stdout = $Capture.StdOutTask.GetAwaiter().GetResult()
        $stderr = $Capture.StdErrTask.GetAwaiter().GetResult()
        return [pscustomobject]@{
            ExitCode = $Capture.Process.ExitCode
            StdOut = $stdout
            StdErr = $stderr
            Output = $stdout + $stderr
        }
    } finally {
        $Capture.Process.Dispose()
    }
}

function Wait-ForBarrierReady {
    param(
        [Parameter(Mandatory)] [string]$Barrier,
        [Parameter(Mandatory)] [string]$Phase,
        [int]$Expected = 2
    )

    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $ready = @(Get-ChildItem -LiteralPath $Barrier -Filter "$Phase-*.ready" -File -ErrorAction SilentlyContinue)
        if ($ready.Count -eq $Expected) { return }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Expected '$Phase' barrier participants"
}

function Assert-MesonConcurrencyCase {
    . $processRunnerScript
    . $toolchainDiscoveryScript
    $python = if (-not [string]::IsNullOrWhiteSpace($PythonPath)) {
        Get-KnownToolPath -Candidates @($PythonPath)
    } else {
        (Find-RenderStackPython -RepoRoot $root).Path
    }
    if ([string]::IsNullOrWhiteSpace($ValidWheelPath) -or
        -not (Test-Path -LiteralPath $ValidWheelPath -PathType Leaf)) {
        throw 'MesonConcurrency requires -ValidWheelPath pointing to the pinned wheel'
    }
    $wheel = [IO.Path]::GetFullPath($ValidWheelPath)
    $repository = New-IsolatedRepository -Name 'meson-concurrent'
    $barrier = Join-Path $fixtureRoot 'meson-concurrent-barrier'
    New-Item -ItemType Directory -Path $barrier -Force | Out-Null
    $arguments = @('-NoProfile', '-File', (Join-Path $repository 'tools/prepare-meson.ps1'),
        '-Python', $python, '-PackageUrl', $wheel)
    $environment = @{ SA_RENDERSTACK_TEST_PREPARE_MESON_BARRIER = $barrier }
    $first = Start-CapturedProcess -FilePath (Get-Command pwsh).Source -ArgumentList $arguments `
        -WorkingDirectory $fixtureRoot -Environment $environment
    $second = Start-CapturedProcess -FilePath (Get-Command pwsh).Source -ArgumentList $arguments `
        -WorkingDirectory $fixtureRoot -Environment $environment
    try {
        Wait-ForBarrierReady -Barrier $barrier -Phase 'wheel'
        [IO.File]::WriteAllText((Join-Path $barrier 'wheel.go'), 'go')
        Wait-ForBarrierReady -Barrier $barrier -Phase 'module'
        [IO.File]::WriteAllText((Join-Path $barrier 'module.go'), 'go')
        $firstResult = Complete-CapturedProcess -Capture $first
        $first = $null
        $secondResult = Complete-CapturedProcess -Capture $second
        $second = $null
    } finally {
        foreach ($capture in @($first, $second)) {
            if ($null -ne $capture) {
                try { $capture.Process.Kill($true) } catch {}
                $capture.Process.Dispose()
            }
        }
    }
    if ($firstResult.ExitCode -ne 0 -or $secondResult.ExitCode -ne 0) {
        throw "Concurrent Meson preparation failed: first=$($firstResult.Output) second=$($secondResult.Output)"
    }
    $moduleDirectory = Join-Path $repository 'out/deps/meson/1.11.1/site-packages'
    foreach ($result in @($firstResult, $secondResult)) {
        if ($result.StdOut.TrimEnd() -notmatch [regex]::Escape($moduleDirectory) + '$') {
            throw "Concurrent Meson preparation returned a different module path: $($result.Output)"
        }
    }
    $archive = Join-Path $repository 'out/deps/meson/1.11.1/meson-1.11.1-py3-none-any.whl'
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToUpperInvariant() -cne $expectedWheelSha256) {
        throw 'Concurrent Meson wheel publication hash differs'
    }
    Assert-NoMesonTemporaryPublication -Cache (Split-Path -Parent $moduleDirectory)

    $reuse = Invoke-IsolatedMesonPrepare -Repository $repository -Python $python -PackageSource $wheel
    if ($reuse.ExitCode -ne 0 -or $reuse.Output -notmatch 'Using verified published Meson') {
        throw "Concurrent Meson result did not pass authenticated reuse: $($reuse.Output)"
    }

    $invalidRepository = New-IsolatedRepository -Name 'meson-concurrent-invalid-winner'
    $invalidBarrier = Join-Path $fixtureRoot 'meson-concurrent-invalid-barrier'
    New-Item -ItemType Directory -Path $invalidBarrier -Force | Out-Null
    $invalidArguments = @('-NoProfile', '-File', (Join-Path $invalidRepository 'tools/prepare-meson.ps1'),
        '-Python', $python, '-PackageUrl', $wheel)
    $invalidCapture = Start-CapturedProcess -FilePath (Get-Command pwsh).Source `
        -ArgumentList $invalidArguments -WorkingDirectory $fixtureRoot `
        -Environment @{ SA_RENDERSTACK_TEST_PREPARE_MESON_BARRIER = $invalidBarrier }
    try {
        Wait-ForBarrierReady -Barrier $invalidBarrier -Phase 'wheel' -Expected 1
        $invalidCache = Join-Path $invalidRepository 'out/deps/meson/1.11.1'
        $invalidWinner = Join-Path $invalidCache 'meson-1.11.1-py3-none-any.whl'
        [IO.File]::WriteAllBytes($invalidWinner, [byte[]](0..31))
        [IO.File]::WriteAllText((Join-Path $invalidBarrier 'wheel.go'), 'go')
        $invalidResult = Complete-CapturedProcess -Capture $invalidCapture
        $invalidCapture = $null
    } finally {
        if ($null -ne $invalidCapture) {
            try { $invalidCapture.Process.Kill($true) } catch {}
            $invalidCapture.Process.Dispose()
        }
    }
    if ($invalidResult.ExitCode -eq 0 -or
        $invalidResult.Output -notmatch 'invalid winner|SHA-256 mismatch') {
        throw "Invalid concurrent Meson wheel winner did not fail closed: $($invalidResult.Output)"
    }
    if (Test-Path -LiteralPath (Join-Path $invalidCache 'site-packages')) {
        throw 'Invalid concurrent Meson wheel winner produced a module tree'
    }
    Assert-NoMesonTemporaryPublication -Cache $invalidCache
    Write-Output 'PASS simultaneous Meson wheel and module publishers converge without residue'
    Write-Output 'PASS invalid concurrent Meson wheel winner fails closed without publication'
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    if ($Case -in @('All', 'Help')) { Assert-HelpCase }
    if ($Case -in @('All', 'Validation')) { Assert-ValidationCase }
    if ($Case -in @('All', 'Discovery')) { Assert-DiscoveryCase }
    if ($Case -in @('All', 'SafeClean')) { Assert-SafeCleanCase }
    if ($Case -in @('All', 'MesonCache')) { Assert-MesonCacheCase }
    if ($Case -in @('All', 'MesonConcurrency')) { Assert-MesonConcurrencyCase }
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        [IO.Directory]::Delete($fixtureRoot, $true)
    }
}

Write-Output 'PASS orchestration regression suite'
