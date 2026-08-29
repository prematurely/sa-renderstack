param(
    [switch]$Help,
    [string]$Configuration = 'Release',
    [string]$Architecture = 'x86',
    [string]$Component = 'All',
    [switch]$Clean,
    [string]$MSBuild,
    [string]$Python,
    [string]$LlvmMingwBin,
    [string]$Ninja,
    [string]$Glslang
)

$env:GIT_CONFIG_GLOBAL = 'NUL'

if ($Help) {
    @'
Usage: pwsh -NoProfile -File tools/build.ps1 [-Help] [-Configuration Release] [-Architecture x86]
       [-Component All|Bridge|Dxvk] [-Clean]
       [-MSBuild <path>] [-Python <path>] [-LlvmMingwBin <path>]
       [-Ninja <path>] [-Glslang <path>]

Builds the x86 DXVK backend, five Meson probe targets, the Bridge DLLs, the
seven Bridge test projects, and the backend ABI executable into out/build.
'@ | Write-Output
    exit 0
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Configuration -cne 'Release') {
    Write-Error "Unsupported configuration '$Configuration'; only Release is accepted"
    exit 2
}
if ($Architecture -cne 'x86') {
    Write-Error "Unsupported architecture '$Architecture'; only x86 is accepted"
    exit 2
}
if ($Component -notin @('All', 'Bridge', 'Dxvk')) {
    Write-Error "Unsupported component '$Component'; expected All|Bridge|Dxvk"
    exit 2
}

. (Join-Path $PSScriptRoot 'lib/process-runner.ps1')
. (Join-Path $PSScriptRoot 'lib/toolchain-discovery.ps1')

$root = Get-RenderStackRepositoryRoot -CallerScriptPath $PSCommandPath
$buildRoot = Join-Path $root 'out/build'
$logsRoot = Join-Path $root 'out/logs'
$logPath = Join-Path $logsRoot (New-RenderStackArtifactName -Prefix 'build' -Extension '.log')
$metadataPath = Join-Path $root 'out/build-metadata.json'
$startUtc = [DateTime]::UtcNow
$totalStopwatch = [Diagnostics.Stopwatch]::StartNew()
$firstExitCode = 0
$failureMessage = $null
$toolchain = $null
$mesonModuleDirectory = $null
$componentResults = [Collections.Generic.List[object]]::new()
$expectedOutputs = [Collections.Generic.List[string]]::new()
$processResults = [Collections.Generic.List[object]]::new()

