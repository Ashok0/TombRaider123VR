from pathlib import Path
import re, json
root=Path.cwd()
ref=Path('C:/dev/TombRaider456VR')
def read(p): return Path(p).read_text(encoding='utf-8-sig')
def write(p,s): Path(p).write_text(s,encoding='utf-8',newline='\n')
def change(p,a,b):
    s=read(p)
    assert a in s, (str(p),a[:70])
    write(p,s.replace(a,b))
# Preserve all existing layout fields; add the verified hook columns.
change('src/Engine.h','constexpr uint32_t init_ogl', 'constexpr uint32_t shader_init        = 0x0000EC10; // void(Shader*, cull, fvf, vs, fs)\nconstexpr uint32_t init_ogl')
change('src/Engine.h','    uint32_t gTargetHeight;','    uint32_t gTargetHeight;\n    uint32_t shader_init;')
change('src/Engine.h','drva::gTargetWidth, drva::gTargetHeight,','drva::gTargetWidth, drva::gTargetHeight,\n    rva::shader_init,')
s=read('src/Engine.h'); start=s.index('constexpr Layout kBuildAspyrRetail'); end=s.index('};',start)
s=s[:end]+'    /* shader_init         */ 0x0000EC40,\n'+s[end:]; write('src/Engine.h',s)
change('src/GameDll.h','    uint32_t drawCreatureHD;','    uint32_t drawCreatureHD;\n    uint32_t drawLaraHD;        // void(ITEM_INFO*): scope for Lara-only physics')
s=read('src/GameDll.cpp'); vals=iter([0x6D9A0,0x9FC80,0xE9FA0,0x6DDB0,0x9FB40,0xEBCB0])
s=re.sub(r'(          /\* DrawCreatureHD  \*/ 0x[0-9A-Fa-f]+,\n)',lambda m:m[0]+f'          /* DrawLaraHD      */ 0x{next(vals):08X},\n',s); write('src/GameDll.cpp',s)
# Carry every public tuning key and its default from the source feature.
fields=re.findall(r'^    (?:bool|int|float)\s+dynamicBones\w*\s*=.*?;',read(ref/'src/Config.h'),re.M)
change('src/Config.h','    bool  eyeMarkers       = false;', '    bool  eyeMarkers       = false;\n\n    // Chest physics ported from TR4/5. Engine-state drive, vertical landing\n    // response, and per-vertex chest masking use the same tuning defaults.\n'+ '\n'.join('    '+x for x in fields))
r=read(ref/'src/Config.cpp'); a=r.index('    g_cfg.dynamicBones '); b=r.index('\n',r.index('if (g_cfg.dynamicBonesReportFrames < 1)',a))
block=r[a:b]+'\n'
change('src/Config.cpp','    g_liveScale = g_cfg.worldUnitsPerMetre;','    // TR1-3 chest physics: same configuration keys as the TR4/5 implementation.\n'+block+'\n    g_liveScale = g_cfg.worldUnitsPerMetre;')
# Add only the missing GL enums/API rather than replacing this renderer's API.
s=read('src/GL.h'); r=read(ref/'src/GL.h')
for line in r.splitlines():
    if line.startswith('#define ') and line.split()[1] not in s:
        s=s.replace('typedef ptrdiff_t GLsizeiptr_t;',line+'\n\ntypedef ptrdiff_t GLsizeiptr_t;')
