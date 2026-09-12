#include "Engine.h"
#include "Log.h"

#include <windows.h>

namespace tr {
namespace {

uint64_t      g_base   = 0;
const Layout* g_layout = nullptr;

// Every build there is an address table for.
const Layout* const kBuilds[] = { &kBuildStock };

// Set by the ogl_setRenderTarget hook. Starts true because the engine's very
// first frames render to the backbuffer before it ever calls setRenderTarget,
// and defaulting to "offscreen" there would suppress injection on exactly the
// frames LazyInit is trying to validate.
bool g_backbuffer = true;

// Does the running image actually look like the build these RVAs describe?
//
// The point of this is to catch a MOVED layout, not to be a checksum. Each test
// below is a relationship that the compiler's own output fixes and that a
// relaid-out build would break, so it is cheap and it is falsifiable:
//
//   * vid_state_prev sits exactly 160 bytes above vid_state. That is
//     sizeof(RenderState) rounded up to the next 32 -- an allocation artefact
//     of these two being declared together, and the first thing to change if
//     the struct changes.
//   * mView_packed sits 400 bytes above vid_state, mProj 592 below it.
//   * gWidth and gHeight are adjacent, gTargetHeight 32 above gHeight.
//   * FBO_default and ogl_textures are both inside the 0x0C94xxxx block, and
//     ogl_textures is 76 bytes -- 19 GLuints -- below FBO_default.
//   * Every RVA has to be inside SizeOfImage, or we would be writing into
//     unmapped memory.
//
// A build that passes all of this and is still wrong is possible in principle.
// It is far less likely than a build that fails one of them, which is the case
// worth defending against.
bool StructuralCheckPasses(uint64_t base, const Layout& b, uint32_t sizeOfImage) {
    struct Test { const char* what; bool ok; };
    const Test tests[] = {
        { "vid_state_prev == vid_state + 160",
          b.vid_state_prev == b.vid_state + 160 },
        { "mView_packed == vid_state + 400",
          b.mView_packed == b.vid_state + 400 },
        { "mProj == vid_state - 592",
          b.mProj + 592 == b.vid_state },
        { "gWidth == gHeight + 4",
          b.gWidth == b.gHeight + 4 },
        { "gTargetHeight == gHeight + 32",
          b.gTargetHeight == b.gHeight + 32 },
        { "ogl_textures == FBO_default - 76",
          b.ogl_textures + 76 == b.FBO_default },
        { "all RVAs inside SizeOfImage",
          b.shaders     < sizeOfImage && b.vid_state  < sizeOfImage &&
          b.FBO_default < sizeOfImage && b.mProj      < sizeOfImage &&
          b.gTargetWidth < sizeOfImage && b.app       < sizeOfImage &&
          b.ogl_setRenderTarget < sizeOfImage },
    };

    bool all = true;
    for (const Test& t : tests) {
        if (!t.ok) {
            LogF("engine: structural check FAILED -- %s", t.what);
            all = false;
        }
    }
    if (!all) return false;

    // One live read, as a final sanity check that we are pointed at real data
    // rather than at a zero-filled hole: the shader array's entries are GL
    // program objects, which are small positive integers, and the game has
    // certainly created shader 0 by the time anything calls us. This runs
    // before any hook is installed, so it is only ever a read.
    const Shader* sh = reinterpret_cast<const Shader*>(base + b.shaders);
    if (IsBadReadPtr(sh, sizeof(Shader) * 4)) {
        Log("engine: structural check FAILED -- shaders[] is not readable");
        return false;
    }
    return true;
}

// Identify the host binary and pick its address table.
//
// Matching on the PE TimeDateStamp rather than a file name means a renamed or
// copied exe is still recognised. Unlike the TR4-6 mod this does NOT refuse an
// unknown stamp outright -- it falls back to the structural check, because
// there is only one address table here and a Steam patch that relinks without
// moving anything is a likely and harmless case.
const Layout* IdentifyBuild(uint64_t base) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return nullptr;

    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    const uint32_t size  = nt->OptionalHeader.SizeOfImage;

    for (const Layout* b : kBuilds) {
        if (b->timestamp == stamp) {
            LogF("engine: build matched by PE timestamp 0x%08X", stamp);
            if (!StructuralCheckPasses(base, *b, size)) return nullptr;
            return b;
        }
    }

