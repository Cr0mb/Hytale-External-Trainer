// =============================================================================
//  Agent.java — bootstrap loaded into the single-player server JVM.
//
//  Knows nothing about Hytale. It finds the class loader that owns the server
//  (the app loader, or the TransformingClassLoader when early plugins with
//  transformers are installed), then loads hytrainer-core.jar in a child loader
//  of it so a fresh core can replace an old one on every attach.
//
//  main() is the attach launcher the trainer runs with the game's own JRE:
//      java -cp hytrainer-agent.jar hytrainer.Agent <pid> <agent.jar> <args>
// =============================================================================
package hytrainer;

import com.sun.tools.attach.VirtualMachine;
import java.lang.instrument.Instrumentation;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public final class Agent {
    private static final String ANCHOR = "com.hypixel.hytale.server.core.universe.Universe";

    // args: <core jar>|<status file>|<token>
    public static void agentmain(String args, Instrumentation inst) {
        String[] a = args.split("\\|", -1);
        Path status = Path.of(a[1]);
        try {
            ClassLoader server = null;
            for (Class<?> c : inst.getAllLoadedClasses()) {
                if (c.getName().equals(ANCHOR)) { server = c.getClassLoader(); break; }
            }
            if (server == null) { fail(status, "server not ready"); return; }
            URLClassLoader cl = new URLClassLoader(new URL[]{ Path.of(a[0]).toUri().toURL() }, server);
            Class<?> core = Class.forName("hytrainer.core.Core", true, cl);
            core.getMethod("start", String.class, String.class).invoke(null, a[1], a[2]);
        } catch (Throwable t) {
            Throwable c = t.getCause() != null ? t.getCause() : t;
            fail(status, c.getClass().getSimpleName() + ": " + c.getMessage());
        }
    }

    private static void fail(Path status, String msg) {
        try { Files.writeString(status, "ERR " + msg.replace('\n', ' '), StandardCharsets.UTF_8); } catch (Exception ignored) {}
    }

    public static void main(String[] a) throws Exception {
        VirtualMachine vm = VirtualMachine.attach(a[0]);
        try { vm.loadAgent(a[1], a[2]); } finally { vm.detach(); }
    }
}
