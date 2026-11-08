#define RS_PART 1
// Race Start: the race from the pit box to parc ferme, in one module with one phase machine and one rule for who
// may take the player's car.
//
// Phases of a race session:
//   Garage      warm-up: the race flow (game/flow_race.xml, edited while the game parses it) opens with the practice
//               garage state; every car sits in its box. Control window: leave the box, start the warm-up lap.
//   WarmUpLap   a formation lap state before the broadcast: the AI cars drive from the pit lane to the grid, the
//               player drives out of the box (a box stop is allowed), parks on the grid; ends when the player parked.
//   PreRace     intro video, talking points, grid rundown, grid scene (the stock flow).
//   Formation   the stock formation lap: drive to the grid from the pit lane (unless the warm-up lap did), pit lane
//               starters are put into the pit lane.
//   Race        pit lane starters join once the field has passed.
//   Cooldown    after the player's chequered flag: keeps driving until parked in parc ferme; AI top 3 park too.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>

#include <swofty/events.hpp>
#include <swofty/hud.hpp>
#include <swofty/mem.hpp>
#include <swofty/mixin.hpp>

#if __has_include("rd_timing.hpp")
#include "rd_timing.hpp"
#else
#define RD_TIMED(label)
#endif

#include "board_overlay.hpp"
#include "car_hold.hpp"
#include "dev_vars.hpp"
#include "parc_ferme_table.hpp"
#include "pit_entry.hpp"
#include "pit_exit_table.hpp"
#include "rd_shared.hpp"
#include "rs_bindings.hpp"
#ifdef SWOFTY_PUBLIC
#include "rs_own_sigs.hpp"
#endif
#include "track_points.hpp"

SWOFTY_SETTING(bool, warmup, true);
SWOFTY_SETTING(int, warmup_minutes, 15);
SWOFTY_SETTING(int, warmup_laps, 0);
SWOFTY_SETTING(int, warmup_drive_out_s, 8);
SWOFTY_SETTING(bool, grid_lap, true);
SWOFTY_SETTING(bool, red_flag_lap, false);
SWOFTY_SETTING(bool, warmup_grid_lap, true);
SWOFTY_SETTING(bool, pit_lane_start, false);
SWOFTY_SETTING(int, starters, 1);
SWOFTY_SETTING(bool, player_pit_start, false);
SWOFTY_SETTING(std::string_view, starters_who, "last");
SWOFTY_SETTING(int, starters_hold_max, 90);
SWOFTY_SETTING(bool, cooldown, true);
SWOFTY_SETTING(int, cooldown_seconds, 300);
SWOFTY_SETTING(std::string_view, cooldown_end, "parc_ferme");
SWOFTY_SETTING(bool, park_top3, true);
SWOFTY_SETTING(bool, pf_marking, true);
SWOFTY_SETTING(bool, verbose, true);
SWOFTY_SETTING(bool, hide_garage_crew, true);
SWOFTY_SETTING(bool, log_field, false);
SWOFTY_SETTING(bool, hold_probe, false);

