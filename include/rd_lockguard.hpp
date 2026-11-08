// Lock guard shared by the vmknolle addons that touch cars (Race Start, Red Flag, SC Unlapping).
// The player's car can be left with a controller flag that hands it to the AI although the game no longer needs it
// (pit lane AI on a car out on the track, seen 2026-10-25 17:45; a red flag / pre-start flag after the race goes
// on). Each addon calls rd::unlock_player() every frame; it clears those flags through the game's own
// setController(car, human, reason), at most every 250 ms across all addons.
//
// It acts only when the game's race state (Session Watch, rd::Race) says it is safe: in a session, the start hold
// over, no red flag, no red flag pit hold, no autopilot, and the player's car out on the track (pit status OnTrack
// or Invalid). Without Session Watch it does nothing.
#pragma once

#include <cstdint>

#include <windows.h>

#include <swofty/mem.hpp>

#include "dev_vars.hpp"
#include "rd_shared.hpp"

namespace rd {

// setController reasons (the game's table 0x1457d7b10).
enum CtrlReason : int { kCtrlPitLane = 0, kCtrlSessionNotStarted = 10, kCtrlSafetyCar = 11, kCtrlReformGrid = 13 };
constexpr uint32_t kLeftoverLocks =
    (1u << kCtrlPitLane) | (1u << kCtrlSessionNotStarted) | (1u << kCtrlSafetyCar) | (1u << kCtrlReformGrid);

// The controller flag bits of a car (VehicleControllerSystem +0x30 entries, entry +0x30).
inline uint32_t ctrl_flags(void *system, int vehicle)
{
    auto sys = reinterpret_cast<uintptr_t>(system);
    uintptr_t arr = 0, end = 0, e = 0;
    uint32_t f = 0;
    if (!sys || vehicle < 0 || !swofty::mem::read(sys + 0x30, arr) || !swofty::mem::read(sys + 0x38, end) ||
        arr + size_t(vehicle) * 8 + 8 > end || !swofty::mem::read(arr + size_t(vehicle) * 8, e) || !e ||
        !swofty::mem::read(e + 0x30, f))
        return 0;
    return f;
}

inline std::atomic<uint64_t> &guard_clock()
{
    static std::atomic<uint64_t> *t = [] {
        wchar_t name[64];
        wsprintfW(name, L"Local\\vmknolle_race_director_guard_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(uint64_t), name);
        void *p = h ? MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(uint64_t)) : nullptr;
        static std::atomic<uint64_t> local{0};
        return p ? static_cast<std::atomic<uint64_t> *>(p) : &local;
    }();
    return *t;
}

// --- Warm-up and red flag: the player's car is never locked (user 2026-10-25 21:41) -----------------------------
// While the warm-up runs (Race Start: kWarmupActive) or a red flag is out (Session Watch, or the Red Flag addon's
// pit hold), every AI takeover of the player's car is dropped at the game's setController call, so the car is never
// held: no pit lane AI after the car has left its garage (garage states 9..13 keep it, the game moves the car out of
// the box), no SAFETY_CAR, SESSION_NOT_STARTED (red flag freeze), grid reform, rolling start or cinematic SC.
inline bool free_phase()
{
    uintptr_t hold = 0;
    return mode(kWarmupActive) == kOn || red_flag_on() || (devvars::lookup("rf_hold", hold) && hold == 1);
}

inline uint32_t free_phase_blocked(int pit_status)
{
    uint32_t b = (1u << 1) /* ROLLING_START */ | (1u << kCtrlSessionNotStarted) | (1u << kCtrlSafetyCar) |
                 (1u << kCtrlReformGrid) | (1u << 16) /* CINEMATIC_SAFETY_CAR */;
    if (pit_status < 9 || pit_status > 13)
        b |= 1u << kCtrlPitLane;
    return b;
}

inline int player_id(int fallback)
{
    Race *r = race();
    return r && r->player < 32 ? r->player : fallback;
}

// For every addon's setController wrap: true = drop this call (an AI takeover of the player's car in the warm-up or
// under a red flag). fallback_player = the addon's own idea of the player's id (-1 = none).
inline bool block_lock(uint64_t vehicle, uint32_t ai, int32_t flag, int fallback_player, const char *who)
{
    if (!ai || flag < 0 || flag > 31 || mode(kAutopilot) == kOn || !free_phase())
        return false;
    int v = int(vehicle & 0xff), pl = player_id(fallback_player);
    if (v != pl)
        return false;
    Race *r = race();
    int st = r && race_live() ? r->pit[v] : 1;
    if (!(free_phase_blocked(st) & (1u << flag)))
        return false;
    static uint32_t s_logged = 0;
    if (!(s_logged & (1u << flag))) {
        s_logged |= 1u << flag;
        swofty::log("[%s] no lock: AI takeover of the player's car (reason %d, pit status %d) dropped (%s)", who, flag,
                    st, mode(kWarmupActive) == kOn ? "warm-up" : "red flag");
    }
    return true;
}

// Clears what is still set on the player's car in the warm-up / under a red flag: the controller flags above (at once,
// wherever the car is) and the start hold bits of the VehicleAntiStallSystem entry (input +0x1fc bit 0x2, controller
// +0x1c bit 0x20; entries +0x48..+0x50, entry +0x48 -> +0x38 id, +0x50 vehicle input, +0x58 controller).
template <class SetController>
uint32_t free_player(void *system, SetController set, void *antistall, int fallback_player, const char *who)
{
    if (mode(kAutopilot) == kOn || !free_phase())
        return 0;
    int pl = player_id(fallback_player);
    if (pl < 0 || pl >= 32)
        return 0;
    Race *r = race();
    int st = r && race_live() ? r->pit[pl] : 1;
    uint32_t unwanted = system ? ctrl_flags(system, pl) & free_phase_blocked(st) : 0;
    for (int b = 0; b < 18; b++)
        if (unwanted & (1u << b))
            set(system, uint64_t(pl), 0, b);
    if (auto as = reinterpret_cast<uintptr_t>(antistall)) {
        uintptr_t it = 0, end = 0;
        if (swofty::mem::read(as + 0x48, it) && swofty::mem::read(as + 0x50, end))
            for (; it && it < end; it += 8) {
                uintptr_t e = 0, id_obj = 0, input = 0, ctrl = 0;
                uint8_t id = 0xff;
                if (!swofty::mem::read(it, e) || !e || !swofty::mem::read(e + 0x48, id_obj) ||
                    !swofty::mem::read(id_obj + 0x38, id) || id != pl)
                    continue;
                uint32_t f = 0;
                if (swofty::mem::read(e + 0x50, input) && input && swofty::mem::read(input + 0x1fc, f) && (f & 2))
                    *reinterpret_cast<uint32_t *>(input + 0x1fc) = f & ~2u;
                if (swofty::mem::read(e + 0x58, ctrl) && ctrl && swofty::mem::read(ctrl + 0x1c, f) && (f & 0x20))
                    *reinterpret_cast<uint32_t *>(ctrl + 0x1c) = f & ~0x20u;
                break;
            }
    }
    static uint32_t s_last = 0;
    if (unwanted && unwanted != s_last)
        swofty::log("[%s] no lock: vehicle %d (pit status %d) controller flags %#x cleared (%s)", who, pl, st, unwanted,
                    mode(kWarmupActive) == kOn ? "warm-up" : "red flag");
    s_last = unwanted;
    return unwanted;
}

// set(system, vehicle, ai, reason) = the addon's binding of the game's setController. Returns the flags cleared.
template <class SetController> uint32_t unlock_player(void *system, SetController set, const char *who)
{
    Race *r = race();
    if (free_phase())
        return 0; // free_player() handles the warm-up and the red flag
    if (!system || !r || !race_live() || before_start() || red_flag_on() || mode(kAutopilot) == kOn)
        return 0;
    int pl = r->player;
    if (pl < 0 || pl >= 32)
        return 0;
    int st = r->pit[pl];
    if (st != 1 /* OnTrack */ && st != 16 /* Invalid */)
        return 0;
    uintptr_t hold = 0;
    if (devvars::lookup("rf_hold", hold) && hold == 1)
        return 0;
    uint64_t now = GetTickCount64(), last = guard_clock().load(std::memory_order_relaxed);
    if (now - last < 250 || !guard_clock().compare_exchange_strong(last, now))
        return 0;
    uint32_t flags = ctrl_flags(system, pl), unwanted = flags & kLeftoverLocks;
    // Only a lock that stays 1.5 s on a car on the track (the game sets the pit lane AI a moment before the pit
    // status changes at the pit entry, and takes it off a moment after the exit).
    static uint32_t s_seen = 0;
    static uint64_t s_since = 0;
    if (unwanted != s_seen) {
        s_seen = unwanted;
        s_since = now;
    }
    if (!unwanted || now - s_since < 1500)
        return 0;
    for (int b = 0; b < 18; b++)
        if (unwanted & (1u << b))
            set(system, uint64_t(pl), 0, b);
    static uint32_t s_last = 0;
    if (unwanted && unwanted != s_last)
        swofty::log("[%s] lock guard: vehicle %d on track (pit status %d) had controller flags %#x, cleared %#x", who,
                    pl, st, flags, unwanted);
    s_last = unwanted;
    return unwanted;
}

}
