package com.noblearchimedes;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Checks GitHub Releases on launch, downloads and SHA-256-verifies any newer
 * Android APK in the background, and prompts the user to install it.
 */
public final class UpdateChecker {
    private static final String TAG = "NobleUpdater";
    public static final String GITHUB_RELEASES_API =
            "https://api.github.com/repos/sergiomanzur/noble-archimedes/releases?per_page=10";
    private static final String USER_AGENT = "NobleArchimedes-Android";
    private static final String APK_MIME = "application/vnd.android.package-archive";
    private static final int CONNECT_TIMEOUT_MS = 15000;
    private static final int READ_TIMEOUT_MS = 30000;

    public static final class ReleaseInfo {
        public final String version;
        public final String publishedAt;
        public final String assetName;
        public final String downloadUrl;
        public final long size;
        public final String sha256;

        public ReleaseInfo(String version, String publishedAt, String assetName,
                           String downloadUrl, long size, String sha256) {
            this.version = version;
            this.publishedAt = publishedAt;
            this.assetName = assetName;
            this.downloadUrl = downloadUrl;
            this.size = size;
            this.sha256 = sha256;
        }
    }

    private static final AtomicBoolean started = new AtomicBoolean(false);
    private static volatile Handler mainHandler = null;

    private static void postMain(Runnable r) {
        Handler h = mainHandler;
        if (h == null) {
            h = new Handler(Looper.getMainLooper());
            mainHandler = h;
        }
        h.post(r);
    }

    private static WeakReference<Activity> currentActivity = new WeakReference<>(null);
    private static boolean currentBusy = false;
    private static volatile ReleaseInfo readyRelease = null;
    private static volatile File readyApk = null;
    private static boolean promptDismissed = false;
    private static boolean dialogShowing = false;
    private static boolean waitingForInstallPermission = false;

    private UpdateChecker() {}

    public static File updateApkFile(Context context) {
        File base = context.getExternalFilesDir("updates");
        if (base == null) {
            base = new File(context.getFilesDir(), "updates");
        }
        //noinspection ResultOfMethodCallIgnored
        base.mkdirs();
        return new File(base, "noble-update.apk");
    }

    public static void startIfNeeded(Activity activity) {
        if (!started.compareAndSet(false, true)) return;
        final Context appCtx = activity.getApplicationContext();
        final String ownVersion = ownVersionName(appCtx);
        if (ownVersion.isEmpty()) return;

        new Thread(() -> runCheckAndDownload(appCtx, ownVersion, GITHUB_RELEASES_API), "noble-updater").start();
    }

