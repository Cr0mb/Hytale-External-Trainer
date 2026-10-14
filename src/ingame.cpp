#include "ingame.h"
#include "embedded.h"
#include "log.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace ingame {
namespace {

std::mutex   g_mx;                 // guards the mapping against Stop() while the renderer writes
HANDLE       g_map = nullptr;
shm::Block*  g_blk = nullptr;
DWORD        g_pid = 0;
std::string  g_status = "off";
std::mutex   g_statusMx;

void SetStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_statusMx); g_status = s; }

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

// LoadLibraryW in the client on a thread of its own. kernel32 sits at the
// same address in every process of a boot, so its LoadLibraryW is ours.
bool LoadInto(DWORD pid, const std::wstring& dll, std::string& err) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { err = "cannot open the client (" + std::to_string(GetLastError()) + ")"; return false; }
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (mem && WriteProcessMemory(p, mem, dll.c_str(), bytes, nullptr)) {
        auto fn = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        if (HANDLE t = CreateRemoteThread(p, nullptr, 0, fn, mem, 0, nullptr)) {
            if (WaitForSingleObject(t, 10000) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeThread(t, &code);
                ok = code != 0;
                if (!ok) err = "LoadLibrary failed in the client";
            } else {
                err = "LoadLibrary timed out";
            }
            CloseHandle(t);
        } else {
            err = "cannot start a thread in the client (" + std::to_string(GetLastError()) + ")";
        }
    } else {
        err = "cannot write to the client";
    }
    if (mem && ok) VirtualFreeEx(p, mem, 0, MEM_RELEASE);   // keep it if the thread may still read it
    CloseHandle(p);
    return ok;
}

void Unmap() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_blk) UnmapViewOfFile(g_blk);
    if (g_map) CloseHandle(g_map);
    g_blk = nullptr;
    g_map = nullptr;
    g_pid = 0;
}

} // namespace

bool Start(DWORD clientPid) {
    if (g_blk) return true;
    wchar_t name[64];
    shm::MappingName(name, 64, clientPid);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(shm::Block), name);
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    if (!map) { SetStatus("cannot create shared memory"); return false; }
    auto* blk = (shm::Block*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shm::Block));
    if (!blk) { CloseHandle(map); SetStatus("cannot map shared memory"); return false; }
    // A renderer told to quit by the previous trainer is still on its way
    // out; let it finish before this session loads a new one.
    if (existed && blk->magic == shm::kMagic && blk->quit)
        for (int i = 0; i < 150 && blk->rendererState == shm::kHooked; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // A renderer from a trainer that just exited may still be loaded and
    // waiting out its timeout; it will happily keep drawing for us.
    bool reuse = existed && blk->magic == shm::kMagic && blk->version == shm::kVersion &&
                 blk->rendererState == shm::kHooked && !blk->quit;
    if (!reuse) {
        memset(blk, 0, sizeof(shm::Block));
        blk->magic = shm::kMagic;
        blk->version = shm::kVersion;
    }
    blk->quit = 0;
    blk->trainerBeatMs = shm::NowMs();          // fresh before the DLL first looks
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_map = map; g_blk = blk; g_pid = clientPid;
    }
    if (reuse) { log_::Info("[test] in-game renderer: reusing the one already loaded"); SetStatus("hooked"); return true; }

    // The renderer ships inside the exe (embedded.h); load a private copy.
    std::error_code ec;
    auto src = ExeDir() / L"hytrainer-render.dll";      // developer builds only
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    auto dir = std::filesystem::path(tmp) / L"hytrainer-render";
    std::filesystem::create_directories(dir, ec);
    for (auto& old : std::filesystem::directory_iterator(dir, ec)) std::filesystem::remove(old.path(), ec);   // unloaded ones go
    auto dll = dir / (std::to_wstring(clientPid) + L"_" + std::to_wstring(GetTickCount64()) + L".dll");
    if (!embedded::Stage(embedded::kRenderDll, src, dll)) {
        SetStatus("cannot unpack the in-game renderer");
        Unmap();
        return false;
    }
    std::string err;
    if (!LoadInto(clientPid, dll.wstring(), err)) {
        log_::Error("in-game renderer: %s", err.c_str());
        SetStatus(err);
        Unmap();
        return false;
    }
    for (int i = 0; i < 100; ++i) {
        LONG st = blk->rendererState;
        if (st == shm::kHooked) { log_::Info("[test] in-game renderer: hooked SDL3!SwapBuffers"); SetStatus("hooked"); return true; }
        if (st == shm::kFailed) break;
        blk->trainerBeatMs = shm::NowMs();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::string why = blk->rendererError[0] ? blk->rendererError : "renderer did not start";
    log_::Error("in-game renderer: %s", why.c_str());
    SetStatus(why);
    Unmap();
    return false;
}

void Stop() {
    shm::Block* blk;
    { std::lock_guard<std::mutex> lk(g_mx); blk = g_blk; }
    if (!blk) return;
    blk->quit = 1;
    for (int i = 0; i < 150 && blk->rendererState == shm::kHooked; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    log_::Info("[test] in-game renderer: %s", blk->rendererState == shm::kHooked ? "still hooked at stop (will time out)" : "unloaded");
    Unmap();
    SetStatus("off");
}

bool Running() { std::lock_guard<std::mutex> lk(g_mx); return g_blk != nullptr; }

bool Active() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_blk && g_blk->rendererState == shm::kHooked && shm::NowMs() - g_blk->rendererBeatMs < 250.0;
}

std::string Status() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_blk && g_blk->rendererState == shm::kHooked) {
            char t[128];
            if (shm::NowMs() - g_blk->rendererBeatMs < 250.0)
                snprintf(t, sizeof(t), "drawing in game, %.0f fps", g_blk->rendererFps);
            else if (g_blk->rendererError[0])
                snprintf(t, sizeof(t), "%s", g_blk->rendererError);
            else
                snprintf(t, sizeof(t), "hooked, idle");
            return t;
        }
    }
    std::lock_guard<std::mutex> lk(g_statusMx);
    return g_status;
}

std::string TakeStats() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_blk) return "off";
    char t[256];
    LONG matched = InterlockedExchange(&g_blk->entsMatched, 0), total = InterlockedExchange(&g_blk->entsTotal, 0);
    snprintf(t, sizeof(t), "frames=%ld client=%ld server=%ld hidden=%ld ownorigin=%ld you-from-client=%ld ents-from-client=%.0f%% fps=%.0f state=%ld%s%s",
             InterlockedExchange(&g_blk->frames, 0), InterlockedExchange(&g_blk->framesClient, 0),
             InterlockedExchange(&g_blk->framesServer, 0), InterlockedExchange(&g_blk->framesHidden, 0),
             InterlockedExchange(&g_blk->framesOwnOrigin, 0), InterlockedExchange(&g_blk->meFromClient, 0),
             total ? 100.0 * matched / total : 0.0, g_blk->rendererFps,
             (long)g_blk->rendererState, g_blk->rendererError[0] ? " err=" : "", g_blk->rendererError);
    return t;
}

shm::Block* BeginWrite() {
    g_mx.lock();
    if (!g_blk) { g_mx.unlock(); return nullptr; }
    InterlockedIncrement(&g_blk->seq);           // odd: being written
    MemoryBarrier();
    return g_blk;
}

void EndWrite() {
    g_blk->trainerBeatMs = shm::NowMs();
    MemoryBarrier();
    InterlockedIncrement(&g_blk->seq);           // even: stable
    g_mx.unlock();
}

} // namespace ingame
