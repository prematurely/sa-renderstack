#pragma once

#include "../util/config/config.h"
#include "../dxvk/dxvk_device.h"

namespace dxvk {

  enum class D3D9FloatEmulation : uint8_t {
    Disabled = 0,
    Enabled  = 1,
    Strict   = 2
  };

  struct D3D9Options {

    D3D9Options(const Rc<DxvkDevice>& device, const Config& config);

    /// Override PCI vendor and device IDs reported to the
    /// application. This may make apps think they are running
    /// on a different GPU than they do and behave differently.
    int32_t customVendorId;
    int32_t customDeviceId;
    std::string customDeviceDesc;

    /// Reports Nvidia GPUs running on the proprietary driver as a different
    /// vendor (usually AMD)
    bool hideNvidiaGpu;

    /// Reports Nvidia GPUs running on NVK as a different vendor (usually AMD)
    bool hideNvkGpu;

    /// Reports AMD GPUs as a different vendor (usually Nvidia)
    bool hideAmdGpu;

    /// Reports Intel GPUs as a different vendor (usually AMD)
    bool hideIntelGpu;

    /// Present interval. Overrides the value
    /// in D3DPRESENT_PARAMS used in swapchain present.
    int32_t presentInterval;

    /// Override maximum frame latency if the app specifies
    /// a higher value. May help with frame timing issues.
    int32_t maxFrameLatency;

    /// Limit frame rate
    int32_t maxFrameRate;

    /// Set the max shader model the device can support in the caps.
    uint32_t shaderModel;

    /// Whether or not to set the process as DPI aware in Windows when the API interface is created.
    bool dpiAware;

    /// Whether or not to do a fast path clear if we're close enough to the whole render target.
    bool lenientClear;

    /// Defer surface creation
    bool deferSurfaceCreation;

    /// Anisotropic filter override
    ///
    /// Enforces anisotropic filtering with the
    /// given anisotropy value for all samplers.
    int32_t samplerAnisotropy;

    /// Max available memory override
    ///
    /// Changes the max initial value used in
    /// tracking and GetAvailableTextureMem
    uint32_t maxAvailableMemory;

    /// D3D9 Floating Point Emulation (anything * 0 = 0)
    D3D9FloatEmulation d3d9FloatEmulation;

    /// Whether shaders use FP16 for partial precision instructions
    bool useFP16;

    /// Support depth formats for cube textures
    bool supportCubeDepthFormats;

    /// Support the DF16 & DF24 texture format
    bool supportDFFormats;

    /// Support X4R4G4B4
    bool supportX4R4G4B4;

    /// Use D32f for D24
    bool useD32forD24;

    /// Disable D3DFMT_A8 for render targets.
    /// Specifically to work around a game
    /// bug in The Sims 2 that happens on native too!
    bool disableA8RT;

    /// Whether or not to respect memory tracking for
    /// failing resource allocation.
    bool memoryTrackTest;

    /// Forced aspect ratio, disable other modes
    std::string forceAspectRatio;

    /// Forced refresh rate, disable other modes
    uint32_t forceRefreshRate;

    /// Restrict the mode count to ensure a maximum total count of 24
    bool modeCountCompatibility;

    /// Always use a spec constant to determine sampler type (instead of just in PS 1.x)
    /// Works around a game bug in Halo CE where it gives cube textures to 2d/volume samplers
    bool forceSamplerTypeSpecConstants;

    /// Forces sample rate shading
    bool forceSampleRateShading;

    /// Allow D3DLOCK_DISCARD
    bool allowDiscard;

    /// Enumerate adapters by displays
    bool enumerateByDisplays;

    /// Cached dynamic buffers: Maps all buffers in cached memory.
    bool cachedWriteOnlyBuffers;

    /// Use device local memory for constant buffers.
    Tristate deviceLocalConstantBuffers;

    /// Disable direct buffer mapping
    bool allowDirectBufferMapping;

    /// Force flushing D3DPOOL_DEFAULT buffers at draw time rather than on unlock
    /// Used to work around game bugs in source engine games like CSGO and Insurgency.
    /// Those games write to buffers after unlocking them. Uploading on unlock leads to black
    /// objects because they never get their proper UVs.
    bool forceDrawTimeBufferUpload;

    /// Don't use non seamless cube maps
    bool seamlessCubes;

    /// Mipmap LOD bias
    ///
    /// Enforces the given LOD bias for all samplers.
    float samplerLodBias;

    /// Clamps negative LOD bias
    bool clampNegativeLodBias;

    /// How much virtual memory will be used for textures (in MB).
    int32_t textureMemory;

    /// Shader dump path
    std::string shaderDumpPath;

    /// Enable emulation of device loss when a fullscreen app loses focus
    bool deviceLossOnFocusLoss;

    /// Disable counting losable resources and rejecting calls to Reset() if any are still alive
    bool countLosableResources;

    /// Ensure that for the same D3D commands the output VK commands
    /// don't change between runs. Useful for comparative benchmarking,
    /// can negatively affect performance.
    bool reproducibleCommandStream;

    /// Enable depth texcoord Z (Dref) scaling (D3D8 quirk)
    int32_t drefScaling;

    /// Add an extra front buffer to make GetFrontBufferData() work correctly when the swapchain only has a single buffer
    bool extraFrontbuffer;

    /// Enables the GTA San Andreas compatibility runtime and its native
    /// Vulkan capability interface.
    bool gtaSaCompat;

    /// Logs GTA San Andreas device lifecycle and Vulkan backend details.
    bool gtaSaCompatDiagnostics;

    /// Skips state-block setter calls when the saved value already exactly
    /// matches the logical D3D9 device state.
    bool gtaSaStateBlockPrefilter;

    /// Skips a repeated state-block Apply when neither the device logical
    /// state nor the state block contents changed since its previous Apply.
    bool gtaSaStateBlockFastSkip;

    /// Defers backend vertex and pixel shader binds until the next draw so
    /// transient state-block restore/reapply cycles collapse to one decision.
    bool gtaSaDeferShaderBinding;

    /// Defers scalar blend-factor and stencil-reference backend updates until
    /// the next draw so transient state-block cycles collapse to one command.
    bool gtaSaDeferScalarStateBindings;

    /// Avoids emitting an identical D3D9 input-layout command when the
    /// declaration, vertex shader, and instance stream configuration are
    /// unchanged.
    bool gtaSaInputLayoutCache;

    /// Coalesces all dirty fixed-function push-data updates for one draw into
    /// a single command-stream callback while preserving their update order.
    bool gtaSaCoalescePushData;

    /// Coalesces specialization constants and push-data updates when both are
    /// dirty for the same draw. Specialization is still written first.
    bool gtaSaCoalesceSpecAndPushData;

    /// Coalesces dirty sampler bindings for one draw into a single
    /// command-stream callback while preserving ascending sampler order.
    bool gtaSaCoalesceSamplerBindings;

    /// Coalesces dirty texture image-view bindings for one draw into a single
    /// command-stream callback while preserving ascending sampler order.
    bool gtaSaCoalesceTextureBindings;

    /// Avoids queuing a producer-side sampler or image-view callback when the
    /// effective binding is already the last binding queued for that slot.
    /// The cache is invalidated on device reset and never changes D3D9 state.
    bool gtaSaResourceBindingCache;
  };

}
