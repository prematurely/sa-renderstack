# GTA San Andreas compatibility runtime

This fork keeps the complete Direct3D 9 ABI expected by `gta_sa.exe` while
using DXVK and Vulkan as the rendering backend. The runtime is enabled by the
built-in `gta_sa.exe` application profile and can be overridden in `dxvk.conf`:

```ini
d3d9.gtaSaCompat = True
d3d9.gtaSaCompatDiagnostics = True
```

The compatibility runtime currently adds:

- `ID3D9GtaSaCompatDevice`, queried from the DXVK D3D9 device;
- a stable API version and capability flags;
- access to the existing `ID3D9VkInteropDevice` interface;
- device-ready, Reset, Present, and destruction lifecycle tracking;
- an API v2 registry that records ordered native passes into DXVK's existing
  Present command buffer;
- no additional Vulkan submissions and no visual-setting overrides.

The public interface is declared in `include/d3d9_gta_sa_api.h`. Native Vulkan
passes are ordered by priority and registration sequence. They run after the
DXVK swapchain blit and share one command buffer. Individual plugins must not
flush D3D9, lock the DXVK submission queue, submit, end, or reset that command
buffer. Each pass must restore the supplied source and output image layouts.
Pass recording is serialized with unregistration, so callback code and data
may be unloaded after `UnregisterVulkanPass` returns. A pass must not register
or unregister passes from inside its own record callback.

## Probe

Build `tools/gta_sa_compat_probe.cpp` as a 32-bit executable. Use `--force` when
the executable is not named `gta_sa.exe`; omit it to validate the built-in
application profile.

The probe creates a D3D9 device, presents, resets, presents again, queries the
compatibility status, verifies Vulkan interop, validates priority order and
checks that unregistration waits for an in-flight record callback.
