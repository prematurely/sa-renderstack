#include <windows.h>
#include <atomic>

namespace {
thread_local bool g_pauseNextExclusive = false;
HANDLE g_admissionAtLock = nullptr;
HANDLE g_continueAdmission = nullptr;

// Keep the actual SRW lock, and provide one deterministic scheduling point
// between the optimistic active read and the locked admission recheck. This
// interposition exists only in the test translation unit.
void AcquireJournalTestLock(PSRWLOCK lock)
{
    if (g_pauseNextExclusive) {
        g_pauseNextExclusive = false;
        SetEvent(g_admissionAtLock);
        WaitForSingleObject(g_continueAdmission, INFINITE);
    }
    AcquireSRWLockExclusive(lock);
}
}

#define AcquireSRWLockExclusive AcquireJournalTestLock
#include "../src/bridge/legacy/EffectInspector.cpp"
#undef AcquireSRWLockExclusive

#include <chrono>
#include <future>
#include <thread>

namespace {
unsigned g_checks = 0;
unsigned g_failures = 0;

void Check(bool condition, const char* message)
{
    ++g_checks;
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++g_failures;
}

void TestInactiveAdmission()
{
    // A disabled transaction must return while another thread owns the stats
    // lock. Removing the inactive fast check makes this observable test fail.
    JournalAttributionStopCapture();
    AcquireSRWLockExclusive(&g_journalAttributionLock);
    std::promise<bool> result;
    auto ready = result.get_future();
    std::thread worker([&] { result.set_value(JournalAttributionTryBeginTransaction()); });
    const bool returnedWithoutLock = ready.wait_for(std::chrono::milliseconds(150)) ==
        std::future_status::ready;
    ReleaseSRWLockExclusive(&g_journalAttributionLock);
    worker.join();
    Check(returnedWithoutLock && !ready.get(),
        "inactive admission returns false without acquiring the stats lock");
}

#if defined(JOURNAL_ATTRIBUTION_SUMMARY_VERSION)
void Observe(const char* name, std::uint64_t sequence, std::uint64_t span,
    std::uint64_t restore, std::span<const JournalProbeRecord> records = {},
    bool native = false, bool truncated = false)
{
    if (!JournalAttributionTryBeginTransaction()) {
        Check(false, "active transaction admitted");
        return;
    }
    JournalProbeTransactionInfo info{sequence, JournalAttributionCurrentFrame(), span, native};
    JournalAttributionObserveTransaction(name, records, truncated, info);
    JournalAttributionObserveRestore(sequence, restore);
    JournalAttributionEndTransaction();
}

void TestSummary()
{
    JournalAttributionSummary summary{};
    JournalAttributionReset();
    Check(JournalAttributionGetSummary(&summary) && !summary.captureStarted &&
        !summary.available && !summary.active,
        "no capture is explicitly unavailable");
    Check(!JournalAttributionGetSummary(nullptr), "null summary output is rejected");
    JournalAttributionSetCaptureIdentity(7654321);
    JournalAttributionStartCapture();
    JournalAttributionSetCaptureIdentity(99);
    JournalAttributionGetSummary(&summary);
    Check(summary.captureId == 7654321 && summary.captureStarted && summary.active &&
        !summary.available, "capture identity stays stable and empty capture stays unavailable");
    const JournalProbeRecord records[] = {
        {5, 22, 0, 0xFFFFFFFFu}, {5, 22, 0, 3}, {5, 22, 0, 3}, {6, 0, 0, 4}
    };
    Observe("Alpha", 1, 100, 10, records, true);
    Observe("Beta", 2, 40, 5);
    JournalAttributionOnPresent();
    Observe("Alpha", 3, 20, 4, std::span(records, 2), true, true);
    JournalAttributionOnPresent();
    JournalAttributionStopCapture();
    JournalAttributionOnPresent();
    JournalAttributionGetSummary(&summary);
    Check(summary.available && !summary.active && summary.inFlight == 0 &&
        summary.captureId == 7654321 && summary.frames == 2 && summary.presents == 2,
        "completed summary preserves capture and successful Present counts");
    Check(summary.transactions == 3 && summary.records == 6 &&
        summary.nativeTransactions == 2 && summary.localTransactions == 1 &&
        summary.emptyTransactions == 1 && summary.truncatedTransactions == 1 &&
        summary.techniqueCount == 2 && summary.passCount == 4,
        "summary counts technique passes and admitted transaction records");
    Check(summary.transactionSpanQpc == 160 && summary.maxTransactionSpanQpc == 100 &&
        summary.restoreQpc == 19 && summary.maxRestoreQpc == 10 && summary.restoreCount == 3,
        "inclusive transaction spans and restore timing remain separate");
    Check(summary.frameStatCount == 2 && summary.frameStats[0].frame == 0 &&
        summary.frameStats[0].transactions == 2 && summary.frameStats[0].records == 4 &&
        summary.frameStats[0].transactionSpanQpc == 140 && summary.frameStats[0].restoreQpc == 15 &&
        summary.frameStats[1].frame == 1 && summary.frameStats[1].transactions == 1 &&
        summary.frameStats[1].records == 2 && summary.frameStats[1].transactionSpanQpc == 20 &&
        summary.frameStats[1].restoreQpc == 4,
        "transaction start frame receives records and distinct restore timings");
    Check(summary.topTechniqueCount == 2 &&
        std::strcmp(summary.topTechniques[0].name, "Alpha") == 0 &&
        summary.topTechniques[0].transactionSpanQpc == 120 &&
        summary.topTechniques[0].restoreQpc == 14 && summary.topTechniques[0].passCount == 3,
        "top named techniques are ranked by measured inclusive transaction span");
    JournalAttributionDump();
}

void TestStopBoundary()
{
    JournalAttributionStartCapture();
    Check(JournalAttributionTryBeginTransaction(), "boundary transaction admitted");
    std::promise<void> stopped;
    auto ready = stopped.get_future();
    std::thread stopper([&] { JournalAttributionStopCapture(); stopped.set_value(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_properShadersStateAttributionActive.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Check(!g_properShadersStateAttributionActive.load(std::memory_order_acquire) &&
        !JournalAttributionTryBeginTransaction() &&
        ready.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout,
        "StopCapture closes admission and waits for an admitted transaction");
    JournalAttributionObserveTransaction("CrossBoundary", {}, false, {77, 0, 50, false});
    JournalAttributionObserveRestore(77, 7);
    JournalAttributionEndTransaction();
    stopper.join();
    JournalAttributionSummary summary{};
    JournalAttributionGetSummary(&summary);
    Check(summary.transactions == 1 && summary.restoreCount == 1 &&
        summary.transactionSpanQpc == 50 && summary.restoreQpc == 7 && summary.inFlight == 0,
        "in-flight transaction and restore publish before StopCapture returns");
}

void TestReportWriter()
{
#if defined(JOURNAL_ATTRIBUTION_REPORT_WRITER_VERSION)
    Check(!JournalAttributionWriteReport(nullptr), "report writer rejects null destination");
    FILE* output = nullptr;
    fopen_s(&output, "journal-writer-test.log", "w+b");
    Check(output != nullptr, "report writer fixture opens a real output file");
    if (!output) return;
    const bool written = JournalAttributionWriteReport(output);
    std::rewind(output);
    char text[16384]{};
    std::fread(text, 1, sizeof(text) - 1, output);
    Check(written && std::strstr(text, "captureId=7654321") &&
        std::strstr(text, "technique=Alpha") && std::strstr(text, "technique=Beta") &&
        std::strstr(text, "pass=pass index=4 transactions=1 records=1") &&
        std::strstr(text, "op=TEX count=1"),
        "direct writer retains full technique, pass and operation attribution in one destination");
    std::fclose(output);
    FILE* readOnly = nullptr;
    fopen_s(&readOnly, "journal-writer-test.log", "rb");
    Check(readOnly && !JournalAttributionWriteReport(readOnly),
        "actual FILE output failure is reported without leaking the stats lock");
    if (readOnly) std::fclose(readOnly);
    JournalAttributionStartCapture();
    JournalAttributionStopCapture();
    fopen_s(&output, "journal-writer-empty.log", "w+b");
    if (!output) { Check(false, "empty report output opens"); return; }
    const bool emptyWritten = JournalAttributionWriteReport(output);
    std::rewind(output);
    std::memset(text, 0, sizeof(text));
    std::fread(text, 1, sizeof(text) - 1, output);
    Check(emptyWritten && std::strstr(text, "available=false") &&
        std::strstr(text, "frames=0 presents=0 techniques=0"),
        "direct writer preserves an empty window as explicitly unavailable");
    std::fclose(output);
#else
    Check(false, "full journal report writer is available to the combined producer");
#endif
}

void TestLockedAdmissionRecheck()
{
    JournalAttributionStartCapture();
    g_admissionAtLock = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_continueAdmission = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    bool admitted = false;
    std::thread contender([&] {
        g_pauseNextExclusive = true;
        admitted = JournalAttributionTryBeginTransaction();
        if (admitted) JournalAttributionEndTransaction();
    });
    const bool reachedLock = WaitForSingleObject(g_admissionAtLock, 2000) == WAIT_OBJECT_0;
    JournalAttributionStopCapture();
    SetEvent(g_continueAdmission);
    contender.join();
    Check(reachedLock && !admitted,
        "locked admission recheck rejects a stale active read after StopCapture");
    CloseHandle(g_admissionAtLock);
    CloseHandle(g_continueAdmission);
    g_admissionAtLock = nullptr;
    g_continueAdmission = nullptr;
}

void TestCloseAdmission()
{
#if defined(JOURNAL_ATTRIBUTION_CLOSE_ADMISSION_VERSION)
    JournalAttributionStartCapture();
    Check(JournalAttributionTryBeginTransaction(), "nonblocking-close transaction admitted");
    std::promise<void> closed;
    auto ready = closed.get_future();
    std::thread closer([&] { JournalAttributionCloseAdmission(); closed.set_value(); });
    const bool returned = ready.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready;
    Check(returned && !g_properShadersStateAttributionActive.load(std::memory_order_acquire),
        "CloseAdmission returns while a transaction remains admitted");
    if (returned) {
        JournalAttributionSummary pending{};
        JournalAttributionGetSummary(&pending);
        Check(pending.inFlight == 1 && !pending.active && !JournalAttributionTryBeginTransaction(),
            "closed admission rejects new work and preserves the admitted transaction");
    }
    JournalAttributionObserveTransaction("AfterClose", {}, false, {501, 0, 50, false});
    JournalAttributionObserveRestore(501, 5);
    JournalAttributionEndTransaction();
    closer.join();
    JournalAttributionStopCapture();
    JournalAttributionSummary completed{};
    JournalAttributionGetSummary(&completed);
    Check(completed.transactions == 1 && completed.restoreCount == 1 && completed.inFlight == 0,
        "transaction completion after CloseAdmission remains visible to synchronous drain");
#else
    Check(false, "nonblocking journal admission close is available");
#endif
}

void TestBounds()
{
    JournalAttributionStartCapture();
    for (unsigned i = 0; i < 1100; ++i) {
        Observe("Bounded", i + 1, 2, 1);
        JournalAttributionOnPresent();
    }
    JournalAttributionStopCapture();
    JournalAttributionSummary summary{};
    JournalAttributionGetSummary(&summary);
    Check(summary.frames == 1100 && summary.transactions == 1100 &&
        summary.frameStatCount == 1024 && summary.droppedFrames == 76 &&
        summary.transactionSpanQpc == 2200 && summary.restoreQpc == 1100,
        "bounded frame table reports unavailable frames without dropping capture totals");
}

void TestDroppedDetails()
{
    JournalAttributionStartCapture();
    char name[32]{};
    for (unsigned i = 0; i < 100; ++i) {
        std::snprintf(name, sizeof(name), "Technique%u", i);
        Observe(name, i + 1, i + 1, 1);
    }
    JournalAttributionStopCapture();
    JournalAttributionSummary summary{};
    JournalAttributionGetSummary(&summary);
    Check(summary.techniqueCount == 96 && summary.droppedTechniques == 4 &&
        summary.transactions == 100 && summary.records == 0 &&
        summary.transactionSpanQpc == 5050 && summary.restoreCount == 100 &&
        summary.restoreQpc == 100 && summary.orphanRestores == 0 &&
        summary.topTechniqueCount == 8 &&
        std::strcmp(summary.topTechniques[0].name, "Technique95") == 0,
        "technique overflow preserves aggregate and restore totals and bounds the top list");
    JournalAttributionStartCapture();
    JournalProbeRecord passes[40]{};
    for (unsigned i = 0; i < 40; ++i) passes[i] = {5, 22, 0, i};
    Observe("PassOverflow", 200, 10, 2, passes);
    JournalAttributionStopCapture();
    JournalAttributionGetSummary(&summary);
    Check(summary.passCount == 32 && summary.droppedPasses == 8 &&
        summary.records == 40 && summary.frameStats[0].records == 40,
        "pass overflow is explicit without erasing observed records");
}

LRESULT CALLBACK JournalTestWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

void TestRealDevice(const char* backendPath)
{
    const bool native = backendPath != nullptr;
    std::string path;
    if (backendPath) {
        path = backendPath;
        SetEnvironmentVariableA("DXVK_CONFIG", "d3d9.gtaSaCompat = True; d3d9.gtaSaCompatDiagnostics = True");
    } else {
        char systemDirectory[MAX_PATH]{};
        GetSystemDirectoryA(systemDirectory, MAX_PATH);
        path = std::string(systemDirectory) + "\\d3d9.dll";
    }
    const HMODULE module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Check(module != nullptr, "real D3D9 library loads");
    if (!module) return;
    using Create = IDirect3D9* (WINAPI*)(UINT);
    const auto create = reinterpret_cast<Create>(GetProcAddress(module, "Direct3DCreate9"));
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    Check(d3d != nullptr, "real D3D9 factory creates");
    if (!d3d) { FreeLibrary(module); return; }
    WNDCLASSA wc{};
    wc.lpfnWndProc = JournalTestWindowProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "JournalAttributionRealDeviceTest";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "journal test", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferWidth = 64;
    parameters.BackBufferHeight = 64;
    IDirect3DDevice9* device = nullptr;
    const HRESULT createResult = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &parameters, &device);
    Check(SUCCEEDED(createResult) && device, "real D3D9 HAL device creates");
    if (device) {
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        auto* journal = new ProperShadersStateJournal(device);
        Check(journal->SupportsNativeStateJournal() == native,
            "device journal uses the requested native or local restore path");
        JournalAttributionSetCaptureIdentity(9876);
        JournalAttributionStartCapture();
        HRESULT result = journal->BeginTransaction(GetCurrentThreadId(), 0);
        journal->SetProbeTechnique("RealDuplicateChanged");
        journal->SetCurrentPass(2);
        if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        const HRESULT restored = journal->Restore();
        DWORD cull = 0;
        device->GetRenderState(D3DRS_CULLMODE, &cull);
        Check(SUCCEEDED(result) && SUCCEEDED(restored) && cull == D3DCULL_CCW,
            "real duplicate and changed journal writes restore original state");
        if (SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr))) JournalAttributionOnPresent();
        result = journal->BeginTransaction(GetCurrentThreadId(), 0);
        journal->SetProbeTechnique("RealEmpty");
        Check(SUCCEEDED(result) && SUCCEEDED(journal->Restore()),
            "real empty transaction restores and publishes");
        if (SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr))) JournalAttributionOnPresent();
        result = journal->BeginTransaction(GetCurrentThreadId(), 0);
        journal->SetProbeTechnique("RealStopBoundary");
        journal->SetCurrentPass(3);
        if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        std::promise<void> stopped;
        auto ready = stopped.get_future();
        std::thread stopper([&] { JournalAttributionStopCapture(); stopped.set_value(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_properShadersStateAttributionActive.load(std::memory_order_acquire) &&
            std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        Check(ready.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout,
            "real journal remains admitted across StopCapture boundary");
        const HRESULT boundaryRestore = journal->Restore();
        stopper.join();
        device->GetRenderState(D3DRS_CULLMODE, &cull);
        JournalAttributionSummary summary{};
        JournalAttributionGetSummary(&summary);
        Check(SUCCEEDED(result) && SUCCEEDED(boundaryRestore) && cull == D3DCULL_CCW &&
            summary.captureId == 9876 && summary.transactions == 3 && summary.records == 4 &&
            summary.restoreCount == 3 && summary.emptyTransactions == 1 && summary.inFlight == 0 &&
            summary.frames == 2 && summary.frameStatCount == 3 &&
            summary.frameStats[0].transactions == 1 && summary.frameStats[0].records == 3 &&
            summary.frameStats[1].transactions == 1 && summary.frameStats[1].records == 0 &&
            summary.frameStats[2].transactions == 1 && summary.frameStats[2].records == 1 &&
            summary.transactionSpanQpc > 0 && summary.restoreQpc > 0 &&
            summary.nativeTransactions == (native ? 3u : 0u),
            "real journal publishes precise counts, partial admission frame and separate QPC totals");
        JournalAttributionDump();
        FILE* report = nullptr;
        fopen_s(&report, "journal-writer-device.log", "w+b");
        bool reportWritten = report && JournalAttributionWriteReport(report);
        char reportText[16384]{};
        if (report) {
            std::rewind(report);
            std::fread(reportText, 1, sizeof(reportText) - 1, report);
            std::fclose(report);
        }
        Check(reportWritten && std::strstr(reportText, "captureId=9876") &&
            std::strstr(reportText, "technique=RealDuplicateChanged transactions=1 records=3") &&
            std::strstr(reportText, "technique=RealStopBoundary"),
            "direct writer emits complete actual native/local journal technique rows");
        journal->Release();
        device->Release();
    }
    d3d->Release();
    FreeLibrary(module);
    if (window) DestroyWindow(window);
}
#endif
}

int main(int argc, char** argv)
{
#if defined(JOURNAL_ATTRIBUTION_SUMMARY_VERSION)
    if (argc > 1 && std::strcmp(argv[1], "--device") == 0) {
        TestRealDevice(argc > 2 ? argv[2] : nullptr);
    } else {
#endif
    TestInactiveAdmission();
#if defined(JOURNAL_ATTRIBUTION_SUMMARY_VERSION)
    TestSummary();
    TestReportWriter();
    TestStopBoundary();
    TestLockedAdmissionRecheck();
    TestCloseAdmission();
    TestBounds();
    TestDroppedDetails();
#else
    Check(false, "bounded capture summary and per-frame attribution are available");
#endif
#if defined(JOURNAL_ATTRIBUTION_SUMMARY_VERSION)
    }
#endif
    std::printf("checks=%u failures=%u\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
