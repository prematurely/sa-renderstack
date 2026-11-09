// ProperShaders state ring implementation.
//
// Ported from FLA++ (FLACompatBridge/BridgeCompatProperShaders.cpp): a lazily
// allocated, fixed-size ring of ProperShadersStateTraceRecord entries written
// under an exclusive SRWLOCK by the render thread, dumped on a hotkey edge to
// scripts\BridgeD3D9.propershaders-state-ring.log. Readers validate the record
// sequence so a torn slot is skipped instead of being printed.

#include "ProperShadersStateRing.h"

#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <atomic>

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

const char* g_properShadersStateTracePath = "scripts\\BridgeD3D9.propershaders-state-ring.log";

SRWLOCK g_properShadersStateTraceLock = SRWLOCK_INIT;
ProperShadersStateTraceRecord* g_properShadersStateTraceRing = nullptr;
uint32_t g_properShadersStateTraceSequence = 0;
LONG g_properShadersStateTraceCaptureId = 0;
LONG g_properShadersStateTraceTriggerDown = 0;
std::atomic<bool> g_properShadersStateTraceActive{false};
std::atomic<uint32_t> g_properShadersStateTraceGeneration{0};
std::atomic<ULONGLONG> g_properShadersStateTraceDeadline{0};
constexpr ULONGLONG kManualCaptureMs = 1000;
} // namespace

bool IsProperShadersStateTraceCaptureActive() noexcept
{
    return g_properShadersRenderStateGuardConfig.enableStateRing &&
        g_properShadersStateTraceActive.load(std::memory_order_acquire) &&
        GetTickCount64() < g_properShadersStateTraceDeadline.load(std::memory_order_relaxed);
}

bool BeginProperShadersStateTraceCapture()
{
    if (!g_properShadersRenderStateGuardConfig.enableStateRing) return false;
    AcquireSRWLockExclusive(&g_properShadersStateTraceLock);
    if (g_properShadersStateTraceActive.load(std::memory_order_acquire)) {
        ReleaseSRWLockExclusive(&g_properShadersStateTraceLock);
        return false;
    }
    g_properShadersStateTraceGeneration.fetch_add(1, std::memory_order_relaxed);
    g_properShadersStateTraceSequence = 0;
    g_properShadersStateTraceDeadline.store(GetTickCount64() + kManualCaptureMs, std::memory_order_relaxed);
    g_properShadersStateTraceActive.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_properShadersStateTraceLock);
    Log("proper shaders state ring: on-demand capture started durationMs={}", kManualCaptureMs);
    return true;
}

static const char* ProperShadersStateTraceKindName(uint32_t kind)
{
    switch (kind) {
    case kProperShadersStateTraceRenderScene:
        return "RenderScene";
    case kProperShadersStateTraceFadingEntities:
        return "FadingEntities";
    default:
        return "Unknown";
    }
}

static void WriteProperShadersD3D9StateSnapshot(
    FILE* file,
    uint32_t index,
    const ProperShadersD3D9StateSnapshot& snapshot)
{
    if (!file) {
        return;
    }

    std::fprintf(file,
        " snapshot=%u valid=0x%08X device=%p tex0=%p vs=%p ps=%p rt0=%p ds=%p "
        "viewport=%u,%u,%u,%u,%.6f,%.6f phaseAlt=%u phase9=%u phase10=%u\n",
        index,
        snapshot.validMask,
        snapshot.device,
        snapshot.texture0,
        snapshot.vertexShader,
        snapshot.pixelShader,
        snapshot.renderTarget0,
        snapshot.depthStencil,
        snapshot.viewport.x,
        snapshot.viewport.y,
        snapshot.viewport.width,
        snapshot.viewport.height,
        snapshot.viewport.minZ,
        snapshot.viewport.maxZ,
        snapshot.phaseAlternate,
        snapshot.phaseByte9,
        snapshot.phaseByte10);

    std::fprintf(file, "  rs");
    for (size_t i = 0; i < kProperShadersTraceRenderStateCount; ++i) {
        std::fprintf(file, " %s=%u(0x%08X)",
            kProperShadersTraceRenderStates[i].name,
            snapshot.renderStates[i],
            snapshot.renderStates[i]);
    }
    std::fputc('\n', file);

    std::fprintf(file, "  tss0");
    for (size_t i = 0; i < kProperShadersTraceTextureStageStateCount; ++i) {
        std::fprintf(file, " %s=%u(0x%08X)",
            kProperShadersTraceTextureStageStates[i].name,
            snapshot.textureStageStates[i],
            snapshot.textureStageStates[i]);
    }
    std::fputc('\n', file);

    std::fprintf(file, "  sampler0");
    for (size_t i = 0; i < kProperShadersTraceSamplerStateCount; ++i) {
        std::fprintf(file, " %s=%u(0x%08X)",
            kProperShadersTraceSamplerStates[i].name,
            snapshot.samplerStates[i],
            snapshot.samplerStates[i]);
    }
    std::fputc('\n', file);
}

