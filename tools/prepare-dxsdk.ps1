param(
    [switch]$Help,
    [string]$PackageUrl = 'https://api.nuget.org/v3-flatcontainer/microsoft.dxsdk.d3dx/9.29.952.8/microsoft.dxsdk.d3dx.9.29.952.8.nupkg'
)

if ($Help) {
    @'
Usage: pwsh -NoProfile -File tools/prepare-dxsdk.ps1 [-Help] [-PackageUrl <url-or-local-path>]

Downloads and verifies Microsoft.DXSDK.D3DX 9.29.952.8, then publishes its
verified headers under out/deps/Microsoft.DXSDK.D3DX/9.29.952.8/package.
'@ | Write-Output
    exit 0
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$packageId = 'Microsoft.DXSDK.D3DX'
$packageVersion = '9.29.952.8'
$expectedPackageSha256 = 'EAD0906AE8A26C18A7525DA7490127A2110F7C58F18293738283E30E97C6EA4B'
$requiredHeader = 'build/native/include/d3dx9effect.h'
$expectedHeaderSha256 = '72D6665D54C425B8A99FE0716518B2711F7CECE6A3B2F8E7C6FC307E0A3FAE26'

$downloadTemp = $null
$extractTemp = $null
$cache = $null
$failureMessage = $null

function Get-FullPath {
    param([Parameter(Mandatory)] [string]$Path)

    return [IO.Path]::GetFullPath($Path)
}

function Assert-PathUnder {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Base,
        [Parameter(Mandatory)] [string]$Description
    )

    $fullPath = Get-FullPath -Path $Path
    $fullBase = (Get-FullPath -Path $Base).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $basePrefix = $fullBase + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.Equals($fullBase, [StringComparison]::OrdinalIgnoreCase) -and
        -not $fullPath.StartsWith($basePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description is outside its permitted directory: $fullPath"
    }

    return $fullPath
}

function Get-PathState {
    param([Parameter(Mandatory)] [string]$Path)

    try {
        return [pscustomobject]@{
            Exists = $true
            Attributes = [IO.File]::GetAttributes($Path)
        }
    } catch [IO.FileNotFoundException] {
        return [pscustomobject]@{ Exists = $false; Attributes = $null }
    } catch [IO.DirectoryNotFoundException] {
        return [pscustomobject]@{ Exists = $false; Attributes = $null }
    }
}

function Assert-NoReparsePath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Anchor,
        [Parameter(Mandatory)] [string]$Description
    )

    $fullPath = Assert-PathUnder -Path $Path -Base $Anchor -Description $Description
    $fullAnchor = Get-FullPath -Path $Anchor
    $relative = [IO.Path]::GetRelativePath($fullAnchor, $fullPath)
    $components = [Collections.Generic.List[string]]::new()
    [void]$components.Add($fullAnchor)
    if ($relative -ne '.') {
        $current = $fullAnchor
        foreach ($segment in $relative.Split(
                [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar),
                [StringSplitOptions]::RemoveEmptyEntries)) {
            $current = Join-Path $current $segment
            [void]$components.Add($current)
        }
    }

    foreach ($component in $components) {
        $state = Get-PathState -Path $component
        if (-not $state.Exists) {
            break
        }
        if (($state.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description contains a reparse point: $component"
        }
    }

    return $fullPath
}

function Assert-NoReparseTree {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Anchor,
        [Parameter(Mandatory)] [string]$Description
    )

    $rootPath = Assert-NoReparsePath -Path $Path -Anchor $Anchor -Description $Description
    $rootState = Get-PathState -Path $rootPath
    if (-not $rootState.Exists) {
        throw "$Description does not exist: $rootPath"
    }
    if (($rootState.Attributes -band [IO.FileAttributes]::Directory) -eq 0) {
        return
    }

    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($rootPath)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($directory)) {
            $entryPath = Assert-PathUnder -Path $entry -Base $rootPath -Description $Description
            $attributes = [IO.File]::GetAttributes($entryPath)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description contains a reparse point: $entryPath"
            }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                $pending.Push($entryPath)
            }
        }
    }
}

function Get-UniqueSiblingPath {
    param(
        [Parameter(Mandatory)] [string]$Base,
        [Parameter(Mandatory)] [string]$Prefix,
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$Extension
    )

    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $name = "$Prefix$([Guid]::NewGuid().ToString('N'))$Extension"
        $candidate = Join-Path $Base $name
        $candidate = Assert-NoReparsePath -Path $candidate -Anchor $Base -Description 'Temporary path'
        if (-not (Get-PathState -Path $candidate).Exists) {
            return $candidate
        }
    }

    throw "Unable to allocate a unique temporary path under $Base"
}

