#include "game.h"
#include "attach.h"
#include "camera.h"
#include "embedded.h"
#include "cent.h"
#include "ingame.h"
#include "log.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <algorithm>
#include <set>
#include <mutex>
#include <random>
#include <thread>

#include <timeapi.h>

namespace game {
namespace {

Config            g_cfg;
std::atomic<bool> g_run{false}, g_attached{false}, g_clientOnly{false};
std::thread       g_thread;
std::mutex        g_snapMx, g_qMx, g_statusMx, g_itemsMx, g_wpMx;
std::shared_ptr<const Snapshot> g_snap = std::make_shared<Snapshot>();
std::string       g_status = "waiting for game", g_result;
std::vector<std::string> g_queue, g_weathers;
std::vector<ItemInfo>    g_items;
std::vector<Waypoint>    g_waypoints;

void SetStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_statusMx); g_status = s; }
void SetResult(const std::string& s) { std::lock_guard<std::mutex> lk(g_statusMx); g_result = s; }

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
//  Finding the single-player server
// ---------------------------------------------------------------------------
std::wstring CommandLineOf(DWORD pid) {
    using NtQIP = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto q = (NtQIP)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h || !q) { if (h) CloseHandle(h); return out; }
    ULONG len = 0;
    q(h, 60 /*ProcessCommandLineInformation*/, nullptr, 0, &len);
    if (len > sizeof(UNICODE_STRING) && len < (1 << 20)) {
        std::vector<BYTE> buf(len);
        if (q(h, 60, buf.data(), len, &len) >= 0) {
            auto* us = (UNICODE_STRING*)buf.data();
            out.assign(us->Buffer, us->Length / sizeof(wchar_t));
        }
    }
    CloseHandle(h);
    return out;
}

std::wstring ExeName(DWORD pid) {
    std::wstring name;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == pid) { name = pe.szExeFile; break; }
    CloseHandle(snap);
    return name;
}

struct Server { DWORD pid = 0, clientPid = 0; };

// Any running HytaleClient.exe (for client-only mode when no local server).
DWORD FindClient() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD found = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"HytaleClient.exe") == 0) { found = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return found;
}

// A java.exe running HytaleServer.jar with --singleplayer whose --client-pid is a live HytaleClient.exe.
Server FindServer() {
    Server s;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return s;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"java.exe") != 0) continue;
        std::wstring cl = CommandLineOf(pe.th32ProcessID);
        if (cl.find(L"HytaleServer.jar") == std::wstring::npos) continue;
        if (cl.find(L" --singleplayer") == std::wstring::npos) continue;
        size_t at = cl.find(L"--client-pid ");
        if (at == std::wstring::npos) continue;
        DWORD client = (DWORD)_wtoi(cl.c_str() + at + 13);
        if (_wcsicmp(ExeName(client).c_str(), L"HytaleClient.exe") != 0) continue;
        s.pid = pe.th32ProcessID; s.clientPid = client;
        break;
    }
    CloseHandle(snap);
    return s;
}

struct FindWnd { DWORD pid; HWND best; };
BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* f = (FindWnd*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->best = h; return FALSE; }
    return TRUE;
}
HWND WindowOf(DWORD pid) { FindWnd f{ pid, nullptr }; EnumWindows(EnumProc, (LPARAM)&f); return f.best; }

bool Alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// ---------------------------------------------------------------------------
//  Agent link (loopback TCP, one request line -> one reply line)
// ---------------------------------------------------------------------------
class Link {
public:
    ~Link() { Close(); }
    bool Connect(int port, const std::string& token) {
        Close();
        s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_ == INVALID_SOCKET) return false;
        DWORD to = 5000;
        setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
        setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
        BOOL nd = TRUE;
        setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s_, (sockaddr*)&a, sizeof(a)) != 0) { Close(); return false; }
        std::string r;
        return Call("HELLO " + token, r) && r.rfind("OK", 0) == 0;
    }
    bool Call(const std::string& req, std::string& reply) {
        if (s_ == INVALID_SOCKET) return false;
        std::string line = req + "\n";
        if (send(s_, line.data(), (int)line.size(), 0) != (int)line.size()) { Close(); return false; }
        for (;;) {
            size_t nl = buf_.find('\n');
            if (nl != std::string::npos) { reply = buf_.substr(0, nl); buf_.erase(0, nl + 1); return true; }
            char tmp[65536];
            int n = recv(s_, tmp, sizeof(tmp), 0);
            if (n <= 0) { Close(); return false; }
            buf_.append(tmp, n);
        }
    }
    bool Open() const { return s_ != INVALID_SOCKET; }
    void Close() { if (s_ != INVALID_SOCKET) closesocket(s_); s_ = INVALID_SOCKET; buf_.clear(); }
private:
    SOCKET s_ = INVALID_SOCKET;
    std::string buf_;
};

Link   g_link;
Server g_server;
std::map<std::string, std::string> g_sent;   // last value pushed per SET key

