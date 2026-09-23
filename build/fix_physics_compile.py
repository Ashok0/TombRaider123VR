from pathlib import Path
p=Path('src/Config.cpp'); s=p.read_text(); a=s.index('    // TR1-3 chest physics:'); b=s.index('    g_liveScale = g_cfg.worldUnitsPerMetre;',a); assert a<s.index('void LoadConfig'); s=s[:a]+s[b:]; p.write_text(s)
p=Path('src/DynamicBones.cpp'); s=p.read_text().replace('vs.proj->e33','vs.proj->m[15]'); p.write_text(s)
