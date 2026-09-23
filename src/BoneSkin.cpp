#include "BoneSkin.h"
#include "DynamicBones.h"
#include "Engine.h"
#include "GL.h"
#include "Config.h"
#include "InlineHook.h"
#include "Log.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tr {
namespace {

// --- the shader_init hook ----------------------------------------------------

hook::InlineHook g_hShaderInit;

// TR1-3: void shader_init(Shader*, int cull, int fvf, const char* vs,
//                        const char* fs). The fragment source is argument five.
// Verified via PDB signature and stores to Shader+52/+56 in both exe builds.
typedef void (__fastcall* Fn_shader_init)(Shader* shader, int cull, int fvf,
                                          const char* vs, const char* fs);
// One complete, position-independent mov [rsp+0x10],rbx.
const uint8_t kShaderInitPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };

// Cumulative across every shader_init call. shader_init builds one program per
// call, so these must never be reset inside the detour.
int  g_matched  = 0;   // recognised as the index+weight skinning family
int  g_patched  = 0;   // patched and passed the test compile
int  g_rejected = 0;   // patched but failed to compile -- original sent instead
bool g_summaryLogged = false;

// --- the patch ---------------------------------------------------------------
//
// TR1-3's three-weight skin sources use one model-space aCoord, joint indices
// in aLight.xyz, and weights in aColor.xyz. Match source structure instead of
// hard-coding TR4/5's program indices. The physics self-test compiles all eight
// distinct matching sources in each supported TR1-3 executable.
// Two-matrix shaders use per-joint coordinates and must not receive this mask.
// Patch after the third weighted contribution, before projection/fog outputs.
const char kAnchor[] = "p.z += dot(uJoints[index[2] + 2], coord) * weight;";

const char* const kRequired[] = {
    "vec4 coord = vec4(aCoord, 1.0);",
    "vec4 j = aLight;",
    "vec4 w = aColor;",
    "ivec3 index = ivec3(j.xyz);",
    "gl_Position",
    "void main()",
};

// uDynRegion layout, kept in step with Region below:
//   [0] = chest top Y, chest bottom Y, height fade, torso joint index
//   [1] = forward sign, depth midline Z, depth ramp, unused
//   [2] = lateral centre X, lateral fade start, lateral fade end, unused
//
// All in bind-pose MODEL space, the space aCoord is in. +Y is DOWN in TR1-3,
// so "top" is the smaller Y. Every smoothstep keeps its first edge strictly
// below its second, which GLSL leaves undefined otherwise.
const char kDecl[] =
    "uniform vec4 uDynBone;\n"
    "uniform vec4 uDynRegion[3];\n"
    "float dynWeight(vec3 p) {\n"
    "    float soft = max(uDynRegion[0].z, 1.0);\n"
    "    float wh = smoothstep(uDynRegion[0].x - soft, uDynRegion[0].x + soft, p.y)\n"
    "             * (1.0 - smoothstep(uDynRegion[0].y - soft, uDynRegion[0].y + soft, p.y));\n"
    "    float wd = smoothstep(0.0, max(uDynRegion[1].z, 1.0),\n"
    "                          uDynRegion[1].x * (p.z - uDynRegion[1].y));\n"
    "    float wl = 1.0 - smoothstep(uDynRegion[2].y,\n"
    "                                max(uDynRegion[2].z, uDynRegion[2].y + 1.0),\n"
    "                                abs(p.x - uDynRegion[2].x));\n"
    "    return wh * wd * wl;\n"
    "}\n";

// The vertex follows only as far as it belongs to TORSO: its weights on joints
// other than TORSO do not count. A shoulder vertex split with an upper arm
// therefore moves partway at most, which is what keeps the armpit from tearing.
const char kBody[] =
    "p.z += dot(uJoints[index[2] + 2], coord) * weight;\n"
    "\tif (uDynBone.w > 0.5) {\n"
    "\t\tivec3 dynIdx = ivec3(j.xyz);\n"
    "\t\tint dynJ = int(uDynRegion[0].w + 0.5);\n"
    "\t\tfloat dynW = 0.0;\n"
    "\t\tif (dynIdx.x == dynJ) dynW += w.x;\n"
    "\t\tif (dynIdx.y == dynJ) dynW += w.y;\n"
    "\t\tif (dynIdx.z == dynJ) dynW += w.z;\n"
    "\t\tp.xyz += uDynBone.xyz * (dynW * dynWeight(coord.xyz));\n"
    "\t}\n";

