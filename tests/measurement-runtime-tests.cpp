#include "../src/bridge/legacy/EffectInspector.cpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <map>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if __has_include("../src/bridge/legacy/BridgeMeasurementRuntime.h")
#include "../src/bridge/legacy/BridgeMeasurementRuntime.h"
#include "../src/bridge/legacy/MeasurementSymbols.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_diagnostics.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_telemetry.h"
#include "../backend/dxvk/src/util/util_thread_scheduling.h"
#define HAS_MEASUREMENT_RUNTIME 1
#endif

#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
#include "../src/bridge/legacy/MeasurementEventStorage.h"
namespace BridgeMeasurement::detail {
std::atomic<bool> g_pauseEmptyStorage{false};
HANDLE g_emptyStorageSnapshot = nullptr, g_releaseEmptyStorage = nullptr;
void MeasurementStorageEmptyHeadSnapshot(MeasurementEventStorage&) noexcept {
    if (!g_pauseEmptyStorage.exchange(false)) return;
    SetEvent(g_emptyStorageSnapshot);
    // A finite timeout keeps a broken fixture bounded; the caller asserts entry.
    WaitForSingleObject(g_releaseEmptyStorage, 10000);
}
}
#endif

// Deterministic fault injection is confined to this fixture executable. The
// allocator rejects one matching allocation on the selected calling thread.
std::atomic<DWORD> g_failAllocationThread{0};
std::atomic<std::size_t> g_failAllocationSize{0};
std::atomic<unsigned> g_failAllocationCountdown{0};
std::atomic<unsigned> g_workerAllocationCount{0};
std::atomic<std::size_t> g_workerAllocationSizes[16]{};
std::atomic<bool> g_seenMainSampleBuffer{false};
void* operator new(std::size_t size)
{
    const auto target = g_failAllocationSize.load(std::memory_order_relaxed);
    if (target && g_failAllocationThread.load(std::memory_order_relaxed) == GetCurrentThreadId()) {
        if (target == SIZE_MAX - 3) {
            const auto index = g_workerAllocationCount.fetch_add(1);
            if (index < std::size(g_workerAllocationSizes)) g_workerAllocationSizes[index] = size;
            if (!g_seenMainSampleBuffer.load() && size >= 1024 * 1024) g_seenMainSampleBuffer = true;
            else if (g_seenMainSampleBuffer.load() && g_failAllocationSize.exchange(0) == target) throw std::bad_alloc();
        } else if (target == SIZE_MAX - 2) {
            const auto index = g_workerAllocationCount.fetch_add(1);
            if (index < std::size(g_workerAllocationSizes)) g_workerAllocationSizes[index] = size;
            const auto remaining = g_failAllocationCountdown.load(std::memory_order_relaxed);
            if (remaining && g_failAllocationCountdown.fetch_sub(1) == 1 &&
                g_failAllocationSize.exchange(0) == target) throw std::bad_alloc();
        } else if ((target == SIZE_MAX || (target == SIZE_MAX - 1 && size >= 96 * 1024 && size <= 512 * 1024) ||
            (target < SIZE_MAX - 3 && size >= target && size - target <= 128)) &&
            g_failAllocationSize.exchange(0) == target) throw std::bad_alloc();
    }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace {
unsigned g_checks = 0, g_failures = 0;
void Check(bool value, const char* message)
{
    ++g_checks;
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++g_failures;
    std::fflush(stdout);
}

#if defined(HAS_MEASUREMENT_RUNTIME)
using namespace BridgeMeasurement;
std::atomic<bool> g_f7{false}, g_f8{false};
bool g_failWorker = false;
bool g_failOneWorker = false;
std::atomic<bool> g_delayNextWorker{false};
bool g_delayReportWorker = false;
bool g_deviceHasTelemetry = false;
bool g_matchHostWorkerScheduling = false;
renderstack::scheduling::Options g_hostWorkerSchedulingOptions{};
std::atomic<unsigned> g_failNextWorkerAllocationOrdinal{0};
std::atomic<bool> g_failNextWorkerCsObject{false};
HANDLE g_reportWorkerStarted = nullptr, g_releaseReportWorker = nullptr;
std::atomic<std::uint32_t> g_presentFrame{0};
std::atomic<bool> g_holdGameFrame{false};
std::atomic<bool> g_throwFrameRead{false};
std::vector<HANDLE> g_workers;
using StartAuditFn = HRESULT (STDMETHODCALLTYPE*)(ID3D9BridgeDiagnostics*, UINT64, UINT);
using StopAuditFn = HRESULT (STDMETHODCALLTYPE*)(ID3D9BridgeDiagnostics*, UINT64);
StartAuditFn g_realStartAudit = nullptr;
StopAuditFn g_realStopAudit = nullptr;
std::atomic<bool> g_throwAfterAuditStart{false}, g_throwAfterAuditStop{false};

HRESULT STDMETHODCALLTYPE FaultAfterAuditStart(ID3D9BridgeDiagnostics* self, UINT64 capture, UINT duration)
{
    const auto result = g_realStartAudit(self, capture, duration);
    if (SUCCEEDED(result) && g_throwAfterAuditStart.exchange(false))
        throw std::runtime_error("fixture exception after real StartAudit");
    return result;
}
HRESULT STDMETHODCALLTYPE FaultAfterAuditStop(ID3D9BridgeDiagnostics* self, UINT64 capture)
{
    const auto result = g_realStopAudit(self, capture);
    if (SUCCEEDED(result) && g_throwAfterAuditStop.exchange(false))
        throw std::runtime_error("fixture exception after real StopAudit");
    return result;
}
struct DiagnosticFault {
    ID3D9BridgeDiagnostics* diagnostics = nullptr;
    void** original = nullptr;
    std::array<void*, 9> replacement{};
    explicit DiagnosticFault(IDirect3DDevice9* device) {
        if (FAILED(device->QueryInterface(IID_D3D9BridgeDiagnostics, reinterpret_cast<void**>(&diagnostics))) || !diagnostics) return;
        original = *reinterpret_cast<void***>(diagnostics);
        std::copy(original, original + replacement.size(), replacement.begin());
        g_realStartAudit = reinterpret_cast<StartAuditFn>(replacement[4]);
        g_realStopAudit = reinterpret_cast<StopAuditFn>(replacement[5]);
        replacement[4] = reinterpret_cast<void*>(&FaultAfterAuditStart);
        replacement[5] = reinterpret_cast<void*>(&FaultAfterAuditStop);
        *reinterpret_cast<void***>(diagnostics) = replacement.data();
    }
    ~DiagnosticFault() {
        if (diagnostics) {
            *reinterpret_cast<void***>(diagnostics) = original;
            diagnostics->Release();
        }
        g_throwAfterAuditStart = g_throwAfterAuditStop = false;
    }
};

bool TestKeyDown(int key) { return key == VK_F7 ? g_f7.load() : key == VK_F8 && g_f8.load(); }
bool TestFrame(std::uint32_t& frame) {
    if (g_throwFrameRead.exchange(false)) throw std::runtime_error("fixture frame callback failure");
    frame = g_presentFrame.load(std::memory_order_relaxed); return true;
}
void TestLog(const char* text) { std::printf("runtime: %s\n", text ? text : "<null>"); }
struct DelayedWorker { LPTHREAD_START_ROUTINE start; void* context; };
DWORD WINAPI StartDelayedWorker(void* context)
{
    std::unique_ptr<DelayedWorker> worker(static_cast<DelayedWorker*>(context));
    SetEvent(g_reportWorkerStarted);
    WaitForSingleObject(g_releaseReportWorker, INFINITE);
    return worker->start(worker->context);
}
struct HostScheduledWorker { LPTHREAD_START_ROUTINE entry; void* context; };
DWORD WINAPI StartHostScheduledWorker(void* context) {
    std::unique_ptr<HostScheduledWorker> start(static_cast<HostScheduledWorker*>(context));
    renderstack::scheduling::ThreadSchedulingScope scheduling(
        g_hostWorkerSchedulingOptions, renderstack::scheduling::Role::Background, &TestLog);
    return start->entry(start->context);
}
HANDLE StartTestWorker(LPTHREAD_START_ROUTINE start, void* context)
{
    if (g_failWorker || std::exchange(g_failOneWorker, false)) return nullptr;
    const bool delay = g_delayNextWorker.exchange(false) || g_delayReportWorker && g_workers.size() == (g_deviceHasTelemetry ? 3 : 2);
    auto* delayed = delay ? new DelayedWorker{start, context} : nullptr;
    const auto failureOrdinal = g_failNextWorkerAllocationOrdinal.exchange(0);
    const bool failCsObject = g_failNextWorkerCsObject.exchange(false);
    const auto entry = delay ? StartDelayedWorker : start;
    void* entryContext = delay ? static_cast<void*>(delayed) : context;
    auto* scheduled = g_matchHostWorkerScheduling ? new HostScheduledWorker{entry, entryContext} : nullptr;
    HANDLE handle = CreateThread(nullptr, 0, scheduled ? StartHostScheduledWorker : entry,
        scheduled ? static_cast<void*>(scheduled) : entryContext, failureOrdinal ? CREATE_SUSPENDED : 0, nullptr);
    if (handle && failureOrdinal) {
        g_workerAllocationCount = 0;
        g_seenMainSampleBuffer = false;
        g_failAllocationThread = GetThreadId(handle);
        g_failAllocationCountdown = failureOrdinal;
        g_failAllocationSize = failCsObject ? SIZE_MAX - 3 : SIZE_MAX - 2;
        ResumeThread(handle);
    }
    if (!handle) { delete scheduled; delete delayed; }
    if (handle) g_workers.push_back(handle);
    return handle;
}

LRESULT CALLBACK FixtureWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

struct DeviceFixture {
    HMODULE module = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    HWND window = nullptr;
    D3DPRESENT_PARAMETERS parameters{};
    bool backend = false;
    std::string libraryPath;

    explicit DeviceFixture(const char* backendPath, UINT width = 64, UINT height = 64, bool visible = false)
    {
        backend = backendPath != nullptr;
        std::string path;
        if (backendPath) {
            path = backendPath;
            SetEnvironmentVariableA("DXVK_CONFIG", "d3d9.gtaSaCompat = True; d3d9.gtaSaCompatDiagnostics = True");
        } else {
            char systemDirectory[MAX_PATH]{};
            GetSystemDirectoryA(systemDirectory, MAX_PATH);
            path = std::string(systemDirectory) + "\\d3d9.dll";
        }
        libraryPath = path;
        module = LoadLibraryExW(std::filesystem::u8path(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        using Create = IDirect3D9* (WINAPI*)(UINT);
        const auto create = module ? reinterpret_cast<Create>(GetProcAddress(module, "Direct3DCreate9")) : nullptr;
        d3d = create ? create(D3D_SDK_VERSION) : nullptr;
        if (!d3d) return;
        WNDCLASSA wc{};
        wc.lpfnWndProc = FixtureWindowProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "MeasurementRuntimeProductionTest";
        RegisterClassA(&wc);
        RECT windowRect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
        window = CreateWindowA(wc.lpszClassName, "runtime fixture", WS_OVERLAPPEDWINDOW,
            0, 0, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (visible) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);
        }
        parameters.Windowed = TRUE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = window;
        parameters.BackBufferWidth = width;
        parameters.BackBufferHeight = height;
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &parameters, &device);
        ID3D9BridgeTelemetry* telemetry = nullptr;
        g_deviceHasTelemetry = device && SUCCEEDED(device->QueryInterface(IID_D3D9BridgeTelemetry,
            reinterpret_cast<void**>(&telemetry))) && telemetry;
        if (telemetry) telemetry->Release();
    }
    ~DeviceFixture()
    {
        if (device) device->Release();
        if (d3d) d3d->Release();
        if (module) FreeLibrary(module);
        if (window) DestroyWindow(window);
    }
    HRESULT Present()
    {
        const HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
        if (!g_holdGameFrame.load()) ++g_presentFrame;
        RuntimePresent(device, result);
        return result;
    }
};

unsigned StorageTempFiles() {
    wchar_t directory[MAX_PATH]{}, pattern[MAX_PATH]{};
    const auto length = GetTempPathW(MAX_PATH, directory);
    if (!length || length >= MAX_PATH) return 0;
    swprintf_s(pattern, L"%sF8e*.tmp", directory);
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    unsigned count = 0;
    do { if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++count; } while (FindNextFileW(search, &found));
    FindClose(search); return count;
}

struct RuntimeFixture {
    HANDLE stopEvent = nullptr;
    std::string directory;
    RuntimeConfig config{};
    bool stopped = false;
    unsigned initialStorageFiles = 0;

    explicit RuntimeFixture(const char* caseName)
    {
        RuntimeShutdown();
        initialStorageFiles = StorageTempFiles();
        g_f7 = g_f8 = g_failWorker = false;
        g_delayReportWorker = false;
        g_delayNextWorker = false;
        g_failNextWorkerAllocationOrdinal = 0;
        g_failNextWorkerCsObject = false;
        g_workers.clear();
        g_presentFrame = 0;
        g_holdGameFrame = false;
        g_throwFrameRead = false;
        directory = (std::filesystem::current_path() / "cases" /
            (std::string(caseName) + "-" + std::to_string(GetCurrentProcessId()))).string();
        std::filesystem::create_directories(std::filesystem::path(directory) / "scripts");
        stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        config.enabled = config.hud = config.journal = config.cpu = true;
        config.timing = {10, 60, 20, 50};
        config.intervalMs = 1;
        config.gameDirectory = directory.c_str();
        config.stopEvent = stopEvent;
        config.startWorker = StartTestWorker;
        config.keyDown = TestKeyDown;
        config.log = TestLog;
        config.readFrame = TestFrame;
    }
    void Configure() { ConfigureRuntime(config); }
    void Stop()
    {
        if (stopped) return;
        if (g_releaseReportWorker) SetEvent(g_releaseReportWorker);
        RuntimeShutdown();
        SetEvent(stopEvent);
        for (HANDLE worker : g_workers) {
            Check(WaitForSingleObject(worker, 10000) == WAIT_OBJECT_0,
                "provided worker joins before reconfiguration or event destruction");
            CloseHandle(worker);
        }
        g_workers.clear();
        Check(StorageTempFiles() == initialStorageFiles, "capture owner removes temporary event files after all workers join");
        if (g_reportWorkerStarted) CloseHandle(g_reportWorkerStarted);
        if (g_releaseReportWorker) CloseHandle(g_releaseReportWorker);
        g_reportWorkerStarted = g_releaseReportWorker = nullptr;
        g_delayReportWorker = false;
        stopped = true;
    }
    ~RuntimeFixture() { Stop(); CloseHandle(stopEvent); }
};

RuntimeSnapshot Snapshot()
{
    RuntimeSnapshot snapshot{};
    RuntimeGetSnapshot(&snapshot);
    return snapshot;
}

void TestFrameCorrelation()
{
    FrameTrace trace;
    trace.Add(10, 20, 1, Phase::Baseline, true);
    trace.Add(20, 30, 2, Phase::Cpu, true);
    trace.Add(40, 60, 3, Phase::Attribution, false);
    std::vector<std::uint64_t> ids, lengths;
    const auto covered = trace.ForEachOverlap(15, 45, [&](const FrameRecord& row, auto a, auto b) {
        ids.push_back(row.frameId); lengths.push_back(b - a);
    });
    Check(ids == std::vector<std::uint64_t>({1, 2, 3}) &&
        lengths == std::vector<std::uint64_t>({5, 10, 5}) && covered == 20,
        "cross-frame spans retain each half-open overlap and leave the phase gap unmatched");
    unsigned calls = 0;
    Check(trace.ForEachOverlap(20, 20, [&](const FrameRecord&, auto, auto) { ++calls; }) == 0 && !calls,
        "zero-length spans and exact boundaries do not create false overlaps");
    Controller controller({1, 1, 1, 50});
    controller.Key(false, true, 100, true);
    controller.Present(1'000'100, false);
    controller.Present(2'000'100, true);
    Check(controller.trace().size() == 1 && !controller.trace().data()[0].beginIsSuccessfulPresent,
        "failed phase-entry Present marks the next interval boundary truncated");
}

bool WaitPhase(DeviceFixture& device, Phase phase, DWORD maximumMs = 8000)
{
    const ULONGLONG deadline = GetTickCount64() + maximumMs;
    do {
        device.Present();
        if (Snapshot().phase == phase) return true;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return false;
}

void TestDisabled(DeviceFixture& device)
{
    RuntimeFixture runtime(device.backend ? "dxvk-disabled" : "system-disabled");
    runtime.config.enabled = false;
    runtime.Configure();
    g_f7 = g_f8 = true;
    for (unsigned i = 0; i < 4; ++i) device.Present();
    const auto snapshot = Snapshot();
    Check(!RuntimeEnabled() && snapshot.phase == Phase::Ready &&
        snapshot.captureId == 0 && !snapshot.workerBusy && g_workers.empty(),
        "disabled runtime admits no capture and starts no sampling worker");
}

void TestSecondaryDeviceCannotClaimCapture(DeviceFixture& selected)
{
    RuntimeFixture runtime(selected.backend ? "dxvk-selected-owner" : "system-selected-owner");
    runtime.Configure();
    RuntimePrepareDevice(selected.device);
    g_f8 = true;
    bool secondaryCreated = false, secondaryPresented = false;
    std::thread secondary([&] {
        DeviceFixture other(selected.backend ? selected.libraryPath.c_str() : nullptr);
        secondaryCreated = other.device != nullptr;
        if (other.device) {
            secondaryPresented = SUCCEEDED(other.Present());
            RuntimeInterrupt(other.device, "secondary device released", true);
        }
    });
    secondary.join();
    Check(secondaryCreated && secondaryPresented, "secondary real device Presents first on a different thread");
    Check(Snapshot().captureId == 0 && Snapshot().phase == Phase::Ready && g_workers.empty(),
        "secondary device neither consumes F8 nor starts selected-device capture");
    selected.Present();
    const auto started = Snapshot();
    Check(started.captureId != 0 && started.phase == Phase::Baseline,
        "selected device owns its render thread and accepts F8 after secondary first Present");
    g_f8 = false;
    if (started.captureId) {
        Check(WaitPhase(selected, Phase::Complete),
            "selected-device F8 capture completes after another device presented first");
        Check(Snapshot().cpuSamples > 0, "selected render thread supplies actual CPU samples");
    }
    runtime.Stop();
}

std::string JsonString(std::string_view text);

void TestUnicodeModuleEnumeration(DeviceFixture& original)
{
    if (!original.backend) return;
    RuntimeFixture runtime("unicode-module-enumeration");
    const auto folder = std::filesystem::path(runtime.directory) / L"\u6d4b\u8bd5-\u00e9-\u03a9";
    std::filesystem::create_directories(folder);
    const auto binary = folder / "d3d9.dll";
    const auto source = std::filesystem::u8path(original.libraryPath);
    std::filesystem::copy_file(source, binary, std::filesystem::copy_options::overwrite_existing);
    auto sidecar = source; sidecar += L".rsym";
    Check(std::filesystem::is_regular_file(sidecar), "Unicode fixture has matching real backend RSYM sidecar");
    if (!std::filesystem::is_regular_file(sidecar)) return;
    std::filesystem::copy_file(sidecar, folder / "d3d9.dll.rsym", std::filesystem::copy_options::overwrite_existing);
    const auto encoded = binary.u8string();
    const std::string utf8(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    DeviceFixture unicode(utf8.c_str());
    Check(unicode.device && unicode.module != original.module,
        "actual backend loads as a separate module from mixed Unicode directory");
    if (!unicode.device) return;
    runtime.Configure();
    g_f8 = true; unicode.Present(); g_f8 = false;
    Check(WaitPhase(unicode, Phase::Complete), "Unicode-loaded backend completes production CPU capture");
    const auto snapshot = Snapshot();
    runtime.Stop();
    std::ifstream input(snapshot.reportPath);
    const std::string report((std::istreambuf_iterator<char>(input)), {});
    const auto expected = "path=" + JsonString(utf8);
    Check(report.find(expected) != std::string::npos,
        "Toolhelp module path reaches capture report as lossless UTF-8");
    // Resolve using the path emitted by the production enumerator, after the
    // exact report/path comparison above. This uses the real on-disk sidecar.
    if (report.find(expected) != std::string::npos) {
        MeasurementSymbolResolver resolver;
        const auto base = reinterpret_cast<std::uintptr_t>(unicode.module);
        Check(resolver.LoadModule(base, utf8, utf8 + ".rsym") == SymbolLoadStatus::Ready,
            "enumerated Unicode path opens and validates matching RSYM and binary");
        const auto address = reinterpret_cast<std::uintptr_t>(GetProcAddress(unicode.module, "Direct3DCreate9"));
        const auto symbol = resolver.Resolve(address);
        Check(symbol.name.find("Direct3DCreate9") != std::string::npos &&
            (symbol.quality == SymbolQuality::Exact || symbol.quality == SymbolQuality::Inferred),
            "Unicode module resolves an actual executable address to its function");
    }
    std::printf("Unicode module enumeration tested with Windows ACP=%u\n", GetACP());
}

bool RecordJournal(ProperShadersStateJournal* journal)
{
    HRESULT result = journal->BeginTransaction(GetCurrentThreadId(), 0);
    journal->SetProbeTechnique("RuntimeDuplicateChanged");
    journal->SetCurrentPass(3);
    if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
    return SUCCEEDED(journal->Restore()) && SUCCEEDED(result);
}

template<class Counter>
bool HasMeasuredCoverage(const Counter& counter)
{
    if constexpr (requires { counter.TimedCalls; }) return counter.TimedCalls > 0;
    else return false;
}

void TestComplete(DeviceFixture& device)
{
    RuntimeFixture runtime(device.backend ? "dxvk-complete" : "system-complete");
    g_reportWorkerStarted = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_releaseReportWorker = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_delayReportWorker = true;
    runtime.Configure();
    Check(RuntimeEnabled() && !RuntimeGetSnapshot(nullptr),
        "enabled runtime exposes snapshots and rejects null output");
    device.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    auto* journal = new ProperShadersStateJournal(device.device);
    Check(journal->SupportsNativeStateJournal() == device.backend,
        "runtime fixture uses the expected real native or local journal path");
    IDirect3DStateBlock9* stateBlock = nullptr;
    Check(SUCCEEDED(device.device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock,
        "real state block creates for inclusive capture and apply timing");
    ID3D9BridgeDiagnostics* diagnostics = nullptr;
    const HRESULT query = device.device->QueryInterface(IID_D3D9BridgeDiagnostics,
        reinterpret_cast<void**>(&diagnostics));
    g_f8 = true;
    device.Present();
    auto snapshot = Snapshot();
    const std::uint64_t capture = snapshot.captureId;
    Check(capture != 0 && snapshot.phase == Phase::Baseline,
        "injected F8 edge starts a baseline capture at a real Present boundary");
    g_f7 = true;
    std::vector<Phase> phases{snapshot.phase};
    unsigned journalTransactions = 0;
    bool journalOk = true, stableIdentity = true;
    bool observerStagesSeparated = true;
    bool sampledCpuHud = false;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (snapshot.phase != Phase::Complete && snapshot.phase != Phase::Error &&
        GetTickCount64() < deadline) {
        if (snapshot.phase == Phase::Baseline || snapshot.phase == Phase::Cpu ||
            snapshot.phase == Phase::CpuFinishing) {
            observerStagesSeparated &= !g_properShadersStateAttributionActive.load(std::memory_order_acquire);
            if (snapshot.phase == Phase::Baseline) observerStagesSeparated &= snapshot.cpuSamples == 0;
            if (diagnostics) {
                D3D9BridgeAuditSnapshot audit{};
                audit.StructSize = sizeof(audit);
                audit.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
                observerStagesSeparated &= SUCCEEDED(diagnostics->GetAudit(&audit)) && !audit.Active;
            }
        }
        if (snapshot.phase == Phase::Attribution) {
            journalOk &= RecordJournal(journal);
            if (stateBlock) journalOk &= SUCCEEDED(stateBlock->Capture()) && SUCCEEDED(stateBlock->Apply());
            ++journalTransactions;
        }
        device.Present();
        snapshot = Snapshot();
        if (snapshot.phase == Phase::Cpu && diagnostics) {
            D3D9BridgeHudSnapshot hud{};
            hud.StructSize = sizeof(hud); hud.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
            if (SUCCEEDED(diagnostics->GetHud(&hud))) {
                const std::string line(hud.Lines[4]);
                sampledCpuHud |= line.find("sampling") != std::string::npos &&
                    line.find("baseline") == std::string::npos;
            }
        }
        stableIdentity &= snapshot.captureId == capture;
        if (phases.back() != snapshot.phase) phases.push_back(snapshot.phase);
        if (snapshot.phase == Phase::AttributionFinishing &&
            WaitForSingleObject(g_reportWorkerStarted, 0) == WAIT_OBJECT_0) break;
        Sleep(1);
    }
    const bool reportPaused = WaitForSingleObject(g_reportWorkerStarted, 2000) == WAIT_OBJECT_0;
    g_f7 = g_f8 = false;
    device.Present();
    g_f7 = true;
    device.Present();
    Check(reportPaused && Snapshot().workerBusy && Snapshot().captureId == capture &&
        (Snapshot().phase == Phase::Complete || Snapshot().phase == Phase::AttributionFinishing),
        "fresh key edges while the actual report worker is busy stay suppressed");
    SetEvent(g_releaseReportWorker);
    g_delayReportWorker = false;
    const ULONGLONG reportDeadline = GetTickCount64() + 10000;
    while ((snapshot.workerBusy || snapshot.phase == Phase::AttributionFinishing) &&
        GetTickCount64() < reportDeadline) {
        device.Present();
        Sleep(1);
        snapshot = Snapshot();
        if (phases.back() != snapshot.phase) phases.push_back(snapshot.phase);
    }
    std::vector<Phase> withoutFinishing;
    for (const auto phase : phases)
        if (phase != Phase::CpuFinishing && phase != Phase::AttributionFinishing) withoutFinishing.push_back(phase);
    const std::vector<Phase> expected{Phase::Baseline, Phase::Cpu, Phase::Attribution, Phase::Complete};
    Check(snapshot.phase == Phase::Complete && withoutFinishing == expected,
        "production runtime orders baseline, CPU sampling and state attribution through completion");
    Check(!snapshot.workerBusy, "completed report worker finishes before report inspection");
    device.Present();
    Check(Snapshot().captureId == capture && Snapshot().phase == Phase::Complete,
        "a key held across report finalization does not start a delayed capture");
    Check(observerStagesSeparated,
        "baseline runs without sampling and baseline/CPU phases leave journal and backend attribution inactive");
    if (diagnostics) Check(sampledCpuHud,
        "actual published HUD describes CPU sampling while its worker is still running");
    Check(stableIdentity && snapshot.cpuSamples > 0 && snapshot.suspendedNs > 0 &&
        snapshot.phaseStats[0].presents > 0 && snapshot.phaseStats[1].presents > 0 &&
        snapshot.phaseStats[2].presents > 0,
        "one capture retains identity and records actual CPU samples and phase Present counts");
    JournalAttributionSummary summary{};
    JournalAttributionGetSummary(&summary);
    DWORD cull = 0;
    device.device->GetRenderState(D3DRS_CULLMODE, &cull);
    Check(journalOk && journalTransactions > 0 && summary.available &&
        summary.captureId == capture && summary.transactions == journalTransactions &&
        summary.records == std::uint64_t(journalTransactions) * 3 &&
        summary.restoreCount == journalTransactions && summary.restoreQpc > 0 &&
        summary.topTechniqueCount > 0 &&
        std::strcmp(summary.topTechniques[0].name, "RuntimeDuplicateChanged") == 0 &&
        cull == D3DCULL_CCW,
        "runtime attributes real named duplicate and changed journal writes without changing restore state");
    Check(snapshot.reportPath[0] && std::filesystem::is_regular_file(snapshot.reportPath),
        "completed production runtime writes an inspectable combined report");
    if (snapshot.reportPath[0] && std::filesystem::is_regular_file(snapshot.reportPath)) {
        std::ifstream file(snapshot.reportPath);
        const std::string report((std::istreambuf_iterator<char>(file)), {});
        Check(report.find("threadActivity phase=baseline available=" + std::string(device.backend ? "1" : "0")) != std::string::npos &&
            report.find("threadActivity phase=cpu available=" + std::string(device.backend ? "1" : "0")) != std::string::npos &&
            report.find("threadActivity phase=attribution available=" + std::string(device.backend ? "1" : "0")) != std::string::npos &&
            report.find("non-CPU-wall-is-not-proven-idle") != std::string::npos,
            "combined report includes real per-phase main/CS activity or explicit unavailable status");
        Check(!report.empty() &&
            report.find("captureId=" + std::to_string(capture)) != std::string::npos &&
            report.find("phase=baseline") != std::string::npos &&
            report.find("phase=cpu") != std::string::npos &&
            report.find("phase=attribution") != std::string::npos &&
            report.find("sample ") != std::string::npos &&
            report.find("frame ") != std::string::npos &&
            (!device.backend || report.find("backendCounter kind=") != std::string::npos) &&
            report.find("journalTechnique name=RuntimeDuplicateChanged") != std::string::npos &&
            report.find("technique=RuntimeDuplicateChanged") != std::string::npos &&
            report.find("pass=pass index=3") != std::string::npos &&
            report.find("op=RS count=") != std::string::npos,
            "combined report links capture identity, phases, raw samples/frames and named attribution");
    }
    if (device.backend) {
        Check(SUCCEEDED(query) && diagnostics, "backend provides the actual private diagnostics interface");
        if (diagnostics) {
            D3D9BridgeAuditSnapshot audit{};
            audit.StructSize = sizeof(audit);
            audit.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
            Check(SUCCEEDED(diagnostics->GetAudit(&audit)) && audit.CaptureId == capture &&
                audit.Status == D3D9_BRIDGE_AUDIT_COMPLETE && audit.Available && !audit.Active &&
                audit.Total.Calls > 0 && audit.Total.RedundantCalls > 0 &&
                audit.ObservedPresentBoundaries > 0,
                "completed runtime links measured backend calls and redundant writes to capture identity");
            Check(HasMeasuredCoverage(audit.Total) && HasMeasuredCoverage(audit.Counters[0]) &&
                audit.Total.QpcTicks > 0 && audit.Total.MaxQpcTicks > 0 &&
                audit.Counters[15].Calls >= journalTransactions && audit.Counters[15].QpcTicks > 0 &&
                audit.Counters[16].Calls >= journalTransactions && audit.Counters[16].QpcTicks > 0,
                "backend reports timing coverage and positive setter/state-block inclusive QPC observations");
            D3D9BridgeHudSnapshot hud{};
            hud.StructSize = sizeof(hud);
            hud.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
            Check(SUCCEEDED(diagnostics->GetHud(&hud)) && hud.CaptureId == capture &&
                hud.Phase == static_cast<UINT>(Phase::Complete) && hud.Visible && hud.LineCount > 0,
                "completed runtime publishes the copied HUD capture and phase");
            diagnostics->Release();
        }
    } else {
        Check(FAILED(query) && !diagnostics &&
            (snapshot.unavailableMask & RuntimeBackendUnavailable) != 0 &&
            (snapshot.unavailableMask & RuntimeJournalUnavailable) == 0,
            "system D3D9 completes with explicit unavailable backend integration");
    }
    if (stateBlock) stateBlock->Release();
    journal->Release();
    for (unsigned i = 0; i < 4; ++i) device.Present();
    Check(Snapshot().captureId == capture && Snapshot().phase == Phase::Complete,
        "held F7 and F8 never restart a completed capture");
    g_f7 = g_f8 = false;
    device.Present();
    g_f7 = true;
    device.Present();
    Check(Snapshot().captureId != capture && Snapshot().phase == Phase::Baseline,
        "released keys followed by an F7 edge start the same workflow");
    RuntimeInterrupt(device.device, "alias fixture cleanup");
    runtime.Stop();
}

// This exercises the production runtime and real device over multiple memory
// snapshot intervals, including unchanged game-frame observations and actual
// Win32 minimize/restore transitions. Old backends must report absence rather
// than suppressing the main sampler or pretending telemetry was captured.
void TestCoverageCapture(DeviceFixture& device)
{
    RuntimeFixture runtime("coverage-capture");
    runtime.config.timing = {160, 240, 300, 50};
    runtime.config.intervalMs = 3;
    runtime.Configure();
    ID3D9BridgeTelemetry* telemetry = nullptr;
    const bool hasTelemetry = SUCCEEDED(device.device->QueryInterface(IID_D3D9BridgeTelemetry,
        reinterpret_cast<void**>(&telemetry))) && telemetry;
    if (telemetry) telemetry->Release();

    g_f8 = true;
    device.Present();
    const auto capture = Snapshot().captureId;
    g_holdGameFrame = true;
    ShowWindow(device.window, SW_MINIMIZE);
    IDirect3DTexture9* stalledTexture = nullptr;
    if (SUCCEEDED(device.device->CreateTexture(1024, 1024, 1, D3DUSAGE_DYNAMIC,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &stalledTexture, nullptr)) && stalledTexture) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(stalledTexture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD)))
            stalledTexture->UnlockRect(0);
        stalledTexture->Release();
    }
    Sleep(700); // no Presents: the observer and snapshot-gap report must retain the gap
    const auto deadline = GetTickCount64() + 15000;
    unsigned presents = 0;
    while (Snapshot().phase != Phase::Complete && Snapshot().phase != Phase::Error &&
        GetTickCount64() < deadline) {
        if (presents == 12) {
            ShowWindow(device.window, SW_RESTORE);
            g_holdGameFrame = false;
        }
        IDirect3DTexture9* texture = nullptr;
        if (presents % 16 == 0 && SUCCEEDED(device.device->CreateTexture(512, 512, 1,
            D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr)) && texture) {
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD)))
                texture->UnlockRect(0);
            texture->Release();
        }
        device.Present();
        ++presents;
        Sleep(2);
    }
    auto snapshot = Snapshot();
    while (snapshot.workerBusy && GetTickCount64() < deadline) {
        device.Present(); Sleep(1); snapshot = Snapshot();
    }
    Check(capture && snapshot.phase == Phase::Complete && !snapshot.workerBusy,
        "coverage capture completes through the actual runtime and device");
    std::ifstream file(snapshot.reportPath);
    const std::string report((std::istreambuf_iterator<char>(file)), {});
    Check(report.find("cpuStream kind=main available=1") != std::string::npos &&
        report.find("sample stream=main ") != std::string::npos,
        "combined report labels main-thread samples separately");
    if (device.backend) Check(report.find("cpuStream kind=cs available=1") != std::string::npos &&
        report.find("sample stream=cs ") != std::string::npos &&
        (report.find("caller stream=cs kind=structural-frame-chain") != std::string::npos ||
         report.find("caller stream=cs kind=heuristic-candidate") != std::string::npos),
        "real backend CS thread contributes labelled samples and at least one qualified caller");
    else Check(report.find("cpuStream kind=cs available=0") != std::string::npos,
        "unsupported CS thread sampling is explicitly unavailable");
    Check(report.find("context kind=window") != std::string::npos &&
        report.find("minimized=1") != std::string::npos &&
        report.find("context kind=gameFrame") != std::string::npos &&
        report.find("unchanged=1") != std::string::npos &&
        report.find("context kind=presentGap") != std::string::npos &&
        report.find("reuseCandidate confidence=heuristic") != std::string::npos,
        "report records real window and unchanged game-frame evidence as a heuristic candidate");
    if (hasTelemetry) {
        Check(report.find("telemetry available=1") != std::string::npos &&
            report.find("telemetryEvent ") != std::string::npos &&
            report.find("memorySnapshot ") != std::string::npos &&
            report.find("memoryChunk ") != std::string::npos &&
            report.find("frameOverlap ") != std::string::npos,
            "real telemetry events and allocator snapshots reach the frame-correlated report");
        Check(report.find("boundary=periodic") != std::string::npos &&
            report.find("cadenceGap=1") != std::string::npos &&
            report.find("memoryChangeSemantics=adjacent-snapshots") != std::string::npos,
            "periodic snapshot reports its delayed interval and bounded adjacent-history semantics");
        auto value = [](const std::string& line, const char* key, std::uint64_t& out) {
            const auto position = line.find(std::string(" ") + key + "=");
            if (position == std::string::npos) return false;
            const char* begin = line.c_str() + position + std::strlen(key) + 2;
            char* end = nullptr;
            out = std::strtoull(begin, &end, 10);
            return end != begin;
        };
        std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> frames, events;
        std::istringstream lines(report);
        std::string line;
        unsigned checked = 0;
        bool validOverlap = true, sawSlowOverlap = false;
        while (std::getline(lines, line)) {
            std::uint64_t id = 0, begin = 0, end = 0;
            if (line.rfind("frame ", 0) == 0 && value(line, "id", id) &&
                value(line, "beginNs", begin) && value(line, "endNs", end)) frames[id] = {begin, end};
            if (line.rfind("telemetryEvent ", 0) == 0 && value(line, "index", id) &&
                value(line, "beginNs", begin) && value(line, "endNs", end)) events[id] = {begin, end};
            if (line.rfind("frameOverlap ", 0) != 0) continue;
            std::uint64_t event = 0, frame = 0, overlap = 0;
            const bool fields = value(line, "event", event) && value(line, "frame", frame) &&
                value(line, "beginNs", begin) && value(line, "endNs", end) && value(line, "overlapNs", overlap);
            if (!fields || !events.count(event) || !frames.count(frame)) { validOverlap = false; continue; }
            const auto& span = events[event]; const auto& interval = frames[frame];
            validOverlap &= begin == (std::max)(span.first, interval.first) &&
                end == (std::min)(span.second, interval.second) && end > begin && overlap == end - begin;
            sawSlowOverlap |= line.find("class=slow") != std::string::npos;
            ++checked;
        }
        Check(validOverlap && checked > 0 && sawSlowOverlap,
            "real event overlaps equal the independently checked frame intersections including a slow frame");
    } else Check(report.find("telemetry available=0") != std::string::npos,
        "older backend explicitly reports missing event and memory telemetry");
    g_holdGameFrame = false;
    g_f8 = false;
    runtime.Stop();
}