a=r.index('// GL 1.5/2.0, used only by BoneSkin.cpp:'); b=r.index('// Resolve everything.',a)
s=s.replace('// Resolve everything.',r[a:b]+'// Resolve everything.'); write('src/GL.h',s)
r=read(ref/'src/GL.cpp'); a=r.index('void (APIENTRY* Uniform4fv)'); b=r.index('namespace { bool g_shaderApi',a)
change('src/GL.cpp','namespace { bool g_shaderApi = false; }',r[a:b]+'namespace { bool g_shaderApi = false; bool g_skinApi = false; }\nbool LoadedSkinApi() { return g_skinApi; }')
a=r.index('    // Dynamic-bone skinning set'); b=r.index('    g_loaded = ok;',a)
change('src/GL.cpp','    g_loaded = ok;',r[a:b]+'    g_loaded = ok;')
# Hook before every return path and restore only after engine uniform upload.
change('src/Hooks.cpp','#include "GameDll.h"','#include "GameDll.h"\n#include "DynamicBones.h"\n#include "BoneSkin.h"')
change('src/Hooks.cpp','void __cdecl Detour_validate_draw() {','''void __cdecl Detour_validate_draw() {
    DynamicBonesObserveDraw();
    DynamicBonesApplyToDraw();
    struct PhysicsDrawGuard {
        ~PhysicsDrawGuard() {
            DynamicBonesRestoreDraw();
            BoneSkinAfterValidate();
        }
    } physicsDrawGuard;
''')
change('src/Hooks.cpp','    GameDllUpdate();','    GameDllUpdate();\n    DynamicBonesUpdate();')
change('src/Hooks.cpp','    Log("hooks: all six installed");','    BoneSkinInstall();\n    Log("hooks: all six installed");')
change('src/Hooks.cpp','void RemoveHooks() {','void RemoveHooks() {\n    DynamicBonesShutdown();\n    BoneSkinShutdown();')
for name in ('DynamicBones','BoneSkin'):
    change('TombRaiderVR.vcxproj','    <ClCompile Include="src\\Config.cpp" />',f'    <ClCompile Include="src\\{name}.cpp" />\n    <ClCompile Include="src\\Config.cpp" />')
    change('TombRaiderVR.vcxproj','    <ClInclude Include="src\\Config.h" />',f'    <ClInclude Include="src\\{name}.h" />\n    <ClInclude Include="src\\Config.h" />')
# TR1/2/3 ITEM_INFO: +36 fallspeed, +88 pos, +484 uint16 gravity flags.
s=read('src/DynamicBones.cpp'); a=s.index('// ITEM_INFO offsets,'); b=s.index('// Game logic ticks',a)
s=s[:a]+'''// Verified against all three PDBs and retail instruction operands.
constexpr uint32_t kItemFallspeed    = 36;
constexpr uint32_t kItemYRot         = 88 + 14;
constexpr uint32_t kItemFlags        = 484; // uint16 bitfield
constexpr uint32_t kGravityStatusBit = 3;

'''+s[b:]
s=s.replace('*reinterpret_cast<uint32_t*>(item + kItemFlags)','*reinterpret_cast<uint16_t*>(item + kItemFlags)')
s=s.replace('constexpr int kMaxJoints = 72;    // renderer ceiling (mBoneMats = 72 * 4x3)', '''constexpr int kMaxJoints = 32;    // fixed GPU upload: 96 vec4s
constexpr int kLaraJoints = 15;   // canonical skeleton, excluding upload padding''')
a=s.index('// TR4 and TR5 both give'); b=s.index('\nvoid ResetSolver()',a)
s=s[:a]+'''// All six supported DLL rows share these three complete push instructions.
const uint8_t kDrawLaraHDPrologue[] = { 0x40, 0x53, 0x41, 0x55, 0x41, 0x57 };

bool Install(const GameDllLayout& d, uint64_t base) {
    if (d.drawLaraHD == 0) return false;
    return g_hDrawLaraHD.Install(
        reinterpret_cast<void*>(base + d.drawLaraHD),
        reinterpret_cast<void*>(&Detour_DrawLaraHD),
        6, kDrawLaraHDPrologue, sizeof(kDrawLaraHDPrologue), "DrawLaraHD");
}
'''+s[b:]
s=s.replace('vs.num_joints','kLaraJoints')
s=s.replace('    if (kLaraJoints <= 0 || !vs.joints) return;', '''    if (!vs.joints || vs.shader < 0 || vs.shader >= kShaderCount
        || Shaders()[vs.shader].uid[7] < 0) return;''')
