param(
    [string]$Backend,
    [string]$Bridge,
    [string]$LlvmMingwBin,
    [ValidateSet('All', 'Backend', 'Bridge')][string]$Component = 'All'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
if (-not $Backend) { $Backend = Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll' }
if (-not $Bridge) { $Bridge = Join-Path $root 'out/build/bridge/d3d9.dll' }
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$toolchain = Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
$fixtureParent = Join-Path $root 'out/test-fixtures'
$fixtureRoot = Assert-RenderStackPathUnder -Base $fixtureParent `
    -Path (Join-Path $fixtureParent ('config-ownership-' + [guid]::NewGuid().ToString('N'))) `
    -Description 'Configuration ownership fixture'
Assert-RenderStackNoReparsePath -Path $fixtureRoot -Anchor $root `
    -Description 'Configuration ownership fixture' | Out-Null
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
Assert-RenderStackNoReparsePath -Path $fixtureRoot -Anchor $root `
    -Description 'Configuration ownership fixture' | Out-Null
$executable = Join-Path $fixtureRoot 'config-ownership-probe.exe'
& $toolchain.LlvmMingw.CompilerPath '-std=c++17' '-O2' '-static' `
    (Join-Path $PSScriptRoot 'config-ownership-probe.cpp') '-o' $executable
if ($LASTEXITCODE -ne 0) { throw "Probe compile failed: $LASTEXITCODE" }
$description = & $executable --describe | Out-String
if ($LASTEXITCODE -ne 0 -or $description -notmatch 'first_cpu=(0x[0-9A-F]+)') {
    throw "Cannot read host affinity: $description"
}
$firstCpu = $Matches[1]
$utf8 = [Text.UTF8Encoding]::new($false)
$libraries = @()
if ($Component -in @('All', 'Backend')) { $libraries += $Backend }
if ($Component -in @('All', 'Bridge')) { $libraries += @($Bridge, $Backend) }
$inputs = @($libraries | Select-Object -Unique | ForEach-Object {
    $item = Get-Item -LiteralPath $_
    [ordered]@{ path = $item.FullName; size = $item.Length;
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash;
        lastWriteTimeUtc = $item.LastWriteTimeUtc.ToString('o') }
})
[IO.File]::WriteAllText((Join-Path $fixtureRoot 'inputs.json'), ($inputs | ConvertTo-Json -Depth 3), $utf8)
$failures = [Collections.Generic.List[string]]::new()
$caseCount = 0

function Invoke-ConfigCase {
    param([string]$Name, [string]$Library, [string]$Factory = 'Direct3DCreate9',
        [AllowNull()][string]$RootConfig, [AllowNull()][string]$LegacyConfig,
        [switch]$ExplicitBridgePolicy, [switch]$CheckDirectConstants,
        [int]$RequestedDirectConstants = 0, [string]$ExpectedConfigSource)
    $directory = Join-Path $fixtureRoot $Name
    New-Item -ItemType Directory -Path (Join-Path $directory 'scripts') -Force | Out-Null
    $probe = Join-Path $directory 'config-ownership-probe.exe'
    Copy-Item -LiteralPath $executable -Destination $probe
    if ($RootConfig) { [IO.File]::WriteAllText((Join-Path $directory 'SA.RenderStack.ini'), $RootConfig, $utf8) }
    if ($LegacyConfig) { [IO.File]::WriteAllText((Join-Path $directory 'scripts/BridgeD3D9.ini'), $LegacyConfig, $utf8) }
    $arguments = @([IO.Path]::GetFullPath($Library), $Factory, 'preserve')
    if ($ExplicitBridgePolicy) { $arguments = @([IO.Path]::GetFullPath($Library), $Factory, 'bridge-policy', $firstCpu) }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $probe
    foreach ($argument in $arguments) { [void]$start.ArgumentList.Add($argument) }
    $start.WorkingDirectory = $directory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment['DXVK_LOG_PATH'] = $directory
    [void]$start.Environment.Remove('DXVK_CONFIG')
    [void]$start.Environment.Remove('DXVK_CONFIG_FILE')
    $child = [Diagnostics.Process]::Start($start)
    $stdoutTask = $child.StandardOutput.ReadToEndAsync()
    $stderrTask = $child.StandardError.ReadToEndAsync()
    if (-not $child.WaitForExit(30000)) {
        $child.Kill($true)
        $child.WaitForExit()
        $script:failures.Add("$Name timed out")
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'), $stdout, $utf8)
    [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'), $stderr, $utf8)
    $script:caseCount++
    Write-Output "CASE $Name exit=$($child.ExitCode)"
    Write-Output $stdout.TrimEnd()
    if ($child.ExitCode -ne 0) { $script:failures.Add("$Name factory behavior failed (exit $($child.ExitCode))") }
    if ($CheckDirectConstants) {
        $log = Join-Path $directory 'scripts/BridgeD3D9.log'
        $text = if (Test-Path -LiteralPath $log) { [IO.File]::ReadAllText($log) } else { '' }
        # This is the effective runtime configuration emitted by the loaded
        # Bridge, consumed by its effect hooks; never inspect source text.
        if ($text -notmatch 'effectopt: enable=1 .*directConstants=0\b') {
            $script:failures.Add("$Name DirectConstants was not disabled in the loaded Bridge")
        }
        if ($text -notmatch "directConstants: requested=$RequestedDirectConstants effective=0 reason=production-quarantine") {
            $script:failures.Add("$Name missing requested/effective quarantine diagnostic")
        }
        if ($ExpectedConfigSource -and $text.Replace('\', '/') -notmatch [regex]::Escape($ExpectedConfigSource)) {
            $script:failures.Add("$Name used the wrong configuration source")
        }
    }
    $child.Dispose()
}

if ($Component -in @('All', 'Backend')) {
    foreach ($factory in @('Direct3DCreate9', 'Direct3DCreate9Ex')) {
        Invoke-ConfigCase -Name "backend-missing-$factory" -Library $Backend -Factory $factory
        foreach ($perThread in @(0, 1)) {
            Invoke-ConfigCase -Name "backend-disabled-perthread$perThread-$factory" -Library $Backend -Factory $factory `
                -RootConfig "[Affinity]`nEnable=0`nPerThread=$perThread`nMmcss=0`n"
        }
    }
}
if ($Component -in @('All', 'Bridge')) {
    $safeConfig = "[Backend]`nUseDxvkBackend=0`n[ProxyChain]`nEnable=0`n[PostFX]`nEnableHost=0`n[DXVKDiagnostics]`nEnable=0`n[Affinity]`nEnable=0`nPerThread=0`n[ProperShadersEffectOptimization]`nEnable=1`nStateJournal=1`n"
    Invoke-ConfigCase -Name 'bridge-direct-missing' -Library $Bridge -RootConfig $safeConfig `
        -CheckDirectConstants -ExpectedConfigSource 'SA.RenderStack.ini'
    Invoke-ConfigCase -Name 'bridge-direct-root-one' -Library $Bridge -RootConfig ($safeConfig + "DirectConstants=1`n") `
        -CheckDirectConstants -RequestedDirectConstants 1 -ExpectedConfigSource 'SA.RenderStack.ini'
    Invoke-ConfigCase -Name 'bridge-direct-legacy-one' -Library $Bridge -LegacyConfig ($safeConfig + "DirectConstants=1`n") `
        -CheckDirectConstants -RequestedDirectConstants 1 -ExpectedConfigSource 'scripts/BridgeD3D9.ini'
    Invoke-ConfigCase -Name 'bridge-root-overrides-legacy' -Library $Bridge -RootConfig ($safeConfig + "DirectConstants=0`n") `
        -LegacyConfig ($safeConfig + "DirectConstants=1`n") -CheckDirectConstants -ExpectedConfigSource 'SA.RenderStack.ini'
    $explicit = $safeConfig.Replace("Enable=0`nPerThread=0", "Enable=1`nMask=$firstCpu`nPriority=Normal`nReapply=0`nPerThread=0")
    Invoke-ConfigCase -Name 'bridge-explicit-policy' -Library $Bridge -RootConfig $explicit -ExplicitBridgePolicy `
        -CheckDirectConstants -ExpectedConfigSource 'SA.RenderStack.ini'
    $dxvkPolicy = $explicit.Replace("UseDxvkBackend=0", "UseDxvkBackend=1`nDxvkBackendDir=$([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Backend)))")
    Invoke-ConfigCase -Name 'bridge-explicit-dxvk-policy' -Library $Bridge -RootConfig $dxvkPolicy -ExplicitBridgePolicy `
        -CheckDirectConstants -ExpectedConfigSource 'SA.RenderStack.ini'
}
Write-Output "Fixture retained: $fixtureRoot"
if ($failures.Count) {
    $failures | ForEach-Object { Write-Output "FAIL $_" }
    throw "$($failures.Count) failures across $caseCount configuration cases"
}
Write-Output "PASS $caseCount configuration cases"
