# Static contract gate for the seven revisioned GTA-SA compatibility interfaces.
# This intentionally does not mutate or rebuild production sources.  Pass
# -ProbeExe and -DllPath to add an optional GPU/runtime smoke invocation.
param(
    [string]$ProbeExe,
    [string]$DllPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$header = Join-Path $root 'sdk/include/sa_renderstack/backend_api.h'
$compat = Join-Path $root 'backend/dxvk/src/d3d9/d3d9_gta_sa_compat.cpp'
$device = Join-Path $root 'backend/dxvk/src/d3d9/d3d9_device.cpp'
$probe = Join-Path $root 'backend/dxvk/tools/gta_sa_compat_probe.cpp'
foreach ($path in @($header, $compat, $device, $probe)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing contract input: $path" }
}

$h = [IO.File]::ReadAllText($header)
$c = [IO.File]::ReadAllText($compat)
$d = [IO.File]::ReadAllText($device)
$p = [IO.File]::ReadAllText($probe)

function Require([string]$text, [string]$pattern, [string]$name) {
    if ($text -notmatch $pattern) { throw "FAIL $name (pattern '$pattern')" }
    Write-Output "PASS $name"
}

Require $h '#define\s+D3D9_GTA_SA_COMPAT_API_VERSION\s+7u' 'API revision is 7'
$interfaces = @(
    @{ Api = 1; Type = 'ID3D9GtaSaCompatDevice'; Methods = @('GetStatus','GetVulkanInterop') },
    @{ Api = 2; Type = 'ID3D9GtaSaCompatDevice1'; Methods = @('RegisterVulkanPass','UnregisterVulkanPass') },
    @{ Api = 3; Type = 'ID3D9GtaSaCompatDevice2'; Methods = @('SubmitStateBatch') },
    @{ Api = 4; Type = 'ID3D9GtaSaCompatDevice3'; Methods = @('BeginStateJournal','RestoreStateJournal') },
    @{ Api = 5; Type = 'ID3D9GtaSaCompatDevice4'; Methods = @('SubmitEffectStateBatch') },
    @{ Api = 6; Type = 'ID3D9GtaSaCompatDevice5'; Methods = @('SubmitStateDrawBatch') },
    @{ Api = 7; Type = 'ID3D9GtaSaCompatDevice6'; Methods = @('BeginSelectiveStateJournal','SetStateJournalCaptureEnabled') }
)
foreach ($item in $interfaces) {
    Require $h ('MIDL_INTERFACE\("[0-9a-f-]+"\)\s+' + $item.Type) ("API$($item.Api) $($item.Type) declaration")
    foreach ($method in $item.Methods) {
        Require $h ("virtual HRESULT STDMETHODCALLTYPE\s+" + $method + '\s*\(') ("API$($item.Api) $method declaration")
        Require $c ("D3D9GtaSaCompatDevice::" + $method + '\s*\(') ("API$($item.Api) $method implementation")
        # The legacy GPU probe exercises API1-6 transactions. API7 is a
        # selective-journal control surface and is checked below for explicit
        # implementation plus capability wiring; a future runtime probe can
        # opt into the two controls without changing this contract gate.
        if ($item.Api -le 6) {
            Require $p ([regex]::Escape($method) + '\s*\(') ("API$($item.Api) $method probe call")
        }
    }
}

$flags = @(
    'ACTIVE','VULKAN_BACKEND','VULKAN_INTEROP','FRAME_LIFECYCLE','DEVICE_READY',
    'PASS_REGISTRY','COMMAND_RECORD','STATE_BATCH','STATE_JOURNAL',
    'EFFECT_STATE_BATCH','STATE_DRAW_BATCH','SELECTIVE_STATE_JOURNAL'
)
foreach ($flag in $flags) {
    Require $h ("D3D9_GTA_SA_COMPAT_$flag\s*=\s*1u\s*<<") "capability flag $flag"
}
Require $c 'D3D9_GTA_SA_COMPAT_DEVICE_READY' 'DEVICE_READY lifecycle flag is set by OnDeviceReady'
Require $c 'OnDeviceReady\s*\(' 'device-ready lifecycle hook'
Require $c 'OnResetBegin\s*\(' 'reset-begin lifecycle hook'
Require $c 'OnResetEnd\s*\(' 'reset-end lifecycle hook'
Require $c 'OnPresent\s*\(' 'present lifecycle hook'
Require $c 'OnDeviceDestroy\s*\(' 'destroy lifecycle hook'
Require $c 'BeginGtaSaStateJournal\(true\)' 'API7 selective journal forwards selective=true'
Require $c 'SetGtaSaStateJournalCaptureEnabled\(Enable\)' 'API7 capture control forwards to device'
Require $p 'api=%u\s+flags=0x%08x' 'probe reports API version and flags'
Require $p 'vulkan_pass_calls=.*order=' 'probe verifies pass execution/order'
Require $p 'slow_unregister=.*callback_finished=' 'probe verifies unregister waits for callback'
Require $p 'vulkan_interop=.*result=' 'probe verifies Vulkan interop'

if ($ProbeExe -or $DllPath) {
    if (-not ($ProbeExe -and $DllPath)) { throw 'Runtime mode requires both -ProbeExe and -DllPath' }
    $output = & $ProbeExe $DllPath '--force' 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Runtime probe failed with exit $LASTEXITCODE`n$output" }
    foreach ($pattern in @(
        'api=7\s+flags=0x[0-9a-fA-F]+',
        'vulkan_pass_calls=6\s+order=123123',
        'slow_unregister=yes\s+elapsed_ms=\d+\s+callback_finished=yes',
        'vulkan_interop=yes\s+result=0x0'
    )) {
        if ($output -notmatch $pattern) { throw "Runtime probe missing '$pattern'`n$output" }
    }
    Write-Output 'PASS API1-7 runtime smoke'
}

Write-Output 'PASS GTA-SA API1-7 contract gate'
