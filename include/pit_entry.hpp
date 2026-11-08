// Sending an AI car into the pit lane: PitLaneSystem::setPitState(car, EnteringPitLane) makes the pit lane manager
// take the car and run it down the pit lane (box stop, then out) — confirmed live 2026-10-24 (Melbourne, cars at lap
// fraction 0.973..0.993 of the lap, i.e. at the pit entry). A car far from the entry is not tried: the call is made
// when the car's lap fraction passes the entry window.
//
// The entry fraction is learned from the game's own pit stops (OnTrack -> EnteringPitLane) and shared between the
// modules through dev_vars ("pit_entry_frac" = float bits + 1, per process); until one was seen, the window sits
// 1.5..3.5 % of the lap before the parc ferme spot (Melbourne: parc ferme 0.0 -> window 0.965..0.985, entry 0.972).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "dev_vars.hpp"

namespace pitentry {

// Fallback window relative to the parc ferme fraction. A car sent at 0.962..0.966 of the lap (Melbourne) was only
// taken by the pit lane manager a lap later; cars sent at 0.972..0.985 went in at once, so the window starts late.
constexpr float kBefore = 0.028f, kAfter = 0.012f;

inline bool learned(float &frac)
{
    uintptr_t v = 0;
    if (!devvars::lookup("pit_entry_frac", v) || !v)
        return false;
    uint32_t bits = uint32_t(v - 1);
    std::memcpy(&frac, &bits, sizeof frac);
    return frac >= 0 && frac <= 1;
}

// pf = parc ferme fraction when known: a value far from the expected spot (a car re-entering from the pit exit
// queue, a drive-through being taken back) is not an entry and is ignored.
inline void learn(float frac, float pf = -1)
{
    float old;
    if (frac < 0 || frac > 1 || learned(old))
        return;
    // A pit entry the game made itself is trusted even far from the table's parc ferme spot: on a mod track the
    // stock table is another track's (2026-10-26: Bahrain Outer matched Melbourne, entry 0.919 was thrown away and
    // the red flag sent the cars at 0.972..0.988, 300 m after the entry - the game teleported them back into the
    // pit lane). The callers only pass entries the game decided (no hold, no cool-down, no car we sent).
    (void)pf;
    uint32_t bits;
    std::memcpy(&bits, &frac, sizeof bits);
    devvars::publish("pit_entry_frac", uintptr_t(bits) + 1);
}

// Entry window [lo, hi] on the lap (may wrap around 1.0); pf = parc ferme fraction (-1 = unknown -> no window).
inline bool window(float pf, float &lo, float &hi)
{
    float entry;
    if (learned(entry)) {
        lo = entry - 0.004f, hi = entry + 0.012f;
    } else if (pf >= 0) {
        lo = pf - kBefore, hi = pf - kAfter;
    } else
        return false;
    lo = std::fmod(lo + 1.0f, 1.0f), hi = std::fmod(hi + 1.0f, 1.0f);
    return true;
}

inline bool in_window(float frac, float lo, float hi)
{
    return lo <= hi ? frac >= lo && frac <= hi : frac >= lo || frac <= hi;
}

}
