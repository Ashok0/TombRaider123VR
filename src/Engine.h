// Engine.h -- Verified layout of tomb123.exe's OpenGL renderer.
//
// Tomb Raider I-III Remastered ships PRIVATE PDBs next to the executable, so
// unlike the TR4-6 work none of this was recovered by signature matching or by
// reading a decompiler. Every RVA below came out of dbghelp's symbol
// enumeration and every struct out of dbghelp's type information, both against
//
//   tomb123.exe  (Tomb Raider I-III Remastered, PE timestamp checked at runtime)
//
// The tools that produced it are in tools\ and are reproducible:
//   python tools\pdbdump.py  PDB\tomb123.exe            -- name -> RVA
//   python tools\typedump.py PDB\tomb123.exe RenderState -- struct layouts
//   python tools\funcsig.py  PDB\tomb123.exe ogl_draw    -- prototypes
//   python tools\disasm.py   PDB\tomb123.exe validate_draw -- annotated asm
//
// So the numbers here are not "verified against a decompiler" -- they are the
// compiler's own symbols. If a future build changes them, re-run pdbdump.py
// rather than re-deriving anything by hand.
//
// Image base is 0x140000000. As in TR4-6 the .data section is enormous and
// mostly zero-filled, so globals at 0x0C9xxxxx are legitimate image-relative
// addresses even though the file on disk is only 1.1 MB.
//
// ---------------------------------------------------------------------------
// HOW TR1-3 DIFFERS FROM TR4-6  (read this before porting anything back)
// ---------------------------------------------------------------------------
//
//   RenderState   152 bytes here, 240 in TR4-6. Fewer members and DIFFERENT
//                 offsets. It has `mesh` where TR4-6 had separate vb/ib, and
//                 it drops shadowFalloff/shadowPos/color/fogPlane/dofPlane/
//                 effectOffsets/LDir/num_joints entirely.
//   Shader        60 bytes with uid[12], versus 80 bytes with uid[18]. There
//                 are 74 shaders here, not 202.
//   ogl_drawVB    DOES NOT EXIST. ogl_draw is the only draw entry point, which
//                 removes an entire duplication path.
//   vid_setPass   Takes THREE arguments (shader, params, cull) -- no `blend`.
//   vid_setPerspOffset / vid_setPerspMatrix / ogl_setPerspAngles
//                 DO NOT EXIST. There is no engine-side frustum-shear lever,
//                 so nothing here can carry a vid_setPerspOffset shear and the
//                 whole preserveProjOffset problem from TR4-6 is absent.
//   ogl_rt        DOES NOT EXIST. TR4-6 had a 24-byte render-target latch to
//                 read; here the only way to know whether the engine is drawing
//                 to the backbuffer is to watch ogl_setRenderTarget, which is
//                 why it is hooked (see Hooks.cpp).
//   TR6 / AER     Not applicable. All three games render straight to the
//                 backbuffer, so per-draw duplication covers everything and the
//                 alternate-eye path TR6 needed is not built.
//
// What is IDENTICAL, and therefore ports unchanged:
//   * mat4 is column-major, uploaded with transpose = GL_FALSE.
//   * mProj is mat4[2]: [0] ortho, [1] perspective, selected into
//     vid_state.proj by vid_setPass exactly as in TR4-6.
//   * The engine projection is Y-flipped (e11 = -1/tanY).
//   * mView_packed is a row-major 3x4 affine uploaded as vec4[4], rotation in
//     1/16384 fixed point, translation raw, third rotation row negated.
//   * validate_draw is the single choke point where uniforms reach the GPU.
//   * FBO_default is a single global that ogl_setRenderTarget binds for the
//     backbuffer case, so redirecting it redirects the whole engine.
#pragma once

#include <cstdint>