namespace {

// The per-frame hooks read the game's multi-GB heaps through swofty::mem::read, whose VirtualQuery answer for such a
// region expires after 250 ms and can take milliseconds to renew (SDK mem.hpp). In 2026-10-28 logs
// Sess_InGame_UpdateBase took 32-36 ms/s that way (#44: ~30 fps lost in Time Trial). These hooks keep a big region's
// answer for 2 s instead (swofty::mem::Trust).
constexpr uint32_t kHeapTrustMs = 2000;

// Pre-formation-lap part (warm-up from the box, lap to the grid, drive out) off for now (user 2026-10-25); the
// saved settings cannot turn it on.
constexpr bool kWarmupFeature = true;
// One source, four addons (user 2026-10-31 "mache es als eignes", "red flag inlap ... box start alles extra"); each
// build compiles one part, the others stay off: RS_PART 0 = Race Start (warm-up, lap to the grid, drive out),
// 1 = Cool-down Lap (cool-down, parc ferme parking, 1/2/3 boards), 2 = Pit Lane Start (pit lane starters, the player
// too as an option), 3 = Red Flag In-Lap (field into the pit lane, drive to the grid, SC lap restart), 4 = Autopilot
// (test aid, not released: the AI drives the player's car while the dashboard switch is on; user 2026-10-31).
#ifndef RS_PART
#define RS_PART 0
#endif
constexpr bool kStart = RS_PART == 0, kCd = RS_PART == 1, kPls = RS_PART == 2, kRf = RS_PART == 3,
               kAp = RS_PART == 4;
constexpr const char *kPartName = kStart ? "race start" : kCd ? "cool-down lap" : kPls ? "pit lane start"
                                     : kRf  ? "red flag in-lap"
                                            : "autopilot";

using rs::kMaxCars;

enum class Phase { Idle, Garage, WarmUpLap, WarmUpRace, PreRace, Formation, Race, Cooldown, Over, RfLap };
enum class CdEnd { PitLane, ParcFerme, Time };

const char *phase_name(Phase p)
{
    static const char *names[] = {"idle", "garage", "warm-up lap", "warm-up (race state)", "pre-race", "formation lap", "race", "cool-down", "over", "red flag SC lap"};
    return names[int(p)];
}

// Settings (live).
std::atomic<bool> g_warmup_on{false}, g_grid_lap_on{true}, g_pls_setting{false}, g_cd_setting{true}, g_park_top3{true},
    g_verbose{true}, g_who_random{false};
std::atomic<bool> g_hide_crew{true}; // warm-up / lap to the grid: the garage crews are hidden (user 2026-10-31)
std::atomic<bool> g_warm_gridlap_on{true}; // warm-up ends with every car driving from the pit lane to its slot
bool g_warm_gridlap_pending = false, g_flow_gridlap = false;
bool g_flow_freelaps = false; // the flow has free warm-up laps (race state) before the lap to the grid
bool g_warm_to_grid = false;  // control window "to the grid": the free laps are skipped
bool g_warm_end_parked = false; // the free warm-up ended with the player parked on the grid
bool g_gridlap_restart = false; // our lap to the grid ended, the game's Restart is next
uintptr_t g_dsq_rules = 0;      // rules object of the DSQs we blocked (masks +0x1d08 / +0x1d0c)
uint32_t g_dsq_blocked = 0;     // cars whose DSQ we blocked; their mask bits are cleared later
uintptr_t g_fll = 0;            // FormationLapLogic whose done flag we cleared (logged at the race start)
bool g_rf_lap = false;          // the lap to the grid is the red flag restart
std::atomic<bool> g_rf_lap_on{false}; // red flag restart: SC lap out of the pit lane, then the standing start
void *g_race_state = nullptr;  // the stock in-game state object of the race (RfLap is another one)
bool g_rf_seen = false;        // a red flag was out in this race
int g_rf_lead = -1;            // RfLap: the first car out of the pit lane
bool g_rf_lead_away = false;   // RfLap: that car was more than 400 m from its slot
double g_rf_lap_t = 0;
void rf_lap_begin();
bool rf_lap_update(double dt);
void rf_grid_begin();
bool rf_grid_update(double dt);
std::atomic<bool> g_pf_mark_on{true}; // parc ferme marking in the cool-down (prototype)
void *g_pf_mark = nullptr;           // our spawned parc ferme scene
double g_pf_mark_t = 0;
int g_pf_mark_tries = 0;
 // red flag restart: formation lap out of the pit lane instead of the standing restart
std::atomic<int> g_warmup_drive_out_s{8};
std::atomic<int> g_warmup_max{15}, g_warmup_laps{0}, g_starters{1}, g_hold_max{90}, g_cd_seconds{180};
std::atomic<bool> g_player_pls{false}; // the player starts from the pit lane too
std::atomic<bool> g_field_log{false};
std::atomic<bool> g_hold_probe{false}; // hold probe (test aid part) // log_field: field lines every 5 s (test aid)
std::atomic<CdEnd> g_cd_end{CdEnd::ParcFerme};

bool pls_on() { return kPls && ::rd::on(::rd::kPitlaneStart, g_pls_setting.load()); }
bool cd_on() { return kCd && ::rd::on(::rd::kCooldownLap, g_cd_setting.load()); }
bool autopilot() { return ::rd::mode(::rd::kAutopilot) == ::rd::kOn; }
int g_auto_car = -1;      // the car the autopilot drives (ROBODRIVER), -1 = none
bool g_auto_done = false; // this session's race is over for the autopilot's car
void autopilot_update();

// Session.
std::atomic<Phase> g_phase{Phase::Idle};
std::atomic<uintptr_t> g_rules{0}, g_session{0};
std::atomic<void *> g_ctrl_sys{nullptr}, g_pit_sys{nullptr};
std::atomic<uint32_t> g_human{0}, g_sc{0}; // cars ever driven by a human; safety cars
std::atomic<int> g_pit_state[kMaxCars];
[[maybe_unused]] void pit_poll(); // public build: pit states read per frame from the pit lane system
std::atomic<bool> g_flow_edited{false}; // the last flow parsed is the race flow with the warm-up phase
bool g_grid_done;                       // the drive from the pit lane to the grid happened this session
thread_local bool t_in_formation = false, t_in_pq = false;
thread_local int t_pit_vehicle = -1;
std::atomic<uint32_t> g_cd_sent{0}; // cool-down: cars sent into the pit lane (setPitState EnteringPitLane)
bool g_warm_leave = false;          // warm-up: the player asked to leave the box when the race state begins
double g_warm_leave_t = 0;
uint32_t g_warm_laps0[32];          // warm-up: lap counts when the race state began
// Warm-up laps the player has driven, counted by us: the game counts no laps before the race has started
// (2026-10-26 test: the lap limit never came, the warm-up never ended). A lap = out on the track more than 400 m from
// the car's grid slot, then back past it (within 40 m) without being in the pit lane.
uint32_t g_warm_lapcount = 0;
// Warm-up: seconds the player's car has stood still in the pit lane on its way out (LeavingPitLane).
double g_pit_still_t = 0;
double g_pit_lane_t = 0; // time in LeavingPitLane on this run out of the box
float g_min_pf[32];                // cool-down: closest a car came to the parc ferme spot in the pit lane
std::atomic<uint32_t> g_cd_through{0}; // cool-down: cars that reached their box and are sent on (no pit stop)
const redflag::PitExit *g_exit;
const pftab::ParcFerme *g_pf;
redflag::PitExit g_exit_rt{"learned", 0, {}};      // pit exit learned from the game's cars (track_points.hpp)
pftab::ParcFerme g_pf_rt{"placement", 0, -1, true, {}}; // parc ferme from the track's cutscene placement
std::atomic<uint32_t> g_exit_pending{0};           // cars that just went LeavingPitLane -> OnTrack
float g_pos_ring[kMaxCars][8][2];                  // positions every 0.25 s (pit exit head = 1 s back)
int g_ring_at = 0;
double g_ring_t = 0;

// Warm-up. The grid slots of every car as loaded (before the cars went into their garages).
float g_slot_mtx[kMaxCars][16];
bool g_slot_ok[kMaxCars];
float g_garage_pos[2];           // where the player's car left its garage
double g_garage_t, g_warm_t, g_still_t;
bool g_player_out, g_player_away, g_warm_requested;
float g_last_pos[2];

// Grid queue (drive to the grid): cars pinned in the pit lane, leaving one per second in grid order.
bool g_grid_pinned[kMaxCars];
uintptr_t g_grid_car[kMaxCars];
float g_grid_mtx[kMaxCars][16];
int g_grid_order[kMaxCars], g_grid_n, g_grid_next;
double g_grid_timer;

// Red flag, drive to the grid (user 2026-10-26 18:28/18:32): the start grid slots in grid order as the race started.
float g_start_slot[kMaxCars][16];
int g_start_n = 0;
bool g_rf_grid = false; // the field drives from the pit lane onto the grid before the game's red flag

// Pit lane starters.
std::atomic<uint32_t> g_chosen{0}, g_released{0};
std::atomic<uint32_t> g_pls_sent{0};    // starters sent into the pit lane at the end of the formation lap
std::atomic<uint32_t> g_pls_through{0}; // starters that reached their box: drive through (flow thread)
float g_pls_form_prog[kMaxCars];        // lap progress at the start of the formation lap
float g_pls_bx = 0, g_pls_bz = 0;       // towards the back of the grid: the exit queue direction
float g_start_prog[kMaxCars]; // lap progress of every car on the first race frame
bool g_start_prog_set = false;
std::atomic<bool> g_frozen[kMaxCars];
float g_frozen_mtx[kMaxCars][16];
std::atomic<int> g_frozen_count{0};
float g_prev_pos[kMaxCars][2];
bool g_prev_ok[kMaxCars];
std::atomic<uint32_t> g_logged_pit{0};
// Parc ferme boards: where the podium cutscene (podium2022_celebrate_parcferme_01/02, clips 1st/2nd/3rd car) puts the
// top 3, in the frame of the ParcFerme placement: P1 at the point, P2 2.28 m to its side and 1.49 m back, P3 2.62 m to
// the other side and 1.68 m back. A parked top-3 car slides from where it stopped onto its board.
constexpr float kBoardLocal[3][3] = {{0.0f, 0.0f, 0.0f}, {2.284f, 0.0f, -1.491f}, {-2.620f, 0.0f, -1.679f}};
float g_board[3][3];
bool g_board_ok;
float g_slide_from[kMaxCars][2];
float g_slide_t[kMaxCars];
bool g_on_board[kMaxCars];
float g_board_still[kMaxCars]; // cool-down: seconds a track-board car stood still near its board (Mexico)
bool g_race_started;
double g_race_time;
uint32_t g_rewinds_seen; // Session Watch's flashback count already taken off g_race_time

// Cool-down.
bool g_cd_holding, g_cd_done;
std::atomic<bool> g_cd_player_ai{false}, g_cd_in_pit_lane{false};
double g_cd_held, g_cd_parked, g_cd_next_log;
int g_cd_player = -1;
uint32_t g_cd_finish_laps = 0;
float g_cd_prev_frac[kMaxCars];      // cool-down: last lap fraction per car (a drop > 0.5 = crossed the line)
uint32_t g_cd_crossed = 0;           // cool-down: cars that crossed the line since it began // laps the player had when the cool-down began (an AI car is sent once it has them too)
uint32_t g_cd_robo = 0;              // cool-down: cars the autopilot (ROBODRIVER) drove this session: their finish goes to the AI
uint32_t g_cd_out = 0;               // cool-down: player cars out of the race this session (DNF, retired, DSQ): no cool-down
uint32_t g_cd_game_held = 0;         // cool-down: cars stopped by the game's own hold (cd_game_hold)
uint32_t g_cd_pin = 0;               // cool-down: cars the game's hold did not stop: held by our pin instead
float g_cd_hold_t[kMaxCars];         // cool-down: seconds since the game's hold was set
constexpr float kParkRadius = 8.0f;  // cool-down: the player is parked when stopped this close to his board (was 2.5 m)
int g_pub_player = -2;        // last published "player_vehicle" dev var

template <class T> bool rd(uintptr_t addr, T &out)
{
    return addr && swofty::mem::read(addr, out);
}

void logv(const char *fmt, auto... a)
{
    if (g_verbose)
        swofty::log(fmt, a...);
}

// The race start / anti-stall system holds the cars until the race has started: +0x100 = 0 (race started) holds the
// input, +0x109 = 0 (lights out) keeps the controller's hold bit; both are set by MessageRaceStarted. During the
// warm-up the plugin sets both so the car can be driven, and puts them back when the warm-up ends.
bool g_as_forced = false;

void antistall_release(bool on)
{
    auto sys = reinterpret_cast<uintptr_t>(*swofty::game::g_Veh_AntiStallSystem);
    uint8_t started = 0, lights_out = 0;
    if (!sys || !swofty::mem::read(sys + 0x100, started) || !swofty::mem::read(sys + 0x109, lights_out))
        return;
    if (on && (!started || !lights_out)) {
        *reinterpret_cast<uint8_t *>(sys + 0x100) = 1;
        *reinterpret_cast<uint8_t *>(sys + 0x109) = 1;
        if (!g_as_forced)
            swofty::log("[racestart] warm-up: the car's start hold is released (race started + lights out)");
        g_as_forced = true;
    } else if (!on && g_as_forced) {
        *reinterpret_cast<uint8_t *>(sys + 0x100) = 0;
        *reinterpret_cast<uint8_t *>(sys + 0x109) = 0;
        g_as_forced = false;
        swofty::log("[racestart] warm-up over: the start hold is back for the real start");
    }
}

void set_phase(Phase p, const char *why)
{
    if (g_phase.exchange(p) != p)
        swofty::log("[racestart] phase %s (%s)", phase_name(p), why);
    bool warm = p == Phase::Garage || p == Phase::WarmUpLap || p == Phase::WarmUpRace;
    ::rd::set_mode(::rd::kWarmupActive, warm ? ::rd::kOn : ::rd::kFollow);
    if (p != Phase::WarmUpLap && p != Phase::WarmUpRace)
        antistall_release(false);
}

// --- session model ---------------------------------------------------------------------------------------------
int vehicle_id(uintptr_t record)
{
    uintptr_t a = 0, b = 0;
    uint8_t id = 0xff;
    return rd(record + 0x18, a) && rd(a + 0x58, b) && rd(b + 0xe8, id) ? id : -1;
}

// The classified cars (no safety car). pos = lap position; with unwrap, fractions below 0.5 count as one lap on
// (grid slots just past the line), so before the start the order is the grid order.
struct Field {
    int n = 0, ids[kMaxCars];
    float pos[kMaxCars];
    float progress[kMaxCars] = {};
    bool seen[kMaxCars] = {};
    uintptr_t car[kMaxCars] = {};
};

Field read_field(bool unwrap)
{
    Field f;
    uintptr_t rules = g_rules, mgr = 0, it = 0, end = 0;
    if (!rd(rules + rs::kRulesRecords, mgr) || !rd(mgr + 0x88, it) || !rd(mgr + 0x90, end))
        return f;
    for (; it && it < end && f.n < kMaxCars; it += 8) {
        uintptr_t rec = 0;
        uint32_t laps;
        float frac;
        int id = rd(it, rec) && rec ? vehicle_id(rec) : -1;
        if (id < 0 || id >= kMaxCars || (g_sc & (1u << id)) || !rd(rec + rs::kRecLaps, laps) ||
            !rd(rec + rs::kRecLapFrac, frac))
            continue;
        float p = unwrap ? (frac < 0.5f ? frac + 1.0f : frac) : float(laps) + frac;
        f.ids[f.n] = id;
        f.pos[f.n++] = p;
        f.seen[id] = true;
        f.progress[id] = p;
        f.car[id] = carhold::car_of(rec);
    }
    return f;
}

int player_vehicle()
{
    for (int v = 0; v < kMaxCars; v++)
        if (g_human & (1u << v))
            return v;
    return -1;
}

bool human(int v) { return v >= 0 && v < kMaxCars && (g_human & (1u << v)); }

uintptr_t record_of(int id)
{
    uintptr_t mgr = reinterpret_cast<uintptr_t>(*swofty::game::g_Veh_RecordManager), rec = 0;
    if (!mgr || id < 0 || id > 0xff)
        return 0;
    if (rd(mgr + rs::kMgrCache + size_t(id) * 8, rec) && rec)
        return rec;
    uintptr_t it = 0, end = 0;
    rd(mgr + rs::kMgrListBegin, it);
    rd(mgr + rs::kMgrListBegin + 8, end);
    for (; it && it < end; it += 8) {
        uintptr_t r = 0;
        if (rd(it, r) && r && vehicle_id(r) == id)
            return r;
    }
    return 0;
}

bool speed_of(uintptr_t rec, float &speed)
{
    uintptr_t root = reinterpret_cast<uintptr_t>(*swofty::game::g_Veh_StateRoot), a = 0, table = 0, link = 0, state = 0;
    uint8_t k = 0;
    return rd(root + 0x38, a) && rd(a + 0xa8, table) && rd(rec + rs::kRecStateLink, link) && rd(link + 0x38, k) &&
           rd(table + 0x38 + size_t(k) * 0x508, state) && rd(state + 0x2c, speed);
}

// Race position (1-based) from the game session's race-order list, or 0.
int rank_of(int id)
{
    uintptr_t session = g_session, it = 0, end = 0;
    if (!rd(session + rs::kSessionOrder, it) || !rd(session + rs::kSessionOrder + 8, end))
        return 0;
    for (int rank = 1; it && it < end && rank <= 64; it += 8, rank++) {
        uintptr_t entry = 0;
        uint8_t vid = 0xff;
        if (rd(it, entry) && rd(entry, vid) && vid == id)
            return rank;
    }
    return 0;
}

// A car parked in its garage (pit state InGarage, or 0 = InactiveInGarage) has retired.
bool retired(int id)
{
    int st = id >= 0 && id < kMaxCars ? g_pit_state[id].load() : -1;
    return st == rs::kPitInGarage || st == 0;
}

// Parc ferme position (1-based) in the race order without the retired cars, or 0. The race order keeps a retired car
// at its old place: on 2026-10-26 a car in its garage since lap 1 counted as P3 and the cool-down waited 300 s for it.
int cd_rank(int id)
{
    if (retired(id))
        return 0;
    uintptr_t session = g_session, it = 0, end = 0;
    if (!rd(session + rs::kSessionOrder, it) || !rd(session + rs::kSessionOrder + 8, end))
        return 0;
    for (int rank = 1, n = 0; it && it < end && n < 64; it += 8, n++) {
        uintptr_t entry = 0;
        uint8_t vid = 0xff;
        if (!rd(it, entry) || !rd(entry, vid))
            continue;
        if (vid == id)
            return rank;
        if (!retired(vid))
            rank++;
    }
    return 0;
}

template <class T, size_t N> const T *nearest(const T (&table)[N], float length)
{
    const T *best = nullptr;
    for (auto &p : table)
        if (std::fabs(p.length - length) < 0.03f * length && (!best || std::fabs(p.length - length) < std::fabs(best->length - length)))
            best = &p;
    return best;
}

// Unit vector from the pole car towards the back of the grid (x, z); 0 when unknown.
void grid_back(const Field &f, float &bx, float &bz)
{
    bx = bz = 0;
    int front = -1, back = -1;
    for (int i = 0; i < f.n; i++) {
        if (front < 0 || f.pos[i] > f.pos[front])
            front = i;
        if (back < 0 || f.pos[i] < f.pos[back])
            back = i;
    }
    const float *a = front >= 0 ? carhold::matrix(f.car[f.ids[front]]) : nullptr;
    const float *b = back >= 0 ? carhold::matrix(f.car[f.ids[back]]) : nullptr;
    if (!a || !b || front == back)
        return;
    float dx = b[12] - a[12], dz = b[14] - a[14], d = std::sqrt(dx * dx + dz * dz);
    if (d > 1.f)
        bx = dx / d, bz = dz / d;
}

void reset_session()
{
    set_phase(Phase::Idle, "new session");
    g_warm_to_grid = g_warm_end_parked = false;
    g_grid_done = g_warm_requested = g_player_out = g_player_away = false;
    g_warm_gridlap_pending = false;
    g_rf_lap = false;
    g_race_state = nullptr;
    g_rf_seen = false;
    devvars::publish("rs_rf_lap", 0);
    g_start_n = 0;
    g_rf_grid = false;
    devvars::publish("rs_rf_grid", 0);
    g_pf_mark = nullptr;
    g_pf_mark_t = 0;
    g_pf_mark_tries = 0;
    g_garage_t = g_warm_t = g_still_t = 0;
    for (auto &s : g_pit_state)
        s = -1;
    for (auto &p : g_grid_pinned)
        p = false;
    g_grid_n = g_grid_next = 0;
    g_grid_timer = 0;
    g_chosen = g_released = 0;
    g_pls_sent = 0;
    g_pls_through = 0;
    g_start_prog_set = false;
    for (auto &f : g_frozen)
        f = false;
    for (auto &ok : g_prev_ok)
        ok = false;
    g_frozen_count = 0;
    g_logged_pit = 0;
    g_race_started = false;
    g_race_time = 0;
    ::rd::take_rewind(g_rewinds_seen); // flashbacks before this race do not count
    g_cd_holding = g_cd_done = false;
    g_cd_player = -1;
    g_cd_robo = g_cd_game_held = g_cd_pin = g_cd_out = 0;
    boards::hide();
    g_sc = 0;
    g_exit = nullptr; // the track length is not known yet at session enter: looked up when first needed
    devvars::publish("rs_exit", 0);
    g_pf = nullptr;
}

void find_tables()
{
    float length = *swofty::game::g_Track_Length;
    trackpts::Block *tp = trackpts::for_track(length);
    const pftab::ParcFerme *tab_pf = nearest(pftab::kParcFerme, length);
    const redflag::PitExit *tab_exit = nearest(redflag::kPitExit, length);
    // The track's own parc ferme placement decides whether the stock tables apply: a stock track has the table's
    // point (checked progress and pit lane side); any other track (own tracks, or one whose length is within 3 % of
    // a stock track) gets the placement and the points learned from the game's cars.
    bool stock = true;
    if (!g_pf || g_pf == &g_pf_rt) {
        float p[3];
        void *reg = *swofty::game::g_Cutscene_Placements;
        if (reg && trackpts::placement(reg, "ParcFerme", p)) {
            if (tab_pf && std::hypot(tab_pf->pos[0] - p[0], tab_pf->pos[2] - p[2]) < 5.0f) {
                g_pf = tab_pf;
            } else {
                stock = false;
                float frac = -1, dist = 0;
                bool have = trackpts::fraction_of(tp, p[0], p[2], frac, dist) && trackpts::samples(tp) > 64;
                if (g_pf != &g_pf_rt || (have && g_pf_rt.progress < 0))
                    swofty::log("[racestart] parc ferme from the track's placement at (%.1f, %.1f, %.1f): %s", p[0], p[1],
                                p[2], have ? "lap fraction known" : "lap fraction not known yet");
                g_pf_rt.length = length;
                std::memcpy(g_pf_rt.pos, p, sizeof p);
                g_pf_rt.progress = have ? frac : -1.0f;
                g_pf_rt.in_pit_lane = !have || dist > 6.0f; // on the racing line (Monaco, Mexico): park before the exit
                g_pf = &g_pf_rt;
            }
        } else if (!g_pf) {
            g_pf = tab_pf;
        }
    } else if (g_pf != tab_pf) {
        stock = false;
    }
    if (g_pf && g_pf->progress >= 0) {
        uint32_t bits; // shared with the red flag module: pit entry window fallback (pit_entry.hpp)
        std::memcpy(&bits, &g_pf->progress, sizeof bits);
        devvars::publish("pf_frac", uintptr_t(bits) + 1);
    }
    if (tp && tp->exit_ok && (!stock || !tab_exit)) {
        if (g_exit != &g_exit_rt)
            swofty::log("[racestart] pit exit learned from the cars at (%.1f, %.1f, %.1f)", tp->exit_head[0],
                        tp->exit_head[1], tp->exit_head[2]);
        g_exit_rt.length = length;
        std::memcpy(g_exit_rt.head, tp->exit_head, sizeof g_exit_rt.head);
        g_exit = &g_exit_rt;
    } else if (!g_exit && stock) {
        g_exit = tab_exit;
    }
    devvars::publish("rs_exit", reinterpret_cast<uintptr_t>(g_exit)); // the red flag queue uses the same exit
    static float s_logged = -1;
    if ((!g_exit || !g_pf) && s_logged != length) {
        s_logged = length;
        swofty::log("[racestart] track %.1f m: pit exit %s, parc ferme %s", length, g_exit ? g_exit->track : "unknown",
                    g_pf ? g_pf->track : "unknown");
    }
}

// Lap-fraction samples of the cars on track and the pit exit learned from cars leaving the pit lane (every 0.25 s,
// flow thread).
void learn_track_points(double dt)
{
    g_ring_t += dt;
    if (g_ring_t < 0.25)
        return;
    g_ring_t = 0;
    trackpts::Block *tp = trackpts::for_track(*swofty::game::g_Track_Length);
    if (!tp)
        return;
    g_ring_at = (g_ring_at + 1) % 8;
    Field f = read_field(false);
    for (int i = 0; i < f.n; i++) {
        int v = f.ids[i];
        const float *m = carhold::matrix(f.car[v]);
        if (!m)
            continue;
        g_pos_ring[v][g_ring_at][0] = m[12], g_pos_ring[v][g_ring_at][1] = m[14];
        if (g_pit_state[v] == rs::kPitOnTrack)
            trackpts::sample(tp, f.progress[v] - std::floor(f.progress[v]), m[12], m[14]);
        if ((g_exit_pending.load() & (1u << v)) && !tp->exit_ok) {
            const float *back = g_pos_ring[v][(g_ring_at + 4) % 8]; // 4 ticks = 1 s ago
            if ((back[0] != 0 || back[1] != 0) && std::hypot(m[12] - back[0], m[14] - back[1]) > 5.0f) {
                tp->exit_end[0] = m[12], tp->exit_end[1] = m[13], tp->exit_end[2] = m[14];
                tp->exit_head[0] = back[0], tp->exit_head[1] = m[13], tp->exit_head[2] = back[1];
                tp->exit_ok = 1;
            }
        }
    }
    g_exit_pending = 0;
}

// --- player control --------------------------------------------------------------------------------------------
// VehicleControllerSystem: [sys+0x30][id*8] -> controller entry; entry+0x30 = flag bitmask (bit = flag id of the
// ControllerSet table 0x1457d7b10), +0x20 human controller, +0x28 AI controller, +0x10^+0x18 = mode. setController
// (sys, id, 1, flag) sets the bit and the AI mode, (sys, id, 0, flag) clears the bit (human again once no bit is left).
uintptr_t ctrl_entry(int v)
{
    auto sys = reinterpret_cast<uintptr_t>(g_ctrl_sys.load());
    uintptr_t arr = 0, end = 0, e = 0;
    if (!sys || v < 0 || !rd(sys + 0x30, arr) || !rd(sys + 0x38, end) || arr + size_t(v) * 8 + 8 > end || !rd(arr + size_t(v) * 8, e))
        return 0;
    return e;
}

uint32_t ctrl_flags(int v)
{
    uint32_t f = 0;
    uintptr_t e = ctrl_entry(v);
    return e && rd(e + 0x30, f) ? f : 0;
}

// The flags the player's car may carry in the current phase: in the garage and the warm-up only the pit lane AI
// while the car is moved out of the garage or through the pit lane, and our autopilot. Every other flag the game set
// is cleared through the game's own call, so the car is the player's (as Kizi's Manual Pit Lane does for the pit
// lane). After the start only leftover locks are cleared: the pit lane AI on a car that is out on the track (pit
// status OnTrack or Invalid 16, seen in the 17:45 test with the car standing on track) and the pre-start / grid
// reform flags. Not during a red flag (the game freezes the cars on purpose).
constexpr int kPitInvalid = 16;

uint32_t allowed_flags(Phase phase, int st)
{
    uint32_t allowed = 1u << rs::kFlagRoboDriver;
    bool on_track = st == rs::kPitOnTrack || st == kPitInvalid;
    if (phase == Phase::Garage || phase == Phase::WarmUpLap || phase == Phase::WarmUpRace || phase == Phase::RfLap) {
        // Pit lane AI while the game moves the car out of its garage and down the pit lane to the track (InGarage ..
        // LeavingPitLane): the game drives the car out (2026-10-26 01:08: taking it off at LeavingPitLane left the car
        // standing in the pit lane). Only when the car stands still there for 3 s (a closed pit exit) the player gets
        // the car (user 2026-10-25 21:41: the pit lane AI held the car in the pit lane).
        // Also when it is still in the pit lane after 20 s (it crept behind the exit queue: a whole minute in the
        // lap to the grid after the red flag, self-test 2026-10-26 16:22).
        // Also the drive-through past the box (warm-up: no pit stop, see on_pit_state).
        if ((st >= 9 && st <= 13) || st == rs::kPitDrivingThrough ||
            (st == rs::kPitLeavingPitLane && g_pit_still_t < 3.0 && g_pit_lane_t < 20.0))
            allowed |= 1u << rs::kFlagPitLane;
        // The formation lap state reforms the grid: its AI drives a car that is off its slot (also out of the pit
        // lane) onto the slot and hands it back; without that the car is held.
        if (phase == Phase::WarmUpLap)
            allowed |= 1u << rs::kFlagReformGrid;
    } else if (uintptr_t hold = 0; phase == Phase::Race && !::rd::red_flag_on() && !::rd::before_start() &&
                                   !(devvars::lookup("rf_hold", hold) && hold == 1)) {
        // (rf_hold: the Red Flag addon is sending the field into the pit lane with the game's pit lane AI.)
        allowed = ~((1u << rs::kFlagSessionNotStarted) | (1u << rs::kFlagReformGrid) |
                    (on_track ? 1u << rs::kFlagPitLane : 0u));
    } else
        allowed = ~0u;
    return allowed;
}

// Warm-up: the player's car carries no hold at all. Every frame of the garage / warm-up phases the controller flags
// the phase does not allow are cleared (enforce_player_control) and the start hold bits of the player's
// VehicleAntiStallSystem entry (input +0x1fc bit 0x2, controller +0x1c bit 0x20) are taken off, so the car always
// takes throttle (user 2026-10-25 21:41).
void enforce_player_control();
std::atomic<uint32_t> g_warm_through{0}; // warm-up: the player's car reached its box, drives through (no pit stop)

void warm_free_player()
{
    Phase phase = g_phase;
    if (uint32_t through = g_warm_through.exchange(0))
        if (void *sys = g_pit_sys.load())
            for (int v = 0; v < kMaxCars; v++)
                if (through & (1u << v)) {
                    swofty::game::Pit_SetPitState(sys, uint32_t(v), rs::kPitDrivingThrough);
                    swofty::log("[racestart] warm-up: vehicle %d drives through its box, no pit stop", v);
                }
    if (phase != Phase::Garage && phase != Phase::WarmUpLap && phase != Phase::WarmUpRace && phase != Phase::RfLap)
        return;
    enforce_player_control();
    int pl = player_vehicle();
    auto as = reinterpret_cast<uintptr_t>(*swofty::game::g_Veh_AntiStallSystem);
    uintptr_t it = 0, end = 0;
    if (pl < 0 || !as || !rd(as + 0x48, it) || !rd(as + 0x50, end))
        return;
    for (; it && it < end; it += 8) {
        uintptr_t e = 0, part = 0, input = 0, ctrl = 0;
        uint8_t id = 0xff;
        uint32_t f = 0;
        if (!rd(it, e) || !e || !rd(e + 0x48, part) || !rd(part + 0x38, id) || id != pl)
            continue;
        if (rd(e + 0x50, input) && input && rd(input + 0x1fc, f) && (f & 2))
            *reinterpret_cast<uint32_t *>(input + 0x1fc) = f & ~2u;
        if (rd(e + 0x58, ctrl) && ctrl && rd(ctrl + 0x1c, f) && (f & 0x20))
            *reinterpret_cast<uint32_t *>(ctrl + 0x1c) = f & ~0x20u;
        break;
    }
}

void enforce_player_control()
{
    Phase phase = g_phase;
    if (phase != Phase::Garage && phase != Phase::WarmUpLap && phase != Phase::WarmUpRace)
        return;
    if (autopilot())
        return;
    int pl = player_vehicle();
    void *sys = g_ctrl_sys.load();
    if (pl < 0 || !sys)
        return;
    uint32_t flags = ctrl_flags(pl), unwanted = flags & ~allowed_flags(phase, g_pit_state[pl].load());
    static uint32_t s_last = 0;
    if (unwanted) {
        for (int b = 0; b < 18; b++)
            if (unwanted & (1u << b))
                swofty::game::Veh_SetController_Thunk(sys, uint64_t(pl), 0, b);
        if (unwanted != s_last)
            swofty::log("[racestart] lock guard (%s): vehicle %d controller flags %#x, cleared %#x (pit status %d)",
                        phase_name(phase), pl, flags, unwanted, g_pit_state[pl].load());
    }
    s_last = unwanted;
}

// Routes the warm-up holds let through: leaving to the menu, a flashback, a mid-session save, terminal damage and
// retiring must work in the warm-up too (before, every route was held, so "quit" from the pause menu did nothing).
// Warm-up crash (the in-game state asks for the terminal damage route): the player's car goes back into its garage
// (the game's garage teleport, the car is fixed there) and can drive out again; the warm-up goes on.
bool warm_terminal(uint32_t r)
{
    if (r != rs::route("terminal"))
        return false;
    static uint64_t s_last = 0;
    int pl = player_vehicle();
    void *sys = g_pit_sys.load();
    if (pl >= 0 && sys && GetTickCount64() - s_last > 3000) {
        s_last = GetTickCount64();
        swofty::game::Pit_TeleportToGarage(sys, uint32_t(pl));
        g_player_out = g_player_away = false;
        g_still_t = 0;
        // ... and drives out again on its own a second later (like after the box time).
        g_warm_leave = true;
        g_warm_leave_t = 0;
        swofty::log("[racestart] warm-up: car %d damaged (terminal route held): back into its garage, drive out again",
                    pl);
    }
    return true;
}

bool warm_route_passes(uint32_t r)
{
    // Not "terminal": a crash in the warm-up sends the car back to its garage (warm_terminal), it does not start the
    // race (2026-10-26 00:14: the terminal route went straight to the intro video, the race began without a
    // formation lap).
    // "finished" / "spectate": the session ends (simulated from the pause menu: 2026-10-26 01:08 the held finished
    // route left the game stuck).
    static const uint32_t pass[] = {rs::route("quit"), rs::route("replay"), rs::route("load_mid_session_save"),
                                    rs::route("retire"), rs::route("suspension"), rs::kRouteFinished,
                                    rs::kRouteSpectate};
    for (uint32_t p : pass)
        if (r == p)
            return true;
    return false;
}

// --- flow authoring -------------------------------------------------------------------------------------------
// The warm-up phase is inserted into the race flow before the broadcast part (intro video, talking points, grid
// rundown, grid scene): garage with the practice in-game state, then a formation lap state from the pit lane to the
// grid, then the stock intro.
bool edit_race_flow(std::string &x)
{
    // Warm-up chain before the stock intro: box (practice state) -> race in-game state (the player drives freely,
    // pit exit open, AI in the garages) -> the game's own restart (restartReadyMonitor: cars back on the grid, laps
    // reset) -> stock pre-race, formation lap and race.
    std::string_view from = "<link id=\"next\" target=\"IntroVideo\" />", anchor = "<state name=\"IntroVideo\" ";
    size_t at = x.find(from);
    if (at == std::string::npos || x.find(anchor) == std::string::npos || x.find("restartReadyMonitor") == std::string::npos)
        return false;
    x.replace(at, from.size(), "<link id=\"next\" target=\"WarmUpZoom\" />");
    x.insert(x.find(anchor),
             "<state name=\"WarmUpZoom\" state=\"SessionStateZoomToMonitorCutscene\" audioFilterHash=\"3410201907\">"
             "<link id=\"_interrupt\" target=\"Exit\" /><link id=\"1\" target=\"WarmUpBegin\" /></state>\n"
             "<state name=\"WarmUpBegin\" state=\"SessionStateBeginGameplay\" pausable=\"true\" audioFilterHash=\"2573303364\">"
             "<link id=\"next\" target=\"WarmUpGarage\"/><link id=\"qualifying\" target=\"WarmUpGarage\"/></state>\n"
             "<state name=\"WarmUpGarage\" state=\"SessionStateInGamePQ\" pausable=\"true\" audioFilterHash=\"984691642\" "
             "audioState=\"in_game_practice_qualifying\"><link id=\"_interrupt\" target=\"Exit\" />"
             "<link id=\"quit\" target=\"Quit\"/><link id=\"finished\" target=\"WarmUpPreRace\"/>"
             "<link id=\"retire\" target=\"WarmUpPreRace\"/><link id=\"restart\" target=\"WarmUpPreRace\"/>"
             "<link id=\"terminal\" target=\"WarmUpPreRace\"/></state>\n"
             "<state name=\"WarmUpPreRace\" state=\"SessionStatePreRaceToRace\" pausable=\"true\" audioFilterHash=\"924544851\">"
             "<link id=\"_interrupt\" target=\"Exit\" /><link id=\"next\" target=\"WarmUpBeginRace\" /></state>\n"
             "<state name=\"WarmUpBeginRace\" state=\"SessionStateBeginGameplay\" pausable=\"true\" audioFilterHash=\"2573303364\">"
             "<link id=\"next\" target=\"WarmUpRace\"/></state>\n"
             "<state name=\"WarmUpRace\" state=\"SessionStateInGamePlaying\" pausable=\"true\" audioFilterHash=\"984691642\" "
             "audioState=\"in_game\"><link id=\"_interrupt\" target=\"Exit\" /><link id=\"quit\" target=\"Quit\"/>"
             "<link id=\"finished\" target=\"IntroVideo\"/><link id=\"retire\" target=\"IntroVideo\"/>"
             "<link id=\"terminal\" target=\"IntroVideo\"/><link id=\"spectate\" target=\"IntroVideo\"/>"
             "<link id=\"simulate_session\" target=\"IntroVideo\"/><link id=\"restart\" target=\"restartReadyMonitor\"/>"
             "<link id=\"replay\" target=\"InstantReplayLockFrameRate\"/><link id=\"suspension\" target=\"RedFlagFadeOut\"/>"
             "<link id=\"load_mid_session_save\" target=\"load_mid_session_save\"/></state>\n                ");
    g_flow_gridlap = false;
    if (g_warm_gridlap_on) {
        // Lap to the grid: after the free warm-up a formation lap state (its grid parking logic) where every car
        // leaves the pit lane in grid order and parks on its slot; then the stock intro, grid scene and formation lap.
        std::string_view restart = "<link id=\"restart\" target=\"restartReadyMonitor\"/><link id=\"replay\"";
        size_t w = x.find("<state name=\"WarmUpRace\"");
        size_t at2 = w == std::string::npos ? std::string::npos : x.find(restart, w);
        if (at2 != std::string::npos) {
            x.replace(at2, restart.size(), "<link id=\"restart\" target=\"WarmUpGridLap\"/><link id=\"replay\"");
            // The warm-up IS the lap to the grid (user 2026-10-26 14:05): out of the box (BeginGameplay) straight into
            // it, the free drive in the race state is skipped. The lap to the grid is a formation lap state: its end
            // goes through the stock Restart (SessionStateRestart), which resets the race - straight to the intro, the
            // game took the start without a formation lap but kept the formation lap flag, the field drove
            // "formation lap" for ever (self-test 2026-10-26).
            // With free warm-up laps (user 2026-10-31 14:34: 2-5 laps must work) the box leads into the race state
            // first (the player drives, the AI waits in the garages); its restart link then goes to the lap to the
            // grid.
            std::string_view begin = "<link id=\"next\" target=\"WarmUpRace\"/></state>";
            g_flow_freelaps = g_warmup_laps.load() > 0;
            if (size_t b = x.find(begin); b != std::string::npos && !g_flow_freelaps)
                x.replace(b, begin.size(), "<link id=\"next\" target=\"WarmUpGridLap\"/></state>");
            x.insert(x.find(anchor),
                     "<state name=\"WarmUpGridLap\" state=\"SessionStateFormationLap\" pausable=\"true\" "
                     "audioFilterHash=\"735235189\" audioState=\"in_game\"><link id=\"_interrupt\" target=\"Exit\" />"
                     // The end goes through restartReadyMonitor, the in-game "restart race" path (restart net
                     // message -> Restart). Straight to Restart took its "go" link (skip the formation lap,
                     // PreRace -> start lights) without a reset: the race started with the formation lap state
                     // kept, lap counter and gaps frozen (test 2026-10-29 02:50, log run_cdB_warmup).
                     "<link id=\"quit\" target=\"Quit\"/><link id=\"finished\" target=\"restartReadyMonitor\"/>"
                     "<link id=\"retire\" target=\"restartReadyMonitor\"/><link id=\"terminal\" target=\"restartReadyMonitor\"/>"
                     "<link id=\"skip\" target=\"restartReadyMonitor\"/><link id=\"restart\" target=\"restartReadyMonitor\"/>"
                     "<link id=\"spectate\" target=\"restartReadyMonitor\"/></state>\n                ");
            g_flow_gridlap = true;
        }
    }
    return true;
}

// Red flag restart: the stock flow goes RedFlagToRestart -> PreRaceToRace -> PreLights -> start lights, with the cars
// teleported onto the grid (RedFlagTeleportVehicles). The restart link goes to the formation lap instead
// (FormationLapStartLights -> BeginFormationLap -> FormationLap); its end returns to PreRaceToRace and the start
// lights as after a normal formation lap. The formation lap state then drives the field out of the pit lane onto
// the grid (grid_queue_start).
// Since 2026-10-26 16:28 (user): the restart link goes to an own in-game state, the SC lap: every car drives out of
// the pit lane for one lap behind the leader at safety car pace, then the stock PreRaceToRace and the start lights
// (standing start on the grid). The formation lap states reset the race to lap 1 and disqualified every car off the
// grid (self-test 16:23), the in-game state keeps the race.
bool edit_red_flag_flow(std::string &x)
{
    std::string_view from = "<link id=\"restart\" target=\"PreRacePlayersMonitor/PreRaceToRace\"/>";
    size_t state = x.find("<state name=\"RedFlagToRestart\"");
    size_t at = state == std::string::npos ? std::string::npos : x.find(from, state);
    std::string_view cleanup = "<state name=\"PreGameCleanup\"", ingame = "<state name=\"InGame\"";
    if (at == std::string::npos || at - state > 400 || x.find(cleanup) == std::string::npos ||
        x.find(ingame) == std::string::npos)
        return false;
    x.replace(at, from.size(), "<link id=\"restart\" target=\"RfLapBegin\"/>");
    x.insert(x.find(ingame),
             "<state name=\"RfLap\" state=\"SessionStateInGamePlaying\" pausable=\"true\" audioFilterHash=\"984691642\" "
             "audioState=\"in_game\"><link id=\"_interrupt\" target=\"Exit\" /><link id=\"quit\" target=\"StateCompleteEventOnQuit\"/>"
             "<link id=\"rf_grid\" target=\"PreRacePlayersMonitor/PreRaceToRace\"/>"
             "<link id=\"retire\" target=\"PostRaceCameraControlInhibit/RSCameraInhibit/PTS/RTTDOD/WinnerSequence\"/>"
             "<link id=\"terminal\" target=\"TerminalDamageMonitor\"/>"
             "<link id=\"finished\" target=\"PostRaceCameraControlInhibit/RSCameraInhibit/PTS/RTTDOD/WinnerSequence\"/>"
             "<link id=\"replay\" target=\"InstantReplayLockFrameRate\"/><link id=\"spectate\" target=\"Spectate\"/>"
             "<link id=\"suspension\" target=\"RedFlagFadeOut\"/></state>\n            ");
    x.insert(x.find(cleanup),
             "<state name=\"RfLapBegin\" state=\"SessionStateBeginGameplay\" pausable=\"true\" audioFilterHash=\"2573303364\">"
             "<link id=\"next\" target=\"SplitscreenMonitor/RfLap\"/><link id=\"qualifying\" target=\"SplitscreenMonitor/RfLap\"/>"
             "</state>\n        ");
    return true;
}

// Developer override: <game>/swofty/dev/flow_<name>.xml replaces the flow <name> on the next session load.
bool dev_flow(std::string_view text, std::string &out)
{
    size_t at = text.find("<state_machine name=\"");
    if (at == std::string_view::npos)
        return false;
    at += 21;
    size_t end = text.find('\x22', at); // a lone quote char literal confuses merge.py's string scanner
    if (end == std::string_view::npos || end - at > 64)
        return false;
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string path(exe, n);
    path = path.substr(0, path.find_last_of("\\/") + 1) + "swofty\\dev\\flow_" + std::string(text.substr(at, end - at)) +
           ".xml";
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0)
        out.append(chunk, got);
    std::fclose(f);
    swofty::log("[racestart] dev: flow replaced by %s (%zu bytes)", path.c_str(), out.size());
    return !out.empty();
}

