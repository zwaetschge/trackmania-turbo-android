package ch.plum.tmturbo.recomp;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.StatFs;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Copies the player's own Trackmania Turbo installation into the app's storage.
 *
 * The APK carries no game data: the recompiled code is built from TrackmaniaTurbo.exe of
 * one specific build (SHA-256 below), and everything the game loads at run time -- the exe
 * image itself, its DLLs, GameData and the packs -- comes from the player's copy, picked as
 * a folder (Storage Access Framework) or as a ZIP of that folder. Only what the game uses is
 * copied: TrackmaniaTurbo.exe, nadeo.ini, the DLLs, GameData/ and Packs/ (about 3.8 GB);
 * installers, Uplay and support files are left out. Files that are already there with the
 * same size are kept, so an interrupted import continues where it stopped.
 */
final class GameImporter {
    /** TrackmaniaTurbo.exe the code was recompiled from (version 2016-11-07_16_15). */
    static final String EXE_SHA256 = "454b0d4d9eb778a298e523c20f9bb0b9e5459217afd45566dfcffe9bb8312879";
    static final String MARKER = ".import-ok";

    interface Progress { void update(String file, long done, long total, long bytesPerSecond); }

    static final class Result {
        final boolean ok; final String message; final int files; final long bytes;
        Result(boolean ok, String message, int files, long bytes) { this.ok = ok; this.message = message; this.files = files; this.bytes = bytes; }
    }

    private final ContentResolver cr;
    private final File dest;
    private final Progress progress;
    private volatile boolean cancelled;
    private long done, total, t0, lastUi;
    private final byte[] buf = new byte[1 << 20];

    GameImporter(ContentResolver cr, File dest, Progress progress) { this.cr = cr; this.dest = dest; this.progress = progress; }
    void cancel() { cancelled = true; }

    /** Is "rel" (forward slashes, relative to the game folder) something the game needs? */
    static boolean wanted(String rel) {
        String l = rel.toLowerCase(Locale.ROOT);
        if (l.contains("..")) return false;
        if (l.startsWith("gamedata/") || l.startsWith("packs/")) return !l.endsWith("/");
        if (l.contains("/")) return false;
        return l.equals("trackmaniaturbo.exe") || l.equals("nadeo.ini") || l.endsWith(".dll");
    }

    static boolean installed(File dir) {
        return new File(dir, "TrackmaniaTurbo.exe").isFile() && new File(dir, "Packs/TMTurbo.Title.Pack.Gbx").isFile();
    }