std::string RandomToken() {
    std::random_device rd;
    char t[33];
    snprintf(t, sizeof(t), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    return t;
}

// Load the agent and open the link. Returns false with the status set on failure.
bool Connect(const Server& srv) {
    // The agent ships inside the exe (embedded.h). The JVM keeps loaded jars
    // open until it exits, so every attach gets its own fresh copy.
    std::string token = RandomToken(), err;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::error_code ec;
    auto work = std::filesystem::path(tmp) / L"hytrainer" / std::wstring(token.begin(), token.begin() + 8);
    std::filesystem::create_directories(work, ec);
    auto agentJar = work / L"hytrainer-agent.jar", coreJar = work / L"hytrainer-core.jar";
    auto dir = ExeDir() / L"agent";
    if (!embedded::Stage(embedded::kAgentJar, dir / L"hytrainer-agent.jar", agentJar) ||
        !embedded::Stage(embedded::kCoreJar, dir / L"hytrainer-core.jar", coreJar)) {
        SetStatus("cannot unpack the agent");
        return false;
    }
    // copies from earlier sessions (no longer open once that game closed)
    for (auto& old : std::filesystem::directory_iterator(work.parent_path(), ec))
        if (old.path() != work) std::filesystem::remove_all(old.path(), ec);
    auto statusFile = work / L"status.txt";
    std::string opts = Narrow(coreJar.wstring()) + "|" + Narrow(statusFile.wstring()) + "|" + token;
    log_::Info("attaching to server pid %lu (client %lu)", srv.pid, srv.clientPid);
    if (!attach::LoadAgent(srv.pid, Narrow(agentJar.wstring()), opts, err)) {
        log_::Error("attach failed: %s", err.c_str());
        SetStatus("attach failed: " + err);
        return false;
    }
    // The core writes "PORT n" (or "ERR reason") once it is listening.
    std::string st;
    for (int i = 0; i < 100 && st.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (FILE* f = _wfopen(statusFile.c_str(), L"r")) {
            char line[512] = {};
            fgets(line, sizeof(line), f);
            fclose(f);
            st = line;
        }
    }
    std::filesystem::remove(statusFile, ec);
    if (st.rfind("PORT ", 0) != 0) {
        log_::Error("agent status: '%s'", st.c_str());
        SetStatus(st.empty() ? "agent did not start" : st.substr(st.rfind("ERR ", 0) == 0 ? 4 : 0));
        return false;
    }
    if (!g_link.Connect(atoi(st.c_str() + 5), token)) { SetStatus("agent link failed"); return false; }

    auto fetch = [](const char* what, std::vector<std::string>& into) {
        std::string r, head = std::string(what) + " ";
        if (!g_link.Call(what, r) || r.rfind(head, 0) != 0) return;
        std::vector<std::string> v;
        size_t p = head.size();
        while (p < r.size()) {
            size_t t = r.find('\t', p);
            if (t == std::string::npos) t = r.size();
            if (t > p) v.push_back(r.substr(p, t - p));
            p = t + 1;
        }
        std::lock_guard<std::mutex> lk(g_itemsMx);
        into.swap(v);
    };
    {
        // id <US> name <US> category <US> sub-category <US> type <US> quality <US> max stack
        std::vector<std::string> raw;
        fetch("ITEMS", raw);
        std::vector<ItemInfo> items;
        items.reserve(raw.size());
        for (auto& r : raw) {
            ItemInfo it;
            std::string* fields[] = { &it.id, &it.name, &it.category, &it.subCategory, &it.kind, &it.quality };
            size_t p = 0, k = 0;
            while (k < 6) {
                size_t e = r.find('\x1f', p);
                *fields[k++] = r.substr(p, e == std::string::npos ? std::string::npos : e - p);
                if (e == std::string::npos) { p = r.size(); break; }
                p = e + 1;
            }
            if (p < r.size()) it.maxStack = (std::max)(1, atoi(r.c_str() + p));
            if (it.name.empty()) it.name = it.id;
            if (it.kind.empty()) it.kind = "Items";
            if (!it.id.empty()) items.push_back(std::move(it));
        }
        std::lock_guard<std::mutex> lk(g_itemsMx);
        g_items.swap(items);
    }
    fetch("WEATHERS", g_weathers);
    g_sent.clear();
    log_::Info("agent connected (%zu items)", g_items.size());
    return true;
}

// ---------------------------------------------------------------------------
//  Sync: push feature values that changed
// ---------------------------------------------------------------------------
void Push(const char* key, const std::string& v) {
    auto it = g_sent.find(key);
    if (it != g_sent.end() && it->second == v) return;
    std::string r;
    if (g_link.Call(std::string("SET ") + key + " " + v, r)) {
        g_sent[key] = v;
        log_::Info("[test] %s=%s", key, v.c_str());
    }
}
std::string B(const std::atomic<bool>& b) { return b ? "1" : "0"; }
std::string B(bool b) { return b ? "1" : "0"; }
std::string F(float f) { char t[32]; snprintf(t, sizeof(t), "%.3f", f); return t; }

// Features that change the world for everybody in it. In a world you have
// opened to other players they stay off until the host allows them, and the
// agent enforces the same rule on its side.
std::atomic<bool> g_sharedLocked{false};

void PushAll() {
    auto& c = g_cfg;
    bool lock = g_sharedLocked.load();
    Push("god", B(c.god));           Push("infhp", B(c.infHp));     Push("infstam", B(c.infStam));
    Push("infmana", B(c.infMana));   Push("infoxy", B(c.infOxy));   Push("nofall", B(c.noFall));
    Push("fly", B(c.fly));           Push("noclip", B(c.noclip));   Push("infdur", B(c.infDur));
    Push("infammo", B(c.infAmmo));   Push("freezemobs", B(c.freezeMobs && !lock));
    Push("freezetime", B(c.freezeTime && !lock)); Push("onehit", B(c.oneHit));
    Push("espplayers", B(c.espPlayers));
    Push("allowshared", B(c.allowShared));
    Push("autoattack", B(c.autoAttack && !lock));
    Push("aafront", B(c.aaFrontOnly));
    Push("tgthostile", B(c.tgtHostile)); Push("tgtneutral", B(c.tgtNeutral));
    Push("tgtfriendly", B(c.tgtFriendly)); Push("tgtplayers", B(c.tgtPlayers));
    Push("swingaura", B(c.swingAura && !lock)); Push("swingreach", B(c.swingReach && !lock));
    Push("ghosthit", B(c.ghostHit && !lock));
    Push("reachrange", F(c.reachRange)); Push("ghostrange", F(c.ghostRange)); Push("auramax", F(c.auraMax));
    Push("instabreak", B(c.instantBreak)); Push("breakrange", F(c.breakRange));
    Push("vacuum", B(c.vacuum && !lock)); Push("vacuumradius", F(c.vacuumRadius));
    Push("autodupe", B(c.autoDupe)); Push("dupemult", F(c.dupeMult));
    Push("fastpickup", B(c.fastPickup));
    Push("projaim", B(c.projAim && !lock)); Push("magicbullet", B(c.magicBullet && !lock));
    Push("projrange", F(c.projRange)); Push("projcone", F(c.projCone));
    Push("aaradius", F(c.aaRadius));
    Push("aarate", F(c.aaRate));
    Push("aadamage", F(c.aaDamage));
    Push("speed", F(c.speedOn ? c.speed.load() : 1.f));
    Push("jump", F(c.jumpOn ? c.jump.load() : 1.f));
    Push("flyspeed", F(c.flySpeedOn ? c.flySpeed.load() : 1.f));
    Push("dmgmult", F(c.dmgOn ? c.dmgMult.load() : 1.f));
    Push("esprange", F(c.espMaxDist));
    Push("espitems", B(c.espItems));
}

// ---------------------------------------------------------------------------
//  Parsing replies
// ---------------------------------------------------------------------------
std::vector<std::string> Split(const std::string& s, char sep, size_t from = 0) {
    std::vector<std::string> v;
    size_t p = from;
    while (p <= s.size()) {
        size_t t = s.find(sep, p);
        if (t == std::string::npos) t = s.size();
        if (t > p) v.push_back(s.substr(p, t - p));
        p = t + 1;
    }
    return v;
}

void ParseState(const std::string& r, Snapshot& s) {
    std::map<std::string, std::string> kv;
    for (auto& f : Split(r, '\t', 6)) {
        size_t e = f.find('=');
        if (e != std::string::npos) kv[f.substr(0, e)] = f.substr(e + 1);
    }
    auto num = [&](const char* k, float d = 0.f) { auto it = kv.find(k); return it == kv.end() ? d : (float)atof(it->second.c_str()); };
    s.havePlayer = kv["player"] == "1";
    s.pos = { num("x"), num("y"), num("z") };
    s.pitch = num("pitch"); s.yaw = num("yaw"); s.eye = num("eye", 1.6f);
    s.hp = num("hp"); s.hpMax = num("hpmax"); s.stam = num("stam"); s.stamMax = num("stammax");
    s.mana = num("mana"); s.manaMax = num("manamax"); s.oxy = num("oxy"); s.oxyMax = num("oxymax");
    s.dayProgress = num("day"); s.mode = kv["mode"]; s.world = kv["world"];
    s.filterOk = kv["filter"] != "0";
    s.seq = kv.count("seq") ? strtoull(kv["seq"].c_str(), nullptr, 10) : 0;
    s.players = (int)num("players", 1);
    s.swings = (long long)num("swings"); s.ghosted = (long long)num("ghosted"); s.hits = (long long)num("aahits");
    s.broken = (long long)num("broken");
    s.duped = (long long)num("duped"); s.pulled = (long long)num("pulled");
    s.homed = (long long)num("homed"); s.bullets = (long long)num("bullets");
    s.shared = s.players > 1;
}

void ParseEnts(const std::string& r, Snapshot& s) {
    s.ents.clear();
    for (auto& e : Split(r, '\t', 5)) {
        // type,id,x,y,z,height,hp,max,name
        auto f = Split(e, ',');
        if (f.size() < 9) continue;
        Entity en;
        en.kind = (Kind)f[0][0];
        en.id = atoi(f[1].c_str());
        en.pos = { (float)atof(f[2].c_str()), (float)atof(f[3].c_str()), (float)atof(f[4].c_str()) };
        en.height = (float)atof(f[5].c_str());
        en.hp = (float)atof(f[6].c_str()); en.maxHp = (float)atof(f[7].c_str());
        en.name = f[8];
        s.ents.push_back(std::move(en));
    }
}

void Disconnect(const char* why) {
    g_clientOnly = false;
    ingame::Stop();          // out of the client before anything else
    cam::Stop();
    cent::Stop();
    g_sharedLocked = false;
    if (g_link.Open()) { std::string r; g_link.Call("BYE", r); }
    g_link.Close();
    g_attached = false;
    { std::lock_guard<std::mutex> lk(g_snapMx); g_snap = std::make_shared<Snapshot>(); }
    log_::Info("detached: %s", why);
}

void Loop() {
    using namespace std::chrono;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // 1 ms timer resolution so the short poll sleep below is really short:
    // the default 15.6 ms tick would round it up to a whole server tick.
    timeBeginPeriod(1);
    uint64_t lastSeq = ~0ull;
    double   lastSampleTime = 0;
    HWND     gameWnd = nullptr;
    auto nextTry = steady_clock::now();
    auto nextLog = steady_clock::now();
    int failures = 0;
    while (g_run) {
        if (!g_attached) {
            if (steady_clock::now() < nextTry) { std::this_thread::sleep_for(milliseconds(50)); continue; }
            nextTry = steady_clock::now() + seconds(2);
            Server srv = FindServer();
            // No local server: the client is in a menu, or you joined someone
            // else's world. Report that distinctly instead of the generic wait,
            // because nothing here is broken and nothing can be done about it.
            //
            // Why no features work without the local server, measured on 0.7.0:
            // the client does hold live entity positions (the ESP's refinement
            // path reads them), but the ESP draws the server's entity records --
            // kind, name, HP and height exist only there. And the camera, which
            // every projection needs, is found by scoring candidate matrices
            // against the server's eye and facing; its cache is keyed to the
            // process instance, so it cannot carry over from a single-player
            // session. Positions alone cannot be drawn.
            g_clientOnly = !srv.pid && FindClient() != 0;
            if (!srv.pid) {
                SetStatus(g_clientOnly ? "no local server - online or in a menu, features unavailable"
                                       : "waiting for a single-player world");
                failures = 0;
                continue;
            }
            if (failures >= 3) { nextTry = steady_clock::now() + seconds(15); }   // back off, keep the reason visible
            if (!Connect(srv)) { ++failures; continue; }
            failures = 0;
            g_server = srv;
            g_attached = true;
            SetStatus("attached");
            LoadConfig();
            if (g_cfg.useClientCam) cam::Start(srv.clientPid);
            continue;
        }
        if (!Alive(g_server.pid)) { Disconnect("world closed"); SetStatus("waiting for a single-player world"); continue; }

        std::vector<std::string> q;
        { std::lock_guard<std::mutex> lk(g_qMx); q.swap(g_queue); }
        PushAll();
        for (auto& a : q) {
            std::string r;
            if (g_link.Call("DO " + a, r)) { SetResult(r.size() > 3 ? r.substr(3) : r); log_::Info("[test] DO %s -> %s", a.c_str(), r.c_str()); }
        }

        Snapshot s;
        std::string st, en;
        if (!g_link.Call("STATE", st) || !g_link.Call("ENTS", en)) {
            Disconnect("link lost");
            SetStatus("reconnecting");
            nextTry = steady_clock::now() + seconds(1);
            continue;
        }
        s.attached = true;
        ParseState(st, s);
        ParseEnts(en, s);
        g_sharedLocked = s.shared && !g_cfg.allowShared.load();
        // Finding the window enumerates every top-level window; only redo
        // it when the cached handle stops being valid.
        if (!gameWnd || !IsWindow(gameWnd)) gameWnd = WindowOf(g_server.clientPid);
        if (HWND w = gameWnd) {
            RECT rc; POINT o{ 0, 0 };
            GetClientRect(w, &rc);
            ClientToScreen(w, &o);
            s.gameX = o.x; s.gameY = o.y; s.gameW = rc.right; s.gameH = rc.bottom;
        }
        if (s.seq != lastSeq || s.seq == 0) { lastSeq = s.seq; lastSampleTime = NowSeconds(); }
        s.sampleTime = lastSampleTime;
        if (steady_clock::now() >= nextLog) {
            nextLog = steady_clock::now() + seconds(5);
            log_::Info("[test] player=%d pos=(%.1f %.1f %.1f) hp=%.0f/%.0f stam=%.0f/%.0f mode=%s ents=%zu filter=%d",
                s.havePlayer, s.pos.x, s.pos.y, s.pos.z, s.hp, s.hpMax, s.stam, s.stamMax, s.mode.c_str(), s.ents.size(), s.filterOk);
            if (cam::Running()) log_::Info("[test] camera reads: %s  (%s)", cam::TakeStats().c_str(), cam::Status().c_str());
            if (ingame::Running()) log_::Info("[test] in-game renderer: %s", ingame::TakeStats().c_str());
            log_::Info("[test] client entities: %s", cent::Status().c_str());
        }
        // In-game renderer: loaded only while attached, and only when wanted.
        // A failed start is not retried until the switch is toggled.
        {
            static bool failed = false, lastWant = false;
            bool want = g_cfg.inGameEsp.load();
            if (want != lastWant) { failed = false; lastWant = want; }
            if (want && !failed && !ingame::Running()) failed = !ingame::Start(g_server.clientPid);
            else if (!want && ingame::Running()) ingame::Stop();
        }
        // Client entity objects: started with the session, fed the server's
        // positions so it can find and verify them.
        if (!cent::Running()) cent::Start(g_server.clientPid);
        if (s.havePlayer) {
            float me[3] = { s.pos.x, s.pos.y, s.pos.z };
            std::vector<std::array<float, 3>> ep;
            ep.reserve(s.ents.size());
            for (auto& e : s.ents) ep.push_back({ e.pos.x, e.pos.y, e.pos.z });
            cent::Hint(me, ep);
        }
        // The camera scanner needs to know where the server thinks we are.
        if (g_cfg.useClientCam) {
            if (!cam::Running()) cam::Start(g_server.clientPid);
            if (s.havePlayer) {
                float eye[3] = { s.pos.x, s.pos.y + s.eye, s.pos.z };
                cam::Hint(eye, s.pitch, s.yaw, s.gameW, s.gameH);
            }
        } else if (cam::Running()) {
            cam::Stop();
        }
        auto ptr = std::make_shared<const Snapshot>(std::move(s));
        { std::lock_guard<std::mutex> lk(g_snapMx); g_snap = std::move(ptr); }
        // Poll well above the 30 Hz tick so a new tick is picked up within a
        // few milliseconds; sampleTime is only as accurate as this.
        std::this_thread::sleep_for(milliseconds(4));
    }
    timeEndPeriod(1);
    if (g_attached) Disconnect("trainer exit");
    WSACleanup();
}

std::filesystem::path ConfigPath() { return ExeDir() / L"hytale_trainer.cfg"; }

struct BoolKey { const char* k; std::atomic<bool>* v; };
struct FloatKey { const char* k; std::atomic<float>* v; };
std::vector<BoolKey> BoolKeys() {
    auto& c = g_cfg;
    return { {"god",&c.god},{"infHp",&c.infHp},{"infStam",&c.infStam},{"infMana",&c.infMana},{"infOxy",&c.infOxy},
             {"noFall",&c.noFall},{"speedOn",&c.speedOn},{"jumpOn",&c.jumpOn},{"fly",&c.fly},{"flySpeedOn",&c.flySpeedOn},
             {"oneHit",&c.oneHit},{"dmgOn",&c.dmgOn},{"infDur",&c.infDur},{"infAmmo",&c.infAmmo},
             {"esp",&c.esp},{"espHostile",&c.espHostile},{"espNeutral",&c.espNeutral},{"espFriendly",&c.espFriendly},
             {"espItems",&c.espItems},{"espPlayers",&c.espPlayers},{"espLines",&c.espLines},
             {"espInfo",&c.espInfo},{"espHealth",&c.espHealth},{"espSmooth",&c.espSmooth},{"inGameEsp",&c.inGameEsp},{"espServerFallback",&c.espServerFallback},{"useClientCam",&c.useClientCam},
             {"aaFrontOnly",&c.aaFrontOnly},{"tgtHostile",&c.tgtHostile},{"tgtNeutral",&c.tgtNeutral},
             {"tgtFriendly",&c.tgtFriendly},{"statsHud",&c.statsHud},{"fastPickup",&c.fastPickup} };
    // tgtPlayers and the swing features are not persisted either: they start off
    // autoAttack, allowShared, noclip, freezeMobs and freezeTime are deliberately
    // not persisted: they start off every session
    // noclip, freezeMobs and freezeTime are deliberately not persisted: they start off every session
}
std::vector<FloatKey> FloatKeys() {
    auto& c = g_cfg;
    return { {"speed",&c.speed},{"jump",&c.jump},{"flySpeed",&c.flySpeed},{"dmgMult",&c.dmgMult},
             {"killRadius",&c.killRadius},{"espMaxDist",&c.espMaxDist},
             {"reachRange",&c.reachRange},{"ghostRange",&c.ghostRange},{"auraMax",&c.auraMax},{"breakRange",&c.breakRange},
             {"vacuumRadius",&c.vacuumRadius},{"dupeMult",&c.dupeMult},
             {"projRange",&c.projRange},{"projCone",&c.projCone},
             {"aaRadius",&c.aaRadius},{"aaRate",&c.aaRate},{"aaDamage",&c.aaDamage} };
}

// FieldOfView from the game's own settings, used as the ESP default.
void ReadGameFov() {
    std::filesystem::path p = L"D:\\Games\\Hytale_Game\\UserData\\Settings.json";
    // derive from the running client's --user-dir when available
    if (g_server.clientPid) {
        std::wstring cl = CommandLineOf(g_server.clientPid);
        size_t at = cl.find(L"--user-dir ");
        if (at != std::wstring::npos) {
            size_t b = at + 11, e = cl.find(L" --", b);
            std::wstring dir = cl.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
            if (!dir.empty() && dir.front() == L'"') dir = dir.substr(1, dir.find(L'"', 1) - 1);
            p = std::filesystem::path(dir) / L"Settings.json";
        }
    }
    FILE* f = _wfopen(p.c_str(), L"r");
    if (!f) return;
    char buf[16384]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    if (const char* k = strstr(buf, "\"FieldOfView\"")) if (const char* c = strchr(k, ':')) {
        float v = (float)atof(c + 1);
        if (v >= 30 && v <= 150) g_cfg.fov = v;
    }
}

} // namespace

