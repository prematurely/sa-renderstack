#pragma once

#include <d3d9.h>
#include <vulkan/vulkan.h>

#define D3D9_GTA_SA_COMPAT_API_VERSION 7u

struct ID3D9VkInteropDevice;

enum D3D9GtaSaCompatFlags : UINT {
  D3D9_GTA_SA_COMPAT_ACTIVE          = 1u << 0,
  D3D9_GTA_SA_COMPAT_VULKAN_BACKEND  = 1u << 1,
  D3D9_GTA_SA_COMPAT_VULKAN_INTEROP  = 1u << 2,
  D3D9_GTA_SA_COMPAT_FRAME_LIFECYCLE = 1u << 3,
  D3D9_GTA_SA_COMPAT_DEVICE_READY    = 1u << 4,
  D3D9_GTA_SA_COMPAT_PASS_REGISTRY   = 1u << 5,
  D3D9_GTA_SA_COMPAT_COMMAND_RECORD  = 1u << 6,
  D3D9_GTA_SA_COMPAT_STATE_BATCH     = 1u << 7,
  D3D9_GTA_SA_COMPAT_STATE_JOURNAL   = 1u << 8,
    D3D9_GTA_SA_COMPAT_EFFECT_STATE_BATCH = 1u << 9,
    D3D9_GTA_SA_COMPAT_STATE_DRAW_BATCH = 1u << 10,
    D3D9_GTA_SA_COMPAT_SELECTIVE_STATE_JOURNAL = 1u << 11,
};

struct D3D9GtaSaCompatStatus {
  UINT      StructSize;
  UINT      ApiVersion;
  UINT      Flags;
  UINT      BackBufferWidth;
  UINT      BackBufferHeight;
  D3DFORMAT BackBufferFormat;
  UINT      PresentInterval;
  UINT      Reserved;
  UINT64    PresentCount;
  UINT64    ResetCount;
  UINT64    FailedResetCount;
};

/**
 * Stable GTA San Andreas capability probe for BridgeD3D9 and native modules.
 * Vulkan resources and submission remain owned by ID3D9VkInteropDevice.
 */
MIDL_INTERFACE("9f89b542-4f50-4e7d-b2a4-e8eab3c7d9f1")
ID3D9GtaSaCompatDevice : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetStatus(
          D3D9GtaSaCompatStatus*    pStatus) = 0;

  virtual HRESULT STDMETHODCALLTYPE GetVulkanInterop(
          ID3D9VkInteropDevice**    ppInterop) = 0;
};

enum D3D9GtaSaVulkanPassStage : UINT {
  D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT = 1u,
};

enum D3D9GtaSaVulkanPassFlags : UINT {
  D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS = 1u << 0,
};

struct D3D9GtaSaVulkanFrameContext {
  UINT                    StructSize;
  UINT                    ApiVersion;
  UINT                    Stage;
  UINT                    Reserved;
  UINT64                  FrameId;
  VkInstance              Instance;
  VkPhysicalDevice        PhysicalDevice;
  VkDevice                Device;
  VkCommandBuffer         CommandBuffer;
  VkImage                 SourceImage;
  VkImageView             SourceImageView;
  VkImageLayout           SourceImageLayout;
  VkFormat                SourceFormat;
  VkExtent2D              SourceExtent;
  VkImageSubresourceRange SourceSubresources;
  VkImageUsageFlags       SourceUsage;
  VkImage                 OutputImage;
  VkImageView             OutputImageView;
  VkImageLayout           OutputImageLayout;
  VkFormat                OutputFormat;
  VkExtent2D              OutputExtent;
  VkImageSubresourceRange OutputSubresources;
  VkImageUsageFlags       OutputUsage;
};

using D3D9GtaSaRecordVulkanPass = HRESULT (STDMETHODCALLTYPE*) (
        void*                             userData,
  const D3D9GtaSaVulkanFrameContext*      frame);

struct D3D9GtaSaVulkanPassDesc {
  UINT                         StructSize;
  UINT                         ApiVersion;
  INT                          Priority;
  UINT                         Stage;
  UINT                         Flags;
  char                         Name[64];
  D3D9GtaSaRecordVulkanPass    Record;
  void*                        UserData;
};

/**
 * Version 2 records all registered passes into DXVK's existing Present
 * command buffer. A pass must not submit, end, or reset the command buffer,
 * call D3D9, call RegisterVulkanPass/UnregisterVulkanPass from its Record
 * callback, or leave either supplied image in a different layout.
 * UnregisterVulkanPass waits for an in-flight Record callback to finish.
 */
MIDL_INTERFACE("9f89b542-4f50-4e7d-b2a4-e8eab3c7d9f2")
ID3D9GtaSaCompatDevice1 : public ID3D9GtaSaCompatDevice {
  virtual HRESULT STDMETHODCALLTYPE RegisterVulkanPass(
    const D3D9GtaSaVulkanPassDesc* pDesc,
          UINT64*                   pToken) = 0;

  virtual HRESULT STDMETHODCALLTYPE UnregisterVulkanPass(
          UINT64                    token) = 0;
};

