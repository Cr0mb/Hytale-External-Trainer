#include "camera.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace cam {
namespace {

// ---------------------------------------------------------------------------
//  Matrix algebra
//
//  A world->clip matrix is three planes: clip x, clip y and w. Written as
//  functionals, X(p) = gx.p + cx, Y(p) = gy.p + cy, W(p) = gw.p + cw, and
//  ndc = (X/W, Y/W). Because the view half of the matrix is a rigid transform
//  and the projection half is a plain perspective divide:
//
//    |gw| == 1                    (gw is the camera forward)
//    gx, gy, gw are orthogonal    (the camera basis)
//    |gy| == 1 / tan(fovY / 2)
//    |gy| / |gx| == the aspect ratio
//
//  Those facts identify the matrix on their own; the player position from the
//  server then picks the right one out of the few that remain.
// ---------------------------------------------------------------------------
struct Plane { float g[3], c; };
struct Planes { Plane X, Y, W; };

// Layout 0: row-vector (clip = p * M, the DirectX convention). Layout 1: the
// transpose of that (clip = M * p).
Planes Extract(const float* m, int layout) {
    Planes p{};
    if (layout == 0) {
        p.X = { { m[0], m[4], m[8]   }, m[12] };
        p.Y = { { m[1], m[5], m[9]   }, m[13] };
        p.W = { { m[3], m[7], m[11]  }, m[15] };
    } else {
        p.X = { { m[0], m[1], m[2]   }, m[3]  };
        p.Y = { { m[4], m[5], m[6]   }, m[7]  };
        p.W = { { m[12], m[13], m[14] }, m[15] };
    }
    return p;
}

float Len(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The camera centre is the only point that lies on all three planes: there
// clip x, clip y and w are all zero.
bool SolveEye(const Planes& p, float out[3]) {
    const float* r0 = p.X.g;
    const float* r1 = p.Y.g;
    const float* r2 = p.W.g;
    auto det3 = [](const float a[3], const float b[3], const float c[3]) {
        return a[0] * (b[1] * c[2] - b[2] * c[1])
             - a[1] * (b[0] * c[2] - b[2] * c[0])
             + a[2] * (b[0] * c[1] - b[1] * c[0]);
    };
    float d = det3(r0, r1, r2);
    if (!std::isfinite(d) || std::fabs(d) < 1e-9f) return false;
    const float b[3] = { -p.X.c, -p.Y.c, -p.W.c };
    // Cramer, one column replaced at a time.
    const float x0[3] = { b[0],  r0[1], r0[2] }, x1[3] = { b[1],  r1[1], r1[2] }, x2[3] = { b[2],  r2[1], r2[2] };
    const float y0[3] = { r0[0], b[0],  r0[2] }, y1[3] = { r1[0], b[1],  r1[2] }, y2[3] = { r2[0], b[2],  r2[2] };
    const float z0[3] = { r0[0], r0[1], b[0]  }, z1[3] = { r1[0], r1[1], b[1]  }, z2[3] = { r2[0], r2[1], b[2]  };
    // det3 takes rows, so rebuild them row-wise.
    const float* rx[3] = { x0, x1, x2 };
    const float* ry[3] = { y0, y1, y2 };
    const float* rz[3] = { z0, z1, z2 };
    out[0] = det3(rx[0], rx[1], rx[2]) / d;
    out[1] = det3(ry[0], ry[1], ry[2]) / d;
    out[2] = det3(rz[0], rz[1], rz[2]) / d;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

constexpr float kPi = 3.14159265f;
constexpr float kMaxBoom = 10.f;     // how far the camera may sit from the eye (third person)

struct Hints {
    float eye[3]{};
    float fwd[3]{}, right[3]{}, up[3]{};
    int   w = 0, h = 0;
    bool  have = false;
};

// Shape only: no game state involved, so this is the test the scan runs.
bool ShapeOk(const float* m, int layout, float aspect, Planes& po, float& fovY) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e6f) return false;
    Planes p = Extract(m, layout);
    float lw = Len(p.W.g);
    if (lw < 0.97f || lw > 1.03f) return false;
    float ly = Len(p.Y.g), lx = Len(p.X.g);
    if (ly < 0.466f || ly > 2.75f) return false;              // fovY between 40 and 130 degrees
    if (lx < 1e-3f) return false;
    float a = ly / lx;
    if (aspect > 0.1f) {
        if (std::fabs(a - aspect) > aspect * 0.04f) return false;
    } else if (a < 0.5f || a > 4.f) {
        return false;
    }
    if (std::fabs(Dot(p.X.g, p.W.g)) > 0.05f * lx * lw) return false;
    if (std::fabs(Dot(p.Y.g, p.W.g)) > 0.05f * ly * lw) return false;
    if (std::fabs(Dot(p.X.g, p.Y.g)) > 0.05f * lx * ly) return false;
    po = p;
    fovY = 2.f * std::atan(1.f / ly);
    return true;
}

// Shape plus agreement with the player the server reports. score is the
// residual, smaller is better; the sign flags are resolved here too.
bool Validate(const float* raw, int layout, const Hints& hi, View& out, float& score, bool& negate) {
    float aspect = hi.h > 0 ? (float)hi.w / (float)hi.h : 0.f;
    float m[16];
    memcpy(m, raw, sizeof(m));
    Planes p;
    float fovY;
    if (!ShapeOk(m, layout, aspect, p, fovY)) return false;

    float eye[3];
    if (!SolveEye(p, eye)) return false;

    float dx = eye[0] - hi.eye[0], dy = eye[1] - hi.eye[1], dz = eye[2] - hi.eye[2];
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool relative = false;
    if (dist > kMaxBoom) {
        // Some renderers build the matrix around the camera rather than the
        // world origin. Then the solved centre sits at (0,0,0) and world
        // positions have to be offset by the eye before projecting.
        if (Len(eye) > kMaxBoom) return false;
        relative = true;
        dist = Len(eye);
    }

    float lw = Len(p.W.g);
    float f[3] = { p.W.g[0] / lw, p.W.g[1] / lw, p.W.g[2] / lw };
    float align = Dot(f, hi.fwd);
    negate = false;
    if (align < -0.82f) { negate = true; align = -align; }    // w negative in front of the camera
    else if (align < 0.82f) return false;                     // not looking where we are looking

    if (negate) {
        for (int i = 0; i < 16; ++i) m[i] = -m[i];
        p = Extract(m, layout);
        for (int i = 0; i < 3; ++i) f[i] = -f[i];
    }

    out = View{};
    out.valid = true;
    out.layout = layout;
    memcpy(out.m, m, sizeof(m));
    memcpy(out.eye, eye, sizeof(eye));
    memcpy(out.fwd, f, sizeof(f));
    out.fovY = fovY;
    out.aspect = Len(p.Y.g) / Len(p.X.g);
    out.relative = relative;
    // Which way the clip axes run on screen, measured against the basis the
    // server rotation gives us.
    out.xSign = Dot(p.X.g, hi.right) >= 0 ? 1.f : -1.f;
    out.ySign = Dot(p.Y.g, hi.up) >= 0 ? 1.f : -1.f;
    // A matrix that carries its own translation is worth more than a
    // camera-relative one, because it pins the camera position exactly.
    score = dist + 10.f * (1.f - align) + (relative ? 6.f : 0.f);
    return true;
}

// ---------------------------------------------------------------------------
//  Shared state
// ---------------------------------------------------------------------------
std::mutex         g_mx;
Hints              g_hint;
std::string        g_status = "off";
std::atomic<bool>  g_run{ false };
std::atomic<DWORD> g_pid{ 0 };
std::thread        g_thread;

std::atomic<uintptr_t> g_addr{ 0 };      // locked matrix address inside the client
std::atomic<int>       g_layout{ 0 };
std::atomic<bool>      g_negate{ false };
std::atomic<bool>      g_relative{ false };
std::atomic<float>     g_xSign{ 1.f }, g_ySign{ 1.f };
std::atomic<uint32_t>  g_misses{ 0 };    // counted by the render thread
std::atomic<float>     g_fov{ 0.f };
// Camera-relative matrices carry no translation, so the camera's own world
// position has to come from somewhere. Until it is found the server eye is
// used, which is right in first person and short by the boom arm in third.
std::atomic<uintptr_t> g_origin{ 0 };
std::atomic<bool>      g_originDbl{ false };
std::atomic<bool>      g_originTurned{ false };   // proven by orbiting, not just by update rate

// Lock generation: bumps on every new lock so the freshness watcher of an
// old lock stops.
std::atomic<uint32_t>  g_lockGen{ 0 };
// The copy the lock had before the freshness check moved it, and a copy that
// stopped being a camera after we moved to it (never moved to again).
std::atomic<uintptr_t> g_addrPrev{ 0 }, g_addrBad{ 0 };
std::thread            g_freshThread;

// Read outcomes, counted by the render thread.
std::atomic<uint32_t>  g_stOk{ 0 }, g_stTorn{ 0 }, g_stHeld{ 0 }, g_stShape{ 0 }, g_stEye{ 0 }, g_stFail{ 0 };

// Read-only handle. The discovery thread opens and closes it; the render
// thread reads through it, so it is atomic and every use takes a local copy.
std::atomic<HANDLE> g_proc{ nullptr };

void  SetStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_mx); g_status = s; }
Hints GetHint() { std::lock_guard<std::mutex> lk(g_mx); return g_hint; }

