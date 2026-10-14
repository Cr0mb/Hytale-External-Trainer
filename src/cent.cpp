#include "cent.h"
#include "log.h"

#include <TlHelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace cent {
namespace {

std::atomic<bool>  g_run{ false };
std::thread        g_thread;
DWORD              g_pid = 0;
HANDLE             g_proc = nullptr;
uintptr_t          g_modBase = 0, g_modEnd = 0;
uint32_t           g_peStamp = 0, g_peSize = 0;

std::mutex                          g_mx;
float                               g_me[3]{};
std::vector<std::array<float, 3>>   g_ents;
bool                                g_haveHint = false;
Layout                              g_layout;
std::vector<uintptr_t>              g_objs;
uintptr_t                           g_player = 0;     // your character
float                               g_lastMe[3]{};
int                                 g_meStill = 0;    // refreshes in a row you stood still
std::string                         g_status = "off";

void SetStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_mx); g_status = s; }

bool Read(uintptr_t at, void* into, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, (LPCVOID)at, into, n, &got) && got == n;
}

bool Finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
float Dist(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Scannable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT || (m.Protect & PAGE_GUARD) || m.Type != MEM_PRIVATE) return false;   // the GC heap
    DWORD p = m.Protect & 0xFF;
    return (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE) && m.RegionSize <= (SIZE_T)1024 << 20;
}

// Calls fn(chunkAddress, bytes, size) for every readable chunk of the heap.
template <class Fn> uint64_t WalkHeap(Fn fn) {
    static std::vector<uint8_t> buf(4u << 20);
    uint64_t total = 0;
    MEMORY_BASIC_INFORMATION m{};
    for (uintptr_t a = 0x10000; g_run && VirtualQueryEx(g_proc, (LPCVOID)a, &m, sizeof(m)); ) {
        uintptr_t base = (uintptr_t)m.BaseAddress, end = base + m.RegionSize;
        if (end <= a) break;
        if (Scannable(m)) {
            for (uintptr_t c = base; c < end && g_run; c += buf.size() - 64) {
                SIZE_T want = (SIZE_T)(std::min)((uintptr_t)buf.size(), end - c), got = 0;
                if (!ReadProcessMemory(g_proc, (LPCVOID)c, buf.data(), want, &got) || got < 64) break;
                total += got;
                fn(c, buf.data(), (size_t)got);
                if (want < buf.size()) break;
            }
        }
        a = end;
    }
    return total;
}

// ---------------------------------------------------------------------------
//  Discovery
// ---------------------------------------------------------------------------
bool FindModule() {
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
    if (sn == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof(me) };
    for (BOOL ok = Module32FirstW(sn, &me); ok; ok = Module32NextW(sn, &me))
        if (_wcsicmp(me.szModule, L"HytaleClient.exe") == 0) {
            g_modBase = (uintptr_t)me.modBaseAddr;
            g_modEnd = g_modBase + me.modBaseSize;
            break;
        }
    CloseHandle(sn);
    if (!g_modBase) return false;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!Read(g_modBase, &dos, sizeof(dos)) || !Read(g_modBase + dos.e_lfanew, &nt, sizeof(nt))) return false;
    g_peStamp = nt.FileHeader.TimeDateStamp;
    g_peSize = nt.OptionalHeader.SizeOfImage;
    return true;
}

// A NativeAOT MethodTable starts with uint32 flags (low 16 bits: component
// size, 0 for ordinary classes) and uint32 BaseSize.
bool MethodTableCovers(uintptr_t mt, uint32_t offset) {
    if (mt < g_modBase || mt >= g_modEnd) return false;
    uint32_t hdr[2];
    if (!Read(mt, hdr, sizeof(hdr))) return false;
    uint32_t base = hdr[1];
    return (hdr[0] & 0xFFFF) == 0 && base % 8 == 0 && base >= offset + 36 && base <= 0x8000;
}

struct Hints { float me[3]; std::vector<std::array<float, 3>> ents; bool have; };
Hints GetHints() {
    std::lock_guard<std::mutex> lk(g_mx);
    return { { g_me[0], g_me[1], g_me[2] }, g_ents, g_haveHint };
}

