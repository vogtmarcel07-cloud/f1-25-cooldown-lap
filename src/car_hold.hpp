#pragma once
// Holding a driven car in place. A car serving a drive-through is driven by the AI with full physics (pit state
// InPitLane_DrivingThrough), not moved along the pit lane spline, so the spline freeze does not reach it. Instead the
// car is put back on its stored matrix every frame with the game's teleport and flag ZeroVelocity, the same call
// OSQTeleportToStart and the practice start use. The car object comes from the race record: record+0x18 -> +0x58
// (vehicle id u8 at car+0xe8), world matrix [car+0x558]+0x190. Shared by red-flag-pit, pitlane-start and
// sc-unlapping (one copy per module folder).
#include <cstring>

#include <swofty/mem.hpp>
#include <swofty/mixin.hpp>

namespace swofty::game {

// Vehicle teleport (0x14473dcc0): (car, caller name, prev matrix, new matrix, dt, flags).
using Veh_Teleport_t = void(void *car, const char *name, float *prev_mtx, float *new_mtx, float dt, uint32_t flags);
inline constexpr fn<"Game_Teleport_Vehicle", Veh_Teleport_t> Veh_Teleport{}; // L1 local

}

namespace carhold {

constexpr size_t kRecCar = 0x18, kRecCarObj = 0x58, kCarPhysics = 0x558, kPhysicsMatrix = 0x190;
constexpr uint32_t kZeroVelocity = 0x40;

inline uintptr_t car_of(uintptr_t record)
{
    uintptr_t a = 0, car = 0;
    return record && swofty::mem::read(record + kRecCar, a) && a && swofty::mem::read(a + kRecCarObj, car) ? car : 0;
}

// The car's live world matrix (4x4, translation at [12..14]), or nullptr.
inline float *matrix(uintptr_t car)
{
    uintptr_t phys = 0;
    if (!car || !swofty::mem::read(car + kCarPhysics, phys) || !phys ||
        !swofty::mem::readable(phys + kPhysicsMatrix, 64))
        return nullptr;
    return reinterpret_cast<float *>(phys + kPhysicsMatrix);
}

// Puts the car back on `at` with zero velocity (call every frame while it is held). keep_height leaves the car's own
// height, so a car placed from data settles on the ground instead of being held in the air. Only a car that has moved
// more than 0.5 m is teleported: every teleport makes the game allocate in its TRACK_INFO heap, and teleporting a
// whole field every frame filled that heap until the game stopped (2026-10-23).
inline void pin(uintptr_t car, const float *at, float dt, const char *who, bool keep_height = false)
{
    float *cur = matrix(car);
    if (!cur)
        return;
    float dx = cur[12] - at[12], dz = cur[14] - at[14];
    if (dx * dx + dz * dz < 0.25f)
        return;
    float target[16];
    std::memcpy(target, at, sizeof target);
    if (keep_height)
        target[13] = cur[13];
    swofty::game::Veh_Teleport(reinterpret_cast<void *>(car), who, cur, target, dt > 0 ? dt : 1.f / 60.f,
                               kZeroVelocity);
}

}
