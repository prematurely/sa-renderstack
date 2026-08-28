param(
    [string]$BridgeSource,
    [string]$ActiveBridgeConfig,
    [string]$ActiveDxvkConfig
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$allowlistPath = Join-Path $root 'tools/migration/bridge-overlay-files.txt'
$bridgeManifestPath = Join-Path $root 'migration/bridge-overlay-manifest.json'
$runtimeManifestPath = Join-Path $root 'migration/runtime-config-manifest.json'
$bridgeDestination = Join-Path $root 'src/bridge/legacy'
$generatedBridgeConfigPath = Join-Path $root 'config/SA.RenderStack.ini'
$generatedDxvkConfigPath = Join-Path $root 'config/dxvk.conf'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$expectedAllowlistSha256 = '1764CD9BB1D4524EA4C389A3CF9CD1A9DAFFC35E43E35E4541FBC7DA695BC2CF'
$expectedBridgeConfigSourceSha256 = 'B16D6C68DB6A7BF71AD4AA5504B38AA11FB886AD4A0BF85689F667A1844CD748'
$expectedNormalizedBridgeConfigSha256 = '71257E027B801814BBCAB45137A0A16FDFCC1B9EAD20B73C96B305805ECD21F1'
$expectedGeneratedBridgeConfigSha256 = '1B7547E4709950B61C123E19ACD9DAF3F430A5744BAFD5BBFF5E354C855863D5'
$expectedDxvkConfigSha256 = '6D516372A9B71B786AB6CE886FB4CB8147BD1FE225B206150BA78603A66EEA9C'
$forbiddenPathPattern = '(^|/)build(/|$)|\.bak$|\.log$|\.exe$|\.dll$|\.pdb$|\.map$|\.obj$|\.tlog$'

$providedLiveArguments = @(
    $PSBoundParameters.ContainsKey('BridgeSource'),
    $PSBoundParameters.ContainsKey('ActiveBridgeConfig'),
    $PSBoundParameters.ContainsKey('ActiveDxvkConfig')
)
$providedLiveArgumentCount = @($providedLiveArguments | Where-Object { $_ }).Count
if ($providedLiveArgumentCount -notin 0, 3) {
    throw 'Live Bridge audit requires -BridgeSource, -ActiveBridgeConfig, and -ActiveDxvkConfig together'
}
$liveAudit = $providedLiveArgumentCount -eq 3
if ($liveAudit -and (
        [string]::IsNullOrWhiteSpace($BridgeSource) -or
        [string]::IsNullOrWhiteSpace($ActiveBridgeConfig) -or
        [string]::IsNullOrWhiteSpace($ActiveDxvkConfig))) {
    throw 'Live Bridge audit arguments must be non-empty'
}

$expectedRowsText = @'
BridgeD3D9.cpp|AAF27275CFAFAACEC0A41F5175E2CC9F917027FF04ECB27530ECC71F4E27508E|A0E68B198E7940F8EB15B3942D96BE8BCBA46EEDA126628864C2D5DA2D88338C
BridgeD3D9.def|25B8D467061CB6259D12459463C96374416C165E052843BEEFB692F01B5768E0|25B8D467061CB6259D12459463C96374416C165E052843BEEFB692F01B5768E0
BridgeD3D9.ini|231D338C08E21206244E7CCFF78BAABF71CDDEB507A8191E576BD43BDB801A99|231D338C08E21206244E7CCFF78BAABF71CDDEB507A8191E576BD43BDB801A99
BridgeD3D9.vcxproj|2B60ACBA7070A93A561A32D55360EA643408582FCD7A5C57D14F5DCF910D7F8A|30514555A38C3C404B5715ED44D8EA8C2F761242B32404CB70F22AF48F6162D6
BridgeD3D9BackendTrace.vcxproj|52607DA120CDA77D8D1DAB0A7BE3B0E242D15B710635BFC35259F61D6BF3976F|C1A92AB6276E336AA2F196FE2E695A4A4BEDDAF23BCBE48F7C5B679CCDE6728A
BridgeD3D9Plugin.h|99A2CEAC01AD6303DA30F11072D06C5D6B5C11A019B7113985D7955EE20A7724|99A2CEAC01AD6303DA30F11072D06C5D6B5C11A019B7113985D7955EE20A7724
BridgePerformanceProviderV1.h|E48B18037F5C2C8E5F213B2EFA4139BB725F0C2C300B51625FE42FD8DC8528DF|E48B18037F5C2C8E5F213B2EFA4139BB725F0C2C300B51625FE42FD8DC8528DF
d3d9_gta_sa_api.h|6EBB0798EE196401395243EA0F6C211B57AD8DFCA18A272E897FA2447D7DB5A9|6EBB0798EE196401395243EA0F6C211B57AD8DFCA18A272E897FA2447D7DB5A9
EffectInspector.cpp|3FE85EB2AA1F2627418EB9855740D0DD7E9C503493F870D7B14D5C36DED7DE6E|3FE85EB2AA1F2627418EB9855740D0DD7E9C503493F870D7B14D5C36DED7DE6E
EffectInspector.h|A03BE20227F37EA6BBCE1AEFC4CFC8637819236DFE2663B90BD7A4219F811569|F9EA9179A328D1ABCB636F4401B837E893BF3B9CB88AE64290CE99BEAE4DAFCC
GtaSaCompatApiVersions.h|03EBC6BD21BFE44EBE049C35201CA96E4C04ACC1DC1BD10DB1507AFC491DB05A|03EBC6BD21BFE44EBE049C35201CA96E4C04ACC1DC1BD10DB1507AFC491DB05A
PerformanceAdapterConfig.cpp|B2625AAD2B442CABA1F380734D602FFACD88C37B2EB88FE8955625ACD3036F0B|B2625AAD2B442CABA1F380734D602FFACD88C37B2EB88FE8955625ACD3036F0B
PerformanceAdapterConfig.h|B688724CF809B8BE557D430123A04E81EC8A9B904C214EFDF4164EF78E86E95C|B688724CF809B8BE557D430123A04E81EC8A9B904C214EFDF4164EF78E86E95C
PerformanceAdapters.cpp|BEBD804B3520CC3E56024058D063FC606F70D4D082C9A4C1B8F6BFA062C77A9E|BEBD804B3520CC3E56024058D063FC606F70D4D082C9A4C1B8F6BFA062C77A9E
PerformanceAdapters.h|440EBD1E0BF4B43E9DCC2637CE3B322A2303DA4B2A0C44F65DC1C372F60A24B2|440EBD1E0BF4B43E9DCC2637CE3B322A2303DA4B2A0C44F65DC1C372F60A24B2
POSTFX_CHAIN.md|E65CAFDBBE1428D343B20112780923561C61B196F4A73ACF62C5B6FFB6B12859|E65CAFDBBE1428D343B20112780923561C61B196F4A73ACF62C5B6FFB6B12859
ProperShadersBatchPolicy.h|5F4CEE210333C6391BF9EB3E6D4C3131FE9E59F00631DCDE857BD744E46B984F|5F4CEE210333C6391BF9EB3E6D4C3131FE9E59F00631DCDE857BD744E46B984F
ProperShadersEffectBindingCache.h|E2D1B93284693FBA3F6017661E123A434E7724C973835D20006A6920AFC9F85E|E2D1B93284693FBA3F6017661E123A434E7724C973835D20006A6920AFC9F85E
ProperShadersPatchValidation.h|DBB17167CBC5D1C973E64D81C31719CE5D1E92B1631735FF09E6E1E46C51E97C|DBB17167CBC5D1C973E64D81C31719CE5D1E92B1631735FF09E6E1E46C51E97C
ProperShadersStateJournal.cpp|8F4A79F9EA89DAD3D8DD0AA083EBC446112617F33518747612C62CCB5519F39C|8F4A79F9EA89DAD3D8DD0AA083EBC446112617F33518747612C62CCB5519F39C
ProperShadersStateJournal.h|3498FFE54CA8C8FADB3333F3B1513A052392E5113E95CFE2ADE5477985B62449|41CF87B0F3A6676A99EF194B008E56C33C5D443C5B8502FBBC811B2665D2E0B1
tests/BridgeD3D9.test.ini|47D59540E27E2874ECD12B1DBE6ADA31108B618682AB642F8BDA125B6E201EBA|47D59540E27E2874ECD12B1DBE6ADA31108B618682AB642F8BDA125B6E201EBA
tests/BridgeLegacyPluginProbe.cpp|722C9355B88D9B78CC10A1F5935D78B142DE2BD71EE87B1159ABE37610BD93F5|722C9355B88D9B78CC10A1F5935D78B142DE2BD71EE87B1159ABE37610BD93F5
tests/BridgeLegacyPluginProbe.def|38FFBFB8B43F2408C21EEFFA5B79650F2D4F9F6A1CE4F68B17973BFE98768ACF|38FFBFB8B43F2408C21EEFFA5B79650F2D4F9F6A1CE4F68B17973BFE98768ACF
tests/BridgeLegacyPluginProbe.vcxproj|26AEEF6C991305437BA13EC6BD09DF56A33AFACA59A040F4BA01294BB9C3A403|8CFC6EF0DEDE3AE853D3D1E8861D89097EFD6C85ADCCB4425EE70F9246EC84F7
tests/BridgeVulkanPassProbe.cpp|39CF43318FAE1B2F810A837019D847EBD80627FE2BEF2EB26AC7F8F43430F649|39CF43318FAE1B2F810A837019D847EBD80627FE2BEF2EB26AC7F8F43430F649
tests/BridgeVulkanPassProbe.def|19391BB27471EE9BF7A33E19DBC008F84E627B48CD1620290AABE3CDA76EF876|19391BB27471EE9BF7A33E19DBC008F84E627B48CD1620290AABE3CDA76EF876
tests/BridgeVulkanPassProbe.vcxproj|1B9E7B508AC8278D561814C51FA8EAAB226E32F0F6D4FE25108FC4C265B11012|323F428D894FC26A39D394276F0146EEC01C5FCF719C1DD1BFD0E8B3BA04E73A
tests/GtaSaCompatApi3Smoke.cpp|AFF96C658187605AF70DAAB29DB6532F9F59205F8D9F164E3774DE5E5FB7D356|AFF96C658187605AF70DAAB29DB6532F9F59205F8D9F164E3774DE5E5FB7D356
tests/GtaSaCompatApi3Smoke.vcxproj|8B513E56B5A4578868023F46BBBFEC3B0E47A6F4AEA0271ABFE2D4AF75145D95|BAA891F40DC83CFCF134D83CFCB7D7B4B073C3FB1AC0348A11214E3AFF2426E1
tests/PerformanceAdapterConfigTests.cpp|5EB765B9471E9BE4C73C2CAD46A34B00189D3C54BCC2C415B68733C968B862CB|5EB765B9471E9BE4C73C2CAD46A34B00189D3C54BCC2C415B68733C968B862CB
tests/PerformanceAdapterConfigTests.vcxproj|8194813D77623466965A9CF9CF126171FC9E65C6AE2AC7B582BE835E8A52CDC8|3B055B72D36467CED1BC4A0FA20DA42F4B044F70D8974EBC94F16E0CF64AB606
tests/PerformanceAdaptersTests.cpp|22DFAD687F85EDED8F7F2041D24527D6AC965CE7DAB4178381D02EDD97F382E3|22DFAD687F85EDED8F7F2041D24527D6AC965CE7DAB4178381D02EDD97F382E3
tests/PerformanceAdaptersTests.vcxproj|113A977BB13884E6AF1B395790A00BEA4E5F2F08FF8857E67E79ED5360C481A3|440F98265A2F734D6C369462FE990A16DBC0D100DD12D44BCB983E56981EFCED
tests/ProperShadersBatchPolicyTests.cpp|0843AC9C3DF2584E32DDC5FCF9393A8D5580813E613D410E46BCA49DDC6D3983|0843AC9C3DF2584E32DDC5FCF9393A8D5580813E613D410E46BCA49DDC6D3983
tests/ProperShadersBatchPolicyTests.vcxproj|2F4D0D38FD74EAF05CBAE03EE40FFB68BD3BEE2BC0E1888FB787EC4E06F6A693|BCC1FA6185F8F14618C070AA9FE09F130D8CF69113ADDB198708C35D370A70D6
tests/ProperShadersStateJournalTests.cpp|B3C521012579E3837B18750C43ACF3F4C7EB8B5EE8BE761831D2FB4DE89512D1|B3C521012579E3837B18750C43ACF3F4C7EB8B5EE8BE761831D2FB4DE89512D1
tests/ProperShadersStateJournalTests.vcxproj|9E406EF79C26E51541A9767635000638FFC752F2CCA3DDA51D734693182811EF|8565E0F0F16CBA12E1D23FB9BB0B1BFEC408806ADBA97B30D6C04E0044791096
'@

$expectedTransformationsText = @'
BridgeD3D9.vcxproj|build-include-relocation|$(ProjectDir)..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include|$(ProjectDir)..\..\..\backend\dxvk\include\vulkan\include;$(D3DX9IncludeDir)
BridgeD3D9BackendTrace.vcxproj|build-include-relocation|$(ProjectDir)..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include|$(ProjectDir)..\..\..\backend\dxvk\include\vulkan\include;$(D3DX9IncludeDir)
EffectInspector.h|dxsdk-header-relocation|#include "../plugin-sdk/shared/dxsdk/d3dx9effect.h"|#include <d3dx9effect.h>
tests/BridgeLegacyPluginProbe.vcxproj|build-include-relocation|$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include|$(ProjectDir)..\..\..\..\backend\dxvk\include\vulkan\include
tests/BridgeVulkanPassProbe.vcxproj|build-include-relocation|$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include|$(ProjectDir)..\..\..\..\backend\dxvk\include\vulkan\include
tests/GtaSaCompatApi3Smoke.vcxproj|build-include-relocation|$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include|$(ProjectDir)..\..\..\..\backend\dxvk\include\vulkan\include
tests/ProperShadersStateJournalTests.vcxproj|build-include-relocation|$(ProjectDir)..\..\dxvk\dxvk-3.0.1-bridge\include\vulkan\include|$(ProjectDir)..\..\..\..\backend\dxvk\include\vulkan\include;$(D3DX9IncludeDir)
BridgeD3D9BackendTrace.vcxproj|build-source-inclusion|<ClCompile Include="BridgeD3D9.cpp" />|<ClCompile Include="BridgeD3D9.cpp" />\n    <ClCompile Include="EffectInspector.cpp" />
ProperShadersStateJournal.h|dxsdk-header-relocation|#include "../plugin-sdk/shared/dxsdk/d3dx9effect.h"|#include <d3dx9effect.h>
'@

$expectedRows = @(
    $expectedRowsText -split "`n" | ForEach-Object {
        $fields = $_.TrimEnd("`r").Split('|')
        if ($fields.Count -ne 3) {
            throw "Invalid literal Bridge anchor row: $_"
        }
        [pscustomobject]@{
            path = $fields[0]
            sourceSha256 = $fields[1]
            sha256 = $fields[2]
        }
    }
)
$expectedTransformations = @(
    $expectedTransformationsText -split "`n" | ForEach-Object {
        $fields = $_.TrimEnd("`r").Split('|')
        if ($fields.Count -ne 4) {
            throw "Invalid literal Bridge transformation row: $_"
        }
        [pscustomobject]@{
            path = $fields[0]
            kind = $fields[1]
            from = $fields[2]
            to = $fields[3].Replace('\n', "`n")
        }
    }
)
if ($expectedRows.Count -ne 38 -or $expectedTransformations.Count -ne 9) {
    throw 'Invalid literal Bridge anchor counts'
}

if (-not ('Task4JsonDuplicatePropertyValidator' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

public static class Task4JsonDuplicatePropertyValidator {
  public static void AssertNoDuplicateProperties(byte[] utf8Json, string label) {
    var scopes = new Stack<HashSet<string>>();
    var reader = new Utf8JsonReader(utf8Json, new JsonReaderOptions {
      AllowTrailingCommas = false,
      CommentHandling = JsonCommentHandling.Disallow,
    });

    while (reader.Read()) {
      if (reader.TokenType == JsonTokenType.StartObject) {
        scopes.Push(new HashSet<string>(StringComparer.Ordinal));
      } else if (reader.TokenType == JsonTokenType.PropertyName) {
        if (scopes.Count == 0)
          throw new InvalidDataException("JSON property outside object in " + label);
        var property = reader.GetString() ?? String.Empty;
        if (!scopes.Peek().Add(property))
          throw new InvalidDataException("Duplicate JSON property '" + property + "' in " + label);
      } else if (reader.TokenType == JsonTokenType.String) {
        var value = reader.GetString() ?? String.Empty;
        if (Path.IsPathRooted(value) || value.StartsWith("\\\\", StringComparison.Ordinal) ||
            value.StartsWith("//", StringComparison.Ordinal))
          throw new InvalidDataException("Rooted JSON string in " + label);
      } else if (reader.TokenType == JsonTokenType.EndObject) {
        if (scopes.Count == 0)
          throw new InvalidDataException("Unexpected JSON object end in " + label);
        scopes.Pop();
      }
    }

    if (scopes.Count != 0)
      throw new InvalidDataException("Unclosed JSON object in " + label);
  }
}
'@
}

function Get-BytesSha256 {
    param([Parameter(Mandatory)] [byte[]]$Bytes)

    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
}

function Get-NormalizedUtf8Text {
    param([Parameter(Mandatory)] [byte[]]$Bytes)

    $text = $strictUtf8.GetString($Bytes)
    if ($text.Length -and $text[0] -eq [char]0xFEFF) {
        $text = $text.Substring(1)
    }
    return $text.Replace("`r`n", "`n").Replace("`r", "`n")
}

function Test-ByteSequenceEqual {
    param(
        [Parameter(Mandatory)] [byte[]]$Left,
        [Parameter(Mandatory)] [byte[]]$Right
    )

    if ($Left.Length -ne $Right.Length) {
        return $false
    }
    for ($index = 0; $index -lt $Left.Length; $index++) {
        if ($Left[$index] -ne $Right[$index]) {
            return $false
        }
    }
    return $true
}

function Assert-CanonicalTrackedText {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Label
    )

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 3 -and
        $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and
        $bytes[2] -eq 0xBF) {
        throw "$Label contains a UTF-8 BOM"
    }
    $text = $strictUtf8.GetString($bytes)
    if ($text.Contains("`r")) {
        throw "$Label is not LF-only"
    }
    if (-not $text.EndsWith("`n", [StringComparison]::Ordinal) -or
        $text.EndsWith("`n`n", [StringComparison]::Ordinal)) {
        throw "$Label must end in exactly one LF"
    }
    return $text
}