// Blocks of three float3 on server positions, then their object headers.
bool Discover(Layout& out) {
    Hints h = GetHints();
    std::vector<std::array<float, 3>> targets = h.ents;
    targets.push_back({ h.me[0], h.me[1], h.me[2] });
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (auto& t : targets)
        for (int i = 0; i < 3; ++i) { lo[i] = (std::min)(lo[i], t[i] - 1.f); hi[i] = (std::max)(hi[i], t[i] + 1.f); }

    struct Triple { uintptr_t addr; int target; };
    std::vector<Triple> triples;
    auto t0 = GetTickCount64();
    uint64_t bytes = WalkHeap([&](uintptr_t base, const uint8_t* p, size_t n) {
        for (size_t o = 0; o + 36 <= n; o += 4) {
            const float* v = (const float*)(p + o);
            if (!(v[0] >= lo[0] && v[0] <= hi[0] && v[1] >= lo[1] && v[1] <= hi[1] && v[2] >= lo[2] && v[2] <= hi[2])) continue;
            for (int i = 0; i < (int)targets.size(); ++i) {
                const float* t = targets[i].data();
                float tol = i == (int)targets.size() - 1 ? 2.0f : 0.6f;     // last target is you
                if (Dist(v, t) < tol && Dist(v + 3, t) < tol + 1.f && Dist(v + 6, t) < tol + 1.f) {
                    triples.push_back({ base + o, i });
                    break;
                }
            }
        }
    });
    log_::Info("[test] client entities: %zu position blocks in %.0f MB (%llu ms)", triples.size(), bytes / 1048576.0,
               (unsigned long long)(GetTickCount64() - t0));

    // (type, offset) -> distinct targets that agree
    std::map<std::pair<uintptr_t, uint32_t>, std::set<int>> votes;
    std::map<std::pair<uintptr_t, uint32_t>, int> mine;          // headers of blocks on you
    const int meTarget = (int)targets.size() - 1;
    for (auto& tr : triples) {
        const int N = 0x2000 / 8;
        static uint64_t back[N];
        uintptr_t from = (tr.addr & ~(uintptr_t)7) - sizeof(back);
        if (!Read(from, back, sizeof(back))) continue;
        for (int i = N - 1; i >= 0; --i) {
            uint32_t off = (uint32_t)(tr.addr - (from + i * 8));
            if (MethodTableCovers((uintptr_t)back[i], off)) {
                votes[{ (uintptr_t)back[i], off }].insert(tr.target);
                if (tr.target == meTarget) mine[{ (uintptr_t)back[i], off }]++;
                break;
            }
        }
    }
    size_t best = 0;
    for (auto& [k, v] : votes)
        if (v.size() > best) { best = v.size(); out.mt = k.first; out.posOff = (int)k.second; }
    if (best < 2) return false;                 // need two different entities to agree
    out.ok = true;
    // You are an entity too, possibly of a derived class with the same
    // position offset.
    for (auto& [k, cnt] : mine)
        if (k.first != out.mt && (int)k.second == out.posOff) { out.mt2 = k.first; break; }
    if (out.mt2) log_::Info("[test] client entities: your character is type image+%llx", (unsigned long long)(out.mt2 - g_modBase));
    log_::Info("[test] client entities: type image+%llx, positions at +0x%x (%zu entities agree)",
               (unsigned long long)(out.mt - g_modBase), out.posOff, best);
    return true;
}

std::wstring CachePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/") + 1) + L"client_layout.txt";
}

bool LoadCache(Layout& out) {
    FILE* f = _wfopen(CachePath().c_str(), L"r");
    if (!f) return false;
    unsigned stamp = 0, size = 0, off = 0;
    unsigned long long rva = 0, rva2 = 0;
    int n = fscanf(f, "%x %x %llx %x %llx", &stamp, &size, &rva, &off, &rva2);
    fclose(f);
    if (n < 4 || stamp != g_peStamp || size != g_peSize) return false;
    out.mt = g_modBase + (uintptr_t)rva;
    out.mt2 = rva2 ? g_modBase + (uintptr_t)rva2 : 0;
    out.posOff = (int)off;
    out.ok = MethodTableCovers(out.mt, off);
    return out.ok;
}

void SaveCache(const Layout& l) {
    FILE* f = _wfopen(CachePath().c_str(), L"w");
    if (!f) return;
    fprintf(f, "%x %x %llx %x %llx\n", g_peStamp, g_peSize, (unsigned long long)(l.mt - g_modBase), (unsigned)l.posOff,
            (unsigned long long)(l.mt2 ? l.mt2 - g_modBase : 0));
    fclose(f);
}