    LogF("engine: unrecognised build (PE timestamp 0x%08X, SizeOfImage 0x%08X). "
         "Known builds:", stamp, size);
    for (const Layout* b : kBuilds)
        LogF("engine:   0x%08X  %s", b->timestamp, b->name);
    Log("engine: falling back to the structural self-check against the stock table");

    if (!StructuralCheckPasses(base, kBuildStock, size)) {
        Log("engine: the structural check did not pass, so the stock addresses do "
            "NOT describe this executable. Refusing to patch.");
        Log("engine: to add this build, drop its tomb123.exe and tomb123.pdb into "
            "PDB\\ and re-run tools\\pdbdump.py -- see Engine.h.");
        return nullptr;
    }
    Log("engine: structural check passed; proceeding with the stock addresses");
    return &kBuildStock;
}

} // namespace

bool Bind() {
    if (g_base) return true;

    // The name is only a fast path; GetModuleHandle(nullptr) -- the host
    // executable whatever it is called -- is the reliable answer, and the build
    // is identified from its PE header below rather than its name.
    HMODULE h = GetModuleHandleW(L"tomb123.exe");
    if (!h) h = GetModuleHandleW(nullptr);
    if (!h) {
        Log("engine: GetModuleHandle failed");
        return false;
    }

    const uint64_t base   = reinterpret_cast<uint64_t>(h);
    const Layout*  layout = IdentifyBuild(base);
    if (!layout) {
        Log("engine: host module is not a supported build; refusing to patch");
        return false;
    }

    g_base   = base;
    g_layout = layout;
    LogF("engine: bound to %p (reference base %p, slide %+lld)",
         reinterpret_cast<void*>(base),
         reinterpret_cast<void*>(kReferenceImageBase),
         static_cast<long long>(base - kReferenceImageBase));
    LogF("engine: build identified as %s", layout->name);
    return true;
}

uint64_t Base() { return g_base; }

const Layout& L() { return g_layout ? *g_layout : kBuildStock; }

RenderState& VidState()     { return *reinterpret_cast<RenderState*>(Var(L().vid_state)); }
RenderState& VidStatePrev() { return *reinterpret_cast<RenderState*>(Var(L().vid_state_prev)); }
mat4*        Proj()         { return  reinterpret_cast<mat4*>(Var(L().mProj)); }
mat4&        ViewPacked()   { return *reinterpret_cast<mat4*>(Var(L().mView_packed)); }
Shader*      Shaders()      { return  reinterpret_cast<Shader*>(Var(L().shaders)); }
uint32_t*    OglTextures()  { return  reinterpret_cast<uint32_t*>(Var(L().ogl_textures)); }
uint32_t&    FboCustom()    { return *reinterpret_cast<uint32_t*>(Var(L().FBO_custom)); }
uint32_t&    FboDefault()   { return *reinterpret_cast<uint32_t*>(Var(L().FBO_default)); }
int32_t&     ScreenWidth()  { return *reinterpret_cast<int32_t*>(Var(L().gWidth)); }
int32_t&     ScreenHeight() { return *reinterpret_cast<int32_t*>(Var(L().gHeight)); }
int32_t&     TargetWidth()  { return *reinterpret_cast<int32_t*>(Var(L().gTargetWidth)); }
int32_t&     TargetHeight() { return *reinterpret_cast<int32_t*>(Var(L().gTargetHeight)); }

int32_t AppFlag(uint32_t byteOffset) {
    if (!g_base) return 0;
    return *reinterpret_cast<int32_t*>(Var(L().app + byteOffset));
}

bool IsWorldPass() {
    return VidState().proj == &Proj()[1];
}

// The engine's own backbuffer test, reproduced. ogl_setRenderTarget branches on
// its second argument alone (`test edi, edi` at RVA 0x0001059A, where edi is the
// sign-extended arg1): zero binds FBO_default, anything else binds FBO_custom.
void NoteRenderTarget(int32_t arg1) {
    g_backbuffer = (arg1 == 0);
}

bool TargetIsBackbuffer() { return g_backbuffer; }

int CurrentGame() {
    if (!g_base) return -1;
    return *reinterpret_cast<int32_t*>(Var(L().gGame));
}

const char* CurrentGameName() {
    switch (CurrentGame()) {
    case 0:  return "Tomb Raider I";
    case 1:  return "Tomb Raider II";
    case 2:  return "Tomb Raider III";
    default: return "unknown";
    }
}

void* XInputGetStateSlot() {
    if (!g_base) return nullptr;
    return reinterpret_cast<void*>(Var(L().XInputGetState));
}

} // namespace tr
