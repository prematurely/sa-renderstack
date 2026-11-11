param([string]$Backend,[string]$OutputDirectory,[string]$LlvmMingwBin,[switch]$DisableCompat,[switch]$CompileOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$Backend) { $Backend = Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'out/perf/20261109-sampler-batch/native' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$llvm = Find-RenderStackLlvmMingw -RepoRoot $root -LlvmMingwBin $LlvmMingwBin
$exe = Join-Path $OutputDirectory 'sampler-batch-probe.exe'
& $llvm.CompilerPath '-std=c++17' '-O2' '-static' (Join-Path $PSScriptRoot 'sampler-batch-probe.cpp') '-I' (Join-Path $root 'backend/dxvk/include/vulkan/include') '-o' $exe '-luuid' '-luser32'
if ($LASTEXITCODE) { throw 'sampler/batch probe compile failed' }
if ($CompileOnly) { return }
$env:DXVK_CONFIG = "d3d9.gtaSaCompat = $(!$DisableCompat); d3d9.gtaSaStateBlockPrefilter = True; d3d9.gtaSaCompatDiagnostics = False; dxvk.hud = 0"
$env:DXVK_HUD = ''; $env:DXVK_LOG_PATH = $OutputDirectory
$backendPath = (Resolve-Path -LiteralPath $Backend).Path
Push-Location $OutputDirectory
try {
    foreach ($protected in 0,1) {
        & $exe check $backendPath $protected ([int](!$DisableCompat)) > "check-$protected.log" 2>&1
        $code = $LASTEXITCODE
        Get-Content -LiteralPath "check-$protected.log" -Tail 8
        if ($code) { throw "sampler/batch behavior failed: protected=$protected exit=$code" }
    }
} finally { Pop-Location }