function Read-ValidatedJson {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    [Task4JsonDuplicatePropertyValidator]::AssertNoDuplicateProperties($bytes, $Label)
    $text = Assert-CanonicalTrackedText -Path $Path -Label $Label
    return [pscustomobject]@{
        Text = $text
        Value = ($text | ConvertFrom-Json)
    }
}

function Assert-PropertySet {
    param(
        [Parameter(Mandatory)]$Object,
        [Parameter(Mandatory)] [string[]]$Expected,
        [Parameter(Mandatory)] [string]$Label
    )

    $actualNames = ($Object.PSObject.Properties.Name | Sort-Object) -join ','
    $expectedNames = ($Expected | Sort-Object) -join ','
    if ($actualNames -cne $expectedNames) {
        throw "$Label properties differ: expected '$expectedNames', found '$actualNames'"
    }
}

function Assert-NoRootedStrings {
    param(
        [Parameter(Mandatory)]$Object,
        [Parameter(Mandatory)] [string]$Label
    )

    foreach ($property in $Object.PSObject.Properties) {
        $value = $property.Value
        if ($null -eq $value) {
            continue
        }
        if ($value -is [string]) {
            if ([IO.Path]::IsPathRooted($value) -or $value -match '^(\\\\|//)') {
                throw "$Label contains rooted path text in '$($property.Name)'"
            }
        } elseif ($value -is [System.Collections.IEnumerable]) {
            foreach ($item in $value) {
                if ($item -is [string]) {
                    if ([IO.Path]::IsPathRooted($item) -or $item -match '^(\\\\|//)') {
                        throw "$Label contains rooted path text in '$($property.Name)'"
                    }
                } elseif ($null -ne $item -and $item.PSObject.Properties.Count) {
                    Assert-NoRootedStrings -Object $item -Label $Label
                }
            }
        } elseif ($value.PSObject.Properties.Count) {
            Assert-NoRootedStrings -Object $value -Label $Label
        }
    }
}

