$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exportsPath = Join-Path $root 'migration/bridge-exports.txt'
$evidencePath = Join-Path $root 'migration/bridge-build-evidence.json'
$candidatePath = Join-Path $root 'out/build/bridge/d3d9.dll'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$expectedReferenceSha256 = 'B0BBE0C98B132EA0B8E6BA0FA2C978D82C48063A50C2324BE3A8F7141FE7E9FF'
$expectedExportsSha256 = '93B2438F39A97F664BE6B3F2791C36981358B18647FE463BEEC94E0F66DA76B0'
$expectedExportsText = @'
1 Direct3DCreate9
2 DebugSetLevel
3 DebugSetMute
4 Direct3DShaderValidatorCreate9
5 PSGPError
6 PSGPSampleTexture
7 CreateDXGIFactory1
8 CreateDXGIFactory2
9 CreateDXGIFactory
10 DXGIDeclareAdapterRemovalSupport
11 DXGIGetDebugInterface1
'@ + "`n"

if (-not ('Task4BuildJsonDuplicatePropertyValidator' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

public static class Task4BuildJsonDuplicatePropertyValidator {
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

function Assert-CanonicalTrackedText {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
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
    return [pscustomobject]@{
        Bytes = $bytes
        Text = $text
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

function Assert-ExactType {
    param(
        [Parameter(Mandatory)]$Value,
        [Parameter(Mandatory)] [type]$ExpectedType,
        [Parameter(Mandatory)] [string]$Label
    )

    $actualType = if ($null -eq $Value) { '<null>' } else { $Value.GetType().FullName }
    if ($actualType -cne $ExpectedType.FullName) {
        throw "$Label type differs: expected $($ExpectedType.FullName), found $actualType"
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
        } elseif ($value.PSObject.Properties.Count) {
            Assert-NoRootedStrings -Object $value -Label $Label
        }
    }
}

$exportsFile = Assert-CanonicalTrackedText -Path $exportsPath -Label 'Bridge export evidence'
if ($exportsFile.Text -cne $expectedExportsText) {
    throw 'Bridge export rows differ from the independent exact rows'
}
if ((Get-BytesSha256 -Bytes $exportsFile.Bytes) -cne $expectedExportsSha256) {
    throw 'Bridge export evidence hash differs'
}

$evidenceFile = Assert-CanonicalTrackedText -Path $evidencePath -Label 'Bridge build evidence'
[Task4BuildJsonDuplicatePropertyValidator]::AssertNoDuplicateProperties(
    $evidenceFile.Bytes,
    'Bridge build evidence')
$evidence = $evidenceFile.Text | ConvertFrom-Json
Assert-PropertySet -Object $evidence -Expected @(
    'schemaVersion',
    'reference',
    'candidate',
    'exportsEqual',
    'msbuildVersion',
    'toolsetVersion',
    'compilerVersion',
    'binaryHashExpectedToDiffer'
) -Label 'Bridge build evidence'
Assert-NoRootedStrings -Object $evidence -Label 'Bridge build evidence'
Assert-ExactType -Value $evidence.schemaVersion -ExpectedType ([Int64]) `
    -Label 'Bridge build evidence schemaVersion'
if ($evidence.schemaVersion -ne 1) {
    throw "Unexpected Bridge build evidence schema: $($evidence.schemaVersion)"
}
foreach ($binaryLabel in @('reference', 'candidate')) {
    $binary = $evidence.$binaryLabel
    Assert-PropertySet -Object $binary -Expected @(
        'label', 'sha256', 'machine', 'exportCount', 'exportSetSha256'
    ) -Label "Bridge build evidence $binaryLabel"
    if ([string]::IsNullOrWhiteSpace([string]$binary.label)) {
        throw "Bridge build evidence $binaryLabel label is empty"
    }
    Assert-ExactType -Value $binary.exportCount -ExpectedType ([Int64]) `
        -Label "Bridge build evidence $binaryLabel exportCount"
    if ($binary.machine -cne 'I386' -or $binary.exportCount -ne 11) {
        throw "Bridge build evidence $binaryLabel PE/export metadata differs"
    }
    if ($binary.exportSetSha256 -cne $expectedExportsSha256) {
        throw "Bridge build evidence $binaryLabel export-set hash differs"
    }
}
if ($evidence.reference.label -cne 'audited-bridge-baseline' -or
    $evidence.candidate.label -cne 'monorepo-bridge-candidate') {
    throw 'Bridge build evidence labels differ'
}
if ($evidence.reference.sha256 -cne $expectedReferenceSha256) {
    throw 'Bridge build evidence reference hash differs'
}
Assert-ExactType -Value $evidence.exportsEqual -ExpectedType ([Boolean]) `
    -Label 'Bridge build evidence exportsEqual'
Assert-ExactType -Value $evidence.binaryHashExpectedToDiffer -ExpectedType ([Boolean]) `
    -Label 'Bridge build evidence binaryHashExpectedToDiffer'
if ($evidence.exportsEqual -ne $true -or
    $evidence.binaryHashExpectedToDiffer -ne $true) {
    throw 'Bridge build evidence comparison flags differ'
}
foreach ($versionProperty in @('msbuildVersion', 'toolsetVersion', 'compilerVersion')) {
    if ([string]::IsNullOrWhiteSpace([string]$evidence.$versionProperty)) {
        throw "Bridge build evidence $versionProperty is empty"
    }
}
if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
    throw 'Current Bridge candidate DLL is missing'
}
$candidateHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $candidatePath).Hash
if ($evidence.candidate.sha256 -cne $candidateHash) {
    throw 'Bridge build evidence candidate hash differs from the current DLL'
}

Write-Output 'PASS Bridge PE/export build evidence'
