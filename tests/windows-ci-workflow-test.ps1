$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:GIT_CONFIG_GLOBAL = 'NUL'

$root = Split-Path -Parent $PSScriptRoot
$workflowPath = Join-Path $root '.github/workflows/windows-ci.yml'

function Assert-Contains {
    param(
        [Parameter(Mandatory)] [string]$Text,
        [Parameter(Mandatory)] [string]$Pattern,
        [Parameter(Mandatory)] [string]$Description
    )

    if ($Text -notmatch $Pattern) {
        throw "Windows CI workflow is missing ${Description}: $Pattern"
    }
}

if (-not (Test-Path -LiteralPath $workflowPath -PathType Leaf)) {
    throw "Windows CI workflow is missing: $workflowPath"
}

$workflow = Get-Content -LiteralPath $workflowPath -Raw

Assert-Contains -Text $workflow -Pattern '(?m)^name:\s*Windows CI\s*$' -Description 'workflow name'
Assert-Contains -Text $workflow -Pattern '(?m)^\s*pull_request:\s*$' -Description 'pull_request trigger'
Assert-Contains -Text $workflow -Pattern '(?m)^\s*workflow_dispatch:\s*$' -Description 'workflow_dispatch trigger'
Assert-Contains -Text $workflow -Pattern '(?ms)^\s*push:\s*\r?\n(?:\s+[^\r\n]*\r?\n)*?\s+branches:\s*\r?\n\s+-\s+main\s*$' -Description 'push to main trigger'
Assert-Contains -Text $workflow -Pattern '(?m)^\s*runs-on:\s*windows-2025\s*$' -Description 'Windows 2025 runner'
Assert-Contains -Text $workflow -Pattern '(?ms)^permissions:\s*\r?\n\s+contents:\s*read\s*$' -Description 'read-only contents permission'
Assert-Contains -Text $workflow -Pattern '(?m)^\s*timeout-minutes:\s*60\s*$' -Description '60-minute timeout'
Assert-Contains -Text $workflow -Pattern '(?m)^\s*cancel-in-progress:\s*true\s*$' -Description 'run cancellation policy'

Assert-Contains -Text $workflow -Pattern 'https://github\.com/mstorsjo/llvm-mingw/releases/download/20260602/llvm-mingw-20260602-msvcrt-i686\.zip' -Description 'pinned LLVM-MinGW URL'
Assert-Contains -Text $workflow -Pattern '2c2ced6587900fd0a4ea27d1215d5ae3176ef136da0287acae7f2881b5da4a3e' -Description 'pinned LLVM-MinGW SHA-256'

foreach ($token in @(
        'tools/build\.ps1',
        'tools/test\.ps1',
        'tools/package\.ps1',
        'tests/package-layout-test\.ps1',
        '-AllowNonV18MsBuild',
        '-SkipLocalBridgeEvidence',
        'actions/checkout@v4',
        'actions/setup-python@v5',
        'microsoft/setup-msbuild@v2',
        'msys2/setup-msys2@v2',
        'actions/cache@v4',
        'actions/upload-artifact@v4',
        'if-no-files-found:\s*error')) {
    Assert-Contains -Text $workflow -Pattern $token -Description "required token '$token'"
}

if ($workflow -match '(?i)release-gate\.ps1|gh\s+release\s+create|softprops/action-gh-release|git\s+tag|git\s+push') {
    throw 'Windows CI workflow contains a release/tag operation that belongs to the local release process'
}
if ($workflow -match '(?i)D:\\GTA San Andreas|game-root|game directory') {
    throw 'Windows CI workflow refers to the local GTA game installation'
}
if ($workflow -match '(?i)out[/\\]reports[/\\]phase-1-release-gate\.md') {
    throw 'Windows CI workflow must not upload the local release-gate report'
}
if ($workflow -notmatch '(?m)^\s*if-no-files-found:\s*warn\s*$') {
    throw 'Windows CI workflow must upload optional logs with if-no-files-found: warn'
}

$requiredPackagePaths = @(
    'out/packages/SA-RenderStack-v${{ steps.version.outputs.value }}-split.zip',
    'out/packages/SA-RenderStack-v${{ steps.version.outputs.value }}-sdk.zip',
    'out/packages/SA-RenderStack-v${{ steps.version.outputs.value }}-symbols.zip',
    'out/packages/SA-RenderStack-v${{ steps.version.outputs.value }}-source-manifest.json',
    'out/test-results.json'
)
foreach ($path in $requiredPackagePaths) {
    if (-not $workflow.Contains($path, [StringComparison]::Ordinal)) {
        throw "Windows CI workflow is missing required artifact path: $path"
    }
}

Write-Output 'PASS Windows CI workflow contract'
exit 0