// ---------------------------------------------------------------------------
//  Enumeration
// ---------------------------------------------------------------------------
// Entity objects live in a handful of GC regions. A full pass walks the whole
// heap (~15 s from outside the process); a quick pass rescans only the 4 MB
// windows that held entities last time (well under a second), which catches
// objects the GC moved nearby and most new spawns.
constexpr uintptr_t kWindow = 4u << 20;
std::vector<uintptr_t> g_windows;

template <class Fn> void WalkWindows(const std::vector<uintptr_t>& wins, Fn fn) {
    static std::vector<uint8_t> buf(kWindow);
    for (uintptr_t w : wins) {
        if (!g_run) return;
        MEMORY_BASIC_INFORMATION m{};
        for (uintptr_t a = w; a < w + kWindow && VirtualQueryEx(g_proc, (LPCVOID)a, &m, sizeof(m)); ) {
            uintptr_t base = (uintptr_t)m.BaseAddress, end = (std::min)(base + m.RegionSize, w + kWindow);
            if (end <= a) break;
            uintptr_t s = (std::max)(base, w);
            if (Scannable(m) && end > s) {
                SIZE_T got = 0;
                if (ReadProcessMemory(g_proc, (LPCVOID)s, buf.data(), end - s, &got) && got >= 8) fn(s, buf.data(), (size_t)got);
            }
            a = end;
        }
    }
}

std::vector<uintptr_t> Enumerate(const Layout& l, bool quick) {
    std::vector<uintptr_t> objs;
    const uint64_t mt = l.mt, mt2 = l.mt2 ? l.mt2 : l.mt;
    auto scan = [&](uintptr_t base, const uint8_t* p, size_t n) {
        size_t start = (8 - (base & 7)) & 7;
        for (size_t o = start; o + 8 <= n; o += 8) {
            uint64_t q = *(const uint64_t*)(p + o);
            if (q == mt || q == mt2) objs.push_back(base + o);
        }
    };
    if (quick && !g_windows.empty()) WalkWindows(g_windows, scan);
    else WalkHeap(scan);
    // keep the ones with a sane position block
    std::vector<uintptr_t> keep;
    for (uintptr_t o : objs) {
        float v[9];
        if (!Read(o + l.posOff, v, sizeof(v)) || !Finite3(v) || !Finite3(v + 6)) continue;
        if (std::fabs(v[0]) > 1e7f || std::fabs(v[1]) > 1e5f || std::fabs(v[2]) > 1e7f) continue;
        keep.push_back(o);
    }
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    if (!quick) {                                  // remember where they live
        g_windows.clear();
        for (uintptr_t o : keep) {
            uintptr_t w = o & ~(kWindow - 1);
            if (g_windows.empty() || g_windows.back() != w) g_windows.push_back(w);
        }
    }
    return keep;
}

// Does the enumerated set actually sit on the server's entities?
int Matches(const Layout& l, const std::vector<uintptr_t>& objs) {
    Hints h = GetHints();
    std::vector<std::array<float, 3>> targets = h.ents;
    targets.push_back({ h.me[0], h.me[1], h.me[2] });
    int hit = 0;
    for (auto& t : targets)
        for (uintptr_t o : objs) {
            float v[3];
            if (cent::ReadPos(o, l, v) && Dist(v, t.data()) < 1.5f) { ++hit; break; }
        }
    return hit;
}

// Your character is an object of the same type. While you stand still the
// server position is exact, and the one object on it (current tick slot) is
// you. Kept until it stops being valid (death, respawn, the GC moving it) or
// drifts from where the server has you.
void IdentifyPlayer(const Layout& l, const std::vector<uintptr_t>& objs) {
    Hints h = GetHints();
    if (!h.have) return;
    bool still = Dist(h.me, g_lastMe) < 0.02f;
    memcpy(g_lastMe, h.me, sizeof(g_lastMe));
    g_meStill = still ? g_meStill + 1 : 0;

    uintptr_t cur;
    { std::lock_guard<std::mutex> lk(g_mx); cur = g_player; }
    if (cur) {
        float v[9];
        uint64_t mt = 0;
        bool ok = Read(cur, &mt, sizeof(mt)) && (mt == l.mt || (l.mt2 && mt == l.mt2)) &&
                  Read(cur + l.posOff, v, sizeof(v)) && Finite3(v + 3) && Dist(v + 3, h.me) < 4.f;
        if (ok) return;
        log_::Info("[test] client entities: your object %p went away, finding it again", (void*)cur);
        std::lock_guard<std::mutex> lk(g_mx);
        g_player = 0;
    }
    if (g_meStill < 1) return;                      // wait for a still moment
    uintptr_t best = 0;
    int onYou = 0;
    for (uintptr_t o : objs) {
        float v[9];
        if (!Read(o + l.posOff, v, sizeof(v)) || !Finite3(v + 3)) continue;
        if (Dist(v + 3, h.me) < 0.05f) { best = o; ++onYou; }
    }
    if (onYou == 1) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_player = best;
        log_::Info("[test] client entities: you are object %p", (void*)best);
    }
}

