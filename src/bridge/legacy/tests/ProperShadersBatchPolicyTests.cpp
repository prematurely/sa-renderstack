#include "../ProperShadersBatchPolicy.h"
#include "../ProperShadersEffectBindingCache.h"
#include "../ProperShadersPatchValidation.h"
#include "../GtaSaCompatApiVersions.h"

#include <cstdint>
#include <cstdio>

namespace
{
bool Check(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

struct TestEffectBinding
{
    void* effect = nullptr;
};
}

int main()
{
    using ProperShadersBatching::BatchRejectReason;
    using ProperShadersBatching::BatchStartFacts;
    using ProperShadersBatching::EvaluateBatchStart;

    bool ok = true;

    int effectA = 0;
    int effectB = 0;
    TestEffectBinding bindingA{ &effectA };
    TestEffectBinding bindingB{ &effectB };
    ProperShadersBindingLookup::LastHitCache<TestEffectBinding> bindingCache;

    ok &= Check(
        bindingCache.Find(&effectA) == nullptr,
        "empty effect binding last-hit cache misses");
    bindingCache.Remember(&bindingA);
    ok &= Check(
        bindingCache.Find(&effectA) == &bindingA,
        "effect binding last-hit cache returns the remembered binding");
    ok &= Check(
        bindingCache.Find(&effectB) == nullptr,
        "effect binding last-hit cache does not alias another effect");
    bindingCache.Remember(&bindingB);
    bindingCache.Forget(&bindingA);
    ok &= Check(
        bindingCache.Find(&effectB) == &bindingB,
        "forgetting a non-current binding preserves the current hit");
    bindingCache.Forget(&bindingB);
    ok &= Check(
        bindingCache.Find(&effectB) == nullptr,
        "forgetting the current binding clears the last hit");
    bindingCache.Remember(&bindingA);
    bindingA.effect = nullptr;
    ok &= Check(
        bindingCache.Find(&effectA) == nullptr,
        "a released tombstoned binding cannot remain a last-hit match");

    ok &= Check(
        GtaSaCompatApiVersions::kVulkanPass == 2u,
        "Vulkan pass payload keeps its API v2 compatibility floor");
    ok &= Check(
        GtaSaCompatApiVersions::kStateBatch == 3u,
        "state batch payload uses the API v3 contract");
    ok &= Check(
        GtaSaCompatApiVersions::kStateJournal == 4u,
        "state journal capability starts at API v4");
    ok &= Check(
        GtaSaCompatApiVersions::kEffectStateBatch == 5u,
        "effect state batch payload uses the API v5 contract");
    ok &= Check(
        GtaSaCompatApiVersions::kStateDrawBatch == 6u,
        "state draw batch payload uses the API v6 contract");
    ok &= Check(
        GtaSaCompatApiVersions::Supports(
            3u, GtaSaCompatApiVersions::kStateBatch),
        "API v3 backend accepts a state batch payload");
    ok &= Check(
        !GtaSaCompatApiVersions::Supports(
            2u, GtaSaCompatApiVersions::kStateBatch),
        "API v2 backend rejects a state batch payload");

    BatchStartFacts facts{};
    facts.batchingEnabled = true;
    facts.noSaveStateActive = true;
    facts.techniqueKnown = true;
    facts.beginSucceeded = true;
    facts.passCountAvailable = true;
    facts.passCount = 1;
    facts.bindingAvailable = true;
    facts.journalActive = true;

    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::None,
        "eligible single-pass effect starts a batch");

    facts.batchingEnabled = false;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::BatchingDisabled,
        "disabled batching rejects a batch start");
    facts.batchingEnabled = true;

    facts.noSaveStateActive = false;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::NoSaveStateInactive,
        "inactive no-save-state path rejects batching");
    facts.noSaveStateActive = true;

    facts.techniqueKnown = false;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::MissingTechnique,
        "missing technique rejects batching");
    facts.techniqueKnown = true;

    facts.beginSucceeded = false;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::BeginFailed,
        "failed Begin rejects batching");
    facts.beginSucceeded = true;

    facts.passCountAvailable = false;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::MissingPassCount,
        "missing pass count rejects batching");
    facts.passCountAvailable = true;

    facts.passCount = 2;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::MultiPass,
        "multipass effect rejects batching");
    facts.passCount = 1;

    facts.bindingAvailable = false;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::MissingBinding,
        "missing effect binding rejects batching");
    facts.bindingAvailable = true;

    facts.journalActive = false;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::JournalInactive,
        "inactive journal rejects batching");
    facts.journalActive = true;

    facts.journalDisabled = true;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::JournalDisabled,
        "disabled journal rejects batching");

    facts.journalActive = false;
    ok &= Check(
        EvaluateBatchStart(facts) == BatchRejectReason::JournalDisabled,
        "disabled journal takes precedence over inactive journal");

    const std::uint8_t expectedBytes[6] = {
        0xFF, 0x90, 0xFC, 0x00, 0x00, 0x00,
    };
    std::uint8_t matchingBytes[6] = {
        0xFF, 0x90, 0xFC, 0x00, 0x00, 0x00,
    };
    std::uint8_t changedBytes[6] = {
        0xE8, 0x00, 0x00, 0x00, 0x00, 0x90,
    };

    ok &= Check(
        ProperShadersPatching::BytesMatch(
            matchingBytes, expectedBytes, sizeof(expectedBytes)),
        "matching call-site bytes validate");
    ok &= Check(
        !ProperShadersPatching::BytesMatch(
            changedBytes, expectedBytes, sizeof(expectedBytes)),
        "changed call-site bytes are rejected");
    ok &= Check(
        ProperShadersPatching::BytesMatch(nullptr, nullptr, 0),
        "zero-byte comparison succeeds without dereferencing pointers");
    ok &= Check(
        !ProperShadersPatching::BytesMatch(nullptr, expectedBytes, 1),
        "non-empty comparison rejects a null actual pointer");

    const ProperShadersPatching::PatchBytes mismatchingPatches[] = {
        { matchingBytes, expectedBytes, sizeof(expectedBytes) },
        { changedBytes, expectedBytes, sizeof(expectedBytes) },
    };
    ok &= Check(
        ProperShadersPatching::FindFirstMismatch(
            mismatchingPatches, 2) == 1,
        "preflight reports the first mismatching patch before writes");

    const ProperShadersPatching::PatchBytes matchingPatches[] = {
        { matchingBytes, expectedBytes, sizeof(expectedBytes) },
        { matchingBytes, expectedBytes, sizeof(expectedBytes) },
    };
    ok &= Check(
        ProperShadersPatching::FindFirstMismatch(matchingPatches, 2) == 2,
        "preflight returns the patch count when every patch matches");
    ok &= Check(
        ProperShadersPatching::FindFirstMismatch(nullptr, 0) == 0,
        "empty preflight succeeds without a patch array");

    if (!ok) return 1;
    std::puts("PASS: ProperShaders batch policy");
    return 0;
}
