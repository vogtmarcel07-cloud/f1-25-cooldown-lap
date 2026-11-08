// SPDX-License-Identifier: MIT
#pragma once
// Parc ferme boards "1", "2", "3" in the game picture (Swofty overlay): the parc ferme marking of the game is a
// podium cutscene scene that is not loaded in the cool-down lap (user 2026-10-29 11:05: simple boards with 1/2/3).
// The camera comes from an own call-site redirect (no launcher binding): in the frame function 0x140157d90 the call
// Render_View_SetFromCamera(V, cam) (0x143be0360, call site 0x1401583d0, one per viewport) is sent through a stub
// that calls the original and copies V+0x80 (projection) and V+0xc0 (view), both 4x4 row-major, v' = v * M
// (Engine-RE 2026-10-29, static). The first call after each painted frame is taken as the player view.
// Test 0.33.1 (Mexico): with V+0xc0 * V+0x80 the boards came out behind the camera (clip w < 0, no digits). So the
// first 0x200 bytes of V are kept and the matrix layout is calibrated on the player's car (which the camera looks
// at): every 16-byte aligned 4x4 in V, plain or transposed, alone or as view * projection, is tried; the first that
// puts the car in front of the camera near the picture centre for 20 frames in a row is used (and logged).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include <windows.h>

#include <swofty/mem.hpp>
#include <swofty/overlay.hpp>