// --- drive to the grid (grid queue) ---------------------------------------------------------------------------
// Every classified AI car (and the player, unless warm: the player leaves the box with the control window) queues in
// grid order, player first; race_order: race order (laps + lap fraction) instead, the player in his place (red flag
// drive to the grid).
// Race Start (0.37, Engine-RE): on the game's own pit lane - every car goes into its garage (Pit_TeleportToGarage;
// the game holds it there, pit state InGarage) and leaves through the practice drive-out
// (Pit_VehicleWantsToLeaveGarage 0x1434d1350), one every kGridGap s, so the game's pit lane AI drives it down the pit
// lane with the limiter (the player's warm-up drive-out does this inside the formation lap state: 9 -> 10 -> ... ->
// OnTrack, log run_cdB_warmup 2026-10-29). No per-frame pin, no pit exit table: works on every track.
// The other parts (red flag) keep the old queue: cars pinned behind the pit exit head, 10 m apart, one per second.
constexpr double kGridGap = 1.5;
double g_grid_leave_t[kMaxCars]; // Race Start: seconds since the drive-out was sent (sent again after 4 s in the garage)
// Warm-up lap to the grid: the AI cars wait until the player's car has left its garage (user 2026-10-31 14:15: in
// Monaco half the grid was already on its way while the player still sat in the box).
bool g_grid_wait_player = false;

void grid_queue_start(bool warm, bool race_order = false)
{
    if (!g_rules || (!kStart && !g_exit))
        return;
    if (kStart && !g_pit_sys.load()) {
        swofty::log("[racestart] drive to the grid: pit lane system not seen yet, the cars stay where they are");
        return;
    }
    g_grid_wait_player = kStart && warm && player_vehicle() >= 0;
    Field f = read_field(!race_order);
    float bx, bz;
    grid_back(f, bx, bz);
    if (race_order && g_start_n > 1) {
        // The field stands in the pit lane, not on the grid: "back" is the grid's direction (first -> last slot).
        float dx = g_start_slot[g_start_n - 1][12] - g_start_slot[0][12];
        float dz = g_start_slot[g_start_n - 1][14] - g_start_slot[0][14], d = std::sqrt(dx * dx + dz * dz);
        if (d > 1.f)
            bx = dx / d, bz = dz / d;
    }
    int order[kMaxCars], n = 0;
    for (int i = 0; i < f.n; i++)
        if (!(warm && human(f.ids[i])))
            order[n++] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            bool hj = !race_order && human(f.ids[order[j]]), hi = !race_order && human(f.ids[order[i]]);
            if (hj > hi || (hj == hi && f.pos[order[j]] > f.pos[order[i]]))
                std::swap(order[i], order[j]);
        }
    void *sys = g_pit_sys.load();
    for (int i = 0; i < n; i++) {
        int id = f.ids[order[i]];
        if constexpr (kStart) {
            if (g_pit_state[id] != rs::kPitInGarage) {
                swofty::game::Pit_TeleportToGarage(sys, uint32_t(id));
                g_pit_state[id] = rs::kPitInGarage;
            }
            g_grid_leave_t[id] = 0;
            g_grid_pinned[id] = true; // here: waits in its garage
            g_grid_order[g_grid_n++] = id;
            continue;
        }
        const float *m = carhold::matrix(f.car[id]);
        if (!m)
            continue;
        // A car that sat in its garage is held there every frame until its pit state leaves the garage.
        if (sys && g_pit_state[id] == rs::kPitInGarage)
            swofty::game::Pit_SetPitState(sys, uint32_t(id), rs::kPitOnTrack);
        std::memcpy(g_grid_mtx[id], m, sizeof g_grid_mtx[id]);
        g_grid_mtx[id][12] = g_exit->head[0] + bx * 10.0f * g_grid_n;
        g_grid_mtx[id][13] = g_exit->head[1] + 0.5f;
        g_grid_mtx[id][14] = g_exit->head[2] + bz * 10.0f * g_grid_n;
        carhold::pin(f.car[id], g_grid_mtx[id], 1.f / 60.f, "GridLap");
        g_grid_car[id] = f.car[id];
        g_grid_pinned[id] = true;
        g_grid_order[g_grid_n++] = id;
    }
    g_grid_timer = 0;
    g_grid_next = 0;
    g_frozen_count = g_grid_n;
    if constexpr (kStart)
        swofty::log("[racestart] drive to the grid: %d cars in their garages, they drive out in grid order every %.1f s",
                    g_grid_n, kGridGap);
    else
        swofty::log("[racestart] drive to the grid: %d cars put into the pit lane (%s), they leave in grid order",
                    g_grid_n, g_exit->track);
}

void grid_queue_update(double dt)
{
    if (!g_grid_n)
        return;
    if (g_grid_wait_player) {
        // Out of the garage = LeavingGarage (13), LeavingPitLane or on the track.
        int pl = player_vehicle(), st = pl >= 0 ? g_pit_state[pl].load() : rs::kPitOnTrack;
        if (st == 13 || st == rs::kPitLeavingPitLane || st == rs::kPitOnTrack) {
            g_grid_wait_player = false;
            g_grid_timer = 0;
            swofty::log("[racestart] drive to the grid: the player is out of the garage, the field follows in grid order");
        }
    }
    if (!g_grid_wait_player)
        g_grid_timer += dt;
    void *sys = g_pit_sys.load();
    double gap = kStart ? kGridGap : 1.0;
    if (g_grid_next < g_grid_n && g_grid_timer >= 1.0 + gap * g_grid_next) {
        int id = g_grid_order[g_grid_next++];
        if (g_grid_pinned[id]) {
            g_grid_pinned[id] = false;
            if (kStart && sys) {
                g_grid_leave_t[id] = 0;
                swofty::game::Pit_VehicleWantsToLeaveGarage(sys, uint8_t(id), 0);
            }
            logv("[racestart] drive to the grid: vehicle %d leaves the %s (%d of %d)", id, kStart ? "garage" : "pit lane",
                 g_grid_next, g_grid_n);
        }
    }
    for (int i = 0; i < g_grid_n; i++) {
        int id = g_grid_order[i];
        if constexpr (kStart) {
            if (!sys)
                break;
            int st = g_pit_state[id].load();
            if (g_grid_pinned[id]) {
                if (st == rs::kPitWantsToLeave) // not its turn yet
                    swofty::game::Pit_SetPitState(sys, uint32_t(id), rs::kPitInGarage);
            } else if (st == rs::kPitInGarage && (g_grid_leave_t[id] += dt) > 4.0) {
                g_grid_leave_t[id] = 0;
                swofty::game::Pit_SetPitState(sys, uint32_t(id), rs::kPitWantsToLeave);
                logv("[racestart] drive to the grid: vehicle %d still in its garage, WantsToLeave again", id);
            }
        } else if (g_grid_pinned[id]) {
            carhold::pin(g_grid_car[id], g_grid_mtx[id], float(dt), "GridLap", true);
        }
    }
}

void grid_queue_clear()
{
    for (auto &p : g_grid_pinned)
        p = false;
    g_grid_n = g_grid_next = 0;
    g_frozen_count = 0;
}

// --- pit lane starters ----------------------------------------------------------------------------------------
// Only with a formation lap (user 2026-10-31 21:48): the starters (AI cars, the player as an option) drive the
// formation lap from the grid and turn into the pit lane at its end. The game's own pit entry takes them
// (setPitState EnteringPitLane in the entry window, as the red flag and the cool-down do, pit_entry.hpp), they drive
// through their box without a stop (DrivingThrough, as in the warm-up) and are held at the end of the pit lane (the
// pit exit head, 10 m apart, the 0.36.0 hold) until the field has passed after the start or the hold time is used up.
// Without a formation lap there are no pit lane starters (the garage teleports of 0.37.x put cars off the track).

void mark_starter(int id, const Field &f, const char *why)
{
    g_chosen |= 1u << id;
    g_pls_form_prog[id] = f.progress[id];
    swofty::log("[racestart] vehicle %d starts from the pit lane: turns into the pit lane at the end of the formation "
                "lap (%s, progress %.3f)", id, why, f.progress[id]);
}

// First frame of the formation lap.
void pick_starters(const char *why)
{
    if (!pls_on() || !g_pit_sys.load() || g_chosen)
        return;
    Field f = read_field(false);
    grid_back(f, g_pls_bx, g_pls_bz);
    int order[kMaxCars], n = 0;
    for (int i = 0; i < f.n; i++)
        if (!human(f.ids[i]))
            order[n++] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (f.pos[order[j]] < f.pos[order[i]])
                std::swap(order[i], order[j]);
    if (g_who_random)
        for (int i = n - 1; i > 0; i--) {
            int j = int((uint32_t(f.ids[order[i]]) * 2654435761u + uint32_t(f.n) * 40503u + uint32_t(i)) % uint32_t(i + 1));
            std::swap(order[i], order[j]);
        }
    int want = g_starters.load();
    swofty::log("[racestart] %s: %d cars, %d %s AI car(s) start from the pit lane%s", why, f.n, want,
                g_who_random ? "random" : "last", g_player_pls ? ", the player too" : "");
    for (int i = 0; i < n && i < want; i++)
        mark_starter(f.ids[order[i]], f, "AI");
    if (int pl = player_vehicle(); g_player_pls && pl >= 0 && pl < kMaxCars) {
        mark_starter(pl, f, "the player");
        swofty::hud::toast("Pit lane start", "Take the pit entry at the end of the formation lap", 10.0f,
                           "formation_lap");
    }
}

// Formation lap and race, every frame of the in-game state.
void starters_update(double dt, bool race)
{
    uint32_t chosen = g_chosen;
    void *sys = g_pit_sys.load();
    if (!chosen || !sys)
        return;
    if (race) {
        g_race_time += dt;
        if (float back = ::rd::take_rewind(g_rewinds_seen); back > 0) {
            g_race_time = g_race_time > back ? g_race_time - back : 0;
            swofty::log("[racestart] flashback %.1f s: pit lane starters' race time back to %.0f s", back, g_race_time);
        }
    }
    if (uint32_t through = g_pls_through.exchange(0))
        for (int v = 0; v < kMaxCars; v++)
            if (through & (1u << v)) {
                swofty::game::Pit_SetPitState(sys, uint32_t(v), rs::kPitDrivingThrough);
                swofty::log("[racestart] pit lane starter %d drives through its box, no pit stop", v);
            }
    float lo = 0, hi = 0;
    bool have_window = pitentry::window(g_pf ? g_pf->progress : -1.0f, lo, hi);
    Field f = read_field(false);
    for (int v = 0; v < kMaxCars; v++) {
        if (!(chosen & (1u << v)) || (g_released & (1u << v)) || !f.seen[v])
            continue;
        int st = g_pit_state[v].load();
        const float *m = carhold::matrix(f.car[v]);
        // 1. End of the formation lap: into the pit lane (the player may take the entry himself; the call is the same).
        if (!(g_pls_sent & (1u << v))) {
            float frac = f.progress[v] - std::floor(f.progress[v]);
            bool in_lane = st >= rs::kPitEnteringLane && st <= rs::kPitDrivingThrough && st != rs::kPitInGarage;
            if (in_lane || (!race && have_window && f.progress[v] - g_pls_form_prog[v] > 0.5f &&
                            pitentry::in_window(frac, lo, hi))) {
                g_pls_sent |= 1u << v;
                if (!in_lane)
                    swofty::game::Pit_SetPitState(sys, uint32_t(v), rs::kPitEnteringLane);
                swofty::log("[racestart] pit lane starter %d sent into the pit lane at %.4f of the lap (window "
                            "%.3f..%.3f, pit state %d)", v, frac, lo, hi, st);
            }
            continue;
        }
        // 2. In the pit lane: held once it reaches the pit exit (or leaves the lane when the exit is not known).
        if (!g_frozen[v]) {
            float d = 1e9f;
            if (g_exit && m)
                d = std::hypot(m[12] - g_exit->head[0], m[14] - g_exit->head[2]);
            if (m && (d < 12.0f || (!g_exit && st == rs::kPitLeavingPitLane))) {
                int place = g_frozen_count++;
                std::memcpy(g_frozen_mtx[v], m, sizeof g_frozen_mtx[v]);
                if (g_exit) {
                    g_frozen_mtx[v][12] = g_exit->head[0] + g_pls_bx * 10.0f * place;
                    g_frozen_mtx[v][13] = g_exit->head[1] + 0.5f;
                    g_frozen_mtx[v][14] = g_exit->head[2] + g_pls_bz * 10.0f * place;
                }
                carhold::pin(f.car[v], g_frozen_mtx[v], float(dt), "PitLaneStart");
                g_frozen[v] = true;
                swofty::log("[racestart] pit lane starter %d waits at the end of the pit lane (place %d, %.1f %.1f %.1f, "
                            "pit state %d)", v, place, g_frozen_mtx[v][12], g_frozen_mtx[v][13], g_frozen_mtx[v][14], st);
            }
            continue;
        }
        // 3. Waiting at the pit exit: until the field has passed after the start.
        bool release = false, all_past = true;
        if (race) {
            if (!g_start_prog_set) {
                g_start_prog_set = true;
                for (int i = 0; i < f.n; i++)
                    g_start_prog[f.ids[i]] = f.progress[f.ids[i]];
            }
            // Every other car is 5 % of a lap beyond where it stood at the start.
            for (int i = 0; i < f.n; i++)
                if (int id = f.ids[i]; id != v && !(chosen & (1u << id)) && f.progress[id] < g_start_prog[id] + 0.05f)
                    all_past = false;
            release = (all_past && g_race_time > 3.0) || g_race_time >= g_hold_max;
        }
        if (release) {
            g_frozen[v] = false;
            g_released |= 1u << v;
            swofty::log("[racestart] pit lane starter %d leaves the pit lane after %.0f s of racing (%s)", v, g_race_time,
                        all_past ? "the field has passed" : "hold time used up");
        } else
            carhold::pin(f.car[v], g_frozen_mtx[v], float(dt), "PitLaneStart", true);
    }
}

// --- warm-up --------------------------------------------------------------------------------------------------
// Hint in the game's own race notification while the player's car waits in its garage or box in the warm-up or the
// lap to the grid (once per phase).
void drive_out_hint(int pl)
{
    static Phase s_hinted = Phase::Idle;
    Phase ph = g_phase;
    if (pl < 0 || s_hinted == ph)
        return;
    int st = g_pit_state[pl].load();
    if (st != rs::kPitInGarage && st != rs::kPitInBox)
        return;
    s_hinted = ph;
    swofty::hud::toast("Pit lane", "Drive out: Ctrl+Alt+G, your drive-out wheel button or the control window", 10.0f,
                       "formation_lap");
}

