# SA RenderStack Module Map

## Attached API subprojects

`src/bridge/legacy/api-projects/` contains seven API-version subprojects owned by
the Bridge main program. Their library implementations are compiled into the
Bridge Win32 projects; demos and unit tests are development targets from the
aggregate CMake file. They are separate code units, not separate runtime DLLs.

## Runtime Graph

```text
gta_sa.exe (GTA SA 1.0.0.0 US, x86)
    |
    | loads root d3d9.dll
    v
Bridge D3D9 proxy
    |  src/bridge/legacy/BridgeD3D9.cpp
    |  reads SA.RenderStack.ini / scripts/BridgeD3D9.ini
    |  owns proxy registry, diagnostics, adapters, and compatibility API
    v
DXVK GTA backend
    |  backend/dxvk-gta/d3d9.dll in the runtime package
    |  backend/dxvk/src/d3d9/* in the source tree
    |  D3D9 ABI -> DXVK device -> Vulkan command submission
    v
Vulkan driver and GPU
```

The package keeps the Bridge and backend as separate DLLs. The backend is
selected by `Backend=DXVK` and `DxvkBackendDir=backend\dxvk-gta`. A separate
DXGI runtime DLL is not selected in this profile; DXGI support is part of the
merged DXVK backend build.

## Ownership Boundaries

| Area | Owner | Contract |
| --- | --- | --- |
| D3D9 entry point and proxy chain | Bridge | Loads the configured primary backend and optional registered modules. |
| D3D9 implementation | DXVK backend | Implements the exported D3D9 interfaces expected by the game. |
| Vulkan device and queue | DXVK backend | Owns Vulkan object lifetime and command submission. |
| Third-party module observation | Bridge adapter | Observes configured ASI/device claims; it does not add a second D3D9 proxy. |
| ReShade/ENB entries | Bridge registry metadata | Disabled placeholders unless the external component is installed and explicitly enabled. |
| Native post-process passes | Bridge host plus DXVK API | Registers ordered callbacks into the existing Present command buffer. |
| Performance diagnostics | Bridge | F7/F8/F9/F10 captures and config snapshots; disabled or read-only by default. |
| Runtime configuration | `SA.RenderStack.ini` and `dxvk.conf` | Selects ownership and diagnostic behavior without changing build outputs. |

## Bridge Registry

The `[ProxyChain]` section enables the ordered registry. The `[register]`
section gives numeric order and each named section describes the component:

```ini
[register]
1=ProperShaders
2=ReShade
3=ENB
```

Each registered section declares its type, path (`dll=` or `asi=`), lifecycle
stage, claimed hooks, and `ConflictPolicy`. Missing optional modules are
skipped. A later module that claims an already-owned hook is skipped according
to `ConflictPolicy=SkipIfClaimed`. This registry is metadata-driven; it does
not turn an absent ReShade or ENB installation into a loaded module.

Paths without a drive prefix are resolved below the GTA game root. An absolute
path is accepted for a deliberately external component. The registry is not a
general-purpose loader for arbitrary untrusted code.

## Public API Layers

There are two separate versioned contracts:

### Bridge plugin API

Declared in `src/bridge/legacy/BridgeD3D9Plugin.h`:

- API v1: logging and D3D9 lifecycle callbacks.
- API v2: Vulkan status, ordered pass registration, and pass unregistration.

### GTA compatibility API

Declared in `src/bridge/legacy/d3d9_gta_sa_api.h` and mirrored for the DXVK
backend. `GtaSaCompatApiVersions.h` records the feature floors:

| Version | Feature |
| ---: | --- |
| 2 | Vulkan pass registration |
| 3 | State batch submission |
| 4 | Native state journal |
| 5 | Effect state batch |
| 6 | State-plus-draw batch |
| 7 | Selective state journal |

Consumers must check `StructSize`, `ApiVersion`, capability flags, and the
returned HRESULT before using a newer interface. Older consumers remain
valid when the runtime advertises a newer compatible version.

## Native Vulkan Pass Rules

The backend records registered passes into DXVK's existing Present command
buffer. Pass order is priority first and registration order for equal
priorities. A pass callback:

- must use the supplied frame context;
- must restore the source and output image layouts it changes;
- must not flush, submit, end, reset, or lock the DXVK command buffer;
- must not register or unregister a pass from inside its own callback.

Unregistration waits for an in-flight callback before returning, so a plugin
can release its callback data after `UnregisterVulkanPass` completes.

## D3D9 Optimization Ownership

The GTA compatibility overlay is inside the DXVK D3D9 backend. It contains
state-block prefiltering, state-block fast-skip, deferred shader/scalar
bindings, input-layout caching, push-data and resource-binding coalescing, and
the API v7 selective state-journal path. These switches are `d3d9.gtaSa*`
options in `config/dxvk.conf`.

The Bridge-side third-party effect journal remains a compatibility and
observation layer. The current profile exercises it through a configured
external integration. It must not maintain a stale cache that bypasses state changes made by
DXVK-native restores or unwrapped state blocks. Experimental direct-effect
and batch modes remain disabled unless a separate test proves them safe.

## Build and Package Ownership

- `tools/build.ps1` builds Bridge, DXVK, probes, tests, and build metadata.
- `tools/test.ps1` runs source, provenance, runtime, ABI, Bridge, and export
  tests.
- `packaging/split/package.toml` is the authoritative runtime file mapping.
- `tools/package.ps1` stages the split, SDK, and symbols archives and writes
  manifests.
- `tools/release-gate.ps1` verifies the exact release contract.

None of these scripts deploys files to the game root. End-user installation is
the archive extraction procedure in `docs/installation.md`.
