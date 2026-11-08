#pragma once
// Generated from data/parc_ferme.json (CutscenePlacementParcFerme of every stock track common ERP, projected onto
// the main AI gates). length = AI main-gate loop in m, progress = parc ferme position as fraction of the lap from
// gate 0, pos = placement position in world space (y up).
namespace pftab {

struct ParcFerme {
    const char *track;
    float length;
    float progress;
    bool in_pit_lane;
    float pos[3];
};

inline constexpr ParcFerme kParcFerme[] = {
    {"abu_dhabi", 5214.41f, 0.0167f, true, {-170.935f, 3.330f, -47.332f}},
    {"austria", 4291.91f, 0.0112f, true, {418.521f, -42.596f, 168.534f}},
    {"austria_reverse", 4290.59f, 0.9888f, true, {418.521f, -42.596f, 168.534f}},
    {"bahrain", 5349.55f, 0.0097f, true, {-364.687f, -15.427f, 101.841f}},
    {"baku", 5950.10f, 0.0000f, true, {598.307f, 0.614f, -468.716f}},
    {"brazil", 4216.54f, 0.0109f, true, {-481.845f, 60.742f, 397.523f}},
    {"catalunya", 4618.87f, 0.0000f, false, {289.179f, -77.096f, -198.253f}},
    {"hungaroring", 4321.80f, 0.0000f, true, {-129.127f, 37.540f, 439.020f}},
    {"imola", 4867.88f, 0.0000f, true, {448.317f, 38.875f, -384.997f}},
    {"jeddah", 6096.10f, 0.0072f, false, {138.684f, 0.811f, 772.579f}},
    {"las_vegas", 6154.52f, 0.0534f, true, {535.501f, 34.613f, 625.252f}},
    {"losail", 5358.47f, 0.0000f, true, {-271.125f, 26.888f, 358.647f}},
    {"madrid", 5329.96f, 0.0058f, true, {347.079f, 27.417f, 760.509f}},
    {"melbourne", 5223.73f, 0.0000f, true, {-97.710f, 2.975f, 452.340f}},
    {"mexico", 4455.55f, 0.8800f, false, {-827.690f, -0.088f, -841.930f}},
    {"miami", 5332.64f, 0.0234f, false, {-62.085f, 1.983f, -146.630f}},
    {"monaco", 3261.00f, 0.9927f, false, {-348.702f, -14.040f, 235.919f}},
    {"montreal", 4312.35f, 0.0000f, true, {390.579f, 87.365f, 1138.450f}},
    {"monza", 5765.60f, 0.0053f, true, {-599.556f, -3.317f, 587.231f}},
    {"shanghai", 5369.85f, 0.0000f, true, {68.426f, -3.231f, 318.174f}},
    {"silverstone", 5812.66f, 0.9978f, true, {-437.708f, -2.234f, 377.173f}},
    {"silverstone_reverse", 5801.44f, 0.0485f, true, {-437.907f, -2.234f, 379.986f}},
    {"singapore", 4880.56f, 0.9952f, true, {700.339f, -12.118f, 78.549f}},
    {"spa_francorchamps", 6938.08f, 0.0000f, true, {-220.230f, 14.504f, -684.188f}},
    {"suzuka", 5755.66f, 0.0069f, true, {630.344f, -15.992f, -7.936f}},
    {"texas", 5417.98f, 0.0084f, true, {-670.630f, -139.510f, 658.987f}},
    {"zandvoort", 4194.46f, 0.0000f, true, {-310.109f, 4.386f, -105.966f}},
    {"zandvoort_reverse", 4191.74f, 0.0000f, true, {-310.109f, 4.460f, -105.966f}},
};

}
