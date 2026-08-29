param(
    [switch]$Help,
    [string]$Python,
    [string]$PackageUrl = 'https://files.pythonhosted.org/packages/5e/cd/f3a881ff5e601d6bbeff63b38ee2362e1167c47d9cde03eddf8d71a4ffb0/meson-1.11.1-py3-none-any.whl'
)

$env:GIT_CONFIG_GLOBAL = 'NUL'

if ($Help) {
    @'
Usage: pwsh -NoProfile -File tools/prepare-meson.ps1 [-Help] [-Python <path>] [-PackageUrl <url-or-local-path>]

Downloads and verifies the Meson 1.11.1 wheel, then publishes its module tree
under out/deps/meson/1.11.1/site-packages.
'@ | Write-Output
    exit 0
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'lib/process-runner.ps1')
. (Join-Path $PSScriptRoot 'lib/toolchain-discovery.ps1')

$mesonVersion = '1.11.1'
$expectedWheelSha256 = '9B3A023657E393DBC5335B95C561337D49B7A458F5541E47EC44F2CC566E0D80'
$wheelName = 'meson-1.11.1-py3-none-any.whl'
$downloadTemporary = $null
$extractTemporary = $null
$cache = $null
$failureMessage = $null
$moduleDirectory = $null

function Get-MesonFileSha256 {
    param([Parameter(Mandatory)] [string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Cannot hash missing Meson wheel: $Path"
    }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path -ErrorAction Stop).Hash.ToUpperInvariant()
}

function New-MesonTemporaryPath {
    param(
        [Parameter(Mandatory)] [string]$Cache,
        [Parameter(Mandatory)] [string]$Prefix,
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$Extension
    )

    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $name = "$Prefix$([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))-$PID-$([Guid]::NewGuid().ToString('N').Substring(0, 8))$Extension"
        $candidate = Join-Path $Cache $name
        Assert-RenderStackNoReparsePath -Path $candidate -Anchor $Cache -Description 'Meson temporary path' | Out-Null
        if (-not (Get-RenderStackPathState -Path $candidate).Exists) {
            return $candidate
        }
    }
    throw "Unable to allocate a unique Meson temporary path under $Cache"
}

