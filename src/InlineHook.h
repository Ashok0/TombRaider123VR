// InlineHook.h -- Minimal x64 inline hook, no external dependencies.
//
// Why not MinHook/Detours: we do not need a general length-disassembler. Every
// target in this project had its prologue read out of Ghidra, so the number of
// bytes to steal is a verified constant, and the hook refuses to install if the
// bytes at the target are not exactly what we expect. That turns "the game
// updated" into a clean log line instead of a corrupted instruction stream.
//
// Mechanism:
//   target:  E9 <rel32 to detour>          (5 bytes, padded with 0x90/int3)
//   tramp:   <stolen bytes> FF 25 00000000 <qword target+stolen>
//
// A 5-byte relative jump only reaches +/-2 GB, and this DLL can easily land
// further than that from the exe, so the detour is not jumped to directly:
// `E9` targets a stub allocated near the target which then does an absolute
// jump. Alloc() walks down from the target looking for free pages.
#pragma once

#include <cstdint>
#include <cstddef>

namespace hook {

class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();

    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    // `expectedBytes` / `expectedLen` describe the instruction bytes that must
    // be present at `target`. `stolen` must be >= 5 and must land exactly on an
    // instruction boundary -- both are verified facts from the disassembly, not
    // guesses. All stolen instructions must be position-independent (no
    // RIP-relative operands), which is checked by eye when picking the count.
    // ripDisp32Offsets lists byte offsets, within the stolen bytes, of RIP-
    // relative disp32 fields that must be fixed up when the bytes are copied to
    // the trampoline.
    //
    // This is NOT optional where it applies. A raw copy leaves the displacement
    // pointing relative to the trampoline instead of the original site, and the
    // trampoline can sit up to 2 GB away, so the instruction reads or writes an
    // arbitrary address. Several of the engine's prologues start with exactly
    // that kind of instruction, e.g.
    //     83 0D <disp32> 01     OR dword ptr [rip+disp32], 1
    // which would corrupt memory rather than merely misbehave.
    bool Install(void* target,
                 void* detour,
                 size_t stolen,
                 const uint8_t* expectedBytes,
                 size_t expectedLen,
                 const char* name,
                 const int* ripDisp32Offsets = nullptr,
                 size_t ripCount = 0);

    void Remove();

    // Call this to invoke the original function.
    template <typename Fn>
    Fn Original() const { return reinterpret_cast<Fn>(m_trampoline); }

    bool installed() const { return m_installed; }

private:
    void*   m_target     = nullptr;
    void*   m_trampoline = nullptr;   // stolen bytes + jmp back
    void*   m_bridge     = nullptr;   // near-stub: abs jmp to the detour
    uint8_t m_saved[32]  = {};
    size_t  m_stolen     = 0;
    bool    m_installed  = false;
    const char* m_name   = "?";
};

} // namespace hook
