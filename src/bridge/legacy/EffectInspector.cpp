#include "EffectInspector.h"
#include "ProperShadersStateJournal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <span>
#include <atomic>
#include <vector>

bool g_properShadersInspectEffects = false;
bool g_properShadersGenericDirectDryRun = false;
bool g_properShadersGenericDirect = false;
std::atomic<bool> g_properShadersStateAttributionActive{ false };

namespace {

FILE* g_report = nullptr;
int g_effectIndex = 0;

// Truncating, null-terminated format into a fixed buffer: byte-identical to
// snprintf for the same format string, but checked at compile time.
template <class... Args>
void FormatTo(char* output, size_t outputSize,
    std::format_string<Args...> fmt, Args&&... args)
{
    if (!output || !outputSize) return;
    const std::string text = std::vformat(
        fmt.get(), std::make_format_args(args...));
    const size_t count = text.size() < outputSize
        ? text.size()
        : outputSize - 1;
    std::memcpy(output, text.data(), count);
    output[count] = '\0';
}

template <class... Args>
void Report(std::format_string<Args...> fmt, Args&&... args)
{
    if (!g_report) return;
    std::vprint_nonunicode(g_report, fmt.get(), std::make_format_args(args...));
    fputc('\n', g_report);
    fflush(g_report);
}

const char* ClassName(D3DXPARAMETER_CLASS c)
{
    switch (c) {
    case D3DXPC_SCALAR:        return "scalar";
    case D3DXPC_VECTOR:        return "vector";
    case D3DXPC_MATRIX_ROWS:   return "matrix_rows";
    case D3DXPC_MATRIX_COLUMNS:return "matrix_cols";
    case D3DXPC_OBJECT:        return "object";
    case D3DXPC_STRUCT:        return "struct";
    default:                   return "?";
    }
}

const char* TypeName(D3DXPARAMETER_TYPE t)
{
    switch (t) {
    case D3DXPT_VOID:        return "void";
    case D3DXPT_BOOL:        return "bool";
    case D3DXPT_INT:         return "int";
    case D3DXPT_FLOAT:       return "float";
    case D3DXPT_STRING:      return "string";
    case D3DXPT_TEXTURE:     return "texture";
    case D3DXPT_TEXTURE1D:   return "texture1D";
    case D3DXPT_TEXTURE2D:   return "texture2D";
    case D3DXPT_TEXTURE3D:   return "texture3D";
    case D3DXPT_TEXTURECUBE: return "textureCube";
    case D3DXPT_SAMPLER:     return "sampler";
    case D3DXPT_SAMPLER1D:   return "sampler1D";
    case D3DXPT_SAMPLER2D:   return "sampler2D";
    case D3DXPT_SAMPLER3D:   return "sampler3D";
    case D3DXPT_SAMPLERCUBE: return "samplerCube";
    case D3DXPT_PIXELSHADER: return "pixelshader";
    case D3DXPT_VERTEXSHADER:return "vertexshader";
    default:                 return "?";
    }
}

const char* RegSetName(D3DXREGISTER_SET s)
{
    switch (s) {
    case D3DXRS_BOOL:     return "b";
    case D3DXRS_INT4:     return "i";
    case D3DXRS_FLOAT4:   return "c";
    case D3DXRS_SAMPLER:  return "s";
    default:              return "?";
    }
}

// The bridge does not import d3dx9_43.dll statically; resolve at first use.
// d3dx9_43 is guaranteed to be loaded here because ProperShaders created the
// effect we are inspecting with it.
using D3DXGetShaderConstantTableFn = HRESULT(WINAPI*)(
    const DWORD*, ID3DXConstantTable**);

D3DXGetShaderConstantTableFn ResolveGetShaderConstantTable()
{
    static D3DXGetShaderConstantTableFn fn = []() -> D3DXGetShaderConstantTableFn {
        HMODULE module = GetModuleHandleA("d3dx9_43.dll");
        if (!module) return nullptr;
        return reinterpret_cast<D3DXGetShaderConstantTableFn>(
            GetProcAddress(module, "D3DXGetShaderConstantTable"));
    }();
    return fn;
}

// Dumps one shader's CTAB: the authoritative name -> register mapping baked
// into the bytecode by the HLSL compiler. This is what a generic submitter
// would build its plan from, instead of the hardcoded register numbers used by
// the current LitPrelight-only path.
void DumpConstantTable(const char* tag, const DWORD* function)
{
    if (!function) {
        Report("    {}: <no bytecode>", tag);
        return;
    }

    const D3DXGetShaderConstantTableFn getTable = ResolveGetShaderConstantTable();
    if (!getTable) {
        Report("    {}: D3DXGetShaderConstantTable unavailable", tag);
        return;
    }

    ID3DXConstantTable* table = nullptr;
    const HRESULT hr = getTable(function, &table);
    if (FAILED(hr) || !table) {
        Report("    {}: D3DXGetShaderConstantTable failed hr=0x{:08X}", tag,
            static_cast<unsigned>(hr));
        return;
    }

    D3DXCONSTANTTABLE_DESC desc{};
    if (FAILED(table->GetDesc(&desc))) {
        Report("    {}: GetDesc failed", tag);
        table->Release();
        return;
    }

    Report("    {}: creator=\"{}\" version=0x{:08X} constants={}",
        tag, desc.Creator ? desc.Creator : "", desc.Version, desc.Constants);

    for (UINT i = 0; i < desc.Constants; ++i) {
        D3DXHANDLE h = table->GetConstant(nullptr, i);
        if (!h) continue;
        D3DXCONSTANT_DESC cd{};
        UINT count = 1;
        if (FAILED(table->GetConstantDesc(h, &cd, &count))) continue;
        Report("      const[{:02}] {:<34} {}{}..{} count={} class={} type={} rows={} cols={} elems={} bytes={}",
            i,
            cd.Name ? cd.Name : "",
            RegSetName(cd.RegisterSet),
            cd.RegisterIndex,
            cd.RegisterIndex + (cd.RegisterCount ? cd.RegisterCount - 1 : 0),
            cd.RegisterCount,
            ClassName(cd.Class),
            TypeName(cd.Type),
            cd.Rows, cd.Columns, cd.Elements, cd.Bytes);
    }
    table->Release();
}

// Effect parameters are what ProperShaders sets via SetMatrix/SetVector/etc.
// A generic submitter needs to join these against the CTAB above.
void DumpParameters(ID3DXEffect* effect, const D3DXEFFECT_DESC& ed)
{
    Report("  parameters={}", ed.Parameters);
    for (UINT i = 0; i < ed.Parameters; ++i) {
        D3DXHANDLE p = effect->GetParameter(nullptr, i);
        if (!p) continue;
        D3DXPARAMETER_DESC pd{};
        if (FAILED(effect->GetParameterDesc(p, &pd))) continue;
        Report("    param[{:02}] {:<34} semantic={:<18} class={:<12} type={:<12} rows={} cols={} elems={} bytes={} annots={} structs={}",
            i,
            pd.Name ? pd.Name : "",
            pd.Semantic ? pd.Semantic : "-",
            ClassName(pd.Class),
            TypeName(pd.Type),
            pd.Rows, pd.Columns, pd.Elements, pd.Bytes,
            pd.Annotations, pd.StructMembers);
    }
}

} // namespace

namespace {

bool EnsureReport(const char* gameDir)
{
    if (g_report) return true;
    const std::string path = std::format(
        "{}\\scripts\\BridgeD3D9.effectinspect.log",
        gameDir ? gameDir : ".");
    g_report = fopen(path.c_str(), "w");
    return g_report != nullptr;
}

// One resolved constant destination for a parameter in one shader stage.
struct DryRunSlot
{
    bool vs = false;
    UINT base = 0;
    UINT count = 0;
    bool columns = false; // CTAB class is MATRIX_COLUMNS -> transpose on upload
};

// Joins one name against a stage's CTAB. Returns 0 = not referenced,
// 1 = direct float slot, 2 = sampler, -1 = unsupported register set.
int JoinName(ID3DXConstantTable* table, bool isVs, const char* name,
    DryRunSlot* slot, UINT* samplerIndex)
{
    if (!table || !name) return 0;
    D3DXHANDLE h = table->GetConstantByName(nullptr, name);
    if (!h) return 0;
    D3DXCONSTANT_DESC cd{};
    UINT count = 1;
    if (FAILED(table->GetConstantDesc(h, &cd, &count))) return 0;
    if (cd.RegisterSet == D3DXRS_FLOAT4) {
        slot->vs = isVs;
        slot->base = cd.RegisterIndex;
        slot->count = cd.RegisterCount;
        slot->columns = cd.Class == D3DXPC_MATRIX_COLUMNS;
        return 1;
    }
    if (cd.RegisterSet == D3DXRS_SAMPLER) {
        *samplerIndex = cd.RegisterIndex;
        return 2;
    }
    return -1; // b#/i# constants need SetVertexShaderConstantB/I handling.
}

} // namespace