// End of the lap to the grid: the stock flow puts the cars on their slots in its own formation lap, the grid scene
// (PreRace) after our lap does not. A car parked a few metres off (or still in the pit lane) then stands where the
// grid scene's people stand. Every car with a recorded slot is put exactly onto it; a car still in the pit lane or
// garage gets the pit state OnTrack first.
void snap_to_slots(const char *why)
{
    Field f = read_field(false);
    void *sys = g_pit_sys.load();
    int n = 0;
    for (int i = 0; i < f.n; i++) {
        int v = f.ids[i];
        if (!g_slot_ok[v] || !f.car[v])
            continue;
        int st = g_pit_state[v].load();
        if (sys && st >= 0 && st != rs::kPitOnTrack)
            swofty::game::Pit_SetPitState(sys, uint32_t(v), rs::kPitOnTrack);
        carhold::pin(f.car[v], g_slot_mtx[v], 1.f / 60.f, "GridSlot");
        n++;
    }
    swofty::log("[racestart] %s: %d cars put exactly onto their grid slots for the grid scene", why, n);
}

// Drive out exactly like the practice garage button: PitLaneSystem::vehicleWantsToLeaveGarage(pit, car, 0) sends the
// garage crew's "send car to track" (the crew steps back) and sets the pit state WantsToLeave.
int g_drive_out_car = -1;
double g_drive_out_t = 0;
int g_drive_out_state = -1;

void leave_box(int pl)
{
    void *sys = g_pit_sys.load();
    if (pl < 0 || !sys)
        return;
    g_drive_out_car = pl;
    g_drive_out_t = 0;
    g_drive_out_state = g_pit_state[pl].load();
    swofty::game::Pit_VehicleWantsToLeaveGarage(sys, uint8_t(pl), 0);
    swofty::log("[racestart] drive out: vehicle %d, the game's practice drive-out (crew + pit state, from %d)", pl,
                g_drive_out_state);
}

// After the drive-out: log what the pit state does; when nothing moved in 4 s, set WantsToLeave again.
void drive_out_check(double dt)
{
    if (g_drive_out_car < 0)
        return;
    g_drive_out_t += dt;
    int st = g_pit_state[g_drive_out_car].load();
    if (st != g_drive_out_state) {
        swofty::log("[racestart] drive out: vehicle %d pit state %d -> %d after %.1f s", g_drive_out_car,
                    g_drive_out_state, st, g_drive_out_t);
        g_drive_out_car = -1;
        return;
    }
    if (g_drive_out_t < 4.0)
        return;
    if (void *sys = g_pit_sys.load()) {
        swofty::game::Pit_SetPitState(sys, uint32_t(g_drive_out_car), rs::kPitWantsToLeave);
        swofty::log("[racestart] drive out: nothing moved in 4 s, vehicle %d: pit state WantsToLeave again",
                    g_drive_out_car);
    }
    g_drive_out_car = -1;
}

// --- cool-down ------------------------------------------------------------------------------------------------
float parc_ferme_distance(uintptr_t rec)
{
    float frac, length = *swofty::game::g_Track_Length;
    uint32_t laps;
    if (!g_pf || !rd(rec + rs::kRecLaps, laps) || !rd(rec + rs::kRecLapFrac, frac) || length <= 0)
        return -1;
    float d = std::fabs(frac - g_pf->progress);
    return std::fmin(d, 1 - d) * length;
}

void start_cooldown(void *system, int v, bool ai)
{
    find_tables();
    set_phase(Phase::Cooldown, ai ? "player finished, the autopilot drives on" : "player finished, keeps driving");
    g_cd_holding = true;
    g_cd_player_ai = ai;
    g_cd_held = g_cd_parked = g_cd_next_log = 0;
    g_cd_player = v;
    g_ctrl_sys = system;
    g_cd_in_pit_lane = false;
    {
        Field f = read_field(false);
        g_cd_finish_laps = v >= 0 && f.seen[v] ? uint32_t(std::floor(f.progress[v])) : 0;
        g_cd_crossed = 0;
        for (int i = 0; i < f.n; i++)
            g_cd_prev_frac[f.ids[i]] = f.progress[f.ids[i]] - std::floor(f.progress[f.ids[i]]);
    }
    devvars::publish("cd_hold", 1); // the red flag module: no pit entry learning from the cars we send
    for (auto &d : g_min_pf)
        d = 1e9f;
    for (auto &f : g_frozen)
        f = false;
    g_frozen_count = 0;
    g_logged_pit = 0;
    g_cd_game_held = g_cd_pin = 0;
    if (g_pf)
        swofty::log("[racestart] cool-down: parc ferme / podium point of %s at (%.1f, %.1f), %s the pit lane%s", g_pf->track,
                    g_pf->pos[0], g_pf->pos[2], g_pf->in_pit_lane ? "beside" : "NOT at",
                    g_pf->in_pit_lane ? "" : " (parking before the pit exit instead)");
    swofty::log("[racestart] cool-down: parc ferme %s at %.4f of the lap", g_pf ? g_pf->track : "unknown",
                g_pf ? g_pf->progress : -1.0f);
    {
        int top[3] = {-1, -1, -1};
        char out[160];
        int len = 0;
        for (int c = 0; c < kMaxCars; c++) {
            if (int r = cd_rank(c); r >= 1 && r <= 3)
                top[r - 1] = c;
            else if (retired(c) && rank_of(c) && len < 120)
                len += std::snprintf(out + len, sizeof out - len, " %d(was P%d)", c, rank_of(c));
        }
        swofty::log("[racestart] cool-down: parc ferme cars P1 %d, P2 %d, P3 %d; retired, skipped:%s", top[0], top[1],
                    top[2], len ? out : " none");
    }
    g_board_ok = false;
    for (int v = 0; v < kMaxCars; v++)
        g_on_board[v] = false, g_slide_t[v] = -1.0f, g_board_still[v] = 0.0f;
    float pm[16];
    void *reg = *swofty::game::g_Cutscene_Placements;
    // Also off the pit lane (Mexico stadium): the top 3 then stay on the track and park at the boards there (the
    // in_pit_lane condition here left Mexico without boards, test 2026-10-29 04:49).
    if (g_pf && reg && trackpts::placement_matrix(reg, "ParcFerme", pm)) {
        for (int k = 0; k < 3; k++)
            for (int a = 0; a < 3; a++)
                g_board[k][a] = kBoardLocal[k][0] * pm[a] + kBoardLocal[k][1] * pm[4 + a] + kBoardLocal[k][2] * pm[8 + a] +
                                pm[12 + a];
        g_board_ok = true;
        boards::show(g_board);
        swofty::log("[racestart] cool-down: parc ferme boards P1 (%.1f, %.1f), P2 (%.1f, %.1f), P3 (%.1f, %.1f)",
                    g_board[0][0], g_board[0][2], g_board[1][0], g_board[1][2], g_board[2][0], g_board[2][2]);
    } else {
        swofty::log("[racestart] cool-down: no parc ferme boards (placement not found), the cars stop in a row");
    }
    if (!ai)
    {
        int k = cd_rank(v) - 1;
        char text[96];
        if (g_board_ok && k >= 0 && k < 3)
            std::snprintf(text, sizeof text, "Park on your P%d board in parc ferme (%s)", k + 1,
                          g_pf && g_pf->in_pit_lane ? "pit lane, under the podium" : "on the track");
        else
            std::snprintf(text, sizeof text, "Drive into the pit lane and stop before the pit exit");
        swofty::hud::toast("Cool-down lap", text, 10.0f, "chequered");
    }
}

// Parc ferme marking (prototype). The podium script puts the scene "parcferme_geo" (floor with the P1/P2/P3
// boxes, model core_assets/objects/idf/pitlane/parcferme_floor_a) at the track's ParcFerme placement only when the
// podium cutscene starts, from the cutscene packages. In the cool-down, every 5 s: is that scene loaded (scene
// library lookup)? If so it is spawned once at the ParcFerme placement. Every try is logged (measurement: is the
// podium package loaded during the race?). The copy is not removed when the podium starts (no remove call yet).
void pf_marking_update(double dt)
{
    if (!g_pf_mark_on || g_pf_mark || g_pf_mark_tries >= 60)
        return;
    g_pf_mark_t -= dt;
    if (g_pf_mark_t > 0)
        return;
    g_pf_mark_t = 5.0;
    g_pf_mark_tries++;
    void *root = *swofty::game::g_Scene_Library;
    uintptr_t lib = 0;
    if (!root || !rd(reinterpret_cast<uintptr_t>(root) + 0x68, lib) || !lib) {
        swofty::log("[racestart] parc ferme marking: scene library not there (try %d)", g_pf_mark_tries);
        return;
    }
    static const char *names[] = {"podium2022_parcferme_geo", "parcferme_geo", "parcferme_floor_a", "parcferme_f2_a"};
    void *scene = nullptr;
    const char *found = nullptr;
    char seen[200];
    int len = 0;
    for (const char *n : names) {
        void *s = swofty::game::Scene_FindByName(reinterpret_cast<void *>(lib), n, false);
        len += std::snprintf(seen + len, sizeof seen - len, " %s=%s", n, s ? "loaded" : "no");
        if (s && !scene)
            scene = s, found = n;
    }
    float mtx[16];
    void *reg = *swofty::game::g_Cutscene_Placements;
    bool have_spot = reg && trackpts::placement_matrix(reg, "ParcFerme", mtx);
    if (g_pf_mark_tries == 1 || scene || g_pf_mark_tries % 6 == 0)
        swofty::log("[racestart] parc ferme marking (try %d): scenes%s; parc ferme placement %s", g_pf_mark_tries, seen,
                    have_spot ? "found" : "NOT found");
    if (!scene || !have_spot)
        return;
    // Flags block as the race marshal spawn uses it (0x1403d2ed0).
    uint8_t flags[0x20] = {0, 0, 2, 0, 0, 1, 0, 0, 0, 1, 0, 1};
    alignas(16) static uint8_t over_a[0x100], over_b[0x100]; // empty override lists (counts at +0xa0 / +0x50 = 0)
    std::memset(over_a, 0, sizeof over_a);
    std::memset(over_b, 0, sizeof over_b);
    swofty::log("[racestart] parc ferme marking: spawning %s at (%.1f, %.1f, %.1f)", found, mtx[12], mtx[13], mtx[14]);
    g_pf_mark = swofty::game::Scene_Spawn(reinterpret_cast<void *>(lib), flags, scene, mtx, over_a, over_b, 0);
    swofty::log("[racestart] parc ferme marking: %s (instance %p)", g_pf_mark ? "spawned" : "spawn returned nothing",
                g_pf_mark);
    if (!g_pf_mark)
        g_pf_mark_tries = 60; // do not try again this session
}

// The game's own hold of ONE car, the way Sess_FreezeAllVehicles (0x142c76220) holds the whole field under the red
// flag (instead of our per-frame pin): setController(AI, SESSION_NOT_STARTED), participant +0x17c |= 0x80 on entry v
// of [state+0xd40]+0x70 (list order = car id: as the hold probe B), and the start hold bit 0x20 at +0x1c of the AI
// controller state ([ControllerSet+0x30][v]+0x20, vtable slot 3), which keeps the cars on the grid before the
// lights. Released by the game at the next start lights (Sess_Controllers_ReleaseForRaceStart).
void cd_game_hold(int v)
{
    void *ctrl = g_ctrl_sys.load();
    if (!ctrl || v < 0 || v >= kMaxCars || (g_cd_game_held & (1u << v)))
        return;
    swofty::game::Veh_SetController_Thunk(ctrl, uint64_t(v), 1, rs::kFlagSessionNotStarted);
    bool part = false, start_hold = false;
    uintptr_t list = 0, it = 0, end = 0, e = 0;
    if (rd(reinterpret_cast<uintptr_t>(g_race_state) + 0xd40, list) && rd(list + 0x70, it) && rd(list + 0x78, end) &&
        end > it && uintptr_t(v) < (end - it) / 8 && rd(it + 8u * unsigned(v), e) && e &&
        swofty::mem::readable(e + 0x17c, 1)) {
        *reinterpret_cast<volatile uint8_t *>(e + 0x17c) |= 0x80;
        part = true;
    }
    uintptr_t cs = reinterpret_cast<uintptr_t>(ctrl), vb = 0, ve = 0, c = 0, aic = 0, vt = 0, fn = 0;
    if (rd(cs + 0x30, vb) && rd(cs + 0x38, ve) && ve > vb && uintptr_t(v) < (ve - vb) / 8 &&
        rd(vb + 8u * unsigned(v), c) && c && rd(c + 0x20, aic) && aic && rd(aic, vt) && rd(vt + 0x18, fn) && fn) {
        uintptr_t s = reinterpret_cast<uintptr_t (*)(uintptr_t)>(fn)(aic);
        if (s && swofty::mem::readable(s + 0x1c, 1)) {
            *reinterpret_cast<volatile uint8_t *>(s + 0x1c) |= 0x20;
            start_hold = true;
        }
    }
    g_cd_game_held |= 1u << v;
    g_cd_hold_t[v] = 0.0f;
    swofty::log("[racestart] cool-down: vehicle %d held by the game (SESSION_NOT_STARTED, participant bit %s, start "
                "hold %s)", v, part ? "set" : "not found", start_hold ? "set" : "not found");
}

void end_cooldown(const char *why)
{
    g_cd_holding = false;
    g_cd_done = true;
    boards::hide();
    // A player parked in parc ferme stays there: the game's hold instead of the handover to the AI (it drove off).
    bool parked = !g_cd_player_ai && g_cd_parked >= 2.0 && g_cd_player >= 0;
    swofty::log("[racestart] cool-down over after %.1f s (%s); vehicle %d %s", g_cd_held, why, g_cd_player,
                parked ? "stays parked (game hold)" : "to the AI");
    devvars::publish("cd_hold", 2);
    {
        char buf[200];
        int len = 0;
        for (int v = 0; v < 32 && len < 170; v++)
            if (g_min_pf[v] < 1e8f)
                len += std::snprintf(buf + len, sizeof buf - len, " %d:%.1f", v, g_min_pf[v]);
        if (len)
            swofty::log("[racestart] cool-down: closest to the pit exit (id:m):%s", buf);
    }
    if (parked)
        cd_game_hold(g_cd_player);
    else if (void *sys = g_ctrl_sys.load(); sys && g_cd_player >= 0)
        swofty::game::Veh_SetController_Thunk(sys, uint64_t(g_cd_player), 1, rs::kFlagNotInSession);
    set_phase(Phase::Over, why);
}

// Parked = in the pit lane (or on track where the placement is), within 25 m of it along the lap, below 0.5 m/s
// for 2 s.
bool cd_parked(double dt)
{
    uintptr_t rec = record_of(g_cd_player);
    float speed = -1, dist = rec ? parc_ferme_distance(rec) : -1;
    // Speed from the car matrix over about 0.25 s (speed_of reads a wrong value, ~5.5 m/s always; Race Start 10-31).
    {
        static float s_pos[2];
        static double s_t = -1;
        static float s_speed = -1;
        Field pf = read_field(false);
        const float *pm = g_cd_player >= 0 && pf.seen[g_cd_player] ? carhold::matrix(pf.car[g_cd_player]) : nullptr;
        if (!pm || s_t < 0 || g_cd_held < s_t) {
            if (pm)
                s_pos[0] = pm[12], s_pos[1] = pm[14], s_t = g_cd_held, s_speed = -1;
        } else if (g_cd_held - s_t >= 0.25) {
            s_speed = float(std::hypot(pm[12] - s_pos[0], pm[14] - s_pos[1]) / (g_cd_held - s_t));
            s_pos[0] = pm[12], s_pos[1] = pm[14], s_t = g_cd_held;
        }
        speed = pm ? s_speed : -1;
    }
    if (g_cd_held >= g_cd_next_log) {
        g_cd_next_log = g_cd_held + 2.0;
        logv("[racestart] cool-down t %.0f s: speed %.2f m/s, pit state %d", g_cd_held, speed,
             g_cd_player >= 0 ? g_pit_state[g_cd_player].load() : -1);
    }
    // Top 3 with boards: the driver parks at his own board (within kParkRadius of it); a hint with the distance and
    // direction from 150 m on. Otherwise the whole pit lane is the parc ferme: stopped anywhere in it counts
    // (or within 30 m of an on-track parc ferme).
    int k = g_cd_player >= 0 ? cd_rank(g_cd_player) - 1 : -1;
    bool at_spot = false;
    if (g_board_ok && k >= 0 && k < 3) {
        Field f = read_field(false);
        const float *m = f.seen[g_cd_player] ? carhold::matrix(f.car[g_cd_player]) : nullptr;
        if (m) {
            float bx = g_board[k][0] - m[12], bz = g_board[k][2] - m[14];
            float d = std::sqrt(bx * bx + bz * bz);
            // Driving direction from the car's own movement (last 1 m driven): the hint says ahead / behind and how
            // far to the side. Every 3 s from 150 m, every second from 40 m (user 2026-10-31 15:49: searched 2 min
            // for the spot 5 m away with the 2.5 m window and a distance-only hint).
            static float s_last[2], s_dir[2];
            static bool s_dir_ok = false;
            if (g_cd_held < 0.1)
                s_dir_ok = false, s_last[0] = m[12], s_last[1] = m[14];
            float mx = m[12] - s_last[0], mz = m[14] - s_last[1], mv = std::sqrt(mx * mx + mz * mz);
            if (mv > 1.0f)
                s_dir[0] = mx / mv, s_dir[1] = mz / mv, s_dir_ok = true, s_last[0] = m[12], s_last[1] = m[14];
            static double hint_t = 0;
            if (g_cd_held < hint_t - 10.0)
                hint_t = 0;
            if (d < 150.0f && g_cd_held >= hint_t) {
                hint_t = g_cd_held + (d < 40.0f ? 1.0 : 3.0);
                char text[64];
                if (d < kParkRadius)
                    std::snprintf(text, sizeof text, "P%d: here - stop", k + 1);
                else if (s_dir_ok) {
                    float along = bx * s_dir[0] + bz * s_dir[1], side = std::fabs(bx * s_dir[1] - bz * s_dir[0]);
                    std::snprintf(text, sizeof text, "P%d board: %.0f m %s, %.0f m to the side", k + 1,
                                  std::fabs(along), along >= 0 ? "ahead" : "behind", side);
                } else
                    std::snprintf(text, sizeof text, "P%d board: %.0f m", k + 1, d);
                swofty::hud::toast("Parc ferme", text, d < 40.0f ? 1.2f : 3.0f, "chequered");
            }
            at_spot = speed >= 0 && speed < 0.5f && d < kParkRadius;
            if (at_spot && g_cd_parked == 0)
                swofty::log("[racestart] vehicle %d (P%d) stopped %.1f m from its parc ferme board", g_cd_player, k + 1, d);
        }
    } else {
        int st = g_cd_player >= 0 ? g_pit_state[g_cd_player].load() : -1;
        bool in_lane = g_cd_in_pit_lane || (st >= rs::kPitEnteringLane && st <= rs::kPitDrivingThrough && st != rs::kPitInGarage);
        bool on_track_pf = g_pf && !g_pf->in_pit_lane && dist >= 0 && dist < 30.0f;
        at_spot = speed >= 0 && speed < 0.5f && (in_lane || on_track_pf);
    }
    g_cd_parked = at_spot ? g_cd_parked + dt : 0;
    return g_cd_parked >= 2.0;
}

// After the finish the AI (and the autopilot) just keep lapping: the top 3 (parc ferme parking) and the autopilot's
// player are handed to the pit lane manager when they pass the pit entry (setPitState EnteringPitLane, see
// pit_entry.hpp); the placement below then parks them in parc ferme.
// Sends cars into the pit lane through the pit lane manager (pit_entry.hpp): in the cool-down the AI top 3 (parc
// ferme) and the autopilot's player; under a red flag hold (dev var "rf_hold" from the red flag module) every car,
// the player only on autopilot (a human drives in by himself). Every 10 s the cars still to be sent are logged
// with their lap fraction and pit state.
double g_send_log_t = 0;

