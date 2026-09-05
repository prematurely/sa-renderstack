#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#define Direct3DCreate9 BridgeD3D9_System_Direct3DCreate9
#include <d3d9.h>
#include "BridgeD3D9Plugin.h"
#undef Direct3DCreate9
#include "GtaSaCompatApiVersions.h"
#include "ProperShadersBatchPolicy.h"
#include "ProperShadersEffectBindingCache.h"
#include "ProperShadersPatchValidation.h"
#include "EffectInspector.h"
#include "ProperShadersStateJournal.h"
#include "BridgePerformanceProviderV1.h"
#include "PerformanceAdapterConfig.h"
#include "PerformanceAdapters.h"
#include "../../../backend/dxvk/src/util/util_thread_scheduling.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dxguid.lib")

#ifdef BRIDGE_D3D9_BACKEND_TRACE
static constexpr bool kBackendTraceBuild = true;
#else
static constexpr bool kBackendTraceBuild = false;
#endif

static HMODULE g_selfModule = nullptr;
static HMODULE g_realD3D9 = nullptr;
static HMODULE g_psProxy = nullptr;
static bool g_useDxvkBackend = false;
static bool g_enablePostFxHost = false;
static char g_postFxIniPath[MAX_PATH]{};
static INIT_ONCE g_postFxInitOnce = INIT_ONCE_STATIC_INIT;
static bool g_enableProxyChain = false;
static bool g_enableLegacyD3D9PSAutoProbe = false;
static bool g_enableD3D9Stats = false;
static DWORD g_d3d9StatsIntervalMs = 1000;
static bool g_enableD3D9Trace = false;
static int g_d3d9TraceTriggerKey = VK_F10;
static UINT g_d3d9TraceMaxDraws = 12000;
static bool g_enableD3D9CallsiteProfile = false;
static int g_d3d9CallsiteTriggerKey = VK_F9;
static UINT g_d3d9CallsiteCaptureFrames = 120;
static UINT g_d3d9CallsiteSampleEveryDraws = 64;
static bool g_enableCpuHotspotProfile = false;
static int g_cpuHotspotTriggerKey = VK_F8;
static DWORD g_cpuHotspotDurationMs = 10000;
static DWORD g_cpuHotspotIntervalMs = 2;
static bool g_cpuHotspotChainD3D9CallsiteProfile = false;
static std::atomic<LONG> g_cpuHotspotActive{ 0 };
static std::atomic<LONG> g_cpuHotspotCaptureId{ 0 };
static std::atomic<LONG> g_cpuHotspotPresents{ 0 };
static std::atomic<LONG> g_cpuHotspotCallsitePending{ 0 };
static bool g_enableProperShadersEffectProfile = false;
static int g_properShadersEffectProfileTriggerKey = VK_F7;
static DWORD g_properShadersEffectProfileDurationMs = 3000;
static bool g_properShadersEffectProfileTestNoSaveState = false;
static bool g_properShadersEffectProfileTestSkipDuplicateMatrices = false;
static bool g_enableProperShadersStateAttribution = false;
static int g_properShadersStateAttributionTriggerKey = VK_F7;
static DWORD g_properShadersStateAttributionDurationMs = 3000;
static ULONGLONG g_properShadersStateAttributionStartTick = 0;
static UINT g_properShadersStateAttributionCaptureId = 0;
static ProperShadersStateJournalDiagnostics
    g_properShadersStateAttributionStartJournalDiagnostics{};
static bool g_enableProperShadersEffectOptimization = false;
static bool g_properShadersEffectOptimizationStateJournal = true;
static bool g_properShadersGeneralStateJournal = true;
static bool g_properShadersEffectBatching = false;
static bool g_properShadersSkipDuplicateMatrices = true;
static bool g_properShadersSkipDuplicateParameters = true;
static bool g_properShadersDirectConstants = true;
static bool g_properShadersEffectOptimizationNoSaveState = false;
static bool g_properShadersEffectOptimizationAutoBenchmark = false;
static DWORD g_properShadersEffectOptimizationWarmupMs = 10000;
static DWORD g_properShadersEffectOptimizationEpochMs = 3000;
static UINT g_properShadersEffectOptimizationPairs = 4;
static double g_properShadersEffectOptimizationMinimumGainPercent = 1.0;
static bool g_enableD3D9Optimizer = false;
static bool g_skipRedundantShaders = true;
static bool g_skipRedundantConstants = true;
static bool g_affinityEnable = false;
static DWORD_PTR g_affinityRequestedMask = 0;
static DWORD g_affinityPriorityClass = NORMAL_PRIORITY_CLASS;
static bool g_affinityReapply = false;
static DWORD g_affinityReapplyCount = 60;
static DWORD g_affinityReapplyIntervalMs = 2000;
static renderstack::scheduling::Options g_threadSchedulingOptions{};
static char g_dxvkBackendDir[MAX_PATH]{};
static char g_gameDir[MAX_PATH]{};
static char g_primaryProxyName[64]{};
static char g_performanceIniPath[MAX_PATH]{};
static BridgePerformance::AdapterRuntimeConfig g_performanceRuntimeConfig{};
static std::mutex g_performanceConfigMutex;
static bool g_performanceConfigLoaded = false;
static std::unordered_map<std::string,
    BridgePerformanceProviderV1::ProviderHealth> g_performanceProviderHealth;
static void* g_getSystemDirectoryAAddress = nullptr;
static uint8_t g_getSystemDirectoryAOriginal[5]{};
static bool g_getSystemDirectoryAHookInstalled = false;

using PFN_Direct3DCreate9 = IDirect3D9* (__stdcall *)(UINT SDKVersion);
using PFN_DebugSetLevel = int (__stdcall *)(void);
using PFN_DebugSetMute = int (__stdcall *)(void);
using PFN_Direct3DShaderValidatorCreate9 = void* (__stdcall *)(void);
using PFN_PSGPError = void* (__stdcall *)(void);
using PFN_PSGPSampleTexture = void* (__stdcall *)(void);
using PFN_CreateDXGIFactory = HRESULT (__stdcall *)(REFIID riid, void** ppFactory);
using PFN_CreateDXGIFactory2 = HRESULT (__stdcall *)(UINT flags, REFIID riid, void** ppFactory);
using PFN_DXGIDeclareAdapterRemovalSupport = HRESULT (__stdcall *)(void);
using PFN_DXGIGetDebugInterface1 = HRESULT (__stdcall *)(UINT flags, REFIID riid, void** ppDebug);

static PFN_Direct3DCreate9 g_real_Direct3DCreate9 = nullptr;
static PFN_Direct3DCreate9 g_ps_Direct3DCreate9 = nullptr;

enum ProxyClaim : uint32_t
{
    CLAIM_D3D9_ENTRY = 1u << 0,
    CLAIM_DEVICE_WRAP = 1u << 1,
    CLAIM_POSTFX_ENDSCENE = 1u << 2,
    CLAIM_POSTFX_PRESENT = 1u << 3,
    CLAIM_DEPTH = 1u << 4,
    CLAIM_SHADER_PATCH = 1u << 5,
    CLAIM_DXGI = 1u << 6,
    CLAIM_BACKEND = 1u << 7,
};

struct ProxyConfig
{
    char name[64]{};
    char type[32]{};
    char path[MAX_PATH]{};
    char mode[32]{};
    char conflictPolicy[32]{};
    char stage[32]{};
    uint32_t claims = 0;
    bool enabled = true;
    bool required = false;
};

struct ProxyClaimOwner
{
    uint32_t bit = 0;
    char owner[64]{};
};

struct VulkanHostDevice;

struct VulkanPassBinding
{
    VulkanHostDevice* host = nullptr;
    UINT64 backendToken = 0;
};

struct NativeVulkanPass
{
    HMODULE owner = nullptr;
    UINT64 bridgeToken = 0;
    D3D9GtaSaVulkanPassDesc desc{};
    std::vector<VulkanPassBinding> bindings;
};

struct VulkanHostDevice
{
    ID3D9GtaSaCompatDevice1* compat = nullptr;
    D3D9GtaSaCompatStatus status{};
    bool suspended = false;
};

struct LoadedPlugin
{
    HMODULE module = nullptr;
    char path[MAX_PATH]{};
    BridgeD3D9_PluginShutdown shutdown = nullptr;
    BridgeD3D9_OnCreateDevice onCreateDevice = nullptr;
    BridgeD3D9_OnResetBefore onResetBefore = nullptr;
    BridgeD3D9_OnResetAfter onResetAfter = nullptr;
    BridgeD3D9_OnEndScene onEndScene = nullptr;
    BridgeD3D9_OnPresentBefore onPresentBefore = nullptr;
    BridgeD3D9_OnPresentAfter onPresentAfter = nullptr;
    BridgeD3D9_OnReleaseDevice onReleaseDevice = nullptr;
    bool usesApi2 = false;
    std::vector<std::unique_ptr<NativeVulkanPass>> vulkanPasses;
};

static std::vector<LoadedPlugin> g_plugins;
static std::vector<VulkanHostDevice*> g_vulkanHostDevices;
static std::mutex g_vulkanHostMutex;
static std::atomic<UINT64> g_nextVulkanPassToken{ 1 };
static thread_local bool g_inVulkanPassRecord = false;
static std::vector<ProxyConfig> g_proxyChain;
static ProxyClaimOwner g_claimOwners[] = {
    { CLAIM_D3D9_ENTRY, "" },
    { CLAIM_DEVICE_WRAP, "" },
    { CLAIM_POSTFX_ENDSCENE, "" },
    { CLAIM_POSTFX_PRESENT, "" },
    { CLAIM_DEPTH, "" },
    { CLAIM_SHADER_PATCH, "" },
    { CLAIM_DXGI, "" },
    { CLAIM_BACKEND, "" },
};

static bool InstallSystemDirectoryHook();
static void RestoreSystemDirectoryHook();
static bool FileExistsA(const char* path);
static void EnsurePerformanceConfigLoaded();

// Truncating, null-terminated format into a fixed buffer: byte-identical to
// snprintf for the same format string, but checked at compile time.
template <class... Args>
static void FormatTo(char* output, size_t outputSize,
    std::format_string<Args...> fmt, Args&&... args)
{
    if (!output || !outputSize) return;
    const std::string text = std::vformat(
        fmt.get(), std::make_format_args(args...));
    const size_t count = text.size() < outputSize
        ? text.size()
        : outputSize - 1;
    std::memcpy(output, text.data(), count);
    output[count] = '\0';
}

template <class... Args>
static void Log(std::format_string<Args...> fmt, Args&&... args)
{
    static FILE* f = nullptr;
    if (!f) {
        f = fopen(kBackendTraceBuild
            ? "scripts\\BridgeD3D9.backend.log"
            : "scripts\\BridgeD3D9.log", "a");
        if (!f) return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::print(f, "[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::vprint_nonunicode(f, fmt.get(), std::make_format_args(args...));
    fputc('\n', f);
    fflush(f);
}

static void ThreadSchedulingLog(const char* message) noexcept
{
    try {
        Log("{}", message ? message : "");
    } catch (...) {
        // Scheduling diagnostics must not interrupt the host thread.
    }
}

enum class ProperShadersMatrixSlot : uint32_t
{
    WorldViewProjection,
    WorldInverse,
    World,
    TextureTransform,
    Count,
};

struct ProperShadersEffectMethodStats
{
    uint64_t calls = 0;
    uint64_t qpcTicks = 0;
    uint64_t maxQpcTicks = 0;
};

struct ProperShadersMatrixStats
{
    ProperShadersEffectMethodStats method;
    uint64_t duplicateCalls = 0;
    uint64_t skippedDuplicateCalls = 0;
    void* lastEffect = nullptr;
    const char* lastParameter = nullptr;
    uint8_t lastMatrix[64]{};
    bool hasLastMatrix = false;
};

struct ProperShadersEffectProfileState
{
    bool active = false;
    HMODULE module = nullptr;
    IDirect3DStateBlock9* savedDeviceState = nullptr;
    DWORD startTick = 0;
    DWORD threadId = 0;
    uint64_t frames = 0;
    LARGE_INTEGER qpcFrequency{};
    ProperShadersMatrixStats matrices[static_cast<size_t>(ProperShadersMatrixSlot::Count)]{};
    ProperShadersEffectMethodStats commitChanges;
    ProperShadersEffectMethodStats begin;
    ProperShadersEffectMethodStats beginPass;
    ProperShadersEffectMethodStats endPass;
    ProperShadersEffectMethodStats end;
    DWORD beginOriginalFlagsOr = 0;
    DWORD beginAppliedFlagsOr = 0;
    uint64_t beginFlagsModifiedCalls = 0;
    std::atomic<LONG> foreignThreadCalls{ 0 };
};

struct ProperShadersCallPatch
{
    const char* name = nullptr;
    uintptr_t rva = 0;
    uint8_t expected[6]{};
    void* replacement = nullptr;
    uint8_t original[6]{};
    bool installed = false;
};

enum class ProperShadersAutoBenchmarkStage : uint32_t
{
    WaitingForWorld,
    Warmup,
    Epoch,
    Complete,
};

struct ProperShadersAutoBenchmarkState
{
    ProperShadersAutoBenchmarkStage stage = ProperShadersAutoBenchmarkStage::WaitingForWorld;
    ULONGLONG stageStartTick = 0;
    uint32_t frameStart = 0;
    UINT epochIndex = 0;
    LONG beginCallsStart = 0;
    LONG modifiedCallsStart = 0;
    LONG failuresStart = 0;
    double baselineFps[16]{};
    double optimizedFps[16]{};
    UINT baselineCount = 0;
    UINT optimizedCount = 0;
};

static ProperShadersEffectProfileState g_properShadersEffectProfile;
static ProperShadersAutoBenchmarkState g_properShadersAutoBenchmark;
static HMODULE g_properShadersOptimizationModule = nullptr;
static bool g_properShadersOptimizationHooksInstalled = false;
static PVOID volatile* g_properShadersCreateEffectIatSlot = nullptr;
static std::atomic<bool> g_properShadersNoSaveStateActive{ false };
static thread_local uint32_t g_properShadersOptimizationBeginCalls = 0;
static thread_local uint32_t g_properShadersOptimizationModifiedCalls = 0;
static thread_local uint32_t g_properShadersOptimizationFailures = 0;
static thread_local uint64_t g_properShadersBatchStarts = 0;
static thread_local uint64_t g_properShadersBatchReuses = 0;
static thread_local uint64_t g_properShadersBatchCommits = 0;
static thread_local uint64_t g_properShadersBatchFallbacks = 0;
static thread_local uint64_t g_properShadersBatchBeginAttempts = 0;
static thread_local uint64_t g_properShadersBatchTechniqueCalls = 0;
static thread_local uint64_t g_properShadersBatchMode2Attempts = 0;
static thread_local uint64_t g_properShadersBatchStandaloneAttempts = 0;
static constexpr size_t kProperShadersBatchRejectReasonCount =
    static_cast<size_t>(ProperShadersBatching::BatchRejectReason::Count);
static thread_local uint64_t g_properShadersBatchRejectCounts[
    kProperShadersBatchRejectReasonCount]{};
static thread_local uint64_t g_properShadersMatrixCalls = 0;
static thread_local uint64_t g_properShadersMatrixSkips = 0;
static thread_local uint64_t g_properShadersParameterCalls = 0;
static thread_local uint64_t g_properShadersParameterSkips = 0;
static thread_local uint64_t g_properShadersDirectActivations = 0;
static thread_local uint64_t g_properShadersDirectCommits = 0;
static thread_local uint64_t g_properShadersDirectFallbacks = 0;
static thread_local uint64_t g_properShadersDirectVsWrites = 0;
static thread_local uint64_t g_properShadersDirectPsWrites = 0;
static thread_local uint64_t g_properShadersDirectTextureWrites = 0;
static thread_local uint64_t g_properShadersDirectBatchSubmissions = 0;
static thread_local uint64_t g_properShadersGeneralTransactions = 0;
static thread_local uint64_t g_properShadersGeneralRestores = 0;
static thread_local uint64_t g_properShadersGeneralFallbacks = 0;

static void RecordProperShadersBatchReject(
    ProperShadersBatching::BatchRejectReason reason)
{
    const size_t index = static_cast<size_t>(reason);
    if (reason == ProperShadersBatching::BatchRejectReason::None ||
        index >= kProperShadersBatchRejectReasonCount) {
        return;
    }
    ++g_properShadersBatchRejectCounts[index];
}

using ProperShadersReleaseFn = ULONG (WINAPI*)(void*);
using ProperShadersSetFloatFn = HRESULT (WINAPI*)(void*, const char*, float);
using ProperShadersSetMatrixFn = HRESULT (WINAPI*)(void*, const char*, const float*);
using ProperShadersSetFloatArrayFn = HRESULT (WINAPI*)(void*, const char*, const float*, UINT);
using ProperShadersSetVectorFn = HRESULT (WINAPI*)(void*, const char*, const float*);
using ProperShadersSetTextureFn = HRESULT (WINAPI*)(
    void*, const char*, IDirect3DBaseTexture9*);
using ProperShadersSetTechniqueFn = HRESULT (WINAPI*)(void*, const char*);
using ProperShadersCommitChangesFn = HRESULT (WINAPI*)(void*);
using ProperShadersBeginFn = HRESULT (WINAPI*)(void*, UINT*, DWORD);
using ProperShadersBeginPassFn = HRESULT (WINAPI*)(void*, UINT);
using ProperShadersEndPassFn = HRESULT (WINAPI*)(void*);
using ProperShadersEndFn = HRESULT (WINAPI*)(void*);
using ProperShadersGetDeviceFn = HRESULT (WINAPI*)(void*, IDirect3DDevice9**);
using ProperShadersSetStateManagerFn = HRESULT (WINAPI*)(void*, ID3DXEffectStateManager*);
using ProperShadersGetStateManagerFn = HRESULT (WINAPI*)(void*, ID3DXEffectStateManager**);
using ProperShadersD3DXCreateEffectFn = HRESULT (WINAPI*)(
    IDirect3DDevice9*, const void*, UINT, const void*, void*, DWORD,
    void*, void**, void**);

static ProperShadersD3DXCreateEffectFn g_originalProperShadersD3DXCreateEffect = nullptr;

static uint64_t ReadPerformanceCounter()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

static DWORD ReadCurrentThreadIdFast()
{
#if defined(_M_IX86)
    return __readfsdword(0x24);
#else
    return GetCurrentThreadId();
#endif
}

static void RecordProperShadersEffectTiming(
    ProperShadersEffectMethodStats& stats, uint64_t begin, uint64_t end)
{
    const uint64_t elapsed = end >= begin ? end - begin : 0;
    ++stats.calls;
    stats.qpcTicks += elapsed;
    stats.maxQpcTicks = (std::max)(stats.maxQpcTicks, elapsed);
}

enum class ProperShadersDirectProfile : uint8_t
{
    None,
    LitPrelight,
    LitPrelightShadowMask,
    LitPrelightNoShadows,
    LitPrelightDeferred,
};

struct ProperShadersDirectHandles
{
    const char* worldViewProjection = nullptr;
    const char* worldInverse = nullptr;
    const char* world = nullptr;
    const char* textureTransform = nullptr;
    const char* day = nullptr;
    const char* night = nullptr;
    const char* material = nullptr;
    const char* surfaceProps = nullptr;
    const char* pixelColorScale = nullptr;
    const char* roughness = nullptr;
    const char* metallicness = nullptr;
    const char* stochastic = nullptr;
    const char* wind = nullptr;
    const char* layerId = nullptr;
    const char* baseTexture = nullptr;
    const char* packedTexture = nullptr;
    const char* ambient = nullptr;
    const char* sunStrength = nullptr;
    const char* specularPower = nullptr;
    const char* screenSize = nullptr;
    bool resolved = false;
};

struct ProperShadersEffectBinding
{
    void* effect = nullptr;
    ProperShadersStateJournal* journal = nullptr;
    void** originalVtable = nullptr;
    void** hookedVtable = nullptr;
    ProperShadersReleaseFn originalRelease = nullptr;
    ProperShadersBeginFn originalBegin = nullptr;
    ProperShadersEndFn originalEnd = nullptr;
    ProperShadersSetFloatFn originalSetFloat = nullptr;
    ProperShadersSetTextureFn originalSetTexture = nullptr;
    ProperShadersDirectHandles directHandles;
    // Last technique passed to SetTechnique for THIS effect. Tracked per effect
    // (not in the single global batch slot) so a flush or an interleaved Begin
    // on another effect cannot erase it — see OptimizedProperShadersBeginForBatch.
    const char* lastTechnique = nullptr;
    // ①-3b-ii generic direct state: plan of the technique bound at the last
    // Begin, and a permanent opt-out set on the first technique-lookup miss so
    // partially covered effects never mix direct and D3DX-authoritative values.
    const GenericDirectTechniquePlan* genericPlan = nullptr;
    bool genericDirectDisabled = false;
    // Trace-stability probe: the plan being recorded this transaction. Kept
    // separate from genericPlan so the probe can record without ever arming the
    // direct-submission path (which keys off genericPlan being non-null).
    const GenericDirectTechniquePlan* probePlan = nullptr;
    // Stage B pass-lite transaction flags.
    bool liteActive = false;    // this transaction replays a validated recording
    bool liteRecording = false; // this transaction is being recorded
    bool genericJournalHooks = false;
    bool directHooks = false;
    bool unsupported = false;
};

enum ProperShadersDirectSeen : uint32_t
{
    DirectSeenWorldViewProjection = 1u << 0,
    DirectSeenWorldInverse = 1u << 1,
    DirectSeenWorld = 1u << 2,
    DirectSeenTextureTransform = 1u << 3,
    DirectSeenDay = 1u << 4,
    DirectSeenNight = 1u << 5,
    DirectSeenMaterial = 1u << 6,
    DirectSeenSurfaceProps = 1u << 7,
    DirectSeenPixelColorScale = 1u << 8,
    DirectSeenRoughness = 1u << 9,
    DirectSeenMetallicness = 1u << 10,
    DirectSeenStochastic = 1u << 11,
    DirectSeenWind = 1u << 12,
    DirectSeenLayerId = 1u << 13,
    DirectSeenBaseTexture = 1u << 14,
    DirectSeenPackedTexture = 1u << 15,
};

struct ProperShadersDirectContext
{
    void* effect = nullptr;
    ProperShadersDirectProfile profile = ProperShadersDirectProfile::None;
    uint32_t seen = 0;
    uint32_t captured = 0;
    bool active = false;
    bool disabled = false;

    float worldViewProjection[16]{};
    float worldInverse[16]{};
    float world[16]{};
    float textureTransform[16]{};
    float day[4]{};
    float night[4]{};
    float material[4]{};
    float surfaceProps[3]{};
    float pixelColorScale = 1.0f;
    float roughness = 0.0f;
    float metallicness = 0.0f;
    float stochastic = 0.0f;
    float wind = 0.0f;
    float layerId = 0.0f;
    IDirect3DBaseTexture9* baseTexture = nullptr;

    float ambient[3]{};
    float sunStrength = 0.0f;
    float specularPower = 0.0f;
    float screenSize[2]{};

    float vs13To14[8]{};
    float ps1To4[16]{};
    float ps9To10[8]{};
    float ps137To138[8]{};
    float ps173To174[8]{};
};

struct ProperShadersShaderHashEntry
{
    void* shader = nullptr;
    uint64_t hash = 0;
    UINT bytecodeSize = 0;
    bool pixel = false;
};

enum class ProperShadersBatchPath : uint8_t
{
    None,
    Mode2,
    Standalone,
};

struct ProperShadersBatchState
{
    ProperShadersBatchPath path = ProperShadersBatchPath::None;
    void* effect = nullptr;
    const char* technique = nullptr;
    DWORD flags = 0;
    UINT passes = 0;
    bool active = false;
    bool reusePending = false;
};

struct ProperShadersMatrixCache
{
    void* effect = nullptr;
    const char* parameter = nullptr;
    uint8_t matrix[64]{};
    bool valid = false;
};

enum class ProperShadersParameterSlot : uint8_t
{
    Day,
    Night,
    Material,
    Ambient,
    SurfaceProps,
    Count,
};

struct ProperShadersParameterCache
{
    void* effect = nullptr;
    const char* parameter = nullptr;
    uint8_t data[16]{};
    uint8_t size = 0;
    bool valid = false;
};

static SRWLOCK g_properShadersEffectBindingLock = SRWLOCK_INIT;
static std::unordered_map<void*, ProperShadersEffectBinding*>
    g_properShadersEffectBindingRegistry;
static thread_local std::vector<ProperShadersEffectBinding*>
    g_properShadersEffectBindingCache;
static thread_local ProperShadersBindingLookup::LastHitCache<ProperShadersEffectBinding>
    g_properShadersEffectBindingLastHit;
static thread_local ProperShadersBatchState g_properShadersBatchState;
static thread_local ProperShadersMatrixCache g_properShadersMatrixCaches[
    static_cast<size_t>(ProperShadersMatrixSlot::Count)];
static thread_local ProperShadersParameterCache g_properShadersParameterCaches[
    static_cast<size_t>(ProperShadersParameterSlot::Count)];
static thread_local ProperShadersDirectContext g_properShadersDirectContext;
static thread_local std::vector<ProperShadersShaderHashEntry>
    g_properShadersShaderHashCache;

static ProperShadersEffectBinding* FindProperShadersEffectBinding(void* effect)
{
    if (!effect) return nullptr;
    if (ProperShadersEffectBinding* binding =
            g_properShadersEffectBindingLastHit.Find(effect)) {
        return binding;
    }
    for (ProperShadersEffectBinding* binding : g_properShadersEffectBindingCache) {
        if (binding && binding->effect == effect) {
            g_properShadersEffectBindingLastHit.Remember(binding);
            return binding;
        }
    }

    ProperShadersEffectBinding* binding = nullptr;
    AcquireSRWLockShared(&g_properShadersEffectBindingLock);
    const auto it = g_properShadersEffectBindingRegistry.find(effect);
    if (it != g_properShadersEffectBindingRegistry.end()) binding = it->second;
    ReleaseSRWLockShared(&g_properShadersEffectBindingLock);
    if (binding) {
        try {
            g_properShadersEffectBindingCache.push_back(binding);
        } catch (const std::bad_alloc&) {
        }
        g_properShadersEffectBindingLastHit.Remember(binding);
    }
    return binding;
}

template <typename Fn>
static Fn GetProperShadersEffectMethod(void* effect, size_t byteOffset)
{
    if (!effect) return nullptr;
    if (ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect)) {
        if (byteOffset == 0x08 && binding->originalRelease)
            return reinterpret_cast<Fn>(binding->originalRelease);
        if (byteOffset == 0x78 && binding->originalSetFloat)
            return reinterpret_cast<Fn>(binding->originalSetFloat);
        if (byteOffset == 0xD0 && binding->originalSetTexture)
            return reinterpret_cast<Fn>(binding->originalSetTexture);
        if (byteOffset == 0xFC && binding->originalBegin)
            return reinterpret_cast<Fn>(binding->originalBegin);
        if (byteOffset == 0x10C && binding->originalEnd)
            return reinterpret_cast<Fn>(binding->originalEnd);
    }
    void** vtable = *reinterpret_cast<void***>(effect);
    if (!vtable) return nullptr;
    return reinterpret_cast<Fn>(vtable[byteOffset / sizeof(void*)]);
}

static ULONG WINAPI HookedProperShadersRelease(void* effect);
static HRESULT WINAPI HookedProperShadersGeneralBegin(
    void* effect, UINT* passes, DWORD flags);
static HRESULT WINAPI HookedProperShadersGeneralEnd(void* effect);
static void ResetProperShadersBatchState();
// Defined later; used by FlushProperShadersBatch to drive periodic stats in the
// unwrap build, where the wrapped Present no longer exists.
static void OnProperShadersEffectOptimizationPresent();
static void FinishProperShadersStateAttribution(const char* reason);
static bool StartProperShadersStateAttribution();
static void PollProperShadersStateAttribution(bool& triggerWasDown, bool countPresent);
static HRESULT WINAPI HookedProperShadersSetFloat(
    void* effect, const char* parameter, float value);
static HRESULT WINAPI HookedProperShadersSetTexture(
    void* effect, const char* parameter, IDirect3DBaseTexture9* texture);

// ---- ①-3b-ii generic direct hooks (vtable slots 0x98/0x88/0x80/0x100) ------
// SetMatrix/SetVector/SetFloatArray for plan-covered parameters skip D3DX
// entirely: the value goes to the effect's shadow store and, inside an active
// journal transaction, straight to the device through the journal's constant
// interface (recorded + restored like any D3DX write). BeginPass replays the
// seen shadow values afterwards because D3DX applies its own (stale) store.
// Everything else forwards to the original D3DX methods unchanged.

using ProperShadersSetMatrixRawFn = HRESULT(WINAPI*)(void*, D3DXHANDLE, const void*);
using ProperShadersSetVectorRawFn = HRESULT(WINAPI*)(void*, D3DXHANDLE, const void*);
using ProperShadersSetFloatArrayRawFn = HRESULT(WINAPI*)(void*, D3DXHANDLE, const float*, UINT);
using ProperShadersBeginPassRawFn = HRESULT(WINAPI*)(void*, UINT);
using ProperShadersCommitChangesRawFn = HRESULT(WINAPI*)(void*);

static HRESULT RestartProperShadersBaselinePass(
    void* effect,
    ProperShadersStateJournal* journal,
    HRESULT triggerHr,
    const char* phase);

static HRESULT WINAPI HookedProperShadersGenericSetMatrix(
    void* effect, D3DXHANDLE parameter, const void* matrix)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    const auto original = binding && binding->originalVtable
        ? reinterpret_cast<ProperShadersSetMatrixRawFn>(
              binding->originalVtable[0x98 / sizeof(void*)])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;
    if (matrix && binding->genericPlan && !binding->genericDirectDisabled &&
        GenericDirectHandleSet(effect, binding->genericPlan, binding->journal,
            parameter, static_cast<const float*>(matrix), 16, true)) {
        return D3D_OK;
    }
    return original(effect, parameter, matrix);
}

static HRESULT WINAPI HookedProperShadersGenericSetVector(
    void* effect, D3DXHANDLE parameter, const void* vector)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    const auto original = binding && binding->originalVtable
        ? reinterpret_cast<ProperShadersSetVectorRawFn>(
              binding->originalVtable[0x88 / sizeof(void*)])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;
    if (vector && binding->genericPlan && !binding->genericDirectDisabled &&
        GenericDirectHandleSet(effect, binding->genericPlan, binding->journal,
            parameter, static_cast<const float*>(vector), 4, false)) {
        return D3D_OK;
    }
    return original(effect, parameter, vector);
}

static HRESULT WINAPI HookedProperShadersGenericSetFloatArray(
    void* effect, D3DXHANDLE parameter, const float* values, UINT count)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    const auto original = binding && binding->originalVtable
        ? reinterpret_cast<ProperShadersSetFloatArrayRawFn>(
              binding->originalVtable[0x80 / sizeof(void*)])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;
    if (values && count && count <= 16 && binding->genericPlan &&
        !binding->genericDirectDisabled &&
        GenericDirectHandleSet(effect, binding->genericPlan, binding->journal,
            parameter, values, count, false)) {
        return D3D_OK;
    }
    return original(effect, parameter, values, count);
}

static HRESULT WINAPI HookedProperShadersGenericBeginPass(void* effect, UINT pass)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    const auto original = binding && binding->originalVtable
        ? reinterpret_cast<ProperShadersBeginPassRawFn>(
              binding->originalVtable[0x100 / sizeof(void*)])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;

    if (binding->liteActive) {
        // Stage B: D3DX was never begun; replay the validated recording.
        if (binding->journal) {
            binding->journal->SetCurrentPass(pass);
            binding->journal->SetPassActive(true);
            GenericDirectApplyPassLite(effect, binding->genericPlan, binding->journal);
        }
        return D3D_OK;
    }

    ProperShadersStateJournal* journal = binding->journal;
    if (journal && journal->IsActive()) journal->SetCurrentPass(pass);
    ProperShadersNativeCaptureScope captureScope(journal);
    if (FAILED(captureScope.BeginResult())) {
        return RestartProperShadersBaselinePass(
            effect, journal, captureScope.BeginResult(), "GeneralNativeCaptureBeginPass");
    }
    const HRESULT operationHr = original(effect, pass);
    const HRESULT hr = captureScope.Finish(operationHr);
    if (SUCCEEDED(operationHr) && journal && journal->IsActive()) {
        journal->SetPassActive(true);
    }
    if (binding->liteRecording) {
        // Under the probe, genericPlan is deliberately null; probePlan carries
        // the technique being observed.
        GenericDirectEndRecording(effect,
            binding->probePlan ? binding->probePlan : binding->genericPlan,
            binding->journal, SUCCEEDED(hr));
        binding->liteRecording = false;
    }
    if (SUCCEEDED(hr) && binding->genericPlan && !binding->genericDirectDisabled &&
        binding->journal && binding->journal->IsActive()) {
        GenericDirectReplaySeen(effect, binding->genericPlan, binding->journal);
    }
    if (!ProperShadersStateJournalRequiresBaselineRestart(journal)) return hr;
    return RestartProperShadersBaselinePass(
        effect, journal, hr, "GeneralBeginPass");
}

typedef HRESULT(WINAPI* ProperShadersEndPassRawFn)(void*);

static HRESULT WINAPI HookedProperShadersGeneralCommitChanges(void* effect)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    const auto original = binding && binding->originalVtable
        ? reinterpret_cast<ProperShadersCommitChangesRawFn>(
              binding->originalVtable[0x104 / sizeof(void*)])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;

    if (binding->liteActive) return D3D_OK;
    ProperShadersStateJournal* journal = binding->journal;
    if (!journal || !journal->IsActive()) return original(effect);
    ProperShadersNativeCaptureScope captureScope(journal);
    if (FAILED(captureScope.BeginResult())) {
        return RestartProperShadersBaselinePass(
            effect, journal, captureScope.BeginResult(),
            "GeneralNativeCaptureCommitChanges");
    }
    const HRESULT hr = captureScope.Finish(original(effect));
    if (!ProperShadersStateJournalRequiresBaselineRestart(journal)) return hr;
    return RestartProperShadersBaselinePass(
        effect, journal, hr, "GeneralCommitChanges");
}

static HRESULT WINAPI HookedProperShadersGenericEndPass(void* effect)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    const auto original = binding && binding->originalVtable
        ? reinterpret_cast<ProperShadersEndPassRawFn>(
              binding->originalVtable[0x108 / sizeof(void*)])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;
    if (binding->liteActive) {
        if (binding->journal) binding->journal->SetPassActive(false);
        return D3D_OK;
    }
    ProperShadersStateJournal* journal = binding->journal;
    const HRESULT hr = original(effect);
    if (SUCCEEDED(hr) && journal && journal->IsActive()) {
        journal->SetPassActive(false);
    }
    return hr;
}

static HRESULT WINAPI HookedProperShadersGenericSetFloat(
    void* effect, const char* parameter, float value)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    if (!binding || !binding->originalSetFloat) return D3DERR_INVALIDCALL;
    if (binding->genericPlan && !binding->genericDirectDisabled &&
        GenericDirectHandleSet(effect, binding->genericPlan, binding->journal,
            reinterpret_cast<D3DXHANDLE>(const_cast<char*>(parameter)),
            &value, 1, false)) {
        return D3D_OK;
    }
    return binding->originalSetFloat(effect, parameter, value);
}

static HRESULT WINAPI HookedProperShadersGenericSetTexture(
    void* effect, const char* parameter, IDirect3DBaseTexture9* texture)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    if (!binding || !binding->originalSetTexture) return D3DERR_INVALIDCALL;
    // Track for pass-lite dynamic textures; always forward so the D3DX store
    // stays coherent for recording transactions and fallback paths.
    GenericDirectShadowTexture(effect,
        reinterpret_cast<D3DXHANDLE>(const_cast<char*>(parameter)), texture);
    return binding->originalSetTexture(effect, parameter, texture);
}

static void ResetProperShadersDirectContext()
{
    g_properShadersDirectContext = ProperShadersDirectContext{};
}

static constexpr size_t kProperShadersVtablePrefixEntries = 5;

static bool InstallProperShadersEffectVtableHooks(ProperShadersEffectBinding& binding)
{
    if (!binding.effect || (!binding.genericJournalHooks && !binding.directHooks)) return true;
    if (binding.hookedVtable) {
        if (binding.genericJournalHooks) {
            binding.hookedVtable[0xFC / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersGeneralBegin);
            binding.hookedVtable[0x100 / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersGenericBeginPass);
            binding.hookedVtable[0x104 / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersGeneralCommitChanges);
            binding.hookedVtable[0x108 / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersGenericEndPass);
            binding.hookedVtable[0x10C / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersGeneralEnd);
            if (g_properShadersGenericDirect) {
                binding.hookedVtable[0x98 / sizeof(void*)] =
                    reinterpret_cast<void*>(&HookedProperShadersGenericSetMatrix);
                binding.hookedVtable[0x88 / sizeof(void*)] =
                    reinterpret_cast<void*>(&HookedProperShadersGenericSetVector);
                binding.hookedVtable[0x80 / sizeof(void*)] =
                    reinterpret_cast<void*>(&HookedProperShadersGenericSetFloatArray);
                if (g_properShadersGenericPassLite) {
                    binding.hookedVtable[0x78 / sizeof(void*)] =
                        reinterpret_cast<void*>(&HookedProperShadersGenericSetFloat);
                    binding.hookedVtable[0xD0 / sizeof(void*)] =
                        reinterpret_cast<void*>(&HookedProperShadersGenericSetTexture);
                }
            }
        }
        if (binding.directHooks && g_properShadersDirectConstants) {
            binding.hookedVtable[0x78 / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersSetFloat);
            binding.hookedVtable[0xD0 / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersSetTexture);
        }
        return true;
    }

    constexpr size_t kEffectVtableEntries = 0x13C / sizeof(void*);
    void** original = *reinterpret_cast<void***>(binding.effect);
    if (!original) return false;

    void** storage = static_cast<void**>(VirtualAlloc(
        nullptr,
        (kEffectVtableEntries + kProperShadersVtablePrefixEntries) * sizeof(void*),
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (!storage) return false;
    void** hooked = storage + kProperShadersVtablePrefixEntries;
    std::memcpy(hooked, original, kEffectVtableEntries * sizeof(void*));

    binding.originalRelease = reinterpret_cast<ProperShadersReleaseFn>(
        original[0x08 / sizeof(void*)]);
    binding.originalBegin = reinterpret_cast<ProperShadersBeginFn>(
        original[0xFC / sizeof(void*)]);
    binding.originalEnd = reinterpret_cast<ProperShadersEndFn>(
        original[0x10C / sizeof(void*)]);
    binding.originalSetFloat = reinterpret_cast<ProperShadersSetFloatFn>(
        original[0x78 / sizeof(void*)]);
    binding.originalSetTexture = reinterpret_cast<ProperShadersSetTextureFn>(
        original[0xD0 / sizeof(void*)]);
    if (!binding.originalRelease || !binding.originalBegin || !binding.originalEnd ||
        !binding.originalSetFloat || !binding.originalSetTexture) {
        VirtualFree(storage, 0, MEM_RELEASE);
        binding.originalRelease = nullptr;
        binding.originalBegin = nullptr;
        binding.originalEnd = nullptr;
        binding.originalSetFloat = nullptr;
        binding.originalSetTexture = nullptr;
        return false;
    }

    storage[0] = reinterpret_cast<void*>(binding.originalRelease);
    storage[1] = reinterpret_cast<void*>(binding.originalBegin);
    storage[2] = reinterpret_cast<void*>(binding.originalEnd);
    storage[3] = reinterpret_cast<void*>(binding.originalSetFloat);
    storage[4] = reinterpret_cast<void*>(binding.originalSetTexture);

    hooked[0x08 / sizeof(void*)] = reinterpret_cast<void*>(&HookedProperShadersRelease);
    if (binding.genericJournalHooks) {
        hooked[0xFC / sizeof(void*)] =
            reinterpret_cast<void*>(&HookedProperShadersGeneralBegin);
        hooked[0x100 / sizeof(void*)] =
            reinterpret_cast<void*>(&HookedProperShadersGenericBeginPass);
        hooked[0x104 / sizeof(void*)] =
            reinterpret_cast<void*>(&HookedProperShadersGeneralCommitChanges);
        hooked[0x108 / sizeof(void*)] =
            reinterpret_cast<void*>(&HookedProperShadersGenericEndPass);
        hooked[0x10C / sizeof(void*)] =
            reinterpret_cast<void*>(&HookedProperShadersGeneralEnd);
        if (g_properShadersGenericDirect) {
            // ①-3b-ii: generic direct submission for plan-covered params.
            hooked[0x98 / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersGenericSetMatrix);
            hooked[0x88 / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersGenericSetVector);
            hooked[0x80 / sizeof(void*)] =
                reinterpret_cast<void*>(&HookedProperShadersGenericSetFloatArray);
            if (g_properShadersGenericPassLite) {
                // Stage B pass-lite: EndPass must be swallowed in replay mode,
                // scalars and textures must be tracked for dynamic replay.
                hooked[0x78 / sizeof(void*)] =
                    reinterpret_cast<void*>(&HookedProperShadersGenericSetFloat);
                hooked[0xD0 / sizeof(void*)] =
                    reinterpret_cast<void*>(&HookedProperShadersGenericSetTexture);
            }
        }
    }
    if (binding.directHooks && g_properShadersDirectConstants) {
        hooked[0x78 / sizeof(void*)] = reinterpret_cast<void*>(&HookedProperShadersSetFloat);
        hooked[0xD0 / sizeof(void*)] = reinterpret_cast<void*>(&HookedProperShadersSetTexture);
    }

    auto* vtableSlot = reinterpret_cast<PVOID volatile*>(binding.effect);
    PVOID previous = InterlockedCompareExchangePointer(vtableSlot, hooked, original);
    if (previous != original) {
        VirtualFree(storage, 0, MEM_RELEASE);
        binding.originalSetFloat = nullptr;
        binding.originalSetTexture = nullptr;
        return false;
    }

    binding.originalVtable = original;
    binding.hookedVtable = hooked;
    Log("effectopt: vtable hooks attached effect={:08X} original={:08X} hooked={:08X} general={} direct={}", reinterpret_cast<std::uintptr_t>(binding.effect), reinterpret_cast<std::uintptr_t>(original), reinterpret_cast<std::uintptr_t>(hooked),
        binding.genericJournalHooks ? 1 : 0,
        binding.directHooks && g_properShadersDirectConstants ? 1 : 0);
    return true;
}

static void RestoreProperShadersEffectVtable(ProperShadersEffectBinding& binding)
{
    if (!binding.effect || !binding.originalVtable || !binding.hookedVtable) return;
    auto* vtableSlot = reinterpret_cast<PVOID volatile*>(binding.effect);
    PVOID current = InterlockedCompareExchangePointer(
        vtableSlot, binding.originalVtable, binding.hookedVtable);
    if (current == binding.hookedVtable || current == binding.originalVtable) {
        VirtualFree(
            binding.hookedVtable - kProperShadersVtablePrefixEntries,
            0,
            MEM_RELEASE);
    } else {
        Log("effectdirect: vtable changed before restore effect={:08X} current={:08X}; storage retained", reinterpret_cast<std::uintptr_t>(binding.effect), reinterpret_cast<std::uintptr_t>(current));
    }
    binding.originalVtable = nullptr;
    binding.hookedVtable = nullptr;
    binding.originalRelease = nullptr;
    binding.originalBegin = nullptr;
    binding.originalEnd = nullptr;
    binding.originalSetFloat = nullptr;
    binding.originalSetTexture = nullptr;
}

static bool ResolveProperShadersDirectHandles(ProperShadersEffectBinding& binding)
{
    if (binding.directHandles.resolved) return true;
    if (!binding.effect) return false;

    ID3DXEffect* effect = reinterpret_cast<ID3DXEffect*>(binding.effect);
    auto get = [effect](const char* name) {
        return effect->GetParameterByName(nullptr, name);
    };
    ProperShadersDirectHandles handles{};
    handles.worldViewProjection = get("g_mWorldViewProjection");
    handles.worldInverse = get("g_mWorldInv");
    handles.world = get("g_mWorld");
    handles.textureTransform = get("g_mTextureTransform");
    handles.day = get("g_vDayParam");
    handles.night = get("g_vNightParam");
    handles.material = get("g_vMaterialColor");
    handles.surfaceProps = get("g_vSurfaceProps");
    handles.pixelColorScale = get("g_fPixelColorScale");
    handles.roughness = get("g_fRoughness");
    handles.metallicness = get("g_fMetallicness");
    handles.stochastic = get("g_bStochastic");
    handles.wind = get("g_Wind");
    handles.layerId = get("g_fLayerID");
    handles.baseTexture = get("g_txBaseColor");
    handles.packedTexture = get("g_txPacked");
    handles.ambient = get("g_vAmbient");
    handles.sunStrength = get("g_fSunStrength");
    handles.specularPower = get("g_fSpecularPower");
    handles.screenSize = get("g_vScreenSize");

    if (!handles.worldViewProjection || !handles.worldInverse || !handles.world ||
        !handles.textureTransform || !handles.day || !handles.night ||
        !handles.material || !handles.surfaceProps || !handles.pixelColorScale ||
        !handles.roughness || !handles.metallicness || !handles.stochastic ||
        !handles.wind || !handles.layerId || !handles.baseTexture ||
        !handles.packedTexture || !handles.ambient || !handles.sunStrength ||
        !handles.specularPower || !handles.screenSize) {
        return false;
    }

    handles.resolved = true;
    binding.directHandles = handles;
    return true;
}

static ProperShadersStateJournal* AcquireProperShadersStateJournal(
    void* effect,
    bool genericJournalHooks = false,
    bool directHooks = false)
{
    if (!effect) return nullptr;

    if (ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect)) {
        binding->genericJournalHooks = binding->genericJournalHooks || genericJournalHooks;
        binding->directHooks = binding->directHooks || directHooks;
        if (binding->unsupported || !binding->journal || binding->journal->IsDisabled()) {
            return nullptr;
        }
        if (!InstallProperShadersEffectVtableHooks(*binding)) {
            Log("effectopt: failed to extend vtable hooks effect={:08X} general={} direct={}", reinterpret_cast<std::uintptr_t>(effect),
                binding->genericJournalHooks ? 1 : 0,
                binding->directHooks ? 1 : 0);
        }
        return binding->journal;
    }

    auto* binding = new (std::nothrow) ProperShadersEffectBinding();
    if (!binding) return nullptr;
    binding->effect = effect;
    binding->genericJournalHooks = genericJournalHooks;
    binding->directHooks = directHooks;

    const auto getStateManager = GetProperShadersEffectMethod<ProperShadersGetStateManagerFn>(
        effect, 0x120);
    const auto setStateManager = GetProperShadersEffectMethod<ProperShadersSetStateManagerFn>(
        effect, 0x11C);
    const auto getDevice = GetProperShadersEffectMethod<ProperShadersGetDeviceFn>(effect, 0x110);
    if (!getStateManager || !setStateManager || !getDevice) {
        binding->unsupported = true;
    } else {
        ID3DXEffectStateManager* existing = nullptr;
        const HRESULT managerHr = getStateManager(effect, &existing);
        if (FAILED(managerHr) || existing) {
            if (existing) existing->Release();
            binding->unsupported = true;
            Log("effectopt: baseline retained effect={:08X} existingStateManager={} hr=0x{:08X}", reinterpret_cast<std::uintptr_t>(effect), existing ? 1 : 0, static_cast<unsigned>(managerHr));
        } else {
            IDirect3DDevice9* device = nullptr;
            const HRESULT deviceHr = getDevice(effect, &device);
            if (FAILED(deviceHr) || !device) {
                if (device) device->Release();
                binding->unsupported = true;
                Log("effectopt: GetDevice failed effect={:08X} hr=0x{:08X}", reinterpret_cast<std::uintptr_t>(effect), static_cast<unsigned>(deviceHr));
            } else {
                auto* journal = new (std::nothrow) ProperShadersStateJournal(device);
                device->Release();
                if (!journal) {
                    binding->unsupported = true;
                } else {
                    const HRESULT setHr = setStateManager(effect, journal);
                    if (FAILED(setHr)) {
                        journal->Release();
                        binding->unsupported = true;
                        Log("effectopt: SetStateManager failed effect={:08X} hr=0x{:08X}", reinterpret_cast<std::uintptr_t>(effect), static_cast<unsigned>(setHr));
                    } else {
                        binding->journal = journal;
                        Log("effectopt: incremental state journal attached effect={:08X} device={:08X}", reinterpret_cast<std::uintptr_t>(effect), reinterpret_cast<std::uintptr_t>(device));
                    }
                }
            }
        }
    }

    ProperShadersEffectBinding* existingBinding = nullptr;
    bool inserted = false;
    AcquireSRWLockExclusive(&g_properShadersEffectBindingLock);
    try {
        const auto result = g_properShadersEffectBindingRegistry.emplace(effect, binding);
        inserted = result.second;
        existingBinding = result.first->second;
    } catch (const std::bad_alloc&) {
    }
    ReleaseSRWLockExclusive(&g_properShadersEffectBindingLock);

    if (!inserted) {
        if (binding->journal && setStateManager) {
            setStateManager(effect, nullptr);
            binding->journal->Release();
        }
        delete binding;
        if (!existingBinding) return nullptr;
        existingBinding->genericJournalHooks =
            existingBinding->genericJournalHooks || genericJournalHooks;
        existingBinding->directHooks = existingBinding->directHooks || directHooks;
        InstallProperShadersEffectVtableHooks(*existingBinding);
        g_properShadersEffectBindingLastHit.Remember(existingBinding);
        return existingBinding->unsupported || !existingBinding->journal ||
            existingBinding->journal->IsDisabled()
            ? nullptr
            : existingBinding->journal;
    }

    if (!InstallProperShadersEffectVtableHooks(*binding)) {
        AcquireSRWLockExclusive(&g_properShadersEffectBindingLock);
        g_properShadersEffectBindingRegistry.erase(effect);
        ReleaseSRWLockExclusive(&g_properShadersEffectBindingLock);
        if (binding->journal && setStateManager) {
            setStateManager(effect, nullptr);
            binding->journal->Release();
        }
        delete binding;
        Log("effectopt: vtable hook unavailable effect={:08X}; baseline retained", reinterpret_cast<std::uintptr_t>(effect));
        return nullptr;
    }

    try {
        g_properShadersEffectBindingCache.push_back(binding);
    } catch (const std::bad_alloc&) {
    }
    g_properShadersEffectBindingLastHit.Remember(binding);
    return binding->unsupported ? nullptr : binding->journal;
}

static void RecordProperShadersGeneralFallback(
    const char* phase, void* effect, HRESULT primaryHr, HRESULT restoreHr)
{
    const uint64_t count = ++g_properShadersGeneralFallbacks;
    if (count <= 8) {
        Log("effectgeneral: fallback={} phase={} effect={:08X} primaryHr=0x{:08X} restoreHr=0x{:08X}",
            static_cast<long long>(count),
            phase, reinterpret_cast<std::uintptr_t>(effect),
            static_cast<unsigned>(primaryHr),
            static_cast<unsigned>(restoreHr));
    }
}

static ULONG WINAPI HookedProperShadersRelease(void* effect)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    void** vtable = effect ? *reinterpret_cast<void***>(effect) : nullptr;
    const auto original = binding && binding->originalRelease
        ? binding->originalRelease
        : vtable
        ? reinterpret_cast<ProperShadersReleaseFn>(vtable[-5])
        : nullptr;
    if (!original) return 0;

    const ULONG refs = original(effect);
    if (refs != 0) return refs;

    ProperShadersEffectBinding* removed = nullptr;
    AcquireSRWLockExclusive(&g_properShadersEffectBindingLock);
    const auto registryIt = g_properShadersEffectBindingRegistry.find(effect);
    if (registryIt != g_properShadersEffectBindingRegistry.end()) {
        removed = registryIt->second;
        g_properShadersEffectBindingRegistry.erase(registryIt);
    }
    ReleaseSRWLockExclusive(&g_properShadersEffectBindingLock);

    g_properShadersEffectBindingCache.erase(
        std::remove(
            g_properShadersEffectBindingCache.begin(),
            g_properShadersEffectBindingCache.end(),
            removed),
        g_properShadersEffectBindingCache.end());
    g_properShadersEffectBindingLastHit.Forget(removed);
    DropGenericDirectPlans(effect);

    if (removed) {
        ProperShadersStateJournal* journal = removed->journal;
        void** hookedVtable = removed->hookedVtable;
        if (g_properShadersDirectContext.effect == effect) {
            ResetProperShadersDirectContext();
        }
        if (g_properShadersBatchState.effect == effect) {
            ResetProperShadersBatchState();
        }
        // The pass recorder may still hold this journal (EndRecording keeps the
        // pointer so late D3DX callbacks forward). Drop it before the release.
        ReleaseRecorderForJournal(journal);
        if (journal) journal->Release();
        if (hookedVtable) {
            VirtualFree(
                hookedVtable - kProperShadersVtablePrefixEntries,
                0,
                MEM_RELEASE);
        }
        // Other threads may retain this tiny binding in their lock-free hot cache.
        // Tombstone it instead of creating a cross-thread use-after-free risk.
        removed->effect = nullptr;
        removed->journal = nullptr;
        removed->hookedVtable = nullptr;
    }
    return refs;
}

// Labels the transaction about to run with the technique D3DX currently has
// bound. binding->lastTechnique cannot be used: ProperShaders does not set the
// technique through the two patched SetTechnique call sites, so it stays null and
// every transaction aggregates under "<unknown>". GetCurrentTechnique is the same
// source the effectdirect path already trusts (see the "effectdirect: active
// effect=... technique=..." log line); the desc name is a stable pointer into the
// effect's own string pool.
//
// Called from BOTH Begin paths. Instrumenting only the general path is why
// LitPrelight and DepthPass were absent from the probe: they run through the
// optimization Begin, which has its own BeginTransaction call.
static void LabelJournalProbeTransaction(
    void* effect, ProperShadersStateJournal* journal)
{
    if (!journal || !journal->ProbeCaptureEnabled() || !effect) return;
    const char* probeName = nullptr;
    ID3DXEffect* probeEffect = reinterpret_cast<ID3DXEffect*>(effect);
    if (D3DXHANDLE current = probeEffect->GetCurrentTechnique()) {
        D3DXTECHNIQUE_DESC probeDesc{};
        if (SUCCEEDED(probeEffect->GetTechniqueDesc(current, &probeDesc))) {
            probeName = probeDesc.Name;
        }
    }
    journal->SetProbeTechnique(probeName);
}

static HRESULT WINAPI HookedProperShadersGeneralBegin(
    void* effect, UINT* passes, DWORD flags)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    void** vtable = effect ? *reinterpret_cast<void***>(effect) : nullptr;
    const auto original = binding && binding->originalBegin
        ? binding->originalBegin
        : vtable
        ? reinterpret_cast<ProperShadersBeginFn>(vtable[-4])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;

    ProperShadersStateJournal* journal = binding ? binding->journal : nullptr;
    constexpr DWORD kD3dxFxDoNotSaveState = 0x1;
    if (!binding || !binding->genericJournalHooks || !journal ||
        journal->IsDisabled() || (flags & kD3dxFxDoNotSaveState) != 0) {
        return original(effect, passes, flags);
    }

    const HRESULT transactionHr = journal->BeginTransaction(
        ReadCurrentThreadIdFast(), flags);
    // Journal probe: label this transaction with the technique D3DX currently has
    // bound. binding->lastTechnique cannot be used: ProperShaders does not set the
    // technique through the two patched SetTechnique call sites, so it stays null
    // and every transaction aggregated under "<unknown>". GetCurrentTechnique is
    // the same source the effectdirect path already trusts (see the
    // "effectdirect: active effect=... technique=..." log line), and the desc name
    // is a stable pointer into the effect's own string pool.
    LabelJournalProbeTransaction(effect, journal);
    if (FAILED(transactionHr)) {
        RecordProperShadersGeneralFallback(
            "BeginTransaction", effect, transactionHr, D3D_OK);
        if (journal->HasFailed()) {
            const HRESULT failure = journal->FailureCode();
            journal->Disable();
            return failure;
        }
        return original(effect, passes, flags);
    }

    // Resolve the technique plan BEFORE D3DX Begin so pass-lite can bypass it.
    const GenericDirectTechniquePlan* plan = nullptr;
    if (g_properShadersGenericDirect && !binding->genericDirectDisabled) {
        ID3DXEffect* d3dxEffect = reinterpret_cast<ID3DXEffect*>(effect);
        D3DXHANDLE currentTechnique = d3dxEffect->GetCurrentTechnique();
        plan = FindGenericDirectPlan(effect, currentTechnique);
        binding->genericPlan = plan;
        if (plan) {
            static thread_local const GenericDirectTechniquePlan* loggedHits[48]{};
            for (size_t i = 0; i < 48; ++i) {
                if (loggedHits[i] == plan) break;
                if (!loggedHits[i]) {
                    loggedHits[i] = plan;
                    Log("genericdirect: begin-hit technique={} slots={} samplers={} liteEligible={}",
                        plan->name ? plan->name : "?", plan->slotCount,
                        plan->samplerCount, plan->liteEligible ? 1 : 0);
                    break;
                }
            }
        } else {
            binding->genericDirectDisabled = true;
            Log("genericdirect: effect={:08X} disabled (technique={:08X} has no plan)", reinterpret_cast<std::uintptr_t>(effect), reinterpret_cast<std::uintptr_t>(currentTechnique));
        }
    } else if (g_properShadersTraceStabilityProbe && !binding->genericDirectDisabled) {
        // Probe-only plan resolution: look up the plan for recording WITHOUT
        // writing binding->genericPlan. Leaving genericPlan null keeps the
        // direct-submission path (ReplaySeen/HandleSet) inert, so the probe
        // observes the real D3DX render without altering it.
        ID3DXEffect* d3dxEffect = reinterpret_cast<ID3DXEffect*>(effect);
        D3DXHANDLE currentTechnique = d3dxEffect->GetCurrentTechnique();
        plan = FindGenericDirectPlan(effect, currentTechnique);
        binding->probePlan = plan;
        if (plan) {
            static thread_local const GenericDirectTechniquePlan* loggedProbe[48]{};
            for (size_t i = 0; i < 48; ++i) {
                if (loggedProbe[i] == plan) break;
                if (!loggedProbe[i]) {
                    loggedProbe[i] = plan;
                    Log("tracestab: observing technique={} slots={} samplers={} liteEligible={}",
                        plan->name ? plan->name : "?", plan->slotCount,
                        plan->samplerCount, plan->liteEligible ? 1 : 0);
                    break;
                }
            }
        }
    }

    if (plan) {
        const int action = GenericDirectPassAction(plan);
        if (action == 2) {
            // Validated recording: never enter D3DX. The BeginPass hook
            // replays the recorded pass; End restores through the journal.
            if (passes) *passes = 1;
            binding->liteActive = true;
            ++g_properShadersGeneralTransactions;
            static thread_local const GenericDirectTechniquePlan* loggedLite[48]{};
            for (size_t i = 0; i < 48; ++i) {
                if (loggedLite[i] == plan) break;
                if (!loggedLite[i]) {
                    loggedLite[i] = plan;
                    Log("passlite: replay-active technique={} recBytes={}",
                        plan->name ? plan->name : "?",
                        static_cast<unsigned>(plan->liteRec1.size()));
                    break;
                }
            }
            return D3D_OK;
        }
        if (action == 1 &&
            GenericDirectBeginRecording(effect, plan, journal)) {
            binding->liteRecording = true;
        }
    }

    // Pass-lite recordings must capture the full state sequence D3DX applies
    // (Begin施加全局状态如天空/环境贴图), so the DONOTSAVESTATE bypass is
    // disabled during recording. Normal (non-recording) transactions still use
    // the bypass because the journal's transaction layer handles restoration.
    //
    // The trace-stability probe is the exception: it must NOT change the flags.
    // Dropping DONOTSAVESTATE sends D3DX down its full state-block save path,
    // which on this unwrap build (ForceDeviceWrap=0) crashed inside d3dx9 with
    // an access violation reading 0x00000000. The probe only needs the
    // per-object pass state, so it keeps the exact same flags the proven
    // non-recording path uses and simply observes the forwards.
    const DWORD effectiveFlags =
        (binding->liteRecording && !g_properShadersTraceStabilityProbe)
        ? flags
        : (flags | kD3dxFxDoNotSaveState);
    ProperShadersNativeCaptureScope captureScope(journal);
    if (FAILED(captureScope.BeginResult())) {
        const HRESULT failure = captureScope.BeginResult();
        journal->Restore();
        journal->Disable();
        RecordProperShadersGeneralFallback("NativeCaptureBegin", effect, failure, D3D_OK);
        return original(effect, passes, flags);
    }
    const HRESULT beginHr = captureScope.Finish(
        original(effect, passes, effectiveFlags));
    if (SUCCEEDED(beginHr) && !journal->HasFailed()) {
        ++g_properShadersGeneralTransactions;
        return beginHr;
    }

    if (binding->liteRecording) {
        GenericDirectEndRecording(effect, plan, journal, false);
        binding->liteRecording = false;
    }

    if (SUCCEEDED(beginHr)) {
        const auto end = binding && binding->originalEnd
            ? binding->originalEnd
            : reinterpret_cast<ProperShadersEndFn>(vtable[-3]);
        if (end) end(effect);
    }
    const HRESULT restoreHr = journal->Restore();
    const HRESULT failure = journal->HasFailed()
        ? journal->FailureCode()
        : FAILED(beginHr) ? beginHr : restoreHr;
    journal->Disable();
    RecordProperShadersGeneralFallback("Begin", effect, failure, restoreHr);
    if (FAILED(restoreHr)) return restoreHr;
    return original(effect, passes, flags);
}

static HRESULT WINAPI HookedProperShadersGeneralEnd(void* effect)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    void** vtable = effect ? *reinterpret_cast<void***>(effect) : nullptr;
    const auto original = binding && binding->originalEnd
        ? binding->originalEnd
        : vtable
        ? reinterpret_cast<ProperShadersEndFn>(vtable[-3])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;

    ProperShadersStateJournal* journal = binding ? binding->journal : nullptr;
    if (binding && binding->liteRecording) {
        // Safety: a recording whose BeginPass never came must not leave the
        // recorder installed as the effect's state manager.
        GenericDirectEndRecording(effect,
            binding->probePlan ? binding->probePlan : binding->genericPlan,
            journal, false);
        binding->liteRecording = false;
    }
    HRESULT endHr;
    if (binding && binding->liteActive) {
        // Stage B: D3DX was never begun, so there is nothing to End there.
        binding->liteActive = false;
        endHr = D3D_OK;
    } else {
        ProperShadersNativeCaptureScope captureScope(journal);
        if (FAILED(captureScope.BeginResult())) {
            endHr = captureScope.BeginResult();
        } else {
            endHr = captureScope.Finish(original(effect));
        }
    }
    if (!binding || !binding->genericJournalHooks || !journal || !journal->IsActive()) {
        return endHr;
    }

    // Journal probe: the transaction's state writes are complete here, before
    // Restore advances the generation. Hand them to the analyser, then throttle
    // a dump. Observation only — capture/restore behaviour is unchanged.
    if (journal->ProbeCaptureEnabled()) {
        journal->FlushProbeRecords();
        if (g_properShadersJournalProbe) {
            static ULONGLONG lastJournalDump = 0;
            const ULONGLONG nowTick = GetTickCount64();
            if (nowTick - lastJournalDump >= 10000ull) {
                lastJournalDump = nowTick;
                JournalProbeDump();
            }
        }
    }

    const bool journalFailed = journal->HasFailed();
    const HRESULT journalFailure = journal->FailureCode();
    const HRESULT restoreHr = journal->Restore();
    if (journalFailed || FAILED(restoreHr)) {
        journal->Disable();
        RecordProperShadersGeneralFallback(
            "End",
            effect,
            journalFailed ? journalFailure : restoreHr,
            restoreHr);
    } else {
        ++g_properShadersGeneralRestores;
    }
    return FAILED(endHr) ? endHr : restoreHr;
}

static HRESULT WINAPI HookedProperShadersD3DXCreateEffect(
    IDirect3DDevice9* device,
    const void* sourceData,
    UINT sourceDataLength,
    const void* defines,
    void* includeHandler,
    DWORD flags,
    void* pool,
    void** effect,
    void** compilationErrors)
{
    const auto original = g_originalProperShadersD3DXCreateEffect;
    if (!original) return E_FAIL;

    const HRESULT hr = original(
        device,
        sourceData,
        sourceDataLength,
        defines,
        includeHandler,
        flags,
        pool,
        effect,
        compilationErrors);
    if (SUCCEEDED(hr) && effect && *effect && g_properShadersGeneralStateJournal) {
        ProperShadersStateJournal* journal = AcquireProperShadersStateJournal(
            *effect, true, false);
        Log("effectgeneral: created effect={:08X} bytes={} journal={}", reinterpret_cast<std::uintptr_t>(*effect),
            sourceDataLength,
            journal ? 1 : 0);
    }
    if (SUCCEEDED(hr) && effect && *effect && g_properShadersInspectEffects) {
        // Read-only reconnaissance for the D3DX-bypass work: dump every
        // technique/pass and both shader constant tables of this effect.
        InspectProperShadersEffect(*effect, sourceDataLength, g_gameDir);
    }
    if (SUCCEEDED(hr) && effect && *effect && g_properShadersGenericDirectDryRun) {
        // Stage ①-3 dry run: build the generic submission plan per technique
        // and log DIRECT-OK / FALLBACK verdicts. No rendering change.
        BuildGenericDirectPlanDryRun(*effect, g_gameDir);
    }
    if (SUCCEEDED(hr) && effect && *effect &&
        (g_properShadersGenericDirect || g_properShadersGenericDirectDryRun ||
         g_properShadersTraceStabilityProbe)) {
        // Stage ①-3b: build the real CTAB-driven submission plans for this
        // effect. Consumed by the CommitChanges hook (direct submission) and by
        // the trace-stability probe (observation only). Building them under the
        // probe alone does NOT enable direct submission.
        const int okPlans = BuildGenericDirectPlans(*effect, g_gameDir);
        Log("genericdirect: plans built effect={:08X} okTechniques={}", reinterpret_cast<std::uintptr_t>(*effect), okPlans);
    }
    return hr;
}

static bool ShouldRecordProperShadersEffectCall()
{
    if (!g_properShadersEffectProfile.active) return false;
    if (GetCurrentThreadId() == g_properShadersEffectProfile.threadId) return true;
    g_properShadersEffectProfile.foreignThreadCalls.fetch_add(1);
    return false;
}

static HRESULT ProfileProperShadersSetMatrix(
    ProperShadersMatrixSlot slot, void* effect, const char* parameter, const float* matrix)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersSetMatrixFn>(effect, 0x98);
    if (!original) return D3DERR_INVALIDCALL;

    if (!ShouldRecordProperShadersEffectCall()) {
        return original(effect, parameter, matrix);
    }

    ProperShadersMatrixStats& stats =
        g_properShadersEffectProfile.matrices[static_cast<size_t>(slot)];
    const bool duplicate = matrix && stats.hasLastMatrix && stats.lastEffect == effect &&
        stats.lastParameter == parameter &&
        std::memcmp(stats.lastMatrix, matrix, sizeof(stats.lastMatrix)) == 0;

    if (duplicate) ++stats.duplicateCalls;
    if (duplicate && g_properShadersEffectProfileTestSkipDuplicateMatrices) {
        ++stats.skippedDuplicateCalls;
        ++stats.method.calls;
        return D3D_OK;
    }

    const uint64_t begin = ReadPerformanceCounter();
    const HRESULT hr = original(effect, parameter, matrix);
    const uint64_t end = ReadPerformanceCounter();
    RecordProperShadersEffectTiming(stats.method, begin, end);

    if (SUCCEEDED(hr) && matrix) {
        stats.lastEffect = effect;
        stats.lastParameter = parameter;
        std::memcpy(stats.lastMatrix, matrix, sizeof(stats.lastMatrix));
        stats.hasLastMatrix = true;
    }
    return hr;
}

static HRESULT WINAPI ProfileProperShadersWorldViewProjection(
    void* effect, const char* parameter, const float* matrix)
{
    return ProfileProperShadersSetMatrix(
        ProperShadersMatrixSlot::WorldViewProjection, effect, parameter, matrix);
}

static HRESULT WINAPI ProfileProperShadersWorldInverse(
    void* effect, const char* parameter, const float* matrix)
{
    return ProfileProperShadersSetMatrix(
        ProperShadersMatrixSlot::WorldInverse, effect, parameter, matrix);
}

static HRESULT WINAPI ProfileProperShadersWorld(
    void* effect, const char* parameter, const float* matrix)
{
    return ProfileProperShadersSetMatrix(
        ProperShadersMatrixSlot::World, effect, parameter, matrix);
}

static HRESULT WINAPI ProfileProperShadersTextureTransform(
    void* effect, const char* parameter, const float* matrix)
{
    return ProfileProperShadersSetMatrix(
        ProperShadersMatrixSlot::TextureTransform, effect, parameter, matrix);
}

static void ResetProperShadersMatrixCaches()
{
    for (auto& cache : g_properShadersMatrixCaches) {
        cache = ProperShadersMatrixCache{};
    }
}

static bool CaptureProperShadersDirectMatrix(
    ProperShadersMatrixSlot slot,
    void* effect,
    const char* parameter,
    const float* matrix)
{
    ProperShadersDirectContext& context = g_properShadersDirectContext;
    if (!context.active || context.disabled || context.effect != effect || !matrix) {
        return false;
    }
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    if (!binding || !binding->directHandles.resolved) return false;

    float* destination = nullptr;
    const char* expected = nullptr;
    uint32_t seen = 0;
    switch (slot) {
    case ProperShadersMatrixSlot::WorldViewProjection:
        destination = context.worldViewProjection;
        expected = binding->directHandles.worldViewProjection;
        seen = DirectSeenWorldViewProjection;
        break;
    case ProperShadersMatrixSlot::WorldInverse:
        destination = context.worldInverse;
        expected = binding->directHandles.worldInverse;
        seen = DirectSeenWorldInverse;
        break;
    case ProperShadersMatrixSlot::World:
        destination = context.world;
        expected = binding->directHandles.world;
        seen = DirectSeenWorld;
        break;
    case ProperShadersMatrixSlot::TextureTransform:
        destination = context.textureTransform;
        expected = binding->directHandles.textureTransform;
        seen = DirectSeenTextureTransform;
        break;
    default:
        return false;
    }
    if (parameter != expected) return false;

    std::memcpy(destination, matrix, sizeof(float) * 16);
    context.seen |= seen;
    context.captured |= seen;
    return true;
}

static bool CaptureProperShadersDirectVector(
    ProperShadersParameterSlot slot,
    void* effect,
    const char* parameter,
    const float* vector)
{
    ProperShadersDirectContext& context = g_properShadersDirectContext;
    if (!context.active || context.disabled || context.effect != effect || !vector) {
        return false;
    }
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    if (!binding || !binding->directHandles.resolved) return false;

    float* destination = nullptr;
    const char* expected = nullptr;
    uint32_t seen = 0;
    switch (slot) {
    case ProperShadersParameterSlot::Day:
        destination = context.day;
        expected = binding->directHandles.day;
        seen = DirectSeenDay;
        break;
    case ProperShadersParameterSlot::Night:
        destination = context.night;
        expected = binding->directHandles.night;
        seen = DirectSeenNight;
        break;
    case ProperShadersParameterSlot::Material:
        destination = context.material;
        expected = binding->directHandles.material;
        seen = DirectSeenMaterial;
        break;
    default:
        return false;
    }
    if (parameter != expected) return false;

    std::memcpy(destination, vector, sizeof(float) * 4);
    context.seen |= seen;
    context.captured |= seen;
    return true;
}

static bool CaptureProperShadersDirectFloatArray(
    ProperShadersParameterSlot slot,
    void* effect,
    const char* parameter,
    const float* values,
    UINT count)
{
    ProperShadersDirectContext& context = g_properShadersDirectContext;
    if (!context.active || context.disabled || context.effect != effect ||
        !values || count < 3) {
        return false;
    }
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    if (!binding || !binding->directHandles.resolved) return false;

    if (slot == ProperShadersParameterSlot::SurfaceProps &&
        parameter == binding->directHandles.surfaceProps) {
        std::memcpy(context.surfaceProps, values, sizeof(context.surfaceProps));
        context.seen |= DirectSeenSurfaceProps;
        context.captured |= DirectSeenSurfaceProps;
        return true;
    }
    if (slot == ProperShadersParameterSlot::Ambient &&
        parameter == binding->directHandles.ambient) {
        std::memcpy(context.ambient, values, sizeof(context.ambient));
        return false;
    }
    return false;
}

static HRESULT OptimizedProperShadersSetMatrix(
    ProperShadersMatrixSlot slot, void* effect, const char* parameter, const float* matrix)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersSetMatrixFn>(effect, 0x98);
    if (!original) return D3DERR_INVALIDCALL;
    if (CaptureProperShadersDirectMatrix(slot, effect, parameter, matrix)) {
        return D3D_OK;
    }
    if (!g_properShadersSkipDuplicateMatrices || !matrix) {
        return original(effect, parameter, matrix);
    }

    ++g_properShadersMatrixCalls;
    ProperShadersMatrixCache& cache =
        g_properShadersMatrixCaches[static_cast<size_t>(slot)];
    if (cache.valid && cache.effect == effect && cache.parameter == parameter &&
        std::memcmp(cache.matrix, matrix, sizeof(cache.matrix)) == 0) {
        ++g_properShadersMatrixSkips;
        return D3D_OK;
    }

    const HRESULT hr = original(effect, parameter, matrix);
    if (SUCCEEDED(hr)) {
        cache.effect = effect;
        cache.parameter = parameter;
        std::memcpy(cache.matrix, matrix, sizeof(cache.matrix));
        cache.valid = true;
    }
    return hr;
}

static HRESULT WINAPI OptimizedProperShadersWorldViewProjection(
    void* effect, const char* parameter, const float* matrix)
{
    return OptimizedProperShadersSetMatrix(
        ProperShadersMatrixSlot::WorldViewProjection, effect, parameter, matrix);
}

static HRESULT WINAPI OptimizedProperShadersWorldInverse(
    void* effect, const char* parameter, const float* matrix)
{
    return OptimizedProperShadersSetMatrix(
        ProperShadersMatrixSlot::WorldInverse, effect, parameter, matrix);
}

static HRESULT WINAPI OptimizedProperShadersWorld(
    void* effect, const char* parameter, const float* matrix)
{
    return OptimizedProperShadersSetMatrix(
        ProperShadersMatrixSlot::World, effect, parameter, matrix);
}

static HRESULT WINAPI OptimizedProperShadersTextureTransform(
    void* effect, const char* parameter, const float* matrix)
{
    return OptimizedProperShadersSetMatrix(
        ProperShadersMatrixSlot::TextureTransform, effect, parameter, matrix);
}

static void ResetProperShadersParameterCaches()
{
    for (auto& cache : g_properShadersParameterCaches) {
        cache = ProperShadersParameterCache{};
    }
}

static bool ShouldSkipProperShadersParameter(
    ProperShadersParameterSlot slot,
    void* effect,
    const char* parameter,
    const void* data,
    size_t size)
{
    if (!g_properShadersSkipDuplicateParameters || !data || !size ||
        size > sizeof(g_properShadersParameterCaches[0].data)) {
        return false;
    }

    ++g_properShadersParameterCalls;
    ProperShadersParameterCache& cache =
        g_properShadersParameterCaches[static_cast<size_t>(slot)];
    if (cache.valid && cache.effect == effect && cache.parameter == parameter &&
        cache.size == size && std::memcmp(cache.data, data, size) == 0) {
        ++g_properShadersParameterSkips;
        return true;
    }
    return false;
}

static void RememberProperShadersParameter(
    ProperShadersParameterSlot slot,
    void* effect,
    const char* parameter,
    const void* data,
    size_t size)
{
    if (!g_properShadersSkipDuplicateParameters || !data || !size ||
        size > sizeof(g_properShadersParameterCaches[0].data)) {
        return;
    }

    ProperShadersParameterCache& cache =
        g_properShadersParameterCaches[static_cast<size_t>(slot)];
    cache.effect = effect;
    cache.parameter = parameter;
    std::memcpy(cache.data, data, size);
    cache.size = static_cast<uint8_t>(size);
    cache.valid = true;
}

static HRESULT OptimizedProperShadersSetVector(
    ProperShadersParameterSlot slot,
    void* effect,
    const char* parameter,
    const float* vector)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersSetVectorFn>(effect, 0x88);
    if (!original) return D3DERR_INVALIDCALL;
    if (CaptureProperShadersDirectVector(slot, effect, parameter, vector)) {
        return D3D_OK;
    }
    constexpr size_t kVectorBytes = sizeof(float) * 4;
    if (ShouldSkipProperShadersParameter(slot, effect, parameter, vector, kVectorBytes)) {
        return D3D_OK;
    }

    const HRESULT hr = original(effect, parameter, vector);
    if (SUCCEEDED(hr)) {
        RememberProperShadersParameter(slot, effect, parameter, vector, kVectorBytes);
    }
    return hr;
}

static HRESULT OptimizedProperShadersSetFloatArray(
    ProperShadersParameterSlot slot,
    void* effect,
    const char* parameter,
    const float* values,
    UINT count)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersSetFloatArrayFn>(effect, 0x80);
    if (!original) return D3DERR_INVALIDCALL;
    if (CaptureProperShadersDirectFloatArray(
            slot, effect, parameter, values, count)) {
        return D3D_OK;
    }
    const size_t size = static_cast<size_t>(count) * sizeof(float);
    if (ShouldSkipProperShadersParameter(slot, effect, parameter, values, size)) {
        return D3D_OK;
    }

    const HRESULT hr = original(effect, parameter, values, count);
    if (SUCCEEDED(hr)) {
        RememberProperShadersParameter(slot, effect, parameter, values, size);
    }
    return hr;
}

static HRESULT WINAPI OptimizedProperShadersDayVector(
    void* effect, const char* parameter, const float* vector)
{
    return OptimizedProperShadersSetVector(
        ProperShadersParameterSlot::Day, effect, parameter, vector);
}

static HRESULT WINAPI OptimizedProperShadersNightVector(
    void* effect, const char* parameter, const float* vector)
{
    return OptimizedProperShadersSetVector(
        ProperShadersParameterSlot::Night, effect, parameter, vector);
}

static HRESULT WINAPI OptimizedProperShadersMaterialVector(
    void* effect, const char* parameter, const float* vector)
{
    return OptimizedProperShadersSetVector(
        ProperShadersParameterSlot::Material, effect, parameter, vector);
}

static HRESULT WINAPI OptimizedProperShadersAmbientFloatArray(
    void* effect, const char* parameter, const float* values, UINT count)
{
    return OptimizedProperShadersSetFloatArray(
        ProperShadersParameterSlot::Ambient, effect, parameter, values, count);
}

static HRESULT WINAPI OptimizedProperShadersSurfacePropsFloatArray(
    void* effect, const char* parameter, const float* values, UINT count)
{
    return OptimizedProperShadersSetFloatArray(
        ProperShadersParameterSlot::SurfaceProps, effect, parameter, values, count);
}

static HRESULT WINAPI HookedProperShadersSetFloat(
    void* effect, const char* parameter, float value)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    void** vtable = effect ? *reinterpret_cast<void***>(effect) : nullptr;
    const auto original = binding && binding->originalSetFloat
        ? binding->originalSetFloat
        : vtable
        ? reinterpret_cast<ProperShadersSetFloatFn>(vtable[-2])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;
    if (!binding) return original(effect, parameter, value);

    ProperShadersDirectContext& context = g_properShadersDirectContext;
    if (!context.active || context.disabled || context.effect != effect ||
        !binding->directHandles.resolved) {
        return original(effect, parameter, value);
    }

    uint32_t seen = 0;
    float* destination = nullptr;
    const ProperShadersDirectHandles& handles = binding->directHandles;
    if (parameter == handles.pixelColorScale) {
        destination = &context.pixelColorScale;
        seen = DirectSeenPixelColorScale;
    } else if (parameter == handles.roughness) {
        destination = &context.roughness;
        seen = DirectSeenRoughness;
    } else if (parameter == handles.metallicness) {
        destination = &context.metallicness;
        seen = DirectSeenMetallicness;
    } else if (parameter == handles.stochastic) {
        destination = &context.stochastic;
        seen = DirectSeenStochastic;
    } else if (parameter == handles.wind) {
        destination = &context.wind;
        seen = DirectSeenWind;
    } else if (parameter == handles.layerId) {
        destination = &context.layerId;
        seen = DirectSeenLayerId;
    }
    if (!destination) return original(effect, parameter, value);

    *destination = value;
    context.seen |= seen;
    context.captured |= seen;
    return D3D_OK;
}

static HRESULT WINAPI HookedProperShadersSetTexture(
    void* effect, const char* parameter, IDirect3DBaseTexture9* texture)
{
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    void** vtable = effect ? *reinterpret_cast<void***>(effect) : nullptr;
    const auto original = binding && binding->originalSetTexture
        ? binding->originalSetTexture
        : vtable
        ? reinterpret_cast<ProperShadersSetTextureFn>(vtable[-1])
        : nullptr;
    if (!original) return D3DERR_INVALIDCALL;
    if (!binding) return original(effect, parameter, texture);

    const HRESULT hr = original(effect, parameter, texture);
    ProperShadersDirectContext& context = g_properShadersDirectContext;
    if (FAILED(hr) || !context.active || context.disabled || context.effect != effect ||
        !binding->directHandles.resolved) {
        return hr;
    }

    if (parameter == binding->directHandles.baseTexture) {
        context.baseTexture = texture;
        context.seen |= DirectSeenBaseTexture;
        context.captured |= DirectSeenBaseTexture;
    } else if (parameter == binding->directHandles.packedTexture) {
        context.seen |= DirectSeenPackedTexture;
        context.captured |= DirectSeenPackedTexture;
    }
    return hr;
}

static HRESULT WINAPI ProfileProperShadersCommitChanges(void* effect)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersCommitChangesFn>(effect, 0x104);
    if (!original) return D3DERR_INVALIDCALL;
    if (!ShouldRecordProperShadersEffectCall()) return original(effect);

    const uint64_t begin = ReadPerformanceCounter();
    const HRESULT hr = original(effect);
    const uint64_t end = ReadPerformanceCounter();
    RecordProperShadersEffectTiming(g_properShadersEffectProfile.commitChanges, begin, end);
    return hr;
}

static HRESULT WINAPI ProfileProperShadersBegin(void* effect, UINT* passes, DWORD flags)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersBeginFn>(effect, 0xFC);
    if (!original) return D3DERR_INVALIDCALL;
    if (!ShouldRecordProperShadersEffectCall()) return original(effect, passes, flags);

    constexpr DWORD kD3dxFxDoNotSaveState = 0x1;
    const DWORD appliedFlags = g_properShadersEffectProfileTestNoSaveState
        ? flags | kD3dxFxDoNotSaveState
        : flags;
    g_properShadersEffectProfile.beginOriginalFlagsOr |= flags;
    g_properShadersEffectProfile.beginAppliedFlagsOr |= appliedFlags;
    if (appliedFlags != flags) {
        ++g_properShadersEffectProfile.beginFlagsModifiedCalls;
    }

    const uint64_t begin = ReadPerformanceCounter();
    const HRESULT hr = original(effect, passes, appliedFlags);
    const uint64_t end = ReadPerformanceCounter();
    RecordProperShadersEffectTiming(g_properShadersEffectProfile.begin, begin, end);
    return hr;
}

static HRESULT WINAPI OptimizedProperShadersBegin(void* effect, UINT* passes, DWORD flags)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersBeginFn>(effect, 0xFC);
    if (!original) {
        ++g_properShadersOptimizationFailures;
        return D3DERR_INVALIDCALL;
    }

    ++g_properShadersOptimizationBeginCalls;
    if (!g_properShadersNoSaveStateActive.load(std::memory_order_relaxed) ||
        !g_properShadersEffectOptimizationStateJournal) {
        return original(effect, passes, flags);
    }

    ProperShadersStateJournal* journal = AcquireProperShadersStateJournal(
        effect, false, true);
    if (!journal) return original(effect, passes, flags);

    const HRESULT transactionHr = journal->BeginTransaction(ReadCurrentThreadIdFast(), flags);
    LabelJournalProbeTransaction(effect, journal);
    if (FAILED(transactionHr)) {
        const bool journalFailed = journal->HasFailed();
        const HRESULT failure = journalFailed
            ? journal->FailureCode()
            : transactionHr;
        journal->Disable();
        ++g_properShadersOptimizationFailures;
        Log("effectopt: BeginTransaction failed effect={:08X} hr=0x{:08X}; action={}", reinterpret_cast<std::uintptr_t>(effect),
            static_cast<unsigned>(failure),
            journalFailed ? "propagate-failure" : "baseline-fallback");
        if (journalFailed) return failure;
        return original(effect, passes, flags);
    }

    const DWORD appliedFlags = flags | D3DXFX_DONOTSAVESTATE;
    if (appliedFlags != flags) {
        ++g_properShadersOptimizationModifiedCalls;
    }
    ProperShadersNativeCaptureScope captureScope(journal);
    if (FAILED(captureScope.BeginResult())) {
        const HRESULT failure = captureScope.BeginResult();
        journal->Restore();
        journal->Disable();
        ++g_properShadersOptimizationFailures;
        Log("effectopt: native capture Begin failed effect={:08X} hr=0x{:08X}", reinterpret_cast<std::uintptr_t>(effect), static_cast<unsigned>(failure));
        return original(effect, passes, flags);
    }
    const HRESULT hr = captureScope.Finish(
        original(effect, passes, appliedFlags));
    if (SUCCEEDED(hr) && !journal->HasFailed()) return hr;

    if (SUCCEEDED(hr)) {
        const auto end = GetProperShadersEffectMethod<ProperShadersEndFn>(effect, 0x10C);
        if (end) end(effect);
    }
    const HRESULT restoreHr = journal->Restore();
    ResetProperShadersParameterCaches();
    const HRESULT failure = journal->HasFailed()
        ? journal->FailureCode()
        : FAILED(hr) ? hr : restoreHr;
    journal->Disable();
    ++g_properShadersOptimizationFailures;
    Log("effectopt: journal Begin fallback effect={:08X} beginHr=0x{:08X} restoreHr=0x{:08X} failure=0x{:08X}", reinterpret_cast<std::uintptr_t>(effect),
        static_cast<unsigned>(hr),
        static_cast<unsigned>(restoreHr),
        static_cast<unsigned>(failure));
    if (FAILED(restoreHr)) return restoreHr;
    return original(effect, passes, flags);
}

template <typename Shader>
static bool GetProperShadersShaderHash(
    Shader* shader, bool pixel, uint64_t& hash, UINT& bytecodeSize)
{
    hash = 0;
    bytecodeSize = 0;
    if (!shader) return false;
    for (const auto& entry : g_properShadersShaderHashCache) {
        if (entry.shader == shader && entry.pixel == pixel) {
            hash = entry.hash;
            bytecodeSize = entry.bytecodeSize;
            return true;
        }
    }

    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || !size || size > 0x100000) {
        return false;
    }
    std::vector<uint8_t> bytecode;
    try {
        bytecode.resize(size);
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (FAILED(shader->GetFunction(bytecode.data(), &size))) return false;

    uint64_t value = 1469598103934665603ull;
    for (UINT i = 0; i < size; ++i) {
        value ^= bytecode[i];
        value *= 1099511628211ull;
    }

    try {
        if (g_properShadersShaderHashCache.empty()) {
            g_properShadersShaderHashCache.reserve(16);
        }
        g_properShadersShaderHashCache.push_back({shader, value, size, pixel});
    } catch (const std::bad_alloc&) {
        return false;
    }
    hash = value;
    bytecodeSize = size;
    return true;
}

static ProperShadersDirectProfile ProperShadersDirectProfileFromTechnique(
    const char* technique)
{
    if (!technique) return ProperShadersDirectProfile::None;
    if (std::strcmp(technique, "LitPrelight") == 0) {
        return ProperShadersDirectProfile::LitPrelight;
    }
    if (std::strcmp(technique, "LitPrelight_ShadowMask") == 0) {
        return ProperShadersDirectProfile::LitPrelightShadowMask;
    }
    if (std::strcmp(technique, "LitPrelight_NoShadows") == 0) {
        return ProperShadersDirectProfile::LitPrelightNoShadows;
    }
    if (std::strcmp(technique, "LitPrelight_Deferred") == 0) {
        return ProperShadersDirectProfile::LitPrelightDeferred;
    }
    return ProperShadersDirectProfile::None;
}

static bool IsExpectedProperShadersDirectShader(
    ProperShadersDirectProfile profile,
    uint64_t vertexHash,
    UINT vertexSize,
    uint64_t pixelHash,
    UINT pixelSize)
{
    if (vertexHash != 0x7A05E62D875B5816ull || vertexSize != 3060) return false;
    switch (profile) {
    case ProperShadersDirectProfile::LitPrelight:
        return pixelHash == 0x54642949E230ADFCull && pixelSize == 31164;
    case ProperShadersDirectProfile::LitPrelightShadowMask:
        return pixelHash == 0xD9B34F001D125589ull && pixelSize == 4264;
    case ProperShadersDirectProfile::LitPrelightNoShadows:
        return pixelHash == 0x704F2E0BC4E27025ull && pixelSize == 3940;
    case ProperShadersDirectProfile::LitPrelightDeferred:
        return pixelHash == 0x2B576F1D89218BB0ull && pixelSize == 2864;
    default:
        return false;
    }
}

static bool ActivateProperShadersDirectConstants(
    void* effect, ProperShadersStateJournal* journal)
{
    ResetProperShadersDirectContext();
    if (!g_properShadersDirectConstants || !effect || !journal ||
        !journal->IsActive() || !journal->Device()) {
        return false;
    }

    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    if (!binding || !binding->hookedVtable ||
        !ResolveProperShadersDirectHandles(*binding)) {
        return false;
    }

    ID3DXEffect* d3dxEffect = reinterpret_cast<ID3DXEffect*>(effect);
    D3DXHANDLE technique = d3dxEffect->GetCurrentTechnique();
    D3DXTECHNIQUE_DESC techniqueDesc{};
    if (!technique || FAILED(d3dxEffect->GetTechniqueDesc(technique, &techniqueDesc))) {
        return false;
    }
    const ProperShadersDirectProfile profile =
        ProperShadersDirectProfileFromTechnique(techniqueDesc.Name);
    if (profile == ProperShadersDirectProfile::None || techniqueDesc.Passes != 1) {
        return false;
    }

    IDirect3DDevice9* device = journal->Device();
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    if (FAILED(device->GetVertexShader(&vertexShader)) || !vertexShader ||
        FAILED(device->GetPixelShader(&pixelShader)) || !pixelShader) {
        if (vertexShader) vertexShader->Release();
        if (pixelShader) pixelShader->Release();
        return false;
    }

    uint64_t vertexHash = 0;
    uint64_t pixelHash = 0;
    UINT vertexSize = 0;
    UINT pixelSize = 0;
    const bool hashesValid = GetProperShadersShaderHash(
        vertexShader, false, vertexHash, vertexSize) &&
        GetProperShadersShaderHash(pixelShader, true, pixelHash, pixelSize);
    vertexShader->Release();
    pixelShader->Release();
    if (!hashesValid || !IsExpectedProperShadersDirectShader(
            profile, vertexHash, vertexSize, pixelHash, pixelSize)) {
        return false;
    }

    ProperShadersDirectContext context{};
    context.effect = effect;
    context.profile = profile;
    const ProperShadersDirectHandles& handles = binding->directHandles;
    if (FAILED(d3dxEffect->GetFloatArray(handles.ambient, context.ambient, 3)) ||
        FAILED(d3dxEffect->GetFloat(handles.sunStrength, &context.sunStrength)) ||
        FAILED(d3dxEffect->GetFloat(handles.specularPower, &context.specularPower)) ||
        FAILED(d3dxEffect->GetFloatArray(handles.screenSize, context.screenSize, 2)) ||
        FAILED(device->GetVertexShaderConstantF(13, context.vs13To14, 2)) ||
        FAILED(device->GetPixelShaderConstantF(1, context.ps1To4, 4)) ||
        FAILED(device->GetPixelShaderConstantF(9, context.ps9To10, 2)) ||
        FAILED(device->GetPixelShaderConstantF(137, context.ps137To138, 2)) ||
        FAILED(device->GetPixelShaderConstantF(173, context.ps173To174, 2))) {
        return false;
    }

    context.active = true;
    g_properShadersDirectContext = context;
    ++g_properShadersDirectActivations;

    static thread_local bool loggedProfiles[5]{};
    const size_t profileIndex = static_cast<size_t>(profile);
    if (profileIndex < sizeof(loggedProfiles) / sizeof(loggedProfiles[0]) &&
        !loggedProfiles[profileIndex]) {
        loggedProfiles[profileIndex] = true;
        Log("effectdirect: active effect={:08X} technique={} vs={}/0x{:016X} ps={}/0x{:016X}", reinterpret_cast<std::uintptr_t>(effect),
            techniqueDesc.Name ? techniqueDesc.Name : "",
            vertexSize,
            static_cast<unsigned long long>(vertexHash),
            pixelSize,
            static_cast<unsigned long long>(pixelHash));
    }
    return true;
}

static HRESULT SynchronizeProperShadersDirectParameters(void* effect)
{
    ProperShadersDirectContext& context = g_properShadersDirectContext;
    if (!context.active || context.effect != effect || !context.captured) return D3D_OK;

    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    if (!binding || !binding->directHandles.resolved || !binding->originalSetFloat) {
        return D3DERR_INVALIDCALL;
    }
    const auto setMatrix = GetProperShadersEffectMethod<ProperShadersSetMatrixFn>(
        effect, 0x98);
    const auto setVector = GetProperShadersEffectMethod<ProperShadersSetVectorFn>(
        effect, 0x88);
    const auto setFloatArray = GetProperShadersEffectMethod<ProperShadersSetFloatArrayFn>(
        effect, 0x80);
    if (!setMatrix || !setVector || !setFloatArray) return D3DERR_INVALIDCALL;

    HRESULT result = D3D_OK;
    auto record = [&result](HRESULT hr) {
        if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    };
    const uint32_t captured = context.captured;
    const ProperShadersDirectHandles& h = binding->directHandles;
    if (captured & DirectSeenWorldViewProjection) {
        record(setMatrix(effect, h.worldViewProjection, context.worldViewProjection));
    }
    if (captured & DirectSeenWorldInverse) {
        record(setMatrix(effect, h.worldInverse, context.worldInverse));
    }
    if (captured & DirectSeenWorld) {
        record(setMatrix(effect, h.world, context.world));
    }
    if (captured & DirectSeenTextureTransform) {
        record(setMatrix(effect, h.textureTransform, context.textureTransform));
    }
    if (captured & DirectSeenDay) record(setVector(effect, h.day, context.day));
    if (captured & DirectSeenNight) record(setVector(effect, h.night, context.night));
    if (captured & DirectSeenMaterial) {
        record(setVector(effect, h.material, context.material));
    }
    if (captured & DirectSeenSurfaceProps) {
        record(setFloatArray(effect, h.surfaceProps, context.surfaceProps, 3));
    }
    if (captured & DirectSeenPixelColorScale) {
        record(binding->originalSetFloat(effect, h.pixelColorScale, context.pixelColorScale));
    }
    if (captured & DirectSeenRoughness) {
        record(binding->originalSetFloat(effect, h.roughness, context.roughness));
    }
    if (captured & DirectSeenMetallicness) {
        record(binding->originalSetFloat(effect, h.metallicness, context.metallicness));
    }
    if (captured & DirectSeenStochastic) {
        record(binding->originalSetFloat(effect, h.stochastic, context.stochastic));
    }
    if (captured & DirectSeenWind) {
        record(binding->originalSetFloat(effect, h.wind, context.wind));
    }
    if (captured & DirectSeenLayerId) {
        record(binding->originalSetFloat(effect, h.layerId, context.layerId));
    }
    return result;
}

static void TransposeProperShadersMatrix(const float* source, float* destination)
{
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            destination[column * 4 + row] = source[row * 4 + column];
        }
    }
}

static HRESULT SubmitProperShadersDirectState(
    ProperShadersStateJournal* journal,
    const D3D9GtaSaFloatConstantRange* vertexFloatRanges,
    UINT vertexFloatRangeCount,
    const D3D9GtaSaBoolConstantRange* vertexBoolRanges,
    UINT vertexBoolRangeCount,
    const D3D9GtaSaFloatConstantRange* pixelFloatRanges,
    UINT pixelFloatRangeCount,
    const D3D9GtaSaBoolConstantRange* pixelBoolRanges,
    UINT pixelBoolRangeCount,
    const D3D9GtaSaTextureBinding* textureBindings,
    UINT textureBindingCount)
{
    D3D9GtaSaStateBatch batch{};
    batch.StructSize = sizeof(batch);
    batch.ApiVersion = GtaSaCompatApiVersions::kStateBatch;
    batch.VertexFloatRangeCount = vertexFloatRangeCount;
    batch.VertexFloatRanges = vertexFloatRanges;
    batch.VertexBoolRangeCount = vertexBoolRangeCount;
    batch.VertexBoolRanges = vertexBoolRanges;
    batch.PixelFloatRangeCount = pixelFloatRangeCount;
    batch.PixelFloatRanges = pixelFloatRanges;
    batch.PixelBoolRangeCount = pixelBoolRangeCount;
    batch.PixelBoolRanges = pixelBoolRanges;
    batch.TextureBindingCount = textureBindingCount;
    batch.TextureBindings = textureBindings;

    if (journal->SupportsStateBatch()) {
        const HRESULT batchHr = journal->SubmitStateBatch(&batch);
        if (batchHr != E_NOINTERFACE) {
            if (SUCCEEDED(batchHr)) {
                ++g_properShadersDirectBatchSubmissions;
                g_properShadersDirectVsWrites +=
                    vertexFloatRangeCount + vertexBoolRangeCount;
                g_properShadersDirectPsWrites +=
                    pixelFloatRangeCount + pixelBoolRangeCount;
                g_properShadersDirectTextureWrites += textureBindingCount;
            }
            return batchHr;
        }
    }

    HRESULT hr = D3D_OK;
    for (UINT i = 0; i < vertexFloatRangeCount; ++i) {
        const auto& range = vertexFloatRanges[i];
        hr = journal->SetVertexShaderConstantF(
            range.StartRegister, range.Data, range.RegisterCount);
        if (FAILED(hr)) return hr;
        ++g_properShadersDirectVsWrites;
    }
    for (UINT i = 0; i < vertexBoolRangeCount; ++i) {
        const auto& range = vertexBoolRanges[i];
        hr = journal->SetVertexShaderConstantB(
            range.StartRegister, range.Data, range.RegisterCount);
        if (FAILED(hr)) return hr;
        ++g_properShadersDirectVsWrites;
    }
    for (UINT i = 0; i < pixelFloatRangeCount; ++i) {
        const auto& range = pixelFloatRanges[i];
        hr = journal->SetPixelShaderConstantF(
            range.StartRegister, range.Data, range.RegisterCount);
        if (FAILED(hr)) return hr;
        ++g_properShadersDirectPsWrites;
    }
    for (UINT i = 0; i < pixelBoolRangeCount; ++i) {
        const auto& range = pixelBoolRanges[i];
        hr = journal->SetPixelShaderConstantB(
            range.StartRegister, range.Data, range.RegisterCount);
        if (FAILED(hr)) return hr;
        ++g_properShadersDirectPsWrites;
    }
    for (UINT i = 0; i < textureBindingCount; ++i) {
        const auto& binding = textureBindings[i];
        hr = journal->SetTexture(binding.Stage, binding.Texture);
        if (FAILED(hr)) return hr;
        ++g_properShadersDirectTextureWrites;
    }
    return D3D_OK;
}

static HRESULT CommitProperShadersDirectConstants(
    void* effect, ProperShadersStateJournal* journal, bool& attempted)
{
    attempted = false;
    ProperShadersDirectContext& context = g_properShadersDirectContext;
    if (!context.active || context.disabled || context.effect != effect || !journal ||
        !journal->IsActive()) {
        return D3D_OK;
    }
    attempted = true;

    constexpr uint32_t kRequired = (1u << 16) - 1u;
    if ((context.seen & kRequired) != kRequired) return S_FALSE;

    float vertex0To7[8 * 4]{};
    TransposeProperShadersMatrix(context.worldViewProjection, vertex0To7);
    TransposeProperShadersMatrix(context.world, vertex0To7 + 16);

    float vertex13To18[6 * 4]{};
    std::memcpy(vertex13To18, context.vs13To14, sizeof(context.vs13To14));
    const float reciprocalPixelScale = 1.0f / context.pixelColorScale;
    for (unsigned i = 0; i < 4; ++i) {
        vertex13To18[i] = context.material[i] * reciprocalPixelScale;
    }
    for (unsigned i = 0; i < 3; ++i) {
        vertex13To18[4 + i] = context.surfaceProps[0] * context.ambient[i];
    }
    for (unsigned i = 0; i < 4; ++i) {
        vertex13To18[8 + i] = context.textureTransform[i * 4];
        vertex13To18[12 + i] = context.textureTransform[i * 4 + 1];
    }
    std::memcpy(vertex13To18 + 16, context.day, sizeof(context.day));
    std::memcpy(vertex13To18 + 20, context.night, sizeof(context.night));

    const D3D9GtaSaFloatConstantRange vertexFloatRanges[] = {
        { 0, 8, vertex0To7 },
        { 13, 6, vertex13To18 },
    };
    const BOOL windEnabled = context.wind != 0.0f ? TRUE : FALSE;
    const D3D9GtaSaBoolConstantRange vertexBoolRanges[] = {
        { 0, 1, &windEnabled },
    };

    const float oneMinusRoughness = 1.0f - context.roughness;
    const BOOL stochasticEnabled = context.stochastic != 0.0f ? TRUE : FALSE;
    float pixel1To4[16]{};
    float pixel9To10[8]{};
    float pixel137To138[8]{};
    float pixel173To174[8]{};
    D3D9GtaSaFloatConstantRange pixelFloatRanges[3]{};
    UINT pixelFloatRangeCount = 0;

    switch (context.profile) {
    case ProperShadersDirectProfile::LitPrelight: {
        std::memcpy(pixel1To4, context.ps1To4, 4 * sizeof(float));
        std::memcpy(pixel137To138, context.ps137To138, sizeof(pixel137To138));
        std::memcpy(pixel173To174, context.ps173To174, sizeof(pixel173To174));
        pixel1To4[0] = oneMinusRoughness * context.specularPower * context.sunStrength;
        pixel137To138[0] = oneMinusRoughness;
        pixel137To138[4] = context.layerId;
        pixel173To174[0] = context.ambient[0];
        pixel173To174[1] = context.ambient[1];
        pixel173To174[2] = context.ambient[2];
        pixel173To174[4] = context.pixelColorScale;
        pixelFloatRanges[0] = { 1, 1, pixel1To4 };
        pixelFloatRanges[1] = { 137, 2, pixel137To138 };
        pixelFloatRanges[2] = { 173, 2, pixel173To174 };
        pixelFloatRangeCount = 3;
        break;
    }
    case ProperShadersDirectProfile::LitPrelightShadowMask: {
        std::memcpy(pixel1To4, context.ps1To4, sizeof(pixel1To4));
        std::memcpy(pixel9To10, context.ps9To10, sizeof(pixel9To10));
        pixel1To4[0] = oneMinusRoughness * context.specularPower * context.sunStrength;
        pixel1To4[8] = oneMinusRoughness;
        pixel1To4[12] = context.layerId;
        pixel9To10[0] = context.ambient[0];
        pixel9To10[1] = context.ambient[1];
        pixel9To10[2] = context.ambient[2];
        pixel9To10[4] = context.pixelColorScale;
        pixelFloatRanges[0] = { 1, 4, pixel1To4 };
        pixelFloatRanges[1] = { 9, 2, pixel9To10 };
        pixelFloatRangeCount = 2;
        break;
    }
    case ProperShadersDirectProfile::LitPrelightNoShadows: {
        std::memcpy(pixel1To4, context.ps1To4, sizeof(pixel1To4));
        std::memcpy(pixel9To10, context.ps9To10, 4 * sizeof(float));
        pixel1To4[0] = oneMinusRoughness * context.specularPower * context.sunStrength;
        pixel1To4[4] = (std::clamp)(context.ambient[0], 0.0f, 1.0f);
        pixel1To4[5] = (std::clamp)(context.ambient[1], 0.0f, 1.0f);
        pixel1To4[6] = (std::clamp)(context.ambient[2], 0.0f, 1.0f);
        pixel1To4[8] = oneMinusRoughness;
        pixel1To4[12] = context.layerId;
        pixel9To10[0] = context.pixelColorScale;
        pixelFloatRanges[0] = { 1, 4, pixel1To4 };
        pixelFloatRanges[1] = { 9, 1, pixel9To10 };
        pixelFloatRangeCount = 2;
        break;
    }
    case ProperShadersDirectProfile::LitPrelightDeferred: {
        std::memcpy(pixel1To4, context.ps1To4, 12 * sizeof(float));
        pixel1To4[0] = context.layerId;
        pixel1To4[1] = 1.0f;
        pixel1To4[4] = context.metallicness;
        pixel1To4[5] = context.roughness;
        pixel1To4[6] = 1.0f;
        pixel1To4[7] = 1.0f;
        pixel1To4[8] = context.pixelColorScale;
        pixelFloatRanges[0] = { 1, 3, pixel1To4 };
        pixelFloatRangeCount = 1;
        break;
    }
    default:
        return S_FALSE;
    }

    const D3D9GtaSaBoolConstantRange pixelBoolRanges[] = {
        { 0, 1, &stochasticEnabled },
    };
    const D3D9GtaSaTextureBinding textureBindings[] = {
        { 0, context.baseTexture },
    };

    ProperShadersNativeCaptureScope captureScope(journal);
    if (FAILED(captureScope.BeginResult())) return captureScope.BeginResult();
    const HRESULT hr = captureScope.Finish(SubmitProperShadersDirectState(
        journal,
        vertexFloatRanges, static_cast<UINT>(std::size(vertexFloatRanges)),
        vertexBoolRanges, static_cast<UINT>(std::size(vertexBoolRanges)),
        pixelFloatRanges, pixelFloatRangeCount,
        pixelBoolRanges, static_cast<UINT>(std::size(pixelBoolRanges)),
        textureBindings, static_cast<UINT>(std::size(textureBindings))));
    if (FAILED(hr)) return hr;

    context.seen = 0;
    ++g_properShadersDirectCommits;
    return D3D_OK;
}

static HRESULT RestartProperShadersBaselinePass(
    void* effect,
    ProperShadersStateJournal* journal,
    HRESULT triggerHr,
    const char* phase)
{
    if (g_properShadersDirectContext.effect == effect) {
        SynchronizeProperShadersDirectParameters(effect);
        ResetProperShadersDirectContext();
    }
    const auto begin = GetProperShadersEffectMethod<ProperShadersBeginFn>(effect, 0xFC);
    const auto beginPass = GetProperShadersEffectMethod<ProperShadersBeginPassFn>(effect, 0x100);
    const auto endPass = GetProperShadersEffectMethod<ProperShadersEndPassFn>(effect, 0x108);
    const auto end = GetProperShadersEffectMethod<ProperShadersEndFn>(effect, 0x10C);
    if (!begin || !beginPass || !endPass || !end) {
        journal->Disable();
        ++g_properShadersOptimizationFailures;
        return D3DERR_INVALIDCALL;
    }

    const DWORD originalFlags = journal->OriginalFlags();
    const UINT pass = journal->HasCurrentPass() ? journal->CurrentPass() : 0;
    if (journal->IsPassActive()) endPass(effect);
    end(effect);
    const HRESULT restoreHr = journal->Restore();
    const HRESULT journalFailure = journal->HasFailed()
        ? journal->FailureCode()
        : triggerHr;
    journal->Disable();
    ResetProperShadersParameterCaches();
    ++g_properShadersOptimizationFailures;
    Log("effectopt: journal {} fallback effect={:08X} pass={} triggerHr=0x{:08X} restoreHr=0x{:08X} failure=0x{:08X}",
        phase, reinterpret_cast<std::uintptr_t>(effect),
        pass,
        static_cast<unsigned>(triggerHr),
        static_cast<unsigned>(restoreHr),
        static_cast<unsigned>(journalFailure));

    if (FAILED(restoreHr)) return restoreHr;

    UINT passes = 0;
    HRESULT hr = begin(effect, &passes, originalFlags);
    if (SUCCEEDED(hr)) hr = beginPass(effect, pass);
    return hr;
}

static HRESULT WINAPI OptimizedProperShadersBeginPass(void* effect, UINT pass)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersBeginPassFn>(effect, 0x100);
    if (!original) return D3DERR_INVALIDCALL;

    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    ProperShadersStateJournal* journal = binding ? binding->journal : nullptr;
    if (!journal || !journal->IsActive()) return original(effect, pass);

    journal->SetCurrentPass(pass);
    ProperShadersNativeCaptureScope captureScope(journal);
    if (FAILED(captureScope.BeginResult())) {
        return RestartProperShadersBaselinePass(
            effect, journal, captureScope.BeginResult(), "NativeCaptureBeginPass");
    }
    const HRESULT operationHr = original(effect, pass);
    const HRESULT hr = captureScope.Finish(operationHr);
    if (SUCCEEDED(operationHr)) journal->SetPassActive(true);
    if (SUCCEEDED(hr) && !journal->HasFailed()) {
        ActivateProperShadersDirectConstants(effect, journal);
        return hr;
    }
    return RestartProperShadersBaselinePass(effect, journal, hr, "BeginPass");
}

static HRESULT WINAPI OptimizedProperShadersCommitChanges(void* effect)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersCommitChangesFn>(
        effect, 0x104);
    if (!original) return D3DERR_INVALIDCALL;

    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    ProperShadersStateJournal* journal = binding ? binding->journal : nullptr;
    if (binding && binding->liteActive) {
        // Stage B: D3DX was never begun; plan-covered constants were written
        // directly at SetXXX time and everything else at ApplyPassLite.
        return D3D_OK;
    }
    if (!journal || !journal->IsActive()) return original(effect);

    bool directAttempted = false;
    const HRESULT directHr = CommitProperShadersDirectConstants(
        effect, journal, directAttempted);
    if (directAttempted && directHr == D3D_OK && !journal->HasFailed()) {
        return D3D_OK;
    }
    if (directAttempted) {
        ++g_properShadersDirectFallbacks;
        const HRESULT syncHr = SynchronizeProperShadersDirectParameters(effect);
        ResetProperShadersDirectContext();
        if (FAILED(directHr) || journal->HasFailed()) {
            const HRESULT failure = journal->HasFailed()
                ? journal->FailureCode()
                : directHr;
            return RestartProperShadersBaselinePass(
                effect, journal, failure, "DirectConstants");
        }
        if (FAILED(syncHr)) {
            return RestartProperShadersBaselinePass(
                effect, journal, syncHr, "DirectSync");
        }
    }

    if (g_properShadersGenericDirect) {
        // Stage ①-3b-i validation: confirm plan lookup works on live effects
        // (technique handles from GetTechnique(t) at build time must equal
        // GetCurrentTechnique() here). One log line per technique; the actual
        // direct submission lands in the next stage.
        ID3DXEffect* d3dxEffect = reinterpret_cast<ID3DXEffect*>(effect);
        D3DXHANDLE currentTechnique = d3dxEffect->GetCurrentTechnique();
        const GenericDirectTechniquePlan* plan =
            FindGenericDirectPlan(effect, currentTechnique);
        if (plan) {
            static thread_local const GenericDirectTechniquePlan* loggedPlans[40]{};
            for (size_t i = 0; i < 40; ++i) {
                if (loggedPlans[i] == plan) break;
                if (!loggedPlans[i]) {
                    loggedPlans[i] = plan;
                    Log("genericdirect: live-hit technique={} slots={} samplers={}",
                        plan->name ? plan->name : "?", plan->slotCount,
                        plan->samplerCount);
                    break;
                }
            }
        } else {
            // Diagnose misses: is it "no plans for this effect" or a technique
            // handle mismatch? Log the first few with raw pointers.
            static thread_local int missLogs = 0;
            if (missLogs < 6) {
                ++missLogs;
                Log("genericdirect: live-miss effect={:08X} technique={:08X}", reinterpret_cast<std::uintptr_t>(effect), reinterpret_cast<std::uintptr_t>(currentTechnique));
            }
        }
    }

    ProperShadersNativeCaptureScope captureScope(journal);
    if (FAILED(captureScope.BeginResult())) {
        return RestartProperShadersBaselinePass(
            effect, journal, captureScope.BeginResult(), "NativeCaptureCommitChanges");
    }
    const HRESULT hr = captureScope.Finish(original(effect));
    if (SUCCEEDED(hr) && !journal->HasFailed()) return hr;
    return RestartProperShadersBaselinePass(effect, journal, hr, "CommitChanges");
}

static HRESULT WINAPI OptimizedProperShadersEndPass(void* effect)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersEndPassFn>(effect, 0x108);
    if (!original) return D3DERR_INVALIDCALL;
    const HRESULT hr = original(effect);

    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    if (SUCCEEDED(hr) && binding && binding->journal && binding->journal->IsActive()) {
        binding->journal->SetPassActive(false);
    }
    return hr;
}

static HRESULT WINAPI OptimizedProperShadersEnd(void* effect)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersEndFn>(effect, 0x10C);
    if (!original) return D3DERR_INVALIDCALL;

    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    ProperShadersStateJournal* journal = binding ? binding->journal : nullptr;
    const HRESULT directSyncHr = SynchronizeProperShadersDirectParameters(effect);
    ResetProperShadersDirectContext();
    HRESULT hr = D3D_OK;
    if (journal && journal->IsActive()) {
        ProperShadersNativeCaptureScope captureScope(journal);
        if (FAILED(captureScope.BeginResult())) {
            hr = captureScope.BeginResult();
        } else {
            hr = captureScope.Finish(original(effect));
        }
    } else {
        hr = original(effect);
    }
    ResetProperShadersParameterCaches();
    if (!journal || !journal->IsActive()) {
        return FAILED(hr) ? hr : directSyncHr;
    }

    // Journal probe: this is the End matching the optimization Begin path, so the
    // hot per-object techniques (LitPrelight, DepthPass) flush here. Must run
    // before Restore advances the generation. The dump throttle is shared with
    // the general path via its own static, so both paths dump independently.
    if (journal->ProbeCaptureEnabled()) {
        journal->FlushProbeRecords();
        if (g_properShadersJournalProbe) {
            static ULONGLONG lastOptDump = 0;
            const ULONGLONG nowTick = GetTickCount64();
            if (nowTick - lastOptDump >= 10000ull) {
                lastOptDump = nowTick;
                JournalProbeDump();
            }
        }
    }

    const bool journalFailed = journal->HasFailed();
    const HRESULT journalFailure = journal->FailureCode();
    const HRESULT restoreHr = journal->Restore();
    if (journalFailed || FAILED(restoreHr)) {
        journal->Disable();
        ++g_properShadersOptimizationFailures;
        Log("effectopt: journal End disabled effect={:08X} endHr=0x{:08X} restoreHr=0x{:08X} failure=0x{:08X}", reinterpret_cast<std::uintptr_t>(effect),
            static_cast<unsigned>(hr),
            static_cast<unsigned>(restoreHr),
            static_cast<unsigned>(journalFailure));
    }
    if (FAILED(hr)) return hr;
    if (FAILED(directSyncHr)) return directSyncHr;
    return restoreHr;
}

static void ResetProperShadersBatchState()
{
    g_properShadersBatchState = ProperShadersBatchState{};
}

static void RecordProperShadersBatchFallback(
    const char* reason, void* effect, HRESULT primaryHr, HRESULT secondaryHr)
{
    const uint64_t count = ++g_properShadersBatchFallbacks;
    if (count <= 8) {
        Log("effectbatch: fallback={} reason={} effect={:08X} primaryHr=0x{:08X} secondaryHr=0x{:08X}",
            static_cast<long long>(count),
            reason, reinterpret_cast<std::uintptr_t>(effect),
            static_cast<unsigned>(primaryHr),
            static_cast<unsigned>(secondaryHr));
    }
}

static HRESULT FlushProperShadersBatch(const char* reason, bool fallback)
{
    // The unwrap build removed the wrapped Present that used to drive the
    // periodic effectopt/effectbatch stats. Drive them from here instead:
    // this runs once per completed batch, and the counter+tick gate keeps the
    // cost to one GetTickCount64 per 256 batches.
    if (g_properShadersEffectBatching) {
        static uint32_t flushCounter = 0;
        if ((++flushCounter & 0xFFu) == 0) {
            static ULONGLONG lastStatsCheck = 0;
            const ULONGLONG nowTick = GetTickCount64();
            if (nowTick - lastStatsCheck >= 5000ull) {
                lastStatsCheck = nowTick;
                OnProperShadersEffectOptimizationPresent();
            }
        }
    }

    if (!g_properShadersBatchState.active || !g_properShadersBatchState.effect) {
        ResetProperShadersBatchState();
        return D3D_OK;
    }

    void* effect = g_properShadersBatchState.effect;
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    ProperShadersStateJournal* journal = binding ? binding->journal : nullptr;
    const bool passActive = !journal || journal->IsPassActive();
    ResetProperShadersBatchState();

    HRESULT passHr = D3D_OK;
    if (passActive) passHr = OptimizedProperShadersEndPass(effect);
    const HRESULT endHr = OptimizedProperShadersEnd(effect);
    if (fallback) {
        RecordProperShadersBatchFallback(reason, effect, passHr, endHr);
    }
    return FAILED(passHr) ? passHr : endHr;
}

static HRESULT OptimizedProperShadersSetTechniqueForBatch(
    ProperShadersBatchPath path, void* effect, const char* technique)
{
    ++g_properShadersBatchTechniqueCalls;
    const auto original = GetProperShadersEffectMethod<ProperShadersSetTechniqueFn>(effect, 0xE8);
    if (!original) return D3DERR_INVALIDCALL;
    if (!g_properShadersEffectBatching ||
        !g_properShadersNoSaveStateActive.load(std::memory_order_relaxed)) {
        // Record the technique even when batching is off: lastTechnique is plain
        // bookkeeping, and the journal probe labels its transactions with it.
        // Batching has been disabled since 2026-07-26, so this early return is
        // the only path that runs — leaving the assignment below the branch is
        // why every probe transaction reported technique=<unknown>.
        const HRESULT plainHr = original(effect, technique);
        if (SUCCEEDED(plainHr)) {
            if (ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect)) {
                binding->lastTechnique = technique;
            }
        }
        return plainHr;
    }

    if (g_properShadersBatchState.active) {
        if (g_properShadersBatchState.path == path &&
            g_properShadersBatchState.effect == effect &&
            g_properShadersBatchState.technique == technique) {
            return D3D_OK;
        }
        FlushProperShadersBatch("technique-change", true);
    }

    const HRESULT hr = original(effect, technique);
    if (SUCCEEDED(hr)) {
        g_properShadersBatchState.path = path;
        g_properShadersBatchState.effect = effect;
        g_properShadersBatchState.technique = technique;
        // Persist per effect so a later flush cannot erase it.
        if (ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect)) {
            binding->lastTechnique = technique;
        }
    }
    return hr;
}

static HRESULT WINAPI OptimizedProperShadersSetTechniqueB(
    void* effect, const char* technique)
{
    return OptimizedProperShadersSetTechniqueForBatch(
        ProperShadersBatchPath::Mode2, effect, technique);
}

static HRESULT WINAPI OptimizedProperShadersSetTechniqueStandalone(
    void* effect, const char* technique)
{
    return OptimizedProperShadersSetTechniqueForBatch(
        ProperShadersBatchPath::Standalone, effect, technique);
}

static HRESULT OptimizedProperShadersBeginForBatch(
    ProperShadersBatchPath path, void* effect, UINT* passes, DWORD flags)
{
    ++g_properShadersBatchBeginAttempts;
    if (path == ProperShadersBatchPath::Mode2) {
        ++g_properShadersBatchMode2Attempts;
    } else if (path == ProperShadersBatchPath::Standalone) {
        ++g_properShadersBatchStandaloneAttempts;
    }

    const bool batchingEnabled = g_properShadersEffectBatching;
    const bool noSaveStateActive =
        g_properShadersNoSaveStateActive.load(std::memory_order_relaxed);
    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    ProperShadersStateJournal* journal = binding ? binding->journal : nullptr;
    if (g_properShadersBatchState.active) {
        if (batchingEnabled && noSaveStateActive &&
            g_properShadersBatchState.path == path &&
            g_properShadersBatchState.effect == effect &&
            g_properShadersBatchState.flags == flags &&
            journal && journal->IsActive() && !journal->IsDisabled()) {
            if (passes) *passes = g_properShadersBatchState.passes;
            g_properShadersBatchState.reusePending = true;
            ++g_properShadersBatchReuses;
            return D3D_OK;
        }
        FlushProperShadersBatch("begin-mismatch", true);
    }

    const char* technique = g_properShadersBatchState.path == path &&
        g_properShadersBatchState.effect == effect
        ? g_properShadersBatchState.technique
        : nullptr;
    const HRESULT hr = OptimizedProperShadersBegin(effect, passes, flags);
    binding = FindProperShadersEffectBinding(effect);
    journal = binding ? binding->journal : nullptr;
    // The global batch slot is cleared by the flush above, and it only ever
    // holds ONE effect. Fall back to this effect's own recorded technique so a
    // flush, or a Begin interleaved from another effect, cannot force a
    // permanent "technique" reject and keep batching from ever starting.
    if (!technique && binding) technique = binding->lastTechnique;
    ProperShadersBatching::BatchStartFacts facts{};
    facts.batchingEnabled = batchingEnabled;
    facts.noSaveStateActive = noSaveStateActive;
    facts.techniqueKnown = technique != nullptr;
    facts.beginSucceeded = SUCCEEDED(hr);
    facts.passCountAvailable = passes != nullptr;
    facts.passCount = passes ? *passes : 0;
    facts.bindingAvailable = binding != nullptr && journal != nullptr;
    facts.journalActive = journal && journal->IsActive();
    facts.journalDisabled = journal && journal->IsDisabled();
    const ProperShadersBatching::BatchRejectReason rejectReason =
        ProperShadersBatching::EvaluateBatchStart(facts);
    if (rejectReason == ProperShadersBatching::BatchRejectReason::None) {
        g_properShadersBatchState.path = path;
        g_properShadersBatchState.effect = effect;
        g_properShadersBatchState.technique = technique;
        g_properShadersBatchState.flags = flags;
        g_properShadersBatchState.passes = *passes;
        g_properShadersBatchState.active = true;
        g_properShadersBatchState.reusePending = false;
        ++g_properShadersBatchStarts;
    } else {
        RecordProperShadersBatchReject(rejectReason);
        ResetProperShadersBatchState();
    }
    return hr;
}

static HRESULT WINAPI OptimizedProperShadersBeginB(
    void* effect, UINT* passes, DWORD flags)
{
    return OptimizedProperShadersBeginForBatch(
        ProperShadersBatchPath::Mode2, effect, passes, flags);
}

static HRESULT WINAPI OptimizedProperShadersBeginStandalone(
    void* effect, UINT* passes, DWORD flags)
{
    return OptimizedProperShadersBeginForBatch(
        ProperShadersBatchPath::Standalone, effect, passes, flags);
}

static HRESULT WINAPI OptimizedProperShadersBeginPassB(void* effect, UINT pass)
{
    if (!g_properShadersBatchState.active ||
        g_properShadersBatchState.effect != effect) {
        return OptimizedProperShadersBeginPass(effect, pass);
    }

    ProperShadersEffectBinding* binding = FindProperShadersEffectBinding(effect);
    ProperShadersStateJournal* journal = binding ? binding->journal : nullptr;
    if (!journal || !journal->IsActive() || journal->IsDisabled()) {
        ResetProperShadersBatchState();
        return OptimizedProperShadersBeginPass(effect, pass);
    }

    if (!g_properShadersBatchState.reusePending) {
        const HRESULT hr = OptimizedProperShadersBeginPass(effect, pass);
        if (!journal->IsActive() || journal->IsDisabled()) {
            ResetProperShadersBatchState();
            RecordProperShadersBatchFallback(
                "first-pass-baseline", effect, hr, journal->FailureCode());
        } else if (FAILED(hr)) {
            return FlushProperShadersBatch("first-pass-failed", true);
        }
        return hr;
    }

    g_properShadersBatchState.reusePending = false;
    const HRESULT hr = OptimizedProperShadersCommitChanges(effect);
    if (!journal->IsActive() || journal->IsDisabled()) {
        ResetProperShadersBatchState();
        RecordProperShadersBatchFallback(
            "commit-baseline", effect, hr, journal->FailureCode());
    } else if (SUCCEEDED(hr)) {
        ++g_properShadersBatchCommits;
    } else {
        return FlushProperShadersBatch("commit-failed", true);
    }
    return hr;
}

static HRESULT WINAPI OptimizedProperShadersEndPassB(void* effect)
{
    if (g_properShadersBatchState.active &&
        g_properShadersBatchState.effect == effect) {
        return D3D_OK;
    }
    return OptimizedProperShadersEndPass(effect);
}

static bool ReadProperShadersLoopRemaining(
    uintptr_t callerFrame, unsigned parentFrames, int& remaining)
{
    remaining = 0;
    if (!callerFrame) return false;
    __try {
        const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const uintptr_t stackLow = reinterpret_cast<uintptr_t>(tib->StackLimit);
        const uintptr_t stackHigh = reinterpret_cast<uintptr_t>(tib->StackBase);
        auto isStackRange = [stackLow, stackHigh](uintptr_t address, size_t size) {
            return address >= stackLow && address <= stackHigh &&
                size <= stackHigh - address;
        };

        uintptr_t loopFrame = callerFrame;
        if (!isStackRange(loopFrame, sizeof(uintptr_t) * 2)) return false;
        for (unsigned i = 0; i < parentFrames; ++i) {
            const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(loopFrame);
            if (parent <= loopFrame || parent - loopFrame > 0x100000 ||
                !isStackRange(parent, sizeof(uintptr_t) * 2)) {
                return false;
            }
            loopFrame = parent;
        }
        if (loopFrame < 0x2B8 ||
            !isStackRange(loopFrame - 0x2B8, sizeof(remaining))) {
            return false;
        }

        remaining = *reinterpret_cast<const int*>(loopFrame - 0x2B8);
        return remaining >= 0 && remaining < 0x100000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        remaining = 0;
        return false;
    }
}

static HRESULT FinishProperShadersLoopBatch(
    ProperShadersBatchPath path,
    void* effect,
    uintptr_t callerFrame,
    unsigned parentFrames,
    const char* unavailableReason)
{
    if (!g_properShadersBatchState.active ||
        g_properShadersBatchState.path != path ||
        g_properShadersBatchState.effect != effect) {
        return OptimizedProperShadersEnd(effect);
    }

    int remaining = 0;
    if (!ReadProperShadersLoopRemaining(callerFrame, parentFrames, remaining)) {
        return FlushProperShadersBatch(unavailableReason, true);
    }
    if (remaining > 0) return D3D_OK;
    return FlushProperShadersBatch("batch-complete", false);
}

#if defined(_M_IX86)
#pragma optimize("y", off)
#endif
static __declspec(noinline) HRESULT WINAPI OptimizedProperShadersEndB(void* effect)
{
    uintptr_t callerFrame = 0;
#if defined(_M_IX86)
    __asm {
        mov eax, dword ptr [ebp]
        mov callerFrame, eax
    }
#endif
    return FinishProperShadersLoopBatch(
        ProperShadersBatchPath::Mode2,
        effect,
        callerFrame,
        0,
        "mode2-loop-frame-unavailable");
}

static __declspec(noinline) HRESULT WINAPI OptimizedProperShadersEndStandalone(void* effect)
{
    uintptr_t callerFrame = 0;
#if defined(_M_IX86)
    __asm {
        mov eax, dword ptr [ebp]
        mov callerFrame, eax
    }
#endif
    return FinishProperShadersLoopBatch(
        ProperShadersBatchPath::Standalone,
        effect,
        callerFrame,
        1,
        "standalone-loop-frame-unavailable");
}
#if defined(_M_IX86)
#pragma optimize("y", on)
#endif

static HRESULT WINAPI ProfileProperShadersBeginPass(void* effect, UINT pass)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersBeginPassFn>(effect, 0x100);
    if (!original) return D3DERR_INVALIDCALL;
    if (!ShouldRecordProperShadersEffectCall()) return original(effect, pass);

    const uint64_t begin = ReadPerformanceCounter();
    const HRESULT hr = original(effect, pass);
    const uint64_t end = ReadPerformanceCounter();
    RecordProperShadersEffectTiming(g_properShadersEffectProfile.beginPass, begin, end);
    return hr;
}

static HRESULT WINAPI ProfileProperShadersEndPass(void* effect)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersEndPassFn>(effect, 0x108);
    if (!original) return D3DERR_INVALIDCALL;
    if (!ShouldRecordProperShadersEffectCall()) return original(effect);

    const uint64_t begin = ReadPerformanceCounter();
    const HRESULT hr = original(effect);
    const uint64_t end = ReadPerformanceCounter();
    RecordProperShadersEffectTiming(g_properShadersEffectProfile.endPass, begin, end);
    return hr;
}

static HRESULT WINAPI ProfileProperShadersEnd(void* effect)
{
    const auto original = GetProperShadersEffectMethod<ProperShadersEndFn>(effect, 0x10C);
    if (!original) return D3DERR_INVALIDCALL;
    if (!ShouldRecordProperShadersEffectCall()) return original(effect);

    const uint64_t begin = ReadPerformanceCounter();
    const HRESULT hr = original(effect);
    const uint64_t end = ReadPerformanceCounter();
    RecordProperShadersEffectTiming(g_properShadersEffectProfile.end, begin, end);
    return hr;
}

static ProperShadersCallPatch g_properShadersCallPatches[] = {
    { "TextureTransform-special", 0x00012E2F, { 0xFF, 0x92, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersTextureTransform) },
    { "TextureTransform",         0x00012E44, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersTextureTransform) },
    { "WorldViewProjection",      0x000132EE, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersWorldViewProjection) },
    { "WorldInverse",             0x00013300, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersWorldInverse) },
    { "World",                    0x00013312, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersWorld) },
    { "CommitChanges",            0x000134F1, { 0xFF, 0x90, 0x04, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersCommitChanges) },
    { "Begin-A",                  0x00013784, { 0xFF, 0x91, 0xFC, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersBegin) },
    { "BeginPass-A",              0x00013794, { 0xFF, 0x91, 0x00, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersBeginPass) },
    { "Begin-B",                  0x000138A4, { 0xFF, 0x90, 0xFC, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersBegin) },
    { "BeginPass-B",              0x000138AF, { 0xFF, 0x90, 0x00, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersBeginPass) },
    { "EndPass-B",                0x000138C9, { 0xFF, 0x90, 0x08, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersEndPass) },
    { "End-B",                    0x000138D2, { 0xFF, 0x90, 0x0C, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersEnd) },
    { "EndPass-A",                0x000138EE, { 0xFF, 0x91, 0x08, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersEndPass) },
    { "End-A",                    0x000138FC, { 0xFF, 0x91, 0x0C, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersEnd) },
    { "EndPass-C",                0x00013DA9, { 0xFF, 0x91, 0x08, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersEndPass) },
    { "End-C",                    0x00013DB7, { 0xFF, 0x91, 0x0C, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&ProfileProperShadersEnd) },
};

static ProperShadersCallPatch g_properShadersOptimizationPatches[] = {
    { "DayVector", 0x00012DC9, { 0xFF, 0x90, 0x88, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersDayVector) },
    { "NightVector", 0x00012DD9, { 0xFF, 0x90, 0x88, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersNightVector) },
    { "TextureTransform-special", 0x00012E2F, { 0xFF, 0x92, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersTextureTransform) },
    { "TextureTransform-helper", 0x00012E44, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersTextureTransform) },
    { "WorldViewProjection-Standalone", 0x00012F0F, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersWorldViewProjection) },
    { "TextureTransform-Standalone-special", 0x00012FC4, { 0xFF, 0x92, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersTextureTransform) },
    { "TextureTransform-Standalone", 0x00012FD5, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersTextureTransform) },
    { "MaterialVector-Standalone", 0x00013014, { 0xFF, 0x90, 0x88, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersMaterialVector) },
    { "AmbientFloatArray-Standalone", 0x00013029, { 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersAmbientFloatArray) },
    { "SurfacePropsFloatArray-Standalone", 0x0001303D, { 0xFF, 0x91, 0x80, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersSurfacePropsFloatArray) },
    { "SetTechnique-Standalone", 0x00013078, { 0xFF, 0x90, 0xE8, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersSetTechniqueStandalone) },
    { "Begin-Standalone", 0x00013087, { 0xFF, 0x90, 0xFC, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersBeginStandalone) },
    { "BeginPass-Standalone", 0x00013092, { 0xFF, 0x90, 0x00, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersBeginPassB) },
    { "EndPass-Standalone", 0x000130AF, { 0xFF, 0x90, 0x08, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersEndPassB) },
    { "End-Standalone", 0x000130B8, { 0xFF, 0x90, 0x0C, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersEndStandalone) },
    { "WorldViewProjection", 0x000132EE, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersWorldViewProjection) },
    { "WorldInverse", 0x00013300, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersWorldInverse) },
    { "World", 0x00013312, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersWorld) },
    { "MaterialVector", 0x000133F1, { 0xFF, 0x90, 0x88, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersMaterialVector) },
    { "SurfacePropsFloatArray", 0x00013409, { 0xFF, 0x91, 0x80, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersSurfacePropsFloatArray) },
    { "CommitChanges", 0x000134F1, { 0xFF, 0x90, 0x04, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersCommitChanges) },
    { "Begin-A", 0x00013784, { 0xFF, 0x91, 0xFC, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersBegin) },
    { "BeginPass-A", 0x00013794, { 0xFF, 0x91, 0x00, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersBeginPass) },
    { "WorldViewProjection-B", 0x0001386F, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersWorldViewProjection) },
    { "WorldInverse-B", 0x00013884, { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersWorldInverse) },
    { "SetTechnique-B", 0x00013892, { 0xFF, 0x90, 0xE8, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersSetTechniqueB) },
    { "Begin-B", 0x000138A4, { 0xFF, 0x90, 0xFC, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersBeginB) },
    { "BeginPass-B", 0x000138AF, { 0xFF, 0x90, 0x00, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersBeginPassB) },
    { "EndPass-B", 0x000138C9, { 0xFF, 0x90, 0x08, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersEndPassB) },
    { "End-B", 0x000138D2, { 0xFF, 0x90, 0x0C, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersEndB) },
    { "EndPass-A", 0x000138EE, { 0xFF, 0x91, 0x08, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersEndPass) },
    { "End-A", 0x000138FC, { 0xFF, 0x91, 0x0C, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersEnd) },
    { "EndPass-C", 0x00013DA9, { 0xFF, 0x91, 0x08, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersEndPass) },
    { "End-C", 0x00013DB7, { 0xFF, 0x91, 0x0C, 0x01, 0x00, 0x00 }, reinterpret_cast<void*>(&OptimizedProperShadersEnd) },
};

static bool GetModuleImageSize(HMODULE module, size_t& imageSize)
{
    imageSize = 0;
    if (!module) return false;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return false;
        }
        imageSize = nt->OptionalHeader.SizeOfImage;
        return imageSize != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static PVOID volatile* FindModuleImportSlot(
    HMODULE module,
    const char* importedModule,
    const char* importedFunction)
{
    size_t imageSize = 0;
    if (!module || !importedModule || !importedFunction ||
        !GetModuleImageSize(module, imageSize)) {
        return nullptr;
    }

    __try {
        auto* base = reinterpret_cast<uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& imports =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!imports.VirtualAddress || imports.VirtualAddress >= imageSize) return nullptr;

        auto rangeValid = [imageSize](size_t rva, size_t size) {
            return rva < imageSize && size <= imageSize - rva;
        };
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + imports.VirtualAddress);
        while (descriptor->Name) {
            if (!rangeValid(descriptor->Name, 1)) return nullptr;
            const char* moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
            if (_stricmp(moduleName, importedModule) == 0) {
                const DWORD namesRva = descriptor->OriginalFirstThunk
                    ? descriptor->OriginalFirstThunk
                    : descriptor->FirstThunk;
                if (!rangeValid(namesRva, sizeof(IMAGE_THUNK_DATA32)) ||
                    !rangeValid(descriptor->FirstThunk, sizeof(IMAGE_THUNK_DATA32))) {
                    return nullptr;
                }
                auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + namesRva);
                auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(
                    base + descriptor->FirstThunk);
                for (size_t index = 0; names[index].u1.AddressOfData; ++index) {
                    const size_t nameThunkRva = namesRva + index * sizeof(IMAGE_THUNK_DATA32);
                    const size_t addressThunkRva = descriptor->FirstThunk +
                        index * sizeof(IMAGE_THUNK_DATA32);
                    if (!rangeValid(nameThunkRva, sizeof(IMAGE_THUNK_DATA32)) ||
                        !rangeValid(addressThunkRva, sizeof(IMAGE_THUNK_DATA32))) {
                        return nullptr;
                    }
                    if (IMAGE_SNAP_BY_ORDINAL32(names[index].u1.Ordinal)) continue;
                    const DWORD importByNameRva = names[index].u1.AddressOfData;
                    if (!rangeValid(importByNameRva, sizeof(IMAGE_IMPORT_BY_NAME))) {
                        return nullptr;
                    }
                    const auto* importByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                        base + importByNameRva);
                    if (std::strcmp(
                            reinterpret_cast<const char*>(importByName->Name),
                            importedFunction) == 0) {
                        return reinterpret_cast<PVOID volatile*>(
                            &addresses[index].u1.Function);
                    }
                }
                return nullptr;
            }
            ++descriptor;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

static bool InstallProperShadersCreateEffectHook(HMODULE module)
{
    if (!g_properShadersGeneralStateJournal) return true;
    if (g_properShadersCreateEffectIatSlot &&
        g_originalProperShadersD3DXCreateEffect) {
        return true;
    }

    PVOID volatile* slot = FindModuleImportSlot(
        module, "d3dx9_43.dll", "D3DXCreateEffect");
    if (!slot) {
        Log("effectgeneral: D3DXCreateEffect import not found");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            const_cast<PVOID*>(slot), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log("effectgeneral: IAT VirtualProtect failed err={}", GetLastError());
        return false;
    }

    PVOID current = *slot;
    const PVOID hook = reinterpret_cast<PVOID>(&HookedProperShadersD3DXCreateEffect);
    bool installed = false;
    if (current == hook && g_originalProperShadersD3DXCreateEffect) {
        installed = true;
    } else if (current && current != hook) {
        PVOID previous = InterlockedCompareExchangePointer(slot, hook, current);
        if (previous == current) {
            g_originalProperShadersD3DXCreateEffect =
                reinterpret_cast<ProperShadersD3DXCreateEffectFn>(current);
            g_properShadersCreateEffectIatSlot = slot;
            installed = true;
        }
    }

    DWORD ignored = 0;
    VirtualProtect(const_cast<PVOID*>(slot), sizeof(void*), oldProtect, &ignored);
    if (installed) {
        Log("effectgeneral: D3DXCreateEffect IAT hook installed slot={:08X} original={:08X}", reinterpret_cast<std::uintptr_t>(slot), reinterpret_cast<std::uintptr_t>(g_originalProperShadersD3DXCreateEffect));
    } else {
        Log("effectgeneral: D3DXCreateEffect IAT hook install race current={:08X}", reinterpret_cast<std::uintptr_t>(current));
    }
    return installed;
}

static void RestoreProperShadersCreateEffectHook()
{
    PVOID volatile* slot = g_properShadersCreateEffectIatSlot;
    const auto original = g_originalProperShadersD3DXCreateEffect;
    if (!slot || !original) return;

    DWORD oldProtect = 0;
    if (VirtualProtect(
            const_cast<PVOID*>(slot), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        InterlockedCompareExchangePointer(
            slot,
            reinterpret_cast<PVOID>(original),
            reinterpret_cast<PVOID>(&HookedProperShadersD3DXCreateEffect));
        DWORD ignored = 0;
        VirtualProtect(const_cast<PVOID*>(slot), sizeof(void*), oldProtect, &ignored);
    }
    g_properShadersCreateEffectIatSlot = nullptr;
    g_originalProperShadersD3DXCreateEffect = nullptr;
}

static bool WriteProperShadersCallPatch(ProperShadersCallPatch& patch, HMODULE module)
{
    uint8_t* site = reinterpret_cast<uint8_t*>(module) + patch.rva;
    if (!ProperShadersPatching::BytesMatch(
            site, patch.expected, sizeof(patch.expected))) {
        SetLastError(ERROR_REVISION_MISMATCH);
        return false;
    }
    const intptr_t displacement = reinterpret_cast<uint8_t*>(patch.replacement) - (site + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }

    uint8_t bytes[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    const int32_t relative = static_cast<int32_t>(displacement);
    std::memcpy(bytes + 1, &relative, sizeof(relative));

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, sizeof(bytes), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(patch.original, site, sizeof(patch.original));
    std::memcpy(site, bytes, sizeof(bytes));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(bytes));
    DWORD ignored = 0;
    VirtualProtect(site, sizeof(bytes), oldProtect, &ignored);
    patch.installed = true;
    return true;
}

static void RestoreProperShadersOptimizationPatches()
{
    HMODULE module = g_properShadersOptimizationModule;
    if (!module) return;
    for (size_t i = sizeof(g_properShadersOptimizationPatches) /
             sizeof(g_properShadersOptimizationPatches[0]); i > 0; --i) {
        ProperShadersCallPatch& patch = g_properShadersOptimizationPatches[i - 1];
        if (!patch.installed) continue;
        uint8_t* site = reinterpret_cast<uint8_t*>(module) + patch.rva;
        DWORD oldProtect = 0;
        if (VirtualProtect(site, sizeof(patch.original), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(site, patch.original, sizeof(patch.original));
            FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch.original));
            DWORD ignored = 0;
            VirtualProtect(site, sizeof(patch.original), oldProtect, &ignored);
        }
        patch.installed = false;
    }
    g_properShadersOptimizationHooksInstalled = false;
    g_properShadersOptimizationModule = nullptr;
    g_properShadersNoSaveStateActive.store(false, std::memory_order_relaxed);
}

static bool InstallProperShadersOptimizationPatches(HMODULE module)
{
    if (g_properShadersOptimizationHooksInstalled) return true;

    size_t imageSize = 0;
    if (!GetModuleImageSize(module, imageSize)) {
        Log("effectopt: failed to read ProperShaders PE image");
        return false;
    }

    constexpr size_t patchCount = std::size(g_properShadersOptimizationPatches);
    ProperShadersPatching::PatchBytes patchBytes[patchCount]{};
    for (size_t i = 0; i < patchCount; ++i) {
        const ProperShadersCallPatch& patch = g_properShadersOptimizationPatches[i];
        if (patch.rva > imageSize ||
            sizeof(patch.expected) > imageSize - patch.rva) {
            Log("effectopt: signature out of image name={} rva=0x{:08X} imageSize=0x{:08X}",
                patch.name, static_cast<unsigned>(patch.rva), static_cast<unsigned>(imageSize));
            return false;
        }
        const uint8_t* site = reinterpret_cast<const uint8_t*>(module) + patch.rva;
        patchBytes[i] = { site, patch.expected, sizeof(patch.expected) };
    }

    const size_t mismatch = ProperShadersPatching::FindFirstMismatch(
        patchBytes);
    if (mismatch != patchCount) {
        const ProperShadersCallPatch& patch =
            g_properShadersOptimizationPatches[mismatch];
        const uint8_t* actual = patchBytes[mismatch].actual;
        Log("effectopt: patch preflight mismatch name={} rva=0x{:08X} expected={:02X}{:02X}{:02X}{:02X}{:02X}{:02X} actual={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
            patch.name,
            static_cast<unsigned>(patch.rva),
            static_cast<unsigned>(patch.expected[0]),
            static_cast<unsigned>(patch.expected[1]),
            static_cast<unsigned>(patch.expected[2]),
            static_cast<unsigned>(patch.expected[3]),
            static_cast<unsigned>(patch.expected[4]),
            static_cast<unsigned>(patch.expected[5]),
            static_cast<unsigned>(actual[0]),
            static_cast<unsigned>(actual[1]),
            static_cast<unsigned>(actual[2]),
            static_cast<unsigned>(actual[3]),
            static_cast<unsigned>(actual[4]),
            static_cast<unsigned>(actual[5]));
        return false;
    }

    g_properShadersOptimizationModule = module;
    for (auto& patch : g_properShadersOptimizationPatches) {
        if (!WriteProperShadersCallPatch(patch, module)) {
            Log("effectopt: failed to install name={} rva=0x{:08X} err={}",
                patch.name, static_cast<unsigned>(patch.rva), GetLastError());
            RestoreProperShadersOptimizationPatches();
            return false;
        }
    }

    g_properShadersOptimizationHooksInstalled = true;
    Log("effectopt: installed module={:08X} patches={} noSaveState={} autoBenchmark={} skipDuplicateMatrices={} skipDuplicateParameters={} directConstants={}", reinterpret_cast<std::uintptr_t>(module),
        static_cast<unsigned>(sizeof(g_properShadersOptimizationPatches) /
            sizeof(g_properShadersOptimizationPatches[0])),
        g_properShadersEffectOptimizationNoSaveState ? 1 : 0,
        g_properShadersEffectOptimizationAutoBenchmark ? 1 : 0,
        g_properShadersSkipDuplicateMatrices ? 1 : 0,
        g_properShadersSkipDuplicateParameters ? 1 : 0,
        g_properShadersDirectConstants ? 1 : 0);
    return true;
}

static void RestoreProperShadersCallPatches()
{
    HMODULE module = g_properShadersEffectProfile.module;
    if (!module) return;
    for (size_t i = sizeof(g_properShadersCallPatches) / sizeof(g_properShadersCallPatches[0]);
         i > 0; --i) {
        ProperShadersCallPatch& patch = g_properShadersCallPatches[i - 1];
        if (!patch.installed) continue;
        uint8_t* site = reinterpret_cast<uint8_t*>(module) + patch.rva;
        DWORD oldProtect = 0;
        if (VirtualProtect(site, sizeof(patch.original), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(site, patch.original, sizeof(patch.original));
            FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch.original));
            DWORD ignored = 0;
            VirtualProtect(site, sizeof(patch.original), oldProtect, &ignored);
        }
        patch.installed = false;
    }
}

static bool InstallProperShadersCallPatches(HMODULE module)
{
    size_t imageSize = 0;
    if (!GetModuleImageSize(module, imageSize)) {
        Log("effectprofile: failed to read ProperShaders PE image");
        return false;
    }

    for (const auto& patch : g_properShadersCallPatches) {
        if (patch.rva + sizeof(patch.expected) > imageSize) {
            Log("effectprofile: signature out of image name={} rva=0x{:08X} imageSize=0x{:08X}",
                patch.name, static_cast<unsigned>(patch.rva), static_cast<unsigned>(imageSize));
            return false;
        }
        const uint8_t* site = reinterpret_cast<const uint8_t*>(module) + patch.rva;
        if (std::memcmp(site, patch.expected, sizeof(patch.expected)) != 0) {
            Log("effectprofile: signature mismatch name={} rva=0x{:08X}",
                patch.name, static_cast<unsigned>(patch.rva));
            return false;
        }
    }

    for (auto& patch : g_properShadersCallPatches) {
        if (!WriteProperShadersCallPatch(patch, module)) {
            Log("effectprofile: failed to install name={} rva=0x{:08X} err={}",
                patch.name, static_cast<unsigned>(patch.rva), GetLastError());
            RestoreProperShadersCallPatches();
            return false;
        }
    }
    return true;
}

static double ProperShadersTicksToMilliseconds(uint64_t ticks)
{
    const LONGLONG frequency = g_properShadersEffectProfile.qpcFrequency.QuadPart;
    return frequency > 0
        ? 1000.0 * static_cast<double>(ticks) / static_cast<double>(frequency)
        : 0.0;
}

static void WriteProperShadersMethodStats(
    FILE* file, const char* name, const ProperShadersEffectMethodStats& stats, uint64_t frames)
{
    const double totalMs = ProperShadersTicksToMilliseconds(stats.qpcTicks);
    const double maxUs = ProperShadersTicksToMilliseconds(stats.maxQpcTicks) * 1000.0;
    const double averageNs = stats.calls
        ? totalMs * 1000000.0 / static_cast<double>(stats.calls)
        : 0.0;
    const double callsPerFrame = frames
        ? static_cast<double>(stats.calls) / static_cast<double>(frames)
        : 0.0;
    std::print(file,
        "method={} calls={} callsPerFrame={:.2f} totalMs={:.3f} avgNs={:.1f} maxUs={:.3f}\n",
        name,
        static_cast<unsigned long long>(stats.calls),
        callsPerFrame,
        totalMs,
        averageNs,
        maxUs);
}

static bool StartProperShadersEffectProfile(IDirect3DDevice9* device)
{
    if (g_properShadersEffectProfile.active) return false;
    if (g_properShadersOptimizationHooksInstalled) {
        Log("effectprofile: unavailable while permanent effect optimization hooks are installed");
        return false;
    }
    HMODULE module = GetModuleHandleA("ProperShaders.asi");
    if (!module) module = GetModuleHandleA("propershaders.asi");
    if (!module) {
        Log("effectprofile: ProperShaders.asi is not loaded");
        return false;
    }

    IDirect3DStateBlock9* savedDeviceState = nullptr;
    if (g_properShadersEffectProfileTestNoSaveState) {
        const HRESULT stateHr = device
            ? device->CreateStateBlock(D3DSBT_ALL, &savedDeviceState)
            : D3DERR_INVALIDCALL;
        if (FAILED(stateHr) || !savedDeviceState) {
            Log("effectprofile: refusing no-save-state test because state capture failed hr=0x{:08X}",
                static_cast<unsigned>(stateHr));
            if (savedDeviceState) savedDeviceState->Release();
            return false;
        }
    }

    std::memset(&g_properShadersEffectProfile, 0, sizeof(g_properShadersEffectProfile));
    g_properShadersEffectProfile.module = module;
    g_properShadersEffectProfile.savedDeviceState = savedDeviceState;
    g_properShadersEffectProfile.threadId = GetCurrentThreadId();
    QueryPerformanceFrequency(&g_properShadersEffectProfile.qpcFrequency);
    if (!InstallProperShadersCallPatches(module)) {
        if (g_properShadersEffectProfile.savedDeviceState) {
            g_properShadersEffectProfile.savedDeviceState->Release();
            g_properShadersEffectProfile.savedDeviceState = nullptr;
        }
        g_properShadersEffectProfile.module = nullptr;
        return false;
    }

    g_properShadersEffectProfile.startTick = GetTickCount();
    g_properShadersEffectProfile.active = true;
    Log("effectprofile: armed durationMs={} thread={} module={:08X} patches={} noSaveState={} skipDuplicateMatrices={} savedState={:08X}",
        g_properShadersEffectProfileDurationMs,
        g_properShadersEffectProfile.threadId, reinterpret_cast<std::uintptr_t>(module),
        static_cast<unsigned>(sizeof(g_properShadersCallPatches) / sizeof(g_properShadersCallPatches[0])),
        g_properShadersEffectProfileTestNoSaveState ? 1 : 0,
        g_properShadersEffectProfileTestSkipDuplicateMatrices ? 1 : 0, reinterpret_cast<std::uintptr_t>(savedDeviceState));
    return true;
}

static void FinishProperShadersEffectProfile(const char* reason)
{
    if (!g_properShadersEffectProfile.active && !g_properShadersEffectProfile.module) return;
    const DWORD elapsedMs = g_properShadersEffectProfile.startTick
        ? GetTickCount() - g_properShadersEffectProfile.startTick
        : 0;
    g_properShadersEffectProfile.active = false;
    RestoreProperShadersCallPatches();

    HRESULT stateRestoreHr = D3D_OK;
    if (g_properShadersEffectProfile.savedDeviceState) {
        stateRestoreHr = g_properShadersEffectProfile.savedDeviceState->Apply();
        g_properShadersEffectProfile.savedDeviceState->Release();
        g_properShadersEffectProfile.savedDeviceState = nullptr;
        Log("effectprofile: restored pre-test D3D9 state hr=0x{:08X}",
            static_cast<unsigned>(stateRestoreHr));
    }

    char path[MAX_PATH]{};
    FormatTo(path, sizeof(path), "{}\\scripts\\BridgeD3D9.effectprofile.log", g_gameDir);
    FILE* file = nullptr;
    if (fopen_s(&file, path, "a") == 0 && file) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::print(file,
            "# capture begin={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} reason={} elapsedMs={} frames={} thread={} foreignThreadCalls={} module={:08X} noSaveState={} skipDuplicateMatrices={} stateRestoreHr=0x{:08X}\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            reason ? reason : "unknown",
            elapsedMs,
            static_cast<unsigned long long>(g_properShadersEffectProfile.frames),
            g_properShadersEffectProfile.threadId,
            g_properShadersEffectProfile.foreignThreadCalls.load(), reinterpret_cast<std::uintptr_t>(g_properShadersEffectProfile.module),
            g_properShadersEffectProfileTestNoSaveState ? 1 : 0,
            g_properShadersEffectProfileTestSkipDuplicateMatrices ? 1 : 0,
            static_cast<unsigned>(stateRestoreHr));

        static const char* matrixNames[] = {
            "g_mWorldViewProjection",
            "g_mWorldInv",
            "g_mWorld",
            "g_mTextureTransform",
        };
        uint64_t matrixCalls = 0;
        uint64_t duplicateCalls = 0;
        uint64_t skippedDuplicateCalls = 0;
        for (size_t i = 0; i < static_cast<size_t>(ProperShadersMatrixSlot::Count); ++i) {
            const ProperShadersMatrixStats& stats = g_properShadersEffectProfile.matrices[i];
            matrixCalls += stats.method.calls;
            duplicateCalls += stats.duplicateCalls;
            skippedDuplicateCalls += stats.skippedDuplicateCalls;
            const double duplicatePercent = stats.method.calls
                ? 100.0 * static_cast<double>(stats.duplicateCalls) /
                    static_cast<double>(stats.method.calls)
                : 0.0;
            std::print(file,
                "matrix={} calls={} callsPerFrame={:.2f} duplicates={} skippedDuplicates={} duplicatePct={:.2f} totalMs={:.3f} avgNs={:.1f} maxUs={:.3f}\n",
                matrixNames[i],
                static_cast<unsigned long long>(stats.method.calls),
                g_properShadersEffectProfile.frames
                    ? static_cast<double>(stats.method.calls) /
                        static_cast<double>(g_properShadersEffectProfile.frames)
                    : 0.0,
                static_cast<unsigned long long>(stats.duplicateCalls),
                static_cast<unsigned long long>(stats.skippedDuplicateCalls),
                duplicatePercent,
                ProperShadersTicksToMilliseconds(stats.method.qpcTicks),
                stats.method.calls
                    ? ProperShadersTicksToMilliseconds(stats.method.qpcTicks) * 1000000.0 /
                        static_cast<double>(stats.method.calls)
                    : 0.0,
                ProperShadersTicksToMilliseconds(stats.method.maxQpcTicks) * 1000.0);
        }

        WriteProperShadersMethodStats(file, "CommitChanges",
            g_properShadersEffectProfile.commitChanges, g_properShadersEffectProfile.frames);
        WriteProperShadersMethodStats(file, "Begin",
            g_properShadersEffectProfile.begin, g_properShadersEffectProfile.frames);
        WriteProperShadersMethodStats(file, "BeginPass",
            g_properShadersEffectProfile.beginPass, g_properShadersEffectProfile.frames);
        WriteProperShadersMethodStats(file, "EndPass",
            g_properShadersEffectProfile.endPass, g_properShadersEffectProfile.frames);
        WriteProperShadersMethodStats(file, "End",
            g_properShadersEffectProfile.end, g_properShadersEffectProfile.frames);
        std::print(file,
            "beginFlags originalOr=0x{:08X} appliedOr=0x{:08X} modifiedCalls={}\n",
            static_cast<unsigned>(g_properShadersEffectProfile.beginOriginalFlagsOr),
            static_cast<unsigned>(g_properShadersEffectProfile.beginAppliedFlagsOr),
            static_cast<unsigned long long>(g_properShadersEffectProfile.beginFlagsModifiedCalls));
        std::print(file,
            "summary matrixCalls={} duplicateMatrices={} skippedDuplicateMatrices={} duplicatePct={:.2f}\n# capture end\n",
            static_cast<unsigned long long>(matrixCalls),
            static_cast<unsigned long long>(duplicateCalls),
            static_cast<unsigned long long>(skippedDuplicateCalls),
            matrixCalls
                ? 100.0 * static_cast<double>(duplicateCalls) / static_cast<double>(matrixCalls)
                : 0.0);
        fflush(file);
        fclose(file);

        Log("effectprofile: complete reason={} elapsedMs={} frames={} matrixCalls={} duplicateMatrices={} skippedDuplicateMatrices={} duplicatePct={:.2f} commitCalls={} output={}",
            reason ? reason : "unknown",
            elapsedMs,
            static_cast<unsigned long long>(g_properShadersEffectProfile.frames),
            static_cast<unsigned long long>(matrixCalls),
            static_cast<unsigned long long>(duplicateCalls),
            static_cast<unsigned long long>(skippedDuplicateCalls),
            matrixCalls
                ? 100.0 * static_cast<double>(duplicateCalls) / static_cast<double>(matrixCalls)
                : 0.0,
            static_cast<unsigned long long>(g_properShadersEffectProfile.commitChanges.calls),
            path);
    } else {
        Log("effectprofile: failed to open output={}", path);
    }

    g_properShadersEffectProfile.module = nullptr;
}

static void __stdcall PluginLog(const char* message)
{
    Log("plugin: {}", message ? message : "(null)");
}

template <typename Fn, typename... Args>
static void SafePluginCall(const char* name, Fn fn, Args... args)
{
    if (!fn) return;
    __try {
        fn(args...);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("postfx: plugin callback {} crashed, ignored", name);
    }
}

static BOOL SafePluginInit1(
    BridgeD3D9_PluginInit init,
    const BridgeD3D9PluginApi* api,
    bool* crashed)
{
    *crashed = false;
    __try {
        return init(api);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return FALSE;
    }
}

static BOOL SafePluginInit2(
    BridgeD3D9_PluginInit2 init,
    const BridgeD3D9PluginApi2* api,
    bool* crashed)
{
    *crashed = false;
    __try {
        return init(api);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return FALSE;
    }
}

static LoadedPlugin* FindLoadedPlugin(void* hostContext)
{
    HMODULE owner = reinterpret_cast<HMODULE>(hostContext);
    for (auto& plugin : g_plugins) {
        if (plugin.module == owner) return &plugin;
    }
    return nullptr;
}

static bool IsValidVulkanPassDesc(const D3D9GtaSaVulkanPassDesc* desc)
{
    return desc &&
        desc->StructSize >= sizeof(D3D9GtaSaVulkanPassDesc) &&
        desc->ApiVersion >= GtaSaCompatApiVersions::kVulkanPass &&
        desc->ApiVersion <= D3D9_GTA_SA_COMPAT_API_VERSION &&
        desc->Stage == D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT &&
        (desc->Flags & D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS) != 0 &&
        desc->Record != nullptr;
}

static HRESULT InvokeNativeVulkanPass(
    NativeVulkanPass* pass,
    const D3D9GtaSaVulkanFrameContext* frame)
{
    __try {
        return pass->desc.Record(pass->desc.UserData, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

static HRESULT STDMETHODCALLTYPE RecordNativeVulkanPass(
    void* userData,
    const D3D9GtaSaVulkanFrameContext* frame)
{
    auto* pass = static_cast<NativeVulkanPass*>(userData);
    if (!pass || !frame || !pass->desc.Record || g_inVulkanPassRecord) {
        return D3DERR_INVALIDCALL;
    }

    g_inVulkanPassRecord = true;
    HRESULT result = InvokeNativeVulkanPass(pass, frame);
    g_inVulkanPassRecord = false;

    if (FAILED(result)) {
        Log("vulkanhost: pass callback failed name={} bridgeToken={} result=0x{:08X}",
            pass->desc.Name,
            static_cast<unsigned long long>(pass->bridgeToken),
            static_cast<unsigned>(result));
    }
    return result;
}

static HRESULT RegisterPassOnHostLocked(NativeVulkanPass& pass, VulkanHostDevice& host)
{
    if (host.suspended || !host.compat) return S_FALSE;

    for (const auto& binding : pass.bindings) {
        if (binding.host == &host) return S_FALSE;
    }

    D3D9GtaSaVulkanPassDesc backendDesc = pass.desc;
    backendDesc.StructSize = sizeof(backendDesc);
    backendDesc.Record = &RecordNativeVulkanPass;
    backendDesc.UserData = &pass;

    UINT64 backendToken = 0;
    HRESULT result = host.compat->RegisterVulkanPass(&backendDesc, &backendToken);
    if (SUCCEEDED(result) && backendToken != 0) {
        pass.bindings.push_back({ &host, backendToken });
        Log("vulkanhost: pass bound name={} priority={} bridgeToken={} backendToken={}",
            pass.desc.Name,
            pass.desc.Priority,
            static_cast<unsigned long long>(pass.bridgeToken),
            static_cast<unsigned long long>(backendToken));
    } else {
        Log("vulkanhost: pass bind failed name={} bridgeToken={} result=0x{:08X}",
            pass.desc.Name,
            static_cast<unsigned long long>(pass.bridgeToken),
            static_cast<unsigned>(result));
    }
    return result;
}

static void UnregisterPassBindingsLocked(
    NativeVulkanPass& pass,
    VulkanHostDevice* onlyHost = nullptr)
{
    for (size_t i = pass.bindings.size(); i > 0; --i) {
        VulkanPassBinding binding = pass.bindings[i - 1];
        if (onlyHost && binding.host != onlyHost) continue;

        HRESULT result = binding.host && binding.host->compat
            ? binding.host->compat->UnregisterVulkanPass(binding.backendToken)
            : E_NOINTERFACE;
        if (FAILED(result) && result != D3DERR_NOTFOUND) {
            Log("vulkanhost: pass unbind failed name={} bridgeToken={} backendToken={} result=0x{:08X}",
                pass.desc.Name,
                static_cast<unsigned long long>(pass.bridgeToken),
                static_cast<unsigned long long>(binding.backendToken),
                static_cast<unsigned>(result));
        }
        pass.bindings.erase(pass.bindings.begin() + (i - 1));
    }
}

static VulkanHostDevice* AttachVulkanHost(IDirect3DDevice9* device)
{
    if (!device) return nullptr;

    ID3D9GtaSaCompatDevice1* compat = nullptr;
    HRESULT result = device->QueryInterface(
        __uuidof(ID3D9GtaSaCompatDevice1),
        reinterpret_cast<void**>(&compat));
    if (FAILED(result) || !compat) {
        Log("vulkanhost: GTA SA DXVK API v2 unavailable result=0x{:08X}; legacy callbacks remain active",
            static_cast<unsigned>(result));
        return nullptr;
    }

    D3D9GtaSaCompatStatus status{};
    status.StructSize = sizeof(status);
    result = compat->GetStatus(&status);
    const UINT requiredFlags = D3D9_GTA_SA_COMPAT_VULKAN_BACKEND |
        D3D9_GTA_SA_COMPAT_PASS_REGISTRY |
        D3D9_GTA_SA_COMPAT_COMMAND_RECORD;
    if (FAILED(result) ||
        !GtaSaCompatApiVersions::Supports(
            status.ApiVersion, GtaSaCompatApiVersions::kVulkanPass) ||
        (status.Flags & requiredFlags) != requiredFlags) {
        Log("vulkanhost: incompatible GTA SA DXVK API result=0x{:08X} api={} flags=0x{:08X}",
            static_cast<unsigned>(result), status.ApiVersion, status.Flags);
        compat->Release();
        return nullptr;
    }

    auto* host = new (std::nothrow) VulkanHostDevice();
    if (!host) {
        compat->Release();
        return nullptr;
    }
    host->compat = compat;
    host->status = status;

    size_t passCount = 0;
    size_t boundCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_vulkanHostMutex);
        g_vulkanHostDevices.push_back(host);
        for (auto& plugin : g_plugins) {
            for (auto& pass : plugin.vulkanPasses) {
                ++passCount;
                if (SUCCEEDED(RegisterPassOnHostLocked(*pass, *host))) ++boundCount;
            }
        }
    }

    Log("vulkanhost: attached api={} flags=0x{:08X} backbuffer={}x{} queued={} bound={}",
        status.ApiVersion, status.Flags, status.BackBufferWidth, status.BackBufferHeight,
        static_cast<unsigned long long>(passCount),
        static_cast<unsigned long long>(boundCount));
    return host;
}

static void SuspendVulkanHost(VulkanHostDevice* host)
{
    if (!host) return;
    std::lock_guard<std::mutex> lock(g_vulkanHostMutex);
    if (host->suspended) return;

    host->suspended = true;
    for (auto& plugin : g_plugins) {
        for (auto& pass : plugin.vulkanPasses) {
            UnregisterPassBindingsLocked(*pass, host);
        }
    }
    Log("vulkanhost: suspended for device reset");
}

static void ResumeVulkanHost(VulkanHostDevice* host)
{
    if (!host) return;
    std::lock_guard<std::mutex> lock(g_vulkanHostMutex);
    if (!host->suspended) return;

    host->suspended = false;
    for (auto& plugin : g_plugins) {
        for (auto& pass : plugin.vulkanPasses) {
            RegisterPassOnHostLocked(*pass, *host);
        }
    }
    Log("vulkanhost: resumed after device reset");
}

static void DetachVulkanHost(VulkanHostDevice*& host)
{
    if (!host) return;

    {
        std::lock_guard<std::mutex> lock(g_vulkanHostMutex);
        for (auto& plugin : g_plugins) {
            for (auto& pass : plugin.vulkanPasses) {
                UnregisterPassBindingsLocked(*pass, host);
            }
        }
        auto entry = std::find(g_vulkanHostDevices.begin(), g_vulkanHostDevices.end(), host);
        if (entry != g_vulkanHostDevices.end()) g_vulkanHostDevices.erase(entry);
    }

    host->compat->Release();
    delete host;
    host = nullptr;
    Log("vulkanhost: detached");
}

static HRESULT __stdcall HostGetVulkanStatus(
    void* hostContext,
    D3D9GtaSaCompatStatus* status)
{
    if (!status) return E_POINTER;
    if (status->StructSize < sizeof(D3D9GtaSaCompatStatus) || g_inVulkanPassRecord) {
        return D3DERR_INVALIDCALL;
    }

    std::lock_guard<std::mutex> lock(g_vulkanHostMutex);
    if (!FindLoadedPlugin(hostContext)) return E_ACCESSDENIED;
    if (g_vulkanHostDevices.empty()) {
        D3D9GtaSaCompatStatus pending{};
        pending.StructSize = sizeof(pending);
        pending.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
        *status = pending;
        return S_FALSE;
    }
    return g_vulkanHostDevices.front()->compat->GetStatus(status);
}

static HRESULT __stdcall HostRegisterVulkanPass(
    void* hostContext,
    const D3D9GtaSaVulkanPassDesc* desc,
    UINT64* token)
{
    if (!token) return E_POINTER;
    *token = 0;
    if (g_inVulkanPassRecord || !IsValidVulkanPassDesc(desc)) return D3DERR_INVALIDCALL;

    auto pass = std::unique_ptr<NativeVulkanPass>(new (std::nothrow) NativeVulkanPass());
    if (!pass) return E_OUTOFMEMORY;

    std::lock_guard<std::mutex> lock(g_vulkanHostMutex);
    LoadedPlugin* plugin = FindLoadedPlugin(hostContext);
    if (!plugin) return E_ACCESSDENIED;

    pass->owner = plugin->module;
    pass->bridgeToken = g_nextVulkanPassToken.fetch_add(1, std::memory_order_relaxed);
    pass->desc = *desc;
    pass->desc.StructSize = sizeof(pass->desc);
    pass->desc.Name[sizeof(pass->desc.Name) - 1] = '\0';

    NativeVulkanPass* registered = pass.get();
    plugin->vulkanPasses.push_back(std::move(pass));
    for (auto* host : g_vulkanHostDevices) {
        RegisterPassOnHostLocked(*registered, *host);
    }

    *token = registered->bridgeToken;
    Log("vulkanhost: pass registered name={} priority={} bridgeToken={} activeDevices={}",
        registered->desc.Name,
        registered->desc.Priority,
        static_cast<unsigned long long>(registered->bridgeToken),
        static_cast<unsigned long long>(g_vulkanHostDevices.size()));
    return D3D_OK;
}

static HRESULT __stdcall HostUnregisterVulkanPass(void* hostContext, UINT64 token)
{
    if (g_inVulkanPassRecord || token == 0) return D3DERR_INVALIDCALL;

    std::lock_guard<std::mutex> lock(g_vulkanHostMutex);
    LoadedPlugin* plugin = FindLoadedPlugin(hostContext);
    if (!plugin) return E_ACCESSDENIED;

    auto entry = std::find_if(plugin->vulkanPasses.begin(), plugin->vulkanPasses.end(),
        [token](const std::unique_ptr<NativeVulkanPass>& pass) {
            return pass->bridgeToken == token;
        });
    if (entry == plugin->vulkanPasses.end()) return D3DERR_NOTFOUND;

    UnregisterPassBindingsLocked(**entry);
    Log("vulkanhost: pass unregistered name={} bridgeToken={}",
        (*entry)->desc.Name,
        static_cast<unsigned long long>((*entry)->bridgeToken));
    plugin->vulkanPasses.erase(entry);
    return D3D_OK;
}

static void UnregisterPluginVulkanPasses(LoadedPlugin& plugin)
{
    std::lock_guard<std::mutex> lock(g_vulkanHostMutex);
    for (auto& pass : plugin.vulkanPasses) {
        UnregisterPassBindingsLocked(*pass);
    }
    plugin.vulkanPasses.clear();
}

struct D3D9CallCounters
{
    uint64_t beginScene = 0;
    uint64_t endScene = 0;
    uint64_t present = 0;
    uint64_t reset = 0;
    uint64_t clear = 0;
    uint64_t drawPrimitive = 0;
    uint64_t drawIndexedPrimitive = 0;
    uint64_t drawPrimitiveUP = 0;
    uint64_t drawIndexedPrimitiveUP = 0;
    uint64_t primitives = 0;
    uint64_t setTexture = 0;
    uint64_t redundantSetTexture = 0;
    uint64_t setTextureStageState = 0;
    uint64_t setSamplerState = 0;
    uint64_t setRenderState = 0;
    uint64_t redundantSetRenderState = 0;
    uint64_t setTransform = 0;
    uint64_t setMaterial = 0;
    uint64_t setLight = 0;
    uint64_t lightEnable = 0;
    uint64_t setRenderTarget = 0;
    uint64_t setDepthStencilSurface = 0;
    uint64_t setVertexShader = 0;
    uint64_t redundantSetVertexShader = 0;
    uint64_t setPixelShader = 0;
    uint64_t redundantSetPixelShader = 0;
    uint64_t setVertexShaderConstantF = 0;
    uint64_t redundantSetVertexShaderConstantF = 0;
    uint64_t vertexShaderConstantFVectors = 0;
    uint64_t redundantVertexShaderConstantFVectors = 0;
    uint64_t setPixelShaderConstantF = 0;
    uint64_t redundantSetPixelShaderConstantF = 0;
    uint64_t pixelShaderConstantFVectors = 0;
    uint64_t redundantPixelShaderConstantFVectors = 0;
    uint64_t setStreamSource = 0;
    uint64_t setIndices = 0;
    uint64_t setVertexDeclaration = 0;
    uint64_t setFVF = 0;
    uint64_t createTexture = 0;
    uint64_t createVertexBuffer = 0;
    uint64_t createIndexBuffer = 0;
    uint64_t createRenderTarget = 0;
    uint64_t createDepthStencilSurface = 0;
    uint64_t createVertexShader = 0;
    uint64_t createPixelShader = 0;
};

static constexpr UINT kD3D9CallsiteStackDepth = 12;
static constexpr UINT kD3D9CallsiteMaxEntries = 512;

struct D3D9CallsiteEntry
{
    uintptr_t keyAddress = 0;
    uintptr_t originRva = 0;
    uintptr_t firstMainRva = 0;
    uintptr_t immediateModuleBase = 0;
    uintptr_t immediateModuleRva = 0;
    uint64_t samples = 0;
    uint64_t sampledPrimitives = 0;
    UINT kind = 0;
    UINT renderTargetWidth = 0;
    UINT renderTargetHeight = 0;
    DWORD renderTargetFormat = D3DFMT_UNKNOWN;
    USHORT stackDepth = 0;
    void* stack[kD3D9CallsiteStackDepth]{};
};

static bool GetPeImageInfo(HMODULE module, uintptr_t& imageBase, uintptr_t& imageSize,
    uintptr_t& preferredBase)
{
    imageBase = reinterpret_cast<uintptr_t>(module);
    imageSize = 0;
    preferredBase = 0;
    if (!module) return false;

    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(imageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return false;
        }
        imageSize = nt->OptionalHeader.SizeOfImage;
        preferredBase = nt->OptionalHeader.ImageBase;
        return imageSize != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        imageSize = 0;
        preferredBase = 0;
        return false;
    }
}

static const char* D3D9DrawKindName(UINT kind)
{
    switch (kind) {
    case 1: return "DP";
    case 2: return "DIP";
    case 3: return "DPUP";
    case 4: return "DIPUP";
    default: return "UNKNOWN";
    }
}

static uint64_t CounterDelta(uint64_t current, uint64_t previous)
{
    return current >= previous ? current - previous : 0;
}

struct CpuHotspotWorkerContext
{
    DWORD targetThreadId = 0;
    UINT captureId = 0;
    DWORD durationMs = 0;
    DWORD intervalMs = 0;
    char outputPath[MAX_PATH]{};
    char screenshotPath[MAX_PATH]{};
};

static bool ReadGtasaFrameCounter(uint32_t& frameCounter)
{
    constexpr uintptr_t kGtasaFrameCounterAddress = 0x00B7CB4C;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(kGtasaFrameCounterAddress),
        &frameCounter,
        sizeof(frameCounter),
        &bytesRead) != FALSE && bytesRead == sizeof(frameCounter);
}

template <typename T>
static bool ReadGtasaValue(uintptr_t address, T& value)
{
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(address),
        &value,
        sizeof(value),
        &bytesRead) != FALSE && bytesRead == sizeof(value);
}

static bool IsGtasaBenchmarkWorldReady()
{
    uint8_t playerInFocus = 0;
    bool codePause = true;
    bool userPause = true;
    if (!ReadGtasaValue(0x00B7CD74, playerInFocus) || playerInFocus >= 2 ||
        !ReadGtasaValue(0x00B7CB48, codePause) ||
        !ReadGtasaValue(0x00B7CB49, userPause)) {
        return false;
    }

    uintptr_t playerPed = 0;
    const uintptr_t playerInfo = 0x00B7CD98 + static_cast<uintptr_t>(playerInFocus) * 0x190;
    return ReadGtasaValue(playerInfo, playerPed) && playerPed != 0 && !codePause && !userPause;
}

static double MedianFps(const double* values, UINT count)
{
    if (!values || !count) return 0.0;
    std::vector<double> sorted(values, values + count);
    std::sort(sorted.begin(), sorted.end());
    const size_t middle = sorted.size() / 2;
    return sorted.size() & 1
        ? sorted[middle]
        : (sorted[middle - 1] + sorted[middle]) * 0.5;
}

template <class... Args>
static void WriteProperShadersAutoBenchmarkLine(
    std::format_string<Args...> fmt, Args&&... args)
{
    char path[MAX_PATH]{};
    FormatTo(path, sizeof(path), "{}\\scripts\\BridgeD3D9.autobenchmark.log", g_gameDir);
    FILE* file = nullptr;
    if (fopen_s(&file, path, "a") != 0 || !file) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::print(file, "[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::vprint_nonunicode(file, fmt.get(), std::make_format_args(args...));
    fputc('\n', file);
    fclose(file);
}

static void BeginProperShadersBenchmarkEpoch(bool optimized, ULONGLONG now, uint32_t frameCounter)
{
    g_properShadersNoSaveStateActive.store(false, std::memory_order_relaxed);
    g_properShadersAutoBenchmark.stage = ProperShadersAutoBenchmarkStage::Epoch;
    g_properShadersAutoBenchmark.stageStartTick = now;
    g_properShadersAutoBenchmark.frameStart = frameCounter;
    g_properShadersAutoBenchmark.beginCallsStart = static_cast<LONG>(
        g_properShadersOptimizationBeginCalls);
    g_properShadersAutoBenchmark.modifiedCallsStart = static_cast<LONG>(
        g_properShadersOptimizationModifiedCalls);
    g_properShadersAutoBenchmark.failuresStart = static_cast<LONG>(
        g_properShadersOptimizationFailures);
}

static void OnProperShadersEffectOptimizationPresent()
{
    static ULONGLONG lastBatchLogTick = 0;
    static uint32_t lastBatchFrameCounter = 0;
    static bool lastBatchFrameCounterValid = false;
    static uint64_t lastBatchStarts = 0;
    static uint64_t lastBatchReuses = 0;
    static uint64_t lastBatchCommits = 0;
    static uint64_t lastBatchFallbacks = 0;
    static uint64_t lastBatchBeginAttempts = 0;
    static uint64_t lastBatchTechniqueCalls = 0;
    static uint64_t lastBatchMode2Attempts = 0;
    static uint64_t lastBatchStandaloneAttempts = 0;
    static uint64_t lastBatchRejectCounts[kProperShadersBatchRejectReasonCount]{};
    static uint64_t lastMatrixCalls = 0;
    static uint64_t lastMatrixSkips = 0;
    static uint64_t lastParameterCalls = 0;
    static uint64_t lastParameterSkips = 0;
    static uint64_t lastDirectActivations = 0;
    static uint64_t lastDirectCommits = 0;
    static uint64_t lastDirectFallbacks = 0;
    static uint64_t lastDirectVsWrites = 0;
    static uint64_t lastDirectPsWrites = 0;
    static uint64_t lastDirectTextureWrites = 0;
    static uint64_t lastDirectBatchSubmissions = 0;
    static ProperShadersStateJournalDiagnostics lastJournalDiagnostics{};
    if (!g_enableProperShadersEffectOptimization) return;

    HMODULE module = GetModuleHandleA("ProperShaders.asi");
    if (!module) module = GetModuleHandleA("propershaders.asi");
    if (!module) return;

    if (g_properShadersGeneralStateJournal && !g_properShadersCreateEffectIatSlot) {
        InstallProperShadersCreateEffectHook(module);
    }
    if (!g_properShadersOptimizationHooksInstalled) {
        if (!InstallProperShadersOptimizationPatches(module)) return;
        g_properShadersNoSaveStateActive.store(true, std::memory_order_release);
        Log("effectopt: incremental state journal active; manual A/B loop disabled");
    }

    const ULONGLONG now = GetTickCount64();
    if (g_properShadersEffectBatching && now - lastBatchLogTick >= 5000) {
        const ULONGLONG elapsedMs = lastBatchLogTick ? now - lastBatchLogTick : 0;
        lastBatchLogTick = now;
        const uint64_t starts = g_properShadersBatchStarts;
        const uint64_t reuses = g_properShadersBatchReuses;
        const uint64_t commits = g_properShadersBatchCommits;
        const uint64_t fallbacks = g_properShadersBatchFallbacks;
        const uint64_t beginAttempts = g_properShadersBatchBeginAttempts;
        const uint64_t techniqueCalls = g_properShadersBatchTechniqueCalls;
        const uint64_t mode2Attempts = g_properShadersBatchMode2Attempts;
        const uint64_t standaloneAttempts =
            g_properShadersBatchStandaloneAttempts;
        uint64_t rejectDeltas[kProperShadersBatchRejectReasonCount]{};
        for (size_t i = 0; i < kProperShadersBatchRejectReasonCount; ++i) {
            rejectDeltas[i] =
                g_properShadersBatchRejectCounts[i] - lastBatchRejectCounts[i];
        }
        const uint64_t matrixCalls = g_properShadersMatrixCalls;
        const uint64_t matrixSkips = g_properShadersMatrixSkips;
        const uint64_t parameterCalls = g_properShadersParameterCalls;
        const uint64_t parameterSkips = g_properShadersParameterSkips;
        const uint64_t directActivations = g_properShadersDirectActivations;
        const uint64_t directCommits = g_properShadersDirectCommits;
        const uint64_t directFallbacks = g_properShadersDirectFallbacks;
        const uint64_t directVsWrites = g_properShadersDirectVsWrites;
        const uint64_t directPsWrites = g_properShadersDirectPsWrites;
        const uint64_t directTextureWrites = g_properShadersDirectTextureWrites;
        const uint64_t directBatchSubmissions =
            g_properShadersDirectBatchSubmissions;
        const ProperShadersStateJournalDiagnostics journalDiagnostics =
            ProperShadersStateJournal::GetDiagnostics();

        uint32_t frameCounter = 0;
        const bool frameCounterValid = ReadGtasaFrameCounter(frameCounter);
        const uint32_t frames = frameCounterValid && lastBatchFrameCounterValid
            ? frameCounter - lastBatchFrameCounter
            : 0;
        const double fps = elapsedMs && frameCounterValid && lastBatchFrameCounterValid
            ? 1000.0 * static_cast<double>(frames) / static_cast<double>(elapsedMs)
            : 0.0;
        const uint64_t intervalMatrixCalls = matrixCalls - lastMatrixCalls;
        const uint64_t intervalMatrixSkips = matrixSkips - lastMatrixSkips;
        const double matrixSkipPercent = intervalMatrixCalls > 0
            ? 100.0 * static_cast<double>(intervalMatrixSkips) /
                static_cast<double>(intervalMatrixCalls)
            : 0.0;
        const uint64_t intervalParameterCalls = parameterCalls - lastParameterCalls;
        const uint64_t intervalParameterSkips = parameterSkips - lastParameterSkips;
        const double parameterSkipPercent = intervalParameterCalls > 0
            ? 100.0 * static_cast<double>(intervalParameterSkips) /
                static_cast<double>(intervalParameterCalls)
            : 0.0;

        Log("effectbatch: intervalMs={} fps={:.1f} frames={} attempts={} mode2={} standalone={} techniqueCalls={} starts={} reusedBegins={} commits={} fallbacks={} reject=disabled:{}/noSave:{}/technique:{}/begin:{}/passCount:{}/multiPass:{}/binding:{}/journalInactive:{}/journalDisabled:{} matrixCalls={} matrixSkips={} matrixSkipPercent={:.1f} parameterCalls={} parameterSkips={} parameterSkipPercent={:.1f} directActivations={} directCommits={} directFallbacks={} directVsWrites={} directPsWrites={} directTextureWrites={} directBatches={} nativeJournal=begin:{}/restore:{}/localFallback:{}/failure:{}/captureEnable:{}/captureDisable:{} totals={}/{}/{}/{}",
            static_cast<unsigned long long>(elapsedMs),
            fps,
            frames,
            static_cast<long long>(beginAttempts - lastBatchBeginAttempts),
            static_cast<long long>(mode2Attempts - lastBatchMode2Attempts),
            static_cast<long long>(standaloneAttempts -
                lastBatchStandaloneAttempts),
            static_cast<long long>(techniqueCalls - lastBatchTechniqueCalls),
            static_cast<long long>(starts - lastBatchStarts),
            static_cast<long long>(reuses - lastBatchReuses),
            static_cast<long long>(commits - lastBatchCommits),
            static_cast<long long>(fallbacks - lastBatchFallbacks),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::BatchingDisabled)]),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::NoSaveStateInactive)]),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::MissingTechnique)]),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::BeginFailed)]),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::MissingPassCount)]),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::MultiPass)]),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::MissingBinding)]),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::JournalInactive)]),
            static_cast<long long>(rejectDeltas[static_cast<size_t>(
                ProperShadersBatching::BatchRejectReason::JournalDisabled)]),
            static_cast<long long>(intervalMatrixCalls),
            static_cast<long long>(intervalMatrixSkips),
            matrixSkipPercent,
            static_cast<long long>(intervalParameterCalls),
            static_cast<long long>(intervalParameterSkips),
            parameterSkipPercent,
            static_cast<long long>(directActivations - lastDirectActivations),
            static_cast<long long>(directCommits - lastDirectCommits),
            static_cast<long long>(directFallbacks - lastDirectFallbacks),
            static_cast<long long>(directVsWrites - lastDirectVsWrites),
            static_cast<long long>(directPsWrites - lastDirectPsWrites),
            static_cast<long long>(directTextureWrites - lastDirectTextureWrites),
            static_cast<long long>(directBatchSubmissions -
                lastDirectBatchSubmissions),
            static_cast<long long>(journalDiagnostics.nativeBegins -
                lastJournalDiagnostics.nativeBegins),
            static_cast<long long>(journalDiagnostics.nativeRestores -
                lastJournalDiagnostics.nativeRestores),
            static_cast<long long>(journalDiagnostics.localFallbacks -
                lastJournalDiagnostics.localFallbacks),
            static_cast<long long>(journalDiagnostics.nativeFailures -
                lastJournalDiagnostics.nativeFailures),
            static_cast<long long>(journalDiagnostics.nativeCaptureEnables -
                lastJournalDiagnostics.nativeCaptureEnables),
            static_cast<long long>(journalDiagnostics.nativeCaptureDisables -
                lastJournalDiagnostics.nativeCaptureDisables),
            static_cast<long long>(starts),
            static_cast<long long>(reuses),
            static_cast<long long>(commits),
            static_cast<long long>(fallbacks));

        lastBatchFrameCounter = frameCounter;
        lastBatchFrameCounterValid = frameCounterValid;
        lastBatchStarts = starts;
        lastBatchReuses = reuses;
        lastBatchCommits = commits;
        lastBatchFallbacks = fallbacks;
        lastBatchBeginAttempts = beginAttempts;
        lastBatchTechniqueCalls = techniqueCalls;
        lastBatchMode2Attempts = mode2Attempts;
        lastBatchStandaloneAttempts = standaloneAttempts;
        for (size_t i = 0; i < kProperShadersBatchRejectReasonCount; ++i) {
            lastBatchRejectCounts[i] = g_properShadersBatchRejectCounts[i];
        }
        lastMatrixCalls = matrixCalls;
        lastMatrixSkips = matrixSkips;
        lastParameterCalls = parameterCalls;
        lastParameterSkips = parameterSkips;
        lastDirectActivations = directActivations;
        lastDirectCommits = directCommits;
        lastDirectFallbacks = directFallbacks;
        lastDirectVsWrites = directVsWrites;
        lastDirectPsWrites = directPsWrites;
        lastDirectTextureWrites = directTextureWrites;
        lastDirectBatchSubmissions = directBatchSubmissions;
        lastJournalDiagnostics = journalDiagnostics;
    }
    ResetProperShadersMatrixCaches();
    ResetProperShadersParameterCaches();
}

static bool StartProperShadersStateAttribution()
{
    if (!g_enableProperShadersStateAttribution ||
        g_properShadersStateAttributionActive.load(std::memory_order_acquire)) {
        return false;
    }
    if (g_enableProperShadersEffectProfile) {
        Log("stateattribution: trigger ignored while legacy effect profile is enabled");
        return false;
    }

    g_properShadersStateAttributionStartJournalDiagnostics =
        ProperShadersStateJournal::GetDiagnostics();
    JournalAttributionStartCapture();
    g_properShadersStateAttributionStartTick = GetTickCount64();
    ++g_properShadersStateAttributionCaptureId;
    Log("stateattribution: capture={} started durationMs={} triggerVirtualKey={}",
        g_properShadersStateAttributionCaptureId,
        g_properShadersStateAttributionDurationMs,
        g_properShadersStateAttributionTriggerKey);
    return true;
}

static void FinishProperShadersStateAttribution(const char* reason)
{
    if (!g_properShadersStateAttributionActive.load(std::memory_order_acquire)) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = g_properShadersStateAttributionStartTick
        ? now - g_properShadersStateAttributionStartTick
        : 0;

    // Stop admission first, then wait for any transaction that entered the
    // window before dumping. This preserves the final Restore measurement.
    JournalAttributionStopCapture();
    JournalAttributionDump();
    const ProperShadersStateJournalDiagnostics endDiagnostics =
        ProperShadersStateJournal::GetDiagnostics();
    const auto delta = [](std::uint64_t end, std::uint64_t begin) {
        return end >= begin ? end - begin : 0;
    };
    Log("stateattribution: capture={} finished reason={} elapsedMs={} nativeBegin={} nativeRestore={} localFallback={} nativeFailure={} captureEnable={} captureDisable={} output=scripts\\BridgeD3D9.state-attribution.log",
        g_properShadersStateAttributionCaptureId,
        reason ? reason : "unknown",
        static_cast<unsigned long long>(elapsed),
        static_cast<unsigned long long>(delta(
            endDiagnostics.nativeBegins,
            g_properShadersStateAttributionStartJournalDiagnostics.nativeBegins)),
        static_cast<unsigned long long>(delta(
            endDiagnostics.nativeRestores,
            g_properShadersStateAttributionStartJournalDiagnostics.nativeRestores)),
        static_cast<unsigned long long>(delta(
            endDiagnostics.localFallbacks,
            g_properShadersStateAttributionStartJournalDiagnostics.localFallbacks)),
        static_cast<unsigned long long>(delta(
            endDiagnostics.nativeFailures,
            g_properShadersStateAttributionStartJournalDiagnostics.nativeFailures)),
        static_cast<unsigned long long>(delta(
            endDiagnostics.nativeCaptureEnables,
            g_properShadersStateAttributionStartJournalDiagnostics.nativeCaptureEnables)),
        static_cast<unsigned long long>(delta(
            endDiagnostics.nativeCaptureDisables,
            g_properShadersStateAttributionStartJournalDiagnostics.nativeCaptureDisables)));
}

static void PollProperShadersStateAttribution(bool& triggerWasDown, bool countPresent)
{
    if (!g_enableProperShadersStateAttribution) {
        triggerWasDown = false;
        return;
    }

    const bool triggerDown =
        (GetAsyncKeyState(g_properShadersStateAttributionTriggerKey) & 0x8000) != 0;
    if (!g_properShadersStateAttributionActive.load(std::memory_order_acquire)) {
        if (!triggerDown) {
            triggerWasDown = false;
            return;
        }
        if (triggerWasDown) return;
        triggerWasDown = true;
        Log("stateattribution: trigger observed on unwrapped support worker key={}",
            g_properShadersStateAttributionTriggerKey);
        StartProperShadersStateAttribution();
        return;
    }

    if (countPresent) JournalAttributionOnPresent();
    if (!triggerDown) triggerWasDown = false;
    const ULONGLONG now = GetTickCount64();
    if (g_properShadersStateAttributionStartTick &&
        now - g_properShadersStateAttributionStartTick >=
            g_properShadersStateAttributionDurationMs) {
        FinishProperShadersStateAttribution("duration-complete");
    }
}

struct CpuExecutableRange
{
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

struct CpuHotspotStackKey
{
    uintptr_t instructionPointer = 0;
    uintptr_t caller1 = 0;
    uintptr_t caller2 = 0;
    uintptr_t caller3 = 0;

    bool operator==(const CpuHotspotStackKey& other) const
    {
        return instructionPointer == other.instructionPointer &&
            caller1 == other.caller1 &&
            caller2 == other.caller2 &&
            caller3 == other.caller3;
    }
};

struct CpuHotspotStackKeyHash
{
    size_t operator()(const CpuHotspotStackKey& key) const
    {
        size_t hash = static_cast<size_t>(2166136261u);
        const uintptr_t values[] = {
            key.instructionPointer,
            key.caller1,
            key.caller2,
            key.caller3,
        };
        for (uintptr_t value : values) {
            hash ^= static_cast<size_t>(value);
            hash *= static_cast<size_t>(16777619u);
        }
        return hash;
    }
};

static void CollectCpuExecutableRangesForModule(
    std::vector<CpuExecutableRange>& ranges,
    const uintptr_t moduleBase)
{
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;

        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
            const uintptr_t begin = moduleBase + section[i].VirtualAddress;
            const uintptr_t size = (std::max)(
                static_cast<uintptr_t>(section[i].Misc.VirtualSize),
                static_cast<uintptr_t>(section[i].SizeOfRawData));
            if (!size || begin + size < begin) continue;
            ranges.push_back({ begin, begin + size });
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void CollectCpuExecutableRanges(
    std::vector<CpuExecutableRange>& ranges,
    std::vector<BridgePerformance::ModuleIdentity>& modules)
{
    ranges.clear();
    modules.clear();
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32 moduleEntry{};
    moduleEntry.dwSize = sizeof(moduleEntry);
    if (Module32First(snapshot, &moduleEntry)) {
        do {
            const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(moduleEntry.modBaseAddr);
            const uintptr_t moduleSize = static_cast<uintptr_t>(
                moduleEntry.modBaseSize);
            if (moduleBase && moduleSize && moduleBase + moduleSize >= moduleBase) {
                BridgePerformance::ModuleIdentity identity{};
                identity.normalizedPath = BridgePerformance::NormalizeModulePath(
                    moduleEntry.szExePath);
                identity.basename = BridgePerformance::ModuleBasename(
                    identity.normalizedPath);
                identity.moduleBase = moduleBase;
                identity.imageSize = moduleSize;
                modules.push_back(identity);
            }
            CollectCpuExecutableRangesForModule(ranges, moduleBase);
        } while (Module32Next(snapshot, &moduleEntry));
    }
    CloseHandle(snapshot);

    std::sort(ranges.begin(), ranges.end(), [](const CpuExecutableRange& left, const CpuExecutableRange& right) {
        return left.begin < right.begin;
    });
    std::sort(
        modules.begin(),
        modules.end(),
        [](const BridgePerformance::ModuleIdentity& left,
           const BridgePerformance::ModuleIdentity& right) {
            return left.moduleBase < right.moduleBase;
        });
}

static bool IsCpuExecutableAddress(const std::vector<CpuExecutableRange>& ranges, uintptr_t address)
{
    if (!address || ranges.empty()) return false;
    auto it = std::upper_bound(
        ranges.begin(),
        ranges.end(),
        address,
        [](uintptr_t value, const CpuExecutableRange& range) {
            return value < range.begin;
        });
    if (it == ranges.begin()) return false;
    --it;
    return address >= it->begin && address < it->end;
}

static UINT CopyCpuStackWords(
    uintptr_t stackPointer,
    std::span<uintptr_t> output)
{
    if (!stackPointer || output.empty()) return 0;
    __try {
        std::memcpy(output.data(), reinterpret_cast<const void*>(stackPointer),
            output.size_bytes());
        return static_cast<UINT>(output.size());
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct ResolvedCpuAddress
{
    std::string moduleName = "private";
    uintptr_t moduleBase = 0;
    uintptr_t rva = 0;
    uintptr_t preferredAddress = 0;
};

static ResolvedCpuAddress ResolveCpuHotspotAddress(uintptr_t address)
{
    ResolvedCpuAddress result;
    if (!address) return result;

    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region)) {
        return result;
    }

    result.moduleBase = reinterpret_cast<uintptr_t>(region.AllocationBase);
    if (!result.moduleBase || address < result.moduleBase) return result;
    result.rva = address - result.moduleBase;

    HMODULE module = reinterpret_cast<HMODULE>(result.moduleBase);
    char path[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    if (pathLength) {
        const char* name = path;
        if (const char* slash = std::strrchr(name, '\\')) name = slash + 1;
        result.moduleName = name;
    }

    uintptr_t imageBase = 0;
    uintptr_t imageSize = 0;
    uintptr_t preferredBase = 0;
    if (GetPeImageInfo(module, imageBase, imageSize, preferredBase) &&
        address >= imageBase && address < imageBase + imageSize) {
        result.preferredAddress = preferredBase + (address - imageBase);
    }
    return result;
}

static void FormatCpuHotspotAddressBrief(uintptr_t address, char* output, size_t outputSize)
{
    if (!output || !outputSize) return;
    if (!address) {
        const auto [end, written] = std::format_to_n(
            output, outputSize - 1, "none");
        *end = '\0';
        return;
    }

    const ResolvedCpuAddress resolved = ResolveCpuHotspotAddress(address);
    const auto [end, written] = std::format_to_n(
        output, outputSize - 1, "{}+0x{:08X}", resolved.moduleName,
        static_cast<unsigned>(resolved.rva));
    *end = '\0';
}

static BridgePerformance::ModuleIdentity MakePerformanceModuleIdentity(
    const uintptr_t moduleBase)
{
    BridgePerformance::ModuleIdentity identity{};
    identity.moduleBase = moduleBase;
    if (!moduleBase)
    {
        identity.normalizedPath = "private";
        identity.basename = "private";
        return identity;
    }

    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(
        reinterpret_cast<HMODULE>(moduleBase),
        path,
        static_cast<DWORD>(sizeof(path)));
    if (length != 0)
    {
        identity.normalizedPath = BridgePerformance::NormalizeModulePath(path);
        identity.basename = BridgePerformance::ModuleBasename(
            identity.normalizedPath);
    }

    uintptr_t imageBase = 0;
    uintptr_t imageSize = 0;
    uintptr_t preferredBase = 0;
    if (GetPeImageInfo(
            reinterpret_cast<HMODULE>(moduleBase),
            imageBase,
            imageSize,
            preferredBase))
    {
        identity.imageSize = imageSize;
    }
    return identity;
}

static BridgePerformance::CaptureSamples BuildPerformanceCaptureSamples(
    const std::vector<std::pair<uintptr_t, uint64_t>>& orderedAddresses,
    const std::vector<std::pair<CpuHotspotStackKey, uint64_t>>& orderedStacks,
    const std::vector<BridgePerformance::ModuleIdentity>& moduleSnapshot,
    const uint64_t totalSamples)
{
    BridgePerformance::CaptureSamples capture{};
    capture.totalSamples = totalSamples;

    std::unordered_map<uintptr_t, uintptr_t> addressToModule;
    std::unordered_map<uintptr_t, BridgePerformance::ModuleIdentity> identities;
    std::unordered_map<uintptr_t, uint64_t> moduleSamples;

    const auto resolveModule = [&](const uintptr_t address) -> uintptr_t
    {
        if (!address)
        {
            return 0;
        }
        const auto cached = addressToModule.find(address);
        if (cached != addressToModule.end())
        {
            return cached->second;
        }

        const BridgePerformance::ModuleIdentity* snapshotModule =
            BridgePerformance::FindSnapshotModuleByAddress(
                moduleSnapshot,
                address);
        const uintptr_t moduleBase = snapshotModule
            ? snapshotModule->moduleBase
            : 0;
        addressToModule.emplace(address, moduleBase);
        if (identities.find(moduleBase) == identities.end())
        {
            identities.emplace(
                moduleBase,
                snapshotModule
                    ? *snapshotModule
                    : MakePerformanceModuleIdentity(0));
        }
        return moduleBase;
    };

    for (const auto& entry : orderedAddresses)
    {
        moduleSamples[resolveModule(entry.first)] += entry.second;
    }

    for (const auto& entry : orderedStacks)
    {
        BridgePerformance::StackSample stack{};
        stack.instructionModuleBase = resolveModule(
            entry.first.instructionPointer);
        stack.callerModuleBases = {
            resolveModule(entry.first.caller1),
            resolveModule(entry.first.caller2),
            resolveModule(entry.first.caller3),
        };
        stack.samples = entry.second;
        capture.stacks.push_back(stack);
    }

    for (const auto& entry : identities)
    {
        if (moduleSamples.find(entry.first) == moduleSamples.end())
        {
            moduleSamples.emplace(entry.first, 0);
        }
    }

    std::vector<std::pair<uintptr_t, uint64_t>> orderedModules;
    orderedModules.reserve(moduleSamples.size());
    for (const auto& entry : moduleSamples)
    {
        orderedModules.push_back(entry);
    }
    std::sort(
        orderedModules.begin(),
        orderedModules.end(),
        [](const auto& left, const auto& right)
        {
            return left.first < right.first;
        });
    for (const auto& entry : orderedModules)
    {
        const auto identity = identities.find(entry.first);
        capture.modules.push_back({
            identity != identities.end()
                ? identity->second
                : MakePerformanceModuleIdentity(entry.first),
            entry.second});
    }
    return capture;
}

static void AppendPerformanceWarning(
    std::vector<BridgePerformance::AdapterWarning>& warnings,
    const std::string& adapter,
    const char* code,
    const std::string& detail)
{
    for (const BridgePerformance::AdapterWarning& warning : warnings)
    {
        if (warning.adapter == adapter && warning.code == code &&
            warning.detail == detail)
        {
            return;
        }
    }
    warnings.push_back({code, adapter, detail});
}

static void CollectPerformanceConfigRows(
    const BridgePerformance::AdapterRuntimeConfig& runtime,
    std::vector<BridgePerformance::AdapterConfigRow>& rows,
    std::vector<BridgePerformance::AdapterWarning>& warnings)
{
    if (!runtime.includeConfigSnapshots)
    {
        return;
    }

    for (const BridgePerformance::AdapterDefinition& adapter :
         runtime.registry.adapters)
    {
        if (!adapter.enabled)
        {
            continue;
        }
        for (const std::string& path : adapter.configPaths)
        {
            const BridgePerformance::ConfigSnapshot snapshot =
                BridgePerformance::ReadConfigSnapshot(path);
            for (const BridgePerformance::ConfigSnapshotEntry& entry :
                 snapshot.entries)
            {
                const std::string key = entry.section.empty()
                    ? entry.key
                    : entry.section + "." + entry.key;
                rows.push_back({adapter.name, key, entry.value, entry.source});
            }
            for (const BridgePerformance::AdapterWarning& warning :
                 snapshot.warnings)
            {
                AppendPerformanceWarning(
                    warnings,
                    adapter.name,
                    warning.code.c_str(),
                    warning.detail);
            }
        }
    }
}

static HMODULE RevalidatePerformanceProviderModule(
    const BridgePerformance::ModuleIdentity& capturedModule)
{
    if (!capturedModule.moduleBase || !capturedModule.imageSize)
    {
        return nullptr;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCSTR>(capturedModule.moduleBase),
            &module) ||
        reinterpret_cast<uintptr_t>(module) != capturedModule.moduleBase)
    {
        if (module)
        {
            FreeLibrary(module);
        }
        return nullptr;
    }

    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(capturedModule.moduleBase),
            &region,
            sizeof(region)) != sizeof(region) ||
        region.AllocationBase !=
            reinterpret_cast<const void*>(capturedModule.moduleBase) ||
        region.Type != MEM_IMAGE)
    {
        FreeLibrary(module);
        return nullptr;
    }

    uintptr_t imageBase = 0;
    uintptr_t imageSize = 0;
    uintptr_t preferredBase = 0;
    if (!GetPeImageInfo(module, imageBase, imageSize, preferredBase) ||
        imageBase != capturedModule.moduleBase || imageSize == 0)
    {
        FreeLibrary(module);
        return nullptr;
    }

    char currentPath[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameA(
        module,
        currentPath,
        static_cast<DWORD>(sizeof(currentPath)));
    if (!pathLength || pathLength >= sizeof(currentPath))
    {
        FreeLibrary(module);
        return nullptr;
    }
    if (!capturedModule.normalizedPath.empty() &&
        BridgePerformance::NormalizeModulePath(currentPath) !=
            capturedModule.normalizedPath)
    {
        FreeLibrary(module);
        return nullptr;
    }

    return module;
}

static const char* ProviderCallStatusName(
    const BridgePerformanceProviderV1::CallStatus status)
{
    using Status = BridgePerformanceProviderV1::CallStatus;
    switch (status)
    {
    case Status::Accepted:
        return "accepted";
    case Status::SkippedQuarantined:
        return "provider-quarantined";
    case Status::InvalidFunction:
        return "invalid-function";
    case Status::InvalidQuery:
        return "invalid-query";
    case Status::ProviderReturnedFalse:
        return "provider-returned-false";
    case Status::ProviderException:
        return "provider-exception";
    case Status::SlowProvider:
        return "slow-provider";
    case Status::InvalidSnapshot:
        return "invalid-snapshot";
    }
    return "unknown-provider-status";
}

static void CollectPerformanceProviderRows(
    const BridgePerformance::AdapterRuntimeConfig& runtime,
    const BridgePerformance::CaptureMetadata& metadata,
    const std::vector<BridgePerformance::ModuleIdentity>& moduleSnapshot,
    std::vector<BridgePerformance::AdapterMetricRow>& metrics,
    std::vector<BridgePerformance::AdapterWarning>& warnings)
{
    if (!runtime.enableProviders)
    {
        return;
    }

    for (const BridgePerformance::AdapterDefinition& adapter :
         runtime.registry.adapters)
    {
        if (!adapter.enabled || adapter.providerExport.empty())
        {
            continue;
        }

        const std::string healthKey =
            BridgePerformance::NormalizeModulePath(adapter.name) + "|" +
            adapter.providerExport;
        BridgePerformanceProviderV1::ProviderHealth& health =
            g_performanceProviderHealth[healthKey];
        const BridgePerformance::ModuleIdentity* capturedModule =
            BridgePerformance::FindSnapshotProviderModule(
                adapter,
                moduleSnapshot);
        if (!capturedModule)
        {
            AppendPerformanceWarning(
                warnings,
                adapter.name,
                "provider-not-loaded",
                "provider module is not loaded");
            continue;
        }
        HMODULE module = RevalidatePerformanceProviderModule(*capturedModule);
        if (!module)
        {
            AppendPerformanceWarning(
                warnings,
                adapter.name,
                "provider-module-changed",
                "captured provider module is no longer the same loaded image");
            continue;
        }

        FARPROC exportAddress = GetProcAddress(
            module,
            adapter.providerExport.c_str());
        if (!exportAddress)
        {
            FreeLibrary(module);
            AppendPerformanceWarning(
                warnings,
                adapter.name,
                "provider-export-missing",
                "provider export '" + adapter.providerExport + "' is missing");
            continue;
        }

        BridgePerformanceProviderV1::Query query{};
        query.size = sizeof(query);
        query.apiVersion = BridgePerformanceProviderV1::ApiVersion;
        query.captureId = metadata.captureId;
        query.elapsedMs = metadata.elapsedMs;
        query.frameCount = metadata.frameCount;
        query.fps = metadata.fps;
        const auto function = reinterpret_cast<
            BridgePerformanceProviderV1::QueryFunction>(exportAddress);
        const BridgePerformanceProviderV1::CallResult result =
            BridgePerformanceProviderV1::Invoke(
                function,
                query,
                runtime.providerSlowWarningUs,
                health);
        FreeLibrary(module);
        if (result.status != BridgePerformanceProviderV1::CallStatus::Accepted)
        {
            std::string detail = ProviderCallStatusName(result.status);
            if (result.validation.code !=
                BridgePerformanceProviderV1::ValidationCode::Valid)
            {
                detail += ": ";
                detail += BridgePerformanceProviderV1::ValidationCodeName(
                    result.validation.code);
            }
            AppendPerformanceWarning(
                warnings,
                adapter.name,
                "provider-quarantined",
                detail);
            continue;
        }

        for (std::uint32_t index = 0;
             index < result.snapshot.metricCount;
             ++index)
        {
            const auto& metric = result.snapshot.metrics[index];
            std::size_t length = 0;
            while (length < BridgePerformanceProviderV1::MetricNameCapacity &&
                   metric.name[length] != '\0')
            {
                ++length;
            }
            metrics.push_back({
                adapter.name,
                std::string(metric.name, length),
                metric.value});
        }
    }
}

static std::expected<void, std::string> SaveD3D9BackBufferBmp(
    IDirect3DDevice9* device,
    const char* outputPath)
{
    if (!device || !outputPath || !outputPath[0]) {
        return std::unexpected("invalid arguments");
    }

    IDirect3DSurface9* backBuffer = nullptr;
    HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr) || !backBuffer) {
        return std::unexpected(
            std::format("GetBackBuffer hr=0x{:08X}", static_cast<unsigned>(hr)));
    }

    D3DSURFACE_DESC backBufferDesc{};
    hr = backBuffer->GetDesc(&backBufferDesc);
    if (FAILED(hr)) {
        backBuffer->Release();
        return std::unexpected(
            std::format("GetDesc hr=0x{:08X}", static_cast<unsigned>(hr)));
    }

    IDirect3DSurface9* readableSurface = nullptr;
    D3DFORMAT readableFormat = backBufferDesc.Format;
    hr = device->CreateOffscreenPlainSurface(
        backBufferDesc.Width,
        backBufferDesc.Height,
        readableFormat,
        D3DPOOL_SYSTEMMEM,
        &readableSurface,
        nullptr);
    if (SUCCEEDED(hr) && readableSurface) {
        hr = device->GetRenderTargetData(backBuffer, readableSurface);
    }

    IDirect3DSurface9* resolvedSurface = nullptr;
    if (FAILED(hr) || !readableSurface) {
        if (readableSurface) {
            readableSurface->Release();
            readableSurface = nullptr;
        }

        readableFormat = D3DFMT_A8R8G8B8;
        hr = device->CreateRenderTarget(
            backBufferDesc.Width,
            backBufferDesc.Height,
            readableFormat,
            D3DMULTISAMPLE_NONE,
            0,
            FALSE,
            &resolvedSurface,
            nullptr);
        if (SUCCEEDED(hr) && resolvedSurface) {
            hr = device->StretchRect(backBuffer, nullptr, resolvedSurface, nullptr, D3DTEXF_NONE);
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreateOffscreenPlainSurface(
                backBufferDesc.Width,
                backBufferDesc.Height,
                readableFormat,
                D3DPOOL_SYSTEMMEM,
                &readableSurface,
                nullptr);
        }
        if (SUCCEEDED(hr) && readableSurface) {
            hr = device->GetRenderTargetData(resolvedSurface, readableSurface);
        }
    }

    backBuffer->Release();
    if (resolvedSurface) resolvedSurface->Release();

    if (FAILED(hr) || !readableSurface) {
        if (readableSurface) readableSurface->Release();
        return std::unexpected(
            std::format("readback hr=0x{:08X}", static_cast<unsigned>(hr)));
    }

    if (readableFormat != D3DFMT_A8R8G8B8 && readableFormat != D3DFMT_X8R8G8B8) {
        readableSurface->Release();
        return std::unexpected(
            std::format("unsupported format=0x{:08X}", static_cast<unsigned>(readableFormat)));
    }

    D3DLOCKED_RECT locked{};
    hr = readableSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        readableSurface->Release();
        return std::unexpected(
            std::format("LockRect hr=0x{:08X}", static_cast<unsigned>(hr)));
    }

    const uint64_t rowBytes = static_cast<uint64_t>(backBufferDesc.Width) * 4;
    const uint64_t imageBytes = rowBytes * static_cast<uint64_t>(backBufferDesc.Height);
    if (imageBytes > UINT32_MAX) {
        readableSurface->UnlockRect();
        readableSurface->Release();
        return std::unexpected("image too large");
    }

    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(imageBytes);
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = static_cast<LONG>(backBufferDesc.Width);
    infoHeader.biHeight = -static_cast<LONG>(backBufferDesc.Height);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = static_cast<DWORD>(imageBytes);

    FILE* file = nullptr;
    if (fopen_s(&file, outputPath, "wb") != 0 || !file) {
        readableSurface->UnlockRect();
        readableSurface->Release();
        return std::unexpected("failed to open output");
    }

    bool success =
        fwrite(&fileHeader, sizeof(fileHeader), 1, file) == 1 &&
        fwrite(&infoHeader, sizeof(infoHeader), 1, file) == 1;
    const uint8_t* row = static_cast<const uint8_t*>(locked.pBits);
    for (UINT y = 0; success && y < backBufferDesc.Height; ++y) {
        success = fwrite(row, 1, static_cast<size_t>(rowBytes), file) == rowBytes;
        row += locked.Pitch;
    }

    fflush(file);
    fclose(file);
    readableSurface->UnlockRect();
    readableSurface->Release();

    if (!success) {
        return std::unexpected("failed while writing output");
    }
    return {};
}

// i9-14900HX: logical CPUs 0-15 are P-cores, 16-31 are E-cores. Sampling and
// reapply workers are compute-bound and must never compete with the render
// thread for P-core fetch/cache bandwidth.
constexpr DWORD_PTR kEfficiencyCoreMask = 0xFFFF0000ull;

static DWORD WINAPI CpuHotspotWorker(void* parameter)
{
    renderstack::scheduling::ThreadSchedulingScope scheduling(
        g_threadSchedulingOptions, renderstack::scheduling::Role::Background,
        &ThreadSchedulingLog);
    if (!g_threadSchedulingOptions.enabled) {
        // Preserve the existing worker affinity when the M1 policy is disabled.
        SetThreadAffinityMask(GetCurrentThread(), kEfficiencyCoreMask);
    }
    CpuHotspotWorkerContext* context = static_cast<CpuHotspotWorkerContext*>(parameter);
    if (!context) {
        g_cpuHotspotActive.store(0);
        return 0;
    }

    HANDLE targetThread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE,
        context->targetThreadId);
    if (!targetThread) {
        Log("cpuhotspots: capture={} OpenThread failed targetThread={} err={}",
            context->captureId, context->targetThreadId, GetLastError());
        delete context;
        g_cpuHotspotActive.store(0);
        return 0;
    }

    std::unordered_map<uintptr_t, uint64_t> addressSamples;
    addressSamples.reserve(4096);
    std::unordered_map<CpuHotspotStackKey, uint64_t, CpuHotspotStackKeyHash> stackSamples;
    stackSamples.reserve(4096);
    std::vector<CpuExecutableRange> executableRanges;
    std::vector<BridgePerformance::ModuleIdentity> moduleSnapshot;
    CollectCpuExecutableRanges(executableRanges, moduleSnapshot);
    uint64_t missedSamples = 0;
    ULONG64 startCycles = 0;
    ULONG64 endCycles = 0;
    QueryThreadCycleTime(targetThread, &startCycles);
    uint32_t gameFrameStart = 0;
    const bool gameFrameStartReadable = ReadGtasaFrameCounter(gameFrameStart);

    SYSTEMTIME beginTime{};
    GetLocalTime(&beginTime);
    const DWORD startTick = GetTickCount();
    while (GetTickCount() - startTick < context->durationMs) {
        Sleep(context->intervalMs);

        const DWORD suspendResult = SuspendThread(targetThread);
        if (suspendResult == static_cast<DWORD>(-1)) {
            ++missedSamples;
            continue;
        }

        CONTEXT threadContext{};
        threadContext.ContextFlags = CONTEXT_CONTROL;
        const BOOL gotContext = GetThreadContext(targetThread, &threadContext);
        uintptr_t stackWords[64]{};
        UINT stackWordCount = 0;
#if defined(_M_IX86)
        if (gotContext) {
            stackWordCount = CopyCpuStackWords(
                static_cast<uintptr_t>(threadContext.Esp),
                stackWords);
        }
#elif defined(_M_X64)
        if (gotContext) {
            stackWordCount = CopyCpuStackWords(
                static_cast<uintptr_t>(threadContext.Rsp),
                stackWords);
        }
#endif
        ResumeThread(targetThread);

        if (!gotContext) {
            ++missedSamples;
            continue;
        }

#if defined(_M_IX86)
        const uintptr_t instructionPointer = static_cast<uintptr_t>(threadContext.Eip);
#elif defined(_M_X64)
        const uintptr_t instructionPointer = static_cast<uintptr_t>(threadContext.Rip);
#else
        const uintptr_t instructionPointer = 0;
#endif
        if (instructionPointer) {
            ++addressSamples[instructionPointer];

            CpuHotspotStackKey stackKey{};
            stackKey.instructionPointer = instructionPointer;
            uintptr_t* callers[] = {
                &stackKey.caller1,
                &stackKey.caller2,
                &stackKey.caller3,
            };
            UINT callerCount = 0;
            for (UINT i = 0; i < stackWordCount && callerCount < 3; ++i) {
                const uintptr_t candidate = stackWords[i];
                if (!IsCpuExecutableAddress(executableRanges, candidate)) continue;
                if (candidate == instructionPointer ||
                    (callerCount > 0 && candidate == *callers[callerCount - 1])) {
                    continue;
                }
                *callers[callerCount++] = candidate;
            }
            ++stackSamples[stackKey];
        } else {
            ++missedSamples;
        }
    }

    const DWORD elapsedMs = GetTickCount() - startTick;
    QueryThreadCycleTime(targetThread, &endCycles);
    CloseHandle(targetThread);
    uint32_t gameFrameEnd = 0;
    const bool gameFrameEndReadable = ReadGtasaFrameCounter(gameFrameEnd);
    const bool gameFrameCounterValid = gameFrameStartReadable && gameFrameEndReadable;
    const uint32_t capturedFrames = gameFrameCounterValid
        ? static_cast<uint32_t>(gameFrameEnd - gameFrameStart)
        : 0;
    const LONG capturedPresents = g_cpuHotspotPresents.load();
    const double capturedFps = elapsedMs && gameFrameCounterValid
        ? 1000.0 * static_cast<double>(capturedFrames) / static_cast<double>(elapsedMs)
        : 0.0;
    const double capturedPresentRate = elapsedMs
        ? 1000.0 * static_cast<double>(capturedPresents) / static_cast<double>(elapsedMs)
        : 0.0;

    uint64_t totalSamples = 0;
    std::vector<std::pair<uintptr_t, uint64_t>> orderedAddresses;
    orderedAddresses.reserve(addressSamples.size());
    for (const auto& entry : addressSamples) {
        totalSamples += entry.second;
        orderedAddresses.push_back(entry);
    }
    std::sort(orderedAddresses.begin(), orderedAddresses.end(), [](const auto& left, const auto& right) {
        return left.second > right.second;
    });

    std::vector<std::pair<CpuHotspotStackKey, uint64_t>> orderedStacks;
    orderedStacks.reserve(stackSamples.size());
    for (const auto& entry : stackSamples) orderedStacks.push_back(entry);
    std::sort(orderedStacks.begin(), orderedStacks.end(), [](const auto& left, const auto& right) {
        return left.second > right.second;
    });

    EnsurePerformanceConfigLoaded();
    BridgePerformance::CaptureMetadata performanceMetadata{};
    performanceMetadata.captureId = context->captureId;
    performanceMetadata.elapsedMs = elapsedMs;
    performanceMetadata.frameCount = capturedFrames;
    performanceMetadata.fps = capturedFps;
    BridgePerformance::CaptureSamples performanceCapture;
    BridgePerformance::AttributionResult performanceAttribution;
    BridgePerformance::ProcessMemoryEvidence performanceMemory;
    std::vector<BridgePerformance::AdapterMetricRow> performanceMetrics;
    std::vector<BridgePerformance::AdapterConfigRow> performanceConfigs;
    std::vector<BridgePerformance::AdapterWarning> performanceWarnings;
    if (g_performanceRuntimeConfig.enabled)
    {
        performanceCapture = BuildPerformanceCaptureSamples(
            orderedAddresses,
            orderedStacks,
            moduleSnapshot,
            totalSamples);
        performanceAttribution = BridgePerformance::AttributeCapture(
            g_performanceRuntimeConfig.registry,
            performanceCapture);
        performanceWarnings = g_performanceRuntimeConfig.warnings;
        for (const BridgePerformance::AdapterWarning& warning :
             performanceAttribution.warnings)
        {
            AppendPerformanceWarning(
                performanceWarnings,
                warning.adapter,
                warning.code.c_str(),
                warning.detail);
        }
        if (g_performanceRuntimeConfig.includeProcessMemory)
        {
            performanceMemory = BridgePerformance::CollectProcessMemory();
        }
        CollectPerformanceConfigRows(
            g_performanceRuntimeConfig,
            performanceConfigs,
            performanceWarnings);
        CollectPerformanceProviderRows(
            g_performanceRuntimeConfig,
            performanceMetadata,
            moduleSnapshot,
            performanceMetrics,
            performanceWarnings);
    }

    std::unordered_map<uintptr_t, uint64_t> moduleSamples;
    for (const auto& entry : orderedAddresses) {
        const ResolvedCpuAddress resolved = ResolveCpuHotspotAddress(entry.first);
        moduleSamples[resolved.moduleBase] += entry.second;
    }

    std::vector<std::pair<uintptr_t, uint64_t>> orderedModules;
    orderedModules.reserve(moduleSamples.size());
    for (const auto& entry : moduleSamples) orderedModules.push_back(entry);
    std::sort(orderedModules.begin(), orderedModules.end(), [](const auto& left, const auto& right) {
        return left.second > right.second;
    });

    FILE* file = nullptr;
    if (fopen_s(&file, context->outputPath, "a") == 0 && file) {
        std::print(file,
            "# capture={} begin={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} targetThread={} durationMs={} intervalMs={} elapsedMs={} frames={} fps={:.2f} presents={} presentRate={:.2f} gameFrameStart={} gameFrameEnd={} frameCounterValid={} samples={} missed={} threadCycles={} uniqueAddresses={} uniqueStacks={} executableRanges={} screenshot=\"{}\"\n",
            context->captureId,
            beginTime.wYear, beginTime.wMonth, beginTime.wDay,
            beginTime.wHour, beginTime.wMinute, beginTime.wSecond, beginTime.wMilliseconds,
            context->targetThreadId,
            context->durationMs,
            context->intervalMs,
            elapsedMs,
            static_cast<unsigned>(capturedFrames),
            capturedFps,
            static_cast<unsigned>(capturedPresents),
            capturedPresentRate,
            static_cast<unsigned>(gameFrameStart),
            static_cast<unsigned>(gameFrameEnd),
            gameFrameCounterValid ? 1 : 0,
            static_cast<unsigned long long>(totalSamples),
            static_cast<unsigned long long>(missedSamples),
            static_cast<unsigned long long>(endCycles >= startCycles ? endCycles - startCycles : 0),
            static_cast<unsigned>(orderedAddresses.size()),
            static_cast<unsigned>(orderedStacks.size()),
            static_cast<unsigned>(executableRanges.size()),
            context->screenshotPath);

        UINT moduleRank = 0;
        for (const auto& entry : orderedModules) {
            const ResolvedCpuAddress resolved = ResolveCpuHotspotAddress(entry.first);
            const double percent = totalSamples
                ? 100.0 * static_cast<double>(entry.second) / static_cast<double>(totalSamples)
                : 0.0;
            std::print(file,
                "moduleRank={} samples={} percent={:.2f} module={} base={:08X}\n",
                ++moduleRank,
                static_cast<unsigned long long>(entry.second),
                percent,
                resolved.moduleName, reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(entry.first)));
        }

        const size_t outputStackCount = (std::min)(orderedStacks.size(), static_cast<size_t>(256));
        for (size_t i = 0; i < outputStackCount; ++i) {
            const auto& entry = orderedStacks[i];
            char instructionPointer[2 * MAX_PATH]{};
            char caller1[2 * MAX_PATH]{};
            char caller2[2 * MAX_PATH]{};
            char caller3[2 * MAX_PATH]{};
            FormatCpuHotspotAddressBrief(
                entry.first.instructionPointer, instructionPointer, sizeof(instructionPointer));
            FormatCpuHotspotAddressBrief(entry.first.caller1, caller1, sizeof(caller1));
            FormatCpuHotspotAddressBrief(entry.first.caller2, caller2, sizeof(caller2));
            FormatCpuHotspotAddressBrief(entry.first.caller3, caller3, sizeof(caller3));
            const double percent = totalSamples
                ? 100.0 * static_cast<double>(entry.second) / static_cast<double>(totalSamples)
                : 0.0;
            std::print(file,
                "stackRank={} samples={} percent={:.2f} ip={} caller1={} caller2={} caller3={}\n",
                static_cast<unsigned>(i + 1),
                static_cast<unsigned long long>(entry.second),
                percent,
                instructionPointer,
                caller1,
                caller2,
                caller3);
        }

        const size_t outputAddressCount = (std::min)(orderedAddresses.size(), static_cast<size_t>(256));
        for (size_t i = 0; i < outputAddressCount; ++i) {
            const auto& entry = orderedAddresses[i];
            const ResolvedCpuAddress resolved = ResolveCpuHotspotAddress(entry.first);
            const double percent = totalSamples
                ? 100.0 * static_cast<double>(entry.second) / static_cast<double>(totalSamples)
                : 0.0;
            std::print(file,
                "rank={} samples={} percent={:.2f} address={:08X} module={} base={:08X} rva=0x{:08X} preferred=0x{:08X}\n",
                static_cast<unsigned>(i + 1),
                static_cast<unsigned long long>(entry.second),
                percent, reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(entry.first)),
                resolved.moduleName, reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(resolved.moduleBase)),
                static_cast<unsigned>(resolved.rva),
                static_cast<unsigned>(resolved.preferredAddress));
        }
        if (g_performanceRuntimeConfig.enabled)
        {
            BridgePerformance::AppendAdapterReport(
                file,
                performanceMetadata,
                performanceAttribution,
                performanceMemory,
                performanceMetrics,
                performanceConfigs,
                performanceWarnings);
        }
        std::print(file, "# capture={} end\n", context->captureId);
        fflush(file);
        fclose(file);
    } else {
        Log("cpuhotspots: capture={} failed to open {}",
            context->captureId, context->outputPath);
    }

    Log("cpuhotspots: capture={} complete targetThread={} frames={} fps={:.2f} presents={} presentRate={:.2f} frameCounterValid={} samples={} missed={} elapsedMs={} screenshot={} path={}",
        context->captureId,
        context->targetThreadId,
        static_cast<unsigned>(capturedFrames),
        capturedFps,
        static_cast<unsigned>(capturedPresents),
        capturedPresentRate,
        gameFrameCounterValid ? 1 : 0,
        static_cast<unsigned long long>(totalSamples),
        static_cast<unsigned long long>(missedSamples),
        elapsedMs,
        context->screenshotPath,
        context->outputPath);

    const UINT completedCaptureId = context->captureId;
    delete context;
    g_cpuHotspotActive.store(0);
    if (g_enableD3D9CallsiteProfile && g_cpuHotspotChainD3D9CallsiteProfile) {
        g_cpuHotspotCallsitePending.store(static_cast<LONG>(completedCaptureId));
        Log("cpuhotspots: capture={} queued sequential D3D9 callsite stage",
            completedCaptureId);
    }
    return 0;
}

// Set 1 via [ProxyChain] ForceDeviceWrap to restore the legacy full device wrap
// (needed only for wrapper-based diagnostics such as F9/F10 or the old present
// driver). The default keeps the game on the raw backend device.
static bool g_forceDeviceWrap = false;

static bool ShouldWrapD3D9Device()
{
    // Only diagnostics that intercept individual D3D9 calls require the
    // per-call device wrapper. Effect optimization, the F7/F8 profilers and
    // the CPU hotspot sampler run without it: hook installation moved to the
    // unwrapped support worker below, and F8 arming polls from that worker.
    return kBackendTraceBuild || g_forceDeviceWrap || g_enablePostFxHost ||
        !g_plugins.empty() || g_enableD3D9Stats || g_enableD3D9Trace ||
        g_enableD3D9CallsiteProfile || g_enableD3D9Optimizer;
}

// ---- Unwrapped-device support ----------------------------------------------
// With the per-call device wrapper removed, three responsibilities move here:
//  1. ProperShaders hook installation (previously retried from the wrapped
//     Present every frame) now runs on a worker thread. Code patches use a
//     suspend + EIP-outside-module check so the render thread is never paused
//     inside the bytes being rewritten.
//  2. F8 CPU hotspot arming (previously polled in the wrapped Present).
//     The backbuffer screenshot is skipped in this mode: this thread must not
//     call into a device the render thread is using concurrently.
//  3. Vulkan host attach and render-thread id capture happen at CreateDevice.
static IDirect3DDevice9* g_unwrappedDevice = nullptr;
static VulkanHostDevice* g_unwrappedVulkanHost = nullptr;
static DWORD g_unwrappedRenderThreadId = 0;
static std::atomic<LONG> g_unwrappedDeviceClaimed{ 0 };
static std::atomic<LONG> g_unwrappedWorkerStarted{ 0 };
static bool g_unwrappedHotspotTriggerWasDown = false;
static bool g_unwrappedStateAttributionTriggerWasDown = false;

static bool ThreadOutsideModule(HANDLE thread, HMODULE module)
{
    uintptr_t base = 0;
    uintptr_t size = 0;
    uintptr_t preferred = 0;
    if (!GetPeImageInfo(module, base, size, preferred) || !size) return false;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &context)) return false;
    const uintptr_t ip = context.Eip;
    return ip < base || ip >= base + size;
}

static void MaybeArmCpuHotspotProfileUnwrapped()
{
    if (!g_enableCpuHotspotProfile) return;

    const bool triggerDown = (GetAsyncKeyState(g_cpuHotspotTriggerKey) & 0x8000) != 0;
    if (!triggerDown) {
        g_unwrappedHotspotTriggerWasDown = false;
        return;
    }
    if (g_unwrappedHotspotTriggerWasDown) return;
    g_unwrappedHotspotTriggerWasDown = true;
    Log("cpuhotspots: trigger observed on unwrapped support worker key={}",
        g_cpuHotspotTriggerKey);

    if (g_cpuHotspotCallsitePending.load() != 0) {
        Log("cpuhotspots: trigger ignored because a D3D9 callsite stage is pending");
        return;
    }
    if (g_cpuHotspotActive.exchange(1) != 0) {
        Log("cpuhotspots: trigger ignored because a capture is already active");
        return;
    }
    g_cpuHotspotPresents.store(0);

    CpuHotspotWorkerContext* context = new (std::nothrow) CpuHotspotWorkerContext();
    if (!context) {
        Log("cpuhotspots: failed to allocate worker context");
        g_cpuHotspotActive.store(0);
        return;
    }

    context->targetThreadId = g_unwrappedRenderThreadId
        ? g_unwrappedRenderThreadId
        : GetCurrentThreadId();
    context->captureId = static_cast<UINT>(
        g_cpuHotspotCaptureId.fetch_add(1) + 1);
    context->durationMs = g_cpuHotspotDurationMs;
    context->intervalMs = g_cpuHotspotIntervalMs;
    FormatTo(context->outputPath, sizeof(context->outputPath),
        "{}\\scripts\\BridgeD3D9.cpuhotspots.log", g_gameDir);
    context->screenshotPath[0] = '\0';

    const UINT captureId = context->captureId;
    const DWORD targetThreadId = context->targetThreadId;
    const DWORD durationMs = context->durationMs;
    const DWORD intervalMs = context->intervalMs;

    HANDLE worker = CreateThread(nullptr, 0, CpuHotspotWorker, context, 0, nullptr);
    if (!worker) {
        Log("cpuhotspots: capture={} CreateThread failed err={}",
            captureId, GetLastError());
        delete context;
        g_cpuHotspotActive.store(0);
        return;
    }
    CloseHandle(worker);

    Log("cpuhotspots: capture={} armed targetThread={} durationMs={} intervalMs={} screenshotSaved=0 screenshot=(skipped-unwrapped)",
        captureId, targetThreadId, durationMs, intervalMs);
}

static DWORD WINAPI UnwrappedSupportThread(LPVOID)
{
    renderstack::scheduling::ThreadSchedulingScope scheduling(
        g_threadSchedulingOptions, renderstack::scheduling::Role::Background,
        &ThreadSchedulingLog);
    if (!g_threadSchedulingOptions.enabled) {
        // Preserve the existing worker affinity when the M1 policy is disabled.
        SetThreadAffinityMask(GetCurrentThread(), kEfficiencyCoreMask);
    }
    Log("postfx: unwrapped support worker started renderThread={}",
        g_unwrappedRenderThreadId);
    const ULONGLONG installDeadline = GetTickCount64() + 180000ull;
    bool installDone = !g_enableProperShadersEffectOptimization;
    for (;;) {
        if (!installDone) {
            HMODULE module = GetModuleHandleA("ProperShaders.asi");
            if (!module) module = GetModuleHandleA("propershaders.asi");
            if (module) {
                if (g_properShadersGeneralStateJournal && !g_properShadersCreateEffectIatSlot) {
                    // Single aligned pointer store; safe without suspending.
                    InstallProperShadersCreateEffectHook(module);
                }
                if (!g_properShadersOptimizationHooksInstalled) {
                    HANDLE renderThread = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, g_unwrappedRenderThreadId);
                    if (renderThread) {
                        if (SuspendThread(renderThread) != static_cast<DWORD>(-1)) {
                            if (ThreadOutsideModule(renderThread, module)) {
                                InstallProperShadersOptimizationPatches(module);
                            }
                            ResumeThread(renderThread);
                        }
                        CloseHandle(renderThread);
                    }
                }
                const bool iatReady = !g_properShadersGeneralStateJournal ||
                    g_properShadersCreateEffectIatSlot;
                if (iatReady && g_properShadersOptimizationHooksInstalled) {
                    installDone = true;
                    g_properShadersNoSaveStateActive.store(true, std::memory_order_release);
                    Log("effectopt: incremental state journal active; installed from unwrapped worker");
                }
            }
            if (!installDone && GetTickCount64() >= installDeadline) {
                installDone = true;
                Log("effectopt: unwrapped install timed out after 180s; optimization inactive this session");
            }
        }
        MaybeArmCpuHotspotProfileUnwrapped();
        PollProperShadersStateAttribution(
            g_unwrappedStateAttributionTriggerWasDown, false);
        // The worker owns the diagnostic hotkeys in unwrapped mode. Keep its
        // polling interval short only while a diagnostic is enabled so normal
        // gameplay does not acquire a tighter wake-up loop.
        const DWORD pollIntervalMs =
            (g_enableCpuHotspotProfile || g_enableProperShadersStateAttribution)
            ? 8u
            : (installDone ? 30u : 100u);
        Sleep(pollIntervalMs);
    }
    return 0;
}

static void OnUnwrappedDeviceCreated(IDirect3DDevice9* device)
{
    // Only the first device is the game's render device. D3DX9 helper devices
    // created later must not steal the render-thread id or the Vulkan host.
    if (g_unwrappedDeviceClaimed.exchange(1) != 0) {
        Log("postfx: additional device left unwrapped inner=0x{:08X} (not claimed)", reinterpret_cast<std::uintptr_t>(device));
        return;
    }
    g_unwrappedDevice = device;
    g_unwrappedRenderThreadId = GetCurrentThreadId();
    g_unwrappedVulkanHost = AttachVulkanHost(device);
    if (g_unwrappedWorkerStarted.exchange(1) == 0) {
        HANDLE thread = CreateThread(nullptr, 0, UnwrappedSupportThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            g_unwrappedWorkerStarted.store(0);
            Log("postfx: unwrapped support worker CreateThread failed err={}", GetLastError());
        }
    }
    Log("postfx: device left unwrapped inner=0x{:08X} renderThread={} (per-call wrapper disabled)", reinterpret_cast<std::uintptr_t>(device), g_unwrappedRenderThreadId);
}


static std::string TrimCopy(const std::string& text)
{
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }

    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(begin, end - begin);
}

static bool EqualsNoCase(const std::string& a, const char* b)
{
    return _stricmp(a.c_str(), b ? b : "") == 0;
}

static bool LoadTextLines(const char* path, std::vector<std::string>& lines)
{
    lines.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) return false;

    std::string line;
    while (std::getline(input, line)) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return true;
}

static bool SaveTextLines(const char* path, const std::vector<std::string>& lines)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    for (size_t i = 0; i < lines.size(); ++i) {
        fwrite(lines[i].c_str(), 1, lines[i].size(), f);
        fwrite("\r\n", 1, 2, f);
    }

    fclose(f);
    return true;
}

static bool IsIniSectionLine(const std::string& trimmed, const char* section)
{
    if (trimmed.size() < 3 || trimmed.front() != '[' || trimmed.back() != ']') {
        return false;
    }
    std::string name = trimmed.substr(1, trimmed.size() - 2);
    return EqualsNoCase(TrimCopy(name), section);
}

static bool IniLineMatchesKey(const std::string& line, const char* key)
{
    std::string text = TrimCopy(line);
    if (text.empty()) return false;

    if (text[0] == '#' || text[0] == ';') {
        text = TrimCopy(text.substr(1));
    }

    size_t eq = text.find('=');
    if (eq == std::string::npos) return false;

    std::string lineKey = TrimCopy(text.substr(0, eq));
    return EqualsNoCase(lineKey, key);
}

static std::string BuildIniLine(const char* key, const char* value, bool enabled)
{
    return std::format(
        "{}{} = {}",
        enabled ? "" : "#",
        key ? key : "",
        value ? value : "");
}

static bool SetIniKey(std::vector<std::string>& lines, const char* section,
    const char* key, const char* value, bool enabled)
{
    bool inSection = false;
    bool sectionFound = false;
    size_t sectionInsert = lines.size();
    std::string wanted = BuildIniLine(key, value, enabled);

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string trimmed = TrimCopy(lines[i]);
        if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
            if (IsIniSectionLine(trimmed, section)) {
                inSection = true;
                sectionFound = true;
                sectionInsert = i + 1;
                continue;
            }
            if (inSection) break;
            continue;
        }

        if (!inSection) continue;
        sectionInsert = i + 1;
        if (IniLineMatchesKey(lines[i], key)) {
            if (lines[i] == wanted) return false;
            lines[i] = wanted;
            return true;
        }
    }

    if (!enabled) return false;

    if (!sectionFound) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.push_back("");
        }
        lines.push_back(std::format("[{}]", section));
        lines.push_back(wanted);
        return true;
    }

    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionInsert), wanted);
    return true;
}

static bool PreviousRunHadOlaCrash()
{
    const std::string logPath = std::format(
        "{}\\fastman92limitAdjuster.log",
        g_gameDir);

    std::vector<std::string> lines;
    if (!LoadTextLines(logPath.c_str(), lines)) {
        return false;
    }

    bool sawCrash = false;
    bool sawOlaModule = false;
    for (const auto& line : lines) {
        if (line.find("Game has crashed") != std::string::npos ||
            line.find("EXCEPTION_ACCESS_VIOLATION") != std::string::npos) {
            sawCrash = true;
        }
        if (line.find("iii.vc.sa.limitadjuster.asi") != std::string::npos ||
            line.find("III.VC.SA.LimitAdjuster.asi") != std::string::npos) {
            sawOlaModule = true;
        }
    }

    return sawCrash && sawOlaModule;
}

struct LimitCoordinatorRule
{
    const char* name;
    const char* flaSection;
    const char* flaKey;
    const char* flaValue;
    const char* olaKey;
    const char* olaValue;
    bool preferOla;
    const char* reason;
};

static void ApplyLimitCoordinator(const char* bridgeIniPath)
{
    if (GetPrivateProfileIntA("LimitCoordinator", "Enable", 0, bridgeIniPath) == 0) {
        Log("limitcoord: disabled");
        return;
    }

    char mode[32]{};
    GetPrivateProfileStringA("LimitCoordinator", "Mode", "PreferFLA", mode, sizeof(mode), bridgeIniPath);
    bool preferOlaMode = _stricmp(mode, "PreferOLA") == 0 || _stricmp(mode, "Auto") == 0;
    bool crashGuard = GetPrivateProfileIntA("LimitCoordinator", "CrashGuard", 1, bridgeIniPath) != 0;
    bool quarantineAfterCrash = GetPrivateProfileIntA("LimitCoordinator", "QuarantineOlaAfterCrash", 1, bridgeIniPath) != 0;
    bool previousOlaCrash = crashGuard && quarantineAfterCrash && PreviousRunHadOlaCrash();
    if (previousOlaCrash) {
        preferOlaMode = false;
    }

    char flaPath[MAX_PATH]{};
    char olaPath[MAX_PATH]{};
    FormatTo(flaPath, sizeof(flaPath), "{}\\fastman92limitAdjuster_GTASA.ini", g_gameDir);
    FormatTo(olaPath, sizeof(olaPath), "{}\\modloader\\OLA\\III.VC.SA.LimitAdjuster.ini", g_gameDir);

    std::vector<std::string> flaLines;
    std::vector<std::string> olaLines;
    if (!LoadTextLines(flaPath, flaLines)) {
        Log("limitcoord: failed to read FLA ini {}", flaPath);
        return;
    }
    if (!LoadTextLines(olaPath, olaLines)) {
        Log("limitcoord: failed to read OLA ini {}", olaPath);
        return;
    }

    const LimitCoordinatorRule rules[] = {
        { "PtrNodeSingle", "DYNAMIC LIMITS", "PtrNode Singles", "300000", "PtrNodeSingle", "unlimited", true, "OLA unlimited pointer pool" },
        { "PtrNodeDouble", "DYNAMIC LIMITS", "PtrNode Doubles", "150000", "PtrNodeDouble", "unlimited", true, "OLA unlimited pointer pool" },
        { "EntryInfoNode", "DYNAMIC LIMITS", "EntryInfoNodes", "150000", "EntryInfoNode", "unlimited", true, "OLA unlimited entry pool" },
        { "Task", "DYNAMIC LIMITS", "Tasks", "3000", "Task", "unlimited", true, "OLA unlimited task pool" },
        { "Event", "DYNAMIC LIMITS", "Events", "1000", "Event", "unlimited", true, "OLA unlimited event pool" },
        { "PointRoute", "DYNAMIC LIMITS", "PointRoute", "256", "PointRoute", "unlimited", true, "OLA unlimited route pool" },
        { "PatrolRoute", "DYNAMIC LIMITS", "PatrolRoute", "256", "PatrolRoute", "unlimited", true, "OLA unlimited route pool" },
        { "NodeRoute", "DYNAMIC LIMITS", "NodeRoute", "512", "NodeRoute", "unlimited", true, "OLA unlimited route pool" },
        { "TaskAllocator", "DYNAMIC LIMITS", "TaskAllocator", "128", "TaskAllocator", "unlimited", true, "OLA unlimited allocator pool" },
        { "PedAttractors", "DYNAMIC LIMITS", "PedAttractors", "300", "PedAttractors", "unlimited", true, "OLA unlimited attractor pool" },
        { "VehicleStructs", "DYNAMIC LIMITS", "VehicleStructs", "256", "VehicleStructs", "unlimited", true, "OLA unlimited vehicle model structs" },
        { "VisibleEntityPtrs", "RENDERER LIMITS", "Visible entity pointers", "10000", "VisibleEntityPtrs", "unlimited", true, "OLA unlimited visible entity list" },
        { "VisibleLodPtrs", "RENDERER LIMITS", "Visible LOD pointers", "10000", "VisibleLodPtrs", "unlimited", true, "OLA unlimited visible LOD list" },
        { "AlphaEntityList", "VISIBILITY LIMITS", "Alpha entity list limit", "2000", "AlphaEntityList", "unlimited", true, "OLA unlimited alpha list" },
        { "Coronas", "OTHER LIMITS", "Coronas", "8192", "Coronas", "20000", true, "OLA higher corona limit" },
        { "ColModel", "DYNAMIC LIMITS", "ColModels", "30000", "ColModel", "unlimited", false, "Crash guard: OLA crashed while loading COL3/lodveg_tree7vbig" },
        { "MatrixList", "DYNAMIC LIMITS", "Matrices", "150000", "MatrixList", "unlimited", false, "FLA matrix pool is already high and avoids extra OLA hooks" },
        { "StreamingObjectInstancesList", "DYNAMIC LIMITS", "rwObjectInstances", "20000", "StreamingObjectInstancesList", "30000", false, "FLA rwObjectInstances is stable with FLA ID/streaming patches" },
    };

    bool flaChanged = false;
    bool olaChanged = false;
    Log("limitcoord: enabled mode={} crashGuard={} quarantineAfterCrash={} previousOlaCrash={}",
        mode, crashGuard ? 1 : 0, quarantineAfterCrash ? 1 : 0, previousOlaCrash ? 1 : 0);
    if (previousOlaCrash) {
        Log("limitcoord: OLA SA limits quarantined because previous run crashed inside iii.vc.sa.limitadjuster.asi");
    }

    for (const auto& rule : rules) {
        bool useOla = preferOlaMode && rule.preferOla;
        if (crashGuard && !rule.preferOla) {
            useOla = false;
        }

        flaChanged |= SetIniKey(flaLines, rule.flaSection, rule.flaKey, rule.flaValue, !useOla);
        olaChanged |= SetIniKey(olaLines, "SALIMITS", rule.olaKey, rule.olaValue, useOla);
        Log("limitcoord: {} owner={} reason={}", rule.name, useOla ? "OLA" : "FLA", rule.reason);
    }

    if (flaChanged) {
        if (SaveTextLines(flaPath, flaLines)) {
            Log("limitcoord: wrote FLA ini");
        } else {
            Log("limitcoord: failed to write FLA ini {}", flaPath);
        }
    } else {
        Log("limitcoord: FLA ini unchanged");
    }

    if (olaChanged) {
        if (SaveTextLines(olaPath, olaLines)) {
            Log("limitcoord: wrote OLA ini");
        } else {
            Log("limitcoord: failed to write OLA ini {}", olaPath);
        }
    } else {
        Log("limitcoord: OLA ini unchanged");
    }
}

static DWORD PriorityNameToClass(const char* name)
{
    if (!name || !name[0]) return NORMAL_PRIORITY_CLASS;
    constexpr std::array<std::pair<std::string_view, DWORD>, 7> kNames = {{
        {"Idle", IDLE_PRIORITY_CLASS},
        {"BelowNormal", BELOW_NORMAL_PRIORITY_CLASS},
        {"Normal", NORMAL_PRIORITY_CLASS},
        {"AboveNormal", ABOVE_NORMAL_PRIORITY_CLASS},
        {"High", HIGH_PRIORITY_CLASS},
        {"Realtime", REALTIME_PRIORITY_CLASS},
        {"RealTime", REALTIME_PRIORITY_CLASS},
    }};
    const auto match = std::ranges::find_if(
        kNames,
        [name](const auto& entry) {
            return _stricmp(name, entry.first.data()) == 0;
        });
    return match == kNames.end() ? NORMAL_PRIORITY_CLASS : match->second;
}

static const char* PriorityClassName(DWORD priorityClass)
{
    switch (priorityClass) {
    case IDLE_PRIORITY_CLASS: return "Idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "BelowNormal";
    case NORMAL_PRIORITY_CLASS: return "Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "AboveNormal";
    case HIGH_PRIORITY_CLASS: return "High";
    case REALTIME_PRIORITY_CLASS: return "Realtime";
    default: return "Unknown";
    }
}

static DWORD_PTR ParseAffinityMask(const char* text)
{
    if (!text || !text[0]) return 0;
    char* end = nullptr;
    unsigned long long value = _strtoui64(text, &end, 0);
    return static_cast<DWORD_PTR>(value);
}

static void ApplyAffinityAndPriority(const char* reason)
{
    if (!g_affinityEnable) return;

    HANDLE process = GetCurrentProcess();
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    DWORD_PTR targetMask = g_affinityRequestedMask;
    bool gotMasks = GetProcessAffinityMask(process, &processMask, &systemMask) != FALSE;
    if (gotMasks && systemMask) {
        targetMask &= systemMask;
    }

    // Apply the requested legacy process mask once, before thread selection.
    // Later priority reapplication must not overwrite the per-thread policy.
    const bool allowProcessAffinity = !g_threadSchedulingOptions.enabled ||
        (reason && _stricmp(reason, "startup") == 0);
    const bool affinityChanged = allowProcessAffinity && targetMask &&
        (!gotMasks || processMask != targetMask);
    BOOL affinityOk = TRUE;
    DWORD affinityErr = 0;
    if (affinityChanged) {
        affinityOk = SetProcessAffinityMask(process, targetMask);
        if (!affinityOk) affinityErr = GetLastError();
    }

    const DWORD currentPriorityClass = GetPriorityClass(process);
    const bool priorityChanged = !currentPriorityClass || currentPriorityClass != g_affinityPriorityClass;
    BOOL priorityOk = TRUE;
    if (priorityChanged) {
        priorityOk = SetPriorityClass(process, g_affinityPriorityClass);
    }
    DWORD priorityErr = priorityOk ? 0 : GetLastError();

    if (!affinityChanged && !priorityChanged && reason && _stricmp(reason, "startup") != 0) {
        return;
    }

    Log("affinity: {} enable=1 requested=0x{:08X} current=0x{:08X} target=0x{:08X} system=0x{:08X} affinityChanged={} applied={} err={} priorityCurrent=0x{:08X} priority={} class=0x{:08X} priorityChanged={} applied={} err={}",
        reason ? reason : "apply", reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(g_affinityRequestedMask)), reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(gotMasks ? processMask : 0)), reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(targetMask)), reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(gotMasks ? systemMask : 0)),
        affinityChanged ? 1 : 0,
        affinityOk ? 1 : 0,
        affinityErr,
        currentPriorityClass,
        PriorityClassName(g_affinityPriorityClass),
        g_affinityPriorityClass,
        priorityChanged ? 1 : 0,
        priorityOk ? 1 : 0,
        priorityErr);
}

static DWORD WINAPI AffinityReapplyThread(void*)
{
    renderstack::scheduling::ThreadSchedulingScope scheduling(
        g_threadSchedulingOptions, renderstack::scheduling::Role::Background,
        &ThreadSchedulingLog);
    if (!g_threadSchedulingOptions.enabled) {
        // Preserve the existing worker affinity when the M1 policy is disabled.
        SetThreadAffinityMask(GetCurrentThread(), kEfficiencyCoreMask);
    }
    Log("affinity: reapply worker started count={} intervalMs={}",
        g_affinityReapplyCount, g_affinityReapplyIntervalMs);
    for (DWORD i = 0; i < g_affinityReapplyCount; ++i) {
        Sleep(g_affinityReapplyIntervalMs);
        ApplyAffinityAndPriority("reapply");
    }
    Log("affinity: reapply worker finished");
    return 0;
}

static bool IsAbsolutePathA(const char* path)
{
    return path && (strchr(path, ':') || (path[0] == '\\' && path[1] == '\\'));
}

static void TrimSpaces(char* text)
{
    if (!text) return;
    char* start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') ++start;
    if (start != text) memmove(text, start, strlen(start) + 1);

    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
            text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
}

static void BuildGamePath(const char* relOrAbs, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!relOrAbs || !relOrAbs[0]) return;

    if (IsAbsolutePathA(relOrAbs)) {
        FormatTo(out, outSize, "{}", relOrAbs);
    } else {
        FormatTo(out, outSize, "{}\\{}", g_gameDir, relOrAbs);
    }
}

static bool EnsureDirectoryTreeA(const char* path)
{
    if (!path || !path[0]) return false;

    char current[MAX_PATH]{};
    FormatTo(current, sizeof(current), "{}", path);

    size_t length = strlen(current);
    while (length > 3 && (current[length - 1] == '\\' || current[length - 1] == '/')) {
        current[--length] = '\0';
    }

    char* scan = current;
    if (current[0] && current[1] == ':') {
        scan = current + 3;
    } else if (current[0] == '\\' && current[1] == '\\') {
        scan = current + 2;
        for (int separators = 0; *scan && separators < 2; ++scan) {
            if (*scan == '\\' || *scan == '/') ++separators;
        }
    }

    for (char* cursor = scan; *cursor; ++cursor) {
        if (*cursor != '\\' && *cursor != '/') continue;
        const char separator = *cursor;
        *cursor = '\0';
        if (current[0] && !CreateDirectoryA(current, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            *cursor = separator;
            return false;
        }
        *cursor = separator;
    }

    return CreateDirectoryA(current, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool SetDxvkEnvironment(const char* name, const char* value)
{
    if (!name || !name[0] || !value || !value[0]) return false;
    if (SetEnvironmentVariableA(name, value)) return true;
    Log("dxvkdiag: SetEnvironmentVariable failed name={} err={}", name, GetLastError());
    return false;
}

static void ConfigureDxvkDiagnostics(const char* iniPath)
{
    const bool enabled = GetPrivateProfileIntA("DXVKDiagnostics", "Enable", 0, iniPath) != 0;
    if (!enabled) {
        Log("dxvkdiag: disabled");
        return;
    }

    char outputDirText[MAX_PATH]{};
    GetPrivateProfileStringA("DXVKDiagnostics", "OutputDir", "Diagnostics\\DXVK",
        outputDirText, sizeof(outputDirText), iniPath);

    char outputDir[MAX_PATH]{};
    BuildGamePath(outputDirText, outputDir, sizeof(outputDir));
    if (!EnsureDirectoryTreeA(outputDir)) {
        Log("dxvkdiag: failed to create output directory path={} err={}", outputDir, GetLastError());
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char sessionDir[MAX_PATH]{};
    FormatTo(sessionDir, sizeof(sessionDir), "{}\\{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-pid{}",
        outputDir,
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId());
    if (!EnsureDirectoryTreeA(sessionDir)) {
        Log("dxvkdiag: failed to create session directory path={} err={}", sessionDir, GetLastError());
        return;
    }

    char logLevel[32]{};
    char hud[256]{};
    char debugMode[32]{};
    GetPrivateProfileStringA("DXVKDiagnostics", "LogLevel", "trace", logLevel, sizeof(logLevel), iniPath);
    GetPrivateProfileStringA("DXVKDiagnostics", "Hud", "full", hud, sizeof(hud), iniPath);
    GetPrivateProfileStringA("DXVKDiagnostics", "Debug", "markers", debugMode, sizeof(debugMode), iniPath);

    SetDxvkEnvironment("DXVK_LOG_LEVEL", logLevel);
    SetDxvkEnvironment("DXVK_LOG_PATH", sessionDir);
    SetDxvkEnvironment("DXVK_HUD", hud);
    SetDxvkEnvironment("DXVK_DEBUG", debugMode);

    char configPath[MAX_PATH]{};
    FormatTo(configPath, sizeof(configPath), "{}\\dxvk.conf", g_gameDir);
    if (FileExistsA(configPath)) {
        SetDxvkEnvironment("DXVK_CONFIG_FILE", configPath);
    }

    char statsPath[MAX_PATH]{};
    if (GetPrivateProfileIntA("DXVKDiagnostics", "StatsLog", 1, iniPath) != 0) {
        FormatTo(statsPath, sizeof(statsPath), "{}\\dxvk-stats.csv", sessionDir);
        SetDxvkEnvironment("DXVK_STATS_LOG", statsPath);
    }

    char frameTimePath[MAX_PATH]{};
    if (GetPrivateProfileIntA("DXVKDiagnostics", "FrameTimeLog", 1, iniPath) != 0) {
        FormatTo(frameTimePath, sizeof(frameTimePath), "{}\\dxvk-frametimes.csv", sessionDir);
        SetDxvkEnvironment("DXVK_FRAME_TIME_LOG", frameTimePath);
    }

    char shaderDir[MAX_PATH]{};
    if (GetPrivateProfileIntA("DXVKDiagnostics", "DumpShaders", 1, iniPath) != 0) {
        FormatTo(shaderDir, sizeof(shaderDir), "{}\\shaders", sessionDir);
        if (EnsureDirectoryTreeA(shaderDir)) {
            SetDxvkEnvironment("DXVK_SHADER_DUMP_PATH", shaderDir);
        } else {
            Log("dxvkdiag: failed to create shader dump directory path={} err={}", shaderDir, GetLastError());
        }
    }

    char manifestPath[MAX_PATH]{};
    FormatTo(manifestPath, sizeof(manifestPath), "{}\\session.txt", sessionDir);
    FILE* manifest = nullptr;
    if (fopen_s(&manifest, manifestPath, "wb") == 0 && manifest) {
        std::print(manifest, "DXVK diagnostic session\r\n");
        std::print(manifest, "LogLevel={}\r\nHud={}\r\nDebug={}\r\n", logLevel, hud, debugMode);
        std::print(manifest, "ConfigFile={}\r\n", FileExistsA(configPath) ? configPath : "(not found)");
        std::print(manifest, "StatsLog={}\r\n", statsPath[0] ? statsPath : "disabled");
        std::print(manifest, "FrameTimeLog={}\r\n", frameTimePath[0] ? frameTimePath : "disabled");
        std::print(manifest, "ShaderDump={}\r\n", shaderDir[0] ? shaderDir : "disabled");
        fclose(manifest);
    }

    Log("dxvkdiag: enabled session={} logLevel={} hud={} debug={} stats={} frametimes={} shaders={}",
        sessionDir,
        logLevel,
        hud,
        debugMode,
        statsPath[0] ? 1 : 0,
        frameTimePath[0] ? 1 : 0,
        shaderDir[0] ? 1 : 0);
}

static uint32_t ClaimNameToBit(const char* name)
{
    if (!name || !name[0]) return 0;
    constexpr std::array<std::pair<std::string_view, uint32_t>, 15> kNames = {{
        {"D3D9Entry", CLAIM_D3D9_ENTRY},
        {"D3D9", CLAIM_D3D9_ENTRY},
        {"DeviceWrap", CLAIM_DEVICE_WRAP},
        {"Device", CLAIM_DEVICE_WRAP},
        {"EndScene", CLAIM_POSTFX_ENDSCENE},
        {"PostFXEndScene", CLAIM_POSTFX_ENDSCENE},
        {"Present", CLAIM_POSTFX_PRESENT},
        {"PostFXPresent", CLAIM_POSTFX_PRESENT},
        {"Depth", CLAIM_DEPTH},
        {"DepthAccess", CLAIM_DEPTH},
        {"ShaderPatch", CLAIM_SHADER_PATCH},
        {"Shader", CLAIM_SHADER_PATCH},
        {"DXGI", CLAIM_DXGI},
        {"Backend", CLAIM_BACKEND},
        {"DXVKBackend", CLAIM_BACKEND},
    }};
    const auto match = std::ranges::find_if(
        kNames,
        [name](const auto& entry) {
            return _stricmp(name, entry.first.data()) == 0;
        });
    return match == kNames.end() ? 0 : match->second;
}

static uint32_t ParseClaimMask(char* text)
{
    uint32_t mask = 0;
    if (!text) return mask;
    char* ctx = nullptr;
    for (char* token = strtok_s(text, ";,", &ctx); token; token = strtok_s(nullptr, ";,", &ctx)) {
        TrimSpaces(token);
        const uint32_t bit = ClaimNameToBit(token);
        if (bit) {
            mask |= bit;
        } else if (token[0]) {
            Log("proxychain: unknown claim '{}'", token);
        }
    }
    return mask;
}

static void ClaimMaskToString(uint32_t mask, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;

    struct ClaimName { uint32_t bit; const char* name; };
    static const ClaimName names[] = {
        { CLAIM_D3D9_ENTRY, "D3D9Entry" },
        { CLAIM_DEVICE_WRAP, "DeviceWrap" },
        { CLAIM_POSTFX_ENDSCENE, "EndScene" },
        { CLAIM_POSTFX_PRESENT, "Present" },
        { CLAIM_DEPTH, "Depth" },
        { CLAIM_SHADER_PATCH, "ShaderPatch" },
        { CLAIM_DXGI, "DXGI" },
        { CLAIM_BACKEND, "Backend" },
    };

    std::string text;
    for (const auto& item : names) {
        if (!(mask & item.bit)) continue;
        if (!text.empty()) text += ';';
        text += item.name;
    }
    FormatTo(out, outSize, "{}", text.empty() ? "None" : text.c_str());
}

static void ResetProxyClaimOwners()
{
    for (auto& owner : g_claimOwners) {
        owner.owner[0] = '\0';
    }
}

static void AssignProxyClaimOwner(uint32_t mask, const char* ownerName)
{
    if (!ownerName || !ownerName[0]) return;
    for (auto& owner : g_claimOwners) {
        if (mask & owner.bit) {
            FormatTo(owner.owner, sizeof(owner.owner), "{}", ownerName);
        }
    }
}

static void DescribeProxyClaimOwners(uint32_t mask, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;

    std::string text;
    for (const auto& owner : g_claimOwners) {
        if (!(mask & owner.bit)) continue;

        char claimText[64]{};
        ClaimMaskToString(owner.bit, claimText, sizeof(claimText));

        if (!text.empty()) text += "; ";
        text += std::format(
            "{}={}",
            claimText,
            owner.owner[0] ? owner.owner : "(unowned)");
    }

    FormatTo(out, outSize, "{}", text.empty() ? "None" : text.c_str());
}

static void LogProxyClaimTable()
{
    char owned[512]{};
    for (const auto& owner : g_claimOwners) {
        if (!owner.owner[0]) continue;

        char claimText[64]{};
        ClaimMaskToString(owner.bit, claimText, sizeof(claimText));

        char item[160]{};
        FormatTo(item, sizeof(item), "{}={}", claimText, owner.owner);
        if (owned[0]) strncat(owned, "; ", sizeof(owned) - strlen(owned) - 1);
        strncat(owned, item, sizeof(owned) - strlen(owned) - 1);
    }

    Log("proxychain: ownership {}", owned[0] ? owned : "(none)");
}

static void LoadProxyChainConfig(const char* iniPath)
{
    g_proxyChain.clear();
    g_enableProxyChain = GetPrivateProfileIntA("ProxyChain", "Enable", 0, iniPath) != 0;
    g_enableLegacyD3D9PSAutoProbe =
        GetPrivateProfileIntA("ProxyChain", "LegacyAutoProbeD3D9PS", 0, iniPath) != 0;
    if (!g_enableProxyChain) {
        Log("proxychain: disabled legacyAutoProbeD3D9PS={}",
            g_enableLegacyD3D9PSAutoProbe ? 1 : 0);
        return;
    }

    char order[2048]{};
    GetPrivateProfileStringA("ProxyChain", "Order", "", order, sizeof(order), iniPath);
    TrimSpaces(order);

    if (!order[0]) {
        for (int i = 1; i <= 128; ++i) {
            char key[16]{};
            char value[128]{};
            FormatTo(key, sizeof(key), "{}", i);
            GetPrivateProfileStringA("register", key, "", value, sizeof(value), iniPath);
            TrimSpaces(value);
            if (!value[0]) continue;

            if (order[0]) strncat(order, ";", sizeof(order) - strlen(order) - 1);
            strncat(order, value, sizeof(order) - strlen(order) - 1);
        }
    }

    Log("proxychain: enabled order={}", order[0] ? order : "(empty)");
    if (!order[0]) return;

    char* ctx = nullptr;
    for (char* token = strtok_s(order, ";,", &ctx); token; token = strtok_s(nullptr, ";,", &ctx)) {
        TrimSpaces(token);
        if (!token[0]) continue;

        char section[128]{};
        FormatTo(section, sizeof(section), "Proxy.{}", token);

        ProxyConfig proxy{};
        FormatTo(proxy.name, sizeof(proxy.name), "{}", token);
        proxy.enabled = GetPrivateProfileIntA(section, "Enable", 1, iniPath) != 0;
        proxy.required = GetPrivateProfileIntA(section, "Required", 0, iniPath) != 0;
        GetPrivateProfileStringA(section, "Type", "D3D9Proxy", proxy.type, sizeof(proxy.type), iniPath);
        GetPrivateProfileStringA(section, "Path", "", proxy.path, sizeof(proxy.path), iniPath);
        GetPrivateProfileStringA(section, "Mode", "Compatibility", proxy.mode, sizeof(proxy.mode), iniPath);
        GetPrivateProfileStringA(section, "Stage", "Auto", proxy.stage, sizeof(proxy.stage), iniPath);
        GetPrivateProfileStringA(section, "ConflictPolicy", "SkipIfClaimed", proxy.conflictPolicy, sizeof(proxy.conflictPolicy), iniPath);

        char claims[512]{};
        GetPrivateProfileStringA(section, "Claims", "", claims, sizeof(claims), iniPath);
        proxy.claims = ParseClaimMask(claims);

        if (!proxy.path[0]) {
            FormatTo(section, sizeof(section), "{}", token);
            proxy.enabled = GetPrivateProfileIntA(section, "Enable", proxy.enabled ? 1 : 0, iniPath) != 0;
            proxy.required = GetPrivateProfileIntA(section, "Required", proxy.required ? 1 : 0, iniPath) != 0;
            GetPrivateProfileStringA(section, "Type", proxy.type, proxy.type, sizeof(proxy.type), iniPath);
            GetPrivateProfileStringA(section, "dll", "", proxy.path, sizeof(proxy.path), iniPath);
            if (!proxy.path[0]) {
                GetPrivateProfileStringA(section, "asi", "", proxy.path, sizeof(proxy.path), iniPath);
                if (proxy.path[0] && _stricmp(proxy.type, "D3D9Proxy") == 0) {
                    FormatTo(proxy.type, sizeof(proxy.type), "ASI");
                }
            }
            if (!proxy.path[0]) {
                GetPrivateProfileStringA(section, "Path", "", proxy.path, sizeof(proxy.path), iniPath);
            }
            GetPrivateProfileStringA(section, "Mode", proxy.mode, proxy.mode, sizeof(proxy.mode), iniPath);
            GetPrivateProfileStringA(section, "Stage", proxy.stage, proxy.stage, sizeof(proxy.stage), iniPath);
            GetPrivateProfileStringA(section, "ConflictPolicy", proxy.conflictPolicy, proxy.conflictPolicy, sizeof(proxy.conflictPolicy), iniPath);

            claims[0] = '\0';
            GetPrivateProfileStringA(section, "Claims", "", claims, sizeof(claims), iniPath);
            if (claims[0]) {
                proxy.claims = ParseClaimMask(claims);
            }
        }

        if (!proxy.path[0]) {
            Log("proxychain: {} skipped, missing Path", proxy.name);
            continue;
        }

        char claimText[256]{};
        ClaimMaskToString(proxy.claims, claimText, sizeof(claimText));
        Log("proxychain: configured name={} type={} path={} claims={} mode={} stage={} conflict={} enabled={} required={}",
            proxy.name, proxy.type, proxy.path, claimText, proxy.mode, proxy.stage, proxy.conflictPolicy,
            proxy.enabled ? 1 : 0, proxy.required ? 1 : 0);
        g_proxyChain.push_back(proxy);
    }
}

static void LoadPostFxPlugins(const char* iniPath)
{
    g_enablePostFxHost = GetPrivateProfileIntA("PostFX", "EnableHost", 0, iniPath) != 0;
    if (!g_enablePostFxHost) {
        Log("postfx: host disabled");
        return;
    }

    char pluginDirRel[MAX_PATH]{};
    GetPrivateProfileStringA("PostFX", "PluginDir", "plugins\\d3d9chain",
        pluginDirRel, sizeof(pluginDirRel), iniPath);

    char pluginDir[MAX_PATH]{};
    BuildGamePath(pluginDirRel, pluginDir, sizeof(pluginDir));

    char list[2048]{};
    GetPrivateProfileStringA("PostFX", "Plugins", "", list, sizeof(list), iniPath);
    TrimSpaces(list);

    Log("postfx: host enabled dir={} plugins={}", pluginDir, list[0] ? list : "(none)");
    if (!list[0]) return;

    char* ctx = nullptr;
    for (char* token = strtok_s(list, ";,", &ctx); token; token = strtok_s(nullptr, ";,", &ctx)) {
        TrimSpaces(token);
        if (!token[0]) continue;

        char path[MAX_PATH]{};
        if (IsAbsolutePathA(token)) {
            FormatTo(path, sizeof(path), "{}", token);
        } else {
            FormatTo(path, sizeof(path), "{}\\{}", pluginDir, token);
        }

        HMODULE module = LoadLibraryA(path);
        if (!module) {
            Log("postfx: failed to load {} err={}", path, GetLastError());
            continue;
        }

        LoadedPlugin plugin{};
        plugin.module = module;
        FormatTo(plugin.path, sizeof(plugin.path), "{}", path);
        g_plugins.push_back(std::move(plugin));
        LoadedPlugin* loaded = &g_plugins.back();

        auto init2 = reinterpret_cast<BridgeD3D9_PluginInit2>(
            GetProcAddress(module, "BridgeD3D9_PluginInit2"));
        auto init1 = reinterpret_cast<BridgeD3D9_PluginInit>(
            GetProcAddress(module, "BridgeD3D9_PluginInit"));

        if (init2) {
            BridgeD3D9PluginApi2 api2{};
            api2.apiVersion = BRIDGE_D3D9_PLUGIN_API_VERSION_2;
            api2.structSize = sizeof(api2);
            api2.Log = &PluginLog;
            api2.hostContext = module;
            api2.GetVulkanStatus = &HostGetVulkanStatus;
            api2.RegisterVulkanPass = &HostRegisterVulkanPass;
            api2.UnregisterVulkanPass = &HostUnregisterVulkanPass;

            bool crashed = false;
            BOOL ok = SafePluginInit2(init2, &api2, &crashed);
            if (!ok) {
                Log("postfx: API v2 init {} {}", crashed ? "crashed for" : "rejected", path);
                UnregisterPluginVulkanPasses(*loaded);
                g_plugins.pop_back();
                FreeLibrary(module);
                continue;
            }
            loaded->usesApi2 = true;
        } else if (init1) {
            BridgeD3D9PluginApi api1{};
            api1.apiVersion = BRIDGE_D3D9_PLUGIN_API_VERSION_1;
            api1.Log = &PluginLog;

            bool crashed = false;
            BOOL ok = SafePluginInit1(init1, &api1, &crashed);
            if (!ok) {
                Log("postfx: API v1 init {} {}", crashed ? "crashed for" : "rejected", path);
                g_plugins.pop_back();
                FreeLibrary(module);
                continue;
            }
        }

        loaded->shutdown = reinterpret_cast<BridgeD3D9_PluginShutdown>(GetProcAddress(module, "BridgeD3D9_PluginShutdown"));
        loaded->onCreateDevice = reinterpret_cast<BridgeD3D9_OnCreateDevice>(GetProcAddress(module, "BridgeD3D9_OnCreateDevice"));
        loaded->onResetBefore = reinterpret_cast<BridgeD3D9_OnResetBefore>(GetProcAddress(module, "BridgeD3D9_OnResetBefore"));
        loaded->onResetAfter = reinterpret_cast<BridgeD3D9_OnResetAfter>(GetProcAddress(module, "BridgeD3D9_OnResetAfter"));
        loaded->onEndScene = reinterpret_cast<BridgeD3D9_OnEndScene>(GetProcAddress(module, "BridgeD3D9_OnEndScene"));
        loaded->onPresentBefore = reinterpret_cast<BridgeD3D9_OnPresentBefore>(GetProcAddress(module, "BridgeD3D9_OnPresentBefore"));
        loaded->onPresentAfter = reinterpret_cast<BridgeD3D9_OnPresentAfter>(GetProcAddress(module, "BridgeD3D9_OnPresentAfter"));
        loaded->onReleaseDevice = reinterpret_cast<BridgeD3D9_OnReleaseDevice>(GetProcAddress(module, "BridgeD3D9_OnReleaseDevice"));

        Log("postfx: loaded {} api={} nativePasses={}", path,
            loaded->usesApi2 ? "v2" : "v1",
            static_cast<unsigned long long>(loaded->vulkanPasses.size()));
    }
}

static void ShutdownPostFxPlugins()
{
    for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
        SafePluginCall("Shutdown", it->shutdown);
        UnregisterPluginVulkanPasses(*it);
        if (it->module) {
            FreeLibrary(it->module);
        }
    }
    g_plugins.clear();
}

static BOOL CALLBACK InitializePostFxOnce(PINIT_ONCE, PVOID, PVOID*)
{
    if (g_enablePostFxHost && g_postFxIniPath[0]) {
        LoadPostFxPlugins(g_postFxIniPath);
    }
    return TRUE;
}

static void EnsurePostFxPluginsLoaded()
{
    if (!g_enablePostFxHost) return;
    if (!InitOnceExecuteOnce(&g_postFxInitOnce, InitializePostFxOnce, nullptr, nullptr)) {
        Log("postfx: deferred initialization failed err={}", GetLastError());
    }
}

extern "C" UINT WINAPI Bridge_GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize);

static HMODULE LoadBackendD3D9()
{
    char path[MAX_PATH]{};

    if (kBackendTraceBuild) {
        if (!g_selfModule || !GetModuleFileNameA(g_selfModule, path, sizeof(path))) {
            Log("backendtrace: failed to resolve proxy module path err={}", GetLastError());
            return nullptr;
        }
        char* slash = strrchr(path, '\\');
        if (!slash) {
            Log("backendtrace: proxy module path has no directory: {}", path);
            return nullptr;
        }
        FormatTo(slash + 1, static_cast<size_t>(path + sizeof(path) - slash - 1), "d3d9_dxvk.dll");
    } else if (g_useDxvkBackend && g_dxvkBackendDir[0]) {
        FormatTo(path, sizeof(path), "{}\\d3d9.dll", g_dxvkBackendDir);
    } else {
        char sysDir[MAX_PATH]{};
        GetSystemDirectoryA(sysDir, sizeof(sysDir));
        FormatTo(path, sizeof(path), "{}\\d3d9.dll", sysDir);
    }

    HMODULE h = LoadLibraryA(path);
    Log("{} d3d9: {} -> 0x{:08X}",
        kBackendTraceBuild ? "backendtrace" :
        (g_useDxvkBackend && g_dxvkBackendDir[0] ? "backend" : "system"),
        path, reinterpret_cast<std::uintptr_t>(h));
    // Log every module named d3d9.dll with its base so CPU hotspot captures
    // can be attributed unambiguously (module bases change across launches).
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&g_dxvkBackendDir), &self);
        char sysD3d9[MAX_PATH]{};
        GetSystemDirectoryA(sysD3d9, sizeof(sysD3d9));
        strncat_s(sysD3d9, "\\d3d9.dll", _TRUNCATE);
        Log("modulemap: bridge=0x{:08X} backend=0x{:08X} system-d3d9=0x{:08X}", reinterpret_cast<std::uintptr_t>(self), reinterpret_cast<std::uintptr_t>(h), reinterpret_cast<std::uintptr_t>(GetModuleHandleA(sysD3d9)));
    }
    return h;
}

static HMODULE LoadPSProxy()
{
    if (g_enableProxyChain) {
        ResetProxyClaimOwners();
        uint32_t claimed = 0;
        HMODULE selectedD3D9Proxy = nullptr;
        for (const auto& proxy : g_proxyChain) {
            if (!proxy.enabled) {
                Log("proxychain: {} disabled", proxy.name);
                continue;
            }

            const uint32_t conflict = claimed & proxy.claims;
            const bool allowShared = _stricmp(proxy.conflictPolicy, "AllowShared") == 0;
            const bool replaceEarlier = _stricmp(proxy.conflictPolicy, "ReplaceEarlier") == 0;
            if (conflict && !allowShared && !replaceEarlier) {
                char conflictText[256]{};
                DescribeProxyClaimOwners(conflict, conflictText, sizeof(conflictText));
                Log("proxychain: {} skipped, claims already owned: {}", proxy.name, conflictText);
                continue;
            }

            if (_stricmp(proxy.type, "D3D9Proxy") != 0) {
                const bool observedAsi =
                    _stricmp(proxy.type, "ObservedASI") == 0 ||
                    _stricmp(proxy.type, "ManagedASI") == 0 ||
                    _stricmp(proxy.type, "InternalDeviceProxy") == 0;
                if (observedAsi) {
                    char path[MAX_PATH]{};
                    BuildGamePath(proxy.path, path, sizeof(path));
                    DWORD attrs = GetFileAttributesA(path);
                    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                        Log("proxychain: observed ASI {} missing path={}", proxy.name, path);
                        continue;
                    }

                    const char* moduleName = strrchr(path, '\\');
                    moduleName = moduleName ? moduleName + 1 : path;
                    HMODULE h = GetModuleHandleA(moduleName);
                    Log("proxychain: observed ASI {} path={} module=0x{:08X} state={}",
                        proxy.name, path, reinterpret_cast<std::uintptr_t>(h), h ? "loaded" : "deferred-to-ASI-loader");
                }

                if (_stricmp(proxy.type, "ASI") == 0) {
                    char path[MAX_PATH]{};
                    BuildGamePath(proxy.path, path, sizeof(path));
                    HMODULE h = LoadLibraryA(path);
                    if (!h) {
                        Log("proxychain: failed to load ASI {} path={} err={}", proxy.name, path, GetLastError());
                        continue;
                    }
                    Log("proxychain: loaded ASI {} path={} -> 0x{:08X}", proxy.name, path, reinterpret_cast<std::uintptr_t>(h));
                }

                if (proxy.claims && !conflict) {
                    claimed = replaceEarlier ? proxy.claims : (claimed | proxy.claims);
                    AssignProxyClaimOwner(proxy.claims, proxy.name);
                }
                if (_stricmp(proxy.type, "ASI") != 0 && !observedAsi) {
                    Log("proxychain: {} type={} accepted for ownership audit but no adapter is implemented yet",
                        proxy.name, proxy.type);
                }
                continue;
            }

            if (selectedD3D9Proxy) {
                Log("proxychain: {} skipped, primary D3D9 proxy already selected: {}",
                    proxy.name, g_primaryProxyName[0] ? g_primaryProxyName : "(unnamed)");
                continue;
            }

            char path[MAX_PATH]{};
            BuildGamePath(proxy.path, path, sizeof(path));

            bool hooked = false;
            if (g_useDxvkBackend && g_dxvkBackendDir[0]) {
                hooked = InstallSystemDirectoryHook();
            }

            HMODULE h = LoadLibraryA(path);

            if (hooked) {
                RestoreSystemDirectoryHook();
            }

            if (!h) {
                Log("proxychain: failed to load {} path={} err={}", proxy.name, path, GetLastError());
                if (proxy.required) {
                    Log("proxychain: required proxy {} missing; continuing without D3D9 proxy to avoid loader abort", proxy.name);
                }
                continue;
            }

            claimed = replaceEarlier ? proxy.claims : (claimed | proxy.claims);
            AssignProxyClaimOwner(proxy.claims, proxy.name);
            FormatTo(g_primaryProxyName, sizeof(g_primaryProxyName), "{}", proxy.name);
            char claimText[256]{};
            ClaimMaskToString(proxy.claims, claimText, sizeof(claimText));
            Log("proxychain: loaded primary D3D9 proxy {} path={} claims={} -> 0x{:08X}",
                proxy.name, path, claimText, reinterpret_cast<std::uintptr_t>(h));
            selectedD3D9Proxy = h;
        }

        LogProxyClaimTable();
        if (!selectedD3D9Proxy) {
            Log("proxychain: no D3D9 proxy selected");
        }
        return selectedD3D9Proxy;
    }

    if (!g_enableLegacyD3D9PSAutoProbe) {
        Log("proxychain: legacy d3d9_ps.dll auto-probe disabled");
        return nullptr;
    }

    bool hooked = false;
    if (g_useDxvkBackend && g_dxvkBackendDir[0]) {
        hooked = InstallSystemDirectoryHook();
    }

    HMODULE h = LoadLibraryA("d3d9_ps.dll");

    if (hooked) {
        RestoreSystemDirectoryHook();
    }

    if (h) {
        Log("PS proxy: loaded d3d9_ps.dll -> 0x{:08X}", reinterpret_cast<std::uintptr_t>(h));
        FormatTo(g_primaryProxyName, sizeof(g_primaryProxyName), "ProperShaders");
    } else {
        Log("PS proxy: d3d9_ps.dll not found, PS disabled");
    }
    return h;
}

static void GetGameDirectory(char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';

    DWORD len = GetModuleFileNameA(nullptr, out, static_cast<DWORD>(outSize));
    if (len == 0 || len >= outSize) {
        out[0] = '\0';
        return;
    }

    char* slash = strrchr(out, '\\');
    if (slash) {
        *slash = '\0';
    }
}

static bool FileExistsA(const char* path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool InstallSystemDirectoryHook()
{
    if (g_getSystemDirectoryAHookInstalled) return true;

    HMODULE kernel32 = GetModuleHandleA("KERNEL32.dll");
    if (!kernel32) {
        Log("backend: GetModuleHandleA(KERNEL32.dll) failed err={}", GetLastError());
        return false;
    }

    g_getSystemDirectoryAAddress = reinterpret_cast<void*>(GetProcAddress(kernel32, "GetSystemDirectoryA"));
    if (!g_getSystemDirectoryAAddress) {
        Log("backend: GetProcAddress(GetSystemDirectoryA) failed err={}", GetLastError());
        return false;
    }

    uint8_t patch[5]{};
    patch[0] = 0xE9;
    intptr_t rel = reinterpret_cast<uintptr_t>(&Bridge_GetSystemDirectoryA)
        - reinterpret_cast<uintptr_t>(g_getSystemDirectoryAAddress) - 5;
    *reinterpret_cast<int32_t*>(&patch[1]) = static_cast<int32_t>(rel);

    DWORD oldProtect = 0;
    if (!VirtualProtect(g_getSystemDirectoryAAddress, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("backend: VirtualProtect GetSystemDirectoryA failed err={}", GetLastError());
        return false;
    }

    memcpy(g_getSystemDirectoryAOriginal, g_getSystemDirectoryAAddress, sizeof(patch));
    memcpy(g_getSystemDirectoryAAddress, patch, sizeof(patch));

    DWORD ignored = 0;
    VirtualProtect(g_getSystemDirectoryAAddress, sizeof(patch), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_getSystemDirectoryAAddress, sizeof(patch));

    g_getSystemDirectoryAHookInstalled = true;
    Log("backend: temporary GetSystemDirectoryA hook installed");
    return true;
}

static void RestoreSystemDirectoryHook()
{
    if (!g_getSystemDirectoryAHookInstalled || !g_getSystemDirectoryAAddress) return;

    DWORD oldProtect = 0;
    if (!VirtualProtect(g_getSystemDirectoryAAddress, sizeof(g_getSystemDirectoryAOriginal),
            PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("backend: restore VirtualProtect GetSystemDirectoryA failed err={}", GetLastError());
        return;
    }

    memcpy(g_getSystemDirectoryAAddress, g_getSystemDirectoryAOriginal, sizeof(g_getSystemDirectoryAOriginal));

    DWORD ignored = 0;
    VirtualProtect(g_getSystemDirectoryAAddress, sizeof(g_getSystemDirectoryAOriginal), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_getSystemDirectoryAAddress, sizeof(g_getSystemDirectoryAOriginal));

    g_getSystemDirectoryAHookInstalled = false;
    Log("backend: temporary GetSystemDirectoryA hook restored");
}

static void LoadBridgeConfig()
{
    char gameDir[MAX_PATH]{};
    GetGameDirectory(gameDir, sizeof(gameDir));
    if (!gameDir[0]) return;
    FormatTo(g_gameDir, sizeof(g_gameDir), "{}", gameDir);

    if (kBackendTraceBuild) {
        g_enablePostFxHost = false;
        g_enableProxyChain = false;
        g_enableD3D9Stats = false;
        g_enableD3D9Trace = false;
        g_enableD3D9CallsiteProfile = false;
        g_enableCpuHotspotProfile = false;
        g_enableD3D9Optimizer = false;
        g_affinityEnable = false;
        g_useDxvkBackend = false;
        Log("backendtrace: isolated mode enabled; exportKey=F10 ring=alpha-texture draws backend=d3d9_dxvk.dll");
        return;
    }

    char iniPath[MAX_PATH]{};
    FormatTo(iniPath, sizeof(iniPath), "{}\\SA.RenderStack.ini", gameDir);
    if (!FileExistsA(iniPath)) {
        FormatTo(iniPath, sizeof(iniPath), "{}\\scripts\\BridgeD3D9.ini", gameDir);
    }
    FormatTo(g_performanceIniPath, sizeof(g_performanceIniPath), "{}", iniPath);
    g_threadSchedulingOptions = renderstack::scheduling::ReadOptions();
    Log("config: source={}", iniPath);
    Log("scheduling: PerThread={} Mmcss={} deviceOwnerMmcss=0",
        g_threadSchedulingOptions.enabled ? 1 : 0,
        g_threadSchedulingOptions.mmcss ? 1 : 0);

    ConfigureDxvkDiagnostics(iniPath);

    g_affinityEnable = GetPrivateProfileIntA("Affinity", "Enable", 0, iniPath) != 0;
    char affinityMaskText[64]{};
    GetPrivateProfileStringA("Affinity", "Mask", "0", affinityMaskText, sizeof(affinityMaskText), iniPath);
    g_affinityRequestedMask = ParseAffinityMask(affinityMaskText);
    char priorityText[32]{};
    GetPrivateProfileStringA("Affinity", "Priority", "Normal", priorityText, sizeof(priorityText), iniPath);
    g_affinityPriorityClass = PriorityNameToClass(priorityText);
    g_affinityReapply = GetPrivateProfileIntA("Affinity", "Reapply", 0, iniPath) != 0;
    g_affinityReapplyCount = static_cast<DWORD>(GetPrivateProfileIntA("Affinity", "ReapplyCount", 60, iniPath));
    g_affinityReapplyIntervalMs = static_cast<DWORD>(GetPrivateProfileIntA("Affinity", "ReapplyIntervalMs", 2000, iniPath));
    if (g_affinityReapplyIntervalMs < 250) {
        g_affinityReapplyIntervalMs = 250;
    }
    Log("affinity: config enable={} mask={} priority={} reapply={} count={} intervalMs={}",
        g_affinityEnable ? 1 : 0,
        affinityMaskText,
        PriorityClassName(g_affinityPriorityClass),
        g_affinityReapply ? 1 : 0,
        g_affinityReapplyCount,
        g_affinityReapplyIntervalMs);

    g_enableD3D9Stats = GetPrivateProfileIntA("D3D9Stats", "Enable", 0, iniPath) != 0;
    g_d3d9StatsIntervalMs = static_cast<DWORD>(GetPrivateProfileIntA("D3D9Stats", "IntervalMs", 1000, iniPath));
    if (g_d3d9StatsIntervalMs < 250) {
        g_d3d9StatsIntervalMs = 250;
    }
    Log("d3d9stats: enable={} intervalMs={} layer=game-to-primary-proxy",
        g_enableD3D9Stats ? 1 : 0, g_d3d9StatsIntervalMs);

    g_enableD3D9Trace = GetPrivateProfileIntA("D3D9Trace", "Enable", 0, iniPath) != 0;
    g_d3d9TraceTriggerKey = GetPrivateProfileIntA("D3D9Trace", "TriggerVirtualKey", VK_F10, iniPath);
    g_d3d9TraceMaxDraws = static_cast<UINT>(
        GetPrivateProfileIntA("D3D9Trace", "MaxDraws", 12000, iniPath));
    if (g_d3d9TraceTriggerKey < 1 || g_d3d9TraceTriggerKey > 255) {
        g_d3d9TraceTriggerKey = VK_F10;
    }
    if (g_d3d9TraceMaxDraws < 100) {
        g_d3d9TraceMaxDraws = 100;
    } else if (g_d3d9TraceMaxDraws > 100000) {
        g_d3d9TraceMaxDraws = 100000;
    }
    Log("d3d9trace: enable={} triggerVirtualKey={} maxDraws={} output=scripts\\BridgeD3D9.drawtrace.log",
        g_enableD3D9Trace ? 1 : 0,
        g_d3d9TraceTriggerKey,
        g_d3d9TraceMaxDraws);

    g_enableD3D9CallsiteProfile =
        GetPrivateProfileIntA("D3D9CallsiteProfile", "Enable", 0, iniPath) != 0;
    g_d3d9CallsiteTriggerKey =
        GetPrivateProfileIntA("D3D9CallsiteProfile", "TriggerVirtualKey", VK_F9, iniPath);
    g_d3d9CallsiteCaptureFrames = static_cast<UINT>(
        GetPrivateProfileIntA("D3D9CallsiteProfile", "CaptureFrames", 120, iniPath));
    g_d3d9CallsiteSampleEveryDraws = static_cast<UINT>(
        GetPrivateProfileIntA("D3D9CallsiteProfile", "SampleEveryDraws", 64, iniPath));
    if (g_d3d9CallsiteTriggerKey < 1 || g_d3d9CallsiteTriggerKey > 255) {
        g_d3d9CallsiteTriggerKey = VK_F9;
    }
    if (g_d3d9CallsiteCaptureFrames < 1) {
        g_d3d9CallsiteCaptureFrames = 1;
    } else if (g_d3d9CallsiteCaptureFrames > 3600) {
        g_d3d9CallsiteCaptureFrames = 3600;
    }
    if (g_d3d9CallsiteSampleEveryDraws < 1) {
        g_d3d9CallsiteSampleEveryDraws = 1;
    } else if (g_d3d9CallsiteSampleEveryDraws > 4096) {
        g_d3d9CallsiteSampleEveryDraws = 4096;
    }
    Log("d3d9callsites: enable={} triggerVirtualKey={} captureFrames={} sampleEveryDraws={} output=scripts\\BridgeD3D9.callsites.log",
        g_enableD3D9CallsiteProfile ? 1 : 0,
        g_d3d9CallsiteTriggerKey,
        g_d3d9CallsiteCaptureFrames,
        g_d3d9CallsiteSampleEveryDraws);

    g_enableCpuHotspotProfile =
        GetPrivateProfileIntA("CPUHotspotProfile", "Enable", 0, iniPath) != 0;
    g_cpuHotspotTriggerKey =
        GetPrivateProfileIntA("CPUHotspotProfile", "TriggerVirtualKey", VK_F8, iniPath);
    g_cpuHotspotDurationMs = static_cast<DWORD>(
        GetPrivateProfileIntA("CPUHotspotProfile", "DurationMs", 10000, iniPath));
    g_cpuHotspotIntervalMs = static_cast<DWORD>(
        GetPrivateProfileIntA("CPUHotspotProfile", "IntervalMs", 2, iniPath));
    g_cpuHotspotChainD3D9CallsiteProfile =
        GetPrivateProfileIntA("CPUHotspotProfile", "ChainD3D9CallsiteProfile", 0, iniPath) != 0;
    if (g_cpuHotspotTriggerKey < 1 || g_cpuHotspotTriggerKey > 255) {
        g_cpuHotspotTriggerKey = VK_F8;
    }
    if (g_cpuHotspotDurationMs < 1000) {
        g_cpuHotspotDurationMs = 1000;
    } else if (g_cpuHotspotDurationMs > 60000) {
        g_cpuHotspotDurationMs = 60000;
    }
    if (g_cpuHotspotIntervalMs < 1) {
        g_cpuHotspotIntervalMs = 1;
    } else if (g_cpuHotspotIntervalMs > 100) {
        g_cpuHotspotIntervalMs = 100;
    }
    Log("cpuhotspots: enable={} triggerVirtualKey={} durationMs={} intervalMs={} chainD3D9Callsites={} output=scripts\\BridgeD3D9.cpuhotspots.log",
        g_enableCpuHotspotProfile ? 1 : 0,
        g_cpuHotspotTriggerKey,
        g_cpuHotspotDurationMs,
        g_cpuHotspotIntervalMs,
        g_cpuHotspotChainD3D9CallsiteProfile ? 1 : 0);

    g_enableProperShadersStateAttribution =
        GetPrivateProfileIntA("PerformanceDiagnostics", "EnableStateAttribution", 0, iniPath) != 0;
    g_properShadersStateAttributionTriggerKey = GetPrivateProfileIntA(
        "PerformanceDiagnostics", "StateAttributionTriggerVirtualKey", VK_F7, iniPath);
    g_properShadersStateAttributionDurationMs = static_cast<DWORD>(GetPrivateProfileIntA(
        "PerformanceDiagnostics", "StateAttributionDurationMs", 3000, iniPath));
    if (g_properShadersStateAttributionTriggerKey < 1 ||
        g_properShadersStateAttributionTriggerKey > 255) {
        g_properShadersStateAttributionTriggerKey = VK_F7;
    }
    if (g_properShadersStateAttributionDurationMs < 500) {
        g_properShadersStateAttributionDurationMs = 500;
    } else if (g_properShadersStateAttributionDurationMs > 10000) {
        g_properShadersStateAttributionDurationMs = 10000;
    }
    Log("stateattribution: enable={} triggerVirtualKey={} durationMs={} output=scripts\\BridgeD3D9.state-attribution.log",
        g_enableProperShadersStateAttribution ? 1 : 0,
        g_properShadersStateAttributionTriggerKey,
        g_properShadersStateAttributionDurationMs);

    g_enableProperShadersEffectProfile =
        GetPrivateProfileIntA("ProperShadersEffectProfile", "Enable", 0, iniPath) != 0;
    g_properShadersEffectProfileTriggerKey = GetPrivateProfileIntA(
        "ProperShadersEffectProfile", "TriggerVirtualKey", VK_F7, iniPath);
    g_properShadersEffectProfileDurationMs = static_cast<DWORD>(GetPrivateProfileIntA(
        "ProperShadersEffectProfile", "DurationMs", 3000, iniPath));
    g_properShadersEffectProfileTestNoSaveState = GetPrivateProfileIntA(
        "ProperShadersEffectProfile", "TestNoSaveState", 0, iniPath) != 0;
    g_properShadersEffectProfileTestSkipDuplicateMatrices = GetPrivateProfileIntA(
        "ProperShadersEffectProfile", "TestSkipDuplicateMatrices", 0, iniPath) != 0;
    if (g_properShadersEffectProfileTestNoSaveState) {
        Log("effectprofile: TestNoSaveState rejected because ProperShaders requires state restoration");
        g_properShadersEffectProfileTestNoSaveState = false;
    }
    if (g_properShadersEffectProfileTriggerKey < 1 ||
        g_properShadersEffectProfileTriggerKey > 255) {
        g_properShadersEffectProfileTriggerKey = VK_F7;
    }
    if (g_properShadersEffectProfileDurationMs < 500) {
        g_properShadersEffectProfileDurationMs = 500;
    } else if (g_properShadersEffectProfileDurationMs > 10000) {
        g_properShadersEffectProfileDurationMs = 10000;
    }
    Log("effectprofile: enable={} triggerVirtualKey={} durationMs={} noSaveState={} skipDuplicateMatrices={} output=scripts\\BridgeD3D9.effectprofile.log",
        g_enableProperShadersEffectProfile ? 1 : 0,
        g_properShadersEffectProfileTriggerKey,
        g_properShadersEffectProfileDurationMs,
        g_properShadersEffectProfileTestNoSaveState ? 1 : 0,
        g_properShadersEffectProfileTestSkipDuplicateMatrices ? 1 : 0);

    g_enableProperShadersEffectOptimization = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "Enable", 0, iniPath) != 0;
    g_properShadersEffectOptimizationStateJournal = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "StateJournal", 1, iniPath) != 0;
    g_properShadersGeneralStateJournal = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "GeneralStateJournal", 1, iniPath) != 0;
    g_properShadersEffectBatching = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "BatchConsecutiveEffects", 0, iniPath) != 0;
    g_properShadersSkipDuplicateMatrices = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "SkipDuplicateMatrices", 1, iniPath) != 0;
    g_properShadersSkipDuplicateParameters = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "SkipDuplicateParameters", 1, iniPath) != 0;
    g_properShadersDirectConstants = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "DirectConstants", 1, iniPath) != 0;
    g_properShadersNativeStateJournalPolicy = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "NativeStateJournal", 1, iniPath) != 0;
    g_properShadersInspectEffects = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "InspectEffects", 0, iniPath) != 0;
    g_properShadersGenericDirectDryRun = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "GenericDirectDryRun", 0, iniPath) != 0;
    g_properShadersGenericDirect = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "GenericDirect", 0, iniPath) != 0;
    g_properShadersGenericPassLite = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "GenericDirectPassLite", 0, iniPath) != 0;
    g_properShadersTraceStabilityProbe = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "TraceStabilityProbe", 0, iniPath) != 0;
    g_properShadersJournalProbe = GetPrivateProfileIntA(
        "ProperShadersEffectOptimization", "JournalProbe", 0, iniPath) != 0;
    Log("effectopt: genericDirect={} genericDirectDryRun={} passLite={} traceStabProbe={} journalProbe={}",
        g_properShadersGenericDirect ? 1 : 0,
        g_properShadersGenericDirectDryRun ? 1 : 0,
        g_properShadersGenericPassLite ? 1 : 0,
        g_properShadersTraceStabilityProbe ? 1 : 0,
        g_properShadersJournalProbe ? 1 : 0);
    g_forceDeviceWrap = GetPrivateProfileIntA(
        "ProxyChain", "ForceDeviceWrap", 0, iniPath) != 0;
    Log("effectopt: nativeStateJournal={} forceDeviceWrap={} inspectEffects={}",
        g_properShadersNativeStateJournalPolicy ? 1 : 0,
        g_forceDeviceWrap ? 1 : 0,
        g_properShadersInspectEffects ? 1 : 0);
    g_enableProperShadersEffectOptimization =
        g_enableProperShadersEffectOptimization &&
        g_properShadersEffectOptimizationStateJournal;
    g_properShadersEffectOptimizationNoSaveState =
        g_enableProperShadersEffectOptimization;
    g_properShadersEffectOptimizationAutoBenchmark = false;
    if (g_enableProperShadersEffectOptimization && g_enableProperShadersEffectProfile) {
        Log("effectprofile: disabled while incremental state journal hooks are active");
        g_enableProperShadersEffectProfile = false;
    }
    Log("effectopt: enable={} stateJournal={} generalStateJournal={} batching={} skipDuplicateMatrices={} skipDuplicateParameters={} directConstants={} autoBenchmark=0 failPolicy=per-effect-baseline",
        g_enableProperShadersEffectOptimization ? 1 : 0,
        g_properShadersEffectOptimizationStateJournal ? 1 : 0,
        g_properShadersGeneralStateJournal ? 1 : 0,
        g_properShadersEffectBatching ? 1 : 0,
        g_properShadersSkipDuplicateMatrices ? 1 : 0,
        g_properShadersSkipDuplicateParameters ? 1 : 0,
        g_properShadersDirectConstants ? 1 : 0);

    g_enableD3D9Optimizer = GetPrivateProfileIntA("D3D9Optimizer", "Enable", 0, iniPath) != 0;
    g_skipRedundantShaders = GetPrivateProfileIntA("D3D9Optimizer", "SkipRedundantShaders", 1, iniPath) != 0;
    g_skipRedundantConstants = GetPrivateProfileIntA("D3D9Optimizer", "SkipRedundantConstants", 1, iniPath) != 0;
    Log("d3d9optimizer: enable={} shaders={} constants={}",
        g_enableD3D9Optimizer ? 1 : 0,
        g_skipRedundantShaders ? 1 : 0,
        g_skipRedundantConstants ? 1 : 0);

    ApplyLimitCoordinator(iniPath);

    g_enablePostFxHost = GetPrivateProfileIntA("PostFX", "EnableHost", 0, iniPath) != 0;
    FormatTo(g_postFxIniPath, sizeof(g_postFxIniPath), "{}", iniPath);
    Log("postfx: host {} initialization={}",
        g_enablePostFxHost ? "enabled" : "disabled",
        g_enablePostFxHost ? "deferred-to-Direct3DCreate9" : "none");
    LoadProxyChainConfig(iniPath);

    int useDxvk = GetPrivateProfileIntA("Backend", "UseDxvkBackend", 0, iniPath);
    char backendRel[MAX_PATH]{};
    GetPrivateProfileStringA("Backend", "DxvkBackendDir", "dxvk", backendRel, sizeof(backendRel), iniPath);

    if (!useDxvk) {
        Log("backend: DXVK backend disabled");
        return;
    }

    if (strchr(backendRel, ':') || (backendRel[0] == '\\' && backendRel[1] == '\\')) {
        FormatTo(g_dxvkBackendDir, sizeof(g_dxvkBackendDir), "{}", backendRel);
    } else {
        FormatTo(g_dxvkBackendDir, sizeof(g_dxvkBackendDir), "{}\\{}", gameDir, backendRel);
    }

    char dxvkD3D9[MAX_PATH]{};
    FormatTo(dxvkD3D9, sizeof(dxvkD3D9), "{}\\d3d9.dll", g_dxvkBackendDir);

    if (!FileExistsA(dxvkD3D9)) {
        Log("backend: DXVK requested but missing {}", dxvkD3D9);
        g_dxvkBackendDir[0] = '\0';
        return;
    }

    g_useDxvkBackend = true;
    Log("backend: DXVK enabled dir={}", g_dxvkBackendDir);
}

static void EnsurePerformanceConfigLoaded()
{
    std::lock_guard<std::mutex> lock(g_performanceConfigMutex);
    if (g_performanceConfigLoaded)
    {
        return;
    }

    g_performanceRuntimeConfig = BridgePerformance::LoadAdapterConfig(
        g_performanceIniPath,
        g_gameDir);
    g_performanceConfigLoaded = true;
    Log(
        "performance-adapters: loaded enabled={} adapters={} warnings={} "
        "providers=%d snapshots=%d",
        g_performanceRuntimeConfig.enabled ? 1 : 0,
        static_cast<unsigned>(g_performanceRuntimeConfig.registry.adapters.size()),
        static_cast<unsigned>(g_performanceRuntimeConfig.warnings.size()),
        g_performanceRuntimeConfig.enableProviders ? 1 : 0,
        g_performanceRuntimeConfig.includeConfigSnapshots ? 1 : 0);
    for (const BridgePerformance::AdapterWarning& warning :
         g_performanceRuntimeConfig.warnings)
    {
        Log(
            "performance-adapters: warning code={} adapter={} detail={}",
            warning.code.c_str(),
            warning.adapter.c_str(),
            warning.detail.c_str());
    }
}

extern "C" UINT WINAPI Bridge_GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize)
{
    if (!g_useDxvkBackend || !g_dxvkBackendDir[0]) {
        return GetSystemDirectoryA(lpBuffer, uSize);
    }

    UINT required = static_cast<UINT>(strlen(g_dxvkBackendDir));
    if (lpBuffer && uSize > 0) {
        strncpy(lpBuffer, g_dxvkBackendDir, uSize);
        lpBuffer[uSize - 1] = '\0';
    }
    return required;
}

static bool PatchImportByName(HMODULE module, const char* importedModule, const char* importedName, void* replacement)
{
    if (!module || !importedModule || !importedName || !replacement) return false;

    auto base = reinterpret_cast<uintptr_t>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return false;

    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dllName, importedModule) != 0) continue;

        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto origThunk = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
            : thunk;

        for (; origThunk->u1.AddressOfData; ++origThunk, ++thunk) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), importedName) != 0) continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &oldProtect)) {
                Log("backend: VirtualProtect failed for {}!{} err={}", importedModule, importedName, GetLastError());
                return false;
            }

            thunk->u1.Function = reinterpret_cast<uintptr_t>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function, sizeof(thunk->u1.Function));
            Log("backend: patched {}!{} in module 0x{:08X}", importedModule, importedName, reinterpret_cast<std::uintptr_t>(module));
            return true;
        }
    }

    Log("backend: import not found {}!{} in module 0x{:08X}", importedModule, importedName, reinterpret_cast<std::uintptr_t>(module));
    return false;
}

static void PatchPSProxyBackend()
{
    if (!g_useDxvkBackend || !g_psProxy) return;

    bool patched = PatchImportByName(g_psProxy, "KERNEL32.dll", "GetSystemDirectoryA",
        reinterpret_cast<void*>(&Bridge_GetSystemDirectoryA));
    if (!patched) {
        g_useDxvkBackend = false;
        Log("backend: disabled because d3d9_ps.dll import patch failed");
    }
}

#ifdef BRIDGE_D3D9_BACKEND_TRACE
static constexpr size_t kBackendAlphaRingCapacity = 32768;

enum BackendTextureAlphaClass : uint32_t
{
    BACKEND_ALPHA_OPAQUE = 0,
    BACKEND_ALPHA_TRANSPARENT = 1,
    BACKEND_ALPHA_UNKNOWN = 2,
    BACKEND_ALPHA_UNPROBED = 3,
};

struct BackendTextureInfo
{
    D3DFORMAT format = D3DFMT_UNKNOWN;
    D3DPOOL pool = D3DPOOL_DEFAULT;
    DWORD usage = 0;
    UINT width = 0;
    UINT height = 0;
    UINT levels = 0;
    BackendTextureAlphaClass alphaClass = BACKEND_ALPHA_OPAQUE;
};

struct BackendAlphaPathState
{
    UINT frame = UINT32_MAX;
    bool submittedOpaque = false;
    uint64_t signature = 0;
};

struct BackendAlphaDrawRecord
{
    uint64_t sequence = 0;
    uint64_t vertexShaderHash = 0;
    uint64_t pixelShaderHash = 0;
    uint64_t pixelConstantEpochHash = 0;
    DWORD tick = 0;
    UINT frame = 0;
    UINT drawInFrame = 0;
    uintptr_t callerBase = 0;
    uintptr_t callerRva = 0;
    uintptr_t texture = 0;
    uintptr_t vertexShader = 0;
    uintptr_t pixelShader = 0;
    UINT pixelShaderHasTexkill = 0;
    uintptr_t renderTarget = 0;
    uintptr_t depthSurface = 0;
    DWORD textureFormat = 0;
    UINT textureWidth = 0;
    UINT textureHeight = 0;
    UINT textureLevels = 0;
    DWORD texturePool = 0;
    DWORD textureUsage = 0;
    DWORD alphaTest = 0;
    DWORD alphaRef = 0;
    DWORD alphaFunc = 0;
    DWORD alphaBlend = 0;
    DWORD srcBlend = 0;
    DWORD destBlend = 0;
    DWORD blendOp = 0;
    DWORD separateAlpha = 0;
    DWORD srcBlendAlpha = 0;
    DWORD destBlendAlpha = 0;
    DWORD blendOpAlpha = 0;
    DWORD zEnable = 0;
    DWORD zWrite = 0;
    DWORD zFunc = 0;
    DWORD colorWrite = 0;
    DWORD stencilEnable = 0;
    DWORD stencilFunc = 0;
    DWORD stencilPass = 0;
    DWORD stencilFail = 0;
    DWORD stencilZFail = 0;
    DWORD cullMode = 0;
    DWORD fogEnable = 0;
    DWORD colorOp = 0;
    DWORD colorArg1 = 0;
    DWORD colorArg2 = 0;
    DWORD alphaOp = 0;
    DWORD alphaArg1 = 0;
    DWORD alphaArg2 = 0;
    DWORD addressU = 0;
    DWORD addressV = 0;
    DWORD minFilter = 0;
    DWORD magFilter = 0;
    DWORD mipFilter = 0;
    DWORD mipLodBias = 0;
    DWORD maxAnisotropy = 0;
    DWORD fvf = 0;
    UINT kind = 0;
    UINT primitiveType = 0;
    UINT primitiveCount = 0;
    UINT vertexCount = 0;
    UINT vertexStride = 0;
    UINT phaseAlternate = 0;
    UINT phaseDepth = 0;
    UINT phaseFading = 0;
    UINT alphaClass = 0;
    UINT anomalyFlags = 0;
};

static uint64_t BackendHashBytes(const void* data, size_t size, uint64_t seed = 1469598103934665603ull)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool BackendFormatMayContainAlpha(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_A8P8:
    case D3DFMT_A8L8:
    case D3DFMT_A4L4:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A32B32G32R32F:
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        return true;
    default:
        return false;
    }
}

static uint16_t BackendReadU16(const uint8_t* data)
{
    uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

static uint32_t BackendReadU32(const uint8_t* data)
{
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

static bool BackendDXT5BlockHasTransparency(const uint8_t* block)
{
    uint8_t palette[8]{};
    palette[0] = block[0];
    palette[1] = block[1];
    if (palette[0] > palette[1]) {
        for (UINT i = 1; i <= 6; ++i) {
            palette[i + 1] = static_cast<uint8_t>(
                ((7 - i) * palette[0] + i * palette[1]) / 7);
        }
    } else {
        for (UINT i = 1; i <= 4; ++i) {
            palette[i + 1] = static_cast<uint8_t>(
                ((5 - i) * palette[0] + i * palette[1]) / 5);
        }
        palette[6] = 0;
        palette[7] = 255;
    }

    uint64_t indices = 0;
    for (UINT i = 0; i < 6; ++i) {
        indices |= static_cast<uint64_t>(block[2 + i]) << (i * 8);
    }
    for (UINT i = 0; i < 16; ++i) {
        if (palette[(indices >> (i * 3)) & 7] != 255) return true;
    }
    return false;
}

static BackendTextureAlphaClass BackendProbeTextureAlpha(
    IDirect3DTexture9* texture,
    const D3DSURFACE_DESC& desc)
{
    if (!BackendFormatMayContainAlpha(desc.Format)) return BACKEND_ALPHA_OPAQUE;
    if (!texture || (desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        return BACKEND_ALPHA_UNKNOWN;
    }

    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)) || !locked.pBits) {
        return BACKEND_ALPHA_UNKNOWN;
    }

    bool transparent = false;
    const uint8_t* pixels = static_cast<const uint8_t*>(locked.pBits);
    const UINT width = desc.Width;
    const UINT height = desc.Height;

    if (desc.Format == D3DFMT_DXT1 || desc.Format == D3DFMT_DXT2 ||
        desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT4 ||
        desc.Format == D3DFMT_DXT5) {
        const UINT blockBytes = desc.Format == D3DFMT_DXT1 ? 8u : 16u;
        const UINT blocksX = (width + 3) / 4;
        const UINT blocksY = (height + 3) / 4;
        for (UINT y = 0; y < blocksY && !transparent; ++y) {
            const uint8_t* row = pixels + static_cast<size_t>(y) * locked.Pitch;
            for (UINT x = 0; x < blocksX && !transparent; ++x) {
                const uint8_t* block = row + static_cast<size_t>(x) * blockBytes;
                if (desc.Format == D3DFMT_DXT1) {
                    const uint16_t color0 = BackendReadU16(block);
                    const uint16_t color1 = BackendReadU16(block + 2);
                    if (color0 <= color1) {
                        const uint32_t indices = BackendReadU32(block + 4);
                        for (UINT i = 0; i < 16; ++i) {
                            if (((indices >> (i * 2)) & 3) == 3) {
                                transparent = true;
                                break;
                            }
                        }
                    }
                } else if (desc.Format == D3DFMT_DXT2 || desc.Format == D3DFMT_DXT3) {
                    for (UINT i = 0; i < 8; ++i) {
                        if ((block[i] & 0x0f) != 0x0f || (block[i] >> 4) != 0x0f) {
                            transparent = true;
                            break;
                        }
                    }
                } else {
                    transparent = BackendDXT5BlockHasTransparency(block);
                }
            }
        }
    } else {
        for (UINT y = 0; y < height && !transparent; ++y) {
            const uint8_t* row = pixels + static_cast<size_t>(y) * locked.Pitch;
            for (UINT x = 0; x < width && !transparent; ++x) {
                switch (desc.Format) {
                case D3DFMT_A8R8G8B8:
                case D3DFMT_A8B8G8R8:
                    transparent = row[x * 4 + 3] != 255;
                    break;
                case D3DFMT_A1R5G5B5:
                    transparent = (BackendReadU16(row + x * 2) & 0x8000) == 0;
                    break;
                case D3DFMT_A4R4G4B4:
                    transparent = (BackendReadU16(row + x * 2) & 0xf000) != 0xf000;
                    break;
                case D3DFMT_A8:
                    transparent = row[x] != 255;
                    break;
                case D3DFMT_A8R3G3B2:
                case D3DFMT_A8L8:
                    transparent = row[x * 2 + 1] != 255;
                    break;
                case D3DFMT_A4L4:
                    transparent = (row[x] & 0xf0) != 0xf0;
                    break;
                case D3DFMT_A2B10G10R10:
                case D3DFMT_A2R10G10B10:
                    transparent = (BackendReadU32(row + x * 4) & 0xc0000000u) != 0xc0000000u;
                    break;
                case D3DFMT_A16B16G16R16:
                case D3DFMT_A16B16G16R16F:
                    transparent = BackendReadU16(row + x * 8 + 6) != 0xffff;
                    break;
                case D3DFMT_A32B32G32R32F:
                    transparent = BackendReadU32(row + x * 16 + 12) != 0x3f800000u;
                    break;
                default:
                    transparent = false;
                    break;
                }
            }
        }
    }

    texture->UnlockRect(0);
    return transparent ? BACKEND_ALPHA_TRANSPARENT : BACKEND_ALPHA_OPAQUE;
}
#endif

class BridgeDirect3DDevice9 final : public IDirect3DDevice9
{
public:
    explicit BridgeDirect3DDevice9(IDirect3DDevice9* inner)
        : m_inner(inner), m_refs(1), m_renderThreadId(GetCurrentThreadId())
    {
        Log("postfx: wrapped IDirect3DDevice9 0x{:08X} -> 0x{:08X}", reinterpret_cast<std::uintptr_t>(inner), reinterpret_cast<std::uintptr_t>(this));
        m_vulkanHost = AttachVulkanHost(inner);
        m_lastStatsTick = GetTickCount();
        m_lastStats = m_stats;
        GetPeImageInfo(GetModuleHandleA(nullptr), m_gameImageBase, m_gameImageSize,
            m_gamePreferredBase);
        if (g_enableD3D9CallsiteProfile) {
            Log("d3d9callsites: gameImage base={:08X} size=0x{:08X} preferred={:08X}", reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(m_gameImageBase)),
                static_cast<unsigned>(m_gameImageSize), reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(m_gamePreferredBase)));
            RefreshCallsiteRenderTarget();
        }
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        m_backendAlphaRing = new (std::nothrow) BackendAlphaDrawRecord[kBackendAlphaRingCapacity];
        Log("backendtrace: alpha ring allocated records={} bytes={} ptr={:08X}",
            static_cast<unsigned>(kBackendAlphaRingCapacity),
            static_cast<unsigned long long>(sizeof(BackendAlphaDrawRecord) * kBackendAlphaRingCapacity), reinterpret_cast<std::uintptr_t>(m_backendAlphaRing));
#endif
    }

    ~BridgeDirect3DDevice9()
    {
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        DumpBackendTrace("device-destroyed");
        delete[] m_backendAlphaRing;
        m_backendAlphaRing = nullptr;
#endif
        FinishCallsiteProfile("device-destroyed");
        FinishDrawTrace("device-destroyed");
        FinishProperShadersEffectProfile("device-destroyed");
        FinishProperShadersStateAttribution("device-destroyed");
        DetachVulkanHost(m_vulkanHost);
        for (auto& plugin : g_plugins) {
            SafePluginCall("OnReleaseDevice", plugin.onReleaseDevice, m_inner);
        }
        if (m_inner) {
            m_inner->Release();
            m_inner = nullptr;
        }
        Log("postfx: device wrapper destroyed");
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override
    {
        if (!ppvObj) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
            *ppvObj = static_cast<IDirect3DDevice9*>(this);
            AddRef();
            return S_OK;
        }
        return m_inner->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_refs.fetch_add(1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG refs = m_refs.fetch_sub(1) - 1;
        if (refs == 0) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return m_inner->TestCooperativeLevel(); }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return m_inner->GetAvailableTextureMem(); }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return m_inner->EvictManagedResources(); }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override { return m_inner->GetDirect3D(ppD3D9); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override { return m_inner->GetDeviceCaps(pCaps); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override { return m_inner->GetDisplayMode(iSwapChain, pMode); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) override { return m_inner->GetCreationParameters(pParameters); }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override { return m_inner->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
    void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override { m_inner->SetCursorPosition(X, Y, Flags); }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { return m_inner->ShowCursor(bShow); }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override { return m_inner->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override { return m_inner->GetSwapChain(iSwapChain, pSwapChain); }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return m_inner->GetNumberOfSwapChains(); }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override
    {
        if (g_enableD3D9Stats) ++m_stats.reset;
        FinishCallsiteProfile("device-reset");
        FinishDrawTrace("device-reset");
        FinishProperShadersEffectProfile("device-reset");
        FinishProperShadersStateAttribution("device-reset");
        ResetTrackedState();
        SuspendVulkanHost(m_vulkanHost);
        for (auto& plugin : g_plugins) {
            SafePluginCall("OnResetBefore", plugin.onResetBefore, m_inner, pPresentationParameters);
        }
        HRESULT hr = m_inner->Reset(pPresentationParameters);
        for (auto& plugin : g_plugins) {
            SafePluginCall("OnResetAfter", plugin.onResetAfter, m_inner, hr, pPresentationParameters);
        }
        if (SUCCEEDED(hr)) ResumeVulkanHost(m_vulkanHost);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override
    {
        m_renderThreadId = GetCurrentThreadId();
        MaybeArmCpuHotspotProfile();
        for (auto& plugin : g_plugins) {
            SafePluginCall("OnPresentBefore", plugin.onPresentBefore, m_inner, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        }
        HRESULT hr = m_inner->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        if (g_cpuHotspotActive.load() != 0) {
            g_cpuHotspotPresents.fetch_add(1);
        }
        if (g_enableD3D9Stats) {
            ++m_stats.present;
            MaybeLogStats();
        }
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        BackendOnPresent();
#else
        if (m_drawTraceActive) {
            FinishDrawTrace("present");
        }
        MaybeArmDrawTrace();
#endif
        OnCallsitePresent();
        OnProperShadersEffectProfilePresent();
        PollProperShadersStateAttribution(m_stateAttributionTriggerWasDown, true);
        OnProperShadersEffectOptimizationPresent();
        ResetTrackedState();
        for (auto& plugin : g_plugins) {
            SafePluginCall("OnPresentAfter", plugin.onPresentAfter, m_inner, hr);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override { return m_inner->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override { return m_inner->GetRasterStatus(iSwapChain, pRasterStatus); }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override { return m_inner->SetDialogBoxMode(bEnableDialogs); }
    void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) override { m_inner->SetGammaRamp(iSwapChain, Flags, pRamp); }
    void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override { m_inner->GetGammaRamp(iSwapChain, pRamp); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override
    {
        if (g_enableD3D9Stats) ++m_stats.createTexture;
        const HRESULT hr = m_inner->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        if (SUCCEEDED(hr) && ppTexture && *ppTexture) {
            TrackBackendTexture(*ppTexture);
        }
#endif
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override { return m_inner->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override { return m_inner->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override { if (g_enableD3D9Stats) ++m_stats.createVertexBuffer; return m_inner->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override { if (g_enableD3D9Stats) ++m_stats.createIndexBuffer; return m_inner->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { if (g_enableD3D9Stats) ++m_stats.createRenderTarget; return m_inner->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { if (g_enableD3D9Stats) ++m_stats.createDepthStencilSurface; return m_inner->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, const POINT* pDestPoint) override { return m_inner->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override { return m_inner->UpdateTexture(pSourceTexture, pDestinationTexture); }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override { return m_inner->GetRenderTargetData(pRenderTarget, pDestSurface); }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override { return m_inner->GetFrontBufferData(iSwapChain, pDestSurface); }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) override { return m_inner->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter); }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurface, const RECT* pRect, D3DCOLOR color) override { return m_inner->ColorFill(pSurface, pRect, color); }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return m_inner->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override
    {
        if (g_enableD3D9Stats) ++m_stats.setRenderTarget;
        const HRESULT hr = m_inner->SetRenderTarget(RenderTargetIndex, pRenderTarget);
        if (SUCCEEDED(hr) && RenderTargetIndex == 0 && g_enableD3D9CallsiteProfile) {
            TrackCallsiteRenderTarget(pRenderTarget);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override { return m_inner->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override { if (g_enableD3D9Stats) ++m_stats.setDepthStencilSurface; return m_inner->SetDepthStencilSurface(pNewZStencil); }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override { return m_inner->GetDepthStencilSurface(ppZStencilSurface); }
    HRESULT STDMETHODCALLTYPE BeginScene() override { if (g_enableD3D9Stats) ++m_stats.beginScene; return m_inner->BeginScene(); }

    HRESULT STDMETHODCALLTYPE EndScene() override
    {
        if (g_enableD3D9Stats) ++m_stats.endScene;
        for (auto& plugin : g_plugins) {
            SafePluginCall("OnEndScene", plugin.onEndScene, m_inner);
        }
        return m_inner->EndScene();
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override { if (g_enableD3D9Stats) ++m_stats.clear; return m_inner->Clear(Count, pRects, Flags, Color, Z, Stencil); }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override { if (g_enableD3D9Stats) ++m_stats.setTransform; return m_inner->SetTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override { return m_inner->GetTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override { return m_inner->MultiplyTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pViewport) override { return m_inner->SetViewport(pViewport); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport) override { return m_inner->GetViewport(pViewport); }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pMaterial) override { if (g_enableD3D9Stats) ++m_stats.setMaterial; return m_inner->SetMaterial(pMaterial); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial) override { return m_inner->GetMaterial(pMaterial); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9* pLight) override { if (g_enableD3D9Stats) ++m_stats.setLight; return m_inner->SetLight(Index, pLight); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9* pLight) override { return m_inner->GetLight(Index, pLight); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override { if (g_enableD3D9Stats) ++m_stats.lightEnable; return m_inner->LightEnable(Index, Enable); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL* pEnable) override { return m_inner->GetLightEnable(Index, pEnable); }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, const float* pPlane) override { return m_inner->SetClipPlane(Index, pPlane); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float* pPlane) override { return m_inner->GetClipPlane(Index, pPlane); }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override
    {
        if (g_enableD3D9Stats) {
            ++m_stats.setRenderState;
            if (TrackRenderState(State, Value)) ++m_stats.redundantSetRenderState;
        }
        return m_inner->SetRenderState(State, Value);
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override { return m_inner->GetRenderState(State, pValue); }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override { return m_inner->CreateStateBlock(Type, ppSB); }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return m_inner->BeginStateBlock(); }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override { return m_inner->EndStateBlock(ppSB); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) override { return m_inner->SetClipStatus(pClipStatus); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override { return m_inner->GetClipStatus(pClipStatus); }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override { return m_inner->GetTexture(Stage, ppTexture); }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) override
    {
        if (g_enableD3D9Stats) {
            ++m_stats.setTexture;
            if (TrackTexture(Stage, pTexture)) ++m_stats.redundantSetTexture;
        }
        return m_inner->SetTexture(Stage, pTexture);
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override { return m_inner->GetTextureStageState(Stage, Type, pValue); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override { if (g_enableD3D9Stats) ++m_stats.setTextureStageState; return m_inner->SetTextureStageState(Stage, Type, Value); }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override { return m_inner->GetSamplerState(Sampler, Type, pValue); }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override { if (g_enableD3D9Stats) ++m_stats.setSamplerState; return m_inner->SetSamplerState(Sampler, Type, Value); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override { return m_inner->ValidateDevice(pNumPasses); }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) override { return m_inner->SetPaletteEntries(PaletteNumber, pEntries); }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override { return m_inner->GetPaletteEntries(PaletteNumber, pEntries); }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { return m_inner->SetCurrentTexturePalette(PaletteNumber); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* PaletteNumber) override { return m_inner->GetCurrentTexturePalette(PaletteNumber); }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pRect) override { return m_inner->SetScissorRect(pRect); }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect) override { return m_inner->GetScissorRect(pRect); }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { return m_inner->SetSoftwareVertexProcessing(bSoftware); }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return m_inner->GetSoftwareVertexProcessing(); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { return m_inner->SetNPatchMode(nSegments); }
    float STDMETHODCALLTYPE GetNPatchMode() override { return m_inner->GetNPatchMode(); }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override
    {
        if (g_enableD3D9Stats) { ++m_stats.drawPrimitive; m_stats.primitives += PrimitiveCount; }
        ProfileDrawCall(1, PrimitiveCount);
        void* returnAddress = _ReturnAddress();
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        BackendTraceDraw(1, PrimitiveType, PrimitiveCount, 0, StartVertex, 0, 0, returnAddress);
#endif
        TraceDraw("DP", PrimitiveType, PrimitiveCount, 0, StartVertex, 0, 0, 0, returnAddress);
        return m_inner->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override
    {
        if (g_enableD3D9Stats) { ++m_stats.drawIndexedPrimitive; m_stats.primitives += primCount; }
        ProfileDrawCall(2, primCount);
        void* returnAddress = _ReturnAddress();
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        BackendTraceDraw(2, Type, primCount, BaseVertexIndex, MinVertexIndex, NumVertices, 0, returnAddress);
#endif
        TraceDraw("DIP", Type, primCount, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, 0, returnAddress);
        return m_inner->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override
    {
        if (g_enableD3D9Stats) { ++m_stats.drawPrimitiveUP; m_stats.primitives += PrimitiveCount; }
        ProfileDrawCall(3, PrimitiveCount);
        void* returnAddress = _ReturnAddress();
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        BackendTraceDraw(3, PrimitiveType, PrimitiveCount, 0, 0, 0, VertexStreamZeroStride, returnAddress);
#endif
        TraceDraw("DPUP", PrimitiveType, PrimitiveCount, 0, 0, 0, 0, VertexStreamZeroStride, returnAddress);
        return m_inner->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override
    {
        if (g_enableD3D9Stats) { ++m_stats.drawIndexedPrimitiveUP; m_stats.primitives += PrimitiveCount; }
        ProfileDrawCall(4, PrimitiveCount);
        void* returnAddress = _ReturnAddress();
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        BackendTraceDraw(4, PrimitiveType, PrimitiveCount, 0, MinVertexIndex, NumVertices, VertexStreamZeroStride, returnAddress);
#endif
        TraceDraw("DIPUP", PrimitiveType, PrimitiveCount, 0, MinVertexIndex, NumVertices,
            static_cast<UINT>(IndexDataFormat), VertexStreamZeroStride, returnAddress);
        return m_inner->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount,
            pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override { return m_inner->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override { return m_inner->CreateVertexDeclaration(pVertexElements, ppDecl); }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override { if (g_enableD3D9Stats) ++m_stats.setVertexDeclaration; return m_inner->SetVertexDeclaration(pDecl); }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override { return m_inner->GetVertexDeclaration(ppDecl); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { if (g_enableD3D9Stats) ++m_stats.setFVF; return m_inner->SetFVF(FVF); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override { return m_inner->GetFVF(pFVF); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) override
    {
        if (g_enableD3D9Stats) ++m_stats.createVertexShader;
        const HRESULT hr = m_inner->CreateVertexShader(pFunction, ppShader);
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        if (SUCCEEDED(hr) && ppShader && *ppShader) {
            TrackBackendVertexShader(*ppShader);
        }
#endif
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override
    {
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        m_backendCurrentVertexShaderHash = LookupBackendVertexShaderHash(pShader);
#endif
        const bool shouldTrack = g_enableD3D9Stats || (g_enableD3D9Optimizer && g_skipRedundantShaders);
        const bool redundant = shouldTrack && m_vertexShaderSeen && m_vertexShader == pShader;
        if (g_enableD3D9Stats) {
            ++m_stats.setVertexShader;
            if (redundant) ++m_stats.redundantSetVertexShader;
        }
        if (shouldTrack) {
            m_vertexShader = pShader;
            m_vertexShaderSeen = true;
        }
        if (g_enableD3D9Optimizer && g_skipRedundantShaders && redundant) return D3D_OK;
        const HRESULT hr = m_inner->SetVertexShader(pShader);
        if (FAILED(hr)) ResetTrackedState();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override { return m_inner->GetVertexShader(ppShader); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override
    {
        const bool shouldTrack = g_enableD3D9Stats || (g_enableD3D9Optimizer && g_skipRedundantConstants);
        if (g_enableD3D9Stats) {
            ++m_stats.setVertexShaderConstantF;
            m_stats.vertexShaderConstantFVectors += Vector4fCount;
        }
        const bool redundant = shouldTrack && TrackFloatConstants(
            m_vertexShaderConstants, m_vertexShaderConstantSeen,
            kTrackedVertexShaderConstantCount, StartRegister, pConstantData, Vector4fCount,
            g_enableD3D9Stats ? &m_stats.redundantVertexShaderConstantFVectors : nullptr);
        if (g_enableD3D9Stats && redundant) ++m_stats.redundantSetVertexShaderConstantF;
        if (g_enableD3D9Optimizer && g_skipRedundantConstants && redundant) return D3D_OK;
        const HRESULT hr = m_inner->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
        if (FAILED(hr)) ResetTrackedState();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return m_inner->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override { return m_inner->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return m_inner->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override { return m_inner->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return m_inner->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override { if (g_enableD3D9Stats) ++m_stats.setStreamSource; return m_inner->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) override { return m_inner->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override { return m_inner->SetStreamSourceFreq(StreamNumber, Setting); }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) override { return m_inner->GetStreamSourceFreq(StreamNumber, pSetting); }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData) override { if (g_enableD3D9Stats) ++m_stats.setIndices; return m_inner->SetIndices(pIndexData); }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData) override { return m_inner->GetIndices(ppIndexData); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) override
    {
        if (g_enableD3D9Stats) ++m_stats.createPixelShader;
        const HRESULT hr = m_inner->CreatePixelShader(pFunction, ppShader);
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        if (SUCCEEDED(hr) && ppShader && *ppShader) {
            TrackBackendPixelShader(*ppShader);
        }
#endif
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override
    {
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        m_backendCurrentPixelShaderHash = LookupBackendPixelShaderHash(pShader);
        m_backendPixelConstantEpochHash = BackendHashBytes(
            &m_backendCurrentPixelShaderHash, sizeof(m_backendCurrentPixelShaderHash));
#endif
        const bool shouldTrack = g_enableD3D9Stats || (g_enableD3D9Optimizer && g_skipRedundantShaders);
        const bool redundant = shouldTrack && m_pixelShaderSeen && m_pixelShader == pShader;
        if (g_enableD3D9Stats) {
            ++m_stats.setPixelShader;
            if (redundant) ++m_stats.redundantSetPixelShader;
        }
        if (shouldTrack) {
            m_pixelShader = pShader;
            m_pixelShaderSeen = true;
        }
        if (g_enableD3D9Optimizer && g_skipRedundantShaders && redundant) return D3D_OK;
        const HRESULT hr = m_inner->SetPixelShader(pShader);
        if (FAILED(hr)) ResetTrackedState();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override { return m_inner->GetPixelShader(ppShader); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override
    {
#ifdef BRIDGE_D3D9_BACKEND_TRACE
        if (pConstantData && Vector4fCount) {
            m_backendPixelConstantEpochHash = BackendHashBytes(
                &StartRegister, sizeof(StartRegister), m_backendPixelConstantEpochHash);
            m_backendPixelConstantEpochHash = BackendHashBytes(
                &Vector4fCount, sizeof(Vector4fCount), m_backendPixelConstantEpochHash);
            m_backendPixelConstantEpochHash = BackendHashBytes(
                pConstantData, static_cast<size_t>(Vector4fCount) * 4 * sizeof(float),
                m_backendPixelConstantEpochHash);
        }
#endif
        const bool shouldTrack = g_enableD3D9Stats || (g_enableD3D9Optimizer && g_skipRedundantConstants);
        if (g_enableD3D9Stats) {
            ++m_stats.setPixelShaderConstantF;
            m_stats.pixelShaderConstantFVectors += Vector4fCount;
        }
        const bool redundant = shouldTrack && TrackFloatConstants(
            m_pixelShaderConstants, m_pixelShaderConstantSeen,
            kTrackedPixelShaderConstantCount, StartRegister, pConstantData, Vector4fCount,
            g_enableD3D9Stats ? &m_stats.redundantPixelShaderConstantFVectors : nullptr);
        if (g_enableD3D9Stats && redundant) ++m_stats.redundantSetPixelShaderConstantF;
        if (g_enableD3D9Optimizer && g_skipRedundantConstants && redundant) return D3D_OK;
        const HRESULT hr = m_inner->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
        if (FAILED(hr)) ResetTrackedState();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return m_inner->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override { return m_inner->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return m_inner->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override { return m_inner->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return m_inner->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) override { return m_inner->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) override { return m_inner->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return m_inner->DeletePatch(Handle); }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override { return m_inner->CreateQuery(Type, ppQuery); }

private:
    static constexpr UINT kTrackedRenderStateCount = 256;
    static constexpr UINT kTrackedTextureCount = 20;
    static constexpr UINT kTrackedVertexShaderConstantCount = 256;
    static constexpr UINT kTrackedPixelShaderConstantCount = 224;

    static int GetTrackedTextureSlot(DWORD stage)
    {
        if (stage < 16) return static_cast<int>(stage);
        if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) {
            return 16 + static_cast<int>(stage - D3DVERTEXTEXTURESAMPLER0);
        }
        return -1;
    }

    bool TrackRenderState(D3DRENDERSTATETYPE state, DWORD value)
    {
        const UINT index = static_cast<UINT>(state);
        if (index >= kTrackedRenderStateCount) return false;
        const bool redundant = m_renderStateSeen[index] && m_renderStates[index] == value;
        m_renderStates[index] = value;
        m_renderStateSeen[index] = true;
        return redundant;
    }

    bool TrackTexture(DWORD stage, IDirect3DBaseTexture9* texture)
    {
        const int slot = GetTrackedTextureSlot(stage);
        if (slot < 0 || static_cast<UINT>(slot) >= kTrackedTextureCount) return false;
        const bool redundant = m_textureSeen[slot] && m_textures[slot] == texture;
        m_textures[slot] = texture;
        m_textureSeen[slot] = true;
        return redundant;
    }

    static bool TrackFloatConstants(
        float* cache,
        bool* seen,
        UINT maxRegisters,
        UINT startRegister,
        const float* data,
        UINT vectorCount,
        uint64_t* redundantVectors)
    {
        if (!data || vectorCount == 0 || startRegister >= maxRegisters || vectorCount > maxRegisters - startRegister) {
            return false;
        }

        bool allRedundant = true;
        for (UINT i = 0; i < vectorCount; ++i) {
            const UINT reg = startRegister + i;
            float* destination = cache + reg * 4;
            const float* source = data + i * 4;
            const bool vectorRedundant = seen[reg] && std::memcmp(destination, source, sizeof(float) * 4) == 0;
            if (vectorRedundant) {
                if (redundantVectors) ++(*redundantVectors);
            } else {
                allRedundant = false;
                std::memcpy(destination, source, sizeof(float) * 4);
                seen[reg] = true;
            }
        }
        return allRedundant;
    }

#ifdef BRIDGE_D3D9_BACKEND_TRACE
    void TrackBackendTexture(IDirect3DTexture9* texture)
    {
        if (!texture) return;
        D3DSURFACE_DESC desc{};
        if (FAILED(texture->GetLevelDesc(0, &desc))) return;

        BackendTextureInfo info{};
        info.format = desc.Format;
        info.pool = desc.Pool;
        info.usage = desc.Usage;
        info.width = desc.Width;
        info.height = desc.Height;
        info.levels = texture->GetLevelCount();
        info.alphaClass = BackendFormatMayContainAlpha(desc.Format)
            ? BACKEND_ALPHA_UNPROBED
            : BACKEND_ALPHA_OPAQUE;
        m_backendTextures[texture] = info;
    }

    BackendTextureInfo* GetBackendTextureInfo(IDirect3DBaseTexture9* baseTexture)
    {
        if (!baseTexture || baseTexture->GetType() != D3DRTYPE_TEXTURE) return nullptr;
        IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(baseTexture);
        auto it = m_backendTextures.find(texture);
        if (it == m_backendTextures.end()) {
            TrackBackendTexture(texture);
            it = m_backendTextures.find(texture);
            if (it == m_backendTextures.end()) return nullptr;
        }

        BackendTextureInfo& info = it->second;
        if (info.alphaClass == BACKEND_ALPHA_UNPROBED) {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(texture->GetLevelDesc(0, &desc))) {
                info.alphaClass = BackendProbeTextureAlpha(texture, desc);
                ++m_backendTextureProbes;
                if (info.alphaClass == BACKEND_ALPHA_TRANSPARENT) {
                    ++m_backendTransparentTextures;
                } else if (info.alphaClass == BACKEND_ALPHA_UNKNOWN) {
                    ++m_backendUnknownAlphaTextures;
                }
            } else {
                info.alphaClass = BACKEND_ALPHA_UNKNOWN;
                ++m_backendUnknownAlphaTextures;
            }
        }
        return &info;
    }

    template <typename ShaderType>
    uint64_t TrackBackendShader(
        ShaderType* shader,
        const char* prefix,
        std::unordered_map<void*, uint64_t>& hashes)
    {
        if (!shader) return 0;
        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0) {
            hashes[shader] = 0;
            return 0;
        }

        std::vector<uint8_t> bytecode(size);
        if (FAILED(shader->GetFunction(bytecode.data(), &size)) || size == 0) {
            hashes[shader] = 0;
            return 0;
        }

        const uint64_t hash = BackendHashBytes(bytecode.data(), size);
        hashes[shader] = hash;

        char directory[MAX_PATH]{};
        FormatTo(directory, sizeof(directory), "{}\\scripts\\BridgeD3D9.backend-shaders", g_gameDir);
        CreateDirectoryA(directory, nullptr);

        char path[MAX_PATH]{};
        FormatTo(path, sizeof(path), "{}\\{}-{:016X}.bin", directory,
            prefix ? prefix : "shader", static_cast<unsigned long long>(hash));
        if (!FileExistsA(path)) {
            FILE* file = nullptr;
            if (fopen_s(&file, path, "wb") == 0 && file) {
                fwrite(bytecode.data(), 1, size, file);
                fclose(file);
            }
        }
        Log("backendtrace: shader kind={} ptr={:08X} hash={:016X} bytes={}",
            prefix ? prefix : "?", reinterpret_cast<std::uintptr_t>(shader),
            static_cast<unsigned long long>(hash), size);
        return hash;
    }

    void TrackBackendVertexShader(IDirect3DVertexShader9* shader)
    {
        TrackBackendShader(shader, "vs", m_backendVertexShaderHashes);
    }

    void TrackBackendPixelShader(IDirect3DPixelShader9* shader)
    {
        TrackBackendShader(shader, "ps", m_backendPixelShaderHashes);
        if (!shader) return;

        bool hasTexkill = false;
        UINT size = 0;
        if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size >= sizeof(DWORD)) {
            std::vector<DWORD> tokens((size + sizeof(DWORD) - 1) / sizeof(DWORD));
            if (SUCCEEDED(shader->GetFunction(tokens.data(), &size))) {
                const size_t tokenCount = size / sizeof(DWORD);
                for (size_t i = 1; i < tokenCount;) {
                    const DWORD token = tokens[i];
                    const DWORD opcode = token & D3DSI_OPCODE_MASK;
                    if (opcode == D3DSIO_END) break;
                    if (opcode == D3DSIO_COMMENT) {
                        const size_t commentDwords =
                            (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT;
                        i += 1 + commentDwords;
                        continue;
                    }
                    if (opcode == D3DSIO_TEXKILL) {
                        hasTexkill = true;
                        break;
                    }
                    const size_t parameterDwords =
                        (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
                    i += parameterDwords ? 1 + parameterDwords : 1;
                }
            }
        }
        m_backendPixelShaderHasTexkill[shader] = hasTexkill;
    }

    uint64_t LookupBackendVertexShaderHash(IDirect3DVertexShader9* shader)
    {
        if (!shader) return 0;
        auto it = m_backendVertexShaderHashes.find(shader);
        return it == m_backendVertexShaderHashes.end() ? 0 : it->second;
    }

    uint64_t LookupBackendPixelShaderHash(IDirect3DPixelShader9* shader)
    {
        if (!shader) return 0;
        auto it = m_backendPixelShaderHashes.find(shader);
        return it == m_backendPixelShaderHashes.end() ? 0 : it->second;
    }

    bool LookupBackendPixelShaderHasTexkill(IDirect3DPixelShader9* shader)
    {
        if (!shader) return false;
        auto it = m_backendPixelShaderHasTexkill.find(shader);
        return it != m_backendPixelShaderHasTexkill.end() && it->second;
    }

    static void ReadProperShadersPhase(UINT& alternate, UINT& depth, UINT& fading)
    {
        alternate = UINT32_MAX;
        depth = UINT32_MAX;
        fading = UINT32_MAX;
        HMODULE properShaders = GetModuleHandleA("ProperShaders.asi");
        if (!properShaders) properShaders = GetModuleHandleA("propershaders.asi");
        if (!properShaders) return;

        BYTE phaseBytes[11]{};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(properShaders) + 0x153CE4),
                phaseBytes, sizeof(phaseBytes), &bytesRead) || bytesRead != sizeof(phaseBytes)) {
            return;
        }
        uint32_t alternateValue = 0;
        std::memcpy(&alternateValue, phaseBytes, sizeof(alternateValue));
        alternate = alternateValue;
        depth = phaseBytes[9];
        fading = phaseBytes[10];
    }

    void BackendTraceDraw(
        UINT kind,
        D3DPRIMITIVETYPE primitiveType,
        UINT primitiveCount,
        INT,
        UINT,
        UINT vertexCount,
        UINT suppliedStride,
        void* returnAddress)
    {
        ++m_backendDrawInFrame;
        if (!m_backendAlphaRing) return;

        IDirect3DBaseTexture9* texture = nullptr;
        if (FAILED(m_inner->GetTexture(0, &texture)) || !texture) return;
        BackendTextureInfo* textureInfo = GetBackendTextureInfo(texture);
        if (!textureInfo || textureInfo->alphaClass == BACKEND_ALPHA_OPAQUE) {
            texture->Release();
            return;
        }

        auto getRenderState = [this](D3DRENDERSTATETYPE state) {
            DWORD value = UINT32_MAX;
            m_inner->GetRenderState(state, &value);
            return value;
        };
        auto getTextureStageState = [this](D3DTEXTURESTAGESTATETYPE state) {
            DWORD value = UINT32_MAX;
            m_inner->GetTextureStageState(0, state, &value);
            return value;
        };
        auto getSamplerState = [this](D3DSAMPLERSTATETYPE state) {
            DWORD value = UINT32_MAX;
            m_inner->GetSamplerState(0, state, &value);
            return value;
        };

        BackendAlphaDrawRecord record{};
        record.sequence = ++m_backendRecordSequence;
        record.tick = GetTickCount();
        record.frame = m_backendFrame;
        record.drawInFrame = m_backendDrawInFrame;
        record.texture = reinterpret_cast<uintptr_t>(texture);
        record.textureFormat = static_cast<DWORD>(textureInfo->format);
        record.textureWidth = textureInfo->width;
        record.textureHeight = textureInfo->height;
        record.textureLevels = textureInfo->levels;
        record.texturePool = static_cast<DWORD>(textureInfo->pool);
        record.textureUsage = textureInfo->usage;
        record.alphaClass = textureInfo->alphaClass;
        record.kind = kind;
        record.primitiveType = static_cast<UINT>(primitiveType);
        record.primitiveCount = primitiveCount;
        record.vertexCount = vertexCount;
        record.vertexStride = suppliedStride;

        record.alphaTest = getRenderState(D3DRS_ALPHATESTENABLE);
        record.alphaRef = getRenderState(D3DRS_ALPHAREF);
        record.alphaFunc = getRenderState(D3DRS_ALPHAFUNC);
        record.alphaBlend = getRenderState(D3DRS_ALPHABLENDENABLE);
        record.srcBlend = getRenderState(D3DRS_SRCBLEND);
        record.destBlend = getRenderState(D3DRS_DESTBLEND);
        record.blendOp = getRenderState(D3DRS_BLENDOP);
        record.separateAlpha = getRenderState(D3DRS_SEPARATEALPHABLENDENABLE);
        record.srcBlendAlpha = getRenderState(D3DRS_SRCBLENDALPHA);
        record.destBlendAlpha = getRenderState(D3DRS_DESTBLENDALPHA);
        record.blendOpAlpha = getRenderState(D3DRS_BLENDOPALPHA);
        record.zEnable = getRenderState(D3DRS_ZENABLE);
        record.zWrite = getRenderState(D3DRS_ZWRITEENABLE);
        record.zFunc = getRenderState(D3DRS_ZFUNC);
        record.colorWrite = getRenderState(D3DRS_COLORWRITEENABLE);
        record.stencilEnable = getRenderState(D3DRS_STENCILENABLE);
        record.stencilFunc = getRenderState(D3DRS_STENCILFUNC);
        record.stencilPass = getRenderState(D3DRS_STENCILPASS);
        record.stencilFail = getRenderState(D3DRS_STENCILFAIL);
        record.stencilZFail = getRenderState(D3DRS_STENCILZFAIL);
        record.cullMode = getRenderState(D3DRS_CULLMODE);
        record.fogEnable = getRenderState(D3DRS_FOGENABLE);

        record.colorOp = getTextureStageState(D3DTSS_COLOROP);
        record.colorArg1 = getTextureStageState(D3DTSS_COLORARG1);
        record.colorArg2 = getTextureStageState(D3DTSS_COLORARG2);
        record.alphaOp = getTextureStageState(D3DTSS_ALPHAOP);
        record.alphaArg1 = getTextureStageState(D3DTSS_ALPHAARG1);
        record.alphaArg2 = getTextureStageState(D3DTSS_ALPHAARG2);
        record.addressU = getSamplerState(D3DSAMP_ADDRESSU);
        record.addressV = getSamplerState(D3DSAMP_ADDRESSV);
        record.minFilter = getSamplerState(D3DSAMP_MINFILTER);
        record.magFilter = getSamplerState(D3DSAMP_MAGFILTER);
        record.mipFilter = getSamplerState(D3DSAMP_MIPFILTER);
        record.mipLodBias = getSamplerState(D3DSAMP_MIPMAPLODBIAS);
        record.maxAnisotropy = getSamplerState(D3DSAMP_MAXANISOTROPY);
        m_inner->GetFVF(&record.fvf);

        IDirect3DVertexShader9* vertexShader = nullptr;
        IDirect3DPixelShader9* pixelShader = nullptr;
        m_inner->GetVertexShader(&vertexShader);
        m_inner->GetPixelShader(&pixelShader);
        record.vertexShader = reinterpret_cast<uintptr_t>(vertexShader);
        record.pixelShader = reinterpret_cast<uintptr_t>(pixelShader);
        record.vertexShaderHash = LookupBackendVertexShaderHash(vertexShader);
        record.pixelShaderHash = LookupBackendPixelShaderHash(pixelShader);
        record.pixelShaderHasTexkill = LookupBackendPixelShaderHasTexkill(pixelShader) ? 1u : 0u;
        record.pixelConstantEpochHash = m_backendPixelConstantEpochHash;
        if (vertexShader) vertexShader->Release();
        if (pixelShader) pixelShader->Release();

        IDirect3DVertexBuffer9* vertexBuffer = nullptr;
        UINT vertexOffset = 0;
        UINT vertexStride = 0;
        if (SUCCEEDED(m_inner->GetStreamSource(0, &vertexBuffer, &vertexOffset, &vertexStride))) {
            if (!record.vertexStride) record.vertexStride = vertexStride;
            if (vertexBuffer) vertexBuffer->Release();
        }

        IDirect3DSurface9* renderTarget = nullptr;
        IDirect3DSurface9* depthSurface = nullptr;
        m_inner->GetRenderTarget(0, &renderTarget);
        m_inner->GetDepthStencilSurface(&depthSurface);
        record.renderTarget = reinterpret_cast<uintptr_t>(renderTarget);
        record.depthSurface = reinterpret_cast<uintptr_t>(depthSurface);
        if (renderTarget) renderTarget->Release();
        if (depthSurface) depthSurface->Release();

        MEMORY_BASIC_INFORMATION returnRegion{};
        if (returnAddress && VirtualQuery(returnAddress, &returnRegion, sizeof(returnRegion))) {
            record.callerBase = reinterpret_cast<uintptr_t>(returnRegion.AllocationBase);
            const uintptr_t address = reinterpret_cast<uintptr_t>(returnAddress);
            if (record.callerBase && address >= record.callerBase) {
                record.callerRva = address - record.callerBase;
            }
        }

        ReadProperShadersPhase(record.phaseAlternate, record.phaseDepth, record.phaseFading);
        if (record.alphaTest == FALSE) record.anomalyFlags |= 1u << 0;
        if (record.alphaBlend == FALSE) record.anomalyFlags |= 1u << 1;
        if (record.phaseDepth == 1 && record.colorWrite != 0) record.anomalyFlags |= 1u << 2;
        if (record.phaseDepth == 0 && record.alphaTest == FALSE &&
            record.alphaBlend == FALSE && record.pixelShader == 0) {
            record.anomalyFlags |= 1u << 3;
        }
        if (record.phaseDepth == 1 && record.zWrite == FALSE) record.anomalyFlags |= 1u << 4;
        const bool submittedOpaque = record.alphaClass == BACKEND_ALPHA_TRANSPARENT &&
            record.alphaTest == FALSE && record.alphaBlend == FALSE &&
            (record.pixelShader == 0 || record.pixelShaderHasTexkill == 0);
        if (submittedOpaque) {
            record.anomalyFlags |= 1u << 5;
        }

        const uint64_t pathKeyParts[] = {
            static_cast<uint64_t>(record.texture),
            static_cast<uint64_t>(record.callerRva),
            (static_cast<uint64_t>(record.phaseAlternate) << 32) |
                (static_cast<uint64_t>(record.phaseDepth & 0xffff) << 16) |
                static_cast<uint64_t>(record.phaseFading & 0xffff),
            (static_cast<uint64_t>(record.primitiveType) << 32) |
                static_cast<uint64_t>(record.primitiveCount),
        };
        const uint64_t pathKey = BackendHashBytes(pathKeyParts, sizeof(pathKeyParts));
        const uint64_t pathSignatureParts[] = {
            record.pixelShaderHash,
            (static_cast<uint64_t>(record.pixelShaderHasTexkill) << 48) |
                (static_cast<uint64_t>(record.alphaTest & 0xffff) << 32) |
                (static_cast<uint64_t>(record.alphaBlend & 0xffff) << 16) |
                static_cast<uint64_t>(record.colorWrite & 0xffff),
        };
        const uint64_t pathSignature = BackendHashBytes(pathSignatureParts, sizeof(pathSignatureParts));
        auto pathIt = m_backendAlphaPathStates.find(pathKey);
        if (pathIt == m_backendAlphaPathStates.end()) {
            BackendAlphaPathState state{};
            state.frame = record.frame;
            state.submittedOpaque = submittedOpaque;
            state.signature = pathSignature;
            m_backendAlphaPathStates.emplace(pathKey, state);
        } else if (pathIt->second.frame != record.frame) {
            const BackendAlphaPathState previous = pathIt->second;
            if (previous.frame != UINT32_MAX && record.frame > previous.frame &&
                record.frame - previous.frame <= 2 &&
                !previous.submittedOpaque && submittedOpaque &&
                previous.signature != pathSignature) {
                record.anomalyFlags |= 1u << 6;
            }
            pathIt->second.frame = record.frame;
            pathIt->second.submittedOpaque = submittedOpaque;
            pathIt->second.signature = pathSignature;
        }

        const size_t index = static_cast<size_t>(m_backendRecordSequence - 1) % kBackendAlphaRingCapacity;
        m_backendAlphaRing[index] = record;
        if (m_backendAlphaRingCount < kBackendAlphaRingCapacity) ++m_backendAlphaRingCount;
        ++m_backendAlphaDraws;

        if ((record.anomalyFlags & (1u << 2)) != 0) {
            m_backendAutoDumpPending = true;
            if (m_backendLastDepthColorWriteLogFrame != m_backendFrame) {
                m_backendLastDepthColorWriteLogFrame = m_backendFrame;
                Log("backendtrace: depth pass wrote color frame={} draw={} tex={:08X} fmt=0x{:08X} psHash={:016X} colorWrite=0x{:08X} callerRva=0x{:08X}",
                    record.frame, record.drawInFrame, reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(record.texture)),
                    record.textureFormat,
                    static_cast<unsigned long long>(record.pixelShaderHash),
                    record.colorWrite, static_cast<unsigned>(record.callerRva));
            }
        }
        if ((record.anomalyFlags & (1u << 6)) != 0) {
            m_backendAutoDumpPending = true;
            if (m_backendLastOpaqueAlphaLogFrame != m_backendFrame) {
                m_backendLastOpaqueAlphaLogFrame = m_backendFrame;
                Log("backendtrace: alpha path changed safe-to-opaque frame={} draw={} tex={:08X} fmt=0x{:08X} psHash={:016X} texkill={} alphaTest={} alphaBlend={} phaseDepth={} callerRva=0x{:08X}",
                    record.frame, record.drawInFrame, reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(record.texture)),
                    record.textureFormat,
                    static_cast<unsigned long long>(record.pixelShaderHash),
                    record.pixelShaderHasTexkill, record.alphaTest, record.alphaBlend,
                    record.phaseDepth, static_cast<unsigned>(record.callerRva));
            }
        }

        texture->Release();
    }

    void BackendOnPresent()
    {
        const bool triggerDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (!triggerDown) {
            m_backendTraceTriggerWasDown = false;
        } else if (!m_backendTraceTriggerWasDown) {
            m_backendTraceTriggerWasDown = true;
            DumpBackendTrace("hotkey-F10");
        }

        if (m_backendAutoDumpPending &&
            (m_backendFrame - m_backendLastDumpFrame > 30 || m_backendLastDumpFrame == UINT32_MAX)) {
            m_backendAutoDumpPending = false;
            DumpBackendTrace("auto-alpha-path-anomaly");
        }

        if ((m_backendFrame % 300) == 0) {
            Log("backendtrace: frame={} drawInFrame={} ring={} alphaDraws={} probes={} transparentTextures={} unknownAlphaTextures={}",
                m_backendFrame, m_backendDrawInFrame,
                static_cast<unsigned>(m_backendAlphaRingCount),
                static_cast<unsigned long long>(m_backendAlphaDraws),
                static_cast<unsigned long long>(m_backendTextureProbes),
                static_cast<unsigned long long>(m_backendTransparentTextures),
                static_cast<unsigned long long>(m_backendUnknownAlphaTextures));
        }

        ++m_backendFrame;
        m_backendDrawInFrame = 0;
    }

    void DumpBackendTrace(const char* reason)
    {
        if (!m_backendAlphaRing || m_backendAlphaRingCount == 0) return;

        char path[MAX_PATH]{};
        FormatTo(path, sizeof(path), "{}\\scripts\\BridgeD3D9.backend-alpha-ring.log", g_gameDir);
        FILE* file = nullptr;
        if (fopen_s(&file, path, "a") != 0 || !file) {
            Log("backendtrace: failed to open alpha ring output {}", path);
            return;
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);
        const uint64_t totalRecords = m_backendRecordSequence;
        const size_t count = m_backendAlphaRingCount;
        const size_t start = totalRecords > kBackendAlphaRingCapacity
            ? static_cast<size_t>(totalRecords % kBackendAlphaRingCapacity)
            : 0;
        std::print(file,
            "# backend-capture begin={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} reason={} currentFrame={} records={} sequence={} device={:08X} inner={:08X}\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            reason ? reason : "unknown", m_backendFrame, static_cast<unsigned>(count),
            static_cast<unsigned long long>(totalRecords), reinterpret_cast<std::uintptr_t>(this), reinterpret_cast<std::uintptr_t>(m_inner));

        for (size_t i = 0; i < count; ++i) {
            const BackendAlphaDrawRecord& r = m_backendAlphaRing[(start + i) % kBackendAlphaRingCapacity];
            std::print(file,
                "seq={} tick={} frame={} age={} draw={} kind={} ptype={} prim={} verts={} stride={} "
                "callerBase=%p callerRva=0x%08X phaseAlt=%u phaseDepth=%u phaseFading=%u anomaly=0x%02X "
                "tex=%p alphaClass=%u fmt=0x%08X size=%ux%u levels=%u pool=%u usage=0x%08X "
                "vs=%p vsHash=%016llX ps=%p psHash=%016llX psTexkill=%u psConstHash=%016llX "
                "alphaTest=%u alphaRef=%u alphaFunc=%u alphaBlend=%u src=%u dest=%u blendOp=%u sepAlpha=%u srcAlpha=%u destAlpha=%u blendOpAlpha=%u "
                "z=%u zWrite=%u zFunc=%u colorWrite=0x%08X stencil=%u stencilFunc=%u stencilPass=%u stencilFail=%u stencilZFail=%u cull=%u fog=%u "
                "colorOp=%u colorArg1=%u colorArg2=%u alphaOp=%u alphaArg1=%u alphaArg2=%u "
                "addressU=%u addressV=%u minFilter=%u magFilter=%u mipFilter=%u mipLodBias=0x%08X maxAniso=%u fvf=0x%08X rt=%p depth=%p\n",
                static_cast<unsigned long long>(r.sequence), r.tick, r.frame,
                m_backendFrame >= r.frame ? m_backendFrame - r.frame : 0,
                r.drawInFrame, r.kind, r.primitiveType, r.primitiveCount, r.vertexCount, r.vertexStride,
                reinterpret_cast<void*>(r.callerBase), static_cast<unsigned>(r.callerRva),
                r.phaseAlternate, r.phaseDepth, r.phaseFading, r.anomalyFlags,
                reinterpret_cast<void*>(r.texture), r.alphaClass, r.textureFormat,
                r.textureWidth, r.textureHeight, r.textureLevels, r.texturePool, r.textureUsage,
                reinterpret_cast<void*>(r.vertexShader), static_cast<unsigned long long>(r.vertexShaderHash),
                reinterpret_cast<void*>(r.pixelShader), static_cast<unsigned long long>(r.pixelShaderHash),
                r.pixelShaderHasTexkill,
                static_cast<unsigned long long>(r.pixelConstantEpochHash),
                r.alphaTest, r.alphaRef, r.alphaFunc, r.alphaBlend, r.srcBlend, r.destBlend,
                r.blendOp, r.separateAlpha, r.srcBlendAlpha, r.destBlendAlpha, r.blendOpAlpha,
                r.zEnable, r.zWrite, r.zFunc, r.colorWrite,
                r.stencilEnable, r.stencilFunc, r.stencilPass, r.stencilFail, r.stencilZFail,
                r.cullMode, r.fogEnable,
                r.colorOp, r.colorArg1, r.colorArg2, r.alphaOp, r.alphaArg1, r.alphaArg2,
                r.addressU, r.addressV, r.minFilter, r.magFilter, r.mipFilter,
                r.mipLodBias, r.maxAnisotropy, r.fvf,
                reinterpret_cast<void*>(r.renderTarget), reinterpret_cast<void*>(r.depthSurface));
        }
        std::print(file, "# backend-capture end reason={} records={}\n",
            reason ? reason : "unknown", static_cast<unsigned>(count));
        fflush(file);
        fclose(file);
        m_backendLastDumpFrame = m_backendFrame;
        Log("backendtrace: alpha ring exported reason={} records={} path={}",
            reason ? reason : "unknown", static_cast<unsigned>(count), path);
    }
#endif

    void OnProperShadersEffectProfilePresent()
    {
        if (!g_enableProperShadersEffectProfile) return;

        const DWORD currentThreadId = GetCurrentThreadId();
        if (g_properShadersEffectProfile.active &&
            g_properShadersEffectProfile.threadId == currentThreadId) {
            ++g_properShadersEffectProfile.frames;
            if (GetTickCount() - g_properShadersEffectProfile.startTick >=
                g_properShadersEffectProfileDurationMs) {
                FinishProperShadersEffectProfile("duration-complete");
            }
        }

        const bool triggerDown =
            (GetAsyncKeyState(g_properShadersEffectProfileTriggerKey) & 0x8000) != 0;
        if (!triggerDown) {
            m_effectProfileTriggerWasDown = false;
            return;
        }
        if (m_effectProfileTriggerWasDown) return;
        m_effectProfileTriggerWasDown = true;

        if (g_properShadersEffectProfile.active) {
            Log("effectprofile: trigger ignored because a capture is already active");
            return;
        }
        StartProperShadersEffectProfile(m_inner);
    }

    void MaybeArmCpuHotspotProfile()
    {
        if (!g_enableCpuHotspotProfile) return;

        const bool triggerDown = (GetAsyncKeyState(g_cpuHotspotTriggerKey) & 0x8000) != 0;
        if (!triggerDown) {
            m_cpuHotspotTriggerWasDown = false;
            return;
        }
        if (m_cpuHotspotTriggerWasDown) return;
        m_cpuHotspotTriggerWasDown = true;

        if (m_callsiteProfileActive ||
            g_cpuHotspotCallsitePending.load() != 0) {
            Log("cpuhotspots: trigger ignored because a D3D9 callsite stage is active or pending");
            return;
        }

        if (g_cpuHotspotActive.exchange(1) != 0) {
            Log("cpuhotspots: trigger ignored because a capture is already active");
            return;
        }
        g_cpuHotspotPresents.store(0);

        CpuHotspotWorkerContext* context = new (std::nothrow) CpuHotspotWorkerContext();
        if (!context) {
            Log("cpuhotspots: failed to allocate worker context");
            g_cpuHotspotActive.store(0);
            return;
        }

        context->targetThreadId = m_renderThreadId ? m_renderThreadId : GetCurrentThreadId();
        context->captureId = static_cast<UINT>(
        g_cpuHotspotCaptureId.fetch_add(1) + 1);
        context->durationMs = g_cpuHotspotDurationMs;
        context->intervalMs = g_cpuHotspotIntervalMs;
        FormatTo(context->outputPath, sizeof(context->outputPath),
            "{}\\scripts\\BridgeD3D9.cpuhotspots.log", g_gameDir);

        char diagnosticsDirectory[MAX_PATH]{};
        char cpuDirectory[MAX_PATH]{};
        FormatTo(diagnosticsDirectory, sizeof(diagnosticsDirectory), "{}\\Diagnostics", g_gameDir);
        FormatTo(cpuDirectory, sizeof(cpuDirectory), "{}\\CPU", diagnosticsDirectory);
        CreateDirectoryA(diagnosticsDirectory, nullptr);
        CreateDirectoryA(cpuDirectory, nullptr);

        SYSTEMTIME screenshotTime{};
        GetLocalTime(&screenshotTime);
        FormatTo(context->screenshotPath, sizeof(context->screenshotPath),
            "{}\\capture-{:04}-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.bmp",
            cpuDirectory,
            context->captureId,
            screenshotTime.wYear, screenshotTime.wMonth, screenshotTime.wDay,
            screenshotTime.wHour, screenshotTime.wMinute, screenshotTime.wSecond,
            screenshotTime.wMilliseconds);

        const std::expected<void, std::string> screenshotSaved =
            SaveD3D9BackBufferBmp(m_inner, context->screenshotPath);
        if (!screenshotSaved) {
            Log("cpuhotspots: capture={} screenshot failed path={} error={}",
                context->captureId,
                context->screenshotPath,
                screenshotSaved.error());
        }

        const UINT captureId = context->captureId;
        const DWORD targetThreadId = context->targetThreadId;
        const DWORD durationMs = context->durationMs;
        const DWORD intervalMs = context->intervalMs;
        char screenshotPath[MAX_PATH]{};
        FormatTo(screenshotPath, sizeof(screenshotPath), "{}", context->screenshotPath);

        HANDLE worker = CreateThread(nullptr, 0, CpuHotspotWorker, context, 0, nullptr);
        if (!worker) {
            Log("cpuhotspots: capture={} CreateThread failed err={}",
                captureId, GetLastError());
            delete context;
            g_cpuHotspotActive.store(0);
            return;
        }
        CloseHandle(worker);

        Log("cpuhotspots: capture={} armed targetThread={} durationMs={} intervalMs={} screenshotSaved={} screenshot={}",
            captureId,
            targetThreadId,
            durationMs,
            intervalMs,
            screenshotSaved ? 1 : 0,
            screenshotPath);
    }

    void ArmCallsiteProfile(const char* source, UINT parentCpuCaptureId)
    {
        if (m_callsiteProfileActive) return;

        std::memset(m_callsiteEntries, 0, sizeof(m_callsiteEntries));
        m_callsiteEntryCount = 0;
        m_callsiteDroppedEntries = 0;
        m_callsiteFrameCount = 0;
        m_callsiteTotalDraws = 0;
        m_callsiteTotalPrimitives = 0;
        m_callsiteCapturedSamples = 0;
        m_callsiteStartTick = GetTickCount();
        m_callsiteParentCpuCaptureId = parentCpuCaptureId;
        m_callsiteProfileActive = true;
        ++m_callsiteCaptureId;

        Log("d3d9callsites: capture={} armed source={} parentCpuCapture={} frames={} sampleEveryDraws={}",
            m_callsiteCaptureId,
            source ? source : "unknown",
            m_callsiteParentCpuCaptureId,
            g_d3d9CallsiteCaptureFrames,
            g_d3d9CallsiteSampleEveryDraws);
    }

    void MaybeArmCallsiteProfile()
    {
        if (!g_enableD3D9CallsiteProfile) return;

        const bool triggerDown = (GetAsyncKeyState(g_d3d9CallsiteTriggerKey) & 0x8000) != 0;
        if (g_cpuHotspotActive.load() != 0) {
            m_callsiteTriggerWasDown = triggerDown;
            return;
        }

        if (!m_callsiteProfileActive) {
            const LONG parentCpuCaptureId = g_cpuHotspotCallsitePending.exchange(0);
            if (parentCpuCaptureId > 0) {
                ArmCallsiteProfile("sequential-f8", static_cast<UINT>(parentCpuCaptureId));
                return;
            }
        }

        if (!triggerDown) {
            m_callsiteTriggerWasDown = false;
            return;
        }
        if (m_callsiteTriggerWasDown) return;
        m_callsiteTriggerWasDown = true;
        ArmCallsiteProfile("manual-hotkey", 0);
    }

    void OnCallsitePresent()
    {
        if (!g_enableD3D9CallsiteProfile) return;

        if (m_callsiteProfileActive) {
            ++m_callsiteFrameCount;
            if (m_callsiteFrameCount >= g_d3d9CallsiteCaptureFrames) {
                FinishCallsiteProfile("capture-complete");
            }
        }
        MaybeArmCallsiteProfile();
    }

    void ProfileDrawCall(UINT kind, UINT primitiveCount)
    {
        if (!m_callsiteProfileActive) return;

        ++m_callsiteTotalDraws;
        m_callsiteTotalPrimitives += primitiveCount;
        if (((m_callsiteTotalDraws - 1) % g_d3d9CallsiteSampleEveryDraws) != 0) return;

        void* stack[kD3D9CallsiteStackDepth]{};
        const USHORT depth = CaptureStackBackTrace(0, kD3D9CallsiteStackDepth, stack, nullptr);
        if (depth == 0) return;

        uintptr_t firstMainRva = 0;
        uintptr_t originRva = 0;
        uintptr_t immediateModuleBase = 0;
        uintptr_t immediateModuleRva = 0;
        uintptr_t firstExternalAddress = 0;

        for (USHORT i = 0; i < depth; ++i) {
            const uintptr_t address = reinterpret_cast<uintptr_t>(stack[i]);
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(stack[i], &region, sizeof(region)) != sizeof(region)) continue;

            const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(region.AllocationBase);
            if (!immediateModuleBase && moduleBase != reinterpret_cast<uintptr_t>(g_selfModule)) {
                immediateModuleBase = moduleBase;
                immediateModuleRva = address >= moduleBase ? address - moduleBase : 0;
                firstExternalAddress = address;
            }

            if (m_gameImageBase && address >= m_gameImageBase &&
                address < m_gameImageBase + m_gameImageSize) {
                const uintptr_t rva = address - m_gameImageBase;
                if (!firstMainRva) firstMainRva = rva;

                // Skip the low-level RenderWare D3D9 dispatch range. The next
                // GTA frame identifies the pipeline or scene code that caused it.
                if (!originRva && rva < 0x003F0000u) {
                    originRva = rva;
                }
            }
        }

        uintptr_t keyAddress = 0;
        if (originRva) {
            keyAddress = m_gameImageBase + originRva;
        } else if (firstMainRva) {
            keyAddress = m_gameImageBase + firstMainRva;
        } else if (firstExternalAddress) {
            keyAddress = firstExternalAddress;
        } else {
            keyAddress = reinterpret_cast<uintptr_t>(stack[0]);
        }

        D3D9CallsiteEntry* entry = nullptr;
        for (UINT i = 0; i < m_callsiteEntryCount; ++i) {
            if (m_callsiteEntries[i].keyAddress == keyAddress &&
                m_callsiteEntries[i].kind == kind &&
                m_callsiteEntries[i].renderTargetWidth == m_callsiteRenderTargetWidth &&
                m_callsiteEntries[i].renderTargetHeight == m_callsiteRenderTargetHeight &&
                m_callsiteEntries[i].renderTargetFormat == m_callsiteRenderTargetFormat) {
                entry = &m_callsiteEntries[i];
                break;
            }
        }

        if (!entry) {
            if (m_callsiteEntryCount >= kD3D9CallsiteMaxEntries) {
                ++m_callsiteDroppedEntries;
                return;
            }
            entry = &m_callsiteEntries[m_callsiteEntryCount++];
            entry->keyAddress = keyAddress;
            entry->originRva = originRva;
            entry->firstMainRva = firstMainRva;
            entry->immediateModuleBase = immediateModuleBase;
            entry->immediateModuleRva = immediateModuleRva;
            entry->kind = kind;
            entry->renderTargetWidth = m_callsiteRenderTargetWidth;
            entry->renderTargetHeight = m_callsiteRenderTargetHeight;
            entry->renderTargetFormat = m_callsiteRenderTargetFormat;
            entry->stackDepth = depth;
            std::memcpy(entry->stack, stack, depth * sizeof(stack[0]));
        }

        ++entry->samples;
        entry->sampledPrimitives += primitiveCount;
        ++m_callsiteCapturedSamples;
    }

    void FormatCallsiteFrame(void* address, char* output, size_t outputSize) const
    {
        if (!output || outputSize == 0) return;
        output[0] = '\0';

        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) {
            FormatTo(output, outputSize, "{:08X}", reinterpret_cast<std::uintptr_t>(address));
            return;
        }

        HMODULE module = reinterpret_cast<HMODULE>(region.AllocationBase);
        char path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
        const char* name = length ? path : "unknown";
        if (const char* slash = std::strrchr(name, '\\')) name = slash + 1;
        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(module);
        const uintptr_t value = reinterpret_cast<uintptr_t>(address);
        const uintptr_t rva = value >= moduleBase ? value - moduleBase : 0;

        if (moduleBase == m_gameImageBase && m_gamePreferredBase) {
            FormatTo(output, outputSize, "{}+0x{:08X}[ghidra=0x{:08X}]",
                name,
                static_cast<unsigned>(rva),
                static_cast<unsigned>(m_gamePreferredBase + rva));
        } else {
            FormatTo(output, outputSize, "{}+0x{:08X}", name, static_cast<unsigned>(rva));
        }
    }

    void FinishCallsiteProfile(const char* reason)
    {
        if (!m_callsiteProfileActive) return;
        m_callsiteProfileActive = false;

        char path[MAX_PATH]{};
        FormatTo(path, sizeof(path), "{}\\scripts\\BridgeD3D9.callsites.log", g_gameDir);
        FILE* file = nullptr;
        if (fopen_s(&file, path, "a") != 0 || !file) {
            Log("d3d9callsites: capture={} failed to open {}", m_callsiteCaptureId, path);
            return;
        }

        std::vector<UINT> order;
        order.reserve(m_callsiteEntryCount);
        for (UINT i = 0; i < m_callsiteEntryCount; ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [this](UINT left, UINT right) {
            return m_callsiteEntries[left].samples > m_callsiteEntries[right].samples;
        });

        SYSTEMTIME st{};
        GetLocalTime(&st);
        const DWORD elapsed = GetTickCount() - m_callsiteStartTick;
        std::print(file,
            "# capture={} parentCpuCapture={} begin={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} reason={} frames={} draws={} primitives={} samples={} entries={} dropped={} sampleEveryDraws={} elapsedMs={} gameBase={:08X} preferredBase={:08X}\n",
            m_callsiteCaptureId,
            m_callsiteParentCpuCaptureId,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            reason ? reason : "unknown",
            m_callsiteFrameCount,
            static_cast<unsigned long long>(m_callsiteTotalDraws),
            static_cast<unsigned long long>(m_callsiteTotalPrimitives),
            static_cast<unsigned long long>(m_callsiteCapturedSamples),
            m_callsiteEntryCount,
            static_cast<unsigned long long>(m_callsiteDroppedEntries),
            g_d3d9CallsiteSampleEveryDraws,
            elapsed, reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(m_gameImageBase)), reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(m_gamePreferredBase)));

        UINT rank = 0;
        for (UINT index : order) {
            const D3D9CallsiteEntry& entry = m_callsiteEntries[index];
            char immediate[2 * MAX_PATH]{};
            FormatCallsiteFrame(
                reinterpret_cast<void*>(entry.immediateModuleBase + entry.immediateModuleRva),
                immediate,
                sizeof(immediate));

            const uintptr_t ghidraAddress = entry.originRva && m_gamePreferredBase
                ? m_gamePreferredBase + entry.originRva
                : 0;
            std::print(file,
                "rank={} kind={} samples={} estimatedDraws={} sampledPrimitives={} rt={}x{} rtFmt=0x{:08X} originRva=0x{:08X} ghidra=0x{:08X} firstMainRva=0x{:08X} immediate={} stack=",
                ++rank,
                D3D9DrawKindName(entry.kind),
                static_cast<unsigned long long>(entry.samples),
                static_cast<unsigned long long>(entry.samples * g_d3d9CallsiteSampleEveryDraws),
                static_cast<unsigned long long>(entry.sampledPrimitives),
                entry.renderTargetWidth,
                entry.renderTargetHeight,
                static_cast<unsigned>(entry.renderTargetFormat),
                static_cast<unsigned>(entry.originRva),
                static_cast<unsigned>(ghidraAddress),
                static_cast<unsigned>(entry.firstMainRva),
                immediate);

            for (USHORT i = 0; i < entry.stackDepth; ++i) {
                char frame[2 * MAX_PATH]{};
                FormatCallsiteFrame(entry.stack[i], frame, sizeof(frame));
                std::print(file, "{}{}", i ? ">" : "", frame);
            }
            std::print(file, "\n");
        }
        std::print(file, "# capture={} end\n", m_callsiteCaptureId);
        fflush(file);
        fclose(file);

        Log("d3d9callsites: capture={} parentCpuCapture={} complete reason={} frames={} draws={} samples={} entries={} dropped={} elapsedMs={} path={}",
            m_callsiteCaptureId,
            m_callsiteParentCpuCaptureId,
            reason ? reason : "unknown",
            m_callsiteFrameCount,
            static_cast<unsigned long long>(m_callsiteTotalDraws),
            static_cast<unsigned long long>(m_callsiteCapturedSamples),
            m_callsiteEntryCount,
            static_cast<unsigned long long>(m_callsiteDroppedEntries),
            elapsed,
            path);
        m_callsiteParentCpuCaptureId = 0;
    }

    void MaybeArmDrawTrace()
    {
        if (!g_enableD3D9Trace || m_drawTraceActive) return;
        const bool triggerDown = (GetAsyncKeyState(g_d3d9TraceTriggerKey) & 0x8000) != 0;
        if (!triggerDown) {
            m_drawTraceTriggerWasDown = false;
            return;
        }
        if (m_drawTraceTriggerWasDown) return;
        m_drawTraceTriggerWasDown = true;

        char path[MAX_PATH]{};
        FormatTo(path, sizeof(path), "{}\\scripts\\BridgeD3D9.drawtrace.log", g_gameDir);
        FILE* file = nullptr;
        if (fopen_s(&file, path, "a") != 0 || !file) {
            Log("d3d9trace: failed to open {}", path);
            return;
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);
        ++m_drawTraceCaptureId;
        m_drawTraceFile = file;
        m_drawTraceActive = true;
        m_drawTraceTruncated = false;
        m_drawTraceDrawCount = 0;
        m_drawTraceStartTick = GetTickCount();
        std::print(m_drawTraceFile,
            "# capture={} begin={:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} triggerVK={} maxDraws={} device={:08X} inner={:08X}\n",
            m_drawTraceCaptureId,
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            g_d3d9TraceTriggerKey,
            g_d3d9TraceMaxDraws, reinterpret_cast<std::uintptr_t>(this), reinterpret_cast<std::uintptr_t>(m_inner));
        Log("d3d9trace: capture={} armed for next presented frame maxDraws={}",
            m_drawTraceCaptureId, g_d3d9TraceMaxDraws);
    }

    void FinishDrawTrace(const char* reason)
    {
        if (!m_drawTraceActive && !m_drawTraceFile) return;
        const DWORD elapsed = GetTickCount() - m_drawTraceStartTick;
        if (m_drawTraceFile) {
            std::print(m_drawTraceFile,
                "# capture={} end reason={} draws={} truncated={} elapsedMs={}\n",
                m_drawTraceCaptureId,
                reason ? reason : "unknown",
                m_drawTraceDrawCount,
                m_drawTraceTruncated ? 1u : 0u,
                elapsed);
            fflush(m_drawTraceFile);
            fclose(m_drawTraceFile);
            m_drawTraceFile = nullptr;
        }
        Log("d3d9trace: capture={} complete reason={} draws={} truncated={} elapsedMs={}",
            m_drawTraceCaptureId,
            reason ? reason : "unknown",
            m_drawTraceDrawCount,
            m_drawTraceTruncated ? 1 : 0,
            elapsed);
        m_drawTraceActive = false;
    }

    void TraceDraw(
        const char* kind,
        D3DPRIMITIVETYPE primitiveType,
        UINT primitiveCount,
        INT baseVertex,
        UINT minVertex,
        UINT vertexCount,
        UINT startIndex,
        UINT suppliedStride,
        void* returnAddress)
    {
        if (!m_drawTraceActive || !m_drawTraceFile) return;
        if (m_drawTraceDrawCount >= g_d3d9TraceMaxDraws) {
            if (!m_drawTraceTruncated) {
                std::print(m_drawTraceFile, "# capture={} truncated-at={}\n",
                    m_drawTraceCaptureId, m_drawTraceDrawCount);
                m_drawTraceTruncated = true;
            }
            return;
        }

        auto getRenderState = [this](D3DRENDERSTATETYPE state) {
            DWORD value = UINT32_MAX;
            m_inner->GetRenderState(state, &value);
            return value;
        };
        auto getTextureStageState = [this](D3DTEXTURESTAGESTATETYPE state) {
            DWORD value = UINT32_MAX;
            m_inner->GetTextureStageState(0, state, &value);
            return value;
        };
        auto getSamplerState = [this](D3DSAMPLERSTATETYPE state) {
            DWORD value = UINT32_MAX;
            m_inner->GetSamplerState(0, state, &value);
            return value;
        };

        const DWORD alphaTest = getRenderState(D3DRS_ALPHATESTENABLE);
        const DWORD alphaRef = getRenderState(D3DRS_ALPHAREF);
        const DWORD alphaFunc = getRenderState(D3DRS_ALPHAFUNC);
        const DWORD alphaBlend = getRenderState(D3DRS_ALPHABLENDENABLE);
        const DWORD srcBlend = getRenderState(D3DRS_SRCBLEND);
        const DWORD destBlend = getRenderState(D3DRS_DESTBLEND);
        const DWORD blendOp = getRenderState(D3DRS_BLENDOP);
        const DWORD separateAlpha = getRenderState(D3DRS_SEPARATEALPHABLENDENABLE);
        const DWORD srcBlendAlpha = getRenderState(D3DRS_SRCBLENDALPHA);
        const DWORD destBlendAlpha = getRenderState(D3DRS_DESTBLENDALPHA);
        const DWORD blendOpAlpha = getRenderState(D3DRS_BLENDOPALPHA);
        const DWORD cullMode = getRenderState(D3DRS_CULLMODE);
        const DWORD zEnable = getRenderState(D3DRS_ZENABLE);
        const DWORD zWrite = getRenderState(D3DRS_ZWRITEENABLE);
        const DWORD zFunc = getRenderState(D3DRS_ZFUNC);
        const DWORD colorWrite = getRenderState(D3DRS_COLORWRITEENABLE);
        const DWORD fogEnable = getRenderState(D3DRS_FOGENABLE);

        const DWORD colorOp = getTextureStageState(D3DTSS_COLOROP);
        const DWORD colorArg1 = getTextureStageState(D3DTSS_COLORARG1);
        const DWORD colorArg2 = getTextureStageState(D3DTSS_COLORARG2);
        const DWORD alphaOp = getTextureStageState(D3DTSS_ALPHAOP);
        const DWORD alphaArg1 = getTextureStageState(D3DTSS_ALPHAARG1);
        const DWORD alphaArg2 = getTextureStageState(D3DTSS_ALPHAARG2);

        const DWORD addressU = getSamplerState(D3DSAMP_ADDRESSU);
        const DWORD addressV = getSamplerState(D3DSAMP_ADDRESSV);
        const DWORD minFilter = getSamplerState(D3DSAMP_MINFILTER);
        const DWORD magFilter = getSamplerState(D3DSAMP_MAGFILTER);
        const DWORD mipFilter = getSamplerState(D3DSAMP_MIPFILTER);
        const DWORD mipLodBias = getSamplerState(D3DSAMP_MIPMAPLODBIAS);
        const DWORD maxAnisotropy = getSamplerState(D3DSAMP_MAXANISOTROPY);

        IDirect3DBaseTexture9* texture = nullptr;
        void* texturePointer = nullptr;
        UINT textureType = 0;
        UINT textureWidth = 0;
        UINT textureHeight = 0;
        UINT textureLevels = 0;
        DWORD textureFormat = 0;
        if (SUCCEEDED(m_inner->GetTexture(0, &texture)) && texture) {
            texturePointer = texture;
            textureType = static_cast<UINT>(texture->GetType());
            textureLevels = texture->GetLevelCount();
            if (textureType == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC desc{};
                if (SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc))) {
                    textureWidth = desc.Width;
                    textureHeight = desc.Height;
                    textureFormat = static_cast<DWORD>(desc.Format);
                }
            } else if (textureType == D3DRTYPE_CUBETEXTURE) {
                D3DSURFACE_DESC desc{};
                if (SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(texture)->GetLevelDesc(0, &desc))) {
                    textureWidth = desc.Width;
                    textureHeight = desc.Height;
                    textureFormat = static_cast<DWORD>(desc.Format);
                }
            } else if (textureType == D3DRTYPE_VOLUMETEXTURE) {
                D3DVOLUME_DESC desc{};
                if (SUCCEEDED(static_cast<IDirect3DVolumeTexture9*>(texture)->GetLevelDesc(0, &desc))) {
                    textureWidth = desc.Width;
                    textureHeight = desc.Height;
                    textureFormat = static_cast<DWORD>(desc.Format);
                }
            }
            texture->Release();
        }

        IDirect3DVertexShader9* vertexShader = nullptr;
        IDirect3DPixelShader9* pixelShader = nullptr;
        m_inner->GetVertexShader(&vertexShader);
        m_inner->GetPixelShader(&pixelShader);
        void* vertexShaderPointer = vertexShader;
        void* pixelShaderPointer = pixelShader;
        if (vertexShader) vertexShader->Release();
        if (pixelShader) pixelShader->Release();

        IDirect3DVertexBuffer9* vertexBuffer = nullptr;
        UINT vertexOffset = 0;
        UINT vertexStride = 0;
        m_inner->GetStreamSource(0, &vertexBuffer, &vertexOffset, &vertexStride);
        void* vertexBufferPointer = vertexBuffer;
        if (vertexBuffer) vertexBuffer->Release();

        IDirect3DIndexBuffer9* indexBuffer = nullptr;
        m_inner->GetIndices(&indexBuffer);
        void* indexBufferPointer = indexBuffer;
        if (indexBuffer) indexBuffer->Release();

        IDirect3DVertexDeclaration9* vertexDeclaration = nullptr;
        m_inner->GetVertexDeclaration(&vertexDeclaration);
        void* vertexDeclarationPointer = vertexDeclaration;
        if (vertexDeclaration) vertexDeclaration->Release();

        IDirect3DSurface9* renderTarget = nullptr;
        IDirect3DSurface9* depthSurface = nullptr;
        D3DSURFACE_DESC renderTargetDesc{};
        D3DSURFACE_DESC depthSurfaceDesc{};
        m_inner->GetRenderTarget(0, &renderTarget);
        m_inner->GetDepthStencilSurface(&depthSurface);
        void* renderTargetPointer = renderTarget;
        void* depthSurfacePointer = depthSurface;
        if (renderTarget) {
            renderTarget->GetDesc(&renderTargetDesc);
            renderTarget->Release();
        }
        if (depthSurface) {
            depthSurface->GetDesc(&depthSurfaceDesc);
            depthSurface->Release();
        }

        D3DVIEWPORT9 viewport{};
        m_inner->GetViewport(&viewport);

        MEMORY_BASIC_INFORMATION returnRegion{};
        VirtualQuery(returnAddress, &returnRegion, sizeof(returnRegion));
        const uintptr_t callerBase = reinterpret_cast<uintptr_t>(returnRegion.AllocationBase);
        const uintptr_t callerAddress = reinterpret_cast<uintptr_t>(returnAddress);
        const uintptr_t callerRva = callerBase && callerAddress >= callerBase
            ? callerAddress - callerBase
            : 0;

        UINT psFading = UINT32_MAX;
        UINT psDepthPass = UINT32_MAX;
        UINT psAlternatePass = UINT32_MAX;
        HMODULE properShaders = GetModuleHandleA("ProperShaders.asi");
        if (!properShaders) properShaders = GetModuleHandleA("propershaders.asi");
        if (properShaders) {
            const uintptr_t psBase = reinterpret_cast<uintptr_t>(properShaders);
            BYTE phaseBytes[11]{};
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(psBase + 0x153CE4),
                    phaseBytes,
                    sizeof(phaseBytes),
                    &bytesRead) && bytesRead == sizeof(phaseBytes)) {
                uint32_t alternate = 0;
                std::memcpy(&alternate, phaseBytes, sizeof(alternate));
                psAlternatePass = alternate;
                psDepthPass = phaseBytes[9];
                psFading = phaseBytes[10];
            }
        }

        ++m_drawTraceDrawCount;
        std::print(m_drawTraceFile,
            "draw={} kind={} ra={:08X} callerBase={:08X} callerRva=0x{:08X} psFading={} psDepth={} psAlt={} "
            "ptype=%u prim=%u base=%d min=%u verts=%u start=%u suppliedStride=%u "
            "alphaTest=%u alphaRef=%u alphaFunc=%u alphaBlend=%u src=%u dest=%u blendOp=%u sepAlpha=%u srcAlpha=%u destAlpha=%u blendOpAlpha=%u "
            "cull=%u z=%u zWrite=%u zFunc=%u colorWrite=0x%08X fog=%u "
            "colorOp=%u colorArg1=%u colorArg2=%u alphaOp=%u alphaArg1=%u alphaArg2=%u "
            "addressU=%u addressV=%u minFilter=%u magFilter=%u mipFilter=%u mipLodBias=0x%08X maxAniso=%u "
            "tex=%p texType=%u texW=%u texH=%u texFmt=0x%08X texLevels=%u vs=%p ps=%p "
            "vb=%p vbOffset=%u vbStride=%u ib=%p decl=%p "
            "rt=%p rtW=%u rtH=%u rtFmt=0x%08X depth=%p depthW=%u depthH=%u depthFmt=0x%08X "
            "vpX=%u vpY=%u vpW=%u vpH=%u vpMin=%.6f vpMax=%.6f\n",
            m_drawTraceDrawCount, reinterpret_cast<std::uintptr_t>(kind ? kind : "?"), reinterpret_cast<std::uintptr_t>(returnAddress),
            reinterpret_cast<std::uintptr_t>(returnRegion.AllocationBase),
            static_cast<unsigned>(callerRva),
            psFading,
            psDepthPass,
            psAlternatePass,
            static_cast<unsigned>(primitiveType),
            primitiveCount,
            baseVertex,
            minVertex,
            vertexCount,
            startIndex,
            suppliedStride,
            alphaTest,
            alphaRef,
            alphaFunc,
            alphaBlend,
            srcBlend,
            destBlend,
            blendOp,
            separateAlpha,
            srcBlendAlpha,
            destBlendAlpha,
            blendOpAlpha,
            cullMode,
            zEnable,
            zWrite,
            zFunc,
            colorWrite,
            fogEnable,
            colorOp,
            colorArg1,
            colorArg2,
            alphaOp,
            alphaArg1,
            alphaArg2,
            addressU,
            addressV,
            minFilter,
            magFilter,
            mipFilter,
            mipLodBias,
            maxAnisotropy,
            texturePointer,
            textureType,
            textureWidth,
            textureHeight,
            textureFormat,
            textureLevels,
            vertexShaderPointer,
            pixelShaderPointer,
            vertexBufferPointer,
            vertexOffset,
            vertexStride,
            indexBufferPointer,
            vertexDeclarationPointer,
            renderTargetPointer,
            renderTargetDesc.Width,
            renderTargetDesc.Height,
            static_cast<DWORD>(renderTargetDesc.Format),
            depthSurfacePointer,
            depthSurfaceDesc.Width,
            depthSurfaceDesc.Height,
            static_cast<DWORD>(depthSurfaceDesc.Format),
            viewport.X,
            viewport.Y,
            viewport.Width,
            viewport.Height,
            viewport.MinZ,
            viewport.MaxZ);
    }

    void ResetTrackedState()
    {
        std::memset(m_renderStateSeen, 0, sizeof(m_renderStateSeen));
        std::memset(m_textureSeen, 0, sizeof(m_textureSeen));
        std::memset(m_vertexShaderConstantSeen, 0, sizeof(m_vertexShaderConstantSeen));
        std::memset(m_pixelShaderConstantSeen, 0, sizeof(m_pixelShaderConstantSeen));
        m_vertexShader = nullptr;
        m_pixelShader = nullptr;
        m_vertexShaderSeen = false;
        m_pixelShaderSeen = false;
    }

    void TrackCallsiteRenderTarget(IDirect3DSurface9* renderTarget)
    {
        m_callsiteRenderTargetWidth = 0;
        m_callsiteRenderTargetHeight = 0;
        m_callsiteRenderTargetFormat = D3DFMT_UNKNOWN;
        if (!renderTarget) return;

        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(renderTarget->GetDesc(&desc))) {
            m_callsiteRenderTargetWidth = desc.Width;
            m_callsiteRenderTargetHeight = desc.Height;
            m_callsiteRenderTargetFormat = desc.Format;
        }
    }

    void RefreshCallsiteRenderTarget()
    {
        IDirect3DSurface9* renderTarget = nullptr;
        if (SUCCEEDED(m_inner->GetRenderTarget(0, &renderTarget)) && renderTarget) {
            TrackCallsiteRenderTarget(renderTarget);
            renderTarget->Release();
        } else {
            TrackCallsiteRenderTarget(nullptr);
        }
    }

    void MaybeLogStats()
    {
        DWORD now = GetTickCount();
        DWORD elapsedMs = now - m_lastStatsTick;
        if (elapsedMs < g_d3d9StatsIntervalMs) return;

        const D3D9CallCounters& c = m_stats;
        const D3D9CallCounters& p = m_lastStats;
        const uint64_t frames = CounterDelta(c.present, p.present);
        const uint64_t drawCalls =
            CounterDelta(c.drawPrimitive, p.drawPrimitive) +
            CounterDelta(c.drawIndexedPrimitive, p.drawIndexedPrimitive) +
            CounterDelta(c.drawPrimitiveUP, p.drawPrimitiveUP) +
            CounterDelta(c.drawIndexedPrimitiveUP, p.drawIndexedPrimitiveUP);
        const uint64_t shaderSets =
            CounterDelta(c.setVertexShader, p.setVertexShader) +
            CounterDelta(c.setPixelShader, p.setPixelShader);
        const uint64_t shaderConstSets =
            CounterDelta(c.setVertexShaderConstantF, p.setVertexShaderConstantF) +
            CounterDelta(c.setPixelShaderConstantF, p.setPixelShaderConstantF);
        const uint64_t textureSets = CounterDelta(c.setTexture, p.setTexture);
        const uint64_t renderStateSets = CounterDelta(c.setRenderState, p.setRenderState);
        const uint64_t vertexShaderSets = CounterDelta(c.setVertexShader, p.setVertexShader);
        const uint64_t pixelShaderSets = CounterDelta(c.setPixelShader, p.setPixelShader);
        const uint64_t vertexShaderConstSets = CounterDelta(c.setVertexShaderConstantF, p.setVertexShaderConstantF);
        const uint64_t pixelShaderConstSets = CounterDelta(c.setPixelShaderConstantF, p.setPixelShaderConstantF);
        const uint64_t vertexShaderConstVectors = CounterDelta(c.vertexShaderConstantFVectors, p.vertexShaderConstantFVectors);
        const uint64_t pixelShaderConstVectors = CounterDelta(c.pixelShaderConstantFVectors, p.pixelShaderConstantFVectors);
        const uint64_t creates =
            CounterDelta(c.createTexture, p.createTexture) +
            CounterDelta(c.createVertexBuffer, p.createVertexBuffer) +
            CounterDelta(c.createIndexBuffer, p.createIndexBuffer) +
            CounterDelta(c.createRenderTarget, p.createRenderTarget) +
            CounterDelta(c.createDepthStencilSurface, p.createDepthStencilSurface) +
            CounterDelta(c.createVertexShader, p.createVertexShader) +
            CounterDelta(c.createPixelShader, p.createPixelShader);

        Log("d3d9stats: ms={} fps={:.1f} frames={} draws={} prims={} tex={} rs={} tss={} samp={} shader={} constF={} vb={} ib={} decl={} fvf={} rt={} z={} clear={} begin={} end={} reset={} creates={}",
            elapsedMs,
            elapsedMs ? (1000.0 * static_cast<double>(frames) / static_cast<double>(elapsedMs)) : 0.0,
            static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(drawCalls),
            static_cast<unsigned long long>(CounterDelta(c.primitives, p.primitives)),
            static_cast<unsigned long long>(textureSets),
            static_cast<unsigned long long>(renderStateSets),
            static_cast<unsigned long long>(CounterDelta(c.setTextureStageState, p.setTextureStageState)),
            static_cast<unsigned long long>(CounterDelta(c.setSamplerState, p.setSamplerState)),
            static_cast<unsigned long long>(shaderSets),
            static_cast<unsigned long long>(shaderConstSets),
            static_cast<unsigned long long>(CounterDelta(c.setStreamSource, p.setStreamSource)),
            static_cast<unsigned long long>(CounterDelta(c.setIndices, p.setIndices)),
            static_cast<unsigned long long>(CounterDelta(c.setVertexDeclaration, p.setVertexDeclaration)),
            static_cast<unsigned long long>(CounterDelta(c.setFVF, p.setFVF)),
            static_cast<unsigned long long>(CounterDelta(c.setRenderTarget, p.setRenderTarget)),
            static_cast<unsigned long long>(CounterDelta(c.setDepthStencilSurface, p.setDepthStencilSurface)),
            static_cast<unsigned long long>(CounterDelta(c.clear, p.clear)),
            static_cast<unsigned long long>(CounterDelta(c.beginScene, p.beginScene)),
            static_cast<unsigned long long>(CounterDelta(c.endScene, p.endScene)),
            static_cast<unsigned long long>(CounterDelta(c.reset, p.reset)),
            static_cast<unsigned long long>(creates));

        Log("d3d9redundancy: frames={} texSame={}/{} rsSame={}/{} shaderSame={}/{} vsSame={}/{} psSame={}/{} constCallSame={}/{} vsConstCallSame={}/{} psConstCallSame={}/{} constVecSame={}/{} vsConstVecSame={}/{} psConstVecSame={}/{}",
            static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(CounterDelta(c.redundantSetTexture, p.redundantSetTexture)),
            static_cast<unsigned long long>(textureSets),
            static_cast<unsigned long long>(CounterDelta(c.redundantSetRenderState, p.redundantSetRenderState)),
            static_cast<unsigned long long>(renderStateSets),
            static_cast<unsigned long long>(
                CounterDelta(c.redundantSetVertexShader, p.redundantSetVertexShader) +
                CounterDelta(c.redundantSetPixelShader, p.redundantSetPixelShader)),
            static_cast<unsigned long long>(shaderSets),
            static_cast<unsigned long long>(CounterDelta(c.redundantSetVertexShader, p.redundantSetVertexShader)),
            static_cast<unsigned long long>(vertexShaderSets),
            static_cast<unsigned long long>(CounterDelta(c.redundantSetPixelShader, p.redundantSetPixelShader)),
            static_cast<unsigned long long>(pixelShaderSets),
            static_cast<unsigned long long>(
                CounterDelta(c.redundantSetVertexShaderConstantF, p.redundantSetVertexShaderConstantF) +
                CounterDelta(c.redundantSetPixelShaderConstantF, p.redundantSetPixelShaderConstantF)),
            static_cast<unsigned long long>(shaderConstSets),
            static_cast<unsigned long long>(CounterDelta(c.redundantSetVertexShaderConstantF, p.redundantSetVertexShaderConstantF)),
            static_cast<unsigned long long>(vertexShaderConstSets),
            static_cast<unsigned long long>(CounterDelta(c.redundantSetPixelShaderConstantF, p.redundantSetPixelShaderConstantF)),
            static_cast<unsigned long long>(pixelShaderConstSets),
            static_cast<unsigned long long>(
                CounterDelta(c.redundantVertexShaderConstantFVectors, p.redundantVertexShaderConstantFVectors) +
                CounterDelta(c.redundantPixelShaderConstantFVectors, p.redundantPixelShaderConstantFVectors)),
            static_cast<unsigned long long>(vertexShaderConstVectors + pixelShaderConstVectors),
            static_cast<unsigned long long>(CounterDelta(c.redundantVertexShaderConstantFVectors, p.redundantVertexShaderConstantFVectors)),
            static_cast<unsigned long long>(vertexShaderConstVectors),
            static_cast<unsigned long long>(CounterDelta(c.redundantPixelShaderConstantFVectors, p.redundantPixelShaderConstantFVectors)),
            static_cast<unsigned long long>(pixelShaderConstVectors));

        m_lastStats = m_stats;
        m_lastStatsTick = now;
    }

    IDirect3DDevice9* m_inner = nullptr;
    VulkanHostDevice* m_vulkanHost = nullptr;
    std::atomic<LONG> m_refs{ 0 };
    D3D9CallCounters m_stats{};
    D3D9CallCounters m_lastStats{};
    DWORD m_lastStatsTick = 0;
    FILE* m_drawTraceFile = nullptr;
    bool m_drawTraceActive = false;
    bool m_drawTraceTruncated = false;
    bool m_drawTraceTriggerWasDown = false;
    UINT m_drawTraceCaptureId = 0;
    UINT m_drawTraceDrawCount = 0;
    DWORD m_drawTraceStartTick = 0;
    D3D9CallsiteEntry m_callsiteEntries[kD3D9CallsiteMaxEntries]{};
    uintptr_t m_gameImageBase = 0;
    uintptr_t m_gameImageSize = 0;
    uintptr_t m_gamePreferredBase = 0;
    UINT m_callsiteEntryCount = 0;
    UINT m_callsiteFrameCount = 0;
    UINT m_callsiteCaptureId = 0;
    UINT m_callsiteParentCpuCaptureId = 0;
    uint64_t m_callsiteTotalDraws = 0;
    uint64_t m_callsiteTotalPrimitives = 0;
    uint64_t m_callsiteCapturedSamples = 0;
    uint64_t m_callsiteDroppedEntries = 0;
    DWORD m_callsiteStartTick = 0;
    bool m_callsiteProfileActive = false;
    bool m_callsiteTriggerWasDown = false;
    bool m_cpuHotspotTriggerWasDown = false;
    bool m_effectProfileTriggerWasDown = false;
    bool m_stateAttributionTriggerWasDown = false;
    DWORD m_renderThreadId = 0;
    UINT m_callsiteRenderTargetWidth = 0;
    UINT m_callsiteRenderTargetHeight = 0;
    D3DFORMAT m_callsiteRenderTargetFormat = D3DFMT_UNKNOWN;
    DWORD m_renderStates[kTrackedRenderStateCount]{};
    bool m_renderStateSeen[kTrackedRenderStateCount]{};
    IDirect3DBaseTexture9* m_textures[kTrackedTextureCount]{};
    bool m_textureSeen[kTrackedTextureCount]{};
    IDirect3DVertexShader9* m_vertexShader = nullptr;
    IDirect3DPixelShader9* m_pixelShader = nullptr;
    bool m_vertexShaderSeen = false;
    bool m_pixelShaderSeen = false;
    float m_vertexShaderConstants[kTrackedVertexShaderConstantCount * 4]{};
    float m_pixelShaderConstants[kTrackedPixelShaderConstantCount * 4]{};
    bool m_vertexShaderConstantSeen[kTrackedVertexShaderConstantCount]{};
    bool m_pixelShaderConstantSeen[kTrackedPixelShaderConstantCount]{};
#ifdef BRIDGE_D3D9_BACKEND_TRACE
    BackendAlphaDrawRecord* m_backendAlphaRing = nullptr;
    size_t m_backendAlphaRingCount = 0;
    uint64_t m_backendRecordSequence = 0;
    uint64_t m_backendAlphaDraws = 0;
    uint64_t m_backendTextureProbes = 0;
    uint64_t m_backendTransparentTextures = 0;
    uint64_t m_backendUnknownAlphaTextures = 0;
    UINT m_backendFrame = 0;
    UINT m_backendDrawInFrame = 0;
    UINT m_backendLastDumpFrame = UINT32_MAX;
    UINT m_backendLastDepthColorWriteLogFrame = UINT32_MAX;
    UINT m_backendLastOpaqueAlphaLogFrame = UINT32_MAX;
    bool m_backendTraceTriggerWasDown = false;
    bool m_backendAutoDumpPending = false;
    uint64_t m_backendCurrentVertexShaderHash = 0;
    uint64_t m_backendCurrentPixelShaderHash = 0;
    uint64_t m_backendPixelConstantEpochHash = 0;
    std::unordered_map<IDirect3DTexture9*, BackendTextureInfo> m_backendTextures;
    std::unordered_map<void*, uint64_t> m_backendVertexShaderHashes;
    std::unordered_map<void*, uint64_t> m_backendPixelShaderHashes;
    std::unordered_map<void*, bool> m_backendPixelShaderHasTexkill;
    std::unordered_map<uint64_t, BackendAlphaPathState> m_backendAlphaPathStates;
#endif
};

class BridgeDirect3D9 final : public IDirect3D9
{
public:
    explicit BridgeDirect3D9(IDirect3D9* inner)
        : m_inner(inner), m_refs(1)
    {
        Log("postfx: wrapped IDirect3D9 0x{:08X} -> 0x{:08X}", reinterpret_cast<std::uintptr_t>(inner), reinterpret_cast<std::uintptr_t>(this));
    }

    ~BridgeDirect3D9()
    {
        if (m_inner) {
            m_inner->Release();
            m_inner = nullptr;
        }
        Log("postfx: d3d9 wrapper destroyed");
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override
    {
        if (!ppvObj) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
            *ppvObj = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        return m_inner->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG refs = m_refs.fetch_sub(1) - 1;
        if (refs == 0) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { return m_inner->RegisterSoftwareDevice(pInitializeFunction); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return m_inner->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override { return m_inner->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { return m_inner->GetAdapterModeCount(Adapter, Format); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override { return m_inner->EnumAdapterModes(Adapter, Format, Mode, pMode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override { return m_inner->GetAdapterDisplayMode(Adapter, pMode); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return m_inner->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return m_inner->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { return m_inner->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return m_inner->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return m_inner->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override { return m_inner->GetDeviceCaps(Adapter, DeviceType, pCaps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return m_inner->GetAdapterMonitor(Adapter); }

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
        DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        IDirect3DDevice9* device = nullptr;
        HRESULT hr = m_inner->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
            pPresentationParameters, &device);
        if (FAILED(hr) || !device) {
            if (ppReturnedDeviceInterface) *ppReturnedDeviceInterface = device;
            return hr;
        }

        // The device creation thread is also used in unwrapped mode. Keep M1
        // out of Present/Draw and apply it only once per creating thread.
        // DeviceOwner uses CPU Sets only: this thread has no controlled MMCSS
        // shutdown point in the current Bridge lifecycle.
        static thread_local renderstack::scheduling::ThreadSchedulingScope scheduling(
            renderstack::scheduling::Options{g_threadSchedulingOptions.enabled, false},
            renderstack::scheduling::Role::DeviceOwner, &ThreadSchedulingLog);

        for (auto& plugin : g_plugins) {
            SafePluginCall("OnCreateDevice", plugin.onCreateDevice, device, pPresentationParameters);
        }

        if (ShouldWrapD3D9Device()) {
            *ppReturnedDeviceInterface = new BridgeDirect3DDevice9(device);
        } else {
            OnUnwrappedDeviceCreated(device);
            *ppReturnedDeviceInterface = device;
        }
        return hr;
    }

private:
    IDirect3D9* m_inner = nullptr;
    std::atomic<LONG> m_refs{ 0 };
};

extern "C" __declspec(dllexport) IDirect3D9* __stdcall Direct3DCreate9(UINT SDKVersion)
{
    Log("Direct3DCreate9 called SDKVersion={}", SDKVersion);
    EnsurePostFxPluginsLoaded();

    if (g_ps_Direct3DCreate9) {
        Log("Direct3DCreate9: using ProperShaders proxy");
        IDirect3D9* result = g_ps_Direct3DCreate9(SDKVersion);
        Log("Direct3DCreate9: ProperShaders proxy returned 0x{:08X}", reinterpret_cast<std::uintptr_t>(result));
        if (result) {
            // The IDirect3D9 wrapper only intercepts creation-time calls and
            // costs nothing per frame; it must stay so CreateDevice can attach
            // the Vulkan host and the unwrapped support worker.
            return new BridgeDirect3D9(result);
        }
        Log("Direct3DCreate9: ProperShaders proxy returned null, falling back to system d3d9");
    }

    if (g_real_Direct3DCreate9) {
        const char* backendName = g_useDxvkBackend ? "DXVK" : "SystemD3D9";
        Log("Direct3DCreate9: using configured backend={}", backendName);
        IDirect3D9* result = g_real_Direct3DCreate9(SDKVersion);
        Log("Direct3DCreate9: backend={} returned 0x{:08X}", backendName, reinterpret_cast<std::uintptr_t>(result));
        if (result) {
            // Always wrap the interface (creation-time only, no per-frame cost)
            // so CreateDevice interception keeps working in unwrapped mode.
            return new BridgeDirect3D9(result);
        }
        return result;
    }

    return nullptr;
}

extern "C" __declspec(dllexport) int __stdcall DebugSetLevel()
{
    if (g_psProxy) {
        auto fn = (PFN_DebugSetLevel)GetProcAddress(g_psProxy, "DebugSetLevel");
        if (fn) return fn();
    }
    if (g_realD3D9) {
        auto fn = (PFN_DebugSetLevel)GetProcAddress(g_realD3D9, "DebugSetLevel");
        if (fn) return fn();
    }
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall DebugSetMute()
{
    if (g_psProxy) {
        auto fn = (PFN_DebugSetMute)GetProcAddress(g_psProxy, "DebugSetMute");
        if (fn) return fn();
    }
    if (g_realD3D9) {
        auto fn = (PFN_DebugSetMute)GetProcAddress(g_realD3D9, "DebugSetMute");
        if (fn) return fn();
    }
    return 0;
}

extern "C" __declspec(dllexport) void* __stdcall Direct3DShaderValidatorCreate9()
{
    if (g_psProxy) {
        auto fn = (PFN_Direct3DShaderValidatorCreate9)GetProcAddress(g_psProxy, "Direct3DShaderValidatorCreate9");
        if (fn) return fn();
    }
    if (g_realD3D9) {
        auto fn = (PFN_Direct3DShaderValidatorCreate9)GetProcAddress(g_realD3D9, "Direct3DShaderValidatorCreate9");
        if (fn) return fn();
    }
    return nullptr;
}

extern "C" __declspec(dllexport) void* __stdcall PSGPError()
{
    if (g_psProxy) {
        auto fn = (PFN_PSGPError)GetProcAddress(g_psProxy, "PSGPError");
        if (fn) return fn();
    }
    if (g_realD3D9) {
        auto fn = (PFN_PSGPError)GetProcAddress(g_realD3D9, "PSGPError");
        if (fn) return fn();
    }
    return nullptr;
}

extern "C" __declspec(dllexport) void* __stdcall PSGPSampleTexture()
{
    if (g_psProxy) {
        auto fn = (PFN_PSGPSampleTexture)GetProcAddress(g_psProxy, "PSGPSampleTexture");
        if (fn) return fn();
    }
    if (g_realD3D9) {
        auto fn = (PFN_PSGPSampleTexture)GetProcAddress(g_realD3D9, "PSGPSampleTexture");
        if (fn) return fn();
    }
    return nullptr;
}

extern "C" __declspec(dllexport) HRESULT __stdcall CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    if (g_realD3D9) {
        auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(g_realD3D9, "CreateDXGIFactory"));
        if (fn) return fn(riid, ppFactory);
    }
    return E_NOINTERFACE;
}

extern "C" __declspec(dllexport) HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    if (g_realD3D9) {
        auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(g_realD3D9, "CreateDXGIFactory1"));
        if (fn) return fn(riid, ppFactory);
    }
    return E_NOINTERFACE;
}

extern "C" __declspec(dllexport) HRESULT __stdcall CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    if (g_realD3D9) {
        auto fn = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(g_realD3D9, "CreateDXGIFactory2"));
        if (fn) return fn(flags, riid, ppFactory);
    }
    return E_NOINTERFACE;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DXGIDeclareAdapterRemovalSupport()
{
    if (g_realD3D9) {
        auto fn = reinterpret_cast<PFN_DXGIDeclareAdapterRemovalSupport>(
            GetProcAddress(g_realD3D9, "DXGIDeclareAdapterRemovalSupport"));
        if (fn) return fn();
    }
    return E_NOINTERFACE;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DXGIGetDebugInterface1(UINT flags, REFIID riid, void** ppDebug)
{
    if (g_realD3D9) {
        auto fn = reinterpret_cast<PFN_DXGIGetDebugInterface1>(
            GetProcAddress(g_realD3D9, "DXGIGetDebugInterface1"));
        if (fn) return fn(flags, riid, ppDebug);
    }
    return E_NOINTERFACE;
}

static void InstallSafeStub()
{
    constexpr uintptr_t kBadAddress = 0x615CD2C0;
    constexpr uintptr_t kAllocBase = kBadAddress & 0xFFFF0000;
    constexpr size_t kAllocSize = (kBadAddress - kAllocBase) + 0x100;

    void* stubMem = VirtualAlloc(reinterpret_cast<void*>(kAllocBase), kAllocSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stubMem) {
        Log("stub: VirtualAlloc at 0x{:08X} size=0x{:X} failed (err={})",
            kAllocBase, kAllocSize, GetLastError());
        return;
    }

    uintptr_t stubBase = reinterpret_cast<uintptr_t>(stubMem);
    if (kBadAddress >= stubBase && kBadAddress < stubBase + kAllocSize) {
        uint8_t* stub = reinterpret_cast<uint8_t*>(kBadAddress);
        // Safe fallback for a corrupted FLA AddTxdSlot jump. Returning the argument
        // can turn a TXD name pointer into an index and corrupt later loading.
        stub[0] = 0x33; stub[1] = 0xC0;  // xor eax, eax
        stub[2] = 0xC3;                  // ret
        Log("stub: installed xor eax,eax;ret at 0x{:08X}", kBadAddress);
    } else {
        Log("stub: alloc at 0x{:08X} does not cover 0x{:08X}", reinterpret_cast<std::uintptr_t>(stubMem), kBadAddress);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = hModule;
        DisableThreadLibraryCalls(hModule);
        Log("BridgeD3D9: loaded mode={}", kBackendTraceBuild ? "backend-trace" : "root-proxy");

        if (!kBackendTraceBuild) {
            InstallSafeStub();
        }
        LoadBridgeConfig();
        if (!kBackendTraceBuild) {
            ApplyAffinityAndPriority("startup");
        }
        if (!kBackendTraceBuild && g_affinityEnable && g_affinityReapply && g_affinityReapplyCount > 0) {
            HANDLE thread = CreateThread(nullptr, 0, AffinityReapplyThread, nullptr, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            } else {
                Log("affinity: CreateThread failed err={}", GetLastError());
            }
        }

        g_realD3D9 = LoadBackendD3D9();
        if (g_realD3D9) {
            g_real_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_realD3D9, "Direct3DCreate9");
        }

        if (!kBackendTraceBuild) {
            g_psProxy = LoadPSProxy();
            if (g_psProxy) {
                PatchPSProxyBackend();
                g_ps_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_psProxy, "Direct3DCreate9");
                Log("BridgeD3D9: primary proxy={} Direct3DCreate9=0x{:08X}",
                    g_primaryProxyName[0] ? g_primaryProxyName : "(unnamed)", reinterpret_cast<std::uintptr_t>(g_ps_Direct3DCreate9));
            }
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        RestoreProperShadersCreateEffectHook();
        RestoreProperShadersOptimizationPatches();
        ShutdownPostFxPlugins();
    }
    return TRUE;
}
