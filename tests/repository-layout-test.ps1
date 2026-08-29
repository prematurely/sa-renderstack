$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$required = @(
    '.gitattributes',
    '.gitignore',
    'LICENSE',
    'THIRD_PARTY_NOTICES.md',
    'README.md',
    'VERSION',
    'backend',
    'config',
    'docs',
    'packaging',
    'sdk',
    'sdk/include/sa_renderstack',
    'src',
    'tests',
    'third_party',
    'third_party/licenses/mingw-w64',
    'toolchains',
    'tools',
    'tools/migration'
)

$required += @(
    'sdk/include/sa_renderstack/.gitkeep',
    'tools/migration/.gitkeep'
)

$missing = @($required | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $root $_))
})

if ($missing.Count) {
    throw "Missing repository paths: $($missing -join ', ')"
}

$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -ne '0.1.0-alpha.1') {
    throw "Unexpected VERSION: $version"
}

$licenseText = Get-Content -LiteralPath (Join-Path $root 'LICENSE') -Raw
if ($licenseText -notmatch '(?im)SA RenderStack-specific source code') {
    throw 'Root LICENSE does not identify the SA RenderStack-specific scope'
}
if ($licenseText -notmatch '(?im)does not replace or relicense vendored DXVK') {
    throw 'Root LICENSE does not exclude vendored DXVK material'
}
$rootLicenseHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $root 'LICENSE')).Hash
$dxvkLicenseHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $root 'backend/dxvk/LICENSE')).Hash
if ($rootLicenseHash -ceq $dxvkLicenseHash) {
    throw 'Root LICENSE must not be identical to the vendored DXVK license'
}
foreach ($path in @(
        'third_party/licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt',
        'third_party/licenses/mingw-w64/COPYING.winpthreads.txt',
        'third_party/licenses/mingw-w64/COPYING.winstorecompat.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $path) -PathType Leaf)) {
        throw "Missing bundled runtime notice: $path"
    }
}

Write-Output 'PASS repository layout'
