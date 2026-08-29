param(
    [string]$Version = '0.1.0-alpha.1',
    [string]$Configuration = 'Release',
    [string]$StagePath,
    [string]$SourceManifestPath
)

$env:GIT_CONFIG_GLOBAL = 'NUL'
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'lib/process-runner.ps1')

$root = Get-RenderStackRepositoryRoot -CallerScriptPath $PSCommandPath
$stageRoot = if ([string]::IsNullOrWhiteSpace($StagePath)) {
    Join-Path $root 'out/stage/split'
} else {
    [IO.Path]::GetFullPath($StagePath)
}
$packagesRoot = Join-Path $root 'out/packages'
$sourceManifest = if ([string]::IsNullOrWhiteSpace($SourceManifestPath)) {
    Join-Path $packagesRoot "SA-RenderStack-v$Version-source-manifest.json"
} else {
    [IO.Path]::GetFullPath($SourceManifestPath)
}

function Assert-PathUnder {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Base,
        [Parameter(Mandatory)] [string]$Description
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullBase = [IO.Path]::GetFullPath($Base).TrimEnd('\', '/')
    $prefix = $fullBase + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.Equals($fullBase, [StringComparison]::OrdinalIgnoreCase) -and
        -not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description is outside its permitted directory: $fullPath"
    }
    return $fullPath
}

function Assert-SafeRelativePath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.IndexOf([char]0) -ge 0) {
        throw "$Description is empty or contains a NUL"
    }
    $canonical = $Path.Replace('\', '/')
    if ([IO.Path]::IsPathRooted($Path) -or $canonical -match '^(?:/|[A-Za-z]:|//)') {
        throw "$Description must be relative: $Path"
    }
    foreach ($segment in $canonical.Split('/')) {
        if ([string]::IsNullOrEmpty($segment) -or $segment -eq '.' -or $segment -eq '..' -or
            $segment.Contains(':') -or $segment.EndsWith('.') -or $segment.EndsWith(' ')) {
            throw "$Description contains an unsafe path segment: $Path"
        }
    }
    return $canonical
}

function Read-StrictPackageToml {
    param([Parameter(Mandatory)] [string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Package mapping file is missing: $Path"
    }
    $entries = [Collections.Generic.List[object]]::new()
    $current = $null
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $Path) {
        $lineNumber++
        $trimmed = ([string]$line).Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed) -or $trimmed.StartsWith('#')) {
            continue
        }
        if ($trimmed -ceq '[[file]]') {
            if ($null -ne $current) {
                if (-not $current.Contains('source') -or -not $current.Contains('destination')) {
                    throw "Package mapping entry before line $lineNumber is incomplete"
                }
                [void]$entries.Add([pscustomobject]@{
                    Source = [string]$current['source']
                    Destination = [string]$current['destination']
                })
            }
            $current = @{}
            continue
        }
        if ($null -eq $current) {
            throw "Unexpected content before [[file]] at line $lineNumber"
        }
        if ($trimmed -notmatch '^(?<key>[A-Za-z][A-Za-z0-9_]*)\s*=\s*"(?<value>[^"]*)"$') {
            throw "Unsupported package TOML syntax at line $lineNumber"
        }
        $key = [string]$Matches.key
        if ($key -notin @('source', 'destination')) {
            throw "Unknown package TOML key '$key' at line $lineNumber"
        }
        if ($current.Contains($key)) {
            throw "Duplicate package TOML key '$key' at line $lineNumber"
        }
        $current[$key] = [string]$Matches.value
    }
    if ($null -ne $current) {
        if (-not $current.Contains('source') -or -not $current.Contains('destination')) {
            throw 'Final package mapping entry is incomplete'
        }
        [void]$entries.Add([pscustomobject]@{
            Source = [string]$current['source']
            Destination = [string]$current['destination']
        })
    }
    if ($entries.Count -eq 0) {
        throw 'Package mapping file contains no [[file]] entries'
    }
    return $entries.ToArray()
}

