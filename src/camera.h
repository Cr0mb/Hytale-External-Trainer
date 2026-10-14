// =============================================================================
//  camera.h — the client's own camera, read out of HytaleClient.exe.
//
//  Why: the server only knows your head rotation at tick rate (~30 Hz) and one
//  tick behind your mouse, so an ESP camera rebuilt from it trails and steps.
//  It also knows nothing about the third-person boom arm or the real field of
//  view. The client has the finished world->clip matrix for the frame it is
//  drawing, so the ESP uses that instead and matches the picture exactly.
//
//  How: the worker opens the client read-only (PROCESS_VM_READ, nothing is
//  written and no code runs in the client) and a discovery thread looks for a
//  4x4 float matrix that behaves like a view-projection matrix. Nothing is
//  hard-coded: every candidate has to satisfy the algebra a view-projection
//  matrix must satisfy (orthogonal basis, unit forward row, aspect ratio equal
//  to the client window, plausible field of view) and has to agree with where
//  the server says the player is, over several samples taken while the player
//  moves. Once a candidate is locked the render thread reads those 64 bytes
//  per frame; the address is re-checked every frame and re-found if the world
//  or the renderer moves it.
// =============================================================================
#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

namespace cam {

// What the render thread needs to project a world point.
struct View {
    bool  valid = false;
    float m[16]{};        // world -> clip, as the client stores it
    int   layout = 0;     // 0 = clip is p * M, 1 = clip is M * p
    float eye[3]{};       // camera position solved from the matrix (boom arm included)
    float fwd[3]{};       // camera forward
    float fovY = 0;       // vertical field of view, radians
    float aspect = 0;
    float xSign = 1.f, ySign = 1.f;   // clip-axis orientation, resolved at lock time
    bool  relative = false;           // matrix expects positions relative to the camera
    float origin[3]{};                // subtract this before projecting (zero unless relative)
    bool  originFromClient = false;   // origin is the client's own camera position, not the server eye
    uint32_t misses = 0;              // consecutive frames the matrix failed its checks
};

void Start(DWORD clientPid);
void Stop();
bool Running();

// Server truth, pushed once per frame: eye position, look direction and the
// client window size. Discovery needs it; so does the per-frame re-check.
void Hint(const float eye[3], float pitch, float yaw, int winW, int winH);

// Render thread: reads the locked matrix (64 bytes) and re-checks it.
// False means "no client camera right now, use the server fallback".
bool Read(View& out);

// Projects a world position to normalized device coordinates. False means the
// point is behind the camera.
bool Project(const View& v, float x, float y, float z, float& ndcX, float& ndcY);

std::string Status();     // one short line for the VISUALS tab

// The current lock, for a renderer that reads the matrix itself (the in-game
// renderer reads it inside the client at the moment the frame is presented).
struct Lock {
    uintptr_t addr = 0, origin = 0;
    uintptr_t addrAlt = 0;        // previous copy, used if addr stops passing
    int   layout = 0;
    bool  negate = false, relative = false, originDbl = false;
    float xSign = 1.f, ySign = 1.f;
};
bool GetLock(Lock& out);

// Per-frame read outcomes since the last call (and resets them):
// "ok=N torn=N held=N shape=N eye=N fail=N". For the 5 s [test] log line.
std::string TakeStats();

} // namespace cam
