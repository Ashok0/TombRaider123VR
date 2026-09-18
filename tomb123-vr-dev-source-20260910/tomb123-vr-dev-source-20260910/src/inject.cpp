#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <tlhelp32.h>
uintptr_t moduleBase(DWORD pid, const wchar_t *name) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (s == INVALID_HANDLE_VALUE)
    return 0;
  MODULEENTRY32W m{sizeof(m)};
  uintptr_t p = 0;
  for (BOOL b = Module32FirstW(s, &m); b; b = Module32NextW(s, &m))
    if (!_wcsicmp(m.szModule, name)) {
      p = (uintptr_t)m.modBaseAddr;
      break;
    }
  CloseHandle(s);
  return p;
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 3) {
    puts("Usage: inject.exe PID absolute-dll-path");
    return 2;
  }
  DWORD pid = wcstoul(argv[1], nullptr, 10);
  HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                             PROCESS_VM_WRITE | PROCESS_VM_READ,
                         FALSE, pid);
  if (!p) {
    printf("OpenProcess error %lu\n", GetLastError());
    return 1;
  }
  size_t n = (wcslen(argv[2]) + 1) * 2;
  void *remote = VirtualAllocEx(p, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote || !WriteProcessMemory(p, remote, argv[2], n, nullptr))
    return 3;
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  auto load = (uintptr_t)GetProcAddress(k, "LoadLibraryW");
  HMODULE owner = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)load, &owner);
  wchar_t ownerPath[MAX_PATH];
  GetModuleFileNameW(owner, ownerPath, MAX_PATH);
  const wchar_t *ownerName = wcsrchr(ownerPath, L'\\');
  uintptr_t target =
      moduleBase(pid, ownerName ? ownerName + 1 : ownerPath) + load - (uintptr_t)owner;
  HANDLE t = CreateRemoteThread(p, nullptr, 0, (LPTHREAD_START_ROUTINE)target, remote, 0, nullptr);
  if (!t)
    return 4;
  if (WaitForSingleObject(t, 15000) != WAIT_OBJECT_0) {
    puts("Load timed out; leaving remote allocation in use.");
    return 5;
  }
  CloseHandle(t);
  VirtualFreeEx(p, remote, 0, MEM_RELEASE);
  const wchar_t *name = wcsrchr(argv[2], L'\\');
  uintptr_t base = moduleBase(pid, name ? name + 1 : argv[2]);
  if (!base) {
    puts("DLL did not load");
    return 6;
  }
  HMODULE local = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
  auto init = GetProcAddress(local, "Initialize");
  if (!init)
    return 7;
  uintptr_t rva = (uintptr_t)init - (uintptr_t)local;
  FreeLibrary(local);
  t = CreateRemoteThread(p, nullptr, 0, (LPTHREAD_START_ROUTINE)(base + rva), nullptr, 0, nullptr);
  if (!t)
    return 8;
  if (WaitForSingleObject(t, 15000) != WAIT_OBJECT_0)
    return 9;
  DWORD result = 0;
  GetExitCodeThread(t, &result);
  CloseHandle(t);
  CloseHandle(p);
  printf("Initialize returned %lu\n", result);
  return result ? 0 : 10;
}