function Assert-SafeMesonZipEntry {
    param([Parameter(Mandatory)] [string]$Path)

    if ([string]::IsNullOrEmpty($Path) -or $Path.IndexOf([char]0) -ge 0) {
        throw "Unsafe ZIP entry path: '$Path'"
    }
    $canonical = $Path.Replace('\', '/')
    if ($canonical -match '^(?:/|[A-Za-z]:)') {
        throw "Unsafe rooted ZIP entry path: $Path"
    }
    foreach ($segment in $canonical.Split('/')) {
        if ([string]::IsNullOrEmpty($segment) -or $segment -eq '.' -or $segment -eq '..' -or
            $segment.Contains(':') -or $segment.EndsWith('.') -or $segment.EndsWith(' ')) {
            throw "Unsafe ZIP entry path: $Path"
        }
    }
    return $canonical
}

function Copy-MesonPackageSource {
    param(
        [Parameter(Mandatory)] [string]$Url,
        [Parameter(Mandatory)] [string]$Destination,
        [Parameter(Mandatory)] [string]$Cache
    )

    Assert-RenderStackNoReparsePath -Path $Destination -Anchor $Cache -Description 'Meson download destination' | Out-Null
    if (Test-Path -LiteralPath $Url -PathType Leaf) {
        [IO.File]::Copy((Get-RenderStackFullPath -Path $Url), $Destination, $false)
        return
    }
    $uri = $null
    if ([Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri) -and $uri.IsFile) {
        if (-not (Test-Path -LiteralPath $uri.LocalPath -PathType Leaf)) {
            throw "Meson wheel file URL does not exist: $Url"
        }
        [IO.File]::Copy((Get-RenderStackFullPath -Path $uri.LocalPath), $Destination, $false)
        return
    }
    try {
        Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing -ErrorAction Stop
    } catch {
        throw "Meson wheel download failed for '$Url': $($_.Exception.Message)"
    }
}

function Expand-ValidatedMesonWheel {
    param(
        [Parameter(Mandatory)] [string]$ArchivePath,
        [Parameter(Mandatory)] [string]$Destination,
        [Parameter(Mandatory)] [string]$Cache
    )

    $archive = $null
    try {
        Assert-RenderStackNoReparsePath -Path $ArchivePath -Anchor $Cache -Description 'Meson wheel archive' | Out-Null
        $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
        $entryNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $entries = [Collections.Generic.List[object]]::new()
        $mainFound = $false
        foreach ($entry in $archive.Entries) {
            $rawName = $entry.FullName
            $isDirectory = $rawName.EndsWith('/') -or $rawName.EndsWith('\')
            $canonical = Assert-SafeMesonZipEntry -Path ($rawName.Replace('\', '/').TrimEnd('/'))
            if (-not $entryNames.Add($canonical)) {
                throw "Duplicate ZIP entry path (case-insensitive): $canonical"
            }
            if ($canonical -ceq 'mesonbuild/mesonmain.py' -and -not $isDirectory) {
                $mainFound = $true
            }
            [void]$entries.Add([pscustomobject]@{
                Entry = $entry
                Name = $canonical
                IsDirectory = $isDirectory
            })
        }
        if (-not $mainFound) {
            throw 'Meson wheel is missing mesonbuild/mesonmain.py'
        }

        Assert-RenderStackNoReparsePath -Path $Destination -Anchor $Cache -Description 'Meson extraction directory' | Out-Null
        New-Item -ItemType Directory -Path $Destination -ErrorAction Stop | Out-Null
        foreach ($item in $entries) {
            $relative = $item.Name.Replace('/', [IO.Path]::DirectorySeparatorChar)
            $target = Assert-RenderStackPathUnder -Path (Join-Path $Destination $relative) `
                -Base $Destination -Description 'Meson ZIP extraction target'
            if ($item.IsDirectory) {
                if (-not (Test-Path -LiteralPath $target)) {
                    New-Item -ItemType Directory -Path $target -Force | Out-Null
                }
                continue
            }
            $parent = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $parent)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            $inputStream = $item.Entry.Open()
            $outputStream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                $inputStream.CopyTo($outputStream)
            } finally {
                $outputStream.Dispose()
                $inputStream.Dispose()
            }
        }
    } finally {
        if ($null -ne $archive) {
            $archive.Dispose()
        }
    }
    Assert-RenderStackNoReparseTree -Path $Destination -Anchor $Cache -Description 'Extracted Meson tree' | Out-Null
}

function Get-MesonStreamSha256Base64Url {
    param([Parameter(Mandatory)] [IO.Stream]$Stream)

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha256.ComputeHash($Stream)
    } finally {
        $sha256.Dispose()
    }
    return [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
}

function Get-MesonWheelManifest {
    param(
        [Parameter(Mandatory)] [string]$WheelPath,
        [Parameter(Mandatory)] [string]$Cache
    )

    $safeWheel = Assert-RenderStackNoReparsePath -Path $WheelPath -Anchor $Cache `
        -Description 'Pinned Meson wheel'
    if ((Get-MesonFileSha256 -Path $safeWheel) -cne $expectedWheelSha256) {
        throw "Pinned Meson wheel SHA-256 mismatch: $safeWheel"
    }

    $archive = $null
    try {
        $archive = [IO.Compression.ZipFile]::OpenRead($safeWheel)
        $archiveEntries = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')) {
                continue
            }
            $canonical = Assert-SafeMesonZipEntry -Path $entry.FullName.Replace('\', '/')
            if (-not $archiveEntries.TryAdd($canonical, $entry)) {
                throw "Duplicate Meson wheel file path (case-insensitive): $canonical"
            }
        }
        $recordPaths = @($archiveEntries.Keys | Where-Object { $_ -match '(?i)\.dist-info/RECORD$' })
        if ($recordPaths.Count -ne 1) {
            throw "Meson wheel must contain exactly one dist-info/RECORD file; found $($recordPaths.Count)"
        }
        $recordPath = $recordPaths[0]
        $recordEntry = $archiveEntries[$recordPath]
        $recordStream = $recordEntry.Open()
        try {
            $memory = [IO.MemoryStream]::new()
            try {
                $recordStream.CopyTo($memory)
                $recordBytes = $memory.ToArray()
            } finally {
                $memory.Dispose()
            }
        } finally {
            $recordStream.Dispose()
        }
        $recordText = [Text.Encoding]::UTF8.GetString($recordBytes)
        $records = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($row in @($recordText | ConvertFrom-Csv -Header Path, Hash, Size)) {
            if ([string]::IsNullOrWhiteSpace($row.Path)) {
                continue
            }
            $path = Assert-SafeMesonZipEntry -Path ([string]$row.Path).Replace('\', '/')
            if (-not $records.TryAdd($path, $row)) {
                throw "Duplicate Meson RECORD path (case-insensitive): $path"
            }
        }
        $missingRecords = @($archiveEntries.Keys | Where-Object { -not $records.ContainsKey($_) })
        $extraRecords = @($records.Keys | Where-Object { -not $archiveEntries.ContainsKey($_) })
        if ($missingRecords.Count -ne 0 -or $extraRecords.Count -ne 0) {
            throw "Meson wheel RECORD file set mismatch: missing=$($missingRecords -join ', ') extra=$($extraRecords -join ', ')"
        }

        $manifest = [Collections.Generic.List[object]]::new()
        foreach ($path in @($records.Keys | Sort-Object)) {
            $row = $records[$path]
            $entry = $archiveEntries[$path]
            $isRecord = $path.Equals($recordPath, [StringComparison]::OrdinalIgnoreCase)
            if ($isRecord) {
                if (-not [string]::IsNullOrEmpty($row.Hash) -or -not [string]::IsNullOrEmpty($row.Size)) {
                    throw 'Meson RECORD self-entry must omit hash and size'
                }
                $stream = $entry.Open()
                try {
                    $hash = Get-MesonStreamSha256Base64Url -Stream $stream
                } finally {
                    $stream.Dispose()
                }
                $size = [long]$entry.Length
            } else {
                if ([string]::IsNullOrWhiteSpace($row.Hash) -or [string]::IsNullOrWhiteSpace($row.Size)) {
                    throw "Meson RECORD entry must provide hash and size: $path"
                }
                if ($row.Hash -notmatch '^sha256=(?<hash>[A-Za-z0-9_-]+)$') {
                    throw "Meson RECORD entry uses an unsupported hash: $path"
                }
                $hash = $Matches.hash
                $size = [long]$row.Size
                if ($size -ne [long]$entry.Length) {
                    throw "Meson wheel RECORD size mismatch for $path"
                }
                $stream = $entry.Open()
                try {
                    $actualHash = Get-MesonStreamSha256Base64Url -Stream $stream
                } finally {
                    $stream.Dispose()
                }
                if ($actualHash -cne $hash) {
                    throw "Meson wheel RECORD SHA-256 mismatch for $path"
                }
            }
            [void]$manifest.Add([pscustomobject]@{ Path = $path; Hash = $hash; Size = $size })
        }
        return [pscustomobject]@{ RecordPath = $recordPath; Files = $manifest.ToArray() }
    } finally {
        if ($null -ne $archive) {
            $archive.Dispose()
        }
    }
}

function Assert-PublishedMesonManifest {
    param(
        [Parameter(Mandatory)] [string]$ModuleDirectory,
        [Parameter(Mandatory)] [string]$Cache,
        [Parameter(Mandatory)] [object]$Manifest
    )

    Assert-RenderStackNoReparseTree -Path $ModuleDirectory -Anchor $Cache `
        -Description 'Published Meson module tree' | Out-Null
    $expected = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $Manifest.Files) {
        $expected.Add([string]$file.Path, $file)
    }
    $actual = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in [IO.Directory]::EnumerateFiles($ModuleDirectory, '*', [IO.SearchOption]::AllDirectories)) {
        $safeFile = Assert-RenderStackNoReparsePath -Path $file -Anchor $ModuleDirectory `
            -Description 'Published Meson file'
        $relative = [IO.Path]::GetRelativePath($ModuleDirectory, $safeFile).Replace('\', '/')
        if (-not $actual.TryAdd($relative, $safeFile)) {
            throw "Published Meson cache is invalid: duplicate file path: $relative"
        }
    }
    $missing = @($expected.Keys | Where-Object { -not $actual.ContainsKey($_) })
    $extra = @($actual.Keys | Where-Object { -not $expected.ContainsKey($_) })
    if ($missing.Count -ne 0 -or $extra.Count -ne 0) {
        throw "Published Meson cache is invalid: file set mismatch: missing=$($missing -join ', ') extra=$($extra -join ', ')"
    }
    foreach ($path in $expected.Keys) {
        $expectedFile = $expected[$path]
        $actualPath = $actual[$path]
        $item = Get-Item -LiteralPath $actualPath -ErrorAction Stop
        if ([long]$item.Length -ne [long]$expectedFile.Size) {
            throw "Published Meson cache is invalid: size mismatch for $path"
        }
        $stream = [IO.File]::Open($actualPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            $actualHash = Get-MesonStreamSha256Base64Url -Stream $stream
        } finally {
            $stream.Dispose()
        }
        if ($actualHash -cne [string]$expectedFile.Hash) {
            throw "Published Meson cache is invalid: SHA-256 mismatch for $path"
        }
    }
}

function Wait-MesonTestPublicationBarrier {
    param([Parameter(Mandatory)] [ValidateSet('wheel', 'module')] [string]$Phase)

    $barrier = [Environment]::GetEnvironmentVariable('SA_RENDERSTACK_TEST_PREPARE_MESON_BARRIER', 'Process')
    if ([string]::IsNullOrWhiteSpace($barrier)) {
        return
    }
    $barrier = Get-RenderStackFullPath -Path $barrier
    if (-not (Test-Path -LiteralPath $barrier -PathType Container)) {
        throw "Meson test publication barrier does not exist: $barrier"
    }
    [IO.File]::WriteAllText((Join-Path $barrier "$Phase-$PID.ready"), 'ready')
    $go = Join-Path $barrier "$Phase.go"
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $go -PathType Leaf)) {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Timed out waiting for Meson test publication barrier: $Phase"
        }
        Start-Sleep -Milliseconds 25
    }
}

function Test-PublishedMeson {
    param(
        [Parameter(Mandatory)] [string]$ModuleDirectory,
        [Parameter(Mandatory)] [string]$Cache,
        [Parameter(Mandatory)] [string]$PythonPath,
        [Parameter(Mandatory)] [object]$Manifest
    )

    $safeModuleDirectory = Assert-RenderStackNoReparsePath -Path $ModuleDirectory -Anchor $Cache `
        -Description 'Published Meson module directory'
    $state = Get-RenderStackPathState -Path $safeModuleDirectory
    if (-not $state.Exists) {
        return $false
    }
    if (($state.Attributes -band [IO.FileAttributes]::Directory) -eq 0) {
        throw "Published Meson cache is invalid: module path is not a directory: $safeModuleDirectory"
    }
    Assert-PublishedMesonManifest -ModuleDirectory $safeModuleDirectory -Cache $Cache -Manifest $Manifest
    $result = Invoke-RenderStackProcess -FilePath $PythonPath -ArgumentList @(
        '-m', 'mesonbuild.mesonmain', '--version'
    ) -WorkingDirectory $Cache -EnvironmentOverrides @{
        PYTHONPATH = $safeModuleDirectory
        PYTHONDONTWRITEBYTECODE = '1'
    } `
        -Label 'meson-published-version'
    if ($result.ExitCode -ne 0 -or $result.StandardOutput.Trim() -cne $mesonVersion) {
        throw "Published Meson cache is invalid: expected version $mesonVersion, got '$($result.StandardOutput.Trim())'"
    }
    return $true
}

function Remove-MesonTemporaryPath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Cache
    )

    $safePath = Assert-RenderStackNoReparsePath -Path $Path -Anchor $Cache -Description 'Meson cleanup path'
    $state = Get-RenderStackPathState -Path $safePath
    if ($state.Exists) {
        if (($state.Attributes -band [IO.FileAttributes]::Directory) -ne 0) {
            Assert-RenderStackNoReparseTree -Path $safePath -Anchor $Cache -Description 'Meson cleanup tree' | Out-Null
        }
        Remove-Item -LiteralPath $safePath -Recurse -Force -ErrorAction Stop
    }
}

try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
    $root = Get-RenderStackRepositoryRoot -CallerScriptPath $PSCommandPath
    $selectedPython = if (-not [string]::IsNullOrWhiteSpace($Python)) {
        $Python
    } else {
        (Find-RenderStackPython -RepoRoot $root).Path
    }
    $pythonInfo = Get-RenderStackPythonInfo -Path $selectedPython `
        -Source 'prepare-meson' -WorkingDirectory $root

    $cache = Assert-RenderStackPathUnder -Path (Join-Path $root 'out/deps/meson/1.11.1') `
        -Base $root -Description 'Meson cache directory'
    Assert-RenderStackNoReparsePath -Path $cache -Anchor $root -Description 'Meson cache directory' | Out-Null
    $cacheState = Get-RenderStackPathState -Path $cache
    if ($cacheState.Exists) {
        if (($cacheState.Attributes -band [IO.FileAttributes]::Directory) -eq 0) {
            throw "Meson cache path is not a directory: $cache"
        }
    } else {
        New-Item -ItemType Directory -Path $cache -Force | Out-Null
        Assert-RenderStackNoReparsePath -Path $cache -Anchor $root -Description 'Meson cache directory' | Out-Null
    }

    $wheelPath = Assert-RenderStackPathUnder -Path (Join-Path $cache $wheelName) `
        -Base $cache -Description 'Meson wheel cache path'
    $moduleDirectory = Assert-RenderStackPathUnder -Path (Join-Path $cache 'site-packages') `
        -Base $cache -Description 'Meson module directory'
    $moduleState = Get-RenderStackPathState -Path $moduleDirectory
    if ($moduleState.Exists -and -not (Test-Path -LiteralPath $wheelPath -PathType Leaf)) {
        throw "Published Meson cache is invalid: pinned wheel is missing: $wheelPath"
    }
    $manifest = if (Test-Path -LiteralPath $wheelPath -PathType Leaf) {
        Get-MesonWheelManifest -WheelPath $wheelPath -Cache $cache
    } else {
        $null
    }
    if ($null -ne $manifest -and
        (Test-PublishedMeson -ModuleDirectory $moduleDirectory -Cache $cache `
            -PythonPath $pythonInfo.Path -Manifest $manifest)) {
        Write-Output "Using verified published Meson: $moduleDirectory"
    } else {
        Assert-RenderStackNoReparsePath -Path $wheelPath -Anchor $cache -Description 'Meson wheel cache path' | Out-Null
        if (Test-Path -LiteralPath $wheelPath) {
            $cachedHash = Get-MesonFileSha256 -Path $wheelPath
            if ($cachedHash -cne $expectedWheelSha256) {
                throw "Cached Meson wheel SHA-256 mismatch: expected $expectedWheelSha256, got $cachedHash"
            }
            Write-Output "Using verified cached Meson wheel: $wheelPath"
        } else {
            $downloadTemporary = New-MesonTemporaryPath -Cache $cache -Prefix 'meson-download-' -Extension '.tmp'
            Copy-MesonPackageSource -Url $PackageUrl -Destination $downloadTemporary -Cache $cache
            $downloadHash = Get-MesonFileSha256 -Path $downloadTemporary
            if ($downloadHash -cne $expectedWheelSha256) {
                throw "Downloaded Meson wheel SHA-256 mismatch: expected $expectedWheelSha256, got $downloadHash"
            }
            Wait-MesonTestPublicationBarrier -Phase wheel
            try {
                [IO.File]::Move($downloadTemporary, $wheelPath)
                $downloadTemporary = $null
            } catch {
                $moveError = $_.Exception.Message
                if (-not (Test-Path -LiteralPath $wheelPath -PathType Leaf)) {
                    throw "Meson wheel publication failed: $moveError"
                }
                $winnerHash = Get-MesonFileSha256 -Path $wheelPath
                if ($winnerHash -cne $expectedWheelSha256) {
                    throw "Concurrent Meson wheel publication produced an invalid winner: $winnerHash"
                }
                Write-Output "Using concurrently published Meson wheel: $wheelPath"
            }
            if ((Get-MesonFileSha256 -Path $wheelPath) -cne $expectedWheelSha256) {
                throw "Cached Meson wheel changed during publication: $wheelPath"
            }
            Write-Output "Cached verified Meson wheel: $wheelPath"
        }

        $manifest = Get-MesonWheelManifest -WheelPath $wheelPath -Cache $cache

        $extractTemporary = New-MesonTemporaryPath -Cache $cache -Prefix 'meson-extract-' -Extension ''
        Expand-ValidatedMesonWheel -ArchivePath $wheelPath -Destination $extractTemporary -Cache $cache
        if (-not (Test-PublishedMeson -ModuleDirectory $extractTemporary -Cache $cache `
                -PythonPath $pythonInfo.Path -Manifest $manifest)) {
            throw "Extracted Meson module tree disappeared: $extractTemporary"
        }
        Wait-MesonTestPublicationBarrier -Phase module
        if (Test-PublishedMeson -ModuleDirectory $moduleDirectory -Cache $cache `
                -PythonPath $pythonInfo.Path -Manifest $manifest) {
            Write-Output "Using concurrently published Meson: $moduleDirectory"
        } else {
            Assert-RenderStackNoReparseTree -Path $extractTemporary -Anchor $cache `
                -Description 'Meson publication source' | Out-Null
            try {
                [IO.Directory]::Move($extractTemporary, $moduleDirectory)
                $extractTemporary = $null
                if (-not (Test-PublishedMeson -ModuleDirectory $moduleDirectory -Cache $cache `
                        -PythonPath $pythonInfo.Path -Manifest $manifest)) {
                    throw "Published Meson module tree disappeared: $moduleDirectory"
                }
                Write-Output "Published verified Meson: $moduleDirectory"
            } catch {
                $publishError = $_.Exception.Message
                if (-not (Test-PublishedMeson -ModuleDirectory $moduleDirectory -Cache $cache `
                        -PythonPath $pythonInfo.Path -Manifest $manifest)) {
                    throw "Meson publication failed: $publishError"
                }
                Write-Output "Using concurrently published Meson: $moduleDirectory"
            }
        }
    }
} catch {
    $failureMessage = $_.Exception.Message
} finally {
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    foreach ($temporaryPath in @($downloadTemporary, $extractTemporary)) {
        if ($null -eq $temporaryPath -or $null -eq $cache) {
            continue
        }
        try {
            Remove-MesonTemporaryPath -Path $temporaryPath -Cache $cache
        } catch {
            [void]$cleanupErrors.Add("${temporaryPath}: $($_.Exception.Message)")
        }
    }
    if ($cleanupErrors.Count -gt 0) {
        $cleanupMessage = "Temporary cleanup failed: $($cleanupErrors -join '; ')"
        if ($null -eq $failureMessage) {
            $failureMessage = $cleanupMessage
        } else {
            $failureMessage = "$failureMessage; $cleanupMessage"
        }
    }
}

if ($null -ne $failureMessage) {
    Write-Error "Meson preparation failed: $failureMessage"
    exit 1
}

Write-Output $moduleDirectory
exit 0
