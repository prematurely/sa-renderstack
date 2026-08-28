param(
    [Parameter(Mandatory)] [string]$Reference,
    [Parameter(Mandatory)] [string]$Candidate,
    [string]$ExportsOut,
    [string]$EvidenceOut
)
$ErrorActionPreference = 'Stop'
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($ExportsOut)) {
    $ExportsOut = Join-Path $root 'migration/bridge-exports.txt'
}
if ([string]::IsNullOrWhiteSpace($EvidenceOut)) {
    $EvidenceOut = Join-Path $root 'migration/bridge-build-evidence.json'
}

$expectedReferenceSha256 = 'B0BBE0C98B132EA0B8E6BA0FA2C978D82C48063A50C2324BE3A8F7141FE7E9FF'
$expectedExportsSha256 = '93B2438F39A97F664BE6B3F2791C36981358B18647FE463BEEC94E0F66DA76B0'
$expectedExports = @(
    [pscustomobject]@{ Ordinal = 1; Name = 'Direct3DCreate9' }
    [pscustomobject]@{ Ordinal = 2; Name = 'DebugSetLevel' }
    [pscustomobject]@{ Ordinal = 3; Name = 'DebugSetMute' }
    [pscustomobject]@{ Ordinal = 4; Name = 'Direct3DShaderValidatorCreate9' }
    [pscustomobject]@{ Ordinal = 5; Name = 'PSGPError' }
    [pscustomobject]@{ Ordinal = 6; Name = 'PSGPSampleTexture' }
    [pscustomobject]@{ Ordinal = 7; Name = 'CreateDXGIFactory1' }
    [pscustomobject]@{ Ordinal = 8; Name = 'CreateDXGIFactory2' }
    [pscustomobject]@{ Ordinal = 9; Name = 'CreateDXGIFactory' }
    [pscustomobject]@{ Ordinal = 10; Name = 'DXGIDeclareAdapterRemovalSupport' }
    [pscustomobject]@{ Ordinal = 11; Name = 'DXGIGetDebugInterface1' }
)
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Get-FileSha256 {
    param([Parameter(Mandatory)] [string]$Path)

    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToUpperInvariant()
}

function Get-BytesSha256 {
    param([Parameter(Mandatory)] [byte[]]$Bytes)

    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
}

function Get-PeMetadata {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Label does not have a valid DOS header"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset -gt ($bytes.Length - 26)) {
        throw "$Label has an invalid PE header offset"
    }
    if ($bytes[$peOffset] -ne 0x50 -or
        $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or
        $bytes[$peOffset + 3] -ne 0) {
        throw "$Label does not have a valid PE signature"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    if ($machine -ne 0x14C) {
        throw ("$Label machine differs: expected I386/0x14C, got 0x{0:X}" -f $machine)
    }
    if ($optionalMagic -ne 0x10B) {
        throw ("$Label optional-header magic differs: expected PE32/0x10B, got 0x{0:X}" -f $optionalMagic)
    }

    return [pscustomobject]@{
        Sha256 = Get-FileSha256 -Path $Path
        Machine = 'I386'
    }
}

function Get-Toolchain {
    $vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
    $msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
        throw "MSBuild is missing: $msbuild"
    }

    $dumpbinCandidates = @(Get-ChildItem -LiteralPath $vsRoot -Filter dumpbin.exe -Recurse -File |
        Where-Object { $_.FullName -match '\\bin\\Hostx64\\x86\\dumpbin\.exe$' } |
        ForEach-Object {
            $toolsetVersion = $_.Directory.Parent.Parent.Parent.Name
            $parsedVersion = $null
            if (-not [version]::TryParse($toolsetVersion, [ref]$parsedVersion)) {
                throw "Cannot parse MSVC toolset version from $($_.FullName)"
            }
            [pscustomobject]@{
                Path = $_.FullName
                ToolsetVersion = $toolsetVersion
                ParsedVersion = $parsedVersion
            }
        } | Sort-Object ParsedVersion -Descending)
    if ($dumpbinCandidates.Count -eq 0) {
        throw "No Hostx64\\x86 dumpbin.exe found under $vsRoot"
    }
    $dumpbin = $dumpbinCandidates[0]
    $compiler = Join-Path (Split-Path -Parent $dumpbin.Path) 'cl.exe'
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
        throw "Compiler paired with dumpbin is missing: $compiler"
    }
    $compilerVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($compiler).FileVersion
    if ([string]::IsNullOrWhiteSpace($compilerVersion)) {
        throw "Compiler version is empty: $compiler"
    }

    $msbuildOutput = @(& $msbuild -version -nologo 2>&1)
    $msbuildExitCode = $LASTEXITCODE
    if ($msbuildExitCode -ne 0) {
        throw "MSBuild version query failed with exit $msbuildExitCode`: $($msbuildOutput -join [Environment]::NewLine)"
    }
    $msbuildVersion = [string]($msbuildOutput |
        Where-Object { [string]$_ -match '^\d+(?:\.\d+)+$' } |
        Select-Object -Last 1)
    if ([string]::IsNullOrWhiteSpace($msbuildVersion)) {
        throw "MSBuild version could not be parsed: $($msbuildOutput -join [Environment]::NewLine)"
    }

    return [pscustomobject]@{
        Dumpbin = $dumpbin.Path
        MsbuildVersion = $msbuildVersion
        ToolsetVersion = $dumpbin.ToolsetVersion
        CompilerVersion = $compilerVersion
    }
}

