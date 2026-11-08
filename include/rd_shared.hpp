// Live switches shared by the Race Director DLLs of one game process.
// The director's hotkeys write one byte per module into a small named memory block; each module asks rd::on() before
// it acts. 0 = follow the module's own setting, 1 = forced off, 2 = forced on. The block starts zeroed, so modules
// follow their settings until a hotkey is pressed.
#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

namespace rd {

enum Module : int {
    kTrackLimits,
    kScUnlapping,
    kRedFlagPit,
    kRedFlagFromSc,
    kCooldownLap,
    kPitlaneStart,
    kPracticeStart,
    kPartsDetach,
    kRecovery,
    kShowers,
    kCount
};

enum Mode : uint8_t { kFollow = 0, kOff = 1, kOn = 2 };

// Command slots after the module switches: the director posts, the module takes the command on the game thread.
enum Command : int { kRaceCtlCommand = 22, kPartsCommand = 23, kLapDownCommand = 27, kPenaltyCommand = 29, kRainCommand = 30, kWarmupCommand = 31 };
// kRaceCtlCommand values: the Red Flag addon makes the game's own SC logic call them.
enum RaceCtlCommand : uint8_t { kRcNone = 0, kRcSafetyCar = 1, kRcVsc = 2, kRcRedFlag = 3 };
// kPartsCommand values: which parts of the player's car come off now (Parts Detach module).
enum PartsCommand : uint8_t { kPartsNone = 0, kPartsRearWing = 1, kPartsMirrors = 2, kPartsSidepods = 3, kPartsFrontWing = 4,
                              kPartsAll = 5 };
// Lasting switch after the module switches: kOn = the game's AI drives the player's car (set by the director).
constexpr Module kAutopilot = Module(28);
// Lasting switch: kOn = the player drives through the pit lane (the game's pit lane AI does not take the car).
constexpr Module kSelfPit = Module(26);
// Lasting switch: kOn = rain cells cross the track on the radar and bring the rain (Passing Showers module).
constexpr Module kRadarCells = Module(25);
// Lasting state set by the Pit Lane Start module: kOn while the warm-up (pit box, warm-up lap) runs.
constexpr Module kWarmupActive = Module(24);

// Rain radar picture, written by the Passing Showers module on the game thread, drawn by the director's radar
// window. World x/z in metres; only a picture, so no locking (a torn frame is redrawn 250 ms later).
struct Radar {
    std::atomic<uint32_t> seq;
    int32_t n_track;          // track outline, recorded from one car's first lap
    float track[640][2];
    int32_t n_cars;
    float car[32][2];
    int32_t player;           // index into car[], -1 = none
    int32_t n_cells;
    float cell[4][4];         // x, z, radius, intensity (0.3 light ... 1 heavy)
    float coverage;           // share of the track under a cell, 0..1
    float eta;                // seconds until a cell reaches the track (-1 = none coming)
    int32_t raining;          // the module has made it rain
    float wind[2];            // direction the cells move (unit x, z)
    float cell_speed[4];      // m/s of each cell along wind[]
};

inline Radar *radar()
{
    static Radar *r = [] {
        wchar_t name[64];
        wsprintfW(name, L"Local\\vmknolle_race_director_radar_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Radar), name);
        return h ? static_cast<Radar *>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Radar))) : nullptr;
    }();
    return r;
}
// The game's race state, written by the Session Watch addon on the game thread (from the game's own systems, see
// the KB: VehicleAntiStallSystem, SafetyCarDeployment record, RedFlagSystem, setController, PitLaneSystem). Other
// addons read it to stay out of the game's way. updated_ms = 0 or older than 2 s: Session Watch is not running.
struct Race {
    std::atomic<uint64_t> updated_ms;  // GetTickCount64 of the last in-game frame
    int32_t session_type;              // 1..4 practice, 5..9 qualifying, 10..14 sprint shootout, 15..17 race, 18 TT
    uint8_t race_started;              // VehicleAntiStallSystem +0x100: 0 = the start hold is on (race types only)
    uint8_t lights_out;                // +0x109
    uint8_t lights_on;                 // +0x104: start lights lit
    uint8_t sc_type;                   // last SafetyCarDeployment: 0 none, 1 safety car, 2 VSC
    uint8_t sc_phase;                  // 0 deployed, 1 ending (in this lap), 2 SC in the pit lane, 3 racing again
    uint8_t red_flag;                  // RedFlagSystem +0x40: 0 none, 1 deployed (countdown), 2 suspended
    uint8_t player;                    // player's vehicle id, 0xff = unknown
    uint8_t pit[32];                   // pit lane status per vehicle (InactiveInGarage 0 .. DrivingThrough 15)
    uint8_t ai[32];                    // last setController: 1 = AI drives, 0 = human
    uint8_t reason[32];                // last setController reason (PIT_LANE 0 .. PDS 17), 0xff = none yet
    std::atomic<uint32_t> rewinds;     // flashbacks seen (session time went back while the session stayed)
    float rewind_s;                    // seconds the last flashback took back
};

