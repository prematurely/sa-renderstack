$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
$workflowPath = Join-Path $root '.github/workflows/windows-ci.yml'
$testPath = Join-Path $root 'tools/test.ps1'
$exportPath = Join-Path $root 'tools/verify-exports.ps1'
$backendApiPath = Join-Path $root 'tests/backend-api-source-test.ps1'
$dxvkRegressionPath = Join-Path $root 'tests/dxvk-overlay-regression-test.ps1'

function Assert-Contains {
    param(
        [Parameter(Mandatory)] [string]$Text,
        [Parameter(Mandatory)] [string]$Pattern,
        [Parameter(Mandatory)] [string]$Description
    )

    if ($Text -notmatch $Pattern) {
        throw "Hosted CI boundary is missing ${Description}: $Pattern"
    }
}

foreach ($path in @($workflowPath, $testPath, $exportPath, $backendApiPath, $dxvkRegressionPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Hosted CI boundary source is missing: $path"
    }
}

$workflow = Get-Content -LiteralPath $workflowPath -Raw
$testSource = Get-Content -LiteralPath $testPath -Raw
$exportSource = Get-Content -LiteralPath $exportPath -Raw
$backendApiSource = Get-Content -LiteralPath $backendApiPath -Raw
$dxvkRegressionSource = Get-Content -LiteralPath $dxvkRegressionPath -Raw

foreach ($switchName in @('SkipGpuRuntimeProbes', 'SkipEnvironmentSensitiveBridgeTests')) {
    $declarationPattern = '\[switch\]' + [regex]::Escape("`$$switchName")
    Assert-Contains -Text $testSource -Pattern $declarationPattern -Description "test switch declaration $switchName"
    Assert-Contains -Text $testSource -Pattern "-$switchName" -Description "test help entry $switchName"
    Assert-Contains -Text $workflow -Pattern "-$switchName" -Description "workflow switch $switchName"
}

Assert-Contains -Text $testSource -Pattern 'Add-SkippedGate[\s\S]*runtime-compatibility-probe' `
    -Description 'explicit GPU runtime skip records'
Assert-Contains -Text $testSource -Pattern 'Add-SkippedGate[\s\S]*bridge-adapter-config' `
    -Description 'explicit environment-sensitive Bridge skip record'
Assert-Contains -Text $testSource -Pattern 'Add-BridgeRuntimeGates[\s\S]*SkipGpuRuntimeProbes[\s\S]*SkipEnvironmentSensitiveBridgeTests' `
    -Description 'Bridge runtime skip forwarding'

Assert-Contains -Text $exportSource -Pattern '\[string\]\$Ninja' -Description 'export verifier Ninja parameter'
Assert-Contains -Text $exportSource -Pattern '\[string\]\$Glslang' -Description 'export verifier glslang parameter'
Assert-Contains -Text $exportSource -Pattern 'NinjaPath \$Ninja[\s\S]*GlslangPath \$Glslang' `
    -Description 'export verifier tool path forwarding'
Assert-Contains -Text $testSource -Pattern "'-Ninja'[\s\S]*\`$Ninja" -Description 'test-to-export Ninja forwarding'
Assert-Contains -Text $testSource -Pattern "'-Glslang'[\s\S]*\`$Glslang" -Description 'test-to-export glslang forwarding'

Assert-Contains -Text $workflow -Pattern '(?ms)uses:\s*actions/checkout@v4\s*\r?\n\s*with:\s*\r?\n\s*fetch-depth:\s*0' `
    -Description 'full-history checkout'

Assert-Contains -Text $backendApiSource -Pattern 'Get-Command\s+rg\.exe' -Description 'optional ripgrep discovery'
Assert-Contains -Text $backendApiSource -Pattern 'Select-String' -Description 'PowerShell source-search fallback'

Assert-Contains -Text $dxvkRegressionSource -Pattern '(?s)^param\([\s\S]*\[string\]\$Source' `
    -Description 'DXVK regression source override parameter'
Assert-Contains -Text $dxvkRegressionSource -Pattern 'backend/dxvk' `
    -Description 'repository-local DXVK source fallback'
if ($dxvkRegressionSource -match '(?i)[A-Za-z]:\\.*\\\.codex-src\\dxvk') {
    throw 'DXVK regression test contains a machine-specific audited-source path'
}

Write-Output 'PASS hosted CI boundary regression contract'
exit 0