function Assert-SafeRelativePath {
    param([Parameter(Mandatory)] [string]$Path)

    if ([IO.Path]::IsPathRooted($Path) -or
        @($Path.Split('/')) -contains '..' -or
        $Path -match $forbiddenPathPattern) {
        throw "Forbidden Bridge overlay path: $Path"
    }
}

function Get-ExpectedTransformation {
    param([Parameter(Mandatory)] [string]$Path)

    return @($expectedTransformations | Where-Object { $_.path -ceq $Path })
}

if (-not (Test-Path -LiteralPath $allowlistPath -PathType Leaf)) {
    throw 'Bridge overlay allowlist is missing'
}
$allowlistText = Assert-CanonicalTrackedText -Path $allowlistPath -Label 'Bridge overlay allowlist'
$allowlistSha256 = Get-BytesSha256 -Bytes $utf8NoBom.GetBytes($allowlistText)
if ($allowlistSha256 -cne $expectedAllowlistSha256) {
    throw "Bridge overlay allowlist hash differs: $allowlistSha256"
}
$allowlist = @($allowlistText.TrimEnd("`n") -split "`n")
$literalPaths = @($expectedRows | ForEach-Object { $_.path })
if (($allowlist -join "`n") -cne ($literalPaths -join "`n")) {
    throw 'Bridge overlay allowlist differs from literal path anchors'
}
if (@($allowlist | ForEach-Object { $_.ToLowerInvariant() } |
        Group-Object | Where-Object Count -gt 1).Count) {
    throw 'Bridge overlay allowlist contains case-insensitive duplicates'
}
foreach ($relativePath in $allowlist) {
    Assert-SafeRelativePath -Path $relativePath
}

