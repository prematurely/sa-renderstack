param([string]$Backend, [string]$OutputDirectory, [string]$LlvmMingwBin,
  [ValidateSet('regression','initializer','bench','lru','allocation-failure','row-stride','fresh-zero','unused-admission','mapping-failure','retirement','retirement-failure','retirement-pressure','retirement-retry','retirement-teardown','retirement-cap')][string]$Mode='regression',
  [int]$BudgetMiB=100,
  [ValidateSet('below','hot','cold','over','create','unused','admission')][string]$Workload='hot',
  [int]$Iterations=512, [string]$Probe, [switch]$BuildOnly)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$Backend){$Backend=Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'out/perf/20261114-f8-tail-mapping/mapping-native'}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
if(!$Probe){
  $toolchain=Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
  $source=if($Mode -eq 'lru'){'mapping-lru-tests.cpp'}else{'mapping-budget-probe.cpp'}
  $Probe=Join-Path $OutputDirectory ([IO.Path]::GetFileNameWithoutExtension($source)+'.exe')
  & $toolchain.LlvmMingw.CompilerPath '-std=c++17' '-O2' '-static' (Join-Path $PSScriptRoot $source) '-o' $Probe '-luuid' '-luser32'
  if($LASTEXITCODE){throw "Mapping probe compilation failed ($LASTEXITCODE)"}
}
if($BuildOnly){return}
$Probe=(Resolve-Path -LiteralPath $Probe).Path
$arguments=@()
if($Mode -ne 'lru'){
  $libraryArgument='"'+(Resolve-Path -LiteralPath $Backend).Path+'"'
  $arguments=@($libraryArgument,$Mode,[string]$BudgetMiB)
}
if($Mode -eq 'bench'){$arguments+=@($Workload,$Iterations)}
$env:DXVK_LOG_PATH=$OutputDirectory
$stdout=Join-Path $OutputDirectory "$Mode-$Workload-$BudgetMiB.stdout.log"
$stderr=Join-Path $OutputDirectory "$Mode-$Workload-$BudgetMiB.stderr.log"
$start=@{FilePath=$Probe;WorkingDirectory=$OutputDirectory;WindowStyle='Hidden';PassThru=$true;RedirectStandardOutput=$stdout;RedirectStandardError=$stderr}
if($arguments.Count){$start.ArgumentList=$arguments}
$process=Start-Process @start
try {
  if(!$process.WaitForExit(120000)){$process.Kill();$process.WaitForExit();$result=124}else{$result=$process.ExitCode}
} finally {
  Get-Content -LiteralPath $stdout
  Get-Content -LiteralPath $stderr
  $process.Dispose()
}
exit $result
