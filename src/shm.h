// =============================================================================
//  shm.h — shared memory between the trainer and the in-game renderer DLL.
//
//  The trainer owns the data (entities, camera lock, options) and writes the
//  block once per overlay frame under a sequence lock. The renderer, running
//  inside HytaleClient.exe, copies it at the start of every game frame and
//  draws. Times are milliseconds on the QueryPerformanceCounter clock, which
//  is the same in both processes.
//
//  Lifetime rules, so nothing outlives a single-player session:
//    - the trainer sets quit (or stops writing) when it detaches or exits;
//    - the renderer unhooks and unloads on quit, or by itself once the
//      trainer has been silent for kStaleUnloadMs.
// =============================================================================
#pragma once
#include <Windows.h>
#include <cstdint>
#include <cwchar>

namespace shm {

constexpr uint32_t kMagic = 0x53455948;     // "HYES"
constexpr uint32_t kVersion = 5;
constexpr int      kMaxObjs = 1024;
constexpr int      kMaxEnts = 512;
constexpr double   kStaleDrawMs = 500.0;    // stop drawing when the trainer is this quiet
constexpr double   kStaleUnloadMs = 3000.0; // and unload after this

inline void MappingName(wchar_t* out, size_t n, DWORD clientPid) { swprintf(out, n, L"Local\\HyTrainerEsp_%lu", clientPid); }
inline void MutexName(wchar_t* out, size_t n, DWORD clientPid)   { swprintf(out, n, L"Local\\HyTrainerRender_%lu", clientPid); }

inline double NowMs() {
    static const double inv = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / (double)f.QuadPart; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * inv;
}

struct Ent {
    char    kind;           // game::Kind
    uint8_t hasPrev;
    int32_t id;
    float   pos[3];         // latest tick
    float   prev[3];        // previous tick (valid when hasPrev)
    float   height, hp, maxHp;
    char    name[48];
};

enum RendererState : LONG { kNone = 0, kHooked = 1, kUnloaded = 2, kFailed = 3 };

struct Block {
    uint32_t magic, version;

    // ---- trainer -> renderer --------------------------------------------
    volatile LONG seq;              // even = stable, odd = being written
    double  trainerBeatMs;          // last write
    volatile LONG quit;             // unhook and unload now
    int32_t enabled;                // draw the ESP in game

    // client camera lock (see camera.h); camAddr 0 = use the server view
    uint64_t camAddr, camAddrAlt;
    int32_t  layout, negate, relative, originDbl;
    float    xSign, ySign;
    uint64_t originAddr;            // client camera position, 0 = not found

    // server view: fallback camera, and the origin for camera-relative matrices
    float   pitch, yaw, eyeHeight, fovDeg;

    // tick interpolation
    double  curTimeMs, intervalMs;
    int32_t smooth, meHasPrev;
    float   me[3], mePrev[3];

    // options
    int32_t hostile, neutral, friendly, players, items, lines, info, health;
    float   maxDist;
    int32_t serverFallback;         // draw with the server view when the client camera is not locked

    int32_t count;
    Ent     ents[kMaxEnts];

    // the client's own entity objects (cent.h): rendered positions
    uint64_t entMt;                 // type pointer, 0 = not found
    uint64_t entMt2;                // your own character's type, if different
    uint64_t playerObj;             // your own character's object, 0 = not identified
    int32_t  entPosOff, entSlot;
    int32_t  objCount;
    uint64_t objs[kMaxObjs];

    // ---- renderer -> trainer --------------------------------------------
    double  rendererBeatMs;         // last frame drawn in game
    volatile LONG rendererState;
    volatile LONG frames;
    volatile LONG framesClient, framesServer, framesHidden, framesOwnOrigin;   // how each frame was drawn
    volatile LONG entsMatched, entsTotal, meFromClient;                        // summed over frames
    float   rendererFps;
    char    rendererError[128];
};

} // namespace shm