$bridgeManifestJson = Read-ValidatedJson -Path $bridgeManifestPath -Label 'Bridge overlay manifest'
$bridgeManifest = $bridgeManifestJson.Value
Assert-PropertySet -Object $bridgeManifest -Expected @('sourceLabel', 'files') -Label 'Bridge overlay manifest'
Assert-NoRootedStrings -Object $bridgeManifest -Label 'Bridge overlay manifest'
if ($bridgeManifest.sourceLabel -cne 'BridgeD3D9-audited-20260829') {
    throw "Unexpected Bridge source label: $($bridgeManifest.sourceLabel)"
}
$manifestFiles = @($bridgeManifest.files)
if ($manifestFiles.Count -ne 38) {
    throw "Expected 38 Bridge manifest entries, found $($manifestFiles.Count)"
}
$manifestPaths = @($manifestFiles | ForEach-Object { [string]$_.path })
if ((($manifestPaths | Sort-Object -CaseSensitive) -join "`n") -cne
    (($literalPaths | Sort-Object -CaseSensitive) -join "`n")) {
    throw 'Bridge manifest path set differs from the allowlist'
}
if (@($manifestPaths | ForEach-Object { $_.ToLowerInvariant() } |
        Group-Object | Where-Object Count -gt 1).Count) {
    throw 'Bridge manifest contains case-insensitive duplicate paths'
}

