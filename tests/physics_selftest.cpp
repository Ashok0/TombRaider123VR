// Exercises the actual port with synthetic game state and a hidden OpenGL context.
#include "../src/DynamicBones.cpp"
#include "../src/BoneSkin.cpp"
#include <fstream>
#include <iterator>
#include <set>
#include <cstdlib>

namespace tr {
Config testConfig;
RenderState testState{};
Shader testShaders[kShaderCount]{};
Layout testLayout{};
GameDllLayout testDll{};
alignas(8) unsigned char testModule[32]{};
alignas(8) unsigned char testItem[512]{};
bool testWorld = true;
int testLevel = 1;
const Config& Cfg() { return testConfig; }
RenderState& VidState() { return testState; }
Shader* Shaders() { return testShaders; }
const Layout& L() { return testLayout; }
uint64_t Base() { return 0; }
const GameDllLayout* GameDllBound() { return &testDll; }
uint64_t GameDllBase() { return reinterpret_cast<uint64_t>(testModule); }
bool IsWorldPass() { return testWorld; }
int32_t AppFlag(uint32_t off) { return off == drva::app_off::level ? testLevel : 0; }
}

int checks = 0;
void Require(bool pass, const char* what) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
void Air(bool air, int fall) {
    uint16_t flags = air ? 8 : 0;
    int16_t speed = static_cast<int16_t>(fall);
    std::memcpy(tr::testItem + 484, &flags, 2);
    std::memcpy(tr::testItem + 36, &speed, 2);
}
void ResetPhysics() {
    using namespace tr;
    ResetSolver();
    testConfig = Config{};
    testDll.laraItem = 8;
    const auto item = reinterpret_cast<uint64_t>(testItem);
    std::memcpy(testModule + 8, &item, 8);
    g_boundDll = &testDll;
    g_boundBase = GameDllBase();
    g_torsoThis = {};
    g_torsoThis.m[0] = g_torsoThis.m[5] = g_torsoThis.m[10] = 1.0f;
    g_haveThis = true;
    Air(false, 0);
}
float LandingPeak(int hz) {
    using namespace tr;
    ResetPhysics();
    const float dt = 1.0f / hz;
    float off[3]{};
    for (int i = 0; i < hz; ++i) Solve(dt);
    Require(DynamicBonesWorldOffset(off) && std::fabs(off[1]) < 0.001f,
            "standing begins at equilibrium without startup bounce");
    for (int i = 0; i < hz; ++i) { Air(true, 70); Solve(dt); }
    Require(g_jumps == 1, "one jump event per airborne spell");
    Air(false, 0);
    float peak = 0;
    for (int i = 0; i < 2 * hz; ++i) {
        Solve(dt);
        DynamicBonesWorldOffset(off);
        peak = std::fmax(peak, off[1]);
        Require(std::isfinite(off[1]) && std::fabs(off[0]) < 0.001f && std::fabs(off[2]) < 0.001f,
                "landing stays finite and world vertical");
    }
    Require(g_lands == 1 && g_lastLandFall == 70, "landing uses deepest fall speed, not zeroed speed");
    Require(peak > 8 && peak < 35, "landing produces a bounded visible bounce");
    Require(std::fabs(off[1]) < 0.1f, "landing decays back to rest");
    g_torsoThis.m[3] += 2000;
    Solve(dt);
    DynamicBonesWorldOffset(off);
    Require(std::fabs(off[1]) < 0.001f, "teleport resets to rest");
    return peak;
}

