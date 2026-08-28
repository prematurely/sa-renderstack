param(
    [string]$Source = 'D:\GTA San Andreas\.codex-src\dxvk\dxvk-3.0.1-bridge'
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$allowlistPath = Join-Path $root 'tools/migration/dxvk-overlay-files.txt'
$manifestPath = Join-Path $root 'migration/dxvk-overlay-manifest.json'
$evidencePath = Join-Path $root 'migration/dxvk-baseline-evidence.json'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)

function Get-CanonicalPropertyNames {
    param([Parameter(Mandatory)] $Object)

    return @(($Object.PSObject.Properties.Name | Sort-Object) -join ',')
}

function Assert-PropertySet {
    param(
        [Parameter(Mandatory)] $Object,
        [Parameter(Mandatory)] [string[]] $Expected,
        [Parameter(Mandatory)] [string] $Label
    )

    $actualNames = Get-CanonicalPropertyNames -Object $Object
    $expectedNames = @(($Expected | Sort-Object) -join ',')
    if ($actualNames -ne $expectedNames) {
        throw "$Label properties differ: expected '$expectedNames', found '$actualNames'"
    }
}

function Assert-NoRootedStrings {
    param(
        [Parameter(Mandatory)] $Object,
        [Parameter(Mandatory)] [string] $Label
    )

    foreach ($property in $Object.PSObject.Properties) {
        $value = $property.Value
        if ($null -eq $value) {
            continue
        }
        if ($value -is [string]) {
            if ([IO.Path]::IsPathRooted($value) -or $value -match '^(\\\\|//)') {
                throw "$Label contains rooted path text in '$($property.Name)'"
            }
            continue
        }
        if ($value -is [System.Collections.IEnumerable]) {
            foreach ($item in $value) {
                if ($item -is [string]) {
                    if ([IO.Path]::IsPathRooted($item) -or $item -match '^(\\\\|//)') {
                        throw "$Label contains rooted path text in '$($property.Name)'"
                    }
                } elseif ($item -and $item.PSObject.Properties.Count) {
                    Assert-NoRootedStrings -Object $item -Label $Label
                }
            }
        } elseif ($value.PSObject.Properties.Count) {
            Assert-NoRootedStrings -Object $value -Label $Label
        }
    }
}

if (-not (Test-Path -LiteralPath $allowlistPath -PathType Leaf)) {
    throw 'DXVK overlay allowlist is missing'
}

$allowlist = @(
    Get-Content -LiteralPath $allowlistPath |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith('#') } |
        ForEach-Object { $_.Replace('\', '/') }
)
if ($allowlist.Count -ne 30) {
    throw "Expected 30 DXVK overlay allowlist entries, found $($allowlist.Count)"
}
if (@($allowlist | ForEach-Object { $_.ToLowerInvariant() } |
        Group-Object | Where-Object Count -gt 1).Count) {
    throw 'DXVK overlay allowlist contains duplicate paths'
}
foreach ($relative in $allowlist) {
    if ([IO.Path]::IsPathRooted($relative) -or
        @($relative.Split('/')) -contains '..' -or
        $relative -match '(^|/)build(/|$)|\.bak$|\.log$|\.exe$|\.dll$|\.pdb$') {
        throw "Forbidden overlay allowlist entry: $relative"
    }
}

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'DXVK overlay manifest is missing'
}
$manifestText = Get-Content -LiteralPath $manifestPath -Raw
$manifest = $manifestText | ConvertFrom-Json
Assert-PropertySet -Object $manifest -Expected @('sourceLabel', 'upstreamCommit', 'files') -Label 'Overlay manifest'
Assert-NoRootedStrings -Object $manifest -Label 'Overlay manifest'
if ($manifest.sourceLabel -ne 'dxvk-3.0.1-bridge-audited-20260828') {
    throw "Unexpected overlay source label: $($manifest.sourceLabel)"
}
if ($manifest.upstreamCommit -ne 'c850747f1df24180ce97b7a9094603f39da1251d') {
    throw "Unexpected overlay upstream commit: $($manifest.upstreamCommit)"
}

$manifestFiles = @($manifest.files)
if ($manifestFiles.Count -ne 30) {
    throw "Expected 30 DXVK overlay manifest entries, found $($manifestFiles.Count)"
}
$manifestPaths = @($manifestFiles | ForEach-Object { [string]$_.path })
$expectedPaths = @(($allowlist | Sort-Object) -join "`n")
$actualPaths = @(($manifestPaths | Sort-Object) -join "`n")
if ($actualPaths -ne $expectedPaths) {
    throw 'DXVK overlay manifest paths do not equal the allowlist'
}
if (@($manifestPaths | ForEach-Object { $_.ToLowerInvariant() } |
        Group-Object | Where-Object Count -gt 1).Count) {
    throw 'DXVK overlay manifest contains duplicate paths'
}

