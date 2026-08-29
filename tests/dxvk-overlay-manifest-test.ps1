param(
    [string]$Source,
    [string]$BaselineCommit = '07d715df896a1b54d8e08086435408b38f688fae'
)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'helpers/git-baseline.ps1')
$DxvkBaselineCommit = $BaselineCommit
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$liveAudit = $PSBoundParameters.ContainsKey('Source')
$expectedAllowlistSha256 = '471A118A195A758CB6C4D1F9ADEA8A5DCEE09F2E256F54D6B4F551249ECF4C9D'
$forbiddenPathPattern = '(^|/)build(/|$)|(^|/)\.wraplock$|\.bak$|\.log$|\.exe$|\.dll$|\.pdb$|\.obj$|\.o$'

if ($liveAudit -and [string]::IsNullOrWhiteSpace($Source)) {
    throw 'Live DXVK overlay audit requires a non-empty -Source path'
}
if ($DxvkBaselineCommit -cnotmatch '\A[0-9a-fA-F]{40}\z') {
    throw 'DXVK baseline override must be a full 40-character hexadecimal Git commit'
}

Assert-GitFullHistory
Assert-GitCommit -Commit $DxvkBaselineCommit -Label 'DXVK source baseline'

$expectedRowsText = @'
meson_options.txt|75DD24BAF0C080D013AFDE82ABBFC913FBD1E818E88FABEF48604E4C1EA1E054|E08A0E9E84F058955C8B67F0D74AE4D3D0AE221F1635D6BD9FFACF2E2F5835A4
src/d3d9/d3d9_device.cpp|A856C83A18EBDB32D877A25F10060EE611B3E07B2AEEA76096058293EC4851F7|E20BA109B5F60C83579A11C1C5D26F552ACBF2A80518C646C8A61952C6CC48C8
src/d3d9/d3d9_device.h|F27A8565261801A1F97D880BA613DDEBF59574BCD6E7D5D14FE87969D54519D0|EFD9B9F2569AD59FE0D2F033F8C17A39F1B3A8E4E725F4025DC213550D17D70B
src/d3d9/d3d9_interfaces.h|25FAEA033D35A5F6B9156BB6E43817D91C1B56374F228B301A42DFD425433263|822A2E89FA50A420B1D82C888B336CAD21EB165405D5872C471541610295B991
src/d3d9/d3d9_options.cpp|F2FF365ABB5F695D4CB72C589957C529C7052BA97A08657C7F019164DE01D780|809CF861A5BDA1001692C596F4D30AF881E6B52E901DE15636B7284501C8A096
src/d3d9/d3d9_options.h|B4EC4BAE5C31FFC4D186B244A8C255CEFA9799D1B3B7AD351524744AD5820CED|E4844976F6B618C6A7FD2A5098E3385DFC48475800DEF422DB273D47B09E0FD8
src/d3d9/d3d9_state.h|1F205E8EF4FC54F9C5AE34344E74E439E99B6C0F377A9DFCB58BCB1F1C6A2D61|28A72190BC26798AB8B8103F3DE8E70F90B126498946BEE52961A57C5D81D0CE
src/d3d9/d3d9_stateblock.cpp|984530F71967D18D73A0433806B8C897FDE9C11B5D5EB1D9F01BC827680ABBB4|41CC126D2FEDB6082779E193EEDF7B9F4AC4D79879C60FD5BBD0EE32F454FD53
src/d3d9/d3d9_stateblock.h|29EC73C113BA7C22A4D5EBC91125D893CE5A8880611337B3310441BC089B6EF5|2DDAFAFCF701BB4DFAD58D2F3E3727B6773102DDC546E2C112B5B6318514476A
src/d3d9/d3d9_swapchain.cpp|0C1CCAAA9674B727732819FE960E98FD8F8428178A1D56F066E1758A6FFD9315|6FB561DB533D095050F4F0F38FD84D75F9D2BF269904BC80707F83C3BB6A5341
src/d3d9/meson.build|A0B160FBBE030FED37C2F21AD23B42B395D33C9A0D2AA998B90969CE67BC6CD7|ABF141F3FFD540EC0233E88713BF2E91F88C3E71EB69FE70F3E65F9F21702AD1
src/dxgi/dxgi_main.cpp|5ABEE0C31191D0BBA63D594DEC8213AF4FE9D9D15CFF0D85912C446C14CB2E6F|FB2B91A5A3082D4121FC91DBD6352E1FE6681A1A98DDD3BE54BFCCCE4F7D957A
src/dxvk/dxvk_cmdlist.h|B279143A752F79539266B18DAEE02FAADEEB6B25D486632C46A3C7C7559436FF|A84530DDF189AF468B9FB52503183E5C0BD8436E94141EEABBC50C7E3BD649E5
src/dxvk/dxvk_context.cpp|0BAC13C3459B1C220090C10647A24B230C25299FFDF13DE283A4D96CB5636DA2|F14E16DFCAB3FA782AF35411CC309FFFC15D12A11EC7EB3B6B9CF02B5EE4F5B5
src/dxvk/dxvk_context.h|5D6E9CAA395437E527E3416543EAC1568170A85A667B6A0653A6C42E895E1493|E6E6861ECADE23ACF807495A4B9AFE1F17E1A37B8C3137EFBB629C022DE314AB
src/dxvk/dxvk_graphics_state.h|1E1A0CCC0FE2CE22CB0871A4218DB42C139311B15EA02DB7C3D00F415EB6DFEC|F7D6A5AE34595D22D9F3AA4AB1FEC1FBA0780D604486BC6277D5DC50C00874FE
src/dxvk/dxvk_stats.h|B9B3E7FE9636D68D26371401E676BE0B29441253560553C03A8BF24B7B77AF14|EB906BEA4D2AF79B245486387FDEC19AB2BC8CE6EEAEBC799B97A5B35F6C34FC
src/util/config/config.cpp|1F3F511DE07BA6E647DCA7FBE950D496F02E52819C243A3977B0FD90FBC62A4F|67902680E24E85FA83FFC13906C34116AEDD55BE09C9EA1E09486E31EA828E1A
src/util/util_bit.h|6020BBF3A2ADDA4501A70A3B44F169BE1269DAB071F072AF762C37667976B099|63B2A8D906D7182BF139193F683568AA7B6D822842F79154D3A0DDBD9107BA74
include/d3d9_gta_sa_api.h|98A18E993376E911FD7297772C2BDE6E95DA6D8AB6091E9608DDCE3694AE3F79|98A18E993376E911FD7297772C2BDE6E95DA6D8AB6091E9608DDCE3694AE3F79
src/d3d9/d3d9_gta_sa_compat.cpp|CB0E222E485C3DEFFA86A7CC42748A87E22D41CDD6D1EAE807412089748E958D|CB0E222E485C3DEFFA86A7CC42748A87E22D41CDD6D1EAE807412089748E958D
src/d3d9/d3d9_gta_sa_compat.h|A2D3C732DFE761761F603B83F389B653A383CB91BE91BEFB1EDBB240DFD918DD|A2D3C732DFE761761F603B83F389B653A383CB91BE91BEFB1EDBB240DFD918DD
src/d3d9/d3d9_gta_sa_deferred_binding.h|1989BD84694DB3FAADAB418ED2AA940152831B35170628CAC2676D60B70814ED|1989BD84694DB3FAADAB418ED2AA940152831B35170628CAC2676D60B70814ED
src/d3d9/d3d9_merged.def|398C0531C1C5865C6D6FEEB01F1C5447AB25355BAFF1635A5A126879A16B5343|398C0531C1C5865C6D6FEEB01F1C5447AB25355BAFF1635A5A126879A16B5343
GTA_SA_COMPAT.md|766E996FBB27437F579D4F180C6B4EEAE245B601CAC7D007F3B8714783DC6902|766E996FBB27437F579D4F180C6B4EEAE245B601CAC7D007F3B8714783DC6902
tools/gta_sa_compat_probe.cpp|3B18BFB2E1B512B18C747615441F7EAD6A108AFF66E5351D9CAFFA87005FC5F8|3B18BFB2E1B512B18C747615441F7EAD6A108AFF66E5351D9CAFFA87005FC5F8
tools/stateblock_prefilter_probe.cpp|1EB7F9FA66E678426A709CF30F8DE93D433BB642C1F7BD55520E257CE65793CC|1EB7F9FA66E678426A709CF30F8DE93D433BB642C1F7BD55520E257CE65793CC
tools/d3d9_batch_audit_test.cpp|A6B07F80A0EBF18642A65F804B14FF78A5B8435F0670556CD36C79B8E44AE587|A6B07F80A0EBF18642A65F804B14FF78A5B8435F0670556CD36C79B8E44AE587
tools/d3d9_deferred_shader_binding_test.cpp|8E37D884BEBC54EDBF3E2FF158A545F659A36F9ABDE23E0931819FECDCCF08A2|8E37D884BEBC54EDBF3E2FF158A545F659A36F9ABDE23E0931819FECDCCF08A2
tools/dxvk_state_dedup_test.cpp|BDA34449CB39D901A9BA99033519833F347ECF52AC6AA18E00CB21E16A51490D|BDA34449CB39D901A9BA99033519833F347ECF52AC6AA18E00CB21E16A51490D
'@

