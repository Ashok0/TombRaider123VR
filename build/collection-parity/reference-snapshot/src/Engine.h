// Engine.h -- Verified layout of tomb456.exe's OpenGL renderer.
//
// Every address and offset below was read out of the Ghidra database for
//   tomb456.exe  (Tomb Raider IV-VI Remastered v1.0.2a, 2026-01-17, with PDB)
// and re-verified against the decompiler/disassembler before being written
// here. Do not "clean up" a number without re-checking it in the DB.
//
// Image base is 0x140000000. The .data section is enormous:
//   .data  0x140167000 .. 0x14E9BDBDF   (232 MB, mostly zero-filled BSS)
// so globals at 0x14E5xxxxx are legitimate image-relative addresses even
// though the file on disk is only 1.6 MB. Everything here is expressed as an
// RVA and rebased onto the real module base at runtime.
#pragma once

#include <cstdint>

namespace tr {

// Ghidra reported image base for the build these RVAs came from.
constexpr uint64_t kReferenceImageBase = 0x140000000ull;

// ---------------------------------------------------------------------------
// Function RVAs
// ---------------------------------------------------------------------------
namespace rva {

// --- hook targets -----------------------------------------------------------

// void vid_setPass(int shader, float* params, int cull, int blend)
// 2944 bytes. The pass dispatcher: a 202-case switch that decides, per pass,
// which projection matrix is live (mProj[0] ortho vs mProj[1] perspective),
// depth/blend/cull state and the seven texture slots. Installed as app.setPass
// by vidInit; the game DLLs only ever reach it through the app vtable.
constexpr uint32_t vid_setPass        = 0x0000C4C0;

// void validate_draw(void)
// 1967 bytes. The single choke point where every uniform reaches the GPU.
// Called immediately before glDrawElements by ogl_draw and ogl_drawVB.
constexpr uint32_t validate_draw      = 0x00011CC0;

// void ogl_draw(void* vb, uint firstIndex, uint count, int strip)
// void ogl_drawVB(int fvf, void* vb, void* ib, int stride, uint first,
//                 uint count, int strip)
// The only two callers of validate_draw. Each does:
//     ...bind vao/vbo...  validate_draw();  glDrawElements(...);
// which is why duplicating a draw per eye has to happen here, not inside
// validate_draw -- the draw call itself is issued by the caller.
constexpr uint32_t ogl_draw           = 0x00012BD0;
constexpr uint32_t ogl_drawVB         = 0x00012CF0;

// void ogl_present(void)
// 83 bytes. wglSwapIntervalEXT(app.dbg_vsync); SwapBuffers(hDC); clears the
// vb/ib cache; toggles GL_FRAMEBUFFER_SRGB (0x8F9D) on game == 2.
// The engine's own frame boundary -- a better hook than wglSwapBuffers.
constexpr uint32_t ogl_present        = 0x00012600;

// void fmvShow(void)
// Draws the current video frame. Installed as app.fmvShow and called once per
// frame while a cutscene plays -- an exact "a video is on screen right now"
// signal, which the uid[0] < 0 shader test only approximates. TR6's scene
// composite uses a clip-space-direct shader too, so the shader test alone
// cannot tell its gameplay from its cutscenes.
constexpr uint32_t fmvShow            = 0x00011350;

// --- called, not hooked -----------------------------------------------------

// void vid_setPerspOffset(float x, float y)
// Writes mProj[1].e02/.e12 (the frustum shear terms) and sets consts |= 1.
// This is the engine's own asymmetric-frustum lever, already exposed as API.
constexpr uint32_t vid_setPerspOffset = 0x0000B8D0;

// void ogl_setPerspAngles(float tanX, float tanY, float zNear, float zFar)
// Builds a symmetric frustum into mProj[1]. Note e11 = -1/tanY: the engine
// projection is Y-flipped relative to a textbook GL frustum.
// NOT hooked, and never called on this build: the game reaches the perspective
// through ogl_setPersp instead. Verified by hooking it and logging -- zero calls.
constexpr uint32_t ogl_setPerspAngles = 0x00012840;

// void ogl_getPerspAngles(float tanX, float tanY, float zNear, float zFar,
//                         mat4* out)
// Misnamed by the PDB: it does not GET anything, it BUILDS a projection matrix
// into the caller's own buffer. init_ogl installs it in the app vtable, so the
// game DLL calls into the engine to construct a projection for its own use --
// deriving cull planes being the plausible reason. NOT hooked, and never called
// either; verified the same way.
constexpr uint32_t ogl_getPerspAngles = 0x000127D0;

// void ogl_setPersp(float tanY, int width, int height, float zNear, float zFar)
// e11 = -1/tanY, e00 = height / (width * tanY). THIS is what the game calls.
//
// Widening it does NOT affect culling. Measured: hooking this and scaling tanY
// by 2 widened the projection from 64.4 to 103.1 degrees and produced no extra
// geometry at all, so the DLL builds its cull planes independently of the
// engine's projection. Fixing the geometry that goes missing when you look away
// from the game camera means finding the frustum/portal test inside
// tomb4/5/6.dll. Do not re-run the widening experiment; it is settled.
constexpr uint32_t ogl_setPersp       = 0x000128E0;

// void vid_setPerspMatrix(const float m[16])
// The game hands over a complete projection. Not called on this build.
constexpr uint32_t vid_setPerspMatrix = 0x0000B920;

// void vid_setViewMatrix(int* m)  -- 3x4 fixed-point (16384 = 1.0) row-major
constexpr uint32_t vid_setViewMatrix  = 0x0000B960;

// void ogl_setViewport(int x, int y, int w, int h)  -- tail-jmp to glViewport
constexpr uint32_t ogl_setViewport    = 0x00012AE0;
// void ogl_setScissor(int x, int y, int w, int h)
// -> glScissor(x, gTargetHeight - y - h, w, h)   (Y is flipped here too)
constexpr uint32_t ogl_setScissor     = 0x00012AC0;

// void ogl_setRenderTarget(int colorId, int depthId, int colorIndex,
//                          int depthIndex, int colorMip, int depthMip)
constexpr uint32_t ogl_setRenderTarget = 0x00013C30;

constexpr uint32_t vidInit            = 0x0000D750;
constexpr uint32_t init_ogl           = 0x00015BA0;

// void shader_init(Shader* shader, int fvf, const char* vs, const char* fs)
// 1175 bytes. Builds ONE program per call -- glCreateProgram into shader->id,
// fvf into shader->fvf at +0x4C, then compiles vs and fs and links. Hooked by
// BoneSkin.cpp, which substitutes the vertex source argument for the TR4/TR5
// skinning shaders. The signature was read from the body, not assumed; see
// BoneSkin.cpp for what assuming it cost.
constexpr uint32_t shader_init        = 0x00011820;

} // namespace rva

// ---------------------------------------------------------------------------
// Data RVAs
// ---------------------------------------------------------------------------
namespace drva {

// _XInputGetState / _XInputSetState: function-pointer globals WinMain fills in
// from GetProcAddress. There is no XInput import to hook -- overwriting these is
// how we present the Touch controllers to the game as an Xbox pad.
// Which game is running: 0 = TR4, 1 = TR5, 2 = TR6 (Angel of Darkness).
// TR6 has a different render path -- it draws the whole scene into custom
// offscreen targets and composites -- so some of our handling is per-game.
constexpr uint32_t gGame           = 0x0017938C;

constexpr uint32_t XInputGetState  = 0x00692858;
constexpr uint32_t XInputSetState  = 0x006928B0;

constexpr uint32_t vid_state       = 0x0E51C900;  // RenderState, 240 bytes
constexpr uint32_t vid_state_prev  = 0x0E51CA00;  // shadow copy, redundancy filter
constexpr uint32_t mProj           = 0x0E51C6F0;  // mat4[2]: [0]=ortho, [1]=perspective
constexpr uint32_t mView           = 0x0E51C8C0;  // rotation-only view (separate consumer)
constexpr uint32_t mView_packed    = 0x0E51CB30;  // what vid_state.view points at
constexpr uint32_t mShadow         = 0x0E51CB70;  // mat4[6]
constexpr uint32_t mInverseCulling = 0x0E5186C8;  // int, set by vid_setInverseCullingFlag
constexpr uint32_t shaders         = 0x0E9AE040;  // Shader[202]
constexpr uint32_t ogl_textures    = 0x0E9ADF30;  // GLuint[]
constexpr uint32_t FBO_custom      = 0x0E9ADF28;  // the ONE offscreen FBO
constexpr uint32_t FBO_default     = 0x0E9ADF2C;  // latched at init from GL_FRAMEBUFFER_BINDING
constexpr uint32_t ogl_rt          = 0x0E9AE000;  // 24-byte render-target latch
constexpr uint32_t app             = 0x005833E0;  // the renderer/game API struct
constexpr uint32_t gWidth          = 0x00698664;
constexpr uint32_t gHeight         = 0x00698660;
constexpr uint32_t gTargetHeight   = 0x00698678;
// NOTE: gTargetWidth sits 44 MB away from gTargetHeight at RVA 0x3298680 even
// though ogl_setRenderTarget writes the pair together. Confirmed by xrefs from
// both appGetWidth and ogl_setRenderTarget, so it is a real location, just an
// odd one. Treat with suspicion if you ever write to it.
constexpr uint32_t gTargetWidth    = 0x03298680;

namespace app_off {
constexpr uint32_t level           = 1264;
constexpr uint32_t photoMode       = 1308; // APP::photo_mode, shared TR4/5 host
constexpr uint32_t InventoryActive = 2148;
constexpr uint32_t InFMV           = 2152;
constexpr uint32_t InTitle         = 2156;
// APP_CONFIG starts at 2528. Its first bitfield word is +4: bit 0 selects the
// HD renderer and bit 1 selects camera-relative (modern) controls.
constexpr uint32_t cfgFlags        = 2532;
} // namespace app_off


} // namespace drva

// ---------------------------------------------------------------------------
// Per-build address tables
// ---------------------------------------------------------------------------
//
// Several builds of the exe are in circulation and their addresses do NOT
// differ by a constant. Between the stock build and the HD pack, .text moved by
// +688..+816 depending on the function while .data moved by +0x2020, +0x2190,
// +0x21A0 or +0x21B0 depending on which variable you ask about. So every
// address is carried per-build rather than derived by a shift.
//
// How a table is produced (repeat this for any future build):
//   functions -- an exact byte signature from the reference build, slid past
//                any leading RIP-relative displacement, matched uniquely in the
//                new .text. All six hook prologues are then confirmed
//                byte-identical, so the stolen bytes need no relocation.
//   globals   -- for every RIP-relative reference to a global in the reference
//                .text, the referencing instruction's surroundings are matched
//                in the new .text with ALL displacements in the window
//                wildcarded, and the new displacement read back. Accepted only
//                on agreement between independent reference sites.
//
// Then every structural relationship is re-checked: gWidth/gHeight 4 apart,
// FBO_default/FBO_custom 4, ogl_textures/FBO_custom 8, vid_state_prev 256 above
// vid_state, shaders 64 above ogl_rt, mView_packed 560 above vid_state,
// gTargetHeight 24 above gHeight. The one that legitimately differs is mProj,
// which sits 512 below vid_state in the HD builds rather than 528 -- confirmed
// by 31 references across mProj[0], mProj[1] and mProj+0x80 all agreeing on
// +0x21B0. Do not "correct" mProj back into line with its neighbours.
struct Layout {
    const char* name;
    uint32_t    timestamp;      // PE TimeDateStamp -- the build discriminator