void BuildGenericDirectPlanDryRun(void* effectPtr, const char* gameDir)
{
    if (!g_properShadersGenericDirectDryRun || !effectPtr) return;
    if (!EnsureReport(gameDir)) {
        g_properShadersGenericDirectDryRun = false;
        return;
    }
    const D3DXGetShaderConstantTableFn getTable = ResolveGetShaderConstantTable();
    if (!getTable) {
        Report("PLAN: D3DXGetShaderConstantTable unavailable; dry run disabled");
        g_properShadersGenericDirectDryRun = false;
        return;
    }

    ID3DXEffect* effect = reinterpret_cast<ID3DXEffect*>(effectPtr);
    D3DXEFFECT_DESC ed{};
    if (FAILED(effect->GetDesc(&ed))) return;

    Report("");
    Report("PLAN effect={:08X} techniques={} parameters={}", reinterpret_cast<std::uintptr_t>(effectPtr), ed.Techniques, ed.Parameters);

    for (UINT t = 0; t < ed.Techniques; ++t) {
        D3DXHANDLE th = effect->GetTechnique(t);
        D3DXTECHNIQUE_DESC td{};
        if (!th || FAILED(effect->GetTechniqueDesc(th, &td))) continue;

        const char* fallbackReason = nullptr;
        if (td.Passes != 1) fallbackReason = "multi-pass";

        ID3DXConstantTable* vsTable = nullptr;
        ID3DXConstantTable* psTable = nullptr;
        if (!fallbackReason) {
            D3DXHANDLE ph = effect->GetPass(th, 0);
            D3DXPASS_DESC pd{};
            if (!ph || FAILED(effect->GetPassDesc(ph, &pd))) {
                fallbackReason = "no-pass-desc";
            } else {
                if (pd.pVertexShaderFunction) getTable(pd.pVertexShaderFunction, &vsTable);
                if (pd.pPixelShaderFunction) getTable(pd.pPixelShaderFunction, &psTable);
                if (pd.pVertexShaderFunction && !vsTable) fallbackReason = "vs-ctab-missing";
                if (pd.pPixelShaderFunction && !psTable) fallbackReason = "ps-ctab-missing";
            }
        }

        UINT direct = 0, samplers = 0, unused = 0, unsupported = 0, structJoin = 0;
        for (UINT i = 0; i < ed.Parameters && !fallbackReason; ++i) {
            D3DXHANDLE p = effect->GetParameter(nullptr, i);
            D3DXPARAMETER_DESC pdd{};
            if (!p || FAILED(effect->GetParameterDesc(p, &pdd)) || !pdd.Name) continue;

            // Struct parameters appear in the CTAB flattened as "parent.member".
            char nameBuf[192]{};
            int joined = 0;
            if (pdd.Class == D3DXPC_STRUCT && pdd.StructMembers > 0) {
                bool anyMember = false, badMember = false;
                for (UINT m2 = 0; m2 < pdd.StructMembers; ++m2) {
                    D3DXHANDLE mh = effect->GetParameter(p, m2);
                    D3DXPARAMETER_DESC md{};
                    if (!mh || FAILED(effect->GetParameterDesc(mh, &md)) || !md.Name) continue;
                    FormatTo(nameBuf, sizeof(nameBuf), "{}.{}", pdd.Name, md.Name);
                    DryRunSlot s{}; UINT si = 0;
                    const int rv = JoinName(vsTable, true, nameBuf, &s, &si);
                    const int rp = JoinName(psTable, false, nameBuf, &s, &si);
                    if (rv < 0 || rp < 0) badMember = true;
                    if (rv > 0 || rp > 0) anyMember = true;
                }
                if (badMember) ++unsupported;
                else if (anyMember) { ++direct; ++structJoin; }
                else ++unused;
                continue;
            }

            DryRunSlot sv{}, sp{}; UINT si = 0;
            const int rv = JoinName(vsTable, true, pdd.Name, &sv, &si);
            const int rp = JoinName(psTable, false, pdd.Name, &sp, &si);
            if (rv < 0 || rp < 0) { ++unsupported; continue; }
            if (rv == 2 || rp == 2) { ++samplers; joined = 1; }
            if (rv == 1 || rp == 1) { ++direct; joined = 1; }
            if (!joined) ++unused;
        }

        if (vsTable) vsTable->Release();
        if (psTable) psTable->Release();

        if (!fallbackReason && unsupported > 0) fallbackReason = "b/i-register-param";
        Report("PLAN   {:<40} direct={} structJoin={} samplers={} unused={} unsupported={} verdict={}{}{}",
            td.Name ? td.Name : "?",
            direct, structJoin, samplers, unused, unsupported,
            fallbackReason ? "FALLBACK(" : "DIRECT-OK",
            fallbackReason ? fallbackReason : "",
            fallbackReason ? ")" : "");
    }
}

// ---- Stage ①-3a: real submission plans -------------------------------------

namespace {

// Registry of built plans. Creation and lookup both happen on the render
// thread (D3DXCreateEffect hook and the effect vtable hooks respectively), so
// no locking is needed under the current architecture.
struct ShadowEntry
{
    D3DXHANDLE param = nullptr;
    unsigned floats = 0;
    bool matrix = false;
    float v[16]{};
};
struct TextureShadowEntry
{
    D3DXHANDLE param = nullptr;
    IDirect3DBaseTexture9* texture = nullptr; // not AddRef'd (game-owned)
};
struct EffectPlans
{
    void* effect = nullptr;
    std::vector<GenericDirectTechniquePlan> techniques;
    std::vector<ShadowEntry> shadow;           // seen parameter values
    std::vector<TextureShadowEntry> texShadow; // seen texture parameters
};
std::vector<EffectPlans> g_genericDirectRegistry;

EffectPlans* FindEffectPlans(void* effect)
{
    const auto entry = std::ranges::find_if(
        g_genericDirectRegistry,
        [effect](const EffectPlans& candidate) {
            return candidate.effect == effect;
        });
    return entry == g_genericDirectRegistry.end() ? nullptr : &*entry;
}

// Adds one stage's join result to the plan. Returns false on overflow or an
// unsupported register set, which downgrades the whole technique to D3DX.
bool AddJoin(GenericDirectTechniquePlan& plan, D3DXHANDLE param, bool isMatrix,
    ID3DXConstantTable* table, bool isVs, const char* name)
{
    DryRunSlot s{};
    UINT samplerIndex = 0;
    const int r = JoinName(table, isVs, name, &s, &samplerIndex);
    if (r == 0) return true;   // not referenced by this stage
    if (r < 0) return false;   // b#/i# register
    if (r == 2) {
        if (plan.samplerCount >= 16) return false;
        GenericDirectSampler& out = plan.samplers[plan.samplerCount++];
        out.param = param;
        out.vs = isVs;
        out.index = samplerIndex;
        return true;
    }
    if (plan.slotCount >= 96) return false;
    GenericDirectSlot& out = plan.slots[plan.slotCount++];
    out.param = param;
    out.vs = isVs;
    out.base = s.base;
    out.count = s.count;
    out.matrix = isMatrix;
    out.columns = s.columns;
    return true;
}

} // namespace

int BuildGenericDirectPlans(void* effectPtr, const char* gameDir)
{
    // The trace-stability probe needs the CTAB plans too: liteEligible (computed
    // below) is what gates recording, and FindGenericDirectPlan returns null
    // without a stored plan, so the recorder would never run.
    if (!effectPtr || (!g_properShadersGenericDirect &&
            !g_properShadersGenericDirectDryRun &&
            !g_properShadersTraceStabilityProbe)) {
        return -1;
    }
    const D3DXGetShaderConstantTableFn getTable = ResolveGetShaderConstantTable();
    if (!getTable) return -1;

    ID3DXEffect* effect = reinterpret_cast<ID3DXEffect*>(effectPtr);
    D3DXEFFECT_DESC ed{};
    if (FAILED(effect->GetDesc(&ed))) return -1;

    EffectPlans entry;
    entry.effect = effectPtr;
    entry.techniques.reserve(ed.Techniques);

    for (UINT t = 0; t < ed.Techniques; ++t) {
        D3DXHANDLE th = effect->GetTechnique(t);
        D3DXTECHNIQUE_DESC td{};
        if (!th || FAILED(effect->GetTechniqueDesc(th, &td))) continue;

        GenericDirectTechniquePlan plan;
        plan.technique = th;
        plan.name = td.Name;
        plan.ok = td.Passes == 1;

        ID3DXConstantTable* vsTable = nullptr;
        ID3DXConstantTable* psTable = nullptr;
        if (plan.ok) {
            D3DXHANDLE ph = effect->GetPass(th, 0);
            D3DXPASS_DESC pdesc{};
            if (!ph || FAILED(effect->GetPassDesc(ph, &pdesc))) {
                plan.ok = false;
            } else {
                if (pdesc.pVertexShaderFunction) getTable(pdesc.pVertexShaderFunction, &vsTable);
                if (pdesc.pPixelShaderFunction) getTable(pdesc.pPixelShaderFunction, &psTable);
                if ((pdesc.pVertexShaderFunction && !vsTable) ||
                    (pdesc.pPixelShaderFunction && !psTable)) {
                    plan.ok = false;
                }
            }
        }

        for (UINT i = 0; plan.ok && i < ed.Parameters; ++i) {
            D3DXHANDLE p = effect->GetParameter(nullptr, i);
            D3DXPARAMETER_DESC pdd{};
            if (!p || FAILED(effect->GetParameterDesc(p, &pdd)) || !pdd.Name) continue;
            if (pdd.Class == D3DXPC_STRUCT) {
                // The current shader set has no struct parameters; if one ever
                // appears, stay on the safe D3DX path for this technique.
                plan.ok = false;
                break;
            }
            const bool isMatrix = pdd.Class == D3DXPC_MATRIX_ROWS ||
                pdd.Class == D3DXPC_MATRIX_COLUMNS;
            if (!AddJoin(plan, p, isMatrix, vsTable, true, pdd.Name) ||
                !AddJoin(plan, p, isMatrix, psTable, false, pdd.Name)) {
                plan.ok = false;
            }
        }

        if (vsTable) vsTable->Release();
        if (psTable) psTable->Release();

        // Pass-lite eligibility (stage B): every slot small enough for the
        // hooked setters, and no vertex-stage samplers.
        plan.liteEligible = plan.ok;
        for (unsigned i = 0; plan.liteEligible && i < plan.slotCount; ++i) {
            if (plan.slots[i].count > 4) plan.liteEligible = false;
        }
        for (unsigned i = 0; plan.liteEligible && i < plan.samplerCount; ++i) {
            if (plan.samplers[i].vs) plan.liteEligible = false;
        }
        entry.techniques.push_back(plan);
    }

    // Replace any stale entry for a reused pointer, then store.
    DropGenericDirectPlans(effectPtr);
    int okCount = 0;
    for (const GenericDirectTechniquePlan& plan : entry.techniques) {
        if (plan.ok) ++okCount;
    }
    g_genericDirectRegistry.push_back(std::move(entry));

    if (g_properShadersGenericDirectDryRun && EnsureReport(gameDir)) {
        for (const GenericDirectTechniquePlan& plan : g_genericDirectRegistry.back().techniques) {
            Report("PLANSTORE {:<38} slots={} samplers={} ok={}",
                plan.name ? plan.name : "?", plan.slotCount, plan.samplerCount,
                plan.ok ? 1 : 0);
        }
    }
    return okCount;
}

