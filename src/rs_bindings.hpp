#pragma once
// Local bindings of the Race Start module (race-start/bindings/race_start.bindings.json, merged into the launcher's
// loader by test-package/enable_test_bindings.ps1). Steam build d873a1c8 only.
#include <swofty/mixin.hpp>

namespace swofty::game {

// Flow states. An update returns the route the flow takes next: a djb2 hash of a link name of the flow XML, or
// g_Flow_LinkNone (0) to stay.
using Sess_StateUpdate_t = uint32_t *(void *state, uint32_t *route, double dt);
using Sess_FormationLap_Update_t = Sess_StateUpdate_t; // SWOFTY_WRAP wants a <name>_t per target
using Sess_InGamePQ_Update_t = Sess_StateUpdate_t;
using Sess_InGame_UpdateBase_t = Sess_StateUpdate_t;
inline constexpr fn<"Sess_FormationLap_Update", Sess_StateUpdate_t> Sess_FormationLap_Update{}; // vtable 0x1457def60 slot 9
inline constexpr fn<"Sess_InGamePQ_Update", Sess_StateUpdate_t> Sess_InGamePQ_Update{};         // practice garage state
inline constexpr fn<"Sess_InGame_UpdateBase", Sess_StateUpdate_t> Sess_InGame_UpdateBase{};     // 0x142d566a0, every in-game frame
inline constexpr data<"g_Game_FlowLinkStay", uint32_t> g_Flow_LinkNone{};

// Session flow files (game/flow_race.xml, ...) are read from the packed archive and handed to this parser
// (0x144421c60): (state machine, load context, {data, size} *). The caller frees the buffer afterwards.
struct FlowXmlBuf {
    const char *data;
    uint64_t size;
};
using Flow_ParseXml_t = uint64_t(void *machine, void *ctx, FlowXmlBuf *buf);
inline constexpr fn<"Sess_Flow_ParseXml", Flow_ParseXml_t> Flow_ParseXml{};

// Is the session's ruleset practice/qualifying (0x142a40710)? The garage state asks before it starts the cars in
// their garages.
using Rules_IsPracticeQualifying_t = bool(void *garage);
inline constexpr fn<"Sess_Rules_IsPracticeQualifying", Rules_IsPracticeQualifying_t> Rules_IsPracticeQualifying{};

// onStateEnter of a garage session state (0x142d692e0, L2, launcher binding V1865): starts the cars in their garages
// ([state+0xea8]+0x1c8 = 1); it asks Rules_IsPracticeQualifying on the way.
using Sess_Start_VehiclesInGarages_t = uint64_t(void *state);
inline constexpr fn<"Sess_Start_VehiclesInGarages", Sess_Start_VehiclesInGarages_t> Sess_Start_VehiclesInGarages{};

// RuleManager: disqualifies a car (0x143571440; called when the disqualification timer runs out, e.g. a car out of
// position on the formation lap): marks the record, setController(car, AI, DISQUALIFIED).
using Rules_Disqualify_t = void(void *rules, uint32_t vehicle);
inline constexpr fn<"Sess_Rules_Disqualify", Rules_Disqualify_t> Rules_Disqualify{};

// RuleManager::onSessionEnter (0x1435676a0): rules+0x1c30 vehicle record manager.
using Rules_OnSessionEnter_t = uint64_t(void *rules, void *systems, void *components);
inline constexpr fn<"Sess_RuleManager_Initialise", Rules_OnSessionEnter_t> Rules_OnSessionEnter{};

// VehicleControllerSystem::setController(system, vehicle id, 1 = AI / 0 = human, flag).
using Veh_SetController_Thunk_t = void(void *system, uint64_t vehicle, uint32_t ai, int32_t flag);
inline constexpr fn<"Game_Set_VehicleController_Thunk", Veh_SetController_Thunk_t> Veh_SetController_Thunk{};

// In the InGame base update: setController(player, AI, NOT_IN_SESSION) once the player finished (0x142d56d21) and
// setController(every car, AI, NOT_IN_SESSION) once the session is complete (0x142d56785).
inline constexpr call<"site_Sess_InGamePlayerFinishedAiCall"> call_Sess_InGame_PlayerFinishedAi{};
inline constexpr call<"site_Sess_InGameSessionCompleteAiCall"> call_Sess_InGame_SessionCompleteAi{};

// PitLaneSystem (vtable 0x145843d00): set a car's pit state (0x1434bc540) and put a car into its garage
// (0x1434d17f0: state InGarage, then placed there; the garage hold teleports it back every frame).
using Pit_SetPitState_t = void(void *pit_lane_system, uint32_t vehicle, uint32_t state);
// VehicleAntiStallSystem singleton: +0x100 = race started (0 holds every car's input like on the grid).
inline constexpr data<"g_Game_RaceStartSystem", void *> g_Veh_AntiStallSystem{}; // L1 local
// PitLaneSystem::vehicleWantsToLeaveGarage(pit, vehicle, instant) (0x1434d1350): what the practice garage button
// does (via MessageRequestVehicleExitGarage -> GarageSystem): crew 'send car to track' + pit state WantsToLeave.
using Pit_VehicleWantsToLeaveGarage_t = void(void *pit_lane_system, uint8_t vehicle, uint8_t instant);
inline constexpr fn<"local:Pit_VehicleWantsToLeaveGarage", Pit_VehicleWantsToLeaveGarage_t> Pit_VehicleWantsToLeaveGarage{}; // L1 local
inline constexpr fn<"Game_Pitlane_SetVehicleStatus", Pit_SetPitState_t> Pit_SetPitState{};
using Pit_TeleportToGarage_t = void(void *pit_lane_system, uint32_t vehicle);
inline constexpr fn<"Game_Pitlane_Multiplayer_teleportToGarage", Pit_TeleportToGarage_t> Pit_TeleportToGarage{};
// PitLaneSystem: Game_PitLane_PutVehicleInGarage (0x1434d1260), what the red flag does offline for every car: the car
// is put at its team's pit box, pit state 4 InPitBox, its pit plan dropped (pitlane-start 0.37.2).
using Pit_PutVehicleInBox_t = void(void *pit_lane_system, uint8_t vehicle);
inline constexpr fn<"Game_PitLane_PutVehicleInPitBox", Pit_PutVehicleInBox_t> Pit_PutVehicleInBox{}; // L1 local

// PitLaneSystem::vehicleEnteringPitLaneInternal (0x1434cfc30): at the pit entry, drive through or stop at the box.
using Pit_VehicleEnteringPitLaneInternal_t = void(void *pit_lane_system, uint32_t vehicle);
inline constexpr fn<"Game_PitLane_VehicleEnteringInternal", Pit_VehicleEnteringPitLaneInternal_t>
    Pit_VehicleEnteringPitLaneInternal{};

// AI handler for MessagePitStateChanged: msg+0x10 vehicle id, msg+0x18 pit state.
using Ai_OnPitStateChanged_Thunk_t = uint64_t(void *ai, void *msg);
inline constexpr fn<"local:Ai_OnPitStateChanged_Thunk", Ai_OnPitStateChanged_Thunk_t> Ai_OnPitStateChanged_Thunk{};

// PitLaneMovementManager::moveDownSpline (0x1434be940) and the two placement functions it calls (WRAPs chain with
// the red flag module's).
using Pit_MoveDownSpline_t = void(void *mgr, uint8_t vehicle, void *car, uint64_t route, float speed, char a, char b,
                                  char c, char d);
inline constexpr fn<"Game_PitLane_MoveDownSpline", Pit_MoveDownSpline_t> Pit_MoveDownSpline{};
using Pit_PlaceCar_t = uint64_t(void *car, const char *name, int32_t mode, void *p4, float *mtx, float speed,
                                uint32_t flags);
inline constexpr fn<"Game_Place_Vehicle", Pit_PlaceCar_t> Pit_PlaceCar{};
using Pit_PlaceCarBlend_t = uint64_t(void *car, const char *name, void *p3, float *mtx, float speed, uint32_t flags);
inline constexpr fn<"local:Pit_PlaceCarBlend", Pit_PlaceCarBlend_t> Pit_PlaceCarBlend{};

// Track length in m (track identification for the data tables).
inline constexpr data<"g_Game_TrackLength", float> g_Track_Length{};
// Cutscene placement registry that CutSceneScript::setLocation (0x140241330) searches by name (track_points.hpp).
inline constexpr data<"local:g_Cutscene_Placements", void *> g_Cutscene_Placements{};
// Scene library ([g_Scene_Library]+0x68): find a loaded scene by name (djb2 of the name -> string id, then the
// library's scene list +0xd8..+0xe0, scene+0xe8 = id; 0 when no loaded package has it) and spawn it into the world
// (SceneManager::spawnScene 0x1401a4b70, the way the race marshals are built in 0x1403d2ed0: flags block 0x20 bytes,
// transform = 4x4 row major, two override lists with their counts at +0xa0 and +0x50).
inline constexpr data<"g_Game_Root", void *> g_Scene_Library{};
using Scene_FindByName_t = void *(void *library, const char *name, bool warn);
inline constexpr fn<"local:Scene_FindByName", Scene_FindByName_t> Scene_FindByName{};
using Scene_Spawn_t = void *(void *library, const uint8_t *flags, void *scene, const float *transform, void *overrides_a,
                             void *overrides_b, uint64_t p7);
inline constexpr fn<"Game_SceneManagerSpawn_SpawnScene", Scene_Spawn_t> Scene_Spawn{};
// Vehicle race-record manager (+0xa0 record *cache[id], +0x88..+0x90 record list) and the per-car state root
// ([[root+0x38]+0xa8] + 0x38 + k*0x508 -> state, state+0x2c = speed m/s).
inline constexpr data<"g_Game_VehicleRecordManager", void *> g_Veh_RecordManager{};
inline constexpr data<"g_Game_VehicleStateRoot", void *> g_Veh_StateRoot{};

// Garage crew (Engine-RE 2026-10-31, static): GarageCrewInstance::playSequence (0x140382500, launcher binding) runs
// for every crew member's animation, so it shows each instance; GarageCrewInstance::hide (0x140355360, L2, own scan)
// (inst, extras, keep_render_flag): +0x132 |= 2, hides the scene +0x210 and the parts; a second call is a no-op.
// extras = 0 (crew only), keep_render_flag = 1 = the smallest change.
using Game_GarageCrewInstance_PlaySequence_t = void(void *inst, uint64_t name, int32_t idx, float start, uint32_t flags);
inline constexpr fn<"Game_GarageCrewInstance_PlaySequence", Game_GarageCrewInstance_PlaySequence_t>
    Game_GarageCrewInstance_PlaySequence{};
using Game_GarageCrewInstance_Hide_t = void(void *inst, uint8_t extras, uint8_t keep_render_flag);
inline constexpr fn<"local:Game_GarageCrewInstance_Hide", Game_GarageCrewInstance_Hide_t> Game_GarageCrewInstance_Hide{}; // L2 local

}