$expectedRows = @(
    $expectedRowsText -split "`n" |
        ForEach-Object {
            $fields = $_.TrimEnd("`r").Split('|')
            if ($fields.Count -ne 3) {
                throw "Invalid literal DXVK overlay anchor row: $_"
            }
            [pscustomobject]@{
                path = $fields[0]
                sourceSha256 = $fields[1]
                sha256 = $fields[2]
            }
        }
)

if ($expectedRows.Count -ne 30) {
    throw "Invalid literal DXVK overlay anchor count: $($expectedRows.Count)"
}

if (-not ('Task3JsonDuplicatePropertyValidator' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

public static class Task3JsonDuplicatePropertyValidator {
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
          throw new InvalidDataException(
            "Duplicate JSON property '" + property + "' in " + label);
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

function Get-Sha256 {
    param([Parameter(Mandatory)] [byte[]] $Bytes)

    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
}

function Get-CanonicalUtf8Text {
    param([Parameter(Mandatory)] [byte[]] $Bytes)

    $text = $strictUtf8.GetString($Bytes)
    if ($text.Length -and $text[0] -eq [char]0xFEFF) {
        $text = $text.Substring(1)
    }
    return $text.Replace("`r`n", "`n").Replace("`r", "`n")
}

function Read-ValidatedJson {
    param(
        [Parameter(Mandatory)] [byte[]] $Bytes,
        [Parameter(Mandatory)] [string] $Label
    )

    try {
        [Task3JsonDuplicatePropertyValidator]::AssertNoDuplicateProperties($Bytes, $Label)
    } catch {
        $exception = $_.Exception
        while ($exception.InnerException) {
            $exception = $exception.InnerException
        }
        throw $exception.Message
    }

    $text = $strictUtf8.GetString($Bytes)
    return [pscustomobject]@{
        Text = $text
        Value = ($text | ConvertFrom-Json)
    }
}

function Assert-ValidOverlayPath {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Label
    )

    if ([IO.Path]::IsPathRooted($Path) -or
        @($Path.Split('/')) -contains '..' -or
        $Path -match $forbiddenPathPattern) {
        throw "Forbidden $Label entry: $Path"
    }
}

function Get-CanonicalPropertyNames {
    param([Parameter(Mandatory)] $Object)

    return @(($Object.PSObject.Properties.Name | Sort-Object) -join ',')
}

function Assert-PropertySet {
    param(
        [Parameter(Mandatory)] $Object,
        [Parameter(Mandatory)] [string[]] $Expected,
        [Parameter(Mandatory)] [string] $Label
    )

    $actualNames = Get-CanonicalPropertyNames -Object $Object
    $expectedNames = @(($Expected | Sort-Object) -join ',')
    if ($actualNames -ne $expectedNames) {
        throw "$Label properties differ: expected '$expectedNames', found '$actualNames'"
    }
}

function Assert-NoRootedStrings {
    param(
        [Parameter(Mandatory)] $Object,
        [Parameter(Mandatory)] [string] $Label
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
            continue
        }
        if ($value -is [System.Collections.IEnumerable]) {
            foreach ($item in $value) {
                if ($item -is [string]) {
                    if ([IO.Path]::IsPathRooted($item) -or $item -match '^(\\\\|//)') {
                        throw "$Label contains rooted path text in '$($property.Name)'"
                    }
                } elseif ($item -and $item.PSObject.Properties.Count) {
                    Assert-NoRootedStrings -Object $item -Label $Label
                }
            }
        } elseif ($value.PSObject.Properties.Count) {
            Assert-NoRootedStrings -Object $value -Label $Label
        }
    }
}

$allowlistBytes = Read-GitBlob -Commit $DxvkBaselineCommit `
    -Path 'tools/migration/dxvk-overlay-files.txt' `
    -Label 'DXVK overlay allowlist'
$allowlistText = Get-CanonicalUtf8Text -Bytes $allowlistBytes
$allowlist = @(
    $allowlistText -split "`n" |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith('#') } |
        ForEach-Object { $_.Replace('\', '/') }
)
if ($allowlist.Count -ne 30) {
    throw "Expected 30 DXVK overlay allowlist entries, found $($allowlist.Count)"
}
if (@($allowlist | ForEach-Object { $_.ToLowerInvariant() } |
        Group-Object | Where-Object Count -gt 1).Count) {
    throw 'DXVK overlay allowlist contains duplicate paths'
}
foreach ($relative in $allowlist) {
    Assert-ValidOverlayPath -Path $relative -Label 'overlay allowlist'
}

if (-not $allowlistText.EndsWith("`n", [StringComparison]::Ordinal)) {
    throw 'DXVK overlay allowlist must end with LF'
}
$allowlistSha256 = Get-Sha256 -Bytes $allowlistBytes
if ($allowlistSha256 -cne $expectedAllowlistSha256) {
    throw "DXVK overlay allowlist hash differs: $allowlistSha256"
}
$literalPaths = @($expectedRows | ForEach-Object { $_.path })
if (($allowlist -join "`n") -cne ($literalPaths -join "`n")) {
    throw 'DXVK overlay allowlist entries differ from literal anchors'
}

$manifestBytes = Read-GitBlob -Commit $DxvkBaselineCommit `
    -Path 'migration/dxvk-overlay-manifest.json' `
    -Label 'DXVK overlay manifest'
$manifestJson = Read-ValidatedJson -Bytes $manifestBytes -Label 'DXVK overlay manifest'
$manifestText = $manifestJson.Text
$manifest = $manifestJson.Value
Assert-PropertySet -Object $manifest -Expected @('sourceLabel', 'upstreamCommit', 'files') -Label 'Overlay manifest'
Assert-NoRootedStrings -Object $manifest -Label 'Overlay manifest'
if ($manifest.sourceLabel -ne 'dxvk-3.0.1-bridge-audited-20260828') {
    throw "Unexpected overlay source label: $($manifest.sourceLabel)"
}
if ($manifest.upstreamCommit -ne 'c850747f1df24180ce97b7a9094603f39da1251d') {
    throw "Unexpected overlay upstream commit: $($manifest.upstreamCommit)"
}

$manifestFiles = @($manifest.files)
if ($manifestFiles.Count -ne 30) {
    throw "Expected 30 DXVK overlay manifest entries, found $($manifestFiles.Count)"
}
$manifestPaths = @($manifestFiles | ForEach-Object { [string]$_.path })
$expectedPaths = @(($literalPaths | Sort-Object -CaseSensitive) -join "`n")
$actualPaths = @(($manifestPaths | Sort-Object -CaseSensitive) -join "`n")
if ($actualPaths -ne $expectedPaths) {
    throw 'DXVK overlay manifest paths differ from literal anchors'
}
if (@($manifestPaths | ForEach-Object { $_.ToLowerInvariant() } |
        Group-Object | Where-Object Count -gt 1).Count) {
    throw 'DXVK overlay manifest contains duplicate paths'
}

foreach ($entry in $manifestFiles) {
    Assert-PropertySet -Object $entry -Expected @('path', 'sourceSha256', 'sha256', 'normalization') -Label "Overlay entry '$($entry.path)'"
    $relative = [string]$entry.path
    Assert-ValidOverlayPath -Path $relative -Label 'overlay manifest'
    $expected = @($expectedRows | Where-Object { $_.path -ceq $relative })
    if ($expected.Count -ne 1) {
        throw "DXVK overlay manifest path is not independently anchored: $relative"
    }
    if ($entry.sourceSha256 -cne $expected[0].sourceSha256) {
        throw "DXVK overlay source hash differs from literal anchor for $relative"
    }
    if ($entry.sha256 -cne $expected[0].sha256) {
        throw "DXVK overlay canonical hash differs from literal anchor for $relative"
    }
    if ($entry.normalization -ne 'utf8-bomless-lf') {
        throw "Unexpected normalization for $relative"
    }

    $destinationBytes = Read-GitBlob -Commit $DxvkBaselineCommit `
        -Path "backend/dxvk/$relative" `
        -Label "DXVK baseline destination '$relative'"
    $destinationSha256 = Get-Sha256 -Bytes $destinationBytes
    if ($destinationSha256 -cne $expected[0].sha256) {
        throw "Historical destination canonical hash differs for $relative"
    }
    if ($destinationBytes.Length -ge 3 -and
        $destinationBytes[0] -eq 0xEF -and
        $destinationBytes[1] -eq 0xBB -and
        $destinationBytes[2] -eq 0xBF) {
        throw "UTF-8 BOM remains in imported destination: $relative"
    }
    if ($destinationBytes -contains 13) {
        throw "CR byte remains in imported destination: $relative"
    }
    $null = $strictUtf8.GetString($destinationBytes)

    if ($liveAudit) {
        $sourcePath = Join-Path $Source $relative
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Audited source file is missing: $relative"
        }
        $sourceBytes = [IO.File]::ReadAllBytes($sourcePath)
        $sourceSha256 = Get-Sha256 -Bytes $sourceBytes
        if ($sourceSha256 -cne $expected[0].sourceSha256) {
            throw "Audited source hash differs from literal anchor for $relative"
        }

        $normalizedSourceText = Get-CanonicalUtf8Text -Bytes $sourceBytes
        $normalizedSourceBytes = $utf8NoBom.GetBytes($normalizedSourceText)
        $normalizedSourceSha256 = Get-Sha256 -Bytes $normalizedSourceBytes
        if ($normalizedSourceSha256 -cne $expected[0].sha256 -or
            $normalizedSourceSha256 -cne $destinationSha256) {
            throw "Normalized audited source hash differs from historical destination for $relative"
        }
        if (-not [Linq.Enumerable]::SequenceEqual[byte](
                $normalizedSourceBytes,
                $destinationBytes)) {
            throw "Normalized audited source bytes differ from historical destination for $relative"
        }
    }
}

$evidenceBytes = Read-GitBlob -Commit $DxvkBaselineCommit `
    -Path 'migration/dxvk-baseline-evidence.json' `
    -Label 'DXVK baseline evidence'
$evidenceJson = Read-ValidatedJson -Bytes $evidenceBytes -Label 'DXVK baseline evidence'
$evidenceText = $evidenceJson.Text
if ($evidenceText -match '(?i)\.exe') {
    throw 'DXVK baseline evidence contains forbidden .exe text'
}
$evidence = $evidenceJson.Value
Assert-PropertySet -Object $evidence -Expected @(
    'sourceReportSha256',
    'auditedBackendSha256',
    'priorProbeResults',
    'probeSourceSha256'
) -Label 'Baseline evidence'
Assert-NoRootedStrings -Object $evidence -Label 'Baseline evidence'

if ($evidence.sourceReportSha256 -cne 'BD99955F9FB6BB51196B857CF13720F9F6B6EB83384B89DC5290F8ED9B3FA100') {
    throw 'Unexpected prior verification report hash'
}
if ($evidence.auditedBackendSha256 -cne '69454C02480981686731B7975EDEA5452E64F02425624BEA410C3A432933FF5F') {
    throw 'Unexpected audited backend hash'
}

$expectedProbeNames = @(
    'd3d9_batch_audit_test',
    'd3d9_deferred_shader_binding_test',
    'dxvk_state_dedup_test',
    'stateblock_prefilter_probe',
    'gta_sa_compat_probe'
)
$priorResults = @($evidence.priorProbeResults)
if ($priorResults.Count -ne 5) {
    throw "Expected five prior probe results, found $($priorResults.Count)"
}
foreach ($result in $priorResults) {
    Assert-PropertySet -Object $result -Expected @('name', 'exitCode') -Label "Prior probe '$($result.name)'"
    if ([int]$result.exitCode -ne 0) {
        throw "Prior probe did not record a zero exit: $($result.name)"
    }
}
$actualProbeNames = @(($priorResults.name | Sort-Object) -join "`n")
$sortedExpectedProbeNames = @(($expectedProbeNames | Sort-Object) -join "`n")
if ($actualProbeNames -ne $sortedExpectedProbeNames) {
    throw 'Prior probe result names differ from the required set'
}

$expectedProbeSources = @(
    $expectedRows |
        Where-Object { $_.path -like 'tools/*' } |
        Sort-Object path |
        ForEach-Object {
            [pscustomobject]@{
                path = [string]$_.path
                sourceSha256 = [string]$_.sourceSha256
            }
        }
)
$probeSources = @($evidence.probeSourceSha256)
if ($probeSources.Count -ne 5) {
    throw "Expected five probe source hashes, found $($probeSources.Count)"
}
for ($index = 0; $index -lt $probeSources.Count; $index++) {
    $actual = $probeSources[$index]
    $expected = $expectedProbeSources[$index]
    Assert-PropertySet -Object $actual -Expected @('path', 'sourceSha256') -Label "Probe source '$($actual.path)'"
    if ($actual.path -cne $expected.path -or $actual.sourceSha256 -cne $expected.sourceSha256) {
        throw "Probe source evidence differs at index $index"
    }
}

Write-Output 'PASS DXVK overlay manifest and baseline evidence'
