param(
    [string]$ExportsPath,
    [string]$EvidencePath,
    [string]$Candidate
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$providedArguments = @(
    $PSBoundParameters.ContainsKey('ExportsPath'),
    $PSBoundParameters.ContainsKey('EvidencePath'),
    $PSBoundParameters.ContainsKey('Candidate')
)
$providedArgumentCount = @($providedArguments | Where-Object { $_ }).Count
if ($providedArgumentCount -ne 3) {
    throw 'Bridge evidence type regression requires -ExportsPath, -EvidencePath, and -Candidate together'
}
if ([string]::IsNullOrWhiteSpace($ExportsPath) -or
    [string]::IsNullOrWhiteSpace($EvidencePath) -or
    [string]::IsNullOrWhiteSpace($Candidate)) {
    throw 'Bridge evidence type regression arguments must be non-empty'
}

$root = Split-Path -Parent $PSScriptRoot
$evidenceTest = Join-Path $PSScriptRoot 'bridge-build-evidence-test.ps1'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Resolve-InputFile {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Label
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "$Label is missing or is not a file: $fullPath"
    }
    $item = Get-Item -LiteralPath $fullPath -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "$Label must not be a reparse point: $fullPath"
    }
    return $fullPath
}

function Test-BytesEqual {
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

function Assert-SafeFixtureDirectory {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Parent
    )

    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd('\')
    if (-not [IO.Path]::GetDirectoryName($fullPath).Equals(
            $fullParent,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Fixture directory escaped its parent: $fullPath"
    }
    if ([IO.Path]::GetFileName($fullPath) -notmatch '^bridge-evidence-types-[0-9]+-[0-9a-f]{32}$') {
        throw "Fixture directory name is not allowlisted: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if (-not $item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Fixture path must be a non-reparse directory: $fullPath"
        }
    }
    return $fullPath
}

function Invoke-EvidenceTest {
    param([Parameter(Mandatory)] [string]$FixtureEvidencePath)

    $output = @(& pwsh -NoLogo -NoProfile -File $evidenceTest `
        -ExportsPath $resolvedExportsPath `
        -EvidencePath $FixtureEvidencePath `
        -Candidate $resolvedCandidate 2>&1)
    $exitCode = $LASTEXITCODE
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output -join [Environment]::NewLine
    }
}

function Write-EvidenceMutation {
    param(
        [Parameter(Mandatory)] [string]$FixtureEvidencePath,
        [Parameter(Mandatory)] [byte[]]$OriginalEvidenceBytes,
        [Parameter(Mandatory)] [scriptblock]$Mutation
    )

    [IO.File]::WriteAllBytes($FixtureEvidencePath, $OriginalEvidenceBytes)
    $evidence = $utf8NoBom.GetString($OriginalEvidenceBytes) | ConvertFrom-Json
    & $Mutation $evidence
    $text = (($evidence | ConvertTo-Json -Depth 4) -replace "`r`n", "`n").TrimEnd("`r", "`n") + "`n"
    [IO.File]::WriteAllText($FixtureEvidencePath, $text, $utf8NoBom)
}

$resolvedExportsPath = Resolve-InputFile -Path $ExportsPath -Label 'Bridge export evidence'
$resolvedEvidencePath = Resolve-InputFile -Path $EvidencePath -Label 'Bridge build evidence'
$resolvedCandidate = Resolve-InputFile -Path $Candidate -Label 'Bridge candidate'
$resolvedInputs = @($resolvedExportsPath, $resolvedEvidencePath, $resolvedCandidate)
if (@($resolvedInputs | Sort-Object -Unique).Count -ne 3) {
    throw 'Bridge evidence type regression inputs must be three distinct files'
}

$originalInputBytes = [ordered]@{
    $resolvedExportsPath = [IO.File]::ReadAllBytes($resolvedExportsPath)
    $resolvedEvidencePath = [IO.File]::ReadAllBytes($resolvedEvidencePath)
    $resolvedCandidate = [IO.File]::ReadAllBytes($resolvedCandidate)
}

$fixtureParent = Join-Path $root 'out/test-fixtures'
foreach ($directory in @((Join-Path $root 'out'), $fixtureParent)) {
    if (Test-Path -LiteralPath $directory) {
        $item = Get-Item -LiteralPath $directory -Force
        if (-not $item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Fixture parent must be a non-reparse directory: $directory"
        }
    } else {
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
}

$fixtureDirectory = Join-Path $fixtureParent "bridge-evidence-types-$PID-$([Guid]::NewGuid().ToString('N'))"
$fixtureDirectory = Assert-SafeFixtureDirectory -Path $fixtureDirectory -Parent $fixtureParent
[IO.Directory]::CreateDirectory($fixtureDirectory) | Out-Null
$fixtureDirectory = Assert-SafeFixtureDirectory -Path $fixtureDirectory -Parent $fixtureParent
$fixtureEvidencePath = Join-Path $fixtureDirectory 'bridge-build-evidence.json'

$cases = @(
    [pscustomobject]@{
        Name = 'schemaVersion string'
        Expected = 'Bridge build evidence schemaVersion type differs: expected System.Int64, found System.String'
        Mutation = { param($Evidence) $Evidence.schemaVersion = '1' }
    },
    [pscustomobject]@{
        Name = 'reference exportCount string'
        Expected = 'Bridge build evidence reference exportCount type differs: expected System.Int64, found System.String'
        Mutation = { param($Evidence) $Evidence.reference.exportCount = '11' }
    },
    [pscustomobject]@{
        Name = 'candidate exportCount string'
        Expected = 'Bridge build evidence candidate exportCount type differs: expected System.Int64, found System.String'
        Mutation = { param($Evidence) $Evidence.candidate.exportCount = '11' }
    },
    [pscustomobject]@{
        Name = 'exportsEqual string'
        Expected = 'Bridge build evidence exportsEqual type differs: expected System.Boolean, found System.String'
        Mutation = { param($Evidence) $Evidence.exportsEqual = 'true' }
    },
    [pscustomobject]@{
        Name = 'binaryHashExpectedToDiffer string'
        Expected = 'Bridge build evidence binaryHashExpectedToDiffer type differs: expected System.Boolean, found System.String'
        Mutation = { param($Evidence) $Evidence.binaryHashExpectedToDiffer = 'true' }
    }
)

$testFailure = $null
$cleanupFailures = [Collections.Generic.List[string]]::new()
try {
    foreach ($case in $cases) {
        Write-EvidenceMutation -FixtureEvidencePath $fixtureEvidencePath `
            -OriginalEvidenceBytes $originalInputBytes[$resolvedEvidencePath] `
            -Mutation $case.Mutation
        $result = Invoke-EvidenceTest -FixtureEvidencePath $fixtureEvidencePath
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch [regex]::Escape($case.Expected)) {
            throw "$($case.Name) was not rejected by exact runtime type: $($result.Output)"
        }
    }
    $validResult = Invoke-EvidenceTest -FixtureEvidencePath $resolvedEvidencePath
    if ($validResult.ExitCode -ne 0 -or
        $validResult.Output -notmatch 'PASS Bridge PE/export build evidence') {
        throw "Valid typed Bridge evidence failed in current mode: $($validResult.Output)"
    }
} catch {
    $testFailure = $_
} finally {
    try {
        $fixtureDirectory = Assert-SafeFixtureDirectory -Path $fixtureDirectory -Parent $fixtureParent
        if (Test-Path -LiteralPath $fixtureDirectory) {
            Remove-Item -LiteralPath $fixtureDirectory -Recurse -Force
        }
    } catch {
        [void]$cleanupFailures.Add("Fixture cleanup failed: $($_.Exception.Message)")
    }

    foreach ($entry in $originalInputBytes.GetEnumerator()) {
        try {
            $currentBytes = [IO.File]::ReadAllBytes($entry.Key)
            if (-not (Test-BytesEqual -Left $entry.Value -Right $currentBytes)) {
                [void]$cleanupFailures.Add("Supplied input bytes changed: $($entry.Key)")
            }
        } catch {
            [void]$cleanupFailures.Add("Supplied input verification failed for $($entry.Key): $($_.Exception.Message)")
        }
    }
}

if ($cleanupFailures.Count -gt 0) {
    throw ($cleanupFailures -join '; ')
}
if ($null -ne $testFailure) {
    throw $testFailure
}

Write-Output 'PASS Bridge build evidence current mode rejects string-typed schema, counts, and flags without modifying supplied inputs'
