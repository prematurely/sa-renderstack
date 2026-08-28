param(
    [ValidateSet('All', 'ValidReuse', 'CorruptSource', 'InvalidPublished', 'Junction')]
    [string]$Case = 'All',
    [string]$ValidPackagePath
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$prepareScript = Join-Path $root 'tools/prepare-dxsdk.ps1'
$expectedPackageSha256 = 'EAD0906AE8A26C18A7525DA7490127A2110F7C58F18293738283E30E97C6EA4B'
$expectedHeaderSha256 = '72D6665D54C425B8A99FE0716518B2711F7CECE6A3B2F8E7C6FC307E0A3FAE26'
$packageRelativePath = 'out/deps/Microsoft.DXSDK.D3DX/9.29.952.8'

if ([string]::IsNullOrWhiteSpace($ValidPackagePath)) {
    $ValidPackagePath = Join-Path $root "$packageRelativePath/microsoft.dxsdk.d3dx.9.29.952.8.nupkg"
}
if (-not (Test-Path -LiteralPath $ValidPackagePath -PathType Leaf)) {
    throw "Valid D3DX package fixture is missing: $ValidPackagePath"
}
$validPackageHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ValidPackagePath).Hash.ToUpperInvariant()
if ($validPackageHash -cne $expectedPackageSha256) {
    throw "Valid D3DX package fixture hash differs: $validPackageHash"
}

$fixtureRoot = Join-Path $root "out/test-fixtures/prepare-dxsdk-$([Guid]::NewGuid().ToString('N'))"
$corruptPackage = Join-Path $fixtureRoot 'corrupt.nupkg'

function New-IsolatedRepository {
    param([Parameter(Mandatory)] [string]$Name)

    $repository = Join-Path $fixtureRoot $Name
    $toolsDirectory = Join-Path $repository 'tools'
    New-Item -ItemType Directory -Path $toolsDirectory -Force | Out-Null
    Copy-Item -LiteralPath $prepareScript -Destination (Join-Path $toolsDirectory 'prepare-dxsdk.ps1')
    return $repository
}

function Invoke-IsolatedPrepare {
    param(
        [Parameter(Mandatory)] [string]$Repository,
        [Parameter(Mandatory)] [string]$PackageSource
    )

    $script = Join-Path $Repository 'tools/prepare-dxsdk.ps1'
    $output = @(& pwsh -NoProfile -File $script -PackageUrl $PackageSource 2>&1)
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = $output -join [Environment]::NewLine
        Lines = $output
    }
}

function Assert-NoTemporaryPublication {
    param([Parameter(Mandatory)] [string]$Cache)

    $temporaryEntries = @(Get-ChildItem -LiteralPath $Cache -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^package-(download|extract|old)-' })
    if ($temporaryEntries.Count -ne 0) {
        throw "Temporary or backup publication remains: $($temporaryEntries.Name -join ', ')"
    }
}

function Invoke-ValidReuseCase {
    $repository = New-IsolatedRepository -Name 'valid-reuse'
    $first = Invoke-IsolatedPrepare -Repository $repository -PackageSource $ValidPackagePath
    if ($first.ExitCode -ne 0) {
        throw "Initial valid prepare failed: $($first.Output)"
    }

    $cache = Join-Path $repository $packageRelativePath
    $packageDirectory = Join-Path $cache 'package'
    $headerPath = Join-Path $packageDirectory 'build/native/include/d3dx9effect.h'
    $expectedInclude = Join-Path $packageDirectory 'build/native/include'
    if ([string]($first.Lines | Select-Object -Last 1) -cne $expectedInclude) {
        throw "Initial prepare returned an unexpected include path: $($first.Output)"
    }
    $headerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $headerPath).Hash.ToUpperInvariant()
    if ($headerHash -cne $expectedHeaderSha256) {
        throw "Initial published header hash differs: $headerHash"
    }

    $packageTimestamp = (Get-Item -LiteralPath $packageDirectory).LastWriteTimeUtc
    $archivePath = Join-Path $cache 'microsoft.dxsdk.d3dx.9.29.952.8.nupkg'
    [IO.File]::Delete($archivePath)
    $second = Invoke-IsolatedPrepare -Repository $repository -PackageSource $corruptPackage
    if ($second.ExitCode -ne 0) {
        throw "Second valid-cache prepare failed instead of reusing publication: $($second.Output)"
    }
    if ($second.Output -notmatch 'Using verified published package') {
        throw "Second prepare did not report published-package reuse: $($second.Output)"
    }
    if ([string]($second.Lines | Select-Object -Last 1) -cne $expectedInclude) {
        throw "Second prepare returned an unexpected include path: $($second.Output)"
    }
    if (Test-Path -LiteralPath $archivePath) {
        throw 'Second prepare accessed the corrupt source despite a valid published package'
    }
    if ((Get-Item -LiteralPath $packageDirectory).LastWriteTimeUtc -ne $packageTimestamp) {
        throw 'Second prepare replaced or modified the valid published package'
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $headerPath).Hash.ToUpperInvariant() -cne $headerHash) {
        throw 'Second prepare changed the published header'
    }
    Assert-NoTemporaryPublication -Cache $cache
    Write-Output 'PASS valid D3DX publication reused without replacement'
}

