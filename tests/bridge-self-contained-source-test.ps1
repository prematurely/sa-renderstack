$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$legacyRoot = Join-Path $root 'src/bridge/legacy'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)

function Read-StrictUtf8Text {
    param([Parameter(Mandatory)] [string]$Path)

    return $strictUtf8.GetString([IO.File]::ReadAllBytes($Path))
}

$backendTraceProjectPath = Join-Path $legacyRoot 'BridgeD3D9BackendTrace.vcxproj'
$backendTraceProject = [xml](Read-StrictUtf8Text -Path $backendTraceProjectPath)
$effectInspectorItems = @(
    $backendTraceProject.SelectNodes(
        "//*[local-name()='ClCompile' and @Include='EffectInspector.cpp']"
    )
)
if ($effectInspectorItems.Count -ne 1) {
    throw "Expected exactly one EffectInspector.cpp compile item in BridgeD3D9BackendTrace.vcxproj, found $($effectInspectorItems.Count)"
}

foreach ($header in @('EffectInspector.h', 'ProperShadersStateJournal.h')) {
    $headerText = Read-StrictUtf8Text -Path (Join-Path $legacyRoot $header)
    if (-not $headerText.Contains('#include <d3dx9effect.h>', [StringComparison]::Ordinal)) {
        throw "$header does not include <d3dx9effect.h>"
    }
}

$legacySdkReferences = @(
    Get-ChildItem -LiteralPath $legacyRoot -Recurse -File |
        Where-Object {
            (Read-StrictUtf8Text -Path $_.FullName).Contains(
                'plugin-sdk/shared/dxsdk',
                [StringComparison]::Ordinal
            )
        } |
        ForEach-Object {
            [IO.Path]::GetRelativePath($legacyRoot, $_.FullName).Replace('\', '/')
        }
)
if ($legacySdkReferences.Count -ne 0) {
    throw "Legacy Bridge sources retain plugin-sdk/shared/dxsdk references: $($legacySdkReferences -join ', ')"
}

Write-Output 'PASS Bridge imported projects are self-contained'