function Get-FileSha256 {
    param([Parameter(Mandatory)] [string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Cannot hash missing file: $Path"
    }

    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path -ErrorAction Stop).Hash.ToUpperInvariant()
}

function Assert-SafeZipEntryPath {
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

function Remove-GuardedPath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Cache
    )

    $safePath = Assert-NoReparsePath -Path $Path -Anchor $Cache -Description 'Cleanup path'
    $state = Get-PathState -Path $safePath
    if ($state.Exists) {
        if (($state.Attributes -band [IO.FileAttributes]::Directory) -ne 0) {
            Assert-NoReparseTree -Path $safePath -Anchor $Cache -Description 'Cleanup tree'
        }
        Assert-NoReparsePath -Path $safePath -Anchor $Cache -Description 'Cleanup path' | Out-Null
        Remove-Item -LiteralPath $safePath -Recurse -Force -ErrorAction Stop
    }
}

function Copy-DownloadedPackage {
    param(
        [Parameter(Mandatory)] [string]$Url,
        [Parameter(Mandatory)] [string]$Destination,
        [Parameter(Mandatory)] [string]$Cache
    )

    Assert-NoReparsePath -Path $Destination -Anchor $Cache -Description 'Download destination' | Out-Null
    if (Test-Path -LiteralPath $Url -PathType Leaf) {
        [IO.File]::Copy((Get-FullPath -Path $Url), $Destination, $false)
        return
    }

    $uri = $null
    if ([Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri) -and $uri.IsFile) {
        if (-not (Test-Path -LiteralPath $uri.LocalPath -PathType Leaf)) {
            throw "Package file URL does not exist: $Url"
        }
        [IO.File]::Copy((Get-FullPath -Path $uri.LocalPath), $Destination, $false)
        return
    }

    try {
        Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing -ErrorAction Stop
    } catch {
        throw "Package download failed for '$Url': $($_.Exception.Message)"
    }
}

