#include <sa_renderstack/backend_api.h>

#include <cstddef>
#include <cstdio>
#include <type_traits>

static_assert(sizeof(void*) == 4u);
static_assert(D3D9_GTA_SA_COMPAT_API_VERSION == 7u);

static_assert(D3D9_GTA_SA_COMPAT_ACTIVE == 0x001u);
static_assert(D3D9_GTA_SA_COMPAT_VULKAN_BACKEND == 0x002u);
static_assert(D3D9_GTA_SA_COMPAT_VULKAN_INTEROP == 0x004u);
static_assert(D3D9_GTA_SA_COMPAT_FRAME_LIFECYCLE == 0x008u);
static_assert(D3D9_GTA_SA_COMPAT_DEVICE_READY == 0x010u);
static_assert(D3D9_GTA_SA_COMPAT_PASS_REGISTRY == 0x020u);
static_assert(D3D9_GTA_SA_COMPAT_COMMAND_RECORD == 0x040u);
static_assert(D3D9_GTA_SA_COMPAT_STATE_BATCH == 0x080u);
static_assert(D3D9_GTA_SA_COMPAT_STATE_JOURNAL == 0x100u);
static_assert(D3D9_GTA_SA_COMPAT_EFFECT_STATE_BATCH == 0x200u);
static_assert(D3D9_GTA_SA_COMPAT_STATE_DRAW_BATCH == 0x400u);
static_assert(D3D9_GTA_SA_COMPAT_SELECTIVE_STATE_JOURNAL == 0x800u);
static_assert(D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT == 1u);
static_assert(D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS == 1u);
static_assert(D3D9_GTA_SA_EFFECT_STATE_HAS_MATERIAL == 0x01u);
static_assert(D3D9_GTA_SA_EFFECT_STATE_HAS_NPATCH_MODE == 0x02u);
static_assert(D3D9_GTA_SA_EFFECT_STATE_HAS_FVF == 0x04u);
static_assert(D3D9_GTA_SA_EFFECT_STATE_HAS_VERTEX_SHADER == 0x08u);
static_assert(D3D9_GTA_SA_EFFECT_STATE_HAS_PIXEL_SHADER == 0x10u);
static_assert(D3D9_GTA_SA_DRAW_PRIMITIVE == 1u);
static_assert(D3D9_GTA_SA_DRAW_INDEXED_PRIMITIVE == 2u);

static_assert(std::is_standard_layout_v<D3D9GtaSaCompatStatus>);
static_assert(std::is_standard_layout_v<D3D9GtaSaVulkanFrameContext>);
static_assert(std::is_standard_layout_v<D3D9GtaSaVulkanPassDesc>);
static_assert(std::is_standard_layout_v<D3D9GtaSaFloatConstantRange>);
static_assert(std::is_standard_layout_v<D3D9GtaSaBoolConstantRange>);
static_assert(std::is_standard_layout_v<D3D9GtaSaTextureBinding>);
static_assert(std::is_standard_layout_v<D3D9GtaSaStateBatch>);
static_assert(std::is_standard_layout_v<D3D9GtaSaTransformBinding>);
static_assert(std::is_standard_layout_v<D3D9GtaSaLightBinding>);
static_assert(std::is_standard_layout_v<D3D9GtaSaLightEnableBinding>);
static_assert(std::is_standard_layout_v<D3D9GtaSaRenderStateBinding>);
static_assert(std::is_standard_layout_v<D3D9GtaSaTextureStageStateBinding>);
static_assert(std::is_standard_layout_v<D3D9GtaSaSamplerStateBinding>);
static_assert(std::is_standard_layout_v<D3D9GtaSaIntConstantRange>);
static_assert(std::is_standard_layout_v<D3D9GtaSaEffectStateBatch>);
static_assert(std::is_standard_layout_v<D3D9GtaSaDrawDesc>);
static_assert(std::is_standard_layout_v<D3D9GtaSaStateDrawBatch>);

