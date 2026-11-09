param([string]$Backend, [string]$EvidenceName='candidate', [switch]$BaselineSource,
    [switch]$BuildOnly, [string]$MSBuild, [switch]$AllowNonV18MsBuild)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$compiler=Find-RenderStackMSBuild -MsBuildPath $MSBuild -RepoRoot $root -AllowNonV18MsBuild:$AllowNonV18MsBuild
$evidence=Join-Path $root "out/perf/20261027-statering-cache/$EvidenceName"
New-Item -ItemType Directory -Path $evidence -Force|Out-Null
$buildArgs=@((Join-Path $PSScriptRoot 'statering-runtime-tests.vcxproj'),'/nologo','/m:1','/nr:false','/p:Configuration=Release','/p:Platform=Win32',"/p:EvidenceRoot=$evidence")
if($compiler.ProductMajor -lt 18){$buildArgs+='/p:PlatformToolset=v143'}
if($BaselineSource){$buildArgs+=@("/p:GuardSourceRoot=$(Join-Path $root 'out/perf/20261027-statering-cache/source-before/src/bridge/legacy')",'/p:FixtureDefinitions=BASELINE_SOURCE')}
& $compiler.Path @buildArgs *> (Join-Path $evidence 'build.log')
if($LASTEXITCODE){Get-Content (Join-Path $evidence 'build.log') -Tail 15;throw 'StateRing native fixture build failed'}
if($BuildOnly){return}
$backendPath=if($Backend){(Resolve-Path -LiteralPath $Backend).Path}else{$null}
Push-Location $evidence
try { $run=@();if($backendPath){$run+=$backendPath}; & (Join-Path $evidence 'bin/StateRingRuntimeTests.exe') @run 2>&1|Tee-Object (Join-Path $evidence 'test.log');$code=$LASTEXITCODE }
finally {Pop-Location}
if($code){throw "StateRing native fixture failed: $code"}
