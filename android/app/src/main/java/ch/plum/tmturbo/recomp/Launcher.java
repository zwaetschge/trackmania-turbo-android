package ch.plum.tmturbo.recomp;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.util.List;
import java.util.Locale;

/**
 * The start screen: game data status and import, settings, play, LAN games. Built in code (no
 * resources), usable with touch, a mouse and the gamepad (D-pad moves the focus, A selects,
 * B goes back).
 */
final class Launcher {
    interface Actions {
        void play();
        void importFolder();
        void importZip();
        void cancelImport();
        void quit();
        void lanHost();                      // start the game as LAN host (settings in prefs)
        void lanJoin(String ip, int port);   // start the game and join that host
        void lanFind(boolean on);            // listen for hosts on the Wi-Fi (results: lanHosts)
        void lanEditName();                  // player name dialog
        String lanAddress();                 // this device's address on the Wi-Fi, or null
    }

    static final int ACCENT = Color.rgb(255, 196, 0), ACCENT2 = Color.rgb(0, 200, 255);
    static final int TEXT = Color.rgb(236, 240, 246), DIM = Color.rgb(150, 160, 178);

    final View root;
    private final Activity a;
    private final Actions act;
    private final SharedPreferences prefs;
    private final LinearLayout panel;
    private final TextView statusTitle, statusText;
    private final ProgressBar bar;
    private boolean ready, importing;
    private String page = "main";

