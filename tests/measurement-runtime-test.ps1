param([string]$EvidenceName = 'green', [string]$Backend, [switch]$BuildOnly,
    [string]$MSBuild, [switch]$AllowNonV18MsBuild, [string]$RuntimeSource,
    [switch]$PendingStopOnly, [switch]$DeviceOwnerOnly, [switch]$UnicodeModulesOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out/perf/20261027-unified-measurement/runtime'
New-Item -ItemType Directory -Path (Join-Path $out 'scripts') -Force | Out-Null
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$msBuildInfo = Find-RenderStackMSBuild -MsBuildPath $MSBuild -RepoRoot $root -AllowNonV18MsBuild:$AllowNonV18MsBuild
$include = Join-Path $root 'out/deps/Microsoft.DXSDK.D3DX/9.29.952.8/package/build/native/include'
$buildArguments = @((Join-Path $PSScriptRoot 'measurement-runtime-tests.vcxproj'), '/nologo', '/m:1', '/nr:false', '/t:Build',
    '/p:Configuration=Release', '/p:Platform=Win32', '/p:PreferredToolArchitecture=x64', "/p:D3DX9IncludeDir=$include")
if ($msBuildInfo.ProductMajor -lt 18) { $buildArguments += '/p:PlatformToolset=v143' }
$testExe = Join-Path $out 'bin/MeasurementRuntimeTests.exe'
if ($RuntimeSource) {
    $sourceItem = Get-Item -LiteralPath $RuntimeSource
    $mutationRoot = Join-Path $out "mutations/$EvidenceName"
    $buildArguments += @("/p:RuntimeSource=$($sourceItem.FullName)",
        "/p:OutDir=$mutationRoot/bin/", "/p:IntDir=$mutationRoot/obj/")
    $testExe = Join-Path $mutationRoot 'bin/MeasurementRuntimeTests.exe'
}
& $msBuildInfo.Path @buildArguments 2>&1 |
    Tee-Object -FilePath (Join-Path $out "$EvidenceName-build.log")
if ($LASTEXITCODE -ne 0) { throw "measurement runtime build failed: $LASTEXITCODE" }
$inputs = @()
if ($RuntimeSource) {
    $inputs += [ordered]@{ path = $sourceItem.FullName; size = $sourceItem.Length;
        sha256 = (Get-FileHash -LiteralPath $sourceItem.FullName -Algorithm SHA256).Hash }
}
if ($Backend) {
    $item = Get-Item -LiteralPath $Backend
    $inputs += [ordered]@{ path = $item.FullName; size = $item.Length;
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash;
        lastWriteTimeUtc = $item.LastWriteTimeUtc.ToString('o') }
}
[IO.File]::WriteAllText((Join-Path $out "$EvidenceName-inputs.json"),
    (ConvertTo-Json -InputObject @($inputs) -Depth 3), [Text.UTF8Encoding]::new($false))
if ($BuildOnly) { return }
Push-Location $out
try {
    $testArguments = @()
    if ($PendingStopOnly) { $testArguments += '--pending-stop-only' }
    if ($DeviceOwnerOnly) { $testArguments += '--device-owner-only' }
    if ($UnicodeModulesOnly) { $testArguments += '--unicode-modules-only' }
    if ($Backend) { $testArguments += $Backend }
    & $testExe @testArguments 2>&1 |
        Tee-Object -FilePath (Join-Path $out "$EvidenceName-test.log")
    $result = $LASTEXITCODE
} finally { Pop-Location }
if ($result -ne 0) { throw "measurement runtime tests failed: $result" }
