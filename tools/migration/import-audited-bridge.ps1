param(
    [string]$Source = 'D:\GTA San Andreas\.codex-src\BridgeD3D9'
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$allowlistPath = Join-Path $PSScriptRoot 'bridge-overlay-files.txt'
$destination = Join-Path $root 'src/bridge/legacy'
$manifestPath = Join-Path $root 'migration/bridge-overlay-manifest.json'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$forbiddenPathPattern = '(^|/)build(/|$)|(^|/)\.wraplock$|\.bak$|\.log$|\.exe$|\.dll$|\.pdb$|\.map$|\.obj$|\.o$|\.tlog$'

function Get-BytesSha256 {
    param([Parameter(Mandatory)] [byte[]]$Bytes)

    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
}

function Get-OrdinalOccurrenceCount {
    param(
        [Parameter(Mandatory)] [string]$Text,
        [Parameter(Mandatory)] [string]$Value
    )

    $count = 0
    $startIndex = 0
    while (($matchIndex = $Text.IndexOf($Value, $startIndex, [StringComparison]::Ordinal)) -ge 0) {
        $count++
        $startIndex = $matchIndex + $Value.Length
    }
    return $count
}

function Assert-SafeRelativePath {
    param([Parameter(Mandatory)] [AllowEmptyString()] [string]$Path)

    if ([string]::IsNullOrEmpty($Path)) {
        throw "Unsafe Bridge overlay path '$Path': empty path or segment"
    }
    if ([IO.Path]::IsPathRooted($Path) -or $Path -match '^(\\|//)') {
        throw "Unsafe Bridge overlay path '$Path': rooted/UNC path"
    }
    if ($Path.Contains(':')) {
        throw "Unsafe Bridge overlay path '$Path': colon or ADS"
    }
    if ($Path.Contains('\')) {
        throw "Unsafe Bridge overlay path '$Path': non-canonical separator"
    }

    $canonical = $Path.Replace('\', '/')
    foreach ($segment in $canonical.Split('/')) {
        if ([string]::IsNullOrEmpty($segment)) {
            throw "Unsafe Bridge overlay path '$Path': empty path segment"
        }
        if ($segment -eq '.' -or $segment -eq '..') {
            throw "Unsafe Bridge overlay path '$Path': dot path segment"
        }
        if ($segment.Equals('.wraplock', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unsafe Bridge overlay path '$Path': wraplock path component"
        }
        if ($segment.EndsWith('.') -or $segment.EndsWith(' ')) {
            throw "Unsafe Bridge overlay path '$Path': trailing dot or space segment"
        }
    }

    if ($canonical -match '(^|/)build(/|$)|\.bak$|\.log$|\.exe$|\.dll$|\.pdb$|\.map$|\.obj$|\.o$|\.tlog$') {
        throw "Unsafe Bridge overlay path '$Path': forbidden artifact path"
    }

    return $canonical
}

function Get-NormalizedUtf8Text {
    param([Parameter(Mandatory)] [byte[]]$Bytes)

    $text = $strictUtf8.GetString($Bytes)
    if ($text.Length -and $text[0] -eq [char]0xFEFF) {
        $text = $text.Substring(1)
    }
    return $text.Replace("`r`n", "`n").Replace("`r", "`n")
}

$allowlistBytes = [IO.File]::ReadAllBytes($allowlistPath)
$allowlistText = Get-NormalizedUtf8Text -Bytes $allowlistBytes
if (-not $allowlistText.EndsWith("`n", [StringComparison]::Ordinal) -or
    $allowlistText.EndsWith("`n`n", [StringComparison]::Ordinal)) {
    throw 'Bridge allowlist must end in exactly one LF'
}
$allowlist = @(
    $allowlistText.TrimEnd("`n") -split "`n"
)
if ($allowlist.Count -ne 38) {
    throw "Unexpected Bridge overlay count: $($allowlist.Count)"
}

$canonicalAllowlist = @($allowlist | ForEach-Object {
    Assert-SafeRelativePath -Path $_
})
if (@($canonicalAllowlist | ForEach-Object { $_.ToLowerInvariant() } |
        Group-Object | Where-Object Count -gt 1).Count) {
    throw 'Bridge overlay allowlist contains case-insensitive duplicate canonical path'
}

$transforms = @{
    'BridgeD3D9.vcxproj' = @(
        [ordered]@{
            kind = 'build-include-relocation'
            from = '$(ProjectDir)..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
            to = '$(ProjectDir)..\..\..\backend\dxvk\include\vulkan\include;$(D3DX9IncludeDir)'
        }
    )
    'BridgeD3D9BackendTrace.vcxproj' = @(
        [ordered]@{
            kind = 'build-include-relocation'
            from = '$(ProjectDir)..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
            to = '$(ProjectDir)..\..\..\backend\dxvk\include\vulkan\include;$(D3DX9IncludeDir)'
        }
        [ordered]@{
            kind = 'build-source-inclusion'
            from = '<ClCompile Include="BridgeD3D9.cpp" />'
            to = '<ClCompile Include="BridgeD3D9.cpp" />' + "`n" +
                '    <ClCompile Include="EffectInspector.cpp" />'
        }
    )
    'EffectInspector.h' = @(
        [ordered]@{
            kind = 'dxsdk-header-relocation'
            from = '#include "../plugin-sdk/shared/dxsdk/d3dx9effect.h"'
            to = '#include <d3dx9effect.h>'
        }
    )
    'ProperShadersStateJournal.h' = @(
        [ordered]@{
            kind = 'dxsdk-header-relocation'
            from = '#include "../plugin-sdk/shared/dxsdk/d3dx9effect.h"'
            to = '#include <d3dx9effect.h>'
        }
    )
    'tests/BridgeLegacyPluginProbe.vcxproj' = @(
        [ordered]@{
            kind = 'build-include-relocation'
            from = '$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
            to = '$(ProjectDir)..\..\..\..\backend\dxvk\include\vulkan\include'
        }
    )
    'tests/BridgeVulkanPassProbe.vcxproj' = @(
        [ordered]@{
            kind = 'build-include-relocation'
            from = '$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
            to = '$(ProjectDir)..\..\..\..\backend\dxvk\include\vulkan\include'
        }
    )
    'tests/GtaSaCompatApi3Smoke.vcxproj' = @(
        [ordered]@{
            kind = 'build-include-relocation'
            from = '$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
            to = '$(ProjectDir)..\..\..\..\backend\dxvk\include\vulkan\include'
        }
    )
    'tests/ProperShadersStateJournalTests.vcxproj' = @(
        [ordered]@{
            kind = 'build-include-relocation'
            from = '$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
            to = '$(ProjectDir)..\..\..\..\backend\dxvk\include\vulkan\include;$(D3DX9IncludeDir)'
        }
    )
}

$manifest = [ordered]@{
    sourceLabel = 'BridgeD3D9-audited-20260829'
    files = @()
}

foreach ($canonicalRelative in $canonicalAllowlist) {
    $from = Join-Path $Source $canonicalRelative
    $to = Join-Path $destination ($canonicalRelative.Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $from -PathType Leaf)) {
        throw "Missing audited source file: $canonicalRelative"
    }

    $sourceBytes = [IO.File]::ReadAllBytes($from)
    $text = Get-NormalizedUtf8Text -Bytes $sourceBytes
    $transformations = if ($transforms.ContainsKey($canonicalRelative)) {
        @($transforms[$canonicalRelative])
    } else {
        @()
    }
    foreach ($transformation in $transformations) {
        $occurrenceCount = Get-OrdinalOccurrenceCount -Text $text -Value $transformation.from
        if ($occurrenceCount -ne 1) {
            throw "Expected one '$($transformation.from)' occurrence in $canonicalRelative, found $occurrenceCount"
        }
        $text = $text.Replace($transformation.from, $transformation.to)
    }

    $isProject = $canonicalRelative.EndsWith('.vcxproj', [StringComparison]::Ordinal)
    $normalization = if ($isProject) { 'utf8-bomless-crlf' } else { 'utf8-bomless-lf' }
    if ($isProject) {
        $text = $text.Replace("`n", "`r`n")
    }
    $destinationBytes = $utf8NoBom.GetBytes($text)
    if ($destinationBytes.Length -ge 3 -and
        $destinationBytes[0] -eq 0xEF -and
        $destinationBytes[1] -eq 0xBB -and
        $destinationBytes[2] -eq 0xBF) {
        throw "Destination contains a UTF-8 BOM: $canonicalRelative"
    }

    $destinationDirectory = Split-Path -Parent $to
    New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
    [IO.File]::WriteAllBytes($to, $destinationBytes)
    $transformationRows = [System.Collections.Generic.List[object]]::new()
    foreach ($transformation in $transformations) {
        [void]$transformationRows.Add($transformation)
    }
    $manifest.files += [ordered]@{
        path = $canonicalRelative
        sourceSha256 = Get-BytesSha256 -Bytes $sourceBytes
        sha256 = Get-BytesSha256 -Bytes $destinationBytes
        normalization = $normalization
        transformations = $transformationRows
    }
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifestPath) | Out-Null
$manifestText = (($manifest | ConvertTo-Json -Depth 5) + "`n").Replace("`r`n", "`n").Replace("`r", "`n")
[IO.File]::WriteAllText($manifestPath, $manifestText, $utf8NoBom)
Write-Output "Imported $($allowlist.Count) audited Bridge files"
