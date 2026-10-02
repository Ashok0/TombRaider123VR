#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace tr {
void EnhancedEffectsInstall();
void EnhancedEffectsShutdown();
bool EnhancedEffectsReady();
void EnhancedEffectsAfterValidate();
void EnhancedEffectsInvalidateProgram(uint32_t program);

namespace effects {
enum Kind { None, Fire, Lamps, Particles, Bubbles };
// The native HD tile array has 384 layers. Pack eight four-bit kinds per
// integer: a single indexed vertex lookup, no per-pixel searches or textures.
constexpr int kLayers = 384;
using Table = std::array<int32_t, kLayers / 8>;
inline void Set(Table& table, int layer, Kind kind) {
    if (layer < 0 || layer >= kLayers) return;
    const unsigned shift = (layer & 7) * 4;
    uint32_t word = static_cast<uint32_t>(table[layer / 8]);
    word = (word & ~(15u << shift)) | (static_cast<uint32_t>(kind) << shift);
    table[layer / 8] = static_cast<int32_t>(word);
}
inline Kind Get(const Table& table, int layer) {
    if (layer < 0 || layer >= kLayers) return None;
    return static_cast<Kind>((static_cast<uint32_t>(table[layer / 8]) >> ((layer & 7) * 4)) & 15);
}
// Accept only a TEX path and a complete numeric DDS basename. No substring
// matching against unrelated textures, UI images, or similarly named mods.
inline Kind Classify(const char* path, int game) {
    if (!path || game < 0 || game > 2) return None;
    std::string name(path);
    for (char& ch : name) {
        if (ch == '\\') ch = '/';
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
    const auto slash = name.rfind('/');
    if (slash == std::string::npos) return None;
    const auto prev = slash ? name.rfind('/', slash - 1) : std::string::npos;
    const auto begin = prev == std::string::npos ? 0 : prev + 1;
    if (name.substr(begin, slash - begin) != "tex") return None;
    const std::string file = name.substr(slash + 1);
    if (file == "5134.dds") return Fire;
    if (file == "5031.dds") return Lamps;
    if (game == 0 && file == "8999.dds") return Particles;
    if (game == 2 && file.size() == 8 && file.substr(4) == ".dds") {
        int id = 0;
        for (int i = 0; i < 4; ++i) {
            if (file[i] < '0' || file[i] > '9') return None;
            id = id * 10 + file[i] - '0';
        }
        if ((id >= 3400 && id <= 3403) || (id >= 3530 && id <= 3544)) return Bubbles;
    }
    return None;
}

inline bool Patch(std::string& vertex, std::string& fragment) {
    // Only native world/sprite pairs carrying a tile layer. Postprocessing,
    // movie, shadow-only and UI shaders keep their original source.
    if (vertex.find("out float vLayer;") == std::string::npos ||
        fragment.find("in float vLayer;") == std::string::npos ||
        fragment.find("uniform sampler2DArray sTex0;") == std::string::npos ||
        vertex.find("uEffectKinds") != std::string::npos) return false;
    const auto assign = vertex.find("vLayer = ");
    if (assign == std::string::npos || vertex.find("vLayer = ", assign + 1) != std::string::npos)
        return false;
    const auto end = vertex.find(';', assign);
    if (end == std::string::npos || vertex.find("void main()") == std::string::npos ||
        fragment.find("void main()") == std::string::npos) return false;
    std::string v = vertex, f = fragment;
    // Patch only the tile samples that actually use vLayer, never a normal map
    // or an auxiliary texture addressed through uParams.
    const char* samples[] = {
        "texture(sTex0, vec3(uv.xy, vLayer))",
        "texture(sTex0, vec3(vTexCoord.xy, vLayer))",
        "texture(sTex0, vec3(vTexCoord.xy.xy, vLayer))",
        "texture(sTex0_wrap, vec3(uv.xy, vLayer))",
        "texture(sTex0_wrap, vec3(tc_top.xy, vLayer))",
        "texture(sTex0_wrap, vec3(uv + v.xy, vLayer))",
    };
    int changed = 0;
    for (const char* sample : samples) {
        size_t at = 0;
        while ((at = f.find(sample, at)) != std::string::npos) {
            f.replace(at, 7, "enhancedEffectTexture");
            at += 21; ++changed;
        }
    }
    if (!changed) return false;
    v.insert(end + 1, R"GLSL(
    vEnhancedEffect = 0;
    int effectLayer = int(floor(vLayer + 0.5));
    if (effectLayer >= 0 && effectLayer < 384) {
        int effectWord = effectLayer >> 3;
        vEnhancedEffect = (uEffectKinds[effectWord >> 2][effectWord & 3]
                           >> ((effectLayer & 7) * 4)) & 15;
    }
)GLSL");
    v.insert(v.find("void main()"), "uniform ivec4 uEffectKinds[12];\nflat out int vEnhancedEffect;\n");
    f.insert(f.find("void main()"), R"GLSL(
flat in int vEnhancedEffect;
bool effectRect(vec2 p, vec4 r) {
    return p.x >= r.x && p.y >= r.y && p.x < r.z && p.y < r.w;
}
vec3 effectFire(vec3 c) {
    float hot = smoothstep(0.7, 1.0, max(c.r, max(c.g, c.b)));
    return clamp(pow(max(c, vec3(0.0)), vec3(0.8, 1.4, 1.8))
                 * vec3(1.18, 1.10, 1.05) + vec3(0.20, 0.35, 0.45) * hot, 0.0, 1.0);
}
vec4 enhancedEffectTexture(sampler2DArray tex, vec3 coord) {
    vec4 c = texture(tex, coord);
    if (vEnhancedEffect == 0 || c.a <= 0.0) return c;
    vec2 p = fract(coord.xy) * 512.0;
    if (vEnhancedEffect == 1) {
        c.rgb = effectFire(c.rgb);
    } else if (vEnhancedEffect == 2) {
        // Small luminous strips/buttons within the otherwise ordinary 5031
        // equipment atlas. Coordinates are in the DDS upload orientation.
        if (effectRect(p, vec4(386.0, 65.0, 424.0, 86.0)) ||
            effectRect(p, vec4(235.0, 160.0, 326.0, 180.0)) ||
            effectRect(p, vec4(481.0, 378.0, 512.0, 405.0)))
            c.rgb = clamp(c.rgb * 1.45, 0.0, 1.0);
    } else if (vEnhancedEffect == 3) {
        // Leave boot prints and the solid strip at the bottom unmodified.
        if (effectRect(p, vec4(0.0, 155.0, 400.0, 231.0)))
            c.rgb = effectFire(c.rgb);
        else if (p.y < 155.0 || p.x >= 480.0 ||
                 effectRect(p, vec4(155.0, 231.0, 470.0, 385.0)) ||
                 effectRect(p, vec4(0.0, 385.0, 128.0, 512.0)))
            c.rgb = 1.0 - pow(max(vec3(0.0), 1.0 - c.rgb), vec3(1.45));
    } else if (vEnhancedEffect == 4) {
        // Lift faint bubble rims, keeping true black and transparent texels
        // empty. Native animation, tint, mip filtering and alpha still apply.
        c.rgb = clamp(pow(max(c.rgb, vec3(0.0)), vec3(0.82)) * 1.8, 0.0, 1.0);
    }
    return c;
}
)GLSL");
    vertex = std::move(v); fragment = std::move(f);
    return true;
}
} // namespace effects
} // namespace tr
