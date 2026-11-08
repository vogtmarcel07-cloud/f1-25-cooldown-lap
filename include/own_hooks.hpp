// Own hooks for the public builds (no test bindings): the game functions the launcher's bindings.json does not list
// are found by the addon's own byte patterns and detoured here instead of by swofty_core. Launcher 1.6 only plans
// addon-shipped bindings (docs/design/std-2/coverage.md), the core does not read them yet.
//
//   #ifdef SWOFTY_PUBLIC
//   OWN_WRAP(Rules_OnSessionEnter, uint64_t, void *rules, void *systems, void *components)
//   #else
//   SWOFTY_WRAP(Rules_OnSessionEnter, uint64_t, void *rules, void *systems, void *components)
//   #endif
//   { ... orig(rules, systems, components) ... }
//
// Every OWN_WRAP target needs a row in own_sigs.hpp (pattern, how the address follows from the hit, and how many
// prologue bytes move to the trampoline). own::install_all() runs from the addon's on-load; a target whose pattern
// does not match once, or whose first bytes differ from the row, stays unhooked with one log line (the rest of the
// addon works).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <windows.h>
#include <tlhelp32.h>

#include <swofty/mixin.hpp>
#include <vm_known.hpp>

namespace own {

enum class Resolve : uint8_t { addr, rel32, riprel };

struct Sig {
    const char *name;
    const char *pattern;
    uint32_t anchor;      // addr: address = hit + anchor; riprel: offset of the disp32 of a 7-byte lea at hit + insn
    Resolve resolve;
    uint32_t insn;        // rel32: offset of the E8/E9 opcode in the pattern
    const char *head;     // expected first bytes at the address ("E9" = a jmp thunk: its target is the original)
    uint32_t len;         // prologue bytes moved to the trampoline (whole instructions, no rip-relative ones); 0 = thunk
    bool movabs = false;  // patch with mov rax, imm64; jmp rax (12 bytes; rax free at the entry)
    uint32_t reloc = 0;   // offset of one mov rax, [rip+d] (48 8B 05) inside len, rebuilt as mov rax, imm64; mov rax, [rax]
};

struct Reg {
    const char *name;
    void *handler;
    void **orig;
    Reg *next;
    int slot; // >= 0: vtable slot (the sig finds the vtable), orig = the old function
};

inline Reg *&regs()
{
    static Reg *r = nullptr;
    return r;
}

struct AutoReg {
    Reg r;
    AutoReg(const char *name, void *handler, void **orig, int slot = -1) : r{name, handler, orig, regs(), slot}
    {
        regs() = &r;
    }
};

inline bool parse_head(const char *s, uint8_t *out, uint32_t &n)
{
    n = 0;
    while (*s && n < 32) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        char b[3] = {s[0], s[1], 0};
        out[n++] = static_cast<uint8_t>(strtoul(b, nullptr, 16));
        s += 2;
    }
    return n > 0;
}

// Executable memory within +-2 GB of at (a rel32 jmp from the game's code reaches it).
inline uint8_t *alloc_near(uintptr_t at, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t g = si.dwAllocationGranularity;
    for (uintptr_t d = g; d < 0x7ff00000; d += g * 16) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            uintptr_t want = ((at + sgn * static_cast<intptr_t>(d)) / g) * g;
            if (void *p = VirtualAlloc(reinterpret_cast<void *>(want), size, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t *>(p);
        }
    }
    return nullptr;
}

inline void jump_abs(uint8_t *at, uintptr_t to)
{
    at[0] = 0xFF, at[1] = 0x25; // jmp qword [rip+0]
    std::memset(at + 2, 0, 4);
    std::memcpy(at + 6, &to, 8);
}

