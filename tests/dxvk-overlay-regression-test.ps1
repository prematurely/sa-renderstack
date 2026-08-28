$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$auditedSource = 'D:\GTA San Andreas\.codex-src\dxvk\dxvk-3.0.1-bridge'
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$failures = [Collections.Generic.List[string]]::new()

function Get-Sha256 {
    param([Parameter(Mandatory)] [byte[]] $Bytes)

    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
}

function Get-NormalizedSourceBytes {
    param([Parameter(Mandatory)] [string] $Path)

    $text = $strictUtf8.GetString([IO.File]::ReadAllBytes($Path))
    if ($text.Length -and $text[0] -eq [char]0xFEFF) {
        $text = $text.Substring(1)
    }
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    return $utf8NoBom.GetBytes($text)
}

function Write-Utf8File {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Text
    )

    [IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function New-OverlayFixture {
    $fixture = Join-Path $tempBase ('sa-renderstack-task3-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'tests') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'tools/migration') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'migration') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'backend/dxvk') | Out-Null

    Copy-Item -LiteralPath (Join-Path $root 'tests/dxvk-overlay-manifest-test.ps1') `
        -Destination (Join-Path $fixture 'tests/dxvk-overlay-manifest-test.ps1')
    Copy-Item -LiteralPath (Join-Path $root 'tools/migration/import-audited-dxvk.ps1') `
        -Destination (Join-Path $fixture 'tools/migration/import-audited-dxvk.ps1')
    Copy-Item -LiteralPath (Join-Path $root 'tools/migration/dxvk-overlay-files.txt') `
        -Destination (Join-Path $fixture 'tools/migration/dxvk-overlay-files.txt')
    Copy-Item -LiteralPath (Join-Path $root 'migration/dxvk-overlay-manifest.json') `
        -Destination (Join-Path $fixture 'migration/dxvk-overlay-manifest.json')
    Copy-Item -LiteralPath (Join-Path $root 'migration/dxvk-baseline-evidence.json') `
        -Destination (Join-Path $fixture 'migration/dxvk-baseline-evidence.json')

    $manifest = Get-Content -LiteralPath (Join-Path $root 'migration/dxvk-overlay-manifest.json') -Raw |
        ConvertFrom-Json
    foreach ($entry in $manifest.files) {
        $from = Join-Path $root (Join-Path 'backend/dxvk' $entry.path)
        $to = Join-Path $fixture (Join-Path 'backend/dxvk' $entry.path)
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
        Copy-Item -LiteralPath $from -Destination $to
    }

    return $fixture
}

function Remove-OverlayFixture {
    param([Parameter(Mandatory)] [string] $Fixture)

    $resolved = [IO.Path]::GetFullPath($Fixture)
    if (-not $resolved.StartsWith(
            (Join-Path $tempBase 'sa-renderstack-task3-'),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove unexpected fixture path: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

function Invoke-FixtureScript {
    param(
        [Parameter(Mandatory)] [string] $Script,
        [string[]] $Arguments = @()
    )

    $output = @(& pwsh -NoProfile -File $Script @Arguments 2>&1)
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = ($output | Out-String).Trim()
    }
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [scriptblock] $Arrange,
        [Parameter(Mandatory)] [string] $ExpectedPattern,
        [switch] $Live
    )

    $fixture = New-OverlayFixture
    try {
        & $Arrange $fixture
        $arguments = if ($Live) { @('-Source', $auditedSource) } else { @() }
        $result = Invoke-FixtureScript `
            -Script (Join-Path $fixture 'tests/dxvk-overlay-manifest-test.ps1') `
            -Arguments $arguments
        if ($result.ExitCode -eq 0) {
            $failures.Add("$Name was accepted")
        } elseif ($result.Output -notmatch $ExpectedPattern) {
            $failures.Add("$Name failed for the wrong reason: $($result.Output)")
        }
    } finally {
        Remove-OverlayFixture -Fixture $fixture
    }
}

function Set-CoordinatedAllowlistSubstitution {
    param([Parameter(Mandatory)] [string] $Fixture)

    $allowlistPath = Join-Path $Fixture 'tools/migration/dxvk-overlay-files.txt'
    $allowlist = @(Get-Content -LiteralPath $allowlistPath)
    $allowlist[0] = 'README.md'
    Write-Utf8File -Path $allowlistPath -Text (($allowlist -join "`n") + "`n")

    $sourcePath = Join-Path $auditedSource 'README.md'
    $destinationPath = Join-Path $Fixture 'backend/dxvk/README.md'
    $destinationBytes = Get-NormalizedSourceBytes -Path $sourcePath
    [IO.File]::WriteAllBytes($destinationPath, $destinationBytes)

    $manifestPath = Join-Path $Fixture 'migration/dxvk-overlay-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.files[0].path = 'README.md'
    $manifest.files[0].sourceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash
    $manifest.files[0].sha256 = Get-Sha256 -Bytes $destinationBytes
    Write-Utf8File -Path $manifestPath `
        -Text (($manifest | ConvertTo-Json -Depth 10) + "`n")
}

function Set-CoordinatedDestinationTampering {
    param([Parameter(Mandatory)] [string] $Fixture)

    $destinationPath = Join-Path $Fixture 'backend/dxvk/meson_options.txt'
    $tamperedBytes = $utf8NoBom.GetBytes(
        ([IO.File]::ReadAllText($destinationPath, $strictUtf8) + "tampered`n"))
    [IO.File]::WriteAllBytes($destinationPath, $tamperedBytes)

    $manifestPath = Join-Path $Fixture 'migration/dxvk-overlay-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.files[0].sha256 = Get-Sha256 -Bytes $tamperedBytes
    Write-Utf8File -Path $manifestPath `
        -Text (($manifest | ConvertTo-Json -Depth 10) + "`n")
}

function Add-DuplicateProperty {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Property,
        [Parameter(Mandatory)] [string] $Value
    )

    $json = [IO.File]::ReadAllText($Path, $strictUtf8)
    $escapedValue = $Value.Replace('\', '\\').Replace('"', '\"')
    $insertion = "{`n  `"$Property`": `"$escapedValue`","
    Write-Utf8File -Path $Path -Text ($insertion + $json.Substring(1))
}

Assert-Rejected -Name 'coordinated allowlist substitution' `
    -Arrange ${function:Set-CoordinatedAllowlistSubstitution} `
    -ExpectedPattern 'allowlist (hash|entries).*differ'

Assert-Rejected -Name 'offline coordinated destination tampering' `
    -Arrange ${function:Set-CoordinatedDestinationTampering} `
    -ExpectedPattern 'canonical hash differs'

Assert-Rejected -Name 'live coordinated destination tampering' `
    -Arrange ${function:Set-CoordinatedDestinationTampering} `
    -ExpectedPattern 'canonical hash differs|normalized audited source differs' `
    -Live

Assert-Rejected -Name 'duplicate overlay-manifest property' -Arrange {
    param($fixture)
    Add-DuplicateProperty `
        -Path (Join-Path $fixture 'migration/dxvk-overlay-manifest.json') `
        -Property 'sourceLabel' `
        -Value 'C:\hidden-root'
} -ExpectedPattern "Duplicate JSON property 'sourceLabel'"

Assert-Rejected -Name 'duplicate baseline-evidence property' -Arrange {
    param($fixture)
    Add-DuplicateProperty `
        -Path (Join-Path $fixture 'migration/dxvk-baseline-evidence.json') `
        -Property 'sourceReportSha256' `
        -Value 'C:\hidden-root'
} -ExpectedPattern "Duplicate JSON property 'sourceReportSha256'"

foreach ($forbiddenPath in @('artifact.obj', 'artifact.o', 'nested/.wraplock')) {
    Assert-Rejected -Name "verifier forbidden path $forbiddenPath" -Arrange {
        param($fixture)
        $allowlistPath = Join-Path $fixture 'tools/migration/dxvk-overlay-files.txt'
        $allowlist = @(Get-Content -LiteralPath $allowlistPath)
        $allowlist[0] = $forbiddenPath
        Write-Utf8File -Path $allowlistPath -Text (($allowlist -join "`n") + "`n")
    }.GetNewClosure() -ExpectedPattern 'Forbidden overlay allowlist entry'

    $fixture = New-OverlayFixture
    try {
        $allowlistPath = Join-Path $fixture 'tools/migration/dxvk-overlay-files.txt'
        $allowlist = @(Get-Content -LiteralPath $allowlistPath)
        $allowlist[0] = $forbiddenPath
        Write-Utf8File -Path $allowlistPath -Text (($allowlist -join "`n") + "`n")
        $result = Invoke-FixtureScript `
            -Script (Join-Path $fixture 'tools/migration/import-audited-dxvk.ps1') `
            -Arguments @('-Source', (Join-Path $fixture 'audited-source'))
        if ($result.ExitCode -eq 0) {
            $failures.Add("importer forbidden path $forbiddenPath was accepted")
        } elseif ($result.Output -notmatch 'Forbidden overlay entry') {
            $failures.Add(
                "importer forbidden path $forbiddenPath failed for the wrong reason: $($result.Output)")
        }
    } finally {
        Remove-OverlayFixture -Fixture $fixture
    }
}

if ($failures.Count) {
    throw "DXVK overlay regressions failed:`n$($failures -join "`n")"
}

Write-Output 'PASS DXVK overlay regression coverage'
