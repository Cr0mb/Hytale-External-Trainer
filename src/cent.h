// =============================================================================
//  cent.h — the client's own entity objects (HytaleClient.exe).
//
//  Same approach as the YAZD / Skyrim / Terraria trainers: draw entities at
//  the positions the game itself renders them at, read from the game client,
//  instead of at the server's positions (30 Hz and a tick or two behind
//  what the client shows).
//
//  The client is NativeAOT with no metadata, so the layout is discovered at
//  runtime, read-only, and nothing is hard-coded:
//    1. the server says exactly where every entity stands;
//    2. client memory is scanned for a block of three float3 positions on
//       those spots;
//    3. every NativeAOT object starts with its MethodTable pointer, and the
//       MethodTable records the object size (BaseSize, at +4). The nearest
//       pointer in front of a block that reads as a MethodTable whose size
//       covers the block is the object header. The (type, offset) that
//       several different entities agree on is the entity layout.
//       Measured on 0.7.0: type image+0x2B3F3D0, BaseSize 0x4F8, positions
//       at +0x450 (pigs, sheep, dropped items alike).
//    4. entity objects are enumerated by scanning the heap for that type
//       pointer, and their positions are read per frame.
//  The layout is cached per client build (PE timestamp + image size) and
//  re-verified against the server on every start.
//
//  The block holds three float3, measured on a walking entity at 60 fps:
//    [0] previous tick, [1] current tick, [2] the interpolated position the
//  game draws, always between the other two. The ESP uses [2].
// =============================================================================
#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cent {

constexpr int kRenderSlot = 2;          // see above

struct Layout {
    uintptr_t mt = 0;       // type pointer (absolute, in this client process)
    uintptr_t mt2 = 0;      // your own character, when it is a different class
    int  posOff = 0;        // offset of the three-float3 block in the object
    bool ok = false;
};

void Start(DWORD clientPid);
void Stop();
bool Running();

// Server truth for discovery and verification: your feet and every entity.
void Hint(const float me[3], const std::vector<std::array<float, 3>>& ents);

bool GetLayout(Layout& out);
std::vector<uintptr_t> Objects();     // live entity objects, refreshed in the background
uintptr_t PlayerObject();             // your own character's object, 0 until identified
std::string Status();

// Out of process (overlay path): the rendered position of one object.
bool ReadPos(uintptr_t obj, const Layout& l, float out[3]);

} // namespace cent
