param(
    [string]$Version = '0.1.0-alpha.1',
    [string]$Configuration = 'Release'
)

$env:GIT_CONFIG_GLOBAL = 'NUL'
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$stageRoot = Join-Path $root 'out/stage'
$splitRoot = Join-Path $stageRoot 'split'
$sdkRoot = Join-Path $stageRoot 'sdk'
$symbolsRoot = Join-Path $stageRoot 'symbols'
$packagesRoot = Join-Path $root 'out/packages'

function Get-RelativePath {
    param([Parameter(Mandatory)] [string]$Base, [Parameter(Mandatory)] [string]$Path)

    return [IO.Path]::GetRelativePath($Base, $Path).Replace('\', '/')
}

function Assert-Equal {
    param(
        [Parameter(Mandatory)] [AllowNull()] [object]$Actual,
        [Parameter(Mandatory)] [AllowNull()] [object]$Expected,
        [Parameter(Mandatory)] [string]$Description
    )

    if ($Actual -is [System.Array] -or $Expected -is [System.Array]) {
        $actualText = (@($Actual) | ForEach-Object { [string]$_ }) -join "`n"
        $expectedText = (@($Expected) | ForEach-Object { [string]$_ }) -join "`n"
        if ($actualText -cne $expectedText) {
            throw "$Description mismatch.`nExpected:`n$expectedText`nActual:`n$actualText"
        }
        return
    }
    if ([string]$Actual -cne [string]$Expected) {
        throw "$Description mismatch: expected '$Expected', got '$Actual'"
    }
}

function Assert-File {
    param([Parameter(Mandatory)] [string]$Path, [Parameter(Mandatory)] [string]$Description)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
}

function Get-FileRecord {
    param(
        [Parameter(Mandatory)] [string]$Base,
        [Parameter(Mandatory)] [string]$Path
    )

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [pscustomobject]@{
        Path = Get-RelativePath -Base $Base -Path $item.FullName
        Size = [long]$item.Length
        Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.FullName).Hash.ToUpperInvariant()
    }
}

function Get-ManifestRecordMap {
    param([Parameter(Mandatory)] [object]$Manifest)

    $records = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($record in @($Manifest.files)) {
        $path = ([string]$record.path).Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($path)) {
            throw 'Manifest contains an empty file path'
        }
        if (-not $records.TryAdd($path, $record)) {
            throw "Manifest contains a duplicate file path: $path"
        }
    }
    return $records
}