    Launcher(Activity a, SharedPreferences prefs, Actions act) {
        this.a = a; this.prefs = prefs; this.act = act;
        FrameLayout fl = new FrameLayout(a);
        fl.addView(new Backdrop(a), new FrameLayout.LayoutParams(-1, -1));

        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(dp(56), dp(40), dp(56), dp(36));

        // left: title
        LinearLayout left = new LinearLayout(a);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setGravity(Gravity.CENTER_VERTICAL);
        TextView t1 = text("TRACKMANIA", 54, TEXT, true);
        t1.setTypeface(Typeface.create(Typeface.DEFAULT_BOLD, Typeface.BOLD_ITALIC));
        t1.setLetterSpacing(0.02f);
        TextView t2 = text("TURBO", 54, ACCENT, true);
        t2.setTypeface(Typeface.create(Typeface.DEFAULT_BOLD, Typeface.BOLD_ITALIC));
        t2.setPadding(0, dp(-14), 0, 0);
        TextView sub = text("Android · statische Rekompilierung", 16, DIM, false);
        sub.setPadding(dp(2), dp(10), 0, 0);
        String ver = "";
        try { ver = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionName; } catch (Exception ignored) { }
        TextView v = text("Version " + ver, 13, Color.argb(160, 150, 160, 178), false);
        v.setPadding(dp(2), dp(4), 0, 0);
        left.addView(t1); left.addView(t2); left.addView(sub); left.addView(v);
        left.setPadding(0, 0, dp(28), 0);
        // the title stays on one line: on narrow screens both words shrink together
        t1.setMaxLines(1); t2.setMaxLines(1);
        t1.addOnLayoutChangeListener((view, l, t, r, b, ol, ot, or, ob) -> {
            int avail = (r - l) - t1.getPaddingLeft() - t1.getPaddingRight();
            if (avail <= 0 || r - l == or - ol) return;
            android.graphics.Paint pt = new android.graphics.Paint(t1.getPaint());
            float full = 54 * a.getResources().getDisplayMetrics().scaledDensity;
            pt.setTextSize(full);
            float w = pt.measureText("TRACKMANIA") * 1.06f;      // italic overhang
            float size = w > avail ? full * avail / w : full;
            int gap = -Math.round(dp(14) * size / full);        // the tight line gap shrinks with the words
            t1.post(() -> { t1.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, size); t2.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, size); t2.setPadding(0, gap, 0, 0); });
        });
        row.addView(left, new LinearLayout.LayoutParams(0, -1, 1f));

        // right: status card + menu
        LinearLayout right = new LinearLayout(a);
        right.setOrientation(LinearLayout.VERTICAL);
        right.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(20), dp(16), dp(20), dp(16));
        card.setBackground(round(Color.argb(150, 18, 22, 32), dp(14), Color.argb(60, 255, 255, 255)));
        statusTitle = text("", 15, TEXT, true);
        statusText = text("", 13, DIM, false);
        statusText.setPadding(0, dp(4), 0, 0);
        bar = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(View.GONE);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        card.addView(statusTitle); card.addView(statusText);
        card.addView(bar, new LinearLayout.LayoutParams(-1, dp(10)));
        right.addView(card, new LinearLayout.LayoutParams(-1, -2));
        panel = new LinearLayout(a);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(0, dp(14), 0, dp(4));
        // pages taller than the screen (LAN page on small displays) scroll
        android.widget.ScrollView sv = new android.widget.ScrollView(a);
        sv.setVerticalScrollBarEnabled(false);
        sv.setFillViewport(false);
        sv.addView(panel, new android.widget.FrameLayout.LayoutParams(-1, -2));
        right.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));
        row.addView(right, new LinearLayout.LayoutParams(dp(460), -1));

        fl.addView(row, new FrameLayout.LayoutParams(-1, -1));
        root = fl;
    }

    // ------------------------------------------------------------------ state
    void setReady(boolean ready, String detail) {
        this.ready = ready;
        if (importing) return;
        statusTitle.setText(ready ? "Spieldaten bereit" : "Keine Spieldaten");
        statusTitle.setTextColor(ready ? Color.rgb(120, 230, 140) : ACCENT);
        statusText.setText(detail);
        bar.setVisibility(View.GONE);
        if (page.equals("main")) showMain();
    }

    void importStarted() {
        importing = true;
        statusTitle.setText("Spieldaten werden importiert …");
        statusTitle.setTextColor(ACCENT);
        statusText.setText("Dateien werden gezählt …");
        bar.setVisibility(View.VISIBLE); bar.setProgress(0);
        page = "import-run";
        panel.removeAllViews();
        View c = button("ABBRECHEN", false, x -> act.cancelImport());
        panel.addView(c);
        c.requestFocus();
    }

    void importProgress(String file, long done, long total, long bps) {
        if (total > 0) bar.setProgress((int) (1000.0 * done / total));
        String eta = "";
        if (bps > 0 && total > done) {
            long s = (total - done) / bps;
            eta = String.format(Locale.ROOT, " · noch %d:%02d", s / 60, s % 60);
        }
        statusText.setText(String.format(Locale.ROOT, "%.2f / %.2f GB · %.0f MB/s%s\n%s",
                done / 1e9, total / 1e9, bps / 1e6, eta, file));
    }

    void importFinished(boolean ok, String msg) {
        importing = false;
        bar.setVisibility(View.GONE);
        statusTitle.setText(ok ? "Spieldaten bereit" : "Import nicht abgeschlossen");
        statusTitle.setTextColor(ok ? Color.rgb(120, 230, 140) : Color.rgb(255, 110, 90));
        statusText.setText(msg);
        ready = ok || ready;
        showMain();
    }

    /** Gamepad B / Back: one page up; true when handled. */
    boolean back() {
        if (importing) return true;
        if (page.equals("lan-host") || page.equals("lan-join")) { showLan(); return true; }
        if (!page.equals("main")) { showMain(); return true; }
        return false;
    }

    private void leave() { if (page.equals("lan-join")) act.lanFind(false); }

    // ------------------------------------------------------------------ pages
    void showMain() {
        leave();
        page = "main";
        panel.removeAllViews();
        View play = button("SPIELEN", true, x -> { if (ready) act.play(); });
        play.setEnabled(ready);
        play.setAlpha(ready ? 1f : 0.35f);
        panel.addView(play);
        View lan = button("LAN-SPIEL", false, x -> { if (ready) showLan(); });
        lan.setEnabled(ready);
        lan.setAlpha(ready ? 1f : 0.35f);
        panel.addView(lan);
        panel.addView(button(ready ? "SPIELDATEN NEU IMPORTIEREN" : "SPIELDATEN IMPORTIEREN", !ready, x -> showImport()));
        panel.addView(button("EINSTELLUNGEN", false, x -> showSettings()));
        panel.addView(button("BEENDEN", false, x -> act.quit()));
        panel.getChildAt(ready ? 0 : 2).requestFocus();
    }

    // ------------------------------------------------------------------ LAN games
    static String playerName(SharedPreferences p) {
        String n = p.getString("lan_name", "");
        if (n.isEmpty()) n = android.os.Build.MODEL == null ? "Spieler" : android.os.Build.MODEL;
        return n.replaceAll("[^A-Za-z0-9 _.-]", "").trim().isEmpty() ? "Spieler" : n.replaceAll("[^A-Za-z0-9 _.-]", "").trim();
    }

    void showLan() {
        leave();
        page = "lan";
        panel.removeAllViews();
        String ip = act.lanAddress();
        TextView info = text((ip == null ? "Kein WLAN gefunden: alle Geräte müssen im selben WLAN sein (oder im Hotspot eines der Geräte)."
                : "Dieses Gerät: " + ip + " · Name: " + playerName(prefs) + "\nAlle Geräte im selben WLAN, jedes mit eigenen Spieldaten. "
                + "Eines eröffnet das Spiel, die anderen treten bei.\nOffene Spiele erscheinen unter „Spiel beitreten“ von selbst; sonst die IP-Adresse des Gastgebers eingeben.") , 13, DIM, false);
        info.setPadding(dp(4), 0, dp(4), dp(6));
        panel.addView(info);
        View host = button("SPIEL ERÖFFNEN", true, x -> showLanHost());
        panel.addView(host);
        panel.addView(button("SPIEL BEITRETEN", false, x -> showLanJoin()));
        panel.addView(button("SPIELERNAME:  " + playerName(prefs), false, x -> act.lanEditName()));
        panel.addView(button("ZURÜCK", false, x -> showMain()));
        host.requestFocus();
    }

    void showLanHost() {
        leave();
        page = "lan-host";
        panel.removeAllViews();
        View first = null;
        for (String[] s : Lan.HOST_SETTINGS) {
            String[] vals = s[2].split("\\|");
            TextView b = (TextView) button("", false, null);
            Runnable refresh = () -> b.setText(s[1] + ":  " + Lan.label(prefs, s[0]));
            refresh.run();
            b.setOnClickListener(x -> {
                String cur = Lan.get(prefs, s[0]);
                int i = 0; for (int k = 0; k < vals.length; k++) if (vals[k].equals(cur)) i = k;
                prefs.edit().putString(s[0], vals[(i + 1) % vals.length]).apply();
                refresh.run();
            });
            panel.addView(b);
            if (first == null) first = b;
        }
        panel.addView(button("STARTEN", true, x -> act.lanHost()));
        panel.addView(button("ZURÜCK", false, x -> showLan()));
        if (first != null) first.requestFocus();
    }

    private LinearLayout hostList;

    void showLanJoin() {
        page = "lan-join";
        panel.removeAllViews();
        TextView info = text("Offene Spiele im WLAN (die Liste aktualisiert sich selbst):", 13, DIM, false);
        info.setPadding(dp(4), 0, dp(4), dp(2));
        panel.addView(info);
        hostList = new LinearLayout(a);
        hostList.setOrientation(LinearLayout.VERTICAL);
        panel.addView(hostList);
        lanHosts(new java.util.ArrayList<>());
        panel.addView(button("IP-ADRESSE EINGEBEN", false, x -> askIp()));
        View back = button("ZURÜCK", false, x -> showLan());
        panel.addView(back);
        back.requestFocus();
        act.lanFind(true);
    }

    /** From the finder thread's results (UI thread). */
    void lanHosts(List<Lan.Host> hosts) {
        if (!page.equals("lan-join") || hostList == null) return;
        View focused = a.getCurrentFocus();
        String keep = focused != null && focused.getTag() instanceof String ? (String) focused.getTag() : null;
        hostList.removeAllViews();
        if (hosts.isEmpty()) {
            TextView none = text("Noch kein Spiel gefunden … (auf dem anderen Gerät: LAN-SPIEL → SPIEL ERÖFFNEN)", 13, ACCENT2, false);
            none.setPadding(dp(4), dp(8), dp(4), dp(4));
            hostList.addView(none);
            return;
        }
        for (Lan.Host h : hosts) {
            View b = button(h.name + "  ·  " + h.mode + "  ·  " + h.maps + "\n" + h.ip, true, x -> act.lanJoin(h.ip, h.port));
            b.setTag(h.ip);
            hostList.addView(b);
            if (h.ip.equals(keep)) b.requestFocus();
        }
    }

    private void askIp() {
        android.widget.EditText e = new android.widget.EditText(a);
        e.setHint("z. B. 192.168.1.20");
        e.setInputType(android.text.InputType.TYPE_CLASS_TEXT);
        e.setText(prefs.getString("lan_last_ip", ""));
        new android.app.AlertDialog.Builder(a)
                .setTitle("IP-Adresse des Spiels")
                .setView(e)
                .setPositiveButton("BEITRETEN", (d, w) -> {
                    String ip = e.getText().toString().trim();
                    if (ip.matches("\\d{1,3}(\\.\\d{1,3}){3}")) { prefs.edit().putString("lan_last_ip", ip).apply(); act.lanJoin(ip, Lan.GAME_PORT); }
                })
                .setNegativeButton("ABBRECHEN", null)
                .show();
    }

    void showImport() {
        leave();
        page = "import";
        panel.removeAllViews();
        TextView info = text("Wähle den Ordner deiner Trackmania-Turbo-Installation für den PC (der Ordner mit "
                + "TrackmaniaTurbo.exe, z. B. auf einer SD-Karte oder unter Downloads) oder eine ZIP-Datei dieses Ordners. "
                + "Kopiert werden nur die Dateien, die das Spiel braucht (etwa 3,8 GB).", 13, DIM, false);
        info.setPadding(dp(4), 0, dp(4), dp(10));
        panel.addView(info);
        View f = button("ORDNER WÄHLEN", true, x -> act.importFolder());
        panel.addView(f);
        panel.addView(button("ZIP-DATEI WÄHLEN", false, x -> act.importZip()));
        panel.addView(button("ZURÜCK", false, x -> showMain()));
        f.requestFocus();
    }

    // settings: key, label, values (stored), labels (shown)
    static final String[][] SETTINGS = {
            { "fps", "Bildrate", "60|120|30", "60 fps (stabil, empfohlen)|120 fps (120-Hz-Display, Gerät wird warm)|30 fps" },
            { "touch", "Touch-Steuerung", "auto|on|off", "automatisch (ohne Controller)|immer|aus" },
            { "hudcalm", "Turbo-Anzeige", "1|0", "ruhig pulsierend|wie im Original (16 Hz)" },
            { "tracks", "Community-Strecken", "all|800|400|0", "alle|800 beliebteste|400 beliebteste|keine" },
            { "log", "Protokoll beim Start", "0|1", "aus|anzeigen" },
    };

    static String get(SharedPreferences p, String key) {
        for (String[] s : SETTINGS) if (s[0].equals(key)) return p.getString(key, s[2].split("\\|")[0]);
        return "";
    }

    /** Shows the current page again (after a dialog changed something on it). */
    void refreshPage() {
        switch (page) {
            case "settings": showSettings(); break;
            case "lan": showLan(); break;
            default: if (page.startsWith("lan")) showLan(); else showMain();
        }
    }

    void showSettings() {
        leave();
        page = "settings";
        panel.removeAllViews();
        View first = button("SPIELERNAME:  " + playerName(prefs), false, x -> act.lanEditName());
        panel.addView(first);
        for (String[] s : SETTINGS) {
            String[] vals = s[2].split("\\|"), labels = s[3].split("\\|");
            TextView b = (TextView) button("", false, null);
            Runnable refresh = () -> {
                String cur = get(prefs, s[0]);
                int i = 0; for (int k = 0; k < vals.length; k++) if (vals[k].equals(cur)) i = k;
                b.setText(s[1] + ":  " + labels[i]);
            };
            refresh.run();
            b.setOnClickListener(x -> {
                String cur = get(prefs, s[0]);
                int i = 0; for (int k = 0; k < vals.length; k++) if (vals[k].equals(cur)) i = k;
                prefs.edit().putString(s[0], vals[(i + 1) % vals.length]).apply();
                refresh.run();
            });
            panel.addView(b);
            if (first == null) first = b;
        }
        TextView note = text("Grafik, Ton und Steuerung stellst du im Spiel ein: Bildschirm oben antippen (oder die Maus an den "
                + "oberen Rand) und das Zahnrad wählen.", 12, DIM, false);
        note.setPadding(dp(4), dp(6), dp(4), dp(6));
        panel.addView(note);
        panel.addView(button("ZURÜCK", false, x -> showMain()));
        if (first != null) first.requestFocus();
    }

    // ------------------------------------------------------------------ widgets
    private View button(String label, boolean primary, View.OnClickListener l) {
        TextView b = text(label, primary ? 19 : 15, TEXT, true);
        b.setTypeface(Typeface.create(Typeface.DEFAULT_BOLD, primary ? Typeface.BOLD_ITALIC : Typeface.BOLD));
        b.setLetterSpacing(0.04f);
        b.setPadding(dp(20), dp(primary ? 16 : 12), dp(20), dp(primary ? 16 : 12));
        b.setFocusable(true); b.setClickable(true);
        StateListDrawable sl = new StateListDrawable();
        GradientDrawable on = round(ACCENT, dp(10), 0);
        sl.addState(new int[] { android.R.attr.state_focused }, on);
        sl.addState(new int[] { android.R.attr.state_pressed }, on);
        sl.addState(new int[] {}, round(primary ? Color.argb(70, 255, 196, 0) : Color.argb(38, 255, 255, 255), dp(10), 0));
        b.setBackground(sl);
        b.setOnFocusChangeListener((v, f) -> {
            ((TextView) v).setTextColor(f ? Color.rgb(16, 18, 24) : TEXT);
            v.animate().scaleX(f ? 1.02f : 1f).scaleY(f ? 1.02f : 1f).setDuration(120).start();
        });
        if (l != null) b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(8);
        b.setLayoutParams(lp);
        return b;
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private GradientDrawable round(int fill, int radius, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(radius);
        if (stroke != 0) g.setStroke(dp(1), stroke);
        return g;
    }

    private int dp(float v) { return Math.round(v * a.getResources().getDisplayMetrics().density); }

    /** Dark gradient with two diagonal speed stripes. */
    private static final class Backdrop extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        Backdrop(Activity a) { super(a); }
        @Override protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            p.setShader(new LinearGradient(0, 0, w, h, Color.rgb(14, 20, 34), Color.rgb(4, 6, 10), Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, p);
            p.setShader(new LinearGradient(0, h, w * 0.6f, 0, Color.argb(90, 255, 196, 0), Color.argb(0, 255, 196, 0), Shader.TileMode.CLAMP));
            path.reset();
            path.moveTo(w * 0.05f, h); path.lineTo(w * 0.17f, h); path.lineTo(w * 0.62f, 0); path.lineTo(w * 0.50f, 0); path.close();
            c.drawPath(path, p);
            p.setShader(new LinearGradient(0, h, w * 0.7f, 0, Color.argb(70, 0, 200, 255), Color.argb(0, 0, 200, 255), Shader.TileMode.CLAMP));
            path.reset();
            path.moveTo(w * 0.19f, h); path.lineTo(w * 0.22f, h); path.lineTo(w * 0.67f, 0); path.lineTo(w * 0.64f, 0); path.close();
            c.drawPath(path, p);
            p.setShader(null);
        }
    }
}