void TestAllocationFailureAtCaptureEnd(DeviceFixture& device, bool reportAllocation)
{
    RuntimeFixture runtime(reportAllocation ? "report-allocation-failure" : "snapshot-allocation-failure");
    runtime.config.timing = {10, 100, 100, 50};
    runtime.config.telemetry = !reportAllocation;
    runtime.Configure();
    ID3D9BridgeTelemetry* telemetry = nullptr;
    const bool supported = SUCCEEDED(device.device->QueryInterface(IID_D3D9BridgeTelemetry,
        reinterpret_cast<void**>(&telemetry))) && telemetry;
    if (!reportAllocation && !supported) {
        if (telemetry) telemetry->Release();
        runtime.Stop(); return;
    }
    g_f8 = true;
    Check(WaitPhase(device, Phase::Attribution), "allocation-failure fixture reaches attribution");
    const auto capture = Snapshot().captureId;
    Sleep(120);
    g_failAllocationThread = GetCurrentThreadId();
    g_failAllocationSize = reportAllocation ? SIZE_MAX : 256 * sizeof(D3D9BridgeMemoryChunk);
    try { device.Present(); } catch (const std::bad_alloc&) { /* host's BestEffort boundary swallows this */ }
    const bool injected = g_failAllocationSize.exchange(0) == 0;
    auto snapshot = Snapshot();
    const auto deadline = GetTickCount64() + 2000;
    while (snapshot.workerBusy && GetTickCount64() < deadline) {
        device.Present(); Sleep(1); snapshot = Snapshot();
    }
    if (reportAllocation) {
        Check(injected && snapshot.phase == Phase::Error && snapshot.lastError[0],
            "report allocation failure becomes a visible error instead of false completion");
    } else {
        D3D9BridgeTelemetryRead read{};
        read.StructSize = sizeof(read); read.Version = D3D9_BRIDGE_TELEMETRY_VERSION;
        const bool stopped = telemetry && SUCCEEDED(telemetry->ReadEvents(capture, &read, nullptr, 0)) && !read.Active;
        std::ifstream file(snapshot.reportPath);
        const std::string report((std::istreambuf_iterator<char>(file)), {});
        Check(injected && stopped && snapshot.phase == Phase::Complete &&
            report.find("failures=1") != std::string::npos &&
            (snapshot.unavailableMask & RuntimeTelemetryUnavailable) != 0,
            "snapshot allocation failure still stops real telemetry and reports partial capture");
        if (telemetry && !stopped) telemetry->StopCapture(capture);
    }
    if (telemetry) telemetry->Release();
    g_f8 = false;
    runtime.Stop();
}