foreach ($entry in $manifestFiles) {
    Assert-PropertySet -Object $entry -Expected @(
        'path', 'sourceSha256', 'sha256', 'normalization', 'transformations'
    ) -Label "Bridge manifest entry '$($entry.path)'"
    $relativePath = [string]$entry.path
    Assert-SafeRelativePath -Path $relativePath
    $expectedRow = @($expectedRows | Where-Object { $_.path -ceq $relativePath })
    if ($expectedRow.Count -ne 1) {
        throw "Bridge path lacks one literal hash anchor: $relativePath"
    }
    if ($entry.sourceSha256 -cne $expectedRow[0].sourceSha256 -or
        $entry.sha256 -cne $expectedRow[0].sha256) {
        throw "Bridge manifest hashes differ for $relativePath"
    }

    $expectedNormalization = if ($relativePath.EndsWith('.vcxproj', [StringComparison]::Ordinal)) {
        'utf8-bomless-crlf'
    } else {
        'utf8-bomless-lf'
    }
    if ($entry.normalization -cne $expectedNormalization) {
        throw "Unexpected Bridge normalization for $relativePath"
    }

    $expectedTransformation = Get-ExpectedTransformation -Path $relativePath
    $actualTransformations = @($entry.transformations)
    if ($actualTransformations.Count -ne $expectedTransformation.Count) {
        throw "Unexpected transformation count for $relativePath"
    }
    for ($transformationIndex = 0;
        $transformationIndex -lt $expectedTransformation.Count;
        $transformationIndex++) {
        $actualTransformation = $actualTransformations[$transformationIndex]
        $expectedTransformationRow = $expectedTransformation[$transformationIndex]
        Assert-PropertySet -Object $actualTransformation -Expected @(
            'kind', 'from', 'to'
        ) -Label "Transformation '$relativePath' index $transformationIndex"
        if ($actualTransformation.kind -cne $expectedTransformationRow.kind -or
            $actualTransformation.from -cne $expectedTransformationRow.from -or
            $actualTransformation.to -cne $expectedTransformationRow.to) {
            throw "Transformation tuple differs for $relativePath at index $transformationIndex"
        }
    }

    $destinationPath = Join-Path $bridgeDestination $relativePath
    if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
        throw "Imported Bridge file is missing: $relativePath"
    }
    $destinationBytes = [IO.File]::ReadAllBytes($destinationPath)
    if ((Get-BytesSha256 -Bytes $destinationBytes) -cne $expectedRow[0].sha256) {
        throw "Imported Bridge file hash differs: $relativePath"
    }
    if ($destinationBytes.Length -ge 3 -and
        $destinationBytes[0] -eq 0xEF -and
        $destinationBytes[1] -eq 0xBB -and
        $destinationBytes[2] -eq 0xBF) {
        throw "Imported Bridge file contains a BOM: $relativePath"
    }
    $destinationText = $strictUtf8.GetString($destinationBytes)
    if ($expectedNormalization -eq 'utf8-bomless-crlf') {
        if (-not $destinationText.Contains("`r`n") -or
            $destinationText.Replace("`r`n", '').Contains("`r") -or
            $destinationText.Replace("`r`n", '').Contains("`n")) {
            throw "Imported project is not strictly CRLF: $relativePath"
        }
    } elseif ($destinationText.Contains("`r")) {
        throw "Imported Bridge file is not LF-only: $relativePath"
    }

    if ($liveAudit) {
        $sourcePath = Join-Path $BridgeSource $relativePath
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Audited Bridge source file is missing: $relativePath"
        }
        $sourceBytes = [IO.File]::ReadAllBytes($sourcePath)
        if ((Get-BytesSha256 -Bytes $sourceBytes) -cne $expectedRow[0].sourceSha256) {
            throw "Audited Bridge source hash differs: $relativePath"
        }
        $derivedText = Get-NormalizedUtf8Text -Bytes $sourceBytes
        foreach ($expectedTransformationRow in $expectedTransformation) {
            $oldText = $expectedTransformationRow.from
            $matchCount = ([regex]::Matches($derivedText, [regex]::Escape($oldText))).Count
            if ($matchCount -ne 1) {
                throw "Audited Bridge transformation source count differs for $relativePath"
            }
            $derivedText = $derivedText.Replace($oldText, $expectedTransformationRow.to)
        }
        if ($expectedNormalization -eq 'utf8-bomless-crlf') {
            $derivedText = $derivedText.Replace("`n", "`r`n")
        }
        $derivedBytes = $utf8NoBom.GetBytes($derivedText)
        if (-not (Test-ByteSequenceEqual -Left $derivedBytes -Right $destinationBytes)) {
            throw "Audited Bridge source does not derive the committed file: $relativePath"
        }
    }
}