size_t CountOf(const std::string& s, const char* needle) {
    size_t n = 0, at = 0;
    const size_t len = std::strlen(needle);
    while ((at = s.find(needle, at)) != std::string::npos) { ++n; at += len; }
    return n;
}

bool Recognise(const std::string& src) {
    if (CountOf(src, kAnchor) != 1) return false;
    if (src.find("index[3]") != std::string::npos) return false;
    for (const char* need : kRequired)
        if (CountOf(src, need) != 1) return false;
    return true;
}

std::string Patch(const std::string& src) {
    std::string out = src;
    const size_t at = out.find(kAnchor);
    out.replace(at, sizeof(kAnchor) - 1, kBody);
    out.insert(out.find("void main()"), kDecl);
    return out;
}

// Compile a patched source in a throwaway shader object, so a mistake in the
// patch is caught here and the engine never receives source that will not
// build. Runs inside shader_init, where the engine's context is current.
bool CompilesOk(const std::string& src, std::string& err) {
    if (!gl::LoadedShaderApi()) { err = "shader API not loaded"; return false; }
    GLuint sh = gl::CreateShader(GL_VERTEX_SHADER);
    if (!sh) { err = "glCreateShader returned 0"; return false; }
    const char* p = src.c_str();
    gl::ShaderSource(sh, 1, &p, nullptr);
    gl::CompileShader(sh);
    GLint ok = 0;
    gl::GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        gl::GetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            std::string log(static_cast<size_t>(len), '\0');
            gl::GetShaderInfoLog(sh, len, nullptr, &log[0]);
            err = log.c_str();
        } else {
            err = "(no info log)";
        }
    }
    gl::DeleteShader(sh);
    return ok != 0;
}

void InvalidateProgram(uint32_t program);   // with the per-program table below

void __fastcall Detour_shader_init(Shader* shader, int cull, int fvf,
                                   const char* vs, const char* fs) {
    const auto original = g_hShaderInit.Original<Fn_shader_init>();

    const Config& c = Cfg();
    const bool want = c.enabled && c.dynamicBones && c.dynamicBonesShader == 1;

    // Not ours to change: pass every argument through untouched. The source is
    // only ever REPLACED as an argument -- nothing global is swapped, so there
    // is no engine state to restore and nothing to leak if this path is left
    // early.
    if (shader == Shaders()) {
        g_matched = g_patched = g_rejected = 0;
        g_summaryLogged = false;
        BoneSkinResetRegion();
    }
    std::string patched;
    const char* useVs = vs;
    if (want && vs) {
        const std::string src(vs);
        if (Recognise(src)) {
            ++g_matched;
            // The engine's context is current here -- it is about to compile --
            // so the entry points the test compile needs can be resolved now.
            gl::Load();
            std::string candidate = Patch(src);
            std::string err;
            if (CompilesOk(candidate, err)) {
                patched = std::move(candidate);
                useVs = patched.c_str();   // alive until original() returns
                ++g_patched;
            } else {
                ++g_rejected;
                if (g_rejected == 1) {
                    LogF("boneskin: a patched skin shader failed to compile, so "
                         "the ORIGINAL was used for it and that pass will not "
                         "deform. Compiler said: %s", err.c_str());
                }
            }
        }
    }

    original(shader, cull, fvf, useVs, fs);

    // shader_init just gave this Shader a fresh program. If the name is being
    // reused -- a context reset rebuilds the programs -- any uniform locations
    // cached under it belong to the old program.
    if (shader) InvalidateProgram(shader->id);
}

// --- per-program uniform state ------------------------------------------------

// GL program names are small integers; a flat table beats a map on a path that
// runs every draw.
constexpr uint32_t kMaxProgram = 4096;

