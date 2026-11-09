param([string]$Backend, [string]$OutputDirectory, [switch]$CompileOnly, [switch]$DisableCompat, [switch]$RenderHud, [switch]$LifecycleLocks,
    [string]$LlvmMingwBin, [string]$HudItems = 'renderstack,fps,frametimes',
    [switch]$FullHud, [int]$Width = 640, [int]$Height = 480)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if (-not $Backend) { $Backend=Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll' }
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $root 'out/perf/20261027-unified-measurement/backend' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$toolchain=Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
$exe=Join-Path $OutputDirectory 'measurement-backend-probe.exe'
& $toolchain.LlvmMingw.CompilerPath '-std=c++17' '-O2' '-static' (Join-Path $PSScriptRoot 'measurement-backend-probe.cpp') '-I' (Join-Path $root 'backend/dxvk/include/vulkan/include') '-o' $exe '-luuid' '-luser32' '-lgdi32'
if ($LASTEXITCODE -ne 0) { throw "Probe compile failed: $LASTEXITCODE" }
if ($CompileOnly) { exit 0 }
$backendPath=(Resolve-Path -LiteralPath $Backend).Path
$enabled=if ($DisableCompat) { 'False' } else { 'True' }
$env:DXVK_CONFIG="d3d9.gtaSaCompat = $enabled; d3d9.gtaSaCompatDiagnostics = False; d3d9.gtaSaResourceBindingCache = True; dxvk.hud = $HudItems"
$env:DXVK_HUD=$HudItems
$env:DXVK_LOG_PATH=$OutputDirectory
Push-Location $OutputDirectory
try {
  if ($DisableCompat) { & $exe $backendPath --disabled }
  elseif ($FullHud) { & $exe $backendPath --render-full $Width $Height }
  elseif ($RenderHud) { & $exe $backendPath --render $Width $Height }
  elseif ($LifecycleLocks) { & $exe $backendPath --lifecycle-locks }
  else { & $exe $backendPath }
  $result=$LASTEXITCODE
} finally { Pop-Location }
exit $result
