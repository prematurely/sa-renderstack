param(
    [switch]$AllowNonV18MsBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $root 'tools/build.ps1'
$testScript = Join-Path $root 'tools/test.ps1'
$discoveryScript = Join-Path $root 'tools/lib/toolchain-discovery.ps1'
$releaseGateScript = Join-Path $root 'tools/release-gate.ps1'

foreach ($path in @($buildScript, $testScript, $discoveryScript, $releaseGateScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required MSBuild compatibility source is missing: $path"
    }
}

$buildSource = Get-Content -LiteralPath $buildScript -Raw
$testSource = Get-Content -LiteralPath $testScript -Raw
$discoverySource = Get-Content -LiteralPath $discoveryScript -Raw
$releaseGateSource = Get-Content -LiteralPath $releaseGateScript -Raw

foreach ($script in @($buildScript, $testScript)) {
    $help = & (Get-Command pwsh -ErrorAction Stop).Source -NoProfile -File $script -Help 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $help -notmatch '(?i)AllowNonV18MsBuild') {
        throw "Help for $script does not advertise -AllowNonV18MsBuild"
    }
}

foreach ($source in @($buildSource, $testSource, $discoverySource)) {
    if ($source -notmatch '\[switch\]\$AllowNonV18MsBuild') {
        throw 'CI-only MSBuild switch declaration is missing'
    }
}
if ($buildSource -notmatch 'Get-RenderStackToolchain[\s\S]*-AllowNonV18MsBuild') {
    throw 'build.ps1 does not forward -AllowNonV18MsBuild to toolchain discovery'
}
if ($testSource -notmatch 'Get-RenderStackToolchain[\s\S]*-AllowNonV18MsBuild') {
    throw 'test.ps1 does not forward -AllowNonV18MsBuild to toolchain discovery'
}
if ($testSource -notmatch "'-AllowNonV18MsBuild'") {
    throw 'test.ps1 build-refresh path does not forward -AllowNonV18MsBuild'
}
if ($discoverySource -notmatch 'Find-RenderStackMSBuild[\s\S]*-AllowNonV18MsBuild') {
    throw 'Get-RenderStackToolchain does not pass the CI-only switch to MSBuild discovery'
}
if ($discoverySource -notmatch '\[17\.0,19\.0\)') {
    throw 'CI MSBuild discovery does not accept the hosted Visual Studio 17-18 range'
}
if ($discoverySource -notmatch '\[18\.0,19\.0\)') {
    throw 'strict MSBuild discovery range was removed'
}
if ($discoverySource -notmatch '\$allowedProductMajors\s*=\s*if\s*\(\$AllowNonV18MsBuild\)\s*\{\s*@\(17,\s*18\)\s*\}\s*else\s*\{\s*@\(18\)\s*\}') {
    throw 'strict Visual Studio 18 default product-major validation was removed'
}
if ($discoverySource -notmatch '\$allowedProductMajors\s+-notcontains\s+\$productMajor') {
    throw 'MSBuild product-major validation does not enforce the selected allowlist'
}
if ($releaseGateSource -match 'AllowNonV18MsBuild') {
    throw 'release-gate.ps1 must not opt into non-Visual-Studio-18 MSBuild discovery'
}

. (Join-Path $root 'tools/lib/process-runner.ps1')
. $discoveryScript
if ($AllowNonV18MsBuild) {
    $ci = Find-RenderStackMSBuild -RepoRoot $root -AllowNonV18MsBuild
    if ($ci.ProductMajor -notin @(17, 18) -or $ci.HostArchitecture -cne 'amd64') {
        throw "CI MSBuild discovery returned an unsupported toolchain: $($ci | ConvertTo-Json -Compress)"
    }
} else {
    $strict = Find-RenderStackMSBuild -RepoRoot $root
    if ($strict.ProductMajor -ne 18 -or $strict.HostArchitecture -cne 'amd64') {
        throw "Local strict MSBuild discovery changed: $($strict | ConvertTo-Json -Compress)"
    }
}

Write-Output 'PASS CI-only MSBuild compatibility contract'
exit 0
