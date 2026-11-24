package ch.plum.tmturbo.recomp;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Host activity for the statically recompiled TrackmaniaTurbo.exe.
 *
 * The game renders into a full-screen SurfaceView (Direct3D 11 -> DXVK -> Vulkan);
 * a status/log overlay covers it until the first frame is presented and comes
 * back when the game stops. The device gamepad is reported to the game as
 * XInput controller 0, a hardware keyboard as Windows key input.
 *
 * Game files live in the app's external files dir (TrackmaniaTurbo/); they are never
 * bundled. The start screen (Launcher) imports them from the player's own installation,
 * picked as a folder or a ZIP (GameImporter). A development import can pull them from a
 * LAN URL given as the "import_url" intent extra (manifest.txt lists relative paths).
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    static final String TAG = "TMRecomp";
    static { System.loadLibrary("tmturbo"); }

    static native void nativeStart(String exePath, String gameDir, String userDir, String tempDir);
    static native String nativeStatus();
    static native String nativeLogTail();
    static native void nativeSetSurface(Surface surface);
    static native int nativePresentCount();
    static native void nativePad(int index, boolean connected, int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    static native void nativeKey(int vk, boolean down);
    static native void nativeMouse(float nx, float ny, int buttons, int wheel);
    static native void nativeSetMaxFps(int fps);
    static native void nativeSetEnv(String key, String value);
    static native void nativePause(boolean paused);

    private TextView status, log;
    private SurfaceView gameView;
    Surface surfaceHolderSurface() { return gameView != null ? gameView.getHolder().getSurface() : null; }
    private View overlay;
    private Launcher launcher;
    private SharedPreferences prefs;
    private volatile GameImporter importer;
    static final int REQ_TREE = 11, REQ_ZIP = 12;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private static boolean started;      // the game runs once per process, whatever happens to the activity
    private boolean surfaceReady, startPending, overlayPinned, resumed;

    // Touch = mouse cursor (see onCreate): delayed press, minimum hold
    private float touchX, touchY;
    private boolean touchDown, touchUpPending;
    private long touchDownAt;
    private final Runnable touchRelease = () -> { touchDown = false; touchUpPending = false; nativeMouse(touchX, touchY, 0, 0); };
    private final Runnable touchPress = () -> {
        touchDown = true; touchDownAt = SystemClock.uptimeMillis();
        nativeMouse(touchX, touchY, 1, 0);
        if (touchUpPending) ui.postDelayed(touchRelease, 70);
    };

    // XInput state mirrored from Android input events: up to 4 controllers, one slot (XInput
    // user index) per physical device; the on-screen touch pad is a device of its own.
    static final int PADS = 4, NO_DEVICE = Integer.MIN_VALUE, TOUCH_DEVICE = -1000;
    private final int[] slotDevice = { NO_DEVICE, NO_DEVICE, NO_DEVICE, NO_DEVICE };
    private final int[][] pad = new int[PADS][8];      // buttons, hat, lt, rt, lx, ly, rx, ry
    private TouchPad touchPad;
    private static MainActivity current;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        S.apply(this, getSharedPreferences("launcher", MODE_PRIVATE).getString("lang", "auto"));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout frame = new FrameLayout(this);
        SurfaceView sv = new SurfaceView(this);
        gameView = sv;
        sv.getHolder().addCallback(this);
        // Touch screen and mouse = the Windows cursor. A touch moves the cursor there and
        // holds the left button; with a mouse the cursor follows it. Moving it to the top
        // edge opens the ManiaPlanet bar (graphics, sound, input settings), as on the PC.
        sv.setOnTouchListener((v, e) -> {
            if (!started || v.getWidth() <= 0 || v.getHeight() <= 0) return false;
            float nx = e.getX() / v.getWidth(), ny = e.getY() / v.getHeight();
            boolean mouse = (e.getSource() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE;
            int act = e.getActionMasked();
            int buttons;
            if (mouse) {
                int bs = e.getButtonState();
                buttons = ((bs & MotionEvent.BUTTON_PRIMARY) != 0 ? 1 : 0) | ((bs & MotionEvent.BUTTON_SECONDARY) != 0 ? 2 : 0)
                        | ((bs & MotionEvent.BUTTON_TERTIARY) != 0 ? 4 : 0);
            } else {
                // A touch: the cursor moves there at once, the button goes down a few frames
                // later (the menus decide what is under the cursor once per frame and test a
                // press against that) and stays down at least as long.
                touchX = nx; touchY = ny;
                if (act == MotionEvent.ACTION_DOWN) {
                    ui.removeCallbacks(touchPress); ui.removeCallbacks(touchRelease);
                    touchDown = false; touchUpPending = false;
                    nativeMouse(nx, ny, 0, 0);
                    ui.postDelayed(touchPress, 70);
                } else if (act == MotionEvent.ACTION_MOVE) {
                    nativeMouse(nx, ny, touchDown ? 1 : 0, 0);
                } else if (act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_CANCEL) {
                    if (touchDown && SystemClock.uptimeMillis() - touchDownAt >= 70) nativeMouse(nx, ny, 0, 0);
                    else { touchUpPending = true; if (touchDown) ui.postDelayed(touchRelease, 70); }
                }
                return true;
            }
            nativeMouse(nx, ny, buttons, 0);
            return true;
        });
        sv.setOnGenericMotionListener((v, e) -> {
            if (!started || (e.getSource() & InputDevice.SOURCE_MOUSE) != InputDevice.SOURCE_MOUSE || v.getWidth() <= 0) return false;
            float nx = e.getX() / v.getWidth(), ny = e.getY() / v.getHeight();
            int act = e.getActionMasked();
            if (act == MotionEvent.ACTION_HOVER_MOVE || act == MotionEvent.ACTION_HOVER_ENTER) { nativeMouse(nx, ny, 0, 0); return true; }
            if (act == MotionEvent.ACTION_SCROLL) {
                int w = Math.round(e.getAxisValue(MotionEvent.AXIS_VSCROLL));
                if (w != 0) nativeMouse(nx, ny, 0, w);
                return true;
            }
            return false;
        });
        frame.addView(sv, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        // On-screen gamepad (setting "Touch-Steuerung"; automatic = when no controller is there)
        touchPad = new TouchPad(this, (buttons, lt, rt, lx, ly) -> {
            int slot = slotFor(TOUCH_DEVICE, true);
            if (slot < 0) return;
            int[] st = pad[slot];
            st[0] = buttons; st[1] = 0; st[2] = lt; st[3] = rt; st[4] = lx; st[5] = ly; st[6] = 0; st[7] = 0;
            sendPad(slot);
        });
        touchPad.setVisibility(View.GONE);
        frame.addView(touchPad, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        current = this;
        ((android.hardware.input.InputManager) getSystemService(INPUT_SERVICE)).registerInputDeviceListener(
                new android.hardware.input.InputManager.InputDeviceListener() {
                    @Override public void onInputDeviceAdded(int id) { if (started && isGamepad(InputDevice.getDevice(id))) { int s2 = slotFor(id, true); if (s2 >= 0) sendPad(s2); } updateTouchPad(); }
                    @Override public void onInputDeviceRemoved(int id) { releaseSlot(id); updateTouchPad(); }
                    @Override public void onInputDeviceChanged(int id) { }
                }, ui);

        // While the game starts (and if it stops): a black screen with the status line, and
        // the log when asked for (setting "Protokoll beim Start" or "--ez show_log true").
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(4, 6, 10));
        root.setPadding(48, 36, 48, 24);
        TextView title = new TextView(this);
        title.setText("TRACKMANIA TURBO");
        title.setTypeface(Typeface.create(Typeface.DEFAULT_BOLD, Typeface.BOLD_ITALIC));
        title.setTextColor(Launcher.ACCENT);
        title.setTextSize(22);
        root.addView(title);
        status = new TextView(this);
        status.setTextColor(Color.rgb(200, 206, 216));
        status.setTextSize(15);
        status.setPadding(0, 12, 0, 12);
        root.addView(status);
        ScrollView scroll = new ScrollView(this);
        log = new TextView(this);
        log.setTextColor(Color.rgb(170, 220, 170));
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextSize(11);
        scroll.addView(log);
        root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        overlay = root;
        overlay.setVisibility(started ? View.VISIBLE : View.GONE);
        frame.addView(root, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null) {
            pm.addThermalStatusListener(getMainExecutor(), this::onThermal);
            onThermal(pm.getCurrentThermalStatus());
        }
        launcher = new Launcher(this, prefs, new Launcher.Actions() {
            @Override public void play() { withName(() -> startGame()); }
            @Override public void importFolder() { pickFolder(); }
            @Override public void importZip() { pickZip(); }
            @Override public void cancelImport() { GameImporter im = importer; if (im != null) im.cancel(); }
            @Override public void quit() { finishAndRemoveTask(); android.os.Process.killProcess(android.os.Process.myPid()); }
            @Override public void lanHost() { withName(() -> startLan("host", null, 0)); }
            @Override public void lanJoin(String ip, int port) { withName(() -> startLan("join", ip, port)); }
            @Override public void lanFind(boolean on) {
                if (on) Lan.startFinder(MainActivity.this, hosts -> ui.post(() -> launcher.lanHosts(hosts)));
                else Lan.stopFinder();
            }
            @Override public void lanEditName() { editPlayerName(() -> { launcher.refreshPage(); refreshLauncher(); }); }
            @Override public String lanAddress() { return Lan.localIp(); }
            @Override public void languageChanged() { launcher.relabel(); refreshLauncher(); touchPad.relabel(); }
        });
        frame.addView(launcher.root, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        launcher.root.setVisibility(started ? View.GONE : View.VISIBLE);
        setContentView(frame);
        immersive();
        refreshLauncher();

        // Development: "--ez unlock true" shows the game over an insecure lock screen and
        // dismisses it, so scripted tests can start on a sleeping device.
        if (getIntent().getBooleanExtra("unlock", false)) {
            setShowWhenLocked(true);
            setTurnScreenOn(true);
            android.app.KeyguardManager km = (android.app.KeyguardManager) getSystemService(KEYGUARD_SERVICE);
            if (km != null) km.requestDismissKeyguard(this, null);
        }
        String envs = getIntent().getStringExtra("env");          // development switches: "K=V;K2=V2"
        if (envs != null) for (String kv : envs.split(";")) {
            int eq = kv.indexOf('=');
            if (eq > 0) nativeSetEnv(kv.substring(0, eq).trim(), kv.substring(eq + 1).trim());
        }
        String dxvkConf = getIntent().getStringExtra("dxvk_conf");   // development: DXVK options, "a = b;c = d"
        if (dxvkConf != null) {
            try {
                File user = userDir();
                user.mkdirs();
                File conf = new File(user, "dxvk.conf");
                try (FileOutputStream fo = new FileOutputStream(conf)) { fo.write(dxvkConf.replace(';', '\n').getBytes("UTF-8")); }
                nativeSetEnv("DXVK_CONFIG_FILE", conf.getAbsolutePath());
            } catch (Exception e) { Log.e(TAG, "dxvk_conf", e); }
        }
        String url = getIntent().getStringExtra("import_url");
        boolean autostart = getIntent().getBooleanExtra("autostart", false);  // "--ez autostart true" skips the start screen
        overlayPinned = getIntent().getBooleanExtra("show_log", false) || "1".equals(Launcher.get(prefs, "log"));
        String zipPath = getIntent().getStringExtra("import_zip");     // development: the ZIP import of the start screen, from a file
        if (zipPath != null) {
            File zf = new File(zipPath);
            launcher.importStarted();
            GameImporter im = new GameImporter(getContentResolver(), gameDir(),
                    (file, done, total, bps) -> ui.post(() -> launcher.importProgress(file, done, total, bps)));
            importer = im;
            new Thread(() -> {
                long t0 = SystemClock.elapsedRealtime();
                GameImporter.Result r = im.importZip(Uri.fromFile(zf), zf.length());
                importer = null;
                Log.i(TAG, "import_zip " + zipPath + ": " + (r.ok ? "ok" : "failed") + " in " + (SystemClock.elapsedRealtime() - t0) / 1000 + " s: " + r.message);
                ui.post(() -> { launcher.importFinished(r.ok, r.message); if (r.ok) refreshLauncher(); });
            }, "import").start();
        } else if (url != null) {
            status.setText("Importing game files from " + url + " ...");
            new Thread(() -> {
                String r = importFrom(url);
                ui.post(() -> { status.setText(r); refreshLauncher(); if (autostart) startGame(); });
            }).start();
        } else if (getIntent().getStringExtra("lan") != null && gameReady()) {   // development: "--es lan host" / "join:<ip>"
            String l = getIntent().getStringExtra("lan");
            if (l.startsWith("join:")) startLan("join", l.substring(5), Lan.GAME_PORT); else startLan("host", null, 0);
        } else if (autostart && gameReady()) startGame();
        poll();
    }

    void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersive(); }

    // ---- surface -------------------------------------------------------------
    @Override public void surfaceCreated(SurfaceHolder h) { }
    /** The frame rate the game is capped to (setting "Bildrate", or TM_MAX_FPS in the "env" extra). */
    int targetFps() {
        String envs = getIntent().getStringExtra("env");
        if (envs != null) for (String kv : envs.split(";")) if (kv.trim().startsWith("TM_MAX_FPS=")) {
            try { return Integer.parseInt(kv.trim().substring(11).trim()); } catch (Exception ignored) { }
        }
        try { return Integer.parseInt(Launcher.get(prefs, "fps")); } catch (Exception e) { return 60; }
    }

    /** Ask for the display mode that fits the frame rate: the panel runs at 60 Hz unless an
     *  app wants more (and the system's "peak refresh rate" lets it). */
    // When the device gets hot Android caps the display at 60 Hz (on the Odin3 from thermal
    // status MODERATE, a "skin" sensor at ~49 C). A game that keeps asking for 120 then gets
    // every third refresh = 40 fps; asking for 60 while hot gives a clean 60.
    private int thermalCapFps;
    void onThermal(int status) {
        int cap = status >= android.os.PowerManager.THERMAL_STATUS_MODERATE && targetFps() > 60 ? 60 : 0;
        if (cap == thermalCapFps) return;
        thermalCapFps = cap;
        Log.i(TAG, "thermal status " + status + ": frame cap " + (cap > 0 ? cap + " fps" : "off"));
        if (started) { nativeSetMaxFps(cap); requestRefreshRate(surfaceHolderSurface()); }
    }

    void requestRefreshRate(Surface surface) {
        int fps = targetFps();
        if (fps <= 0) fps = 120;
        if (thermalCapFps > 0) fps = Math.min(fps, thermalCapFps);
        try {
            android.view.Display d = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode cur = d.getMode(), best = null;
            for (android.view.Display.Mode m : d.getSupportedModes()) {
                if (m.getPhysicalWidth() != cur.getPhysicalWidth() || m.getPhysicalHeight() != cur.getPhysicalHeight()) continue;
                float r = m.getRefreshRate();
                if (r + 1f < fps) continue;                       // too slow for the target
                if (best == null || r < best.getRefreshRate()) best = m;   // the slowest that is fast enough
            }
            if (best != null) {
                WindowManager.LayoutParams lp = getWindow().getAttributes();
                if (lp.preferredDisplayModeId != best.getModeId()) {
                    lp.preferredDisplayModeId = best.getModeId();
                    getWindow().setAttributes(lp);
                }
                Log.i(TAG, "display mode " + best.getModeId() + " (" + best.getRefreshRate() + " Hz) for " + fps + " fps");
            }
            if (surface != null && android.os.Build.VERSION.SDK_INT >= 30)
                surface.setFrameRate((float) fps, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        } catch (Exception e) { Log.w(TAG, "refresh rate", e); }
    }

    @Override public void surfaceChanged(SurfaceHolder h, int fmt, int w, int ht) {
        if (started) requestRefreshRate(h.getSurface());
        Log.i(TAG, "surfaceChanged " + w + "x" + ht);
        nativeSetSurface(h.getSurface());
        surfaceReady = true;
        if (startPending) { startPending = false; startGame(); }
        if (started && resumed) nativePause(false);
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) {
        surfaceReady = false;
        // The game waits at its next frame (see onPause) until a new surface arrives;
        // the swap chain is rebuilt on it. "--ez exit_on_background true" restores the
        // old behaviour of ending the process here.
        if (started && getIntent().getBooleanExtra("exit_on_background", false)) {
            Log.i(TAG, "surface destroyed while running: exiting");
            android.os.Process.killProcess(android.os.Process.myPid());
        }
        if (started) nativePause(true);
        nativeSetSurface(null);
    }
    @Override protected void onPause() {
        resumed = false;
        if (started) nativePause(true);      // returns when no frame is being presented any more
        super.onPause();
    }
    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        immersive();
        if (started && surfaceReady) nativePause(false);
    }

    // ---- game files ------------------------------------------------------------
    File gameDir() {
        String name = getIntent().getStringExtra("game_dir");      // development: another game data folder
        return new File(getExternalFilesDir(null), name != null && name.matches("[A-Za-z0-9_-]+") ? name : "TrackmaniaTurbo");
    }
    /** Game profile/config/log directory; the "user" extra selects another one (e.g. to start from a clean profile). */
    File userDir() {
        String name = getIntent().getStringExtra("user");
        return new File(getExternalFilesDir(null), name != null && name.matches("[A-Za-z0-9_-]+") ? name : "user");
    }
    boolean gameReady() { return GameImporter.installed(gameDir()); }

    void refreshLauncher() {
        File g = gameDir();
        if (!gameReady()) {
            launcher.setReady(false, S.t(R.string.need_game));
            return;
        }
        launcher.setReady(true, S.t(R.string.checking));
        new Thread(() -> {
            String d;
            try {
                boolean marker = new File(g, GameImporter.MARKER).isFile();
                boolean ok = marker || GameImporter.EXE_SHA256.equals(GameImporter.sha256(new File(g, "TrackmaniaTurbo.exe")));
                d = ok ? S.f(R.string.ready_detail, dirSize(g) / 1e9, Launcher.playerName(prefs), communityTracks())
                       : S.t(R.string.exe_wrong);
                final boolean fok = ok; final String fd = d;
                ui.post(() -> launcher.setReady(fok, fd));
            } catch (Exception e) {
                ui.post(() -> launcher.setReady(true, S.t(R.string.data_present)));
            }
        }).start();
    }

    static long dirSize(File f) {
        if (f.isFile()) return f.length();
        long n = 0; File[] l = f.listFiles();
        if (l != null) for (File c : l) n += dirSize(c);
        return n;
    }

    String communityTracks() {
        String[] n = new File(userDir(), "ImportTracks").list();
        String v = Launcher.get(prefs, "tracks");
        int have = n == null ? 0 : n.length;
        if (have == 0) return S.t(R.string.tracks_none);
        if (v.equals("0")) return S.f(R.string.tracks_off, have);
        return S.f(R.string.tracks_on, v.equals("all") ? have : Math.min(have, Integer.parseInt(v)));
    }

    // ---- import (Storage Access Framework) ----------------------------------------
    void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try { startActivityForResult(i, REQ_TREE); } catch (Exception e) { launcher.importFinished(false, S.f(R.string.no_picker, e.getMessage())); }
    }
    void pickZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/zip", "application/x-zip-compressed", "application/octet-stream" });
        try { startActivityForResult(i, REQ_ZIP); } catch (Exception e) { launcher.importFinished(false, S.f(R.string.no_picker, e.getMessage())); }
    }
    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) { launcher.showImport(); return; }
        Uri uri = data.getData();
        long zipSize = -1;
        if (req == REQ_ZIP) {
            try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.SIZE }, null, null, null)) {
                if (c != null && c.moveToFirst() && !c.isNull(0)) zipSize = c.getLong(0);
            } catch (Exception ignored) { }
        }
        final long zs = zipSize;
        launcher.importStarted();
        GameImporter im = new GameImporter(getContentResolver(), gameDir(),
                (file, done, total, bps) -> ui.post(() -> launcher.importProgress(file, done, total, bps)));
        importer = im;
        new Thread(() -> {
            GameImporter.Result r = req == REQ_TREE ? im.importTree(uri) : im.importZip(uri, zs);
            importer = null;
            ui.post(() -> { launcher.importFinished(r.ok, r.message); if (r.ok) refreshLauncher(); });
        }, "import").start();
    }

    String importFrom(String base) {
        try {
            if (!base.endsWith("/")) base += "/";
            File dst = gameDir();
            dst.mkdirs();
            BufferedReader rd = new BufferedReader(new InputStreamReader(new URL(base + "manifest.txt").openStream()));
            String line; int n = 0; long bytes = 0;
            while ((line = rd.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                String[] parts = line.split("\t");
                String rel = parts[0];
                long size = parts.length > 1 ? Long.parseLong(parts[1]) : -1;
                File out = new File(dst, rel);
                if (out.isFile() && out.length() == size) continue;
                out.getParentFile().mkdirs();
                HttpURLConnection c = (HttpURLConnection) new URL(base + rel.replace(" ", "%20")).openConnection();
                try (InputStream in = c.getInputStream(); FileOutputStream fo = new FileOutputStream(out)) {
                    byte[] buf = new byte[1 << 20]; int k;
                    while ((k = in.read(buf)) > 0) { fo.write(buf, 0, k); bytes += k; }
                }
                n++;
                final String msg = "Imported " + rel + " (" + (bytes >> 20) + " MB so far)";
                ui.post(() -> status.setText(msg));
            }
            return "Import done: " + n + " files, " + (bytes >> 20) + " MB";
        } catch (Exception e) {
            Log.e(TAG, "import failed", e);
            return "Import failed: " + e;
        }
    }

    private String lanMode, lanIp;
    private int lanPort;

    /** A game on the local network: "host" opens one, "join" joins ip:port (see runtime/src/lan.c). */
    void startLan(String mode, String ip, int port) {
        if (started || !gameReady() || importer != null) return;
        Lan.stopFinder();
        lanMode = mode; lanIp = ip; lanPort = port;
        startGame();
    }

    /** The player name is asked for once, before the first game (pre-filled with the device name). */
    void withName(Runnable then) {
        if (prefs.contains("lan_name")) then.run(); else editPlayerName(then);
    }
    /** Player name: shown in races, on the scoreboard and to the other players of a LAN game. */
    void editPlayerName(Runnable then) {
        android.widget.EditText e = new android.widget.EditText(this);
        e.setText(Launcher.playerName(prefs));
        e.setSingleLine(true);
        e.setSelectAllOnFocus(true);
        // no full-screen keyboard in landscape (it hides the dialog); "Done" confirms
        e.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE | android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | android.view.inputmethod.EditorInfo.IME_FLAG_NO_FULLSCREEN);
        boolean first = !prefs.contains("lan_name");
        final android.app.AlertDialog[] dlg = new android.app.AlertDialog[1];
        e.setOnEditorActionListener((v, action, ev) -> {
            if (action == android.view.inputmethod.EditorInfo.IME_ACTION_DONE && dlg[0] != null) {
                dlg[0].getButton(android.app.AlertDialog.BUTTON_POSITIVE).performClick();
                return true;
            }
            return false;
        });
        dlg[0] = new android.app.AlertDialog.Builder(this)
                .setTitle(S.t(R.string.name_title))
                .setMessage(S.t(R.string.name_msg))
                .setView(e)
                .setCancelable(!first)
                .setPositiveButton(S.t(R.string.btn_ok), (d, w) -> {
                    String n = e.getText().toString().replaceAll("[^A-Za-z0-9 _.-]", "").trim();
                    if (n.length() > 20) n = n.substring(0, 20).trim();
                    if (n.isEmpty()) n = Launcher.playerName(prefs);
                    prefs.edit().putString("lan_name", n).apply();
                    if (then != null) then.run();
                })
                .setNegativeButton(S.t(first ? R.string.btn_keep : R.string.btn_cancel), (d, w) -> {
                    if (first) { prefs.edit().putString("lan_name", Launcher.playerName(prefs)).apply(); if (then != null) then.run(); }
                })
                .show();
        dlg[0].getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
    }

    void startGame() {
        if (started || !gameReady() || importer != null) return;
        if (!surfaceReady) { startPending = true; return; }
        started = true;
        launcher.root.setVisibility(View.GONE);
        overlay.setVisibility(View.VISIBLE);
        overlayPinned = overlayPinned || "1".equals(Launcher.get(prefs, "log"));
        // settings of the start screen; an "env" extra given at launch takes precedence
        String envs = getIntent().getStringExtra("env");
        envs = envs == null ? "" : ";" + envs + ";";
        if (!envs.contains(";TM_MAX_FPS=")) nativeSetEnv("TM_MAX_FPS", Launcher.get(prefs, "fps"));
        requestRefreshRate(surfaceHolderSurface());
        if (thermalCapFps > 0) nativeSetMaxFps(thermalCapFps);
        if (!envs.contains(";TM_HUD_CALM=")) nativeSetEnv("TM_HUD_CALM", Launcher.get(prefs, "hudcalm"));
        String tr = Launcher.get(prefs, "tracks");
        if (tr.equals("0")) { if (!envs.contains(";TM_TRACKS_IMPORT=")) nativeSetEnv("TM_TRACKS_IMPORT", "0"); }
        else if (!tr.equals("all") && !envs.contains(";TM_TRACKS_MAX=")) nativeSetEnv("TM_TRACKS_MAX", tr);
        File user = userDir();
        if (!envs.contains(";TM_PLAYER_NAME=")) nativeSetEnv("TM_PLAYER_NAME", Launcher.playerName(prefs));
        if (lanMode != null) {
            nativeSetEnv("TM_LAN", lanMode);
            nativeSetEnv("TM_NET_LAN", "1");
            String cmd;
            if (lanMode.equals("host")) {
                try { nativeSetEnv("TM_LAN_MATCH", Lan.writeMatchSettings(user, prefs)); }
                catch (Exception e) { Log.e(TAG, "match settings", e); }
                cmd = "/server /lan";
                Lan.startBeacon(Launcher.playerName(prefs), Lan.label(prefs, "lan_mode"), Lan.label(prefs, "lan_maps"));
            } else {
                // the runtime joins once the client's title is loaded (runtime/src/lan.c)
                nativeSetEnv("TM_LAN_JOIN", lanIp + ":" + lanPort);
                cmd = "/lan";
            }
            cmd += " /nickname=" + Launcher.playerName(prefs).replace(' ', '_');
            if (!envs.contains(";TM_CMDLINE=")) nativeSetEnv("TM_CMDLINE", cmd);
            Log.i(TAG, "LAN " + lanMode + ": " + cmd);
        }
        File temp = new File(getCacheDir(), "temp");
        user.mkdirs(); temp.mkdirs();
        status.setText("Starting TrackmaniaTurbo.exe ...");
        // Report the controllers as connected from the start (handhelds have one built in):
        // the engine only looks for newly connected XInput pads now and then, so a
        // first button press on a "not yet connected" pad would be missed.
        assignPads();
        nativeStart(new File(gameDir(), "TrackmaniaTurbo.exe").getAbsolutePath(), gameDir().getAbsolutePath(),
                user.getAbsolutePath(), temp.getAbsolutePath());
    }

    void poll() {
        ui.postDelayed(() -> {
            if (started) {
                String s = nativeStatus();
                if (s != null && !s.isEmpty()) status.setText(s);
                boolean stopped = s != null && (s.startsWith("Stopped") || s.startsWith("signal") || s.startsWith("Guest entry returned"));
                boolean show = overlayPinned || stopped || nativePresentCount() == 0;
                overlay.setVisibility(show ? View.VISIBLE : View.GONE);
                if (show) { String t = nativeLogTail(); if (t != null) log.setText(t); }
            }
            poll();
        }, 300);
    }

    // ---- input: gamepad -> XInput controller 0, keyboard -> Windows keys ----------
    static int xinputBit(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_DPAD_UP: return 0x0001;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0x0002;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0x0004;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x0008;
            case KeyEvent.KEYCODE_BUTTON_START: return 0x0010;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 0x0020;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 0x0040;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 0x0080;
            case KeyEvent.KEYCODE_BUTTON_L1: return 0x0100;
            case KeyEvent.KEYCODE_BUTTON_R1: return 0x0200;
            case KeyEvent.KEYCODE_BUTTON_A: return 0x1000;
            case KeyEvent.KEYCODE_BUTTON_B: return 0x2000;
            case KeyEvent.KEYCODE_BUTTON_X: return 0x4000;
            case KeyEvent.KEYCODE_BUTTON_Y: return 0x8000;
        }
        return 0;
    }
    static int windowsKey(int keyCode) {
        if (keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z) return 'A' + (keyCode - KeyEvent.KEYCODE_A);
        if (keyCode >= KeyEvent.KEYCODE_0 && keyCode <= KeyEvent.KEYCODE_9) return '0' + (keyCode - KeyEvent.KEYCODE_0);
        if (keyCode >= KeyEvent.KEYCODE_F1 && keyCode <= KeyEvent.KEYCODE_F12) return 0x70 + (keyCode - KeyEvent.KEYCODE_F1);
        switch (keyCode) {
            case KeyEvent.KEYCODE_ENTER: return 0x0D;
            case KeyEvent.KEYCODE_ESCAPE: return 0x1B;
            case KeyEvent.KEYCODE_SPACE: return 0x20;
            case KeyEvent.KEYCODE_DEL: return 0x08;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 0x2E;
            case KeyEvent.KEYCODE_TAB: return 0x09;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0x25;
            case KeyEvent.KEYCODE_DPAD_UP: return 0x26;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x27;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0x28;
        }
        return 0;
    }
    void sendPad(int slot) {
        int[] st = pad[slot];
        nativePad(slot, slotDevice[slot] != NO_DEVICE, st[0] | st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
    }
    static boolean isGamepad(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int src = d.getSources();
        return (src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }
    /** The XInput slot of an input device (creating one if asked and a slot is free), or -1. */
    int slotFor(int device, boolean create) {
        // injected gamepad events (adb "input gamepad", accessibility tools) come from the
        // virtual device: they act on the first controller
        if (device == android.view.KeyCharacterMap.VIRTUAL_KEYBOARD) {
            for (int i = 0; i < PADS; i++) if (slotDevice[i] != NO_DEVICE) return i;
        }
        for (int i = 0; i < PADS; i++) if (slotDevice[i] == device) return i;
        if (!create) return -1;
        for (int i = 0; i < PADS; i++) if (slotDevice[i] == NO_DEVICE) {
            slotDevice[i] = device; java.util.Arrays.fill(pad[i], 0);
            Log.i(TAG, "controller " + i + " = " + (device == TOUCH_DEVICE ? "touch pad" : String.valueOf(InputDevice.getDevice(device))));
            return i;
        }
        return -1;
    }
    void releaseSlot(int device) {
        int i = slotFor(device, false);
        if (i < 0) return;
        java.util.Arrays.fill(pad[i], 0);
        slotDevice[i] = NO_DEVICE;
        if (started) sendPad(i);              // reports "disconnected"
    }
    boolean anyGamepad() {
        for (int id : InputDevice.getDeviceIds()) if (isGamepad(InputDevice.getDevice(id))) return true;
        return false;
    }
    /** Slots for the controllers present at start: built-in ones first, then the others. */
    void assignPads() {
        java.util.List<InputDevice> devs = new java.util.ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) { InputDevice d = InputDevice.getDevice(id); if (isGamepad(d)) devs.add(d); }
        devs.sort((a, b) -> Boolean.compare(a.isExternal(), b.isExternal()));
        for (InputDevice d : devs) slotFor(d.getId(), true);
        updateTouchPad();
        for (int i = 0; i < PADS; i++) if (slotDevice[i] != NO_DEVICE) sendPad(i);
    }
    /** Shows the touch pad per the setting ("auto": only without a controller). */
    void updateTouchPad() {
        String mode = Launcher.get(prefs, "touch");
        boolean show = started && (mode.equals("on") || (mode.equals("auto") && !anyGamepad()));
        if (show && touchPad.getVisibility() != View.VISIBLE) {
            touchPad.setVisibility(View.VISIBLE);
            int slot = slotFor(TOUCH_DEVICE, true);
            if (slot >= 0) sendPad(slot);
        } else if (!show && touchPad.getVisibility() == View.VISIBLE) {
            touchPad.reset();
            touchPad.setVisibility(View.GONE);
            releaseSlot(TOUCH_DEVICE);
        }
    }

    /** Vibration from the game (XInputSetState, called on a game thread via JNI). */
    @SuppressWarnings("deprecation")
    static void onRumble(int slot, int low, int high) {
        MainActivity a = current;
        if (a == null || slot < 0 || slot >= PADS) return;
        int dev = a.slotDevice[slot];
        int amp = Math.min(255, Math.max(low, high) * 255 / 65535);
        android.os.Vibrator v = null;
        if (dev != NO_DEVICE && dev != TOUCH_DEVICE) { InputDevice d = InputDevice.getDevice(dev); if (d != null && d.getVibrator().hasVibrator()) v = d.getVibrator(); }
        if (v == null && (slot == 0 || dev == TOUCH_DEVICE)) v = (android.os.Vibrator) a.getSystemService(VIBRATOR_SERVICE);
        if (v == null || !v.hasVibrator()) return;
        if (amp <= 0) { v.cancel(); return; }
        try { v.vibrate(android.os.VibrationEffect.createOneShot(1500, Math.max(1, amp))); }
        catch (Exception e) { Log.w(TAG, "vibrate", e); }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN) return super.dispatchKeyEvent(e);
        if (!started) {
            // start screen: D-pad / stick move the focus (framework), A selects, B goes back
            boolean down0 = e.getAction() == KeyEvent.ACTION_DOWN;
            if (code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_BUTTON_START) {
                if (down0 && e.getRepeatCount() == 0) { View f = getCurrentFocus(); if (f != null) f.performClick(); }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_ESCAPE) {
                if (down0 && e.getRepeatCount() == 0 && !launcher.back() && code == KeyEvent.KEYCODE_BACK) moveTaskToBack(true);
                return true;
            }
            return super.dispatchKeyEvent(e);
        }
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        InputDevice dev = e.getDevice();
        int devSrc = (dev != null ? dev.getSources() : 0) | e.getSource();
        boolean fromPad = KeyEvent.isGamepadButton(code)
                || (devSrc & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (devSrc & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        int slot = fromPad ? slotFor(e.getDeviceId(), true) : -1;
        if (slot >= 0) {
            int[] st = pad[slot];
            if (code == KeyEvent.KEYCODE_BUTTON_L2) { st[2] = down ? 255 : 0; sendPad(slot); return true; }
            if (code == KeyEvent.KEYCODE_BUTTON_R2) { st[3] = down ? 255 : 0; sendPad(slot); return true; }
            int bit = xinputBit(code);
            if (code == KeyEvent.KEYCODE_BACK) bit = 0x2000;       // Back acts as B
            if (bit != 0) {
                if (down) st[0] |= bit; else st[0] &= ~bit;
                sendPad(slot);
                return true;
            }
        }
        int vk = code == KeyEvent.KEYCODE_BACK ? 0x1B : windowsKey(code);
        if (vk != 0) {
            if (e.getRepeatCount() == 0 || !down) nativeKey(vk, down);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    static int axis(float v) { return Math.max(-32767, Math.min(32767, Math.round(v * 32767f))); }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (!started) return super.dispatchGenericMotionEvent(e);   // start screen: the stick moves the focus
        int slot = (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK && e.getAction() == MotionEvent.ACTION_MOVE
                ? slotFor(e.getDeviceId(), true) : -1;
        if (slot >= 0) {
            int[] st = pad[slot];
            st[4] = axis(e.getAxisValue(MotionEvent.AXIS_X));
            st[5] = axis(-e.getAxisValue(MotionEvent.AXIS_Y));
            st[6] = axis(e.getAxisValue(MotionEvent.AXIS_Z));
            st[7] = axis(-e.getAxisValue(MotionEvent.AXIS_RZ));
            float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
            float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
            st[2] = Math.round(Math.max(0f, Math.min(1f, lt)) * 255f);
            st[3] = Math.round(Math.max(0f, Math.min(1f, rt)) * 255f);
            float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            st[1] = (hy < -0.5f ? 0x0001 : 0) | (hy > 0.5f ? 0x0002 : 0) | (hx < -0.5f ? 0x0004 : 0) | (hx > 0.5f ? 0x0008 : 0);
            sendPad(slot);
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }
}