const GenericDirectTechniquePlan* FindGenericDirectPlan(
    void* effect, D3DXHANDLE technique)
{
    const auto effectEntry = std::ranges::find_if(
        g_genericDirectRegistry,
        [effect](const EffectPlans& candidate) {
            return candidate.effect == effect;
        });
    if (effectEntry == g_genericDirectRegistry.end()) return nullptr;
    const auto plan = std::ranges::find_if(
        effectEntry->techniques,
        [technique](const GenericDirectTechniquePlan& candidate) {
            return candidate.technique == technique;
        });
    return plan == effectEntry->techniques.end()
        ? nullptr
        : (plan->ok ? &*plan : nullptr);
}

void DropGenericDirectPlans(void* effect)
{
    const auto entry = std::ranges::find_if(
        g_genericDirectRegistry,
        [effect](const EffectPlans& candidate) {
            return candidate.effect == effect;
        });
    if (entry != g_genericDirectRegistry.end()) {
        g_genericDirectRegistry.erase(entry);
    }
}

// ---- Stage ①-3b-ii: shadow store + direct submission -----------------------

namespace {

// Uploads one slot's registers from a shadow value through the journal, which
// records the write so the transaction restore stays correct. Matrix input is
// the 16-float row-major D3DXMATRIX layout; column-major CTAB slots transpose.
void SubmitSlot(const GenericDirectSlot& slot, const ShadowEntry& sh,
    ProperShadersStateJournal* journal)
{
    float buf[16]{};
    const UINT regs = slot.count > 4 ? 4 : slot.count;
    if (slot.matrix && sh.matrix) {
        if (slot.columns) {
            for (UINT r = 0; r < regs; ++r) {
                buf[r * 4 + 0] = sh.v[0 * 4 + r];
                buf[r * 4 + 1] = sh.v[1 * 4 + r];
                buf[r * 4 + 2] = sh.v[2 * 4 + r];
                buf[r * 4 + 3] = sh.v[3 * 4 + r];
            }
        } else {
            memcpy(buf, sh.v, regs * 4 * sizeof(float));
        }
    } else {
        const unsigned floats = sh.floats > regs * 4 ? regs * 4 : sh.floats;
        memcpy(buf, sh.v, floats * sizeof(float));
    }
    if (slot.vs) {
        journal->SetVertexShaderConstantF(slot.base, buf, regs);
    } else {
        journal->SetPixelShaderConstantF(slot.base, buf, regs);
    }
}

} // namespace

bool GenericDirectHandleSet(void* effect, const GenericDirectTechniquePlan* plan,
    ProperShadersStateJournal* journal, D3DXHANDLE param,
    const float* data, unsigned floatCount, bool asMatrix)
{
    if (!plan || !param || !data || floatCount == 0 || floatCount > 16) return false;

    bool covered = false;
    for (unsigned i = 0; i < plan->slotCount; ++i) {
        const GenericDirectSlot& s = plan->slots[i];
        if (s.param != param) continue;
        if (s.count > 4) return false; // larger arrays stay on the D3DX path
        covered = true;
    }
    if (!covered) return false;

    EffectPlans* plans = FindEffectPlans(effect);
    if (!plans) return false;

    const auto existing = std::ranges::find_if(
        plans->shadow,
        [param](const ShadowEntry& candidate) {
            return candidate.param == param;
        });
    ShadowEntry* sh = existing == plans->shadow.end()
        ? nullptr
        : &*existing;
    if (!sh) {
        if (plans->shadow.size() >= 128) return false;
        plans->shadow.push_back(ShadowEntry{});
        sh = &plans->shadow.back();
        sh->param = param;
    }
    sh->floats = floatCount;
    sh->matrix = asMatrix;
    memset(sh->v, 0, sizeof(sh->v));
    memcpy(sh->v, data, floatCount * sizeof(float));

    if (journal && journal->IsActive()) {
        for (unsigned i = 0; i < plan->slotCount; ++i) {
            const GenericDirectSlot& s = plan->slots[i];
            if (s.param == param) SubmitSlot(s, *sh, journal);
        }
    }
    return true;
}

void GenericDirectReplaySeen(void* effect, const GenericDirectTechniquePlan* plan,
    ProperShadersStateJournal* journal)
{
    if (!plan || !journal) return;
    EffectPlans* plans = FindEffectPlans(effect);
    if (!plans || plans->shadow.empty()) return;
    for (unsigned i = 0; i < plan->slotCount; ++i) {
        const GenericDirectSlot& s = plan->slots[i];
        if (s.count > 4) continue;
        for (const ShadowEntry& e : plans->shadow) {
            if (e.param == s.param) { SubmitSlot(s, e, journal); break; }
        }
    }
}

// ---- Stage B: pass-lite (full D3DX pass-machine bypass) --------------------

bool g_properShadersGenericPassLite = false;

// ---- Trace-stability probe (observation only) ------------------------------
bool g_properShadersTraceStabilityProbe = false;

