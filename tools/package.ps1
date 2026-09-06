param(
    [switch]$Help,
    [string]$Version = '0.1.0-alpha.1',
    [string]$Configuration = 'Release',
    [switch]$NoArchive,
    [switch]$AllowMissingBridgeEvidence
)

$env:GIT_CONFIG_GLOBAL = 'NUL'

if ($Help) {
    @'
Usage: pwsh -NoProfile -File tools/package.ps1 [-Help]
       [-Version 0.1.0-alpha.1] [-Configuration Release] [-NoArchive]
       [-AllowMissingBridgeEvidence]

Builds split, SDK, and symbols staging under out/stage and writes release
manifests under out/packages. Archives are created unless -NoArchive is set.
The game installation is never used as an input or output.
'@ | Write-Output
    exit 0
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'lib/process-runner.ps1')

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

function Remove-SafeDirectory {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Anchor,
        [Parameter(Mandatory)] [string]$Description
    )

    $safePath = Assert-PathUnder -Path $Path -Base $Anchor -Description $Description
    if (-not (Test-Path -LiteralPath $safePath)) {
        return
    }
    $item = Get-Item -LiteralPath $safePath -Force -ErrorAction Stop
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "$Description must be a non-reparse directory: $safePath"
    }
    foreach ($entry in Get-ChildItem -LiteralPath $safePath -Recurse -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description contains a reparse point: $($entry.FullName)"
        }
    }
    Remove-Item -LiteralPath $safePath -Recurse -Force -ErrorAction Stop
}

function Copy-MappedFiles {
    param(
        [Parameter(Mandatory)] [object[]]$Mappings,
        [Parameter(Mandatory)] [string]$Root,
        [Parameter(Mandatory)] [string]$StageRoot
    )

    $destinations = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($mapping in $Mappings) {
        $sourceRelative = Assert-SafeRelativePath -Path ([string]$mapping.Source) `
            -Description 'Package source path'
        $destinationRelative = Assert-SafeRelativePath -Path ([string]$mapping.Destination) `
            -Description 'Package destination path'
        if (-not $destinations.Add($destinationRelative)) {
            throw "Duplicate package destination: $destinationRelative"
        }
        $sourcePath = Assert-PathUnder -Path (Join-Path $Root $sourceRelative.Replace('/', [IO.Path]::DirectorySeparatorChar)) `
            -Base $Root -Description 'Package source path'
        $destinationPath = Assert-PathUnder -Path (Join-Path $StageRoot $destinationRelative.Replace('/', [IO.Path]::DirectorySeparatorChar)) `
            -Base $StageRoot -Description 'Package destination path'
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Package source is missing: $sourceRelative"
        }
        $sourceItem = Get-Item -LiteralPath $sourcePath -Force
        if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Package source is a reparse point: $sourceRelative"
        }
        $destinationParent = Split-Path -Parent $destinationPath
        if (-not (Test-Path -LiteralPath $destinationParent -PathType Container)) {
            New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
        }
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force -ErrorAction Stop
    }
    return @($destinations | Sort-Object)
}

function Get-OutputRecord {
    param(
        [Parameter(Mandatory)] [string]$Root,
        [Parameter(Mandatory)] [object]$Metadata,
        [Parameter(Mandatory)] [string]$RelativePath
    )

    $record = @($Metadata.outputs | Where-Object {
        ([string]$_.path).Replace('\', '/') -ceq $RelativePath
    })
    if ($record.Count -ne 1) {
        throw "Build metadata does not contain exactly one output record for $RelativePath"
    }
    $path = Join-Path $Root $RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required build output is missing: $path"
    }
    $item = Get-Item -LiteralPath $path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Required build output is a reparse point: $path"
    }
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToUpperInvariant()
    if ([long]$record[0].size -ne [long]$item.Length -or
        [string]$record[0].sha256 -cne $actualHash) {
        throw "Build output hash/size mismatch: $RelativePath"
    }
    if ($record[0].pe.Machine -cne 'IMAGE_FILE_MACHINE_I386' -or
        $record[0].pe.Format -cne 'PE32') {
        throw "Build output is not PE32/I386: $RelativePath"
    }
    return $record[0]
}

