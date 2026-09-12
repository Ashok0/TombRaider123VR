#include "Log.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

namespace {
FILE*      g_file = nullptr;
std::mutex g_mutex;
}

namespace tr {

void LogOpen(const wchar_t* path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) return;
    _wfopen_s(&g_file, path, L"w");
}

void LogClose() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

} // namespace tr

void Log(const char* msg) {
    std::lock_guard<std::mutex> lk(g_mutex);
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    if (g_file) {
        fprintf(g_file, "%s\n", msg);
        fflush(g_file);
    }
}

void LogF(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log(buf);
}
