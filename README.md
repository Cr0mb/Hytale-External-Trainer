# Hytale Trainer

A single-file trainer for **Hytale single-player**, with an ImGui overlay menu and an ESP drawn inside the game's own frame.

<p>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D6">
  <img alt="Game" src="https://img.shields.io/badge/Hytale%20server-0.7.0-5865F2">
  <img alt="Mode" src="https://img.shields.io/badge/mode-single--player%20only-2EA043">
  <img alt="Build" src="https://img.shields.io/badge/build-MSVC%20%2B%20CMake-A371F7">
</p>

> **Single-player only.** The trainer attaches to the local Java server that Hytale starts for
> your own world. Join someone else's world and there is no local server, so it stays detached
> and every feature is unavailable. This is the design, not a fault — see
> [Limitations](#limitations). Nothing is written to the game client and no code runs inside it.

---

## Contents

- [Requirements](#requirements)
- [Install](#install)
- [Usage](#usage)
- [Features](#features)
- [Shared worlds](#shared-worlds)
- [How it works](#how-it-works)
- [Technical notes](#technical-notes)
- [Building from source](#building-from-source)
- [Diagnostics](#diagnostics)
- [Limitations](#limitations)
- [License](#license)

---

## Requirements

| | |
|---|---|
| OS | Windows 10 / 11, x64 |
| Game | Hytale, server build `0.7.0` (bundled JRE 26) |
| Runtime | None — Java is **not** required on your machine |
| Privileges | Must run as the same user as the game |

## Install

1. Download `HytaleTrainer.exe` from [Releases](../../releases).
2. Put it anywhere and run it. That one file is everything.

The agent and the in-game renderer are embedded in the executable and unpacked to a private
`%TEMP%` folder at run time. On first run the trainer writes its settings, caches and
`trainer.log` next to itself.

## Usage

1. Start Hytale and open a single-player world.
2. Run `HytaleTrainer.exe`. It attaches on its own and shows a green dot when connected.

| Key | Action |
|---|---|
| `Insert` | Show / hide the menu |
| `Ctrl` + `End` | Quit the trainer |

Everything the trainer changed is restored when it exits or is killed: invulnerability,
movement settings, no clip, frozen creatures and forced weather. No Clip, Freeze Creatures,
Freeze Time, Auto Attack and the shared-world switch always start off.

## Features

Seven tabs, each grouped by what it acts on.

### Player

God mode · infinite health / stamina / mana / oxygen · no fall damage · full heal ·
clear effects · Adventure ⇄ Creative · infinite durability · infinite ammo · repair all ·
live status bars.

### Movement

Speed · jump height · fly · fly speed · no clip · teleport (forward / up / XYZ / world
spawn) · named saved positions.

### Combat

| Group | What it does |
|---|---|
| **Targets** | Hostile · Neutral (NPCs & animals) · Friendly · Players. Shared by every feature on the tab. |
| **Damage** | One-hit kills · damage multiplier · per-hit damage. Applies to melee, arrows, spells and status effects, because they all count as your hit. |
| **Melee** (on swing) | **Killaura** — hits the nearest targets in range, up to N per swing. **Reach** — hits whatever you aim at, up to 32 blocks. **Ghost Hit** — pulls the nearest target in front of you. |
| **Ranged** | **Projectile Aimbot** — steers your arrows, spells and thrown items into the target nearest their line of flight, within an aim cone. **Magic Bullet** — on launch the projectile is placed just in front of its target, flying in: any distance, through walls. |
| **Auto Attack** | Timed attacks on the nearest target, with a rate slider and a facing filter. |

Swings are read from your own `InteractionManager`, so mining with a tool never triggers a
combat feature. Hits count as yours — drops and kill credit are normal.

### World

Set time (presets or exact hour) · freeze time · force any weather or return to natural
(runtime only; the world config is never edited) · freeze nearby creatures ·
kill hostiles / all creatures in a radius · **Instant Break**.

### Items

Every item asset in the game (~4,000) with English names from the game's own translations,
grouped by type (Weapons, Tools, Armor, Blocks, Gliders, Utility, Keys, Items), filterable by
category and rarity, with rarity colours and max stack. Give 1 / 10 / a stack / any amount.

**Drops:** Instant Pickup · Item Vacuum · Duplicate Drops (×2–×16), plus a one-shot
"duplicate everything nearby".

### Visuals

Box ESP for hostile / neutral / friendly creatures, other players and dropped items, with
names, HP, distance, health bars and snaplines. Draggable player-stats overlay (health,
stamina, mana, oxygen, position, speed, heading, time). There is no FOV setting — field of
view, third person and camera position are all detected from the client.

### Settings

Shared-world gate · in-game vs. overlay rendering · smoothing · performance readout.

<details>
<summary><b>Implementation notes for the gameplay features</b></summary>

- **Instant Break** — the block you look at breaks in one hit through the game's *survival*
  break path (`BlockHarvestUtils.performBlockDamage` with overwhelming damage), so drops, tool
  requirements and tool wear are normal. If the block still stands (fists and the wrong tool do
  no damage in survival) it is broken outright with the block's own drop list, so something
  always drops. Skipped when no tool in the game could ever mine it (the `Unbreakable` gather
  type: the world floor, sealed coffers) or the world forbids breaking
  (`WorldConfig.isBlockBreakingAllowed`, the same check the game's own break interaction makes).
  A swing that hits a creature never digs.
- **Drops** — Instant Pickup clears the 0.5 s pickup delay and the 0.25 s retry throttle. Item
  Vacuum pulls drops to your feet and lets normal pickup take them, so a full inventory is
  respected. Items *you* throw are recognised by the 1.5 s delay the game gives thrown items
  (`ItemUtils`) and are excluded from both, or you could never drop anything. Duplicate Drops
  copies only *new* drops — those whose spawn delay is still counting down — through the game's
  own drop path (`ItemComponent.generateItemDrops` + `addEntities`). Copies are never copied
  again and the merge system absorbs stacks in place, so nothing snowballs.
- **Projectiles** — yours are identified by `getCreatorUuid()` on `ProjectileComponent` /
  `StandardPhysicsProvider`. Both physics paths reload position from `TransformComponent` and
  velocity from `Velocity` every tick, so rewriting those steers the projectile and the game's
  own collision makes the hit, with the weapon's real damage, effects and knockback. Only
  projectiles in flight are touched; arrows already stuck in the ground are left alone.

</details>

## Shared worlds

A world you have opened to friends is still **your** local server, so every feature still
works there. What changes is who else is affected. While more than one player is connected,
features that change the world for everybody — set / freeze time, forced weather, freeze
creatures, kill creatures, and the combat features — are held back: the menu greys them out
*and* the agent refuses them, so a stale value cannot slip through.

**Allow features that change the world for everyone** on the SETTINGS tab lifts the gate. It
is never saved and starts off every session. Anything that only touches you — god mode, stats,
flight, speed, no clip, items, teleport, ESP — is never gated.

## How it works

Hytale single-player runs two processes:

| Process | What it is | Role |
|---|---|---|
| `HytaleClient.exe` | .NET NativeAOT client (72 MB, no metadata) | rendering, input, client prediction |
| `java.exe … HytaleServer.jar --singleplayer --client-pid N` | local Java server | **owns every gameplay value**: health, stats, inventory, damage, NPCs, time, weather, movement |

The trainer targets the **server**, which is authoritative and unobfuscated Java. That is why
features are reliable rather than a guess at client memory.

1. **Discovery** — finds a `java.exe` whose command line has `HytaleServer.jar` *and*
   `--singleplayer`, and whose `--client-pid` is a live `HytaleClient.exe`. Dedicated and
   multiplayer servers never match; the agent also verifies `Options.SINGLEPLAYER` itself.
2. **Attach** (`src/attach.cpp`) — the game's bundled JRE ships **without** `jdk.attach`, so
   the trainer performs the HotSpot Windows attach handshake itself, exactly as the JDK does. A
   90-byte remote thread calls
   `jvm.dll!JVM_EnqueueOperation("load", "instrument", "false", "<agent.jar>=<args>", <pipe>)`
   and the JVM replies on a `\\.\pipe\javatool…` pipe. The thread only *queues* the request;
   the JVM's own attach-listener thread loads the agent. Stub source: `re/stub.asm`.
3. **Agent** (`agent/src/hytrainer`) —
   `Agent.java` is a bootstrap with no game references; it finds the class loader that owns the
   server and loads `hytrainer-core.jar` in a child loader, so each attach replaces the previous
   core. `Core.java` listens on `127.0.0.1` on an ephemeral port behind a random token and
   restores everything if the trainer disconnects or crashes. `Cheats.java` holds every feature
   and runs on the owner's **world thread** via `World.execute`, never more than one tick in
   flight, using the game's own ECS API. `DamageFilter.java` is a `DamageEventSystem` in the
   game's filter group, registered only from the agent's own thread (the registry update takes
   the store data lock that world threads hold while ticking).
4. **Link protocol** — one line in, one line out: `HELLO`, `SET k v`, `STATE`, `ENTS`, `ITEMS`,
   `WEATHERS`, `DO action args`, `BYE`.

## Technical notes

<details>
<summary><b>Camera and ESP</b></summary>

The ESP projects with the client's own world→clip matrix, read out of `HytaleClient.exe`
(`src/camera.cpp`). The client is opened read-only (`PROCESS_VM_READ`); nothing is written to
it and no code runs inside it.

Nothing about the camera is hard-coded. A discovery thread looks for a 4×4 float matrix that
satisfies the algebra a view-projection matrix must satisfy — unit forward row, orthogonal
basis, aspect ratio equal to the client window, field of view between 40° and 130° — then
checks that the camera position it solves for agrees with where the server says you are, over
several samples taken while you move. Both memory layouts (`p * M` and `M * p`) and both w
conventions are tried, and clip-axis directions are resolved against the basis the server
rotation gives. A first pass uses a 16-byte stride; a full 4-byte pass follows only if that
finds nothing. Once locked, the render thread re-checks those 64 bytes every frame; after about two
seconds of a matrix that no longer makes sense (world change, resolution change, renderer
reload) the address is dropped and the scan restarts near the old address first.

Measured on 0.7.0: the lock takes ≈13 s of background scanning, the matrix sits at a stable
address, and the field of view comes out of the matrix exactly (75.0 for a 75 setting).

**Your position, and the camera while you walk.** This client renders camera-relative: the
matrix carries rotation only, so the camera's world position must come from elsewhere. A
camera-position copy found in memory turned out to trail the picture while walking (its three
slots disagreed by 0.7 blocks mid-stride), which is why the ESP once kept up with turning but
not with walking. The camera position now comes from your own character's client object, read
in the game at the moment the frame is presented, exactly like the creatures:
*eye = interpolated position + eye height*. Your object is identified the first time you stand
still (the one object exactly on the server's position of you) and found again after it goes
away (death, respawn, GC). In third person the camera sits on the view axis behind the eye;
only the boom *length* comes from the camera copy, measured against your object in the same
tick so its lag cancels, while the direction comes from the frame's own matrix. If the matrix
turns out to be built around the eye (its solved centre is non-zero), the eye is used directly.

If the camera cannot be read at all (status shown on VISUALS), the ESP rebuilds it from the
server's view of you — eye position plus head pitch/yaw, with the FOV read from
`UserData/Settings.json` at attach. That fallback is a tick or two behind the mouse and assumes
first person.

</details>

<details>
<summary><b>In-game renderer</b></summary>

The ESP is drawn **inside the game's own frame** by `hytrainer-render.dll` (`hook/render.cpp`).
An external overlay is a second renderer with its own present queue: the game's OpenGL driver
holds one to three frames before they reach the screen, the overlay holds its own, and the two
cannot line up while the camera moves, however cleanly the camera is read (measured: every
per-frame read intact, boxes still trailed). Drawing into the game's back buffer right before
it is presented makes the boxes part of the same picture.

- The client renders with OpenGL through SDL3. `SDL3.dll` calls `GDI32!SwapBuffers` through its
  import table, so the hook is a single pointer swap in that table. No game code is patched, and
  unhooking writes the original pointer back.
- On each `SwapBuffers` the ESP is drawn with Dear ImGui's OpenGL3 backend, which saves and
  restores the GL state it touches. The camera matrix is read directly in the client at that
  moment, and tick interpolation is evaluated then too.
- The trainer owns all data and publishes it through shared memory (`src/shm.h`) under a
  sequence lock once per overlay frame. Both renderers share the same drawing code
  (`src/esp_draw.h`) and projection (`src/viewproj.h`).
- **Lifetime** — loaded only while attached to your single-player world. It unhooks and unloads
  when the trainer detaches or exits, or by itself after three seconds without a heartbeat
  (trainer crashed or killed), so it is gone before you could join a server. A named mutex keeps
  it to one copy per client. Drawing runs under SEH: a fault disables in-game drawing for the
  session instead of taking the game down.
- The overlay draws the ESP only as a fallback, while the in-game renderer is not drawing.
  **Draw inside the game** on VISUALS switches between them.

</details>

<details>
<summary><b>Overlay rendering and performance</b></summary>

- The overlay is a flip-model swap chain with premultiplied alpha, composed by DWM through
  DirectComposition (`WS_EX_NOREDIRECTIONBITMAP`). The old blt-model swap chain on a layered
  window, where DWM copied a monitor-sized redirection bitmap every frame, is only a fallback.
- Frame latency is capped at one frame and the loop waits on the swap chain's latency object, so
  camera and entity data are sampled right before the frame that shows them.
- The camera is read once per frame (64 bytes) and shared by the ESP and the menu; the snapshot
  is handed over as a shared pointer, not copied.
- Server data changes once per world tick. The agent requests a tick every 10 ms (33 ms beat
  against the 30 TPS world clock and skipped ticks), each `STATE` carries the tick counter
  (`seq`), and the trainer polls every 4 ms with a 1 ms timer. The ESP blends the last two ticks
  over one measured tick interval (**Smooth movement between server ticks**, VISUALS) so boxes
  glide at the overlay frame rate. Teleports snap.
- `HytaleTrainer.exe --bench N` runs the overlay for N seconds with the menu closed and logs
  fps, average / worst frame time, compositor path, camera state and tick interval once a
  second. Measured on a 60 Hz display: 60.0 fps, 16.67 ms average, 16.7–17.6 ms worst, 33 ms
  ticks with none skipped. VISUALS shows the same numbers live.

</details>

<details>
<summary><b>Reverse-engineering notes</b></summary>

- `re/src` holds the Vineflower decompile of `com.hypixel.*` from `HytaleServer.jar`. It is
  git-ignored; regenerate it with `re/tools/vineflower.jar`.
- Key entry points: `Universe.get().getPlayer(ownerUuid)`, `PlayerRef.getReference()`,
  `ref.getStore().getExternalData().getWorld()`, `EntityStore.REGISTRY`,
  `DamageModule.getFilterDamageGroup()`, `DefaultEntityStatTypes`,
  `MovementManager.getSettings()/getDefaultSettings()` + `update(packetHandler)`.
- Rotation is in radians, forward = `(−sin yaw · cos pitch, sin pitch, −cos yaw · cos pitch)`,
  Y up.
- This version has no server-side movement validation — no speed or fly checks.

</details>

## Building from source

```bat
build.cmd
```

Needs **JDK 25** at `C:\Program Files\Eclipse Adoptium\jdk-25.0.4.7-hotspot` (to compile the
agent against `HytaleServer.jar`) and **VS 2026 with CMake**.

The result is one file: **`dist\HytaleTrainer.exe`**. The agent jars and the renderer DLL are
linked in as RCDATA resources 101–103 — `CMakeLists.txt` generates `embed_<config>.rc` and
`src/embedded.h` unpacks them at run time, since the JVM and the game keep a loaded file open.

```
src/      trainer: overlay, menu, camera, client entities, agent link
agent/    Java agent: bootstrap, core, features, damage filter
hook/     hytrainer-render.dll: in-game ESP
re/       attach stub source and decompile tooling
```

## Diagnostics

- **`trainer.log`**, next to the exe. `[test]` lines record every value pushed, every action,
  the camera scan and lock, and a status line every 5 s.
- **`HytaleTrainer.exe --selftest report.txt [script.txt]`** attaches, writes one snapshot (plus
  replies to any script lines such as `SET god 1`, `WAIT 500`, `DO heal`), runs the camera scan
  and reports what it locked — layout, field of view, aspect, camera position, and where nearby
  entities land in normalized device coordinates — then detaches.
- The server prints the standard JVM notice `A Java agent has been loaded dynamically` once per
  attach. It is harmless.

### Common cases

| Status | Meaning |
|---|---|
| `waiting for a single-player world` | Hytale is not running, or no world is open. |
| `no local server - online or in a menu, features unavailable` | Hytale is running but there is no local server: you are in a menu or in someone else's world. |
| `attached` | Connected. Features are live. |
| `Damage hooks unavailable in this game version` | The damage filter could not be registered; other features still work. |

## Limitations

**Worlds you join are not supported, and cannot be.** The trainer only attaches to a `java.exe`
started with `--singleplayer` by your own client. On someone else's server there is nothing to
attach to, and there is no client-side path that could substitute. Measured read-only in a live
client with no local server (0.7.0):

- Entity objects **are** present and live (784 objects of the cached type, with real world
  coordinates), so *positions* genuinely are client-side.
- Everything the ESP labels is not. The ESP draws the **server's** entity records — `kind`,
  `name`, `hp`, `maxHp` and `height` exist only there. Client objects are used solely to refine
  the position of an entity the server already reported, so with no server there is nothing to
  draw.
- The camera cannot be found either: discovery scores candidate matrices against the server's
  eye and facing. No camera means no projection, so positions alone are not drawable. The chain
  is circular — client entities need a position hint, the hint needs the camera, the camera needs
  the server.

Writes could never work regardless: health, damage, items, teleport and blocks all live on the
host's machine.

**Version-specific.** Offsets and the agent are built against server `0.7.0`. A game update can
require rediscovery; the camera and client-entity layouts are found at run time and cached, but
the agent is compiled against the server jar.

## License

No license has been chosen yet, so default copyright applies: all rights reserved. Add a
`LICENSE` file before publishing if you intend others to reuse the code.

---

<sub>For single-player use. Modifying a game you host affects only your own save; keep a backup
before using write features on a world you care about.</sub>