    // hook targets
    uint32_t vid_setPass;
    uint32_t validate_draw;
    uint32_t ogl_draw;
    uint32_t ogl_drawVB;
    uint32_t ogl_present;
    uint32_t fmvShow;

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
    uint32_t ogl_rt;
    uint32_t gWidth;
    uint32_t gHeight;
    uint32_t gTargetWidth;
    uint32_t gTargetHeight;
    uint32_t app;

    // Dynamic-bone shader patching (BoneSkin.cpp). LAST on purpose: a row that
    // initialises positionally and stops short of it comes out 0, which
    // BoneSkin reads as "not available on this build".
    uint32_t shader_init;
};

// The build every address above was read out of Ghidra for.
constexpr Layout kBuildStock = {
    "v1.0.2a stock (2026-01-17)", 0x696B49A7,
    rva::vid_setPass, rva::validate_draw, rva::ogl_draw,
    rva::ogl_drawVB,  rva::ogl_present,   rva::fmvShow,
    drva::gGame,       drva::XInputGetState, drva::vid_state,   drva::vid_state_prev,
    drva::mProj,       drva::mView_packed,   drva::shaders,     drva::ogl_textures,
    drva::FBO_custom,  drva::FBO_default,    drva::ogl_rt,
    drva::gWidth,      drva::gHeight,        drva::gTargetWidth, drva::gTargetHeight,
    drva::app, rva::shader_init,
};

// Every non-debug tomb456.exe in circulation. The two timestamps are the
// 2025-07-01 build -- which is what the HD Definitive Patch ships -- and the
// 2025-09-10 one, which is the RETAIL Steam exe and also came with HD pack
// v1.0.2. They are separate rows because the PE timestamp is the discriminator,
// not because their addresses differ: all 23 below were derived independently
// against each binary (tools\port_addresses.py) and came out the same.
//
// The two builds are in fact the same code. Their .text, .data, .pdata and
// .reloc sections are byte-identical; the only differences anywhere are the PE
// debug-directory timestamps, the PDB age, and four bytes in an .rdata table
// the renderer's texture init reads. Both embed the same 315 GLSL sources as
// the stock build, so BoneSkin's shader patch applies unchanged to both.
#define TR_HD_ADDRS                                                     \
    0x0000C770, 0x00011FF0, 0x00012EF0,                                 \
    0x00013010, 0x00012920, 0x00011680,                                 \
    0x0017B3AC, 0x006949F8, 0x0E51EAA0, 0x0E51EBA0,                     \
    0x0E51E8A0, 0x0E51ECD0, 0x0E9B01D0, 0x0E9B00C0,                     \
    0x0E9B00B8, 0x0E9B00BC, 0x0E9B0190,                                 \
    0x0069A804, 0x0069A800, 0x0329A820, 0x0069A818,                     \
    0x00585580, 0x00011B50

constexpr Layout kBuildHD1 = {
    "HD Definitive Patch / community HD pack (2025-07-01)", 0x68639C21,
    TR_HD_ADDRS,
};

constexpr Layout kBuildHD2 = {
    "retail / community HD pack v1.0.2 (2025-09-10)", 0x68C12FEB,
    TR_HD_ADDRS,
};

#undef TR_HD_ADDRS

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------

// Column-major, 16 floats, uploaded straight to GL with transpose = GL_FALSE.
// Field naming follows the PDB: eRC == row R, column C, at float index C*4 + R.
struct mat4 {
    float m[16];

