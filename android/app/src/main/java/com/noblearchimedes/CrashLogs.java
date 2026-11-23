package com.noblearchimedes;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Persists Android 11+ ApplicationExitInfo records (native tombstones, Java crashes, ANRs,
 * and low-memory / LMK kills) into the app's logs/ directory on the next launch so device-specific
 * crash reports contain actionable diagnostics.
 */
final class CrashLogs {
    private static final String TAG = "NobleCrashLogs";
    private static final String PREFS = "noble_crash_logs";
    private static final String LAST = "last_ts";
    private static final AtomicBoolean busy = new AtomicBoolean();

    private CrashLogs() {}

    static void save(Context context) {
        if (Build.VERSION.SDK_INT < 30 || !busy.compareAndSet(false, true)) return;
        Context app = context.getApplicationContext();
        File ext = app.getExternalFilesDir(null);
        File dir = new File(ext != null ? ext : app.getFilesDir(), "logs");
        File fallback = new File(app.getFilesDir(), "logs");
        new Thread(() -> {
            try {
                saveNow(app, dir, fallback);
            } catch (Throwable t) {
                Log.w(TAG, "crash logs", t);
            } finally {
                busy.set(false);
            }
        }, "noble-crash-logs").start();
    }

    private static void saveNow(Context app, File dir, File fallback) throws Exception {
        if (Build.VERSION.SDK_INT < 30) return;
        ActivityManager am = app.getSystemService(ActivityManager.class);
        if (am == null) return;
        SharedPreferences prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        long last = prefs.getLong(LAST, 0), newest = last;
        for (ApplicationExitInfo e : am.getHistoricalProcessExitReasons(app.getPackageName(), 0, 0)) {
            int reason = e.getReason();
            if (e.getTimestamp() <= last) continue;
            if (reason != ApplicationExitInfo.REASON_CRASH_NATIVE
                    && reason != ApplicationExitInfo.REASON_CRASH
                    && reason != ApplicationExitInfo.REASON_ANR
                    && reason != ApplicationExitInfo.REASON_LOW_MEMORY
                    && reason != ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE) {
                continue;
            }
            newest = Math.max(newest, e.getTimestamp());
            byte[] text = describe(e);
            String name = "crash_" + new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US)
                    .format(new Date(e.getTimestamp())) + ".txt";
            if (!write(new File(dir, name), text)) write(new File(fallback, name), text);
        }
        if (newest != last) prefs.edit().putLong(LAST, newest).apply();
    }

    private static boolean write(File f, byte[] text) {
        File parent = f.getParentFile();
        if (parent != null) {
            //noinspection ResultOfMethodCallIgnored
            parent.mkdirs();
        }
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(text);
            return true;
        } catch (Exception ex) {
            return false;
        }
    }

    private static String reasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_CRASH_NATIVE:
                return "native crash";
            case ApplicationExitInfo.REASON_CRASH:
                return "Java crash";
            case ApplicationExitInfo.REASON_ANR:
                return "not responding (ANR)";
            case ApplicationExitInfo.REASON_LOW_MEMORY:
                return "low memory kill (LMK)";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE:
                return "excessive resource kill";
            default:
                return "exit reason " + reason;
        }
    }

    private static byte[] describe(ApplicationExitInfo e) throws Exception {
        StringBuilder s = new StringBuilder();
        s.append("Noble Archimedes ").append(reasonName(e.getReason())).append(", ")
                .append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(e.getTimestamp())))
                .append('\n');
        s.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" (Android ").append(Build.VERSION.RELEASE).append(", API ").append(Build.VERSION.SDK_INT)
                .append(")\n");
        s.append("Process: ").append(e.getProcessName()).append(" (pid ").append(e.getPid()).append(")\n");
        if (e.getDescription() != null) s.append("Description: ").append(e.getDescription()).append('\n');
        s.append("Status: ").append(e.getStatus()).append(", PSS ").append(e.getPss() / 1024).append(" MB, RSS ")
                .append(e.getRss() / 1024).append(" MB\n\n");
        byte[] trace = null;
        try (InputStream in = e.getTraceInputStream()) {
            if (in != null) trace = readAll(in);
        }
        if (trace == null || trace.length == 0) {
            s.append("Android kept no trace for this exit.\n");
        } else if (e.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
            try {
                tombstone(s, new Msg(trace, 0, trace.length));
            } catch (Exception ex) {
                s.append("Could not decode tombstone protobuf (").append(ex).append("), raw hex:\n");
                for (int i = 0; i < trace.length; i++)
                    s.append(String.format(Locale.US, "%02x", trace[i] & 0xFF)).append(i % 32 == 31 ? '\n' : ' ');
                s.append('\n');
            }
        } else {
            s.append(new String(trace, StandardCharsets.UTF_8));
        }
        return s.toString().getBytes(StandardCharsets.UTF_8);
    }

    private static byte[] readAll(InputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
        return out.toByteArray();
    }

    // Decodes Android debuggerd tombstone.proto into standard text tombstone layout.
    private static void tombstone(StringBuilder s, Msg t) {
        s.append("*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n");
        s.append("Build fingerprint: '").append(t.str(2)).append("'\n");
        s.append("Revision: '").append(t.str(3)).append("'\n");
        s.append("Timestamp: ").append(t.str(4)).append('\n');
        s.append("Process uptime: ").append(t.num(20)).append("s\n");
        String cmdline = String.join(" ", t.strs(9));
        s.append("Cmdline: ").append(cmdline).append('\n');
        long pid = t.num(5), tid = t.num(6);
        Map<Long, Msg> threads = new HashMap<>();
        for (Msg entry : t.msgs(16)) {
            Msg th = entry.msg(2);
            if (th != null) threads.put(entry.num(1), th);
        }
        Msg crashed = threads.get(tid);
        s.append("pid: ").append(pid).append(", tid: ").append(tid).append(", name: ")
                .append(crashed != null ? crashed.str(2) : "?").append("  >>> ").append(cmdline).append(" <<<\n");
        s.append("uid: ").append(t.num(7)).append('\n');
        Msg sig = t.msg(10);
        if (sig != null) {
            s.append("signal ").append((int) sig.num(1)).append(" (").append(sig.str(2)).append("), code ")
                    .append((int) sig.num(3)).append(" (").append(sig.str(4)).append(")");
            if (sig.num(8) != 0) s.append(", fault addr 0x").append(Long.toHexString(sig.num(9)));
            if (sig.num(5) != 0) s.append(", from pid ").append((int) sig.num(7)).append(", uid ").append((int) sig.num(6));
            s.append('\n');
        }
        if (!t.str(14).isEmpty()) s.append("Abort message: '").append(t.str(14)).append("'\n");
        for (Msg cause : t.msgs(15)) s.append("Cause: ").append(cause.str(1)).append('\n');
        if (crashed != null) thread(s, crashed, true);
        for (Map.Entry<Long, Msg> th : threads.entrySet()) {
            if (th.getKey() == tid) continue;
            s.append("\n--- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---\n");
            s.append("pid: ").append(pid).append(", tid: ").append(th.getKey()).append(", name: ")
                    .append(th.getValue().str(2)).append("  >>> ").append(cmdline).append(" <<<\n");
            thread(s, th.getValue(), false);
        }
        for (Msg buffer : t.msgs(18)) {
            s.append("\n--------- log ").append(buffer.str(1)).append('\n');
            for (Msg l : buffer.msgs(2)) {
                int prio = (int) l.num(4);
                char p = prio >= 2 && prio <= 7 ? "VDIWEF".charAt(prio - 2) : '?';
                s.append(l.str(1)).append(' ').append(String.format(Locale.US, "%5d %5d", l.num(2), l.num(3)))
                        .append(' ').append(p).append(' ').append(l.str(5)).append(": ").append(l.str(6)).append('\n');
            }
        }
    }

    private static void thread(StringBuilder s, Msg th, boolean registers) {
        if (registers) {
            List<Msg> regs = th.msgs(3);
            for (int i = 0; i < regs.size(); i++) {
                s.append(i % 4 == 0 ? "    " : "  ")
                        .append(String.format(Locale.US, "%-4s %016x", regs.get(i).str(1), regs.get(i).num(2)));
                if (i % 4 == 3 || i == regs.size() - 1) s.append('\n');
            }
        }
        for (String note : th.strs(7)) s.append("  NOTE: ").append(note).append('\n');
        List<Msg> frames = th.msgs(4);
        s.append(frames.size() == 1 ? "\nbacktrace:\n" : "\n" + frames.size() + " total frames\nbacktrace:\n");
        for (int i = 0; i < frames.size(); i++) {
            Msg f = frames.get(i);
            s.append(String.format(Locale.US, "      #%02d pc %016x  %s", i, f.num(1), f.str(6)));
            if (f.num(7) != 0) s.append(" (offset 0x").append(Long.toHexString(f.num(7))).append(')');
            if (!f.str(4).isEmpty()) s.append(" (").append(f.str(4)).append('+').append(f.num(5)).append(')');
            if (!f.str(8).isEmpty()) s.append(" (BuildId: ").append(f.str(8)).append(')');
            s.append('\n');
        }
    }

    private static final class Msg {
        private final Map<Integer, List<Object>> fields = new HashMap<>();

        Msg(byte[] b, int p, int end) {
            while (p < end) {
                long key = 0;
                for (int shift = 0;; shift += 7) {
                    byte c = b[p++];
                    key |= (long) (c & 0x7F) << shift;
                    if (c >= 0) break;
                }
                int num = (int) (key >>> 3), wire = (int) (key & 7);
                Object v;
                switch (wire) {
                    case 0: {
                        long x = 0;
                        for (int shift = 0;; shift += 7) {
                            byte c = b[p++];
                            x |= (long) (c & 0x7F) << shift;
                            if (c >= 0) break;
                        }
                        v = x;
                        break;
                    }
                    case 1: {
                        long x = 0;
                        for (int i = 7; i >= 0; i--) x = (x << 8) | (b[p + i] & 0xFF);
                        p += 8;
                        v = x;
                        break;
                    }
                    case 2: {
                        long n = 0;
                        for (int shift = 0;; shift += 7) {
                            byte c = b[p++];
                            n |= (long) (c & 0x7F) << shift;
                            if (c >= 0) break;
                        }
                        if (n < 0 || p + n > end) throw new IllegalStateException("field " + num + " runs over");
                        v = new Range(b, p, (int) n);
                        p += (int) n;
                        break;
                    }
                    case 5: {
                        long x = (b[p] & 0xFFL) | (b[p + 1] & 0xFFL) << 8 | (b[p + 2] & 0xFFL) << 16
                                | (b[p + 3] & 0xFFL) << 24;
                        p += 4;
                        v = x;
                        break;
                    }
                    default:
                        throw new IllegalStateException("wire type " + wire);
                }
                fields.computeIfAbsent(num, k -> new ArrayList<>()).add(v);
            }
            if (p != end) throw new IllegalStateException("message runs over");
        }

        long num(int n) {
            List<Object> l = fields.get(n);
            return l != null && l.get(0) instanceof Long ? (Long) l.get(0) : 0;
        }

        String str(int n) {
            List<Object> l = fields.get(n);
            return l != null && l.get(0) instanceof Range ? ((Range) l.get(0)).str() : "";
        }

        List<String> strs(int n) {
            List<String> out = new ArrayList<>();
            List<Object> l = fields.get(n);
            if (l != null)
                for (Object o : l)
                    if (o instanceof Range) out.add(((Range) o).str());
            return out;
        }

        Msg msg(int n) {
            List<Object> l = fields.get(n);
            return l != null && l.get(0) instanceof Range ? ((Range) l.get(0)).msg() : null;
        }

        List<Msg> msgs(int n) {
            List<Msg> out = new ArrayList<>();
            List<Object> l = fields.get(n);
            if (l != null)
                for (Object o : l)
                    if (o instanceof Range) out.add(((Range) o).msg());
            return out;
        }
    }

    private static final class Range {
        final byte[] b;
        final int at, length;

        Range(byte[] b, int at, int length) {
            this.b = b;
            this.at = at;
            this.length = length;
        }

        String str() { return new String(b, at, length, StandardCharsets.UTF_8); }

        Msg msg() { return new Msg(b, at, at + length); }
    }
}
