// =============================================================================
//  embedded.h — files shipped inside HytaleTrainer.exe.
//
//  The agent jars and the in-game renderer DLL are linked into the exe as
//  RCDATA resources (see CMakeLists.txt), so the exe is the only file anyone
//  needs. They are written out to a private temp folder right before use: the
//  JVM and the game keep a loaded file open, and a per-session copy keeps
//  every run independent.
// =============================================================================
#pragma once
#include <Windows.h>
#include <cstdio>
#include <filesystem>

namespace embedded {

enum : int { kAgentJar = 101, kCoreJar = 102, kRenderDll = 103 };

// Writes resource `id` to `to`. False if the exe carries no such resource.
inline bool Extract(int id, const std::filesystem::path& to) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    DWORD n = SizeofResource(nullptr, r);
    const void* p = g ? LockResource(g) : nullptr;
    if (!p || !n) return false;
    FILE* f = _wfopen(to.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(p, 1, n, f) == n;
    fclose(f);
    return ok;
}

// The embedded copy, or (developer builds without resources) the file next to
// the exe, written / copied to `to`.
inline bool Stage(int id, const std::filesystem::path& besideExe, const std::filesystem::path& to) {
    if (Extract(id, to)) return true;
    std::error_code ec;
    return std::filesystem::exists(besideExe, ec) &&
           std::filesystem::copy_file(besideExe, to, std::filesystem::copy_options::overwrite_existing, ec);
}

} // namespace embedded
