#include "Overlay.h"
#include "GameDll.h"
#include "Config.h"
#include "Log.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace tr {
namespace {

// The one byte that does the work.
constexpr uint8_t kRet = 0xC3;

// Prologues, verified out of both PDBs and byte-identical between them:
//
//   DrawNormalBinocs        48 89 5C 24 08   mov [rsp+8],    rbx
//   DrawVCIHeadset          48 89 5C 24 10   mov [rsp+0x10], rbx   (debug)
//   DrawLabyrinthFishEye    48 89 5C 24 10   mov [rsp+0x10], rbx   (debug)
//   DrawNormalLaserSight    48 89 5C 24 10   mov [rsp+0x10], rbx
//
// The retail DLLs save rbx at [rsp+8] in DrawVCIHeadset and DrawLabyrinthFishEye,
// so those two take their expected bytes from the row rather than from here.
//
// Only the first byte is overwritten, so the rest of that instruction is left
// as dead bytes that nothing branches into. They are checked anyway: five bytes
// of agreement is what makes "this is the function the PDB named" a fact rather
// than a hope, and it is the same standard InlineHook holds its targets to.
const uint8_t kSaveRbx08[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
const uint8_t kSaveRbx10[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };

//   DoInfraRedQuad          48 83 EC 28 45 33 C0   sub rsp,0x28 / xor r8d,r8d
//
// Seven bytes rather than five, because `sub rsp,0x28` is only four and a
// five-byte window would end mid-instruction. Nothing is jumped over here -- the
// stub is one byte at the entry -- but the window still has to land on a
// boundary for the comparison to mean "this is that function".
const uint8_t kSubRsp28[] = { 0x48, 0x83, 0xEC, 0x28, 0x45, 0x33, 0xC0 };

// Which ini setting governs which draw. The split follows what a player would
// ask for: "the binoculars look wrong" and "the scope looks wrong" are two
// different complaints, and the scope's frame carries its reticle lines, which
// somebody may well want to keep even after the binocular circles are gone.
enum class Group { Binocular, Scope, Tint };

struct Stub {
    uint32_t GameDllLayout::* rva;
    const uint8_t*            prologue;     // null: use rowPrologue
    const uint8_t* GameDllLayout::* rowPrologue;
    size_t                    prologueLen;
    const char*               name;
    Group                     group;
};

const Stub kStubs[] = {
    { &GameDllLayout::drawNormalBinocs,     kSaveRbx08, nullptr, sizeof(kSaveRbx08),
      "DrawNormalBinocs",     Group::Binocular },
    { &GameDllLayout::drawVCIHeadset,       nullptr, &GameDllLayout::drawVCIHeadsetPrologue,
      sizeof(kSaveRbx10),     "DrawVCIHeadset",       Group::Binocular },
    { &GameDllLayout::drawLabyrinthFishEye, nullptr, &GameDllLayout::drawLabyrinthFishEyePrologue,
      sizeof(kSaveRbx10),     "DrawLabyrinthFishEye", Group::Binocular },
    { &GameDllLayout::drawNormalLaserSight, kSaveRbx10, nullptr, sizeof(kSaveRbx10),
      "DrawNormalLaserSight", Group::Scope },
    { &GameDllLayout::doInfraRedQuad,       kSubRsp28,  nullptr, sizeof(kSubRsp28),
      "DoInfraRedQuad",       Group::Tint },
};

constexpr int kStubCount = static_cast<int>(sizeof(kStubs) / sizeof(kStubs[0]));

struct Applied {
    void*   at    = nullptr;   // where the byte was written
    uint8_t saved = 0;         // what was there before
};

Applied              g_applied[kStubCount];
const GameDllLayout* g_boundDll  = nullptr;
uint64_t             g_boundBase = 0;
bool                 g_loggedOnce = false;

bool WantGroup(Group g) {
    switch (g) {
        case Group::Binocular: return Cfg().hideBinocularOverlay;
        case Group::Scope:     return Cfg().hideScopeOverlay;
        case Group::Tint:      return Cfg().hideOpticsTint;
    }
    return false;
}

// Write one byte into the game's code, with the target's identity verified
// first. Returns false without touching anything if the bytes are not what the
// PDB says they should be -- which is the difference between "this build moved
// the function" and "we just corrupted whatever was there".
bool Patch(int i, uint64_t base) {
    const Stub& s = kStubs[i];
    const uint32_t rva = g_boundDll ? (*g_boundDll).*(s.rva) : 0;
    const uint8_t* prologue = s.prologue ? s.prologue
                            : g_boundDll ? (*g_boundDll).*(s.rowPrologue) : nullptr;
    if (rva == 0 || !prologue) return false;

    uint8_t* at = reinterpret_cast<uint8_t*>(base + rva);

    DWORD old = 0;
    if (!VirtualProtect(at, s.prologueLen, PAGE_EXECUTE_READWRITE, &old)) {
        LogF("overlay: VirtualProtect failed for %s", s.name);
        return false;
    }

    bool ok = std::memcmp(at, prologue, s.prologueLen) == 0;
    if (ok) {
        g_applied[i].saved = at[0];
        g_applied[i].at    = at;
        at[0] = kRet;
        FlushInstructionCache(GetCurrentProcess(), at, 1);
    }

    DWORD back = 0;
    VirtualProtect(at, s.prologueLen, old, &back);

    if (!ok) {
        LogF("overlay: %s prologue mismatch at %p -- NOT patched, the overlay "
             "stays. Re-run tools\\verify_addresses.py; the game was patched.",
             s.name, (void*)at);
    }
    return ok;
}

void Unpatch(int i) {
    Applied& a = g_applied[i];
    if (!a.at) return;

    DWORD old = 0;
    if (VirtualProtect(a.at, 1, PAGE_EXECUTE_READWRITE, &old)) {
        *reinterpret_cast<uint8_t*>(a.at) = a.saved;
        FlushInstructionCache(GetCurrentProcess(), a.at, 1);
        DWORD back = 0;
        VirtualProtect(a.at, 1, old, &back);
    }
    a.at = nullptr;
}

void UnpatchAll() {
    for (int i = 0; i < kStubCount; ++i) Unpatch(i);
    g_boundDll  = nullptr;
    g_boundBase = 0;
}

} // namespace

void OverlayUpdate() {
    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();

    const bool any = Cfg().hideBinocularOverlay || Cfg().hideScopeOverlay
                  || Cfg().hideOpticsTint;
    const bool want = Cfg().enabled && any && d && base;

    // Rebind whenever the DLL or its base changes, the same as SkyUpdate: the
    // player can switch between TR4 and TR5 from the title screen without
    // restarting, and a byte written into the old module must come back out
    // before we start writing into the new one.
    if (g_boundDll && (!want || d != g_boundDll || base != g_boundBase)) {
        UnpatchAll();
    }
    if (!want) return;

    g_boundDll  = d;
    g_boundBase = base;

    // Per stub rather than all-or-nothing, so one moved address costs its own
    // overlay and not the other three.
    int done = 0;
    for (int i = 0; i < kStubCount; ++i) {
        const bool on = WantGroup(kStubs[i].group);
        if (on && !g_applied[i].at) {
            if (Patch(i, base)) ++done;
        } else if (!on && g_applied[i].at) {
            Unpatch(i);
        } else if (g_applied[i].at) {
            ++done;
        }
    }

    if (done > 0 && !g_loggedOnce) {
        g_loggedOnce = true;
        LogF("overlay: %d optic overlay draw(s) stubbed in %S (%s) -- binocular=%d "
             "scope=%d tint=%d. The laser dot is a separate sprite and is untouched.",
             done, d->module, d->name,
             Cfg().hideBinocularOverlay, Cfg().hideScopeOverlay,
             Cfg().hideOpticsTint);
    }
}

void OverlayShutdown() {
    UnpatchAll();
    g_loggedOnce = false;
}

} // namespace tr
