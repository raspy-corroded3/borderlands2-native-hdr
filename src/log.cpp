#include "log.h"

#include <windows.h>

#include <share.h>

#include <cstdio>
#include <mutex>

namespace bl2hdr::log {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
LARGE_INTEGER g_freq{}, g_start{};

void Write(const char* level, const char* fmt, va_list args) {
  std::lock_guard lock(g_mutex);
  if (!g_file) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double secs = static_cast<double>(now.QuadPart - g_start.QuadPart) / static_cast<double>(g_freq.QuadPart);
  std::fprintf(g_file, "[%9.3f] [%5lu] %-5s ", secs, GetCurrentThreadId(), level);
  std::vfprintf(g_file, fmt, args);
  std::fputc('\n', g_file);
  std::fflush(g_file);  // survive crashes: every line is on disk
}
}  // namespace

void Init(const wchar_t* file_name) {
  QueryPerformanceFrequency(&g_freq);
  QueryPerformanceCounter(&g_start);
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  wchar_t* slash = wcsrchr(path, L'\\');
  if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), file_name);
  std::lock_guard lock(g_mutex);
  // _SH_DENYNO: never block (or be blocked by) another process that has the same file open.
  // (_wfopen_s opens exclusively for writing; that made the game's log fail while Launcher.exe held it.)
  g_file = _wfsopen(path, L"w", _SH_DENYNO);
}

void Info(const char* fmt, ...) { va_list a; va_start(a, fmt); Write("INFO", fmt, a); va_end(a); }
void Warn(const char* fmt, ...) { va_list a; va_start(a, fmt); Write("WARN", fmt, a); va_end(a); }
void Error(const char* fmt, ...) { va_list a; va_start(a, fmt); Write("ERROR", fmt, a); va_end(a); }

void Shutdown() {
  std::lock_guard lock(g_mutex);
  if (g_file) { std::fclose(g_file); g_file = nullptr; }
}
}  // namespace bl2hdr::log
