#include "Callsite.h"
#include "Log.h"

#include <windows.h>
#include <psapi.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "psapi.lib")

namespace tr {
namespace {

struct Seen {
    const char* tag;
    uint64_t    addr;
};

constexpr int kMaxSeen = 64;
Seen g_seen[kMaxSeen];
int  g_seenCount = 0;

// Resolve an address to "module+RVA", which is what Ghidra needs. The DLLs are
// ASLR'd, so the raw pointer is useless on its own.
bool Describe(uint64_t addr, char* out, size_t outLen) {
    HMODULE mods[256];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        return false;
    }
    const int count = (int)(needed / sizeof(HMODULE));
    for (int i = 0; i < count; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) {
            continue;
        }
        const uint64_t base = (uint64_t)mi.lpBaseOfDll;
        if (addr < base || addr >= base + mi.SizeOfImage) continue;

        char name[MAX_PATH] = {};
        GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof(name) - 1);
        _snprintf_s(out, outLen, _TRUNCATE, "%s+0x%llX",
                    name[0] ? name : "?",
                    (unsigned long long)(addr - base));
        return true;
    }
    return false;
}

} // namespace

void CallsiteCensusReset() {
    g_seenCount = 0;
    std::memset(g_seen, 0, sizeof(g_seen));
}

void NoteCallsite(const char* tag, void* returnAddress) {
    const uint64_t a = (uint64_t)returnAddress;
    if (!a) return;

    for (int i = 0; i < g_seenCount; ++i) {
        if (g_seen[i].addr == a && g_seen[i].tag == tag) return;
    }
    if (g_seenCount >= kMaxSeen) return;

    g_seen[g_seenCount].tag  = tag;
    g_seen[g_seenCount].addr = a;
    ++g_seenCount;

    char where[MAX_PATH + 32] = {};
    if (Describe(a, where, sizeof(where))) {
        LogF("callsite: %-14s <- %s", tag, where);
    } else {
        LogF("callsite: %-14s <- %p (module not found)", tag, returnAddress);
    }
}

} // namespace tr