void TestContextFailureStatus(DeviceFixture& device)
{
    RuntimeFixture runtime("context-worker-failure");
    runtime.Configure();
    g_throwFrameRead = true;
    g_f8 = true;
    const bool completed = WaitPhase(device, Phase::Complete);
    auto snapshot = Snapshot();
    const auto deadline = GetTickCount64() + 5000;
    while (snapshot.workerBusy && GetTickCount64() < deadline) {
        device.Present(); Sleep(1); snapshot = Snapshot();
    }
    std::ifstream file(snapshot.reportPath);
    const std::string report((std::istreambuf_iterator<char>(file)), {});
    Check(completed && !snapshot.workerBusy && (snapshot.unavailableMask & RuntimeContextUnavailable) &&
        report.find("status=complete-partial") != std::string::npos &&
        report.find("contextSummary available=0") != std::string::npos,
        "context observer failure is reflected in both capture status and detailed report");
    runtime.Stop();
}

void TestContextAllocationFailure(DeviceFixture& device)
{
    RuntimeFixture runtime("context-allocation-failure");
    runtime.Configure();
    g_failAllocationThread = GetCurrentThreadId();
    g_failAllocationSize = SIZE_MAX - 1;
    g_f8 = true;
    try { device.Present(); } catch (const std::bad_alloc&) { }
    const bool injected = g_failAllocationSize.exchange(0) == 0;
    const bool completed = WaitPhase(device, Phase::Complete);
    auto snapshot = Snapshot();
    const auto deadline = GetTickCount64() + 5000;
    while (snapshot.workerBusy && GetTickCount64() < deadline) {
        device.Present(); Sleep(1); snapshot = Snapshot();
    }
    std::ifstream file(snapshot.reportPath);
    const std::string report((std::istreambuf_iterator<char>(file)), {});
    Check(injected && completed && !snapshot.workerBusy &&
        (snapshot.unavailableMask & RuntimeContextUnavailable) &&
        report.find("contextSummary available=0") != std::string::npos,
        "context allocation failure preserves the remaining capture and reports partial status");
    runtime.Stop();
}

void TestCpuJobAllocationFailure(DeviceFixture& device)
{
    RuntimeFixture runtime("cpu-job-allocation-failure");
    runtime.config.timing = {100, 1000, 100, 50};
    runtime.Configure();
    ID3D9BridgeTelemetry* telemetry = nullptr;
    const bool supported = SUCCEEDED(device.device->QueryInterface(IID_D3D9BridgeTelemetry,
        reinterpret_cast<void**>(&telemetry))) && telemetry;
    g_f8 = true;
    device.Present();
    const auto capture = Snapshot().captureId;
    Sleep(120);
    g_failAllocationThread = GetCurrentThreadId();
    g_failAllocationSize = SIZE_MAX;
    try { device.Present(); } catch (const std::bad_alloc&) { /* production BestEffort boundary */ }
    const bool injected = g_failAllocationSize.exchange(0) == 0;
    auto snapshot = Snapshot();
    const bool errorImmediate = snapshot.phase == Phase::Error;
    D3D9BridgeTelemetryRead read{};
    read.StructSize = sizeof(read); read.Version = D3D9_BRIDGE_TELEMETRY_VERSION;
    const bool stopped = !supported ||
        (SUCCEEDED(telemetry->ReadEvents(capture, &read, nullptr, 0)) && !read.Active);
    const auto deadline = GetTickCount64() + 2000;
    while (snapshot.workerBusy && GetTickCount64() < deadline) {
        device.Present(); Sleep(1); snapshot = Snapshot();
    }
    std::ifstream file(snapshot.reportPath);
    const std::string report((std::istreambuf_iterator<char>(file)), {});
    Check(capture && injected && errorImmediate && stopped && !snapshot.workerBusy &&
        (snapshot.unavailableMask & RuntimeCpuUnavailable) && report.find("status=error") != std::string::npos,
        "CPU job allocation failure stops active telemetry and reports Error at the phase boundary");
    if (Snapshot().phase != Phase::Error) RuntimeInterrupt(device.device, "fixture cleanup");
    if (telemetry) telemetry->Release();
    g_f8 = false;
    runtime.Stop();
}

