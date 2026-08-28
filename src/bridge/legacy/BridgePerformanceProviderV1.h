#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace BridgePerformanceProviderV1
{
constexpr std::uint32_t ApiVersion = 1;
constexpr std::size_t MetricNameCapacity = 48;
constexpr std::size_t MaxMetrics = 32;

enum class MetricKind : std::uint32_t
{
    Counter = 1,
    Microseconds = 2,
};

#pragma pack(push, 8)
struct Query
{
    std::uint32_t size = sizeof(Query);
    std::uint32_t apiVersion = ApiVersion;
    std::uint32_t captureId = 0;
    std::uint32_t elapsedMs = 0;
    std::uint64_t frameCount = 0;
    double fps = 0.0;
};

struct Metric
{
    char name[MetricNameCapacity]{};
    std::uint32_t kind = 0;
    std::uint32_t reserved = 0;
    std::uint64_t value = 0;
};

struct Snapshot
{
    std::uint32_t size = sizeof(Snapshot);
    std::uint32_t apiVersion = ApiVersion;
    std::uint32_t metricCount = 0;
    std::uint32_t flags = 0;
    Metric metrics[MaxMetrics]{};
};
#pragma pack(pop)

static_assert(sizeof(Query) == 32, "Provider V1 Query layout changed");
static_assert(offsetof(Query, frameCount) == 16, "Provider V1 Query offset changed");
static_assert(offsetof(Query, fps) == 24, "Provider V1 Query offset changed");
static_assert(sizeof(Metric) == 64, "Provider V1 Metric layout changed");
static_assert(offsetof(Metric, value) == 56, "Provider V1 Metric offset changed");
static_assert(sizeof(Snapshot) == 16 + MaxMetrics * sizeof(Metric),
    "Provider V1 Snapshot layout changed");

using QueryFunction = BOOL (__stdcall*)(const Query*, Snapshot*);

enum class ValidationCode : std::uint32_t
{
    Valid = 0,
    SnapshotSize,
    SnapshotVersion,
    MetricCount,
    EmptyMetricName,
    UnterminatedMetricName,
    UnknownMetricKind,
    NonzeroReserved,
};

struct ValidationResult
{
    ValidationCode code = ValidationCode::Valid;
    std::uint32_t metricIndex = 0;
};

ValidationResult ValidateSnapshot(const Snapshot& snapshot) noexcept;
const char* ValidationCodeName(ValidationCode code) noexcept;

struct ProviderHealth
{
    bool quarantined = false;
};

enum class CallStatus : std::uint32_t
{
    Accepted = 0,
    SkippedQuarantined,
    InvalidFunction,
    InvalidQuery,
    ProviderReturnedFalse,
    ProviderException,
    SlowProvider,
    InvalidSnapshot,
};

struct CallResult
{
    CallStatus status = CallStatus::InvalidFunction;
    ValidationResult validation{};
    std::uint64_t elapsedUs = 0;
    Snapshot snapshot{};
    bool called = false;
};

CallResult Invoke(
    QueryFunction function,
    const Query& query,
    std::uint32_t slowWarningUs,
    ProviderHealth& health) noexcept;
}
