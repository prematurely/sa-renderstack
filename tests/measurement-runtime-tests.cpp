#include "../src/bridge/legacy/EffectInspector.cpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if __has_include("../src/bridge/legacy/BridgeMeasurementRuntime.h")
#include "../src/bridge/legacy/BridgeMeasurementRuntime.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_diagnostics.h"
#define HAS_MEASUREMENT_RUNTIME 1
#endif

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
bool g_delayReportWorker = false;
HANDLE g_reportWorkerStarted = nullptr, g_releaseReportWorker = nullptr;
std::atomic<std::uint32_t> g_presentFrame{0};
std::vector<HANDLE> g_workers;

bool TestKeyDown(int key) { return key == VK_F7 ? g_f7.load() : key == VK_F8 && g_f8.load(); }
bool TestFrame(std::uint32_t& frame) { frame = g_presentFrame.load(std::memory_order_relaxed); return true; }
void TestLog(const char* text) { std::printf("runtime: %s\n", text ? text : "<null>"); }
struct DelayedWorker { LPTHREAD_START_ROUTINE start; void* context; };
DWORD WINAPI StartDelayedWorker(void* context)
{
    std::unique_ptr<DelayedWorker> worker(static_cast<DelayedWorker*>(context));
    SetEvent(g_reportWorkerStarted);
    WaitForSingleObject(g_releaseReportWorker, INFINITE);
    return worker->start(worker->context);
}
HANDLE StartTestWorker(LPTHREAD_START_ROUTINE start, void* context)
{
    if (g_failWorker) return nullptr;
    const bool delay = g_delayReportWorker && g_workers.size() == 1;
    auto* delayed = delay ? new DelayedWorker{start, context} : nullptr;
    HANDLE handle = CreateThread(nullptr, 0, delay ? StartDelayedWorker : start,
        delay ? delayed : context, 0, nullptr);
    if (!handle) delete delayed;
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
        module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
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
        ++g_presentFrame;
        RuntimePresent(device, result);
        return result;
    }
};

struct RuntimeFixture {
    HANDLE stopEvent = nullptr;
    std::string directory;
    RuntimeConfig config{};
    bool stopped = false;

    explicit RuntimeFixture(const char* caseName)
    {
        RuntimeShutdown();
        g_f7 = g_f8 = g_failWorker = false;
        g_delayReportWorker = false;
        g_workers.clear();
        g_presentFrame = 0;
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
    bool observerStagesSeparated = g_workers.empty();
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (snapshot.phase != Phase::Complete && snapshot.phase != Phase::Error &&
        GetTickCount64() < deadline) {
        if (snapshot.phase == Phase::Baseline || snapshot.phase == Phase::Cpu ||
            snapshot.phase == Phase::CpuFinishing) {
            observerStagesSeparated &= !g_properShadersStateAttributionActive.load(std::memory_order_acquire);
            if (snapshot.phase == Phase::Baseline) observerStagesSeparated &= g_workers.empty();
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
    g_f8 = true;
    const bool sampling = WaitPhase(device, Phase::Cpu);
    const auto before = Snapshot();
    Check(sampling && before.workerBusy, "lifecycle fixture reaches an in-flight sampling worker");
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

bool BenchmarkWorkload(DeviceFixture& device, HRESULT& presented)
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
    presented = device.Present();
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
#endif
}

int main(int argc, char** argv)
{
#if defined(HAS_MEASUREMENT_RUNTIME)
    const bool benchmark = argc > 1 && std::strcmp(argv[1], "--benchmark") == 0;
    const bool pendingOnly = argc > 1 && std::strcmp(argv[1], "--pending-stop-only") == 0;
    const char* backend = (benchmark || pendingOnly) ? (argc > 2 ? argv[2] : nullptr) : (argc > 1 ? argv[1] : nullptr);
    DeviceFixture device(backend, benchmark ? 640u : 64u, benchmark ? 480u : 64u, benchmark);
    Check(device.device != nullptr, "real D3D9 HAL device creates for production runtime fixture");
    if (device.device && benchmark) {
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
    } else if (device.device) {
        TestDisabled(device);
        TestComplete(device);
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
