#pragma once

// ProperShaders render-state guard.
//
// Migrated from FLA++ (FLACompatBridge/BridgeCompatProperShaders.cpp) so the
// d3d9 layer owns the render-state contract with ProperShaders: the three
// internal ProperShaders patch sites are wrapped, RenderWare's alpha-test
// states are re-synchronised onto the D3D9 device, and sampled state
// transitions can be recorded into a fixed-size state ring.
//
// The port is behaviour-preserving: same patch sites, same signature bytes,
// same state tables and limits, same log text. FLA++ keeps the AddTxdSlot
// arbitration, the CStreamingInfo address rewrites and the fixed PS+0x50C1F
// patch, none of which belong to the render-state contract.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstddef>
#include <cstdint>

// ---- configuration (BridgeD3D9.ini, migrated from FLACompatBridge.ini) ----
struct ProperShadersRenderStateGuardConfig
{
    bool enable = true;                    // [ProperShadersRenderStateGuard] Enable
    bool enableStateRing = false;          // [ProperShadersStateRing] Enable
    int stateRingTriggerVirtualKey = 121;  // [ProperShadersStateRing] TriggerVirtualKey (VK_F10)
};

extern ProperShadersRenderStateGuardConfig g_properShadersRenderStateGuardConfig;

void LoadProperShadersRenderStateGuardConfig(const char* iniPath);

// ---- activation (called once the D3D9 device exists) ----
void StartProperShadersRenderStateGuardWatch();

// ---- state-ring lifecycle (process detach) ----
void ShutdownProperShadersStateRing();

// ---- constants ----
// Batch 5: re-audited against the 2026-07-03 ProperShaders build
// (1,534,592 bytes). The 0x4E3711C9 constant matched the 2026-06-02 build;
// since the 07-15 file swap every PS-gated patch was silently skipping.
constexpr uint32_t kSupportedProperShadersTextHash = 0x526DBD01;
constexpr uintptr_t kRwD3D9SetRenderState = 0x007FC2D0;
constexpr uintptr_t kRwD3D9GetRenderState = 0x007FC320;
constexpr uintptr_t kRwD3D9DevicePointer = 0x00C97C28;
// IDirect3DDevice9 vtable slots (0-based): SetRenderState=57, GetRenderState=58.
// The FLA++ original resolved BOTH entries from the get slot, which made every
// device-side "set" call a no-op GetRenderState. Batch 4 fixes the set entry to
// resolve from its own slot (behaviour change vs the original, dormant while
// the text-hash gate rejects the current ProperShaders build).
constexpr size_t kD3D9SetRenderStateVtableIndex = 57;
constexpr size_t kD3D9GetRenderStateVtableIndex = 58;
constexpr uint32_t kProperShadersAlphaRenderStates[] = {
    15,  // D3DRS_ALPHATESTENABLE
    19,  // D3DRS_SRCBLEND
    20,  // D3DRS_DESTBLEND
    24,  // D3DRS_ALPHAREF
    25,  // D3DRS_ALPHAFUNC
    27,  // D3DRS_ALPHABLENDENABLE
    171, // D3DRS_BLENDOP
    206, // D3DRS_SEPARATEALPHABLENDENABLE
    207, // D3DRS_SRCBLENDALPHA
    208, // D3DRS_DESTBLENDALPHA
    209, // D3DRS_BLENDOPALPHA
};
constexpr size_t kProperShadersAlphaRenderStateCount =
    sizeof(kProperShadersAlphaRenderStates) / sizeof(kProperShadersAlphaRenderStates[0]);
using D3D9SetRenderStateFn = LONG(__stdcall*)(void*, uint32_t, uint32_t);
using D3D9GetRenderStateFn = LONG(__stdcall*)(void*, uint32_t, uint32_t*);
struct D3D9RenderStateApi
{
    void* device = nullptr;
    D3D9SetRenderStateFn setRenderState = nullptr;
    D3D9GetRenderStateFn getRenderState = nullptr;
};
struct ProperShadersNamedD3D9State
{
    uint32_t state = 0;
    const char* name = nullptr;
};
constexpr ProperShadersNamedD3D9State kProperShadersTraceRenderStates[] = {
    {7, "zEnable"},
    {14, "zWrite"},
    {15, "alphaTest"},
    {19, "srcBlend"},
    {20, "destBlend"},
    {22, "cull"},
    {23, "zFunc"},
    {24, "alphaRef"},
    {25, "alphaFunc"},
    {27, "alphaBlend"},
    {28, "fog"},
    {52, "stencil"},
    {152, "clipPlanes"},
    {168, "colorWrite"},
    {171, "blendOp"},
    {175, "slopeDepthBias"},
    {195, "depthBias"},
    {206, "separateAlpha"},
    {207, "srcBlendAlpha"},
    {208, "destBlendAlpha"},
    {209, "blendOpAlpha"},
};
constexpr size_t kProperShadersTraceRenderStateCount =
    sizeof(kProperShadersTraceRenderStates) / sizeof(kProperShadersTraceRenderStates[0]);
constexpr ProperShadersNamedD3D9State kProperShadersTraceTextureStageStates[] = {
    {1, "colorOp"},
    {2, "colorArg1"},
    {3, "colorArg2"},
    {4, "alphaOp"},
    {5, "alphaArg1"},
    {6, "alphaArg2"},
    {11, "texCoordIndex"},
    {24, "texTransform"},
};
constexpr size_t kProperShadersTraceTextureStageStateCount =
    sizeof(kProperShadersTraceTextureStageStates) /
    sizeof(kProperShadersTraceTextureStageStates[0]);
