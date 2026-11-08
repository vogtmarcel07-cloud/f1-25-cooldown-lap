// Pose line for screenshot comparisons (AVUS editor chat, Blender vs game 1:1, user 2026-10-31 14:23). Test aid part
// only (RS_PART 4): the test script posts command slot 25 value 1 (ctl post 25 1) right before a screenshot, the
// next game frame writes one [pose] line with the player's car matrix. Included by race_start.cpp after the probes.
// Matrix rows as the game stores them (row 0 / 1 / 2 = the car's local x / y / z axes, row 3 = position). In game
// (test chat 2026-10-31, AVUS run_a170b): r2 = forward (matches the x/z motion), r1 = up. Camera mode: not in the
// Engine-RE KB yet (cam=?).
constexpr int kPoseCommand = 25; // free slot in rd_shared.hpp's Command list (22 race control ... 31 warm-up)
double g_pose_t = 0;             // seconds of game frames since the DLL loaded

void pose_update(double dt)
{
    g_pose_t += dt;
    if (::rd::take(static_cast<::rd::Command>(kPoseCommand)) != 1)
        return;
    int pl = player_vehicle();
    Field f = read_field(true);
    const float *m = pl >= 0 && pl < kMaxCars && f.seen[pl] && f.car[pl] ? carhold::matrix(f.car[pl]) : nullptr;
    if (!m) {
        swofty::log("[pose] t=%.3f no player car (vehicle %d)", g_pose_t, pl);
        return;
    }
    float len = *swofty::game::g_Track_Length, prog = f.progress[pl];
    swofty::log("[pose] t=%.3f car %d x %.3f y %.3f z %.3f r0 %.4f %.4f %.4f r1 %.4f %.4f %.4f r2 %.4f %.4f %.4f "
                "progress %.5f lapdist %.1f tracklen %.1f phase %s cam=?",
                g_pose_t, pl, m[12], m[13], m[14], m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], prog,
                (prog - std::floor(prog)) * len, len, phase_name(g_phase));
}
