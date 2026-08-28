#pragma once

// Read-only reconnaissance for the "replace D3DX Effect" work.
//
// Goal: decide whether direct constant submission can be generalised beyond the
// four hand-verified LitPrelight techniques. To answer that we need, for every
// effect ProperShaders creates:
//   - its techniques and passes,
//   - every effect parameter (name / semantic / class / type / registers),
//   - the actual shader constant table (CTAB) of each pass's VS and PS, which
//     is what maps a name to a hardware constant register.
//
// If parameter -> register layouts turn out to be regular, a generic submitter
// can be built and D3DX can be bypassed for all techniques. If they are ad hoc,
// the per-shader approach used today is the only safe one and we stop here.
//
// This header only reads. It never patches, never changes device state, and is
// invoked once per effect right after creation.

#include <windows.h>
#include <d3d9.h>
#include <d3dx9effect.h>

#include <vector>

// Writes a full report for one effect to <gameDir>\scripts\BridgeD3D9.effectinspect.log.
// `effect` is the raw ID3DXEffect ProperShaders created. Safe to call with
// anything: every failure path just logs and returns.
void InspectProperShadersEffect(void* effect, unsigned long long effectBytes,
    const char* gameDir);

// Enabled by [ProperShadersEffectOptimization] InspectEffects in BridgeD3D9.ini.
extern bool g_properShadersInspectEffects;

// Dry run for the generic direct submitter (stage ①-3). For every technique it
// builds the parameter -> shader-register plan exactly the way the live
// submitter would (joining effect parameters against both CTABs, flattening
// structs, honouring RegisterCount), then logs the plan and a per-technique
// verdict (DIRECT-OK or FALLBACK+reason) without touching rendering.
// Enabled by [ProperShadersEffectOptimization] GenericDirectDryRun.
void BuildGenericDirectPlanDryRun(void* effect, const char* gameDir);
extern bool g_properShadersGenericDirectDryRun;

// ---- Stage ①-3a: real submission plans -------------------------------------
// Built by the same CTAB join the dry run validated (51 DIRECT-OK / 3
// FALLBACK, all fallbacks multi-pass post FX). The live submitter reads these;
// nothing consumes them until the hot-path hooks are wired (stage ①-3b).

struct GenericDirectSlot
{
    D3DXHANDLE param = nullptr; // effect parameter handle (top-level)
    bool vs = false;            // destination stage
    UINT base = 0;              // first float4 register
    UINT count = 0;             // registers to write (CTAB may truncate matrices)
    bool matrix = false;        // parameter is a matrix
    bool columns = false;       // CTAB says column-major -> transpose on upload
};

struct GenericDirectSampler
{
    D3DXHANDLE param = nullptr;
    bool vs = false;
    UINT index = 0;             // s# register
};

struct GenericDirectTechniquePlan
{
    D3DXHANDLE technique = nullptr;
    const char* name = nullptr; // points into D3DX-owned technique desc string
    bool ok = false;            // false -> stay on the D3DX path
    // Pass-lite eligibility: single pass, every slot <=4 registers (excludes
    // skinned techniques whose bone arrays flow through un-hooked
    // SetMatrixArray), no vertex-stage samplers.
    bool liteEligible = false;
    // 0 = need recording #1, 1 = need recording #2, 2 = validated (replay
    // mode), 3 = disabled (recordings diverged or a recording failed).
    mutable unsigned char liteState = 0;
    mutable std::vector<unsigned char> liteRec1;
    mutable std::vector<unsigned char> liteRec2;
    // Per-stage texture resolution learned from the recordings: FX sampler
    // names (CTAB) and the texture parameters the game actually sets are
    // different objects, so the mapping is discovered by pointer-matching the
    // recorded bindings against the texture shadow at validation time.
    // 0 = stage untouched, 1 = dynamic (texture param), 2 = static pointer.
    mutable unsigned char liteStageMode[20]{};
    mutable D3DXHANDLE liteStageParam[20]{};
    mutable IDirect3DBaseTexture9* liteStageStatic[20]{};
    // ---- Trace-stability probe (observation only; never replays) -----------
    // When g_properShadersTraceStabilityProbe is on, every recorded pass is
    // diffed slot-by-slot against this technique's FIRST recording (stabBase),
    // and per-op-slot "did it change" counts accumulate in stabChanged. This
    // answers the question PassLite could not: WHICH slots are invariant vs
    // per-object, instead of a single binary agree/diverge verdict. The probe
    // short-circuits the lite state machine before any replay, so rendering is
    // untouched. Recording stops after kStabMaxSamples samples per technique.
    static const unsigned kStabMaxSlots = 512;   // op slots tracked per technique
    static const unsigned kStabMaxSamples = 48;  // recordings compared per technique
    mutable std::vector<unsigned char> stabBase;  // first recording (baseline)
    mutable unsigned stabSamples = 0;             // recordings observed so far
    mutable bool stabFull = false;                // reached kStabMaxSamples
    mutable unsigned stabChanged[kStabMaxSlots]{};// per-slot change counters

