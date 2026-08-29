$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
$packageScript = Join-Path $root 'tools/package.ps1'
$manifestScript = Join-Path $root 'tools/write-manifest.ps1'

foreach ($path in @($packageScript, $manifestScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required package manifest script is missing: $path"
    }
}

$packageSource = Get-Content -LiteralPath $packageScript -Raw
$manifestSource = Get-Content -LiteralPath $manifestScript -Raw

foreach ($entry in @(
        @{ Script = $packageScript; Token = 'AllowMissingBridgeEvidence' },
        @{ Script = $manifestScript; Token = 'AllowMissingBridgeEvidence' })) {
    $help = & (Get-Command pwsh -ErrorAction Stop).Source -NoProfile -File $entry.Script -Help 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $help -notmatch [regex]::Escape($entry.Token)) {
        throw "Help for $($entry.Script) does not advertise -$($entry.Token)"
    }
}

if ($packageSource -notmatch '\[switch\]\$AllowMissingBridgeEvidence') {
    throw 'package.ps1 does not declare the CI manifest evidence switch'
}
if ($packageSource -notmatch 'write-manifest\.ps1[\s\S]*AllowMissingBridgeEvidence') {
    throw 'package.ps1 does not forward the CI manifest evidence switch'
}
if ($manifestSource -notmatch '\[switch\]\$AllowMissingBridgeEvidence') {
    throw 'write-manifest.ps1 does not declare the CI manifest evidence switch'
}
if ($manifestSource -notmatch '\$metadata\.tools\.msbuild[\s\S]*InstallationPath') {
    throw 'write-manifest.ps1 does not derive CI Bridge evidence from build metadata'
}
if ($manifestSource -notmatch 'toolsetVersion|compilerVersion') {
    throw 'CI Bridge evidence does not record compiler/toolset metadata'
}
if ($manifestSource -notmatch 'if\s*\(\$AllowMissingBridgeEvidence\)') {
    throw 'write-manifest.ps1 does not separate the CI evidence path from the strict local path'
}

Write-Output 'PASS package manifest CI evidence contract'
exit 0
