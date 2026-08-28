param(
    [string]$Source = 'D:\GTA San Andreas\.codex-src\dxvk\dxvk-3.0.1-bridge'
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$destination = Join-Path $root 'backend/dxvk'
$allowlist = @(
    Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dxvk-overlay-files.txt') |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith('#') }
)

if ($allowlist.Count -ne 30) {
    throw "Unexpected DXVK overlay count: $($allowlist.Count)"
}
if (@($allowlist | ForEach-Object { $_.ToLowerInvariant() } |
        Group-Object | Where-Object Count -gt 1).Count) {
    throw 'DXVK overlay allowlist contains duplicate paths'
}

$manifest = [ordered]@{
    sourceLabel = 'dxvk-3.0.1-bridge-audited-20260828'
    upstreamCommit = 'c850747f1df24180ce97b7a9094603f39da1251d'
    files = @()
}

$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$utf8NoBom = [Text.UTF8Encoding]::new($false)

foreach ($relative in $allowlist) {
    $canonicalRelative = $relative.Replace('\', '/')
    if ([IO.Path]::IsPathRooted($relative) -or
        @($canonicalRelative.Split('/')) -contains '..' -or
        $canonicalRelative -match '(^|/)build(/|$)|(^|/)\.wraplock$|\.bak$|\.log$|\.exe$|\.dll$|\.pdb$|\.obj$|\.o$') {
        throw "Forbidden overlay entry: $relative"
    }

    $from = Join-Path $Source $relative
    $to = Join-Path $destination $relative
    if (-not (Test-Path -LiteralPath $from -PathType Leaf)) {
        throw "Missing audited source file: $from"
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
    $sourceBytes = [IO.File]::ReadAllBytes($from)
    $sourceText = $strictUtf8.GetString($sourceBytes)
    if ($sourceText.Length -and $sourceText[0] -eq [char]0xFEFF) {
        $sourceText = $sourceText.Substring(1)
    }
    $normalizedText = $sourceText.Replace("`r`n", "`n").Replace("`r", "`n")
    [IO.File]::WriteAllText($to, $normalizedText, $utf8NoBom)

    $destinationBytes = [IO.File]::ReadAllBytes($to)
    if ($destinationBytes -contains 13) {
        throw "CR byte remains after LF normalization: $relative"
    }
    $manifest.files += [ordered]@{
        path = $canonicalRelative
        sourceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $from).Hash
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $to).Hash
        normalization = 'utf8-bomless-lf'
    }
}

$manifestPath = Join-Path $root 'migration/dxvk-overlay-manifest.json'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifestPath) | Out-Null
$manifestText = (($manifest | ConvertTo-Json -Depth 5) + "`n").Replace("`r`n", "`n").Replace("`r", "`n")
[IO.File]::WriteAllText($manifestPath, $manifestText, $utf8NoBom)
Write-Output "Imported $($allowlist.Count) audited DXVK files"
