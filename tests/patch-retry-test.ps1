param([string]$Bridge,[string]$Backend,[string]$OutputDirectory,[string]$LlvmMingwBin,[switch]$Observe)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$Bridge){$Bridge=Join-Path $root 'out/build/bridge/d3d9.dll'}
if(!$Backend){$Backend=Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'out/perf/20261108-upload-state/retry'}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$toolchain=Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
$compiler=$toolchain.LlvmMingw.CompilerPath
$fixture=Join-Path $OutputDirectory 'ProperShaders.asi'
$probe=Join-Path $OutputDirectory 'patch-retry-probe.exe'
& $compiler '-O2' '-static' '-shared' (Join-Path $PSScriptRoot 'patch-retry-module.cpp') '-o' $fixture
if($LASTEXITCODE){throw 'Patch fixture compilation failed'}
& $compiler '-std=c++17' '-O2' '-static' (Join-Path $PSScriptRoot 'patch-retry-probe.cpp') '-o' $probe '-luser32' '-luuid'
if($LASTEXITCODE){throw 'Patch retry probe compilation failed'}
$library=(Resolve-Path -LiteralPath $Bridge).Path
$backendDirectory=Split-Path -Parent (Resolve-Path -LiteralPath $Backend).Path
$runDirectory=Join-Path $OutputDirectory ('run-'+[guid]::NewGuid().ToString('N'))
foreach($mode in @('reject','transient','restore')){
    $directory=Join-Path $runDirectory $mode
    New-Item -ItemType Directory -Force -Path "$directory/scripts"|Out-Null
    Copy-Item -LiteralPath $probe -Destination "$directory/probe.exe"
    Copy-Item -LiteralPath $fixture -Destination "$directory/ProperShaders.asi"
    $config=@"
[Backend]
UseDxvkBackend=1
DxvkBackendDir=$backendDirectory
[ProxyChain]
Enable=0
ForceDeviceWrap=0
[PostFX]
EnableHost=0
[Measurement]
Enable=0
[ProperShadersRenderStateGuard]
Enable=0
[ProperShadersEffectOptimization]
Enable=1
GeneralStateJournal=0
StateJournal=1
[Affinity]
Enable=0
"@
    [IO.File]::WriteAllText("$directory/SA.RenderStack.ini",$config)
    $env:DXVK_LOG_PATH=$directory
    $env:DXVK_CONFIG='d3d9.presentInterval=0;dxvk.hud='
    Push-Location $directory
    try{
        & "$directory/probe.exe" $library "$directory/ProperShaders.asi" $mode 2>&1|Tee-Object "$directory/test.log"
        $code=$LASTEXITCODE
    }finally{Pop-Location}
    $log=Get-Content -LiteralPath "$directory/scripts/BridgeD3D9.log"
    $mismatches=@($log|Where-Object {$_ -match 'effectopt: patch preflight mismatch'}).Count
    "mode=$mode mismatchLines=$mismatches nativeExit=$code"
    if(!$Observe -and ($code -ne 0 -or $mismatches -ne 1)){throw "Patch retry regression failed: $mode"}
}
"Fixture retained: $runDirectory"
