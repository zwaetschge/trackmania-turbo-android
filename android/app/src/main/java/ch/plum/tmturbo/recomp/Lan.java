package ch.plum.tmturbo.recomp;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Games over the local network. The game itself plays them (ManiaPlanet's LAN server and
 * client in the engine, see runtime/src/lan.c); this class does what the Turbo menus cannot:
 * it writes the host's MatchSettings file (mode + tracks), tells the players on the same
 * Wi-Fi that a game is open (a small UDP announcement every second while the host plays),
 * and lists the games it hears for the join page.
 */
final class Lan {
    static final String TAG = "TMLan";
    static final int BEACON_PORT = 47350;      // our own announcements, not the game's
    static final int GAME_PORT = 2350;         // the engine's server port (NetworkServerPort)
    static final String MAGIC = "TMTURBO-LAN";

    // host settings (labels in res/values*/strings.xml)
    static final Launcher.Setting[] HOST_SETTINGS = {
            new Launcher.Setting("lan_mode", R.string.lan_mode, "ta|rounds|laps", R.string.lan_mode_vals),
            new Launcher.Setting("lan_maps", R.string.lan_maps, "white|green|blue|red|black|canyon|valley|lagoon|stadium|all", R.string.lan_maps_vals),
            new Launcher.Setting("lan_time", R.string.lan_time, "5|3|10|15", R.string.lan_time_vals),
    };
    static final String[] COLORS = { "01_White", "02_Green", "03_Blue", "04_Red", "05_Black" };
    static final String[] ENVS = { "01_Canyon", "02_Valley", "03_Lagoon", "04_Stadium" };

    static String get(android.content.SharedPreferences p, String key) {
        for (Launcher.Setting s : HOST_SETTINGS) if (s.key.equals(key)) return s.get(p);
        return "";
    }
    static String label(android.content.SharedPreferences p, String key) {
        for (Launcher.Setting s : HOST_SETTINGS) if (s.key.equals(key)) return s.label(p);
        return "";
    }

    /** Campaign track n (1..200) as a path below the Maps folder. */
    static String trackPath(int n) {
        int c = (n - 1) / 40, e = ((n - 1) % 40) / 10;
        return String.format(Locale.ROOT, "Campaigns\\%s\\%s\\%03d.Map.Gbx", COLORS[c], ENVS[e], n);
    }

    static List<Integer> tracks(String set) {
        List<Integer> l = new ArrayList<>();
        for (int n = 1; n <= 200; n++) {
            int c = (n - 1) / 40, e = ((n - 1) % 40) / 10;
            boolean on;
            switch (set) {
                case "white": on = c == 0; break;
                case "green": on = c == 1; break;
                case "blue": on = c == 2; break;
                case "red": on = c == 3; break;
                case "black": on = c == 4; break;
                case "canyon": on = e == 0; break;
                case "valley": on = e == 1; break;
                case "lagoon": on = e == 2; break;
                case "stadium": on = e == 3; break;
                default: on = true;
            }
            if (on) l.add(n);
        }
        return l;
    }

    static String script(String mode) {
        switch (mode) {
            case "rounds": return "TMC_OnlineRounds.Script.txt";
            case "laps": return "TMC_OnlineLaps.Script.txt";
            default: return "TMC_OnlineTimeAttack.Script.txt";
        }
    }

    /**
     * Writes Documents\TrackmaniaTurbo\Maps\MatchSettings\LanHost.txt of the profile in
     * userDir; returns the name relative to the Maps folder (what LoadMatchSettings takes).
     */
    static String writeMatchSettings(File userDir, android.content.SharedPreferences p) throws Exception {
        File dir = new File(userDir, "Users/Player/Documents/TrackmaniaTurbo/Maps/MatchSettings");
        dir.mkdirs();
        int minutes = Integer.parseInt(get(p, "lan_time"));
        StringBuilder x = new StringBuilder();
        x.append("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<playlist>\n\t<gameinfos>\n")
         .append("\t\t<game_mode>0</game_mode>\n")
         .append("\t\t<script_name>").append(script(get(p, "lan_mode"))).append("</script_name>\n")
         .append("\t\t<chat_time>10000</chat_time>\n\t\t<finishtimeout>1</finishtimeout>\n")
         .append("\t\t<allwarmupduration>0</allwarmupduration>\n\t\t<disablerespawn>0</disablerespawn>\n")
         .append("\t\t<forceshowallopponents>0</forceshowallopponents>\n")
         .append("\t\t<timeattack_limit>").append(minutes * 60000).append("</timeattack_limit>\n")
         .append("\t\t<laps_timelimit>").append(minutes * 60000).append("</laps_timelimit>\n")
         .append("\t\t<rounds_pointslimit>50</rounds_pointslimit>\n")
         .append("\t</gameinfos>\n\t<filter>\n\t\t<is_lan>1</is_lan>\n\t\t<is_internet>0</is_internet>\n")
         .append("\t\t<is_solo>0</is_solo>\n\t\t<is_hotseat>0</is_hotseat>\n\t\t<sort_index>1000</sort_index>\n")
         .append("\t\t<random_map_order>0</random_map_order>\n\t</filter>\n\t<startindex>0</startindex>\n");
        for (int n : tracks(get(p, "lan_maps"))) x.append("\t<map><file>").append(trackPath(n)).append("</file></map>\n");
        x.append("</playlist>\n");
        try (FileOutputStream fo = new FileOutputStream(new File(dir, "LanHost.txt"))) {
            fo.write(x.toString().getBytes(StandardCharsets.UTF_8));
        }
        return "MatchSettings\\LanHost.txt";
    }

