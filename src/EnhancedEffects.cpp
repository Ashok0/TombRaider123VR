#include "EnhancedEffects.h"
#include "Config.h"
#include "Engine.h"
#include "GL.h"
#include "InlineHook.h"
#include "Log.h"

namespace tr {
namespace effectruntime {
hook::InlineHook load, upscale, create, update;
bool ready = false;
using Load = void (__fastcall*)(char*, int, int);
using Create = void (__fastcall*)(int);
using Update = void (__fastcall*)(int, int, int, void*);
// Complete, position-independent instructions, checked against both supported
// EXEs by verify_addresses.py and again by InlineHook before installation.
const uint8_t kEffectLoadStockPrologue[] = { 0x40, 0x55, 0x57, 0x41, 0x54 };
const uint8_t kEffectLoadRetailPrologue[] = { 0x40, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00 };
const uint8_t kEffectUpscalePrologue[] = { 0x40, 0x56, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00 };
const uint8_t kEffectCreatePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
const uint8_t kEffectUpdatePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

struct Slot { effects::Table kinds{}; uint64_t revision = 1; };
Slot slots[19];
struct Pending { int slot = -1, layer = -1; effects::Kind kind = effects::None; };
thread_local Pending pending;
struct LoadScope {
    Pending saved;
    LoadScope(const char* name, int slot, int layer) : saved(pending) {
        pending = { slot, layer, effects::Classify(name, CurrentGame()) };
    }
    ~LoadScope() { pending = saved; }
};
struct Program { GLint location = -2; int slot = -2; uint64_t revision = 0; };
Program programs[4096];

void NoteUpload(int slot, int layer, effects::Kind kind) {
    if (slot < 0 || slot >= 19 || layer < 0 || layer >= effects::kLayers) return;
    auto& s = slots[slot];
    if (effects::Get(s.kinds, layer) == kind) return;
    effects::Set(s.kinds, layer, kind); ++s.revision;
    if (kind != effects::None)
        LogF("effects: texture slot %d layer %d classified as %d", slot, layer, static_cast<int>(kind));
}
void ResetSlot(int slot) {
    if (slot < 0 || slot >= 19) return;
    slots[slot].kinds = {}; ++slots[slot].revision;
}
void __fastcall DetourLoad(char* name, int slot, int layer) {
    LoadScope scope(name, slot, layer);
    load.Original<Load>()(name, slot, layer);
}
void __fastcall DetourUpscale(char* name, int unused, int layer) {
    // This native loader ignores its second argument and uploads to slot 4.
    LoadScope scope(name, 4, layer);
    upscale.Original<Load>()(name, unused, layer);
}
void __fastcall DetourCreate(int slot) {
    ResetSlot(slot);
    create.Original<Create>()(slot);
}
void __fastcall DetourUpdate(int slot, int layer, int mip, void* pixels) {
    update.Original<Update>()(slot, layer, mip, pixels);
    if (mip != 0) return;
    // Classify only an upload actually performed by the native loader. Missing
    // or unsupported files cannot relabel an old layer. Unrelated uploads clear
    // the previous kind, including when a level reuses an existing tile slot.
    NoteUpload(slot, layer, pending.slot == slot && pending.layer == layer
               ? pending.kind : effects::None);
}
} // namespace effectruntime

bool EnhancedEffectsReady() { return effectruntime::ready; }
void EnhancedEffectsInstall() {
    using namespace effectruntime;
    if (!Cfg().enabled || !Cfg().enhancedEffects || ready) return;
    const auto& l = L();
    const bool loadOk = l.vidLoadTexture == rva::vidLoadTexture
        ? load.Install(Fn(l.vidLoadTexture), reinterpret_cast<void*>(&DetourLoad),
                       5, kEffectLoadStockPrologue, sizeof(kEffectLoadStockPrologue), "effect texture load")
        : load.Install(Fn(l.vidLoadTexture), reinterpret_cast<void*>(&DetourLoad),
                       11, kEffectLoadRetailPrologue, sizeof(kEffectLoadRetailPrologue), "effect texture load");
    if (!loadOk ||
        !upscale.Install(Fn(l.vidLoadTextureUpscaled), reinterpret_cast<void*>(&DetourUpscale),
                         9, kEffectUpscalePrologue, sizeof(kEffectUpscalePrologue), "effect upscale load") ||
        !create.Install(Fn(l.ogl_texCreate), reinterpret_cast<void*>(&DetourCreate),
                        5, kEffectCreatePrologue, sizeof(kEffectCreatePrologue), "effect texture create") ||
        !update.Install(Fn(l.ogl_texUpdate), reinterpret_cast<void*>(&DetourUpdate),
                        5, kEffectUpdatePrologue, sizeof(kEffectUpdatePrologue), "effect texture update")) {
        EnhancedEffectsShutdown();
        Log("effects: texture hooks unavailable; retaining original effects");
        return;
    }
    ready = true;
    Log("effects: built-in enhancement enabled; no replacement DDS files required");
}
void EnhancedEffectsShutdown() {
    using namespace effectruntime;
    ready = false;
    update.Remove(); create.Remove(); upscale.Remove(); load.Remove();
    for (auto& slot : slots) slot = Slot{};
    for (auto& program : programs) program = Program{};
    pending = {};
}
void EnhancedEffectsInvalidateProgram(uint32_t program) {
    if (program < 4096) effectruntime::programs[program] = effectruntime::Program{};
}
void EnhancedEffectsAfterValidate() {
    using namespace effectruntime;
    if (!gl::Uniform4iv || !gl::GetUniformLocation) return;
    const auto& state = VidState();
    if (state.shader < 0 || state.shader >= kShaderCount) return;
    const uint32_t id = Shaders()[state.shader].id;
    if (!id || id >= 4096) return;
    auto& program = programs[id];
    if (program.location == -2) program.location = gl::GetUniformLocation(id, "uEffectKinds");
    if (program.location < 0) return;
    const int slot = ready && Cfg().enhancedEffects && Cfg().enabled && IsWorldPass()
                     && state.tex[0] < 19 ? static_cast<int>(state.tex[0]) : -1;
    const uint64_t revision = slot >= 0 ? slots[slot].revision : 0;
    if (program.slot == slot && program.revision == revision) return;
    const effects::Table zero{};
    gl::Uniform4iv(program.location, 12, slot >= 0 ? slots[slot].kinds.data() : zero.data());
    program.slot = slot; program.revision = revision;
}
} // namespace tr
