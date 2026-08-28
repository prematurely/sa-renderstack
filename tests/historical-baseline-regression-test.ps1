$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$helperPath = Join-Path $PSScriptRoot 'helpers/git-baseline.ps1'
. $helperPath

$mutationPaths = @(
    'backend/dxvk/include/d3d9_gta_sa_api.h',
    'src/bridge/legacy/d3d9_gta_sa_api.h',
    'migration/bridge-build-evidence.json'
)
$backups = @(
    $mutationPaths | ForEach-Object {
        $fullPath = Join-Path $root $_
        [pscustomobject]@{
            RelativePath = $_
            FullPath = $fullPath
            Bytes = [IO.File]::ReadAllBytes($fullPath)
        }
    }
)
$mutationSuffix = [Text.Encoding]::ASCII.GetBytes("`nTASK-5A1-HISTORICAL-REGRESSION`n")

function Test-ByteSequenceEqual {
    param(
        [Parameter(Mandatory)] [byte[]]$Left,
        [Parameter(Mandatory)] [byte[]]$Right
    )

    if ($Left.Length -ne $Right.Length) {
        return $false
    }
    for ($index = 0; $index -lt $Left.Length; $index++) {
        if ($Left[$index] -ne $Right[$index]) {
            return $false
        }
    }
    return $true
}

Assert-GitFullHistory
Assert-GitTrackedPathsClean -Paths $mutationPaths

$missingCommit = '0000000000000000000000000000000000000001'
$missingCommitError = $null
try {
    Assert-GitCommit -Commit $missingCommit -Label 'Regression missing commit'
} catch {
    $missingCommitError = $_.Exception.Message
}
if ($null -eq $missingCommitError) {
    throw 'Nonexistent baseline commit was accepted'
}
if ($missingCommitError -notmatch [regex]::Escape($missingCommit) -or
    $missingCommitError -notmatch 'full-history checkout' -or
    $missingCommitError -notmatch 'fetch-depth: 0') {
    throw "Nonexistent commit error is unclear: $missingCommitError"
}

$stagedBackup = @($backups | Where-Object {
        $_.RelativePath -ceq $mutationPaths[0]
    })[0]
$stagedMutationError = $null
try {
    $stream = [IO.MemoryStream]::new()
    try {
        $stream.Write($stagedBackup.Bytes, 0, $stagedBackup.Bytes.Length)
        $stream.Write($mutationSuffix, 0, $mutationSuffix.Length)
        [IO.File]::WriteAllBytes($stagedBackup.FullPath, $stream.ToArray())
    } finally {
        $stream.Dispose()
    }

    $null = Invoke-GitBaselineText `
        -Arguments @('add', '--', $stagedBackup.RelativePath) `
        -Operation "staging historical regression path $($stagedBackup.RelativePath)"
    $null = Invoke-GitBaselineText `
        -Arguments @('diff', '--quiet', '--', $stagedBackup.RelativePath) `
        -Operation 'proving the staged regression has no unstaged diff'
    try {
        Assert-GitTrackedPathsClean -Paths @($stagedBackup.RelativePath)
    } catch {
        $stagedMutationError = $_.Exception.Message
    }
    if ($null -eq $stagedMutationError) {
        throw 'Staged tracked baseline mutation was accepted'
    }
    if ($stagedMutationError -notmatch 'Staged tracked baseline paths are dirty') {
        throw "Staged mutation error is unclear: $stagedMutationError"
    }
} finally {
    [IO.File]::WriteAllBytes($stagedBackup.FullPath, $stagedBackup.Bytes)
    $null = Invoke-GitBaselineText `
        -Arguments @('restore', '--staged', '--', $stagedBackup.RelativePath) `
        -Operation "unstaging historical regression path $($stagedBackup.RelativePath)"
}

$stagedRestoredBytes = [IO.File]::ReadAllBytes($stagedBackup.FullPath)
if (-not (Test-ByteSequenceEqual -Left $stagedRestoredBytes -Right $stagedBackup.Bytes)) {
    throw "Staged historical regression did not restore bytes: $($stagedBackup.RelativePath)"
}
Assert-GitTrackedPathsClean -Paths @($stagedBackup.RelativePath)

try {
    foreach ($backup in $backups) {
        $stream = [IO.MemoryStream]::new()
        try {
            $stream.Write($backup.Bytes, 0, $backup.Bytes.Length)
            $stream.Write($mutationSuffix, 0, $mutationSuffix.Length)
            [IO.File]::WriteAllBytes($backup.FullPath, $stream.ToArray())
        } finally {
            $stream.Dispose()
        }
    }

    & (Join-Path $PSScriptRoot 'dxvk-overlay-manifest-test.ps1')
    & (Join-Path $PSScriptRoot 'bridge-migration-manifest-test.ps1')
    & (Join-Path $PSScriptRoot 'bridge-build-evidence-test.ps1')
} finally {
    foreach ($backup in $backups) {
        [IO.File]::WriteAllBytes($backup.FullPath, $backup.Bytes)
    }
}

foreach ($backup in $backups) {
    $restoredBytes = [IO.File]::ReadAllBytes($backup.FullPath)
    if (-not (Test-ByteSequenceEqual -Left $restoredBytes -Right $backup.Bytes)) {
        throw "Historical regression did not restore bytes: $($backup.RelativePath)"
    }
}
Assert-GitTrackedPathsClean -Paths $mutationPaths

Write-Output 'PASS historical baseline regression and restoration'
