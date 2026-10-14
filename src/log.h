#pragma once
namespace log_ {
void Init(const wchar_t* path);
void Info(const char* fmt, ...);
void Error(const char* fmt, ...);
void Shutdown();
}
