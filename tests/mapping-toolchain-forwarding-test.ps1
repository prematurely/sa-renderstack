param(
    [Parameter(Mandatory)] [string]$LlvmMingwBin,
    [Parameter(Mandatory)] [string]$Ninja,
    [Parameter(Mandatory)] [string]$Glslang,
    [Parameter(Mandatory)] [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$missingPrefix = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
$env:SA_RENDERSTACK_NINJA = "$missingPrefix-ninja.exe"
$env:SA_RENDERSTACK_GLSLANG = "$missingPrefix-glslang.exe"

& (Get-Command pwsh).Source -NoProfile -File (Join-Path $PSScriptRoot 'mapping-budget-test.ps1') `
    -Mode lru -OutputDirectory $OutputDirectory -LlvmMingwBin $LlvmMingwBin `
    -Ninja $Ninja -Glslang $Glslang
exit $LASTEXITCODE
