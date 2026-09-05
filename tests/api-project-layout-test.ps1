$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$apiRoot = Join-Path $root 'src/bridge/legacy/api-projects'
$bridgeProjectPath = Join-Path $root 'src/bridge/legacy/BridgeD3D9.vcxproj'
$bridgeProject = Get-Content -LiteralPath $bridgeProjectPath -Raw
$names = @(
  'api1-status', 'api2-vulkan-pass', 'api3-state-batch', 'api4-state-journal',
  'api5-effect-batch', 'api6-state-draw', 'api7-selective-journal')
foreach ($name in $names) {
  $dir = Join-Path $apiRoot $name
  if (-not (Test-Path -LiteralPath $dir -PathType Container)) { throw "Missing attached API project: $name" }
  foreach ($file in @('CMakeLists.txt', 'README.md', 'main.cpp')) {
    if (-not (Test-Path -LiteralPath (Join-Path $dir $file) -PathType Leaf)) { throw "$name missing $file" }
  }
  foreach ($subdir in @('include', 'src', 'tests')) {
    if (-not (Test-Path -LiteralPath (Join-Path $dir $subdir) -PathType Container)) { throw "$name missing $subdir" }
  }
  $cmake = Get-Content -LiteralPath (Join-Path $dir 'CMakeLists.txt') -Raw
  if ($cmake -notmatch 'cxx_std_23') { throw "$name is not declared as C++23" }
  $sources = @(Get-ChildItem -LiteralPath (Join-Path $dir 'src') -File -Filter '*.cpp')
  if ($sources.Count -ne 1) { throw "$name must have exactly one library implementation source" }
  $relative = "api-projects\$name\src\$($sources[0].Name)"
  if ($bridgeProject -notmatch [regex]::Escape($relative)) { throw "BridgeD3D9.vcxproj does not own $relative" }
}
if (Test-Path -LiteralPath (Join-Path $root 'api-projects')) { throw 'API projects must remain attached below src/bridge/legacy' }
Write-Output 'PASS attached API project layout'