function Invoke-BuildProcess {
    param(
        [Parameter(Mandatory)] [string]$Label,
        [Parameter(Mandatory)] [string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [hashtable]$EnvironmentOverrides = @{},
        [string[]]$EnvironmentRemovals = @()
    )

    $result = Invoke-RenderStackProcess -FilePath $FilePath -ArgumentList $Arguments `
        -WorkingDirectory $WorkingDirectory -EnvironmentOverrides $EnvironmentOverrides `
        -EnvironmentRemovals $EnvironmentRemovals -CombinedLogPath $logPath `
        -Label $Label -EchoOutput
    [void]$processResults.Add($result)
    if ($result.ExitCode -ne 0) {
        if ($script:firstExitCode -eq 0) {
            $script:firstExitCode = $result.ExitCode
        }
        throw "$Label failed with exit $($result.ExitCode)"
    }
    return $result
}

function Get-TrailingDirectoryPath {
    param([Parameter(Mandatory)] [string]$Path)

    return (Get-RenderStackFullPath -Path $Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
}

function Add-ExpectedOutput {
    param([Parameter(Mandatory)] [string]$Path)

    [void]$expectedOutputs.Add((Get-RenderStackFullPath -Path $Path))
}

function Remove-GeneratedWrapLocks {
    $backend = Join-Path $root 'backend/dxvk'
    foreach ($lock in @(Get-ChildItem -LiteralPath $backend -Filter '.wraplock' -File -Recurse `
            -Force -ErrorAction SilentlyContinue)) {
        $safeLock = Assert-RenderStackNoReparsePath -Path $lock.FullName -Anchor $backend `
            -Description 'Generated Meson wrap lock'
        [IO.File]::Delete($safeLock)
    }
}

function Get-OutputMetadata {
    param([Parameter(Mandatory)] [string]$Path)

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    $relative = [IO.Path]::GetRelativePath($root, $item.FullName).Replace('\', '/')
    $pe = if ($item.Extension -in @('.dll', '.exe')) {
        Get-RenderStackPeMetadata -Path $item.FullName
    } else {
        $null
    }
    return [ordered]@{
        path = $relative
        size = $item.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.FullName).Hash.ToUpperInvariant()
        pe = $pe
    }
}

function Get-ToolMetadata {
    $tools = [ordered]@{}
    if ($null -ne $toolchain.MSBuild) {
        $tools.msbuild = $toolchain.MSBuild
    }
    if ($null -ne $toolchain.Python) {
        $tools.python = $toolchain.Python
        $tools.meson = [ordered]@{
            moduleDirectory = $mesonModuleDirectory
            version = '1.11.1'
        }
    }
    if ($null -ne $toolchain.LlvmMingw) {
        $tools.llvmMingw = $toolchain.LlvmMingw
        $tools.ninja = $toolchain.Ninja
        $tools.glslang = $toolchain.Glslang
    }
    return $tools
}

try {
    New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null
    Add-RenderStackLogText -Path $logPath -Text (
        "SA RenderStack build startUtc=$($startUtc.ToString('o')) component=$Component configuration=$Configuration architecture=$Architecture clean=$Clean" +
        [Environment]::NewLine)

    $toolchain = Get-RenderStackToolchain -RepoRoot $root -Component $Component `
        -MsBuildPath $MSBuild -PythonPath $Python -LlvmMingwBin $LlvmMingwBin `
        -NinjaPath $Ninja -GlslangPath $Glslang

    $cleanNames = [Collections.Generic.List[string]]::new()
    if ($Component -in @('All', 'Dxvk')) {
        [void]$cleanNames.Add('dxvk-x86')
    }
    if ($Component -in @('All', 'Bridge')) {
        foreach ($name in @(
                'bridge', 'bridge-obj',
                'bridge-backend-trace', 'bridge-backend-trace-obj',
                'bridge-tests', 'bridge-tests-obj',
                'sdk-tests', 'sdk-tests-obj')) {
            [void]$cleanNames.Add($name)
        }
    }
    if ($Clean) {
        foreach ($name in $cleanNames) {
            Remove-RenderStackBuildPath -Path (Join-Path $buildRoot $name) `
                -BuildRoot $buildRoot -AllowedName $cleanNames.ToArray()
        }
    }
    New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null

    if ($Component -in @('All', 'Dxvk')) {
        $componentStopwatch = [Diagnostics.Stopwatch]::StartNew()
        $pythonPath = $toolchain.Python.Path
        $prepareMeson = Invoke-BuildProcess -Label 'prepare-meson' -FilePath (Get-Command pwsh).Source `
            -Arguments @('-NoProfile', '-File', (Join-Path $root 'tools/prepare-meson.ps1'), '-Python', $pythonPath) `
            -WorkingDirectory $root
        $mesonModuleDirectory = ($prepareMeson.StandardOutput -split '\r?\n' | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_)
        } | Select-Object -Last 1).Trim()
        if (-not (Test-Path -LiteralPath $mesonModuleDirectory -PathType Container)) {
            throw "prepare-meson returned an invalid module directory: $mesonModuleDirectory"
        }

        $pathDirectories = [Collections.Generic.List[string]]::new()
        foreach ($directory in @(
                $toolchain.LlvmMingw.BinPath,
                (Split-Path -Parent $toolchain.Ninja.Path),
                (Split-Path -Parent $toolchain.Glslang.Path),
                (Split-Path -Parent $pythonPath),
                (Split-Path -Parent (Get-Command git.exe -ErrorAction Stop).Source),
                (Join-Path $env:SystemRoot 'System32'),
                (Join-Path $env:SystemRoot 'System32/Wbem'))) {
            if (-not $pathDirectories.Contains($directory)) {
                [void]$pathDirectories.Add($directory)
            }
        }
        $dxvkEnvironment = @{
            PATH = ($pathDirectories -join [IO.Path]::PathSeparator)
            PYTHONPATH = $mesonModuleDirectory
        }
        $dxvkBuild = Join-Path $buildRoot 'dxvk-x86'
        $dxvkSource = Join-Path $root 'backend/dxvk'
        $crossFile = Join-Path $root 'toolchains/llvm-mingw-i686.ini'
        $sdkInclude = Get-RenderStackFullPath -Path (Join-Path $root 'sdk/include')
        $setupArguments = [Collections.Generic.List[string]]::new()
        [void]$setupArguments.Add('-m')
        [void]$setupArguments.Add('mesonbuild.mesonmain')
        [void]$setupArguments.Add('setup')
        if (Test-Path -LiteralPath (Join-Path $dxvkBuild 'meson-private/coredata.dat') -PathType Leaf) {
            [void]$setupArguments.Add('--reconfigure')
        }
        [void]$setupArguments.Add($dxvkBuild)
        [void]$setupArguments.Add($dxvkSource)
        foreach ($argument in @(
                '--cross-file', $crossFile,
                '--wrap-mode', 'nodownload',
                '--buildtype', 'release',
                '-Denable_d3d8=false',
                '-Denable_d3d9=true',
                '-Denable_d3d10=false',
                '-Denable_d3d11=false',
                '-Denable_dxgi=false',
                '-Dmerge_dxgi_into_d3d9=true',
                "-Drenderstack_sdk_dir=$sdkInclude",
                '-Denable_renderstack_tests=true')) {
            [void]$setupArguments.Add($argument)
        }
        Invoke-BuildProcess -Label 'meson-setup-dxvk-x86' -FilePath $pythonPath `
            -Arguments $setupArguments.ToArray() -WorkingDirectory $root `
            -EnvironmentOverrides $dxvkEnvironment | Out-Null

        $mesonTargets = @(
            'd3d9',
            'renderstack-d3d9-batch-audit-test',
            'renderstack-d3d9-deferred-shader-binding-test',
            'renderstack-dxvk-state-dedup-test',
            'renderstack-stateblock-prefilter-probe',
            'renderstack-gta-sa-compat-probe'
        )
        Invoke-BuildProcess -Label 'meson-compile-dxvk-x86' -FilePath $pythonPath `
            -Arguments (@('-m', 'mesonbuild.mesonmain', 'compile', '-C', $dxvkBuild) + $mesonTargets) `
            -WorkingDirectory $root -EnvironmentOverrides $dxvkEnvironment | Out-Null
        $componentStopwatch.Stop()

        $dxvkOutputs = @(
            (Join-Path $dxvkBuild 'src/d3d9/d3d9.dll'),
            (Join-Path $dxvkBuild 'tools/renderstack-d3d9-batch-audit-test.exe'),
            (Join-Path $dxvkBuild 'tools/renderstack-d3d9-deferred-shader-binding-test.exe'),
            (Join-Path $dxvkBuild 'tools/renderstack-dxvk-state-dedup-test.exe'),
            (Join-Path $dxvkBuild 'tools/renderstack-stateblock-prefilter-probe.exe'),
            (Join-Path $dxvkBuild 'tools/renderstack-gta-sa-compat-probe.exe')
        )
        foreach ($output in $dxvkOutputs) { Add-ExpectedOutput -Path $output }
        [void]$componentResults.Add([ordered]@{
            component = 'Dxvk'
            exitCode = 0
            durationMilliseconds = [Math]::Round($componentStopwatch.Elapsed.TotalMilliseconds, 3)
            targets = $mesonTargets
            outputs = @($dxvkOutputs | ForEach-Object {
                [IO.Path]::GetRelativePath($root, $_).Replace('\', '/')
            })
        })
    }

    if ($Component -in @('All', 'Bridge')) {
        $componentStopwatch = [Diagnostics.Stopwatch]::StartNew()
        $prepareDxsdk = Invoke-BuildProcess -Label 'prepare-dxsdk' -FilePath (Get-Command pwsh).Source `
            -Arguments @('-NoProfile', '-File', (Join-Path $root 'tools/prepare-dxsdk.ps1')) `
            -WorkingDirectory $root
        $d3dxInclude = ($prepareDxsdk.StandardOutput -split '\r?\n' | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_)
        } | Select-Object -Last 1).Trim()
        $d3dxHeader = Join-Path $d3dxInclude 'd3dx9effect.h'
        if (-not (Test-Path -LiteralPath $d3dxHeader -PathType Leaf)) {
            throw "Prepared D3DX include directory is invalid: $d3dxInclude"
        }

        $bridgeOut = Get-TrailingDirectoryPath -Path (Join-Path $buildRoot 'bridge')
        $bridgeObj = Get-TrailingDirectoryPath -Path (Join-Path $buildRoot 'bridge-obj')
        $traceOut = Get-TrailingDirectoryPath -Path (Join-Path $buildRoot 'bridge-backend-trace')
        $traceObj = Get-TrailingDirectoryPath -Path (Join-Path $buildRoot 'bridge-backend-trace-obj')
        $testOut = Get-TrailingDirectoryPath -Path (Join-Path $buildRoot 'bridge-tests')
        $testObj = Join-Path $buildRoot 'bridge-tests-obj'
        $sdkOut = Get-TrailingDirectoryPath -Path (Join-Path $buildRoot 'sdk-tests')
        $sdkObj = Get-TrailingDirectoryPath -Path (Join-Path $buildRoot 'sdk-tests-obj')
        $commonArguments = @(
            '/nologo', '/m:1', '/nr:false', '/t:Build',
            '/p:Configuration=Release', '/p:Platform=Win32',
            '/p:PreferredToolArchitecture=x64',
            "/p:D3DX9IncludeDir=$d3dxInclude"
        )
        $msbuildEnvironmentRemovals = @('CL', 'LINK', 'ExternalIncludePath')
        $projects = @(
            [ordered]@{
                Name = 'BridgeD3D9'
                Path = Join-Path $root 'src/bridge/legacy/BridgeD3D9.vcxproj'
                OutDir = $bridgeOut
                IntDir = $bridgeObj
                Output = Join-Path $bridgeOut 'd3d9.dll'
            },
            [ordered]@{
                Name = 'BridgeD3D9BackendTrace'
                Path = Join-Path $root 'src/bridge/legacy/BridgeD3D9BackendTrace.vcxproj'
                OutDir = $traceOut
                IntDir = $traceObj
                Output = Join-Path $traceOut 'd3d9.dll'
            },
            [ordered]@{
                Name = 'backend-api-layout-test'
                Path = Join-Path $root 'tests/backend-api-layout-test.vcxproj'
                OutDir = $sdkOut
                IntDir = $sdkObj
                Output = Join-Path $sdkOut 'backend-api-layout-test.exe'
            }
        )
        foreach ($name in @(
                'BridgeLegacyPluginProbe', 'BridgeVulkanPassProbe', 'GtaSaCompatApi3Smoke',
                'PerformanceAdapterConfigTests', 'PerformanceAdaptersTests',
                'ProperShadersBatchPolicyTests', 'ProperShadersStateJournalTests')) {
            $extension = if ($name -in @('BridgeLegacyPluginProbe', 'BridgeVulkanPassProbe')) { '.dll' } else { '.exe' }
            $projects += [ordered]@{
                Name = $name
                Path = Join-Path $root "src/bridge/legacy/tests/$name.vcxproj"
                OutDir = $testOut
                IntDir = Get-TrailingDirectoryPath -Path (Join-Path $testObj $name)
                Output = Join-Path $testOut "$name$extension"
            }
        }
        foreach ($project in $projects) {
            Invoke-BuildProcess -Label "msbuild-$($project.Name)" -FilePath $toolchain.MSBuild.Path `
                -Arguments (@($project.Path) + $commonArguments + @(
                    "/p:OutDir=$($project.OutDir)",
                    "/p:IntDir=$($project.IntDir)")) `
                -WorkingDirectory $root -EnvironmentRemovals $msbuildEnvironmentRemovals | Out-Null
            Add-ExpectedOutput -Path $project.Output
        }
        $componentStopwatch.Stop()
        [void]$componentResults.Add([ordered]@{
            component = 'Bridge'
            exitCode = 0
            durationMilliseconds = [Math]::Round($componentStopwatch.Elapsed.TotalMilliseconds, 3)
            targets = @($projects.Name)
            outputs = @($projects.Output | ForEach-Object {
                [IO.Path]::GetRelativePath($root, $_).Replace('\', '/')
            })
        })
    }

    foreach ($output in $expectedOutputs) {
        if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
            throw "Expected build output is missing: $output"
        }
        $pe = Get-RenderStackPeMetadata -Path $output
        if ($pe.Machine -cne 'IMAGE_FILE_MACHINE_I386' -or $pe.Format -cne 'PE32') {
            throw "Expected PE32 x86 output, found $($pe.Format)/$($pe.Machine): $output"
        }
    }
} catch {
    if ($firstExitCode -eq 0) {
        $firstExitCode = 1
    }
    $failureMessage = $_.Exception.Message
} finally {
    try {
        Remove-GeneratedWrapLocks
    } catch {
        if ($firstExitCode -eq 0) {
            $firstExitCode = 1
            $failureMessage = "Generated wrap-lock cleanup failed: $($_.Exception.Message)"
        } else {
            $failureMessage = "$failureMessage; generated wrap-lock cleanup failed: $($_.Exception.Message)"
        }
    }
}