Config& Cfg() { return g_cfg; }

void StartWorker() {
    if (g_run) return;
    g_run = true;
    g_thread = std::thread(Loop);
}

void StopWorker() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}

bool Attached() { return g_attached; }
bool ClientOnly() { return g_clientOnly; }
unsigned long ClientPid() { return g_attached ? g_server.clientPid : 0; }
bool SharedLocked() { return g_sharedLocked.load(); }
std::string Status() { std::lock_guard<std::mutex> lk(g_statusMx); return g_status; }
std::string LastResult() { std::lock_guard<std::mutex> lk(g_statusMx); return g_result; }
Snapshot GetSnapshot() { std::lock_guard<std::mutex> lk(g_snapMx); return *g_snap; }
std::shared_ptr<const Snapshot> SnapshotPtr() { std::lock_guard<std::mutex> lk(g_snapMx); return g_snap; }
double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
std::vector<ItemInfo> Items() { std::lock_guard<std::mutex> lk(g_itemsMx); return g_items; }
std::vector<std::string> Weathers() { std::lock_guard<std::mutex> lk(g_itemsMx); return g_weathers; }

// Actions that change the world for everyone in it.
bool SharedAction(const std::string& cmd) {
    static const char* kShared[] = { "time ", "weather ", "killmobs" };
    for (const char* k : kShared) if (cmd.rfind(k, 0) == 0) return true;
    return false;
}

