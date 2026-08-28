$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$evidencePath = Join-Path $root 'migration/bridge-build-evidence.json'
$evidenceTest = Join-Path $PSScriptRoot 'bridge-build-evidence-test.ps1'
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$originalBytes = [IO.File]::ReadAllBytes($evidencePath)

function Invoke-EvidenceTest {
    $output = @(& pwsh -NoLogo -NoProfile -File $evidenceTest 2>&1)
    $exitCode = $LASTEXITCODE
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output -join [Environment]::NewLine
    }
}

function Write-EvidenceMutation {
    param([Parameter(Mandatory)] [scriptblock]$Mutation)

    $evidence = $utf8NoBom.GetString($originalBytes) | ConvertFrom-Json
    & $Mutation $evidence
    $text = (($evidence | ConvertTo-Json -Depth 4) -replace "`r`n", "`n").TrimEnd("`r", "`n") + "`n"
    [IO.File]::WriteAllText($evidencePath, $text, $utf8NoBom)
}

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

try {
    foreach ($case in $cases) {
        Write-EvidenceMutation -Mutation $case.Mutation
        $result = Invoke-EvidenceTest
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch [regex]::Escape($case.Expected)) {
            throw "$($case.Name) was not rejected by exact runtime type: $($result.Output)"
        }
    }
} finally {
    [IO.File]::WriteAllBytes($evidencePath, $originalBytes)
}

$restoredBytes = [IO.File]::ReadAllBytes($evidencePath)
if (-not [Linq.Enumerable]::SequenceEqual([byte[]]$originalBytes, [byte[]]$restoredBytes)) {
    throw 'Tracked Bridge evidence bytes were not restored after type regression'
}
$validResult = Invoke-EvidenceTest
if ($validResult.ExitCode -ne 0 -or
    $validResult.Output -notmatch 'PASS Bridge PE/export build evidence') {
    throw "Valid typed Bridge evidence failed after type regression: $($validResult.Output)"
}

Write-Output 'PASS Bridge build evidence rejects string-typed schema, counts, and flags'