function Assert-ManifestFiles {
    param(
        [Parameter(Mandatory)] [string]$Base,
        [Parameter(Mandatory)] [string]$Root,
        [Parameter(Mandatory)] [object]$Manifest,
        [Parameter(Mandatory)] [string[]]$ExpectedPaths,
        [Parameter(Mandatory)] [string]$Description
    )

    $records = Get-ManifestRecordMap -Manifest $Manifest
    $actualPaths = @($records.Keys | Sort-Object)
    $expectedSorted = @($ExpectedPaths | ForEach-Object { $_.Replace('\', '/') } | Sort-Object)
    Assert-Equal -Actual $actualPaths -Expected $expectedSorted -Description "$Description file set"

    foreach ($relative in $expectedSorted) {
        $path = Join-Path $Root ($relative.Replace('/', [IO.Path]::DirectorySeparatorChar))
        Assert-File -Path $path -Description "$Description file"
        $actual = Get-FileRecord -Base $Base -Path $path
        $record = $records[$relative]
        Assert-Equal -Actual ([string]$record.path).Replace('\', '/') -Expected $relative `
            -Description "$Description path"
        Assert-Equal -Actual ([long]$record.size) -Expected $actual.Size `
            -Description "$Description size for $relative"
        Assert-Equal -Actual ([string]$record.sha256).ToUpperInvariant() -Expected $actual.Sha256 `
            -Description "$Description SHA-256 for $relative"
    }
}

function Get-StreamSha256 {
    param([Parameter(Mandatory)] [IO.Stream]$Stream)

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return [Convert]::ToHexString($sha256.ComputeHash($Stream)).ToUpperInvariant()
    } finally {
        $sha256.Dispose()
    }
}

function Assert-ZipMatchesDirectory {
    param(
        [Parameter(Mandatory)] [string]$ArchivePath,
        [Parameter(Mandatory)] [string]$Directory,
        [Parameter(Mandatory)] [string]$Description
    )

    $expected = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    foreach ($file in Get-ChildItem -LiteralPath $Directory -Recurse -File -Force) {
        $relative = [IO.Path]::GetRelativePath($Directory, $file.FullName).Replace('\', '/')
        if (-not $expected.TryAdd($relative, [pscustomobject]@{
                    Size = [long]$file.Length
                    Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToUpperInvariant()
                })) {
            throw "$Description staging contains a duplicate file: $relative"
        }
    }

    $zip = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $actual = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
        foreach ($entry in $zip.Entries) {
            $relative = ([string]$entry.FullName).Replace('\', '/').TrimEnd('/')
            if ([string]::IsNullOrWhiteSpace($relative) -or $entry.FullName.EndsWith('/')) {
                throw "$Description contains a directory or empty ZIP entry: $($entry.FullName)"
            }
            if ($relative -match '^(?:/|[A-Za-z]:|//)|(^|/)\.\.?(/|$)') {
                throw "$Description contains an unsafe ZIP entry: $relative"
            }
            $stream = $entry.Open()
            try {
                $record = [pscustomobject]@{
                    Size = [long]$entry.Length
                    Sha256 = Get-StreamSha256 -Stream $stream
                }
            } finally {
                $stream.Dispose()
            }
            if (-not $actual.TryAdd($relative, $record)) {
                throw "$Description contains a duplicate ZIP entry: $relative"
            }
        }

        $expectedPaths = @($expected.Keys | Sort-Object)
        $actualPaths = @($actual.Keys | Sort-Object)
        Assert-Equal -Actual $actualPaths -Expected $expectedPaths -Description "$Description file set"
        foreach ($relative in $expectedPaths) {
            $expectedRecord = $expected[$relative]
            $actualRecord = $actual[$relative]
            Assert-Equal -Actual $actualRecord.Size -Expected $expectedRecord.Size `
                -Description "$Description size for $relative"
            Assert-Equal -Actual $actualRecord.Sha256 -Expected $expectedRecord.Sha256 `
                -Description "$Description SHA-256 for $relative"
        }
    } finally {
        $zip.Dispose()
    }
}

try {
    $requiredSplit = @(
        'd3d9.dll',
        'SA.RenderStack.ini',
        'dxvk.conf',
        'scripts/BridgeD3D9.ini',
        'backend/dxvk-gta/d3d9.dll',
        'docs/README.md',
        'docs/INSTALL.md',
        'docs/LICENSE',
        'docs/LICENSE-DXVK',
        'manifest.json'
    )
    foreach ($relative in $requiredSplit) {
        Assert-File -Path (Join-Path $splitRoot ($relative.Replace('/', [IO.Path]::DirectorySeparatorChar))) `
            -Description 'Split package file'
    }

    $actualSplit = @(
        Get-ChildItem -LiteralPath $splitRoot -Recurse -File | ForEach-Object {
            Get-RelativePath -Base $splitRoot -Path $_.FullName
        } | Sort-Object
    )
    Assert-Equal -Actual $actualSplit -Expected (@($requiredSplit | Sort-Object)) `
        -Description 'Split package file set'

    $productIni = Join-Path $splitRoot 'SA.RenderStack.ini'
    $legacyIni = Join-Path $splitRoot 'scripts/BridgeD3D9.ini'
    $productBytes = [IO.File]::ReadAllBytes($productIni)
    $legacyBytes = [IO.File]::ReadAllBytes($legacyIni)
    if (-not [Linq.Enumerable]::SequenceEqual($productBytes, $legacyBytes)) {
        throw 'Product and legacy staged INI files are not byte-identical'
    }
    $iniText = [IO.File]::ReadAllText($productIni)
    if ($iniText -notmatch '(?im)^\s*\[Backend\]\s*$') {
        throw 'Staged SA.RenderStack.ini is missing [Backend]'
    }
    if ($iniText -notmatch '(?im)^\s*DxvkBackendDir\s*=\s*backend\\dxvk-gta\s*$') {
        throw 'Staged SA.RenderStack.ini does not select backend\\dxvk-gta'
    }

    $manifestPath = Join-Path $splitRoot 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert-Equal -Actual $manifest.product -Expected 'SA RenderStack' -Description 'Split manifest product'
    Assert-Equal -Actual $manifest.version -Expected $Version -Description 'Split manifest version'
    Assert-Equal -Actual $manifest.architecture -Expected 'x86' -Description 'Split manifest architecture'
    if ($null -eq $manifest.toolchains.bridge -or $null -eq $manifest.toolchains.dxvk) {
        throw 'Split manifest must contain both toolchain records'
    }
    if ($null -eq $manifest.buildOptions.dxvk -or $null -eq $manifest.publicApis -or
        $null -eq $manifest.defaultProfileOptions) {
        throw 'Split manifest is missing release metadata sections'
    }
    if (@($manifest.defaultProfileOptions).Count -eq 0) {
        throw 'Split manifest defaultProfileOptions is empty'
    }
    Assert-ManifestFiles -Base $splitRoot -Root $splitRoot -Manifest $manifest `
        -ExpectedPaths ($requiredSplit | Where-Object { $_ -ne 'manifest.json' }) -Description 'Split manifest'

    $sourceManifestPath = Join-Path $packagesRoot "SA-RenderStack-v$Version-source-manifest.json"
    Assert-File -Path $sourceManifestPath -Description 'Source manifest'
    $sourceManifest = Get-Content -LiteralPath $sourceManifestPath -Raw | ConvertFrom-Json
    Assert-Equal -Actual $sourceManifest.product -Expected 'SA RenderStack' -Description 'Source manifest product'
    Assert-Equal -Actual $sourceManifest.version -Expected $Version -Description 'Source manifest version'
    $head = @(& git.exe -C $root rev-parse HEAD)
    if ($LASTEXITCODE -ne 0 -or $head.Count -ne 1) {
        throw 'Unable to resolve current repository commit for source manifest verification'
    }
    Assert-Equal -Actual ([string]$sourceManifest.repositoryCommit) -Expected ([string]$head[0]).Trim() `
        -Description 'Source manifest repository commit'
    $gitFiles = @(& git.exe -C $root ls-files)
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed with exit $LASTEXITCODE"
    }
    $gitFiles = @($gitFiles | ForEach-Object { ([string]$_).Replace('\', '/') } | Sort-Object)
    Assert-ManifestFiles -Base $root -Root $root -Manifest $sourceManifest `
        -ExpectedPaths $gitFiles -Description 'Source manifest'

    Assert-File -Path (Join-Path $sdkRoot 'include/sa_renderstack/backend_api.h') `
        -Description 'SDK package header'
    Assert-File -Path (Join-Path $symbolsRoot 'bridge/d3d9.pdb') -Description 'Bridge symbols'
    Assert-File -Path (Join-Path $symbolsRoot 'bridge/d3d9.map') -Description 'Bridge map symbols'

    foreach ($suffix in @('split', 'sdk', 'symbols')) {
        $archive = Join-Path $packagesRoot "SA-RenderStack-v$Version-$suffix.zip"
        Assert-File -Path $archive -Description "$suffix archive"
        $directory = switch ($suffix) {
            'split' { $splitRoot }
            'sdk' { $sdkRoot }
            'symbols' { $symbolsRoot }
        }
        Assert-ZipMatchesDirectory -ArchivePath $archive -Directory $directory -Description "$suffix archive"
    }

    Write-Output "Package layout PASS: $Version $Configuration"
    exit 0
} catch {
    Write-Error "Package layout FAILED: $($_.Exception.Message)"
    exit 1
}