    static String sha256(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] b = new byte[1 << 16]; int n;
            while ((n = in.read(b)) > 0) md.update(b, 0, n);
            StringBuilder sb = new StringBuilder();
            for (byte x : md.digest()) sb.append(String.format(Locale.ROOT, "%02x", x));
            return sb.toString();
        } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
    }

    static long freeBytes(File dir) {
        dir.mkdirs();
        StatFs s = new StatFs(dir.getAbsolutePath());
        return s.getAvailableBytes();
    }

    // ------------------------------------------------------------------ folder (SAF tree)
    private static final class Doc { String id, rel; long size; }

    private List<Doc> list(Uri tree, String docId, String rel, int depth, List<Doc> out) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        try (Cursor c = cr.query(children, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE }, null, null, null)) {
            if (c == null) return out;
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                String r = rel.isEmpty() ? name : rel + "/" + name;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (depth < 8) list(tree, id, r, depth + 1, out);
                } else {
                    Doc d = new Doc(); d.id = id; d.rel = r; d.size = c.isNull(3) ? 0 : c.getLong(3);
                    out.add(d);
                }
            }
        }
        return out;
    }

    /** The player picked a folder: the game folder itself, or a folder that contains it. */
    Result importTree(Uri tree) {
        try {
            String rootId = DocumentsContract.getTreeDocumentId(tree);
            List<Doc> all = list(tree, rootId, "", 0, new ArrayList<>());
            String prefix = findPrefix(all);
            if (prefix == null)
                return new Result(false, S.t(R.string.imp_no_exe_dir), 0, 0);
            List<Doc> files = new ArrayList<>();
            total = 0;
            for (Doc d : all) {
                if (!d.rel.startsWith(prefix)) continue;
                String rel = d.rel.substring(prefix.length());
                if (!wanted(rel)) continue;
                d.rel = rel; files.add(d); total += d.size;
            }
            // the exe first: a wrong game version stops the import before 3.8 GB are copied
            files.sort((a, b) -> Boolean.compare(!a.rel.equalsIgnoreCase("TrackmaniaTurbo.exe"), !b.rel.equalsIgnoreCase("TrackmaniaTurbo.exe")));
            long need = 0;
            for (Doc d : files) { File f = new File(dest, d.rel); if (!(f.isFile() && f.length() == d.size)) need += d.size; }
            if (need > freeBytes(dest) - (200L << 20))
                return new Result(false, S.f(R.string.imp_no_space, need / 1e9, freeBytes(dest) / 1e9), 0, 0);
            t0 = System.nanoTime(); done = 0;
            int n = 0;
            for (Doc d : files) {
                if (cancelled) return new Result(false, S.t(R.string.imp_cancelled), n, done);
                File out = new File(dest, d.rel);
                if (out.isFile() && out.length() == d.size) { done += d.size; tick(d.rel); n++; continue; }
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id))) {
                    copy(in, out, d.rel);
                }
                n++;
                if (d.rel.equalsIgnoreCase("TrackmaniaTurbo.exe")) {
                    Result bad = checkExe();
                    if (bad != null) return bad;
                }
            }
            return finish(n);
        } catch (Exception e) {
            return new Result(false, cancelled ? S.t(R.string.imp_cancelled) : S.f(R.string.imp_failed, e.getMessage()), 0, done);
        }
    }

    private static String findPrefix(List<Doc> all) {
        String best = null;
        for (Doc d : all) {
            String l = d.rel.toLowerCase(Locale.ROOT);
            if (l.equals("trackmaniaturbo.exe") || l.endsWith("/trackmaniaturbo.exe")) {
                String p = d.rel.substring(0, d.rel.length() - "TrackmaniaTurbo.exe".length());
                if (best == null || p.length() < best.length()) best = p;
            }
        }
        return best;
    }

    // ------------------------------------------------------------------ ZIP
    /** The player picked a ZIP of the game folder (possibly with a top folder inside). */
    Result importZip(Uri zip, long zipSize) {
        try {
            total = zipSize > 0 ? zipSize : 0;
            if (zipSize > freeBytes(dest) - (200L << 20))
                return new Result(false, S.f(R.string.imp_no_space, zipSize / 1e9, freeBytes(dest) / 1e9), 0, 0);
            t0 = System.nanoTime(); done = 0;
            int n = 0; boolean sawExe = false;
            CountingStream counted;
            try (InputStream raw = cr.openInputStream(zip)) {
                // ZipInputStream reads 512 bytes at a time; straight from shared storage (FUSE /
                // a SAF pipe) that is one slow system call each: 0.2 MB/s. A big buffer in between.
                counted = new CountingStream(new java.io.BufferedInputStream(raw, 1 << 20));
                try (ZipInputStream zin = new ZipInputStream(counted)) {
                    ZipEntry e;
                    while ((e = zin.getNextEntry()) != null) {
                        if (cancelled) return new Result(false, S.t(R.string.imp_cancelled), n, done);
                        if (e.isDirectory()) continue;
                        String rel = strip(e.getName().replace('\\', '/'));
                        if (rel == null || !wanted(rel)) continue;
                        File out = new File(dest, rel);
                        if (out.isFile() && e.getSize() >= 0 && out.length() == e.getSize()) { n++; continue; }
                        copyZip(zin, out, rel, counted);
                        n++;
                        if (rel.equalsIgnoreCase("TrackmaniaTurbo.exe")) {
                            sawExe = true;
                            Result bad = checkExe();
                            if (bad != null) return bad;
                        }
                    }
                }
            }
            if (!sawExe && !new File(dest, "TrackmaniaTurbo.exe").isFile())
                return new Result(false, S.t(R.string.imp_no_exe_zip), n, done);
            return finish(n);
        } catch (Exception e) {
            return new Result(false, cancelled ? S.t(R.string.imp_cancelled) : S.f(R.string.imp_failed, e.getMessage()), 0, done);
        }
    }

    /** Drops leading folders until the path starts with something of the game folder. */
    private static String strip(String name) {
        String s = name;
        while (true) {
            String first = s.contains("/") ? s.substring(0, s.indexOf('/')) : s;
            String l = first.toLowerCase(Locale.ROOT);
            if (l.equals("gamedata") || l.equals("packs") || (!s.contains("/") && wanted(s))) return s;
            if (!s.contains("/")) return null;
            s = s.substring(s.indexOf('/') + 1);
        }
    }

    private static final class CountingStream extends java.io.FilterInputStream {
        long count;
        CountingStream(InputStream in) { super(in); }
        @Override public int read() throws IOException { int r = super.read(); if (r >= 0) count++; return r; }
        @Override public int read(byte[] b, int o, int l) throws IOException { int r = super.read(b, o, l); if (r > 0) count += r; return r; }
        @Override public long skip(long n) throws IOException { long r = super.skip(n); if (r > 0) count += r; return r; }
    }

    // ------------------------------------------------------------------ common
    private void copy(InputStream in, File out, String rel) throws IOException {
        out.getParentFile().mkdirs();
        File part = new File(out.getPath() + ".part");
        try (OutputStream o = new FileOutputStream(part)) {
            int k;
            while ((k = in.read(buf)) > 0) {
                if (cancelled) throw new IOException("cancelled");
                o.write(buf, 0, k); done += k; tick(rel);
            }
        }
        if (!part.renameTo(out)) { out.delete(); if (!part.renameTo(out)) throw new IOException("cannot write " + out); }
    }

    private void copyZip(ZipInputStream in, File out, String rel, CountingStream counted) throws IOException {
        out.getParentFile().mkdirs();
        File part = new File(out.getPath() + ".part");
        try (OutputStream o = new FileOutputStream(part)) {
            int k;
            while ((k = in.read(buf)) > 0) {
                if (cancelled) throw new IOException("cancelled");
                o.write(buf, 0, k); done = counted.count; tick(rel);
            }
        }
        if (!part.renameTo(out)) { out.delete(); if (!part.renameTo(out)) throw new IOException("cannot write " + out); }
    }

    private void tick(String rel) {
        long now = System.nanoTime();
        if (now - lastUi < 150_000_000L && done < total) return;
        lastUi = now;
        double s = (now - t0) / 1e9;
        progress.update(rel, done, total, s > 0.5 ? (long) (done / s) : 0);
    }

    private Result checkExe() throws IOException {
        File exe = new File(dest, "TrackmaniaTurbo.exe");
        String h = sha256(exe);
        if (!EXE_SHA256.equals(h)) {
            exe.delete();
            return new Result(false, S.f(R.string.imp_wrong_exe, EXE_SHA256.substring(0, 12), h.substring(0, 12)), 0, done);
        }
        return null;
    }

    private Result finish(int n) throws IOException {
        if (!installed(dest)) return new Result(false, S.t(R.string.imp_incomplete), n, done);
        File exe = new File(dest, "TrackmaniaTurbo.exe");
        if (!EXE_SHA256.equals(sha256(exe))) return checkExe();
        try (FileOutputStream o = new FileOutputStream(new File(dest, MARKER))) {
            o.write(("sha256=" + EXE_SHA256 + "\nfiles=" + n + "\nbytes=" + done + "\n").getBytes("UTF-8"));
        }
        return new Result(true, S.f(R.string.imp_done, n, done / 1e9), n, done);
    }
}
