// =============================================================================
//  attach.h — HotSpot dynamic attach without a JDK.
//
//  The game's bundled JRE has no jdk.attach module, so the trainer performs the
//  Windows attach handshake itself, exactly as the JDK's own attach provider
//  does: a short remote thread calls jvm.dll!JVM_EnqueueOperation("load",
//  "instrument", "false", "<jar>=<options>", <pipe>), and the JVM's attach
//  listener answers on a named pipe the trainer created. The thread only queues
//  the request; the JVM runs the agent on its own attach-listener thread.
// =============================================================================
#pragma once
#include <Windows.h>
#include <string>

namespace attach {

// Loads a java.lang.instrument agent jar into process `pid`. On failure returns
// false and puts a readable reason in `err`.
bool LoadAgent(DWORD pid, const std::string& agentJar, const std::string& options, std::string& err);

} // namespace attach
