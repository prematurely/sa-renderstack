param(
    [string]$ActiveBridgeConfig = 'D:\GTA San Andreas\scripts\BridgeD3D9.ini',
    [string]$ActiveDxvkConfig = 'D:\GTA San Andreas\dxvk.conf'
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bridgeOutput = Join-Path $root 'config/SA.RenderStack.ini'
$dxvkOutput = Join-Path $root 'config/dxvk.conf'
$manifestPath = Join-Path $root 'migration/runtime-config-manifest.json'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$utf8NoBom = [Text.UTF8Encoding]::new($false)

$expectedBridgeSourceSha256 = 'B16D6C68DB6A7BF71AD4AA5504B38AA11FB886AD4A0BF85689F667A1844CD748'
$expectedDxvkSourceSha256 = '6D516372A9B71B786AB6CE886FB4CB8147BD1FE225B206150BA78603A66EEA9C'
$expectedBridgeNormalizedSha256 = '71257E027B801814BBCAB45137A0A16FDFCC1B9EAD20B73C96B305805ECD21F1'
$expectedDxvkNormalizedSha256 = '6D516372A9B71B786AB6CE886FB4CB8147BD1FE225B206150BA78603A66EEA9C'
$expectedBridgeOutputSha256 = '1B7547E4709950B61C123E19ACD9DAF3F430A5744BAFD5BBFF5E354C855863D5'
$expectedDxvkOutputSha256 = '6D516372A9B71B786AB6CE886FB4CB8147BD1FE225B206150BA78603A66EEA9C'

function Get-BytesSha256 {
    param([Parameter(Mandatory)] [byte[]]$Bytes)

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return [Convert]::ToHexString($sha256.ComputeHash($Bytes))
    } finally {
        $sha256.Dispose()
    }
}

function Get-NormalizedUtf8Text {
    param([Parameter(Mandatory)] [byte[]]$Bytes)

    $text = $strictUtf8.GetString($Bytes)
    if ($text.Length -and $text[0] -eq [char]0xFEFF) {
        $text = $text.Substring(1)
    }
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    return $text.TrimEnd([char]0x0A) + "`n"
}

function Read-AuditedConfig {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$ExpectedRawSha256,
        [Parameter(Mandatory)] [string]$ExpectedNormalizedSha256,
        [Parameter(Mandatory)] [string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing active $Label config: $Path"
    }

    $sourceBytes = [IO.File]::ReadAllBytes($Path)
    $sourceSha256 = Get-BytesSha256 -Bytes $sourceBytes
    if ($sourceSha256 -cne $ExpectedRawSha256) {
        throw "$Label raw SHA-256 mismatch: expected $ExpectedRawSha256, got $sourceSha256"
    }

    $normalizedText = Get-NormalizedUtf8Text -Bytes $sourceBytes
    $normalizedBytes = $utf8NoBom.GetBytes($normalizedText)
    $normalizedSha256 = Get-BytesSha256 -Bytes $normalizedBytes
    if ($normalizedSha256 -cne $ExpectedNormalizedSha256) {
        throw "$Label normalized SHA-256 mismatch: expected $ExpectedNormalizedSha256, got $normalizedSha256"
    }

    return [ordered]@{
        sourceBytes = $sourceBytes
        sourceSha256 = $sourceSha256
        normalizedText = $normalizedText
        normalizedBytes = $normalizedBytes
        normalizedSha256 = $normalizedSha256
    }
}

function Get-BackendTargetLineIndex {
    param([Parameter(Mandatory)] [string]$Text)

    $lines = $Text -split "`n"
    $backendStart = $null
    $backendEnd = $lines.Count
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match '^\s*\[([^\]]+)\]\s*$') {
            $sectionName = $Matches[1]
            if ($sectionName -ceq 'Backend') {
                if ($null -ne $backendStart) {
                    throw 'Bridge config contains multiple [Backend] sections'
                }
                $backendStart = $index + 1
            } elseif ($null -ne $backendStart) {
                $backendEnd = $index
                break
            }
        }
    }
    if ($null -eq $backendStart) {
        throw 'Bridge config is missing the [Backend] section'
    }

    $targetIndexes = @()
    for ($index = $backendStart; $index -lt $backendEnd; $index++) {
        $trimmed = $lines[$index].Trim()
        if (-not $trimmed -or $trimmed.StartsWith(';', [StringComparison]::Ordinal) -or
            $trimmed.StartsWith('#', [StringComparison]::Ordinal)) {
            continue
        }
        if ($trimmed -match '^([^=]+?)\s*=') {
            if ($Matches[1].Trim() -ceq 'DxvkBackendDir') {
                $targetIndexes += $index
            }
        }
    }
    if ($targetIndexes.Count -ne 1) {
        throw "[Backend] must contain exactly one non-comment DxvkBackendDir key; found $($targetIndexes.Count)"
    }

    return [ordered]@{
        lines = $lines
        index = $targetIndexes[0]
    }
}

