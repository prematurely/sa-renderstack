param([string]$Backend,[string]$OutputDirectory,[string]$LlvmMingwBin,[switch]$BuildOnly)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$Backend){$Backend=Join-Path $root 'out/build/dxvk-x86/src/d3d9/d3d9.dll'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'out/tests/retirement-cs'}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$toolchain=Get-RenderStackToolchain -RepoRoot $root -Component Dxvk -LlvmMingwBin $LlvmMingwBin
$probe=Join-Path $OutputDirectory 'retirement-cs-probe.exe'
& $toolchain.LlvmMingw.CompilerPath '-std=c++17' '-O2' '-static' (Join-Path $PSScriptRoot 'retirement-cs-probe.cpp') '-o' $probe '-luuid' '-luser32'
if($LASTEXITCODE){throw "Retirement/CS probe compilation failed ($LASTEXITCODE)"}
if($BuildOnly){return}
$stdout=Join-Path $OutputDirectory 'stdout.log';$stderr=Join-Path $OutputDirectory 'stderr.log';$env:DXVK_LOG_PATH=$OutputDirectory
$p=Start-Process -FilePath $probe -ArgumentList ('"'+(Resolve-Path -LiteralPath $Backend).Path+'"') -WorkingDirectory $OutputDirectory -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try{if(!$p.WaitForExit(120000)){$p.Kill();$p.WaitForExit();$result=124}else{$result=$p.ExitCode}}
finally{Get-Content -LiteralPath $stdout;Get-Content -LiteralPath $stderr;$p.Dispose()}
exit $result
