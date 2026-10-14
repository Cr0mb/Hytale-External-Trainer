// =============================================================================
//  render.cpp — in-game ESP renderer (hytrainer-render.dll).
//
//  Why: an external overlay is a second renderer with its own present queue.
//  The game's OpenGL driver holds one to three frames before they reach the
//  screen, the overlay holds its own, and the two never line up while the
//  camera moves. Drawing into the game's back buffer right before it is
//  presented makes the boxes part of the same frame, so they cannot drift.
//
//  How:
//    - The trainer injects this DLL into HytaleClient.exe only while it is
//      attached to your single-player world, after creating the shared block
//      (shm.h) it reads from.
//    - The hook is a pointer swap in SDL3.dll's import table for
//      GDI32!SwapBuffers. No game code is patched; unhooking writes the
//      original pointer back.
//    - On every SwapBuffers the ESP is drawn with Dear ImGui's OpenGL3 backend
//      (which saves and restores the GL state it touches) using the client's
//      camera matrix read directly from this process at that moment.
//    - Everything runs under SEH: a fault disables drawing instead of taking
//      the game down. The DLL unhooks and unloads itself when the trainer asks,
//      or when the trainer has been silent for three seconds (crash, kill).
// =============================================================================
#include <Windows.h>
#include <gl/GL.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "esp_draw.h"
#include "shm.h"
#include "viewproj.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>

#pragma comment(lib, "opengl32.lib")

namespace {

constexpr GLenum kDrawFramebuffer = 0x8CA9;          // GL_DRAW_FRAMEBUFFER
constexpr GLenum kDrawFramebufferBinding = 0x8CA6;   // GL_DRAW_FRAMEBUFFER_BINDING
constexpr GLenum kFramebufferSrgb = 0x8DB9;          // GL_FRAMEBUFFER_SRGB

using SwapFn = BOOL(WINAPI*)(HDC);
using BindFbFn = void(APIENTRY*)(GLenum, GLuint);

HMODULE       g_self = nullptr;
HANDLE        g_map = nullptr, g_mutex = nullptr;
shm::Block*   g_blk = nullptr;
void**        g_slot = nullptr;
SwapFn        g_orig = nullptr;
BindFbFn      g_bindFb = nullptr;

volatile LONG g_inflight = 0;
volatile LONG g_wantQuit = 0;
volatile LONG g_glDone = 0;
bool          g_glInit = false;
bool          g_broken = false;
double        g_lastFrameMs = 0;
float         g_fps = 0;

shm::Block    g_copy;                     // this frame's copy of the trainer data
espdraw::Ent  g_ents[shm::kMaxEnts];
float         g_cpos[shm::kMaxObjs][3];
uint64_t      g_cobj[shm::kMaxObjs];

// Per entity across frames: which client object it is paired with, and the
// offset still being faded out after its position source changed.
struct Track {
    uint64_t obj = 0;
    float    last[3]{}, offset[3]{};
    double   lastMs = 0;
    bool     client = false;
};
std::unordered_map<uint64_t, Track> g_tracks;
constexpr uint64_t kMeKey = ~0ull;

// Last camera that passed, kept for short gaps (a re-lock, one bad read).
espdraw::Projector g_lastProj;
double             g_lastProjMs = -1e9;

bool SafeCopy(void* dst, const void* src, size_t n);

// Rendered positions of the client's entity objects, read in this process at
// the moment the frame is presented.
int ReadClientPositions(const shm::Block& b) {
    if (!b.entMt) return 0;
    int n = b.objCount < 0 ? 0 : b.objCount > shm::kMaxObjs ? shm::kMaxObjs : b.objCount, m = 0;
    for (int i = 0; i < n; ++i) {
        uint64_t mt = 0;
        uint64_t o = b.objs[i];
        if (!SafeCopy(&mt, (const void*)o, sizeof(mt)) || (mt != b.entMt && (!b.entMt2 || mt != b.entMt2))) continue;   // moved or freed
        float* v = g_cpos[m];
        g_cobj[m] = o;
        if (!SafeCopy(v, (const void*)(o + b.entPosOff + b.entSlot * 12), 12)) continue;
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
        ++m;
    }
    return m;
}

void SetError(const char* msg) {
    if (!g_blk) return;
    strncpy_s(g_blk->rendererError, msg, _TRUNCATE);
}

// ---------------------------------------------------------------------------
//  Import-table hook
// ---------------------------------------------------------------------------
void** FindIatSlot(HMODULE mod, const char* dll, const char* fn) {
    auto* base = (BYTE*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn) == 0) return (void**)&iat->u1.Function;
        }
    }
    return nullptr;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// ---------------------------------------------------------------------------