$totalStopwatch.Stop()
$endUtc = [DateTime]::UtcNow
try {
    if (Test-Path -LiteralPath $logPath -PathType Leaf) {
        Add-ExpectedOutput -Path $logPath
    }
    $gitPath = (Get-Command git.exe -ErrorAction Stop).Source
    $gitResult = Invoke-RenderStackProcess -FilePath $gitPath -ArgumentList @('rev-parse', 'HEAD') `
        -WorkingDirectory $root -Label 'git-rev-parse'
    if ($gitResult.ExitCode -ne 0) {
        throw "Unable to resolve build commit: $($gitResult.StandardError)"
    }
    $outputMetadata = @($expectedOutputs | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -Unique | ForEach-Object { Get-OutputMetadata -Path $_ })
    $metadata = [ordered]@{
        schema = 1
        commit = $gitResult.StandardOutput.Trim()
        configuration = $Configuration
        architecture = $Architecture
        component = $Component
        options = [ordered]@{
            clean = [bool]$Clean
            meson = [ordered]@{
                wrapMode = 'nodownload'
                buildType = 'release'
                enableD3d8 = $false
                enableD3d9 = $true
                enableD3d10 = $false
                enableD3d11 = $false
                enableDxgi = $false
                mergeDxgiIntoD3d9 = $true
                enableRenderStackTests = $true
                renderStackSdkDir = (Join-Path $root 'sdk/include')
            }
        }
        tools = if ($null -ne $toolchain) { Get-ToolMetadata } else { [ordered]@{} }
        startUtc = $startUtc.ToString('o')
        endUtc = $endUtc.ToString('o')
        durationMilliseconds = [Math]::Round($totalStopwatch.Elapsed.TotalMilliseconds, 3)
        exitCode = $firstExitCode
        failure = $failureMessage
        componentResults = @($componentResults)
        outputs = $outputMetadata
        logPath = [IO.Path]::GetRelativePath($root, $logPath).Replace('\', '/')
    }
    Write-RenderStackAtomicJson -Path $metadataPath -Value $metadata
} catch {
    if ($firstExitCode -eq 0) {
        $firstExitCode = 1
        $failureMessage = "Build metadata publication failed: $($_.Exception.Message)"
    } else {
        $failureMessage = "$failureMessage; build metadata publication failed: $($_.Exception.Message)"
    }
}

if ($firstExitCode -ne 0) {
    Write-Error "Build failed: $failureMessage"
    exit $firstExitCode
}

Write-Output "Build log: $logPath"
Write-Output "Build metadata: $metadataPath"
exit 0