$runtimeManifestJson = Read-ValidatedJson -Path $runtimeManifestPath -Label 'Runtime config manifest'
$runtimeManifest = $runtimeManifestJson.Value
Assert-PropertySet -Object $runtimeManifest -Expected @('sourceLabel', 'files') -Label 'Runtime config manifest'
Assert-NoRootedStrings -Object $runtimeManifest -Label 'Runtime config manifest'
if ($runtimeManifest.sourceLabel -cne 'active-game-profile-audited-20260829') {
    throw "Unexpected runtime config source label: $($runtimeManifest.sourceLabel)"
}

$runtimeFiles = @($runtimeManifest.files)
if ($runtimeFiles.Count -ne 2) {
    throw "Expected two runtime config manifest entries, found $($runtimeFiles.Count)"
}
$expectedRuntimePaths = @(
    'config/SA.RenderStack.ini',
    'config/dxvk.conf'
)
$expectedRuntimeSourceHashes = @(
    $expectedBridgeConfigSourceSha256,
    $expectedDxvkConfigSha256
)
$expectedRuntimeNormalizedSourceHashes = @(
    $expectedNormalizedBridgeConfigSha256,
    $expectedDxvkConfigSha256
)
$expectedRuntimeHashes = @(
    $expectedGeneratedBridgeConfigSha256,
    $expectedDxvkConfigSha256
)
for ($runtimeIndex = 0; $runtimeIndex -lt $runtimeFiles.Count; $runtimeIndex++) {
    $runtimeFile = $runtimeFiles[$runtimeIndex]
    $runtimePath = [string]$runtimeFile.path
    Assert-PropertySet -Object $runtimeFile -Expected @(
        'path', 'sourceSha256', 'normalizedSourceSha256', 'sha256',
        'normalization', 'transformations'
    ) -Label "Runtime config entry '$runtimePath'"
    if ($runtimePath -cne $expectedRuntimePaths[$runtimeIndex]) {
        throw "Runtime config manifest order differs at index $runtimeIndex"
    }
    if ($runtimeFile.sourceSha256 -cne $expectedRuntimeSourceHashes[$runtimeIndex] -or
        $runtimeFile.normalizedSourceSha256 -cne $expectedRuntimeNormalizedSourceHashes[$runtimeIndex] -or
        $runtimeFile.sha256 -cne $expectedRuntimeHashes[$runtimeIndex]) {
        throw "Runtime config hashes differ for $runtimePath"
    }
    if ($runtimeFile.normalization -cne 'utf8-bomless-lf') {
        throw "Unexpected runtime config normalization for $runtimePath"
    }

    $runtimeTransformations = @($runtimeFile.transformations)
    if ($runtimeIndex -eq 0) {
        if ($runtimeTransformations.Count -ne 1) {
            throw 'Bridge runtime config must contain one transformation'
        }
        Assert-PropertySet -Object $runtimeTransformations[0] -Expected @(
            'section', 'key', 'from', 'to'
        ) -Label 'Runtime Bridge config transformation'
        if ($runtimeTransformations[0].section -cne 'Backend' -or
            $runtimeTransformations[0].key -cne 'DxvkBackendDir' -or
            $runtimeTransformations[0].from -cne 'dxvk-3.0.1-merged' -or
            $runtimeTransformations[0].to -cne 'backend\dxvk-gta') {
            throw 'Runtime Bridge config transformation differs from the packaging relocation'
        }
    } elseif ($runtimeTransformations.Count -ne 0) {
        throw 'DXVK runtime config must contain zero transformations'
    }
}