struct D3D9GtaSaFloatConstantRange {
  UINT          StartRegister;
  UINT          RegisterCount;
  const float*  Data;
};

struct D3D9GtaSaBoolConstantRange {
  UINT          StartRegister;
  UINT          RegisterCount;
  const BOOL*   Data;
};

struct D3D9GtaSaTextureBinding {
  DWORD                   Stage;
  IDirect3DBaseTexture9*  Texture;
};

struct D3D9GtaSaStateBatch {
  UINT                                  StructSize;
  UINT                                  ApiVersion;
  UINT                                  VertexFloatRangeCount;
  const D3D9GtaSaFloatConstantRange*    VertexFloatRanges;
  UINT                                  VertexBoolRangeCount;
  const D3D9GtaSaBoolConstantRange*     VertexBoolRanges;
  UINT                                  PixelFloatRangeCount;
  const D3D9GtaSaFloatConstantRange*    PixelFloatRanges;
  UINT                                  PixelBoolRangeCount;
  const D3D9GtaSaBoolConstantRange*     PixelBoolRanges;
  UINT                                  TextureBindingCount;
  const D3D9GtaSaTextureBinding*        TextureBindings;
};

/**
 * Version 3 submits a set of shader constants and texture bindings through
 * one COM call while preserving normal D3D9 state-block semantics.
 */
MIDL_INTERFACE("9f89b542-4f50-4e7d-b2a4-e8eab3c7d9f3")
ID3D9GtaSaCompatDevice2 : public ID3D9GtaSaCompatDevice1 {
  virtual HRESULT STDMETHODCALLTYPE SubmitStateBatch(
    const D3D9GtaSaStateBatch* pBatch) = 0;
};

/**
 * Version 4 records the first value of each supported D3D9 pipeline state
 * changed between BeginStateJournal and RestoreStateJournal directly inside
 * the device. Coverage follows D3D9 state-block/effect state plus N-patch
 * mode; render targets, depth-stencil surfaces, resource contents, and device
 * lifecycle state are outside the journal contract.
 */
MIDL_INTERFACE("9f89b542-4f50-4e7d-b2a4-e8eab3c7d9f4")
ID3D9GtaSaCompatDevice3 : public ID3D9GtaSaCompatDevice2 {
  virtual HRESULT STDMETHODCALLTYPE BeginStateJournal() = 0;

  virtual HRESULT STDMETHODCALLTYPE RestoreStateJournal() = 0;
};

struct D3D9GtaSaTransformBinding {
  D3DTRANSFORMSTATETYPE State;
  D3DMATRIX             Matrix;
};

struct D3D9GtaSaLightBinding {
  DWORD     Index;
  D3DLIGHT9 Light;
};

struct D3D9GtaSaLightEnableBinding {
  DWORD Index;
  BOOL  Enable;
};

struct D3D9GtaSaRenderStateBinding {
  D3DRENDERSTATETYPE State;
  DWORD              Value;
};

struct D3D9GtaSaTextureStageStateBinding {
  DWORD                    Stage;
  D3DTEXTURESTAGESTATETYPE Type;
  DWORD                    Value;
};

struct D3D9GtaSaSamplerStateBinding {
  DWORD               Sampler;
  D3DSAMPLERSTATETYPE Type;
  DWORD               Value;
};

struct D3D9GtaSaIntConstantRange {
  UINT       StartRegister;
  UINT       RegisterCount;
  const int* Data;
};

enum D3D9GtaSaEffectStateBatchFlags : UINT {
  D3D9_GTA_SA_EFFECT_STATE_HAS_MATERIAL      = 1u << 0,
  D3D9_GTA_SA_EFFECT_STATE_HAS_NPATCH_MODE   = 1u << 1,
  D3D9_GTA_SA_EFFECT_STATE_HAS_FVF           = 1u << 2,
  D3D9_GTA_SA_EFFECT_STATE_HAS_VERTEX_SHADER = 1u << 3,
  D3D9_GTA_SA_EFFECT_STATE_HAS_PIXEL_SHADER  = 1u << 4,
};

