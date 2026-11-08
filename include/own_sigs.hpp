// Own hook rows for the public builds of Race Start, Red Flag Pit and SC Unlapping (own_hooks.hpp). Patterns from
// the test bindings, prologues from Engine-RE work/ownhooks (2026-10-29, static on the KB exe, build d873a1c8): the
// head bytes must match, else that hook stays off.
#pragma once

#include "own_hooks.hpp"

namespace own {

inline const Sig kSigs[] = {
    // push rsi; push rdi; push rbx; sub rsp,0x50; mov rdi,r8; mov rbx,rdx; mov rsi,rcx = 16 bytes
    {"Rules_OnSessionEnter", "56 57 53 48 83 EC 50 4C 89 C7 48 89 D3 48 89 CE 48 8B 02 48 8D 15 ?? ?? ?? ?? 48 89 D9 FF 50 28 48 89 86 00 1C 00 00",
     0, Resolve::addr, 0, "56 57 53 48 83 EC 50 4C 89 C7 48 89 D3 48 89 CE", 16},
    // push rsi; push rdi; sub rsp,0x28; mov rsi,rcx; mov rax,[rip+d] (relocated) = 16 bytes
    {"Rules_IsPracticeQualifying", "56 57 48 83 EC 28 48 89 CE 48 8B 05 A0 BB 60 03", 0, Resolve::addr, 0,
     "56 57 48 83 EC 28 48 89 CE 48 8B 05", 16, false, 9},
    // the call site's rel32 leads to a jmp thunk (E9 -> .data1): only the thunk's rel32 is bent to our stub
    {"Pit_PlaceCarBlend", "48 8D 15 ?? ?? ?? ?? 4C 8D 4C 24 40 48 89 F9 4D 89 F0 E8 ?? ?? ?? ?? EB 27", 0,
     Resolve::rel32, 18, "E9", 0},
    // jmp thunk -> 0x1484045c0
    {"Ai_OnPitStateChanged_Thunk", "CC CC CC CC CC E9 AB 14 D5 07", 5, Resolve::addr, 0, "E9", 0},
    // push r15; push r14; push rsi; push rdi; push rbx; sub rsp,0x50; vmovaps [rsp+0x40],xmm8 = 17 bytes
    {"Ai_ScDeltaPace", "41 57 41 56 56 57 53 48 83 EC 50 C5 78 29 44 24 40 C5 F8 29 7C 24 30 C5 F8 29 74 24 20 0F B6 02 48 3D FF 00 00 00", 0,
     Resolve::addr, 0, "41 57 41 56 56 57 53 48 83 EC 50 C5 78 29 44 24 40", 17},
    // push rsi; push rdi; push rbx; sub rsp,0x20; mov [rcx+0x358],edx = 13 bytes, mov rax/jmp rax
    {"Sess_RaceRuleset_SetTotalLaps",
     "C3 CC CC CC CC CC CC CC CC CC CC CC 56 57 53 48 83 EC 20 89 91 58 03 00 00 48 8B B9 68 03 00 00 48 85 FF 74 68", 12, Resolve::addr, 0,
     "56 57 53 48 83 EC 20 89 91 58 03 00 00", 13, true},
    // push r14; push rsi; push rdi; push rbx; sub rsp,0x38; cmp byte [rcx+0x40],0 = 13 bytes (the jne after it stays)
    {"Sess_RedFlagSystem_Deploy", "41 56 56 57 53 48 83 EC 38 80 79 40 00 0F 85 C7 00 00 00 89 D3", 0, Resolve::addr, 0,
     "41 56 56 57 53 48 83 EC 38 80 79 40 00", 13, true},
    // site: cmp byte [r8+0x108],2; setne r8b = 12 bytes (rax free there; Engine-RE), run after the handler
    {"site_Ai_ScFollowFlagCmp", "0F B6 69 35 40 80 E5 10 41 0F 94 C1 41 80 B8 08 01 00 00 02", 12, Resolve::addr, 0,
     "41 80 B8 08 01 00 00 02 41 0F 95 C0", 12, true},
    // Session Watch: Sess_ViewInstantReplay_Update's call that posts MessageFlashbackUsed (@0x142da2a53) leads to the
    // jmp thunk Ne_Post_BusMessageQueued_FlashbackUsed 0x142cb7130 (E9 -> .data1, only that message type)
    {"Sess_FlashbackUsed_Post", "41 FF 46 3C 48 8B 0D ?? ?? ?? ?? 48 C7 44 24 48 00 00 00 00 48 8D 15 ?? ?? ?? ?? 48 89 54 24 40 88 44 24 50 48 8B 49 08 48 8D 54 24 40 E8 ?? ?? ?? ??",
     0, Resolve::rel32, 45, "E9", 0},
    // Session Watch: the session clock owner (global 0x146081700) from mov rax, [rip+d] in
    // Sess_Flags_GetSecondsSinceEvent 0x142f247a0; read only, never hooked
    {"g_Sess_ClockOwner", "56 57 53 48 83 EC 40 C5 F9 29 74 24 30 48 89 CE 48 8B 05 ?? ?? ?? ?? 48 8B B8 A8 01 00 00 80 BF 98 00 00 00 00",
     19, Resolve::riprel, 16, "", 0},
    // vtables: lea r, [rip+d] where the constructor stores them
    {"vt_Sess_RaceRuleset", "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 05 ?? ?? ?? ?? 48 8B 48 08 80 79 68 00 0F 84 DC", 3,
     Resolve::riprel, 0, "", 0},
    {"vt_Sc_DebugDeploy", "48 8D 0D ?? ?? ?? ?? 48 89 08 C6 05 C2 C9 99 02 00 48 89 7C 24 20 4C 89", 3, Resolve::riprel, 0,
     "", 0},
    {"vt_Sc_TriggerVsc", "48 8D 0D ?? ?? ?? ?? 48 89 08 48 C7 80 80 00 00 00 00 00 00 00 C5 F8 11", 3, Resolve::riprel, 0,
     "", 0},
    {"vt_Sc_AheadOfLead", "48 8D 05 ?? ?? ?? ?? 49 89 07 48 8D 05 ?? ?? ?? ?? 49 89 47 40 C4 C1 78 11 47 68 C4 C1 78 11 47 58 C4 C1 78 11 47 78 4D 89 B7 88 00 00 00", 3,
     Resolve::riprel, 0, "", 0},
    {"vt_Sc_CarsGrouped", "48 8D 05 ?? ?? ?? ?? 49 89 07 48 8D 05 ?? ?? ?? ?? 49 89 47 40 4D 89 77 58 C4 C1 78 11 47 70 C4 C1 78 11 47 60 C4 C1 78 11 87 90 00 00 00 C4 C1 78 11 87 80 00 00 00 C4 C1 78 11 87 B0 00 00 00", 3,
     Resolve::riprel, 0, "", 0},
    // Red Flag: VSC timing globals (data, never hooked) from Sess_ScState_Vsc_Enter 0x14363a4b0 at 0x14363a4eb:
    // timer 0x145f96b48, longest random length 0x145f96b50, length 0x145f96b54, "VSC ending" point 0x145f96b60, length
    // override 0x145f96b64 (int s, taken once by the enter). insn + 7 = end of the instruction that holds the disp32.
    {"g_Sess_VscTimer", "48 89 CE C7 05 ?? ?? ?? ?? 00 00 80 BF C5 FA 10 05 ?? ?? ?? ?? C5 FA 10 35 ?? ?? ?? ?? BF 34 01 00 00 48 8B 49 38 48 01 F9 C5 FA 5C FE E8 ?? ?? ?? ?? 89 C0 C4 E1 F2 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 85 C0 7E 16 C5 F2 2A C0 C5 FA 11 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? FF FF FF FF C5 FA 5C 0D ?? ?? ?? ?? C5 FA 5C 35 ?? ?? ?? ?? 48 03 7E 38 C5 F2 5C FE 48 89 F9 E8 ?? ?? ?? ?? 89 C0 C4 E1 EA 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ??",
     0x05, Resolve::riprel, 0x06, "", 0},
    {"g_Sess_VscMaxDuration", "48 89 CE C7 05 ?? ?? ?? ?? 00 00 80 BF C5 FA 10 05 ?? ?? ?? ?? C5 FA 10 35 ?? ?? ?? ?? BF 34 01 00 00 48 8B 49 38 48 01 F9 C5 FA 5C FE E8 ?? ?? ?? ?? 89 C0 C4 E1 F2 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 85 C0 7E 16 C5 F2 2A C0 C5 FA 11 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? FF FF FF FF C5 FA 5C 0D ?? ?? ?? ?? C5 FA 5C 35 ?? ?? ?? ?? 48 03 7E 38 C5 F2 5C FE 48 89 F9 E8 ?? ?? ?? ?? 89 C0 C4 E1 EA 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ??",
     0x11, Resolve::riprel, 0x0e, "", 0},
    {"g_Sess_VscDuration", "48 89 CE C7 05 ?? ?? ?? ?? 00 00 80 BF C5 FA 10 05 ?? ?? ?? ?? C5 FA 10 35 ?? ?? ?? ?? BF 34 01 00 00 48 8B 49 38 48 01 F9 C5 FA 5C FE E8 ?? ?? ?? ?? 89 C0 C4 E1 F2 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 85 C0 7E 16 C5 F2 2A C0 C5 FA 11 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? FF FF FF FF C5 FA 5C 0D ?? ?? ?? ?? C5 FA 5C 35 ?? ?? ?? ?? 48 03 7E 38 C5 F2 5C FE 48 89 F9 E8 ?? ?? ?? ?? 89 C0 C4 E1 EA 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ??",
     0x63, Resolve::riprel, 0x60, "", 0},
    {"g_Sess_VscEndingAt", "48 89 CE C7 05 ?? ?? ?? ?? 00 00 80 BF C5 FA 10 05 ?? ?? ?? ?? C5 FA 10 35 ?? ?? ?? ?? BF 34 01 00 00 48 8B 49 38 48 01 F9 C5 FA 5C FE E8 ?? ?? ?? ?? 89 C0 C4 E1 F2 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 85 C0 7E 16 C5 F2 2A C0 C5 FA 11 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? FF FF FF FF C5 FA 5C 0D ?? ?? ?? ?? C5 FA 5C 35 ?? ?? ?? ?? 48 03 7E 38 C5 F2 5C FE 48 89 F9 E8 ?? ?? ?? ?? 89 C0 C4 E1 EA 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ??",
     0xac, Resolve::riprel, 0xa9, "", 0},
    {"g_Sess_VscDurationOverride", "48 89 CE C7 05 ?? ?? ?? ?? 00 00 80 BF C5 FA 10 05 ?? ?? ?? ?? C5 FA 10 35 ?? ?? ?? ?? BF 34 01 00 00 48 8B 49 38 48 01 F9 C5 FA 5C FE E8 ?? ?? ?? ?? 89 C0 C4 E1 F2 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 85 C0 7E 16 C5 F2 2A C0 C5 FA 11 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? FF FF FF FF C5 FA 5C 0D ?? ?? ?? ?? C5 FA 5C 35 ?? ?? ?? ?? 48 03 7E 38 C5 F2 5C FE 48 89 F9 E8 ?? ?? ?? ?? 89 C0 C4 E1 EA 2A C0 C5 FA 59 05 ?? ?? ?? ?? C5 C2 59 C0 C5 FA 58 C6 C5 FA 11 05 ?? ?? ?? ??",
     0x69, Resolve::riprel, 0x6a, "", 0},
};

inline const Sig *sig(const char *name)
{
    for (const Sig &s : kSigs)
        if (!std::strcmp(s.name, name))
            return &s;
    return nullptr;
}

}
