function Read-ControlPathSamples {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [Collections.Generic.HashSet[string]]$Expected,
        [Parameter(Mandatory)] [int]$Rounds,
        [Parameter(Mandatory)] [int]$Seed,
        [Parameter(Mandatory)] [string]$Profile,
        [Parameter(Mandatory)] [string]$Cpu,
        [Parameter(Mandatory)] [bool]$SharedDevice
    )
    $seen = [Collections.Generic.HashSet[string]]::new()
    $samples = [Collections.Generic.List[object]]::new()
    $metadata = $null
    $completed = $false
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($completed) {throw 'Output after completion'}
        if ($line -match '^checks=(\d+) failures=0$') {
            $checks = if ($SharedDevice) {127} else {254}
            if ([int]$Matches[1] -ne $checks) {throw 'Missing behavioral checks'}
            $completed = $true
            continue
        }
        $row = $line | ConvertFrom-Json -ErrorAction Stop
        if ($row.type -eq 'metadata') {
            if ($null -ne $metadata -or $samples.Count -ne 0 -or
                $row.rounds -ne $Rounds -or $row.seed -ne $Seed -or $row.profile -cne $Profile -or
                $row.sharedDevice -isnot [bool] -or $row.sharedDevice -ne $SharedDevice -or
                $row.pinned -isnot [bool] -or $row.pinned -ne ($Cpu -ne 'any')) {throw 'Wrong metadata'}
            foreach ($base in @($row.baselineBase,$row.candidateBase)) {
                if ($base -isnot [string] -or $base -notmatch '^(?:0x)?[0-9a-fA-F]{1,16}$' -or
                    [Convert]::ToUInt64(($base -replace '^0x',''),16) -eq 0) {throw 'Invalid module base'}
            }
            $sameBase = [Convert]::ToUInt64(($row.baselineBase -replace '^0x',''),16) -eq
                [Convert]::ToUInt64(($row.candidateBase -replace '^0x',''),16)
            if ($sameBase -ne $SharedDevice) {throw 'Wrong module topology'}
            $metadata = $row
            continue
        }
        if ($null -eq $metadata -or $row.type -cne 'sample' -or !$Expected.Contains($row.case) -or
            $row.variant -cnotin @('baseline','candidate') -or $row.round -isnot [long] -or
            $row.round -lt 0 -or $row.round -ge $Rounds) {throw 'Unexpected sample'}
        if (!$seen.Add("$($row.case)|$($row.variant)|$($row.round)")) {throw 'Duplicate sample'}
        $iterations = if ($row.case.StartsWith('setter,')) {300000} else {16000}
        foreach ($value in @($row.ns,$row.cycles)) {
            if (($value -isnot [double] -and $value -isnot [long]) -or
                [double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -le 0) {throw 'Invalid timing'}
        }
        if ($row.iterations -ne $iterations -or $row.cpuStart -isnot [long] -or $row.cpuStart -lt 0 -or
            $row.cpuEnd -isnot [long] -or $row.cpuEnd -lt 0) {throw 'Invalid iterations/CPU'}
        if ($Cpu -ne 'any' -and ($row.cpuStart -ne [int]$Cpu -or $row.cpuEnd -ne [int]$Cpu)) {throw 'Pinned CPU mismatch'}
        $samples.Add($row)
    }
    if ($null -eq $metadata -or !$completed -or $seen.Count -ne 2 * $Rounds * $Expected.Count) {throw 'Incomplete output'}
    return [pscustomobject]@{metadata=$metadata;samples=@($samples);checks=$checks}
}