bool ReadMem(uintptr_t at, void* into, size_t n) {
    HANDLE h = g_proc.load();
    SIZE_T got = 0;
    return h && ReadProcessMemory(h, (LPCVOID)at, into, n, &got) && got == n;
}

// ---------------------------------------------------------------------------
//  Scanning
// ---------------------------------------------------------------------------
struct Candidate { uintptr_t addr; int layout; float score; };

bool ScannableRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    DWORD p = mbi.Protect & 0xFF;
    if (p != PAGE_READWRITE && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY)
        return false;                                  // the matrix is rewritten every frame
    return mbi.RegionSize <= (SIZE_T)512 * 1024 * 1024;
}

// Walks [from, to) and appends every 64-byte window that passes the full
// check. stride 16 catches anything a shader constant buffer or a struct
// field can hold (both are 16-byte aligned) and is four times faster than the
// exhaustive stride of 4. Returns how many bytes were actually read.
uint64_t ScanRange(uintptr_t from, uintptr_t to, const Hints& hi, std::vector<Candidate>& out,
                   size_t maxOut, size_t stride) {
    std::vector<uint8_t> buf;
    uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = from; a < to && g_run && out.size() < maxOut; ) {
        if (!VirtualQueryEx(g_proc.load(), (LPCVOID)a, &mbi, sizeof(mbi))) break;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        uintptr_t end = base + (uintptr_t)mbi.RegionSize;
        if (end <= a) break;
        if (!ScannableRegion(mbi)) { a = end; continue; }
        uintptr_t s = (std::max)(base, from), e = (std::min)(end, to);
        const size_t kChunk = 1u << 20;
        buf.resize(kChunk + 64);
        for (uintptr_t c = s; c < e && g_run && out.size() < maxOut; c += kChunk) {
            size_t want = (size_t)(std::min)((uintptr_t)kChunk + 64, e - c);
            SIZE_T got = 0;
            if (!ReadProcessMemory(g_proc.load(), (LPCVOID)c, buf.data(), want, &got) || got < 64) continue;
            scanned += got;
            size_t last = (size_t)got - 64;
            for (size_t o = 0; o <= last; o += stride) {
                const float* m = (const float*)(buf.data() + o);
                // Cheap gate first: the forward row of a view-projection
                // matrix is a unit vector. That alone drops nearly everything.
                float la = m[3] * m[3] + m[7] * m[7] + m[11] * m[11];
                float lb = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];
                bool c0 = la > 0.94f && la < 1.07f;
                bool c1 = lb > 0.94f && lb < 1.07f;
                if (!c0 && !c1) continue;
                for (int layout = 0; layout < 2; ++layout) {
                    if (layout == 0 ? !c0 : !c1) continue;
                    View v;
                    float score;
                    bool neg;
                    if (Validate(m, layout, hi, v, score, neg)) {
                        out.push_back({ c + o, layout, score });
                        break;
                    }
                }
            }
        }
        a = end;
    }
    return scanned;
}

