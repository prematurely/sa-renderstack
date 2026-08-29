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

function Test-PublishedMeson {
    param(
        [Parameter(Mandatory)] [string]$ModuleDirectory,
        [Parameter(Mandatory)] [string]$Cache,
        [Parameter(Mandatory)] [string]$PythonPath
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
    Assert-RenderStackNoReparseTree -Path $safeModuleDirectory -Anchor $Cache `
        -Description 'Published Meson module tree' | Out-Null
    $main = Join-Path $safeModuleDirectory 'mesonbuild/mesonmain.py'
    if (-not (Test-Path -LiteralPath $main -PathType Leaf)) {
        throw "Published Meson cache is invalid: required module is missing: $main"
    }
    $result = Invoke-RenderStackProcess -FilePath $PythonPath -ArgumentList @(
        '-m', 'mesonbuild.mesonmain', '--version'
    ) -WorkingDirectory $Cache -EnvironmentOverrides @{ PYTHONPATH = $safeModuleDirectory } `
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

    $moduleDirectory = Assert-RenderStackPathUnder -Path (Join-Path $cache 'site-packages') `
        -Base $cache -Description 'Meson module directory'
    if (Test-PublishedMeson -ModuleDirectory $moduleDirectory -Cache $cache -PythonPath $pythonInfo.Path) {
        Write-Output "Using verified published Meson: $moduleDirectory"
    } else {
        $wheelPath = Assert-RenderStackPathUnder -Path (Join-Path $cache $wheelName) `
            -Base $cache -Description 'Meson wheel cache path'
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
            [IO.File]::Move($downloadTemporary, $wheelPath)
            $downloadTemporary = $null
            if ((Get-MesonFileSha256 -Path $wheelPath) -cne $expectedWheelSha256) {
                throw "Cached Meson wheel changed during publication: $wheelPath"
            }
            Write-Output "Cached verified Meson wheel: $wheelPath"
        }

        $extractTemporary = New-MesonTemporaryPath -Cache $cache -Prefix 'meson-extract-' -Extension ''
        Expand-ValidatedMesonWheel -ArchivePath $wheelPath -Destination $extractTemporary -Cache $cache
        if (-not (Test-PublishedMeson -ModuleDirectory $extractTemporary -Cache $cache -PythonPath $pythonInfo.Path)) {
            throw "Extracted Meson module tree disappeared: $extractTemporary"
        }
        if (Test-PublishedMeson -ModuleDirectory $moduleDirectory -Cache $cache -PythonPath $pythonInfo.Path) {
            Write-Output "Using concurrently published Meson: $moduleDirectory"
        } else {
            Assert-RenderStackNoReparseTree -Path $extractTemporary -Anchor $cache `
                -Description 'Meson publication source' | Out-Null
            try {
                [IO.Directory]::Move($extractTemporary, $moduleDirectory)
                $extractTemporary = $null
                if (-not (Test-PublishedMeson -ModuleDirectory $moduleDirectory -Cache $cache `
                        -PythonPath $pythonInfo.Path)) {
                    throw "Published Meson module tree disappeared: $moduleDirectory"
                }
                Write-Output "Published verified Meson: $moduleDirectory"
            } catch {
                $publishError = $_.Exception.Message
                if (-not (Test-PublishedMeson -ModuleDirectory $moduleDirectory -Cache $cache `
                        -PythonPath $pythonInfo.Path)) {
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