void ArmCsResolveAllocationFailure(const char*, const RuntimeCpuReport&)
{
    // The production main-stream adapter callback runs after main symbol
    // processing; fail the next allocation on that same sampler thread.
    g_failAllocationThread = GetCurrentThreadId();
    g_failAllocationSize = SIZE_MAX;
}

void TestCsFailureIsolation(DeviceFixture& device, bool resolutionFailure)
{
    if (!device.backend) return;
    RuntimeFixture runtime(resolutionFailure ? "cs-resolution-failure" : "cs-allocation-failure");
    runtime.config.context = false; // the first fixture worker is the sampler
    runtime.config.timing = {30, 200, 50, 50};
    runtime.config.intervalMs = 2;
    runtime.config.csIntervalMs = 5;
    if (resolutionFailure) runtime.config.appendCpuReport = ArmCsResolveAllocationFailure;
    runtime.Configure();
    g_f8 = true;
    device.Present();
    if (!resolutionFailure) { g_failNextWorkerAllocationOrdinal = 1; g_failNextWorkerCsObject = true; }
    const auto deadline = GetTickCount64() + 6000;
    RuntimeSnapshot snapshot{};
    do {
        device.Present(); Sleep(1); snapshot = Snapshot();
    } while (snapshot.phase != Phase::Complete && snapshot.phase != Phase::Error && GetTickCount64() < deadline);
    while (snapshot.workerBusy && GetTickCount64() < deadline) {
        device.Present(); Sleep(1); snapshot = Snapshot();
    }
    const bool injected = g_failAllocationSize.exchange(0) == 0;
    std::ifstream file(snapshot.reportPath);
    const std::string report((std::istreambuf_iterator<char>(file)), {});
    const char* reason = resolutionFailure ? "cs-symbol-resolution-failed" : "cs-allocation-failed";
    const bool passed = injected && snapshot.phase == Phase::Complete && !snapshot.workerBusy && snapshot.cpuSamples > 0 &&
        (snapshot.unavailableMask & RuntimeCsCpuUnavailable) &&
        report.find("cpuStream kind=main available=1") != std::string::npos &&
        report.find("cpuStream kind=cs available=0") != std::string::npos && report.find(reason) != std::string::npos &&
        (!resolutionFailure || snapshot.csSamples > 0);
    if (!passed && !resolutionFailure) {
        std::printf("CS allocation fixture injected=%u phase=%u mainSamples=%llu error=%s allocationSizes=",
            unsigned(injected), unsigned(snapshot.phase), snapshot.cpuSamples, snapshot.lastError);
        const auto count = (std::min)(g_workerAllocationCount.load(), unsigned(std::size(g_workerAllocationSizes)));
        for (unsigned i = 0; i < count; ++i) std::printf("%s%zu", i ? "," : "", g_workerAllocationSizes[i].load());
        std::printf("\n");
    }
    Check(passed,
        resolutionFailure ? "CS symbol resolution failure preserves main results and raw CS samples" :
            "CS-only allocation failure preserves successful main sampling");
    g_f8 = false;
    runtime.Stop();
}

void TestAuditBoundaryException(DeviceFixture& device, bool startFailure)
{
    if (!device.backend) return;
    RuntimeFixture runtime(startFailure ? "audit-start-exception" : "audit-stop-exception");
    runtime.config.timing = {30, 100, 60, 50};
    runtime.Configure();
    RuntimePrepareDevice(device.device);
    DiagnosticFault fault(device.device);
    ID3D9BridgeTelemetry* telemetry = nullptr;
    const bool supported = SUCCEEDED(device.device->QueryInterface(IID_D3D9BridgeTelemetry,
        reinterpret_cast<void**>(&telemetry))) && telemetry;
    if (!fault.diagnostics || !supported) {
        if (telemetry) telemetry->Release(); runtime.Stop(); return;
    }
    if (startFailure) g_throwAfterAuditStart = true; else g_throwAfterAuditStop = true;
    g_f8 = true;
    RuntimeSnapshot snapshot{};
    const auto deadline = GetTickCount64() + 4000;
    do {
        try { device.Present(); } catch (const std::exception&) { /* production BestEffort boundary */ }
        Sleep(1); snapshot = Snapshot();
    } while (snapshot.phase != Phase::Complete && snapshot.phase != Phase::Error && GetTickCount64() < deadline);
    const auto capture = snapshot.captureId;
    const auto drainDeadline = GetTickCount64() + 2000;
    while (snapshot.workerBusy && GetTickCount64() < drainDeadline) {
        try { device.Present(); } catch (const std::exception&) { }
        Sleep(1); snapshot = Snapshot();
    }
    D3D9BridgeAuditSnapshot audit{};
    audit.StructSize = sizeof(audit); audit.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
    D3D9BridgeTelemetryRead read{};
    read.StructSize = sizeof(read); read.Version = D3D9_BRIDGE_TELEMETRY_VERSION;
    const bool stopped = SUCCEEDED(fault.diagnostics->GetAudit(&audit)) && !audit.Active &&
        SUCCEEDED(telemetry->ReadEvents(capture, &read, nullptr, 0)) && !read.Active;
    const bool injected = startFailure ? !g_throwAfterAuditStart.load() : !g_throwAfterAuditStop.load();
    std::ifstream file(snapshot.reportPath);
    const std::string report((std::istreambuf_iterator<char>(file)), {});
    Check(capture && injected && stopped && snapshot.phase == Phase::Complete && !snapshot.workerBusy &&
        (snapshot.unavailableMask & RuntimeBackendUnavailable) && report.find("status=complete-partial") != std::string::npos,
        startFailure ? "exception after real StartAudit stops audit and completes remaining capture as partial" :
            "exception after real StopAudit cannot skip telemetry, context and report cleanup");
    if (!stopped) { telemetry->StopCapture(capture); fault.diagnostics->StopAudit(capture); }
    telemetry->Release();
    g_f8 = false;
    runtime.Stop();
}

void TestWorkerFailure(DeviceFixture& device)
{
    RuntimeFixture runtime(device.backend ? "dxvk-worker-failure" : "system-worker-failure");
    runtime.Configure();
    g_failWorker = true;
    g_f8 = true;
    Check(WaitPhase(device, Phase::Error), "worker creation failure reaches Error");
    const auto snapshot = Snapshot();
    Check(snapshot.lastError[0] && snapshot.cpuSamples == 0 && !snapshot.workerBusy &&
        snapshot.phaseStats[2].presents == 0,
        "failed worker is explicit and never reports successful sampling or attribution");
}

void TestInterruption(DeviceFixture& device, bool release)
{
    RuntimeFixture runtime(release ? (device.backend ? "dxvk-release" : "system-release") :
        (device.backend ? "dxvk-reset" : "system-reset"));
    runtime.config.timing.cpuMs = 5000;
    runtime.Configure();
    HANDLE finished = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([&] {
        if (WaitForSingleObject(finished, 15000) != WAIT_OBJECT_0) {
            std::fputs("FAIL: CS sampling Reset/Release watchdog expired\n", stderr);
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 87);
        }
    });
    g_f8 = true;
    const bool sampling = WaitPhase(device, Phase::Cpu);
    const auto before = Snapshot();
    Check(sampling && before.workerBusy, "lifecycle fixture reaches an in-flight sampling worker");
    const auto workDeadline = GetTickCount64() + 150;
    while (GetTickCount64() < workDeadline) {
        device.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030, 1, 0);
        if (SUCCEEDED(device.device->BeginScene())) device.device->EndScene();
        device.Present(); Sleep(1);
    }
    RuntimeInterrupt(device.device, release ? "Release" : "Reset", release);
    const auto after = Snapshot();
    Check(after.phase == Phase::Interrupted && after.captureId == before.captureId &&
        after.phaseStats[0].presents > 0 && after.phaseStats[2].presents == 0,
        release ? "Release interruption preserves baseline and does not invent completed attribution" :
                  "Reset interruption preserves baseline and does not invent completed attribution");
    if (release) {
        Check(device.device->Release() == 0, "interrupted device reaches final release");
        device.device = nullptr;
    } else {
        Check(SUCCEEDED(device.device->Reset(&device.parameters)),
            "real D3D9 Reset succeeds after runtime interruption");
    }
    runtime.Stop();
    if (device.backend) Check(Snapshot().csSamples > 0,
        "CS samples were actually captured while real work overlapped Reset/Release cancellation");
    SetEvent(finished);
    watchdog.join();
    CloseHandle(finished);
}

void TestShutdown(DeviceFixture& device)
{
    RuntimeFixture runtime(device.backend ? "dxvk-shutdown" : "system-shutdown");
    runtime.config.timing.cpuMs = 5000;
    runtime.Configure();
    g_f8 = true;
    Check(WaitPhase(device, Phase::Cpu) && Snapshot().workerBusy,
        "shutdown fixture reaches in-flight sampling");
    RuntimeShutdown();
    Check(Snapshot().phase == Phase::Interrupted,
        "shutdown terminates an active capture as interrupted");
    runtime.Stop();
}

void TestAdmittedJournalInterruption(DeviceFixture& device, bool lateRestore, bool naturalStop = false)
{
    const std::string name = std::string(device.backend ? "dxvk-admitted-reset" : "system-admitted-reset") +
        (naturalStop ? "-natural-late" : (lateRestore ? "-late" : "-prompt"));
    RuntimeFixture runtime(name.c_str());
    runtime.Configure();
    HANDLE returned = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE allowRestore = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE restored = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE requestPresent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE presented = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE destroyDevice = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    bool reachedAttribution = false, journalOk = false, resetOk = false, restoredOriginal = false;
    std::uint64_t capture = 0;
    g_f8 = true;
    std::thread watchdog([&] {
        if (WaitForSingleObject(done, 20000) != WAIT_OBJECT_0) {
            std::fputs("FAIL: admitted Runtime interruption watchdog expired (owner/drain deadlock)\n", stderr);
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 86);
        }
    });
    std::thread owner([&] {
        // D3D9 Reset must run on the device's creating thread. This actor owns
        // both creation and Reset, and retains the device through host shutdown.
        {
            DeviceFixture ownedDevice(device.backend ? device.libraryPath.c_str() : nullptr);
            if (ownedDevice.device) {
                ownedDevice.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
                reachedAttribution = WaitPhase(ownedDevice, Phase::Attribution);
                capture = Snapshot().captureId;
                auto* journal = new ProperShadersStateJournal(ownedDevice.device);
                HRESULT result = reachedAttribution ? journal->BeginTransaction(GetCurrentThreadId(), 0) : E_FAIL;
                journal->SetProbeTechnique("AdmittedAcrossReset");
                journal->SetCurrentPass(5);
                if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                if (SUCCEEDED(result)) result = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
                if (naturalStop) {
                    const auto boundaryDeadline = GetTickCount64() + 2000;
                    while (Snapshot().phase == Phase::Attribution && GetTickCount64() < boundaryDeadline) {
                        ownedDevice.Present(); Sleep(1);
                    }
                } else RuntimeInterrupt(ownedDevice.device, "Reset with an admitted owner transaction");
                SetEvent(returned);
                while (WaitForSingleObject(allowRestore, 0) != WAIT_OBJECT_0) {
                    if (WaitForSingleObject(requestPresent, 1) == WAIT_OBJECT_0) {
                        ownedDevice.Present(); SetEvent(presented);
                    }
                }
                journalOk = SUCCEEDED(result) && SUCCEEDED(journal->Restore());
                DWORD cull = 0;
                restoredOriginal = SUCCEEDED(ownedDevice.device->GetRenderState(D3DRS_CULLMODE, &cull)) && cull == D3DCULL_CCW;
                journal->Release();
                resetOk = SUCCEEDED(ownedDevice.device->Reset(&ownedDevice.parameters));
                SetEvent(restored);
                while (WaitForSingleObject(destroyDevice, 0) != WAIT_OBJECT_0) {
                    if (WaitForSingleObject(requestPresent, 1) == WAIT_OBJECT_0) {
                        ownedDevice.Present(); SetEvent(presented);
                    }
                }
            } else {
                SetEvent(returned); SetEvent(restored);
                WaitForSingleObject(destroyDevice, INFINITE);
            }
        }
        SetEvent(done);
    });
    const auto requireEvent = [](HANDLE event, DWORD timeout, const char* failure) {
        if (WaitForSingleObject(event, timeout) != WAIT_OBJECT_0) {
            std::fprintf(stderr, "FAIL: %s\n", failure);
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 86);
        }
    };
    const auto presentOnOwner = [&] {
        SetEvent(requestPresent);
        requireEvent(presented, 3000, "owner Present stalled while a journal transaction was pending");
    };
    requireEvent(returned, 10000, "RuntimeInterrupt did not return before the admitted owner Restore");
    const auto pending = Snapshot();
    const HANDLE reportWorker = g_workers.empty() ? nullptr : g_workers.back();
    JournalAttributionSummary pendingJournal{};
    JournalAttributionGetSummary(&pendingJournal);
    Check(pending.phase == (naturalStop ? Phase::AttributionFinishing : Phase::Interrupted) && pending.workerBusy &&
        pendingJournal.inFlight == 1 && !pendingJournal.active,
        "Runtime Reset interruption returns with closed admission and an admitted transaction still pending");
    if (lateRestore) {
        const bool reportFinished = reportWorker && WaitForSingleObject(reportWorker, 5000) == WAIT_OBJECT_0;
        const auto timedOut = Snapshot();
        JournalAttributionGetSummary(&pendingJournal);
        Check(reportFinished && timedOut.phase == Phase::Interrupted &&
            timedOut.captureId == pending.captureId && pendingJournal.inFlight == 1,
            "bounded report worker finishes while late journal remains pending and capture stays Interrupted");
        g_f7 = g_f8 = false; presentOnOwner();
        g_f7 = true; presentOnOwner();
        Check(Snapshot().captureId == pending.captureId && Snapshot().phase == Phase::Interrupted,
            "a fresh capture edge is rejected after report timeout while old journal is still admitted");
        std::ifstream timeoutFile(timedOut.reportPath);
        const std::string timeoutReport((std::istreambuf_iterator<char>(timeoutFile)), {});
        Check(timeoutReport.find("status=interrupted") != std::string::npos &&
            timeoutReport.find("status=complete") == std::string::npos,
            "bounded pending report explicitly retains interrupted status");
    }
    SetEvent(allowRestore);
    requireEvent(restored, 5000, "actual journal Restore/Reset did not finish after owner interruption returned");
    Check(reportWorker && WaitForSingleObject(reportWorker, 5000) == WAIT_OBJECT_0,
        "actual report worker reaches completion after its bounded journal drain");
    const auto allWorkersDeadline = GetTickCount64() + 5000;
    while (Snapshot().workerBusy && GetTickCount64() < allWorkersDeadline) Sleep(1);
    presentOnOwner();
    const auto completed = Snapshot();
    JournalAttributionSummary finalJournal{};
    JournalAttributionGetSummary(&finalJournal);
    Check(reachedAttribution && journalOk && restoredOriginal && resetOk &&
        !completed.workerBusy && completed.phase == Phase::Interrupted &&
        completed.captureId == capture && finalJournal.inFlight == 0 && finalJournal.captureId == capture &&
        finalJournal.transactions == 1 && finalJournal.records == 3 && finalJournal.restoreCount == 1,
        "late real Restore and actual Reset leave the original capture Interrupted with its original identity");
    if (!lateRestore) {
        std::ifstream file(completed.reportPath);
        const std::string report((std::istreambuf_iterator<char>(file)), {});
        Check(report.find("status=interrupted") != std::string::npos &&
            report.find("technique=AdmittedAcrossReset transactions=1 records=3") != std::string::npos &&
            report.find("pass=pass index=5 transactions=1 records=3") != std::string::npos,
            "deferred interrupted report contains the transaction completed after admission closed");
    } else {
        g_f7 = g_f8 = false; presentOnOwner();
        g_f8 = true; presentOnOwner();
        Check(Snapshot().captureId != capture && Snapshot().phase == Phase::Baseline,
            "a new edge starts a fresh capture only after late Restore has drained");
        RuntimeInterrupt(nullptr, "late-restore fixture cleanup");
    }
    runtime.Stop();
    SetEvent(destroyDevice);
    owner.join(); watchdog.join();
    for (HANDLE event : {returned, allowRestore, restored, requestPresent, presented, destroyDevice, done}) CloseHandle(event);
}
struct BenchmarkVertex { float x, y, z, rhw; DWORD colour; };
constexpr BenchmarkVertex BenchmarkTriangle[] = {
    {3, 3, 0, 1, 0xff80c0ff}, {61, 3, 0, 1, 0xff80c0ff}, {3, 61, 0, 1, 0xff80c0ff}
};