void send_into_the_pits(bool red_flag, double dt)
{
    void *sys = g_pit_sys.load();
    float lo, hi;
    g_send_log_t += dt;
    bool log_now = g_send_log_t >= 10.0;
    if (log_now)
        g_send_log_t = 0;
    bool have_window = pitentry::window(g_pf ? g_pf->progress : -1.0f, lo, hi);
    if (!sys || !have_window) {
        if (log_now)
            swofty::log("[racestart] pit entry: pit system %s, window %s (parc ferme %.3f)", sys ? "known" : "UNKNOWN",
                        have_window ? "ok" : "NONE", g_pf ? g_pf->progress : -1.0f);
        return;
    }
    Field f = read_field(false);
    char pend[240];
    int plen = 0;
    for (int i = 0; i < f.n; i++) {
        int v = f.ids[i];
        float frac = f.progress[v] - std::floor(f.progress[v]);
        bool player = human(v);
        bool wanted;
        if (red_flag)
            wanted = !player || autopilot();
        else
            wanted = v == g_cd_player ? g_cd_player_ai.load()
                                      : g_park_top3 && g_cd_end == CdEnd::ParcFerme && cd_rank(v) >= 1 && cd_rank(v) <= 3;
        if (!red_flag && !player && (g_cd_sent & (1u << v)) == 0) {
            // The game stops counting a finished car's laps (Mexico 0.32.8: P2/P3 never counted as crossed).
            if (g_cd_prev_frac[v] - frac > 0.5f || f.progress[v] >= float(g_cd_finish_laps))
                g_cd_crossed |= 1u << v;
            g_cd_prev_frac[v] = frac;
        }
        // Parc ferme on the track (Mexico stadium): the top 3 stay on track and park at their boards there.
        if (!red_flag && g_board_ok && g_pf && !g_pf->in_pit_lane && cd_rank(v) >= 1 && cd_rank(v) <= 3)
            wanted = false;
        if (!wanted || (g_cd_sent & (1u << v)))
            continue;
        // An AI car behind the player has not finished yet: it is sent once it has the player's final lap count
        // (the pit entry is before the line; sent earlier it would pit instead of finishing).
        if (!red_flag && !player) {
            // The game stops counting a car's laps once it has finished, so the line crossing is what counts.
            if (g_cd_prev_frac[v] - frac > 0.5f || f.progress[v] >= float(g_cd_finish_laps))
                g_cd_crossed |= 1u << v;
            g_cd_prev_frac[v] = frac;
            if (!(g_cd_crossed & (1u << v))) {
                if (log_now && plen < 200)
                    plen += std::snprintf(pend + plen, sizeof pend - plen, " %d:lap%.2f(not finished)", v, f.progress[v]);
                continue;
            }
        }
        int st = g_pit_state[v]; // -1: no pit state seen yet (grid start) = on track
        bool in_pits = st >= 0 && st != rs::kPitOnTrack && st != rs::kPitLeavingPitLane;
        if (in_pits) {
            if (log_now && plen < 200)
                plen += std::snprintf(pend + plen, sizeof pend - plen, " %d:in(%d)", v, st);
            continue;
        }
        // A finished car takes the pit entry only on its next pass after the state is set (seen three times), so
        // in the cool-down the state is set at once; under the red flag the car is sent when it reaches the window.
        if (red_flag && !pitentry::in_window(frac, lo, hi)) {
            if (log_now && plen < 200)
                plen += std::snprintf(pend + plen, sizeof pend - plen, " %d:%.3f", v, frac);
            continue;
        }
        g_cd_sent |= 1u << v;
        swofty::game::Pit_SetPitState(sys, uint32_t(v), rs::kPitEnteringLane);
        // A finished car is not moved down the pit lane while the session is held open (seen 2026-10-24: the pit
        // lane manager only took the sent cars once the session completed and every car got NOT_IN_SESSION), so
        // in the cool-down the car gets that handover here, like the game does at the end.
        if (void *cs = g_ctrl_sys.load(); cs && !red_flag)
            swofty::game::Veh_SetController_Thunk(cs, uint64_t(v), 1, rs::kFlagNotInSession);
        swofty::log("[racestart] %s: vehicle %d (P%d) sent into the pit lane at %.4f of the lap (window %.3f..%.3f)",
                    red_flag ? "red flag" : "cool-down", v, rank_of(v), frac, lo, hi);
    }
    if (log_now && plen)
        swofty::log("[racestart] pit entry (window %.3f..%.3f): still to send (id:lap fraction | in(pit state)):%s", lo, hi,
                    pend);
}

// Parking in the parc ferme, by position on the flow thread (a car driving through the pit lane is moved by its AI,
// not by the pit lane manager, so the placement hook never sees it). The parc ferme of the track is the stock
// CutscenePlacementParcFerme point (parc_ferme_table.hpp: 2..18 m beside the pit lane AI route under the podium on
// most tracks; Melbourne 7.6 m from pit gate 411). A wanted car in the pit lane is pinned where it is when it comes
// abeam of that point, P1 at the point, P2 8 m before it, P3 16 m, the player (when not on the podium) 24 m; "abeam"
// = the point is no longer ahead of the car along its direction of travel and it is within 30 m sideways. On tracks
// whose point is off the pit lane (in_pit_lane false) the slot lies before the pit exit head instead.

int cd_slot(int v)
{
    int r = cd_rank(v);
    return r >= 1 && r <= 3 ? r - 1 : 3;
}

void cd_park_update(double dt)
{
    if (g_cd_end != CdEnd::ParcFerme)
        return;
    bool podium = g_pf && g_pf->in_pit_lane;
    const float *spot = podium ? g_pf->pos : g_exit ? g_exit->head : nullptr;
    if (!spot)
        return;
    Field f = read_field(false);
    if (g_cd_player >= 0 && f.seen[g_cd_player] && f.car[g_cd_player])
        boards::player(carhold::matrix(f.car[g_cd_player]));
    static double s_log_t = 0;
    s_log_t += dt;
    bool log_now = !podium && g_board_ok && s_log_t >= 10.0;
    if (log_now)
        s_log_t = 0;
    for (int i = 0; i < f.n; i++) {
        int v = f.ids[i];
        bool player = v == g_cd_player;
        bool wanted = player ? g_cd_player_ai.load() : g_park_top3 && cd_rank(v) >= 1 && cd_rank(v) <= 3;
        if (!wanted || !f.car[v])
            continue;
        if (log_now && !g_frozen[v] && cd_rank(v) >= 1 && cd_rank(v) <= 3)
            if (const float *lm = carhold::matrix(f.car[v])) {
                const float *b = g_board[cd_rank(v) - 1];
                swofty::log("[racestart] cool-down: vehicle %d (P%d) not parked: %.1f m from its board, crossed %d, "
                            "lap %.3f, still %.1f s", v, cd_rank(v), std::hypot(b[0] - lm[12], b[2] - lm[14]),
                            (g_cd_crossed >> v) & 1, f.progress[v], g_board_still[v]);
            }
        if (g_frozen[v] && !(g_cd_pin & (1u << v))) {
            // Held by the game: the car brakes to a stop where it is (no slide, no teleport). Still rolling faster
            // than 1 m/s after 5 s = the hold does not work for one car: our pin takes over (logged). Cars parked
            // in the pit lane (on_pit_place) get the hold here.
            cd_game_hold(v);
            g_on_board[v] = true;
            // Speed = metres moved from 4 s to 5 s from the car matrix (speed_of reads a wrong value, ~5.5 m/s always).
            float before = g_cd_hold_t[v];
            g_cd_hold_t[v] += float(dt);
            const float *hm = carhold::matrix(f.car[v]);
            if (before < 4.0f && g_cd_hold_t[v] >= 4.0f && hm) {
                g_slide_from[v][0] = hm[12], g_slide_from[v][1] = hm[14];
                g_slide_t[v] = g_cd_hold_t[v]; // start of the measurement
            }
            if (before < 5.0f && g_cd_hold_t[v] >= 5.0f) {
                float sp = -1, span = g_cd_hold_t[v] - g_slide_t[v];
                if (hm && span > 0.1f)
                    sp = std::hypot(hm[12] - g_slide_from[v][0], hm[14] - g_slide_from[v][1]) / span;
                bool holds = sp >= 0 && sp < 1.0f;
                if (!holds) {
                    if (const float *lm = carhold::matrix(f.car[v]))
                        std::memcpy(g_frozen_mtx[v], lm, sizeof g_frozen_mtx[v]);
                    g_cd_pin |= 1u << v;
                }
                swofty::log("[racestart] cool-down: game hold of vehicle %d (P%d) after 5 s: %.2f m/s, %s", v,
                            cd_rank(v), sp, holds ? "stands" : "still rolling, pin fallback");
            }
            continue;
        }
        if (g_frozen[v]) {
            // Slide onto the board in 2.5 s (the pin teleports in steps of about 0.5 m), keeping the car's heading.
            if (g_slide_t[v] >= 0.0f && !g_on_board[v] && !(g_cd_pin & (1u << v))) {
                int k = cd_rank(v) - 1;
                g_slide_t[v] += float(dt);
                float s = std::fmin(1.0f, g_slide_t[v] / 2.5f);
                if (k >= 0 && k < 3) {
                    g_frozen_mtx[v][12] = g_slide_from[v][0] + (g_board[k][0] - g_slide_from[v][0]) * s;
                    g_frozen_mtx[v][14] = g_slide_from[v][1] + (g_board[k][2] - g_slide_from[v][1]) * s;
                }
                if (s >= 1.0f) {
                    g_on_board[v] = true;
                    swofty::log("[racestart] vehicle %d (P%d) on its parc ferme board", v, cd_rank(v));
                }
            }
            carhold::pin(f.car[v], g_frozen_mtx[v], float(dt), "ParcFerme", true);
            continue;
        }
        int st = g_pit_state[v];
        // EnteringPitLane is what we set ourselves anywhere on the lap: only the states inside the pit lane count.
        bool in_lane = st >= rs::kPitEnteringBox && st <= rs::kPitDrivingThrough && st != rs::kPitInGarage;
        const float *m = carhold::matrix(f.car[v]);
        if (!m)
            continue;
        float x = m[12], z = m[14];
        float dx = spot[0] - x, dz = spot[2] - z;
        float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < g_min_pf[v])
            g_min_pf[v] = dist;
        bool have_dir = g_prev_ok[v];
        float fx = x - g_prev_pos[v][0], fz = z - g_prev_pos[v][1];
        g_prev_pos[v][0] = x, g_prev_pos[v][1] = z;
        g_prev_ok[v] = true;
        float moved = std::sqrt(fx * fx + fz * fz);
        // With boards a top-3 car stops abeam of its own board (P1 first, so P2 and P3 pass it beside the lane). On
        // tracks whose parc ferme is on the track (Mexico stadium) the top 3 stay on track and stop there once they
        // have crossed the line.
        int k = cd_rank(v) - 1;
        bool track_board = !podium && g_board_ok && k >= 0 && k < 3 && (player || (g_cd_crossed & (1u << v)));
        // A track-board car held up behind the parked P1 (Mexico boards are 3-5 m apart) stands still near its
        // board: after 2 s within 30 m it is parked where it stands and slides onto the board.
        bool stuck = false;
        if (track_board) {
            float bx = g_board[k][0] - x, bz = g_board[k][2] - z;
            bool by_board = bx * bx + bz * bz < 30.0f * 30.0f;
            g_board_still[v] = by_board && moved < 0.02f ? g_board_still[v] + float(dt) : 0.0f;
            stuck = g_board_still[v] >= 2.0f;
        }
        if (!stuck && ((!in_lane && !track_board) || !have_dir || moved < 0.02f))
            continue;
        if (stuck)
            fx = fz = 0.0f, moved = 1.0f;
        fx /= moved, fz /= moved;
        float along = dx * fx + dz * fz;              // > 0: the spot is still ahead
        float lateral = std::fabs(dx * fz - dz * fx); // sideways distance to the spot's line
        float stop_before = 8.0f * cd_slot(v);
        bool board = (podium || track_board) && g_board_ok && k >= 0 && k < 3;
        float board_side = 0.0f, board_dist = 0.0f;
        if (board) {
            float bx = g_board[k][0] - x, bz = g_board[k][2] - z;
            along = bx * fx + bz * fz;
            board_side = std::fabs(bx * fz - bz * fx);
            board_dist = std::sqrt(bx * bx + bz * bz);
        }
        bool reached = stuck    ? true
                       : board  ? (board_side < 15.0f && board_dist < 30.0f && along <= 0.5f)
                       : podium ? (lateral < 15.0f && along <= stop_before)
                                : (dist < 15.0f + stop_before);
        if (reached) {
            std::memcpy(g_frozen_mtx[v], m, sizeof g_frozen_mtx[v]);
            g_frozen[v] = true;
            // The game holds the car (it stops where it is, abeam of its board); no pin, no slide.
            cd_game_hold(v);
            g_on_board[v] = true;
            if (board)
                swofty::log("[racestart] vehicle %d (P%d) stopping %s its parc ferme board, %.1f m from it",
                            v, k + 1, stuck ? "(held up) near" : "abeam of", stuck ? board_dist : board_side);
            swofty::log("[racestart] vehicle %d (P%d) parked in parc ferme slot %d (%s), %.1f m along / %.1f m beside the %s",
                        v, cd_rank(v), cd_slot(v) + 1, podium ? "under the podium" : board ? "on the track" : "before the pit exit",
                        along, board ? board_side : lateral,
                        podium ? "parc ferme point" : board ? "parc ferme board" : "pit exit head");
            g_frozen_count++;
        }
    }
}

// Every wanted car (AI top 3 for the parc ferme, the autopilot's player) is parked.
bool cd_all_parked()
{
    Field f = read_field(false);
    for (int i = 0; i < f.n; i++) {
        int v = f.ids[i];
        bool player = v == g_cd_player;
        bool wanted = player ? g_cd_player_ai.load() : g_park_top3 && g_cd_end == CdEnd::ParcFerme && cd_rank(v) >= 1 &&
                                                       cd_rank(v) <= 3;
        if (wanted && (!g_frozen[v] || (g_slide_t[v] >= 0.0f && !g_on_board[v])))
            return false;
    }
    return true;
}

// Placement of a car moved down a pit lane spline: pit lane starters stop at the exit queue; in the cool-down the
// AI top 3 (and the autopilot's player) park in parc ferme, 8 m apart.
void on_pit_place(float *mtx)
{
    int v = t_pit_vehicle;
    if (v < 0 || v >= kMaxCars || !mtx)
        return;
    if (g_frozen[v]) {
        std::memcpy(mtx, g_frozen_mtx[v], sizeof g_frozen_mtx[v]);
        return;
    }
    if (g_phase == Phase::Cooldown || g_phase == Phase::Over) {
        if (!g_park_top3 || !g_exit || (v == g_cd_player && !g_cd_player_ai) || g_cd_end != CdEnd::ParcFerme)
            return;
        int rank = cd_rank(v);
        // The parc ferme is the pit lane before the exit: the cars line up from the exit queue head backwards,
        // 10 m apart, the player (any position) behind the top 3.
        float dx = mtx[12] - g_exit->head[0], dz = mtx[14] - g_exit->head[2];
        float dist = std::sqrt(dx * dx + dz * dz);
        if (!(g_logged_pit.fetch_or(1u << v) & (1u << v)))
            logv("[racestart] vehicle %d (P%d) in the pit lane, %.1f m from the pit exit", v, rank, dist);
        if (dist < g_min_pf[v])
            g_min_pf[v] = dist;
        bool wanted = (rank >= 1 && rank <= 3) || v == g_cd_player;
        if (!wanted)
            return;
        if (dist < 15.0f + 10.0f * g_frozen_count) {
            std::memcpy(g_frozen_mtx[v], mtx, sizeof g_frozen_mtx[v]);
            g_frozen[v] = true;
            swofty::log("[racestart] vehicle %d (P%d) parked in parc ferme slot %d, %.1f m before the pit exit", v, rank,
                        ++g_frozen_count, dist);
        }
    }
}

}

// --- settings -------------------------------------------------------------------------------------------------

SWOFTY_ON_CHANGE(warmup, bool on) { g_warmup_on = kWarmupFeature && kStart && on; }
SWOFTY_ON_CHANGE(warmup_minutes, int m) { g_warmup_max = m; }
SWOFTY_ON_CHANGE(warmup_drive_out_s, int s) { g_warmup_drive_out_s = s; }
SWOFTY_ON_CHANGE(warmup_laps, int n) { g_warmup_laps = n; }
SWOFTY_ON_CHANGE(grid_lap, bool on) { g_grid_lap_on = kStart && on; }
SWOFTY_ON_CHANGE(red_flag_lap, bool on) { g_rf_lap_on = kRf && on; }
SWOFTY_ON_CHANGE(warmup_grid_lap, bool on) { g_warm_gridlap_on = kStart && on; }
SWOFTY_ON_CHANGE(pf_marking, bool on) { g_pf_mark_on = on; }
SWOFTY_ON_CHANGE(pit_lane_start, bool on) { g_pls_setting = on; }
SWOFTY_ON_CHANGE(starters, int n) { g_starters = n; }
SWOFTY_ON_CHANGE(player_pit_start, bool on) { g_player_pls = on; }
SWOFTY_ON_CHANGE(starters_who, std::string_view w) { g_who_random = w == "random"; }
SWOFTY_ON_CHANGE(starters_hold_max, int s) { g_hold_max = s; }
SWOFTY_ON_CHANGE(cooldown, bool on) { g_cd_setting = on; }
SWOFTY_ON_CHANGE(cooldown_seconds, int s) { g_cd_seconds = s; }
SWOFTY_ON_CHANGE(cooldown_end, std::string_view s)
{
    g_cd_end = s == "pit_lane" ? CdEnd::PitLane : s == "time" ? CdEnd::Time : CdEnd::ParcFerme;
}
SWOFTY_ON_CHANGE(park_top3, bool on) { g_park_top3 = on; }
SWOFTY_ON_CHANGE(verbose, bool on) { g_verbose = on; }
SWOFTY_ON_CHANGE(hide_garage_crew, bool on) { g_hide_crew = kStart && on; }
SWOFTY_ON_CHANGE(log_field, bool on) { g_field_log = on; }
SWOFTY_ON_CHANGE(hold_probe, bool on) { g_hold_probe = kAp && on; }

// --- hooks ----------------------------------------------------------------------------------------------------
#ifdef SWOFTY_PUBLIC
OWN_WRAP(Rules_OnSessionEnter, uint64_t, void *rules, void *systems, void *components)
#else
SWOFTY_WRAP(Rules_OnSessionEnter, uint64_t, void *rules, void *systems, void *components)
#endif
{
    uint64_t r = orig(rules, systems, components);
    RD_TIMED("rs Rules_OnSessionEnter");
    g_rules = reinterpret_cast<uintptr_t>(rules);
    devvars::publish("rules", g_rules);
    reset_session();
    return r;
}

// The race flow gets the warm-up phase while the game parses it (a dev file wins).
SWOFTY_WRAP(Flow_ParseXml, uint64_t, void *machine, void *ctx, swofty::game::FlowXmlBuf *buf)
{
    RD_TIMED("rs Flow_ParseXml +orig");
    if (!(kStart || kRf) || !buf || !buf->data || buf->size < 64 || buf->size > (8u << 20))
        return orig(machine, ctx, buf);
    std::string_view text(buf->data, size_t(buf->size));
    static std::string s_edited;
    s_edited.clear();
    bool dev = kStart && dev_flow(text, s_edited), warm = false, red = false;
    if (!dev && text.find("<state_machine name=\"race\"") != std::string_view::npos && (g_warmup_on || g_rf_lap_on)) {
        s_edited = text;
        warm = g_warmup_on && edit_race_flow(s_edited);
        red = g_rf_lap_on && edit_red_flag_flow(s_edited);
    }
    if (dev || warm || red) {
        g_flow_edited = s_edited.find("WarmUpGarage") != std::string::npos;
        swofty::log("[racestart] flow %s%s (%llu -> %zu bytes)", dev ? "replaced" : warm ? "with the warm-up phase" : "stock",
                    red ? ", red flag restart over a formation lap" : "", (unsigned long long)buf->size, s_edited.size());
        swofty::game::FlowXmlBuf mine{s_edited.c_str(), s_edited.size()};
        return orig(machine, ctx, &mine);
    }
    if (text.find("<state_machine ") != std::string_view::npos)
        g_flow_edited = false;
    return orig(machine, ctx, buf);
}

// True while the garage start (Sess_Start_VehiclesInGarages, launcher binding) runs on this thread. Before 0.37 this
// was a stack walk for one hard-coded return address (exe+0x2d6959c), which a game update would break.
thread_local bool t_in_garage_start = false;

SWOFTY_WRAP(Sess_Start_VehiclesInGarages, uint64_t, void *state)
{
    RD_TIMED("rs Sess_Start_VehiclesInGarages +orig");
    t_in_garage_start = true;
    uint64_t r = orig(state);
    t_in_garage_start = false;
    return r;
}

bool called_from_garage_start() { return t_in_garage_start; }

// The garage state asks whether this is practice before it starts the cars in their garages. Only the garage start
// hears "practice": every other caller (e.g. the session enter, which then skips the race-only setup and leaves the
// red flag strategy screen without its race data, Engine-RE 2026-10-28) gets the game's own answer.
#ifdef SWOFTY_PUBLIC
OWN_WRAP(Rules_IsPracticeQualifying, bool, void *garage)
#else
SWOFTY_WRAP(Rules_IsPracticeQualifying, bool, void *garage)
#endif
{
    bool r = orig(garage);
    RD_TIMED("rs Rules_IsPracticeQualifying");
    if (!kStart || r || !g_flow_edited || (g_phase != Phase::Idle && g_phase != Phase::Garage))
        return r;
    bool garage_start = called_from_garage_start();
    static int logged = 0;
    if (logged < 12) {
        logged++;
        swofty::log("[racestart] practice question in the race flow (phase %d, in pq %d, garage start %d) -> %s",
                    (int)g_phase.load(), (int)t_in_pq, (int)garage_start,
                    t_in_pq || garage_start ? "practice" : "race (game answer)");
    }
    return t_in_pq || garage_start;
}