namespace {

// Stream op codes. Header layout per op:
//   u8 op | u32 a | u32 b | u16 payloadBytes | payload...
enum : unsigned char
{
    kOpVertexShader = 1,  // payload: pointer
    kOpPixelShader = 2,   // payload: pointer
    kOpRenderState = 3,   // a=state b=value
    kOpSamplerState = 4,  // a=(sampler<<8)|type b=value
    kOpTextureStage = 5,  // a=(stage<<8)|type b=value
    kOpTexture = 6,       // a=stage, payload: pointer (dynamic, masked)
    kOpVsConstF = 7,      // a=start b=regCount, payload floats
    kOpPsConstF = 8,
    kOpFvf = 9,           // a=fvf
    kOpNPatch = 10,       // payload: float
    kOpTransform = 11,    // a=state, payload: D3DMATRIX
    kOpMaterial = 12,     // payload: D3DMATERIAL9
    kOpLight = 13,        // a=index, payload: D3DLIGHT9
    kOpLightEnable = 14,  // a=index b=enable
    kOpVsConstI = 15,     // a=start b=count, payload ints
    kOpVsConstB = 16,     // a=start b=count, payload BOOLs
    kOpPsConstI = 17,
    kOpPsConstB = 18,
};

struct OpHeader
{
    unsigned char op;
    unsigned a;
    unsigned b;
    unsigned short bytes;
};

void AppendOp(std::vector<unsigned char>* out, unsigned char op, unsigned a,
    unsigned b, std::span<const std::byte> payload)
{
    if (!out || payload.size() > 0xFFFF) return;
    if (out->size() > (1u << 20)) return; // runaway guard: 1MB per recording
    OpHeader h{ op, a, b, static_cast<unsigned short>(payload.size()) };
    const unsigned char* hp = reinterpret_cast<const unsigned char*>(&h);
    out->insert(out->end(), hp, hp + sizeof(h));
    if (!payload.empty()) {
        const auto* bytes =
            reinterpret_cast<const unsigned char*>(payload.data());
        out->insert(out->end(), bytes, bytes + payload.size());
    }
}

// Records every state write D3DX makes while it is the effect's state
// manager, forwarding each one to the journal unchanged.
struct PassRecorder final : public ID3DXEffectStateManager
{
    ProperShadersStateJournal* inner = nullptr;
    std::vector<unsigned char>* out = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    // Every forward below is guarded: D3DX does not always stop calling a state
    // manager the instant SetStateManager swaps it out, so a recording that has
    // already been closed can still receive callbacks. Dereferencing a cleared
    // `inner` there is what crashed d3dx9 at HookedProperShadersGeneralBegin
    // (access violation reading 0x00000000). A null `inner` means the recording
    // window is over, so the call is dropped rather than forwarded.
    HRESULT STDMETHODCALLTYPE SetTransform(
        D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpTransform, state, 0, std::as_bytes(std::span(matrix, 1)));
        return inner->SetTransform(state, matrix);
    }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* material) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpMaterial, 0, 0, std::as_bytes(std::span(material, 1)));
        return inner->SetMaterial(material);
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9* light) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpLight, index, 0, std::as_bytes(std::span(light, 1)));
        return inner->SetLight(index, light);
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpLightEnable, index, static_cast<unsigned>(enable), {});
        return inner->LightEnable(index, enable);
    }
    HRESULT STDMETHODCALLTYPE SetRenderState(
        D3DRENDERSTATETYPE state, DWORD value) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpRenderState, state, value, {});
        return inner->SetRenderState(state, value);
    }
    HRESULT STDMETHODCALLTYPE SetTexture(
        DWORD stage, IDirect3DBaseTexture9* texture) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpTexture, stage, 0, std::as_bytes(std::span(&texture, 1)));
        return inner->SetTexture(stage, texture);
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(
        DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpTextureStage, (stage << 8) | type, value, {});
        return inner->SetTextureStageState(stage, type, value);
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(
        DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpSamplerState, (sampler << 8) | type, value, {});
        return inner->SetSamplerState(sampler, type, value);
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpNPatch, 0, 0, std::as_bytes(std::span(&segments, 1)));
        return inner->SetNPatchMode(segments);
    }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpFvf, fvf, 0, {});
        return inner->SetFVF(fvf);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* shader) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpVertexShader, 0, 0, std::as_bytes(std::span(&shader, 1)));
        return inner->SetVertexShader(shader);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
        UINT startRegister, const float* data, UINT registerCount) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpVsConstF, startRegister, registerCount,
            std::as_bytes(std::span(data, registerCount * 4)));
        return inner->SetVertexShaderConstantF(startRegister, data, registerCount);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(
        UINT startRegister, const int* data, UINT registerCount) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpVsConstI, startRegister, registerCount,
            std::as_bytes(std::span(data, registerCount * 4)));
        return inner->SetVertexShaderConstantI(startRegister, data, registerCount);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(
        UINT startRegister, const BOOL* data, UINT registerCount) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpVsConstB, startRegister, registerCount,
            std::as_bytes(std::span(data, registerCount)));
        return inner->SetVertexShaderConstantB(startRegister, data, registerCount);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* shader) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpPixelShader, 0, 0, std::as_bytes(std::span(&shader, 1)));
        return inner->SetPixelShader(shader);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(
        UINT startRegister, const float* data, UINT registerCount) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpPsConstF, startRegister, registerCount,
            std::as_bytes(std::span(data, registerCount * 4)));
        return inner->SetPixelShaderConstantF(startRegister, data, registerCount);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(
        UINT startRegister, const int* data, UINT registerCount) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpPsConstI, startRegister, registerCount,
            std::as_bytes(std::span(data, registerCount * 4)));
        return inner->SetPixelShaderConstantI(startRegister, data, registerCount);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(
        UINT startRegister, const BOOL* data, UINT registerCount) override
    {
        if (!inner) return D3D_OK;
        AppendOp(out, kOpPsConstB, startRegister, registerCount,
            std::as_bytes(std::span(data, registerCount)));
        return inner->SetPixelShaderConstantB(startRegister, data, registerCount);
    }
};

PassRecorder g_passRecorder;

// Probe-only scratch recording buffer. The probe does not use liteRec1/2 (it
// never advances liteState), so each pass is recorded here and read back by
// GenericDirectEndRecording before the next pass overwrites it.
std::vector<unsigned char> g_probeScratch;

// True when [start, start+count) overlaps any plan slot of the given stage.
bool IntersectsPlanSlots(const GenericDirectTechniquePlan* plan, bool vs,
    unsigned start, unsigned count)
{
    for (unsigned i = 0; i < plan->slotCount; ++i) {
        const GenericDirectSlot& s = plan->slots[i];
        if (s.vs != vs) continue;
        if (start < s.base + s.count && s.base < start + count) return true;
    }
    return false;
}

// True when every plan slot overlapping [start,count) has a seen shadow value
// (the replay of those values then supersedes the recorded payload).
bool CoveredBySeenShadow(const EffectPlans* plans,
    const GenericDirectTechniquePlan* plan, bool vs, unsigned start, unsigned count)
{
    bool any = false;
    for (unsigned i = 0; i < plan->slotCount; ++i) {
        const GenericDirectSlot& s = plan->slots[i];
        if (s.vs != vs) continue;
        if (!(start < s.base + s.count && s.base < start + count)) continue;
        any = true;
        bool seen = false;
        for (const ShadowEntry& e : plans->shadow) {
            if (e.param == s.param) { seen = true; break; }
        }
        if (!seen) return false;
    }
    return any;
}

// Validates that two recordings describe the same pass. Texture pointers and
// plan-covered constant payloads are per-object dynamic, so only their headers
// must match; everything else must be byte-identical.
bool RecordingsAgree(const GenericDirectTechniquePlan* plan,
    const std::vector<unsigned char>& a, const std::vector<unsigned char>& b)
{
    size_t pa = 0, pb = 0;
    while (pa < a.size() && pb < b.size()) {
        if (pa + sizeof(OpHeader) > a.size() || pb + sizeof(OpHeader) > b.size()) {
            return false;
        }
        OpHeader ha{}, hb{};
        memcpy(&ha, a.data() + pa, sizeof(ha));
        memcpy(&hb, b.data() + pb, sizeof(hb));
        if (ha.op != hb.op || ha.a != hb.a || ha.b != hb.b || ha.bytes != hb.bytes) {
            return false;
        }
        const size_t da = pa + sizeof(OpHeader);
        const size_t db = pb + sizeof(OpHeader);
        if (da + ha.bytes > a.size() || db + hb.bytes > b.size()) return false;

        bool comparePayload = true;
        if (ha.op == kOpTexture) comparePayload = false;
        if ((ha.op == kOpVsConstF || ha.op == kOpPsConstF) &&
            IntersectsPlanSlots(plan, ha.op == kOpVsConstF, ha.a, ha.b)) {
            comparePayload = false;
        }
        if (comparePayload && ha.bytes &&
            memcmp(a.data() + da, b.data() + db, ha.bytes) != 0) {
            return false;
        }
        pa = da + ha.bytes;
        pb = db + hb.bytes;
    }
    return pa == a.size() && pb == b.size();
}

// ---- Trace-stability probe (observation only) ------------------------------

FILE* g_stabReport = nullptr;

bool EnsureStabReport()
{
    if (g_stabReport) return true;
    g_stabReport = fopen("scripts\\BridgeD3D9.tracestab.log", "a");
    return g_stabReport != nullptr;
}

template <class... Args>
void StabReport(std::format_string<Args...> fmt, Args&&... args)
{
    if (!g_stabReport) return;
    std::vprint_nonunicode(g_stabReport, fmt.get(), std::make_format_args(args...));
    fputc('\n', g_stabReport);
    fflush(g_stabReport);
}

const char* StabOpName(unsigned char op)
{
    constexpr std::array<std::pair<unsigned char, const char*>, 18> kNames = {{
        {kOpVertexShader, "VS"},
        {kOpPixelShader, "PS"},
        {kOpRenderState, "RS"},
        {kOpSamplerState, "SAMP"},
        {kOpTextureStage, "TSS"},
        {kOpTexture, "TEX"},
        {kOpVsConstF, "VcF"},
        {kOpPsConstF, "PcF"},
        {kOpFvf, "FVF"},
        {kOpNPatch, "NPATCH"},
        {kOpTransform, "XFORM"},
        {kOpMaterial, "MAT"},
        {kOpLight, "LIGHT"},
        {kOpLightEnable, "LIGHTEN"},
        {kOpVsConstI, "VcI"},
        {kOpVsConstB, "VcB"},
        {kOpPsConstI, "PcI"},
        {kOpPsConstB, "PcB"},
    }};
    const auto match = std::ranges::find_if(
        kNames,
        [op](const auto& entry) {
            return entry.first == op;
        });
    return match == kNames.end() ? "?" : match->second;
}

// Same payload-masking rule as RecordingsAgree: texture pointers are rebound
// per object, and constant ranges overlapping plan-covered slots are superseded
// by the shadow store, so neither counts as a stability "change".
bool StabMaskPayload(const GenericDirectTechniquePlan* plan, unsigned char op,
    unsigned a, unsigned b)
{
    if (op == kOpTexture) return true;
    if ((op == kOpVsConstF || op == kOpPsConstF) &&
        IntersectsPlanSlots(plan, op == kOpVsConstF, a, b)) {
        return true;
    }
    return false;
}

