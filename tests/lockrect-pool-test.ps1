param([string]$Backend,[string]$OutputDirectory,[string]$LlvmMingwBin,
  [ValidateSet('observe','regression','correlation','correlation-allocate','bench')][string]$Mode='regression',
  [ValidateSet('upload-readback','readback')][string]$Workload='upload-readback',
  [int]$Iterations=128,[string]$Probe,[switch]$BuildOnly)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$Backend){$Backend=Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'out/tests/lockrect-pool'}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
if(!$Probe){
  . (Join-Path $root 'tools/lib/process-runner.ps1')
  . (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
  $toolchain=Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
  $Probe=Join-Path $OutputDirectory 'lockrect-pool-probe.exe'
  & $toolchain.LlvmMingw.CompilerPath '-std=c++17' '-O2' '-static' ('-I'+(Join-Path $root 'backend/dxvk/include/vulkan/include')) (Join-Path $PSScriptRoot 'lockrect-pool-probe.cpp') '-o' $Probe '-luuid' '-luser32'
  if($LASTEXITCODE){throw "LockRect probe compilation failed ($LASTEXITCODE)"}
}
if($BuildOnly){return}
$env:DXVK_LOG_PATH=$OutputDirectory
$stdout=Join-Path $OutputDirectory "$Mode-$Workload.stdout.log"
$stderr=Join-Path $OutputDirectory "$Mode-$Workload.stderr.log"
$p=Start-Process -FilePath (Resolve-Path -LiteralPath $Probe).Path -ArgumentList @(('"'+(Resolve-Path -LiteralPath $Backend).Path+'"'),$Mode,$Workload,$Iterations) -WorkingDirectory $OutputDirectory -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try{if(!$p.WaitForExit(120000)){$p.Kill();$p.WaitForExit();$result=124}else{$result=$p.ExitCode}}
finally{Get-Content -LiteralPath $stdout;Get-Content -LiteralPath $stderr;$p.Dispose()}
exit $result
