// ProperShaders render-state guard implementation.
//
// Ported from FLA++ (FLACompatBridge/BridgeCompatProperShaders.cpp): the three
// ProperShaders patch sites (PS+0x1ED80 RenderScene, PS+0x1ED60
// RenderFadingEntities, PS+0x1EAB0 main-scene state restore) are wrapped with
// hand-written rel32 hooks, RenderWare's alpha-test states are synchronised
// onto the D3D9 device, and the main-scene baseline stack is restored after the
// patched scene render. The state ring lives in ProperShadersStateRing.cpp.
//
// The logic is a line-for-line port; only the log plumbing (std::format), the
// configuration source (BridgeD3D9.ini) and the activation entry point (D3D9
// device creation) differ from the FLA++ original.

#include "ProperShadersRenderStateGuard.h"
#include "ProperShadersStateRing.h"

#include <cstdio>
#include <cstring>
#include <format>
#include <print>

namespace
{
template <class... Args>
void Log(std::format_string<Args...> fmt, Args&&... args)
{
    static FILE* f = nullptr;
    if (!f) {
        f = fopen("scripts\\BridgeD3D9.log", "a");
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
} // namespace

ProperShadersRenderStateGuardConfig g_properShadersRenderStateGuardConfig{};

uintptr_t g_properShadersPatchedRenderSceneTrampoline = 0;
uintptr_t g_properShadersRenderFadingEntitiesTrampoline = 0;
uintptr_t g_properShadersMainSceneStateTrampoline = 0;

LONG g_properShadersRenderStateGuardInstallState = 0;
LONG g_properShadersFadingStateGuardInstallState = 0;
LONG g_properShadersMainSceneStateGuardInstallState = 0;
LONG g_properShadersRenderStateGuardApiReady = 0;
LONG g_properShadersRenderStateSampleAttempts = 0;
LONG g_properShadersRenderStateChangeLogs = 0;
LONG g_properShadersFadingStateChangeLogs = 0;
LONG g_properShadersMainSceneStateChangeLogs = 0;

ProperShadersMainSceneBaseline
    g_properShadersMainSceneBaselines[kProperShadersMainSceneBaselineCapacity]{};
LONG g_properShadersMainSceneBaselineDepth = 0;

static bool SynchronizeProperShadersFadingAlphaState();

// ---- configuration ----

void LoadProperShadersRenderStateGuardConfig(const char* iniPath)
{
    if (!iniPath || !iniPath[0]) {
        return;
    }

    g_properShadersRenderStateGuardConfig.enable =
        GetPrivateProfileIntA("ProperShadersRenderStateGuard", "Enable", 1, iniPath) != 0;
    g_properShadersRenderStateGuardConfig.enableStateRing =
        GetPrivateProfileIntA("ProperShadersStateRing", "Enable", 0, iniPath) != 0;
    g_properShadersRenderStateGuardConfig.stateRingTriggerVirtualKey = static_cast<int>(
        GetPrivateProfileIntA("ProperShadersStateRing", "TriggerVirtualKey", 121, iniPath));

    Log("propershaders guard: enable={} stateRing={} triggerVK={}",
        g_properShadersRenderStateGuardConfig.enable ? 1 : 0,
        g_properShadersRenderStateGuardConfig.enableStateRing ? 1 : 0,
        g_properShadersRenderStateGuardConfig.stateRingTriggerVirtualKey);
}

// ---- module and patch helpers (ported from BridgeMemory.cpp) ----

static bool IsReadableCommitted(uintptr_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t end = base + static_cast<uintptr_t>(mbi.RegionSize);
    if (address < base || address + size > end) {
        return false;
    }

    return mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD);
}

static bool IsExecutableCommitted(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
        protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

static bool WriteBytesWithProtect(uintptr_t destination, const uint8_t* bytes, size_t size)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(destination), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("patch: protect failed address=0x{:08X} size=0x{:X} gle={}",
            destination, static_cast<unsigned>(size), GetLastError());
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(destination), bytes, size);

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(destination), size, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(destination), size);
    return true;
}

static uintptr_t DecodeRel32JumpTarget(uintptr_t address)
{
    uint8_t bytes[5]{};
    if (!IsReadableCommitted(address, sizeof(bytes))) {
        return 0;
    }

    __try {
        std::memcpy(bytes, reinterpret_cast<const void*>(address), sizeof(bytes));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    if (bytes[0] != 0xE8 && bytes[0] != 0xE9) {
        return 0;
    }

    int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    return address + 5 + rel;
}

static bool WriteRel32Jump(uintptr_t source, uintptr_t target)
{
    const int64_t diff = static_cast<int64_t>(target) - static_cast<int64_t>(source + 5);
    if (diff < INT32_MIN || diff > INT32_MAX) {
        Log("bridge stub: rel32 jump out of range source=0x{:08X} target=0x{:08X} diff={}",
            source, target, diff);
        return false;
    }

    uint8_t bytes[5]{ 0xE9, 0, 0, 0, 0 };
    const int32_t rel = static_cast<int32_t>(diff);
    std::memcpy(bytes + 1, &rel, sizeof(rel));
    return WriteBytesWithProtect(source, bytes, sizeof(bytes));
}

static uintptr_t CreateRel32Trampoline(uintptr_t source, size_t stolenBytes)
{
    if (stolenBytes < 5 || !IsReadableCommitted(source, stolenBytes)) {
        return 0;
    }

    uint8_t* gateway = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, stolenBytes + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!gateway) {
        Log("trampoline: VirtualAlloc failed source=0x{:08X} stolen={} gle={}",
            source, static_cast<uint32_t>(stolenBytes), GetLastError());
        return 0;
    }

    __try {
        std::memcpy(gateway, reinterpret_cast<const void*>(source), stolenBytes);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualFree(gateway, 0, MEM_RELEASE);
        Log("trampoline: read exception source=0x{:08X} stolen={}",
            source, static_cast<uint32_t>(stolenBytes));
        return 0;
    }

    const uintptr_t jumpBackSource = reinterpret_cast<uintptr_t>(gateway) + stolenBytes;
    const uintptr_t jumpBackTarget = source + stolenBytes;
    const int64_t diff = static_cast<int64_t>(jumpBackTarget) - static_cast<int64_t>(jumpBackSource + 5);
    if (diff < INT32_MIN || diff > INT32_MAX) {
        VirtualFree(gateway, 0, MEM_RELEASE);
        Log("trampoline: jump back out of range source=0x{:08X} target=0x{:08X} diff={}",
            jumpBackSource, jumpBackTarget, diff);
        return 0;
    }

    gateway[stolenBytes] = 0xE9;
    const int32_t rel = static_cast<int32_t>(diff);
    std::memcpy(gateway + stolenBytes + 1, &rel, sizeof(rel));
    return reinterpret_cast<uintptr_t>(gateway);
}