function Invoke-CorruptSourceCase {
    $repository = New-IsolatedRepository -Name 'corrupt-source'
    $result = Invoke-IsolatedPrepare -Repository $repository -PackageSource $corruptPackage
    if ($result.ExitCode -eq 0 -or $result.Output -notmatch 'SHA-256 mismatch') {
        throw "Corrupt source was not rejected by hash: $($result.Output)"
    }

    $cache = Join-Path $repository $packageRelativePath
    if (Test-Path -LiteralPath (Join-Path $cache 'package')) {
        throw 'Corrupt source published a package directory'
    }
    if (@(Get-ChildItem -LiteralPath $cache -Filter '*.nupkg' -Force -ErrorAction SilentlyContinue).Count -ne 0) {
        throw 'Corrupt source left a cached package archive'
    }
    Assert-NoTemporaryPublication -Cache $cache
    Write-Output 'PASS corrupt D3DX source rejected without partial publication'
}

function Invoke-InvalidPublishedCase {
    $repository = New-IsolatedRepository -Name 'invalid-published'
    $cache = Join-Path $repository $packageRelativePath
    $packageDirectory = Join-Path $cache 'package'
    $headerPath = Join-Path $packageDirectory 'build/native/include/d3dx9effect.h'
    $sentinelPath = Join-Path $packageDirectory 'keep.invalid'
    New-Item -ItemType Directory -Path (Split-Path -Parent $headerPath) -Force | Out-Null
    [IO.File]::WriteAllText($headerPath, 'invalid-header')
    [IO.File]::WriteAllText($sentinelPath, 'leave-this-package-untouched')
    $headerBefore = [IO.File]::ReadAllBytes($headerPath)
    $sentinelBefore = [IO.File]::ReadAllBytes($sentinelPath)
    $packageTimestamp = (Get-Item -LiteralPath $packageDirectory).LastWriteTimeUtc

    $result = Invoke-IsolatedPrepare -Repository $repository -PackageSource $ValidPackagePath
    if ($result.ExitCode -eq 0 -or $result.Output -notmatch 'Published package is invalid') {
        throw "Invalid published package did not fail closed: $($result.Output)"
    }
    if (-not (Test-Path -LiteralPath $packageDirectory -PathType Container)) {
        throw 'Invalid published package was removed or renamed'
    }
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]]$headerBefore, [byte[]][IO.File]::ReadAllBytes($headerPath)) -or
        -not [Linq.Enumerable]::SequenceEqual([byte[]]$sentinelBefore, [byte[]][IO.File]::ReadAllBytes($sentinelPath))) {
        throw 'Invalid published package content changed'
    }
    if ((Get-Item -LiteralPath $packageDirectory).LastWriteTimeUtc -ne $packageTimestamp) {
        throw 'Invalid published package timestamp changed'
    }
    if (Test-Path -LiteralPath (Join-Path $cache 'microsoft.dxsdk.d3dx.9.29.952.8.nupkg')) {
        throw 'Invalid published package triggered archive caching before fail-closed validation'
    }
    Assert-NoTemporaryPublication -Cache $cache
    Write-Output 'PASS invalid D3DX publication preserved and rejected'
}

function Invoke-JunctionCase {
    $repository = New-IsolatedRepository -Name 'junction'
    $outsideDirectory = Join-Path $fixtureRoot 'junction-outside'
    $sentinelPath = Join-Path $outsideDirectory 'sentinel.txt'
    $junctionPath = Join-Path $repository 'out'
    New-Item -ItemType Directory -Path $outsideDirectory -Force | Out-Null
    [IO.File]::WriteAllText($sentinelPath, 'outside-sentinel')
    $sentinelBefore = [IO.File]::ReadAllBytes($sentinelPath)
    New-Item -ItemType Junction -Path $junctionPath -Target $outsideDirectory | Out-Null

    try {
        $result = Invoke-IsolatedPrepare -Repository $repository -PackageSource $ValidPackagePath
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch 'reparse point') {
            throw "Junction cache escape was not rejected: $($result.Output)"
        }
        if (-not [Linq.Enumerable]::SequenceEqual([byte[]]$sentinelBefore, [byte[]][IO.File]::ReadAllBytes($sentinelPath))) {
            throw 'Outside sentinel changed through junction cache escape'
        }
        if (Test-Path -LiteralPath (Join-Path $outsideDirectory 'deps')) {
            throw 'D3DX preparation wrote through the junction outside its repository fixture'
        }
        Write-Output 'PASS D3DX junction cache escape rejected'
    } finally {
        if (Test-Path -LiteralPath $junctionPath) {
            $junctionAttributes = [IO.File]::GetAttributes($junctionPath)
            if (($junctionAttributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
                throw "Fixture junction lost its reparse attribute: $junctionPath"
            }
            [IO.Directory]::Delete($junctionPath)
        }
    }
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    [IO.File]::WriteAllBytes($corruptPackage, [byte[]](0..15))
    if ($Case -in @('All', 'ValidReuse')) {
        Invoke-ValidReuseCase
    }
    if ($Case -in @('All', 'CorruptSource')) {
        Invoke-CorruptSourceCase
    }
    if ($Case -in @('All', 'InvalidPublished')) {
        Invoke-InvalidPublishedCase
    }
    if ($Case -in @('All', 'Junction')) {
        Invoke-JunctionCase
    }
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        [IO.Directory]::Delete($fixtureRoot, $true)
    }
}
