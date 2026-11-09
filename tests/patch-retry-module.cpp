#include <windows.h>

// Deliberately incompatible PE image, large enough for every preflight RVA.
// The fixture never invokes a game hook or loads a real MOD.
static volatile unsigned char padding[2 * 1024 * 1024] = {0xcc};
extern "C" __declspec(dllexport) const volatile void* FixtureImage() {
    return padding;
}