void Action(const std::string& cmd) {
    if (!g_attached) { SetResult("Not attached."); return; }
    if (g_sharedLocked.load() && SharedAction(cmd)) {
        SetResult("Other players are in your world. Allow shared-world features on the SETTINGS tab first.");
        return;
    }
    std::lock_guard<std::mutex> lk(g_qMx);
    g_queue.push_back(cmd);
}

std::vector<Waypoint> Waypoints() { std::lock_guard<std::mutex> lk(g_wpMx); return g_waypoints; }
void SaveWaypoint(const std::string& name) {
    Snapshot s = GetSnapshot();
    if (!s.havePlayer) { SetResult("No player."); return; }
    { std::lock_guard<std::mutex> lk(g_wpMx); g_waypoints.push_back({ name.empty() ? "Waypoint" : name, s.pos }); }
    SaveConfig();
    SetResult("Position saved.");
}
void DeleteWaypoint(size_t i) {
    { std::lock_guard<std::mutex> lk(g_wpMx); if (i < g_waypoints.size()) g_waypoints.erase(g_waypoints.begin() + i); }
    SaveConfig();
}
void GotoWaypoint(size_t i) {
    Waypoint w;
    { std::lock_guard<std::mutex> lk(g_wpMx); if (i >= g_waypoints.size()) return; w = g_waypoints[i]; }
    char t[128];
    snprintf(t, sizeof(t), "tp %.3f %.3f %.3f", w.pos.x, w.pos.y, w.pos.z);
    Action(t);
}

