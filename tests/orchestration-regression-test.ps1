param(
    [ValidateSet('All', 'Help', 'Validation', 'Discovery', 'SafeClean', 'MesonCache')]
    [string]$Case = 'All',
    [string]$ValidWheelPath
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

    $msbuild = Get-KnownToolPath -Candidates @(
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
    )
    $python = Get-KnownToolPath -Candidates @(
        (Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe')
    )
    $llvmBin = 'D:\GTA San Andreas\.codex-tools\llvm-mingw-20260602-msvcrt-i686\bin'
    $ninja = Get-KnownToolPath -Candidates @('C:\msys64\mingw32\bin\ninja.exe')
    $glslang = Get-KnownToolPath -Candidates @('C:\msys64\mingw64\bin\glslangValidator.exe')
    if (-not (Test-Path -LiteralPath $llvmBin -PathType Container)) {
        throw "LLVM-MinGW regression fixture is unavailable: $llvmBin"
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
    } finally {
        foreach ($name in $saved.Keys) {
            [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
        }
    }
    Write-Output 'PASS component discovery ignores intentionally missing unrelated tools'
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

    Remove-RenderStackBuildPath -Path $selected -BuildRoot $buildRoot -AllowedName 'dxvk-x86'
    if (Test-Path -LiteralPath $selected) {
        throw 'Selected clean directory remains'
    }

    $outsideRejected = $false
    try {
        Remove-RenderStackBuildPath -Path $outside -BuildRoot $buildRoot -AllowedName 'dxvk-x86'
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
            Remove-RenderStackBuildPath -Path $junction -BuildRoot $buildRoot -AllowedName 'dxvk-x86'
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
    Write-Output 'PASS safe clean removes only allowlisted non-reparse build descendants'
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
    $python = Get-KnownToolPath -Candidates @(
        (Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe')
    )
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
    if (Test-Path -LiteralPath $archive) {
        [IO.File]::Delete($archive)
    }
    $second = Invoke-IsolatedMesonPrepare -Repository $validRepository -Python $python -PackageSource $corruptWheel
    if ($second.ExitCode -ne 0 -or $second.Output -notmatch 'Using verified published Meson') {
        throw "Valid Meson publication was not reused: $($second.Output)"
    }
    $moduleChanged = (Get-Item -LiteralPath $moduleDirectory).LastWriteTimeUtc -ne $moduleTimestamp
    $archiveRecreated = Test-Path -LiteralPath $archive
    if ($moduleChanged -or $archiveRecreated) {
        throw 'Meson published cache reuse modified the cache or consulted the source'
    }
    Assert-NoMesonTemporaryPublication -Cache (Split-Path -Parent $moduleDirectory)
    Write-Output 'PASS Meson cache rejects corrupt/reparse/invalid fixtures and reuses valid publication'
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    if ($Case -in @('All', 'Help')) { Assert-HelpCase }
    if ($Case -in @('All', 'Validation')) { Assert-ValidationCase }
    if ($Case -in @('All', 'Discovery')) { Assert-DiscoveryCase }
    if ($Case -in @('All', 'SafeClean')) { Assert-SafeCleanCase }
    if ($Case -in @('All', 'MesonCache')) { Assert-MesonCacheCase }
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        [IO.Directory]::Delete($fixtureRoot, $true)
    }
}

Write-Output 'PASS orchestration regression suite'