int main(int argc, char** argv) {
    using namespace tr;
    Require(argc >= 2, "supply the stock/retail/Gold executable paths");
    const float p60 = LandingPeak(60), p90 = LandingPeak(90), p120 = LandingPeak(120);
    Require(std::fabs(p60 - p120) / p120 < 0.2f && std::fabs(p90 - p120) / p120 < 0.2f,
            "landing strength remains comparable at 60/90/120 Hz");
    std::printf("Landing peaks: 60Hz %.3f, 90Hz %.3f, 120Hz %.3f\n", p60, p90, p120);

    ResetPhysics();
    bool air = true; int speed = 0;
    testItem[484] = 4; // status bit, not gravity_status
    Require(ReadLaraAir(air, speed) && !air, "status bits do not masquerade as airborne");
    Air(true, 123);
    Require(ReadLaraAir(air, speed) && air && speed == 123, "TR1-3 item offsets and flag width");
    int16_t angle = 16384;
    std::memcpy(testItem + 102, &angle, 2);
    float forward[3]{};
    Require(DynamicBonesLaraForward(forward) && forward[0] > 0.999f && std::fabs(forward[2]) < 0.001f,
            "TR1-3 facing offset drives front-of-chest selection");

    Joint palette[32]{};
    for (int j = 0; j < 32; ++j) {
        palette[j].m[0] = palette[j].m[5] = palette[j].m[10] = 1;
        palette[j].m[3] = float(j * 40);
    }
    mat4 projection{}; projection.m[11] = -1;
    testState.proj = &projection;
    testState.joints = palette[0].m;
    testState.shader = 0;
    testShaders[0].uid[7] = 1;
    g_inLara = false;
    DynamicBonesObserveDraw();
    Require(!DynamicBonesRenderBody(), "NPC outside Lara scope is rejected");
    g_inLara = true;
    DynamicBonesObserveDraw();
    Require(DynamicBonesRenderBody() && g_numJoints == 15, "only 15 skeleton joints classified from fixed upload");
    for (int j = 0; j < 15; ++j) palette[j].m[3] = 0;
    DynamicBonesObserveDraw();
    Require(!DynamicBonesRenderBody(), "distinct padding cannot make an attachment into a body");
    for (int j = 0; j < 15; ++j) palette[j].m[3] = float(j * 40);
    testWorld = false;
    DynamicBonesObserveDraw();
    Require(!DynamicBonesRenderBody(), "non-world passes are rejected");
    testWorld = true;
    DynamicBonesObserveDraw();
    g_settled = true;
    g_bone[0].x[1] = g_bone[1].x[1] = 12;
    testConfig.dynamicBonesShader = 0;
    const Joint saved = palette[7];
    DynamicBonesApplyToDraw();
    Require(DynamicBonesAppliedToDraw() && palette[7].m[7] != saved.m[7], "rigid fallback applies only to Lara body");
    DynamicBonesRestoreDraw();
    Require(std::memcmp(&saved, &palette[7], sizeof(saved)) == 0 && (testState.consts & kJoints),
            "joint palette restored exactly and dirtied for following draw");
    testConfig.dynamicBonesApply = false;
    DynamicBonesApplyToDraw();
    Require(!DynamicBonesAppliedToDraw(), "Apply=0 prevents rigid edits");
    testConfig = Config{};

    // Compile the exact patched game sources on the installed driver without launching the game.
    HWND window = CreateWindowExW(0, L"STATIC", L"TR physics shader test", WS_POPUP,
                                  0, 0, 8, 8, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "hidden GL test window");
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 24;
    const int format = ChoosePixelFormat(dc, &pfd);
    Require(format && SetPixelFormat(dc, format, &pfd), "OpenGL pixel format");
    HGLRC context = wglCreateContext(dc);
    Require(context && wglMakeCurrent(dc, context) && gl::Load() && gl::LoadedSkinApi(), "OpenGL shader and mesh API");
    std::string onePatched;
    for (int arg = 1; arg < argc; ++arg) {
        std::ifstream input(argv[arg], std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(input)), {});
        Require(!data.empty(), "read real game executable");
        std::set<std::string> sources;
        for (size_t at = 0; (at = data.find(kAnchor, at)) != std::string::npos; ++at) {
            const size_t prev = data.rfind('\0', at), next = data.find('\0', at);
            if (prev == std::string::npos || next == std::string::npos) continue;
            const auto source = data.substr(prev + 1, next - prev - 1);
            if (Recognise(source)) sources.insert(source);
        }
        Require(!sources.empty(), "real TR1-3 skin shader family is recognized");
        for (const auto& source : sources) {
            std::string error;
            onePatched = Patch(source);
            const bool ok = CompilesOk(onePatched, error);
            if (!ok) std::fprintf(stderr, "%s\n", error.c_str());
            Require(ok, "patched game vertex shader compiles on driver");
        }
        std::printf("%s: %zu distinct skin sources compiled\n", argv[arg], sources.size());
    }
    GLuint vertex = gl::CreateShader(GL_VERTEX_SHADER), fragment = gl::CreateShader(GL_FRAGMENT_SHADER);
    const char* vertexText = onePatched.c_str();
    const char* fragmentText = "#version 150\nout vec4 color; void main(){ color=vec4(1.0); }";
    gl::ShaderSource(vertex, 1, &vertexText, nullptr); gl::CompileShader(vertex);
    gl::ShaderSource(fragment, 1, &fragmentText, nullptr); gl::CompileShader(fragment);
    GLuint program = gl::CreateProgram();
    gl::AttachShader(program, vertex); gl::AttachShader(program, fragment); gl::LinkProgram(program);
    GLint linked = 0; gl::GetProgramiv(program, GL_LINK_STATUS, &linked);
    Require(linked != 0, "patched vertex shader links with a fragment stage");
    gl::UseProgram(program);
    testShaders[0].id = program;
    g_patched = 1;
    g_drawRenderBody = true;
    g_regionReady = true; g_fwd = 1; g_bodyCalls = 1;
    g_region = {{-200, -100, 5, 7, 1, 0, 50, 0, 0, 30, 50, 0}};
    Require(RegionWeight(g_region, {0,-150,50}) > 0.99f, "front chest is selected");
    Require(RegionWeight(g_region, {0,-150,-50}) == 0, "back is excluded");
    Require(RegionWeight(g_region, {70,-150,50}) == 0, "shoulder sides are excluded");
    Require(RegionWeight(g_region, {0,-260,50}) == 0, "upper shoulder height is excluded");
    BoneSkinAfterValidate(true);
    float uniform[4]{};
    GLint loc = gl::GetUniformLocation(program, "uDynBone");
    gl::GetUniformfv(program, loc, uniform);
    Require(BoneSkinActive() && uniform[3] == 0, "transition draw does not double-apply rigid and shader motion");
    BoneSkinAfterValidate(false);
    gl::GetUniformfv(program, loc, uniform);
    Require(uniform[3] == 1, "following body draw enables shader deformation");
    g_drawRenderBody = false;
    BoneSkinAfterValidate(false);
    gl::GetUniformfv(program, loc, uniform);
    Require(uniform[3] == 0, "NPC draw clears inherited chest uniform");
    BoneSkinResetRegion();
    Require(!BoneSkinActive() && !g_regionReady && g_measuredBuffers.empty(), "game/level reset drops mesh calibration");
    Require(glGetError() == GL_NO_ERROR, "shader validation leaves no GL error");
    gl::UseProgram(0); gl::DeleteProgram(program); gl::DeleteShader(vertex); gl::DeleteShader(fragment);
    wglMakeCurrent(nullptr, nullptr); wglDeleteContext(context); ReleaseDC(window,dc); DestroyWindow(window);
    g_boundDll = nullptr;
    std::printf("Physics self-test: %d checks passed\n", checks);
    return 0;
}
