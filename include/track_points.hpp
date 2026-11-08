// Track points read from the running game instead of the stock tables (pit_exit_table.hpp, parc_ferme_table.hpp),
// so the cool-down, the red flag queue and the warm-up also work on tracks that are not stock (own tracks, a track
// whose length is within 3 % of a stock one would get that track's points from the tables).
//
// Parc ferme: the cutscene placement registry [g_Cutscene_Placements] (0x14607c1d8) that CutSceneScript::setLocation
// (0x140241330) searches by name: entries at +0x278..+0x280, entry +0x18 = name (16 bytes, zero padded), entry +0x8 ->
// placement component; its world matrix is [comp+0xf0] -> {+0x10 base, +0x18 end} [comp+0xf8] * 0x70 + 0x20 when that
// index is in range, else comp+0x90 (4x4, translation +0x30). The podium scene asks for "ParcFerme".
//
// Pit exit: learned from the game's own cars: where a car changes LeavingPitLane -> OnTrack is the end of the pit
// lane; the queue head is where that car was one second earlier (the stock tables use 20 m before the end).
//
// Lap fraction of a point: a ring of (x, z) samples per 1/512 of the lap, filled from the cars on track.
//
// One block per game process (named memory), shared between the modules.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include <windows.h>

#include <swofty/mem.hpp>

namespace trackpts {

constexpr int kBins = 512;

struct Block {
    float length;            // track length the block belongs to (a new track clears it)
    int32_t exit_ok;
    float exit_head[3], exit_end[3];
    int32_t pf_ok;           // parc ferme placement found
    float pf[3];
    float bin[kBins][2];     // x, z per 1/512 of the lap
    uint8_t bin_ok[kBins];
};

inline Block *block()
{
    static Block *b = [] {
        wchar_t name[64];
        wsprintfW(name, L"Local\\vmknolle_track_points_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Block), name);
        return h ? static_cast<Block *>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block))) : nullptr;
    }();
    return b;
}

// A different track length: everything learned belongs to another track.
inline Block *for_track(float length)
{
    Block *b = block();
    if (b && length > 0 && std::fabs(b->length - length) > 1.0f) {
        std::memset(b, 0, sizeof *b);
        b->length = length;
    }
    return b;
}

template <class T> bool rdv(uintptr_t a, T &out)
{
    if (!a || !swofty::mem::readable(reinterpret_cast<void *>(a), sizeof(T)))
        return false;
    std::memcpy(&out, reinterpret_cast<void *>(a), sizeof(T));
    return true;
}

// World position of a cutscene placement by name (at most 15 characters).
// World matrix (4x4, row major, translation in row 3) of a cutscene placement by name.
inline bool placement_matrix(void *registry, const char *name, float out[16])
{
    auto reg = reinterpret_cast<uintptr_t>(registry);
    char want[16] = {};
    std::strncpy(want, name, 15);
    uintptr_t it = 0, end = 0;
    if (!rdv(reg + 0x278, it) || !rdv(reg + 0x280, end) || end < it || end - it > 8 * 4096)
        return false;
    for (; it < end; it += 8) {
        uintptr_t e = 0, comp = 0, arr = 0;
        char got[16];
        if (!rdv(it, e) || !rdv(e + 0x18, got) || std::memcmp(got, want, 16) || !rdv(e + 8, comp) || !comp)
            continue;
        uintptr_t mtx = comp + 0x90;
        uintptr_t base = 0, last = 0;
        uint32_t idx = 0;
        if (rdv(comp + 0xf0, arr) && arr && rdv(arr + 0x10, base) && base && rdv(arr + 0x18, last) &&
            rdv(comp + 0xf8, idx) && last > base && idx < (last - base) / 0x70)
            mtx = base + size_t(idx) * 0x70 + 0x20;
        float m[16];
        if (!rdv(mtx, m) || !std::isfinite(m[12]) || !std::isfinite(m[14]))
            return false;
        std::memcpy(out, m, sizeof m);
        return true;
    }
    return false;
}

inline bool placement(void *registry, const char *name, float out[3])
{
    auto reg = reinterpret_cast<uintptr_t>(registry);
    char want[16] = {};
    std::strncpy(want, name, 15);
    uintptr_t it = 0, end = 0;
    if (!rdv(reg + 0x278, it) || !rdv(reg + 0x280, end) || end < it || end - it > 8 * 4096)
        return false;
    for (; it < end; it += 8) {
        uintptr_t e = 0, comp = 0, arr = 0;
        char got[16];
        if (!rdv(it, e) || !rdv(e + 0x18, got) || std::memcmp(got, want, 16) || !rdv(e + 8, comp) || !comp)
            continue;
        uintptr_t mtx = comp + 0x90;
        uintptr_t base = 0, last = 0;
        uint32_t idx = 0;
        if (rdv(comp + 0xf0, arr) && arr && rdv(arr + 0x10, base) && base && rdv(arr + 0x18, last) &&
            rdv(comp + 0xf8, idx) && last > base && idx < (last - base) / 0x70)
            mtx = base + size_t(idx) * 0x70 + 0x20;
        float t[3];
        if (!rdv(mtx + 0x30, t) || !std::isfinite(t[0]) || !std::isfinite(t[2]))
            return false;
        std::memcpy(out, t, sizeof t);
        return true;
    }
    return false;
}

// A car on track at lap fraction frac is at (x, z).
inline void sample(Block *b, float frac, float x, float z)
{
    if (!b || !(frac >= 0 && frac < 1))
        return;
    int i = int(frac * kBins) % kBins;
    b->bin[i][0] = x, b->bin[i][1] = z;
    b->bin_ok[i] = 1;
}

// Lap fraction of the sample nearest to (x, z); dist = its distance in m. False before any sample.
inline bool fraction_of(const Block *b, float x, float z, float &frac, float &dist)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; b && i < kBins; i++)
        if (b->bin_ok[i]) {
            float dx = b->bin[i][0] - x, dz = b->bin[i][1] - z, d = dx * dx + dz * dz;
            if (d < bd)
                bd = d, best = i;
        }
    if (best < 0)
        return false;
    frac = (best + 0.5f) / kBins;
    dist = std::sqrt(bd);
    return true;
}

inline int samples(const Block *b)
{
    int n = 0;
    for (int i = 0; b && i < kBins; i++)
        n += b->bin_ok[i];
    return n;
}

}
