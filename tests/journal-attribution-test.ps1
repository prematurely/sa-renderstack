param([string]$EvidenceName = 'green', [switch]$Device, [string]$Backend,
    [string]$MSBuild, [switch]$AllowNonV18MsBuild)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out/perf/20261027-unified-measurement/journal'
New-Item -ItemType Directory -Path (Join-Path $out 'scripts') -Force | Out-Null
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$msBuildInfo = Find-RenderStackMSBuild -MsBuildPath $MSBuild -RepoRoot $root -AllowNonV18MsBuild:$AllowNonV18MsBuild
$include = Join-Path $root 'out/deps/Microsoft.DXSDK.D3DX/9.29.952.8/package/build/native/include'
$buildArguments = @((Join-Path $PSScriptRoot 'journal-attribution-tests.vcxproj'), '/nologo', '/m:1', '/nr:false', '/t:Build',
    '/p:Configuration=Release', '/p:Platform=Win32', '/p:PreferredToolArchitecture=x64', "/p:D3DX9IncludeDir=$include")
if ($msBuildInfo.ProductMajor -lt 18) { $buildArguments += '/p:PlatformToolset=v143' }
& $msBuildInfo.Path @buildArguments 2>&1 |
    Tee-Object -FilePath (Join-Path $out "$EvidenceName-build.log")
if ($LASTEXITCODE -ne 0) { throw "journal attribution build failed: $LASTEXITCODE" }
Push-Location $out
try {
    $testArguments = @()
    if ($Device) { $testArguments += '--device' }
    if ($Backend) { $testArguments += $Backend }
    & (Join-Path $out 'bin/JournalAttributionTests.exe') @testArguments 2>&1 |
        Tee-Object -FilePath (Join-Path $out "$EvidenceName-test.log")
    $result = $LASTEXITCODE
} finally { Pop-Location }
if ($result -ne 0) { throw "journal attribution tests failed: $result" }