    float&       at(int row, int col)       { return m[col * 4 + row]; }
    const float& at(int row, int col) const { return m[col * 4 + row]; }
};
static_assert(sizeof(mat4) == 64, "mat4 must be 16 floats");

// vid_state.consts dirty-flag bits.
//
// Derived directly from validate_draw's upload block, NOT from the earlier
// hand-written notes (which had `joints` wrong). Cross-checked against the
// 0xff07bf mask that a shader switch forces, which sets exactly bits
// {0,1,2,3,4,5,7,8,9,10,16..23} -- matching this table.
enum ConstBits : uint32_t {
    kProj          = 1u << 0,   // uid[0]  glUniformMatrix4fv(loc, 1, GL_FALSE, proj)
    kView          = 1u << 1,   // uid[1]  glUniform4fv(loc, 4, view)   <-- not Matrix4fv
    kShadow        = 1u << 2,   // uid[2]  glUniformMatrix4fv(loc, 6, GL_FALSE, shadow)
    kShadowFalloff = 1u << 3,   // uid[3]  glUniform4fv(loc, 6, ...)
    kShadowPos     = 1u << 4,   // uid[4]  glUniform4fv(loc, 6, ...)
    kFogColor      = 1u << 5,   // uid[8]
    kFogPlane      = 1u << 8,   // uid[15]
    kDofPlane      = 1u << 9,   // uid[16]
    kEffectOffsets = 1u << 10,  // uid[17] glUniform4fv(loc, 3, ...)
    kModel         = 1u << 16,  // uid[5]  glUniform4fv(loc, 4, model)  <-- not Matrix4fv
    kParams        = 1u << 17,  // uid[6]
    kJoints        = 1u << 18,  // uid[10] glUniform4fv(loc, num_joints*3, ...)
    kLPos          = 1u << 19,  // uid[11]
    kLCol          = 1u << 20,  // uid[12]
    kLDir          = 1u << 21,  // uid[13]
    kAmbient       = 1u << 22,  // uid[14]
    kColor         = 1u << 23,  // uid[7]
    kAllOnShaderSwitch = 0x00ff07bfu,
};

// 240 bytes, 29 members. This is the engine's entire "camera": there is no
// Camera or Frustum object anywhere in the binary.
struct RenderState {
    void*    vb;              // 0
    void*    ib;              // 8
    int32_t  shader;          // 16
    int32_t  depthTest;       // 20
    int32_t  depthWrite;      // 24
    int32_t  depthSlope;      // 28
    int32_t  blend;           // 32
    int32_t  cull;            // 36
    uint32_t tex[7];          // 40
    uint32_t smp[7];          // 68
    uint32_t consts;          // 96   <- ConstBits
    int32_t  num_joints;      // 100
    mat4*    proj;            // 104  <- &mProj[0] (ortho) or &mProj[1] (persp)
    mat4*    view;            // 112  <- &mView_packed
    mat4*    shadow;          // 120
    float*   shadowFalloff;   // 128
    float*   shadowPos;       // 136
    mat4*    model;           // 144
    float*   params;          // 152
    float*   color;           // 160
    float*   fogColor;        // 168
    float*   fogPlane;        // 176
    float*   dofPlane;        // 184
    float*   effectOffsets;   // 192
    float*   joints;          // 200
    float*   LPos;            // 208
    float*   LCol;            // 216
    float*   LDir;            // 224
    float*   ambient;         // 232
};
static_assert(sizeof(RenderState) == 240, "RenderState must be 240 bytes");

// 80 bytes. shaders[0..201].
struct Shader {
    uint32_t id;        // GL program object
    int32_t  uid[18];   // uniform locations; -1 means "not present in this program"
    int32_t  fvf;       // vertex format
};
static_assert(sizeof(Shader) == 80, "Shader must be 80 bytes");

// 24 bytes. Written only by ogl_setRenderTarget, read only by ogl_texCopy.
// It is a single latch, not a stack -- nested target changes do not restore.
struct OglRenderTarget {
    int32_t color_id;      // 0
    int32_t color_mip;     // 4
    int32_t color_index;   // 8   <- array layer
    int32_t depth_id;      // 12
    int32_t depth_mip;     // 16
    int32_t depth_index;   // 20
};
static_assert(sizeof(OglRenderTarget) == 24, "OglRenderTarget must be 24 bytes");

// ---------------------------------------------------------------------------
// Runtime module binding
// ---------------------------------------------------------------------------

// Resolves the base of tomb456.exe. Returns false if this DLL was loaded into
// something else. Must be called before any of the accessors below.
bool Bind();

uint64_t Base();

// The address table for whichever build we actually bound to. Only meaningful
// after a successful Bind(); before that it returns the stock table so a stray
// early call reads plausible numbers rather than nulls.
const Layout& L();

inline void* Fn(uint32_t r)  { return reinterpret_cast<void*>(Base() + r); }
inline void* Var(uint32_t r) { return reinterpret_cast<void*>(Base() + r); }

RenderState&     VidState();
RenderState&     VidStatePrev();

// Address of the engine's _XInputGetState function-pointer global, or nullptr
// if the module is not bound yet.
void*            XInputGetStateSlot();

// 0 = TR4, 1 = TR5, 2 = TR6. Returns -1 if the module is not bound.
int              CurrentGame();
inline int32_t AppState(uint32_t offset) {
    return *reinterpret_cast<const int32_t*>(Base() + L().app + offset);
}
inline bool InInventory() { return AppState(drva::app_off::InventoryActive) != 0; }
inline bool InFMV()       { return AppState(drva::app_off::InFMV) != 0; }
inline bool InTitle()     { return AppState(drva::app_off::InTitle) != 0; }
inline bool NewControls() { return (AppState(drva::app_off::cfgFlags) & 2) != 0; }
mat4*            Proj();          // mat4[2]
mat4&            ViewPacked();
Shader*          Shaders();
uint32_t*        OglTextures();
uint32_t&        FboCustom();
uint32_t&        FboDefault();
OglRenderTarget& Rt();
int32_t&         ScreenWidth();
int32_t&         ScreenHeight();
int32_t&         TargetWidth();
int32_t&         TargetHeight();

// True when vid_state.proj points at mProj[1] -- i.e. the pass currently being
// configured is world-space 3D rather than the 2D/UI ortho layer.
bool IsWorldPass();

} // namespace tr