namespace boards {

using SetFromCamera_t = void (*)(void *view, void *cam);

inline SetFromCamera_t g_orig = nullptr;
inline std::atomic<uint32_t> g_seq{0};     // odd while the matrices are written
inline constexpr int kVBytes = 0x200, kMats = (kVBytes - 0x40) / 0x10 + 1;
inline float g_v[kVBytes / 4];
inline std::atomic<bool> g_have_player{false};
inline float g_player[3];
inline int g_cal_view = -1, g_cal_proj = -1, g_cal_tv = 0, g_cal_tp = 0, g_cal_streak = 0, g_cal_cand = -1;
inline bool g_cal_done = false;
inline double g_cal_log_frames = 0;
inline std::atomic<int> g_calls{0};        // calls since the last painted frame
inline std::atomic<int> g_calls_seen{0};   // calls in the last frame (log)
inline std::atomic<bool> g_have{false}, g_show{false};
inline float g_pos[3][3];
inline bool g_installed = false;
inline std::atomic<int> g_logged{0};

inline void thunk(void *view, void *cam)
{
    g_orig(view, cam);
    if (g_calls.fetch_add(1) != 0 || !view)
        return;
    auto v = reinterpret_cast<uintptr_t>(view);
    if (!swofty::mem::readable(v, kVBytes))
        return;
    g_seq.fetch_add(1);
    std::memcpy(g_v, reinterpret_cast<const void *>(v), sizeof g_v);
    g_seq.fetch_add(1);
    g_have = true;
}

// [x y z 1] * M (row-major); transposed: M * [x y z 1].
inline void mul(const float *m, const float in[4], float out[4], int transposed = 0)
{
    for (int c = 0; c < 4; c++)
        out[c] = transposed ? in[0] * m[4 * c] + in[1] * m[4 * c + 1] + in[2] * m[4 * c + 2] + in[3] * m[4 * c + 3]
                            : in[0] * m[c] + in[1] * m[4 + c] + in[2] * m[8 + c] + in[3] * m[12 + c];
}

// Clip position of a world point with a candidate layout (view < 0: projection matrix alone = view * projection).
inline void project(const float *vb, int view, int tv, int proj, int tp, const float wp[4], float clip[4])
{
    if (view < 0) {
        mul(vb + proj * 4, wp, clip, tp);
        return;
    }
    float vv[4];
    mul(vb + view * 4, wp, vv, tv);
    mul(vb + proj * 4, vv, clip, tp);
}

// The car is in front and near the picture centre (chase or cockpit view: x within 0.35, y -1.2..0.6).
inline bool car_ok_1(const float clip[4])
{
    if (!(clip[3] > 0.3f) || !std::isfinite(clip[3]) || clip[3] > 1e5f)
        return false;
    float x = clip[0] / clip[3], y = clip[1] / clip[3];
    return std::fabs(x) < 0.35f && y > -1.2f && y < 0.6f;
}

// Test 0.33.2: a layout that maps every point to the picture centre passed. Points 8 m beside the car must land
// elsewhere on the picture.
inline bool car_ok(const float *vb, int view, int tv, int proj, int tp, const float wp[4])
{
    float c0[4], c1[4], c2[4];
    project(vb, view, tv, proj, tp, wp, c0);
    if (!car_ok_1(c0))
        return false;
    float a[4] = {wp[0] + 8.0f, wp[1], wp[2], 1.0f}, b[4] = {wp[0], wp[1], wp[2] + 8.0f, 1.0f};
    project(vb, view, tv, proj, tp, a, c1);
    project(vb, view, tv, proj, tp, b, c2);
    if (!(c1[3] > 0.1f) || !(c2[3] > 0.1f))
        return true; // behind the camera is fine for a side point (cockpit view)
    float x0 = c0[0] / c0[3], y0 = c0[1] / c0[3];
    float d1 = std::fabs(c1[0] / c1[3] - x0) + std::fabs(c1[1] / c1[3] - y0);
    float d2 = std::fabs(c2[0] / c2[3] - x0) + std::fabs(c2[1] / c2[3] - y0);
    return d1 > 0.02f || d2 > 0.02f;
}

// One calibration step: the candidate in progress must hold; otherwise the next one that passes now starts a streak.
inline void calibrate(const float *vb)
{
    if (g_cal_done || !g_have_player)
        return;
    float wp[4] = {g_player[0], g_player[1] + 0.5f, g_player[2], 1.0f}, clip[4];
    int n = kMats * 2 + kMats * kMats * 4;
    auto decode = [](int c, int &view, int &tv, int &proj, int &tp) {
        if (c < kMats * 2) {
            view = -1, tv = 0, proj = c / 2, tp = c % 2;
            return;
        }
        c -= kMats * 2;
        tp = c & 1, tv = (c >> 1) & 1, c >>= 2;
        view = c / kMats, proj = c % kMats;
    };
    int view, tv, proj, tp;
    if (g_cal_cand >= 0) {
        decode(g_cal_cand, view, tv, proj, tp);
        project(vb, view, tv, proj, tp, wp, clip);
        if (car_ok(vb, view, tv, proj, tp, wp)) {
            if (++g_cal_streak >= 20) {
                g_cal_view = view, g_cal_tv = tv, g_cal_proj = proj, g_cal_tp = tp, g_cal_done = true;
                swofty::log("[racestart] board overlay: camera calibrated on the player's car: view %s+0x%x%s, "
                            "projection +0x%x%s (car at screen %.2f / %.2f, w %.2f)",
                            view < 0 ? "none " : "", view < 0 ? 0 : view * 16, tv ? " transposed" : "", proj * 16,
                            tp ? " transposed" : "", clip[0] / clip[3], clip[1] / clip[3], clip[3]);
            }
            return;
        }
        g_cal_streak = 0;
    }
    for (int i = 1; i <= n; i++) {
        int c = (g_cal_cand + i + n) % n;
        decode(c, view, tv, proj, tp);
        if (view == proj)
            continue;
        if (car_ok(vb, view, tv, proj, tp, wp)) {
            g_cal_cand = c, g_cal_streak = 1;
            return;
        }
    }
    g_cal_cand = -1;
    if (g_cal_log_frames++ == 300)
        swofty::log("[racestart] board overlay: no camera layout puts the player's car in the picture yet");
}

inline void paint(SwoftyPaint *p, void *)
{
    int calls = g_calls.exchange(0);
    if (calls)
        g_calls_seen = calls;
    if (!g_show || !g_have)
        return;
    static float vb[kVBytes / 4];
    for (int tries = 0;; tries++) {
        uint32_t s = g_seq.load();
        if (s & 1) {
            if (tries > 50)
                return;
            continue;
        }
        std::memcpy(vb, g_v, sizeof vb);
        if (g_seq.load() == s)
            break;
    }
    namespace ov = swofty::overlay;
    float w = 0, h = 0;
    ov::size(p, &w, &h);
    static int s_frames = 0;
    if (g_logged.load() == 0 && s_frames == 0)
        swofty::log("[racestart] board overlay: paint runs (%.0fx%.0f)", w, h);
    // Raw camera data once, 2 s into the cool-down, for the offline check of the layout (car position first).
    if (++s_frames == 120 && g_have_player) {
        swofty::log("[racestart] board overlay dump: car %.3f %.3f %.3f", g_player[0], g_player[1], g_player[2]);
        for (int r = 0; r < kVBytes / 64; r++) {
            const float *q = vb + r * 16;
            swofty::log("[racestart] board overlay dump +0x%03x: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g",
                        r * 64, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13],
                        q[14], q[15]);
        }
    }
    calibrate(vb);
    if (!g_cal_done)
        return;
    if (w <= 0 || h <= 0)
        return;
    struct Box {
        int k;
        float px, py, size, tw, th, bw, bh;
    } box[3];
    int n = 0;
    for (int k = 0; k < 3; k++) {
        float wp[4] = {g_pos[k][0], g_pos[k][1] + 1.2f, g_pos[k][2], 1.0f}, clip[4];
        project(vb, g_cal_view, g_cal_tv, g_cal_proj, g_cal_tp, wp, clip);
        float dx = g_pos[k][0] - g_player[0], dz = g_pos[k][2] - g_player[2];
        float dist = std::sqrt(dx * dx + dz * dz) + 5.0f; // size by the car's distance (camera about 5 m behind)
        static int s_draws = 0;
        if (g_logged.load() < 3 || (k == 0 && ++s_draws % 300 == 0) || (k > 0 && s_draws % 300 == 0)) {
            g_logged++;
            swofty::log("[racestart] board overlay: board %d (y %.1f, car y %.1f) %.1f m from the car, clip w %.2f, "
                        "screen %.0f / %.0f of "
                        "%.0fx%.0f, %d camera call(s) per frame",
                        k + 1, g_pos[k][1], g_player[1], dist - 5.0f, clip[3], clip[3] > 0 ? (clip[0] / clip[3] + 1) * 0.5f * w : -1.0f,
                        clip[3] > 0 ? (1 - clip[1] / clip[3]) * 0.5f * h : -1.0f, w, h, g_calls_seen.load());
        }
        if (clip[3] <= 0.1f || dist > 300.0f)
            continue;
        // Not mirrored: the "3" seen left in test 0.33.5 was drawn close up (box ~190 px = size ~140), where the P3
        // board lies left of the car; mirroring x (0.33.7) put the boards off the picture.
        float px = (clip[0] / clip[3] + 1.0f) * 0.5f * w, py = (1.0f - clip[1] / clip[3]) * 0.5f * h;
        if (px < -200 || px > w + 200 || py < -200 || py > h + 200)
            continue;
        // Text size = height in px (test 0.33.7: size 8 measured 4 x 8 px); 24 px from about 100 m, 110 px close up.
        float size = std::fmin(110.0f, std::fmax(24.0f, 2400.0f / std::fmax(dist, 1.0f)));
        char txt[4];
        std::snprintf(txt, sizeof txt, "%d", k + 1);
        float tw = 0, th = 0;
        ov::measure(p, size, txt, &tw, &th);
        static int s_measured = 0;
        if (s_measured++ < 2)
            swofty::log("[racestart] board overlay: digit at size %.0f measures %.0f x %.0f px", size, tw, th);
        Box &b = box[n++];
        b = {k, px, py, size, tw, th, std::fmax(tw, th) * 1.5f, th * 1.3f};
    }
    // Far away all three boards fall on almost the same pixel (test 0.33.8: 1143/1137/1143 at 450 m): overlapping
    // boxes are pushed apart sideways in their screen order, the group stays centred where they were.
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && box[j].px < box[j - 1].px; j--)
            std::swap(box[j], box[j - 1]);
    float before = 0, after = 0;
    for (int i = 0; i < n; i++)
        before += box[i].px;
    for (int i = 1; i < n; i++) {
        float need = box[i - 1].px + (box[i - 1].bw + box[i].bw) / 2 + 6.0f;
        if (std::fabs(box[i].py - box[i - 1].py) < (box[i].bh + box[i - 1].bh) / 2 && box[i].px < need)
            box[i].px = need;
    }
    for (int i = 0; i < n; i++)
        after += box[i].px;
    float shift = n ? (after - before) / n : 0;
    char txt[4];
    for (int i = 0; i < n; i++) {
        const Box &b = box[i];
        float px = b.px - shift;
        std::snprintf(txt, sizeof txt, "%d", b.k + 1);
        ov::rect(p, px - b.bw / 2, b.py - b.bh / 2, b.bw, b.bh, 0x111111d8);
        ov::rect_outline(p, px - b.bw / 2, b.py - b.bh / 2, b.bw, b.bh, std::fmax(2.0f, b.size / 20.0f), 0xffffffff);
        ov::text(p, px - b.tw / 2, b.py - b.th / 2, b.size, 0xffffffff, txt);
    }
}