// Writes n bytes of code while every other thread of the game is suspended, so no thread runs half old, half new
// bytes; a thread standing inside at+1 .. at+n-1 makes it retry (up to 50 x 1 ms), then the write is dropped. When
// expect is given the bytes are compared again while the threads stand (another thread may have changed them).
// No heap, no log while threads are suspended (one of them may hold the heap or log lock).
inline bool write_code(uintptr_t at, const void *bytes, size_t n, const void *expect = nullptr)
{
    static HANDLE held[4096];
    const DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (int tries = 0; tries < 50; tries++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return false;
        int nh = 0;
        bool inside = false;
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
                continue;
            if (nh == 4096) {
                inside = true; // more threads than we can hold: do not write
                break;
            }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            if (SuspendThread(h) == static_cast<DWORD>(-1)) {
                CloseHandle(h);
                continue;
            }
            held[nh++] = h;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c) && c.Rip > at && c.Rip < at + n)
                inside = true;
        }
        CloseHandle(snap);
        bool done = false;
        if (!inside && (!expect || !std::memcmp(reinterpret_cast<const void *>(at), expect, n))) {
            DWORD old = 0;
            if (VirtualProtect(reinterpret_cast<void *>(at), n, PAGE_EXECUTE_READWRITE, &old)) {
                std::memcpy(reinterpret_cast<void *>(at), bytes, n);
                VirtualProtect(reinterpret_cast<void *>(at), n, old, &old);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), n);
                done = true;
            }
        }
        for (int i = 0; i < nh; i++) {
            ResumeThread(held[i]);
            CloseHandle(held[i]);
        }
        if (!inside)
            return done;
        Sleep(1);
    }
    return false;
}

// Kill switch: a file vmknolle_own_hooks.off next to loader.log (the swofty folder) turns every own hook off.
inline bool switched_off()
{
    HMODULE self = nullptr;
    wchar_t p[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&switched_off), &self) ||
        !GetModuleFileNameW(self, p, MAX_PATH))
        return false;
    // <swofty>\addons\<id>\native\<dll>: four levels up
    for (int up = 0; up < 4; up++)
        if (wchar_t *s = wcsrchr(p, L'\\'))
            *s = 0;
    if (wcslen(p) + 32 >= MAX_PATH)
        return false;
    wcscat_s(p, L"\\vmknolle_own_hooks.off");
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

inline uintptr_t scan_sig(const Sig &s);

inline uintptr_t find(const Sig &s)
{
    // once another vmknolle addon hooked it, its pattern no longer matches: the address is in vm_known
    if (uintptr_t a = vm_known::get(s.name))
        return a;
    uintptr_t a = scan_sig(s);
    if (a)
        vm_known::put(s.name, a);
    return a;
}

inline uintptr_t scan_sig(const Sig &s)
{
    uintptr_t hit = 0;
    int hits = swofty::api && swofty::api->scan ? swofty::api->scan(".xtls", s.pattern, &hit) : -1;
    if (hits != 1 || !hit) {
        swofty::log("[own] %s: %d pattern hit(s), not hooked", s.name, hits);
        return 0;
    }
    if (s.resolve == Resolve::addr)
        return hit + s.anchor;
    if (s.resolve == Resolve::riprel) {
        int32_t d = 0;
        std::memcpy(&d, reinterpret_cast<const void *>(hit + s.anchor), 4);
        return hit + s.insn + 7 + d;
    }
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void *>(hit + s.insn + 1), 4);
    return hit + s.insn + 5 + rel;
}