// Diffs one recorded pass against the technique's baseline, incrementing
// stabChanged[i] for every op slot whose bytes differ. The first recording for
// a technique becomes the baseline and is not diffed.
void TraceStabilityObserve(GenericDirectTechniquePlan* plan,
    const std::vector<unsigned char>& rec)
{
    if (!plan || plan->stabFull) return;

    if (plan->stabBase.empty()) {
        plan->stabBase = rec;
        plan->stabSamples = 1;
        return;
    }
    if (plan->stabSamples >= GenericDirectTechniquePlan::kStabMaxSamples) {
        plan->stabFull = true;
        return;
    }

    const std::vector<unsigned char>& base = plan->stabBase;
    size_t pb = 0, pc = 0;
    unsigned slot = 0;
    // Walk both streams in lockstep. Headers must line up (same op sequence);
    // a structural divergence means the pass is not comparable, so we stop and
    // conservatively leave the remaining counts as-is.
    while (pb + sizeof(OpHeader) <= base.size() &&
           pc + sizeof(OpHeader) <= rec.size() &&
           slot < GenericDirectTechniquePlan::kStabMaxSlots) {
        OpHeader hb{}, hc{};
        memcpy(&hb, base.data() + pb, sizeof(hb));
        memcpy(&hc, rec.data() + pc, sizeof(hc));
        if (hb.op != hc.op || hb.a != hc.a || hb.b != hc.b) break;
        const size_t db = pb + sizeof(OpHeader);
        const size_t dc = pc + sizeof(OpHeader);
        if (db + hb.bytes > base.size() || dc + hc.bytes > rec.size()) break;
        const bool masked = StabMaskPayload(plan, hb.op, hb.a, hb.b);
        bool changed = (hb.bytes != hc.bytes);
        if (!changed && !masked && hb.bytes &&
            memcmp(base.data() + db, rec.data() + dc, hb.bytes) != 0) {
            changed = true;
        }
        if (changed) ++plan->stabChanged[slot];
        ++slot;
        pb = db + hb.bytes;
        pc = dc + hc.bytes;
    }
    ++plan->stabSamples;
    if (plan->stabSamples >= GenericDirectTechniquePlan::kStabMaxSamples) {
        plan->stabFull = true;
    }
}

} // namespace

// Appends the per-technique stability histogram to the probe log.
void TraceStabilityDumpAll()
{
    if (!g_properShadersTraceStabilityProbe) return;
    if (!EnsureStabReport()) return;

    bool wroteAny = false;
    for (const EffectPlans& e : g_genericDirectRegistry) {
        for (const GenericDirectTechniquePlan& plan : e.techniques) {
            if (plan.stabSamples < 2) continue;
            const unsigned diffs = plan.stabSamples - 1;
            unsigned inv = 0, obj = 0, masked = 0, total = 0;
            if (!wroteAny) {
                StabReport("---- tracestab dump (samples compared, not replayed) ----");
                wroteAny = true;
            }
            StabReport("technique={} samples={} diffs={}",
                plan.name ? plan.name : "?", plan.stabSamples, diffs);

            const std::vector<unsigned char>& b = plan.stabBase;
            size_t p = 0;
            unsigned slot = 0;
            while (p + sizeof(OpHeader) <= b.size() &&
                   slot < GenericDirectTechniquePlan::kStabMaxSlots) {
                OpHeader h{};
                memcpy(&h, b.data() + p, sizeof(h));
                const size_t d = p + sizeof(OpHeader);
                if (d + h.bytes > b.size()) break;
                const bool m = StabMaskPayload(&plan, h.op, h.a, h.b);
                const unsigned ch = plan.stabChanged[slot];
                ++total;
                if (m) ++masked;
                else if (ch == 0) ++inv;
                else ++obj;
                if (ch > 0 || m) {
                    StabReport("  slot[{:3}] {:<7} a={} b={} bytes={} changed={}/{}{}",
                        slot, StabOpName(h.op), h.a, h.b, h.bytes, ch, diffs,
                        m ? " masked" : "");
                }
                ++slot;
                p = d + h.bytes;
            }
            const unsigned classified = inv + obj;
            StabReport(
                "  SUMMARY invariant={} perObject={} masked={} total={} stableRatio={:.3f}",
                inv, obj, masked, total,
                classified ? double(inv) / double(classified) : 0.0);
        }
    }
    if (wroteAny) StabReport("---- end dump ----");
}

int GenericDirectPassAction(const GenericDirectTechniquePlan* plan)
{
    // Trace-stability probe takes precedence: keep recording (action 1) so
    // every pass is observed, but never return 2 — the pass is never replayed
    // and rendering is untouched. Recording stops once the plan is full.
    if (g_properShadersTraceStabilityProbe) {
        if (!plan || !plan->liteEligible) return 0;
        return plan->stabFull ? 0 : 1;
    }
    if (!g_properShadersGenericPassLite || !plan || !plan->liteEligible) return 0;
    if (plan->liteState == 2) return 2;
    if (plan->liteState >= 3) return 0;
    return 1;
}

bool GenericDirectBeginRecording(void* effect,
    const GenericDirectTechniquePlan* plan, ProperShadersStateJournal* journal)
{
    if (!effect || !plan || !journal || g_passRecorder.out) return false;
    std::vector<unsigned char>* target = nullptr;
    if (g_properShadersTraceStabilityProbe) {
        // Probe: liteState is not advanced, so rec1/rec2 selection would be
        // wrong. Use a dedicated scratch buffer that EndRecording reads back.
        target = &g_probeScratch;
    } else {
        target = plan->liteState == 0 ? &plan->liteRec1 : &plan->liteRec2;
    }
    target->clear();
    g_passRecorder.inner = journal;
    g_passRecorder.out = target;
    ID3DXEffect* d3dxEffect = reinterpret_cast<ID3DXEffect*>(effect);
    if (FAILED(d3dxEffect->SetStateManager(&g_passRecorder))) {
        g_passRecorder.out = nullptr;
        g_passRecorder.inner = nullptr;
        return false;
    }
    return true;
}

void ReleaseRecorderForJournal(ProperShadersStateJournal* journal)
{
    // EndRecording deliberately leaves `inner` set so late D3DX callbacks still
    // forward. This is the one place that pointer can become dangling: the
    // journal is about to be released with the effect. Clear it (and any open
    // recording) so the guarded forwards drop instead of touching freed memory.
    if (journal && g_passRecorder.inner != journal) return;
    g_passRecorder.out = nullptr;
    g_passRecorder.inner = nullptr;
}

void GenericDirectEndRecording(void* effect,
    const GenericDirectTechniquePlan* plan, ProperShadersStateJournal* journal,
    bool ok)
{
    if (!effect || !plan) return;
    ID3DXEffect* d3dxEffect = reinterpret_cast<ID3DXEffect*>(effect);
    d3dxEffect->SetStateManager(journal);
    // Stop appending to the recording, but KEEP `inner` pointing at the journal.
    // D3DX can still call this state manager after SetStateManager swapped it
    // out; with `inner` intact those late calls forward transparently to the
    // journal instead of being dropped, so no state write is lost. `inner` is
    // re-pointed by the next BeginRecording and cleared only when the effect is
    // released (ReleaseRecorderForJournal), which is the one point where the
    // journal it references can go away.
    g_passRecorder.out = nullptr;

    // Trace-stability probe: observe this recording and bail out before any
    // pass-lite state/replay logic. `plan` is const here but the stab fields are
    // mutable, matching the rest of the plan's observation-time bookkeeping.
    if (g_properShadersTraceStabilityProbe) {
        GenericDirectTechniquePlan* p = const_cast<GenericDirectTechniquePlan*>(plan);
        if (ok) {
            TraceStabilityObserve(p, g_probeScratch);
        }
        // Throttled dump: one wall-clock check per recording, dump at most
        // once every 8 seconds. Runs on the render thread but only writes when
        // data exists and the interval has elapsed; cost is a GetTickCount64.
        static ULONGLONG lastDump = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastDump >= 8000ull) {
            lastDump = now;
            TraceStabilityDumpAll();
        }
        return;
    }

    if (!ok) {
        plan->liteState = 3;
        return;
    }
    if (plan->liteState == 0) {
        plan->liteState = 1;
        return;
    }
    if (plan->liteState == 1) {
        plan->liteState =
            RecordingsAgree(plan, plan->liteRec1, plan->liteRec2) ? 2 : 3;
        if (plan->liteState == 2) {
            // Learn per-stage texture resolution from recording #1: a recorded
            // binding whose pointer equals a texture parameter's current
            // shadow value is dynamic (rebound per object from that param);
            // anything else is a static texture (LUTs, noise, shadow maps).
            EffectPlans* plans = FindEffectPlans(effect);
            const std::vector<unsigned char>& rec = plan->liteRec1;
            size_t p = 0;
            while (p + sizeof(OpHeader) <= rec.size()) {
                OpHeader h{};
                memcpy(&h, rec.data() + p, sizeof(h));
                const unsigned char* payload = rec.data() + p + sizeof(h);
                if (p + sizeof(h) + h.bytes > rec.size()) break;
                p += sizeof(h) + h.bytes;
                if (h.op != kOpTexture || h.a >= 20) continue;
                IDirect3DBaseTexture9* recorded = nullptr;
                memcpy(&recorded, payload, sizeof(recorded));
                plan->liteStageMode[h.a] = 2;
                plan->liteStageParam[h.a] = nullptr;
                plan->liteStageStatic[h.a] = recorded;
                if (plans && recorded) {
                    for (const TextureShadowEntry& t : plans->texShadow) {
                        if (t.texture == recorded) {
                            plan->liteStageMode[h.a] = 1;
                            plan->liteStageParam[h.a] = t.param;
                            plan->liteStageStatic[h.a] = nullptr;
                            break;
                        }
                    }
                }
            }
        }
    }
}

