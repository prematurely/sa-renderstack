$ErrorActionPreference = 'Stop'
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $root 'backend/dxvk/SA_RENDERSTACK_DEPENDENCIES.toml'

function Invoke-RepositoryGit {
    param(
        [Parameter(Mandatory)]
        [string[]] $GitArguments
    )

    $output = @(& git -C $root @GitArguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "git $($GitArguments -join ' ') failed:`n$($output -join "`n")"
    }

    return $output
}

$dependencies = @(
    [ordered]@{
        Path = 'backend/dxvk/include/native/directx'
        Url = 'https://github.com/Joshua-Ashton/mingw-directx-headers'
        Commit = '9df86f2341616ef1888ae59919feaa6d4fad693d'
        RootTree = '66fcb250507a2c45bd9a22f44795d0e4c7b613f5'
        BuildInput = 'backend/dxvk/include/native/directx/d3d9.h'
    }
    [ordered]@{
        Path = 'backend/dxvk/include/vulkan'
        Url = 'https://github.com/KhronosGroup/Vulkan-Headers'
        Commit = '8864cdc896bbc2a9b6eb36b3218fc9ef57908d77'
        RootTree = '25b446994cd7956846e2d07e9958724d78c7ae3d'
        BuildInput = 'backend/dxvk/include/vulkan/include/vulkan/vulkan.h'
    }
    [ordered]@{
        Path = 'backend/dxvk/include/spirv'
        Url = 'https://github.com/KhronosGroup/SPIRV-Headers.git'
        Commit = '04f10f650d514df88b76d25e83db360142c7b174'
        RootTree = '2883db5426133e951df98616f78904371e854339'
        BuildInput = 'backend/dxvk/include/spirv/include/spirv/unified1/spirv.hpp'
    }
    [ordered]@{
        Path = 'backend/dxvk/subprojects/libdisplay-info'
        Url = 'https://github.com/doitsujin/libdisplay-info.git'
        Commit = '275e6459c7ab1ddd4b125f28d0440716e4888078'
        RootTree = 'af4bbd04360eda1a4634a5aaf5d0408eb71925a1'
        BuildInput = 'backend/dxvk/subprojects/libdisplay-info/meson.build'
    }
    [ordered]@{
        Path = 'backend/dxvk/subprojects/dxbc-spirv'
        Url = 'https://github.com/doitsujin/dxbc-spirv.git'
        Commit = '6da3ed38834b0ae3ae7f4267568181dabc7104bc'
        RootTree = '2c78a92506fe3789d000d42b2dc17cde6fa298a3'
        BuildInput = 'backend/dxvk/subprojects/dxbc-spirv/meson.build'
    }
    [ordered]@{
        Path = 'backend/dxvk/subprojects/dxbc-spirv/submodules/spirv_headers'
        Url = 'https://github.com/KhronosGroup/SPIRV-Headers.git'
        Commit = 'c8ad050fcb29e42a2f57d9f59e97488f465c436d'
        RootTree = '7577966d18e3d5203339a5daa9146f10fe46e785'
        BuildInput = 'backend/dxvk/subprojects/dxbc-spirv/submodules/spirv_headers/include/spirv/unified1/spirv.hpp'
    }
)

$stagedFiles = @(Invoke-RepositoryGit -GitArguments @('ls-files', '--stage'))
$gitlinks = @($stagedFiles | Where-Object { $_ -match '^160000\s' })
if ($gitlinks.Count -gt 0) {
    throw "Active Git dependency links remain:`n$($gitlinks -join "`n")"
}

$rootGitPath = [IO.Path]::GetFullPath((Join-Path $root '.git'))
$nestedGitMetadata = @(
    Get-ChildItem -LiteralPath $root -Force -Recurse -ErrorAction Stop |
        Where-Object {
            $_.Name -eq '.gitmodules' -or
            ($_.Name -eq '.git' -and [IO.Path]::GetFullPath($_.FullName) -ne $rootGitPath)
        } |
        ForEach-Object { $_.FullName }
)
if ($nestedGitMetadata.Count -gt 0) {
    throw "Nested Git metadata remains:`n$($nestedGitMetadata -join "`n")"
}

if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
    throw 'DXVK dependency manifest is missing'
}

$manifestText = Get-Content -LiteralPath $manifest -Raw
$recordMatches = [regex]::Matches(
    $manifestText,
    '(?ms)^\[\[dependency\]\]\r?\n(?<body>.*?)(?=^\[\[dependency\]\]\r?\n|\z)'
)
if ($recordMatches.Count -ne $dependencies.Count) {
    throw "Expected $($dependencies.Count) dependency records, found $($recordMatches.Count)"
}

for ($index = 0; $index -lt $dependencies.Count; $index++) {
    $dependency = $dependencies[$index]
    $recordBody = $recordMatches[$index].Groups['body'].Value
    $requiredLines = @(
        "path = `"$($dependency.Path)`""
        "url = `"$($dependency.Url)`""
        "commit = `"$($dependency.Commit)`""
        "root_tree = `"$($dependency.RootTree)`""
        'import = "git-subtree-squash"'
    )

    foreach ($requiredLine in $requiredLines) {
        if (-not $recordBody.Contains($requiredLine)) {
            throw "Missing dependency manifest entry for $($dependency.Path): $requiredLine"
        }
    }

    $buildInput = Join-Path $root $dependency.BuildInput
    if (-not (Test-Path -LiteralPath $buildInput -PathType Leaf)) {
        throw "DXVK dependency build input is missing: $($dependency.BuildInput)"
    }

    $trailer = "git-subtree-split: $($dependency.Commit)"
    $candidateCommits = @(
        Invoke-RepositoryGit -GitArguments @(
            'log'
            'HEAD'
            '--format=%H'
            '--fixed-strings'
            "--grep=$trailer"
        )
    )
    $syntheticCommits = @(
        $candidateCommits | Where-Object {
            $messageLines = @(Invoke-RepositoryGit -GitArguments @('show', '-s', '--format=%B', $_))
            $messageLines -contains $trailer
        }
    )
    if ($syntheticCommits.Count -ne 1) {
        throw "Expected one synthetic subtree commit for $($dependency.Commit), found $($syntheticCommits.Count)"
    }

    $actualRootTree = @(Invoke-RepositoryGit -GitArguments @(
        'show'
        '-s'
        '--format=%T'
        $syntheticCommits[0]
    ))[0]
    if ($actualRootTree -ne $dependency.RootTree) {
        throw "Unexpected root tree for $($dependency.Path): $actualRootTree"
    }
}

Write-Output 'PASS DXVK dependency provenance'