foreach ($configPath in @($generatedBridgeConfigPath, $generatedDxvkConfigPath)) {
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "Generated runtime config is missing: $configPath"
    }
    $configBytes = [IO.File]::ReadAllBytes($configPath)
    if ($configBytes.Length -ge 3 -and
        $configBytes[0] -eq 0xEF -and
        $configBytes[1] -eq 0xBB -and
        $configBytes[2] -eq 0xBF) {
        throw "Generated runtime config contains a BOM: $configPath"
    }
    $configText = $strictUtf8.GetString($configBytes)
    if ($configText.Contains("`r")) {
        throw "Generated runtime config is not LF-only: $configPath"
    }
}

$generatedBridgeConfigBytes = [IO.File]::ReadAllBytes($generatedBridgeConfigPath)
if ((Get-BytesSha256 -Bytes $generatedBridgeConfigBytes) -cne $expectedGeneratedBridgeConfigSha256) {
    throw 'Generated SA.RenderStack.ini hash differs'
}
$generatedBridgeConfigText = $strictUtf8.GetString($generatedBridgeConfigBytes)
$bridgeConfigLines = [Collections.Generic.List[string]]::new()
$bridgeConfigLines.AddRange([string[]]($generatedBridgeConfigText -split "`n"))
$currentSection = ''
$backendKeyIndexes = [Collections.Generic.List[int]]::new()
for ($lineIndex = 0; $lineIndex -lt $bridgeConfigLines.Count; $lineIndex++) {
    $line = $bridgeConfigLines[$lineIndex]
    if ($line -cmatch '^\s*\[([^]]+)\]\s*(?:[;#].*)?$') {
        $currentSection = $Matches[1]
        continue
    }
    if ($currentSection -ceq 'Backend' -and
        $line -cmatch '^\s*DxvkBackendDir\s*=\s*([^;#]*?)\s*(?:[;#].*)?$') {
        if ($Matches[1] -cne 'backend\dxvk-gta') {
            throw "Generated Backend DxvkBackendDir has an unexpected value: $($Matches[1])"
        }
        $backendKeyIndexes.Add($lineIndex)
    }
}
if ($backendKeyIndexes.Count -ne 1) {
    throw "Expected one generated Backend DxvkBackendDir key, found $($backendKeyIndexes.Count)"
}
$changedLineIndex = $backendKeyIndexes[0]
$sourceLine = $bridgeConfigLines[$changedLineIndex].Replace('backend\dxvk-gta', 'dxvk-3.0.1-merged')
if ($sourceLine -ceq $bridgeConfigLines[$changedLineIndex]) {
    throw 'Generated Bridge config relocation cannot be reversed'
}
$bridgeConfigLines[$changedLineIndex] = $sourceLine
$reconstructedNormalizedBridgeConfig = $bridgeConfigLines -join "`n"
$reconstructedNormalizedBridgeBytes = $utf8NoBom.GetBytes($reconstructedNormalizedBridgeConfig)
if ((Get-BytesSha256 -Bytes $reconstructedNormalizedBridgeBytes) -cne $expectedNormalizedBridgeConfigSha256) {
    throw 'Generated Bridge config differs from normalized active input beyond the declared line'
}