// Garage phase: the practice in-game state inside the race flow.
SWOFTY_WRAP(Sess_InGamePQ_Update, uint32_t *, void *state, uint32_t *route, double dt)
{
    RD_TIMED("rs Sess_InGamePQ_Update +orig");
    t_in_pq = true;
    uint32_t *r = orig(state, route, dt);
    t_in_pq = false;
    if (!kStart || !g_flow_edited || !r)
        return r;
    if (g_phase == Phase::Idle) {
        set_phase(Phase::Garage, "race flow with the warm-up phase");
        g_garage_t = 0;
        if (void *sys = g_pit_sys.load()) {
            Field f = read_field(false);
            for (auto &ok : g_slot_ok)
                ok = false;
            g_player_out = g_player_away = false;
            g_still_t = 0;
            for (int i = 0; i < f.n; i++) {
                int id = f.ids[i];
                if (const float *m = carhold::matrix(f.car[id])) {
                    std::memcpy(g_slot_mtx[id], m, sizeof g_slot_mtx[id]);
                    g_slot_ok[id] = true;
                }
                swofty::game::Pit_TeleportToGarage(sys, uint32_t(id));
            }
            swofty::log("[racestart] warm-up: %d cars put into their garages; pit exit closes in %d min", f.n,
                        g_warmup_max.load());
        } else
            swofty::log("[racestart] warm-up: pit lane system not seen yet, the cars stay where they are");
    }
    if (g_phase != Phase::Garage)
        return r;
    enforce_player_control();
    g_garage_t += dt;
    int pl = player_vehicle();
    uint8_t cmd = ::rd::take(::rd::kWarmupCommand);
    // No garage menu in a race: the car drives out on its own after a few seconds (0 = wait for the button).
    int auto_s = g_warmup_drive_out_s.load();
    bool auto_out = auto_s > 0 && g_garage_t > double(auto_s);
    bool timeout = g_garage_t > 60.0 * g_warmup_max.load() || auto_out;
    if (cmd == ::rd::kWarmupLeaveBox || cmd == ::rd::kWarmupToGrid || timeout) {
        // The box view ends here; the warm-up itself runs in the race in-game state (WarmUpRace), where the pit
        // exit is open and the player drives out of the garage by himself.
        bool direct = g_flow_gridlap && !g_flow_freelaps;
        swofty::log(direct ? "[racestart] warm-up: %s, the lap to the grid from the box starts"
                           : "[racestart] warm-up: %s, the warm-up runs in the race state now",
                    timeout ? "time in the box is over" : cmd == ::rd::kWarmupLeaveBox ? "leave the box (control window)"
                                                                                       : "to the grid (control window)");
        // Free laps but "to the grid" from the control window: the race state ends on its first frame.
        g_warm_to_grid = !direct && cmd == ::rd::kWarmupToGrid;
        if (direct) {
            // The warm-up is the lap to the grid: the formation lap state next drives every car from the box to its
            // slot (the player's car leaves the box with the field).
            g_warm_gridlap_pending = true;
            set_phase(Phase::PreRace, "lap to the grid from the box");
        } else {
            g_warm_requested = true;
            // The car drives out on its own when the box time is over too (before, it stayed in the garage).
            g_warm_leave = cmd == ::rd::kWarmupLeaveBox || timeout;
        }
        *r = rs::kRouteFinished;
        return r;
    }
    (void)pl;
    if (*r != *swofty::game::g_Flow_LinkNone) {
        static uint32_t s_last = 0;
        if (*r != s_last)
            swofty::log("[racestart] garage state leaves on its own (route 0x%08x)", s_last = *r);
    }
    return r;
}

// Formation lap states: the warm-up lap (first instance with the warm-up phase) and the stock formation lap.
// Clears the mask bits the game set after the DSQs we blocked (one frame later; see Rules_Disqualify).
void dsq_bits_clear(const char *when)
{
    uintptr_t r = g_dsq_rules;
    uint32_t m8 = 0, mc = 0, b = g_dsq_blocked;
    if (!r || !b || !swofty::mem::read(r + 0x1d08, m8) || !swofty::mem::read(r + 0x1d0c, mc))
        return;
    if ((m8 | mc) & b) {
        *reinterpret_cast<volatile uint32_t *>(r + 0x1d08) = m8 & ~b;
        *reinterpret_cast<volatile uint32_t *>(r + 0x1d0c) = mc & ~b;
    }
    swofty::log("[racestart] %s: DSQ bits of blocked cars %#x cleared (masks %#x / %#x before)", when, b, m8, mc);
    g_dsq_blocked = 0;
}

SWOFTY_WRAP(Sess_FormationLap_Update, uint32_t *, void *state, uint32_t *route, double dt)
{
    RD_TIMED("rs Sess_FormationLap_Update +orig");
    swofty::mem::Trust trust(kHeapTrustMs);
    static void *s_state = nullptr;
    // The flow keeps one formation lap state object: a formation lap while the race phase runs (red flag restart)
    // is a new one too.
    bool first = s_state != state || g_phase == Phase::Race;
    s_state = state;
    Phase phase = g_phase;
    if (first) {
        if (kRf && g_race_started && !g_warm_gridlap_pending) {
            // A formation lap after the race has started: the red flag restart (edit_red_flag_flow). It is the lap to
            // the grid like after the warm-up: the start hold is released for it (it held every car - lights on, never
            // out: black screen, the AI back in their garages, self-test 2026-10-26 15:22), every car drives from the
            // box to its slot, the player out of his box too (user 2026-10-26 14:05).
            g_rf_lap = true;
            g_warm_gridlap_pending = true;
            int plr = player_vehicle();
            if (void *sys = g_pit_sys.load(); sys && plr >= 0) {
                swofty::game::Pit_TeleportToGarage(sys, uint32_t(plr));
                g_pit_state[plr] = rs::kPitInGarage;
            }
            swofty::log("[racestart] red flag restart: lap to the grid from the box (start hold released for it)");
        }
        if (g_warm_gridlap_pending) {
            // Lap to the grid after the free warm-up: the AI cars leave the pit lane in grid order, the player drives
            // from where he is (already near the slot counts as arrived).
            g_warm_gridlap_pending = false;
            set_phase(Phase::WarmUpLap, "lap to the grid");
            g_warm_t = g_still_t = 0;
            int pl0 = player_vehicle();
            Field f0 = read_field(false);
            const float *m0 = pl0 >= 0 && f0.seen[pl0] ? carhold::matrix(f0.car[pl0]) : nullptr;
            bool on_track = pl0 >= 0 && g_pit_state[pl0].load() == rs::kPitOnTrack;
            g_player_out = on_track;
            g_player_away = on_track && g_warm_end_parked && m0 && g_slot_ok[pl0] &&
                            std::hypot(m0[12] - g_slot_mtx[pl0][12], m0[14] - g_slot_mtx[pl0][14]) < 60.f;
            find_tables();
            grid_queue_start(true);
            // Still in the garage or the box: out of the pit lane to the grid with the field.
            if (pl0 >= 0 && !on_track) {
                int st0 = g_pit_state[pl0].load();
                if (st0 == rs::kPitInGarage || st0 == rs::kPitInBox || st0 < 0)
                    leave_box(pl0);
            }
        } else if (g_warm_requested && (phase == Phase::Garage || phase == Phase::Idle)) {
            set_phase(Phase::WarmUpLap, "formation lap state after the garage");
            g_warm_requested = false;
            g_warm_t = g_still_t = 0;
            g_player_out = g_player_away = false;
            find_tables();
            grid_queue_start(true);
        } else if (g_race_started) {
            // A formation lap after the race has started: the red flag restart (edit_red_flag_flow). The field
            // drives out of the pit lane onto the grid; no pit lane starters.
            set_phase(Phase::Formation, "formation lap after the red flag");
            if (kRf) {
                find_tables();
                grid_queue_start(false);
            }
        } else {
            set_phase(Phase::Formation, "formation lap state");
            find_tables();
            if (g_grid_lap_on && !g_grid_done)
                grid_queue_start(false);
            pick_starters("formation lap");
        }
        phase = g_phase;
    }
    int pl = player_vehicle();
    int st = pl >= 0 ? g_pit_state[pl].load() : -1;
    if (phase == Phase::WarmUpLap) {
        antistall_release(true);
        enforce_player_control();
        drive_out_hint(pl);
        g_warm_t += dt;
        if (uint8_t cmd = ::rd::take(::rd::kWarmupCommand); cmd == ::rd::kWarmupLeaveBox)
            leave_box(pl);
        if (st == rs::kPitOnTrack && !g_player_out) {
            g_player_out = true;
            swofty::log("[racestart] warm-up lap: vehicle %d out of the box", pl);
        }

    }
    grid_queue_update(dt);
    if constexpr (kPls)
        starters_update(dt, false); // the starters turn into the pit lane at the end of the formation lap
    t_in_formation = true;
    uint32_t *r = orig(state, route, dt);
    t_in_formation = false;
    if (!r)
        return r;
    if (phase == Phase::WarmUpLap) {
        // Ends when the player has been out and stands still for 8 s (parked on the grid), or when the pit exit time
        // runs out; the state's own end (every car frozen while the player is still in the box) is held back.
        // Standing still counts only once the car has been away from the pit exit (not the exit hold, not a box
        // stop): more than 400 m from the exit head on this run.
        Field f = read_field(false);
        if (const float *m = pl >= 0 && f.seen[pl] ? carhold::matrix(f.car[pl]) : nullptr) {
            float moved = std::hypot(m[12] - g_last_pos[0], m[14] - g_last_pos[1]);
            g_last_pos[0] = m[12], g_last_pos[1] = m[14];
            // Standing at the pit exit under the game's pit lane AI (allowed_flags hands the car over after 3 s):
            // counted here too - only the free warm-up counted it, the car stood a whole minute at the closed exit
            // in the lap to the grid after the red flag (self-test 2026-10-26 15:42).
            g_pit_still_t = st == rs::kPitLeavingPitLane && moved < 0.05f ? g_pit_still_t + dt : 0;
            double lane0 = g_pit_lane_t;
            g_pit_lane_t = st == rs::kPitLeavingPitLane ? g_pit_lane_t + dt : 0;
            if ((lane0 < 20.0 && g_pit_lane_t >= 20.0) || (g_pit_still_t >= 3.0 && g_pit_still_t - dt < 3.0))
                swofty::log("[racestart] lap to the grid: vehicle %d %s in the pit lane, the pit lane AI hands it over", pl,
                            g_pit_still_t >= 3.0 ? "stands still" : "is 20 s");
            float to_slot = pl >= 0 && g_slot_ok[pl] ? std::hypot(m[12] - g_slot_mtx[pl][12], m[14] - g_slot_mtx[pl][14]) : 1e9f;
            if (g_player_out && to_slot > 400.f && st == rs::kPitOnTrack && !g_player_away) {
                g_player_away = true;
                swofty::log("[racestart] warm-up lap: vehicle %d is out on the lap", pl);
            }
            g_still_t = moved < 0.05f && g_player_away && to_slot < 60.f ? g_still_t + dt : 0;
        }
        bool parked = g_player_out && g_player_away && st == rs::kPitOnTrack && g_still_t > 8.0 && g_warm_t > 30.0;
        // Every AI car on its slot too (within 12 m), or a lap's time for them: a slow lap (25 m/s) plus 90 s.
        int ai = 0, ai_on = 0;
        for (int i = 0; i < f.n; i++) {
            int v = f.ids[i];
            const float *mv = human(v) || !g_slot_ok[v] ? nullptr : carhold::matrix(f.car[v]);
            if (!mv)
                continue;
            ai++;
            ai_on += std::hypot(mv[12] - g_slot_mtx[v][12], mv[14] - g_slot_mtx[v][14]) < 12.f;
        }
        float lap_s = *swofty::game::g_Track_Length / 25.0f + 90.0f;
        if (parked && ai_on < ai && g_warm_t < lap_s) {
            static double s_wait_log = 0;
            if (g_warm_t < 1.0)
                s_wait_log = 0;
            if (g_warm_t >= s_wait_log) {
                s_wait_log = g_warm_t + 10.0;
                swofty::log("[racestart] lap to the grid: player parked, %d of %d AI cars on their slot", ai_on, ai);
            }
            parked = false;
        }
        bool timeout = g_warm_t > 60.0 * g_warmup_max.load();
        if (!parked && !timeout) {
            if (*r != *swofty::game::g_Flow_LinkNone && warm_terminal(*r)) {
                *r = *swofty::game::g_Flow_LinkNone;
                return r;
            }
            if (*r != *swofty::game::g_Flow_LinkNone && warm_route_passes(*r)) {
                swofty::log("[racestart] warm-up lap: route 0x%08x let through", *r);
                set_phase(Phase::PreRace, "warm-up left (quit, flashback, retire)");
                grid_queue_clear();
                return r;
            }
            if (*r != *swofty::game::g_Flow_LinkNone) {
                static int s_held = 0;
                if (s_held++ % 600 == 0)
                    logv("[racestart] warm-up lap: route 0x%08x held back (player %s)", *r,
                         g_player_out ? "not parked yet" : "still in the box");
                *r = *swofty::game::g_Flow_LinkNone;
            }
            return r;
        }
        *r = rs::kRouteFinished;
        g_grid_done = true; // the real formation lap is the game's own
        if (g_rf_lap) {
            g_rf_lap = false; // the red flag restart: start lights next (flow), no Restart
            swofty::log("[racestart] red flag restart: lap to the grid over, start lights next");
        } else {
            g_gridlap_restart = true; // the game's Restart follows (flow): the field stays on the grid after it
            dsq_bits_clear("lap to the grid over");
            swofty::log("[racestart] lap to the grid over: the game's restart resets the race, then grid and formation lap");
            // "Formation lap done" lives in FormationLapLogic (+0x41/+0x42, timer +0x44), reached from this state at
            // +0xde8; only its constructor clears it, the Restart's Reset leaves it (Engine-RE 2026-10-29, static).
            // Kept set, the game skipped the real formation lap and the race never finished (Melbourne 0.33.7: timing
            // froze at the leader's line crossing, no finish handover).
            uintptr_t fll = 0;
            uintptr_t at = reinterpret_cast<uintptr_t>(state) + 0xde8;
            if (swofty::mem::readable(at, 8))
                std::memcpy(&fll, reinterpret_cast<const void *>(at), 8);
            if (fll && swofty::mem::readable(fll + 0x40, 8)) {
                auto *b = reinterpret_cast<uint8_t *>(fll);
                swofty::log("[racestart] formation lap logic %p: done flags %u/%u, timer bytes %02x%02x%02x%02x -> cleared",
                            reinterpret_cast<void *>(fll), b[0x41], b[0x42], b[0x44], b[0x45], b[0x46], b[0x47]);
                b[0x41] = b[0x42] = 0;
                g_fll = fll;
                std::memset(b + 0x44, 0, 4);
            } else {
                swofty::log("[racestart] formation lap logic not readable (%p), done flag kept", reinterpret_cast<void *>(fll));
            }
        }
        grid_queue_clear();
        snap_to_slots(parked ? "lap to the grid over" : "lap to the grid: time over");
        set_phase(Phase::PreRace, parked ? "player parked on the grid" : "warm-up time over");
    } else if (*r != *swofty::game::g_Flow_LinkNone) {
        static uint32_t s_last = 0;
        if (*r != s_last)
            logv("[racestart] formation lap route 0x%08x", s_last = *r);
    }
    return r;
}

// Every in-game frame (also under the formation lap, which calls it itself).
// Test aid (log_field, off by default): every 5 s speed, pit status, controller flags, lap progress and position of
// cars 0, 5, 10, 15 and the player, read from the game itself (the swofty telemetry is experimental and stays empty).

// Hold probe (test aid part only, option hold_probe; user 2026-10-31): can the game itself hold ONE car mid-race,
// instead of our per-frame pin (red flag queue, cool-down parking)? Two ways from Engine-RE (Sess_FreezeAllVehicles
// 0x142c76220, Sess_Controllers_ReleaseForRaceStart 0x142c764b0), tried one after the other on the last AI car:
//   A  race 40-60 s: setController(car, AI, SESSION_NOT_STARTED), 60 s: the reason taken back
//   B  race 75-95 s: participant +0x17c |= 0x80 on entry <car> of [state+0xd40]+0x70 (list order = car id is a
//      guess: every car's movement is logged, so the log shows which car stopped), 95 s: bit cleared
// Every 2 s: metres moved by the target and by the field (median), from the car matrices.
double g_hp_t = 0, g_hp_log_t = 0;
int g_hp_car = -1, g_hp_step = 0;
float g_hp_last[kMaxCars][2];
uintptr_t g_hp_entry = 0;

template <class T> bool hp_read(uintptr_t addr, T &out)
{
    if (!addr || !swofty::mem::readable(addr, sizeof(T)))
        return false;
    out = *reinterpret_cast<const T *>(addr);
    return true;
}

void run_hold_probe(double dt, void *state)
{
    if (!g_hold_probe || g_phase != Phase::Race)
        return;
    g_hp_t += dt;
    void *ctrl = g_ctrl_sys.load();
    Field f = read_field(true);
    if (g_hp_car < 0 && g_hp_t > 35.0 && f.n > 2) {
        float worst = 1e9f;
        for (int i = 0; i < f.n; i++) // the car furthest back on the road
            if (!human(f.ids[i]) && f.progress[f.ids[i]] < worst)
                worst = f.progress[f.ids[i]], g_hp_car = f.ids[i];
        swofty::log("[holdprobe] target vehicle %d (progress %.3f), %d cars", g_hp_car, worst, f.n);
    }
    int v = g_hp_car;
    if (v < 0 || !ctrl)
        return;
    auto step = [&](int s, double at, const char *what) {
        if (g_hp_step != s - 1 || g_hp_t < at)
            return false;
        g_hp_step = s;
        swofty::log("[holdprobe] %.0f s: %s (vehicle %d)", g_hp_t, what, v);
        return true;
    };
    if (step(1, 40.0, "A on: setController AI SESSION_NOT_STARTED"))
        swofty::game::Veh_SetController_Thunk(ctrl, uint64_t(v), 1, rs::kFlagSessionNotStarted);
    if (step(2, 60.0, "A off: SESSION_NOT_STARTED taken back"))
        swofty::game::Veh_SetController_Thunk(ctrl, uint64_t(v), 0, rs::kFlagSessionNotStarted);
    if (step(3, 75.0, "B on: participant +0x17c |= 0x80")) {
        uintptr_t list = 0, it = 0, end = 0, st = reinterpret_cast<uintptr_t>(state);
        int n = 0;
        if (hp_read(st + 0xd40, list) && list && hp_read(list + 0x70, it) && hp_read(list + 0x78, end) &&
            end > it && end - it <= 8 * 64) {
            n = int((end - it) / 8);
            for (int i = 0; i < n; i++) {
                uintptr_t e = 0;
                uint8_t b = 0;
                if (hp_read(it + 8u * unsigned(i), e) && e)
                    hp_read(e + 0x17c, b);
                swofty::log("[holdprobe] B list %d/%d: entry %p, +0x17c %#x", i, n, reinterpret_cast<void *>(e), b);
                if (i == v && e && swofty::mem::readable(e + 0x17c, 1))
                    g_hp_entry = e;
            }
        }
        if (g_hp_entry)
            *reinterpret_cast<volatile uint8_t *>(g_hp_entry + 0x17c) |= 0x80;
        else
            swofty::log("[holdprobe] B: no list entry %d (list of %d), skipped", v, n);
    }
    if (step(4, 95.0, "B off: +0x17c bit cleared") && g_hp_entry)
        *reinterpret_cast<volatile uint8_t *>(g_hp_entry + 0x17c) &= uint8_t(~0x80);
    if (step(5, 110.0, "probe done"))
        g_hold_probe = false;
    if ((g_hp_log_t += dt) < 2.0)
        return;
    g_hp_log_t = 0;
    float moved[kMaxCars], tgt = -1;
    int m = 0;
    std::string slow;
    for (int i = 0; i < f.n; i++) {
        int id = f.ids[i];
        const float *x = f.car[id] ? carhold::matrix(f.car[id]) : nullptr;
        if (!x)
            continue;
        float d = std::hypot(x[12] - g_hp_last[id][0], x[14] - g_hp_last[id][1]);
        g_hp_last[id][0] = x[12], g_hp_last[id][1] = x[14];
        if (id == v)
            tgt = d;
        else
            moved[m++] = d;
        if (d < 5.0f)
            slow += " " + std::to_string(id);
    }
    std::sort(moved, moved + m);
    swofty::log("[holdprobe] %.0f s step %d: target %d moved %.1f m / 2 s, field median %.1f m, pit state %d, "
                "cars under 5 m:%s", g_hp_t, g_hp_step, v, tgt, m ? moved[m / 2] : -1.f, g_pit_state[v].load(),
                slow.empty() ? " none" : slow.c_str());
}