static uint32_t CalculateModuleFileTextHash(HMODULE module)
{
    if (!module) {
        return 0;
    }

    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)))) {
        return 0;
    }

    HANDLE file = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 64ll * 1024ll * 1024ll) {
        CloseHandle(file);
        return 0;
    }

    const size_t size = static_cast<size_t>(fileSize.QuadPart);
    uint8_t* bytes = new uint8_t[size];
    DWORD bytesRead = 0;
    const bool readOk = ReadFile(file, bytes, static_cast<DWORD>(size), &bytesRead, nullptr) && bytesRead == size;
    CloseHandle(file);
    if (!readOk || size < sizeof(IMAGE_DOS_HEADER)) {
        delete[] bytes;
        return 0;
    }

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    const size_t ntOffset = dos->e_lfanew > 0 ? static_cast<size_t>(dos->e_lfanew) : size;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || ntOffset > size || size - ntOffset < sizeof(IMAGE_NT_HEADERS)) {
        delete[] bytes;
        return 0;
    }

    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(bytes + ntOffset);
    const size_t sectionOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
    const size_t sectionBytes = static_cast<size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (nt->Signature != IMAGE_NT_SIGNATURE || sectionOffset > size || sectionBytes > size - sectionOffset) {
        delete[] bytes;
        return 0;
    }

    uint32_t crcTable[256]{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crcTable[i] = c;
    }

    uint32_t result = 0;
    const IMAGE_SECTION_HEADER* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(bytes + sectionOffset);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& section = sections[i];
        if (std::memcmp(section.Name, ".text\0\0\0", 8) != 0) {
            continue;
        }

        const size_t virtualSize = section.Misc.VirtualSize;
        const size_t rawSize = section.SizeOfRawData;
        const size_t rawOffset = section.PointerToRawData;
        if (virtualSize == 0 || virtualSize > 64u * 1024u * 1024u ||
            rawOffset > size || rawSize > size - rawOffset) {
            break;
        }

        uint32_t hash = 0xFFFFFFFFu;
        const size_t fileBackedSize = virtualSize < rawSize ? virtualSize : rawSize;
        for (size_t j = 0; j < fileBackedSize; ++j) {
            hash = crcTable[(hash ^ bytes[rawOffset + j]) & 0xFF] ^ (hash >> 8);
        }
        for (size_t j = fileBackedSize; j < virtualSize; ++j) {
            hash = crcTable[hash & 0xFF] ^ (hash >> 8);
        }
        result = hash ^ 0xFFFFFFFFu;
        break;
    }

    delete[] bytes;
    return result;
}

static bool IsSupportedProperShadersTextHash(uint32_t hash)
{
    return hash == kSupportedProperShadersTextHash;
}

// ---- D3D9 device API resolution ----