struct D3D9GtaSaEffectStateBatch {
  UINT                                      StructSize;
  UINT                                      ApiVersion;
  UINT                                      Flags;
  UINT                                      Reserved;
  UINT                                      TransformCount;
  const D3D9GtaSaTransformBinding*          Transforms;
  UINT                                      LightCount;
  const D3D9GtaSaLightBinding*              Lights;
  UINT                                      LightEnableCount;
  const D3D9GtaSaLightEnableBinding*        LightEnables;
  UINT                                      RenderStateCount;
  const D3D9GtaSaRenderStateBinding*        RenderStates;
  UINT                                      TextureBindingCount;
  const D3D9GtaSaTextureBinding*            TextureBindings;
  UINT                                      TextureStageStateCount;
  const D3D9GtaSaTextureStageStateBinding*  TextureStageStates;
  UINT                                      SamplerStateCount;
  const D3D9GtaSaSamplerStateBinding*       SamplerStates;
  D3DMATERIAL9                              Material;
  float                                     NPatchMode;
  DWORD                                     FVF;
  IDirect3DVertexShader9*                   VertexShader;
  IDirect3DPixelShader9*                    PixelShader;
  UINT                                      VertexFloatRangeCount;
  const D3D9GtaSaFloatConstantRange*        VertexFloatRanges;
  UINT                                      VertexIntRangeCount;
  const D3D9GtaSaIntConstantRange*          VertexIntRanges;
  UINT                                      VertexBoolRangeCount;
  const D3D9GtaSaBoolConstantRange*         VertexBoolRanges;
  UINT                                      PixelFloatRangeCount;
  const D3D9GtaSaFloatConstantRange*        PixelFloatRanges;
  UINT                                      PixelIntRangeCount;
  const D3D9GtaSaIntConstantRange*          PixelIntRanges;
  UINT                                      PixelBoolRangeCount;
  const D3D9GtaSaBoolConstantRange*         PixelBoolRanges;
};

/**
 * Version 5 applies the final state produced by one D3DX Effect pass through
 * one COM call and one outer device lock. It participates in an active native
 * state journal, so RestoreStateJournal returns the device to its prior state.
 */
MIDL_INTERFACE("9f89b542-4f50-4e7d-b2a4-e8eab3c7d9f5")
ID3D9GtaSaCompatDevice4 : public ID3D9GtaSaCompatDevice3 {
  virtual HRESULT STDMETHODCALLTYPE SubmitEffectStateBatch(
    const D3D9GtaSaEffectStateBatch* pBatch) = 0;
};

enum D3D9GtaSaDrawKind : UINT {
  D3D9_GTA_SA_DRAW_PRIMITIVE = 1u,
  D3D9_GTA_SA_DRAW_INDEXED_PRIMITIVE = 2u,
};

struct D3D9GtaSaDrawDesc {
  UINT                  StructSize;
  UINT                  Kind;
  D3DPRIMITIVETYPE      PrimitiveType;
  UINT                  Reserved;
  INT                   BaseVertexIndex;
  UINT                  MinVertexIndex;
  UINT                  NumVertices;
  UINT                  StartIndexOrVertex;
  UINT                  PrimitiveCount;
};

struct D3D9GtaSaStateDrawBatch {
  UINT                           StructSize;
  UINT                           ApiVersion;
  const D3D9GtaSaStateBatch*     StateBatch;
  D3D9GtaSaDrawDesc              Draw;
};

/**
 * Version 6 validates and submits one per-object state batch together with
 * the immediately following DrawPrimitive or DrawIndexedPrimitive call.
 * No state is retained for a later draw, and an active native state journal
 * observes the same state changes as the two-call D3D9 sequence.
 */
MIDL_INTERFACE("9f89b542-4f50-4e7d-b2a4-e8eab3c7d9f6")
ID3D9GtaSaCompatDevice5 : public ID3D9GtaSaCompatDevice4 {
    virtual HRESULT STDMETHODCALLTYPE SubmitStateDrawBatch(
        const D3D9GtaSaStateDrawBatch* pBatch) = 0;
};

// API7 keeps API4's journal alive while allowing an effect state manager to
// exclude unrelated game/device writes from the transaction.
MIDL_INTERFACE("9f89b542-4f50-4e7d-b2a4-e8eab3c7d9f7")
ID3D9GtaSaCompatDevice6 : public ID3D9GtaSaCompatDevice5 {
    virtual HRESULT STDMETHODCALLTYPE BeginSelectiveStateJournal() = 0;

    virtual HRESULT STDMETHODCALLTYPE SetStateJournalCaptureEnabled(
        BOOL Enable) = 0;
};

#ifndef _MSC_VER
__CRT_UUID_DECL(ID3D9GtaSaCompatDevice, 0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf1);
__CRT_UUID_DECL(ID3D9GtaSaCompatDevice1,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf2);
__CRT_UUID_DECL(ID3D9GtaSaCompatDevice2,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf3);
__CRT_UUID_DECL(ID3D9GtaSaCompatDevice3,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf4);
__CRT_UUID_DECL(ID3D9GtaSaCompatDevice4,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf5);
__CRT_UUID_DECL(ID3D9GtaSaCompatDevice5,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf6);
__CRT_UUID_DECL(ID3D9GtaSaCompatDevice6,0x9f89b542,0x4f50,0x4e7d,0xb2,0xa4,0xe8,0xea,0xb3,0xc7,0xd9,0xf7);
#endif