#include "pose_probe.hpp" // test aid part: [pose] line on ctl post 25 1

double g_field_t = 0;
float g_field_pos[kMaxCars][2];
bool g_field_pos_ok[kMaxCars];

// Speed = average over the last interval from the car matrix (speed_of read a wrong value: always 20 km/h, test
// 2026-10-30/31). -1 on the first line of a car.
void field_log(double dt)
{
    if (!g_field_log)
        return;
    g_field_t += dt;
    if (g_field_t < 5.0)
        return;
    double span = g_field_t;
    g_field_t = 0;
    Field f = read_field(false);
    int pl = player_vehicle();
    for (int i = 0; i < f.n; i++) {
        int v = f.ids[i];
        if (v % 5 != 0 && v != pl)
            continue;
        const float *m = f.car[v] ? carhold::matrix(f.car[v]) : nullptr;
        float speed = -1;
        if (m) {
            if (g_field_pos_ok[v])
                speed = float(std::hypot(m[12] - g_field_pos[v][0], m[14] - g_field_pos[v][1]) / span);
            g_field_pos[v][0] = m[12], g_field_pos[v][1] = m[14];
            g_field_pos_ok[v] = true;
        }
        swofty::log("[racestart] field car %d%s: %.0f km/h, pit state %d, controller flags %#x, progress %.3f, "
                    "phase %s, x %.1f y %.1f z %.1f",
                    v, v == pl ? " (player)" : "", speed >= 0 ? speed * 3.6f : -1.0f, g_pit_state[v].load(),
                    ctrl_flags(v), f.progress[v], phase_name(g_phase), m ? m[12] : 0.f, m ? m[13] : 0.f,
                    m ? m[14] : 0.f);
    }
}

SWOFTY_WRAP(Sess_InGame_UpdateBase, uint32_t *, void *state, uint32_t *route, double dt)
{
    uint32_t *r = orig(state, route, dt);
    RD_TIMED("rs Sess_InGame_UpdateBase");
    swofty::mem::Trust trust(kHeapTrustMs);
#ifdef RS_PUBLIC
    pit_poll();
#endif
    uintptr_t session = 0;
    if (rd(reinterpret_cast<uintptr_t>(state) + rs::kStateSession, session) && session)
        g_session = session;
    if (int pl = player_vehicle(); pl != g_pub_player) {
        g_pub_player = pl;
        devvars::publish("player_vehicle", uintptr_t(pl + 1)); // 0 = none, else id + 1 (other modules: wet-track)
    }
    learn_track_points(dt);
    if constexpr (kAp) {
        autopilot_update();
        run_hold_probe(dt, state);
        pose_update(dt);
    }
    if constexpr (kStart) {
        drive_out_check(dt);
        warm_free_player();
        field_log(dt);
    }
    if (t_in_formation || t_in_pq || !r)
        return r;
    Phase phase = g_phase;
    // "Drive out of the pit lane" outside the warm-up: whenever the player's car is in its garage.
    if (kStart && phase != Phase::Garage && phase != Phase::WarmUpRace && phase != Phase::WarmUpLap) {
        if (uint8_t cmd = ::rd::take(::rd::kWarmupCommand); cmd == ::rd::kWarmupLeaveBox) {
            int pl = player_vehicle();
            int st = pl >= 0 ? g_pit_state[pl].load() : -1;
            if (st == rs::kPitInGarage || st == 0 /* InactiveInGarage */)
                leave_box(pl);
            else
                swofty::log("[racestart] drive out: vehicle %d is not in its garage (pit state %d)", pl, st);
        }
    }
    if (phase == Phase::Garage && g_warm_requested) {
        // The race in-game state after the box: the warm-up. AI cars stay in their garages, the player drives.
        set_phase(Phase::WarmUpRace, "race state after the box");
        g_warm_t = g_still_t = 0;
        g_player_out = g_player_away = false;
        g_warm_leave_t = 0;
        Field f = read_field(false);
        for (int i = 0; i < f.n; i++)
            g_warm_laps0[f.ids[i]] = uint32_t(std::floor(f.progress[f.ids[i]]));
        g_warm_lapcount = 0;
        find_tables();
        phase = Phase::WarmUpRace;
    }
    if (phase == Phase::WarmUpRace) {
        antistall_release(true);
        enforce_player_control();
        drive_out_hint(player_vehicle());
        g_warm_t += dt;
        g_warm_leave_t += dt;
        int pl = player_vehicle();
        if (g_warm_leave && g_warm_leave_t > 1.0) {
            g_warm_leave = false;
            leave_box(pl);
        }
        if (uint8_t cmd = ::rd::take(::rd::kWarmupCommand); cmd == ::rd::kWarmupLeaveBox)
            leave_box(pl);
        // AI cars that want to leave their garage are kept in it.
        if (void *sys = g_pit_sys.load())
            for (int v = 0; v < kMaxCars; v++)
                if (!human(v) && g_pit_state[v] == rs::kPitWantsToLeave)
                    swofty::game::Pit_SetPitState(sys, uint32_t(v), rs::kPitInGarage);
        int st = pl >= 0 ? g_pit_state[pl].load() : -1;
        if (st == rs::kPitOnTrack && !g_player_out) {
            g_player_out = true;
            swofty::log("[racestart] warm-up: vehicle %d out on the track", pl);
        }
        Field f = read_field(false);
        bool parked = false;
        uint32_t laps = 0;
        if (const float *m = pl >= 0 && f.seen[pl] ? carhold::matrix(f.car[pl]) : nullptr) {
            float moved = std::hypot(m[12] - g_last_pos[0], m[14] - g_last_pos[1]);
            g_last_pos[0] = m[12], g_last_pos[1] = m[14];
            g_pit_still_t = st == rs::kPitLeavingPitLane && moved < 0.05f ? g_pit_still_t + dt : 0;
            float to_slot = g_slot_ok[pl] ? std::hypot(m[12] - g_slot_mtx[pl][12], m[14] - g_slot_mtx[pl][14]) : 1e9f;
            if (g_player_out && to_slot > 400.f && st == rs::kPitOnTrack && !g_player_away) {
                g_player_away = true;
                swofty::log("[racestart] warm-up: vehicle %d is out on the lap", pl);
            }
            // Parked on the slot after a lap (the lap count below resets g_player_away when the car comes back).
            bool been_round = g_player_away || g_warm_lapcount > 0;
            g_still_t = moved < 0.05f && been_round && to_slot < 60.f ? g_still_t + dt : 0;
            parked = g_player_out && been_round && st == rs::kPitOnTrack && g_still_t > 8.0 && g_warm_t > 30.0;
            if (g_player_away && st == rs::kPitOnTrack && to_slot < 40.f && moved > 0.05f) {
                g_player_away = false; // the next lap starts: away again after 400 m
                g_warm_lapcount++;
                swofty::log("[racestart] warm-up: lap %u of %d done (past the grid slot, not in the box)",
                            g_warm_lapcount, std::max(1, g_warmup_laps.load()));
            }
            uint32_t now = uint32_t(std::floor(f.progress[pl]));
            laps = std::max(g_warm_lapcount, now > g_warm_laps0[pl] ? now - g_warm_laps0[pl] : 0u);
        }
        bool timeout = g_warm_t > 300.0 * std::max(1, g_warmup_laps.load()); // 5 min per lap at most
        bool lap_limit = laps >= uint32_t(std::max(1, g_warmup_laps.load())) || g_warm_to_grid;
        if (parked || timeout || lap_limit) {
            g_warm_requested = false;
            g_warm_to_grid = false;
            g_warm_end_parked = parked; // the lap to the grid: parked = arrived, else one more lap to the slot
            if (g_flow_gridlap) {
                swofty::log("[racestart] warm-up over (%s): lap to the grid next, every car from the pit lane",
                            parked ? "player parked on the grid" : timeout ? "time over" : "lap limit");
                g_warm_gridlap_pending = true;
                set_phase(Phase::PreRace, "lap to the grid next");
            } else {
                swofty::log("[racestart] warm-up over (%s): the game's restart puts the field on the grid",
                            parked ? "player parked on the grid" : timeout ? "time over" : "lap limit");
                g_grid_done = true;
                set_phase(Phase::PreRace, "restart after the warm-up");
            }
            *r = rs::kRouteRestart;
            return r;
        }
        if (*r != *swofty::game::g_Flow_LinkNone && warm_terminal(*r)) {
            *r = *swofty::game::g_Flow_LinkNone;
            return r;
        }
        if (*r != *swofty::game::g_Flow_LinkNone && warm_route_passes(*r)) {
            swofty::log("[racestart] warm-up: route 0x%08x let through", *r);
            g_warm_requested = false;
            set_phase(Phase::PreRace, "warm-up left (quit, flashback, retire)");
            return r;
        }
        if (*r != *swofty::game::g_Flow_LinkNone) {
            static int s_held = 0;
            if (s_held++ % 600 == 0)
                logv("[racestart] warm-up: route 0x%08x held back", *r);
            *r = *swofty::game::g_Flow_LinkNone;
        }
        return r;
    }
    if (kRf && ::rd::red_flag_on())
        g_rf_seen = true;
    // The SC lap after a red flag: an own in-game state (edit_red_flag_flow), not the race's.
    if (g_race_started && g_race_state && state != g_race_state && g_rf_seen && phase != Phase::RfLap &&
        phase != Phase::Cooldown) {
        rf_lap_begin();
        phase = Phase::RfLap;
    }
    if (phase == Phase::RfLap) {
        if (state == g_race_state) {
            dsq_bits_clear("SC lap over");
            set_phase(Phase::PreRace, "SC lap state left");
        } else {
            if (rf_lap_update(dt))
                *r = rs::route("rf_grid");
            else if (*r != *swofty::game::g_Flow_LinkNone && *r != rs::route("quit") && *r != rs::route("suspension") &&
                     *r != rs::route("replay"))
                *r = *swofty::game::g_Flow_LinkNone; // the lap is not over: no finished / spectate from it
            return r;
        }
    }
    // Very short races have no formation lap: the race starts straight from the grid.
    if (phase == Phase::Formation || phase == Phase::Idle || phase == Phase::PreRace) {
        g_race_state = state;
        set_phase(Phase::Race, phase == Phase::Formation ? "in-game state after the formation lap"
                                                         : "in-game state without a formation lap");
        g_race_started = true;
        g_race_time = 0;
        dsq_bits_clear("race start");
        if (uintptr_t fll = g_fll; fll && swofty::mem::readable(fll + 0x40, 8)) {
            auto *b = reinterpret_cast<const uint8_t *>(fll);
            swofty::log("[racestart] race start: formation lap done flags %u/%u", b[0x41], b[0x42]);
            g_fll = 0;
        }
        g_cd_sent = 0;
        find_tables();
        for (int v = 0; v < kMaxCars; v++) // a starter the game put back on the grid is let go
            if ((g_chosen & (1u << v)) && g_pit_state[v] == rs::kPitOnTrack && !g_frozen[v])
                g_released |= 1u << v;
        if (g_start_n == 0) {
            // The cars stand on the grid now: their places in grid order are the slots of a red flag drive to the grid.
            Field f = read_field(true);
            int order[kMaxCars], n = 0;
            for (int i = 0; i < f.n; i++)
                order[n++] = i;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++)
                    if (f.pos[order[j]] > f.pos[order[i]])
                        std::swap(order[i], order[j]);
            for (int i = 0; i < n; i++)
                if (const float *m = carhold::matrix(f.car[f.ids[order[i]]]))
                    std::memcpy(g_start_slot[g_start_n++], m, sizeof g_start_slot[0]);
            swofty::log("[racestart] %d grid slots kept for a red flag", g_start_n);
        }
        if constexpr (kPls)
            if (pls_on() && !g_chosen)
                swofty::log("[racestart] race start without a formation lap: no pit lane starters (they need the "
                            "formation lap)");
        phase = Phase::Race;
    }
    if (phase == Phase::Race) {
        starters_update(dt, true);
    }
    if constexpr (kRf) {
        static bool s_rf = false;
        uintptr_t rf = 0;
        bool hold = devvars::lookup("rf_hold", rf) && rf == 1;
        if (hold && !s_rf) {
            g_cd_sent = 0;
            g_send_log_t = 10.0;
            swofty::log("[racestart] red flag hold: sending the field into the pit lane");
        }
        s_rf = hold;
        if (hold && !g_cd_holding)
            send_into_the_pits(true, dt);
        // Every car is in the pit lane (+10 s): the Red Flag addon asks for the drive to the grid (rf_hold 3).
        bool to_grid = rf == 3;
        if (to_grid && !g_rf_grid && phase == Phase::Race) {
            g_rf_grid = true;
            rf_grid_begin();
        }
        if (g_rf_grid && (!to_grid || rf_grid_update(dt)))
            g_rf_grid = false;
    }
    if (g_cd_holding) {
        g_cd_held += dt;
        CdEnd mode = g_cd_end;
        if (g_cd_held >= std::fmax(float(g_cd_seconds.load()), *swofty::game::g_Track_Length / 28.0f + 90.0f))
            end_cooldown("time");
        else if (mode == CdEnd::PitLane && g_cd_in_pit_lane)
            end_cooldown("pit lane");
        else if (mode == CdEnd::ParcFerme && g_cd_player_ai && g_cd_player >= 0 && g_frozen[g_cd_player] &&
                 cd_all_parked())
            end_cooldown("everyone parked in parc ferme");
        else if (mode == CdEnd::ParcFerme && !g_cd_player_ai && cd_parked(dt))
            end_cooldown("parked in parc ferme");
        else if (*r == rs::kRouteSpectate || *r == rs::kRouteFinished)
            *r = *swofty::game::g_Flow_LinkNone;
        if (g_cd_holding) {
            pf_marking_update(dt);
            send_into_the_pits(false, dt);
            cd_park_update(dt);
            if (uint32_t through = g_cd_through.exchange(0))
                for (int v = 0; v < kMaxCars; v++)
                    if ((through & (1u << v)) && g_pit_sys.load()) {
                        swofty::game::Pit_SetPitState(g_pit_sys.load(), uint32_t(v), rs::kPitDrivingThrough);
                        swofty::log("[racestart] cool-down: vehicle %d drives through, no pit stop", v);
                    }
        }
    } else if (g_cd_done && (*r == rs::kRouteSpectate || *r == rs::kRouteFinished)) {
        g_cd_done = false;
        logv("[racestart] in-game state left (route %s)", *r == rs::kRouteSpectate ? "spectate" : "finished");
    }
    return r;
}

#if RS_PART == 1 // call-site redirects: the cool-down part only (one redirect per call site)
// Player finished: the game hands the car to the AI here every frame; the cool-down keeps it instead.
SWOFTY_REDIRECT(call_Sess_InGame_PlayerFinishedAi, void, void *system, uint64_t vehicle, uint32_t ai, int32_t flag)
{
    RD_TIMED("rs call_Sess_InGame_PlayerFinishedAi +orig");
    int v = int(vehicle & 0xff);
    bool robo = v < kMaxCars && (g_cd_robo & (1u << v));
    if (!g_cd_holding && !g_cd_done && !(v < kMaxCars && (g_cd_out & (1u << v))) && cd_on() && g_cd_seconds > 0)
        start_cooldown(system, v, robo);
    if (g_cd_holding && !g_cd_player_ai)
        return;
    orig(system, vehicle, ai, flag);
}

// Session complete: every car goes to the AI; the player's stays while the cool-down runs.
SWOFTY_REDIRECT(call_Sess_InGame_SessionCompleteAi, void, void *system, uint64_t vehicle, uint32_t ai, int32_t flag)
{
    RD_TIMED("rs call_Sess_InGame_SessionCompleteAi +orig");
    if (g_cd_holding && int(vehicle & 0xff) == g_cd_player)
        return;
    orig(system, vehicle, ai, flag);
}
#endif

// The rules disqualify a car that is out of position on the formation lap for too long: in the garage phase and the
// warm-up lap the player drives where he wants, so the player is never disqualified there.
SWOFTY_WRAP(Rules_Disqualify, void, void *rules, uint32_t vehicle)
{
    RD_TIMED("rs Rules_Disqualify +orig");
    int v = int(vehicle & 0xff);
    Phase phase = g_phase;
    // The red flag SC lap: nobody is disqualified (cars out of the pit lane, off their grid slots).
    // A pit lane starter waiting at the pit exit is off its grid slot: not disqualified either.
    bool starter = v < 32 && (g_chosen & (1u << v)) && !(g_released & (1u << v));
    if ((human(v) && (phase == Phase::Garage || phase == Phase::WarmUpLap || phase == Phase::WarmUpRace)) ||
        phase == Phase::RfLap || starter) {
        static int s_n = 0;
        if (s_n++ % 300 == 0)
            swofty::log("[racestart] %s: disqualification of vehicle %d blocked (%d so far)", phase_name(phase), v, s_n);
        // The DSQ itself (0x143572f90) sets the car's bit in the rules' masks +0x1d08/+0x1d0c AFTER this returns;
        // left set, the finish request becomes "disqualified" (status 5): Melbourne 0.33.7 froze at the finish after
        // a blocked warm-up DSQ (Engine-RE 2026-10-29, static). Remembered here, cleared by dsq_bits_clear().
        if (v < 32 && rules) {
            g_dsq_rules = reinterpret_cast<uintptr_t>(rules);
            g_dsq_blocked |= 1u << v;
        }
        return;
    }
    orig(rules, vehicle);
}

// The one rule for who takes a car. Records human drivers and safety cars; in the garage and the warm-up lap the
// race rules do not get the player's car (they would reset it to the grid): only the pit lane AI (it drives the car
// out of the pit exit), our autopilot and, with the autopilot on, the reform-grid AI. In the cool-down the player
// keeps the car through the pit lane and after the flag.
SWOFTY_WRAP(Veh_SetController_Thunk, void, void *system, uint64_t vehicle, uint32_t ai, int32_t flag)
{
    RD_TIMED("rs Veh_SetController_Thunk +orig");
    if (g_ctrl_sys.exchange(system) != system)
        devvars::publish("ctrl_sys", reinterpret_cast<uintptr_t>(system));
    int v = int(vehicle & 0xff);
    if (v >= kMaxCars) {
        orig(system, vehicle, ai, flag);
        return;
    }
    if (!ai && flag != rs::kFlagRoboDriver)
        g_human |= 1u << v;
    // The safety car vehicle gets SAFETY_CAR when the session is set up. A red flag deploy puts EVERY car on
    // setController(AI, SAFETY_CAR) too (Sess_ScRedFlagDeploy_Update): taking those as safety cars emptied the
    // field reader after a red flag (2026-10-24: the autopilot player was never sent into the pit lane).
    if (Phase ph = g_phase; ai && flag == rs::kFlagSafetyCar && ph != Phase::Race && ph != Phase::Cooldown &&
                            ph != Phase::Over && !g_race_started)
        g_sc |= 1u << v;
    Phase phase = g_phase;
    bool player = human(v);
    if (player && ai && (phase == Phase::Garage || phase == Phase::WarmUpLap || phase == Phase::WarmUpRace || phase == Phase::RfLap) && flag >= 0 && flag < 32 &&
        !(allowed_flags(phase, g_pit_state[v].load()) & (1u << flag))) {
        static uint32_t s_logged = 0;
        if (!(s_logged & (1u << (flag & 31)))) {
            s_logged |= 1u << (flag & 31);
            logv("[racestart] %s: AI takeover of vehicle %d (%s) blocked", phase_name(phase), v, rs::flag_name(flag));
        }
        return;
    }
    // Autopilot finish: the game sends NOT_IN_SESSION instead of the finish handover above.
    if (ai && flag == rs::kFlagNotInSession && player && v == g_auto_car) {
        g_auto_done = true; // finished: the game's cool-down AI has the car, ROBODRIVER comes off
        g_auto_car = -1;
        if (void *cs = g_ctrl_sys.load())
            swofty::game::Veh_SetController_Thunk(cs, vehicle, 0, rs::kFlagRoboDriver);
    }
    // Cool-down: a car the autopilot drove keeps going with the game's cool-down AI; a human driver keeps his car
    // (this handover came instead of the redirected finish call: before, the player always went to the AI here).
    if (kCd && flag == rs::kFlagRoboDriver && ai)
        g_cd_robo |= 1u << v;
    // A player out of the race (DNF, retired, disqualified) gets no cool-down lap (#46: after a DNF the cool-down
    // still asked to drive to parc ferme): the game hands that car to the AI with one of these flags first.
    if (kCd && ai && player && phase == Phase::Race &&
        (flag == rs::kFlagTerminalDamage || flag == rs::kFlagRetired || flag == rs::kFlagDisqualified) &&
        !(g_cd_out & (1u << v))) {
        g_cd_out |= 1u << v;
        swofty::log("[racestart] cool-down: vehicle %d is out of the race (%s), no cool-down lap", v,
                    rs::flag_name(flag));
    }
    if (ai && flag == rs::kFlagNotInSession && player && phase == Phase::Race && !g_cd_holding && !g_cd_done &&
        !(g_cd_out & (1u << v)) && cd_on() && g_cd_seconds > 0)
        start_cooldown(system, v, (g_cd_robo & (1u << v)) != 0);
    bool cd_player = g_cd_holding && v == g_cd_player;
    if (cd_player && flag == rs::kFlagPitLane)
        g_cd_in_pit_lane = ai != 0;
    bool skip = cd_player && ai &&
                ((flag == rs::kFlagPitLane && g_cd_end == CdEnd::ParcFerme && !g_cd_player_ai) ||
                 (flag == rs::kFlagNotInSession && !g_cd_player_ai));
    if (skip) {
        logv("[racestart] cool-down: handover of vehicle %d to the AI (%s) skipped", v, rs::flag_name(flag));
        return;
    }
    orig(system, vehicle, ai, flag);
}

