#pragma once
// Generated from data/pit_exit.json (AI pit route gates of every stock track common ERP): the pit lane proper is the
// run of pit gates more than 8 m from any main gate; queue head = 20 m before its end (pit exit), world space, y up.
// length = AI main-gate loop in m (track identification, as in the cooldown prototype).
namespace redflag {

struct PitExit {
    const char *track;
    float length;
    float head[3];
};

inline constexpr PitExit kPitExit[] = {
    {"abu_dhabi", 5214.41f, {252.580f, 13.304f, -47.147f}},
    {"austria", 4291.91f, {-176.557f, -23.761f, 43.587f}},
    {"austria_reverse", 4290.59f, {698.253f, -29.648f, -30.409f}},
    {"bahrain", 5349.55f, {-358.349f, -13.480f, -514.154f}},
    {"baku", 5950.10f, {798.957f, 0.657f, -795.505f}},
    {"brazil", 4216.54f, {40.150f, 33.153f, 217.379f}},
    {"catalunya", 4618.87f, {-91.122f, -83.810f, 423.762f}},
    {"hungaroring", 4321.80f, {-539.028f, 35.222f, 119.326f}},
    {"imola", 4867.88f, {-164.978f, 42.202f, -398.844f}},
    {"jeddah", 6096.10f, {-47.487f, -0.389f, 337.736f}},
    {"las_vegas", 6154.52f, {520.836f, 34.622f, 600.515f}},
    {"losail", 5358.47f, {-591.370f, 25.357f, -183.289f}},
    {"madrid", 5329.96f, {-357.555f, 31.288f, 644.337f}},
    {"melbourne", 5223.73f, {-333.192f, 1.873f, 239.774f}},
    {"mexico", 4455.55f, {150.400f, 0.372f, -887.899f}},
    {"miami", 5332.64f, {137.429f, 1.568f, 56.894f}},
    {"monaco", 3261.00f, {-320.956f, -10.928f, 16.335f}},
    {"montreal", 4312.35f, {432.666f, 87.667f, 1414.130f}},
    {"monza", 5765.60f, {-543.613f, 1.113f, -202.921f}},
    {"portimao", 4588.21f, {-213.037f, -2.856f, -411.714f}},
    {"shanghai", 5369.85f, {-338.534f, -3.301f, 428.255f}},
    {"silverstone", 5812.66f, {23.173f, 4.092f, 4.068f}},
    {"silverstone_reverse", 5801.44f, {-217.619f, -1.354f, 668.632f}},
    {"singapore", 4880.56f, {576.099f, -12.197f, -383.405f}},
    {"spa_francorchamps", 6938.08f, {-320.723f, 15.728f, -968.328f}},
    {"suzuka", 5755.66f, {964.961f, -29.471f, 355.116f}},
    {"texas", 5417.98f, {-233.060f, -118.426f, 1002.190f}},
    {"zandvoort", 4194.46f, {-164.164f, 4.401f, -311.869f}},
    {"zandvoort_reverse", 4191.74f, {-416.052f, 5.539f, 124.817f}},
};

}