struct ProgramState {
    GLint locBone   = -2;     // -2 = not looked up yet, -1 = not one of ours
    GLint locRegion = -2;
    bool  live      = false;  // uDynBone.w currently uploaded as 1
};
ProgramState g_prog[kMaxProgram];

void InvalidateProgram(uint32_t program) {
    if (program < kMaxProgram) g_prog[program] = ProgramState{};
}

// --- the chest region ---------------------------------------------------------

struct Region { float v[12]; };   // uDynRegion[3], see kDecl for the layout
Region g_region = {};
bool   g_regionReady = false;

// Measured torso-local points, accumulated across every body buffer read.
struct TorsoPoint { float x, y, z; };
std::vector<TorsoPoint> g_torso;
std::vector<GLuint>     g_measuredBuffers;
bool   g_layoutWarned = false;

// Forward sign: +1 if the torso's local +Z points the way Lara faces.
int    g_fwd        = 0;      // 0 = not decided
float  g_fwdEvidence = 0.0f;
int    g_fwdSamples  = 0;

struct Attr {
    GLint  loc = -1;
    GLint  size = 0;
    GLenum type = 0;
    bool   norm = false;
    GLint  stride = 0;
    GLuint buf = 0;
    size_t offset = 0;
    size_t elemBytes = 0;
};

size_t TypeBytes(GLenum t) {
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:                  return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT:    return 4;
    default:                                              return 0;
    }
}

bool QueryAttr(GLuint prog, const char* name, Attr& a) {
    a.loc = gl::GetAttribLocation(prog, name);
    if (a.loc < 0) return false;
    GLint v = 0;
    gl::GetVertexAttribiv(a.loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &v);
    if (!v) return false;
    gl::GetVertexAttribiv(a.loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
    gl::GetVertexAttribiv(a.loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &v);
    a.type = static_cast<GLenum>(v);
    gl::GetVertexAttribiv(a.loc, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &v);
    a.norm = v != 0;
    gl::GetVertexAttribiv(a.loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
    gl::GetVertexAttribiv(a.loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &v);
    a.buf = static_cast<GLuint>(v);
    void* p = nullptr;
    gl::GetVertexAttribPointerv(a.loc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &p);
    a.offset = static_cast<size_t>(reinterpret_cast<uintptr_t>(p));
    const size_t tb = TypeBytes(a.type);
    a.elemBytes = tb * static_cast<size_t>(a.size);
    if (a.stride == 0) a.stride = static_cast<GLint>(a.elemBytes);
    return a.buf != 0 && a.size > 0 && tb != 0;
}

float HalfToFloat(uint16_t h) {
    const int s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float f;
    if (e == 0)       f = std::ldexp(static_cast<float>(m), -24);   // subnormal
    else if (e == 31) f = m ? NAN : INFINITY;
    else              f = std::ldexp(static_cast<float>(m + 1024), e - 25);
    return s ? -f : f;
}

float Component(const uint8_t* p, const Attr& a, int i) {
    const uint8_t* q = p + static_cast<size_t>(i) * TypeBytes(a.type);
    switch (a.type) {
    case GL_FLOAT:          { float v;    std::memcpy(&v, q, 4); return v; }
    case GL_HALF_FLOAT:     { uint16_t v; std::memcpy(&v, q, 2); return HalfToFloat(v); }
    case GL_SHORT:          { int16_t v;  std::memcpy(&v, q, 2);
                              return a.norm ? std::fmax(v / 32767.0f, -1.0f) : v; }
    case GL_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, q, 2);
                              return a.norm ? v / 65535.0f : v; }
    case GL_BYTE:           { int8_t v = static_cast<int8_t>(*q);
                              return a.norm ? std::fmax(v / 127.0f, -1.0f) : v; }
    case GL_UNSIGNED_BYTE:  { return a.norm ? *q / 255.0f : *q; }
    case GL_INT:            { int32_t v;  std::memcpy(&v, q, 4); return static_cast<float>(v); }
    case GL_UNSIGNED_INT:   { uint32_t v; std::memcpy(&v, q, 4); return static_cast<float>(v); }
    default:                return 0.0f;
    }
}