void GenericDirectApplyPassLite(void* effect,
    const GenericDirectTechniquePlan* plan, ProperShadersStateJournal* journal)
{
    if (!effect || !plan || !journal) return;
    EffectPlans* plans = FindEffectPlans(effect);
    const std::vector<unsigned char>& rec = plan->liteRec1;

    size_t p = 0;
    while (p + sizeof(OpHeader) <= rec.size()) {
        OpHeader h{};
        memcpy(&h, rec.data() + p, sizeof(h));
        const unsigned char* payload = rec.data() + p + sizeof(h);
        if (p + sizeof(h) + h.bytes > rec.size()) break;
        p += sizeof(h) + h.bytes;

        switch (h.op) {
        case kOpVertexShader: {
            IDirect3DVertexShader9* s = nullptr;
            memcpy(&s, payload, sizeof(s));
            journal->SetVertexShader(s);
            break;
        }
        case kOpPixelShader: {
            IDirect3DPixelShader9* s = nullptr;
            memcpy(&s, payload, sizeof(s));
            journal->SetPixelShader(s);
            break;
        }
        case kOpRenderState:
            journal->SetRenderState(static_cast<D3DRENDERSTATETYPE>(h.a), h.b);
            break;
        case kOpSamplerState:
            journal->SetSamplerState(h.a >> 8,
                static_cast<D3DSAMPLERSTATETYPE>(h.a & 0xFF), h.b);
            break;
        case kOpTextureStage:
            journal->SetTextureStageState(h.a >> 8,
                static_cast<D3DTEXTURESTAGESTATETYPE>(h.a & 0xFF), h.b);
            break;
        case kOpTexture:
            break; // dynamic: applied from the texture shadow below
        case kOpVsConstF:
        case kOpPsConstF: {
            const bool vs = h.op == kOpVsConstF;
            if (plans && CoveredBySeenShadow(plans, plan, vs, h.a, h.b)) {
                break; // shadow replay writes fresher values below
            }
            const float* data = reinterpret_cast<const float*>(payload);
            if (vs) {
                journal->SetVertexShaderConstantF(h.a, data, h.b);
            } else {
                journal->SetPixelShaderConstantF(h.a, data, h.b);
            }
            break;
        }
        case kOpFvf:
            journal->SetFVF(h.a);
            break;
        case kOpNPatch: {
            float v = 0.0f;
            memcpy(&v, payload, sizeof(v));
            journal->SetNPatchMode(v);
            break;
        }
        case kOpTransform: {
            D3DMATRIX m{};
            memcpy(&m, payload, sizeof(m));
            journal->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(h.a), &m);
            break;
        }
        case kOpMaterial: {
            D3DMATERIAL9 m{};
            memcpy(&m, payload, sizeof(m));
            journal->SetMaterial(&m);
            break;
        }
        case kOpLight: {
            D3DLIGHT9 l{};
            memcpy(&l, payload, sizeof(l));
            journal->SetLight(h.a, &l);
            break;
        }
        case kOpLightEnable:
            journal->LightEnable(h.a, static_cast<BOOL>(h.b));
            break;
        case kOpVsConstI:
            journal->SetVertexShaderConstantI(h.a,
                reinterpret_cast<const int*>(payload), h.b);
            break;
        case kOpVsConstB:
            journal->SetVertexShaderConstantB(h.a,
                reinterpret_cast<const BOOL*>(payload), h.b);
            break;
        case kOpPsConstI:
            journal->SetPixelShaderConstantI(h.a,
                reinterpret_cast<const int*>(payload), h.b);
            break;
        case kOpPsConstB:
            journal->SetPixelShaderConstantB(h.a,
                reinterpret_cast<const BOOL*>(payload), h.b);
            break;
        default:
            break;
        }
    }

    // Dynamic constants from the shadow store.
    GenericDirectReplaySeen(effect, plan, journal);

    // Textures by stage, using the resolution learned from the recordings.
    for (unsigned stage = 0; stage < 20; ++stage) {
        if (plan->liteStageMode[stage] == 1) {
            IDirect3DBaseTexture9* tex = nullptr;
            if (plans) {
                for (const TextureShadowEntry& t : plans->texShadow) {
                    if (t.param == plan->liteStageParam[stage]) {
                        tex = t.texture;
                        break;
                    }
                }
            }
            journal->SetTexture(stage, tex);
        } else if (plan->liteStageMode[stage] == 2) {
            journal->SetTexture(stage, plan->liteStageStatic[stage]);
        }
    }
}

void GenericDirectShadowTexture(void* effect, D3DXHANDLE param,
    IDirect3DBaseTexture9* texture)
{
    if (!param) return;
    EffectPlans* plans = FindEffectPlans(effect);
    if (!plans) return;
    for (TextureShadowEntry& t : plans->texShadow) {
        if (t.param == param) {
            t.texture = texture;
            return;
        }
    }
    if (plans->texShadow.size() >= 32) return;
    plans->texShadow.push_back(TextureShadowEntry{ param, texture });
}

void InspectProperShadersEffect(void* effectPtr, unsigned long long effectBytes,
    const char* gameDir)
{
    if (!g_properShadersInspectEffects || !effectPtr) return;

    if (!g_report) {
        const std::string path = std::format(
            "{}\\scripts\\BridgeD3D9.effectinspect.log",
            gameDir ? gameDir : ".");
        g_report = fopen(path.c_str(), "w");
        if (!g_report) {
            g_properShadersInspectEffects = false;
            return;
        }
        Report("# BridgeD3D9 effect inspection (read-only)");
        Report("# Question: are parameter -> shader-register layouts regular enough");
        Report("# to build a generic direct submitter and bypass D3DX entirely?");
    }

    ID3DXEffect* effect = reinterpret_cast<ID3DXEffect*>(effectPtr);
    const int index = g_effectIndex++;

    D3DXEFFECT_DESC ed{};
    if (FAILED(effect->GetDesc(&ed))) {
        Report("effect[{}] ptr={:08X} bytes={}: GetDesc failed", index, reinterpret_cast<std::uintptr_t>(effectPtr), effectBytes);
        return;
    }

    Report("");
    Report("================================================================");
    Report("effect[{}] ptr={:08X} bytes={} techniques={} parameters={} functions={}",
        index, reinterpret_cast<std::uintptr_t>(effectPtr), effectBytes, ed.Techniques, ed.Parameters, ed.Functions);

    DumpParameters(effect, ed);

    for (UINT t = 0; t < ed.Techniques; ++t) {
        D3DXHANDLE th = effect->GetTechnique(t);
        if (!th) continue;
        D3DXTECHNIQUE_DESC td{};
        if (FAILED(effect->GetTechniqueDesc(th, &td))) continue;

        const HRESULT valid = effect->ValidateTechnique(th);
        Report("  technique[{}] {:<40} passes={} annotations={} validate=0x{:08X}",
            t, td.Name ? td.Name : "", td.Passes, td.Annotations,
            static_cast<unsigned>(valid));

        for (UINT p = 0; p < td.Passes; ++p) {
            D3DXHANDLE ph = effect->GetPass(th, p);
            if (!ph) continue;
            D3DXPASS_DESC pdesc{};
            if (FAILED(effect->GetPassDesc(ph, &pdesc))) {
                Report("    pass[{}]: GetPassDesc failed", p);
                continue;
            }
            Report("    pass[{}] {:<32} annotations={}",
                p, pdesc.Name ? pdesc.Name : "", pdesc.Annotations);
            DumpConstantTable("VS", pdesc.pVertexShaderFunction);
            DumpConstantTable("PS", pdesc.pPixelShaderFunction);
        }
    }
}

// ---- Journal-level trace-stability analyser --------------------------------
// Unlike the EffectInspector probe, this sees every technique: the journal is
// the state manager for all ProperShaders effects, with no CTAB plan or
// pass-lite eligibility requirement. That is why the hot per-OBJECT techniques
// (LitPrelight, DepthPass) are visible here and were not visible there.
//
// Per technique we keep the first transaction's records as a baseline and count,
// per (op,key) slot, how many later transactions wrote a different value hash.
// A slot that never differs is INVARIANT (hoistable out of the object loop); a
// slot that differs often is PER-OBJECT (must stay per draw).

