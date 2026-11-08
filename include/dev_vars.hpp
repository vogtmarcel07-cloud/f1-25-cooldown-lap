// Named pointers an addon publishes for the Dev Bridge console ($name there). Developer aid only: a small named
// memory block per game process; publishing costs one string compare per slot.
#pragma once

#include <cstdint>
#include <cstring>

#include <windows.h>

namespace devvars {

struct Slot {
    char name[24];
    uintptr_t value;
};
constexpr int kSlots = 64;

inline Slot *table()
{
    static Slot *t = [] {
        wchar_t name[64];
        wsprintfW(name, L"Local\\vmknolle_dev_vars_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Slot) * kSlots, name);
        return h ? static_cast<Slot *>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Slot) * kSlots)) : nullptr;
    }();
    return t;
}

inline void publish(const char *name, uintptr_t value)
{
    Slot *t = table();
    if (!t)
        return;
    for (int i = 0; i < kSlots; i++)
        if (!t[i].name[0] || !std::strncmp(t[i].name, name, sizeof t[i].name - 1)) {
            if (!t[i].name[0])
                std::strncpy(t[i].name, name, sizeof t[i].name - 1);
            t[i].value = value;
            return;
        }
}

inline bool lookup(const char *name, uintptr_t &out)
{
    Slot *t = table();
    for (int i = 0; t && i < kSlots && t[i].name[0]; i++)
        if (!std::strncmp(t[i].name, name, sizeof t[i].name - 1))
            return out = t[i].value, true;
    return false;
}

}