// Detour at a: a thunk (E9 rel32) is pointed at a near stub that jumps to the handler, orig = the thunk's old target;
// otherwise len prologue bytes move to a trampoline (+ jmp back) and the prologue becomes jmp [rip] handler.
inline bool detour(const Sig &s, uintptr_t a, void *handler, void **orig)
{
    uint8_t head[32];
    uint32_t hn = 0;
    const auto *cur = reinterpret_cast<const uint8_t *>(a);
    // Another vmknolle addon hooked it first (jmp [rip] abs or mov rax, imm64; jmp rax): chain in front of it, orig
    // runs that addon's handler, which ends in the game's function.
    static const uint8_t kAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint32_t imm = !std::memcmp(cur, kAbs, 6) ? 6 : cur[0] == 0x48 && cur[1] == 0xB8 && cur[10] == 0xFF && cur[11] == 0xE0 ? 2 : 0;
    if (s.len && imm) {
        uint64_t prev = 0;
        std::memcpy(&prev, cur + imm, 8);
        std::memcpy(orig, &prev, 8);
        uintptr_t to = reinterpret_cast<uintptr_t>(handler);
        swofty::log("[own] %s: already hooked by another addon, chained in front", s.name);
        return write_code(a + imm, &to, 8, &prev);
    }
    if (!parse_head(s.head, head, hn) || std::memcmp(cur, head, hn)) {
        swofty::log("[own] %s: first bytes differ from the known build, not hooked", s.name);
        return false;
    }
    uint8_t *stub = alloc_near(a, 64);
    if (!stub)
        return false;
    if (!s.len) {
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void *>(a + 1), 4);
        *orig = reinterpret_cast<void *>(a + 5 + rel);
        jump_abs(stub, reinterpret_cast<uintptr_t>(handler));
        FlushInstructionCache(GetCurrentProcess(), stub, 64);
        int32_t nrel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - static_cast<intptr_t>(a + 5));
        uint8_t jmp[5] = {0xE9}, was[5];
        std::memcpy(jmp + 1, &nrel, 4);
        std::memcpy(was, cur, 5);
        return write_code(a, jmp, 5, was);
    }
    uint32_t jlen = s.movabs ? 12 : 14;
    if (s.len < jlen || s.len > 32)
        return false;
    uint8_t src[32]; // one snapshot: the trampoline is built from it and the write checks the code still equals it
    std::memcpy(src, cur, s.len);
    uint32_t n = 0;
    if (s.reloc && s.reloc + 7 <= s.len && src[s.reloc] == 0x48 && src[s.reloc + 1] == 0x8B && src[s.reloc + 2] == 0x05) {
        int32_t d = 0;
        std::memcpy(&d, src + s.reloc + 3, 4);
        uintptr_t abs = a + s.reloc + 7 + d;
        std::memcpy(stub, src, s.reloc);
        n = s.reloc;
        stub[n++] = 0x48, stub[n++] = 0xB8; // mov rax, imm64
        std::memcpy(stub + n, &abs, 8);
        n += 8;
        stub[n++] = 0x48, stub[n++] = 0x8B, stub[n++] = 0x00; // mov rax, [rax]
        std::memcpy(stub + n, src + s.reloc + 7, s.len - s.reloc - 7);
        n += s.len - s.reloc - 7;
    } else if (s.reloc) {
        swofty::log("[own] %s: the instruction to relocate is not there, not hooked", s.name);
        return false;
    } else {
        std::memcpy(stub, src, s.len);
        n = s.len;
    }
    jump_abs(stub + n, a + s.len);
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    *orig = stub;
    uint8_t patch[32];
    uintptr_t to = reinterpret_cast<uintptr_t>(handler);
    if (s.movabs) {
        patch[0] = 0x48, patch[1] = 0xB8; // mov rax, imm64
        std::memcpy(patch + 2, &to, 8);
        patch[10] = 0xFF, patch[11] = 0xE0; // jmp rax
    } else {
        jump_abs(patch, to);
    }
    std::memset(patch + jlen, 0x90, s.len - jlen);
    return write_code(a, patch, s.len, src);
}

// Swaps one vtable slot; returns the old function (0 = not swapped).
inline uintptr_t vtable_swap(uintptr_t vt, uint32_t slot, void *fn)
{
    uintptr_t at = vt + slot * 8, old = 0;
    if (!vt)
        return 0;
    std::memcpy(&old, reinterpret_cast<const void *>(at), 8);
    uintptr_t now = reinterpret_cast<uintptr_t>(fn);
    return write_code(at, &now, 8, &old) ? old : 0;
}

// A site: the integer registers the stub saved (stack order), read and written by the handler.
struct SiteRegs {
    uint64_t rflags, r11, r10, r9, r8, rdx, rcx, rbp;
};
constexpr int kSiteSlot = -2; // Reg::slot of a site hook

// Runs a SWOFTY_SITE-style handler on the saved registers (the others read as 0 and are not written back).
inline void site_call(SiteRegs *r, void (*h)(swofty::Context &))
{
    swofty::Context c{};
    c.rflags = r->rflags, c.r11 = r->r11, c.r10 = r->r10, c.r9 = r->r9, c.r8 = r->r8, c.rdx = r->rdx, c.rcx = r->rcx;
    c.rbp = r->rbp;
    h(c);
    r->rflags = c.rflags, r->r11 = c.r11, r->r10 = c.r10, r->r9 = c.r9, r->r8 = c.r8, r->rdx = c.rdx, r->rcx = c.rcx;
    r->rbp = c.rbp;
}

