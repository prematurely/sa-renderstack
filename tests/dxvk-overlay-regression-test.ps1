$ErrorActionPreference = 'Stop'
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
$auditedSource = 'D:\GTA San Andreas\.codex-src\dxvk\dxvk-3.0.1-bridge'
$baselineCommit = '07d715df896a1b54d8e08086435408b38f688fae'
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$failures = [Collections.Generic.List[string]]::new()
$createdFixtures = [Collections.Generic.List[string]]::new()

. (Join-Path $PSScriptRoot 'helpers/git-baseline.ps1')

function Get-Sha256 {
    param([Parameter(Mandatory)] [byte[]]$Bytes)

    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
}

function Get-NormalizedSourceBytes {
    param([Parameter(Mandatory)] [string]$Path)

    $text = $strictUtf8.GetString([IO.File]::ReadAllBytes($Path))
    if ($text.Length -and $text[0] -eq [char]0xFEFF) {
        $text = $text.Substring(1)
    }
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    return ,$utf8NoBom.GetBytes($text)
}

function Write-Utf8File {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Text
    )

    [IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Invoke-GitFixture {
    param(
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [string]$Operation
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = 'git'
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw "Git failed to start while $Operation"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($exitCode -ne 0) {
            $null = $stdoutTask.GetAwaiter().GetResult()
            $detail = $stderr.TrimEnd([char[]]"`r`n")
            throw "Git failed while $Operation (exit $exitCode): $detail"
        }
        return $stdoutTask.GetAwaiter().GetResult()
    } finally {
        $process.Dispose()
    }
}

function Get-RootGitState {
    return [ordered]@{
        Head = Invoke-GitBaselineText -Arguments @('rev-parse', 'HEAD') `
            -Operation 'capturing root HEAD'
        Refs = Invoke-GitBaselineText `
            -Arguments @('for-each-ref', '--format=%(refname)%00%(objectname)') `
            -Operation 'capturing root refs'
        IndexTree = Invoke-GitBaselineText -Arguments @('write-tree') `
            -Operation 'capturing root index tree'
        WorktreeDiff = Invoke-GitBaselineText `
            -Arguments @('diff', '--binary', '--no-ext-diff', '--') `
            -Operation 'capturing root tracked worktree diff'
        CachedDiff = Invoke-GitBaselineText `
            -Arguments @('diff', '--cached', '--binary', '--no-ext-diff', '--') `
            -Operation 'capturing root staged diff'
        Status = Invoke-GitBaselineText `
            -Arguments @('status', '--porcelain=v1', '--untracked-files=all') `
            -Operation 'capturing root status'
    }
}

function Assert-RootGitStateUnchanged {
    param([Parameter(Mandatory)] $Expected)

    $actual = Get-RootGitState
    foreach ($name in $Expected.Keys) {
        if ($actual[$name] -cne $Expected[$name]) {
            throw "Root repository $name changed during DXVK fixture regression"
        }
    }
}

function Assert-SafeFixturePath {
    param([Parameter(Mandatory)] [string]$Fixture)

    $resolved = [IO.Path]::GetFullPath($Fixture).TrimEnd('\')
    if (-not [IO.Path]::GetDirectoryName($resolved).Equals(
            $tempBase,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Fixture escaped the temporary parent: $resolved"
    }
    if ([IO.Path]::GetFileName($resolved) -notmatch
        '^sa-renderstack-task5b-[0-9]+-[0-9a-f]{32}$') {
        throw "Fixture name is not allowlisted: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        $item = Get-Item -LiteralPath $resolved -Force
        if (-not $item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Fixture must be a non-reparse directory: $resolved"
        }
    }
    return $resolved
}

function Remove-OverlayFixture {
    param([Parameter(Mandatory)] [string]$Fixture)

    $resolved = Assert-SafeFixturePath -Fixture $Fixture
    if (Test-Path -LiteralPath $resolved) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

function Initialize-HistoricalBlobs {
    Assert-GitFullHistory
    Assert-GitCommit -Commit $baselineCommit -Label 'DXVK fixture source baseline'

    $manifestBytes = Read-GitBlob -Commit $baselineCommit `
        -Path 'migration/dxvk-overlay-manifest.json' `
        -Label 'DXVK fixture overlay manifest'
    $manifest = $strictUtf8.GetString($manifestBytes) | ConvertFrom-Json
    $manifestFiles = @($manifest.files)
    if ($manifestFiles.Count -ne 30) {
        throw "Expected 30 historical DXVK fixture paths, found $($manifestFiles.Count)"
    }

    $blobs = [ordered]@{
        'tools/migration/dxvk-overlay-files.txt' = Read-GitBlob `
            -Commit $baselineCommit `
            -Path 'tools/migration/dxvk-overlay-files.txt' `
            -Label 'DXVK fixture allowlist'
        'migration/dxvk-overlay-manifest.json' = $manifestBytes
        'migration/dxvk-baseline-evidence.json' = Read-GitBlob `
            -Commit $baselineCommit `
            -Path 'migration/dxvk-baseline-evidence.json' `
            -Label 'DXVK fixture baseline evidence'
    }
    foreach ($entry in $manifestFiles) {
        $relative = [string]$entry.path
        $blobPath = "backend/dxvk/$relative"
        $blobs[$blobPath] = Read-GitBlob -Commit $baselineCommit `
            -Path $blobPath `
            -Label "DXVK fixture destination '$relative'"
    }
    return $blobs
}

function New-OverlayFixture {
    param([Parameter(Mandatory)] $HistoricalBlobs)

    $fixture = Join-Path $tempBase (
        "sa-renderstack-task5b-$PID-$([guid]::NewGuid().ToString('N'))")
    $fixture = Assert-SafeFixturePath -Fixture $fixture
    [void]$createdFixtures.Add($fixture)
    try {
        [IO.Directory]::CreateDirectory($fixture) | Out-Null
        foreach ($relativePath in $HistoricalBlobs.Keys) {
            $destination = Join-Path $fixture $relativePath
            [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
            [IO.File]::WriteAllBytes($destination, $HistoricalBlobs[$relativePath])
        }

        $fixtureTestDirectory = Join-Path $fixture 'tests'
        $fixtureHelperDirectory = Join-Path $fixture 'tests/helpers'
        [IO.Directory]::CreateDirectory($fixtureHelperDirectory) | Out-Null
        Copy-Item -LiteralPath (Join-Path $root 'tests/dxvk-overlay-manifest-test.ps1') `
            -Destination (Join-Path $fixtureTestDirectory 'dxvk-overlay-manifest-test.ps1')
        Copy-Item -LiteralPath (Join-Path $root 'tests/helpers/git-baseline.ps1') `
            -Destination (Join-Path $fixtureHelperDirectory 'git-baseline.ps1')
        Copy-Item -LiteralPath (Join-Path $root 'tools/migration/import-audited-dxvk.ps1') `
            -Destination (Join-Path $fixture 'tools/migration/import-audited-dxvk.ps1')
        Write-Utf8File -Path (Join-Path $fixture '.gitattributes') -Text "* -text`n"

        $null = Invoke-GitFixture -WorkingDirectory $fixture `
            -Arguments @('init', '--quiet') `
            -Operation 'initializing a DXVK regression fixture'
        $null = Invoke-GitFixture -WorkingDirectory $fixture `
            -Arguments @('config', 'user.name', 'SA RenderStack Fixture') `
            -Operation 'setting the fixture Git user name'
        $null = Invoke-GitFixture -WorkingDirectory $fixture `
            -Arguments @('config', 'user.email', 'fixture@sa-renderstack.invalid') `
            -Operation 'setting the fixture Git user email'
        $null = Invoke-GitFixture -WorkingDirectory $fixture `
            -Arguments @('config', 'core.autocrlf', 'false') `
            -Operation 'disabling fixture Git EOL conversion'
        $null = Invoke-GitFixture -WorkingDirectory $fixture `
            -Arguments @('add', '--all', '--', '.') `
            -Operation 'staging the correct DXVK fixture baseline'
        $null = Invoke-GitFixture -WorkingDirectory $fixture `
            -Arguments @('commit', '--quiet', '-m', 'correct historical baseline') `
            -Operation 'committing the correct DXVK fixture baseline'
        $fixtureCommit = (Invoke-GitFixture -WorkingDirectory $fixture `
                -Arguments @('rev-parse', 'HEAD') `
                -Operation 'reading the correct DXVK fixture commit').TrimEnd([char[]]"`r`n")
        if ($fixtureCommit -cnotmatch '\A[0-9a-f]{40}\z') {
            throw "Fixture baseline commit is not a full Git SHA: $fixtureCommit"
        }
        return [pscustomobject]@{
            Path = $fixture
            BaselineCommit = $fixtureCommit
        }
    } catch {
        Remove-OverlayFixture -Fixture $fixture
        throw
    }
}

function Commit-FixtureMutation {
    param(
        [Parameter(Mandatory)] [string]$Fixture,
        [Parameter(Mandatory)] [string]$Name
    )

    $null = Invoke-GitFixture -WorkingDirectory $Fixture `
        -Arguments @('add', '--all', '--', '.') `
        -Operation "staging fixture mutation '$Name'"
    $null = Invoke-GitFixture -WorkingDirectory $Fixture `
        -Arguments @('commit', '--quiet', '-m', $Name) `
        -Operation "committing fixture mutation '$Name'"
    $commit = (Invoke-GitFixture -WorkingDirectory $Fixture `
            -Arguments @('rev-parse', 'HEAD') `
            -Operation "reading fixture mutation '$Name'").TrimEnd([char[]]"`r`n")
    if ($commit -cnotmatch '\A[0-9a-f]{40}\z') {
        throw "Fixture mutation commit is not a full Git SHA: $commit"
    }
    return $commit
}

function Invoke-FixtureScript {
    param(
        [Parameter(Mandatory)] [string]$Script,
        [string[]]$Arguments = @()
    )

    $output = @(& pwsh -NoProfile -File $Script @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = ($output | Out-String).Trim()
    }
}

function Assert-BaselineOverrideBehavior {
    param([Parameter(Mandatory)] $HistoricalBlobs)

    $fixtureInfo = New-OverlayFixture -HistoricalBlobs $HistoricalBlobs
    try {
        $script = Join-Path $fixtureInfo.Path 'tests/dxvk-overlay-manifest-test.ps1'
        foreach ($invalidCommit in @(
                'not-a-full-git-commit',
                'gggggggggggggggggggggggggggggggggggggggg')) {
            $invalidResult = Invoke-FixtureScript -Script $script `
                -Arguments @('-BaselineCommit', $invalidCommit)
            if ($invalidResult.ExitCode -eq 0 -or
                $invalidResult.Output -notmatch 'full 40-character hexadecimal Git commit') {
                $failures.Add(
                    "invalid baseline override '$invalidCommit' failed for the wrong reason: $($invalidResult.Output)")
            }
        }

        $uppercaseCommit = $fixtureInfo.BaselineCommit.ToUpperInvariant()
        $validResult = Invoke-FixtureScript -Script $script `
            -Arguments @('-BaselineCommit', $uppercaseCommit)
        if ($validResult.ExitCode -ne 0 -or
            $validResult.Output -notmatch 'PASS DXVK overlay manifest and baseline evidence') {
            $failures.Add(
                "uppercase baseline override was rejected: $($validResult.Output)")
        }
    } finally {
        Remove-OverlayFixture -Fixture $fixtureInfo.Path
    }
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [scriptblock]$Arrange,
        [Parameter(Mandatory)] [string]$ExpectedPattern,
        [Parameter(Mandatory)] $HistoricalBlobs,
        [switch]$Live
    )

    $fixtureInfo = New-OverlayFixture -HistoricalBlobs $HistoricalBlobs
    try {
        & $Arrange $fixtureInfo.Path
        $tamperedCommit = Commit-FixtureMutation -Fixture $fixtureInfo.Path -Name $Name
        $arguments = @('-BaselineCommit', $tamperedCommit)
        if ($Live) {
            $arguments += @('-Source', $auditedSource)
        }
        $result = Invoke-FixtureScript `
            -Script (Join-Path $fixtureInfo.Path 'tests/dxvk-overlay-manifest-test.ps1') `
            -Arguments $arguments
        if ($result.ExitCode -eq 0) {
            $failures.Add("$Name was accepted")
        } elseif ($result.Output -notmatch $ExpectedPattern) {
            $failures.Add("$Name failed for the wrong reason: $($result.Output)")
        }
    } finally {
        Remove-OverlayFixture -Fixture $fixtureInfo.Path
    }
}

function Set-CoordinatedAllowlistSubstitution {
    param([Parameter(Mandatory)] [string]$Fixture)

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
    $manifest.files[0].sourceSha256 = (Get-FileHash -Algorithm SHA256 `
            -LiteralPath $sourcePath).Hash
    $manifest.files[0].sha256 = Get-Sha256 -Bytes $destinationBytes
    Write-Utf8File -Path $manifestPath `
        -Text (($manifest | ConvertTo-Json -Depth 10) + "`n")
}

function Set-CoordinatedDestinationTampering {
    param([Parameter(Mandatory)] [string]$Fixture)

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
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Property,
        [Parameter(Mandatory)] [string]$Value
    )

    $json = [IO.File]::ReadAllText($Path, $strictUtf8)
    $escapedValue = $Value.Replace('\', '\\').Replace('"', '\"')
    $insertion = "{`n  `"$Property`": `"$escapedValue`","
    Write-Utf8File -Path $Path -Text ($insertion + $json.Substring(1))
}

$rootStateBefore = Get-RootGitState
$initialTempFixtures = @(
    Get-ChildItem -LiteralPath $tempBase -Directory `
        -Filter 'sa-renderstack-task5b-*' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName } |
        Sort-Object
)
$testFailure = $null
$cleanupFailures = [Collections.Generic.List[string]]::new()
try {
    $historicalBlobs = Initialize-HistoricalBlobs
    Assert-BaselineOverrideBehavior -HistoricalBlobs $historicalBlobs

    Assert-Rejected -Name 'coordinated allowlist substitution' `
        -Arrange ${function:Set-CoordinatedAllowlistSubstitution} `
        -ExpectedPattern 'allowlist (hash|entries).*differ' `
        -HistoricalBlobs $historicalBlobs

    Assert-Rejected -Name 'offline coordinated destination tampering' `
        -Arrange ${function:Set-CoordinatedDestinationTampering} `
        -ExpectedPattern 'canonical hash differs' `
        -HistoricalBlobs $historicalBlobs

    Assert-Rejected -Name 'live coordinated destination tampering' `
        -Arrange ${function:Set-CoordinatedDestinationTampering} `
        -ExpectedPattern 'canonical hash differs|normalized audited source differs' `
        -HistoricalBlobs $historicalBlobs `
        -Live

    Assert-Rejected -Name 'duplicate overlay-manifest property' -Arrange {
        param($fixture)
        Add-DuplicateProperty `
            -Path (Join-Path $fixture 'migration/dxvk-overlay-manifest.json') `
            -Property 'sourceLabel' `
            -Value 'C:\hidden-root'
    } -ExpectedPattern "Duplicate JSON property 'sourceLabel'" `
        -HistoricalBlobs $historicalBlobs

    Assert-Rejected -Name 'duplicate baseline-evidence property' -Arrange {
        param($fixture)
        Add-DuplicateProperty `
            -Path (Join-Path $fixture 'migration/dxvk-baseline-evidence.json') `
            -Property 'sourceReportSha256' `
            -Value 'C:\hidden-root'
    } -ExpectedPattern "Duplicate JSON property 'sourceReportSha256'" `
        -HistoricalBlobs $historicalBlobs

    foreach ($forbiddenPath in @('artifact.obj', 'artifact.o', 'nested/.wraplock')) {
        Assert-Rejected -Name "verifier forbidden path $forbiddenPath" -Arrange {
            param($fixture)
            $allowlistPath = Join-Path $fixture 'tools/migration/dxvk-overlay-files.txt'
            $allowlist = @(Get-Content -LiteralPath $allowlistPath)
            $allowlist[0] = $forbiddenPath
            Write-Utf8File -Path $allowlistPath -Text (($allowlist -join "`n") + "`n")
        }.GetNewClosure() -ExpectedPattern 'Forbidden overlay allowlist entry' `
            -HistoricalBlobs $historicalBlobs

        $fixtureInfo = New-OverlayFixture -HistoricalBlobs $historicalBlobs
        try {
            $allowlistPath = Join-Path $fixtureInfo.Path 'tools/migration/dxvk-overlay-files.txt'
            $allowlist = @(Get-Content -LiteralPath $allowlistPath)
            $allowlist[0] = $forbiddenPath
            Write-Utf8File -Path $allowlistPath -Text (($allowlist -join "`n") + "`n")
            $result = Invoke-FixtureScript `
                -Script (Join-Path $fixtureInfo.Path 'tools/migration/import-audited-dxvk.ps1') `
                -Arguments @('-Source', (Join-Path $fixtureInfo.Path 'audited-source'))
            if ($result.ExitCode -eq 0) {
                $failures.Add("importer forbidden path $forbiddenPath was accepted")
            } elseif ($result.Output -notmatch 'Forbidden overlay entry') {
                $failures.Add(
                    "importer forbidden path $forbiddenPath failed for the wrong reason: $($result.Output)")
            }
        } finally {
            Remove-OverlayFixture -Fixture $fixtureInfo.Path
        }
    }

    if ($failures.Count) {
        throw "DXVK overlay regressions failed:`n$($failures -join "`n")"
    }
} catch {
    $testFailure = $_
} finally {
    foreach ($fixture in $createdFixtures) {
        try {
            if (Test-Path -LiteralPath $fixture) {
                Remove-OverlayFixture -Fixture $fixture
            }
            if (Test-Path -LiteralPath $fixture) {
                [void]$cleanupFailures.Add("Temporary fixture remains: $fixture")
            }
        } catch {
            [void]$cleanupFailures.Add(
                "Temporary fixture cleanup failed for ${fixture}: $($_.Exception.Message)")
        }
    }

    try {
        $finalTempFixtures = @(
            Get-ChildItem -LiteralPath $tempBase -Directory `
                -Filter 'sa-renderstack-task5b-*' -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName } |
                Sort-Object
        )
        if (($initialTempFixtures -join "`n") -cne ($finalTempFixtures -join "`n")) {
            [void]$cleanupFailures.Add('Temporary DXVK fixture set changed after regression')
        }
    } catch {
        [void]$cleanupFailures.Add(
            "Temporary fixture-set verification failed: $($_.Exception.Message)")
    }

    try {
        Assert-RootGitStateUnchanged -Expected $rootStateBefore
    } catch {
        [void]$cleanupFailures.Add($_.Exception.Message)
    }
}

if ($cleanupFailures.Count) {
    throw ($cleanupFailures -join '; ')
}
if ($null -ne $testFailure) {
    throw $testFailure
}

Write-Output 'PASS DXVK overlay regression coverage with historical Git fixtures'
