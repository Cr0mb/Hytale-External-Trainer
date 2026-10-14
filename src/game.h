// =============================================================================
//  game.h — Hytale model + feature worker.
//
//  Hytale single-player runs a local Java server (HytaleServer.jar
//  --singleplayer) next to the NativeAOT client, and that server owns every
//  gameplay value. The worker finds that server, loads the trainer agent into
//  it (see attach.h) and talks to the agent over a loopback socket. Dedicated /
//  multiplayer servers are never touched: only a JVM started with
//  --singleplayer by the running HytaleClient qualifies, and the agent checks
//  the flag again itself.
// =============================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace game {

struct Vec3 { float x, y, z; };

enum class Kind : char { Hostile = 'H', Neutral = 'N', Friendly = 'F', Item = 'I', Player = 'P' };

struct Entity {
    Kind  kind;
    int   id = 0;       // entity index in its store, stable while it lives
    Vec3  pos;          // feet
    float height;
    float hp, maxHp;    // items: hp = stack size
    std::string name;
};

struct Snapshot {
    bool  attached = false, havePlayer = false;
    int   players = 0;                 // players connected to this world's server
    bool  shared = false;              // someone else is in your world right now
    Vec3  pos{};                       // feet
    float pitch = 0, yaw = 0, eye = 1.6f;   // radians; forward = (-sin yaw cos p, sin p, -cos yaw cos p)
    float hp = 0, hpMax = 0, stam = 0, stamMax = 0, mana = 0, manaMax = 0, oxy = 0, oxyMax = 0;
    float dayProgress = 0;             // 0..1, 0 = midnight
    std::string mode, world;
    bool  filterOk = true;             // damage hooks registered
    int   gameX = 0, gameY = 0, gameW = 0, gameH = 0;   // client area of the game window (screen coords)
    std::vector<Entity> ents;
    // Server data only changes once per world tick (~30 Hz). seq is the
    // agent's tick counter; sampleTime is when this trainer first saw that
    // tick (seconds, steady clock), so the renderer can interpolate between
    // ticks instead of stepping.
    uint64_t seq = 0;
    double   sampleTime = 0;
    long long swings = 0, ghosted = 0, hits = 0, broken = 0, duped = 0, pulled = 0, homed = 0, bullets = 0;
};

struct Config {
    // player
    std::atomic<bool>  god{false}, infHp{false}, infStam{false}, infMana{false}, infOxy{false}, noFall{false};
    // movement
    std::atomic<bool>  speedOn{false}, jumpOn{false}, fly{false}, flySpeedOn{false}, noclip{false};
    std::atomic<float> speed{2.f}, jump{1.5f}, flySpeed{2.f};
    // combat
    std::atomic<bool>  oneHit{false}, dmgOn{false}, infDur{false}, infAmmo{false}, freezeMobs{false};
    std::atomic<float> dmgMult{3.f}, killRadius{48.f};
    // auto attack (creatures only)
    std::atomic<bool>  autoAttack{false}, aaFrontOnly{false};
    std::atomic<float> aaRadius{4.f}, aaRate{3.f}, aaDamage{40.f};
    // combat targets (all combat features) and on-swing features
    std::atomic<bool>  tgtHostile{true}, tgtNeutral{false}, tgtFriendly{false}, tgtPlayers{false};
    std::atomic<bool>  swingAura{false}, swingReach{false}, ghostHit{false};
    std::atomic<float> reachRange{8.f}, ghostRange{8.f}, auraMax{1.f};
    std::atomic<bool>  instantBreak{false};
    std::atomic<float> breakRange{6.f};
    // drops
    std::atomic<bool>  vacuum{false}, autoDupe{false}, fastPickup{false};
    std::atomic<bool>  projAim{false}, magicBullet{false};
    std::atomic<float> projRange{64.f}, projCone{25.f};
    std::atomic<float> vacuumRadius{12.f}, dupeMult{2.f};
    // world
    std::atomic<bool>  freezeTime{false};
    // esp
    std::atomic<bool>  esp{false}, espHostile{true}, espNeutral{true}, espFriendly{false}, espItems{false};
    std::atomic<bool>  espPlayers{true};
    std::atomic<bool>  espLines{false}, espInfo{true}, espHealth{true};
    std::atomic<bool>  espSmooth{true};           // interpolate server ticks between frames
    std::atomic<bool>  inGameEsp{true};           // draw inside the game's frame (hook/render.cpp)
    std::atomic<bool>  espServerFallback{false};  // draw with the laggy server view until the camera locks
    // player stats overlay
    std::atomic<bool>  statsHud{false};
    // position: offset from the game view's top-left, set by dragging it
    std::atomic<float> espMaxDist{64.f}, fov{75.f};
    std::atomic<bool>  useClientCam{true};        // read the camera out of the client (see camera.h)
    // Shared worlds: features that change the world for everyone stay off
    // unless the host turns this on deliberately.
    std::atomic<bool>  allowShared{false};
};

Config& Cfg();

void StartWorker();
void StopWorker();               // agent restores everything, then unloads
bool Attached();
bool ClientOnly();       // visual features only, no local server
bool ClientOnly();       // visual features only, no local server
bool ClientOnly();       // visual features only, no local server
unsigned long ClientPid();       // 0 when not attached
std::string Status();

// True while another player is in your world and shared-world features are
// therefore held back. The UI disables them and the agent refuses them.
bool SharedLocked();
Snapshot GetSnapshot();
std::shared_ptr<const Snapshot> SnapshotPtr();   // no copy; for the per-frame render path
double NowSeconds();                             // the clock sampleTime is on
struct ItemInfo {
    std::string id, name, category, subCategory, kind, quality;
    int maxStack = 1;
};
std::vector<ItemInfo> Items();       // every item asset, by id (empty until attached)
std::vector<std::string> Weathers(); // weather ids, sorted

// one-shot actions; the result line shows in the menu
void Action(const std::string& cmd);   // e.g. "heal", "time 12", "give Tool_Pickaxe_Iron 1"
std::string LastResult();

// saved positions (trainer side, stored in the config file)
struct Waypoint { std::string name; Vec3 pos; };
std::vector<Waypoint> Waypoints();
void SaveWaypoint(const std::string& name);   // current position
void DeleteWaypoint(size_t i);
void GotoWaypoint(size_t i);

std::string SaveConfig();
std::string LoadConfig();

int SelfTest(const wchar_t* reportPath, const wchar_t* script = nullptr);   // attach + snapshot; optional agent script
int EntProbe(const wchar_t* reportPath);   // where do server entities live in client memory? (read-only)

} // namespace game
