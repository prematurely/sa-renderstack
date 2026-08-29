$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$canonicalRelative = 'sdk/include/sa_renderstack/backend_api.h'
$canonical = Join-Path $root $canonicalRelative
$expectedHash = '98A18E993376E911FD7297772C2BDE6E95DA6D8AB6091E9608DDCE3694AE3F79'

function Invoke-BackendSourceSearch {
    param(
        [Parameter(Mandatory)] [string]$Pattern,
        [Parameter(Mandatory)] [string[]]$Paths,
        [Parameter(Mandatory)] [string]$Filter,
        [Parameter(Mandatory)] [bool]$ListFiles
    )

    $rg = Get-Command rg.exe -ErrorAction SilentlyContinue
    if ($null -ne $rg) {
        $arguments = [Collections.Generic.List[string]]::new()
        if ($ListFiles) {
            [void]$arguments.Add('-l')
        } else {
            [void]$arguments.Add('-n')
        }
        [void]$arguments.Add('-g')
        [void]$arguments.Add($Filter)
        [void]$arguments.Add('--')
        [void]$arguments.Add($Pattern)
        foreach ($path in $Paths) { [void]$arguments.Add($path) }
        $lines = @(& $rg.Source @($arguments) 2>&1 | ForEach-Object { [string]$_ })
        return [pscustomobject]@{ ExitCode = [int]$LASTEXITCODE; Lines = $lines }
    }

    $lines = [Collections.Generic.List[string]]::new()
    foreach ($path in $Paths) {
        $files = @(Get-ChildItem -LiteralPath $path -Filter $Filter -File -Recurse -ErrorAction Stop)
        foreach ($file in $files) {
            $matches = @(Select-String -LiteralPath $file.FullName -Pattern $Pattern)
            if ($matches.Count -eq 0) { continue }
            $relative = [IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
            if ($ListFiles) {
                [void]$lines.Add($relative)
                continue
            }
            foreach ($match in $matches) {
                [void]$lines.Add("${relative}:$($match.LineNumber):$($match.Line)")
            }
        }
    }
    return [pscustomobject]@{
        ExitCode = if ($lines.Count -eq 0) { 1 } else { 0 }
        Lines = @($lines)
    }
}

$actualHash = (Get-FileHash -LiteralPath $canonical -Algorithm SHA256).Hash
if ($actualHash -ne $expectedHash) {
    throw "Canonical backend API hash mismatch: expected $expectedHash, got $actualHash"
}

$canonicalText = [IO.File]::ReadAllText(
    $canonical,
    [Text.UTF8Encoding]::new($false, $true))
$guidSuffixes = 1..7 | ForEach-Object { "f$_" }
foreach ($suffix in $guidSuffixes) {
    $guid = "9f89b542-4f50-4e7d-b2a4-e8eab3c7d9$suffix"
    $midlDeclaration = "MIDL_INTERFACE(`"$guid`")"
    if (-not $canonicalText.Contains($midlDeclaration)) {
        throw "Canonical header is missing literal declaration: $midlDeclaration"
    }
}

$expectedUuidDeclarations = @(
    '__CRT_UUID_DECL(ID3D9GtaSaCompatDevice, 0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf1);',
    '__CRT_UUID_DECL(ID3D9GtaSaCompatDevice1,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf2);',
    '__CRT_UUID_DECL(ID3D9GtaSaCompatDevice2,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf3);',
    '__CRT_UUID_DECL(ID3D9GtaSaCompatDevice3,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf4);',
    '__CRT_UUID_DECL(ID3D9GtaSaCompatDevice4,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf5);',
    '__CRT_UUID_DECL(ID3D9GtaSaCompatDevice5,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf6);',
    '__CRT_UUID_DECL(ID3D9GtaSaCompatDevice6,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf7);'
)
foreach ($declaration in $expectedUuidDeclarations) {
    if (-not $canonicalText.Contains($declaration)) {
        throw "Canonical header is missing literal declaration: $declaration"
    }
}

$midlCount = [regex]::Matches($canonicalText, '(?m)^MIDL_INTERFACE\("[0-9a-f-]+"\)$').Count
if ($midlCount -ne 7) {
    throw "Expected seven MIDL_INTERFACE declarations, found $midlCount"
}
$uuidCount = [regex]::Matches($canonicalText, '(?m)^__CRT_UUID_DECL\(').Count
if ($uuidCount -ne 7) {
    throw "Expected seven non-MSVC UUID declarations, found $uuidCount"
}

Push-Location $root
try {
    $macroPattern = '^\s*#\s*define\s+D3D9_GTA_SA_COMPAT_API_VERSION\b'
    $macroSearch = Invoke-BackendSourceSearch -Pattern $macroPattern `
        -Paths @('sdk/include', 'backend/dxvk/include', 'src/bridge/legacy') -Filter '*.h' -ListFiles $true
    $macroMatches = @($macroSearch.Lines)
    $macroExit = $macroSearch.ExitCode
    if ($macroExit -gt 1) {
        throw "rg API-version search failed with exit ${macroExit}: $($macroMatches -join [Environment]::NewLine)"
    }
    if ($macroExit -ne 0) {
        throw 'No backend API version definition was found'
    }

    $normalizedMatches = @($macroMatches | ForEach-Object { ([string]$_).Replace('\', '/') })
    if ($normalizedMatches.Count -ne 1 -or $normalizedMatches[0] -ne $canonicalRelative) {
        throw "Backend API version definition is not canonical-only: $($normalizedMatches -join ', ')"
    }

    $legacyBodyPattern = '^\s*ID3D9GtaSaCompatDevice[0-6]?\s*:\s*public\b'
    $legacySearch = Invoke-BackendSourceSearch -Pattern $legacyBodyPattern `
        -Paths @('backend/dxvk/include', 'src/bridge/legacy') -Filter 'd3d9_gta_sa_api.h' -ListFiles $false
    $legacyBodyMatches = @($legacySearch.Lines)
    $legacyBodyExit = $legacySearch.ExitCode
    if ($legacyBodyExit -gt 1) {
        throw "rg legacy interface-body search failed with exit ${legacyBodyExit}: $($legacyBodyMatches -join [Environment]::NewLine)"
    }
    if ($legacyBodyExit -eq 0) {
        throw "Legacy backend API interface body remains: $($legacyBodyMatches -join [Environment]::NewLine)"
    }
}
finally {
    Pop-Location
}

$expectedForwarder = "#pragma once`n#include <sa_renderstack/backend_api.h>`n"
$forwarders = @(
    'backend/dxvk/include/d3d9_gta_sa_api.h',
    'src/bridge/legacy/d3d9_gta_sa_api.h'
)
foreach ($relativePath in $forwarders) {
    $path = Join-Path $root $relativePath
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 3 -and
        $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        throw "Forwarding header has a UTF-8 BOM: $relativePath"
    }
    if ($bytes -contains 0x0D) {
        throw "Forwarding header is not LF-only: $relativePath"
    }

    $body = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    $normalizedBody = $body.Replace("`r`n", "`n").Replace("`r", "`n")
    if ($normalizedBody -cne $expectedForwarder) {
        throw "Forwarding header body differs from the exact two-line contract: $relativePath"
    }
}

Write-Output 'PASS canonical backend API source boundary'