function Copy-SdkStage {
    param(
        [Parameter(Mandatory)] [string]$Root,
        [Parameter(Mandatory)] [string]$StageRoot
    )

    $files = @(
        [pscustomobject]@{ Source = 'sdk/include/sa_renderstack/backend_api.h'; Destination = 'include/sa_renderstack/backend_api.h' },
        [pscustomobject]@{ Source = 'README.md'; Destination = 'docs/README.md' },
        [pscustomobject]@{ Source = 'README.zh-CN.md'; Destination = 'docs/README.zh-CN.md' },
        [pscustomobject]@{ Source = 'docs/installation.md'; Destination = 'docs/INSTALL.md' },
        [pscustomobject]@{ Source = 'LICENSE'; Destination = 'docs/LICENSE-SA-RENDERSTACK' },
        [pscustomobject]@{ Source = 'THIRD_PARTY_NOTICES.md'; Destination = 'docs/THIRD_PARTY_NOTICES.md' },
        [pscustomobject]@{ Source = 'backend/dxvk/LICENSE'; Destination = 'docs/LICENSE-DXVK' },
        [pscustomobject]@{ Source = 'backend/dxvk/include/native/directx/COPYING.MinGW-w64.txt'; Destination = 'docs/LICENSE-MINGW-W64-HEADERS' },
        [pscustomobject]@{ Source = 'backend/dxvk/include/vulkan/LICENSE.md'; Destination = 'docs/LICENSE-VULKAN-HEADERS' },
        [pscustomobject]@{ Source = 'backend/dxvk/include/spirv/LICENSE'; Destination = 'docs/LICENSE-SPIRV-HEADERS' },
        [pscustomobject]@{ Source = 'backend/dxvk/include/spirv/LICENSES/MIT.txt'; Destination = 'docs/LICENSE-SPIRV-MIT' },
        [pscustomobject]@{ Source = 'backend/dxvk/include/spirv/LICENSES/CC-BY-4.0.txt'; Destination = 'docs/LICENSE-SPIRV-CC-BY-4.0' },
        [pscustomobject]@{ Source = 'backend/dxvk/include/openvr/LICENSE'; Destination = 'docs/LICENSE-OPENVR' },
        [pscustomobject]@{ Source = 'backend/dxvk/subprojects/libdisplay-info/LICENSE'; Destination = 'docs/LICENSE-LIBDISPLAY-INFO' },
        [pscustomobject]@{ Source = 'backend/dxvk/subprojects/dxbc-spirv/LICENSE'; Destination = 'docs/LICENSE-DXBC-SPIRV' },
        [pscustomobject]@{ Source = 'backend/dxvk/subprojects/dxbc-spirv/submodules/spirv_headers/LICENSE'; Destination = 'docs/LICENSE-SPIRV-HEADERS-NESTED' },
        [pscustomobject]@{ Source = 'third_party/licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt'; Destination = 'docs/LICENSE-MINGW-W64-RUNTIME' },
        [pscustomobject]@{ Source = 'third_party/licenses/mingw-w64/COPYING.winpthreads.txt'; Destination = 'docs/LICENSE-WINPTHREADS' },
        [pscustomobject]@{ Source = 'third_party/licenses/mingw-w64/COPYING.winstorecompat.txt'; Destination = 'docs/LICENSE-WINSTORECOMPAT' }
    )
    foreach ($file in $files) {
        $source = Assert-PathUnder -Path (Join-Path $Root $file.Source) -Base $Root `
            -Description 'SDK source path'
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "SDK source is missing: $($file.Source)"
        }
        $destination = Assert-PathUnder -Path (Join-Path $StageRoot $file.Destination) -Base $StageRoot `
            -Description 'SDK destination path'
        $parent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
}

function Copy-SymbolStage {
    param(
        [Parameter(Mandatory)] [string]$Root,
        [Parameter(Mandatory)] [string]$StageRoot
    )

    $files = @(
        [pscustomobject]@{ Source = 'out/build/bridge/d3d9.pdb'; Destination = 'bridge/d3d9.pdb' },
        [pscustomobject]@{ Source = 'out/build/bridge/d3d9.map'; Destination = 'bridge/d3d9.map' }
    )
    foreach ($file in $files) {
        $source = Assert-PathUnder -Path (Join-Path $Root $file.Source) -Base $Root `
            -Description 'Symbols source path'
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Symbols source is missing: $($file.Source)"
        }
        $destination = Assert-PathUnder -Path (Join-Path $StageRoot $file.Destination) -Base $StageRoot `
            -Description 'Symbols destination path'
        $parent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
}

function New-SafeArchive {
    param(
        [Parameter(Mandatory)] [string]$SourceDirectory,
        [Parameter(Mandatory)] [string]$ArchivePath,
        [Parameter(Mandatory)] [string]$PackagesRoot,
        [Parameter(Mandatory)] [string]$Label
    )

    $safeArchive = Assert-PathUnder -Path $ArchivePath -Base $PackagesRoot -Description "$Label archive path"
    if (-not (Test-Path -LiteralPath $SourceDirectory -PathType Container)) {
        throw "$Label staging directory is missing: $SourceDirectory"
    }
    $archiveItem = Get-Item -LiteralPath $safeArchive -Force -ErrorAction SilentlyContinue
    if ($null -ne $archiveItem) {
        if ($archiveItem.PSIsContainer -or ($archiveItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "$Label archive target is not a regular file: $safeArchive"
        }
        Remove-Item -LiteralPath $safeArchive -Force -ErrorAction Stop
    }
    $archivePrefix = $Label.ToLowerInvariant()
    $temporary = Join-Path $PackagesRoot ".${archivePrefix}-$PID-$([Guid]::NewGuid().ToString('N')).tmp"
    $temporary = Assert-PathUnder -Path $temporary -Base $PackagesRoot -Description "$Label temporary archive path"
    try {
        [IO.Compression.ZipFile]::CreateFromDirectory(
            $SourceDirectory,
            $temporary,
            [IO.Compression.CompressionLevel]::Optimal,
            $false)
        [IO.File]::Move($temporary, $safeArchive, $true)
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
    }
}

try {
    if ($Configuration -cne 'Release') {
        throw "Unsupported configuration '$Configuration'; only Release is accepted"
    }
    if ([string]::IsNullOrWhiteSpace($Version) -or $Version -match '[\\/:\x00]') {
        throw "Invalid release version '$Version'"
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop

    $root = Get-RenderStackRepositoryRoot -CallerScriptPath $PSCommandPath
    $outRoot = Assert-PathUnder -Path (Join-Path $root 'out') -Base $root -Description 'Output root'
    $stageRoot = Assert-PathUnder -Path (Join-Path $outRoot 'stage') -Base $outRoot -Description 'Staging root'
    $splitRoot = Assert-PathUnder -Path (Join-Path $stageRoot 'split') -Base $stageRoot -Description 'Split staging root'
    $sdkRoot = Assert-PathUnder -Path (Join-Path $stageRoot 'sdk') -Base $stageRoot -Description 'SDK staging root'
    $symbolsRoot = Assert-PathUnder -Path (Join-Path $stageRoot 'symbols') -Base $stageRoot -Description 'Symbols staging root'
    $packagesRoot = Assert-PathUnder -Path (Join-Path $outRoot 'packages') -Base $outRoot -Description 'Package output root'
    New-Item -ItemType Directory -Path $outRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $packagesRoot -Force | Out-Null

    Remove-SafeDirectory -Path $stageRoot -Anchor $outRoot -Description 'Release staging root'
    New-Item -ItemType Directory -Path $splitRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $sdkRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $symbolsRoot -Force | Out-Null

    $metadataPath = Join-Path $root 'out/build-metadata.json'
    if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        throw "Build metadata is missing: $metadataPath"
    }
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    if ([string]$metadata.configuration -cne $Configuration -or
        [string]$metadata.architecture -cne 'x86' -or [int]$metadata.exitCode -ne 0) {
        throw 'Build metadata does not describe a successful x86 Release build'
    }
    [void](Get-OutputRecord -Root $root -Metadata $metadata -RelativePath 'out/build/bridge/d3d9.dll')
    [void](Get-OutputRecord -Root $root -Metadata $metadata -RelativePath 'out/build/dxvk-x86/src/d3d9/d3d9.dll')

    $mappingPath = Join-Path $root 'packaging/split/package.toml'
    $mappings = Read-StrictPackageToml -Path $mappingPath
    $expectedMappings = @(
        'out/build/bridge/d3d9.dll|d3d9.dll',
        'config/SA.RenderStack.ini|SA.RenderStack.ini',
        'config/SA.RenderStack.ini|scripts/BridgeD3D9.ini',
        'config/dxvk.conf|dxvk.conf',
        'out/build/dxvk-x86/src/d3d9/d3d9.dll|backend/dxvk-gta/d3d9.dll',
        'README.md|docs/README.md',
        'README.zh-CN.md|docs/README.zh-CN.md',
        'docs/installation.md|docs/INSTALL.md',
        'LICENSE|docs/LICENSE-SA-RENDERSTACK',
        'THIRD_PARTY_NOTICES.md|docs/THIRD_PARTY_NOTICES.md',
        'backend/dxvk/LICENSE|docs/LICENSE-DXVK',
        'backend/dxvk/include/native/directx/COPYING.MinGW-w64.txt|docs/LICENSE-MINGW-W64-HEADERS',
        'backend/dxvk/include/vulkan/LICENSE.md|docs/LICENSE-VULKAN-HEADERS',
        'backend/dxvk/include/spirv/LICENSE|docs/LICENSE-SPIRV-HEADERS',
        'backend/dxvk/include/spirv/LICENSES/MIT.txt|docs/LICENSE-SPIRV-MIT',
        'backend/dxvk/include/spirv/LICENSES/CC-BY-4.0.txt|docs/LICENSE-SPIRV-CC-BY-4.0',
        'backend/dxvk/include/openvr/LICENSE|docs/LICENSE-OPENVR',
        'backend/dxvk/subprojects/libdisplay-info/LICENSE|docs/LICENSE-LIBDISPLAY-INFO',
        'backend/dxvk/subprojects/dxbc-spirv/LICENSE|docs/LICENSE-DXBC-SPIRV',
        'backend/dxvk/subprojects/dxbc-spirv/submodules/spirv_headers/LICENSE|docs/LICENSE-SPIRV-HEADERS-NESTED',
        'third_party/licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt|docs/LICENSE-MINGW-W64-RUNTIME',
        'third_party/licenses/mingw-w64/COPYING.winpthreads.txt|docs/LICENSE-WINPTHREADS',
        'third_party/licenses/mingw-w64/COPYING.winstorecompat.txt|docs/LICENSE-WINSTORECOMPAT'
    )
    $actualMappings = @($mappings | ForEach-Object {
        "$(([string]$_.Source).Replace('\', '/'))|$(([string]$_.Destination).Replace('\', '/'))"
    })
    if ((@($actualMappings | Sort-Object) -join "`n") -cne (@($expectedMappings | Sort-Object) -join "`n")) {
        throw 'Split package mappings do not match the release contract'
    }
    [void](Copy-MappedFiles -Mappings $mappings -Root $root -StageRoot $splitRoot)
    Copy-SdkStage -Root $root -StageRoot $sdkRoot
    Copy-SymbolStage -Root $root -StageRoot $symbolsRoot

    $manifestScript = Join-Path $root 'tools/write-manifest.ps1'
    $sourceManifestPath = Join-Path $packagesRoot "SA-RenderStack-v$Version-source-manifest.json"
    $pwsh = (Get-Command pwsh -ErrorAction Stop).Source
    $manifestOutput = @(& $pwsh -NoProfile -File $manifestScript -Version $Version `
        -Configuration $Configuration -StagePath $splitRoot -SourceManifestPath $sourceManifestPath `
        -AllowMissingBridgeEvidence:$AllowMissingBridgeEvidence 2>&1)
    $manifestExitCode = $LASTEXITCODE
    if ($manifestExitCode -ne 0) {
        throw "Manifest writer failed with exit $manifestExitCode`n$($manifestOutput -join [Environment]::NewLine)"
    }

    if (-not $NoArchive) {
        foreach ($kind in @(
                [pscustomobject]@{ Name = 'split'; Directory = $splitRoot },
                [pscustomobject]@{ Name = 'sdk'; Directory = $sdkRoot },
                [pscustomobject]@{ Name = 'symbols'; Directory = $symbolsRoot })) {
            New-SafeArchive -SourceDirectory $kind.Directory `
                -ArchivePath (Join-Path $packagesRoot "SA-RenderStack-v$Version-$($kind.Name).zip") `
                -PackagesRoot $packagesRoot -Label $kind.Name
        }
    }

    Write-Output "Packaged SA RenderStack v$Version"
    Write-Output "Split staging: $splitRoot"
    Write-Output "SDK staging: $sdkRoot"
    Write-Output "Symbols staging: $symbolsRoot"
    Write-Output "Source manifest: $sourceManifestPath"
    if ($NoArchive) {
        Write-Output 'Archives: skipped (-NoArchive)'
    } else {
        Write-Output "Archives: $packagesRoot"
    }
    exit 0
} catch {
    Write-Error "Packaging failed: $($_.Exception.Message)"
    exit 1
}
