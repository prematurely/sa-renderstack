$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$forbidden = Get-ChildItem -LiteralPath $root -Recurse -Force -File |
    Where-Object {
        $relative = [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/')
        $relative -notmatch '^(\.git|\.superpowers|out)/' -and
        ($_.Extension -in @('.bak', '.log', '.exe', '.dll', '.pdb', '.obj', '.o') -or
         $_.Name -eq '.wraplock')
    }

if ($forbidden) {
    $paths = $forbidden.FullName -replace [regex]::Escape($root + '\'), ''
    throw "Forbidden source-tree artifacts:`n$($paths -join "`n")"
}

Write-Output 'PASS source tree hygiene'
