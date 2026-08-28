$env:GIT_CONFIG_GLOBAL = 'NUL'

$script:GitBaselineRepositoryRoot = Split-Path -Parent (
    Split-Path -Parent $PSScriptRoot)

function New-GitBaselineProcessStartInfo {
    param([Parameter(Mandatory)] [string[]]$Arguments)

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = 'git'
    $startInfo.WorkingDirectory = $script:GitBaselineRepositoryRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Environment['GIT_CONFIG_GLOBAL'] = 'NUL'
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }
    return $startInfo
}

function Invoke-GitBaselineText {
    param(
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [string]$Operation
    )

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = New-GitBaselineProcessStartInfo -Arguments $Arguments
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

function Assert-GitFullHistory {
    $shallowText = Invoke-GitBaselineText `
        -Arguments @('rev-parse', '--is-shallow-repository') `
        -Operation 'checking whether the repository is shallow'
    $shallowValue = $shallowText.TrimEnd([char[]]"`r`n")
    if ($shallowValue -cne 'false') {
        throw "Historical baselines require a full-history checkout; future CI checkout must use fetch-depth: 0. Git reported is-shallow-repository='$shallowValue'"
    }
}

function Assert-GitCommit {
    param(
        [Parameter(Mandatory)] [string]$Commit,
        [Parameter(Mandatory)] [string]$Label
    )

    if ($Commit -cnotmatch '\A[0-9a-fA-F]{40}\z') {
        throw "$Label must use a literal full 40-character Git commit: '$Commit'"
    }
    try {
        $null = Invoke-GitBaselineText `
            -Arguments @('cat-file', '-e', "$Commit`^{commit}") `
            -Operation "preflighting $Label commit $Commit"
    } catch {
        throw "Git commit '$Commit' for $Label is unavailable. A full-history checkout is required; configure future CI checkout with fetch-depth: 0. $($_.Exception.Message)"
    }
}

function Read-GitBlob {
    param(
        [Parameter(Mandatory)] [string]$Commit,
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Label
    )

    if ($Commit -cnotmatch '\A[0-9a-fA-F]{40}\z') {
        throw "$Label must use a literal full 40-character Git commit: '$Commit'"
    }
    if ([string]::IsNullOrWhiteSpace($Path) -or
        [IO.Path]::IsPathRooted($Path) -or
        $Path.Contains('\') -or
        @($Path.Split('/')) -contains '..') {
        throw "Unsafe historical Git blob path for ${Label}: '$Path'"
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = New-GitBaselineProcessStartInfo `
        -Arguments @('cat-file', 'blob', "$Commit`:$Path")
    $memory = [IO.MemoryStream]::new()
    try {
        if (-not $process.Start()) {
            throw "Git failed to start while reading $Label"
        }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.StandardOutput.BaseStream.CopyTo($memory)
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($exitCode -ne 0) {
            $detail = $stderr.TrimEnd([char[]]"`r`n")
            throw "Git failed while reading $Label from $Commit`:$Path (exit $exitCode): $detail"
        }
        return ,$memory.ToArray()
    } finally {
        $memory.Dispose()
        $process.Dispose()
    }
}

function Assert-GitTrackedPathsClean {
    param([Parameter(Mandatory)] [string[]]$Paths)

    if ($Paths.Count -eq 0) {
        throw 'At least one tracked path is required for the clean-diff check'
    }
    try {
        $null = Invoke-GitBaselineText `
            -Arguments (@('diff', '--quiet', '--') + $Paths) `
            -Operation 'checking tracked baseline paths for changes'
    } catch {
        throw "Tracked baseline paths are dirty: $($Paths -join ', '). $($_.Exception.Message)"
    }
}
