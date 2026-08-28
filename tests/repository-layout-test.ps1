$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$required = @(
    '.gitattributes',
    '.gitignore',
    'LICENSE',
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

Write-Output 'PASS repository layout'