function Extract-ValidatedArchive {
    param(
        [Parameter(Mandatory)] [string]$ArchivePath,
        [Parameter(Mandatory)] [string]$Destination,
        [Parameter(Mandatory)] [string]$RequiredEntry,
        [Parameter(Mandatory)] [string]$Cache
    )

    $archive = $null
    try {
        Assert-NoReparsePath -Path $ArchivePath -Anchor $Cache -Description 'Package archive' | Out-Null
        $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
        $entryNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $entries = [Collections.Generic.List[object]]::new()
        $requiredFileFound = $false

        foreach ($entry in $archive.Entries) {
            $rawName = $entry.FullName
            $isDirectory = $rawName.EndsWith('/') -or $rawName.EndsWith('\')
            $canonicalName = $rawName.Replace('\', '/').TrimEnd('/')
            $canonicalName = Assert-SafeZipEntryPath -Path $canonicalName
            if (-not $entryNames.Add($canonicalName)) {
                throw "Duplicate ZIP entry path (case-insensitive): $canonicalName"
            }
            if ($canonicalName -ceq $RequiredEntry -and -not $isDirectory) {
                $requiredFileFound = $true
            }
            [void]$entries.Add([pscustomobject]@{
                Entry = $entry
                Path = $canonicalName
                IsDirectory = $isDirectory
            })
        }

        if (-not $requiredFileFound) {
            throw "ZIP archive is missing required file entry: $RequiredEntry"
        }

        $safeDestination = Assert-NoReparsePath -Path $Destination -Anchor $Cache -Description 'Extraction path'
        Assert-NoReparsePath -Path $safeDestination -Anchor $Cache -Description 'Extraction path' | Out-Null
        New-Item -ItemType Directory -Force -Path $safeDestination | Out-Null
        Assert-NoReparsePath -Path $safeDestination -Anchor $Cache -Description 'Extraction path' | Out-Null
        foreach ($row in $entries) {
            $relativePath = $row.Path.Replace('/', [IO.Path]::DirectorySeparatorChar)
            $targetPath = Join-Path $safeDestination $relativePath
            $targetPath = Assert-NoReparsePath -Path $targetPath -Anchor $safeDestination -Description 'ZIP extraction target'
            if ($row.IsDirectory) {
                Assert-NoReparsePath -Path $targetPath -Anchor $safeDestination -Description 'ZIP extraction directory' | Out-Null
                New-Item -ItemType Directory -Force -Path $targetPath | Out-Null
                Assert-NoReparsePath -Path $targetPath -Anchor $safeDestination -Description 'ZIP extraction directory' | Out-Null
                continue
            }

            $parent = Split-Path -Parent $targetPath
            Assert-NoReparsePath -Path $parent -Anchor $safeDestination -Description 'ZIP extraction parent' | Out-Null
            New-Item -ItemType Directory -Force -Path $parent | Out-Null
            Assert-NoReparsePath -Path $parent -Anchor $safeDestination -Description 'ZIP extraction parent' | Out-Null
            $inputStream = $null
            $outputStream = $null
            try {
                $inputStream = $row.Entry.Open()
                Assert-NoReparsePath -Path $targetPath -Anchor $safeDestination -Description 'ZIP extraction file' | Out-Null
                $outputStream = [IO.File]::Open($targetPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                $inputStream.CopyTo($outputStream)
            } finally {
                if ($null -ne $outputStream) {
                    $outputStream.Dispose()
                }
                if ($null -ne $inputStream) {
                    $inputStream.Dispose()
                }
            }
        }
        Assert-NoReparseTree -Path $safeDestination -Anchor $Cache -Description 'Extracted package tree'
    } catch {
        throw "ZIP validation or extraction failed: $($_.Exception.Message)"
    } finally {
        if ($null -ne $archive) {
            $archive.Dispose()
        }
    }
}

function Test-PublishedPackage {
    param(
        [Parameter(Mandatory)] [string]$PackageDirectory,
        [Parameter(Mandatory)] [string]$Cache,
        [Parameter(Mandatory)] [string]$RequiredHeader,
        [Parameter(Mandatory)] [string]$ExpectedHeaderSha256
    )

    $safePackageDirectory = Assert-NoReparsePath -Path $PackageDirectory -Anchor $Cache `
        -Description 'Published package path'
    $packageState = Get-PathState -Path $safePackageDirectory
    if (-not $packageState.Exists) {
        return $false
    }
    if (($packageState.Attributes -band [IO.FileAttributes]::Directory) -eq 0) {
        throw "Published package is invalid: final path is not a directory: $safePackageDirectory"
    }

    Assert-NoReparseTree -Path $safePackageDirectory -Anchor $Cache -Description 'Published package tree'
    $headerPath = Join-Path $safePackageDirectory ($RequiredHeader.Replace('/', [IO.Path]::DirectorySeparatorChar))
    $headerPath = Assert-NoReparsePath -Path $headerPath -Anchor $safePackageDirectory `
        -Description 'Published package header'
    $headerState = Get-PathState -Path $headerPath
    if (-not $headerState.Exists -or ($headerState.Attributes -band [IO.FileAttributes]::Directory) -ne 0) {
        throw "Published package is invalid: required header is missing: $headerPath"
    }
    $headerHash = Get-FileSha256 -Path $headerPath
    if ($headerHash -cne $ExpectedHeaderSha256) {
        throw "Published package is invalid: header SHA-256 mismatch: expected $ExpectedHeaderSha256, got $headerHash"
    }

    return $true
}

try {
    $root = Get-FullPath -Path (Join-Path $PSScriptRoot '..')
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Repository root does not exist: $root"
    }
    Assert-NoReparsePath -Path $root -Anchor $root -Description 'Repository root' | Out-Null

    $cache = Get-FullPath -Path (Join-Path $root "out/deps/$packageId/$packageVersion")
    Assert-PathUnder -Path $cache -Base $root -Description 'D3DX cache directory' | Out-Null
    $packageArchive = Assert-PathUnder -Path (Join-Path $cache 'microsoft.dxsdk.d3dx.9.29.952.8.nupkg') `
        -Base $cache -Description 'Package archive path'
    $packageDirectory = Assert-PathUnder -Path (Join-Path $cache 'package') `
        -Base $cache -Description 'Package directory'

    Assert-NoReparsePath -Path $cache -Anchor $root -Description 'D3DX cache directory' | Out-Null
    $cacheState = Get-PathState -Path $cache
    if ($cacheState.Exists) {
        if (($cacheState.Attributes -band [IO.FileAttributes]::Directory) -eq 0) {
            throw "D3DX cache path is not a directory: $cache"
        }
    } else {
        Assert-NoReparsePath -Path $cache -Anchor $root -Description 'D3DX cache directory' | Out-Null
        New-Item -ItemType Directory -Path $cache -Force | Out-Null
        Assert-NoReparsePath -Path $cache -Anchor $root -Description 'D3DX cache directory' | Out-Null
    }

    $publishedPackageValid = Test-PublishedPackage -PackageDirectory $packageDirectory -Cache $cache `
        -RequiredHeader $requiredHeader -ExpectedHeaderSha256 $expectedHeaderSha256
    if ($publishedPackageValid) {
        Write-Output "Using verified published package: $packageDirectory"
    } else {
        Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
        Assert-NoReparsePath -Path $packageArchive -Anchor $cache -Description 'Cached package archive' | Out-Null
        $archiveState = Get-PathState -Path $packageArchive
        $cachedPackageValid = $false
        if ($archiveState.Exists) {
            if (($archiveState.Attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                throw "Cached package path is not a file: $packageArchive"
            }
            $cachedHash = Get-FileSha256 -Path $packageArchive
            if ($cachedHash -ceq $expectedPackageSha256) {
                $cachedPackageValid = $true
                Write-Output "Using verified cached package: $packageArchive"
            } else {
                Write-Output 'Cached package hash mismatch; downloading a verified replacement'
            }
        }

        if (-not $cachedPackageValid) {
            $downloadTemp = Get-UniqueSiblingPath -Base $cache -Prefix 'package-download-' -Extension '.tmp'
            Copy-DownloadedPackage -Url $PackageUrl -Destination $downloadTemp -Cache $cache
            Assert-NoReparsePath -Path $downloadTemp -Anchor $cache -Description 'Downloaded package path' | Out-Null
            $downloadHash = Get-FileSha256 -Path $downloadTemp
            if ($downloadHash -cne $expectedPackageSha256) {
                throw "Downloaded package SHA-256 mismatch: expected $expectedPackageSha256, got $downloadHash"
            }

            Assert-NoReparsePath -Path $downloadTemp -Anchor $cache -Description 'Downloaded package path' | Out-Null
            Assert-NoReparsePath -Path $packageArchive -Anchor $cache -Description 'Cached package replacement path' | Out-Null
            [IO.File]::Move($downloadTemp, $packageArchive, $true)
            $downloadTemp = $null
            Assert-NoReparsePath -Path $packageArchive -Anchor $cache -Description 'Cached package archive' | Out-Null
            if ((Get-FileSha256 -Path $packageArchive) -cne $expectedPackageSha256) {
                throw "Cached package SHA-256 changed during publication: $packageArchive"
            }
            Write-Output "Cached verified package: $packageArchive"
        }

        $extractTemp = Get-UniqueSiblingPath -Base $cache -Prefix 'package-extract-' -Extension ''
        Assert-NoReparsePath -Path $extractTemp -Anchor $cache -Description 'Extraction directory' | Out-Null
        Extract-ValidatedArchive -ArchivePath $packageArchive -Destination $extractTemp `
            -RequiredEntry $requiredHeader -Cache $cache
        $headerRelativePath = $requiredHeader.Replace('/', [IO.Path]::DirectorySeparatorChar)
        $headerCandidate = Join-Path $extractTemp $headerRelativePath
        $headerPath = Assert-NoReparsePath -Path $headerCandidate -Anchor $extractTemp `
            -Description 'Required header path'
        $headerHash = Get-FileSha256 -Path $headerPath
        if ($headerHash -cne $expectedHeaderSha256) {
            throw "Extracted header SHA-256 mismatch: expected $expectedHeaderSha256, got $headerHash"
        }
        Assert-NoReparseTree -Path $extractTemp -Anchor $cache -Description 'Extracted package tree'

        $concurrentPackageValid = Test-PublishedPackage -PackageDirectory $packageDirectory -Cache $cache `
            -RequiredHeader $requiredHeader -ExpectedHeaderSha256 $expectedHeaderSha256
        if ($concurrentPackageValid) {
            Write-Output "Using concurrently published package: $packageDirectory"
        } else {
            Assert-NoReparseTree -Path $extractTemp -Anchor $cache -Description 'Package publication source'
            Assert-NoReparsePath -Path $extractTemp -Anchor $cache -Description 'Package publication source' | Out-Null
            Assert-NoReparsePath -Path $packageDirectory -Anchor $cache -Description 'Package publication target' | Out-Null
            try {
                [IO.Directory]::Move($extractTemp, $packageDirectory)
                $extractTemp = $null
                if (-not (Test-PublishedPackage -PackageDirectory $packageDirectory -Cache $cache `
                        -RequiredHeader $requiredHeader -ExpectedHeaderSha256 $expectedHeaderSha256)) {
                    throw "Published package disappeared after directory rename: $packageDirectory"
                }
                Write-Output "Published verified package: $packageDirectory"
            } catch {
                $publicationError = $_.Exception.Message
                $winnerValid = Test-PublishedPackage -PackageDirectory $packageDirectory -Cache $cache `
                    -RequiredHeader $requiredHeader -ExpectedHeaderSha256 $expectedHeaderSha256
                if (-not $winnerValid) {
                    throw "Package publication failed: $publicationError"
                }
                Write-Output "Using concurrently published package: $packageDirectory"
            }
        }
    }
} catch {
    $failureMessage = $_.Exception.Message
} finally {
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    foreach ($temporaryPath in @($downloadTemp, $extractTemp)) {
        if ($null -eq $temporaryPath -or $null -eq $cache) {
            continue
        }
        try {
            Remove-GuardedPath -Path $temporaryPath -Cache $cache
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
    Write-Error "D3DX preparation failed: $failureMessage"
    exit 1
}

Write-Output (Join-Path $packageDirectory 'build/native/include')