std::uint64_t BenchmarkNowNs()
{
    static const auto frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }();
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
    if (frequency <= 0 || value.QuadPart < 0) return 0;
    const auto t = std::uint64_t(value.QuadPart), f = std::uint64_t(frequency);
    return (t / f) * 1'000'000'000ull + (t % f) * 1'000'000'000ull / f;
}

bool BenchmarkWorkload(DeviceFixture& device, HRESULT& presented, std::uint64_t* presentNs = nullptr)
{
    HRESULT work = device.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030, 1, 0);
    const HRESULT begun = device.device->BeginScene();
    if (SUCCEEDED(begun)) {
        const HRESULT drawn = device.device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1,
            BenchmarkTriangle, sizeof(BenchmarkVertex));
        if (FAILED(drawn)) work = drawn;
        const HRESULT ended = device.device->EndScene();
        if (FAILED(ended)) work = ended;
    } else work = begun;
    const auto presentBegin = presentNs ? BenchmarkNowNs() : 0;
    presented = device.Present();
    if (presentNs) *presentNs = BenchmarkNowNs() - presentBegin;
    return SUCCEEDED(work);
}

bool BenchmarkFence(IDirect3DQuery9* fence)
{
    if (!fence || FAILED(fence->Issue(D3DISSUE_END))) return false;
    const auto deadline = GetTickCount64() + 5000;
    BOOL completed = FALSE;
    HRESULT result = S_FALSE;
    do {
        result = fence->GetData(&completed, sizeof(completed), D3DGETDATA_FLUSH);
        if (result == S_OK) return completed != FALSE;
        if (FAILED(result)) return false;
        Sleep(0);
    } while (GetTickCount64() < deadline);
    return false;
}

std::string JsonString(std::string_view text)
{
    std::string result = "\"";
    for (char value : text) {
        if (value == '\\' || value == '"') result += '\\';
        if (value == '\n') result += "\\n";
        else if (value == '\r') result += "\\r";
        else result += value;
    }
    result += '"';
    return result;
}

std::string BenchmarkHash(const char* variable)
{
    char hash[65]{};
    return GetEnvironmentVariableA(variable, hash, sizeof(hash)) == 64 ? hash : "unknown";
}

bool BenchmarkVariant(DeviceFixture& device, IDirect3DQuery9* fence, unsigned round,
    const char* pair, const char* order, unsigned position, char variant, unsigned iterations)
{
    MSG message{};
    while (PeekMessageA(&message, device.window, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    const std::string caseName = std::string("benchmark-idle-") + variant;
    RuntimeFixture runtime(caseName.c_str());
    runtime.config.enabled = variant != 'A';
    runtime.config.hud = variant == 'C';
    runtime.Configure();
    RuntimePrepareDevice(device.device);
    device.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device.device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    bool warmOk = true;
    for (unsigned i = 0; i < 64; ++i) {
        HRESULT result = E_FAIL;
        warmOk &= BenchmarkWorkload(device, result) && result == S_OK;
    }
    if (!warmOk || !BenchmarkFence(fence)) {
        Check(false, "benchmark workload warms and GPU completes outside timing");
        return false;
    }
    ID3D9BridgeDiagnostics* diagnostics = nullptr;
    device.device->QueryInterface(IID_D3D9BridgeDiagnostics, reinterpret_cast<void**>(&diagnostics));
    D3D9BridgeHudSnapshot hud{};
    hud.StructSize = sizeof(hud); hud.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
    const bool visibilityMatches = diagnostics && SUCCEEDED(diagnostics->GetHud(&hud)) &&
        (hud.Visible != 0) == (variant == 'C') && (variant != 'C' || hud.LineCount == 8);
    if (diagnostics) diagnostics->Release();
    if (!visibilityMatches) {
        Check(false, "benchmark variant has the expected actual copied HUD visibility");
        return false;
    }
    LARGE_INTEGER frequency{}, begin{}, end{};
    QueryPerformanceFrequency(&frequency);
    std::uint64_t successful = 0, failed = 0, workloadFailed = 0;
    QueryPerformanceCounter(&begin);
    for (unsigned i = 0; i < iterations; ++i) {
        HRESULT result = E_FAIL;
        if (!BenchmarkWorkload(device, result)) ++workloadFailed;
        if (result == S_OK) ++successful;
        else ++failed;
    }
    QueryPerformanceCounter(&end);
    const bool gpuCompleted = BenchmarkFence(fence);
    const auto snapshot = Snapshot();
    const bool idle = snapshot.captureId == 0 && snapshot.phase == Phase::Ready && !snapshot.workerBusy;
    const auto ticks = static_cast<std::uint64_t>(end.QuadPart - begin.QuadPart);
    const auto wallNs = static_cast<std::uint64_t>(
        static_cast<long double>(ticks) * 1'000'000'000.0L / frequency.QuadPart);
    const auto backendHash = BenchmarkHash("MEASUREMENT_BENCH_BACKEND_SHA256");
    const auto probeHash = BenchmarkHash("MEASUREMENT_BENCH_PROBE_SHA256");
    std::printf("{\"type\":\"idle-round\",\"pid\":%lu,\"round\":%u,\"pair\":\"%s\",\"order\":\"%s\",\"position\":%u,\"variant\":\"%c\",\"warmup\":64,\"iterations\":%u,\"successfulPresents\":%llu,\"failedPresents\":%llu,\"workloadFailures\":%llu,\"qpcBegin\":%llu,\"qpcEnd\":%llu,\"qpcFrequency\":%llu,\"wallNs\":%llu,\"wallNsPerSuccessfulPresent\":%.9f,\"gpuFenceOutsideTiming\":true,\"gpuCompleted\":%s,\"idle\":%s,\"measurement\":\"wall-clear-draw-present-runtime\",\"backendSha256\":\"%s\",\"probeSha256\":\"%s\"}\n",
        GetCurrentProcessId(), round, pair, order, position, variant, iterations, successful, failed,
        workloadFailed, static_cast<std::uint64_t>(begin.QuadPart),
        static_cast<std::uint64_t>(end.QuadPart), static_cast<std::uint64_t>(frequency.QuadPart), wallNs,
        successful ? static_cast<double>(wallNs) / static_cast<double>(successful) : 0.0,
        gpuCompleted ? "true" : "false", idle ? "true" : "false", backendHash.c_str(), probeHash.c_str());
    std::fflush(stdout);
    const bool valid = gpuCompleted && idle && failed == 0 && workloadFailed == 0;
    if (!valid) Check(false, "benchmark timed workload completed successfully and stayed idle");
    return valid;
}

void BenchmarkActiveCapture(DeviceFixture& device)
{
    RuntimeFixture runtime("benchmark-active-observer-stages");
    runtime.config.timing = {100, 300, 100, 50};
    runtime.config.intervalMs = 5;
    runtime.Configure();
    RuntimePrepareDevice(device.device);
    auto* journal = new ProperShadersStateJournal(device.device);
    g_f8 = true;
    RuntimeSnapshot snapshot{};
    const auto deadline = GetTickCount64() + 15000;
    do {
        if (snapshot.phase == Phase::Attribution && !RecordJournal(journal)) Check(false,
            "active observer illustration records actual journal transaction");
        HRESULT presented = E_FAIL;
        BenchmarkWorkload(device, presented);
        snapshot = Snapshot();
        if (snapshot.phase == Phase::Complete && !snapshot.workerBusy) break;
        if (snapshot.phase == Phase::Error) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    Check(snapshot.phase == Phase::Complete && !snapshot.workerBusy && snapshot.cpuSamples > 0,
        "untimed observer-stage illustration completes actual sequential capture");
    const char* names[] = {"baseline", "cpu", "attribution"};
    for (unsigned phase = 0; phase < 3; ++phase) {
        const auto& stats = snapshot.phaseStats[phase];
        std::printf("{\"type\":\"active-observer-stage\",\"pid\":%lu,\"captureId\":%llu,\"stage\":\"%s\",\"elapsedNs\":%llu,\"successfulPresents\":%llu,\"failedPresents\":%llu,\"cpuSamples\":%llu,\"suspendedNs\":%llu,\"matchedAB\":false,\"reportPath\":%s}\n",
            GetCurrentProcessId(), snapshot.captureId, names[phase], stats.elapsedNs(), stats.presents,
            stats.failedPresents, snapshot.cpuSamples, snapshot.suspendedNs,
            JsonString(snapshot.reportPath).c_str());
    }
    journal->Release();
    runtime.Stop();
}

void TestBenchmark(DeviceFixture& device, unsigned rounds, unsigned iterations, bool activeCapture)
{
    if (!device.backend || rounds < 4 || rounds > 64 || iterations < 32 || iterations > 4096) {
        Check(false, "benchmark requires backend, 4-64 rounds and 32-4096 iterations");
        return;
    }
    IDirect3DQuery9* fence = nullptr;
    Check(SUCCEEDED(device.device->CreateQuery(D3DQUERYTYPE_EVENT, &fence)) && fence,
        "benchmark creates a real GPU completion fence");
    if (!fence) return;
    bool valid = true;
    for (unsigned round = 0; round < rounds && valid; ++round) {
        const char* orders[] = {round % 2 ? "BA" : "AB", round % 2 ? "CA" : "AC"};
        const char* pairs[] = {"AB", "AC"};
        for (unsigned pair = 0; pair < 2 && valid; ++pair)
            for (unsigned position = 0; position < 2 && valid; ++position)
                valid = BenchmarkVariant(device, fence, round, pairs[pair], orders[pair], position,
                    orders[pair][position], iterations);
    }
    fence->Release();
    if (valid && activeCapture) BenchmarkActiveCapture(device);
}

bool CoverageBenchmarkVariant(DeviceFixture& device, IDirect3DQuery9* fence, unsigned round,
    const char* order, unsigned position, bool enabled, unsigned cpuMs)
{
    RuntimeFixture runtime(enabled ? "coverage-bench-B" : "coverage-bench-A");
    runtime.config.timing = {250, cpuMs, 250, 50};
    runtime.config.intervalMs = 5;
    runtime.config.telemetry = runtime.config.csCpu = runtime.config.context = enabled;
    runtime.Configure();
    RuntimePrepareDevice(device.device);
    device.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device.device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device.device->CreateTexture(256, 256, 1, D3DUSAGE_DYNAMIC,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr)) || !texture) return false;
    bool valid = true;
    std::uint64_t workPresentTotalNs = 0, presentTotalNs = 0, maxWorkPresentNs = 0, maxPresentNs = 0;
    std::uint64_t lastWorkNs = 0, lastPresentNs = 0, completionPresentNs = 0, completionWorkNs = 0;
    std::uint64_t cpuFinishingNs = 0, cpuFinishingBegin = 0, reportWaitNs = 0;
    bool cpuFinishingActive = false, cpuFinishingObserved = false, reportWasPending = false;
    (void)BenchmarkNowNs(); // initialize QPC frequency before timed work
    auto work = [&](unsigned iteration, bool timed) {
        const auto workBegin = timed ? BenchmarkNowNs() : 0;
        D3DLOCKED_RECT locked{};
        HRESULT update = texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD);
        if (SUCCEEDED(update)) {
            *static_cast<DWORD*>(locked.pBits) = 0xff000000u | iteration;
            update = texture->UnlockRect(0);
        }
        if (FAILED(update)) valid = false;
        if (iteration % 32 == 0) {
            IDirect3DTexture9* transient = nullptr;
            if (FAILED(device.device->CreateTexture(256, 256, 1, D3DUSAGE_DYNAMIC,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &transient, nullptr)) || !transient) valid = false;
            if (transient) transient->Release();
        }
        HRESULT presented = E_FAIL;
        std::uint64_t presentNs = 0;
        if (!BenchmarkWorkload(device, presented, timed ? &presentNs : nullptr) || presented != S_OK) valid = false;
        if (timed) {
            lastWorkNs = BenchmarkNowNs() - workBegin; lastPresentNs = presentNs;
            workPresentTotalNs += lastWorkNs; presentTotalNs += lastPresentNs;
            maxWorkPresentNs = (std::max)(maxWorkPresentNs, lastWorkNs);
            maxPresentNs = (std::max)(maxPresentNs, lastPresentNs);
        }
    };
    for (unsigned i = 0; i < 64 && valid; ++i) work(i, false);
    valid &= BenchmarkFence(fence);
    g_f8 = true;
    unsigned iterations = 0;
    const auto deadline = GetTickCount64() + cpuMs + 15000;
    RuntimeSnapshot snapshot{};
    while (valid && GetTickCount64() < deadline) {
        work(iterations++, true);
        snapshot = Snapshot();
        const auto observedAt = BenchmarkNowNs();
        if (snapshot.phase == Phase::CpuFinishing) {
            if (!cpuFinishingActive) { cpuFinishingBegin = observedAt; cpuFinishingActive = cpuFinishingObserved = true; }
        } else if (cpuFinishingActive) {
            cpuFinishingNs += observedAt - cpuFinishingBegin; cpuFinishingActive = false;
        }
        if (snapshot.phase == Phase::Complete || snapshot.phase == Phase::Error) {
            completionPresentNs = lastPresentNs; completionWorkNs = lastWorkNs; break;
        }
        Sleep(1);
    }
    if (cpuFinishingActive) cpuFinishingNs += BenchmarkNowNs() - cpuFinishingBegin;
    const auto reportWaitBegin = BenchmarkNowNs();
    reportWasPending = snapshot.workerBusy;
    while (snapshot.workerBusy && GetTickCount64() < deadline) {
        device.Present(); Sleep(1); snapshot = Snapshot();
    }
    reportWaitNs = BenchmarkNowNs() - reportWaitBegin;
    valid &= snapshot.phase == Phase::Complete && !snapshot.workerBusy && snapshot.cpuSamples > 0 &&
        iterations > 0 && completionPresentNs > 0 && completionWorkNs >= completionPresentNs &&
        maxWorkPresentNs >= maxPresentNs && workPresentTotalNs >= presentTotalNs;
    std::ifstream reportFile(snapshot.reportPath);
    const std::string report((std::istreambuf_iterator<char>(reportFile)), {});
    valid &= enabled ? (snapshot.csSamples > 0 && snapshot.telemetryEvents > 0 && snapshot.memorySnapshots >= 2 &&
        report.find("telemetry available=1") != std::string::npos &&
        report.find("contextSummary available=1") != std::string::npos) :
        (snapshot.csSamples == 0 && snapshot.telemetryEvents == 0 &&
        report.find("contextSummary available=0 reason=disabled") != std::string::npos);
    valid &= BenchmarkFence(fence);
    const char* names[] = {"baseline", "cpu", "attribution"};
    for (unsigned phase = 0; phase < 3; ++phase) {
        const auto& stats = snapshot.phaseStats[phase];
        std::printf("{\"type\":\"coverage-benchmark\",\"pid\":%lu,\"round\":%u,\"variant\":\"%c\",\"order\":\"%s\",\"position\":%u,\"collectionEnabled\":%s,\"counterScope\":\"capture\",\"phase\":\"%s\",\"elapsedNs\":%llu,\"presents\":%llu,\"failedPresents\":%llu,\"mainCpuNs\":%llu,\"csCpuNs\":%llu,\"cpuTimeAvailable\":%s,\"mainSamples\":%llu,\"csSamples\":%llu,\"mainMissed\":%llu,\"csMissed\":%llu,\"mainDropped\":%llu,\"csDropped\":%llu,\"suspendedNs\":%llu,\"csSuspendedNs\":%llu,\"mainThreadCycles\":%llu,\"csThreadCycles\":%llu,\"events\":%llu,\"filtered\":%llu,\"dropped\":%llu,\"memorySnapshots\":%llu,\"frameTraceDropped\":%llu,\"workPresentTotalNs\":%llu,\"presentTotalNs\":%llu,\"maxWorkPresentNs\":%llu,\"maxPresentNs\":%llu,\"completionPresentNs\":%llu,\"completionWorkNs\":%llu,\"cpuFinishingNs\":%llu,\"cpuFinishingObserved\":%s,\"reportWaitNs\":%llu,\"reportWasPending\":%s,\"iterations\":%u,\"valid\":%s,\"reportPath\":%s}\n",
            GetCurrentProcessId(), round, enabled ? 'B' : 'A', order, position, enabled ? "true" : "false", names[phase],
            stats.elapsedNs(), stats.presents, stats.failedPresents, snapshot.phaseMainCpuNs[phase],
            snapshot.phaseCsCpuNs[phase], (snapshot.threadActivityMask & (1u << phase)) ? "true" : "false",
            snapshot.cpuSamples, snapshot.csSamples, snapshot.cpuMissed, snapshot.csMissed,
            snapshot.cpuDropped, snapshot.csDropped, snapshot.suspendedNs, snapshot.csSuspendedNs,
            snapshot.mainThreadCycles, snapshot.csThreadCycles, snapshot.telemetryEvents,
            snapshot.telemetryFiltered, snapshot.telemetryDropped, snapshot.memorySnapshots,
            snapshot.frameTraceDropped, workPresentTotalNs, presentTotalNs, maxWorkPresentNs, maxPresentNs,
            completionPresentNs, completionWorkNs, cpuFinishingNs, cpuFinishingObserved ? "true" : "false",
            reportWaitNs, reportWasPending ? "true" : "false", iterations, valid ? "true" : "false", JsonString(snapshot.reportPath).c_str());
    }
    std::fflush(stdout);
    texture->Release();
    g_f8 = false;
    runtime.Stop();
    return valid;
}

void TestCoverageBenchmark(DeviceFixture& device, unsigned rounds, unsigned cpuMs, bool aa)
{
    if (!device.backend || rounds < 1 || rounds > 64 || cpuMs < 200 || cpuMs > 30000) {
        Check(false, "coverage benchmark requires backend, 1-64 rounds, CPU 200-30000 ms");
        return;
    }
    IDirect3DQuery9* fence = nullptr;
    if (FAILED(device.device->CreateQuery(D3DQUERYTYPE_EVENT, &fence)) || !fence) {
        Check(false, "coverage benchmark creates GPU fence"); return;
    }
    bool valid = true;
    for (unsigned round = 0; round < rounds && valid; ++round) {
        const char* order = aa ? "AA" : round % 2 ? "BA" : "AB";
        for (unsigned position = 0; position < 2 && valid; ++position)
            valid = CoverageBenchmarkVariant(device, fence, round, order, position, order[position] == 'B', cpuMs);
    }
    fence->Release();
    Check(valid, "coverage benchmark completes controlled real draw/update/Present captures");
}
// Platform-boundary fault injection: patch only this fixture's Win32 imports.
std::atomic<unsigned> g_storageFault{0};
std::atomic<HANDLE> g_storageHandle{INVALID_HANDLE_VALUE};
std::atomic<HANDLE> g_reportOutputHandle{INVALID_HANDLE_VALUE};
HANDLE g_releaseStorageWrite = nullptr;
HANDLE g_storageReadEntered = nullptr, g_releaseStorageRead = nullptr;
using StorageCreateFn = decltype(&CreateFileW);
using StorageWriteFn = decltype(&WriteFile);
using StorageReadFn = decltype(&ReadFile);
StorageCreateFn g_realStorageCreate = CreateFileW;
StorageWriteFn g_realStorageWrite = WriteFile;
StorageReadFn g_realStorageRead = ReadFile;
HANDLE WINAPI StorageCreate(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD flags, HANDLE templateFile) {
    const bool storage = path && std::wcsstr(path, L"F8e") && (flags & FILE_FLAG_DELETE_ON_CLOSE);
    const bool report = path && std::wcsstr(path, L"BridgeD3D9.measurement-");
    if (report && g_storageFault == 6) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    if (storage && g_storageFault == 1) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    const auto file = g_realStorageCreate(path, access, share, security, disposition, flags, templateFile);
    if (storage) g_storageHandle = file;
    if (report) g_reportOutputHandle = file;
    return file;
}
BOOL WINAPI StorageWrite(HANDLE file, LPCVOID bytes, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    if (file == g_reportOutputHandle && g_storageFault == 7) { *written = 0; SetLastError(ERROR_DISK_FULL); return FALSE; }
    if (file == g_storageHandle && g_storageFault == 2) { *written = 0; SetLastError(ERROR_DISK_FULL); return FALSE; }
    if (file == g_storageHandle && g_storageFault == 4) WaitForSingleObject(g_releaseStorageWrite, 10000);
    return g_realStorageWrite(file, bytes, size, written, overlapped);
}
BOOL WINAPI StorageRead(HANDLE file, LPVOID bytes, DWORD size, LPDWORD read, LPOVERLAPPED overlapped) {
    if (file == g_storageHandle && g_storageFault == 5) { SetEvent(g_storageReadEntered); WaitForSingleObject(g_releaseStorageRead, 10000); }
    if (file == g_storageHandle && g_storageFault == 3) { *read = 0; SetLastError(ERROR_READ_FAULT); return FALSE; }
    return g_realStorageRead(file, bytes, size, read, overlapped);
}
struct StorageFaultImports {
    std::vector<std::pair<ULONG_PTR*, ULONG_PTR>> patched;
    StorageFaultImports() {
        const auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (; descriptor->Name; ++descriptor) {
            auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; thunk->u1.Function; ++thunk) {
                const auto original = thunk->u1.Function;
                ULONG_PTR replacement = 0;
                if (original == reinterpret_cast<ULONG_PTR>(g_realStorageCreate)) replacement = reinterpret_cast<ULONG_PTR>(&StorageCreate);
                if (original == reinterpret_cast<ULONG_PTR>(g_realStorageWrite)) replacement = reinterpret_cast<ULONG_PTR>(&StorageWrite);
                if (original == reinterpret_cast<ULONG_PTR>(g_realStorageRead)) replacement = reinterpret_cast<ULONG_PTR>(&StorageRead);
                if (!replacement) continue;
                auto* slot = &thunk->u1.Function;
                DWORD protection = 0; VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection);
                *slot = replacement; DWORD ignored = 0; VirtualProtect(slot, sizeof(*slot), protection, &ignored);
                patched.emplace_back(slot, original);
            }
        }
    }
    ~StorageFaultImports() {
        for (const auto& entry : patched) {
            DWORD protection = 0; VirtualProtect(entry.first, sizeof(*entry.first), PAGE_READWRITE, &protection);
            *entry.first = entry.second; DWORD ignored = 0; VirtualProtect(entry.first, sizeof(*entry.first), protection, &ignored);
        }
    }
};