// Read one body buffer and add its TORSO vertices to g_torso. Runs once per
// distinct buffer, so an outfit change measures the new mesh as it appears.
void MeasureBuffer(GLuint prog) {
    // The index+weight layout: one bind-pose position (aCoord), joint indices
    // in aLight.xyz, weights in aColor.xyz. Read back exactly as the shader
    // reads them, including each attribute's own type and normalisation, so a
    // byte-packed weight comes out 0..1 here just as it does on the GPU.
    Attr coord, joints, weights;
    const bool ok = QueryAttr(prog, "aCoord",  coord)
                 && QueryAttr(prog, "aLight",  joints)
                 && QueryAttr(prog, "aColor",  weights)
                 && coord.size >= 3 && joints.size >= 3 && weights.size >= 3;
    if (!ok) {
        if (!g_layoutWarned) {
            g_layoutWarned = true;
            Log("boneskin: could not read the body draw's vertex layout, so the "
                "chest region cannot be measured and the chest will not move");
        }
        return;
    }

    for (GLuint b : g_measuredBuffers) if (b == coord.buf) return;
    g_measuredBuffers.push_back(coord.buf);

    // Every attribute must live in the same buffer for the single read below.
    if (joints.buf != coord.buf || weights.buf != coord.buf) {
        LogF("boneskin: body attributes are split across buffers (%u %u %u); "
             "not measuring this one", coord.buf, joints.buf, weights.buf);
        return;
    }

    GLint prevArray = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
    gl::BindBuffer(GL_ARRAY_BUFFER, coord.buf);
    GLint bytes = 0;
    gl::GetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &bytes);
    std::vector<uint8_t> data;
    if (bytes > 0) {
        data.resize(static_cast<size_t>(bytes));
        gl::GetBufferSubData(GL_ARRAY_BUFFER, 0, bytes, data.data());
    }
    gl::BindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArray));
    if (data.empty()) {
        LogF("boneskin: buffer %u read back empty", coord.buf);
        return;
    }

    const int joint = Cfg().dynamicBonesTorsoJoint;
    const size_t stride = static_cast<size_t>(coord.stride);
    size_t before = g_torso.size(), vertices = 0;

    for (size_t i = 0;; ++i) {
        const Attr* all[3] = { &coord, &joints, &weights };
        bool inside = true;
        for (const Attr* a : all) {
            if (static_cast<size_t>(a->stride) != stride
                || a->offset + i * stride + a->elemBytes > data.size()) {
                inside = false;
                break;
            }
        }
        if (!inside) break;
        ++vertices;

        const uint8_t* base = data.data();
        const uint8_t* jp = base + joints.offset  + i * stride;
        const uint8_t* wp = base + weights.offset + i * stride;

        // Same test the shader applies: the vertex's total weight on TORSO.
        float torsoWeight = 0.0f;
        for (int k = 0; k < 3; ++k) {
            if (static_cast<int>(std::lround(Component(jp, joints, k))) == joint)
                torsoWeight += Component(wp, weights, k);
        }
        if (torsoWeight < 0.5f) continue;

        const uint8_t* cp = base + coord.offset + i * stride;
        g_torso.push_back({ Component(cp, coord, 0),
                            Component(cp, coord, 1),
                            Component(cp, coord, 2) });
    }

    LogF("boneskin: measured buffer %u -- %zu vertices, %zu mainly on joint %d "
         "(coord 0x%04X x%d, joints 0x%04X x%d norm %d, weights 0x%04X x%d "
         "norm %d, stride %zu)", coord.buf, vertices, g_torso.size() - before,
         joint, coord.type, coord.size, joints.type, joints.size,
         joints.norm ? 1 : 0, weights.type, weights.size, weights.norm ? 1 : 0,
         stride);
}

