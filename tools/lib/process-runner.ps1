$env:GIT_CONFIG_GLOBAL = 'NUL'

function Get-RenderStackFullPath {
    param([Parameter(Mandatory)] [string]$Path)

    return [IO.Path]::GetFullPath($Path)
}

function Assert-RenderStackPathUnder {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Base,
        [Parameter(Mandatory)] [string]$Description
    )

    $fullPath = Get-RenderStackFullPath -Path $Path
    $fullBase = (Get-RenderStackFullPath -Path $Base).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $prefix = $fullBase + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.Equals($fullBase, [StringComparison]::OrdinalIgnoreCase) -and
        -not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description is outside its permitted directory: $fullPath"
    }
    return $fullPath
}

function Get-RenderStackPathState {
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

function Assert-RenderStackNoReparsePath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Anchor,
        [Parameter(Mandatory)] [string]$Description
    )

    $fullPath = Assert-RenderStackPathUnder -Path $Path -Base $Anchor -Description $Description
    $fullAnchor = Get-RenderStackFullPath -Path $Anchor
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
        $state = Get-RenderStackPathState -Path $component
        if (-not $state.Exists) {
            break
        }
        if (($state.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description contains a reparse point: $component"
        }
    }
    return $fullPath
}

function Assert-RenderStackNoReparseTree {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Anchor,
        [Parameter(Mandatory)] [string]$Description
    )

    $rootPath = Assert-RenderStackNoReparsePath -Path $Path -Anchor $Anchor -Description $Description
    $state = Get-RenderStackPathState -Path $rootPath
    if (-not $state.Exists) {
        throw "$Description does not exist: $rootPath"
    }
    if (($state.Attributes -band [IO.FileAttributes]::Directory) -eq 0) {
        return $rootPath
    }

    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($rootPath)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($directory)) {
            $entryPath = Assert-RenderStackPathUnder -Path $entry -Base $rootPath -Description $Description
            $attributes = [IO.File]::GetAttributes($entryPath)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description contains a reparse point: $entryPath"
            }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                $pending.Push($entryPath)
            }
        }
    }
    return $rootPath
}