using TelemetryStartFn = HRESULT (STDMETHODCALLTYPE*)(ID3D9BridgeTelemetry*, const D3D9BridgeTelemetryConfig*);
using TelemetryStopFn = HRESULT (STDMETHODCALLTYPE*)(ID3D9BridgeTelemetry*, UINT64);
TelemetryStartFn g_realTelemetryStart = nullptr;
TelemetryStopFn g_realTelemetryStop = nullptr;
unsigned g_telemetryBoundaryFault = 0;
HRESULT STDMETHODCALLTYPE TelemetryStartBoundary(ID3D9BridgeTelemetry* self, const D3D9BridgeTelemetryConfig* config) {
    if (g_telemetryBoundaryFault == 1) return E_FAIL;
    if (g_telemetryBoundaryFault == 2) { g_realTelemetryStart(self, config); throw std::runtime_error("fixture telemetry start failure after real StartCapture"); }
    return g_realTelemetryStart(self, config);
}
HRESULT STDMETHODCALLTYPE TelemetryStopBoundary(ID3D9BridgeTelemetry* self, UINT64 id) {
    if (g_telemetryBoundaryFault == 3) return E_FAIL;
    if (g_telemetryBoundaryFault == 4) throw std::runtime_error("fixture telemetry stop failure");
    return g_realTelemetryStop(self, id);
}
struct TelemetryBoundaryImports {
    ID3D9BridgeTelemetry* telemetry = nullptr;
    void** original = nullptr;
    std::array<void*, 2> methods{};
    bool patched = false;
    bool SetMethods(void* start, void* stop) {
        DWORD protection = 0;
        if (!VirtualProtect(original + 3, 2 * sizeof(void*), PAGE_READWRITE, &protection)) return false;
        original[3] = start; original[4] = stop;
        DWORD ignored = 0;
        const bool restored = VirtualProtect(original + 3, 2 * sizeof(void*), protection, &ignored) != FALSE;
        Check(restored, "telemetry fixture restores original vtable page protection");
        return true;
    }
    TelemetryBoundaryImports(IDirect3DDevice9* device, unsigned fault) {
        // No instrumentation in the normal/benchmark path. Cloning an apphelp
        // COM-router vtable copies its Release hook to an unregistered identity
        // and can route Release recursively back to that same hook.
        if (!fault) return;
        if (FAILED(device->QueryInterface(IID_D3D9BridgeTelemetry, reinterpret_cast<void**>(&telemetry))) || !telemetry) return;
        original = *reinterpret_cast<void***>(telemetry);
        methods = { original[3], original[4] };
        g_realTelemetryStart = reinterpret_cast<TelemetryStartFn>(methods[0]);
        g_realTelemetryStop = reinterpret_cast<TelemetryStopFn>(methods[1]);
        g_telemetryBoundaryFault = fault;
        patched = SetMethods(reinterpret_cast<void*>(&TelemetryStartBoundary), reinterpret_cast<void*>(&TelemetryStopBoundary));
        Check(patched, "telemetry fixture patches only private StartCapture and StopCapture slots");
    }
    ~TelemetryBoundaryImports() {
        if (telemetry) {
            if (patched) Check(SetMethods(methods[0], methods[1]), "telemetry fixture restores original private method slots");
            g_telemetryBoundaryFault = 0;
            telemetry->StopCapture(Snapshot().captureId);
            telemetry->Release();
        }
    }
};
struct TelemetryVtableProbe {
    ID3D9BridgeTelemetry* telemetry = nullptr;
    void** table = nullptr;
    std::array<void*, 3> unknown{};
    explicit TelemetryVtableProbe(IDirect3DDevice9* device) {
        if (FAILED(device->QueryInterface(IID_D3D9BridgeTelemetry, reinterpret_cast<void**>(&telemetry))) || !telemetry) return;
        table = *reinterpret_cast<void***>(telemetry);
        std::copy(table, table + unknown.size(), unknown.begin());
    }
    bool Matches() const {
        return !telemetry || (*reinterpret_cast<void***>(telemetry) == table &&
            std::equal(unknown.begin(), unknown.end(), table));
    }
    ~TelemetryVtableProbe() { if (telemetry) telemetry->Release(); }
};

