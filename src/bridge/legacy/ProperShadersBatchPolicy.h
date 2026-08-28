#pragma once

#include <cstdint>

namespace ProperShadersBatching
{
enum class BatchRejectReason : std::uint8_t
{
    None,
    BatchingDisabled,
    NoSaveStateInactive,
    MissingTechnique,
    BeginFailed,
    MissingPassCount,
    MultiPass,
    MissingBinding,
    JournalInactive,
    JournalDisabled,
    Count,
};

struct BatchStartFacts
{
    bool batchingEnabled{};
    bool noSaveStateActive{};
    bool techniqueKnown{};
    bool beginSucceeded{};
    bool passCountAvailable{};
    std::uint32_t passCount{};
    bool bindingAvailable{};
    bool journalActive{};
    bool journalDisabled{};
};

constexpr BatchRejectReason EvaluateBatchStart(const BatchStartFacts& facts)
{
    if (!facts.batchingEnabled) return BatchRejectReason::BatchingDisabled;
    if (!facts.noSaveStateActive) return BatchRejectReason::NoSaveStateInactive;
    if (!facts.techniqueKnown) return BatchRejectReason::MissingTechnique;
    if (!facts.beginSucceeded) return BatchRejectReason::BeginFailed;
    if (!facts.passCountAvailable) return BatchRejectReason::MissingPassCount;
    if (facts.passCount != 1) return BatchRejectReason::MultiPass;
    if (!facts.bindingAvailable) return BatchRejectReason::MissingBinding;
    if (facts.journalDisabled) return BatchRejectReason::JournalDisabled;
    if (!facts.journalActive) return BatchRejectReason::JournalInactive;
    return BatchRejectReason::None;
}
}
