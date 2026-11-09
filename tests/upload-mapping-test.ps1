param(
    [string]$Backend,
    [string]$OutputDirectory,
    [ValidateSet('check','observe','bench')][string]$Mode='check',
    [int]$BudgetMiB=1,
    [int]$Levels=0,
    [string]$LlvmMingwBin,
    [switch]$CompileOnly
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$Backend){$Backend=Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'out/perf/20261108-upload-state/native'}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$toolchain=Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
$executable=Join-Path $OutputDirectory 'upload-mapping-probe.exe'
& $toolchain.LlvmMingw.CompilerPath '-std=c++17' '-O2' '-static' (Join-Path $PSScriptRoot 'upload-mapping-probe.cpp') '-o' $executable '-luuid' '-luser32'
if($LASTEXITCODE){throw 'Upload mapping probe compilation failed'}
if($CompileOnly){return}
$library=(Resolve-Path -LiteralPath $Backend).Path
$env:DXVK_LOG_PATH=$OutputDirectory
Push-Location $OutputDirectory
try {
    & $executable $library $Mode $BudgetMiB $Levels 2>&1|Tee-Object (Join-Path $OutputDirectory 'test.log')
    $result=$LASTEXITCODE
} finally { Pop-Location }
if($result){throw "Upload mapping fixture failed ($result)"}