function Set-BackendTarget {
    param([Parameter(Mandatory)] [string]$Text)

    $section = Get-BackendTargetLineIndex -Text $Text
    $line = $section.lines[$section.index]
    if ($line -cnotmatch '^(?<prefix>\s*DxvkBackendDir\s*=\s*)(?<value>dxvk-3\.0\.1-merged)(?<suffix>\s*)$') {
        throw "Unexpected [Backend] DxvkBackendDir assignment: $line"
    }
    $section.lines[$section.index] = $Matches['prefix'] + 'backend\dxvk-gta' + $Matches['suffix']
    return ($section.lines -join "`n")
}

function Write-Utf8NoBomLf {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Text
    )

    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $bytes = $utf8NoBom.GetBytes($Text)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        throw "Refusing to write UTF-8 BOM: $Path"
    }
    if (-not $Text.EndsWith("`n", [StringComparison]::Ordinal) -or
        $Text.EndsWith("`n`n", [StringComparison]::Ordinal) -or $Text.Contains("`r")) {
        throw "Output is not exactly one-trailing-LF text: $Path"
    }
    [IO.File]::WriteAllBytes($Path, $bytes)
    return $bytes
}

$bridge = Read-AuditedConfig -Path $ActiveBridgeConfig -ExpectedRawSha256 $expectedBridgeSourceSha256 `
    -ExpectedNormalizedSha256 $expectedBridgeNormalizedSha256 -Label 'Bridge'
$dxvk = Read-AuditedConfig -Path $ActiveDxvkConfig -ExpectedRawSha256 $expectedDxvkSourceSha256 `
    -ExpectedNormalizedSha256 $expectedDxvkNormalizedSha256 -Label 'DXVK'

$bridgeOutputText = Set-BackendTarget -Text $bridge.normalizedText
$bridgeOutputBytes = $utf8NoBom.GetBytes($bridgeOutputText)
$bridgeOutputSha256 = Get-BytesSha256 -Bytes $bridgeOutputBytes
if ($bridgeOutputSha256 -cne $expectedBridgeOutputSha256) {
    throw "Generated Bridge SHA-256 mismatch: expected $expectedBridgeOutputSha256, got $bridgeOutputSha256"
}
$dxvkOutputBytes = $dxvk.normalizedBytes
$dxvkOutputSha256 = Get-BytesSha256 -Bytes $dxvkOutputBytes
if ($dxvkOutputSha256 -cne $expectedDxvkOutputSha256) {
    throw "Generated DXVK SHA-256 mismatch: expected $expectedDxvkOutputSha256, got $dxvkOutputSha256"
}

Write-Utf8NoBomLf -Path $bridgeOutput -Text $bridgeOutputText | Out-Null
Write-Utf8NoBomLf -Path $dxvkOutput -Text $dxvk.normalizedText | Out-Null

$manifest = [ordered]@{
    sourceLabel = 'active-game-profile-audited-20260829'
    files = @(
        [ordered]@{
            path = 'config/SA.RenderStack.ini'
            sourceSha256 = $bridge.sourceSha256
            normalizedSourceSha256 = $bridge.normalizedSha256
            sha256 = $bridgeOutputSha256
            normalization = 'utf8-bomless-lf'
            transformations = @(
                [ordered]@{
                    section = 'Backend'
                    key = 'DxvkBackendDir'
                    from = 'dxvk-3.0.1-merged'
                    to = 'backend\dxvk-gta'
                }
            )
        }
        [ordered]@{
            path = 'config/dxvk.conf'
            sourceSha256 = $dxvk.sourceSha256
            normalizedSourceSha256 = $dxvk.normalizedSha256
            sha256 = $dxvkOutputSha256
            normalization = 'utf8-bomless-lf'
            transformations = @()
        }
    )
}
$manifestText = (($manifest | ConvertTo-Json -Depth 6) + "`n").Replace("`r`n", "`n").Replace("`r", "`n")
$manifestText = $manifestText.TrimEnd([char]0x0A) + "`n"
Write-Utf8NoBomLf -Path $manifestPath -Text $manifestText | Out-Null

Write-Output 'Generated audited runtime configs'