    public static void attach(Activity activity, boolean busy) {
        currentActivity = new WeakReference<>(activity);
        currentBusy = busy;
        if (waitingForInstallPermission && readyApk != null && readyApk.isFile()) {
            waitingForInstallPermission = false;
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O
                    || activity.getPackageManager().canRequestPackageInstalls()) {
                launchApkInstaller(activity);
                return;
            }
        }
        maybePromptOnMainThread();
    }

    public static void detach(Activity activity) {
        Activity cur = currentActivity.get();
        if (cur == activity) {
            currentActivity = new WeakReference<>(null);
        }
    }

    private static String ownVersionName(Context context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return info.versionName != null ? info.versionName.trim() : "";
        } catch (Exception e) {
            return "";
        }
    }

    private static void runCheckAndDownload(Context appCtx, String ownVersion, String apiUrl) {
        try {
            String json = fetchText(apiUrl);
            if (json == null || json.isEmpty()) return;
            ReleaseInfo latest = parseNewestApkRelease(json);
            if (latest == null) return;
            File dest = updateApkFile(appCtx);
            if (compareVersions(latest.version, ownVersion) <= 0) {
                // Already up to date: remove any previously downloaded update APK.
                if (dest.isFile()) {
                    //noinspection ResultOfMethodCallIgnored
                    dest.delete();
                }
                return;
            }
            Log.i(TAG, "Update available: " + latest.version + " (current " + ownVersion + ")");
            postMain(() -> {
                Activity a = currentActivity.get();
                if (a != null && !a.isFinishing() && !a.isDestroyed()) {
                    Toast.makeText(a, "Downloading Noble Archimedes v" + latest.version + " update...",
                            Toast.LENGTH_SHORT).show();
                }
            });
            File downloaded = downloadAndVerify(latest.downloadUrl, latest.size, latest.sha256, dest);
            if (downloaded == null) {
                Log.w(TAG, "Failed to download or verify update APK for " + latest.version);
                return;
            }
            Log.i(TAG, "Verified update APK for v" + latest.version + " at " + downloaded);
            readyRelease = latest;
            readyApk = downloaded;
            postMain(UpdateChecker::maybePromptOnMainThread);
        } catch (Throwable t) {
            Log.w(TAG, "Update check failed: " + t.getMessage());
        }
    }

    private static void maybePromptOnMainThread() {
        if (promptDismissed || dialogShowing || currentBusy) return;
        final ReleaseInfo rel = readyRelease;
        final File apk = readyApk;
        if (rel == null || apk == null || !apk.isFile()) return;
        final Activity activity = currentActivity.get();
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;

        dialogShowing = true;
        new AlertDialog.Builder(activity)
                .setTitle("Update Available (v" + rel.version + ")")
                .setMessage("Noble Archimedes v" + rel.version + " has been downloaded.\n\n"
                        + "Please install the update to get the latest fixes and improvements. "
                        + "Your game files and saves will be kept.")
                .setPositiveButton("Install Update", (d, which) -> {
                    dialogShowing = false;
                    requestInstall(activity);
                })
                .setNegativeButton("Later", (d, which) -> {
                    dialogShowing = false;
                    promptDismissed = true;
                })
                .setOnCancelListener(d -> {
                    dialogShowing = false;
                    promptDismissed = true;
                })
                .show();
    }

    private static void requestInstall(Activity activity) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                && !activity.getPackageManager().canRequestPackageInstalls()) {
            new AlertDialog.Builder(activity)
                    .setTitle("Allow App Updates")
                    .setMessage("Android requires permission for Noble Archimedes to install downloaded updates. "
                            + "Enable \"Allow from this source\" on the next screen, then return to the app.")
                    .setPositiveButton("Open Settings", (d, which) -> {
                        waitingForInstallPermission = true;
                        Intent settings = new Intent(
                                Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                Uri.parse("package:" + activity.getPackageName()));
                        try {
                            activity.startActivity(settings);
                        } catch (Exception e) {
                            waitingForInstallPermission = false;
                            Toast.makeText(activity, "Could not open install settings.", Toast.LENGTH_LONG).show();
                        }
                    })
                    .setNegativeButton("Cancel", null)
                    .show();
            return;
        }
        launchApkInstaller(activity);
    }

    private static void launchApkInstaller(Activity activity) {
        try {
            Intent install = new Intent(Intent.ACTION_VIEW);
            install.setDataAndType(UpdateApkProvider.uriForUpdate(), APK_MIME);
            install.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
            activity.startActivity(install);
        } catch (Exception e) {
            Log.e(TAG, "Failed to launch APK installer", e);
            Toast.makeText(activity, "Could not open installer: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    /** Compares version strings like "0.9.7d", "1.0", "1.0.0", "1.0.1", "1.10.0". */
    public static int compareVersions(String a, String b) {
        List<Integer> numsA = new ArrayList<>();
        List<Integer> numsB = new ArrayList<>();
        String sufA = parseVersion(a, numsA);
        String sufB = parseVersion(b, numsB);
        if (numsA.isEmpty() || numsB.isEmpty()) {
            return String.valueOf(a).compareTo(String.valueOf(b));
        }
        int n = Math.max(numsA.size(), numsB.size());
        for (int i = 0; i < n; i++) {
            int va = i < numsA.size() ? numsA.get(i) : 0;
            int vb = i < numsB.size() ? numsB.get(i) : 0;
            if (va != vb) return Integer.compare(va, vb);
        }
        if (sufA.equals(sufB)) return 0;
        if (sufA.isEmpty()) return 1;   // final release > suffixed/pre-release
        if (sufB.isEmpty()) return -1;
        return sufA.compareTo(sufB);
    }

    private static String parseVersion(String raw, List<Integer> nums) {
        if (raw == null) return "";
        String s = raw.trim();
        if (s.startsWith("v") || s.startsWith("V")) s = s.substring(1);
        int i = 0;
        while (i < s.length() && Character.isDigit(s.charAt(i))) {
            int val = 0;
            while (i < s.length() && Character.isDigit(s.charAt(i))) {
                if (val < 1_000_000) val = val * 10 + (s.charAt(i) - '0');
                i++;
            }
            nums.add(val);
            if (i < s.length() && s.charAt(i) == '.') {
                i++;
            } else {
                break;
            }
        }
        return s.substring(i).toLowerCase(Locale.ROOT);
    }

    public static boolean isAndroidApkAsset(String name) {
        if (name == null) return false;
        String low = name.toLowerCase(Locale.ROOT);
        return low.startsWith("noble-archimedes-") && low.endsWith(".apk");
    }

    /** Picks the newest non-draft, non-prerelease GitHub release with an Android APK asset. */
    public static ReleaseInfo parseNewestApkRelease(String jsonText) {
        try {
            JSONArray releases = new JSONArray(jsonText);
            ReleaseInfo best = null;
            for (int i = 0; i < releases.length(); i++) {
                JSONObject r = releases.optJSONObject(i);
                if (r == null) continue;
                if (r.optBoolean("draft", false) || r.optBoolean("prerelease", false)) continue;
                String tag = r.optString("tag_name", "").trim();
                if (tag.startsWith("v") || tag.startsWith("V")) tag = tag.substring(1);
                if (tag.isEmpty()) continue;
                String published = r.optString("published_at", "");
                JSONArray assets = r.optJSONArray("assets");
                if (assets == null) continue;
                ReleaseInfo candidate = null;
                for (int j = 0; j < assets.length(); j++) {
                    JSONObject a = assets.optJSONObject(j);
                    if (a == null) continue;
                    String name = a.optString("name", "");
                    if (!isAndroidApkAsset(name)) continue;
                    String url = a.optString("browser_download_url", "");
                    if (url.isEmpty()) continue;
                    long size = a.optLong("size", 0L);
                    String digest = a.optString("digest", "");
                    String sha = "";
                    if (digest.startsWith("sha256:")) {
                        sha = digest.substring(7).toLowerCase(Locale.ROOT);
                    }
                    candidate = new ReleaseInfo(tag, published, name, url, size, sha);
                    if (name.toLowerCase(Locale.ROOT).contains("arm64")) break;
                }
                if (candidate == null || candidate.sha256.isEmpty()) continue;
                if (best == null || candidate.publishedAt.compareTo(best.publishedAt) > 0) {
                    best = candidate;
                }
            }
            return best;
        } catch (Exception e) {
            return null;
        }
    }

    public static String sha256File(File file) throws IOException {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[65536];
            try (InputStream in = new BufferedInputStream(new FileInputStream(file))) {
                int n;
                while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            }
            byte[] digest = md.digest();
            StringBuilder hex = new StringBuilder(digest.length * 2);
            for (byte b : digest) {
                hex.append(String.format(Locale.US, "%02x", b & 0xff));
            }
            return hex.toString();
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException("SHA-256 unavailable", e);
        }
    }

    private static HttpURLConnection openFollowingRedirects(String urlStr, String accept) throws IOException {
        String cur = urlStr;
        for (int hops = 0; hops < 6; hops++) {
            HttpURLConnection conn = (HttpURLConnection) new URL(cur).openConnection();
            conn.setInstanceFollowRedirects(false);
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(READ_TIMEOUT_MS);
            conn.setRequestProperty("User-Agent", USER_AGENT);
            if (accept != null) conn.setRequestProperty("Accept", accept);
            int code = conn.getResponseCode();
            if (code == HttpURLConnection.HTTP_MOVED_PERM
                    || code == HttpURLConnection.HTTP_MOVED_TEMP
                    || code == HttpURLConnection.HTTP_SEE_OTHER
                    || code == 307 || code == 308) {
                String loc = conn.getHeaderField("Location");
                conn.disconnect();
                if (loc == null || loc.isEmpty()) throw new IOException("Redirect without Location");
                cur = new URL(new URL(cur), loc).toString();
                continue;
            }
            return conn;
        }
        throw new IOException("Too many redirects");
    }

    public static String fetchText(String urlStr) throws IOException {
        HttpURLConnection conn = openFollowingRedirects(urlStr, "application/vnd.github+json");
        try {
            if (conn.getResponseCode() != HttpURLConnection.HTTP_OK) return null;
            try (InputStream in = conn.getInputStream();
                 ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                return out.toString(StandardCharsets.UTF_8.name());
            }
        } finally {
            conn.disconnect();
        }
    }

    public static File downloadAndVerify(String urlStr, long expectedSize, String expectedSha256, File destFile)
            throws IOException {
        if (expectedSha256 == null || expectedSha256.isEmpty()) return null;
        if (destFile.isFile()
                && (expectedSize <= 0 || destFile.length() == expectedSize)
                && expectedSha256.equalsIgnoreCase(sha256File(destFile))) {
            return destFile;
        }
        File parent = destFile.getParentFile();
        if (parent != null) {
            //noinspection ResultOfMethodCallIgnored
            parent.mkdirs();
        }
        File part = new File(destFile.getPath() + ".part");
        //noinspection ResultOfMethodCallIgnored
        part.delete();
        HttpURLConnection conn = openFollowingRedirects(urlStr, null);
        try {
            if (conn.getResponseCode() != HttpURLConnection.HTTP_OK) return null;
            try (InputStream in = new BufferedInputStream(conn.getInputStream());
                 FileOutputStream out = new FileOutputStream(part)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                out.getFD().sync();
            }
        } finally {
            conn.disconnect();
        }
        if ((expectedSize > 0 && part.length() != expectedSize)
                || !expectedSha256.equalsIgnoreCase(sha256File(part))) {
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            return null;
        }
        //noinspection ResultOfMethodCallIgnored
        destFile.delete();
        if (!part.renameTo(destFile)) {
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            return null;
        }
        return destFile;
    }
}