std::string SaveConfig() {
    FILE* f = _wfopen(ConfigPath().c_str(), L"w");
    if (!f) return "Could not write settings file.";
    for (auto& b : BoolKeys()) fprintf(f, "%s=%d\n", b.k, b.v->load() ? 1 : 0);
    for (auto& k : FloatKeys()) fprintf(f, "%s=%g\n", k.k, k.v->load());
    {
        std::lock_guard<std::mutex> lk(g_wpMx);
        for (auto& w : g_waypoints) fprintf(f, "wp=%.3f,%.3f,%.3f,%s\n", w.pos.x, w.pos.y, w.pos.z, w.name.c_str());
    }
    fclose(f);
    return "Settings saved.";
}

std::string LoadConfig() {
    ReadGameFov();
    FILE* f = _wfopen(ConfigPath().c_str(), L"r");
    if (!f) return "No saved settings yet.";
    char line[512];
    auto bk = BoolKeys(); auto fk = FloatKeys();
    std::vector<Waypoint> wps;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* val = eq + 1;
        for (auto& b : bk) if (strcmp(line, b.k) == 0) b.v->store(atoi(val) != 0);
        for (auto& k : fk) if (strcmp(line, k.k) == 0) k.v->store((float)atof(val));
        if (strcmp(line, "wp") == 0) {
            Waypoint w; char name[256] = {};
            if (sscanf_s(val, "%f,%f,%f,%255[^\n]", &w.pos.x, &w.pos.y, &w.pos.z, name, (unsigned)sizeof(name)) >= 3) {
                w.name = name; wps.push_back(w);
            }
        }
    }
    fclose(f);
    { std::lock_guard<std::mutex> lk(g_wpMx); g_waypoints.swap(wps); }
    return "Settings loaded.";
}