static bool ResolveD3D9RenderStateApi(D3D9RenderStateApi* api)
{
    if (!api || !IsReadableCommitted(kRwD3D9DevicePointer, sizeof(uintptr_t))) {
        return false;
    }

    __try {
        void* device = *reinterpret_cast<void* const*>(kRwD3D9DevicePointer);
        if (!device || !IsReadableCommitted(reinterpret_cast<uintptr_t>(device), sizeof(uintptr_t))) {
            return false;
        }

        void** vtable = *reinterpret_cast<void***>(device);
        const size_t requiredVtableSize = (kD3D9GetRenderStateVtableIndex + 1) * sizeof(void*);
        if (!vtable || !IsReadableCommitted(reinterpret_cast<uintptr_t>(vtable), requiredVtableSize)) {
            return false;
        }

        // The FLA++ original resolved both entries from the get slot; kept as
        // is so this port stays behaviour-identical (see the header TODO).
        auto setRenderState = reinterpret_cast<D3D9SetRenderStateFn>(
            vtable[kD3D9GetRenderStateVtableIndex]);
        auto getRenderState = reinterpret_cast<D3D9GetRenderStateFn>(
            vtable[kD3D9GetRenderStateVtableIndex]);
        if (!setRenderState || !getRenderState ||
            !IsExecutableCommitted(reinterpret_cast<uintptr_t>(setRenderState)) ||
            !IsExecutableCommitted(reinterpret_cast<uintptr_t>(getRenderState))) {
            return false;
        }

        api->device = device;
        api->setRenderState = setRenderState;
        api->getRenderState = getRenderState;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool ReadD3D9DeviceRenderState(uint32_t state, uint32_t* value)
{
    if (!value) {
        return false;
    }

    D3D9RenderStateApi api{};
    if (!ResolveD3D9RenderStateApi(&api)) {
        return false;
    }

    __try {
        return api.getRenderState(api.device, state, value) >= 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SynchronizeD3D9DeviceRenderState(
    uint32_t state,
    uint32_t desired,
    uint32_t* actualBefore,
    bool* changed)
{
    D3D9RenderStateApi api{};
    if (!ResolveD3D9RenderStateApi(&api)) {
        return false;
    }

    __try {
        uint32_t actual = 0;
        if (api.getRenderState(api.device, state, &actual) < 0) {
            return false;
        }
        if (actualBefore) {
            *actualBefore = actual;
        }
        const bool needsChange = actual != desired;
        if (needsChange && api.setRenderState(api.device, state, desired) < 0) {
            return false;
        }
        if (changed) {
            *changed = needsChange;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ResolveD3D9StateTraceApi(D3D9StateTraceApi* api)
{
    if (!api) {
        return false;
    }

    D3D9RenderStateApi renderStateApi{};
    if (!ResolveD3D9RenderStateApi(&renderStateApi)) {
        return false;
    }

    __try {
        void** vtable = *reinterpret_cast<void***>(renderStateApi.device);
        constexpr size_t kRequiredVtableEntries = 112;
        if (!vtable || !IsReadableCommitted(
                reinterpret_cast<uintptr_t>(vtable),
                kRequiredVtableEntries * sizeof(void*))) {
            return false;
        }

        api->device = renderStateApi.device;
        api->getRenderState = renderStateApi.getRenderState;
        api->getRenderTarget = reinterpret_cast<D3D9GetRenderTargetFn>(vtable[38]);
        api->getDepthStencilSurface =
            reinterpret_cast<D3D9GetDepthStencilSurfaceFn>(vtable[40]);
        api->getViewport = reinterpret_cast<D3D9GetViewportFn>(vtable[48]);
        api->getTexture = reinterpret_cast<D3D9GetTextureFn>(vtable[64]);
        api->getTextureStageState =
            reinterpret_cast<D3D9GetTextureStageStateFn>(vtable[66]);
        api->getSamplerState = reinterpret_cast<D3D9GetSamplerStateFn>(vtable[68]);
        api->getVertexShader = reinterpret_cast<D3D9GetVertexShaderFn>(vtable[94]);
        api->getPixelShader = reinterpret_cast<D3D9GetPixelShaderFn>(vtable[111]);

        const uintptr_t functions[] = {
            reinterpret_cast<uintptr_t>(api->getRenderTarget),
            reinterpret_cast<uintptr_t>(api->getDepthStencilSurface),
            reinterpret_cast<uintptr_t>(api->getViewport),
            reinterpret_cast<uintptr_t>(api->getTexture),
            reinterpret_cast<uintptr_t>(api->getTextureStageState),
            reinterpret_cast<uintptr_t>(api->getSamplerState),
            reinterpret_cast<uintptr_t>(api->getVertexShader),
            reinterpret_cast<uintptr_t>(api->getPixelShader),
        };
        for (uintptr_t function : functions) {
            if (!function || !IsExecutableCommitted(function)) {
                return false;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static void ReleaseD3D9StateTraceObject(void* object)
{
    if (!object || !IsReadableCommitted(reinterpret_cast<uintptr_t>(object), sizeof(void*))) {
        return;
    }

    __try {
        void** vtable = *reinterpret_cast<void***>(object);
        if (!vtable || !IsReadableCommitted(reinterpret_cast<uintptr_t>(vtable), 3 * sizeof(void*))) {
            return;
        }
        auto release = reinterpret_cast<D3D9ReleaseFn>(vtable[2]);
        if (release && IsExecutableCommitted(reinterpret_cast<uintptr_t>(release))) {
            release(object);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void CaptureProperShadersPhaseState(ProperShadersD3D9StateSnapshot* snapshot)
{
    if (!snapshot) {
        return;
    }

    HMODULE properShaders = GetModuleHandleA("ProperShaders.asi");
    if (!properShaders) {
        properShaders = GetModuleHandleA("propershaders.asi");
    }
    if (!properShaders) {
        return;
    }

    const uintptr_t phaseAddress =
        reinterpret_cast<uintptr_t>(properShaders) + 0x153CE4;
    if (!IsReadableCommitted(phaseAddress, 11)) {
        return;
    }

    __try {
        uint8_t phaseBytes[11]{};
        std::memcpy(phaseBytes, reinterpret_cast<const void*>(phaseAddress), sizeof(phaseBytes));
        std::memcpy(&snapshot->phaseAlternate, phaseBytes, sizeof(snapshot->phaseAlternate));
        snapshot->phaseByte9 = phaseBytes[9];
        snapshot->phaseByte10 = phaseBytes[10];
        snapshot->validMask |= 0x00000002;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool CaptureProperShadersD3D9StateSnapshot(ProperShadersD3D9StateSnapshot* snapshot)
{
    if (!snapshot) {
        return false;
    }

    *snapshot = ProperShadersD3D9StateSnapshot{};
    for (size_t i = 0; i < kProperShadersTraceRenderStateCount; ++i) {
        snapshot->renderStates[i] = UINT32_MAX;
    }
    for (size_t i = 0; i < kProperShadersTraceTextureStageStateCount; ++i) {
        snapshot->textureStageStates[i] = UINT32_MAX;
    }
    for (size_t i = 0; i < kProperShadersTraceSamplerStateCount; ++i) {
        snapshot->samplerStates[i] = UINT32_MAX;
    }
    CaptureProperShadersPhaseState(snapshot);

    D3D9StateTraceApi api{};
    if (!ResolveD3D9StateTraceApi(&api)) {
        return false;
    }

    snapshot->device = api.device;
    snapshot->validMask |= 0x00000001;
    __try {
        for (size_t i = 0; i < kProperShadersTraceRenderStateCount; ++i) {
            uint32_t value = UINT32_MAX;
            if (api.getRenderState(
                    api.device,
                    kProperShadersTraceRenderStates[i].state,
                    &value) >= 0) {
                snapshot->renderStates[i] = value;
            }
        }
        for (size_t i = 0; i < kProperShadersTraceTextureStageStateCount; ++i) {
            uint32_t value = UINT32_MAX;
            if (api.getTextureStageState(
                    api.device,
                    0,
                    kProperShadersTraceTextureStageStates[i].state,
                    &value) >= 0) {
                snapshot->textureStageStates[i] = value;
            }
        }
        for (size_t i = 0; i < kProperShadersTraceSamplerStateCount; ++i) {
            uint32_t value = UINT32_MAX;
            if (api.getSamplerState(
                    api.device,
                    0,
                    kProperShadersTraceSamplerStates[i].state,
                    &value) >= 0) {
                snapshot->samplerStates[i] = value;
            }
        }

        void* object = nullptr;
        if (api.getTexture(api.device, 0, &object) >= 0) {
            snapshot->texture0 = object;
            snapshot->validMask |= 0x00000004;
            ReleaseD3D9StateTraceObject(object);
        }
        object = nullptr;
        if (api.getVertexShader(api.device, &object) >= 0) {
            snapshot->vertexShader = object;
            snapshot->validMask |= 0x00000008;
            ReleaseD3D9StateTraceObject(object);
        }
        object = nullptr;
        if (api.getPixelShader(api.device, &object) >= 0) {
            snapshot->pixelShader = object;
            snapshot->validMask |= 0x00000010;
            ReleaseD3D9StateTraceObject(object);
        }
        object = nullptr;
        if (api.getRenderTarget(api.device, 0, &object) >= 0) {
            snapshot->renderTarget0 = object;
            snapshot->validMask |= 0x00000020;
            ReleaseD3D9StateTraceObject(object);
        }
        object = nullptr;
        if (api.getDepthStencilSurface(api.device, &object) >= 0) {
            snapshot->depthStencil = object;
            snapshot->validMask |= 0x00000040;
            ReleaseD3D9StateTraceObject(object);
        }
        if (api.getViewport(api.device, &snapshot->viewport) >= 0) {
            snapshot->validMask |= 0x00000080;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ---- alpha render-state synchronisation ----

static bool SynchronizeD3D9DeviceAlphaRenderStates(
    const uint32_t* desired,
    uint32_t* actualBefore,
    uint32_t* changedMask)
{
    if (!desired) {
        return false;
    }

    D3D9RenderStateApi api{};
    if (!ResolveD3D9RenderStateApi(&api)) {
        return false;
    }

    uint32_t mask = 0;
    __try {
        for (size_t i = 0; i < kProperShadersAlphaRenderStateCount; ++i) {
            uint32_t actual = 0;
            const LONG getResult = api.getRenderState(
                api.device, kProperShadersAlphaRenderStates[i], &actual);
            if (getResult < 0) {
                return false;
            }
            if (actualBefore) {
                actualBefore[i] = actual;
            }
            // RenderWare reports UINT32_MAX for D3D9 states that it does not
            // track. Treat that as an unavailable value, never as a device
            // state to apply.
            if (desired[i] == UINT32_MAX) {
                continue;
            }
            if (actual != desired[i]) {
                const LONG setResult = api.setRenderState(
                    api.device, kProperShadersAlphaRenderStates[i], desired[i]);
                if (setResult < 0) {
                    return false;
                }
                mask |= 1u << i;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (changedMask) {
        *changedMask = mask;
    }
    return true;
}

bool CaptureProperShadersAlphaRenderStates(uint32_t* values)
{
    if (!values || InterlockedCompareExchange(&g_properShadersRenderStateGuardApiReady, 0, 0) == 0) {
        return false;
    }

    using GetRenderStateFn = void(__cdecl*)(uint32_t, void*);
    auto getRenderState = reinterpret_cast<GetRenderStateFn>(kRwD3D9GetRenderState);
    __try {
        for (size_t i = 0; i < kProperShadersAlphaRenderStateCount; ++i) {
            getRenderState(kProperShadersAlphaRenderStates[i], &values[i]);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_properShadersRenderStateGuardApiReady, 0);
        return false;
    }
    return true;
}

static bool RestoreProperShadersAlphaTestRenderStates(const uint32_t* values)
{
    if (!values || InterlockedCompareExchange(&g_properShadersRenderStateGuardApiReady, 0, 0) == 0) {
        return false;
    }

    using SetRenderStateFn = void(__cdecl*)(uint32_t, uint32_t);
    auto setRenderState = reinterpret_cast<SetRenderStateFn>(kRwD3D9SetRenderState);
    __try {
        setRenderState(kProperShadersAlphaRenderStates[0], values[0]);
        setRenderState(kProperShadersAlphaRenderStates[3], values[3]);
        setRenderState(kProperShadersAlphaRenderStates[4], values[4]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_properShadersRenderStateGuardApiReady, 0);
        return false;
    }
    return true;
}

// ---- main-scene baseline stack ----

static int PushProperShadersMainSceneBaseline(
    const uint32_t* alphaStates,
    uint32_t* stencilEnable,
    bool* stencilValid)
{
    if (!alphaStates) {
        return -1;
    }

    const LONG depth = g_properShadersMainSceneBaselineDepth;
    if (depth < 0 || depth >= static_cast<LONG>(kProperShadersMainSceneBaselineCapacity)) {
        return -1;
    }

    ProperShadersMainSceneBaseline baseline{};
    baseline.valid = true;
    baseline.threadId = GetCurrentThreadId();
    baseline.alphaTest = alphaStates[0];
    baseline.alphaRef = alphaStates[3];
    baseline.alphaFunc = alphaStates[4];
    baseline.stencilValid = ReadD3D9DeviceRenderState(52, &baseline.stencilEnable);
    g_properShadersMainSceneBaselines[depth] = baseline;
    g_properShadersMainSceneBaselineDepth = depth + 1;

    if (stencilEnable) {
        *stencilEnable = baseline.stencilEnable;
    }
    if (stencilValid) {
        *stencilValid = baseline.stencilValid;
    }
    return static_cast<int>(depth);
}

static void PopProperShadersMainSceneBaseline(int baselineIndex)
{
    if (baselineIndex < 0 ||
        baselineIndex >= static_cast<int>(kProperShadersMainSceneBaselineCapacity)) {
        return;
    }

    g_properShadersMainSceneBaselines[baselineIndex] = ProperShadersMainSceneBaseline{};
    if (g_properShadersMainSceneBaselineDepth == baselineIndex + 1) {
        g_properShadersMainSceneBaselineDepth = baselineIndex;
    }
}

extern "C" void __cdecl Bridge_RestoreProperShadersMainSceneBaseline()
{
    const LONG depth = g_properShadersMainSceneBaselineDepth;
    if (depth <= 0 || depth > static_cast<LONG>(kProperShadersMainSceneBaselineCapacity)) {
        return;
    }

    const ProperShadersMainSceneBaseline baseline =
        g_properShadersMainSceneBaselines[depth - 1];
    if (!baseline.valid || baseline.threadId != GetCurrentThreadId()) {
        return;
    }

    uint32_t desired[kProperShadersAlphaRenderStateCount]{};
    for (size_t i = 0; i < kProperShadersAlphaRenderStateCount; ++i) {
        desired[i] = UINT32_MAX;
    }
    desired[0] = baseline.alphaTest;
    desired[3] = baseline.alphaRef;
    desired[4] = baseline.alphaFunc;

    const bool rwRestored = RestoreProperShadersAlphaTestRenderStates(desired);
    uint32_t deviceBefore[kProperShadersAlphaRenderStateCount]{};
    uint32_t deviceChangedMask = 0;
    const bool deviceRestored = rwRestored && SynchronizeD3D9DeviceAlphaRenderStates(
        desired, deviceBefore, &deviceChangedMask);

    uint32_t stencilBefore = UINT32_MAX;
    bool stencilChanged = false;
    const bool stencilRestored = !baseline.stencilValid || SynchronizeD3D9DeviceRenderState(
        52,
        baseline.stencilEnable,
        &stencilBefore,
        &stencilChanged);

    if (deviceChangedMask != 0 || stencilChanged) {
        const LONG logIndex = InterlockedIncrement(&g_properShadersMainSceneStateChangeLogs);
        if (logIndex <= 32) {
            Log("proper shaders main-scene guard: rwRestored={} deviceRestored={} deviceMask=0x{:03X} stencilRestored={} stencilChanged={} stencilActual={} stencilExpected={} alphaTest={} alphaRef={} alphaFunc={}",
                rwRestored ? 1 : 0,
                deviceRestored ? 1 : 0,
                deviceChangedMask,
                stencilRestored ? 1 : 0,
                stencilChanged ? 1 : 0,
                stencilBefore,
                baseline.stencilEnable,
                baseline.alphaTest,
                baseline.alphaRef,
                baseline.alphaFunc);
        }
    }
}

// ---- guarded ProperShaders entry points ----

#if defined(_M_IX86)
extern "C" __declspec(naked) void Bridge_ProperShadersMainSceneStateGuard()
{
    __asm
    {
        pushfd
        pushad
        call Bridge_RestoreProperShadersMainSceneBaseline
        popad
        popfd
        jmp dword ptr [g_properShadersMainSceneStateTrampoline]
    }
}
#else
extern "C" void Bridge_ProperShadersMainSceneStateGuard()
{
    Bridge_RestoreProperShadersMainSceneBaseline();
}
#endif

extern "C" void __cdecl Bridge_ProperShadersPatchedRenderSceneGuard()
{
    const uintptr_t trampoline = g_properShadersPatchedRenderSceneTrampoline;
    if (!trampoline) {
        return;
    }

    const bool traceEnabled = g_properShadersRenderStateGuardConfig.enableStateRing;
    ProperShadersStateTraceRecord traceRecord{};
    if (traceEnabled) {
        InitializeProperShadersStateTraceRecord(
            &traceRecord, kProperShadersStateTraceRenderScene);
        CaptureProperShadersStateTracePoint(&traceRecord, 0);
    }

    // ProperShaders can leave the low-level D3D9 cache at its depth-pass
    // values before this guard is installed. Reconstruct the RW alpha state
    // first so the snapshot is a usable scene-render baseline.
    const bool entrySynchronized = SynchronizeProperShadersFadingAlphaState();
    if (traceEnabled) {
        CaptureProperShadersStateTracePoint(&traceRecord, 1);
        if (entrySynchronized) {
            traceRecord.flags |= 0x00000001;
        }
    }

    uint32_t before[kProperShadersAlphaRenderStateCount]{};
    bool captured = false;
    if (traceEnabled && (traceRecord.rwValidMask & (1u << 1)) != 0) {
        std::memcpy(before, traceRecord.rwStates[1], sizeof(before));
        captured = true;
    } else {
        captured = CaptureProperShadersAlphaRenderStates(before);
    }

    uint32_t baselineStencilEnable = 0;
    bool baselineStencilValid = false;
    const int mainSceneBaselineIndex = captured
        ? PushProperShadersMainSceneBaseline(
            before, &baselineStencilEnable, &baselineStencilValid)
        : -1;
    reinterpret_cast<void(__cdecl*)()>(trampoline)();
    PopProperShadersMainSceneBaseline(mainSceneBaselineIndex);

    uint32_t after[kProperShadersAlphaRenderStateCount]{};
    const LONG sample = InterlockedIncrement(&g_properShadersRenderStateSampleAttempts);
    bool sampled = false;
    if (traceEnabled) {
        CaptureProperShadersStateTracePoint(&traceRecord, 2);
        if ((traceRecord.rwValidMask & (1u << 2)) != 0) {
            std::memcpy(after, traceRecord.rwStates[2], sizeof(after));
            sampled = true;
        }
    }
    if (!sampled && sample <= 3600) {
        sampled = CaptureProperShadersAlphaRenderStates(after);
    }

    if (!captured) {
        if (traceEnabled) {
            if (sampled) {
                traceRecord.flags |= 0x00000004;
            }
            StoreProperShadersStateTraceRecord(&traceRecord);
        }
        return;
    }

    const bool changed = sampled && std::memcmp(before, after, sizeof(before)) != 0;
    const bool restored = RestoreProperShadersAlphaTestRenderStates(before);
    uint32_t deviceRestoreTarget[kProperShadersAlphaRenderStateCount]{};
    for (size_t i = 0; i < kProperShadersAlphaRenderStateCount; ++i) {
        deviceRestoreTarget[i] = UINT32_MAX;
    }
    deviceRestoreTarget[0] = before[0];
    deviceRestoreTarget[3] = before[3];
    deviceRestoreTarget[4] = before[4];
    uint32_t deviceBeforeRestore[kProperShadersAlphaRenderStateCount]{};
    uint32_t deviceChangedMask = 0;
    const bool deviceRestored = restored && SynchronizeD3D9DeviceAlphaRenderStates(
        deviceRestoreTarget, deviceBeforeRestore, &deviceChangedMask);
    uint32_t stencilBeforeRestore = UINT32_MAX;
    bool stencilChanged = false;
    const bool stencilRestored = !baselineStencilValid || SynchronizeD3D9DeviceRenderState(
        52,
        baselineStencilEnable,
        &stencilBeforeRestore,
        &stencilChanged);

    if (traceEnabled) {
        traceRecord.flags |= 0x00000002;
        if (sampled) {
            traceRecord.flags |= 0x00000004;
        }
        if (restored) {
            traceRecord.flags |= 0x00000008;
        }
        if (deviceRestored) {
            traceRecord.flags |= 0x00000010;
        }
        if (stencilRestored) {
            traceRecord.flags |= 0x00000020;
        }
        traceRecord.deviceChangedMask = deviceChangedMask;
        StoreProperShadersStateTraceRecord(&traceRecord);
    }

    if (changed || deviceChangedMask != 0 || stencilChanged) {
        const LONG logIndex = InterlockedIncrement(&g_properShadersRenderStateChangeLogs);
        if (logIndex <= 16) {
            Log("proper shaders render-state guard: rwRestored={} deviceRestored={} deviceMask=0x{:03X} stencilRestored={} stencilChanged={} stencilActual={} stencilExpected={} alphaTest post={} pre={} device={} alphaBlend post={} pre={} device={} src post={} pre={} dest post={} pre={} alphaFunc post={} pre={} device={} alphaRef post={} pre={} device={} separateAlpha post={} pre={}",
                restored ? 1 : 0,
                deviceRestored ? 1 : 0,
                deviceChangedMask,
                stencilRestored ? 1 : 0,
                stencilChanged ? 1 : 0,
                stencilBeforeRestore,
                baselineStencilEnable,
                after[0], before[0],
                deviceBeforeRestore[0],
                after[5], before[5],
                deviceBeforeRestore[5],
                after[1], before[1],
                after[2], before[2],
                after[4], before[4],
                deviceBeforeRestore[4],
                after[3], before[3],
                deviceBeforeRestore[3],
                after[7], before[7]);
        }
    }
}

static bool ReadProperShadersRwAlphaState(ProperShadersRwAlphaState* state)
{
    if (!state || !IsReadableCommitted(0x00C97B24, sizeof(uintptr_t))) {
        return false;
    }

    using RwRenderStateGetFn = int(__cdecl*)(uint32_t, void*);
    __try {
        const uintptr_t rwGlobals = *reinterpret_cast<const uintptr_t*>(0x00C97B24);
        if (!rwGlobals || !IsReadableCommitted(rwGlobals + 0x24, sizeof(uintptr_t))) {
            return false;
        }
        auto getState = *reinterpret_cast<RwRenderStateGetFn const*>(rwGlobals + 0x24);
        if (!getState || !IsExecutableCommitted(reinterpret_cast<uintptr_t>(getState))) {
            return false;
        }

        getState(29, &state->alphaFunction);
        getState(30, &state->alphaReference);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool SynchronizeProperShadersFadingAlphaState()
{
    ProperShadersRwAlphaState expected{};
    if (!ReadProperShadersRwAlphaState(&expected) ||
        InterlockedCompareExchange(&g_properShadersRenderStateGuardApiReady, 0, 0) == 0) {
        return false;
    }

    uint32_t desired[kProperShadersAlphaRenderStateCount]{};
    for (size_t i = 0; i < kProperShadersAlphaRenderStateCount; ++i) {
        desired[i] = UINT32_MAX;
    }

    const uint32_t expectedAlphaTest =
        expected.alphaFunction != 0 && expected.alphaFunction != 8 ? 1u : 0u;
    desired[0] = expectedAlphaTest;
    desired[3] = expected.alphaReference;
    desired[4] = expected.alphaFunction;

    if (!RestoreProperShadersAlphaTestRenderStates(desired)) {
        return false;
    }

    uint32_t actual[kProperShadersAlphaRenderStateCount]{};
    uint32_t deviceChangedMask = 0;
    if (!SynchronizeD3D9DeviceAlphaRenderStates(desired, actual, &deviceChangedMask)) {
        return false;
    }

    if (deviceChangedMask != 0) {
        const LONG logIndex = InterlockedIncrement(&g_properShadersFadingStateChangeLogs);
        if (logIndex <= 16) {
            Log("proper shaders fading-state guard: alpha-test-only deviceMask=0x{:03X} alphaTest actual={} expected={} alphaFunc actual={} expected={} alphaRef actual={} expected={}",
                deviceChangedMask,
                actual[0], desired[0],
                actual[4], desired[4],
                actual[3], desired[3]);
        }
    }
    return true;
}

extern "C" void __cdecl Bridge_ProperShadersRenderFadingEntitiesGuard()
{
    const bool traceEnabled = g_properShadersRenderStateGuardConfig.enableStateRing;
    ProperShadersStateTraceRecord traceRecord{};
    if (traceEnabled) {
        InitializeProperShadersStateTraceRecord(
            &traceRecord, kProperShadersStateTraceFadingEntities);
        CaptureProperShadersStateTracePoint(&traceRecord, 0);
    }

    const bool synchronized = SynchronizeProperShadersFadingAlphaState();
    if (traceEnabled) {
        CaptureProperShadersStateTracePoint(&traceRecord, 1);
        if (synchronized) {
            traceRecord.flags |= 0x00000001;
        }
    }

    const uintptr_t trampoline = g_properShadersRenderFadingEntitiesTrampoline;
    if (trampoline) {
        reinterpret_cast<void(__cdecl*)()>(trampoline)();
    }

    if (traceEnabled) {
        CaptureProperShadersStateTracePoint(&traceRecord, 2);
        StoreProperShadersStateTraceRecord(&traceRecord);
    }
}

// ---- patch-site installation ----

static void InstallProperShadersRenderStateGuard(HMODULE psAsi, uint32_t psTextHash)
{
#if defined(_M_IX86)
    if (!g_properShadersRenderStateGuardConfig.enable || !psAsi ||
        !IsSupportedProperShadersTextHash(psTextHash)) {
        return;
    }

    const uintptr_t patchAddress = reinterpret_cast<uintptr_t>(psAsi) + 0x1ED80;
    const uintptr_t guardTarget = reinterpret_cast<uintptr_t>(Bridge_ProperShadersPatchedRenderSceneGuard);
    const uintptr_t currentTarget = DecodeRel32JumpTarget(patchAddress);
    if (currentTarget == guardTarget) {
        InterlockedExchange(&g_properShadersRenderStateGuardInstallState, 2);
        return;
    }

    if (InterlockedCompareExchange(&g_properShadersRenderStateGuardInstallState, 1, 0) != 0) {
        return;
    }

    static const uint8_t expected[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
    uint8_t current[sizeof(expected)]{};
    bool readable = IsReadableCommitted(patchAddress, sizeof(current));
    if (readable) {
        __try {
            std::memcpy(current, reinterpret_cast<const void*>(patchAddress), sizeof(current));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            readable = false;
        }
    }

    if (!readable || std::memcmp(current, expected, sizeof(expected)) != 0) {
        Log("proper shaders render-state guard: signature mismatch PS+0x1ED80 got={:02X} {:02X} {:02X} {:02X} {:02X} textHash=0x{:08X}",
            current[0], current[1], current[2], current[3], current[4], psTextHash);
        InterlockedExchange(&g_properShadersRenderStateGuardInstallState, 0);
        return;
    }

    if (!IsExecutableCommitted(kRwD3D9SetRenderState) || !IsExecutableCommitted(kRwD3D9GetRenderState)) {
        Log("proper shaders render-state guard: GTA D3D9 state API unavailable set=0x{:08X} get=0x{:08X}",
            static_cast<uint32_t>(kRwD3D9SetRenderState), static_cast<uint32_t>(kRwD3D9GetRenderState));
        InterlockedExchange(&g_properShadersRenderStateGuardInstallState, 0);
        return;
    }

    const uintptr_t trampoline = CreateRel32Trampoline(patchAddress, sizeof(expected));
    if (!trampoline) {
        Log("proper shaders render-state guard: trampoline creation failed PS+0x1ED80");
        InterlockedExchange(&g_properShadersRenderStateGuardInstallState, 0);
        return;
    }

    g_properShadersPatchedRenderSceneTrampoline = trampoline;
    InterlockedExchange(&g_properShadersRenderStateGuardApiReady, 1);
    if (!WriteRel32Jump(patchAddress, guardTarget)) {
        InterlockedExchange(&g_properShadersRenderStateGuardApiReady, 0);
        g_properShadersPatchedRenderSceneTrampoline = 0;
        VirtualFree(reinterpret_cast<void*>(trampoline), 0, MEM_RELEASE);
        InterlockedExchange(&g_properShadersRenderStateGuardInstallState, 0);
        return;
    }

    InterlockedExchange(&g_properShadersRenderStateGuardInstallState, 2);
    Log("proper shaders render-state guard: installed PS+0x1ED80 trampoline=0x{:08X} guard=0x{:08X} states={} textHash=0x{:08X}",
        trampoline,
        guardTarget,
        static_cast<unsigned>(kProperShadersAlphaRenderStateCount),
        psTextHash);
#else
    (void)psAsi;
    (void)psTextHash;
#endif
}

static void InstallProperShadersFadingStateGuard(HMODULE psAsi, uint32_t psTextHash)
{
#if defined(_M_IX86)
    if (!g_properShadersRenderStateGuardConfig.enable || !psAsi ||
        !IsSupportedProperShadersTextHash(psTextHash)) {
        return;
    }

    const uintptr_t psBase = reinterpret_cast<uintptr_t>(psAsi);
    const uintptr_t patchAddress = psBase + 0x1ED60;
    const uintptr_t guardTarget = reinterpret_cast<uintptr_t>(Bridge_ProperShadersRenderFadingEntitiesGuard);
    if (DecodeRel32JumpTarget(patchAddress) == guardTarget) {
        InterlockedExchange(&g_properShadersFadingStateGuardInstallState, 2);
        return;
    }

    if (InterlockedCompareExchange(&g_properShadersFadingStateGuardInstallState, 1, 0) != 0) {
        return;
    }

    uint8_t current[5]{};
    bool readable = IsReadableCommitted(patchAddress, sizeof(current));
    if (readable) {
        __try {
            std::memcpy(current, reinterpret_cast<const void*>(patchAddress), sizeof(current));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            readable = false;
        }
    }

    uint32_t pushedString = 0;
    if (readable) {
        std::memcpy(&pushedString, current + 1, sizeof(pushedString));
    }
    const uintptr_t expectedString = psBase + 0x12D640;
    if (!readable || current[0] != 0x68 || pushedString != expectedString) {
        Log("proper shaders fading-state guard: signature mismatch PS+0x1ED60 got={:02X} {:02X} {:02X} {:02X} {:02X} pushed=0x{:08X} expected=0x{:08X} textHash=0x{:08X}",
            current[0], current[1], current[2], current[3], current[4],
            pushedString, expectedString, psTextHash);
        InterlockedExchange(&g_properShadersFadingStateGuardInstallState, 0);
        return;
    }

    if (!IsExecutableCommitted(kRwD3D9SetRenderState) || !IsExecutableCommitted(kRwD3D9GetRenderState)) {
        Log("proper shaders fading-state guard: GTA D3D9 state API unavailable");
        InterlockedExchange(&g_properShadersFadingStateGuardInstallState, 0);
        return;
    }

    const uintptr_t trampoline = CreateRel32Trampoline(patchAddress, sizeof(current));
    if (!trampoline) {
        Log("proper shaders fading-state guard: trampoline creation failed PS+0x1ED60");
        InterlockedExchange(&g_properShadersFadingStateGuardInstallState, 0);
        return;
    }

    g_properShadersRenderFadingEntitiesTrampoline = trampoline;
    InterlockedExchange(&g_properShadersRenderStateGuardApiReady, 1);
    if (!WriteRel32Jump(patchAddress, guardTarget)) {
        g_properShadersRenderFadingEntitiesTrampoline = 0;
        VirtualFree(reinterpret_cast<void*>(trampoline), 0, MEM_RELEASE);
        InterlockedExchange(&g_properShadersFadingStateGuardInstallState, 0);
        return;
    }

    InterlockedExchange(&g_properShadersFadingStateGuardInstallState, 2);
    Log("proper shaders fading-state guard: installed PS+0x1ED60 trampoline=0x{:08X} guard=0x{:08X} textHash=0x{:08X}",
        trampoline, guardTarget, psTextHash);
#else
    (void)psAsi;
    (void)psTextHash;
#endif
}

static void InstallProperShadersMainSceneStateGuard(HMODULE psAsi, uint32_t psTextHash)
{
#if defined(_M_IX86)
    if (!g_properShadersRenderStateGuardConfig.enable || !psAsi ||
        !IsSupportedProperShadersTextHash(psTextHash)) {
        return;
    }

    const uintptr_t psBase = reinterpret_cast<uintptr_t>(psAsi);
    // FUN_1001EAA0 clears phaseAlternate at +0x1EAA6. Hook the next
    // complete instruction so state restoration runs after that clear and
    // before the first main-scene render call.
    const uintptr_t patchAddress = psBase + 0x1EAB0;
    const uintptr_t guardTarget =
        reinterpret_cast<uintptr_t>(Bridge_ProperShadersMainSceneStateGuard);
    if (DecodeRel32JumpTarget(patchAddress) == guardTarget) {
        InterlockedExchange(&g_properShadersMainSceneStateGuardInstallState, 2);
        return;
    }

    if (InterlockedCompareExchange(
            &g_properShadersMainSceneStateGuardInstallState, 1, 0) != 0) {
        return;
    }

    static const uint8_t expected[] = {
        0x83, 0x3D, 0x24, 0xC7, 0xC7, 0x00, 0x00,
    };
    uint8_t current[sizeof(expected)]{};
    bool readable = IsReadableCommitted(patchAddress, sizeof(current));
    if (readable) {
        __try {
            std::memcpy(current, reinterpret_cast<const void*>(patchAddress), sizeof(current));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            readable = false;
        }
    }
    if (!readable || std::memcmp(current, expected, sizeof(expected)) != 0) {
        Log("proper shaders main-scene guard: signature mismatch PS+0x1EAB0 got={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} textHash=0x{:08X}",
            current[0], current[1], current[2], current[3],
            current[4], current[5], current[6], psTextHash);
        InterlockedExchange(&g_properShadersMainSceneStateGuardInstallState, 0);
        return;
    }

    const uintptr_t trampoline = CreateRel32Trampoline(patchAddress, sizeof(expected));
    if (!trampoline) {
        Log("proper shaders main-scene guard: trampoline creation failed PS+0x1EAB0");
        InterlockedExchange(&g_properShadersMainSceneStateGuardInstallState, 0);
        return;
    }

    g_properShadersMainSceneStateTrampoline = trampoline;
    if (!WriteRel32Jump(patchAddress, guardTarget)) {
        g_properShadersMainSceneStateTrampoline = 0;
        VirtualFree(reinterpret_cast<void*>(trampoline), 0, MEM_RELEASE);
        InterlockedExchange(&g_properShadersMainSceneStateGuardInstallState, 0);
        return;
    }

    InterlockedExchange(&g_properShadersMainSceneStateGuardInstallState, 2);
    Log("proper shaders main-scene guard: installed PS+0x1EAB0 trampoline=0x{:08X} guard=0x{:08X} textHash=0x{:08X}",
        trampoline, guardTarget, psTextHash);
#else
    (void)psAsi;
    (void)psTextHash;
#endif
}

// FLA++ ran these three installers from InstallProperShadersVtableGuard() after
// its AddTxdSlot arbitration and its fixed PS+0x50C1F patch; both of those stay
// on the FLA++ side, so the d3d9 layer installs only the render-state guards.
static void InstallProperShadersRenderStateGuards(HMODULE psAsi)
{
#if defined(_M_IX86)
    if (!g_properShadersRenderStateGuardConfig.enable || !psAsi) {
        return;
    }

    const uint32_t psTextHash = CalculateModuleFileTextHash(psAsi);
    if (!IsSupportedProperShadersTextHash(psTextHash)) {
        static LONG hashMismatchLogs = 0;
        if (InterlockedIncrement(&hashMismatchLogs) <= 2) {
            Log("proper shaders render-state guard: unsupported file text hash=0x{:08X} expected=0x{:08X}; skipped",
                psTextHash, kSupportedProperShadersTextHash);
        }
        return;
    }

    InstallProperShadersRenderStateGuard(psAsi, psTextHash);
    InstallProperShadersFadingStateGuard(psAsi, psTextHash);
    InstallProperShadersMainSceneStateGuard(psAsi, psTextHash);
#else
    (void)psAsi;
#endif
}

// ---- activation ----

static DWORD WINAPI ProperShadersRenderStateGuardWatchThread(void*)
{
    bool touchedOnce = false;
    bool delayedAttemptDone = false;
    const ULONGLONG startTick = GetTickCount64();

    for (int attempt = 0; attempt < 400; ++attempt) {
        HMODULE psAsi = GetModuleHandleA("ProperShaders.asi");
        if (!psAsi) {
            psAsi = GetModuleHandleA("propershaders.asi");
        }
        if (psAsi) {
            // FLA++ installed once from its polling thread and once more from a
            // delayed direct call; the installers are idempotent, so the second
            // attempt is only a safety net for a partially failed first pass.
            const bool delayedAttempt =
                !delayedAttemptDone && (GetTickCount64() - startTick) >= 1500;
            if (!touchedOnce || delayedAttempt) {
                InstallProperShadersRenderStateGuards(psAsi);
                touchedOnce = true;
                if (delayedAttempt) {
                    delayedAttemptDone = true;
                }
            }
        }

        Sleep(25);
    }

    return 0;
}

void StartProperShadersRenderStateGuardWatch()
{
    if (!g_properShadersRenderStateGuardConfig.enable) {
        return;
    }

    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) {
        return;
    }

    HANDLE thread = CreateThread(
        nullptr, 0, ProperShadersRenderStateGuardWatchThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        Log("propershaders guard: watch thread started");
    } else {
        Log("propershaders guard: watch thread CreateThread failed err={}", GetLastError());
    }
}
