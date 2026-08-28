# BridgeD3D9 ProxyChain

BridgeD3D9 is intended to be the only `d3d9.dll` entry point. Other D3D9
wrappers should be declared as managed proxy entries instead of chaining
themselves through file renames.

The target chain is:

```text
gta_sa.exe
  -> d3d9.dll                    BridgeD3D9 manager
     -> DXVK D3D9/Vulkan         backend owner
     -> observed internal proxy  ProperShaders.asi
     -> managed proxy adapters   ReShade, ENB, ...
```

## Rules

- Earlier proxy entries get first claim.
- Later entries that request an already-owned claim are skipped by default.
- A proxy can be disabled without removing it from the order list.
- A proxy can be marked required for diagnostics, but BridgeD3D9 will still
  avoid aborting the game loader.
- BridgeD3D9 logs a final claim ownership table so conflicts are visible.
- Modern post-processing should prefer Bridge-native plugins over another D3D9
  wrapper layer.
- `ObservedASI` records an ASI module's claims without calling `LoadLibrary`
  from the D3D9 loader. The normal ASI loader remains responsible for it.

This is closer to modloader than a traditional proxy chain: entries declare what
they want to own, and the manager decides whether loading them is safe.

## Claims

| Claim | Meaning |
|---|---|
| `D3D9Entry` | Wants to own `Direct3DCreate9` / `Direct3DCreate9Ex` path. |
| `DeviceWrap` | Wraps `IDirect3D9` or `IDirect3DDevice9`. |
| `EndScene` | Uses the `EndScene` post-process point. |
| `Present` | Uses the `Present` post-process point. |
| `Depth` | Needs depth buffer access or depth-related state. |
| `ShaderPatch` | Modifies shader creation/selection. |
| `DXGI` | Wants DXGI factory/export ownership. |
| `Backend` | Wants final graphics backend ownership. |

## Stages

| Stage | Meaning |
|---|---|
| `D3D9Create` | Loaded before `Direct3DCreate9` ownership is resolved. Use only for real external D3D9 wrapper DLLs. |
| `PostDevice` | Loaded after the D3D9 device exists. Intended for Bridge-native adapters and post-process plugins. |
| `Auto` | Let BridgeD3D9 infer the stage from `Type`. |

## Example

```ini
[Backend]
UseDxvkBackend=1
DxvkBackendDir=dxvk

[ProxyChain]
Enable=1
LegacyAutoProbeD3D9PS=0
Order=ProperShaders;ReShade;ENB

[Proxy.ProperShaders]
Enable=1
Type=ObservedASI
Path=modloader\Proper Shaders\ProperShaders.asi
Mode=InternalDeviceProxy
Stage=PostDevice
Claims=DeviceWrap;Depth;ShaderPatch
ConflictPolicy=SkipIfClaimed
Required=0

[Proxy.ReShade]
Enable=0
Type=PostProcess
Path=ReShade32.dll
Mode=PresentHook
Stage=PostDevice
Claims=Present;EndScene
ConflictPolicy=SkipIfClaimed
Required=0

[Proxy.ENB]
Enable=0
Type=D3D9Proxy
Path=enbseries.dll
Mode=Compatibility
Stage=D3D9Create
Claims=D3D9Entry;DeviceWrap;Present;EndScene;Depth;ShaderPatch
ConflictPolicy=SkipIfClaimed
Required=0
```

## Conflict Policies

| Policy | Behavior |
|---|---|
| `SkipIfClaimed` | Default. Skip this proxy if an earlier loaded proxy owns any requested claim. |
| `AllowShared` | Allow loading even when claims overlap. Use only for known-safe adapters. |
| `ReplaceEarlier` | Let this proxy replace earlier claims. Intended for debugging, not normal packs. |

## Bridge-Native PostFX Plugins

Bridge-native plugins are separate from legacy D3D9 proxy DLLs. Enable them in
`[PostFX]` only when a plugin is built for `BridgeD3D9Plugin.h`. The list is
loaded in order; semicolons or commas separate file names. DLL loading is
deferred until the first `Direct3DCreate9` call so plugin initialization runs
outside the Windows loader lock.

```ini
[PostFX]
EnableHost=1
PluginDir=plugins\d3d9chain
Plugins=MyPostFx.dll
```

### Legacy API v1

Existing API v1 plugins remain binary-compatible. They receive
`BridgeD3D9PluginApi` with `apiVersion=1` and may export these D3D9 lifecycle
callbacks:

```cpp
BridgeD3D9_PluginInit
BridgeD3D9_PluginShutdown
BridgeD3D9_OnCreateDevice
BridgeD3D9_OnResetBefore
BridgeD3D9_OnResetAfter
BridgeD3D9_OnEndScene
BridgeD3D9_OnPresentBefore
BridgeD3D9_OnPresentAfter
BridgeD3D9_OnReleaseDevice
```

### Native Vulkan API v2

API v2 plugins export `BridgeD3D9_PluginInit2` and receive a
`BridgeD3D9PluginApi2`. Copy the structure during initialization; the pointer
itself is temporary. A plugin implementation must define
`BRIDGE_D3D9_PLUGIN_IMPLEMENTATION` before including the header so its exported
function names do not conflict with legacy host-side type aliases.

```cpp
#define BRIDGE_D3D9_PLUGIN_IMPLEMENTATION
#include "BridgeD3D9Plugin.h"

static BridgeD3D9PluginApi2 g_api;

extern "C" __declspec(dllexport) BOOL __stdcall
BridgeD3D9_PluginInit2(const BridgeD3D9PluginApi2* api) {
    if (!api || api->apiVersion < BRIDGE_D3D9_PLUGIN_API_VERSION_2 ||
        api->structSize < sizeof(*api))
        return FALSE;
    g_api = *api;
    return TRUE;
}
```

`RegisterVulkanPass` accepts multiple passes per plugin. Lower `Priority`
values run first; equal priorities preserve registration order. Passes execute
after DXVK's swapchain blit in the existing Present command buffer, so they do
not add a queue submission or a D3D9 flush. The Bridge queues registrations
made before device creation, binds them when the GTA DXVK API becomes ready,
temporarily unbinds them across Reset, and unregisters them before releasing
the device or unloading the plugin.

A record callback must only record Vulkan commands. It must not call D3D9,
submit/end/reset the command buffer, call the registration API recursively, or
leave the supplied images in different layouts. One failing callback is
disabled by DXVK without disabling other passes. Unregistration waits for an
in-flight callback to finish.

The v1 and v2 probes in `tests/` exercise legacy callbacks, priority ordering,
failure isolation, Reset rebinding, and in-flight unregistration.
