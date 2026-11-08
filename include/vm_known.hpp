// Addresses found by any vmknolle public addon of this process (named memory, like rd_shared.hpp). Once one addon
// hooked a function with an own hook its pattern no longer matches, so the next addon (own_hooks.hpp, and the
// local bindings in swofty/mixin.hpp) takes the address from here.
#pragma once

#include <cstdint>
#include <cstring>

#include <windows.h>

namespace vm_known {

struct Entry {
    char name[56];
    uint64_t addr;
};
constexpr int kCount = 64;

inline Entry *table()
{
    static Entry *k = [] {
        wchar_t name[64];
        wsprintfW(name, L"Local\\vmknolle_own_hooks_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Entry) * kCount, name);
        return h ? static_cast<Entry *>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Entry) * kCount)) : nullptr;
    }();
    return k;
}

inline uintptr_t get(const char *name)
{
    Entry *k = table();
    for (int i = 0; k && i < kCount && k[i].name[0]; i++)
        if (!std::strncmp(k[i].name, name, sizeof k[i].name))
            return static_cast<uintptr_t>(k[i].addr);
    return 0;
}

inline void put(const char *name, uintptr_t a)
{
    Entry *k = table();
    for (int i = 0; k && i < kCount; i++) {
        if (k[i].name[0] && std::strncmp(k[i].name, name, sizeof k[i].name))
            continue;
        k[i].addr = a;
        strncpy_s(k[i].name, name, _TRUNCATE);
        return;
    }
}

}