void TestEventStream(DeviceFixture& device, const char* mode, bool correctness)
{
    const bool sealRace = std::strcmp(mode, "sealed-empty") == 0;
#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
    if (sealRace) {
        detail::g_emptyStorageSnapshot = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        detail::g_releaseEmptyStorage = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
#else
    if (sealRace) { Check(false, "sealed-empty regression requires the test-only storage synchronization build"); return; }
#endif
    const bool inactive = std::strcmp(mode, "inactive") == 0;
    const bool high = std::strcmp(mode, "high") == 0;
    const unsigned fault = std::strcmp(mode, "open") == 0 ? 1 : std::strcmp(mode, "write") == 0 ? 2 :
        std::strcmp(mode, "read") == 0 ? 3 : std::strcmp(mode, "queue") == 0 ? 4 : std::strcmp(mode, "handoff") == 0 ? 5 : std::strcmp(mode, "report-open") == 0 ? 6 : std::strcmp(mode, "report-write") == 0 ? 7 : std::strcmp(mode, "cancelled") == 0 ? 8 : 0;
    const bool cap = std::strcmp(mode, "cap") == 0;
    const unsigned boundaryFault = std::strcmp(mode, "start-fail") == 0 ? 1 : std::strcmp(mode, "start-throw") == 0 ? 2 :
        std::strcmp(mode, "stop-fail") == 0 ? 3 : std::strcmp(mode, "stop-throw") == 0 ? 4 : 0;
    TelemetryVtableProbe identity(device.device);
    TelemetryBoundaryImports boundary(device.device, boundaryFault);
    Check(identity.Matches(), "fixture preserves native telemetry vtable and IUnknown compatibility-router identity");
    StorageFaultImports imports;
    g_storageFault = fault; g_storageHandle = g_reportOutputHandle = INVALID_HANDLE_VALUE;
    g_releaseStorageWrite = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_storageReadEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_releaseStorageRead = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool workerFault = std::strcmp(mode, "worker") == 0;
    const bool allocationFault = std::strcmp(mode, "allocation") == 0;
    RuntimeFixture runtime("event-stream");
    runtime.config.cpu = runtime.config.csCpu = runtime.config.context = runtime.config.journal = false;
    runtime.config.timing = {cap ? 120000u : 30000u, 1, 1, 50};
    runtime.Configure();
    RuntimePrepareDevice(device.device);
    ID3D9BridgeTelemetry* telemetry = nullptr;
    const bool supported = SUCCEEDED(device.device->QueryInterface(IID_D3D9BridgeTelemetry,
        reinterpret_cast<void**>(&telemetry))) && telemetry;
    if (telemetry) telemetry->Release();
    if (!supported && !inactive) {
        runtime.Stop();
        CloseHandle(g_releaseStorageWrite); CloseHandle(g_storageReadEntered); CloseHandle(g_releaseStorageRead);
        g_releaseStorageWrite = g_storageReadEntered = g_releaseStorageRead = nullptr;
        g_storageFault = 0;
#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
        if (sealRace) {
            CloseHandle(detail::g_emptyStorageSnapshot); CloseHandle(detail::g_releaseEmptyStorage);
            detail::g_emptyStorageSnapshot = detail::g_releaseEmptyStorage = nullptr;
        }
#endif
        return;
    }
    UINT64 startPresentNs = 0, startCycles = 0, startCycleBegin = 0, startCycleEnd = 0;
    if (!inactive) {
        if (workerFault) g_failOneWorker = true;
        if (allocationFault) { g_failAllocationThread = GetCurrentThreadId(); g_failAllocationSize = 32768 * sizeof(D3D9BridgeTelemetryEvent); }
        QueryThreadCycleTime(GetCurrentThread(), &startCycleBegin);
        const auto startPresentBegin = BenchmarkNowNs();
        g_f8 = true; device.Present(); g_f8 = false;
        startPresentNs = BenchmarkNowNs() - startPresentBegin;
        QueryThreadCycleTime(GetCurrentThread(), &startCycleEnd); startCycles = startCycleEnd - startCycleBegin;
        if (allocationFault) Check(g_failAllocationSize.exchange(0) == 0, "capture event staging allocation fault is exercised");
    }
    // Explicit untimed warmup uses the same actual allocation/map/release path.
    for (unsigned i = 0; i < 32; ++i) {
        IDirect3DTexture9* warm = nullptr;
        if (SUCCEEDED(device.device->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &warm, nullptr)) && warm) { D3DLOCKED_RECT lock{}; if (SUCCEEDED(warm->LockRect(0, &lock, nullptr, 0))) warm->UnlockRect(0); warm->Release(); }
    }
    device.Present();
    UINT64 cyclesBegin = 0, cyclesEnd = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cyclesBegin);
    const auto started = BenchmarkNowNs();
    UINT64 maxPresentNs = 0, successfulPresents = 0, failedPresents = 0;
    const unsigned operations = cap ? 600000 : high ? 80000 : fault == 4 ? 6000 : (fault || boundaryFault || workerFault || allocationFault || cap) ? 256 : 4096;
    bool workOk = true;
    std::uint64_t lateBegin = 0, lateEnd = 0;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(1 + operations / 32);
    auto batchBegin = BenchmarkNowNs();
    for (unsigned i = 0; i <= operations; ++i) {
        const auto begin = BenchmarkNowNs();
        if (i == operations) lateBegin = begin;
        IDirect3DTexture9* texture = nullptr;
        const UINT width = i == operations ? 516 : 128;
        if (SUCCEEDED(device.device->CreateTexture(width, 128, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &texture, nullptr)) && texture) {
            D3DLOCKED_RECT lock{};
            if (SUCCEEDED(texture->LockRect(0, &lock, nullptr, 0))) {
                std::memset(lock.pBits, int(i), width * 4);
                workOk &= SUCCEEDED(texture->UnlockRect(0));
            } else workOk = false;
            texture->Release();
        } else workOk = false;
        if (i == operations) lateEnd = BenchmarkNowNs();
        if (i % 32 == 0 || i == operations) {
            const auto presentBegin = BenchmarkNowNs();
            const auto result = device.Present();
            maxPresentNs = (std::max)(maxPresentNs, BenchmarkNowNs() - presentBegin);
            if (result == S_OK) ++successfulPresents; else ++failedPresents;
            workOk &= result == S_OK;
            latencies.push_back(BenchmarkNowNs() - batchBegin);
            batchBegin = BenchmarkNowNs();
        }
    }
    IDirect3DQuery9* fence = nullptr;
    workOk &= SUCCEEDED(device.device->CreateQuery(D3DQUERYTYPE_EVENT, &fence)) && fence;
    if (fence) {
        workOk &= SUCCEEDED(fence->Issue(D3DISSUE_END));
        const auto drainDeadline = GetTickCount64() + 10000;
        HRESULT ready = S_FALSE;
        while (ready == S_FALSE && GetTickCount64() < drainDeadline) { ready = fence->GetData(nullptr, 0, D3DGETDATA_FLUSH); Sleep(0); }
        workOk &= ready == S_OK; fence->Release();
    }
#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
    if (sealRace) {
        // All earlier real resource events have been published by Present. Hold
        // the actual storage writer after an empty head snapshot, then leave
        // the final real backend resource batch for the producer's last drain.
        detail::g_pauseEmptyStorage = true;
        const bool paused = WaitForSingleObject(detail::g_emptyStorageSnapshot, 5000) == WAIT_OBJECT_0;
        Check(paused, "sealed-empty interleaving pauses actual storage after its empty head snapshot");
        if (!paused) detail::g_pauseEmptyStorage = false;
        lateBegin = BenchmarkNowNs();
        IDirect3DTexture9* finalTexture = nullptr;
        workOk &= SUCCEEDED(device.device->CreateTexture(520, 128, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &finalTexture, nullptr)) && finalTexture;
        if (finalTexture) {
            D3DLOCKED_RECT lock{};
            const auto locked = finalTexture->LockRect(0, &lock, nullptr, 0);
            workOk &= SUCCEEDED(locked);
            if (SUCCEEDED(locked)) { std::memset(lock.pBits, 42, 520 * 4); workOk &= SUCCEEDED(finalTexture->UnlockRect(0)); }
            finalTexture->Release();
        }
        lateEnd = BenchmarkNowNs();
        // Close the real frame interval and publish its resource events while
        // the writer still holds the empty snapshot. Interrupt then seals it.
        workOk &= device.Present() == S_OK;
    }
#endif
    const auto workNs = BenchmarkNowNs() - started;
    QueryThreadCycleTime(GetCurrentThread(), &cyclesEnd);
    SetEvent(g_releaseStorageWrite);
    if (fault == 1 || fault == 2) {
        const auto omissionDeadline = GetTickCount64() + 1000;
        while (!Snapshot().telemetryDropped && GetTickCount64() < omissionDeadline) Sleep(1);
        const auto active = Snapshot();
        Check(!active.telemetryReportFinished && !active.telemetryEvents && active.telemetryDropped > 0,
            "active snapshot counts spooled records and never counts omitted queue acceptance as retained");
    }
    if (fault == 8) {
        g_reportWorkerStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_releaseReportWorker = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_delayNextWorker = true;
    }
    UINT64 stopCyclesBegin = 0, stopCyclesEnd = 0, completionInterruptNs = 0;
    if (!inactive) {
        QueryThreadCycleTime(GetCurrentThread(), &stopCyclesBegin);
        const auto stopBegin = BenchmarkNowNs();
        RuntimeInterrupt(device.device, "event-stream-work-complete");
        completionInterruptNs = BenchmarkNowNs() - stopBegin;
        QueryThreadCycleTime(GetCurrentThread(), &stopCyclesEnd);
    }
#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
    if (sealRace) SetEvent(detail::g_releaseEmptyStorage);
