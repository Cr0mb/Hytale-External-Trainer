// =============================================================================
//  ingame.h — the in-game ESP renderer (hook/render.cpp), seen from the trainer.
//
//  Start() creates the shared block and loads hytrainer-render.dll into the
//  client; Stop() asks it to unhook and unload and waits for it. The worker
//  calls these as the session comes and goes, so the DLL is only ever loaded
//  while the trainer is attached to your single-player world. The render loop
//  publishes ESP data every frame between BeginWrite() and EndWrite().
// =============================================================================
#pragma once
#include "shm.h"
#include <string>

namespace ingame {

bool Start(DWORD clientPid);      // false with Status() set on failure
void Stop();
bool Running();                   // block exists and the DLL was asked to load
bool Active();                    // the DLL drew a frame within the last 250 ms
std::string Status();
std::string TakeStats();          // "frames=N fps=F state=S err=..." since the last call

shm::Block* BeginWrite();         // null when not running; holds a lock until EndWrite
void EndWrite();

} // namespace ingame