// Decide which way is front by comparing the torso's local +Z, carried into
// world by the joint matrix, with Lara's facing from lara_item. Evidence is
// accumulated and only trusted once it is consistent, because she can twist
// her torso against her facing while aiming or looking around.
void UpdateForward() {
    const int forced = Cfg().dynamicBonesForwardSign;
    if (forced == 1 || forced == -1) { g_fwd = forced; return; }
    if (g_fwd != 0) return;

    float m[12], f[3];
    if (!DynamicBonesTorsoFrame(m) || !DynamicBonesLaraForward(f)) return;
    const float d = m[2] * f[0] + m[6] * f[1] + m[10] * f[2];
    if (std::fabs(d) < 0.5f) return;
    g_fwdEvidence += d;
    if (++g_fwdSamples >= 30) {
        g_fwd = (g_fwdEvidence >= 0.0f) ? 1 : -1;
        LogF("boneskin: front of the torso is local %sZ (%d samples, mean "
             "alignment %.2f with Lara's facing)", g_fwd > 0 ? "+" : "-",
             g_fwdSamples, g_fwdEvidence / g_fwdSamples);
    }
}

void LogShape(float xmin, float xmax, float ymin, float ymax,
              float fmin, float fmax) {
    constexpr int kCols = 24, kRows = 14;
    int grid[kRows][kCols] = {};
    for (const TorsoPoint& p : g_torso) {
        const float f = static_cast<float>(g_fwd) * p.z;
        int c = static_cast<int>((f - fmin) / (fmax - fmin + 1e-3f) * kCols);
        int r = static_cast<int>((p.y - ymin) / (ymax - ymin + 1e-3f) * kRows);
        if (c >= 0 && c < kCols && r >= 0 && r < kRows) ++grid[r][c];
    }
    int peak = 1;
    for (auto& row : grid) for (int v : row) if (v > peak) peak = v;

    const char ramp[] = " .:-=+*#%@";
    LogF("boneskin: torso shape, side view -- BACK on the left, FRONT on the "
         "right, top row is the neck end. x %.0f..%.0f  y %.0f..%.0f  "
         "depth %.0f..%.0f", xmin, xmax, ymin, ymax, fmin, fmax);
    for (int r = 0; r < kRows; ++r) {
        char line[kCols + 1];
        for (int c = 0; c < kCols; ++c) {
            const int v = grid[r][c];
            const int k = v ? 1 + (v * 8) / peak : 0;
            line[c] = ramp[k > 9 ? 9 : k];
        }
        line[kCols] = 0;
        LogF("boneskin:   |%s|", line);
    }
}