// Site hook at a (mov rax, imm64; jmp rax over s.len bytes; rax must be free there): the stub saves rbp, the volatile
// integer registers, flags and xmm0-5, calls entry(regs), restores them (with the handler's changes), runs the s.len
// original bytes (no rip-relative code in them) and jumps back to a + s.len.
inline bool site(const Sig &s, uintptr_t a, void (*entry)(SiteRegs *))
{
    uint8_t head[32];
    uint32_t hn = 0;
    if (!parse_head(s.head, head, hn) || std::memcmp(reinterpret_cast<const void *>(a), head, hn) || s.len < 12 ||
        s.len > 32) {
        swofty::log("[own] %s: first bytes differ from the known build, not hooked", s.name);
        return false;
    }
    uint8_t *st = alloc_near(a, 256);
    if (!st)
        return false;
    static const uint8_t kSave[] = {
        0x55, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x9C, // push rbp..r11; pushfq
        0x48, 0x83, 0xEC, 0x60,                                                 // sub rsp, 0x60
        0xF3, 0x0F, 0x7F, 0x04, 0x24, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x10,       // movdqu [rsp], xmm0 / +0x10 xmm1
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x30, // xmm2, xmm3
        0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x50, // xmm4, xmm5
        0x48, 0x8D, 0x4C, 0x24, 0x60,                                           // lea rcx, [rsp+0x60] (SiteRegs*)
        0x53, 0x48, 0x89, 0xE3,                                                 // push rbx; mov rbx, rsp
        0x48, 0x83, 0xE4, 0xF0, 0x48, 0x83, 0xEC, 0x20,                         // and rsp, -16; sub rsp, 0x20
        0x48, 0xB8};                                                            // mov rax, imm64 (entry)
    static const uint8_t kRestore[] = {
        0xFF, 0xD0,                                                             // call rax
        0x48, 0x89, 0xDC, 0x5B,                                                 // mov rsp, rbx; pop rbx
        0xF3, 0x0F, 0x6F, 0x04, 0x24, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x10,
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x30,
        0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x50,
        0x48, 0x83, 0xC4, 0x60,                                                 // add rsp, 0x60
        0x9D, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x5D}; // popfq; pop r11..rbp
    uint8_t was[32];
    std::memcpy(was, reinterpret_cast<const void *>(a), s.len);
    size_t n = 0;
    std::memcpy(st + n, kSave, sizeof kSave);
    n += sizeof kSave;
    uintptr_t e = reinterpret_cast<uintptr_t>(entry);
    std::memcpy(st + n, &e, 8);
    n += 8;
    std::memcpy(st + n, kRestore, sizeof kRestore);
    n += sizeof kRestore;
    std::memcpy(st + n, was, s.len);
    n += s.len;
    uintptr_t back = a + s.len;
    st[n++] = 0x48, st[n++] = 0xB8; // mov rax, imm64; jmp rax
    std::memcpy(st + n, &back, 8);
    n += 8;
    st[n++] = 0xFF, st[n++] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), st, 256);
    uint8_t patch[32];
    uintptr_t to = reinterpret_cast<uintptr_t>(st);
    patch[0] = 0x48, patch[1] = 0xB8;
    std::memcpy(patch + 2, &to, 8);
    patch[10] = 0xFF, patch[11] = 0xE0;
    std::memset(patch + 12, 0x90, s.len - 12);
    return write_code(a, patch, s.len, was);
}

// The sig row of a target; the addon's own sigs header defines it (e.g. rs_own_sigs.hpp).
inline const Sig *sig(const char *name);

