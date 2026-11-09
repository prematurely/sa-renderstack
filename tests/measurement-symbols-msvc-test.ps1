param(
    [string]$EvidenceName = 'msvc-green',
    [string]$MSBuild,
    [string]$Python,
    [string]$LlvmMingwBin,
    [switch]$AllowNonV18MsBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$toolchain = Get-RenderStackToolchain -RepoRoot $root -Component Bridge -MsBuildPath $MSBuild -AllowNonV18MsBuild:$AllowNonV18MsBuild
$pythonInfo = Find-RenderStackPython -PythonPath $Python -RepoRoot $root
$platformToolset = if ($toolchain.MSBuild.ProductMajor -eq 17) { 'v143' } else { 'v145' }
$demangler = $null
try {
    $llvmInfo = Find-RenderStackLlvmMingw -LlvmMingwBin $LlvmMingwBin -RepoRoot $root
    $candidateDemangler = Join-Path $llvmInfo.BinPath 'llvm-cxxfilt.exe'
    if (Test-Path -LiteralPath $candidateDemangler -PathType Leaf) { $demangler = $candidateDemangler }
} catch {
    if ($LlvmMingwBin) { throw }
    Write-Warning "Optional LLVM demangler is unavailable; its test will be skipped: $_"
}
$output = Join-Path $root 'out/perf/20261027-unified-measurement/symbols'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$project = Join-Path $output 'MeasurementSymbolsMsvc.vcxproj'
$xmlRoot = [Security.SecurityElement]::Escape($root)
@"
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|Win32"><Configuration>Release</Configuration><Platform>Win32</Platform></ProjectConfiguration></ItemGroup>
  <PropertyGroup Label="Globals"><RootNamespace>MeasurementSymbolsMsvc</RootNamespace></PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>$platformToolset</PlatformToolset><CharacterSet>MultiByte</CharacterSet></PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <PropertyGroup><OutDir>`$(ProjectDir)msvc-bin\</OutDir><IntDir>`$(ProjectDir)msvc-obj\</IntDir><TargetName>measurement-symbols-probe</TargetName></PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile><Optimization>MaxSpeed</Optimization><RuntimeLibrary>MultiThreaded</RuntimeLibrary><LanguageStandard>stdcpplatest</LanguageStandard><AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions><AdditionalIncludeDirectories>$xmlRoot\src\bridge\legacy;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories></ClCompile>
    <Link><SubSystem>Console</SubSystem><AdditionalDependencies>bcrypt.lib;%(AdditionalDependencies)</AdditionalDependencies></Link>
  </ItemDefinitionGroup>
  <ItemGroup><ClCompile Include="$xmlRoot\tests\measurement-symbols-probe.cpp" /><ClCompile Include="$xmlRoot\src\bridge\legacy\MeasurementSymbols.cpp" /></ItemGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8
& $toolchain.MSBuild.Path $project /nologo /m:1 /nr:false /t:Build /p:Configuration=Release /p:Platform=Win32 /p:PreferredToolArchitecture=x64 2>&1 |
    Tee-Object -FilePath (Join-Path $output "$EvidenceName-build.log")
if ($LASTEXITCODE -ne 0) { throw 'MSVC latest measurement symbol probe build failed' }
$testArguments = @((Join-Path $PSScriptRoot 'measurement-symbols-test.py'), '--probe', (Join-Path $output 'msvc-bin/measurement-symbols-probe.exe'))
if ($demangler) { $testArguments += @('--demangle-tool', $demangler) }
& $pythonInfo.Path @testArguments 2>&1 |
    Tee-Object -FilePath (Join-Path $output "$EvidenceName-test.log")
if ($LASTEXITCODE -ne 0) { throw 'MSVC latest measurement symbol behavior tests failed' }
