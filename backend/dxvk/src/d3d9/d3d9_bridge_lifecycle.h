#pragma once

#include <d3d9.h>

// Private Bridge/backend integration. This is independent of public API1-7.
// Callbacks run synchronously on the calling device thread. The observer must
// outlive registration, must not retain the device, and may not throw. Clear
// registration serializes with callbacks. FinalRelease runs while the device
// is still usable, before its final public reference is retired.
// On multithreaded devices notification and registration take the recursive
// device lock before the observer lock, allowing same-device API reentry.
inline constexpr GUID IID_D3D9BridgeLifecycle =
  {0x36f6d3a1,0xc28c,0x4dd0,{0x91,0xaf,0x23,0x1e,0x8b,0xa0,0xb2,0x01}};
inline constexpr GUID IID_D3D9BridgeRuntime =
  {0x36f6d3a1,0xc28c,0x4dd0,{0x91,0xaf,0x23,0x1e,0x8b,0xa0,0xb2,0x02}};

enum class D3D9BridgeEvent : UINT {
  PresentBefore, PresentAfter, ResetBefore, ResetAfter, FinalRelease
};

struct D3D9BridgePresentArgs {
  const RECT* Source;
  const RECT* Destination;
  HWND Window;
  const RGNDATA* DirtyRegion;
};

struct D3D9BridgeObserver {
  UINT StructSize;
  void* Context;
  void (STDMETHODCALLTYPE* Notify)(void*, IDirect3DDevice9*, D3D9BridgeEvent,
    HRESULT, D3DPRESENT_PARAMETERS*, const D3D9BridgePresentArgs*);
};

struct ID3D9BridgeLifecycle : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetObserver(const D3D9BridgeObserver*) = 0;
};

// Query on the Bridge IDirect3D9 factory. Shutdown is explicit and must be
// called outside callbacks/loader lock, after releasing all created devices.
// It returns D3DERR_INVALIDCALL while a device is live and E_FAIL when rollback
// needs a retry, E_PENDING for a concurrent shutdown, and S_FALSE after completed
// shutdown. An initialized Bridge stays module-pinned for stale hooks.
struct ID3D9BridgeRuntime : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};