#endif
    if (fault == 8) {
        Check(WaitForSingleObject(g_reportWorkerStarted, 5000) == WAIT_OBJECT_0, "cancelled storage test delays the actual report worker before reader handoff");
        SetEvent(runtime.stopEvent);
        Check(!g_workers.empty() && WaitForSingleObject(g_workers.front(), 5000) == WAIT_OBJECT_0,
            "shutdown closes unclaimed storage before report reader starts");
        SetEvent(g_releaseReportWorker);
    }
    if (fault == 5) {
        Check(WaitForSingleObject(g_storageReadEntered, 5000) == WAIT_OBJECT_0, "shutdown handoff reaches actual report-file read");
        SetEvent(runtime.stopEvent);
        Sleep(5);
        Check(GetFileType(g_storageHandle) != FILE_TYPE_UNKNOWN, "reader-owned temporary file remains open across shutdown");
        SetEvent(g_releaseStorageRead);
    }
    const auto reportBegin = BenchmarkNowNs();
    auto snapshot = Snapshot();
    const auto deadline = GetTickCount64() + (boundaryFault ? 3000 : cap ? 120000 : 30000);
    while (snapshot.workerBusy && GetTickCount64() < deadline) { Sleep(1); snapshot = Snapshot(); }
    const auto reportNs = BenchmarkNowNs() - reportBegin;
    if (boundaryFault && snapshot.workerBusy) {
        Check(false, "backend start/stop failure cannot leave report worker waiting on an unlaunched storage worker");
        std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 87);
    }
    std::ifstream input(snapshot.reportPath);
    std::string line;
    std::uint64_t events = 0, tailIndex = 0, overlaps = 0, backendDropped = 0, hostDropped = 0;
    std::string storageLine, telemetryLine, reportHeader, lastReportLine;
    bool contextSummaryPresent = false;
    bool late = false;
    while (std::getline(input, line)) {
        if (!line.empty()) lastReportLine = line;
        contextSummaryPresent |= line.rfind("contextSummary ", 0) == 0;
        if (line.rfind("# measurement ", 0) == 0) reportHeader = line;
        if (line.rfind("telemetryStorage ", 0) == 0) storageLine = line;
        if (line.rfind("telemetry available=", 0) == 0) {
            telemetryLine = line;
            const auto parse = [&](const char* key) { const auto pos = line.find(key); return pos == std::string::npos ? 0ull : std::strtoull(line.c_str() + pos + std::strlen(key), nullptr, 10); };
            backendDropped = parse("backendDropped="); hostDropped = parse("hostDropped=");
        }
        if (line.rfind("telemetryEvent ", 0) == 0) {
            ++events;
            unsigned long long index=0, id=0, begin=0, end=0;
            unsigned kind=0;
            char kindName[128]{};
            if (std::sscanf(line.c_str(), "telemetryEvent index=%llu captureId=%llu kind=%u kindName=%127s beginNs=%llu endNs=%llu",
                &index, &id, &kind, kindName, &begin, &end) == 6 && begin >= lateBegin && end <= lateEnd) {
                late = true; tailIndex = index;
            }
        }
        if (line.rfind("frameOverlap event=" + std::to_string(tailIndex) + " ", 0) == 0 && late) ++overlaps;
    }
    const auto valueFrom = [](const std::string& row, const char* key) {
        const auto position = row.find(key);
        return position == std::string::npos ? 0ull : std::strtoull(row.c_str() + position + std::strlen(key), nullptr, 0);
    };
    const bool reportRequested = !inactive;
    const bool workersFinished = !snapshot.workerBusy;
    const bool storageMetadataPresent = !storageLine.empty();
    const bool finalizedHeader = !reportHeader.empty() && reportHeader.find("status=pending") == std::string::npos &&
        valueFrom(reportHeader, "captureId=") == snapshot.captureId;
    const bool finalRecord = lastReportLine.rfind("reportEnd ", 0) == 0 &&
        valueFrom(lastReportLine, "captureId=") == snapshot.captureId && valueFrom(lastReportLine, "completed=") == 1;
    // Preserved baseline RuntimeSource predates reportEnd. Its existing final
    // context/error row is accepted explicitly as the legacy format terminator.
    const bool legacyFinalRecord = !storageMetadataPresent && contextSummaryPresent &&
        (lastReportLine.rfind("contextSummary ", 0) == 0 || lastReportLine.rfind("error=", 0) == 0);
    const bool reportComplete = reportRequested && workersFinished && input.is_open() && !input.bad() &&
        finalizedHeader && (finalRecord || legacyFinalRecord) && snapshot.phase != Phase::Error;
    const auto storageErrors = storageMetadataPresent ? valueFrom(storageLine, "errors=") : snapshot.telemetryStorageErrors;
    const auto storageUnread = storageMetadataPresent ? valueFrom(storageLine, "unread=") : snapshot.telemetryUnread;
    const char* storageStatusSource = inactive ? "inactive" : storageMetadataPresent ? "report-footer" :
        snapshot.telemetryReportFinished ? "runtime-snapshot" : "legacy-counters";
    const bool legacyComplete = reportComplete && !backendDropped && !hostDropped &&
        valueFrom(telemetryLine, "available=") && !valueFrom(telemetryLine, "active=") &&
        !valueFrom(telemetryLine, "remaining=") && !valueFrom(telemetryLine, "inFlight=") &&
        !valueFrom(telemetryLine, "inFlightAtStop=") && !valueFrom(telemetryLine, "lateDroppedAtRead=") &&
        telemetryLine.find("error=\"\"") != std::string::npos;
    const bool storageComplete = storageMetadataPresent ? valueFrom(storageLine, "complete=") == 1 : legacyComplete;
    const bool valid = workOk && workersFinished && (!reportRequested || reportComplete);
    Check(workOk && !snapshot.workerBusy, "event-stream real resource work, Present and GPU drain finish");
    if ((fault == 6 || fault == 7) && correctness) {
        Check(snapshot.phase == Phase::Error && !reportComplete,
            "report output open/write failure cannot be reported as successful report completion");
        Check(snapshot.telemetryEvents == 0 && snapshot.telemetryDropped == snapshot.telemetryUnread && snapshot.telemetryUnread > 0,
            "failed report leaves zero finalized events and accounts unfinalized stream as dropped");
    } else if (allocationFault && correctness) {
        Check((snapshot.unavailableMask & RuntimeTelemetryUnavailable) && !snapshot.workerBusy &&
            telemetryLine.find("storage-allocation-failed") != std::string::npos,
            "event staging allocation failure reports unavailable and finishes without worker/file leak");
    } else if (((fault && fault != 5) || cap || boundaryFault || workerFault) && correctness) {
        const auto parse = [&](const char* key) { const auto position = storageLine.find(key); return position == std::string::npos ? 0ull : std::strtoull(storageLine.c_str()+position+std::strlen(key), nullptr, 0); };
        const auto received = parse("received="), written = parse("written="), omitted = parse("omitted=");
        Check(storageLine.find("complete=0") != std::string::npos && (parse("errors=") || boundaryFault) &&
            (snapshot.unavailableMask & RuntimeTelemetryUnavailable), "storage fault remains explicitly incomplete after report generation");
        const auto maskPosition = reportHeader.find("unavailableMask=");
        const auto headerMask = maskPosition == std::string::npos ? 0 : std::strtoul(reportHeader.c_str() + maskPosition + 16, nullptr, 10);
        Check((headerMask & RuntimeTelemetryUnavailable) != 0, "final report header retains storage read/write errors");
        Check(received == written + omitted && written == events + parse("unread="),
            "storage fault conserves received, written, omitted, reported and unread counts");
        Check(snapshot.telemetryEvents == events && snapshot.telemetryEvents + snapshot.telemetryDropped == received + backendDropped,
            "terminal snapshot conserves actual reported events and omitted/unread losses");
        Check(StorageTempFiles() == runtime.initialStorageFiles,
            "storage worker removes its capture temporary file before completion");
    } else if (!inactive && correctness) {
        Check(!high || events > 65536, "event stream retains more than the former lifetime limit");
        Check(late && overlaps > 0 && (!high || tailIndex >= 65536), "known late real event retains its frame association");
        Check(events == snapshot.telemetryEvents && !snapshot.telemetryDropped,
            "event stream report and snapshot conserve retained events without loss");
        if (sealRace) {
            Check(events > 0 && valueFrom(storageLine, "received=") == events &&
                valueFrom(storageLine, "accepted=") == events && valueFrom(storageLine, "written=") == events &&
                valueFrom(storageLine, "reported=") == events,
                "sealed-empty final report physically conserves received accepted written and reported events");
            Check(!storageErrors && !storageUnread && !valueFrom(storageLine, "omitted=") &&
                snapshot.telemetryReportFinished && !snapshot.telemetryUnread && !snapshot.telemetryStorageErrors,
                "sealed-empty storage terminal state has zero omissions unread records and errors");
            // Present can still be executing on CS when this fixture interrupts
            // capture. Its newly recorded chunk span may legitimately cross stop;
            // physical storage conservation must not hide that backend boundary.
            Check(storageComplete == legacyComplete,
                "sealed-empty completeness agrees with actual backend stop and in-flight counters");
            Check(StorageTempFiles() == runtime.initialStorageFiles,
                "sealed-empty terminal storage worker removes its temporary file");
        }
    } else if (inactive) Check(!snapshot.captureId && !snapshot.workerBusy && g_workers.empty(), "inactive F8 starts no event storage worker");
    if (fault == 6 || fault == 7) {
        if (!correctness) Check(valid, "benchmark rejects failed report output");
    } else Check(inactive ? !reportRequested && !reportComplete : reportComplete,
        "report completion requires an opened finalized report and valid terminal row");
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](unsigned n) { return latencies.empty() ? 0ull : latencies[(latencies.size()-1)*n/100]; };
    std::printf("{\"type\":\"event-stream-benchmark\",\"mode\":\"%s\",\"operations\":%u,\"warmupOperations\":32,\"latencyScope\":\"resource-batch-and-Present\",\"operationsPerBatch\":32,\"hostWorkerWrapper\":%s,\"workerSchedulingEnabled\":%s,\"workerSchedulingMmcss\":%s,\"processPriorityClass\":%lu,\"mainThreadCycles\":%llu,\"maxPresentNs\":%llu,\"successfulPresents\":%llu,\"failedPresents\":%llu,\"backendDropped\":%llu,\"hostDropped\":%llu,\"valid\":%s,\"workPresentDrainNs\":%llu,\"reportWaitNs\":%llu,\"startPresentNs\":%llu,\"completionInterruptNs\":%llu,\"startCycles\":%llu,\"stopCycles\":%llu,\"totalMainNs\":%llu,\"totalMainThreadCycles\":%llu,\"p95Ns\":%llu,\"p99Ns\":%llu,\"maxNs\":%llu,\"events\":%llu,\"dropped\":%llu,\"unavailableMask\":%u,\"lateEvent\":%s,\"lateFrameOverlaps\":%llu,\"reportRequested\":%s,\"workersFinished\":%s,\"reportComplete\":%s,\"storageMetadataPresent\":%s,\"storageStatusSource\":\"%s\",\"storageComplete\":%s,\"storageErrors\":%llu,\"storageUnread\":%llu,\"snapshotEvents\":%llu,\"snapshotUnread\":%llu,\"snapshotReportFinished\":%s,\"reportPath\":%s}\n",
        mode, operations+1, g_matchHostWorkerScheduling ? "true" : "false", g_hostWorkerSchedulingOptions.enabled ? "true" : "false", g_hostWorkerSchedulingOptions.mmcss ? "true" : "false", GetPriorityClass(GetCurrentProcess()), cyclesEnd-cyclesBegin, maxPresentNs, successfulPresents, failedPresents, backendDropped, hostDropped, valid ? "true" : "false", workNs, reportNs, startPresentNs, completionInterruptNs, startCycles, stopCyclesEnd-stopCyclesBegin, startPresentNs+workNs+completionInterruptNs, startCycles+(cyclesEnd-cyclesBegin)+(stopCyclesEnd-stopCyclesBegin), percentile(95), percentile(99), percentile(100), events,
        snapshot.telemetryDropped, snapshot.unavailableMask, late ? "true" : "false", overlaps,
        reportRequested ? "true" : "false", workersFinished ? "true" : "false", reportComplete ? "true" : "false",
        storageMetadataPresent ? "true" : "false", storageStatusSource, storageComplete ? "true" : "false", storageErrors, storageUnread,
        snapshot.telemetryEvents, snapshot.telemetryUnread, snapshot.telemetryReportFinished ? "true" : "false", JsonString(snapshot.reportPath).c_str());
    runtime.Stop();
    g_storageFault = 0;
    CloseHandle(g_releaseStorageWrite); g_releaseStorageWrite = nullptr;
    CloseHandle(g_storageReadEntered); CloseHandle(g_releaseStorageRead);
    g_storageReadEntered = g_releaseStorageRead = nullptr;
#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
    if (sealRace) {
        CloseHandle(detail::g_emptyStorageSnapshot); CloseHandle(detail::g_releaseEmptyStorage);
        detail::g_emptyStorageSnapshot = detail::g_releaseEmptyStorage = nullptr;
    }
#endif
}

#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
void TestStorageTerminalAccounting() {
    // The old empty-head exit would also skip events published before abort.
    // The second case simulates an unexpected accepted-record loss and checks
    // that the terminal invariant exposes it instead of inventing omissions.
    for (bool unexplainedLoss : {false, true}) {
        auto storage = std::make_unique<detail::MeasurementEventStorage>();
        storage->ready = false;
        storage->consumed = true;
        detail::g_emptyStorageSnapshot = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        detail::g_releaseEmptyStorage = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        detail::g_pauseEmptyStorage = true;
        const auto before = StorageTempFiles();
        std::thread worker([&] { storage->Run(); });
        Check(WaitForSingleObject(detail::g_emptyStorageSnapshot, 5000) == WAIT_OBJECT_0,
            "terminal accounting holds actual storage at an empty head snapshot");
        D3D9BridgeTelemetryEvent event{};
        event.CaptureId = 42;
        storage->Push(event);
        if (unexplainedLoss) storage->tail.store(storage->head.load());
        else storage->abort.store(true, std::memory_order_release);
        storage->sealed.store(true, std::memory_order_release);
        SetEvent(detail::g_releaseEmptyStorage);
        worker.join();
        if (unexplainedLoss) {
            Check((storage->errors.load() & 256) && storage->received == 1 && storage->accepted == 1 &&
                storage->written == 0 && storage->omitted == 0,
                "terminal invariant flags unaccounted acceptance without duplicating or inventing loss counts");
        } else {
            Check(storage->received == 1 && storage->accepted == 1 && storage->written == 0 &&
                storage->omitted == 1 && storage->reported == 0 && storage->errors == 64,
                "abort observed after empty snapshot drains accepted event exactly once as cancelled omission");
        }
        Check(storage->ready && storage->file == INVALID_HANDLE_VALUE && StorageTempFiles() == before,
            "terminal accounting joins writer and removes its temporary file");
        CloseHandle(detail::g_emptyStorageSnapshot); CloseHandle(detail::g_releaseEmptyStorage);
        detail::g_emptyStorageSnapshot = detail::g_releaseEmptyStorage = nullptr;
    }
}
#endif

#endif
}

int main(int argc, char** argv)
{
#if defined(HAS_MEASUREMENT_RUNTIME)
    const bool eventCorrectness = argc > 1 && std::strcmp(argv[1], "--event-stream-test") == 0;
    const bool eventStream = eventCorrectness || (argc > 1 && std::strcmp(argv[1], "--event-stream-benchmark") == 0);
    const bool benchmark = argc > 1 && std::strcmp(argv[1], "--benchmark") == 0;
    const bool coverageBenchmark = argc > 1 && std::strcmp(argv[1], "--coverage-benchmark") == 0;
    const bool storageOnly = argc > 1 && std::strcmp(argv[1], "--storage-terminal-only") == 0;
    const bool faultOnly = argc > 1 && std::strcmp(argv[1], "--fault-only") == 0;
    const bool csAllocOnly = argc > 1 && std::strcmp(argv[1], "--cs-alloc-only") == 0;
    const bool pendingOnly = argc > 1 && std::strcmp(argv[1], "--pending-stop-only") == 0;
    const bool ownerOnly = argc > 1 && std::strcmp(argv[1], "--device-owner-only") == 0;
    const bool unicodeOnly = argc > 1 && std::strcmp(argv[1], "--unicode-modules-only") == 0;
    g_matchHostWorkerScheduling = (eventStream && !eventCorrectness) || benchmark || coverageBenchmark;
    g_hostWorkerSchedulingOptions = renderstack::scheduling::ReadOptions();
    const char* backend = (eventStream || benchmark || coverageBenchmark || storageOnly || faultOnly || csAllocOnly || pendingOnly || ownerOnly || unicodeOnly) ? (argc > 2 ? argv[2] : nullptr) : (argc > 1 ? argv[1] : nullptr);
    DeviceFixture device(backend, (benchmark || coverageBenchmark) ? 640u : 64u,
        (benchmark || coverageBenchmark) ? 480u : 64u, benchmark || coverageBenchmark);
    Check(device.device != nullptr, "real D3D9 HAL device creates for production runtime fixture");
    if (!benchmark && !coverageBenchmark) TestFrameCorrelation();
    if (device.device && eventStream) {
        TestEventStream(device, argc > 3 ? argv[3] : "high", eventCorrectness);
#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
    } else if (device.device && storageOnly) {
        TestStorageTerminalAccounting();
#endif
    } else if (device.device && csAllocOnly) {
        TestCsFailureIsolation(device, false);
    } else if (device.device && faultOnly) {
        TestAllocationFailureAtCaptureEnd(device, false);
        TestAllocationFailureAtCaptureEnd(device, true);
        TestContextFailureStatus(device);
        TestContextAllocationFailure(device);
        TestCpuJobAllocationFailure(device);
        TestCsFailureIsolation(device, false);
        TestCsFailureIsolation(device, true);
        TestAuditBoundaryException(device, true);
        TestAuditBoundaryException(device, false);
    } else if (device.device && coverageBenchmark) {
        TestCoverageBenchmark(device, argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 4,
            argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 1000,
            argc > 5 && std::strcmp(argv[5], "--aa") == 0);
    } else if (device.device && benchmark) {
        IDirect3DSurface9* backBuffer = nullptr;
        D3DSURFACE_DESC description{};
        const bool fullHudViewport = IsWindowVisible(device.window) &&
            SUCCEEDED(device.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
            backBuffer && SUCCEEDED(backBuffer->GetDesc(&description)) &&
            description.Width == 640 && description.Height == 480;
        if (backBuffer) backBuffer->Release();
        Check(fullHudViewport, "benchmark actual backbuffer is 640x480 and fits the full eight-line HUD");
        if (!fullHudViewport) return 1;
        TestBenchmark(device, argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 4,
            argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 256,
            argc > 5 && std::strcmp(argv[5], "--active") == 0);
    } else if (device.device && pendingOnly) {
        TestAdmittedJournalInterruption(device, false);
    } else if (device.device && ownerOnly) {
        TestSecondaryDeviceCannotClaimCapture(device);
    } else if (device.device && unicodeOnly) {
        TestUnicodeModuleEnumeration(device);
    } else if (device.device) {
#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
        TestEventStream(device, "sealed-empty", true);
        TestStorageTerminalAccounting();
#endif
        TestEventStream(device, "high", true);
        TestEventStream(device, "report-open", true);
        TestEventStream(device, "report-write", true);
        TestEventStream(device, "open", true);
        TestEventStream(device, "write", true);
        TestEventStream(device, "read", true);
        TestEventStream(device, "queue", true);
        TestEventStream(device, "cancelled", true);
        TestEventStream(device, "handoff", true);
        TestEventStream(device, "worker", true);
        TestEventStream(device, "allocation", true);
        TestEventStream(device, "start-fail", true);
        TestEventStream(device, "start-throw", true);
        TestEventStream(device, "stop-fail", true);
        TestEventStream(device, "stop-throw", true);
        TestDisabled(device);
        TestSecondaryDeviceCannotClaimCapture(device);
        TestUnicodeModuleEnumeration(device);
        TestComplete(device);
        TestCoverageCapture(device);
        TestAllocationFailureAtCaptureEnd(device, false);
        TestAllocationFailureAtCaptureEnd(device, true);
        TestContextFailureStatus(device);
        TestContextAllocationFailure(device);
        TestCpuJobAllocationFailure(device);
        TestCsFailureIsolation(device, false);
        TestCsFailureIsolation(device, true);
        TestAuditBoundaryException(device, true);
        TestAuditBoundaryException(device, false);
        TestWorkerFailure(device);
        TestInterruption(device, false);
        TestShutdown(device);
        TestAdmittedJournalInterruption(device, false);
        TestAdmittedJournalInterruption(device, true);
        TestAdmittedJournalInterruption(device, true, true);
        TestInterruption(device, true);
    }
#else
    Check(false, "production measurement runtime is unavailable");
#endif
    std::printf("checks=%u failures=%u\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