// ---------------------------------------------------------------------------
//  Finding the camera position for a camera-relative matrix
//
//  The test that matters: the camera position changes when you turn the mouse
//  while standing still, and a copy of the player position does not. Anything
//  that tracks the player, stays on the view axis behind the eye and reacts to
//  turning is the third-person camera. In first person nothing passes the turn
//  test, the eye is used, and that is already correct.
// ---------------------------------------------------------------------------
void SaveCache();   // lock cache, below

struct OCand {
    uintptr_t addr;
    bool  dbl;
    float v[3];       // last reading
    float first[3];   // reading when it entered the list
    float firstEye[3];
    bool  turned = false;
    float boom = 0;
    int   fastRounds = 0;   // moving rounds in which it updated at frame rate
    int   changes = 0;      // scratch for the rate test
};

bool ReadVec(uintptr_t at, bool dbl, float out[3]) {
    if (dbl) {
        double d[3];
        if (!ReadMem(at, d, sizeof(d))) return false;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(d[i])) return false;
            out[i] = (float)d[i];
        }
        return true;
    }
    float f[3];
    if (!ReadMem(at, f, sizeof(f))) return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(f[i])) return false;
        out[i] = f[i];
    }
    return true;
}

float Dist(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// On the view axis, at most kMaxBoom behind the eye.
bool OnViewAxis(const float v[3], const Hints& hi, float& boom, float tol = 0.35f) {
    float d[3] = { hi.eye[0] - v[0], hi.eye[1] - v[1], hi.eye[2] - v[2] };
    float len = Len(d);
    if (len > kMaxBoom) return false;
    float along = Dot(d, hi.fwd);
    if (along < -0.2f) return false;                       // in front of the eye
    float off[3] = { d[0] - along * hi.fwd[0], d[1] - along * hi.fwd[1], d[2] - along * hi.fwd[2] };
    if (Len(off) > tol) return false;                      // not on the view axis
    boom = len;
    return true;
}

uint64_t ScanOrigins(const Hints& hi, std::vector<OCand>& out, size_t maxOut) {
    std::vector<uint8_t> buf;
    uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; g_run && out.size() < maxOut; ) {
        if (!VirtualQueryEx(g_proc.load(), (LPCVOID)a, &mbi, sizeof(mbi))) break;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        uintptr_t end = base + (uintptr_t)mbi.RegionSize;
        if (end <= a) break;
        if (!ScannableRegion(mbi)) { a = end; continue; }
        const size_t kChunk = 1u << 20;
        buf.resize(kChunk + 32);
        for (uintptr_t c = base; c < end && g_run && out.size() < maxOut; c += kChunk) {
            size_t want = (size_t)(std::min)((uintptr_t)kChunk + 32, end - c);
            SIZE_T got = 0;
            if (!ReadProcessMemory(g_proc.load(), (LPCVOID)c, buf.data(), want, &got) || got < 32) continue;
            scanned += got;
            size_t last = (size_t)got - 32;
            // Cheap gate first: each component has to be within the boom arm
            // of the eye. Everything else is far too slow to run per offset.
            const float lo0 = hi.eye[0] - kMaxBoom, hi0f = hi.eye[0] + kMaxBoom;
            const float lo1 = hi.eye[1] - kMaxBoom, hi1f = hi.eye[1] + kMaxBoom;
            const float lo2 = hi.eye[2] - kMaxBoom, hi2f = hi.eye[2] + kMaxBoom;
            for (size_t o = 0; o <= last; o += 4) {
                const float* f = (const float*)(buf.data() + o);
                float boom;
                if (f[0] >= lo0 && f[0] <= hi0f && f[1] >= lo1 && f[1] <= hi1f && f[2] >= lo2 && f[2] <= hi2f) {
                    float v[3] = { f[0], f[1], f[2] };
                    if (OnViewAxis(v, hi, boom)) {
                        OCand oc{ c + o, false };
                        memcpy(oc.v, v, sizeof(v)); memcpy(oc.first, v, sizeof(v));
                        memcpy(oc.firstEye, hi.eye, sizeof(oc.firstEye));
                        oc.boom = boom;
                        out.push_back(oc);
                    }
                }
                if (o % 8 == 0) {
                    const double* d = (const double*)(buf.data() + o);
                    if (d[0] >= lo0 && d[0] <= hi0f && d[1] >= lo1 && d[1] <= hi1f && d[2] >= lo2 && d[2] <= hi2f) {
                        float v[3] = { (float)d[0], (float)d[1], (float)d[2] };
                        if (OnViewAxis(v, hi, boom)) {
                            OCand oc{ c + o, true };
                            memcpy(oc.v, v, sizeof(v)); memcpy(oc.first, v, sizeof(v));
                            memcpy(oc.firstEye, hi.eye, sizeof(oc.firstEye));
                            oc.boom = boom;
                            out.push_back(oc);
                        }
                    }
                }
            }
        }
        a = end;
    }
    return scanned;
}

