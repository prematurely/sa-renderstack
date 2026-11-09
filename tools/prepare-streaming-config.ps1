param([switch]$Help, [string]$Source, [string]$OutputDirectory)

if ($Help) {
    @'
Usage: prepare-streaming-config.ps1 -Source <ImprovedStreaming.ini> -OutputDirectory <new-directory-under-out>

Prepares a full UTF-8 candidate INI and preparation.json under this checkout's
out directory. Renames only Settings.RemoveUnusedIntervalMs to the audited
Improved Streaming v1.1 key RemoveUnusedInterval, preserving its integer value.
Never applies the candidate, overwrites an output, or writes to the source.
'@ | Write-Output
    exit 0
}
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $Source -or -not $OutputDirectory) { throw 'Source and OutputDirectory are required; see -Help' }
. (Join-Path $PSScriptRoot 'lib/process-runner.ps1')
$root = Split-Path -Parent $PSScriptRoot
$sourcePath = [IO.Path]::GetFullPath($Source)
$destination = Assert-RenderStackPathUnder -Base (Join-Path $root 'out') -Path $OutputDirectory `
    -Description 'Streaming candidate output'
Assert-RenderStackNoReparsePath -Path $destination -Anchor $root -Description 'Streaming candidate output' | Out-Null
if ([IO.Directory]::Exists($destination) -or [IO.File]::Exists($destination)) {
    throw 'OutputDirectory must be new; existing output is never overwritten'
}
# Hold a read-only handle which denies concurrent writers/deletion while preparing.
$inputFile = [IO.File]::Open($sourcePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $buffer = [IO.MemoryStream]::new()
    try { $inputFile.CopyTo($buffer); $original = $buffer.ToArray() } finally { $buffer.Dispose() }
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $text = $utf8.GetString($original)
    if ($text.Contains([char]0) -or $text -match '\r(?!\n)') {
        throw 'Only UTF-8 INI files with LF or CRLF line endings are supported'
    }
    $section = ''
    $settingsCount = 0
    # The consumer trims C-locale byte whitespace, not Unicode whitespace.
    $iniWhitespace = [char[]]@(' ', "`t", "`r", "`n", "`v", "`f")
    $entries = [Collections.Generic.List[object]]::new()
    foreach ($lineMatch in [regex]::Matches($text, '[^\n]+|\n')) {
        $line = $lineMatch.Value
        $semantic = $line.TrimStart([char]0xFEFF)
        # Match the pinned consumer's comment syntax, section/key case and trim.
        $comment = $semantic.IndexOf(';', [StringComparison]::Ordinal)
        if ($comment -ge 0) { $semantic = $semantic.Substring(0, $comment) }
        $comment = $semantic.LastIndexOf(' //', [StringComparison]::Ordinal)
        if ($comment -ge 0) { $semantic = $semantic.Substring(0, $comment) }
        $semantic = $semantic.Trim($iniWhitespace)
        if ($semantic -match '^\[(.*)\]$') {
            $section = $Matches[1].Trim($iniWhitespace)
            if ($section -ceq 'Settings') { $settingsCount++ }
            continue
        }
        if ($section -cne 'Settings' -or -not $semantic) { continue }
        $parts = $semantic.Split('=', 2)
        $key = $parts[0].Trim($iniWhitespace)
        if ($key -ieq 'RemoveUnusedIntervalMs' -or $key -ieq 'RemoveUnusedInterval') {
            if ($key -cne 'RemoveUnusedIntervalMs' -and $key -cne 'RemoveUnusedInterval') {
                throw 'Interval key case differs from the audited consumer'
            }
            if ($parts.Count -ne 2) { throw 'Interval must have an integer value' }
            $value = $parts[1].Trim($iniWhitespace)
            if ($value -cmatch '^0[xX][0-9a-fA-F]+$') {
                $number = [Convert]::ToInt64($value.Substring(2), 16)
            } elseif ($value -cmatch '^[0-9]+$') {
                $number = [long]::Parse($value, [Globalization.CultureInfo]::InvariantCulture)
            } else { throw 'Interval must be an unambiguous non-negative integer' }
            if ($number -lt 0 -or $number -gt [int]::MaxValue) { throw 'Interval is outside the consumer int32 range' }
            $entries.Add([pscustomobject]@{key=$key; value=[int]$number;
                offset=$lineMatch.Index + $line.IndexOf($key, [StringComparison]::Ordinal)})
        }
    }
    if ($settingsCount -ne 1 -or $entries.Count -ne 1) {
        throw 'Require one Settings section and exactly one interval key; missing, duplicate and conflicting keys are refused'
    }
    $entry = $entries[0]
    $changed = $entry.key -ceq 'RemoveUnusedIntervalMs'
    $candidateText = $text
    if ($changed) {
        $candidateText = $text.Remove($entry.offset, $entry.key.Length).Insert($entry.offset, 'RemoveUnusedInterval')
    }
    $candidateBytes = $utf8.GetBytes($candidateText)
    $manifest = [ordered]@{
        schemaVersion = 1
        applied = $false
        changed = $changed
        sourcePath = $sourcePath
        sourceSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($original))
        candidateSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($candidateBytes))
        intervalMs = $entry.value
        auditedConsumer = [ordered]@{
            name = 'ImprovedStreaming.SA.asi v1.1'
            sha256 = 'D7BD005EDE20F00B358B75C4D4844618024F2A74AAF60957553EBD964BEE527C'
            readerCallRva = '0x73E9'
            keyRva = '0x47D20'
            key = 'RemoveUnusedInterval'
            defaultMs = 60
        }
        note = 'Prepared only. Consumer identity is audit provenance, not automatic version detection or an FPS claim.'
    }
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    Assert-RenderStackNoReparsePath -Path $destination -Anchor $root -Description 'Streaming candidate output' | Out-Null
    $candidateFile = [IO.File]::Open((Join-Path $destination 'ImprovedStreaming.ini'), [IO.FileMode]::CreateNew)
    try { $candidateFile.Write($candidateBytes, 0, $candidateBytes.Length) } finally { $candidateFile.Dispose() }
    $manifestBytes = $utf8.GetBytes(($manifest | ConvertTo-Json -Depth 5) + "`n")
    $manifestFile = [IO.File]::Open((Join-Path $destination 'preparation.json'), [IO.FileMode]::CreateNew)
    try { $manifestFile.Write($manifestBytes, 0, $manifestBytes.Length) } finally { $manifestFile.Dispose() }
    Write-Output "Prepared candidate: $destination (interval=$($entry.value) ms, changed=$changed, applied=False)"
} finally { $inputFile.Dispose() }
