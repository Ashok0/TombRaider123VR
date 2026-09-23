from pathlib import Path
import re

def edit(p, fn):
    p=Path(p); p.write_text(fn(p.read_text()),encoding='utf-8',newline='\n')
edit('src/Config.h',lambda s:re.sub(r'^        ((?:bool|int|float)\s+dynamicBones)',r'    \1',s,flags=re.M))
edit('src/Config.cpp',lambda s:s.replace('#include <cwchar>','#include <cwchar>\n#include <cmath>'))
fields=re.findall(r'float\s+(dynamicBones\w*)\s*=',Path('src/Config.h').read_text())
checks='    const Config physicsDefaults;\n'+'\n'.join(f'    if (!std::isfinite(g_cfg.{f})) g_cfg.{f} = physicsDefaults.{f};' for f in fields)+'\n'
for f in ['Stiffness','Teleport','Separation']:
    checks+=f'    if (g_cfg.dynamicBones{f} <= 0.0f) g_cfg.dynamicBones{f} = physicsDefaults.dynamicBones{f};\n'
for f in ['Damping','MaxDisplace','DriveMax','ChestStrength']:
    checks+=f'    if (g_cfg.dynamicBones{f} < 0.0f) g_cfg.dynamicBones{f} = physicsDefaults.dynamicBones{f};\n'
checks+='    if (g_cfg.dynamicBonesTorsoJoint < 0 || g_cfg.dynamicBonesTorsoJoint >= 15) g_cfg.dynamicBonesTorsoJoint = 7;\n'
edit('src/Config.cpp',lambda s:s.replace('    // A zero or negative interval',checks+'\n    // A zero or negative interval'))
# Remember whether this GPU upload already contains the rigid fallback.
edit('src/DynamicBones.h',lambda s:s.replace('void DynamicBonesRestoreDraw();','void DynamicBonesRestoreDraw();\nbool DynamicBonesAppliedToDraw();'))
edit('src/DynamicBones.cpp',lambda s:s.replace('float  g_patchedSaved[12] = {};','float  g_patchedSaved[12] = {};\nbool   g_appliedThisDraw = false;\nint    g_level = -1;\nuint64_t g_item = 0;').replace('void DynamicBonesApplyToDraw() {\n    g_patchedAt = nullptr;', 'bool DynamicBonesAppliedToDraw() { return g_appliedThisDraw; }\n\nvoid DynamicBonesApplyToDraw() {\n    g_appliedThisDraw = false;\n    g_patchedAt = nullptr;').replace('    g_patchedAt = m;', '    g_patchedAt = m;\n    g_appliedThisDraw = true;').replace('    g_boundBase = 0;\n    ResetSolver();','    g_boundBase = 0;\n    g_level = -1;\n    g_item = 0;\n    ResetSolver();').replace('    // Lara was not drawn this frame:', '''    const int level = AppFlag(drva::app_off::level);
    const uint64_t item = *reinterpret_cast<uint64_t*>(base + d->laraItem);
    if (level != g_level || item != g_item) {
        ResetSolver();
        BoneSkinResetRegion();
        g_level = level;
        g_item = item;
    }

    // Lara was not drawn this frame:'''))
edit('src/BoneSkin.h',lambda s:s.replace('void BoneSkinAfterValidate();','void BoneSkinAfterValidate(bool jointApplied = false);'))
edit('src/Hooks.cpp',lambda s:s.replace('            DynamicBonesRestoreDraw();\n            BoneSkinAfterValidate();','            const bool jointApplied = DynamicBonesAppliedToDraw();\n            DynamicBonesRestoreDraw();\n            BoneSkinAfterValidate(jointApplied);'))
edit('src/BoneSkin.cpp',lambda s:s.replace('void BoneSkinAfterValidate() {','void BoneSkinAfterValidate(bool jointApplied) {').replace('    std::string patched;\n', '''    if (shader == Shaders()) {
        g_matched = g_patched = g_rejected = 0;
        g_summaryLogged = false;
        BoneSkinResetRegion();
    }
    std::string patched;
''').replace('        const float bone[4] = { off[0], off[1], off[2], 1.0f };','''        // The warm-up draw may already have the rigid offset uploaded. Start
        // shader deformation on the next draw to avoid applying both at once.
        const float bone[4] = { off[0], off[1], off[2], jointApplied ? 0.0f : 1.0f };'''))
# Restoring from a load/teleport should begin at equilibrium, with no fake bounce.
edit('src/DynamicBones.cpp',lambda s:s.replace('std::memset(g_bone[i].x,    0, sizeof(g_bone[i].x));','for (int a = 0; a < 3; ++a) g_bone[i].x[a] = gLocal[a] / c.dynamicBonesStiffness;'))
# Remove claims specific to the source game's observed counts from this port.
edit('src/BoneSkin.cpp',lambda s:s.replace('202','74').replace('TR4/TR5','TR1-3').replace('TR4/TR5','TR1-3'))
edit('src/DynamicBones.cpp',lambda s:s.replace('// at most 72,','// at most 15,').replace('glUniform4fv(loc, num_joints*3, ...)','glUniform4fv(loc, 96, ...)'))
print('Reset, fallback transition, and configuration validation completed')
