param([string]$Backend,[string]$OutputDirectory,[string]$LlvmMingwBin,[switch]$DisableCompat,[switch]$CompileOnly,[string]$Mode='check')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$Backend){$Backend=Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'out/perf/20261031-thread-journal/native'}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$tc=Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
$exe=Join-Path $OutputDirectory 'thread-journal-probe.exe'
& $tc.LlvmMingw.CompilerPath '-std=c++17' '-O2' '-static' (Join-Path $PSScriptRoot 'thread-journal-probe.cpp') '-I' (Join-Path $root 'backend/dxvk/include/vulkan/include') '-o' $exe '-luuid' '-luser32'
if($LASTEXITCODE){throw 'thread/journal probe compile failed'}
if($CompileOnly){return}
$env:DXVK_CONFIG="d3d9.gtaSaCompat = $(!$DisableCompat); d3d9.gtaSaStateBlockPrefilter = True; d3d9.gtaSaCompatDiagnostics = False; dxvk.hud = 0"
$env:DXVK_HUD='';$env:DXVK_LOG_PATH=$OutputDirectory
$backendPath=(Resolve-Path -LiteralPath $Backend).Path
Push-Location $OutputDirectory
try{& $exe $backendPath $Mode 2>&1|Tee-Object (Join-Path $OutputDirectory 'test.log');$code=$LASTEXITCODE}finally{Pop-Location}
if($code){throw "thread/journal fixture failed: $code"}