    unsigned slotCount = 0;
    unsigned samplerCount = 0;
    GenericDirectSlot slots[96];
    GenericDirectSampler samplers[16];
};

// Builds and stores plans for every technique of `effect`. Returns the number
// of DIRECT-OK techniques stored (-1 if building was skipped entirely).
int BuildGenericDirectPlans(void* effect, const char* gameDir);
extern bool g_properShadersGenericDirect;

// Fast lookup for the hot path. Returns null if the technique has no plan or
// its verdict was FALLBACK.
const GenericDirectTechniquePlan* FindGenericDirectPlan(
    void* effect, D3DXHANDLE technique);

// Frees plans for a released effect (call from the effect-release hook).
void DropGenericDirectPlans(void* effect);

// ---- Stage ①-3b-ii: shadow store + direct submission -----------------------
// SetXXX values for plan-covered parameters are written here instead of into
// D3DX, then submitted through the journal's constant interface (so they are
// recorded and restored exactly like D3DX writes) and replayed after every
// BeginPass to overwrite the stale values D3DX applies from its own store.

class ProperShadersStateJournal;

// Handles one SetMatrix/SetVector/SetFloatArray call. Returns true when the
// parameter is covered by `plan` (value stored in the effect's shadow and, if
// `journal` is in an active transaction, written to the device through it) —
// the caller must then SKIP the D3DX original. Returns false when the caller
// must forward to D3DX (param not in plan / no plan / no shadow capacity).
// `asMatrix` selects the 16-float matrix layout (transpose per slot).
bool GenericDirectHandleSet(void* effect, const GenericDirectTechniquePlan* plan,
    ProperShadersStateJournal* journal, D3DXHANDLE param,
    const float* data, unsigned floatCount, bool asMatrix);

// Re-submits every seen shadow value of `plan` through the journal. Call right
// after the original BeginPass returns.
void GenericDirectReplaySeen(void* effect, const GenericDirectTechniquePlan* plan,
    ProperShadersStateJournal* journal);

// ---- Stage B: pass-lite (full D3DX pass-machine bypass) --------------------
// The journal is the effect's state manager, so every state write D3DX makes
// during Begin/BeginPass flows through it. Pass-lite records that sequence
// twice per technique, validates that the two recordings agree (masking
// per-object textures and plan-covered constants), then replays the recording
// directly — the effect's Begin/BeginPass/EndPass/End never run again.

extern bool g_properShadersGenericPassLite;

// ---- Trace-stability probe (observation only) ------------------------------
// Enabled by [ProperShadersEffectOptimization] TraceStabilityProbe. When on it
// takes precedence over pass-lite: recordings are analysed for per-slot
// stability and the pass is NEVER replayed (rendering untouched). Results are
// written periodically to scripts\BridgeD3D9.tracestab.log.
extern bool g_properShadersTraceStabilityProbe;

// Dumps the accumulated per-technique stability histogram. Safe to call any
// time; writes nothing when no probe data exists. Called on a wall-clock
// throttle from inside the recorder, so no explicit driver is required.
void TraceStabilityDumpAll();

// 0 = run the normal D3DX path, 1 = record this transaction, 2 = replay mode.
int GenericDirectPassAction(const GenericDirectTechniquePlan* plan);

// Swaps the effect's state manager to the recording tap. Returns false if the
// recording could not start (caller proceeds on the normal path).
bool GenericDirectBeginRecording(void* effect,
    const GenericDirectTechniquePlan* plan, ProperShadersStateJournal* journal);

// Restores the journal as state manager and advances the recording state
// machine (`ok` false marks the technique disabled).
void GenericDirectEndRecording(void* effect,
    const GenericDirectTechniquePlan* plan, ProperShadersStateJournal* journal,
    bool ok);

// Clears the shared pass recorder if it still references `journal`. Must be
// called before that journal is released (effect teardown): EndRecording keeps
// the recorder's inner pointer alive on purpose so late D3DX callbacks forward
// transparently, and this is the only point where that pointer can dangle.
void ReleaseRecorderForJournal(ProperShadersStateJournal* journal);

// Replays the validated recording + dynamic constants (shadow) + dynamic
// textures (texture shadow) through the journal.
void GenericDirectApplyPassLite(void* effect,
    const GenericDirectTechniquePlan* plan, ProperShadersStateJournal* journal);

// Records the last texture set for a texture parameter (from the SetTexture
// vtable hook). Pointers are not AddRef'd: the game owns them frame-long.
void GenericDirectShadowTexture(void* effect, D3DXHANDLE param,
    IDirect3DBaseTexture9* texture);