s=s.replace('    if (!IsWorldPass()) g_drawIsBody = false;', '''    if (!IsWorldPass() || !vs.proj || std::fabs(vs.proj->e33) > 0.5f
        || InInventory() || InTitle()) g_drawIsBody = false;''')
s=s.replace('    ResetSolver();\n}', '    ResetSolver();\n    BoneSkinResetRegion();\n}')
s=s.replace('hooked %S (%s) -- measuring only, nothing is drawn ', 'hooked %S (%s) -- chest physics enabled; draws scoped ')
s=s.replace('"differently", d->module','"to Lara", d->module')
s=s.replace('"dynbones: TR4/TR5 Lara draws with %d joints (renderer ceiling is "\n             "72), so %d slots are free for synthesised bones",\n             g_numJoints, 72 - g_numJoints);', '"dynbones: TR1-3 Lara: %d skeleton joints, 32-slot GPU upload",\n             g_numJoints);')
s=s.replace('    if (!g_haveThis) {\n        g_havePrev    = false;', '''    if (!g_haveThis) {
        // Do not replay a pre-menu landing or use a stale torso after loading.
        g_havePrev = false;
        g_settled = false;
        g_airKnown = false;
        g_fallMax = 0;
        g_lastTick.QuadPart = 0;
        for (auto& bone : g_bone) bone.live = false;
        if (g_lockShader >= 0 && ++g_lockMiss > kLockGraceFrames) {
            g_lockShader = -1;
            g_lockMiss = 0;
        }''')
s=s.replace('    g_patchedAt = m;','    g_patchedAt = m;\n    vs.consts |= kJoints;')
# A restored palette must be re-uploaded before an NPC sharing it draws.
s=s.replace('    g_patchedAt = nullptr;\n}\n\nvoid DynamicBonesShutdown', '    g_patchedAt = nullptr;\n    VidState().consts |= kJoints;\n}\n\nvoid DynamicBonesShutdown')
write('src/DynamicBones.cpp',s)
# Five arguments: cull is the extra int before fvf (confirmed Shader offsets).
s=read('src/BoneSkin.cpp'); a=s.index('// void shader_init('); b=s.index('// Cumulative across',a)
s=s[:a]+'''// TR1-3: void shader_init(Shader*, int cull, int fvf, const char* vs,
//                        const char* fs). The fragment source is argument five.
// Verified via PDB signature and stores to Shader+52/+56 in both exe builds.
typedef void (__fastcall* Fn_shader_init)(Shader* shader, int cull, int fvf,
                                          const char* vs, const char* fs);
// One complete, position-independent mov [rsp+0x10],rbx.
const uint8_t kShaderInitPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };

'''+s[b:]
s=s.replace('Detour_shader_init(Shader* shader, int fvf,','Detour_shader_init(Shader* shader, int cull, int fvf,')
s=s.replace('original(shader, fvf, useVs, fs);','original(shader, cull, fvf, useVs, fs);')
s=s.replace('7, kShaderInitPrologue','5, kShaderInitPrologue')
s=s.replace('vs.shader > 201','vs.shader >= kShaderCount')
s=s.replace('return Cfg().dynamicBonesShader == 1 && g_patched > 0', 'return Cfg().enabled && Cfg().dynamicBones && Cfg().dynamicBonesShader == 1 && g_patched > 0')
s=s.replace('addresses only the stock build\'s PDB provides','a verified shader_init address')
s=s.replace('    const Layout& lay = L();','    if (!Cfg().enabled || !Cfg().dynamicBones || Cfg().dynamicBonesShader != 1) return;\n    const Layout& lay = L();')
s=s.replace('void BoneSkinAfterValidate() {', '''void BoneSkinResetRegion() {
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

void BoneSkinAfterValidate() {''')
write('src/BoneSkin.cpp',s)
change('src/BoneSkin.h','void BoneSkinShutdown();','void BoneSkinShutdown();\n\n// Forget mesh measurements on game/level/context changes.\nvoid BoneSkinResetRegion();')
# Public documentation replaces stale development-history headers from TR4/5.
for name,desc in [('DynamicBones','Lara-only damped spring chest motion for TR1-3.\n// Ported from TR4/5, with TR1-3 item offsets and a fixed 32-slot palette.\n// Only the first 15 skeleton slots participate in body classification.'),('BoneSkin','Per-vertex chest deformation for TR1-3 HD Lara.\n// Patches the three-weight skin shader and fits a front-of-torso region from\n// the live mesh. Back, backpack, and shoulder vertices stay outside the mask.\n// All supported stock, retail, and Gold builds have verified hook addresses.')]:
    p=f'src/{name}.h'; s=read(p); s='// '+name+'.h -- '+desc+'\n'+s[s.index('#pragma once'):]; write(p,s)