bool Holding() { return g_run && g_addr.load() && g_misses.load() < 120; }

// One scan plus the rounds that narrow it down. Returns true once a candidate
// has proved it is the camera and not a copy of the player position.
// The camera position sits in a block of three like the entities' positions:
// previous tick, current tick, and the interpolated one the frame is drawn
// with. Whichever of the three the hunt found, use the interpolated one.
uintptr_t RenderSlot(uintptr_t a, bool dbl) {
    const uintptr_t stride = dbl ? 24 : 12;
    for (int k = 0; k < 3; ++k) {
        uintptr_t base = a - k * stride;
        float v[3][3];
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) ok = ReadVec(base + i * stride, dbl, v[i]);
        if (!ok) continue;
        if (Dist(v[0], v[1]) < 1.5f && Dist(v[1], v[2]) < 1.5f && Dist(v[0], v[2]) < 1.5f) return base + 2 * stride;
    }
    return a;
}

bool HuntOnce(int maxRounds) {
    std::vector<OCand> cands;
    // The scan compares memory against the server eye with a tight
    // tolerance. While you move, the server is a tick or two behind the
    // client and the real camera position falls outside it, so wait until
    // you have stood still for a moment.
    {
        Hints a = GetHint();
        int stillFor = 0;
        while (Holding() && stillFor < 4) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Hints b = GetHint();
            stillFor = Dist(a.eye, b.eye) < 0.01f ? stillFor + 1 : 0;
            a = b;
        }
        if (!Holding()) return false;
    }
    Hints hi0 = GetHint();
    uint64_t bytes = ScanOrigins(hi0, cands, 4096);
    log_::Info("[test] camera origin scan: %zu candidates (%.0f MB)", cands.size(), bytes / 1048576.0);
    Hints prev = hi0;
    for (int round = 0; Holding() && !cands.empty() && round < maxRounds; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Hints hi = GetHint();
        if (!hi.have) continue;
        bool still = Dist(hi.eye, prev.eye) < 0.05f;
        bool turnedView = Dot(hi.fwd, prev.fwd) < 0.9995f;
        if (!still) {
            // While the player moves, the copy the renderer uses changes every
            // client frame; copies fed by the server change at tick rate.
            // Count changes over ~100 ms.
            for (auto& c : cands) c.changes = 0;
            std::vector<std::array<float, 3>> last(cands.size());
            for (size_t i = 0; i < cands.size(); ++i) ReadVec(cands[i].addr, cands[i].dbl, last[i].data());
            for (int pass = 0; pass < 12 && g_run; ++pass) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                for (size_t i = 0; i < cands.size(); ++i) {
                    float v[3];
                    if (!ReadVec(cands[i].addr, cands[i].dbl, v)) continue;
                    if (v[0] != last[i][0] || v[1] != last[i][1] || v[2] != last[i][2]) {
                        cands[i].changes++;
                        last[i] = { v[0], v[1], v[2] };
                    }
                }
            }
            for (auto& c : cands) if (c.changes >= 5) c.fastRounds++;   // above ~45 Hz
            hi = GetHint();
        }
        std::vector<OCand> keep;
        for (auto& c : cands) {
            float v[3], boom;
            if (!ReadVec(c.addr, c.dbl, v)) continue;
            if (!OnViewAxis(v, hi, boom, still ? 0.35f : 1.2f)) continue;
            float moved = Dist(v, c.v);
            if (!still) {
                // must follow the player the way the eye does
                float dv[3] = { v[0] - c.v[0], v[1] - c.v[1], v[2] - c.v[2] };
                float de[3] = { hi.eye[0] - prev.eye[0], hi.eye[1] - prev.eye[1], hi.eye[2] - prev.eye[2] };
                if (Dist(dv, de) > 0.75f + 0.25f * Len(de)) continue;
            } else if (turnedView && moved > 0.15f) {
                c.turned = true;                 // orbits with the view: this is the camera
            }
            c.boom = boom;
            memcpy(c.v, v, sizeof(v));
            keep.push_back(c);
        }
        cands.swap(keep);
        prev = hi;
        const OCand* best = nullptr;
        for (auto& c : cands) if (c.turned && (!best || c.boom > best->boom)) best = &c;
        if (best) {
            g_originDbl = best->dbl;
            g_origin = RenderSlot(best->addr, best->dbl);
            g_originTurned = true;
            SaveCache();
            log_::Info("[test] camera position found at %p (%s, boom %.2f, %zu candidates)",
                       (void*)best->addr, best->dbl ? "double3" : "float3", best->boom, cands.size());
            return true;
        }
        // No orbit proof yet. A copy that keeps updating at frame rate and sits
        // on the eye is the first-person camera position: use it so walking is
        // as smooth as turning, but keep hunting - in third person the orbiting
        // one, once proven, replaces it.
        const OCand* fast = nullptr;
        for (auto& c : cands)
            if (c.fastRounds >= 2 && c.boom < 0.75f && (!fast || c.fastRounds > fast->fastRounds)) fast = &c;
        if (fast && g_origin.load() != fast->addr) {
            g_originDbl = fast->dbl;
            g_origin = RenderSlot(fast->addr, fast->dbl);
            SaveCache();
            log_::Info("[test] first-person camera position at %p (%s, %d fast rounds, %zu candidates)",
                       (void*)fast->addr, fast->dbl ? "double3" : "float3", fast->fastRounds, cands.size());
        }
    }
    return false;
}