namespace rs {

constexpr int kMaxCars = 32;

// Vehicle race record.
constexpr size_t kRulesRecords = 0x1c30; // rules -> record manager: +0x88..+0x90 record list
constexpr size_t kRecLaps = 0x58;        // u32 laps
constexpr size_t kRecLapFrac = 0x74;     // float lap fraction
constexpr size_t kRecStateLink = 0x118;  // -> object, +0x38 = index into the per-car state table
constexpr size_t kMgrCache = 0xa0;
constexpr size_t kMgrListBegin = 0x88;
constexpr size_t kPitVehicles = 0x938;   // moveDownSpline manager: per vehicle 0x10 -> pitting vehicle
constexpr size_t kPitDistance = 0xd8;    // pitting vehicle: float distance along the pit lane spline
constexpr size_t kStateSession = 0xcb0;  // in-game state: game session
constexpr size_t kSessionOrder = 0x108;  // game session: entries in race order (begin/end), entry byte 0 = id
constexpr size_t kMsgVehicle = 0x10;
constexpr size_t kMsgPitState = 0x18;

// Controller flags (ControllerSet table 0x1457d7b10).
enum Flag : int {
    kFlagPitLane = 0, kFlagDisqualified = 2, kFlagNotInSession = 3, kFlagRetired = 4, kFlagTerminalDamage = 9,
    kFlagSessionNotStarted = 10, kFlagSafetyCar = 11, kFlagReformGrid = 13,
    kFlagRoboDriver = 14
};

// Pit states (PitLaneSystem).
enum Pit : int {
    kPitOnTrack = 1, kPitEnteringLane = 2, kPitEnteringBox = 3, kPitInBox = 4, kPitInGarage = 9,
    kPitWantsToLeave = 10, kPitLeavingPitLane = 14, kPitDrivingThrough = 15
};

inline const char *flag_name(int flag)
{
    static const char *names[] = {"PIT_LANE", "ROLLING_START", "DISQUALIFIED", "NOT_IN_SESSION", "RETIRED", "PAUSE",
                                  "DEV", "BENCHMARK", "CUTSCENE", "TERMINAL_DAMAGE", "SESSION_NOT_STARTED",
                                  "SAFETY_CAR", "DROP_IN", "FORMATION_LAP_REFORM_GRID", "ROBODRIVER", "RESET_TO_TRACK",
                                  "CINEMATIC_SAFETY_CAR", "PDS"};
    return flag >= 0 && flag < int(sizeof names / sizeof *names) ? names[flag] : "?";
}

inline const char *pit_state_name(int s)
{
    static const char *names[] = {"InactiveInGarage", "OnTrack", "EnteringPitLane", "EnteringPitBox", "InPitBox",
                                  "InPitBox_CrewCompleted", "InPitBoxPerformedRelease", "LeavingPitBox",
                                  "BeingPushedIntoGarage", "InGarage", "InGarage_VehicleWantsToLeave",
                                  "InGarage_PreparedToLeave", "InGarage_SafeToLeave", "LeavingGarage",
                                  "LeavingPitLane", "InPitLane_DrivingThrough", "Invalid"};
    return s >= 0 && s <= 16 ? names[s] : "?";
}

// Flow link routes are djb2 hashes of the link names.
inline constexpr uint32_t route(const char *name)
{
    uint32_t h = 5381;
    for (; *name; name++)
        h = h * 33 + uint8_t(*name);
    return h;
}
constexpr uint32_t kRouteFinished = route("finished"), kRouteSpectate = route("spectate"), kRouteRestart = route("restart");

}