# All keys stay under the existing VR section. Same defaults, existing ini preserved at deploy.
keys=[]
for field in fields:
    typ,key,val=re.search(r'(bool|int|float)\s+(dynamicBones\w*)\s*=\s*([^;]+)',field).groups()
    val=val.strip().removesuffix('f')
    if typ=='bool': val='1' if val=='true' else '0'
    keys.append(key[0].upper()+key[1:]+'='+val)
ini='\n; Chest physics (HD Lara, TR1/TR2/TR3; retail and Gold).\n; Same spring and chest-region defaults as the TR4/5 mod.\n; DynamicBones=0 disables all physics. ChestStrength scales the visible bounce.\n; DriveMode=1 responds to landings; 0 uses filtered torso acceleration.\n; Shader=1 moves the chest per vertex; 0 uses the whole-torso debug fallback.\n; Apply=0 keeps measurements only. RegionDebug=30 shows the selected area.\n'+'\n'.join(keys)+'\n'
p='TombRaiderVR.ini'; write(p,read(p)+ini)
# Match the new layout and include hook-window checks on every binary variant.
p='tools/verify_addresses.py'; s=read(p)
s=s.replace("'gHeight', 'gTargetWidth', 'gTargetHeight']", "'gHeight', 'gTargetWidth', 'gTargetHeight', 'shader_init']")
s=s.replace("    check('%s ITEM_INFO::pos' % dll, 88, f.get('pos'))", "    check('%s ITEM_INFO::fallspeed' % dll, 36, f.get('fallspeed'))\n    check('%s ITEM_INFO::gravity_status' % dll, 484, f.get('gravity_status'))\n    check('%s ITEM_INFO::pos' % dll, 88, f.get('pos'))")
s=s.replace("    for dll in ('tomb1.dll', 'tomb2.dll', 'tomb3.dll'):\n        check_prologues", "    check_prologues('tomb123.exe', 'BoneSkin.cpp', {'ShaderInit': 'shader_init'})\n    for dll in ('tomb1.dll', 'tomb2.dll', 'tomb3.dll'):\n        check_prologues(dll, 'DynamicBones.cpp', {'DrawLaraHD': 'DrawLaraHD'})\n        check_prologues",1)
# Insert retail/gold checks immediately after manifests are loaded.
s=s.replace("        # 1. The Engine.h row matches port_build.json.", "        check_prologues('tomb123.exe', 'BoneSkin.cpp', {'ShaderInit': 'shader_init'}, d, P['tomb123.exe'], tag)\n        for dll in ('tomb1.dll', 'tomb2.dll', 'tomb3.dll'):\n            check_prologues(dll, 'DynamicBones.cpp', {'DrawLaraHD': 'DrawLaraHD'}, d, P[dll], tag)\n\n        # 1. The Engine.h row matches port_build.json.")
write(p,s)
print('Physics port integration written')