inline Race *race()
{
    static Race *r = [] {
        wchar_t name[64];
        wsprintfW(name, L"Local\\vmknolle_race_director_race_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Race), name);
        return h ? static_cast<Race *>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Race))) : nullptr;
    }();
    return r;
}

// True while Session Watch reports (an in-game frame within the last 2 s).
inline bool race_live()
{
    Race *r = race();
    return r && r->updated_ms.load(std::memory_order_relaxed) &&
           GetTickCount64() - r->updated_ms.load(std::memory_order_relaxed) < 2000;
}
inline bool sc_out() { Race *r = race(); return race_live() && r->sc_type == 1 && r->sc_phase < 2; }
inline bool vsc_out() { Race *r = race(); return race_live() && r->sc_type == 2 && r->sc_phase < 3; }
inline bool red_flag_on() { Race *r = race(); return race_live() && r->red_flag != 0; }
// Seconds the game rewound since the caller last asked (0 = no new flashback). The game does not rewind addon
// memory (RE findings_flashback), so modules take this off their own timers.
inline float take_rewind(uint32_t &seen)
{
    Race *r = race();
    if (!r)
        return 0;
    uint32_t n = r->rewinds.load(std::memory_order_acquire);
    if (n == seen)
        return 0;
    seen = n;
    return r->rewind_s;
}
// Race session and the cars are still held for the start (before MessageRaceStarted).
inline bool before_start() { Race *r = race(); return race_live() && r->session_type >= 15 && r->session_type <= 17 && !r->race_started; }

enum RainCommand : uint8_t { kRainNone = 0, kRainStart = 1, kRainStop = 2 };
enum WarmupCommand : uint8_t { kWarmupNone = 0, kWarmupLeaveBox = 1, kWarmupToGrid = 2 };
enum PenaltyCommand : uint8_t { kPenaltyNone = 0, kPenaltyWarning = 1, kPenaltyTime = 2, kPenaltyDriveThrough = 3 };

struct Shared {
    std::atomic<uint8_t> mode[32];
    std::atomic<uint64_t> posted[32]; // GetTickCount64 of the last post, per command slot
};

// A command that waits longer than this (the race was paused or not running) is dropped instead of firing later.
constexpr uint64_t kCommandTtlMs = 3000;

inline Shared *shared()
{
    static Shared *s = [] {
        wchar_t name[64];
        wsprintfW(name, L"Local\\vmknolle_race_director_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name);
        return h ? static_cast<Shared *>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    }();
    return s;
}

inline uint8_t mode(Module m)
{
    Shared *s = shared();
    return s ? s->mode[m].load(std::memory_order_relaxed) : kFollow;
}

inline void set_mode(Module m, uint8_t v)
{
    if (Shared *s = shared())
        s->mode[m].store(v, std::memory_order_relaxed);
}

inline void post(Command c, uint8_t v)
{
    if (Shared *s = shared()) {
        s->posted[c].store(GetTickCount64(), std::memory_order_relaxed);
        s->mode[c].store(v, std::memory_order_release);
    }
}

// Takes a waiting command; 0 when there is none or it is older than kCommandTtlMs.
inline uint8_t take(Command c)
{
    Shared *s = shared();
    if (!s)
        return 0;
    uint8_t v = s->mode[c].exchange(0, std::memory_order_acq_rel);
    return v && GetTickCount64() - s->posted[c].load(std::memory_order_relaxed) <= kCommandTtlMs ? v : 0;
}

// The module's effective state: its setting unless a hotkey forced it.
inline bool on(Module m, bool setting)
{
    uint8_t v = mode(m);
    return v == kFollow ? setting : v == kOn;
}

}

#ifdef RD_MODULE
#define RD_ON(flag) ::rd::on(RD_MODULE, (flag).load())
#endif