namespace tr {

// PDB-reported image base for the build these RVAs came from.
constexpr uint64_t kReferenceImageBase = 0x140000000ull;

// ---------------------------------------------------------------------------
// Function RVAs
// ---------------------------------------------------------------------------
namespace rva {

// --- hook targets -----------------------------------------------------------

// void vid_setPass(int32 shader, float* params, int32 cull)
// 1470 bytes. The pass dispatcher. Decides, per pass, which projection matrix
// is live (mProj[0] ortho vs mProj[1] perspective), the depth/blend/cull state
// and the four texture slots. Installed as app.setPass by vidInit, so the game
// DLLs only ever reach it through the APP vtable.
//
// NOTE the arity: three arguments, not four. TR4-6 passed `blend` separately.
constexpr uint32_t vid_setPass        = 0x0000ABA0;

// void validate_draw(void)
// 2044 bytes. The single choke point where every uniform reaches the GPU.
// Called immediately before glDrawElements by ogl_draw -- its ONLY caller,
// since this build has no ogl_drawVB.
constexpr uint32_t validate_draw      = 0x0000EFB0;

// void ogl_draw(void* mesh, uint32 firstIndex, uint32 count)
// 209 bytes. Binds the mesh, calls validate_draw, then glDrawElements. The
// draw call is issued HERE, not inside validate_draw, which is why per-eye
// duplication has to happen at this level.
constexpr uint32_t ogl_draw           = 0x0000FD30;

// void ogl_present(void)
// 433 bytes. Queries the monitor's refresh rate, calls wglSwapIntervalEXT and
// then SwapBuffers. The engine's own frame boundary -- a better hook than
// wglSwapBuffers because it runs inside the engine's own bookkeeping.
constexpr uint32_t ogl_present        = 0x0000F7D0;

// void fmvShow(void)
// 617 bytes. Draws the current video frame. Installed as app.fmvShow and called
// once per frame while a cutscene plays -- an exact "a video is on screen right
// now" signal, which the uid[0] < 0 shader test only approximates.
constexpr uint32_t fmvShow            = 0x0000E620;

// void ogl_setRenderTarget(int32 colorId, int32 arg1, int32 arg2, int32 arg3)
// 483 bytes. Hooked ONLY to latch whether the engine is currently drawing to
// the backbuffer -- this build has no ogl_rt global to read.
//
// The engine's own test, at RVA 0x0001059A, is `test edi, edi` where edi is the
// sign-extended SECOND argument, branching to the FBO_default bind when it is
// zero and to the FBO_custom path otherwise. Our latch reproduces exactly that
// test rather than guessing at the parameter's meaning.
constexpr uint32_t ogl_setRenderTarget = 0x000104D0;

// --- called or read, not hooked ---------------------------------------------

// void ogl_setPersp(float tanY, int32 width, int32 height, float zNear, float zFar)
// e11 = -1/tanY, e00 = height / (width * tanY). This is what the game calls to
// establish the perspective projection.
constexpr uint32_t ogl_setPersp       = 0x0000FA90;

// void ogl_setOrtho(float, float, float, float, float, float)
constexpr uint32_t ogl_setOrtho       = 0x0000FB50;

// void vid_setOrtho3D(void)
// Copies the ortho matrix into the perspective slot -- the reason a pass can be
// "world slot" by pointer and still orthographic by content. See
// IsOrthoProjection() in StereoMath.h.
constexpr uint32_t vid_setOrtho3D     = 0x0000A590;

// void vid_setViewMatrix(int32* m)  -- 3x4 fixed-point (16384 = 1.0) row-major
constexpr uint32_t vid_setViewMatrix  = 0x0000A5E0;

// void ogl_setViewport(int32 x, int32 y, int32 w, int32 h) -- tail-jmp to glViewport
constexpr uint32_t ogl_setViewport    = 0x0000FC80;
// void ogl_setScissor(int32 x, int32 y, int32 w, int32 h)  -- Y flipped
constexpr uint32_t ogl_setScissor     = 0x0000FC60;

constexpr uint32_t vidInit            = 0x0000B430;
constexpr uint32_t init_ogl           = 0x00011200;
constexpr uint32_t appGetGame         = 0x000084D0;

} // namespace rva

// ---------------------------------------------------------------------------
// Data RVAs
// ---------------------------------------------------------------------------
namespace drva {

// Which game is running: 0 = TR1, 1 = TR2, 2 = TR3.
// Unlike TR4-6 this does NOT select a different render path -- all three draw
// to the backbuffer the same way. It is carried for logging and for per-game
// config only.
constexpr uint32_t gGame           = 0x000EE428;

// _XInputGetState / _XInputSetState: function-pointer globals WinMain fills in
// from GetProcAddress. There is no XInput import to hook -- overwriting these
// is how the Touch controllers are presented to the game as an Xbox pad.
constexpr uint32_t XInputGetState  = 0x00417460;
constexpr uint32_t XInputSetState  = 0x00417490;

constexpr uint32_t vid_state       = 0x0C6A1570;  // RenderState, 152 bytes
constexpr uint32_t vid_state_prev  = 0x0C6A1610;  // shadow copy, redundancy filter
constexpr uint32_t mProj           = 0x0C6A1320;  // mat4[2]: [0]=ortho, [1]=perspective
constexpr uint32_t mView           = 0x0C6A1530;  // column-major view (separate consumer)
constexpr uint32_t mView_packed    = 0x0C6A1700;  // what vid_state.view points at
constexpr uint32_t mShadow         = 0x0C6A1740;  // mat4[1] -- ONE, not TR4-6's six
constexpr uint32_t mContacts       = 0x0C6A13E0;  // float[64], uploaded as vec4[16]
constexpr uint32_t shaders         = 0x0C94CAC0;  // Shader[74]  (4440 bytes / 60)
constexpr uint32_t ogl_textures    = 0x0C94CA50;  // GLuint[19]  (76 bytes)
constexpr uint32_t ogl_samplers    = 0x0C94CAA8;  // GLuint[]
constexpr uint32_t FBO_custom      = 0x0C6A9A48;  // the ONE offscreen FBO
constexpr uint32_t FBO_default     = 0x0C94CA9C;  // latched at init from GL_FRAMEBUFFER_BINDING
constexpr uint32_t texDesc         = 0x000EE430;  // VID_TEXTURE_DESC[19], 28 bytes each
constexpr uint32_t app             = 0x0038D4B0;  // APP, 2800 bytes
constexpr uint32_t gWidth          = 0x0041D280;
constexpr uint32_t gHeight         = 0x0041D27C;
constexpr uint32_t gTargetHeight   = 0x0041D29C;
// As in TR4-6, gTargetWidth sits far away from gTargetHeight even though
// ogl_setRenderTarget writes the pair together (0x000105BC / 0x000105D2).
// Confirmed from both write sites; it is a real location, just an odd one.
constexpr uint32_t gTargetWidth    = 0x0269D2D0;

// Selected APP fields, as byte offsets from `app`. Read-only signals that say
// what the game is doing, which is far cheaper than inferring it from draws.
namespace app_off {
constexpr uint32_t InInv       = 1720;  // inventory / ring is up
constexpr uint32_t InCut       = 1724;  // in a cutscene
constexpr uint32_t InTitle     = 1728;  // title screen
constexpr uint32_t photo_mode  = 884;
constexpr uint32_t dbg_vsync   = 2100;
constexpr uint32_t dbg_no_ui   = 2092;
constexpr uint32_t level       = 832;
} // namespace app_off

} // namespace drva

// ---------------------------------------------------------------------------
// Per-build address table
// ---------------------------------------------------------------------------
//
// Carried per-build rather than derived by a shift, for the same reason as in
// TR4-6: between builds .text and .data do not move by the same amount, or even
// by a constant within a section.
//
// Only one build is listed because only one has been read. To add another:
//   1. Copy its tomb123.exe AND tomb123.pdb into PDB\.
//   2. python tools\pdbdump.py PDB\tomb123.exe > syms.txt
//   3. Pull the same names out of syms.txt and add a Layout row.
//   4. python tools\prologue.py PDB\tomb123.exe vid_setPass validate_draw \
//        ogl_draw ogl_present fmvShow ogl_setRenderTarget
//      and confirm the stolen-byte windows in Hooks.cpp still match.
// An unknown build is REFUSED rather than patched with someone else's numbers.
struct Layout {
    const char* name;
    uint32_t    timestamp;      // PE TimeDateStamp -- the build discriminator

