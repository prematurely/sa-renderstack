$env:GIT_CONFIG_GLOBAL = 'NUL'

function Resolve-RenderStackExecutable {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Description
    )

    $fullPath = Get-RenderStackFullPath -Path $Path
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "$Description does not exist: $fullPath"
    }
    return $fullPath
}

function Get-RenderStackOverride {
    param(
        [string]$ParameterValue,
        [Parameter(Mandatory)] [string]$EnvironmentName
    )

    if (-not [string]::IsNullOrWhiteSpace($ParameterValue)) {
        return [pscustomobject]@{ Value = $ParameterValue; Source = 'parameter' }
    }
    $environmentValue = [Environment]::GetEnvironmentVariable($EnvironmentName, 'Process')
    if (-not [string]::IsNullOrWhiteSpace($environmentValue)) {
        return [pscustomobject]@{ Value = $environmentValue; Source = "environment:$EnvironmentName" }
    }
    return $null
}

function Invoke-RenderStackVersionProbe {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [string[]]$ArgumentList,
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [Parameter(Mandatory)] [string]$Description,
        [hashtable]$EnvironmentOverrides = @{}
    )

    $result = Invoke-RenderStackProcess -FilePath $FilePath -ArgumentList $ArgumentList `
        -WorkingDirectory $WorkingDirectory -EnvironmentOverrides $EnvironmentOverrides `
        -Label "$Description-version"
    if ($result.ExitCode -ne 0) {
        throw "$Description version probe failed with exit $($result.ExitCode): $($result.StandardError)"
    }
    $output = ($result.StandardOutput + $result.StandardError).Trim()
    if ([string]::IsNullOrWhiteSpace($output)) {
        throw "$Description version probe returned no output"
    }
    return $output
}

function Get-RenderStackPythonInfo {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Source,
        [Parameter(Mandatory)] [string]$WorkingDirectory
    )

    $python = Resolve-RenderStackExecutable -Path $Path -Description 'Python executable'
    $probe = Invoke-RenderStackVersionProbe -FilePath $python -ArgumentList @(
        '-c', 'import json,sys; print(json.dumps({"version":"%d.%d.%d" % sys.version_info[:3],"major":sys.version_info[0],"minor":sys.version_info[1]}))'
    ) -WorkingDirectory $WorkingDirectory -Description 'Python'
    try {
        $data = $probe | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "Python version probe returned invalid JSON: $probe"
    }
    if ([int]$data.major -ne 3 -or [int]$data.minor -lt 8 -or [int]$data.minor -gt 13) {
        throw "Python must be version 3.8 through 3.13; found $($data.version) at $python"
    }
    return [pscustomobject]@{
        Path = $python
        Version = [string]$data.version
        Source = $Source
    }
}