    /** This device's IPv4 address on the local network (Wi-Fi first), or null. */
    static InterfaceAddress localAddress() {
        InterfaceAddress best = null; String bestName = "";
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress a = ia.getAddress();
                    if (!(a instanceof Inet4Address) || !(a.isSiteLocalAddress() || a.isLinkLocalAddress())) continue;
                    if (best == null || (ni.getName().startsWith("wlan") && !bestName.startsWith("wlan"))) { best = ia; bestName = ni.getName(); }
                }
            }
        } catch (Exception e) { Log.w(TAG, "interfaces", e); }
        return best;
    }
    static String localIp() {
        InterfaceAddress ia = localAddress();
        return ia == null ? null : ia.getAddress().getHostAddress();
    }

    // ------------------------------------------------------------------ announcements
    private static volatile Thread beacon;

    /** While a game is hosted: one broadcast per second so the others' join pages list it. */
    static void startBeacon(String name, String mode, String maps) {
        stopBeacon();
        Thread t = new Thread(() -> {
            byte[] msg = (MAGIC + "|1|" + clean(name) + "|" + GAME_PORT + "|" + clean(mode) + "|" + clean(maps)).getBytes(StandardCharsets.UTF_8);
            try (DatagramSocket s = new DatagramSocket()) {
                s.setBroadcast(true);
                while (beacon == Thread.currentThread()) {
                    InterfaceAddress ia = localAddress();
                    List<InetAddress> to = new ArrayList<>();
                    to.add(InetAddress.getByName("255.255.255.255"));
                    if (ia != null && ia.getBroadcast() != null) to.add(ia.getBroadcast());
                    for (InetAddress d : to) {
                        try { s.send(new DatagramPacket(msg, msg.length, d, BEACON_PORT)); } catch (Exception ignored) { }
                    }
                    Thread.sleep(1000);
                }
            } catch (InterruptedException ignored) {
            } catch (Exception e) { Log.w(TAG, "beacon", e); }
        }, "lan-beacon");
        t.setDaemon(true);
        beacon = t;
        t.start();
    }
    static void stopBeacon() { Thread t = beacon; beacon = null; if (t != null) t.interrupt(); }

    private static String clean(String s) { return s == null ? "" : s.replace('|', '/').trim(); }

    static final class Host {
        final String ip, name, mode, maps; final int port; long seen;
        Host(String ip, String name, int port, String mode, String maps) { this.ip = ip; this.name = name; this.port = port; this.mode = mode; this.maps = maps; }
    }

    interface HostsListener { void hosts(List<Host> hosts); }

    private static volatile Thread finder;

    /** Listens for announcements; reports the games heard in the last 4 s about once a second. */
    static void startFinder(Context ctx, HostsListener l) {
        stopFinder();
        Thread t = new Thread(() -> {
            WifiManager wm = (WifiManager) ctx.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            WifiManager.MulticastLock lock = null;
            try { if (wm != null) { lock = wm.createMulticastLock("tmturbo-lan"); lock.setReferenceCounted(false); lock.acquire(); } } catch (Exception ignored) { }
            Map<String, Host> seen = new LinkedHashMap<>();
            String self = localIp();
            try (DatagramSocket s = new DatagramSocket(null)) {
                s.setReuseAddress(true);
                s.setBroadcast(true);
                s.bind(new java.net.InetSocketAddress(BEACON_PORT));
                s.setSoTimeout(500);
                byte[] buf = new byte[512];
                long lastReport = 0;
                while (finder == Thread.currentThread()) {
                    try {
                        DatagramPacket pk = new DatagramPacket(buf, buf.length);
                        s.receive(pk);
                        String[] f = new String(pk.getData(), 0, pk.getLength(), StandardCharsets.UTF_8).split("\\|", -1);
                        String ip = pk.getAddress().getHostAddress();
                        if (f.length >= 6 && f[0].equals(MAGIC) && !ip.equals(self)) {
                            Host h = new Host(ip, f[2], parse(f[3], GAME_PORT), f[4], f[5]);
                            h.seen = System.currentTimeMillis();
                            seen.put(ip, h);
                        }
                    } catch (SocketTimeoutException ignored) { }
                    long now = System.currentTimeMillis();
                    if (now - lastReport >= 900) {
                        lastReport = now;
                        List<Host> out = new ArrayList<>();
                        for (Host h : seen.values()) if (now - h.seen < 4000) out.add(h);
                        l.hosts(out);
                    }
                }
            } catch (Exception e) { Log.w(TAG, "finder", e); }
            finally { if (lock != null) try { lock.release(); } catch (Exception ignored) { } }
        }, "lan-finder");
        t.setDaemon(true);
        finder = t;
        t.start();
    }
    static void stopFinder() { Thread t = finder; finder = null; if (t != null) t.interrupt(); }

    private static int parse(String s, int def) { try { return Integer.parseInt(s.trim()); } catch (Exception e) { return def; } }
}
