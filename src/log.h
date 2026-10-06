#pragma once
// Thread-safe, flushed-per-line logger writing bl2hdr.log next to the game exe.
#include <cstdarg>

namespace bl2hdr::log {
void Init(const wchar_t* file_name);  // opens <exe dir>\<file_name> (truncates, shared)
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);
void Shutdown();
}  // namespace bl2hdr::log
