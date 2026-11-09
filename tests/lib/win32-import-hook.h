#pragma once
#include <windows.h>
#include <cstring>

// Single-threaded test harness utility. The loaded module outlives this object.
struct ImportHook {
    ULONG_PTR* slot = nullptr;
    ULONG_PTR original = 0;

    bool install(HMODULE module, const char* name, void* replacement) {
        auto base = reinterpret_cast<unsigned char*>(module);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for (; descriptor->Name; ++descriptor) {
            if (!descriptor->OriginalFirstThunk)
                continue;
            auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
            auto addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++addresses) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                    continue;
                auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<char*>(import->Name), name))
                    continue;
                slot = reinterpret_cast<ULONG_PTR*>(&addresses->u1.Function);
                original = *slot;
                DWORD oldProtection = 0;
                if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection))
                    return false;
                *slot = reinterpret_cast<ULONG_PTR>(replacement);
                DWORD ignored;
                return VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored) != FALSE;
            }
        }
        return false;
    }

    ~ImportHook() {
        if (!slot)
            return;
        DWORD oldProtection = 0;
        if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
            *slot = original;
            DWORD ignored;
            VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
        }
    }
};