// Executable stub within +-2 GB of the call site: jmp qword [rip+0] -> thunk.
inline uint8_t *near_stub(uintptr_t at)
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t off = gran; off < 0x70000000; off += gran) {
        for (int dir = 0; dir < 2; dir++) {
            uintptr_t a = dir ? (at & ~(gran - 1)) + off : (at & ~(gran - 1)) - off;
            if (auto *p = static_cast<uint8_t *>(
                    VirtualAlloc(reinterpret_cast<void *>(a), 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
                return p;
        }
    }
    return nullptr;
}

// Redirects the camera call site and registers the paint callback. Logs and gives up on any mismatch.
inline void install()
{
    if (g_installed)
        return;
    g_installed = true;
    if (!swofty::overlay::available()) {
        swofty::log("[racestart] board overlay: Swofty overlay not available, no 1/2/3 boards");
        return;
    }
    uintptr_t hit = 0;
    int hits = swofty::api && swofty::api->scan
                   ? swofty::api->scan(".xtls", "E8 ?? ?? ?? ?? 42 83 BC BE F4 0A 00 00 00", &hit)
                   : -1;
    if (hits != 1 || !hit) {
        swofty::log("[racestart] board overlay: camera call site %d hit(s), no 1/2/3 boards", hits);
        return;
    }
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void *>(hit + 1), 4);
    uintptr_t target = hit + 5 + intptr_t(rel);
    uint8_t *stub = near_stub(hit);
    if (!stub) {
        swofty::log("[racestart] board overlay: no memory near the call site, no 1/2/3 boards");
        return;
    }
    g_orig = reinterpret_cast<SetFromCamera_t>(target);
    stub[0] = 0xFF, stub[1] = 0x25;
    std::memset(stub + 2, 0, 4);
    auto fn = reinterpret_cast<uintptr_t>(&thunk);
    std::memcpy(stub + 6, &fn, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    int32_t nrel = int32_t(intptr_t(reinterpret_cast<uintptr_t>(stub)) - intptr_t(hit + 5));
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void *>(hit + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
        swofty::log("[racestart] board overlay: VirtualProtect failed (%lu), no 1/2/3 boards", GetLastError());
        return;
    }
    std::memcpy(reinterpret_cast<void *>(hit + 1), &nrel, 4);
    VirtualProtect(reinterpret_cast<void *>(hit + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(hit), 5);
    auto pr = swofty::overlay::on_paint(paint);
    if (pr != swofty::overlay::Result::ok)
        swofty::log("[racestart] board overlay: paint callback NOT registered (%d)", int(pr));
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    swofty::log("[racestart] board overlay: camera call site base+0x%llx -> base+0x%llx redirected, 1/2/3 boards ready",
                (unsigned long long)(hit - base), (unsigned long long)(target - base));
}

inline void show(const float pos[3][3])
{
    std::memcpy(g_pos, pos, sizeof g_pos);
    g_logged = 0;
    g_have_player = false;
    g_logged = 0;
    // Test 0.33.3 (Bahrain dump): +0xc0 is the view (inverse of the camera's world matrix at +0x100, which holds the
    // eye), +0x80 the projection (w = -z view), both row-major, v' = v * M: the 0.33.1 layout was right (its Mexico
    // log was taken with the boards behind the car). The calibration on the car picked wrong layouts; it is off.
    g_cal_view = 0xc0 / 16, g_cal_proj = 0x80 / 16, g_cal_tv = g_cal_tp = 0, g_cal_done = true;
    g_show = true;
}

inline void hide() { g_show = false; }

// The player's car position (game thread, every update while the boards are shown).
inline void player(const float *m)
{
    if (!m)
        return;
    g_player[0] = m[12], g_player[1] = m[13], g_player[2] = m[14];
    g_have_player = true;
}

} // namespace boards