// Attach, read one snapshot, detach. Pushes no feature values (all stay at the agent's off defaults).
// With a script, each line is sent to the agent verbatim ("WAIT ms" sleeps) and the replies logged.
int SelfTest(const wchar_t* path, const wchar_t* script) {
    FILE* f = _wfopen(path, L"w");
    if (!f) return 3;
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    int rc = 0;
    Server srv = FindServer();
    if (!srv.pid) { fprintf(f, "FAIL no single-player server running\n"); rc = 1; }
    else if (!Connect(srv)) { fprintf(f, "FAIL connect: %s\n", Status().c_str()); rc = 2; }
    else {
        fprintf(f, "OK attached server=%lu client=%lu items=%zu\n", srv.pid, srv.clientPid, g_items.size());
        std::string st, en;
        g_link.Call("STATE", st);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        g_link.Call("STATE", st);
        g_link.Call("ENTS", en);
        fprintf(f, "%s\n", st.c_str());
        Snapshot s; ParseEnts(en, s);
        fprintf(f, "ents=%zu\n", s.ents.size());
        for (size_t i = 0; i < s.ents.size() && i < 15; ++i) {
            auto& e = s.ents[i];
            fprintf(f, "  %c %-28s (%.1f %.1f %.1f) h=%.2f hp=%.0f/%.0f\n", (char)e.kind, e.name.c_str(), e.pos.x, e.pos.y, e.pos.z, e.height, e.hp, e.maxHp);
        }
        if (script) if (FILE* sf = _wfopen(script, L"r")) {
            char line[512];
            while (fgets(line, sizeof(line), sf)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!line[0] || line[0] == '#') continue;
                if (strncmp(line, "WAIT ", 5) == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(atoi(line + 5))); continue; }
                std::string r;
                bool ok = g_link.Call(line, r);
                if (r.size() > 400) r = r.substr(0, 400) + "...";
                fprintf(f, "> %s\n< %s\n", line, ok ? r.c_str() : "(link failed)");
                fflush(f);
            }
            fclose(sf);
        }
        // Client camera: feed the scanner the server view for a while and
        // report what it locked onto.
        g_server = srv;
        cam::Start(srv.clientPid);
        for (int i = 0; i < 160 && !cam::Running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        cam::View v{};
        bool locked = false;
        for (int i = 0; i < 1200; ++i) {         // up to 60 s
            std::string line;
            if (!g_link.Call("STATE", line)) break;
            Snapshot cs;
            ParseState(line, cs);
            if (HWND w = WindowOf(srv.clientPid)) {
                RECT rc; POINT o{ 0, 0 };
                GetClientRect(w, &rc);
                ClientToScreen(w, &o);
                cs.gameW = rc.right; cs.gameH = rc.bottom;
            }
            if (cs.havePlayer && cs.gameW > 0) {
                float eye[3] = { cs.pos.x, cs.pos.y + cs.eye, cs.pos.z };
                cam::Hint(eye, cs.pitch, cs.yaw, cs.gameW, cs.gameH);
            }
            if (cam::Read(v) && v.valid) { locked = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (locked) {
            // Hold for a while: the camera-position hunt needs a few seconds,
            // and turning the mouse while standing still is what settles it.
            for (int i = 0; i < 300; ++i) {          // 15 s
                std::string line;
                if (!g_link.Call("STATE", line)) break;
                Snapshot cs;
                ParseState(line, cs);
                if (HWND w = WindowOf(srv.clientPid)) {
                    RECT rc; POINT o{ 0, 0 };
                    GetClientRect(w, &rc);
                    ClientToScreen(w, &o);
                    cs.gameW = rc.right; cs.gameH = rc.bottom;
                }
                if (cs.havePlayer && cs.gameW > 0) {
                    float eye[3] = { cs.pos.x, cs.pos.y + cs.eye, cs.pos.z };
                    cam::Hint(eye, cs.pitch, cs.yaw, cs.gameW, cs.gameH);
                }
                cam::Read(v);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        fprintf(f, "camera: %s (%s)\n", locked ? "LOCKED" : "not locked", cam::Status().c_str());
        if (locked) {
            fprintf(f, "  fovY=%.2f deg aspect=%.4f layout=%d relative=%d xSign=%+.0f ySign=%+.0f\n",
                    v.fovY * 180.f / 3.14159265f, v.aspect, v.layout, (int)v.relative, v.xSign, v.ySign);
            fprintf(f, "  camera eye=(%.2f %.2f %.2f) fwd=(%.3f %.3f %.3f) origin=(%.2f %.2f %.2f)\n",
                    v.eye[0], v.eye[1], v.eye[2], v.fwd[0], v.fwd[1], v.fwd[2],
                    v.origin[0], v.origin[1], v.origin[2]);
            std::string line;
            if (g_link.Call("STATE", line)) {
                Snapshot cs; ParseState(line, cs);
                fprintf(f, "  server eye=(%.2f %.2f %.2f) boom=%.2f\n", cs.pos.x, cs.pos.y + cs.eye, cs.pos.z,
                        std::sqrt((v.eye[0] - cs.pos.x) * (v.eye[0] - cs.pos.x)
                                + (v.eye[1] - cs.pos.y - cs.eye) * (v.eye[1] - cs.pos.y - cs.eye)
                                + (v.eye[2] - cs.pos.z) * (v.eye[2] - cs.pos.z)));
            }
            // Where the first few entities land on screen, for eyeballing.
            std::string en2;
            if (g_link.Call("ENTS", en2)) {
                Snapshot es; ParseEnts(en2, es);
                int shown = 0;
                for (auto& e : es.ents) {
                    float nx, ny;
                    if (!cam::Project(v, e.pos.x, e.pos.y + e.height * 0.5f, e.pos.z, nx, ny)) continue;
                    fprintf(f, "  on screen: %-24s ndc=(%+.3f %+.3f)%s\n", e.name.c_str(), nx, ny,
                            std::fabs(nx) <= 1.f && std::fabs(ny) <= 1.f ? "" : "  (off view)");
                    if (++shown >= 8) break;
                }
            }
        }
        cam::Stop();
        fflush(f);

        std::string r; g_link.Call("BYE", r);
        fprintf(f, "BYE -> %s\n", r.c_str());
        g_link.Close();
    }
    WSACleanup();
    fclose(f);
    return rc;
}

// ---------------------------------------------------------------------------
//  EntProbe: find where the client keeps entity positions.
//
//  Read-only on the client. Takes the server's entity list (exact positions),
//  scans the client's writable memory for double3 / float3 values that sit on
//  those positions, then looks backwards from every hit for a pointer into
//  HytaleClient.exe's image (a NativeAOT MethodTable sits at offset 0 of
//  every object). A (distance, pointer) pair shared by many different
//  entities is the entity type and the position field offset.
// ---------------------------------------------------------------------------
int EntProbe(const wchar_t* path) {
    FILE* f = _wfopen(path, L"w");
    if (!f) return 3;
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    Server srv = FindServer();
    if (!srv.pid || !Connect(srv)) { fprintf(f, "FAIL attach\n"); fclose(f); WSACleanup(); return 1; }

    auto fetch = [&](Snapshot& sn) {
        std::string st, en;
        g_link.Call("STATE", st);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        g_link.Call("STATE", st);
        g_link.Call("ENTS", en);
        ParseState(st, sn);
        ParseEnts(en, sn);
    };
    Snapshot s0;
    fetch(s0);
    std::vector<Entity> ents = s0.ents;
    // the local player too: feet position
    Entity me{}; me.kind = Kind::Player; me.id = -1; me.pos = s0.pos; me.name = "<you>";
    ents.push_back(me);
    fprintf(f, "server entities: %zu (you at %.2f %.2f %.2f)\n", ents.size(), s0.pos.x, s0.pos.y, s0.pos.z);

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, srv.clientPid);
    if (!h) { fprintf(f, "FAIL open client\n"); fclose(f); return 2; }
    uintptr_t modBase = 0, modEnd = 0;
    {
        HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, srv.clientPid);
        MODULEENTRY32W me32{ sizeof(me32) };
        for (BOOL ok = Module32FirstW(sn, &me32); ok; ok = Module32NextW(sn, &me32))
            if (_wcsicmp(me32.szModule, L"HytaleClient.exe") == 0) { modBase = (uintptr_t)me32.modBaseAddr; modEnd = modBase + me32.modBaseSize; break; }
        CloseHandle(sn);
    }
    fprintf(f, "HytaleClient.exe image %p..%p\n", (void*)modBase, (void*)modEnd);

    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (auto& e : ents) {
        const float p[3] = { e.pos.x, e.pos.y, e.pos.z };
        for (int i = 0; i < 3; ++i) { lo[i] = (std::min)(lo[i], p[i] - 0.6f); hi[i] = (std::max)(hi[i], p[i] + 0.6f); }
    }
    struct Hit { uintptr_t addr; int ent; bool dbl; float d; };
    std::vector<Hit> hits;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t scanned = 0;
    auto t0 = GetTickCount64();
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; VirtualQueryEx(h, (LPCVOID)a, &mbi, sizeof(mbi)); ) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (end <= a) break;
        DWORD pr = mbi.Protect & 0xFF;
        bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                  (pr == PAGE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE);
        if (rw && mbi.RegionSize <= (SIZE_T)512 << 20) {
            for (uintptr_t c = base; c < end; c += buf.size() - 32) {
                SIZE_T want = (SIZE_T)(std::min)((uintptr_t)buf.size(), end - c), got = 0;
                if (!ReadProcessMemory(h, (LPCVOID)c, buf.data(), want, &got) || got < 32) break;
                scanned += got;
                for (size_t o = 0; o + 24 <= got; o += 4) {
                    float v[3];
                    bool dbl = false;
                    const float* fp = (const float*)(buf.data() + o);
                    if (fp[0] >= lo[0] && fp[0] <= hi[0] && fp[1] >= lo[1] && fp[1] <= hi[1] && fp[2] >= lo[2] && fp[2] <= hi[2]) {
                        v[0] = fp[0]; v[1] = fp[1]; v[2] = fp[2];
                    } else if ((o & 7) == 0) {
                        const double* dp = (const double*)(buf.data() + o);
                        if (!(dp[0] >= lo[0] && dp[0] <= hi[0] && dp[1] >= lo[1] && dp[1] <= hi[1] && dp[2] >= lo[2] && dp[2] <= hi[2])) continue;
                        v[0] = (float)dp[0]; v[1] = (float)dp[1]; v[2] = (float)dp[2];
                        dbl = true;
                    } else {
                        continue;
                    }
                    for (int i = 0; i < (int)ents.size(); ++i) {
                        float dx = v[0] - ents[i].pos.x, dy = v[1] - ents[i].pos.y, dz = v[2] - ents[i].pos.z;
                        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d < 0.6f) { hits.push_back({ c + o, i, dbl, d }); break; }
                    }
                    if (hits.size() > 200000) break;
                }
                if (want < buf.size()) break;
            }
        }
        a = end;
    }
    fprintf(f, "scanned %.0f MB in %llu ms, %zu hits\n", scanned / 1048576.0, GetTickCount64() - t0, hits.size());

    std::vector<int> perEnt(ents.size());
    for (auto& x : hits) perEnt[x.ent]++;
    for (size_t i = 0; i < ents.size(); ++i)
        fprintf(f, "  %c %-22s (%.1f %.1f %.1f) hits=%d\n", (char)ents[i].kind, ents[i].name.c_str(),
                ents[i].pos.x, ents[i].pos.y, ents[i].pos.z, perEnt[i]);

    // Movement check: which hits keep following their entity?
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    Snapshot s1;
    fetch(s1);
    std::map<int, Vec3> now;
    for (auto& e : s1.ents) now[e.id] = e.pos;
    now[-1] = s1.pos;
    struct Score { int moved = 0, follow = 0; };
    std::vector<char> follows(hits.size(), 0);
    int movedEnts = 0;
    std::vector<bool> entMoved(ents.size());
    for (size_t i = 0; i < ents.size(); ++i) {
        auto it = now.find(ents[i].id);
        if (it == now.end()) continue;
        float dx = it->second.x - ents[i].pos.x, dz = it->second.z - ents[i].pos.z;
        entMoved[i] = dx * dx + dz * dz > 0.04f;
        movedEnts += entMoved[i];
    }
    int followCount = 0;
    for (size_t k = 0; k < hits.size(); ++k) {
        auto& x = hits[k];
        if (!entMoved[x.ent]) continue;
        auto it = now.find(ents[x.ent].id);
        float v[3];
        if (x.dbl) { double d[3]; if (!ReadProcessMemory(h, (LPCVOID)x.addr, d, 24, nullptr)) continue; v[0] = (float)d[0]; v[1] = (float)d[1]; v[2] = (float)d[2]; }
        else if (!ReadProcessMemory(h, (LPCVOID)x.addr, v, 12, nullptr)) continue;
        float dx = v[0] - it->second.x, dy = v[1] - it->second.y, dz = v[2] - it->second.z;
        if (dx * dx + dy * dy + dz * dz < 0.36f) { follows[k] = 1; ++followCount; }
    }
    fprintf(f, "after 1.5 s: %d entities moved, %d hits still on them\n", movedEnts, followCount);

    // Raw layout: every hit in address order, with the gap to the previous
    // one. Arrays of per-entity structs show up as runs with a fixed stride.
    {
        std::vector<size_t> order(hits.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return hits[x].addr < hits[y].addr; });
        fprintf(f, "\nhits by address:\n");
        uintptr_t prev = 0;
        for (size_t k : order) {
            auto& x = hits[k];
            MEMORY_BASIC_INFORMATION rm{};
            VirtualQueryEx(h, (LPCVOID)x.addr, &rm, sizeof(rm));
            fprintf(f, "  %p %s ent=%2d %-14s d=%.3f gap=%+lld region=%p type=%s%s\n", (void*)x.addr, x.dbl ? "dbl" : "flt",
                    x.ent, ents[x.ent].name.c_str(), x.d, prev ? (long long)(x.addr - prev) : 0LL, rm.AllocationBase,
                    rm.Type == MEM_PRIVATE ? "private" : rm.Type == MEM_MAPPED ? "mapped" : "image", follows[k] ? " FOLLOWS" : "");
            prev = x.addr;
        }
    }

    // Triples: three float3 hits for one entity, 12 bytes apart. For each,
    // every pointer into the client image in the 0x600 bytes before it, and
    // how often each of the three changes (sampled every ~1 ms for 1 s).
    {
        std::vector<size_t> order(hits.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return hits[x].addr < hits[y].addr; });
        std::vector<std::pair<uintptr_t, int>> triples;
        for (size_t i = 0; i + 2 < order.size(); ++i) {
            auto& a0 = hits[order[i]]; auto& a1 = hits[order[i + 1]]; auto& a2 = hits[order[i + 2]];
            if (!a0.dbl && !a1.dbl && !a2.dbl && a1.addr == a0.addr + 12 && a2.addr == a0.addr + 24 && a0.ent == a2.ent)
                triples.push_back({ a0.addr, a0.ent });
        }
        fprintf(f, "\ntriples: %zu\n", triples.size());
        std::map<std::pair<int, uint64_t>, int> common;
        for (auto& [t, ent] : triples) {
            const int N = 0x600 / 8;
            uint64_t back[N];
            uintptr_t from = (t & ~(uintptr_t)7) - sizeof(back);
            fprintf(f, "  triple %p ent=%d %s\n", (void*)t, ent, ents[ent].name.c_str());
            if (!ReadProcessMemory(h, (LPCVOID)from, back, sizeof(back), nullptr)) continue;
            for (int i = N - 1; i >= 0; --i) {
                if (back[i] < modBase || back[i] >= modEnd) continue;
                int off = (int)(t - (from + i * 8));
                common[{ off, back[i] }]++;
                if (off <= 0x200) fprintf(f, "      -0x%03x  image+%llx\n", off, (unsigned long long)(back[i] - modBase));
            }
        }
        // Object header: the nearest 8-aligned pointer before the triple that
        // reads as a NativeAOT MethodTable (uint32 flags, uint32 BaseSize)
        // whose BaseSize covers the triple, i.e. the object it is in.
        fprintf(f, "  object headers (nearest MethodTable whose BaseSize covers the triple):\n");
        for (auto& [t, ent] : triples) {
            const int N = 0x2000 / 8;
            static uint64_t back[0x2000 / 8];
            uintptr_t from = (t & ~(uintptr_t)7) - sizeof(back);
            if (!ReadProcessMemory(h, (LPCVOID)from, back, sizeof(back), nullptr)) continue;
            int found = 0;
            for (int i = N - 1; i >= 0 && found < 3; --i) {
                if (back[i] < modBase || back[i] >= modEnd) continue;
                uint32_t hdr[2];
                if (!ReadProcessMemory(h, (LPCVOID)back[i], hdr, sizeof(hdr), nullptr)) continue;
                uint32_t dist = (uint32_t)(t - (from + i * 8));
                uint32_t base = hdr[1], comp = hdr[0] & 0xFFFF;
                bool covers = comp == 0 && base % 8 == 0 && base >= dist + 36 && base <= 0x8000;
                if (!covers) continue;
                fprintf(f, "    triple %p %-12s obj=%p  pos at +0x%x  MT=image+%llx  flags=%08x BaseSize=0x%x\n",
                        (void*)t, ents[ent].name.c_str(), (void*)(from + i * 8), dist,
                        (unsigned long long)(back[i] - modBase), hdr[0], base);
                ++found;
            }
            if (!found) fprintf(f, "    triple %p %-12s: no covering MethodTable within 0x2000\n", (void*)t, ents[ent].name.c_str());
        }
        fprintf(f, "  shared by all %zu triples:\n", triples.size());
        for (auto& [k, n] : common)
            if (n == (int)triples.size() && triples.size() > 1)
                fprintf(f, "    position at object+0x%x  type=image+%llx\n", k.first, (unsigned long long)(k.second - modBase));

        // update rates of the three slots
        for (auto& [t, ent] : triples) {
            float last[9];
            ReadProcessMemory(h, (LPCVOID)t, last, sizeof(last), nullptr);
            int changes[3] = { 0, 0, 0 };
            double tEnd = NowSeconds() + 1.0;
            while (NowSeconds() < tEnd) {
                float v[9];
                if (!ReadProcessMemory(h, (LPCVOID)t, v, sizeof(v), nullptr)) break;
                for (int k = 0; k < 3; ++k)
                    if (memcmp(v + k * 3, last + k * 3, 12) != 0) { changes[k]++; memcpy(last + k * 3, v + k * 3, 12); }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            fprintf(f, "  triple %p %-12s changes/s: [0]=%d [1]=%d [2]=%d\n", (void*)t, ents[ent].name.c_str(),
                    changes[0], changes[1], changes[2]);
        }
        // Values over time, for the triples that move: which slot leads,
        // which trails, which is in between (the rendered one interpolates).
        for (auto& [t, ent] : triples) {
            fprintf(f, "  samples %p %s (t ms: slot0 | slot1 | slot2), server now: ", (void*)t, ents[ent].name.c_str());
            Snapshot sn; fetch(sn);
            for (auto& e : sn.ents) if (e.id == ents[ent].id && ent != (int)ents.size() - 1) fprintf(f, "(%.3f %.3f %.3f)", e.pos.x, e.pos.y, e.pos.z);
            if (ent == (int)ents.size() - 1) fprintf(f, "(%.3f %.3f %.3f)", sn.pos.x, sn.pos.y, sn.pos.z);
            fprintf(f, "\n");
            double t0s = NowSeconds();
            for (int i = 0; i < 40; ++i) {
                float v[9];
                if (!ReadProcessMemory(h, (LPCVOID)t, v, sizeof(v), nullptr)) break;
                fprintf(f, "    %6.1f  %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f\n", (NowSeconds() - t0s) * 1000.0,
                        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
        }
    }

    // Back-scan for a type pointer into the client image.
    std::map<std::pair<int, uintptr_t>, std::set<int>> tally;         // (offset, ptr) -> entities
    std::map<std::pair<int, uintptr_t>, std::set<int>> tallyFollow;
    std::map<std::pair<int, uintptr_t>, uintptr_t> example;
    for (size_t k = 0; k < hits.size(); ++k) {
        auto& x = hits[k];
        uint64_t back[0x60];
        uintptr_t from = (x.addr & ~(uintptr_t)7) - sizeof(back);
        if (!ReadProcessMemory(h, (LPCVOID)from, back, sizeof(back), nullptr)) continue;
        for (int i = 0x60 - 1; i >= 0; --i) {
            uint64_t ptr = back[i];
            if (ptr < modBase || ptr >= modEnd) continue;
            int off = (int)(x.addr - (from + i * 8));
            auto key = std::make_pair(off * (x.dbl ? 1 : -1), (uintptr_t)ptr);   // negative offset = float3
            tally[key].insert(x.ent);
            if (follows[k]) tallyFollow[key].insert(x.ent);
            example.emplace(key, from + i * 8);
        }
    }
    std::vector<std::pair<size_t, std::pair<int, uintptr_t>>> ranked;
    for (auto& [key, set] : tally) ranked.push_back({ set.size() * 1000 + tallyFollow[key].size(), key });
    std::sort(ranked.rbegin(), ranked.rend());
    fprintf(f, "\ntype-pointer candidates (offset of position from object start; - = float3):\n");
    for (size_t i = 0; i < ranked.size() && i < 25; ++i) {
        auto& key = ranked[i].second;
        fprintf(f, "  off=%+5d  mt=%p (image+%llx)  entities=%zu  following=%zu  e.g. obj=%p\n",
                key.first, (void*)key.second, (unsigned long long)(key.second - modBase),
                tally[key].size(), tallyFollow[key].size(), (void*)example[key]);
    }
    CloseHandle(h);
    std::string r; g_link.Call("BYE", r);
    g_link.Close();
    WSACleanup();
    fclose(f);
    return 0;
}

} // namespace game
