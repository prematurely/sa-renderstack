$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$hygieneScript = Join-Path $root 'tools/verify-source-tree.ps1'
$fixtureDirectories = @(
    (Join-Path $root 'build'),
    (Join-Path $root 'src/bridge/build')
)

foreach ($fixtureDirectory in $fixtureDirectories) {
    if (Test-Path -LiteralPath $fixtureDirectory) {
        throw "Fixture directory already exists: $fixtureDirectory"
    }
}

function Invoke-HygieneGate {
    $output = @(& pwsh -NoProfile -File $hygieneScript 2>&1)
    [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = $output -join [Environment]::NewLine
    }
}

function Assert-CleanPass($result, $phase) {
    if ($result.ExitCode -ne 0) {
        throw "Expected clean tree to pass during $phase. Output: $($result.Output)"
    }
}

function Assert-FixtureFailure($result, $phase, $expectedPath) {
    if ($result.ExitCode -eq 0) {
        throw "Expected hygiene failure during $phase for $expectedPath"
    }

    if ($result.Output -notmatch [regex]::Escape($expectedPath).Replace('/', '[\\/]')) {
        throw "Expected hygiene output to mention $expectedPath during $phase. Output: $($result.Output)"
    }
}

try {
    Assert-CleanPass (Invoke-HygieneGate) 'initial clean-tree check'

    $rootFixture = Join-Path $root 'build/metadata.txt'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $rootFixture) | Out-Null
    Set-Content -LiteralPath $rootFixture -Value 'fixture'
    Assert-FixtureFailure (Invoke-HygieneGate) 'root build fixture check' 'build/metadata.txt'
    Remove-Item -LiteralPath (Join-Path $root 'build') -Recurse -Force

    $nestedFixture = Join-Path $root 'src/bridge/build/metadata.txt'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $nestedFixture) | Out-Null
    Set-Content -LiteralPath $nestedFixture -Value 'fixture'
    Assert-FixtureFailure (Invoke-HygieneGate) 'nested build fixture check' 'src/bridge/build/metadata.txt'
}
finally {
    foreach ($fixtureDirectory in $fixtureDirectories) {
        Remove-Item -LiteralPath $fixtureDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Assert-CleanPass (Invoke-HygieneGate) 'final clean-tree check'
Write-Output 'PASS source hygiene regression'
