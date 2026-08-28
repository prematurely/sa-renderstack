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
$replacementBackup = $null
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

function Get-UniqueSiblingPath {
    param(
        [Parameter(Mandatory)] [string]$Base,
        [Parameter(Mandatory)] [string]$Prefix,
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$Extension
    )

    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $name = "$Prefix$([Guid]::NewGuid().ToString('N'))$Extension"
        $candidate = Join-Path $Base $name
        $candidate = Assert-PathUnder -Path $candidate -Base $Base -Description 'Temporary path'
        if (-not (Test-Path -LiteralPath $candidate)) {
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

    $safePath = Assert-PathUnder -Path $Path -Base $Cache -Description 'Cleanup path'
    if (Test-Path -LiteralPath $safePath) {
        Remove-Item -LiteralPath $safePath -Recurse -Force -ErrorAction Stop
    }
}

function Copy-DownloadedPackage {
    param(
        [Parameter(Mandatory)] [string]$Url,
        [Parameter(Mandatory)] [string]$Destination
    )

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
        [Parameter(Mandatory)] [string]$RequiredEntry
    )

    $archive = $null
    try {
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

        $safeDestination = Assert-PathUnder -Path $Destination -Base $Destination -Description 'Extraction path'
        New-Item -ItemType Directory -Force -Path $safeDestination | Out-Null
        foreach ($row in $entries) {
            $relativePath = $row.Path.Replace('/', [IO.Path]::DirectorySeparatorChar)
            $targetPath = Join-Path $safeDestination $relativePath
            $targetPath = Assert-PathUnder -Path $targetPath -Base $safeDestination -Description 'ZIP extraction target'
            if ($row.IsDirectory) {
                New-Item -ItemType Directory -Force -Path $targetPath | Out-Null
                continue
            }

            $parent = Split-Path -Parent $targetPath
            Assert-PathUnder -Path $parent -Base $safeDestination -Description 'ZIP extraction parent' | Out-Null
            New-Item -ItemType Directory -Force -Path $parent | Out-Null
            $inputStream = $null
            $outputStream = $null
            try {
                $inputStream = $row.Entry.Open()
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
    } catch {
        throw "ZIP validation or extraction failed: $($_.Exception.Message)"
    } finally {
        if ($null -ne $archive) {
            $archive.Dispose()
        }
    }
}

try {
    $root = Assert-PathUnder -Path (Get-FullPath -Path (Join-Path $PSScriptRoot '..')) `
        -Base (Get-FullPath -Path (Join-Path $PSScriptRoot '..')) -Description 'Repository root'
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Repository root does not exist: $root"
    }

    $cache = Get-FullPath -Path (Join-Path $root "out/deps/$packageId/$packageVersion")
    Assert-PathUnder -Path $cache -Base $root -Description 'D3DX cache directory' | Out-Null
    $packageArchive = Assert-PathUnder -Path (Join-Path $cache 'microsoft.dxsdk.d3dx.9.29.952.8.nupkg') `
        -Base $cache -Description 'Package archive path'
    $packageDirectory = Assert-PathUnder -Path (Join-Path $cache 'package') `
        -Base $cache -Description 'Package directory'

    Assert-PathUnder -Path $cache -Base $cache -Description 'Cache directory' | Out-Null
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop

    $cachedPackageValid = $false
    if (Test-Path -LiteralPath $packageArchive) {
        if (-not (Test-Path -LiteralPath $packageArchive -PathType Leaf)) {
            throw "Cached package path is not a file: $packageArchive"
        }
        $cachedHash = Get-FileSha256 -Path $packageArchive
        if ($cachedHash -ceq $expectedPackageSha256) {
            $cachedPackageValid = $true
            Write-Output "Using verified cached package: $packageArchive"
        } else {
            Write-Output "Cached package hash mismatch; downloading a verified replacement"
        }
    }

    if (-not $cachedPackageValid) {
        $downloadTemp = Get-UniqueSiblingPath -Base $cache -Prefix 'package-download-' -Extension '.tmp'
        Copy-DownloadedPackage -Url $PackageUrl -Destination $downloadTemp
        $downloadHash = Get-FileSha256 -Path $downloadTemp
        if ($downloadHash -cne $expectedPackageSha256) {
            throw "Downloaded package SHA-256 mismatch: expected $expectedPackageSha256, got $downloadHash"
        }

        Assert-PathUnder -Path $downloadTemp -Base $cache -Description 'Downloaded package path' | Out-Null
        Assert-PathUnder -Path $packageArchive -Base $cache -Description 'Cached package replacement path' | Out-Null
        [IO.File]::Move($downloadTemp, $packageArchive, $true)
        $downloadTemp = $null
        Write-Output "Cached verified package: $packageArchive"
    }

    $extractTemp = Get-UniqueSiblingPath -Base $cache -Prefix 'package-extract-' -Extension ''
    Assert-PathUnder -Path $extractTemp -Base $cache -Description 'Extraction directory' | Out-Null
    Extract-ValidatedArchive -ArchivePath $packageArchive -Destination $extractTemp -RequiredEntry $requiredHeader
    $headerPath = Assert-PathUnder -Path (Join-Path $extractTemp ($requiredHeader.Replace('/', [IO.Path]::DirectorySeparatorChar))) `
        -Base $extractTemp -Description 'Required header path'
    $headerHash = Get-FileSha256 -Path $headerPath
    if ($headerHash -cne $expectedHeaderSha256) {
        throw "Extracted header SHA-256 mismatch: expected $expectedHeaderSha256, got $headerHash"
    }

    if (Test-Path -LiteralPath $packageDirectory) {
        if (-not (Test-Path -LiteralPath $packageDirectory -PathType Container)) {
            throw "Final package path is not a directory: $packageDirectory"
        }
        $replacementBackup = Get-UniqueSiblingPath -Base $cache -Prefix 'package-old-' -Extension ''
        Assert-PathUnder -Path $packageDirectory -Base $cache -Description 'Existing package path' | Out-Null
        Assert-PathUnder -Path $replacementBackup -Base $cache -Description 'Package backup path' | Out-Null
        [IO.Directory]::Move($packageDirectory, $replacementBackup)
    }

    Assert-PathUnder -Path $extractTemp -Base $cache -Description 'Package publication source' | Out-Null
    Assert-PathUnder -Path $packageDirectory -Base $cache -Description 'Package publication target' | Out-Null
    try {
        [IO.Directory]::Move($extractTemp, $packageDirectory)
        $extractTemp = $null
    } catch {
        if ($null -ne $replacementBackup -and (Test-Path -LiteralPath $replacementBackup) -and
            -not (Test-Path -LiteralPath $packageDirectory)) {
            try {
                Assert-PathUnder -Path $replacementBackup -Base $cache -Description 'Package rollback source' | Out-Null
                Assert-PathUnder -Path $packageDirectory -Base $cache -Description 'Package rollback target' | Out-Null
                [IO.Directory]::Move($replacementBackup, $packageDirectory)
                $replacementBackup = $null
            } catch {
                throw "Package publication failed and rollback failed: $($_.Exception.Message)"
            }
        }
        throw "Package publication failed: $($_.Exception.Message)"
    }
} catch {
    $failureMessage = $_.Exception.Message
} finally {
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    foreach ($temporaryPath in @($downloadTemp, $extractTemp, $replacementBackup)) {
        if ($null -eq $temporaryPath) {
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