namespace {

struct JournalSlotStat
{
    unsigned char op = 0;
    std::uint32_t key = 0;
    std::uint32_t pass = 0xFFFFFFFFu;
    std::uint64_t baseHash = 0;
    unsigned changed = 0;
};

struct JournalTechniqueStat
{
    char name[64]{};
    unsigned transactions = 0;     // total observed
    unsigned comparisons = 0;      // transactions diffed against the baseline
    unsigned truncatedCount = 0;   // transactions that hit the record cap
    unsigned structuralMismatch = 0; // different op/key sequence than baseline
    unsigned stride = 1;           // sample 1 of every `stride` transactions
    unsigned sinceSample = 0;
    std::vector<JournalSlotStat> slots;
};

std::vector<JournalTechniqueStat> g_journalStats;
const unsigned kJournalMaxTechniques = 96;
const unsigned kJournalMaxComparisons = 4096;
const unsigned kJournalMaxStride = 4096;

JournalTechniqueStat* FindOrAddJournalStat(const char* name)
{
    const char* n = (name && *name) ? name : "<unknown>";
    for (JournalTechniqueStat& s : g_journalStats) {
        if (std::strcmp(s.name, n) == 0) return &s;
    }
    if (g_journalStats.size() >= kJournalMaxTechniques) return nullptr;
    g_journalStats.emplace_back();
    JournalTechniqueStat& s = g_journalStats.back();
    FormatTo(s.name, sizeof(s.name), "{}", n);
    return &s;
}

const char* JournalOpName(unsigned char op)
{
    constexpr std::array<std::pair<JournalProbeOp, const char*>, 18> kNames = {{
        {JournalProbeOp::Transform, "XFORM"},
        {JournalProbeOp::Material, "MAT"},
        {JournalProbeOp::Light, "LIGHT"},
        {JournalProbeOp::LightEnable, "LIGHTEN"},
        {JournalProbeOp::RenderState, "RS"},
        {JournalProbeOp::Texture, "TEX"},
        {JournalProbeOp::TextureStage, "TSS"},
        {JournalProbeOp::SamplerState, "SAMP"},
        {JournalProbeOp::NPatch, "NPATCH"},
        {JournalProbeOp::Fvf, "FVF"},
        {JournalProbeOp::VertexShader, "VS"},
        {JournalProbeOp::VsConstF, "VcF"},
        {JournalProbeOp::VsConstI, "VcI"},
        {JournalProbeOp::VsConstB, "VcB"},
        {JournalProbeOp::PixelShader, "PS"},
        {JournalProbeOp::PsConstF, "PcF"},
        {JournalProbeOp::PsConstI, "PcI"},
        {JournalProbeOp::PsConstB, "PcB"},
    }};
    const auto match = std::ranges::find_if(
        kNames,
        [op](const auto& entry) {
            return entry.first == static_cast<JournalProbeOp>(op);
        });
    return match == kNames.end() ? "?" : match->second;
}

} // namespace

void JournalProbeObserveTransaction(const char* techniqueName,
    std::span<const JournalProbeRecord> records, bool truncated)
{
    if (!g_properShadersJournalProbe || records.empty()) return;
    JournalTechniqueStat* stat = FindOrAddJournalStat(techniqueName);
    if (!stat) return;

    ++stat->transactions;
    if (truncated) ++stat->truncatedCount;

    if (stat->slots.empty()) {
        stat->slots.reserve(records.size());
        for (const JournalProbeRecord& record : records) {
            JournalSlotStat s;
            s.op = record.op;
            s.key = record.key;
            s.pass = record.pass;
            s.baseHash = record.hash;
            stat->slots.push_back(s);
        }
        return;
    }
    // Reservoir-free time spreading: a flat comparison cap burned its whole
    // budget in the first seconds (the 08-03 run compared 4096 transactions out
    // of 8.5 MILLION, all of them from the loading frames — none from the scene
    // the user was actually standing in). Sample every Nth transaction instead,
    // so the budget covers the whole session. The stride grows with the
    // technique's own rate, which self-tunes for both per-frame post FX and the
    // thousands-per-frame per-object techniques.
    ++stat->sinceSample;
    if (stat->sinceSample < stat->stride) return;
    stat->sinceSample = 0;
    if (stat->comparisons >= kJournalMaxComparisons) return;
    // Every 256 accepted samples, double the stride (capped). Early samples are
    // dense enough to catch a technique that only runs briefly; later ones thin
    // out so a hot technique keeps contributing across the whole run.
    if (stat->comparisons && (stat->comparisons % 256) == 0 &&
        stat->stride < kJournalMaxStride) {
        stat->stride *= 2;
    }

    // Compare in lockstep. A different op/key at the same index means the
    // technique's write sequence itself varies between objects, which is a
    // stronger form of instability than a value change: record it separately
    // and stop, because slot indices no longer correspond.
    const unsigned n = records.size() < stat->slots.size()
        ? static_cast<unsigned>(records.size())
        : static_cast<unsigned>(stat->slots.size());
    bool structural = (records.size() != stat->slots.size());
    for (unsigned i = 0; i < n; ++i) {
        JournalSlotStat& s = stat->slots[i];
        if (s.op != records[i].op || s.key != records[i].key ||
            s.pass != records[i].pass) {
            structural = true;
            break;
        }
        if (s.baseHash != records[i].hash) ++s.changed;
    }
    if (structural) ++stat->structuralMismatch;
    ++stat->comparisons;
}

void JournalProbeDump()
{
    if (!g_properShadersJournalProbe || g_journalStats.empty()) return;
    if (!EnsureStabReport()) return;

    StabReport("---- journal probe dump (observation only) ----");
    for (const JournalTechniqueStat& s : g_journalStats) {
        if (s.transactions < 2) {
            StabReport("technique={} transactions={} (too few to compare)",
                s.name, s.transactions);
            continue;
        }
        unsigned inv = 0, per = 0;
        for (const JournalSlotStat& sl : s.slots) {
            if (sl.changed == 0) ++inv; else ++per;
        }
        const unsigned total = inv + per;
        StabReport(
            "technique={} transactions={} comparisons={} stride={} slots={} "
            "invariant=%u perObject=%u stableRatio=%.3f structuralMismatch=%u truncated=%u",
            s.name, s.transactions, s.comparisons, s.stride, total, inv, per,
            total ? double(inv) / double(total) : 0.0,
            s.structuralMismatch, s.truncatedCount);
        // List the unstable slots: these are the ones that force per-draw work.
        for (unsigned i = 0; i < s.slots.size(); ++i) {
            const JournalSlotStat& sl = s.slots[i];
            if (!sl.changed) continue;
            StabReport("  slot[{:3}] {:<7} key=0x{:08X} pass={} changed={}/{}",
                i, JournalOpName(sl.op), sl.key, sl.pass,
                sl.changed, s.comparisons);
        }
    }
    StabReport("---- end journal dump ----");
}

// ---- Transaction attribution (observation only) -----------------------------

namespace {

constexpr unsigned kJournalAttributionOpCount = 18;
constexpr unsigned kJournalAttributionMaxTechniques = 96;
constexpr unsigned kJournalAttributionMaxPasses = 32;
constexpr std::uint32_t kJournalAttributionPrePass = 0xFFFFFFFFu;

struct JournalAttributionPassStat
{
    std::uint32_t pass = kJournalAttributionPrePass;
    std::uint64_t transactions = 0;
    std::uint64_t records = 0;
    std::uint64_t ops[kJournalAttributionOpCount]{};
};

struct JournalAttributionTechniqueStat
{
    char name[64]{};
    std::uint64_t transactions = 0;
    std::uint64_t emptyTransactions = 0;
    std::uint64_t truncated = 0;
    std::uint64_t nativeTransactions = 0;
    std::uint64_t localTransactions = 0;
    std::uint64_t records = 0;
    std::uint64_t totalQpc = 0;
    std::uint64_t maxQpc = 0;
    std::uint64_t restoreCount = 0;
    std::uint64_t restoreQpc = 0;
    std::uint64_t maxRestoreQpc = 0;
    unsigned passCount = 0;
    JournalAttributionPassStat passes[kJournalAttributionMaxPasses]{};
};

JournalAttributionTechniqueStat g_journalAttribution[
    kJournalAttributionMaxTechniques]{};
SRWLOCK g_journalAttributionLock = SRWLOCK_INIT;
unsigned g_journalAttributionTechniqueCount = 0;
std::uint64_t g_journalAttributionFrame = 0;
std::uint64_t g_journalAttributionPresentCount = 0;
std::uint64_t g_journalAttributionDroppedTechniques = 0;
std::uint64_t g_journalAttributionDroppedPasses = 0;
std::uint64_t g_journalAttributionOrphanRestores = 0;
std::atomic<LONG> g_journalAttributionInFlight{ 0 };

thread_local JournalAttributionTechniqueStat* g_pendingAttributionTechnique = nullptr;
thread_local std::uint64_t g_pendingAttributionSequence = 0;

JournalAttributionTechniqueStat* FindAttributionTechnique(const char* name)
{
    const char* normalized = name && *name ? name : "<unknown>";
    for (unsigned i = 0; i < g_journalAttributionTechniqueCount; ++i) {
        if (std::strcmp(g_journalAttribution[i].name, normalized) == 0) {
            return &g_journalAttribution[i];
        }
    }
    if (g_journalAttributionTechniqueCount >= kJournalAttributionMaxTechniques) {
        ++g_journalAttributionDroppedTechniques;
        return nullptr;
    }
    JournalAttributionTechniqueStat& result =
        g_journalAttribution[g_journalAttributionTechniqueCount++];
    FormatTo(result.name, sizeof(result.name), "{}", normalized);
    return &result;
}

JournalAttributionPassStat* FindAttributionPass(
    JournalAttributionTechniqueStat& technique, std::uint32_t pass)
{
    for (unsigned i = 0; i < technique.passCount; ++i) {
        if (technique.passes[i].pass == pass) return &technique.passes[i];
    }
    if (technique.passCount >= kJournalAttributionMaxPasses) {
        ++g_journalAttributionDroppedPasses;
        return nullptr;
    }
    JournalAttributionPassStat& result = technique.passes[technique.passCount++];
    result.pass = pass;
    return &result;
}

unsigned AttributionOpIndex(std::uint8_t op)
{
    if (op < static_cast<std::uint8_t>(JournalProbeOp::Transform) ||
        op > static_cast<std::uint8_t>(JournalProbeOp::PsConstB)) {
        return kJournalAttributionOpCount;
    }
    return static_cast<unsigned>(op - 1);
}

const char* AttributionPassName(std::uint32_t pass)
{
    return pass == kJournalAttributionPrePass ? "begin" : "pass";
}

void DumpAttributionOps(FILE* report, const JournalAttributionPassStat& pass)
{
    for (unsigned i = 0; i < kJournalAttributionOpCount; ++i) {
        if (!pass.ops[i]) continue;
        std::print(report, "    op={} count={}\n",
            JournalOpName(static_cast<unsigned char>(i + 1)),
            static_cast<unsigned long long>(pass.ops[i]));
    }
}

} // namespace

