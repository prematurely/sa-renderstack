param(
    [switch]$Help,
    [ValidateSet('Release')] [string]$Configuration = 'Release',
    [ValidateSet('x86')] [string]$Architecture = 'x86',
    [string]$BridgePath,
    [string]$DxvkPath,
    [string]$LlvmMingwBin,
    [string]$Ninja,
    [string]$Glslang
)

if ($Help) {
    @'
Usage: verify-exports.ps1 [-Help] [-Configuration Release] [-Architecture x86]
       [-BridgePath <path>] [-DxvkPath <path>] [-LlvmMingwBin <path>]
       [-Ninja <path>] [-Glslang <path>]

Verifies the PE32/x86 Bridge and merged DXVK export sets and publishes an
atomic report to out/reports/task-6/exports.json.
'@ | Write-Output
    exit 0
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'
. (Join-Path $PSScriptRoot 'lib/process-runner.ps1')
. (Join-Path $PSScriptRoot 'lib/toolchain-discovery.ps1')

$root = Get-RenderStackRepositoryRoot -CallerScriptPath $PSCommandPath
$expectedBridge = Get-Content -LiteralPath (Join-Path $root 'tests/expected/bridge-exports.txt')
$expectedDxvk = Get-Content -LiteralPath (Join-Path $root 'tests/expected/dxvk-merged-exports.txt')
$expectedSets = [ordered]@{ Bridge = @($expectedBridge); Dxvk = @($expectedDxvk) }

if ([string]::IsNullOrWhiteSpace($BridgePath)) {
    $BridgePath = Join-Path $root 'out/build/bridge/d3d9.dll'
}
if ([string]::IsNullOrWhiteSpace($DxvkPath)) {
    $DxvkPath = Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll'
}

function Get-BytesSha256 {
    param([Parameter(Mandatory)] [byte[]]$Bytes)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
}

function Get-TextSha256 {
    param([Parameter(Mandatory)] [string[]]$Rows)
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Rows -join "`n") + "`n")
    return Get-BytesSha256 -Bytes $bytes
}

function Read-PeArchitecture {
    param([Parameter(Mandatory)] [string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "PE file has no valid DOS header: $Path"
    }
    $offset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($offset -lt 0 -or $offset + 26 -gt $bytes.Length -or
        $bytes[$offset] -ne 0x50 -or $bytes[$offset + 1] -ne 0x45 -or
        $bytes[$offset + 2] -ne 0 -or $bytes[$offset + 3] -ne 0) {
        throw "PE file has no valid NT header: $Path"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $offset + 4)
    $magic = [BitConverter]::ToUInt16($bytes, $offset + 24)
    if ($machine -ne 0x14C -or $magic -ne 0x10B) {
        throw "Expected PE32/I386, found machine 0x$('{0:X4}' -f $machine), magic 0x$('{0:X4}' -f $magic): $Path"
    }
    return [ordered]@{ machine = 'I386'; machineValue = '0x014C'; format = 'PE32'; magic = '0x010B' }
}

function Parse-ExportRows {
    param([Parameter(Mandatory)] [string]$Text, [Parameter(Mandatory)] [string]$Label)
    $records = [regex]::Matches($Text, '(?ms)Export\s*\{(?<body>.*?)\}')
    if ($records.Count -eq 0) { throw "$Label returned no complete Export records" }
    $rows = [Collections.Generic.List[string]]::new()
    $ordinals = [Collections.Generic.HashSet[int]]::new()
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($record in $records) {
        $body = $record.Groups['body'].Value
        $ordinalMatches = [regex]::Matches($body, '(?m)^[ \t]*Ordinal:[ \t]*(\d+)[ \t]*$')
        $nameMatches = [regex]::Matches($body, '(?m)^[ \t]*Name:[ \t]*(.*?)[ \t]*$')
        $rvaMatches = [regex]::Matches($body, '(?m)^[ \t]*RVA:[ \t]*(0x[0-9A-Fa-f]+)[ \t]*$')
        if ($ordinalMatches.Count -ne 1 -or $nameMatches.Count -ne 1 -or $rvaMatches.Count -ne 1) {
            throw "$Label contains a malformed Export record"
        }
        $ordinal = [int]$ordinalMatches[0].Groups[1].Value
        if (-not $ordinals.Add($ordinal)) { throw "$Label contains duplicate ordinal $ordinal" }
        $name = $nameMatches[0].Groups[1].Value.Trim()
        if ($name -eq '<NONAME>') { $name = '' }
        if (-not [string]::IsNullOrEmpty($name) -and -not $names.Add($name)) {
            throw "$Label contains duplicate export name $name"
        }
        $rva = [Convert]::ToUInt32($rvaMatches[0].Groups[1].Value.Substring(2), 16)
        if ($rva -ne 0) { [void]$rows.Add(('{0}{1}' -f $ordinal, $(if ($name) { " $name" } else { '' }))) }
    }
    return @($rows | Sort-Object { [int]($_ -split ' ', 2)[0] })
}

function Verify-ExportFile {
    param([Parameter(Mandatory)] [string]$Kind, [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$ReadObj)
    $fullPath = Get-RenderStackFullPath -Path $Path
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) { throw "$Kind DLL is missing: $fullPath" }
    $architecture = Read-PeArchitecture -Path $fullPath
    $result = Invoke-RenderStackProcess -FilePath $ReadObj -ArgumentList @('--coff-exports', $fullPath) `
        -WorkingDirectory $root -Label "readobj-$Kind"
    if ($result.ExitCode -ne 0) { throw "llvm-readobj failed for ${Kind}: $($result.StandardError)" }
    $rows = Parse-ExportRows -Text $result.StandardOutput -Label $Kind
    $expected = $expectedSets[$Kind]
    if (@(Compare-Object $expected $rows).Count -ne 0) {
        throw "$Kind export set differs. Expected $($expected.Count) rows, found $($rows.Count)"
    }
    [pscustomobject]@{
        kind = $Kind
        path = [IO.Path]::GetRelativePath($root, $fullPath).Replace('\', '/')
        size = [int64](Get-Item -LiteralPath $fullPath).Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $fullPath).Hash.ToUpperInvariant()
        architecture = $architecture
        exportCount = [int64]$rows.Count
        exportSetSha256 = Get-TextSha256 -Rows $rows
        exports = @($rows)
    }
}

$toolchain = Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin `
    -NinjaPath $Ninja -GlslangPath $Glslang
$report = [ordered]@{
    schemaVersion = 1
    repositoryCommit = ((Invoke-RenderStackProcess -FilePath (Get-Command git.exe).Source -ArgumentList @('rev-parse', 'HEAD') `
        -WorkingDirectory $root -Label 'git-rev-parse').StandardOutput.Trim())
    configuration = $Configuration
    architecture = $Architecture
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    llvmReadobj = [IO.Path]::GetRelativePath($root, $toolchain.LlvmMingw.ReadObjPath).Replace('\', '/')
    files = @(
        (Verify-ExportFile -Kind Bridge -Path $BridgePath -ReadObj $toolchain.LlvmMingw.ReadObjPath)
        (Verify-ExportFile -Kind Dxvk -Path $DxvkPath -ReadObj $toolchain.LlvmMingw.ReadObjPath)
    )
}
Write-RenderStackAtomicJson -Path (Join-Path $root 'out/reports/task-6/exports.json') -Value $report
Write-Output 'PASS Bridge and DXVK export verification'
exit 0