// Keeps trying while the lock holds. In first person there is nothing to find
// (the camera sits on the eye and never orbits), so this simply keeps looking
// in the background and the eye stays in use, which is correct there.
void HuntOrigin() {
    for (int attempt = 0; Holding() && !g_originTurned.load() && attempt < 10; ++attempt) {
        if (HuntOnce(120)) return;
        SetStatus(g_fov.load() > 0 ? "locked (first person)" : "locked");
    }
    if (!g_origin.load())
        log_::Info("[test] camera position not identified; using the server eye");
}

bool HintMoved(const Hints& a, const Hints& b) {
    float dx = a.eye[0] - b.eye[0], dy = a.eye[1] - b.eye[1], dz = a.eye[2] - b.eye[2];
    if (dx * dx + dy * dy + dz * dz > 0.25f) return true;
    return Dot(a.fwd, b.fwd) < 0.995f;      // about six degrees of turn
}

void Publish(const Candidate& c, const View& v, bool negate) {
    g_layout = c.layout;
    g_relative = v.relative;
    g_xSign = v.xSign;
    g_ySign = v.ySign;
    g_fov = v.fovY;
    g_negate = negate;
    g_misses = 0;
    g_addr = c.addr;
}

// ---------------------------------------------------------------------------
//  Freshness
//
//  Standing still, every copy of the view-projection matrix is identical, so
//  the lock may land on a copy that is written later in the frame than the one
//  the renderer builds - or on last frame's matrix kept for motion vectors,
//  which is exactly one frame behind. While the view changes, the copies show
//  each new matrix at different moments. This samples all bit-identical
//  survivors at sub-millisecond rate, and moves the lock to the copy that
//  shows each new matrix first, once it has won three bursts in a row.
// ---------------------------------------------------------------------------
uint64_t Hash64(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

double Ms() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

void Freshness(std::vector<uintptr_t> addrs, uint32_t gen) {
    uintptr_t challenger = 0;
    int wins = 0;
    while (g_run && g_lockGen.load() == gen && g_addr.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        if (!(g_run && g_lockGen.load() == gen)) break;
        uintptr_t cur = g_addr.load();
        if (std::find(addrs.begin(), addrs.end(), cur) == addrs.end()) addrs.push_back(cur);

        // first time each candidate showed each matrix during the burst
        std::vector<std::vector<std::pair<uint64_t, double>>> seen(addrs.size());
        std::vector<uint64_t> last(addrs.size(), 0);
        float raw[16];
        for (size_t i = 0; i < addrs.size(); ++i)
            if (ReadMem(addrs[i], raw, sizeof(raw))) last[i] = Hash64(raw, sizeof(raw));
        double end = Ms() + 120.0;
        while (Ms() < end && g_run) {
            for (size_t i = 0; i < addrs.size(); ++i) {
                if (!ReadMem(addrs[i], raw, sizeof(raw))) continue;
                uint64_t h = Hash64(raw, sizeof(raw));
                if (h != last[i]) { last[i] = h; seen[i].push_back({ h, Ms() }); }
            }
            std::this_thread::yield();
        }
        size_t ci = std::find(addrs.begin(), addrs.end(), cur) - addrs.begin();
        if (seen[ci].size() < 2) continue;                    // view not changing: nothing to learn

        uintptr_t best = 0;
        double bestLead = 1.5;                                // ms; below that it is noise
        for (size_t j = 0; j < addrs.size(); ++j) {
            if (j == ci) continue;
            double lead = 0;
            int n = 0;
            for (auto& [h, t] : seen[ci])
                for (auto& [h2, t2] : seen[j])
                    if (h2 == h) { lead += t - t2; ++n; break; }
            if (addrs[j] == g_addrBad.load()) continue;
            if (n >= 2 && lead / n > bestLead) { bestLead = lead / n; best = addrs[j]; }
        }
        if (!best) { challenger = 0; wins = 0; continue; }
        wins = best == challenger ? wins + 1 : 1;
        challenger = best;
        if (wins >= 3 && g_lockGen.load() == gen) {
            log_::Info("[test] camera: %p shows each matrix %.1f ms before %p, switching",
                       (void*)best, bestLead, (void*)cur);
            g_addrPrev = cur;
            g_addr = best;        // bit-identical copy: layout and signs carry over
            SaveCache();
            challenger = 0;
            wins = 0;
        }
    }
}

void StartFreshness(const std::vector<Candidate>& cands) {
    uint32_t gen = ++g_lockGen;
    if (g_freshThread.joinable()) g_freshThread.join();     // the old one exits on the gen bump
    std::vector<uintptr_t> addrs;
    for (auto& c : cands) addrs.push_back(c.addr);
    g_freshThread = std::thread(Freshness, std::move(addrs), gen);
}

// Narrow a candidate list down over a few samples taken while the player
// moves: only a matrix that keeps tracking the player survives.
bool Confirm(std::vector<Candidate>& cands) {
    Hints first = GetHint();
    bool moved = false;
    for (int round = 0; round < 8 && g_run && !cands.empty(); ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        Hints hi = GetHint();
        if (!hi.have) return false;
        if (HintMoved(first, hi)) moved = true;

        float raw[16];
        std::vector<Candidate> keep;
        View best{};
        Candidate bestC{};
        float bestScore = 1e9f;
        bool bestNeg = false;
        for (auto& c : cands) {
            if (!ReadMem(c.addr, raw, sizeof(raw))) continue;
            View v;
            float score;
            bool neg;
            if (!Validate(raw, c.layout, hi, v, score, neg)) continue;
            c.score = 0.7f * c.score + 0.3f * score;
            keep.push_back(c);
            if (c.score < bestScore) { bestScore = c.score; best = v; bestC = c; bestNeg = neg; }
        }
        cands.swap(keep);
        if (cands.empty()) return false;
        char t[96];
        snprintf(t, sizeof(t), "confirming %zu candidate%s%s", cands.size(), cands.size() == 1 ? "" : "s",
                 moved ? "" : " (move or look around)");
        SetStatus(t);
        // Moving or turning is what kills off anything that merely looked
        // like a camera, so prefer to lock once that has happened. Standing
        // still is not a reason to wait forever: the renderer keeps several
        // copies of the same matrix, so the best survivor is the camera even
        // when dozens of them pass. A bad pick is caught per frame anyway.
        bool enough = moved ? round >= 2 : round >= 6;
        if (enough) {
            Publish(bestC, best, bestNeg);
            log_::Info("[test] camera locked at %p layout=%d fov=%.1f rel=%d x=%+.0f y=%+.0f neg=%d moved=%d (%zu survivors)",
                       (void*)bestC.addr, bestC.layout, best.fovY * 180.f / kPi, (int)best.relative,
                       best.xSign, best.ySign, (int)bestNeg, (int)moved, cands.size());
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Lock cache
//
//  The matrix and the camera position stay at the same addresses for the
//  life of a client process. The lock is remembered per client (pid plus
//  process creation time, so a reused pid never matches) and re-validated
//  before use, which turns a 15 s scan into an instant lock on every trainer
//  start after the first.
// ---------------------------------------------------------------------------
uint64_t g_clientCreated = 0;

std::wstring CachePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/") + 1) + L"camera_cache.txt";
}

void SaveCache() {
    uintptr_t a = g_addr.load();
    if (!a || !g_clientCreated) return;
    FILE* f = _wfopen(CachePath().c_str(), L"w");
    if (!f) return;
    fprintf(f, "%lu %llu %llx %d %d %d %g %g %llx %d %d\n", g_pid.load(), (unsigned long long)g_clientCreated,
            (unsigned long long)a, g_layout.load(), (int)g_negate.load(), (int)g_relative.load(),
            g_xSign.load(), g_ySign.load(), (unsigned long long)g_origin.load(), (int)g_originDbl.load(),
            (int)g_originTurned.load());
    fclose(f);
}

// Locks from the cache if it is for this client and still checks out.
bool LoadCache(const Hints& hi) {
    FILE* f = _wfopen(CachePath().c_str(), L"r");
    if (!f) return false;
    unsigned long pid = 0;
    unsigned long long created = 0, addr = 0, origin = 0;
    int layout = 0, neg = 0, rel = 0, odbl = 0, turned = 0;
    float xs = 1, ys = 1;
    int n = fscanf(f, "%lu %llu %llx %d %d %d %g %g %llx %d %d", &pid, &created, &addr, &layout, &neg, &rel,
                   &xs, &ys, &origin, &odbl, &turned);
    fclose(f);
    if (n != 11 || pid != g_pid.load() || created != g_clientCreated || !addr) return false;
    float raw[16];
    if (!ReadMem((uintptr_t)addr, raw, sizeof(raw))) return false;
    View v;
    float score;
    bool negNow;
    if (!Validate(raw, layout, hi, v, score, negNow) || negNow != (neg != 0)) return false;
    Candidate c{ (uintptr_t)addr, layout, score };
    Publish(c, v, negNow);
    float p[3];
    if (origin && ReadVec((uintptr_t)origin, odbl != 0, p) && Dist(p, hi.eye) <= kMaxBoom) {
        g_originDbl = odbl != 0;
        g_origin = RenderSlot((uintptr_t)origin, odbl != 0);
        g_originTurned = turned != 0;
    }
    log_::Info("[test] camera locked from cache at %p (origin %p)", (void*)addr, (void*)g_origin.load());
    return true;
}

void Discover() {
    uintptr_t lastGood = 0;
    while (g_run) {
        Hints hi = GetHint();
        if (!hi.have || hi.w <= 0) {
            SetStatus("waiting for the player");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }
        std::vector<Candidate> cands;
        auto t0 = GetTickCount64();
        uint64_t bytes = 0;

        if (!lastGood && LoadCache(hi)) {
            lastGood = g_addr.load();
            char t[96];
            snprintf(t, sizeof(t), "locked, FOV %.0f", g_fov.load() * 180.f / kPi);
            SetStatus(t);
            std::vector<Candidate> self{ { lastGood, g_layout.load(), 0.f } };
            // Rescan quietly for the other copies so the freshness check has
            // something to compare against; the lock is live meanwhile.
            std::vector<Candidate> more;
            ScanRange(0x10000, (uintptr_t)0x7FFFFFFFFFFFull, hi, more, 128, 16);
            for (auto& c : more) if (c.addr != lastGood) self.push_back(c);
            StartFreshness(self);
            if (g_relative.load() && !g_originTurned.load()) HuntOrigin();
            while (g_run && g_misses.load() < 120) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_run) break;
            g_addr = 0;
            ++g_lockGen;
            g_origin = 0;
            g_originTurned = false;
            log_::Info("[test] camera lock lost, re-finding");
            continue;
        }

        // A re-find is usually a few megabytes from where it was.
        if (lastGood) {
            SetStatus("re-finding the camera");
            uintptr_t lo = lastGood > (8u << 20) ? lastGood - (8u << 20) : 0x10000;
            bytes += ScanRange(lo, lastGood + (8u << 20), hi, cands, 64, 4);
        }
        if (cands.empty()) {
            SetStatus("scanning the client for the camera");
            bytes += ScanRange(0x10000, (uintptr_t)0x7FFFFFFFFFFFull, hi, cands, 128, 16);
        }
        if (cands.empty()) {
            SetStatus("scanning the client for the camera (full)");
            bytes += ScanRange(0x10000, (uintptr_t)0x7FFFFFFFFFFFull, hi, cands, 512, 4);
        }
        log_::Info("[test] camera scan: %zu candidates in %llu ms (%.0f MB)",
                   cands.size(), (unsigned long long)(GetTickCount64() - t0), bytes / 1048576.0);
        if (!g_run) break;
        if (cands.empty()) {
            SetStatus("camera not found - using the server view");
            lastGood = 0;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        if (Confirm(cands)) {
            lastGood = g_addr.load();
            char t[96];
            snprintf(t, sizeof(t), "locked, FOV %.0f", g_fov.load() * 180.f / kPi);
            SetStatus(t);
            SaveCache();
            StartFreshness(cands);
            // A camera-relative matrix needs the camera position as well.
            // This runs in the background while the ESP is already using the
            // matrix with the server eye as the origin.
            if (g_relative.load() && !g_originTurned.load()) HuntOrigin();
            if (!g_run) break;
            // Hold the lock until the render thread reports that the matrix
            // stopped making sense: world change, resolution change, reload.
            while (g_run && g_misses.load() < 120) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_run) break;
            g_addr = 0;
            ++g_lockGen;
            g_origin = 0;
            g_originTurned = false;
            log_::Info("[test] camera lock lost, re-finding");
            continue;
        }
        SetStatus("candidates did not hold up - retrying");
        lastGood = 0;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    SetStatus("off");
}

void Thread() {
    DWORD pid = g_pid.load();
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    g_proc = h;
    if (!h) {
        log_::Error("camera: cannot open client pid %lu (err %lu)", pid, GetLastError());
        SetStatus("cannot read the client process");
        return;
    }
    log_::Info("camera: reading client pid %lu", pid);
    FILETIME c, e, k, u;
    g_clientCreated = GetProcessTimes(h, &c, &e, &k, &u) ? ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime : 0;
    Discover();
    g_addr = 0;
    g_origin = 0;
    g_proc = nullptr;
    CloseHandle(h);
}

} // namespace

void Start(DWORD clientPid) {
    if (g_run || !clientPid) return;
    g_pid = clientPid;
    g_addr = 0;
    g_origin = 0;
    g_originTurned = false;
    g_misses = 0;
    { std::lock_guard<std::mutex> lk(g_mx); g_hint = Hints{}; }
    g_run = true;
    g_thread = std::thread(Thread);
}

void Stop() {
    if (!g_run) return;
    g_run = false;
    ++g_lockGen;
    if (g_thread.joinable()) g_thread.join();
    if (g_freshThread.joinable()) g_freshThread.join();
    g_addr = 0;
}

bool Running() { return g_run; }

void Hint(const float eye[3], float pitch, float yaw, int winW, int winH) {
    float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    Hints h;
    memcpy(h.eye, eye, sizeof(h.eye));
    h.fwd[0] = -sy * cp; h.fwd[1] = sp; h.fwd[2] = -cy * cp;
    h.right[0] = cy;     h.right[1] = 0; h.right[2] = -sy;
    h.up[0] = h.right[1] * h.fwd[2] - h.right[2] * h.fwd[1];
    h.up[1] = h.right[2] * h.fwd[0] - h.right[0] * h.fwd[2];
    h.up[2] = h.right[0] * h.fwd[1] - h.right[1] * h.fwd[0];
    h.w = winW; h.h = winH;
    h.have = true;
    std::lock_guard<std::mutex> lk(g_mx);
    g_hint = h;
}

namespace {
// Last matrix that passed, and how many frames ago. A failed frame reuses it
// rather than dropping to the server camera for one frame, which is a visible
// jump; only a run of failures counts as losing the lock.
View     g_lastGood;
uint32_t g_lastGoodAge = ~0u;

// The game rewrites the matrix every frame. A 64-byte read that overlaps the
// write returns half of each frame's rows, which fails the shape check, and
// it happens exactly while the view is changing. Read until two consecutive
// reads agree.
bool ReadStable(uintptr_t at, float raw[16], bool& torn) {
    float a[16], b[16];
    torn = false;
    if (!ReadMem(at, a, sizeof(a))) return false;
    for (int i = 0; i < 4; ++i) {
        if (!ReadMem(at, b, sizeof(b))) return false;
        if (memcmp(a, b, sizeof(a)) == 0) { memcpy(raw, b, sizeof(b)); return true; }
        torn = true;
        memcpy(a, b, sizeof(a));
    }
    return false;
}

bool Held(View& out) {
    if (g_lastGoodAge < 30 && g_lastGood.valid) { ++g_lastGoodAge; ++g_stHeld; out = g_lastGood; return true; }
    return false;
}
} // namespace

bool Read(View& out) {
    uintptr_t at = g_addr.load();
    if (!at || !g_proc.load()) return false;
    float raw[16];
    bool torn;
    if (!ReadStable(at, raw, torn)) { ++g_misses; ++(torn ? g_stTorn : g_stFail); return Held(out); }
    if (torn) ++g_stTorn;
    if (g_negate.load()) for (int i = 0; i < 16; ++i) raw[i] = -raw[i];

    int layout = g_layout.load();
    Hints hi = GetHint();
    float aspect = hi.h > 0 ? (float)hi.w / (float)hi.h : 0.f;
    Planes p;
    float fovY;
    if (!ShapeOk(raw, layout, aspect, p, fovY)) {
        // A copy we moved to can turn out to be something else when the view
        // changes (seen when sprint widens the FOV). Go back to the copy the
        // lock had before, at once, and never move to this one again.
        uintptr_t prev = g_addrPrev.load();
        float alt[16];
        bool altTorn;
        if (prev && prev != at && ReadStable(prev, alt, altTorn)) {
            if (g_negate.load()) for (int i = 0; i < 16; ++i) alt[i] = -alt[i];
            Planes pa; float fa;
            if (ShapeOk(alt, layout, aspect, pa, fa)) {
                log_::Info("[test] camera: %p stopped passing, back to %p", (void*)at, (void*)prev);
                g_addrBad = at;
                g_addrPrev = 0;
                g_addr = prev;
                return Read(out);
            }
        }
        ++g_misses; ++g_stShape; return Held(out);
    }
    float eye[3];
    if (!SolveEye(p, eye)) { ++g_misses; ++g_stShape; return Held(out); }
    bool relative = g_relative.load();
    if (hi.have) {
        float dx = eye[0] - hi.eye[0], dy = eye[1] - hi.eye[1], dz = eye[2] - hi.eye[2];
        float d = relative ? Len(eye) : std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > kMaxBoom * 2.f) { ++g_misses; ++g_stEye; return Held(out); }   // tracking something else now
    }
    out = View{};
    out.valid = true;
    out.layout = layout;
    memcpy(out.m, raw, sizeof(raw));
    memcpy(out.eye, eye, sizeof(eye));
    if (relative && hi.have) {
        memcpy(out.origin, hi.eye, sizeof(out.origin));
        uintptr_t oa = g_origin.load();
        float v[3];
        if (oa && ReadVec(oa, g_originDbl.load(), v) && Dist(v, hi.eye) <= kMaxBoom) {
            // camera world position minus the camera centre in the matrix's frame
            for (int k = 0; k < 3; ++k) out.origin[k] = v[k] - eye[k];
            memcpy(out.eye, v, sizeof(out.eye));          // the real camera, boom arm included
            out.originFromClient = true;
        }
    }
    float lw = Len(p.W.g);
    for (int i = 0; i < 3; ++i) out.fwd[i] = p.W.g[i] / lw;
    out.fovY = fovY;
    out.aspect = Len(p.Y.g) / Len(p.X.g);
    out.xSign = g_xSign.load();
    out.ySign = g_ySign.load();
    out.relative = relative;
    g_fov = fovY;
    g_misses = 0;
    g_lastGood = out;
    g_lastGoodAge = 0;
    ++g_stOk;
    return true;
}

bool GetLock(Lock& out) {
    out = Lock{};
    out.addr = g_addr.load();
    out.addrAlt = g_addrPrev.load();
    if (!out.addr) return false;
    out.layout = g_layout.load();
    out.negate = g_negate.load();
    out.relative = g_relative.load();
    out.xSign = g_xSign.load();
    out.ySign = g_ySign.load();
    out.origin = g_origin.load();
    out.originDbl = g_originDbl.load();
    return true;
}

std::string TakeStats() {
    char t[160];
    snprintf(t, sizeof(t), "ok=%u torn=%u held=%u shape=%u eye=%u fail=%u",
             g_stOk.exchange(0), g_stTorn.exchange(0), g_stHeld.exchange(0),
             g_stShape.exchange(0), g_stEye.exchange(0), g_stFail.exchange(0));
    return t;
}

bool Project(const View& v, float x, float y, float z, float& ndcX, float& ndcY) {
    if (!v.valid) return false;
    const float p[3] = { x - v.origin[0], y - v.origin[1], z - v.origin[2] };
    Planes pl = Extract(v.m, v.layout);
    float w = Dot(pl.W.g, p) + pl.W.c;
    if (!(w > 0.05f)) return false;
    ndcX = (Dot(pl.X.g, p) + pl.X.c) / w;
    ndcY = (Dot(pl.Y.g, p) + pl.Y.c) / w;
    return std::isfinite(ndcX) && std::isfinite(ndcY);
}

std::string Status() { std::lock_guard<std::mutex> lk(g_mx); return g_status; }

} // namespace cam
