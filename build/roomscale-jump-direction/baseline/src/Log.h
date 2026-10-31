#pragma once

namespace tr {
void LogOpen(const wchar_t* path);
void LogClose();
}

void Log(const char* msg);
void LogF(const char* fmt, ...);
