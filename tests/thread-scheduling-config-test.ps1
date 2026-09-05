param([string]$Executable)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $root 'out/build/dxvk-x86/tools/renderstack-thread-scheduling-test.exe'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Build the thread scheduling test first: $Executable"
}

# All configuration files belong to this unique fixture beside its copied EXE.
# Run it from a different CWD to check that runtime lookup follows the EXE.
$fixture = Join-Path $root ('out/test-fixtures/m1-config-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $fixture 'scripts') -Force | Out-Null
$probe = Join-Path $fixture 'thread-scheduling-config.exe'
Copy-Item -LiteralPath $Executable -Destination $probe
$utf8 = [Text.UTF8Encoding]::new($false)

function Assert-Options([string]$Name, [int]$Enabled, [int]$Mmcss, [string]$SourceSuffix) {
    Push-Location $fixture
    try { $output = (& $probe --config 2>&1 | Out-String) } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0 -or $output -notmatch "enabled=$Enabled\b" -or
        $output -notmatch "mmcss=$Mmcss\b" -or
        $output.Replace('\', '/') -notmatch [regex]::Escape($SourceSuffix)) {
        throw "$Name failed: $output"
    }
    Write-Output "PASS $Name"
}

Assert-Options 'missing configuration is disabled' 0 0 'path=none'
[IO.File]::WriteAllText((Join-Path $fixture 'scripts/BridgeD3D9.ini'), "[Affinity]`nPerThread=1`nMmcss=1`n", $utf8)
Assert-Options 'legacy-only configuration' 1 1 'scripts/BridgeD3D9.ini'
[IO.File]::WriteAllText((Join-Path $fixture 'SA.RenderStack.ini'), "[Affinity]`nPerThread=0`nMmcss=0`n", $utf8)
Assert-Options 'root configuration overrides legacy' 0 0 'SA.RenderStack.ini'
[IO.File]::WriteAllText((Join-Path $fixture 'SA.RenderStack.ini'), "[Affinity]`nPerThread=1`nMmcss=0`n", $utf8)
Assert-Options 'CPU selection without MMCSS' 1 0 'SA.RenderStack.ini'
[IO.File]::WriteAllText((Join-Path $fixture 'SA.RenderStack.ini'), "[Affinity]`nPerThread=invalid`nMmcss=invalid`n", $utf8)
Assert-Options 'invalid values default to disabled' 0 0 'SA.RenderStack.ini'
Write-Output "Fixture retained: $fixture"
