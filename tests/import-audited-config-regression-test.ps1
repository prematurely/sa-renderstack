$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$importScript = Join-Path $root 'tools/migration/import-audited-config.ps1'
$realOutputs = @(
    (Join-Path $root 'config/SA.RenderStack.ini'),
    (Join-Path $root 'config/dxvk.conf'),
    (Join-Path $root 'migration/runtime-config-manifest.json')
)
$realOutputHashes = @{}
foreach ($path in $realOutputs) {
    $realOutputHashes[$path] = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToUpperInvariant()
}

$fixtureRoot = Join-Path $root "out/test-fixtures/import-audited-config-$([Guid]::NewGuid().ToString('N'))"
$fixtureScript = Join-Path $fixtureRoot 'tools/migration/import-audited-config.ps1'
$bridgeInput = Join-Path $fixtureRoot 'active/BridgeD3D9.ini'
$dxvkInput = Join-Path $fixtureRoot 'active/dxvk.conf'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Get-TextSha256 {
    param([Parameter(Mandatory)] [string]$Text)

    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8NoBom.GetBytes($Text)))
}

try {
    New-Item -ItemType Directory -Path (Split-Path -Parent $fixtureScript) -Force | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $bridgeInput) -Force | Out-Null
    Copy-Item -LiteralPath $importScript -Destination $fixtureScript

    $bridgeText = @'
[Backend]
DxvkBackendDir=dxvk-3.0.1-merged

[Display]
Mode=windowed

[Backend]
DxvkBackendDir=dxvk-3.0.1-merged
'@.TrimStart("`r", "`n").Replace("`r`n", "`n") + "`n"
    $dxvkText = "dxvk.enableGraphicsPipelineLibrary = True`n"
    [IO.File]::WriteAllText($bridgeInput, $bridgeText, $utf8NoBom)
    [IO.File]::WriteAllText($dxvkInput, $dxvkText, $utf8NoBom)

    $bridgeHash = Get-TextSha256 -Text $bridgeText
    $dxvkHash = Get-TextSha256 -Text $dxvkText
    $scriptText = [IO.File]::ReadAllText($fixtureScript)
    $scriptText = $scriptText.Replace(
        'B16D6C68DB6A7BF71AD4AA5504B38AA11FB886AD4A0BF85689F667A1844CD748', $bridgeHash)
    $scriptText = $scriptText.Replace(
        '71257E027B801814BBCAB45137A0A16FDFCC1B9EAD20B73C96B305805ECD21F1', $bridgeHash)
    $scriptText = $scriptText.Replace(
        '6D516372A9B71B786AB6CE886FB4CB8147BD1FE225B206150BA78603A66EEA9C', $dxvkHash)
    [IO.File]::WriteAllText($fixtureScript, $scriptText, $utf8NoBom)

    $output = @(& pwsh -NoProfile -File $fixtureScript `
        -ActiveBridgeConfig $bridgeInput -ActiveDxvkConfig $dxvkInput 2>&1)
    $exitCode = $LASTEXITCODE
    $outputText = $output -join [Environment]::NewLine
    if ($exitCode -eq 0 -or $outputText -notmatch 'multiple \[Backend\] sections') {
        throw "Duplicate [Backend] fixture was not rejected by the parser: $outputText"
    }
    if ((Test-Path -LiteralPath (Join-Path $fixtureRoot 'config')) -or
        (Test-Path -LiteralPath (Join-Path $fixtureRoot 'migration'))) {
        throw 'Duplicate-section fixture wrote isolated config outputs before rejection'
    }

    foreach ($path in $realOutputs) {
        $afterHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToUpperInvariant()
        if ($afterHash -cne $realOutputHashes[$path]) {
            throw "Duplicate-section regression modified real output: $path"
        }
    }
    Write-Output 'PASS duplicate [Backend] section rejected without output writes'
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        [IO.Directory]::Delete($fixtureRoot, $true)
    }
}