static_assert(sizeof(D3D9GtaSaCompatStatus) == 56u);
static_assert(sizeof(D3D9GtaSaVulkanFrameContext) == 152u);
static_assert(sizeof(D3D9GtaSaVulkanPassDesc) == 92u);
static_assert(sizeof(D3D9GtaSaFloatConstantRange) == 12u);
static_assert(sizeof(D3D9GtaSaBoolConstantRange) == 12u);
static_assert(sizeof(D3D9GtaSaTextureBinding) == 8u);
static_assert(sizeof(D3D9GtaSaStateBatch) == 48u);
static_assert(sizeof(D3D9GtaSaTransformBinding) == 68u);
static_assert(sizeof(D3D9GtaSaLightBinding) == 108u);
static_assert(sizeof(D3D9GtaSaLightEnableBinding) == 8u);
static_assert(sizeof(D3D9GtaSaRenderStateBinding) == 8u);
static_assert(sizeof(D3D9GtaSaTextureStageStateBinding) == 12u);
static_assert(sizeof(D3D9GtaSaSamplerStateBinding) == 12u);
static_assert(sizeof(D3D9GtaSaIntConstantRange) == 12u);
static_assert(sizeof(D3D9GtaSaEffectStateBatch) == 204u);
static_assert(sizeof(D3D9GtaSaDrawDesc) == 36u);
static_assert(sizeof(D3D9GtaSaStateDrawBatch) == 48u);

static_assert(offsetof(D3D9GtaSaCompatStatus, ApiVersion) == 4u);
static_assert(offsetof(D3D9GtaSaCompatStatus, PresentCount) == 32u);
static_assert(offsetof(D3D9GtaSaCompatStatus, ResetCount) == 40u);
static_assert(offsetof(D3D9GtaSaCompatStatus, FailedResetCount) == 48u);

static_assert(offsetof(D3D9GtaSaVulkanFrameContext, FrameId) == 16u);
static_assert(offsetof(D3D9GtaSaVulkanFrameContext, Instance) == 24u);
static_assert(offsetof(D3D9GtaSaVulkanFrameContext, PhysicalDevice) == 28u);
static_assert(offsetof(D3D9GtaSaVulkanFrameContext, Device) == 32u);
static_assert(offsetof(D3D9GtaSaVulkanFrameContext, CommandBuffer) == 36u);
static_assert(offsetof(D3D9GtaSaVulkanFrameContext, SourceImage) == 40u);
static_assert(offsetof(D3D9GtaSaVulkanFrameContext, SourceImageView) == 48u);
static_assert(offsetof(D3D9GtaSaVulkanFrameContext, OutputImage) == 96u);
static_assert(offsetof(D3D9GtaSaVulkanFrameContext, OutputImageView) == 104u);

static_assert(offsetof(D3D9GtaSaVulkanPassDesc, Record) == 84u);
static_assert(offsetof(D3D9GtaSaVulkanPassDesc, UserData) == 88u);
static_assert(offsetof(D3D9GtaSaFloatConstantRange, Data) == 8u);
static_assert(offsetof(D3D9GtaSaBoolConstantRange, Data) == 8u);
static_assert(offsetof(D3D9GtaSaTextureBinding, Texture) == 4u);
static_assert(offsetof(D3D9GtaSaStateBatch, VertexFloatRanges) == 12u);
static_assert(offsetof(D3D9GtaSaStateBatch, VertexBoolRanges) == 20u);
static_assert(offsetof(D3D9GtaSaStateBatch, PixelFloatRanges) == 28u);
static_assert(offsetof(D3D9GtaSaStateBatch, PixelBoolRanges) == 36u);
static_assert(offsetof(D3D9GtaSaStateBatch, TextureBindings) == 44u);
static_assert(offsetof(D3D9GtaSaIntConstantRange, Data) == 8u);

static_assert(offsetof(D3D9GtaSaEffectStateBatch, Transforms) == 20u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, Lights) == 28u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, LightEnables) == 36u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, RenderStates) == 44u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, TextureBindings) == 52u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, TextureStageStates) == 60u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, SamplerStates) == 68u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, VertexShader) == 148u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, PixelShader) == 152u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, VertexFloatRanges) == 160u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, VertexIntRanges) == 168u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, VertexBoolRanges) == 176u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, PixelFloatRanges) == 184u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, PixelIntRanges) == 192u);
static_assert(offsetof(D3D9GtaSaEffectStateBatch, PixelBoolRanges) == 200u);
static_assert(offsetof(D3D9GtaSaStateDrawBatch, StateBatch) == 8u);