function Read-SimpleToml {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string[]]$AllowedKeys
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "TOML metadata file is missing: $Path"
    }
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        $trimmed = ([string]$line).Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed) -or $trimmed.StartsWith('#')) {
            continue
        }
        if ($trimmed -notmatch '^(?<key>[A-Za-z][A-Za-z0-9_]*)\s*=\s*"(?<value>[^"]*)"$') {
            throw "Unsupported TOML metadata syntax in ${Path}: $trimmed"
        }
        $key = [string]$Matches.key
        if ($key -notin $AllowedKeys) {
            throw "Unknown TOML metadata key '$key' in $Path"
        }
        if ($values.ContainsKey($key)) {
            throw "Duplicate TOML metadata key '$key' in $Path"
        }
        $values[$key] = [string]$Matches.value
    }
    foreach ($key in $AllowedKeys) {
        if (-not $values.ContainsKey($key)) {
            throw "Missing TOML metadata key '$key' in $Path"
        }
    }
    return $values
}

function Invoke-GitLines {
    param([Parameter(Mandatory)] [string[]]$Arguments)

    $lines = @(& git.exe -C $root @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Git command failed with exit ${exitCode}: git $($Arguments -join ' ')`n$($lines -join [Environment]::NewLine)"
    }
    return @($lines | ForEach-Object { [string]$_ })
}

function Get-FileRecord {
    param(
        [Parameter(Mandatory)] [string]$Base,
        [Parameter(Mandatory)] [string]$Path
    )

    $safePath = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (-not $safePath.PSIsContainer -and
        ($safePath.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Manifest source is a reparse-point file: $Path"
    }
    return [ordered]@{
        path = [IO.Path]::GetRelativePath($Base, $safePath.FullName).Replace('\', '/')
        size = [long]$safePath.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $safePath.FullName).Hash.ToUpperInvariant()
    }
}

function Get-StageRecords {
    param([Parameter(Mandatory)] [string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Split staging directory is missing: $Path"
    }
    $items = Get-ChildItem -LiteralPath $Path -Recurse -File -Force
    $records = [Collections.Generic.List[object]]::new()
    foreach ($item in $items) {
        $relative = [IO.Path]::GetRelativePath($Path, $item.FullName).Replace('\', '/')
        [void](Assert-SafeRelativePath -Path $relative -Description 'Staged file path')
        if ($relative -ceq 'manifest.json') {
            continue
        }
        [void]$records.Add((Get-FileRecord -Base $Path -Path $item.FullName))
    }
    return @($records | Sort-Object -Property path)
}

function Should-IncludeProfileOption {
    param(
        [Parameter(Mandatory)] [string]$Key,
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $false
    }
    if ($Value -match '^(?i:1|true|high|auto)$') {
        return $true
    }
    return $Key -match '(?i)(backend|path|profile|dir)$'
}

function Read-ProfileOptions {
    param(
        [Parameter(Mandatory)] [string]$IniPath,
        [Parameter(Mandatory)] [string]$DxvkPath
    )

    $options = [Collections.Generic.List[object]]::new()
    foreach ($source in @(
            [pscustomobject]@{ Path = $IniPath; DefaultSection = $null },
            [pscustomobject]@{ Path = $DxvkPath; DefaultSection = 'DXVK' })) {
        if (-not (Test-Path -LiteralPath $source.Path -PathType Leaf)) {
            throw "Release profile source is missing: $($source.Path)"
        }
        $section = $source.DefaultSection
        foreach ($line in Get-Content -LiteralPath $source.Path) {
            $trimmed = ([string]$line).Trim()
            if ([string]::IsNullOrWhiteSpace($trimmed) -or $trimmed.StartsWith(';') -or
                $trimmed.StartsWith('#')) {
                continue
            }
            if ($trimmed -match '^\[(?<section>[^]]+)\]$') {
                $section = [string]$Matches.section.Trim()
                continue
            }
            if ($trimmed -notmatch '^(?<key>[^=]+?)\s*=\s*(?<value>.*)$') {
                throw "Unsupported release profile line in $($source.Path): $trimmed"
            }
            if ([string]::IsNullOrWhiteSpace($section)) {
                throw "Release INI setting appears before a section: $trimmed"
            }
            $key = [string]$Matches.key.Trim()
            $value = [string]$Matches.value.Trim()
            if (Should-IncludeProfileOption -Key $key -Value $value) {
                [void]$options.Add([pscustomobject]@{
                    Name = "$section.$key"
                    Value = $value
                })
            }
        }
    }
    $duplicateNames = @($options | Group-Object Name | Where-Object Count -gt 1)
    if ($duplicateNames.Count -ne 0) {
        throw "Release profile contains duplicate selected options: $($duplicateNames.Name -join ', ')"
    }
    $result = [ordered]@{}
    foreach ($option in @($options | Sort-Object Name)) {
        $result[$option.Name] = [string]$option.Value
    }
    if ($result.Count -eq 0) {
        throw 'Release profile contains no enabled options'
    }
    return $result
}

try {
    if ($Configuration -cne 'Release') {
        throw "Unsupported configuration '$Configuration'; only Release is accepted"
    }
    if ([string]::IsNullOrWhiteSpace($Version) -or $Version -match '[\\/:\x00]') {
        throw "Invalid release version '$Version'"
    }

    $resolvedStage = Assert-PathUnder -Path $stageRoot -Base (Join-Path $root 'out/stage') `
        -Description 'Split staging path'
    $resolvedPackages = Assert-PathUnder -Path $packagesRoot -Base (Join-Path $root 'out') `
        -Description 'Package output path'
    $resolvedSourceManifest = Assert-PathUnder -Path $sourceManifest -Base $resolvedPackages `
        -Description 'Source manifest path'
    if (-not (Test-Path -LiteralPath $resolvedPackages -PathType Container)) {
        New-Item -ItemType Directory -Path $resolvedPackages -Force | Out-Null
    }

    $gitRootLines = @(Invoke-GitLines -Arguments @('rev-parse', '--show-toplevel'))
    if ($gitRootLines.Count -ne 1) {
        throw "Git returned an unexpected repository-root line count: $($gitRootLines.Count)"
    }
    $gitRoot = ([IO.Path]::GetFullPath($gitRootLines[0])).Replace('/', '\').TrimEnd('\', '/')
    $normalizedRoot = $root.Replace('/', '\').TrimEnd('\', '/')
    if (-not $gitRoot.Equals($normalizedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Git repository root differs from monorepo root: $gitRoot"
    }
    $statusLines = @(Invoke-GitLines -Arguments @('status', '--porcelain', '--untracked-files=all') |
        Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
    if ($statusLines.Count -ne 0) {
        throw "Manifest generation requires a clean Git worktree: $($statusLines -join '; ')"
    }
    $commitLines = @(Invoke-GitLines -Arguments @('rev-parse', 'HEAD'))
    if ($commitLines.Count -ne 1) {
        throw "Git returned an unexpected commit line count: $($commitLines.Count)"
    }
    $repositoryCommit = $commitLines[0].Trim()
    if ($repositoryCommit -notmatch '^[0-9a-f]{40}$') {
        throw "Git returned an invalid repository commit: $repositoryCommit"
    }

    $metadataPath = Join-Path $root 'out/build-metadata.json'
    if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        throw "Build metadata is missing: $metadataPath"
    }
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    if ([string]$metadata.commit -cne $repositoryCommit -or
        [string]$metadata.configuration -cne $Configuration -or
        [string]$metadata.architecture -cne 'x86' -or
        [int]$metadata.exitCode -ne 0) {
        throw 'Build metadata does not describe the current successful x86 Release build'
    }

    $outputMap = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($output in @($metadata.outputs)) {
        $path = ([string]$output.path).Replace('\', '/')
        if (-not $outputMap.TryAdd($path, $output)) {
            throw "Build metadata contains duplicate output: $path"
        }
    }
    foreach ($requiredPath in @(
            'out/build/bridge/d3d9.dll',
            'out/build/dxvk-x86/src/d3d9/d3d9.dll')) {
        if (-not $outputMap.ContainsKey($requiredPath)) {
            throw "Build metadata is missing required output: $requiredPath"
        }
        $filePath = Join-Path $root ($requiredPath.Replace('/', [IO.Path]::DirectorySeparatorChar))
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            throw "Required build output is missing: $filePath"
        }
        $actual = Get-FileRecord -Base $root -Path $filePath
        $expected = $outputMap[$requiredPath]
        if ([long]$expected.size -ne $actual.size -or
            [string]$expected.sha256 -cne $actual.sha256) {
            throw "Build output hash/size does not match metadata: $requiredPath"
        }
        if ($expected.pe.Machine -cne 'IMAGE_FILE_MACHINE_I386' -or
            $expected.pe.Format -cne 'PE32') {
            throw "Build output is not PE32/I386: $requiredPath"
        }
    }

    $upstream = Read-SimpleToml -Path (Join-Path $root 'backend/dxvk/SA_RENDERSTACK_UPSTREAM.toml') `
        -AllowedKeys @('url', 'tag', 'commit', 'import')
    $expectedUpstream = @{
        url = 'https://github.com/doitsujin/dxvk.git'
        tag = 'v3.0.1'
        commit = 'c850747f1df24180ce97b7a9094603f39da1251d'
    }
    foreach ($key in $expectedUpstream.Keys) {
        if ([string]$upstream[$key] -cne $expectedUpstream[$key]) {
            throw "DXVK upstream metadata mismatch for '$key'"
        }
    }

    $bridgeEvidencePath = Join-Path $root 'out/reports/task-6/bridge-build-evidence.json'
    if (-not (Test-Path -LiteralPath $bridgeEvidencePath -PathType Leaf)) {
        throw "Bridge build evidence is missing: $bridgeEvidencePath"
    }
    $bridgeEvidence = Get-Content -LiteralPath $bridgeEvidencePath -Raw | ConvertFrom-Json
    $bridgeOutput = $outputMap['out/build/bridge/d3d9.dll']
    if ([string]$bridgeEvidence.candidate.sha256 -cne [string]$bridgeOutput.sha256 -or
        [string]::IsNullOrWhiteSpace([string]$bridgeEvidence.toolsetVersion) -or
        [string]::IsNullOrWhiteSpace([string]$bridgeEvidence.compilerVersion)) {
        throw 'Bridge build evidence does not match the packaged Bridge binary'
    }

    $mesonOptions = $metadata.options.meson
    $buildOptions = [ordered]@{
        configuration = $Configuration
        bridgePlatform = 'Win32'
        dxvk = [ordered]@{
            enable_d3d8 = [bool]$mesonOptions.enableD3d8
            enable_d3d9 = [bool]$mesonOptions.enableD3d9
            enable_d3d10 = [bool]$mesonOptions.enableD3d10
            enable_d3d11 = [bool]$mesonOptions.enableD3d11
            enable_dxgi = [bool]$mesonOptions.enableDxgi
            merge_dxgi_into_d3d9 = [bool]$mesonOptions.mergeDxgiIntoD3d9
            enable_renderstack_tests = [bool]$mesonOptions.enableRenderStackTests
        }
    }
    if ($buildOptions.dxvk.enable_d3d9 -ne $true -or
        $buildOptions.dxvk.enable_dxgi -ne $false -or
        $buildOptions.dxvk.merge_dxgi_into_d3d9 -ne $true) {
        throw 'Build options do not match the supported split DXVK profile'
    }

    $apiHeader = Get-Content -LiteralPath (Join-Path $root 'sdk/include/sa_renderstack/backend_api.h') -Raw
    $pluginHeader = Get-Content -LiteralPath (Join-Path $root 'src/bridge/legacy/BridgeD3D9Plugin.h') -Raw
    if ($apiHeader -notmatch '(?m)^#define\s+D3D9_GTA_SA_COMPAT_API_VERSION\s+7u\s*$' -or
        $pluginHeader -notmatch '(?m)^#define\s+BRIDGE_D3D9_PLUGIN_API_VERSION_2\s+2u\s*$') {
        throw 'Public API version markers do not match the release metadata'
    }

    $toolchains = [ordered]@{
        bridge = [ordered]@{
            msbuildVersion = [string]$metadata.tools.msbuild.Version
            toolsetVersion = [string]$bridgeEvidence.toolsetVersion
            compilerVersion = [string]$bridgeEvidence.compilerVersion
        }
        dxvk = [ordered]@{
            llvmMingwVersion = [string]$metadata.tools.llvmMingw.Version
            pythonVersion = [string]$metadata.tools.python.Version
            mesonVersion = [string]$metadata.tools.meson.version
            ninjaVersion = [string]$metadata.tools.ninja.Version
            glslangVersion = [string]$metadata.tools.glslang.Version
        }
    }
    foreach ($toolchain in @($toolchains.bridge, $toolchains.dxvk)) {
        foreach ($property in $toolchain.Keys) {
            if ([string]::IsNullOrWhiteSpace([string]$toolchain[$property])) {
                throw "Toolchain metadata is empty: $property"
            }
        }
    }

    $profileOptions = Read-ProfileOptions `
        -IniPath (Join-Path $root 'config/SA.RenderStack.ini') `
        -DxvkPath (Join-Path $root 'config/dxvk.conf')
    $stageRecords = Get-StageRecords -Path $resolvedStage
    $sourceRecords = [Collections.Generic.List[object]]::new()
    $trackedFiles = @(Invoke-GitLines -Arguments @('ls-files') | Sort-Object)
    foreach ($tracked in $trackedFiles) {
        $relative = Assert-SafeRelativePath -Path ([string]$tracked).Replace('\', '/') `
            -Description 'Tracked source path'
        $trackedPath = Join-Path $root ($relative.Replace('/', [IO.Path]::DirectorySeparatorChar))
        if (-not (Test-Path -LiteralPath $trackedPath -PathType Leaf)) {
            throw "Tracked source file is missing: $relative"
        }
        [void]$sourceRecords.Add((Get-FileRecord -Base $root -Path $trackedPath))
    }
    $sourceRecords = @($sourceRecords | Sort-Object -Property path)

    $manifest = [ordered]@{
        schemaVersion = 1
        product = 'SA RenderStack'
        version = $Version
        repositoryCommit = $repositoryCommit
        dxvkUpstream = [ordered]@{
            url = [string]$upstream.url
            tag = [string]$upstream.tag
            commit = [string]$upstream.commit
            upstreamBase = [string]$upstream.commit
        }
        architecture = 'x86'
        toolchains = $toolchains
        buildOptions = $buildOptions
        publicApis = [ordered]@{
            backend = 7
            bridgePlugin = 2
        }
        defaultProfileOptions = $profileOptions
        files = $stageRecords
    }
    Write-RenderStackAtomicJson -Path (Join-Path $resolvedStage 'manifest.json') -Value $manifest

    $sourceManifestObject = [ordered]@{
        schemaVersion = 1
        product = 'SA RenderStack'
        version = $Version
        repositoryCommit = $repositoryCommit
        files = $sourceRecords
    }
    Write-RenderStackAtomicJson -Path $resolvedSourceManifest -Value $sourceManifestObject
    Write-Output "Wrote split manifest: $(Join-Path $resolvedStage 'manifest.json')"
    Write-Output "Wrote source manifest: $resolvedSourceManifest"
    exit 0
} catch {
    Write-Error "Manifest generation failed: $($_.Exception.Message)"
    exit 1
}