    // hook targets
    uint32_t vid_setPass;
    uint32_t validate_draw;
    uint32_t ogl_draw;
    uint32_t ogl_present;
    uint32_t fmvShow;
    uint32_t ogl_setRenderTarget;

    // globals
    uint32_t gGame;
    uint32_t XInputGetState;
    uint32_t vid_state;
    uint32_t vid_state_prev;
    uint32_t mProj;
    uint32_t mView_packed;
    uint32_t shaders;
    uint32_t ogl_textures;
    uint32_t FBO_custom;
    uint32_t FBO_default;
    uint32_t app;
    uint32_t gWidth;
    uint32_t gHeight;
    uint32_t gTargetWidth;
    uint32_t gTargetHeight;
};

// The build every address above was read out of the PDB for.
//
// PE TimeDateStamp 0x6A4B4928, SizeOfImage 0x0C971000. Confirmed identical
// between the PDB drop in PDB\ and the retail Steam install, so these addresses
// are exact on the shipping executable and not merely on a debug copy.
//
// A build whose timestamp does not match is NOT refused outright -- it is put
// through the structural self-check in IdentifyBuild() and accepted only if the
// relationships between these addresses still hold in the running image. That
// keeps a patch which happens to move nothing from needing a rebuild, while
// still rejecting one that moved things.
constexpr Layout kBuildStock = {
    "TR I-III Remastered (retail, PE 0x6A4B4928)", 0x6A4B4928,
    rva::vid_setPass, rva::validate_draw, rva::ogl_draw,
    rva::ogl_present, rva::fmvShow,      rva::ogl_setRenderTarget,
    drva::gGame,       drva::XInputGetState, drva::vid_state,   drva::vid_state_prev,
    drva::mProj,       drva::mView_packed,   drva::shaders,     drva::ogl_textures,
    drva::FBO_custom,  drva::FBO_default,    drva::app,
    drva::gWidth,      drva::gHeight,        drva::gTargetWidth, drva::gTargetHeight,
};

// ---------------------------------------------------------------------------
// Structures  -- all three taken verbatim from the PDB's own type information
// ---------------------------------------------------------------------------

// Column-major, 16 floats, uploaded straight to GL with transpose = GL_FALSE.
// The PDB names the fields eRC == row R, column C, at float index C*4 + R.
struct mat4 {
    float m[16];

