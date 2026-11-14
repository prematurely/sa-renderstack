#pragma once

#include <d3d9.h>

// Private, optional, copied-data telemetry. This interface does not alter
// API1-7 or the diagnostics/lifecycle vtables. All timestamps use the same
// QPC-derived nanosecond clock as BridgeMeasurementRuntime::NowNs().
inline constexpr GUID IID_D3D9BridgeTelemetry =
  {0x5722e0c7,0xa495,0x4e93,{0xb7,0xcd,0x13,0x87,0x8c,0xb2,0x94,0x31}};
inline constexpr UINT D3D9_BRIDGE_TELEMETRY_VERSION = 1;
inline constexpr UINT D3D9_BRIDGE_TELEMETRY_MAX_EVENTS = 65536;
inline constexpr UINT D3D9_BRIDGE_TELEMETRY_MAX_CHUNKS = 4096;
inline constexpr UINT D3D9_BRIDGE_TELEMETRY_MAX_MASKS = 65536;

enum D3D9BridgeTelemetryKind : UINT {
  D3D9_BRIDGE_TELEMETRY_ALLOC = 1,
  D3D9_BRIDGE_TELEMETRY_FREE,
  D3D9_BRIDGE_TELEMETRY_MAP,
  D3D9_BRIDGE_TELEMETRY_UNMAP,
  D3D9_BRIDGE_TELEMETRY_CS_WAIT,
  D3D9_BRIDGE_TELEMETRY_RESOURCE_WAIT,
  D3D9_BRIDGE_TELEMETRY_GPU_WAIT,
  // CPU waiting for submit/present status, not GPU completion.
  D3D9_BRIDGE_TELEMETRY_SUBMISSION_WAIT,
  // Signal was observed incomplete before the frame-latency wait call.
  // It may become ready before the calling thread actually blocks.
  D3D9_BRIDGE_TELEMETRY_FRAME_LATENCY_WAIT,
  // Inclusive acquireNextImage call, including CPU, driver and any waits.
  D3D9_BRIDGE_TELEMETRY_PRESENT_ACQUIRE,
  // Sampler-pool pressure: waiting for a submission fence observed incomplete.
  D3D9_BRIDGE_TELEMETRY_SAMPLER_POOL_WAIT,
  // Enqueue-to-completion latency of unreachable file mappings, not CPU time.
  D3D9_BRIDGE_TELEMETRY_MAPPING_RETIRE,
  D3D9_BRIDGE_TELEMETRY_CS_CHUNK,
  D3D9_BRIDGE_TELEMETRY_COMMAND_ALLOCATE,
  D3D9_BRIDGE_TELEMETRY_COMMAND_BEGIN,
  D3D9_BRIDGE_TELEMETRY_COMMAND_RESET,
};
enum D3D9BridgeTelemetryFlags : UINT {
  D3D9_BRIDGE_TELEMETRY_FILE_BACKED = 1u << 0,
  D3D9_BRIDGE_TELEMETRY_FAILED = 1u << 1,
  D3D9_BRIDGE_TELEMETRY_RESOURCE = 1u << 2,
  D3D9_BRIDGE_TELEMETRY_DEVICE_MEMORY = 1u << 3,
  D3D9_BRIDGE_TELEMETRY_OS_MAPPING = 1u << 4,
  // GPU_WAIT includes queue draining plus vkDeviceWaitIdle.
  D3D9_BRIDGE_TELEMETRY_DEVICE_IDLE = 1u << 5,
  D3D9_BRIDGE_TELEMETRY_CACHE_HIT = 1u << 6,
  D3D9_BRIDGE_TELEMETRY_CACHE_RETURN = 1u << 7,
  D3D9_BRIDGE_TELEMETRY_CACHE_FILL = 1u << 8,
  D3D9_BRIDGE_TELEMETRY_CACHE_EVICT = 1u << 9,
  D3D9_BRIDGE_TELEMETRY_DEFERRED = 1u << 10,
  D3D9_BRIDGE_TELEMETRY_HIGH_PRIORITY = 1u << 11,
  D3D9_BRIDGE_TELEMETRY_SECONDARY = 1u << 12,
};
enum D3D9BridgeMemoryFlags : UINT {
  D3D9_BRIDGE_MEMORY_CHUNKS_TRUNCATED = 1u << 0,
  D3D9_BRIDGE_MEMORY_MASKS_TRUNCATED = 1u << 1,
};

struct D3D9BridgeTelemetryConfig {
  UINT StructSize, Version;
  UINT64 CaptureId;
  UINT MaxEvents, Reserved;
  // Successful spans are filtered only if BOTH duration and bytes are below
  // these thresholds. Zero thresholds retain all completed spans. Failures
  // remain eligible regardless of size and duration.
  UINT64 MinDurationNs, MinBytes;
};
struct D3D9BridgeTelemetryEvent {
  UINT64 CaptureId, BeginNs, EndNs, Identity, Bytes;
  UINT Kind, ThreadId;
  INT Result;
  UINT Flags;
};
struct D3D9BridgeTelemetryRead {
  UINT StructSize, Version;
  UINT64 CaptureId, Dropped, Filtered;
  // Events are completed spans only. InFlightAtStop records spans still open
  // at the stop boundary; they cannot be interpreted as completed timings.
  UINT64 Started, InFlight, InFlightAtStop, LateDropped;
  UINT Written, Remaining, Capacity, Active;
};
struct D3D9BridgeMemoryChunk {
  UINT64 Cookie, Capacity, Used;
  UINT Heap, Type, Mapped, Active;
  // Offset/count refer to the caller's mask buffer. If the entire mask for a
  // chunk cannot fit, MaskCount is zero and MASKS_TRUNCATED is set.
  UINT PageCount, MaskOffset, MaskCount, Reserved;
};
struct D3D9BridgeMemorySnapshot {
  UINT StructSize, Version;
  // Timestamp is taken under the allocator lock. ScanDurationNs includes
  // time spent waiting for that lock. Totals describe HUD-visible pooled
  // chunks, not dedicated allocations.
  UINT64 CaptureId, TimestampNs, ScanDurationNs, TotalCapacity, TotalUsed;
  UINT TotalChunks, CopiedChunks, TotalMasks, CopiedMasks, Flags, Reserved;
};

// Calls may run concurrently. Capture IDs must increase strictly over the
// device lifetime, so an old reader cannot consume a later generation.
// Start creates a new generation only when no
// capture is active. Stop is idempotent for that generation; completed data
// remains readable until the next Start. A stale/mismatched ID never consumes
// events of the current generation. Late operation completions from older
// generations are discarded. ReadEvents consumes copied entries in order;
// pass capacity zero and a null array to inspect counters without consuming.
// GetMemorySnapshot succeeds only for the active capture; its arrays and all
// returned data are caller owned. No recorder lock spans device operations,
// waits, or allocator lock acquisition.
struct ID3D9BridgeTelemetry : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE StartCapture(const D3D9BridgeTelemetryConfig*) = 0;
  virtual HRESULT STDMETHODCALLTYPE StopCapture(UINT64 captureId) = 0;
  virtual HRESULT STDMETHODCALLTYPE ReadEvents(UINT64 captureId, D3D9BridgeTelemetryRead*,
    D3D9BridgeTelemetryEvent*, UINT capacity) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMemorySnapshot(UINT64 captureId, D3D9BridgeMemorySnapshot*,
    D3D9BridgeMemoryChunk*, UINT chunkCapacity, UINT* masks, UINT maskCapacity) = 0;
};
