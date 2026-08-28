$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) "bridge-importer-path-safety-$([guid]::NewGuid().ToString('N'))"
$fixtureTools = Join-Path $tempRoot 'tools/migration'
$fixtureSource = Join-Path $tempRoot 'source'
$fixtureImporter = Join-Path $fixtureTools 'import-audited-bridge.ps1'
$fixtureAllowlist = Join-Path $fixtureTools 'bridge-overlay-files.txt'
$sourceAllowlist = Join-Path $root 'tools/migration/bridge-overlay-files.txt'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Write-FixtureAllowlist {
    param([object[]]$Paths)

    [IO.File]::WriteAllText($fixtureAllowlist, (($Paths -join "`n") + "`n"), $utf8NoBom)
}

function Invoke-FixtureImporter {
    $output = (& pwsh -NoProfile -File $fixtureImporter -Source $fixtureSource 2>&1 | Out-String)
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = $output
    }
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [object[]]$Paths,
        [Parameter(Mandatory)] [string]$Reason
    )

    Write-FixtureAllowlist -Paths $Paths
    $result = Invoke-FixtureImporter
    if ($result.ExitCode -eq 0) {
        throw "$Name was accepted"
    }
    if ($result.Output -notmatch $Reason) {
        throw "$Name failed for the wrong reason: $($result.Output.Trim())"
    }
}

try {
    New-Item -ItemType Directory -Force -Path $fixtureTools, $fixtureSource | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'tools/migration/import-audited-bridge.ps1') -Destination $fixtureImporter
    Copy-Item -LiteralPath $sourceAllowlist -Destination $fixtureAllowlist

    $allowlist = @(
        Get-Content -LiteralPath $sourceAllowlist |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ }
    )
    if ($allowlist.Count -ne 38) {
        throw "Expected 38 current allowlist entries, found $($allowlist.Count)"
    }

    $sourceText = @{
        'BridgeD3D9.vcxproj' = '$(ProjectDir)..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
        'BridgeD3D9BackendTrace.vcxproj' =
            '$(ProjectDir)..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include' + "`n" +
            '<ClCompile Include="BridgeD3D9.cpp" />'
        'EffectInspector.h' = '#include "../plugin-sdk/shared/dxsdk/d3dx9effect.h"'
        'ProperShadersStateJournal.h' = '#include "../plugin-sdk/shared/dxsdk/d3dx9effect.h"'
        'tests/BridgeLegacyPluginProbe.vcxproj' = '$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
        'tests/BridgeVulkanPassProbe.vcxproj' = '$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
        'tests/GtaSaCompatApi3Smoke.vcxproj' = '$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
        'tests/ProperShadersStateJournalTests.vcxproj' = '$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include'
    }
    foreach ($relative in $allowlist) {
        $sourcePath = Join-Path $fixtureSource ($relative.Replace('/', '\'))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sourcePath) | Out-Null
        $content = if ($sourceText.ContainsKey($relative)) { $sourceText[$relative] } else { 'fixture' }
        [IO.File]::WriteAllText($sourcePath, $content, $utf8NoBom)
    }

    Write-FixtureAllowlist -Paths $allowlist
    $validResult = Invoke-FixtureImporter
    if ($validResult.ExitCode -ne 0 -or $validResult.Output -notmatch 'Imported 38 audited Bridge files') {
        throw "Current valid allowlist was rejected: $($validResult.Output.Trim())"
    }

    $invalidCases = @(
        [pscustomobject]@{ Name = 'empty path'; Path = ''; Reason = 'empty path or segment' }
        [pscustomobject]@{ Name = 'dot segment'; Path = 'a/./b'; Reason = 'dot path segment' }
        [pscustomobject]@{ Name = 'traversal segment'; Path = 'a/../b'; Reason = 'dot path segment' }
        [pscustomobject]@{ Name = 'repeated separators'; Path = 'a//b'; Reason = 'empty path segment' }
        [pscustomobject]@{ Name = 'backslash alias'; Path = 'tests\BridgeD3D9.test.ini'; Reason = 'non-canonical separator' }
        [pscustomobject]@{ Name = 'ADS colon'; Path = 'file.cpp:stream'; Reason = 'colon or ADS' }
        [pscustomobject]@{ Name = 'trailing dot'; Path = 'file.cpp.'; Reason = 'trailing dot or space' }
        [pscustomobject]@{ Name = 'trailing space'; Path = 'file.cpp '; Reason = 'trailing dot or space' }
        [pscustomobject]@{ Name = 'wraplock component'; Path = 'dir/.WRAPLOCK/file.cpp'; Reason = 'wraplock path component' }
        [pscustomobject]@{ Name = 'drive rooted'; Path = 'C:/file.cpp'; Reason = 'rooted/UNC path' }
        [pscustomobject]@{ Name = 'forward UNC'; Path = '//server/share/file.cpp'; Reason = 'rooted/UNC path' }
        [pscustomobject]@{ Name = 'backslash UNC'; Path = '\\server\share\file.cpp'; Reason = 'rooted/UNC path' }
    )
    foreach ($case in $invalidCases) {
        $candidate = @($allowlist)
        $candidate[0] = $case.Path
        Assert-Rejected -Name $case.Name -Paths $candidate -Reason $case.Reason
    }

    $caseAlias = @($allowlist)
    $caseAlias[1] = 'BRIDGED3D9.CPP'
    Assert-Rejected -Name 'case-insensitive canonical alias' -Paths $caseAlias `
        -Reason 'case-insensitive duplicate canonical path'

    Write-Output 'PASS Bridge importer path-safety regression (38 valid paths; 13 unsafe/collision cases rejected)'
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
