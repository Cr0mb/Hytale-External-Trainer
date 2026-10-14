#include "log.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace log_ {
namespace {
FILE* g_f = nullptr;
std::mutex g_mx;
void Write(const char* lvl, const char* fmt, va_list ap) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_f) return;
    time_t t = time(nullptr); tm lt{}; localtime_s(&lt, &t);
    fprintf(g_f, "%02d:%02d:%02d [%s] ", lt.tm_hour, lt.tm_min, lt.tm_sec, lvl);
    vfprintf(g_f, fmt, ap);
    fputc('\n', g_f);
    fflush(g_f);
}
}
void Init(const wchar_t* path) { g_f = _wfopen(path, L"a"); }
void Info(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); Write("info", fmt, ap); va_end(ap); }
void Error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write("err ", fmt, ap); va_end(ap); }
void Shutdown() { std::lock_guard<std::mutex> lk(g_mx); if (g_f) fclose(g_f); g_f = nullptr; }
}