void Loop() {
    if (!FindModule()) { SetStatus("client module not found"); return; }
    for (;;) {
        while (g_run && !GetHints().have) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!g_run) return;

        Layout l;
        bool cached = LoadCache(l);
        std::vector<uintptr_t> objs;
        if (cached) {
            SetStatus("checking the cached layout");
            objs = Enumerate(l, false);
            if (Matches(l, objs) < 1) { cached = false; l = Layout{}; }
            else log_::Info("[test] client entities: cached layout ok, %zu objects", objs.size());
        }
        if (!cached) {
            SetStatus("finding entities in the client");
            if (!Discover(l)) {
                SetStatus("client entities not found - using server positions");
                for (int i = 0; i < 100 && g_run; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!g_run) return;
                continue;
            }
            SaveCache(l);
            objs = Enumerate(l, false);
        }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_layout = l;
            g_objs = objs;
        }
        // Keep the list current: entities spawn, despawn, and the GC moves
        // objects. A refresh is one pass over the heap for the type pointer.
        int failures = 0;
        auto nextFull = GetTickCount64() + 15000;
        while (g_run) {
            char t[96];
            snprintf(t, sizeof(t), "reading %zu entities from the client", objs.size());
            SetStatus(t);
            for (int i = 0; i < 5 && g_run; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_run) return;
            auto t0 = GetTickCount64();
            bool full = t0 >= nextFull;
            if (full) nextFull = t0 + 15000;
            objs = Enumerate(l, !full);
            int m = Matches(l, objs);
            if (full)
                log_::Info("[test] client entities: %zu objects, %d on server entities (full pass %llu ms, %zu windows)",
                           objs.size(), m, (unsigned long long)(GetTickCount64() - t0), g_windows.size());
            { std::lock_guard<std::mutex> lk(g_mx); g_objs = objs; }
            IdentifyPlayer(l, objs);
            failures = m == 0 ? failures + 1 : 0;
            if (failures >= 3) {                 // layout no longer holds: find it again
                std::lock_guard<std::mutex> lk(g_mx);
                g_layout = Layout{};
                g_objs.clear();
                break;
            }
        }
    }
}

} // namespace

void Start(DWORD clientPid) {
    if (g_run || !clientPid) return;
    g_pid = clientPid;
    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, clientPid);
    if (!g_proc) { SetStatus("cannot read the client"); return; }
    { std::lock_guard<std::mutex> lk(g_mx); g_haveHint = false; g_layout = Layout{}; g_objs.clear(); }
    g_run = true;
    g_thread = std::thread(Loop);
}

void Stop() {
    if (!g_run) return;
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_layout = Layout{};
        g_objs.clear();
        g_player = 0;
    }
    if (g_proc) CloseHandle(g_proc);
    g_proc = nullptr;
    g_modBase = g_modEnd = 0;
    SetStatus("off");
}

void Hint(const float me[3], const std::vector<std::array<float, 3>>& ents) {
    std::lock_guard<std::mutex> lk(g_mx);
    memcpy(g_me, me, sizeof(g_me));
    g_ents = ents;
    g_haveHint = true;
}

bool Running() { return g_run; }

bool GetLayout(Layout& out) { std::lock_guard<std::mutex> lk(g_mx); out = g_layout; return out.ok; }
std::vector<uintptr_t> Objects() { std::lock_guard<std::mutex> lk(g_mx); return g_objs; }
uintptr_t PlayerObject() { std::lock_guard<std::mutex> lk(g_mx); return g_player; }
std::string Status() { std::lock_guard<std::mutex> lk(g_mx); return g_status; }

bool ReadPos(uintptr_t obj, const Layout& l, float out[3]) {
    if (!g_proc || !l.ok) return false;
    uint64_t mt = 0;
    if (!Read(obj, &mt, sizeof(mt)) || (mt != l.mt && (!l.mt2 || mt != l.mt2))) return false;   // moved or freed
    if (!Read(obj + l.posOff + kRenderSlot * 12, out, 12)) return false;
    return Finite3(out);
}

} // namespace cent