inline int install_all(const char *tag)
{
    static bool done = false;
    if (done)
        return 0;
    done = true;
    if (switched_off()) {
        swofty::log("[%s] own hooks: off (vmknolle_own_hooks.off)", tag);
        return 0;
    }
    int ok = 0, all = 0;
    for (Reg *r = regs(); r; r = r->next) {
        all++;
        const Sig *s = sig(r->name);
        uintptr_t a = s ? find(*s) : 0;
        if (a && r->slot == kSiteSlot) {
            if (site(*s, a, reinterpret_cast<void (*)(SiteRegs *)>(r->handler))) {
                ok++;
                swofty::log("[own] %s site hooked at %#llx", r->name, static_cast<unsigned long long>(a));
            }
        } else if (a && r->slot >= 0) {
            uintptr_t old = vtable_swap(a, static_cast<uint32_t>(r->slot), r->handler);
            *r->orig = reinterpret_cast<void *>(old);
            if (old) {
                ok++;
                swofty::log("[own] %s slot %d swapped at %#llx", r->name, r->slot, static_cast<unsigned long long>(a));
            }
        } else if (a && detour(*s, a, r->handler, r->orig)) {
            ok++;
            swofty::log("[own] %s hooked at %#llx", r->name, static_cast<unsigned long long>(a));
        }
    }
    swofty::log("[%s] own hooks: %d of %d set", tag, ok, all);
    return ok;
}


}

// The handler's body follows the macro, as with SWOFTY_WRAP; orig calls the game's function. A target without a
// working hook leaves orig null and the handler is never called.
#define OWN_WRAP_IMPL(target, R, ...)                                                                                       \
    namespace {                                                                                                       \
    namespace own_##target {                                                                                          \
    R (*orig)(__VA_ARGS__) = nullptr;                                                                                  \
    R handler(__VA_ARGS__);                                                                                            \
    ::own::AutoReg reg{#target, reinterpret_cast<void *>(&handler), reinterpret_cast<void **>(&orig)};               \
    }                                                                                                                 \
    }                                                                                                                 \
    R own_##target::handler(__VA_ARGS__)

// A vtable slot: the handler replaces slot; orig is the function that was there.
#define OWN_VTABLE_IMPL(target, slot, R, ...)                                                                               \
    namespace {                                                                                                       \
    namespace own_##target {                                                                                          \
    R (*orig)(__VA_ARGS__) = nullptr;                                                                                  \
    R handler(__VA_ARGS__);                                                                                            \
    ::own::AutoReg reg{#target, reinterpret_cast<void *>(&handler), reinterpret_cast<void **>(&orig), (slot)};       \
    }                                                                                                                 \
    }                                                                                                                 \
    R own_##target::handler(__VA_ARGS__)

// A site (swofty::Context &c as with SWOFTY_SITE): the handler runs before the replaced instructions.
#define OWN_SITE_IMPL(target, param)                                                                                        \
    namespace {                                                                                                       \
    namespace own_##target {                                                                                          \
    void handler(param);                                                                                               \
    void entry(::own::SiteRegs *r) { ::own::site_call(r, &handler); }                                               \
    void *unused = nullptr;                                                                                            \
    ::own::AutoReg reg{#target, reinterpret_cast<void *>(&entry), &unused, ::own::kSiteSlot};                         \
    }                                                                                                                 \
    }                                                                                                                 \
    void own_##target::handler(param)

// OWN_KIND_<target> (generated by public-build/public_all.py into local_sigs.hpp): CORE when the launcher's
// bindings.json has the function (swofty_core hooks it, own_sigs row unused), OWN when only our own hook reaches it.
#define OWN_CAT_(a, b) a##b
#define OWN_CAT(a, b) OWN_CAT_(a, b)
#define OWN_WRAP_CORE SWOFTY_WRAP
#define OWN_WRAP_OWN OWN_WRAP_IMPL
#define OWN_VTABLE_CORE SWOFTY_VTABLE
#define OWN_VTABLE_OWN OWN_VTABLE_IMPL
#define OWN_SITE_CORE SWOFTY_SITE
#define OWN_SITE_OWN OWN_SITE_IMPL
#define OWN_WRAP(target, R, ...) OWN_CAT(OWN_WRAP_, OWN_KIND_##target)(target, R, __VA_ARGS__)
#define OWN_VTABLE(target, slot, R, ...) OWN_CAT(OWN_VTABLE_, OWN_KIND_##target)(target, slot, R, __VA_ARGS__)
#define OWN_SITE(target, param) OWN_CAT(OWN_SITE_, OWN_KIND_##target)(target, param)