void DumpProperShadersStateTraceRing()
{
    auto* records = static_cast<ProperShadersStateTraceRecord*>(VirtualAlloc(
        nullptr,
        kProperShadersStateTraceBytes,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!records) {
        Log("proper shaders state ring: dump allocation failed bytes={} gle={}",
            static_cast<unsigned>(kProperShadersStateTraceBytes),
            GetLastError());
        return;
    }

    uint32_t copied = 0;
    uint32_t newestSequence = 0;
    AcquireSRWLockShared(&g_properShadersStateTraceLock);
    newestSequence = g_properShadersStateTraceSequence;
    const uint32_t available = g_properShadersStateTraceRing
        ? (newestSequence < kProperShadersStateTraceCapacity
            ? newestSequence
            : static_cast<uint32_t>(kProperShadersStateTraceCapacity))
        : 0;
    const uint32_t oldestSequence = available ? newestSequence - available + 1 : 0;
    for (uint32_t sequence = oldestSequence; sequence && sequence <= newestSequence; ++sequence) {
        const ProperShadersStateTraceRecord& record =
            g_properShadersStateTraceRing[(sequence - 1) % kProperShadersStateTraceCapacity];
        if (record.sequence == sequence) {
            records[copied++] = record;
        }
    }
    ReleaseSRWLockShared(&g_properShadersStateTraceLock);

    FILE* file = nullptr;
    if (fopen_s(&file, g_properShadersStateTracePath, "a") != 0 || !file) {
        Log("proper shaders state ring: failed to open path={}", g_properShadersStateTracePath);
        VirtualFree(records, 0, MEM_RELEASE);
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const LONG captureId = InterlockedIncrement(&g_properShadersStateTraceCaptureId);
    std::fprintf(file,
        "# capture=%ld begin=%04u-%02u-%02uT%02u:%02u:%02u.%03u records=%u newest=%u capacity=%u triggerVK=%d\n",
        captureId,
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        copied,
        newestSequence,
        static_cast<unsigned>(kProperShadersStateTraceCapacity),
        g_properShadersRenderStateGuardConfig.stateRingTriggerVirtualKey);

    for (uint32_t i = 0; i < copied; ++i) {
        const ProperShadersStateTraceRecord& record = records[i];
        std::fprintf(file,
            "record seq=%u kind=%s thread=%u beginTick=%llu endTick=%llu durationMs=%llu snapshots=%u rwMask=0x%X flags=0x%X deviceMask=0x%X\n",
            record.sequence,
            ProperShadersStateTraceKindName(record.kind),
            record.threadId,
            static_cast<unsigned long long>(record.beginTick),
            static_cast<unsigned long long>(record.endTick),
            static_cast<unsigned long long>(record.endTick - record.beginTick),
            record.snapshotCount,
            record.rwValidMask,
            record.flags,
            record.deviceChangedMask);
        for (uint32_t snapshotIndex = 0;
             snapshotIndex < record.snapshotCount && snapshotIndex < 3;
             ++snapshotIndex) {
            if ((record.rwValidMask & (1u << snapshotIndex)) != 0) {
                std::fprintf(file, " rw%u", snapshotIndex);
                for (size_t stateIndex = 0;
                     stateIndex < kProperShadersAlphaRenderStateCount;
                     ++stateIndex) {
                    std::fprintf(file, " %s=%u(0x%08X)",
                        kProperShadersAlphaRenderStateNames[stateIndex],
                        record.rwStates[snapshotIndex][stateIndex],
                        record.rwStates[snapshotIndex][stateIndex]);
                }
                std::fputc('\n', file);
            }
            WriteProperShadersD3D9StateSnapshot(
                file, snapshotIndex, record.snapshots[snapshotIndex]);
        }
    }
    std::fprintf(file, "# capture=%ld end records=%u\n", captureId, copied);
    std::fflush(file);
    std::fclose(file);
    VirtualFree(records, 0, MEM_RELEASE);
    Log("proper shaders state ring: capture={} dumped records={} newest={} path={}",
        captureId, copied, newestSequence, g_properShadersStateTracePath);
}

void MaybeDumpProperShadersStateTraceRing()
{
    const bool triggerDown =
        (GetAsyncKeyState(g_properShadersRenderStateGuardConfig.stateRingTriggerVirtualKey) & 0x8000) != 0;
    const bool pressed = InterlockedExchange(&g_properShadersStateTraceTriggerDown, triggerDown ? 1 : 0) == 0 && triggerDown;
    if (!g_properShadersRenderStateGuardConfig.enableStateRing) {
        g_properShadersStateTraceActive.store(false, std::memory_order_release);
        return;
    }
    const auto now = GetTickCount64();
    const bool wasActive = g_properShadersStateTraceActive.load(std::memory_order_acquire);
    if (wasActive) {
        if (now >= g_properShadersStateTraceDeadline.load(std::memory_order_relaxed)) {
            g_properShadersStateTraceActive.store(false, std::memory_order_release);
            DumpProperShadersStateTraceRing();
        }
        // A key edge during the active window is consumed, never deferred.
        return;
    }
    if (!pressed) return;
    BeginProperShadersStateTraceCapture();
}

void StoreProperShadersStateTraceRecord(ProperShadersStateTraceRecord* record)
{
    if (!record || !IsProperShadersStateTraceCaptureActive() ||
        record->captureGeneration != g_properShadersStateTraceGeneration.load(std::memory_order_relaxed)) {
        return;
    }

    record->endTick = GetTickCount64();
    DWORD allocationError = ERROR_SUCCESS;
    AcquireSRWLockExclusive(&g_properShadersStateTraceLock);
    if (!IsProperShadersStateTraceCaptureActive() ||
        record->captureGeneration != g_properShadersStateTraceGeneration.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&g_properShadersStateTraceLock);
        return;
    }
    if (!g_properShadersStateTraceRing) {
        g_properShadersStateTraceRing =
            static_cast<ProperShadersStateTraceRecord*>(VirtualAlloc(
                nullptr,
                kProperShadersStateTraceBytes,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE));
        if (!g_properShadersStateTraceRing) {
            allocationError = GetLastError();
        }
    }
    if (g_properShadersStateTraceRing) {
        record->sequence = ++g_properShadersStateTraceSequence;
        g_properShadersStateTraceRing[
            (record->sequence - 1) % kProperShadersStateTraceCapacity] = *record;
    }
    ReleaseSRWLockExclusive(&g_properShadersStateTraceLock);
    if (!record->sequence) {
        static LONG allocationFailureLogs = 0;
        if (InterlockedIncrement(&allocationFailureLogs) <= 2) {
            Log("proper shaders state ring: allocation failed bytes={} gle={}",
                static_cast<unsigned>(kProperShadersStateTraceBytes),
                allocationError);
        }
        return;
    }
}

void InitializeProperShadersStateTraceRecord(
    ProperShadersStateTraceRecord* record,
    ProperShadersStateTraceKind kind)
{
    if (!record) {
        return;
    }
    *record = ProperShadersStateTraceRecord{};
    record->captureGeneration = g_properShadersStateTraceGeneration.load(std::memory_order_relaxed);
    record->kind = static_cast<uint32_t>(kind);
    record->threadId = GetCurrentThreadId();
    record->beginTick = GetTickCount64();
}

void CaptureProperShadersStateTracePoint(
    ProperShadersStateTraceRecord* record,
    uint32_t snapshotIndex)
{
    if (!record || snapshotIndex >= 3 || !IsProperShadersStateTraceCaptureActive() ||
        record->captureGeneration != g_properShadersStateTraceGeneration.load(std::memory_order_relaxed)) {
        return;
    }

    if (CaptureProperShadersAlphaRenderStates(record->rwStates[snapshotIndex])) {
        record->rwValidMask |= 1u << snapshotIndex;
    }
    CaptureProperShadersD3D9StateSnapshot(&record->snapshots[snapshotIndex]);
    if (record->snapshotCount <= snapshotIndex) {
        record->snapshotCount = snapshotIndex + 1;
    }
}

void ShutdownProperShadersStateRing()
{
    g_properShadersStateTraceActive.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_properShadersStateTraceLock);
    g_properShadersStateTraceGeneration.fetch_add(1, std::memory_order_relaxed);
    // Require release before a held key can arm a new window after shutdown.
    InterlockedExchange(&g_properShadersStateTraceTriggerDown, 1);
    if (g_properShadersStateTraceRing) {
        VirtualFree(g_properShadersStateTraceRing, 0, MEM_RELEASE);
        g_properShadersStateTraceRing = nullptr;
        g_properShadersStateTraceSequence = 0;
    }
    ReleaseSRWLockExclusive(&g_properShadersStateTraceLock);
}