function Find-RenderStackPython {
    param(
        [string]$PythonPath,
        [Parameter(Mandatory)] [string]$RepoRoot
    )

    $override = Get-RenderStackOverride -ParameterValue $PythonPath -EnvironmentName 'SA_RENDERSTACK_PYTHON'
    if ($null -ne $override) {
        return Get-RenderStackPythonInfo -Path $override.Value -Source $override.Source -WorkingDirectory $RepoRoot
    }

    $launcher = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($null -ne $launcher) {
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $launcher.Source
        $startInfo.WorkingDirectory = $RepoRoot
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.CreateNoWindow = $true
        [void]$startInfo.ArgumentList.Add('-0p')
        $startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        try {
            if ($process.Start()) {
                $stdoutTask = $process.StandardOutput.ReadToEndAsync()
                $stderrTask = $process.StandardError.ReadToEndAsync()
                $process.WaitForExit()
                $launcherOutput = $stdoutTask.GetAwaiter().GetResult() + $stderrTask.GetAwaiter().GetResult()
                if ($process.ExitCode -eq 0) {
                    $paths = @($launcherOutput -split '\r?\n' | ForEach-Object {
                        if ($_ -match '(-V:)?3\.(?<minor>\d+).*?\s+(?<path>[A-Za-z]:\\.+?python(?:\.exe)?)\s*$') {
                            [pscustomobject]@{ Minor = [int]$Matches.minor; Path = $Matches.path.Trim() }
                        }
                    } | Where-Object { $null -ne $_ -and $_.Minor -ge 8 -and $_.Minor -le 13 } |
                        Sort-Object Minor -Descending)
                    foreach ($candidate in $paths) {
                        try {
                            return Get-RenderStackPythonInfo -Path $candidate.Path `
                                -Source 'py-launcher' -WorkingDirectory $RepoRoot
                        } catch {
                        }
                    }
                }
            }
        } finally {
            $process.Dispose()
        }
    }

    $runtimeRoot = Join-Path $env:USERPROFILE '.cache/codex-runtimes'
    if (Test-Path -LiteralPath $runtimeRoot -PathType Container) {
        $candidates = @(Get-ChildItem -LiteralPath $runtimeRoot -Filter 'python.exe' -File -Recurse `
            -ErrorAction SilentlyContinue | Where-Object {
                $_.FullName -notmatch '[\\/]Lib[\\/]venv[\\/]'
            } | Sort-Object LastWriteTimeUtc -Descending)
        foreach ($candidate in $candidates) {
            try {
                return Get-RenderStackPythonInfo -Path $candidate.FullName `
                    -Source 'codex-runtime-fallback' -WorkingDirectory $RepoRoot
            } catch {
            }
        }
    }
    throw 'Compatible Python 3.8 through 3.13 was not found'
}

function Find-RenderStackMSBuild {
    param(
        [string]$MsBuildPath,
        [Parameter(Mandatory)] [string]$RepoRoot
    )

    $override = Get-RenderStackOverride -ParameterValue $MsBuildPath -EnvironmentName 'SA_RENDERSTACK_MSBUILD'
    $source = $null
    if ($null -ne $override) {
        $path = Resolve-RenderStackExecutable -Path $override.Value -Description 'MSBuild executable'
        $source = $override.Source
    } else {
        $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            throw "vswhere is required to discover Visual Studio 18 BuildTools: $vswhere"
        }
        $result = Invoke-RenderStackProcess -FilePath $vswhere -ArgumentList @(
            '-latest', '-products', 'Microsoft.VisualStudio.Product.BuildTools',
            '-version', '[18.0,19.0)', '-requires', 'Microsoft.Component.MSBuild',
            '-property', 'installationPath'
        ) -WorkingDirectory $RepoRoot -Label 'vswhere'
        if ($result.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($result.StandardOutput)) {
            throw 'Visual Studio 18 BuildTools with MSBuild was not found'
        }
        $installation = ($result.StandardOutput -split '\r?\n' | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_)
        } | Select-Object -First 1).Trim()
        $path = Resolve-RenderStackExecutable `
            -Path (Join-Path $installation 'MSBuild/Current/Bin/amd64/MSBuild.exe') `
            -Description 'HostX64 MSBuild executable'
        $source = 'vswhere-vs18-buildtools'
    }
    $versionOutput = Invoke-RenderStackVersionProbe -FilePath $path -ArgumentList @('-version', '-nologo') `
        -WorkingDirectory $RepoRoot -Description 'MSBuild'
    $version = ($versionOutput -split '\r?\n' | Where-Object { $_ -match '^\d+\.\d+' } | Select-Object -Last 1).Trim()
    return [pscustomobject]@{ Path = $path; Version = $version; Source = $source; Host = 'HostX64' }
}

function Find-RenderStackLlvmMingw {
    param(
        [string]$LlvmMingwBin,
        [Parameter(Mandatory)] [string]$RepoRoot
    )

    $override = Get-RenderStackOverride -ParameterValue $LlvmMingwBin -EnvironmentName 'SA_RENDERSTACK_LLVM_MINGW_BIN'
    $source = $null
    if ($null -ne $override) {
        $bin = Get-RenderStackFullPath -Path $override.Value
        $source = $override.Source
    } else {
        $workspaceRoot = Split-Path -Parent (Split-Path -Parent $RepoRoot)
        $localTools = Join-Path $workspaceRoot '.codex-tools'
        $candidate = @(Get-ChildItem -LiteralPath $localTools -Directory -Filter 'llvm-mingw*' `
            -ErrorAction SilentlyContinue | Sort-Object Name -Descending | ForEach-Object {
                Join-Path $_.FullName 'bin'
            } | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Select-Object -First 1)
        if ($candidate.Count -eq 0) {
            throw 'LLVM-MinGW bin directory was not found'
        }
        $bin = Get-RenderStackFullPath -Path $candidate[0]
        $source = 'local-workspace-fallback'
    }
    if (-not (Test-Path -LiteralPath $bin -PathType Container)) {
        throw "LLVM-MinGW bin directory does not exist: $bin"
    }

    $crossFile = Join-Path $RepoRoot 'toolchains/llvm-mingw-i686.ini'
    if (-not (Test-Path -LiteralPath $crossFile -PathType Leaf)) {
        throw "LLVM-MinGW cross file is missing: $crossFile"
    }
    $requiredNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $inBinariesSection = $false
    foreach ($line in Get-Content -LiteralPath $crossFile) {
        if ($line -match '^\s*\[(?<section>[^]]+)\]\s*$') {
            $inBinariesSection = $Matches.section -ceq 'binaries'
            continue
        }
        if ($inBinariesSection -and $line -match '^\s*\w+\s*=\s*''(?<name>[^'']+)''\s*$') {
            $name = [IO.Path]::GetFileName($Matches.name)
            if (-not $name.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) {
                $name += '.exe'
            }
            [void]$requiredNames.Add($name)
        }
    }
    [void]$requiredNames.Add('llvm-readobj.exe')
    $missing = @($requiredNames | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $bin $_) -PathType Leaf)
    })
    if ($missing.Count -ne 0) {
        throw "LLVM-MinGW bin is incomplete; missing: $($missing -join ', ')"
    }
    $compiler = Join-Path $bin 'i686-w64-mingw32-clang++.exe'
    $versionOutput = Invoke-RenderStackVersionProbe -FilePath $compiler -ArgumentList @('--version') `
        -WorkingDirectory $RepoRoot -Description 'LLVM-MinGW'
    $version = if ($versionOutput -match 'clang version\s+([^\s]+)') { $Matches[1] } else { $versionOutput.Split("`n")[0].Trim() }
    return [pscustomobject]@{
        BinPath = $bin
        CompilerPath = $compiler
        ReadObjPath = (Join-Path $bin 'llvm-readobj.exe')
        Version = $version
        Source = $source
        RequiredExecutables = @($requiredNames | Sort-Object)
    }
}

function Find-RenderStackSimpleTool {
    param(
        [string]$ParameterValue,
        [Parameter(Mandatory)] [string]$EnvironmentName,
        [Parameter(Mandatory)] [string]$Description,
        [Parameter(Mandatory)] [string[]]$KnownPaths,
        [Parameter(Mandatory)] [string[]]$VersionArguments,
        [Parameter(Mandatory)] [string]$RepoRoot
    )

    $override = Get-RenderStackOverride -ParameterValue $ParameterValue -EnvironmentName $EnvironmentName
    if ($null -ne $override) {
        $path = Resolve-RenderStackExecutable -Path $override.Value -Description "$Description executable"
        $source = $override.Source
    } else {
        $path = $null
        foreach ($knownPath in $KnownPaths) {
            if (Test-Path -LiteralPath $knownPath -PathType Leaf) {
                $path = Get-RenderStackFullPath -Path $knownPath
                break
            }
        }
        if ($null -eq $path) {
            throw "$Description executable was not found in known deterministic locations"
        }
        $source = 'known-path'
    }
    $versionOutput = Invoke-RenderStackVersionProbe -FilePath $path -ArgumentList $VersionArguments `
        -WorkingDirectory $RepoRoot -Description $Description
    $version = ($versionOutput -split '\r?\n' | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    } | Select-Object -First 1).Trim()
    return [pscustomobject]@{ Path = $path; Version = $version; Source = $source }
}

function Get-RenderStackToolchain {
    param(
        [Parameter(Mandatory)] [string]$RepoRoot,
        [ValidateSet('All', 'Bridge', 'Dxvk')] [string]$Component = 'All',
        [string]$MsBuildPath,
        [string]$PythonPath,
        [string]$LlvmMingwBin,
        [string]$NinjaPath,
        [string]$GlslangPath
    )

    $root = Get-RenderStackFullPath -Path $RepoRoot
    $needBridge = $Component -in @('All', 'Bridge')
    $needDxvk = $Component -in @('All', 'Dxvk')
    $msbuild = if ($needBridge) { Find-RenderStackMSBuild -MsBuildPath $MsBuildPath -RepoRoot $root } else { $null }
    $python = if ($needDxvk) { Find-RenderStackPython -PythonPath $PythonPath -RepoRoot $root } else { $null }
    $llvm = if ($needDxvk) { Find-RenderStackLlvmMingw -LlvmMingwBin $LlvmMingwBin -RepoRoot $root } else { $null }
    $ninja = if ($needDxvk) {
        Find-RenderStackSimpleTool -ParameterValue $NinjaPath -EnvironmentName 'SA_RENDERSTACK_NINJA' `
            -Description 'Ninja' -KnownPaths @('C:\msys64\mingw32\bin\ninja.exe', 'C:\msys64\mingw64\bin\ninja.exe') `
            -VersionArguments @('--version') -RepoRoot $root
    } else { $null }
    $glslang = if ($needDxvk) {
        Find-RenderStackSimpleTool -ParameterValue $GlslangPath -EnvironmentName 'SA_RENDERSTACK_GLSLANG' `
            -Description 'glslang' -KnownPaths @(
                'C:\msys64\mingw32\bin\glslangValidator.exe',
                'C:\msys64\mingw64\bin\glslangValidator.exe'
            ) -VersionArguments @('--version') -RepoRoot $root
    } else { $null }

    return [pscustomobject]@{
        Component = $Component
        MSBuild = $msbuild
        Python = $python
        LlvmMingw = $llvm
        Ninja = $ninja
        Glslang = $glslang
    }
}
