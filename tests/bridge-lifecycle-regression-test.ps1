param([string]$Backend, [string]$Bridge, [string]$Probe, [switch]$Measurement)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
if (-not $Backend) { $Backend = Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll' }
if (-not $Bridge) { $Bridge = Join-Path $root 'out/build/bridge/d3d9.dll' }
if (-not $Probe) { $Probe = Join-Path $root 'out/build/api-projects-x86/Release/bridge-lifecycle-runtime-tests.exe' }
. (Join-Path $root 'tools/lib/process-runner.ps1')
$fixture = Join-Path $root ('out/test-fixtures/bridge-lifecycle-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$inputs = @($Backend,$Bridge,$Probe) | ForEach-Object { Get-FileHash -LiteralPath $_ -Algorithm SHA256 } | Select-Object Path,Hash
$inputs | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture 'inputs.json')
$failures = [Collections.Generic.List[string]]::new()
foreach ($mode in @('backend','backend-ex','load-only','bridge-raw','bridge-wrapped')) {
    $directory = Join-Path $fixture $mode
    New-Item -ItemType Directory -Path (Join-Path $directory 'scripts') -Force | Out-Null
    $executable = Join-Path $directory 'lifecycle.exe'
    Copy-Item -LiteralPath $Probe -Destination $executable
    $wrapped = [int]($mode -eq 'bridge-wrapped')
    $pluginDir = Join-Path $root 'out/build/bridge-tests'
    $config = @"
[Backend]
UseDxvkBackend=1
DxvkBackendDir=$(Split-Path -Parent $Backend)
[ProxyChain]
Enable=0
ForceDeviceWrap=$wrapped
[PostFX]
EnableHost=$wrapped
PluginDir=$pluginDir
Plugins=BridgeLegacyPluginProbe.dll;BridgeVulkanPassProbe.dll
[Affinity]
Enable=1
Mask=0
Priority=Normal
PerThread=0
Reapply=1
ReapplyCount=60
ReapplyIntervalMs=60000
[ProperShadersRenderStateGuard]
Enable=0
[ProperShadersEffectOptimization]
Enable=0
[DXVKDiagnostics]
Enable=0
"@
    if ($Measurement) {
        $config += "`r`n[Measurement]`r`nEnable=1`r`nShowHud=0`r`nCpuHotspots=1`r`nJournalAttribution=1`r`n"
    }
    [IO.File]::WriteAllText((Join-Path $directory 'SA.RenderStack.ini'), $config)
    $library = if ($mode -like 'backend*') { $Backend } else { $Bridge }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$executable; $start.WorkingDirectory=$directory
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    [void]$start.ArgumentList.Add($library); [void]$start.ArgumentList.Add($mode)
    $start.Environment['DXVK_LOG_PATH']=$directory
    $child=[Diagnostics.Process]::Start($start)
    $stdoutTask=$child.StandardOutput.ReadToEndAsync(); $stderrTask=$child.StandardError.ReadToEndAsync()
    if (-not $child.WaitForExit(60000)) { $child.Kill($true); $child.WaitForExit(); $failures.Add("$mode timed out") }
    $stdout=$stdoutTask.GetAwaiter().GetResult(); $stderr=$stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'),$stdout)
    [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'),$stderr)
    Write-Output "$mode exit=$($child.ExitCode) $($stdout.Trim()) $($stderr.Trim())"
    if ($child.ExitCode -ne 0) { $failures.Add("$mode behavior failed") }
    $child.Dispose()
    if ($mode -eq 'bridge-raw') {
        $log=[IO.File]::ReadAllText((Join-Path $directory 'scripts/BridgeD3D9.log'))
        if ($log -notmatch 'unwrapped support worker stopped' -or $log -notmatch 'reapply worker finished' -or $log -notmatch 'explicit shutdown complete') {
            $failures.Add('raw workers were not stopped before shutdown')
        }
    }
    if ($mode -eq 'bridge-wrapped') {
        $events=@(Get-Content -LiteralPath (Join-Path $directory 'bridge_legacy_plugin_probe.log'))
        $expected=@('init-v1','create-device','release-device')
        foreach ($i in 1..3) { $expected+=@('create-device','reset-before','reset-after-failed','reset-before','reset-after-ok','release-device') }
        $expected+=@('create-device','create-device','release-device','release-device','shutdown')
        if (($events -join '|') -cne ($expected -join '|')) { $failures.Add('plugin create/reset/release/shutdown event order differs') }
        $passes=@(Get-Content -LiteralPath (Join-Path $directory 'bridge_vulkan_pass_probe.log'))
        if (@($passes | Where-Object { $_ -match '^A frame=' }).Count -ne 7) { $failures.Add('native Vulkan pass was not suspended on failure/rebound across device/Reset lifetimes') }
    }
}
Write-Output "Fixture retained: $fixture"
if ($failures.Count) { $failures | ForEach-Object { Write-Output "FAIL $_" }; exit 1 }
Write-Output 'PASS 5 lifecycle cases'