void JournalAttributionObserveTransaction(const char* techniqueName,
    std::span<const JournalProbeRecord> records, bool truncated,
    const JournalProbeTransactionInfo& info)
{
    // The journal admits a transaction before the capture can be stopped. Do
    // not re-check the global flag here: a transaction already in flight must
    // still publish its final record set and restore timing.
    AcquireSRWLockExclusive(&g_journalAttributionLock);
    JournalAttributionTechniqueStat* technique = FindAttributionTechnique(techniqueName);
    if (!technique) {
        ReleaseSRWLockExclusive(&g_journalAttributionLock);
        return;
    }

    ++technique->transactions;
    technique->records += records.size();
    technique->totalQpc += info.qpcTicks;
    if (info.qpcTicks > technique->maxQpc) technique->maxQpc = info.qpcTicks;
    if (info.native) ++technique->nativeTransactions;
    else ++technique->localTransactions;
    if (truncated) ++technique->truncated;

    std::uint32_t seenPasses[kJournalAttributionMaxPasses]{};
    unsigned seenPassCount = 0;
    for (const JournalProbeRecord& record : records) {
        JournalAttributionPassStat* pass = FindAttributionPass(
            *technique, record.pass);
        if (!pass) continue;
        bool firstInTransaction = true;
        for (unsigned seen = 0; seen < seenPassCount; ++seen) {
            if (seenPasses[seen] == record.pass) {
                firstInTransaction = false;
                break;
            }
        }
        if (firstInTransaction && seenPassCount < kJournalAttributionMaxPasses) {
            seenPasses[seenPassCount++] = record.pass;
            ++pass->transactions;
        }
        ++pass->records;
        const unsigned op = AttributionOpIndex(record.op);
        if (op < kJournalAttributionOpCount) ++pass->ops[op];
    }
    if (records.empty()) {
        ++technique->emptyTransactions;
        if (JournalAttributionPassStat* pass = FindAttributionPass(
                *technique, kJournalAttributionPrePass)) {
            ++pass->transactions;
        }
    }

    g_pendingAttributionTechnique = technique;
    g_pendingAttributionSequence = info.sequence;
    ReleaseSRWLockExclusive(&g_journalAttributionLock);
}

void JournalAttributionObserveRestore(std::uint64_t sequence,
    std::uint64_t qpcTicks)
{
    AcquireSRWLockExclusive(&g_journalAttributionLock);
    if (!g_pendingAttributionTechnique ||
        g_pendingAttributionSequence != sequence) {
        ++g_journalAttributionOrphanRestores;
        ReleaseSRWLockExclusive(&g_journalAttributionLock);
        return;
    }
    JournalAttributionTechniqueStat& technique = *g_pendingAttributionTechnique;
    ++technique.restoreCount;
    technique.restoreQpc += qpcTicks;
    if (qpcTicks > technique.maxRestoreQpc) technique.maxRestoreQpc = qpcTicks;
    g_pendingAttributionTechnique = nullptr;
    g_pendingAttributionSequence = 0;
    ReleaseSRWLockExclusive(&g_journalAttributionLock);
}

void JournalAttributionReset()
{
    AcquireSRWLockExclusive(&g_journalAttributionLock);
    std::memset(g_journalAttribution, 0, sizeof(g_journalAttribution));
    g_journalAttributionTechniqueCount = 0;
    g_journalAttributionFrame = 0;
    g_journalAttributionPresentCount = 0;
    g_journalAttributionDroppedTechniques = 0;
    g_journalAttributionDroppedPasses = 0;
    g_journalAttributionOrphanRestores = 0;
    g_pendingAttributionTechnique = nullptr;
    g_pendingAttributionSequence = 0;
    ReleaseSRWLockExclusive(&g_journalAttributionLock);
}

void JournalAttributionStartCapture()
{
    JournalAttributionReset();
    AcquireSRWLockExclusive(&g_journalAttributionLock);
    g_properShadersStateAttributionActive.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_journalAttributionLock);
}

void JournalAttributionStopCapture()
{
    AcquireSRWLockExclusive(&g_journalAttributionLock);
    g_properShadersStateAttributionActive.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_journalAttributionLock);
    while (g_journalAttributionInFlight.load(std::memory_order_acquire) != 0) {
        Sleep(0);
    }
}

bool JournalAttributionTryBeginTransaction()
{
    AcquireSRWLockExclusive(&g_journalAttributionLock);
    const bool active = g_properShadersStateAttributionActive.load(
        std::memory_order_acquire);
    if (active) g_journalAttributionInFlight.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_journalAttributionLock);
    return active;
}

void JournalAttributionEndTransaction()
{
    g_journalAttributionInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void JournalAttributionOnPresent()
{
    // An admitted transaction may finish after the capture flag is cleared.
    // Its final record and restore timing must still be published.
    if (!g_properShadersStateAttributionActive.load(std::memory_order_acquire)) {
        return;
    }
    AcquireSRWLockExclusive(&g_journalAttributionLock);
    // StopCapture can race with the polling thread between its first check and
    // this lock acquisition. Do not count a Present that belongs to the next
    // capture window.
    if (g_properShadersStateAttributionActive.load(std::memory_order_acquire)) {
        ++g_journalAttributionFrame;
        ++g_journalAttributionPresentCount;
    }
    ReleaseSRWLockExclusive(&g_journalAttributionLock);
}

std::uint64_t JournalAttributionCurrentFrame()
{
    AcquireSRWLockShared(&g_journalAttributionLock);
    const std::uint64_t frame = g_journalAttributionFrame;
    ReleaseSRWLockShared(&g_journalAttributionLock);
    return frame;
}

void JournalAttributionDump()
{
    AcquireSRWLockShared(&g_journalAttributionLock);
    if (!g_journalAttributionTechniqueCount) {
        ReleaseSRWLockShared(&g_journalAttributionLock);
        return;
    }
    FILE* report = std::fopen("scripts\\BridgeD3D9.state-attribution.log", "a");
    if (!report) {
        ReleaseSRWLockShared(&g_journalAttributionLock);
        return;
    }

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double qpcToMs = frequency.QuadPart
        ? 1000.0 / static_cast<double>(frequency.QuadPart)
        : 0.0;
    auto line = [report]<class... Args>(
                     std::format_string<Args...> fmt, Args&&... args) {
        std::vprint_nonunicode(
            report, fmt.get(), std::make_format_args(args...));
        std::fputc('\n', report);
    };
    line("---- ProperShaders state attribution (observation only) ----");
    line("frames={} presents={} techniques={} droppedTechniques={} droppedPasses={} orphanRestores={} qpcToMs={:.9f}",
        g_journalAttributionFrame,
        g_journalAttributionPresentCount,
        g_journalAttributionTechniqueCount,
        g_journalAttributionDroppedTechniques,
        g_journalAttributionDroppedPasses,
        g_journalAttributionOrphanRestores,
        qpcToMs);
    for (unsigned i = 0; i < g_journalAttributionTechniqueCount; ++i) {
        const JournalAttributionTechniqueStat& technique = g_journalAttribution[i];
        const double avgMs = technique.transactions && qpcToMs
            ? static_cast<double>(technique.totalQpc) * qpcToMs /
                static_cast<double>(technique.transactions)
            : 0.0;
        const double restoreAvgMs = technique.restoreCount && qpcToMs
            ? static_cast<double>(technique.restoreQpc) * qpcToMs /
                static_cast<double>(technique.restoreCount)
            : 0.0;
        line("technique={} transactions={} records={} avgMs={:.6f} maxMs={:.6f} native={} local={} truncated={} empty={} restoreCount={} restoreAvgMs={:.6f} restoreMaxMs={:.6f}",
            technique.name,
            technique.transactions,
            technique.records,
            avgMs,
            static_cast<double>(technique.maxQpc) * qpcToMs,
            technique.nativeTransactions,
            technique.localTransactions,
            technique.truncated,
            technique.emptyTransactions,
            technique.restoreCount,
            restoreAvgMs,
            static_cast<double>(technique.maxRestoreQpc) * qpcToMs);
        for (unsigned passIndex = 0; passIndex < technique.passCount; ++passIndex) {
            const JournalAttributionPassStat& pass = technique.passes[passIndex];
            line("  pass={} index={} transactions={} records={}",
                AttributionPassName(pass.pass), pass.pass,
                pass.transactions,
                pass.records);
            DumpAttributionOps(report, pass);
        }
    }
    line("---- end ProperShaders state attribution ----");
    std::fclose(report);
    ReleaseSRWLockShared(&g_journalAttributionLock);
}