static_assert(std::is_base_of_v<ID3D9GtaSaCompatDevice, ID3D9GtaSaCompatDevice1>);
static_assert(std::is_base_of_v<ID3D9GtaSaCompatDevice1, ID3D9GtaSaCompatDevice2>);
static_assert(std::is_base_of_v<ID3D9GtaSaCompatDevice2, ID3D9GtaSaCompatDevice3>);
static_assert(std::is_base_of_v<ID3D9GtaSaCompatDevice3, ID3D9GtaSaCompatDevice4>);
static_assert(std::is_base_of_v<ID3D9GtaSaCompatDevice4, ID3D9GtaSaCompatDevice5>);
static_assert(std::is_base_of_v<ID3D9GtaSaCompatDevice5, ID3D9GtaSaCompatDevice6>);
static_assert(std::is_base_of_v<IUnknown, ID3D9GtaSaCompatDevice6>);

using RecordVulkanPassSignature = HRESULT (STDMETHODCALLTYPE*)(
    void*, const D3D9GtaSaVulkanFrameContext*);
using GetStatusSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice::*)(
    D3D9GtaSaCompatStatus*);
using GetVulkanInteropSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice::*)(
    ID3D9VkInteropDevice**);
using RegisterVulkanPassSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice1::*)(
    const D3D9GtaSaVulkanPassDesc*, UINT64*);
using UnregisterVulkanPassSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice1::*)(UINT64);
using SubmitStateBatchSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice2::*)(
    const D3D9GtaSaStateBatch*);
using BeginStateJournalSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice3::*)();
using RestoreStateJournalSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice3::*)();
using SubmitEffectStateBatchSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice4::*)(
    const D3D9GtaSaEffectStateBatch*);
using SubmitStateDrawBatchSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice5::*)(
    const D3D9GtaSaStateDrawBatch*);
using BeginSelectiveStateJournalSignature = HRESULT (STDMETHODCALLTYPE ID3D9GtaSaCompatDevice6::*)();
using SetStateJournalCaptureEnabledSignature = HRESULT (
    STDMETHODCALLTYPE ID3D9GtaSaCompatDevice6::*)(BOOL);

static_assert(std::is_same_v<D3D9GtaSaRecordVulkanPass, RecordVulkanPassSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice::GetStatus), GetStatusSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice::GetVulkanInterop), GetVulkanInteropSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice1::RegisterVulkanPass), RegisterVulkanPassSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice1::UnregisterVulkanPass), UnregisterVulkanPassSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice2::SubmitStateBatch), SubmitStateBatchSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice3::BeginStateJournal), BeginStateJournalSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice3::RestoreStateJournal), RestoreStateJournalSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice4::SubmitEffectStateBatch), SubmitEffectStateBatchSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice5::SubmitStateDrawBatch), SubmitStateDrawBatchSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice6::BeginSelectiveStateJournal), BeginSelectiveStateJournalSignature>);
static_assert(std::is_same_v<decltype(&ID3D9GtaSaCompatDevice6::SetStateJournalCaptureEnabled), SetStateJournalCaptureEnabledSignature>);

namespace
{
constexpr GUID kExpectedIids[] = {
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf1}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf2}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf3}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf4}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf5}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf6}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf7}},
};

bool GuidEquals(const GUID& left, const GUID& right)
{
    if (left.Data1 != right.Data1 || left.Data2 != right.Data2 || left.Data3 != right.Data3) {
        return false;
    }
    for (std::size_t index = 0; index < 8u; ++index) {
        if (left.Data4[index] != right.Data4[index]) return false;
    }
    return true;
}
}

int main()
{
    const GUID actualIids[] = {
        __uuidof(ID3D9GtaSaCompatDevice),
        __uuidof(ID3D9GtaSaCompatDevice1),
        __uuidof(ID3D9GtaSaCompatDevice2),
        __uuidof(ID3D9GtaSaCompatDevice3),
        __uuidof(ID3D9GtaSaCompatDevice4),
        __uuidof(ID3D9GtaSaCompatDevice5),
        __uuidof(ID3D9GtaSaCompatDevice6),
    };

    for (std::size_t index = 0; index < 7u; ++index) {
        if (!GuidEquals(actualIids[index], kExpectedIids[index])) {
            std::fprintf(stderr, "backend API IID mismatch for API %zu\n", index + 1u);
            return static_cast<int>(index + 1u);
        }
    }

    std::puts("PASS backend API v1-v7 ABI layout and IIDs");
    return 0;
}
