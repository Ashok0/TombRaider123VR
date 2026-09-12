#include "InlineHook.h"
#include "Log.h"

#include <windows.h>
#include <cstring>
#include <cstdint>

namespace hook {
namespace {

constexpr size_t kBlockSize = 0x1000;

// Absolute indirect jump: FF 25 00 00 00 00 <qword>  == jmp [rip+0]
constexpr size_t kAbsJmpSize = 14;

void WriteAbsJmp(uint8_t* at, void* dest) {
    at[0] = 0xFF;
    at[1] = 0x25;
    at[2] = at[3] = at[4] = at[5] = 0x00;
    uint64_t d = reinterpret_cast<uint64_t>(dest);
    std::memcpy(at + 6, &d, sizeof(d));
}

// Find an executable page within +/-2 GB of `anchorPtr` so an E9 rel32 can
// reach it. (Do not name the parameter `near` -- windows.h still defines that
// as an empty macro, a leftover from 16-bit segmented memory.)
void* AllocNear(void* anchorPtr) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t anchor = reinterpret_cast<uintptr_t>(anchorPtr);

    // Stay comfortably inside the 2 GB window (leave 16 MB of slack).
    const uintptr_t reach = 0x7F000000ull;
    const uintptr_t lo = (anchor > reach) ? (anchor - reach) : gran;
    const uintptr_t hi = anchor + reach;

    // Search upward first, then downward; either direction is fine.
    for (uintptr_t p = (anchor + gran - 1) & ~(gran - 1); p < hi; p += gran) {
        if (void* m = VirtualAlloc(reinterpret_cast<void*>(p), kBlockSize,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
            return m;
    }
    for (uintptr_t p = anchor & ~(gran - 1); p > lo; p -= gran) {
        if (void* m = VirtualAlloc(reinterpret_cast<void*>(p), kBlockSize,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
            return m;
    }
    return nullptr;
}

bool WriteCode(void* dst, const void* src, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

} // namespace

InlineHook::~InlineHook() { Remove(); }

bool InlineHook::Install(void* target,
                         void* detour,
                         size_t stolen,
                         const uint8_t* expectedBytes,
                         size_t expectedLen,
                         const char* name,
                         const int* ripDisp32Offsets,
                         size_t ripCount) {
    m_name = name;

    if (m_installed) {
        LogF("hook[%s]: already installed", name);
        return false;
    }
    if (!target || !detour) {
        LogF("hook[%s]: null target or detour", name);
        return false;
    }
    if (stolen < 5 || stolen > sizeof(m_saved)) {
        LogF("hook[%s]: bad stolen size %zu", name, stolen);
        return false;
    }

    // Refuse to patch a binary we do not recognise.
    if (expectedBytes && expectedLen) {
        if (expectedLen > stolen) {
            LogF("hook[%s]: expected-byte window (%zu) exceeds stolen (%zu)",
                 name, expectedLen, stolen);
            return false;
        }
        if (std::memcmp(target, expectedBytes, expectedLen) != 0) {
            const uint8_t* got = static_cast<const uint8_t*>(target);
            LogF("hook[%s]: prologue mismatch at %p -- expected %02X %02X %02X %02X, "
                 "found %02X %02X %02X %02X. Wrong game build; not patching.",
                 name, target,
                 expectedBytes[0], expectedBytes[1],
                 expectedLen > 2 ? expectedBytes[2] : 0,
                 expectedLen > 3 ? expectedBytes[3] : 0,
                 got[0], got[1], got[2], got[3]);
            return false;
        }
    }

    // One page holds both the near-bridge and the trampoline.
    uint8_t* block = static_cast<uint8_t*>(AllocNear(target));
    if (!block) {
        LogF("hook[%s]: could not reserve a page within 2 GB of %p", name, target);
        return false;
    }

    uint8_t* bridge = block;                 // abs jmp -> detour
    uint8_t* tramp  = block + kAbsJmpSize;   // stolen bytes + abs jmp back

    WriteAbsJmp(bridge, detour);

    std::memcpy(m_saved, target, stolen);
    std::memcpy(tramp, target, stolen);

    // Fix up RIP-relative displacements in the copied bytes.
    //
    // A RIP-relative operand resolves to (end of instruction) + disp32. The
    // instruction now lives at `tramp` instead of `target`, so to keep the same
    // absolute address the displacement shifts by exactly (target - tramp) --
    // the offset of the field within the instruction cancels out.
    const int64_t shift = static_cast<uint8_t*>(target) - tramp;
    for (size_t i = 0; i < ripCount; ++i) {
        const int off = ripDisp32Offsets[i];
        if (off < 0 || static_cast<size_t>(off) + 4 > stolen) {
            LogF("hook[%s]: rip fixup offset %d outside stolen bytes (%zu)",
                 name, off, stolen);
            VirtualFree(block, 0, MEM_RELEASE);
            return false;
        }
        int32_t disp = 0;
        std::memcpy(&disp, tramp + off, sizeof(disp));
        const int64_t fixed = static_cast<int64_t>(disp) + shift;
        if (fixed > INT32_MAX || fixed < INT32_MIN) {
            LogF("hook[%s]: rip fixup at +%d overflows int32 (%lld)",
                 name, off, static_cast<long long>(fixed));
            VirtualFree(block, 0, MEM_RELEASE);
            return false;
        }
        const int32_t out = static_cast<int32_t>(fixed);
        std::memcpy(tramp + off, &out, sizeof(out));
        LogF("hook[%s]: relocated rip disp32 at +%d (%d -> %d)",
             name, off, disp, out);
    }

    WriteAbsJmp(tramp + stolen, static_cast<uint8_t*>(target) + stolen);

    // E9 rel32 from target to bridge, NOP-padded out to `stolen`.
    uint8_t patch[sizeof(m_saved)];
    std::memset(patch, 0x90, stolen);
    patch[0] = 0xE9;
    const int64_t rel = reinterpret_cast<uint8_t*>(bridge)
                      - (static_cast<uint8_t*>(target) + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) {
        LogF("hook[%s]: bridge out of rel32 range (%lld)", name, static_cast<long long>(rel));
        VirtualFree(block, 0, MEM_RELEASE);
        return false;
    }
    const int32_t rel32 = static_cast<int32_t>(rel);
    std::memcpy(patch + 1, &rel32, sizeof(rel32));

    if (!WriteCode(target, patch, stolen)) {
        LogF("hook[%s]: VirtualProtect/write failed at %p", name, target);
        VirtualFree(block, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), block, kBlockSize);

    m_target     = target;
    m_bridge     = bridge;
    m_trampoline = tramp;
    m_stolen     = stolen;
    m_installed  = true;

    LogF("hook[%s]: %p -> %p (stole %zu, tramp %p)", name, target, detour, stolen, tramp);
    return true;
}

void InlineHook::Remove() {
    if (!m_installed) return;
    WriteCode(m_target, m_saved, m_stolen);
    VirtualFree(m_bridge, 0, MEM_RELEASE);   // bridge is the block base
    m_installed  = false;
    m_trampoline = nullptr;
    m_bridge     = nullptr;
    LogF("hook[%s]: removed", m_name);
}

} // namespace hook