//  Guarded helpers (no C++ objects in here: SEH only)
// ---------------------------------------------------------------------------
bool SafeCopy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadVec3(uint64_t addr, bool dbl, float out[3]) {
    if (dbl) {
        double d[3];
        if (!SafeCopy(d, (const void*)addr, sizeof(d))) return false;
        out[0] = (float)d[0]; out[1] = (float)d[1]; out[2] = (float)d[2];
    } else if (!SafeCopy(out, (const void*)addr, sizeof(float) * 3)) {
        return false;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Copies the trainer's block under its sequence lock. Keeps the previous copy
// if the writer is mid-update every time we look (it never is for long).
bool CopyBlock() {
    const size_t n = offsetof(shm::Block, rendererBeatMs);
    for (int i = 0; i < 8; ++i) {
        LONG s1 = g_blk->seq;
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(&g_copy, g_blk, n);
        MemoryBarrier();
        if (g_blk->seq == s1) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  One frame
// ---------------------------------------------------------------------------
void Lerp3(const float a[3], const float b[3], float t, float out[3]) {
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    if (dx * dx + dy * dy + dz * dz > 64.f) t = 1.f;          // teleported: do not slide
    out[0] = a[0] + dx * t; out[1] = a[1] + dy * t; out[2] = a[2] + dz * t;
}

void Frame(HDC dc) {
    double now = shm::NowMs();
    CopyBlock();
    const shm::Block& b = g_copy;
    if (!b.enabled || now - b.trainerBeatMs > shm::kStaleDrawMs) return;

    HWND hwnd = WindowFromDC(dc);
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    const float W = (float)rc.right, H = (float)rc.bottom;

    if (!g_glInit) {
        g_bindFb = (BindFbFn)wglGetProcAddress("glBindFramebuffer");
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        if (!ImGui_ImplOpenGL3_Init()) {
            ImGui::DestroyContext();
            g_broken = true;
            SetError("ImGui OpenGL3 backend failed to initialise");
            return;
        }
        g_glInit = true;
        g_lastFrameMs = now;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(W, H);
    double dt = (now - g_lastFrameMs) / 1000.0;
    io.DeltaTime = (float)(dt > 0.0001 && dt < 0.5 ? dt : 1.0 / 60.0);
    if (dt > 0.0001 && dt < 0.5) g_fps = g_fps * 0.9f + (float)(1.0 / dt) * 0.1f;
    g_lastFrameMs = now;

    // Tick interpolation, evaluated at the moment this frame is presented.
    float alpha = 1.f;
    if (b.smooth && b.intervalMs > 1.0) {
        double t = (now - b.curTimeMs) / b.intervalMs;
        alpha = (float)(t < 0 ? 0 : t > 1 ? 1 : t);
    }
    float me[3];
    if (b.meHasPrev) Lerp3(b.mePrev, b.me, alpha, me);
    else { me[0] = b.me[0]; me[1] = b.me[1]; me[2] = b.me[2]; }
    // The camera origin must be where you are now, not where the server
    // last saw you: the client predicts your movement and is ahead of it.
    // Extrapolate the last two ticks forward (at most two ticks, and not
    // across a teleport). Used only until the client's own camera position
    // is known.
    // Entities and you, at the positions the game is drawing this frame.
    int n = b.count < 0 ? 0 : b.count > shm::kMaxEnts ? shm::kMaxEnts : b.count;
    for (int i = 0; i < n; ++i) {
        const shm::Ent& s = b.ents[i];
        espdraw::Ent& e = g_ents[i];
        e.kind = s.kind;
        if (s.hasPrev) Lerp3(s.prev, s.pos, alpha, e.pos);
        else { e.pos[0] = s.pos[0]; e.pos[1] = s.pos[1]; e.pos[2] = s.pos[2]; }
        e.height = s.height; e.hp = s.hp; e.maxHp = s.maxHp;
        g_copy.ents[i].name[sizeof(s.name) - 1] = 0;
        e.name = g_copy.ents[i].name;
    }
    float clientMe[3] = { me[0], me[1], me[2] };
    bool meClient = false;
    // Your own character, read like any entity: the three slots are previous
    // tick, current tick, and the interpolated position this frame shows.
    float pSlots[9];
    bool havePlayer = false;
    if (b.playerObj && b.entMt) {
        uint64_t mt = 0;
        havePlayer = SafeCopy(&mt, (const void*)b.playerObj, sizeof(mt)) && (mt == b.entMt || (b.entMt2 && mt == b.entMt2)) &&
                     SafeCopy(pSlots, (const void*)(b.playerObj + b.entPosOff), sizeof(pSlots)) &&
                     std::isfinite(pSlots[6]) && std::isfinite(pSlots[7]) && std::isfinite(pSlots[8]);
    }
    int cm = ReadClientPositions(b);
    {
        static std::unordered_map<uint64_t, int> index;
        static std::vector<char> used;
        index.clear();
        for (int j = 0; j < cm; ++j) index[g_cobj[j]] = j;
        used.assign(cm, 0);
        auto d2 = [](const float* a, const float* c) {
            float dx = a[0] - c[0], dy = a[1] - c[1], dz = a[2] - c[2];
            return dx * dx + dy * dy + dz * dz;
        };
        // 1) keep existing pairs that still make sense
        auto keep = [&](uint64_t key, const float* target, float maxD) -> int {
            auto it = g_tracks.find(key);
            if (it == g_tracks.end() || !it->second.obj) return -1;
            auto f = index.find(it->second.obj);
            if (f == index.end() || used[f->second] || d2(g_cpos[f->second], target) > maxD * maxD) { it->second.obj = 0; return -1; }
            used[f->second] = 1;
            return f->second;
        };
        // 2) pair the rest with the nearest free object
        auto pair = [&](uint64_t key, const float* target, float maxD) -> int {
            int best = -1;
            float bd = maxD * maxD;
            for (int j = 0; j < cm; ++j) {
                if (used[j]) continue;
                float d = d2(g_cpos[j], target);
                if (d < bd) { bd = d; best = j; }
            }
            if (best >= 0) { used[best] = 1; g_tracks[key].obj = g_cobj[best]; }
            return best;
        };
        // 3) the drawn position: the client one when paired, else the
        //    server one, with any jump between the two faded out.
        auto settle = [&](uint64_t key, int j, const float* serverPos, float out[3]) {
            Track& t = g_tracks[key];
            const float* src = j >= 0 ? g_cpos[j] : serverPos;
            bool isClient = j >= 0;
            if (t.lastMs > 0 && isClient != t.client)
                for (int k = 0; k < 3; ++k) t.offset[k] = t.last[k] - src[k];
            float decay = t.lastMs > 0 ? (float)std::exp(-(now - t.lastMs) / 120.0) : 0.f;
            for (int k = 0; k < 3; ++k) { t.offset[k] *= decay; out[k] = src[k] + t.offset[k]; }
            memcpy(t.last, out, sizeof(t.last));
            t.lastMs = now;
            t.client = isClient;
        };
        auto keyOf = [](const shm::Ent& s) { return ((uint64_t)(uint8_t)s.kind << 32) | (uint32_t)s.id; };

        // your own object is you, never an entity
        if (havePlayer) { auto f = index.find(b.playerObj); if (f != index.end()) used[f->second] = 1; }
        int meJ = havePlayer ? -2 : keep(kMeKey, me, 4.f);
        std::vector<int> js(n, -1);
        for (int i = 0; i < n; ++i) js[i] = keep(keyOf(b.ents[i]), g_ents[i].pos, 3.f);
        if (meJ == -1) meJ = pair(kMeKey, me, 2.5f);
        for (int i = 0; i < n; ++i) if (js[i] < 0) js[i] = pair(keyOf(b.ents[i]), g_ents[i].pos, 1.5f);

        int matched = 0;
        for (int i = 0; i < n; ++i) {
            float server[3] = { g_ents[i].pos[0], g_ents[i].pos[1], g_ents[i].pos[2] };
            settle(keyOf(b.ents[i]), js[i], server, g_ents[i].pos);
            matched += js[i] >= 0;
        }
        if (havePlayer) {
            meClient = true;
            memcpy(clientMe, pSlots + 3 * 2, sizeof(clientMe));      // interpolated slot
        } else {
            meClient = meJ >= 0;
            settle(kMeKey, meJ, me, clientMe);
        }
        memcpy(me, clientMe, sizeof(me));

        // forget entities not seen for two seconds
        for (auto it = g_tracks.begin(); it != g_tracks.end(); )
            it = now - it->second.lastMs > 2000.0 ? g_tracks.erase(it) : std::next(it);

        InterlockedExchangeAdd(&g_blk->entsMatched, matched);
        InterlockedExchangeAdd(&g_blk->entsTotal, n);
        if (meClient) InterlockedIncrement(&g_blk->meFromClient);
    }

    float eye[3] = { b.me[0], b.me[1] + b.eyeHeight, b.me[2] };
    if (meClient) { eye[0] = me[0]; eye[1] = me[1] + b.eyeHeight; eye[2] = me[2]; }
    else if (b.meHasPrev && b.intervalMs > 1.0) {
        float lead = (float)((now - b.curTimeMs) / b.intervalMs);
        lead = lead < 0 ? 0 : lead > 2 ? 2 : lead;
        float d[3] = { b.me[0] - b.mePrev[0], b.me[1] - b.mePrev[1], b.me[2] - b.mePrev[2] };
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 64.f)
            for (int i = 0; i < 3; ++i) eye[i] += d[i] * lead;
    }

    espdraw::Projector pj;
    pj.cx = W * 0.5f; pj.cy = H * 0.5f; pj.halfW = W * 0.5f; pj.halfH = H * 0.5f;
    bool client = false;
    if (b.camAddr) {
        float raw[16];
        bool got = false;
        for (uint64_t addr : { b.camAddr, b.camAddrAlt }) {
            if (!addr || !SafeCopy(raw, (const void*)addr, sizeof(raw))) continue;
            if (b.negate) for (float& v : raw) v = -v;
            if (vp::LooksValid(raw, b.layout)) { got = true; break; }
        }
        if (got) {
            {
                client = true;
                pj.client = true;
                memcpy(pj.m, raw, sizeof(raw));
                pj.layout = b.layout;
                pj.xSign = b.xSign;
                pj.ySign = b.ySign;
                if (b.relative && havePlayer) {
                    // Camera position from your own character, on the same
                    // frame as everything else: eye = rendered position + eye
                    // height. In third person the camera sits behind that on the
                    // view axis; its distance comes from the camera position the
                    // hunt found, measured against your object in the same tick,
                    // so any lag in that copy cancels out.
                    float centre[3] = { 0, 0, 0 };
                    vp::SolveCenter(raw, b.layout, centre);
                    float E[3] = { me[0], me[1] + b.eyeHeight, me[2] };
                    float clen = std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
                    if (clen > 0.5f) {
                        memcpy(pj.origin, E, sizeof(E));          // matrix built around the eye: boom is inside it
                    } else {
                        static float boomLen = 0.f;
                        float L = 0.f;
                        if (b.originAddr) {
                            const uint64_t stride = b.originDbl ? 24 : 12;
                            const uint64_t base = b.originAddr - 2 * stride;
                            float best = 1e9f;
                            for (int k = 0; k < 3; ++k) {
                                float c3[3];
                                if (!ReadVec3(base + k * stride, b.originDbl != 0, c3)) continue;
                                for (int j = 0; j < 3; ++j) {
                                    float dx = c3[0] - pSlots[j * 3], dy = c3[1] - (pSlots[j * 3 + 1] + b.eyeHeight), dz = c3[2] - pSlots[j * 3 + 2];
                                    best = (std::min)(best, std::sqrt(dx * dx + dy * dy + dz * dz));
                                }
                            }
                            if (best > 1.5f && best < 12.f) L = best;     // under 1.5: first person (lag, not a boom)
                        }
                        boomLen = L == 0.f ? 0.f : (boomLen == 0.f ? L : boomLen * 0.85f + L * 0.15f);
                        float fw[3] = { b.layout == 0 ? raw[3] : raw[12], b.layout == 0 ? raw[7] : raw[13], b.layout == 0 ? raw[11] : raw[14] };
                        float fl = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
                        for (int k = 0; k < 3; ++k) pj.origin[k] = E[k] - fw[k] / (fl > 0 ? fl : 1.f) * boomLen;
                    }
                    InterlockedIncrement(&g_blk->framesOwnOrigin);
                } else if (b.relative) {
                    float cp[3];
                    bool own = b.originAddr && ReadVec3(b.originAddr, b.originDbl != 0, cp);
                    if (own) {
                        float dx = cp[0] - eye[0], dy = cp[1] - eye[1], dz = cp[2] - eye[2];
                        own = dx * dx + dy * dy + dz * dz <= 100.f;
                    }
                    // origin = camera world position - camera centre in the
                    // matrix's frame. Right whether the game builds the matrix
                    // around the camera (centre 0) or around the eye with the
                    // third-person boom baked in (centre = boom offset).
                    float centre[3] = { 0, 0, 0 };
                    if (own && vp::SolveCenter(raw, b.layout, centre))
                        for (int k = 0; k < 3; ++k) cp[k] -= centre[k];
                    memcpy(pj.origin, own ? cp : eye, sizeof(pj.origin));
                    if (own) InterlockedIncrement(&g_blk->framesOwnOrigin);
                }
            }
        }
    }
    if (client) {
        g_lastProj = pj;
        g_lastProjMs = now;
    } else if (now - g_lastProjMs < 400.0) {
        // brief gap: keep the last camera rather than blanking or jumping
        float origin[3];
        memcpy(origin, g_lastProj.origin, sizeof(origin));
        pj = g_lastProj;
        if (b.relative) memcpy(pj.origin, eye, sizeof(pj.origin));
        client = true;
    }
    if (!client) {
        if (!b.serverFallback) {
            // No client camera yet: draw nothing rather than boxes that trail.
            InterlockedIncrement(&g_blk->framesHidden);
            g_blk->rendererBeatMs = now;
            InterlockedIncrement(&g_blk->frames);
            return;
        }
        pj.SetServerView(eye, b.pitch, b.yaw, b.fovDeg);
    }
    InterlockedIncrement(client ? &g_blk->framesClient : &g_blk->framesServer);

    espdraw::Options o;
    o.hostile = b.hostile != 0; o.neutral = b.neutral != 0; o.friendly = b.friendly != 0;
    o.players = b.players != 0; o.items = b.items != 0;
    o.lines = b.lines != 0; o.info = b.info != 0; o.health = b.health != 0;
    o.maxDist = b.maxDist;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    espdraw::Draw(ImGui::GetBackgroundDrawList(), ImGui::GetFont(), pj, o, me, g_ents, n, W, H, ImVec2(W * 0.5f, H));
    ImGui::Render();

    // Draw into the default framebuffer (the one being presented), with the
    // colours as given. The backend restores everything else it changes.
    GLint fbo = 0;
    glGetIntegerv(kDrawFramebufferBinding, &fbo);
    if (fbo && g_bindFb) g_bindFb(kDrawFramebuffer, 0);
    GLboolean srgb = glIsEnabled(kFramebufferSrgb);
    if (srgb) glDisable(kFramebufferSrgb);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (srgb) glEnable(kFramebufferSrgb);
    if (fbo && g_bindFb) g_bindFb(kDrawFramebuffer, (GLuint)fbo);

    g_blk->rendererBeatMs = now;
    g_blk->rendererFps = g_fps;
    InterlockedIncrement(&g_blk->frames);
}

void Shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    g_glInit = false;
}

void GuardedFrame(HDC dc) {
    __try { Frame(dc); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_broken = true;
        SetError("fault while drawing; in-game ESP disabled for this session");
    }
}

void GuardedShutdown() {
    __try { Shutdown(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_glInit = false; }
}

BOOL WINAPI HookedSwapBuffers(HDC dc) {
    InterlockedIncrement(&g_inflight);
    if (g_wantQuit) {
        if (g_glInit) GuardedShutdown();      // GL objects go while their context is current
        g_glDone = 1;
    } else if (!g_broken) {
        GuardedFrame(dc);
    }
    BOOL r = g_orig(dc);
    InterlockedDecrement(&g_inflight);
    return r;
}

// ---------------------------------------------------------------------------
//  Lifetime
// ---------------------------------------------------------------------------
void Leave(LONG state) {
    if (g_blk) { g_blk->rendererState = state; UnmapViewOfFile(g_blk); g_blk = nullptr; }
    if (g_map) { CloseHandle(g_map); g_map = nullptr; }
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}

DWORD WINAPI Main(LPVOID) {
    DWORD pid = GetCurrentProcessId();
    wchar_t name[64];

    // One renderer per client, even if a second copy gets loaded.
    shm::MutexName(name, 64, pid);
    g_mutex = CreateMutexW(nullptr, TRUE, name);
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex) CloseHandle(g_mutex);
        g_mutex = nullptr;
        FreeLibraryAndExitThread(g_self, 0);
    }

    shm::MappingName(name, 64, pid);
    g_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (g_map) g_blk = (shm::Block*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shm::Block));
    if (!g_blk || g_blk->magic != shm::kMagic || g_blk->version != shm::kVersion) {
        Leave(shm::kFailed);
        FreeLibraryAndExitThread(g_self, 0);
    }

    HMODULE sdl = GetModuleHandleW(L"SDL3.dll");
    g_slot = sdl ? FindIatSlot(sdl, "GDI32.dll", "SwapBuffers") : nullptr;
    if (!g_slot) {
        SetError("SDL3.dll does not import GDI32!SwapBuffers");
        Leave(shm::kFailed);
        FreeLibraryAndExitThread(g_self, 0);
    }
    g_orig = (SwapFn)*g_slot;
    if (!WriteSlot(g_slot, (void*)&HookedSwapBuffers)) {
        SetError("cannot write SDL3.dll's import table");
        Leave(shm::kFailed);
        FreeLibraryAndExitThread(g_self, 0);
    }
    g_blk->rendererState = shm::kHooked;

    for (;;) {
        Sleep(50);
        if (g_blk->quit) break;
        if (shm::NowMs() - g_blk->trainerBeatMs > shm::kStaleUnloadMs) break;   // trainer gone
    }

    // Release GL objects on the render thread, then take the hook out.
    g_wantQuit = 1;
    for (int i = 0; i < 100 && g_glInit && !g_glDone; ++i) Sleep(20);

    if (*g_slot != (void*)&HookedSwapBuffers) {
        // Someone hooked the same slot after us; pulling ours out would cut
        // them off. Stay loaded as a pass-through instead of unloading.
        Leave(shm::kUnloaded);
        return 0;
    }
    WriteSlot(g_slot, (void*)g_orig);
    Sleep(100);                                   // a call that already read the slot
    while (g_inflight) Sleep(5);
    Sleep(20);
    Leave(shm::kUnloaded);
    FreeLibraryAndExitThread(g_self, 0);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        if (HANDLE t = CreateThread(nullptr, 0, Main, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