// The same weight the shader computes, on the CPU, so the fit can report how
// much of the measured mesh it actually selects rather than leaving that to be
// judged by eye.
float Smooth(float e0, float e1, float x) {
    const float t = std::fmin(std::fmax((x - e0) / (e1 - e0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float RegionWeight(const Region& R, const TorsoPoint& p) {
    const float soft = std::fmax(R.v[2], 1.0f);
    const float wh = Smooth(R.v[0] - soft, R.v[0] + soft, p.y)
                   * (1.0f - Smooth(R.v[1] - soft, R.v[1] + soft, p.y));
    const float wd = Smooth(0.0f, std::fmax(R.v[6], 1.0f), R.v[4] * (p.z - R.v[5]));
    const float wl = 1.0f - Smooth(R.v[9], std::fmax(R.v[10], R.v[9] + 1.0f),
                                   std::fabs(p.x - R.v[8]));
    return wh * wd * wl;
}

void FitRegion() {
    g_regionReady = false;
    if (g_fwd == 0 || g_torso.size() < 200) return;

    float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
    float fmin = 1e30f, fmax = -1e30f;
    const float fwd = static_cast<float>(g_fwd);
    for (const TorsoPoint& p : g_torso) {
        xmin = std::fmin(xmin, p.x); xmax = std::fmax(xmax, p.x);
        ymin = std::fmin(ymin, p.y); ymax = std::fmax(ymax, p.y);
        fmin = std::fmin(fmin, fwd * p.z); fmax = std::fmax(fmax, fwd * p.z);
    }
    const float W = xmax - xmin, H = ymax - ymin;
    if (W < 10.0f || H < 10.0f) return;

    const Config& c = Cfg();
    const float xc = 0.5f * (xmin + xmax);
    const float latIn = c.dynamicBonesChestWidth * W;

    // FIND THE BUST FROM THE MESH'S FRONT PROFILE.
    //
    // The first fit placed the height band at fixed fractions of the torso,
    // 0.18..0.52 from the top, and they were wrong for this mesh: the torso
    // mesh starts well above the shoulders, so the bust sits lower than those
    // fractions assume. The measured front profile peaked at 75 units forward
    // three to five rows BELOW the band, which left the most prominent part of
    // the bust on the band's fade-out edge -- the right area moving, but only
    // weakly. That is the "very subtle" that was reported.
    //
    // So: slice the torso by height and take how far forward each slice
    // reaches near the centre line. The bust is a bump in that profile -- the
    // front rises from the collarbone to a peak and falls back to the belly
    // line below. The band is every slice around the peak that stays within
    // half the bump's height of it. On the logged mesh that is 0.43..0.79 of
    // torso height, excluding the upper chest above and the belly below.
    constexpr int kSlices = 40;
    float raw[kSlices];
    for (float& f : raw) f = -1e30f;
    for (const TorsoPoint& p : g_torso) {
        if (std::fabs(p.x - xc) > 0.35f * W) continue;
        int s = static_cast<int>((p.y - ymin) / H * kSlices);
        if (s < 0) s = 0;
        if (s >= kSlices) s = kSlices - 1;
        raw[s] = std::fmax(raw[s], fwd * p.z);
    }
    // One slice of neighbour smoothing, so a sparsely populated slice does not
    // read as a notch and cut the band in half.
    float front[kSlices];
    for (int s = 0; s < kSlices; ++s) {
        float v = raw[s];
        if (s > 0)           v = std::fmax(v, raw[s - 1]);
        if (s < kSlices - 1) v = std::fmax(v, raw[s + 1]);
        front[s] = v;
    }

    const int searchEnd = static_cast<int>(0.85f * kSlices);   // skip the hips
    int peak = -1;
    for (int s = 0; s < searchEnd; ++s)
        if (front[s] > -1e29f && (peak < 0 || front[s] > front[peak])) peak = s;

    float belly = 0.0f;
    int bellyN = 0;
    for (int s = static_cast<int>(0.75f * kSlices); s < kSlices; ++s)
        if (front[s] > -1e29f) { belly += front[s]; ++bellyN; }
    if (bellyN) belly /= static_cast<float>(bellyN);

    float yTop, yBot, apex;
    const char* how;
    const float bump = (peak >= 0 && bellyN) ? front[peak] - belly : 0.0f;
    if (peak >= 0 && bump >= 0.06f * H) {
        apex = front[peak];
        const float thresh = apex - c.dynamicBonesChestBand * bump;
        int top = peak, bot = peak;
        while (top > 0 && front[top - 1] >= thresh) --top;
        while (bot < kSlices - 1 && front[bot + 1] >= thresh) ++bot;
        yTop = ymin + H * static_cast<float>(top) / kSlices;
        yBot = ymin + H * static_cast<float>(bot + 1) / kSlices;
        // A little past the bump either way, so the moving area covers the
        // whole breast rather than stopping exactly where the profile does.
        yTop -= 0.03f * H;
        yBot += 0.03f * H;
        // Never a sliver, whatever the profile does.
        const float minH = 0.12f * H;
        if (yBot - yTop < minH) {
            const float mid = 0.5f * (yTop + yBot);
            yTop = mid - 0.5f * minH;
            yBot = mid + 0.5f * minH;
        }
        how = "measured bust";
    } else {
        // No distinct bump -- an outfit that flattens the front. Fall back to
        // the configured fractions rather than to nothing.
        yTop = ymin + c.dynamicBonesChestTop * H;
        yBot = ymin + c.dynamicBonesChestBottom * H;
        apex = -1e30f;
        for (const TorsoPoint& p : g_torso)
            if (p.y >= yTop && p.y <= yBot && std::fabs(p.x - xc) <= latIn)
                apex = std::fmax(apex, fwd * p.z);
        if (apex < -1e29f) return;
        how = "no distinct bust in the profile, configured fractions";
    }

    // A torso is about 0.62 times as deep as it is wide. The weight is zero at
    // the resulting midline and full a third of that depth further forward,
    // so the back half of the torso -- and anything behind it -- stays put.
    const float core = 0.62f * W;
    const float midF = apex - c.dynamicBonesChestDepth * core;

    Region& R = g_region;
    R.v[0] = yTop;            R.v[1] = yBot;        R.v[2] = 0.08f * H;
    R.v[3] = static_cast<float>(c.dynamicBonesTorsoJoint);
    R.v[4] = fwd;             R.v[5] = fwd * midF;  R.v[6] = 0.30f * core;  R.v[7] = 0.0f;
    R.v[8] = xc;              R.v[9] = latIn;       R.v[10] = latIn + 0.14f * W;
    R.v[11] = 0.0f;
    g_regionReady = true;

    LogShape(xmin, xmax, ymin, ymax, fmin, fmax);

    // The profile the band came from, every second slice, top to bottom.
    char prof[256];
    int n = 0;
    for (int s = 0; s < kSlices && n < static_cast<int>(sizeof(prof)) - 8; s += 2)
        n += _snprintf_s(prof + n, sizeof(prof) - n, _TRUNCATE, "%s%.0f",
                         s ? " " : "", front[s] > -1e29f ? front[s] : 0.0f);
    LogF("boneskin: front profile, neck to waist: %s", prof);

    // What the region actually selects on this mesh.
    size_t full = 0, partial = 0;
    float yLo = 1e30f, yHi = -1e30f;
    for (const TorsoPoint& p : g_torso) {
        const float w = RegionWeight(R, p);
        if (w >= 0.9f) {
            ++full;
            yLo = std::fmin(yLo, p.y); yHi = std::fmax(yHi, p.y);
        } else if (w > 0.1f) {
            ++partial;
        }
    }
    LogF("boneskin: chest region (%s) -- height %.0f..%.0f of %.0f..%.0f, depth "
         "from %.0f forward (front %.0f, belly line %.0f), within %.0f of centre. "
         "Selects %zu torso vertices at full weight (y %.0f..%.0f) and %zu partly.",
         how, yTop, yBot, ymin, ymax, midF, apex, belly, latIn,
         full, yLo, yHi, partial);
}

// Vertex-attribute queries force a CPU/GPU sync on threaded drivers, so the
// buffer is not re-examined on every body draw. Before the region exists it is
// tried every 120 body draws; once it exists, every 3600, which is enough to
// pick up a new outfit's buffer without costing a frame anywhere near play.
unsigned g_bodyCalls = 0;

bool EnsureRegion(GLuint prog) {
    const int hadFwd = g_fwd;
    UpdateForward();   // no GL, cheap every call

    const unsigned every = g_regionReady ? 3600u : 120u;
    const bool measure = (g_bodyCalls++ % every) == 0;
    const size_t had = g_torso.size();
    if (measure) MeasureBuffer(prog);

    if (!g_regionReady || g_torso.size() != had || g_fwd != hadFwd) FitRegion();
    return g_regionReady;
}

} // namespace

// ---------------------------------------------------------------------------

void BoneSkinInstall() {
    if (!Cfg().enabled || !Cfg().dynamicBones || Cfg().dynamicBonesShader != 1) return;
    const Layout& lay = L();
    if (lay.shader_init == 0) {
        LogF("boneskin: not available on %s -- the per-vertex chest path needs "
             "a verified shader_init address", lay.name);
        return;
    }
    if (!g_hShaderInit.Install(Fn(lay.shader_init),
                               reinterpret_cast<void*>(&Detour_shader_init),
                               5, kShaderInitPrologue, sizeof(kShaderInitPrologue),
                               "shader_init")) {
        Log("boneskin: shader_init could not be hooked -- the rigid joint-7 "
            "view stays in use");
    }
}

void BoneSkinShutdown() {
    g_hShaderInit.Remove();
}

namespace {

// Patched programs exist and the GL entry points resolved. Says nothing about
// whether Lara is actually drawn with one of them -- which is the distinction
// the first version missed.
bool PathPossible() {
    return Cfg().enabled && Cfg().dynamicBones && Cfg().dynamicBonesShader == 1 && g_patched > 0 && gl::LoadedSkinApi();
}

// Set once a Lara body draw has gone through a patched program with a fitted
// region. Until then the rigid joint-7 view keeps running, so a shader path
// that never engages degrades to the old behaviour instead of to nothing.
bool g_bodyOnPatched       = false;
bool g_warnedUnpatchedBody = false;

} // namespace

bool BoneSkinActive() {
    return PathPossible() && g_bodyOnPatched;
}

void BoneSkinResetRegion() {
    g_torso.clear();
    g_measuredBuffers.clear();
    g_regionReady = false;
    g_region = {};
    g_bodyCalls = 0;
    g_fwd = 0;
    g_fwdEvidence = 0.0f;
    g_fwdSamples = 0;
    g_bodyOnPatched = false;
    g_warnedUnpatchedBody = false;
    g_layoutWarned = false;
    // Keep per-program live flags: the next non-body draw must clear uniforms.
}

void BoneSkinAfterValidate(bool jointApplied) {
    // One summary, on the first draw after the programs are built. shader_init
    // runs once per program, so a line per call would be 74 of them.
    if (!g_summaryLogged && Cfg().dynamicBonesShader == 1) {
        g_summaryLogged = true;
        LogF("boneskin: %d skin program(s) matched, %d patched, %d fell back to "
             "the original%s", g_matched, g_patched, g_rejected,
             g_matched == 0 ? " -- none matched, so the whole-torso view stays "
                              "in use" : "");
    }
    if (!PathPossible()) return;

    const RenderState& vs = VidState();
    if (vs.shader < 0 || vs.shader >= kShaderCount) return;
    const uint32_t prog = Shaders()[vs.shader].id;
    if (prog == 0 || prog >= kMaxProgram) return;

    ProgramState& ps = g_prog[prog];
    if (ps.locBone == -2) {
        ps.locBone   = gl::GetUniformLocation(prog, "uDynBone");
        ps.locRegion = gl::GetUniformLocation(prog, "uDynRegion");
    }

    // DynamicBonesApply is the one switch for "change what is drawn", and it
    // has to govern this path as well as the rigid one.
    const bool body = Cfg().dynamicBonesApply && DynamicBonesRenderBody();

    if (ps.locBone < 0) {
        // The failure that went unreported the first time round: her body
        // drawn with a program the patch never touched.
        if (body && !g_warnedUnpatchedBody) {
            g_warnedUnpatchedBody = true;
            LogF("boneskin: Lara's body is drawn with shader %d (program %u), "
                 "which is not patched -- the whole-torso view stays in use",
                 vs.shader, prog);
        }
        return;
    }

    float off[3];
    if (body && DynamicBonesWorldOffset(off) && EnsureRegion(prog)) {
        // Moving only the chest reads as far less motion than moving the whole
        // upper body by the same distance, because so much less of her moves.
        // This scales the chest path alone; the whole-torso view keeps the
        // amplitude that was tuned for it.
        const float strength = Cfg().dynamicBonesChestStrength;
        off[0] *= strength; off[1] *= strength; off[2] *= strength;

        // Reveal the region: a fixed push straight out of her front, so what
        // is selected can be judged standing still.
        const float dbg = Cfg().dynamicBonesRegionDebug;
        if (dbg != 0.0f) {
            float m[12];
            if (DynamicBonesTorsoFrame(m)) {
                const float s = dbg * static_cast<float>(g_fwd);
                off[0] += m[2] * s; off[1] += m[6] * s; off[2] += m[10] * s;
            }
        }
        // The warm-up draw may already have the rigid offset uploaded. Start
        // shader deformation on the next draw to avoid applying both at once.
        const float bone[4] = { off[0], off[1], off[2], jointApplied ? 0.0f : 1.0f };
        gl::Uniform4fv(ps.locBone, 1, bone);
        if (ps.locRegion >= 0) gl::Uniform4fv(ps.locRegion, 3, g_region.v);
        ps.live = true;
        if (!g_bodyOnPatched) {
            g_bodyOnPatched = true;
            LogF("boneskin: live -- Lara's body (shader %d) now deforms per "
                 "vertex, and the whole-torso view has stood down", vs.shader);
        }
    } else if (ps.live) {
        // Uniforms persist per program, and NPCs share these shaders. Leaving
        // the last value in place would bounce whoever draws next.
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        gl::Uniform4fv(ps.locBone, 1, zero);
        ps.live = false;
    }
}

} // namespace tr
