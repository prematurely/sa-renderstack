param(
    [string]$OutputDirectory = 'out/perf/20261027-unified-measurement/symbols',
    [string]$Compiler,
    [string]$Python,
    [string]$LlvmMingwBin
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools/lib/process-runner.ps1')
. (Join-Path $root 'tools/lib/toolchain-discovery.ps1')
$compilerPath = if ($Compiler) { Resolve-RenderStackExecutable -Path $Compiler -Description 'symbol-test compiler' } else { $null }
if (-not $LlvmMingwBin -and $compilerPath) { $LlvmMingwBin = Split-Path -Parent $compilerPath }
# Compile and run this fixture directly; no DXVK build tools are required.
$llvm = Find-RenderStackLlvmMingw -RepoRoot $root -LlvmMingwBin $LlvmMingwBin
if (-not $compilerPath) { $compilerPath = $llvm.CompilerPath }
$pythonPath = (Find-RenderStackPython -RepoRoot $root -PythonPath $Python).Path
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
New-Item -ItemType Directory -Force -Path $output | Out-Null
$probe = Join-Path $output 'measurement-symbols-probe.exe'
$sources = @((Join-Path $PSScriptRoot 'measurement-symbols-probe.cpp'))
$implementation = Join-Path $root 'src/bridge/legacy/MeasurementSymbols.cpp'
if (Test-Path -LiteralPath $implementation) { $sources += $implementation }
& $compilerPath '-std=c++17' '-O2' '-Wall' '-Wextra' '-static' @sources '-I' (Join-Path $root 'src/bridge/legacy') '-lbcrypt' '-o' $probe
if ($LASTEXITCODE -ne 0) { throw 'measurement symbol probe build failed' }
$testArguments = @((Join-Path $PSScriptRoot 'measurement-symbols-test.py'), '--probe', $probe)
$demangler = Join-Path $llvm.BinPath 'llvm-cxxfilt.exe'
if (Test-Path -LiteralPath $demangler -PathType Leaf) { $testArguments += @('--demangle-tool', $demangler) }
& $pythonPath @testArguments
if ($LASTEXITCODE -ne 0) { throw 'measurement symbol behavior tests failed' }