// Pit states of every car; the player's box stops in the warm-up are logged (allowed: one more lap to the grid).
namespace {
void on_pit_state(uint8_t v, int32_t s)
{
    if (v < kMaxCars) {
        int was = g_pit_state[v].exchange(s);
        if (was == s)
            return;
        if (was == rs::kPitLeavingPitLane && s == rs::kPitOnTrack)
            g_exit_pending |= 1u << v; // learn_track_points: the end of the pit lane
        // The podium cars drive through the pit lane without a stop: at the box the state is switched on the flow
        // thread (base update) to InPitLane_DrivingThrough.
        if (g_cd_holding && (g_cd_sent & (1u << v)) && s == rs::kPitEnteringBox)
            g_cd_through |= 1u << v;
        if (kPls && (g_pls_sent & (1u << v)) && !(g_released & (1u << v)) && s == rs::kPitEnteringBox)
            g_pls_through |= 1u << v; // pit lane starter: no stop at the box
        if (was == rs::kPitOnTrack && s == rs::kPitEnteringLane && !g_cd_holding && !(g_cd_sent & (1u << v)) &&
            !(g_pls_sent & (1u << v))) {
            float frac;
            if (uintptr_t rec = record_of(v); rec && rd(rec + rs::kRecLapFrac, frac))
                pitentry::learn(frac, g_pf ? g_pf->progress : -1.0f); // the game's own pit entries show the entry
        }
        if (was != s && human(v)) {
            logv("[racestart] vehicle %u pit state %s -> %s", v, rs::pit_state_name(was), rs::pit_state_name(s));
            // Warm-up: no pit stop, the car drives through its box (user 2026-10-31 14:15, several warm-up laps);
            // switched on the flow thread (warm_free_player), as the cool-down does.
            Phase ph = g_phase;
            if (kStart && s == rs::kPitEnteringBox && (ph == Phase::WarmUpLap || ph == Phase::WarmUpRace))
                g_warm_through |= 1u << v;
        }
    }
}

// Pit state of every car straight from the pit lane system (Engine-RE 2026-10-29, static): record
// [pitSys+0x68+idx*0x10], state u32 at +4, idx < 27. Catches changes that do not go through setVehicleStatus; each
// one is logged, so a test shows whether the post-wrap alone would have been enough.
[[maybe_unused]] void pit_poll()
{
    auto sys = reinterpret_cast<uintptr_t>(g_pit_sys.load());
    if (!sys)
        return;
    static int unseen_logged = 0;
    for (int v = 0; v < 27 && v < kMaxCars; v++) {
        uintptr_t rec = 0;
        uint32_t s = 0;
        if (!rd(sys + 0x68 + v * 0x10, rec) || !rec || !rd(rec + 4, s) || s > 0x40)
            continue;
        int was = g_pit_state[v].load();
        if (was == int(s))
            continue;
        if (was > 0 && unseen_logged < 40) {
            unseen_logged++;
            swofty::log("[racestart] pit state of vehicle %d %d -> %u seen only in the pit lane system", v, was, s);
        }
        on_pit_state(uint8_t(v), int32_t(s));
    }
}
}

#ifndef RS_PUBLIC
// The AI's MessagePitStateChanged handler sees every pit state change.
SWOFTY_WRAP(Ai_OnPitStateChanged_Thunk, uint64_t, void *ai, void *msg)
{
    RD_TIMED("rs Ai_OnPitStateChanged_Thunk +orig");
    uint8_t v = 0xff;
    int32_t s = -1;
    auto m = reinterpret_cast<uintptr_t>(msg);
    if (rd(m + rs::kMsgVehicle, v) && rd(m + rs::kMsgPitState, s))
        on_pit_state(v, s);
    return orig(ai, msg);
}
#endif

// The public build has no AI pit state handler binding (L1 in the Swofty KB): it takes the pit states from the pit
// lane system's setVehicleStatus instead (hypothesis: every change goes through it; RS_PUBLIC builds log each one).
SWOFTY_WRAP(Pit_SetPitState, void, void *pit_lane_system, uint32_t vehicle, uint32_t state)
{
    RD_TIMED("rs Pit_SetPitState +orig");
    if (g_pit_sys.exchange(pit_lane_system) != pit_lane_system)
        devvars::publish("pit_sys", reinterpret_cast<uintptr_t>(pit_lane_system));
    orig(pit_lane_system, vehicle, state);
#ifdef RS_PUBLIC
    on_pit_state(uint8_t(vehicle), int32_t(state));
#endif
}

// Grid / track resets of the player's car in the warm-up ("Reset Vehicle", grid reform) are dropped once it has left
// its garage (pit lane or track): the car drives the lap itself, no reset onto the start / finish straight (user
// 2026-10-25 19:44).
SWOFTY_WRAP(Veh_Teleport, void, void *car, const char *name, float *prev_mtx, float *new_mtx, float dt, uint32_t flags)
{
    RD_TIMED("rs Veh_Teleport +orig");
    swofty::mem::Trust trust(kHeapTrustMs);
    Phase phase = g_phase;
    if ((phase == Phase::WarmUpLap || phase == Phase::WarmUpRace) && !autopilot()) {
        int pl = player_vehicle();
        Field f = read_field(false);
        int st = pl >= 0 ? g_pit_state[pl].load() : -1;
        bool out = st == rs::kPitOnTrack || st > rs::kPitWantsToLeave;
        // Our own pins ("GridSlot" at the end of the lap to the grid) pass: the player's car was the one car left
        // off its slot (test 2026-10-29 02:50).
        bool own = name && std::strcmp(name, "GridSlot") == 0;
        if (pl >= 0 && f.seen[pl] && f.car[pl] == reinterpret_cast<uintptr_t>(car) && out && !own &&
            name && (std::strstr(name, "Reset") || std::strstr(name, "Grid") || std::strstr(name, "grid"))) {
            static int s_dropped = 0;
            if (s_dropped++ % 600 == 0)
                logv("[racestart] warm-up: '%s' teleport of vehicle %d dropped (%d so far)", name, pl, s_dropped);
            return;
        }
    }
    orig(car, name, prev_mtx, new_mtx, dt, flags);
}

// Pit lane movement: which car is being moved, and a parked car's spline distance does not advance.
SWOFTY_WRAP(Pit_MoveDownSpline, void, void *mgr, uint8_t vehicle, void *car, uint64_t route, float speed, char a,
            char b, char c, char d)
{
    RD_TIMED("rs Pit_MoveDownSpline +orig");
    uintptr_t pv = 0;
    float before = 0;
    bool have = rd(reinterpret_cast<uintptr_t>(mgr) + rs::kPitVehicles + size_t(vehicle) * 0x10, pv) && pv &&
                rd(pv + rs::kPitDistance, before);
    t_pit_vehicle = vehicle;
    orig(mgr, vehicle, car, route, speed, a, b, c, d);
    t_pit_vehicle = -1;
    if (have && vehicle < kMaxCars && g_frozen[vehicle])
        *reinterpret_cast<float *>(pv + rs::kPitDistance) = before;
}

// Garage crews (user 2026-10-31 14:15: "the pit crew at the start must go"): while the race starts in the garages
// (warm-up, lap to the grid, formation lap from the pit lane) every crew instance that plays an animation is hidden
// once with the game's GarageCrewInstance::hide(inst, 0, 1) (Engine-RE 2026-10-31, static). The garage logic (drive
// out, pit states) is not touched; the instances go with the session.
void *g_crew_hidden[64];
int g_crew_hidden_n = 0;
void *g_crew_session = nullptr;

SWOFTY_WRAP(Game_GarageCrewInstance_PlaySequence, void, void *inst, uint64_t name, int32_t idx, float start, uint32_t flags)
{
    RD_TIMED("rs Game_GarageCrewInstance_PlaySequence +orig");
    orig(inst, name, idx, start, flags);
    Phase ph = g_phase;
    bool garages = ph == Phase::Garage || ph == Phase::WarmUpLap || ph == Phase::WarmUpRace ||
                   (ph == Phase::Formation && g_grid_n > 0);
    if (!kStart || !g_hide_crew || !garages || !inst)
        return;
    void *session = reinterpret_cast<void *>(g_rules.load());
    if (g_crew_session != session)
        g_crew_session = session, g_crew_hidden_n = 0;
    for (int i = 0; i < g_crew_hidden_n; i++)
        if (g_crew_hidden[i] == inst)
            return;
    auto a = reinterpret_cast<uintptr_t>(inst);
    if (g_crew_hidden_n >= 64 || !swofty::mem::readable(a + 0x132, 2) || !swofty::mem::readable(a + 0x210, 8))
        return;
    g_crew_hidden[g_crew_hidden_n++] = inst;
    swofty::game::Game_GarageCrewInstance_Hide(inst, 0, 1);
    logv("[racestart] garage crew %p hidden (%d, phase %s)", inst, g_crew_hidden_n, phase_name(ph));
}

SWOFTY_WRAP(Pit_PlaceCar, uint64_t, void *car, const char *name, int32_t mode, void *p4, float *mtx, float speed,
            uint32_t flags)
{
    RD_TIMED("rs Pit_PlaceCar +orig");
    on_pit_place(mtx);
    return orig(car, name, mode, p4, mtx, speed, flags);
}

#ifdef SWOFTY_PUBLIC
OWN_WRAP(Pit_PlaceCarBlend, uint64_t, void *car, const char *name, void *p3, float *mtx, float speed, uint32_t flags)
#else
SWOFTY_WRAP(Pit_PlaceCarBlend, uint64_t, void *car, const char *name, void *p3, float *mtx, float speed, uint32_t flags)
#endif
{
    RD_TIMED("rs Pit_PlaceCarBlend +orig");
    on_pit_place(mtx);
    return orig(car, name, p3, mtx, speed, flags);
}

// The core's lifecycle events (game module session enter / exit): a new session starts from a clean state even when
// the rule manager's session enter (Rules_OnSessionEnter) does not run, as after a Time Trial into a race.
// Autopilot: while the dashboard switch is on, the game's AI drives the player's car (setController reason
// ROBODRIVER 14). Applied again in every new session, taken back when the switch goes off; after the finish
// (NOT_IN_SESSION) the game's own cool-down AI has the car. (Before the split it lived in Track Limits, which is off
// now: the switch did nothing - 2026-10-26.)
namespace {
void autopilot_update()
{
    void *sys = g_ctrl_sys.load();
    int v = player_vehicle();
    bool want = autopilot() && !g_auto_done;
    if (!sys)
        return;
    if (g_auto_car >= 0 && (!want || g_auto_car != v)) {
        swofty::game::Veh_SetController_Thunk(sys, uint64_t(g_auto_car), 0, rs::kFlagRoboDriver);
        swofty::log("[racestart] autopilot off for vehicle %d", g_auto_car);
        g_auto_car = -1;
    }
    // The game took the car back (its restart after the lap to the grid hands every car to SESSION_NOT_STARTED and
    // back to the human): the car stood on the grid and was disqualified (self-test 2026-10-26 15:02). Applied again.
    if (want && v >= 0 && g_auto_car == v) {
        static uint64_t s_human_since = 0;
        ::rd::Race *race = ::rd::race();
        bool taken = race && race->player == v && !race->ai[v];
        uint64_t now = GetTickCount64();
        if (!taken)
            s_human_since = 0;
        else if (!s_human_since)
            s_human_since = now;
        else if (now - s_human_since > 1500) {
            s_human_since = 0;
            g_auto_car = -1;
            swofty::log("[racestart] autopilot: the game handed vehicle %d back to the human, applied again", v);
        }
    }
    if (want && v >= 0 && g_auto_car < 0) {
        swofty::game::Veh_SetController_Thunk(sys, uint64_t(v), 1, rs::kFlagRoboDriver);
        g_auto_car = v;
        swofty::log("[racestart] autopilot on: the AI drives vehicle %d", v);
    }
}
}

namespace {
// --- red flag SC lap ---------------------------------------------------------------------------------------------
// After the strategy screen every car drives out of the pit lane in race order for one lap (the AI at safety car
// pace, see the Red Flag addon: rs_rf_lap), then they are put onto their grid slots (the stock red flag teleport had
// put them there in race order: those positions are the slots) and the stock start lights follow.
void rf_lap_begin()
{
    set_phase(Phase::RfLap, "red flag restart: SC lap out of the pit lane");
    Field f = read_field(false);
    int n = 0;
    for (int i = 0; i < f.n; i++) {
        int v = f.ids[i];
        const float *m = f.car[v] ? carhold::matrix(f.car[v]) : nullptr;
        if (m && g_pit_state[v].load() == rs::kPitOnTrack) {
            std::memcpy(g_slot_mtx[v], m, sizeof g_slot_mtx[v]);
            g_slot_ok[v] = true;
            n++;
        }
    }
    swofty::log("[racestart] red flag SC lap: %d grid slots taken from the restart positions", n);
    g_rf_lap_t = 0;
    g_rf_lead = -1;
    g_rf_lead_away = false;
    g_player_out = false;
    find_tables();
    grid_queue_start(false);
    g_rf_lead = g_grid_n > 0 ? g_grid_order[0] : -1;
    devvars::publish("rs_rf_lap", 1);
}

// true when the lap is over (the first car is back at its slot, or a slow lap's time): grid slots, start lights next.
bool rf_lap_update(double dt)
{
    g_rf_lap_t += dt;
    antistall_release(true);
    grid_queue_update(dt);
    bool over = g_rf_lap_t > *swofty::game::g_Track_Length / 25.0f + 120.0f;
    if (g_rf_lead >= 0 && g_slot_ok[g_rf_lead]) {
        Field f = read_field(false);
        if (const float *m = f.car[g_rf_lead] ? carhold::matrix(f.car[g_rf_lead]) : nullptr) {
            float d = std::hypot(m[12] - g_slot_mtx[g_rf_lead][12], m[14] - g_slot_mtx[g_rf_lead][14]);
            if (d > 400.f && !g_rf_lead_away) {
                g_rf_lead_away = true;
                swofty::log("[racestart] red flag SC lap: vehicle %d leads the field round", g_rf_lead);
            }
            if (g_rf_lead_away && d < 60.f)
                over = true;
        }
    }
    if (!over)
        return false;
    grid_queue_clear();
    devvars::publish("rs_rf_lap", 0);
    snap_to_slots(g_rf_lead_away ? "red flag SC lap over" : "red flag SC lap: time over");
    set_phase(Phase::PreRace, "standing start after the red flag");
    swofty::log("[racestart] red flag SC lap over after %.0f s: standing start on the grid", g_rf_lap_t);
    return true;
}

// --- red flag: drive to the grid ----------------------------------------------------------------------------------
// User 2026-10-26 18:28/18:32: under the red flag every car drives into the pit lane; 10 s after the last one is in,
// they drive out in race order onto the grid (slot k = the k-th start slot), then the game's own red flag follows
// (freeze, strategy screen, its restart on the grid). Ends when the first car is back at its slot or after a slow
// lap's time; the Red Flag addon waits for rs_rf_grid = 2.
void rf_grid_begin()
{
    Field f = read_field(false);
    int order[kMaxCars], n = 0;
    for (int i = 0; i < f.n; i++)
        order[n++] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (f.pos[order[j]] > f.pos[order[i]])
                std::swap(order[i], order[j]);
    int slots = 0;
    for (auto &ok : g_slot_ok)
        ok = false;
    for (int i = 0; i < n && i < g_start_n; i++) {
        int v = f.ids[order[i]];
        std::memcpy(g_slot_mtx[v], g_start_slot[i], sizeof g_slot_mtx[v]);
        g_slot_ok[v] = true;
        slots++;
    }
    grid_queue_clear();
    grid_queue_start(false, true);
    g_rf_lap_t = 0;
    g_rf_lead = g_grid_n > 0 ? g_grid_order[0] : -1;
    g_rf_lead_away = false;
    devvars::publish("rs_rf_grid", 1);
    swofty::log("[racestart] red flag: every car in the pit lane, drive to the grid in race order (%d slots, %d cars, "
                "first vehicle %d)", slots, n, g_rf_lead);
}

bool rf_grid_update(double dt)
{
    g_rf_lap_t += dt;
    grid_queue_update(dt);
    bool over = g_rf_lap_t > *swofty::game::g_Track_Length / 25.0f + 120.0f;
    if (g_rf_lead >= 0 && g_slot_ok[g_rf_lead]) {
        Field f = read_field(false);
        if (const float *m = f.car[g_rf_lead] ? carhold::matrix(f.car[g_rf_lead]) : nullptr) {
            float d = std::hypot(m[12] - g_slot_mtx[g_rf_lead][12], m[14] - g_slot_mtx[g_rf_lead][14]);
            if (d > 400.f && !g_rf_lead_away) {
                g_rf_lead_away = true;
                swofty::log("[racestart] red flag: vehicle %d leads the field round to the grid", g_rf_lead);
            }
            if (g_rf_lead_away && d < 60.f)
                over = true;
        }
    }
    if (!over)
        return false;
    grid_queue_clear();
    snap_to_slots(g_rf_lead_away ? "red flag: the field is at the grid" : "red flag: drive to the grid, time over");
    devvars::publish("rs_rf_grid", 2);
    swofty::log("[racestart] red flag: on the grid after %.0f s, the game's red flag follows", g_rf_lap_t);
    return true;
}

} // namespace

void on_session_event(const SwoftyEvent *ev, void *)
{
    swofty::log("[racestart] %s: state reset", ev && ev->name ? ev->name : "session event");
    bool after_gridlap = g_gridlap_restart && ev && ev->name && std::strcmp(ev->name, SWOFTY_EVENT_SESSION_START) == 0;
    reset_session();
    g_gridlap_restart = false;
    if (after_gridlap) {
        g_grid_done = true; // the restart after our lap to the grid: no second drive from the pit lane
        swofty::log("[racestart] restart after the lap to the grid: the stock formation lap starts from the grid");
    }
    g_auto_car = -1; // a new session: the controller is new, apply the autopilot again
    g_auto_done = false;
}

SWOFTY_ON_LOAD
{
    if constexpr (kCd)
        boards::install();
#ifdef SWOFTY_PUBLIC
    own::install_all("racestart");
#endif
    static bool s_subscribed = false;
    if (!s_subscribed) {
        s_subscribed = true;
        swofty::events::subscribe(SWOFTY_EVENT_SESSION_START, on_session_event);
        swofty::events::subscribe(SWOFTY_EVENT_SESSION_END, on_session_event);
    }
    g_warmup_on = kWarmupFeature && kStart && warmup;
    g_warmup_max = warmup_minutes;
    g_warmup_drive_out_s = warmup_drive_out_s;
    g_warmup_laps = warmup_laps;
    g_grid_lap_on = kStart && grid_lap;
    g_rf_lap_on = kRf && red_flag_lap;
    g_warm_gridlap_on = kStart && warmup_grid_lap;
    g_pf_mark_on = pf_marking;
    g_pls_setting = pit_lane_start;
    g_starters = starters;
    g_player_pls = player_pit_start;
    g_who_random = std::string_view(starters_who) == "random";
    g_hold_max = starters_hold_max;
    g_cd_setting = cooldown;
    g_cd_seconds = cooldown_seconds;
    std::string_view e = cooldown_end;
    g_cd_end = e == "pit_lane" ? CdEnd::PitLane : e == "time" ? CdEnd::Time : CdEnd::ParcFerme;
    g_park_top3 = park_top3;
    g_verbose = verbose;
    g_hide_crew = kStart && hide_garage_crew;
    g_field_log = log_field;
    g_hold_probe = kAp && hold_probe;
    for (auto &s : g_pit_state)
        s = -1;
    swofty::log("[racestart] part: %s", kPartName);
    swofty::log("[racestart] loaded: warm-up %d (%d min), grid lap %d, pit lane starters %d (%d), cool-down %d (%d s)",
                int(g_warmup_on.load()), g_warmup_max.load(), int(g_grid_lap_on.load()), int(g_pls_setting.load()),
                g_starters.load(), int(g_cd_setting.load()), g_cd_seconds.load());
}

SWOFTY_ADDON()
