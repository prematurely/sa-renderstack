$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $root 'backend/dxvk/SA_RENDERSTACK_UPSTREAM.toml'

if (-not (Test-Path -LiteralPath $manifest)) {
    throw 'DXVK upstream manifest is missing'
}

$text = Get-Content -LiteralPath $manifest -Raw
$required = @(
    'url = "https://github.com/doitsujin/dxvk.git"',
    'tag = "v3.0.1"',
    'commit = "c850747f1df24180ce97b7a9094603f39da1251d"',
    'import = "git-subtree-squash"'
)

foreach ($line in $required) {
    if (-not $text.Contains($line)) {
        throw "Missing upstream manifest entry: $line"
    }
}

if (Test-Path -LiteralPath (Join-Path $root 'backend/dxvk/.git')) {
    throw 'backend/dxvk must not be a nested Git repository or submodule'
}

Write-Output 'PASS DXVK provenance'