function Get-Exports {
    param(
        [Parameter(Mandatory)] [string]$Dumpbin,
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Label
    )

    $dumpbinOutput = @(& $Dumpbin /nologo /exports $Path 2>&1)
    $dumpbinExitCode = $LASTEXITCODE
    if ($dumpbinExitCode -ne 0) {
        throw "dumpbin failed for $Label with exit $dumpbinExitCode`: $($dumpbinOutput -join [Environment]::NewLine)"
    }

    $functionCounts = @($dumpbinOutput | ForEach-Object {
        if ([string]$_ -match '^\s*(\d+)\s+number of functions\s*$') {
            [int]$Matches[1]
        }
    })
    $nameCounts = @($dumpbinOutput | ForEach-Object {
        if ([string]$_ -match '^\s*(\d+)\s+number of names\s*$') {
            [int]$Matches[1]
        }
    })
    if ($functionCounts.Count -ne 1 -or $nameCounts.Count -ne 1) {
        throw "$Label dumpbin output did not report exactly one function count and one name count"
    }

    $exports = @($dumpbinOutput | ForEach-Object {
        if ([string]$_ -match '^\s*(\d+)\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([^\s=]+)(?:\s+=\s+_\S+)?\s*$') {
            [pscustomobject]@{
                Ordinal = [int]$Matches[1]
                Name = $Matches[2]
            }
        }
    })
    if ($exports.Count -ne $functionCounts[0] -or $exports.Count -ne $nameCounts[0]) {
        throw "$Label parsed export count differs from dumpbin counts"
    }
    $duplicateOrdinals = @($exports | Group-Object Ordinal | Where-Object Count -gt 1)
    $duplicateNames = @($exports | Group-Object Name -CaseSensitive | Where-Object Count -gt 1)
    if ($duplicateOrdinals.Count -ne 0) {
        throw "$Label contains duplicate export ordinals: $($duplicateOrdinals.Name -join ', ')"
    }
    if ($duplicateNames.Count -ne 0) {
        throw "$Label contains duplicate export names: $($duplicateNames.Name -join ', ')"
    }

    $sorted = @($exports | Sort-Object Ordinal, Name)
    $actualRows = ($sorted | ForEach-Object { "$($_.Ordinal) $($_.Name)" }) -join "`n"
    $expectedRows = ($expectedExports | ForEach-Object { "$($_.Ordinal) $($_.Name)" }) -join "`n"
    if ($actualRows -cne $expectedRows) {
        throw "$Label export rows differ from the exact baseline"
    }
    return $sorted
}

function Write-CanonicalText {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Text
    )

    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $canonical = ($Text -replace "`r`n", "`n").TrimEnd("`r", "`n") + "`n"
    [IO.File]::WriteAllText($Path, $canonical, $utf8NoBom)
}

$toolchain = Get-Toolchain
$referenceMetadata = Get-PeMetadata -Path $Reference -Label 'Reference Bridge DLL'
if ($referenceMetadata.Sha256 -cne $expectedReferenceSha256) {
    throw "Reference Bridge SHA-256 differs: $($referenceMetadata.Sha256)"
}
$candidateMetadata = Get-PeMetadata -Path $Candidate -Label 'Candidate Bridge DLL'
if ($candidateMetadata.Sha256 -ceq $referenceMetadata.Sha256) {
    throw 'Candidate Bridge hash unexpectedly equals the audited reference hash'
}

$referenceExports = @(Get-Exports -Dumpbin $toolchain.Dumpbin -Path $Reference -Label 'Reference Bridge DLL')
$candidateExports = @(Get-Exports -Dumpbin $toolchain.Dumpbin -Path $Candidate -Label 'Candidate Bridge DLL')
$referenceRows = ($referenceExports | ForEach-Object { "$($_.Ordinal) $($_.Name)" }) -join "`n"
$candidateRows = ($candidateExports | ForEach-Object { "$($_.Ordinal) $($_.Name)" }) -join "`n"
if ($referenceRows -cne $candidateRows) {
    throw 'Reference and candidate Bridge export rows differ'
}

$exportsText = $candidateRows + "`n"
$exportsBytes = $utf8NoBom.GetBytes($exportsText)
$exportsSha256 = Get-BytesSha256 -Bytes $exportsBytes
if ($exportsSha256 -cne $expectedExportsSha256) {
    throw "Canonical Bridge export hash differs: $exportsSha256"
}
Write-CanonicalText -Path $ExportsOut -Text $exportsText

$evidence = [ordered]@{
    schemaVersion = 1
    reference = [ordered]@{
        label = 'audited-bridge-baseline'
        sha256 = $referenceMetadata.Sha256
        machine = $referenceMetadata.Machine
        exportCount = $referenceExports.Count
        exportSetSha256 = $exportsSha256
    }
    candidate = [ordered]@{
        label = 'monorepo-bridge-candidate'
        sha256 = $candidateMetadata.Sha256
        machine = $candidateMetadata.Machine
        exportCount = $candidateExports.Count
        exportSetSha256 = $exportsSha256
    }
    exportsEqual = $true
    msbuildVersion = $toolchain.MsbuildVersion
    toolsetVersion = $toolchain.ToolsetVersion
    compilerVersion = $toolchain.CompilerVersion
    binaryHashExpectedToDiffer = $true
}
$evidenceJson = $evidence | ConvertTo-Json -Depth 4
Write-CanonicalText -Path $EvidenceOut -Text $evidenceJson

Write-Output 'PASS Bridge baseline ABI: I386 PE32, 11 exact exports'