    float&       at(int row, int col)       { return m[col * 4 + row]; }
    const float& at(int row, int col) const { return m[col * 4 + row]; }
};
static_assert(sizeof(mat4) == 64, "mat4 must be 16 floats");

// vid_state.consts dirty-flag bits.
//
// Derived by disassembling validate_draw (0x0000EFB0) and reading, for each
// bit test, which uid[] index and which vid_state member the guarded
// glUniform* call used. The full derivation:
//
//   bit  test site   uid   vid_state member    upload
//   ---  ----------  ----  ------------------  ---------------------------
//    0   0x0000F086  [0]   proj      (+72)     glUniformMatrix4fv(n=1)
//    1   0x0000F0B3  [1]   view      (+80)     glUniform4fv(n=4)
//    2   0x0000F0E5  [2]   shadow    (+88)     glUniformMatrix4fv(n=1)
//   16   0x0000F119  [3]   model     (+96)     glUniform4fv(n=4)
//   17   0x0000F14C  [4]   params    (+104)    glUniform4fv(n=1)
//    3   0x0000F17F  [5]   fogColor  (+112)    glUniform4fv(n=1)
//    4   0x0000F20A  [6]   mContacts (global)  glUniform4fv(n=16)
//   18   0x0000F22E  [7]   joints    (+120)    glUniform4fv(n=96)
//   19   0x0000F253  [8]   LPos      (+128)    glUniform4fv(n=4)
//   20   0x0000F278  [9]   LCol      (+136)    glUniform4fv(n=4)
//   21   0x0000F29D  [10]  ambient   (+144)    glUniform4fv(n=6)
//
// uid[11] is uploaded unconditionally inside the bit-3 block from an
// app-derived scalar, and Shader.cull sits at +52.
//
// Cross-checked against the 0x3F001F mask a shader switch forces (written at
// 0x0000F053), which sets exactly bits {0,1,2,3,4,16,17,18,19,20,21} --
// matching this table entry for entry.
//
// NOTE the differences from TR4-6: kFogColor is bit 3 here (it was bit 5),
// there is no kShadowFalloff/kShadowPos/kFogPlane/kDofPlane/kEffectOffsets/
// kLDir/kColor, and bit 4 is a contacts array that TR4-6 did not upload.
enum ConstBits : uint32_t {
    kProj      = 1u << 0,    // uid[0]
    kView      = 1u << 1,    // uid[1]
    kShadow    = 1u << 2,    // uid[2]
    kFogColor  = 1u << 3,    // uid[5]
    kContacts  = 1u << 4,    // uid[6]
    kModel     = 1u << 16,   // uid[3]
    kParams    = 1u << 17,   // uid[4]
    kJoints    = 1u << 18,   // uid[7]
    kLPos      = 1u << 19,   // uid[8]
    kLCol      = 1u << 20,   // uid[9]
    kAmbient   = 1u << 21,   // uid[10]
    kAllOnShaderSwitch = 0x003F001Fu,
};

// 152 bytes, 21 members. Straight out of the PDB (tools\typedump.py).
// This is the engine's entire "camera": there is no Camera or Frustum object
// anywhere in the binary, exactly as in TR4-6.
struct RenderState {
    int32_t  shader;          //   0
    int32_t  depthTest;       //   4
    int32_t  depthWrite;      //   8
    int32_t  depthSlope;      //  12
    int32_t  blend;           //  16
    int32_t  cull;            //  20
    uint32_t tex[4];          //  24   <- FOUR slots, not TR4-6's seven
    uint32_t smp[4];          //  40
    uint32_t mirrorTex;       //  56
    uint32_t consts;          //  60   <- ConstBits
    void*    mesh;            //  64   <- replaces TR4-6's separate vb / ib
    mat4*    proj;            //  72   <- &mProj[0] (ortho) or &mProj[1] (persp)
    mat4*    view;            //  80   <- &mView_packed
    mat4*    shadow;          //  88
    mat4*    model;           //  96
    float*   params;          // 104
    float*   fogColor;        // 112
    float*   joints;          // 120
    float*   LPos;            // 128
    float*   LCol;            // 136
    float*   ambient;         // 144
};
static_assert(sizeof(RenderState) == 152, "RenderState must be 152 bytes");

// 60 bytes. shaders[0..73].
struct Shader {
    uint32_t id;        // GL program object
    int32_t  uid[12];   // uniform locations; -1 means "not present in this program"
    int32_t  cull;      // +52
    int32_t  fvf;       // +56
};
static_assert(sizeof(Shader) == 60, "Shader must be 60 bytes");

// shaders[] has 74 entries (4440 bytes / 60). vid_setPass assigns
// vid_state.shader BEFORE its own range check, so anything indexing shaders[]
// must clamp first or validate_draw reads out of bounds.
constexpr int kShaderCount = 74;

// ---------------------------------------------------------------------------
// Runtime module binding
// ---------------------------------------------------------------------------

// Resolves the base of tomb123.exe. Returns false if this DLL was loaded into
// something else, or if the host does not pass the structural self-check.
// Must be called before any of the accessors below.
bool Bind();

uint64_t Base();

// The address table for whichever build we actually bound to. Only meaningful
// after a successful Bind().
const Layout& L();

inline void* Fn(uint32_t r)  { return reinterpret_cast<void*>(Base() + r); }
inline void* Var(uint32_t r) { return reinterpret_cast<void*>(Base() + r); }

RenderState&     VidState();
RenderState&     VidStatePrev();

// Address of the engine's _XInputGetState function-pointer global, or nullptr
// if the module is not bound yet.
void*            XInputGetStateSlot();

// 0 = TR1, 1 = TR2, 2 = TR3. Returns -1 if the module is not bound.
int              CurrentGame();
const char*      CurrentGameName();

mat4*            Proj();          // mat4[2]
mat4&            ViewPacked();
Shader*          Shaders();
uint32_t*        OglTextures();
uint32_t&        FboCustom();
uint32_t&        FboDefault();
int32_t&         ScreenWidth();
int32_t&         ScreenHeight();
int32_t&         TargetWidth();
int32_t&         TargetHeight();

// APP flags. Cheap, exact answers to "what is the game doing right now",
// straight out of the struct rather than inferred from the draw stream.
int32_t          AppFlag(uint32_t byteOffset);
inline bool InInventory() { return AppFlag(drva::app_off::InInv)   != 0; }
inline bool InCutscene()  { return AppFlag(drva::app_off::InCut)   != 0; }
inline bool InTitle()     { return AppFlag(drva::app_off::InTitle) != 0; }

// True when vid_state.proj points at mProj[1] -- i.e. the pass currently being
// configured is world-space 3D rather than the 2D/UI ortho layer.
//
// As in TR4-6 this is necessary but NOT sufficient: vid_setOrtho3D copies the
// ortho matrix into mProj[1], so the content still has to be classified with
// IsOrthoProjection(). See Hooks.cpp.
bool IsWorldPass();

// True while the engine is drawing to the backbuffer rather than to its own
// offscreen target. Maintained by the ogl_setRenderTarget hook, because this
// build has no ogl_rt latch to read.
bool TargetIsBackbuffer();
void NoteRenderTarget(int32_t arg1);   // called from the hook

} // namespace tr
