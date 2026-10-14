// =============================================================================
//  Core.java — agent lifecycle and the trainer link.
//
//  Listens on 127.0.0.1 (ephemeral port, written to the status file) for one
//  trainer connection that must open with the attach token. Every game change
//  is handed to Cheats, which runs it on the owner's world thread. When the
//  trainer disconnects (exit or crash) everything is restored and the agent
//  shuts itself down, so nothing outlives the trainer.
//
//  Only runs in a server started with --singleplayer.
//
//  Protocol: one request line, one reply line (UTF-8, tab-separated fields).
//      HELLO <token>          -> OK <version>
//      SET <key> <value>      -> OK
//      STATE                  -> STATE <k=v>\t<k=v>...
//      ENTS                   -> ENTS <entity>\t<entity>...   (see Cheats.entities)
//      ITEMS                  -> ITEMS <id>\t<id>...
//      WEATHERS               -> WEATHERS <id>\t<id>...
//      DO <action> [args...]  -> OK <msg> | ERR <msg>
//      BYE                    -> OK (restore + shut down)
// =============================================================================
package hytrainer.core;

import com.hypixel.hytale.server.core.Options;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public final class Core {
    public static final String VERSION = "1";
    private static final String STOP_KEY = "hytrainer.stop";

    private final String token;
    private final ServerSocket server;
    private final ScheduledExecutorService sched;
    private final Cheats cheats = new Cheats();
    private final Runnable stopper = this::stop;
    private volatile boolean stopped;

    public static synchronized void start(String statusFile, String token) throws Exception {
        // A previous core (older attach) is torn down first.
        Object old = System.getProperties().get(STOP_KEY);
        if (old instanceof Runnable r) { try { r.run(); } catch (Throwable ignored) {} }

        Path status = Path.of(statusFile);
        if (!Options.getOptionSet().has(Options.SINGLEPLAYER)) {
            Files.writeString(status, "ERR not a single-player world", StandardCharsets.UTF_8);
            return;
        }
        Core c = new Core(token);
        System.getProperties().put(STOP_KEY, c.stopper);
        Files.writeString(status, "PORT " + c.server.getLocalPort(), StandardCharsets.UTF_8);
    }

    private Core(String token) throws Exception {
        this.token = token;
        server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress());
        server.setSoTimeout(15000);          // trainer must connect promptly
        sched = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "hytrainer-tick");
            t.setDaemon(true);
            return t;
        });
        // Damage filter registers from this (non-world) thread; see DamageFilter.
        try { DamageFilter.register(cheats); } catch (Throwable t) { cheats.filterError = t.toString(); }
        // Faster than the 30 TPS world tick on purpose: at most one tick() is
        // in flight, so this just makes sure every world tick gets one. At 33 ms
        // the two clocks beat against each other and a tick is skipped now and
        // then, which showed up as a hitch in the ESP.
        sched.scheduleAtFixedRate(cheats::requestTick, 100, 10, TimeUnit.MILLISECONDS);
        Thread net = new Thread(this::serve, "hytrainer-net");
        net.setDaemon(true);
        net.start();
    }

    private void serve() {
        try (ServerSocket ss = server; Socket s = ss.accept()) {
            s.setTcpNoDelay(true);
            s.setSoTimeout(10000);            // trainer polls every frame; silence means it is gone
            BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
            BufferedWriter out = new BufferedWriter(new OutputStreamWriter(s.getOutputStream(), StandardCharsets.UTF_8));
            String hello = in.readLine();
            if (hello == null || !hello.equals("HELLO " + token)) return;
            reply(out, "OK " + VERSION);
            String line;
            while (!stopped && (line = in.readLine()) != null) {
                String r;
                try { r = handle(line); } catch (Throwable t) { r = "ERR " + t; }
                reply(out, r);
                if (line.equals("BYE")) break;
            }
        } catch (Throwable ignored) {
        } finally {
            stop();
        }
    }

    private static void reply(BufferedWriter out, String r) throws Exception {
        out.write(r.replace('\n', ' ').replace('\r', ' '));
        out.write('\n');
        out.flush();
    }

    private String handle(String line) {
        String[] p = line.split(" ", 3);
        switch (p[0]) {
            case "SET":   return p.length == 3 && cheats.set(p[1], p[2]) ? "OK" : "ERR bad key";
            case "STATE": return "STATE " + cheats.state();
            case "ENTS":  return "ENTS " + cheats.entities();
            case "ITEMS": return "ITEMS " + cheats.items();
            case "WEATHERS": return "WEATHERS " + cheats.weathers();
            case "DO":    return cheats.action(p.length > 1 ? p[1] : "", p.length > 2 ? p[2] : "");
            case "BYE":   return "OK";
            default:      return "ERR unknown";
        }
    }

    private synchronized void stop() {
        if (stopped) return;
        stopped = true;
        sched.shutdownNow();
        try { server.close(); } catch (Exception ignored) {}
        try { DamageFilter.unregister(); } catch (Throwable ignored) {}
        cheats.restore();
        System.getProperties().remove(STOP_KEY, stopper);
    }
}