foreach ($entry in $manifestFiles) {
    Assert-PropertySet -Object $entry -Expected @('path', 'sourceSha256', 'sha256', 'normalization') -Label "Overlay entry '$($entry.path)'"
    $relative = [string]$entry.path
    if ([IO.Path]::IsPathRooted($relative) -or
        @($relative.Split('/')) -contains '..' -or
        $relative -match '(^|/)build(/|$)|\.bak$|\.log$|\.exe$|\.dll$|\.pdb$') {
        throw "Forbidden overlay manifest entry: $relative"
    }
    if ($entry.normalization -ne 'utf8-bomless-lf') {
        throw "Unexpected normalization for $relative"
    }

    $sourcePath = Join-Path $Source $relative
    $destinationPath = Join-Path $root (Join-Path 'backend/dxvk' $relative)
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "Audited source file is missing: $relative"
    }
    if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
        throw "Imported destination file is missing: $relative"
    }
    $sourceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash
    $destinationSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $destinationPath).Hash
    if ($entry.sourceSha256 -cne $sourceSha256) {
        throw "Audited source hash differs for $relative"
    }
    if ($entry.sha256 -cne $destinationSha256) {
        throw "Imported destination hash differs for $relative"
    }

    $destinationBytes = [IO.File]::ReadAllBytes($destinationPath)
    if ($destinationBytes.Length -ge 3 -and
        $destinationBytes[0] -eq 0xEF -and
        $destinationBytes[1] -eq 0xBB -and
        $destinationBytes[2] -eq 0xBF) {
        throw "UTF-8 BOM remains in imported destination: $relative"
    }
    if ($destinationBytes -contains 13) {
        throw "CR byte remains in imported destination: $relative"
    }
    $null = $strictUtf8.GetString($destinationBytes)
}

if (-not (Test-Path -LiteralPath $evidencePath -PathType Leaf)) {
    throw 'DXVK baseline evidence is missing'
}
$evidenceText = Get-Content -LiteralPath $evidencePath -Raw
if ($evidenceText -match '(?i)\.exe') {
    throw 'DXVK baseline evidence contains forbidden .exe text'
}
$evidence = $evidenceText | ConvertFrom-Json
Assert-PropertySet -Object $evidence -Expected @(
    'sourceReportSha256',
    'auditedBackendSha256',
    'priorProbeResults',
    'probeSourceSha256'
) -Label 'Baseline evidence'
Assert-NoRootedStrings -Object $evidence -Label 'Baseline evidence'

if ($evidence.sourceReportSha256 -cne 'BD99955F9FB6BB51196B857CF13720F9F6B6EB83384B89DC5290F8ED9B3FA100') {
    throw 'Unexpected prior verification report hash'
}
if ($evidence.auditedBackendSha256 -cne '69454C02480981686731B7975EDEA5452E64F02425624BEA410C3A432933FF5F') {
    throw 'Unexpected audited backend hash'
}

$expectedProbeNames = @(
    'd3d9_batch_audit_test',
    'd3d9_deferred_shader_binding_test',
    'dxvk_state_dedup_test',
    'stateblock_prefilter_probe',
    'gta_sa_compat_probe'
)
$priorResults = @($evidence.priorProbeResults)
if ($priorResults.Count -ne 5) {
    throw "Expected five prior probe results, found $($priorResults.Count)"
}
foreach ($result in $priorResults) {
    Assert-PropertySet -Object $result -Expected @('name', 'exitCode') -Label "Prior probe '$($result.name)'"
    if ([int]$result.exitCode -ne 0) {
        throw "Prior probe did not record a zero exit: $($result.name)"
    }
}
$actualProbeNames = @(($priorResults.name | Sort-Object) -join "`n")
$sortedExpectedProbeNames = @(($expectedProbeNames | Sort-Object) -join "`n")
if ($actualProbeNames -ne $sortedExpectedProbeNames) {
    throw 'Prior probe result names differ from the required set'
}

$expectedProbeSources = @(
    $manifestFiles |
        Where-Object { $_.path -like 'tools/*' } |
        Sort-Object path |
        ForEach-Object {
            [pscustomobject]@{
                path = [string]$_.path
                sourceSha256 = [string]$_.sourceSha256
            }
        }
)
$probeSources = @($evidence.probeSourceSha256)
if ($probeSources.Count -ne 5) {
    throw "Expected five probe source hashes, found $($probeSources.Count)"
}
for ($index = 0; $index -lt $probeSources.Count; $index++) {
    $actual = $probeSources[$index]
    $expected = $expectedProbeSources[$index]
    Assert-PropertySet -Object $actual -Expected @('path', 'sourceSha256') -Label "Probe source '$($actual.path)'"
    if ($actual.path -cne $expected.path -or $actual.sourceSha256 -cne $expected.sourceSha256) {
        throw "Probe source evidence differs at index $index"
    }
}

Write-Output 'PASS DXVK overlay manifest and baseline evidence'