function Get-RenderStackRepositoryRoot {
    param([Parameter(Mandatory)] [string]$CallerScriptPath)

    $candidate = if (Test-Path -LiteralPath $CallerScriptPath -PathType Leaf) {
        Split-Path -Parent (Get-RenderStackFullPath -Path $CallerScriptPath)
    } else {
        Get-RenderStackFullPath -Path $CallerScriptPath
    }

    while ($true) {
        $version = Join-Path $candidate 'VERSION'
        $dxvk = Join-Path $candidate 'backend/dxvk'
        $bridge = Join-Path $candidate 'src/bridge/legacy'
        if ((Test-Path -LiteralPath $version -PathType Leaf) -and
            (Test-Path -LiteralPath $dxvk -PathType Container) -and
            (Test-Path -LiteralPath $bridge -PathType Container)) {
            Assert-RenderStackNoReparsePath -Path $candidate -Anchor $candidate `
                -Description 'Repository root' | Out-Null
            return $candidate
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrEmpty($parent) -or $parent -ceq $candidate) {
            break
        }
        $candidate = $parent
    }

    throw "Unable to resolve an SA RenderStack repository root from caller: $CallerScriptPath"
}

function New-RenderStackArtifactName {
    param(
        [Parameter(Mandatory)] [string]$Prefix,
        [Parameter(Mandatory)] [string]$Extension
    )

    $timestamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ', [Globalization.CultureInfo]::InvariantCulture)
    $suffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    return "$Prefix-$timestamp-$PID-$suffix$Extension"
}

function Format-RenderStackCommandArgument {
    param([Parameter(Mandatory)] [AllowEmptyString()] [string]$Argument)

    if ($Argument -notmatch '[\s"]') {
        return $Argument
    }
    return '"' + $Argument.Replace('"', '\"') + '"'
}

function Add-RenderStackLogText {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [AllowEmptyString()] [string]$Text
    )

    $directory = Split-Path -Parent (Get-RenderStackFullPath -Path $Path)
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [IO.File]::AppendAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Invoke-RenderStackProcess {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [hashtable]$EnvironmentOverrides = @{},
        [string[]]$EnvironmentRemovals = @(),
        [string]$CombinedLogPath,
        [string]$StandardOutputLogPath,
        [string]$StandardErrorLogPath,
        [string]$Label = 'process',
        [switch]$EchoOutput
    )

    $resolvedExecutable = Get-RenderStackFullPath -Path $FilePath
    if (-not (Test-Path -LiteralPath $resolvedExecutable -PathType Leaf)) {
        throw "Executable does not exist: $resolvedExecutable"
    }
    $resolvedWorkingDirectory = Get-RenderStackFullPath -Path $WorkingDirectory
    if (-not (Test-Path -LiteralPath $resolvedWorkingDirectory -PathType Container)) {
        throw "Working directory does not exist: $resolvedWorkingDirectory"
    }

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $resolvedExecutable
    $startInfo.WorkingDirectory = $resolvedWorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    foreach ($argument in $ArgumentList) {
        [void]$startInfo.ArgumentList.Add([string]$argument)
    }
    foreach ($name in $EnvironmentRemovals) {
        [void]$startInfo.Environment.Remove($name)
    }
    foreach ($entry in $EnvironmentOverrides.GetEnumerator()) {
        $startInfo.Environment[[string]$entry.Key] = [string]$entry.Value
    }
    $startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'

    $command = (@($resolvedExecutable) + @($ArgumentList | ForEach-Object {
        Format-RenderStackCommandArgument -Argument ([string]$_)
    })) -join ' '
    $startUtc = [DateTime]::UtcNow
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw "Failed to start process: $resolvedExecutable"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $exitCode = $process.ExitCode
    } finally {
        $stopwatch.Stop()
        $process.Dispose()
    }
    $endUtc = [DateTime]::UtcNow

    $header = @(
        "[$Label] startUtc=$($startUtc.ToString('o'))",
        "[$Label] workingDirectory=$resolvedWorkingDirectory",
        "[$Label] command=$command"
    ) -join [Environment]::NewLine
    $combined = $header + [Environment]::NewLine + $stdout
    if (-not [string]::IsNullOrEmpty($stderr)) {
        $combined += "[stderr]" + [Environment]::NewLine + $stderr
    }
    $combined += "[$Label] exitCode=$exitCode endUtc=$($endUtc.ToString('o')) durationMilliseconds=$($stopwatch.Elapsed.TotalMilliseconds.ToString('F3', [Globalization.CultureInfo]::InvariantCulture))" + [Environment]::NewLine

    if (-not [string]::IsNullOrWhiteSpace($CombinedLogPath)) {
        Add-RenderStackLogText -Path $CombinedLogPath -Text $combined
    }
    if (-not [string]::IsNullOrWhiteSpace($StandardOutputLogPath)) {
        Add-RenderStackLogText -Path $StandardOutputLogPath -Text $stdout
    }
    if (-not [string]::IsNullOrWhiteSpace($StandardErrorLogPath)) {
        Add-RenderStackLogText -Path $StandardErrorLogPath -Text $stderr
    }
    if ($EchoOutput) {
        if (-not [string]::IsNullOrEmpty($stdout)) {
            [Console]::Out.Write($stdout)
        }
        if (-not [string]::IsNullOrEmpty($stderr)) {
            [Console]::Error.Write($stderr)
        }
    }

    return [pscustomobject]@{
        Label = $Label
        FilePath = $resolvedExecutable
        Arguments = @($ArgumentList)
        Command = $command
        WorkingDirectory = $resolvedWorkingDirectory
        StandardOutput = $stdout
        StandardError = $stderr
        CombinedOutput = $combined
        StartUtc = $startUtc.ToString('o')
        EndUtc = $endUtc.ToString('o')
        DurationMilliseconds = [Math]::Round($stopwatch.Elapsed.TotalMilliseconds, 3)
        ExitCode = $exitCode
    }
}

function Remove-RenderStackBuildPath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$BuildRoot,
        [Parameter(Mandatory)] [string[]]$AllowedName
    )

    $fullBuildRoot = Get-RenderStackFullPath -Path $BuildRoot
    $fullPath = Assert-RenderStackPathUnder -Path $Path -Base $fullBuildRoot -Description 'Build clean path'
    $relative = [IO.Path]::GetRelativePath($fullBuildRoot, $fullPath)
    if ($relative.Contains([IO.Path]::DirectorySeparatorChar) -or
        $relative.Contains([IO.Path]::AltDirectorySeparatorChar) -or
        $relative -eq '.' -or $relative -notin $AllowedName) {
        throw "Build clean path is not an allowlisted direct child: $fullPath"
    }
    $state = Get-RenderStackPathState -Path $fullPath
    if (-not $state.Exists) {
        return
    }
    Assert-RenderStackNoReparseTree -Path $fullPath -Anchor $fullBuildRoot `
        -Description 'Build clean tree' | Out-Null
    Remove-Item -LiteralPath $fullPath -Recurse -Force -ErrorAction Stop
}

function Get-RenderStackPeMetadata {
    param([Parameter(Mandatory)] [string]$Path)

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "PE file is missing the DOS signature: $Path"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -gt $stream.Length - 24) {
            throw "PE header offset is invalid: $Path"
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "PE file is missing the NT signature: $Path"
        }
        $machine = $reader.ReadUInt16()
        $stream.Position = $peOffset + 24
        $optionalMagic = $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }

    $machineName = switch ($machine) {
        0x014C { 'IMAGE_FILE_MACHINE_I386' }
        0x8664 { 'IMAGE_FILE_MACHINE_AMD64' }
        default { ('UNKNOWN_0x{0:X4}' -f $machine) }
    }
    $format = switch ($optionalMagic) {
        0x010B { 'PE32' }
        0x020B { 'PE32+' }
        default { ('UNKNOWN_0x{0:X4}' -f $optionalMagic) }
    }
    return [pscustomobject]@{
        Machine = $machineName
        MachineValue = ('0x{0:X4}' -f $machine)
        Format = $format
    }
}

function Write-RenderStackAtomicJson {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [object]$Value,
        [int]$Depth = 12
    )

    $fullPath = Get-RenderStackFullPath -Path $Path
    $directory = Split-Path -Parent $fullPath
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $temporary = Join-Path $directory (New-RenderStackArtifactName -Prefix 'json-publish' -Extension '.tmp')
    try {
        $json = $Value | ConvertTo-Json -Depth $Depth
        [IO.File]::WriteAllText($temporary, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $fullPath, $true)
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            [IO.File]::Delete($temporary)
        }
    }
}