constexpr ProperShadersNamedD3D9State kProperShadersTraceSamplerStates[] = {
    {1, "addressU"},
    {2, "addressV"},
    {5, "magFilter"},
    {6, "minFilter"},
    {7, "mipFilter"},
    {8, "mipLodBias"},
    {9, "maxMipLevel"},
    {10, "maxAniso"},
};
constexpr size_t kProperShadersTraceSamplerStateCount =
    sizeof(kProperShadersTraceSamplerStates) / sizeof(kProperShadersTraceSamplerStates[0]);
constexpr const char* kProperShadersAlphaRenderStateNames[] = {
    "alphaTest", "srcBlend", "destBlend", "alphaRef", "alphaFunc", "alphaBlend",
    "blendOp", "separateAlpha", "srcBlendAlpha", "destBlendAlpha", "blendOpAlpha",
};
struct ProperShadersMainSceneBaseline
{
    bool valid = false;
    bool stencilValid = false;
    uint32_t threadId = 0;
    uint32_t alphaTest = 0;
    uint32_t alphaRef = 0;
    uint32_t alphaFunc = 0;
    uint32_t stencilEnable = 0;
};
constexpr size_t kProperShadersMainSceneBaselineCapacity = 8;
struct ProperShadersD3D9Viewport
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float minZ = 0.0f;
    float maxZ = 0.0f;
};
using D3D9GetRenderTargetFn = LONG(__stdcall*)(void*, uint32_t, void**);
using D3D9GetDepthStencilSurfaceFn = LONG(__stdcall*)(void*, void**);
using D3D9GetViewportFn = LONG(__stdcall*)(void*, ProperShadersD3D9Viewport*);
using D3D9GetTextureFn = LONG(__stdcall*)(void*, uint32_t, void**);
using D3D9GetTextureStageStateFn = LONG(__stdcall*)(void*, uint32_t, uint32_t, uint32_t*);
using D3D9GetSamplerStateFn = LONG(__stdcall*)(void*, uint32_t, uint32_t, uint32_t*);
using D3D9GetVertexShaderFn = LONG(__stdcall*)(void*, void**);
using D3D9GetPixelShaderFn = LONG(__stdcall*)(void*, void**);
using D3D9ReleaseFn = ULONG(__stdcall*)(void*);
struct D3D9StateTraceApi
{
    void* device = nullptr;
    D3D9GetRenderStateFn getRenderState = nullptr;
    D3D9GetRenderTargetFn getRenderTarget = nullptr;
    D3D9GetDepthStencilSurfaceFn getDepthStencilSurface = nullptr;
    D3D9GetViewportFn getViewport = nullptr;
    D3D9GetTextureFn getTexture = nullptr;
    D3D9GetTextureStageStateFn getTextureStageState = nullptr;
    D3D9GetSamplerStateFn getSamplerState = nullptr;
    D3D9GetVertexShaderFn getVertexShader = nullptr;
    D3D9GetPixelShaderFn getPixelShader = nullptr;
};
struct ProperShadersD3D9StateSnapshot
{
    uint32_t validMask = 0;
    void* device = nullptr;
    uint32_t renderStates[kProperShadersTraceRenderStateCount]{};
    uint32_t textureStageStates[kProperShadersTraceTextureStageStateCount]{};
    uint32_t samplerStates[kProperShadersTraceSamplerStateCount]{};
    void* texture0 = nullptr;
    void* vertexShader = nullptr;
    void* pixelShader = nullptr;
    void* renderTarget0 = nullptr;
    void* depthStencil = nullptr;
    ProperShadersD3D9Viewport viewport{};
    uint32_t phaseAlternate = UINT32_MAX;
    uint32_t phaseByte9 = UINT32_MAX;
    uint32_t phaseByte10 = UINT32_MAX;
};
enum ProperShadersStateTraceKind : uint32_t
{
    kProperShadersStateTraceRenderScene = 1,
    kProperShadersStateTraceFadingEntities = 2,
};
struct ProperShadersStateTraceRecord
{
    uint32_t sequence = 0;
    uint32_t kind = 0;
    uint32_t threadId = 0;
    uint32_t snapshotCount = 0;
    uint64_t beginTick = 0;
    uint64_t endTick = 0;
    uint32_t rwValidMask = 0;
    uint32_t flags = 0;
    uint32_t deviceChangedMask = 0;
    uint32_t rwStates[3][kProperShadersAlphaRenderStateCount]{};
    ProperShadersD3D9StateSnapshot snapshots[3]{};
};
constexpr size_t kProperShadersStateTraceCapacity = 2048;
constexpr size_t kProperShadersStateTraceBytes =
    kProperShadersStateTraceCapacity * sizeof(ProperShadersStateTraceRecord);
struct ProperShadersRwAlphaState
{
    uint32_t alphaFunction = 0;
    uint32_t alphaReference = 0;
};

// ---- capture primitives shared with the state ring ----
bool CaptureProperShadersAlphaRenderStates(uint32_t* values);
bool CaptureProperShadersD3D9StateSnapshot(ProperShadersD3D9StateSnapshot* snapshot);