$generatedDxvkConfigBytes = [IO.File]::ReadAllBytes($generatedDxvkConfigPath)
if ((Get-BytesSha256 -Bytes $generatedDxvkConfigBytes) -cne $expectedDxvkConfigSha256) {
    throw 'Generated dxvk.conf hash differs'
}

if ($liveAudit) {
    if (-not (Test-Path -LiteralPath $ActiveBridgeConfig -PathType Leaf) -or
        -not (Test-Path -LiteralPath $ActiveDxvkConfig -PathType Leaf)) {
        throw 'Live runtime config inputs are missing'
    }
    $activeBridgeBytes = [IO.File]::ReadAllBytes($ActiveBridgeConfig)
    $activeDxvkBytes = [IO.File]::ReadAllBytes($ActiveDxvkConfig)
    if ((Get-BytesSha256 -Bytes $activeBridgeBytes) -cne $expectedBridgeConfigSourceSha256) {
        throw 'Active Bridge config raw hash differs'
    }
    if ((Get-BytesSha256 -Bytes $activeDxvkBytes) -cne $expectedDxvkConfigSha256) {
        throw 'Active DXVK config raw hash differs'
    }
    $normalizedActiveBridgeBytes = $utf8NoBom.GetBytes((Get-NormalizedUtf8Text -Bytes $activeBridgeBytes))
    $normalizedActiveDxvkBytes = $utf8NoBom.GetBytes((Get-NormalizedUtf8Text -Bytes $activeDxvkBytes))
    if (-not (Test-ByteSequenceEqual -Left $normalizedActiveBridgeBytes -Right $reconstructedNormalizedBridgeBytes)) {
        throw 'Active Bridge config does not derive the generated config'
    }
    if (-not (Test-ByteSequenceEqual -Left $normalizedActiveDxvkBytes -Right $generatedDxvkConfigBytes)) {
        throw 'Active DXVK config does not derive the generated config'
    }
}

Write-Output 'PASS Bridge migration manifests and runtime config anchors'
