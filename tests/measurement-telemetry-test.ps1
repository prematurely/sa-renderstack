param([string]$Backend, [string]$OutputDirectory, [string]$LlvmMingwBin,
  [switch]$CompileOnly, [switch]$Benchmark, [switch]$DisableCompat, [switch]$PresentOnly, [ValidateSet('off','on','aa')][string]$Collection='off')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if (-not $Backend) { $Backend=Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll' }
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $root 'out/perf/20261112-measurement-coverage/backend' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$toolchain=Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
$exe=Join-Path $OutputDirectory 'measurement-telemetry-probe.exe'
& $toolchain.LlvmMingw.CompilerPath '-std=c++17' '-O2' '-static' (Join-Path $PSScriptRoot 'measurement-telemetry-probe.cpp') (Join-Path $root 'backend/dxvk/src/dxvk/dxvk_telemetry.cpp') '-I' (Join-Path $root 'backend/dxvk/include/vulkan/include') '-o' $exe '-luuid' '-luser32' '-lgdi32'
if ($LASTEXITCODE -ne 0) { throw "Probe compile failed: $LASTEXITCODE" }
if ($CompileOnly) { exit 0 }
$backendPath=(Resolve-Path -LiteralPath $Backend).Path
$enabled=if ($DisableCompat) { 'False' } else { 'True' }
$env:DXVK_CONFIG="d3d9.gtaSaCompat = $enabled; d3d9.gtaSaCompatDiagnostics = False; dxvk.hud = 0"
if (-not $Benchmark) { $env:DXVK_CONFIG+='; d3d9.maxFrameRate = 30; d3d9.maxFrameLatency = 1' }
$env:DXVK_HUD='0'
$env:DXVK_LOG_PATH=$OutputDirectory
$tag=if ($Benchmark) { "benchmark-$Collection" } elseif ($DisableCompat) { 'no-compat' } else { 'compat' }
$stdout=Join-Path $OutputDirectory "probe-$tag-$PID.stdout.log"
$stderr=Join-Path $OutputDirectory "probe-$tag-$PID.stderr.log"
$arguments=@("`"$backendPath`"")
if ($Benchmark) { $arguments+=@('--benchmark',$Collection) } elseif ($PresentOnly) { $arguments+=@('--present') }
$process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $OutputDirectory `
  -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try {
  if (-not $process.WaitForExit(60000)) {
    $process.Kill(); $process.WaitForExit()
    Write-Output "FAIL probe watchdog exceeded 60000 ms; stdout=$stdout stderr=$stderr"
    $result=124
  } else { $result=$process.ExitCode }
} finally {
  if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout }
  if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr }
  $process.Dispose()
}
exit $result
