#include "test_check.hpp"
#include "../../ProperShadersRenderStateGuard.h"
// The test process heap can reserve GTA's absolute API pages. Supply only
// those external API addresses from disposable executable memory; keep the
// production installer, hook target, gateway and shutdown code unchanged.
static uintptr_t fixtureSetState, fixtureGetState;
#define kRwD3D9SetRenderState fixtureSetState
#define kRwD3D9GetRenderState fixtureGetState
// Compile the production installer/guard/gateway in this executable so the
// regression exercises their actual patch and call paths, including shutdown.
#include "../../ProperShadersRenderStateGuard.cpp"
#undef kRwD3D9SetRenderState
#undef kRwD3D9GetRenderState

static_assert(sizeof(void*) == 4, "The ProperShaders hook contract is x86.");

namespace {
uint8_t* image;
unsigned loggerCalls, bodyCalls;
DWORD beforeEsp, afterEsp, beforeEbp, afterEbp, afterEbx, afterEsi, afterEdi, afterEcx;
constexpr DWORD sentinel = 0x13572468;

void __cdecl LoggerProbe(void* context, const char* name) {
  ++loggerCalls;
  CHECK(name == reinterpret_cast<char*>(image + 0x14156C));
  CHECK(context == image + 0x16C108);
}
void __cdecl BodyProbe() {
  ++bodyCalls;
  CHECK(image[0x16BF96] == 1);
  __asm { xor ecx, ecx }
  __asm { mov edx, 55667788h }
}

__declspec(naked) void Invoke(void* target) {
  __asm {
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov ebx, 11223344h
    mov esi, 22334455h
    mov edi, 33445566h
    mov ecx, sentinel
    mov eax, [ebp + 8]
    mov beforeEsp, esp
    mov beforeEbp, ebp
    call eax
    mov afterEsp, esp
    mov afterEbp, ebp
    mov afterEcx, ecx
    mov afterEbx, ebx
    mov afterEsi, esi
    mov afterEdi, edi
    lea esp, [ebp - 12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
  }
}
DWORD InvokeCaught(void* target) {
  __try { Invoke(target); return 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
void Word(uint8_t* destination, uintptr_t value) {
  const auto word = static_cast<uint32_t>(value);
  std::memcpy(destination, &word, sizeof(word));
}
void MakeFunction() {
  // ProperShaders 526DBD01 RenderFadingEntities, with only its absolute
  // operands and logger rel32 relocated into this disposable executable image.
  const uint8_t bytes[] = {
    0x51, 0x68,0,0,0,0, 0x68,0,0,0,0, 0xE8,0,0,0,0, 0x83,0xC4,0x08,
    0xC6,0x05,0,0,0,0,0x01, 0xFF,0x15,0,0,0,0,
    0xC6,0x05,0,0,0,0,0x00, 0x59,0xC3
  };
  auto* entry = image + 0x25590;
  DWORD old;
  CHECK(VirtualProtect(entry, sizeof(bytes), PAGE_EXECUTE_READWRITE, &old));
  std::memcpy(entry, bytes, sizeof(bytes));
  Word(entry + 2, uintptr_t(image + 0x14156C));
  Word(entry + 7, uintptr_t(image + 0x16C108));
  Word(entry + 12, uintptr_t(&LoggerProbe) - uintptr_t(entry + 16));
  Word(entry + 21, uintptr_t(image + 0x16BF96));
  Word(entry + 28, uintptr_t(image + 0x16C104));
  Word(entry + 34, uintptr_t(image + 0x16BF96));
  Word(image + 0x16C104, uintptr_t(&BodyProbe));
  CHECK(FlushInstructionCache(GetCurrentProcess(), entry, sizeof(bytes)));
  CHECK(VirtualProtect(entry, sizeof(bytes), PAGE_EXECUTE_READ, &old));
}
void VerifyCall(void* target, bool gateway) {
  const auto oldLogger = loggerCalls, oldBody = bodyCalls;
  const DWORD exception = InvokeCaught(target);
  std::printf("call gateway=%d exception=0x%08lX\n", gateway, exception);
  CHECK(exception == 0);
  if (exception) return;
  CHECK(beforeEsp == afterEsp);
  CHECK(beforeEbp == afterEbp);
  CHECK(afterEbx == 0x11223344);
  CHECK(afterEsi == 0x22334455);
  CHECK(afterEdi == 0x33445566);
  // ECX is volatile at the C++ guard boundary. The original/gateway pair
  // independently verifies the saved stack slot is consumed by pop ecx.
  if (gateway) CHECK(afterEcx == sentinel);
  CHECK(loggerCalls == oldLogger + 1);
  CHECK(bodyCalls == oldBody + 1);
  CHECK(image[0x16BF96] == 0);
}

void RoundTrip(bool trace) {
  MakeFunction();
  auto* entry = image + 0x25590;
  std::array<uint8_t, 41> original;
  std::memcpy(original.data(), entry, original.size());
  VerifyCall(entry, true);
  g_properShadersRenderStateGuardConfig.enable = true;
  g_properShadersRenderStateGuardConfig.enableStateRing = trace;
  if (trace) CHECK(BeginProperShadersStateTraceCapture());
  InstallProperShadersFadingStateGuard(reinterpret_cast<HMODULE>(image), kSupportedProperShadersTextHash);
  CHECK(g_properShadersFadingStateGuardInstallState == 2);
  CHECK(g_properShadersRenderFadingEntitiesTrampoline != 0);
  const auto gateway = g_properShadersRenderFadingEntitiesTrampoline;
  CHECK(entry[0] == 0xE9 && entry[5] == 0x90);
  CHECK(DecodeRel32JumpTarget(uintptr_t(entry)) ==
    uintptr_t(&Bridge_ProperShadersRenderFadingEntitiesGuard));
  MEMORY_BASIC_INFORMATION memory{};
  CHECK(VirtualQuery(entry, &memory, sizeof(memory)) == sizeof(memory));
  CHECK(memory.Protect == PAGE_EXECUTE_READ);
  InstallProperShadersFadingStateGuard(reinterpret_cast<HMODULE>(image), kSupportedProperShadersTextHash);
  CHECK(g_properShadersRenderFadingEntitiesTrampoline == gateway);
  if (gateway) {
    CHECK(std::memcmp(reinterpret_cast<void*>(gateway), original.data(), 6) == 0);
    CHECK(DecodeRel32JumpTarget(gateway + 6) == uintptr_t(entry + 6));
    VerifyCall(reinterpret_cast<void*>(gateway), true);
    VerifyCall(entry, false);
    VerifyCall(entry, false);
  }
  CHECK(ShutdownProperShadersRenderStateGuards());
  CHECK(g_properShadersRenderFadingEntitiesTrampoline == 0);
  CHECK(g_properShadersFadingStateGuardInstallState == 0);
  CHECK(std::memcmp(original.data(), entry, original.size()) == 0);
  CHECK(VirtualQuery(entry, &memory, sizeof(memory)) == sizeof(memory));
  CHECK(memory.Protect == PAGE_EXECUTE_READ);
  VerifyCall(entry, true);
}
void RejectWrongEntry() {
  for (size_t offset : {size_t(0), size_t(1), size_t(2)}) {
    MakeFunction();
    auto* entry = image + 0x25590;
    DWORD old, ignored;
    CHECK(VirtualProtect(entry, 6, PAGE_EXECUTE_READWRITE, &old));
    entry[offset] ^= 1; // Independently reject push ecx, push imm32 and its address.
    CHECK(FlushInstructionCache(GetCurrentProcess(), entry, 6));
    CHECK(VirtualProtect(entry, 6, old, &ignored));
    std::array<uint8_t, 6> before;
    std::memcpy(before.data(), entry, before.size());
    InstallProperShadersFadingStateGuard(reinterpret_cast<HMODULE>(image), kSupportedProperShadersTextHash);
    CHECK(g_properShadersFadingStateGuardInstallState == 0);
    CHECK(g_properShadersRenderFadingEntitiesTrampoline == 0);
    CHECK(std::memcmp(before.data(), entry, before.size()) == 0);
    CHECK(ShutdownProperShadersRenderStateGuards());
  }
  MakeFunction();
  InstallProperShadersFadingStateGuard(reinterpret_cast<HMODULE>(image), 0);
  CHECK(g_properShadersFadingStateGuardInstallState == 0);
}
}

int main() {
  auto* apiPage = VirtualAlloc(nullptr, 4096,
    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  image = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x180000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  CHECK(image != nullptr);
  CHECK(apiPage != nullptr);
  if (!image || !apiPage) return api_test::finish();
  fixtureSetState = uintptr_t(apiPage);
  fixtureGetState = uintptr_t(apiPage) + 16;
  *reinterpret_cast<uint8_t*>(fixtureSetState) = 0xC3;
  *reinterpret_cast<uint8_t*>(fixtureGetState) = 0xC3;
  RoundTrip(false);
  RoundTrip(true);
  RejectWrongEntry();
  CHECK(ShutdownProperShadersRenderStateGuards());
  CHECK(VirtualFree(apiPage, 0, MEM_RELEASE));
  CHECK(VirtualFree(image, 0, MEM_RELEASE));
  return api_test::finish();
}
