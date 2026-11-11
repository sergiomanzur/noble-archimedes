package com.noblearchimedes;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.util.Locale;

/**
 * First-run setup. Noble Archimedes is bring-your-own-game: when the app's
 * assets folder holds neither default.xbe nor a disc image, this screen asks
 * for the .iso / .xiso with Android's file picker and copies it into the folder
 * the native game reads. Once a game is present it starts the game directly.
 */
public class SetupActivity extends Activity {
    private static final int REQUEST_PICK = 1;
    // Offsets at which an Xbox disc image's XDVDFS volume descriptor can sit
    // (the descriptor is 0x10000 into the volume): a plain .xiso, and the
    // game partition of the XGD1, XGD2 and XGD3 layouts.
    private static final long[] VOLUME_OFFSETS = {0L, 0x18300000L, 0xFD90000L, 0x2090000L};
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes();

    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView status;
    private TextView detail;
    private ProgressBar progress;
    private Button pick;
    private volatile boolean copying;

    /** True when the native game will find something to run in files/assets. */
    static boolean hasGame(File assets) {
        if (new File(assets, "default.xbe").isFile()) return true;
        File[] files = assets.listFiles();
        if (files == null) return false;
        for (File f : files) {
            if (f.isFile() && f.length() > 1024 * 1024 && f.getName().toLowerCase(Locale.ROOT).endsWith(".iso"))
                return true;
        }
        return false;
    }

    private File assetsDir() {
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        File assets = new File(base, "assets");
        //noinspection ResultOfMethodCallIgnored
        assets.mkdirs();
        return assets;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (hasGame(assetsDir()) && !getIntent().getBooleanExtra("change", false)) {
            startGame();
            return;
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        buildUi();
    }

    private void startGame() {
        Intent intent = new Intent(this, NobleActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
        finish();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.parseColor("#061114"));
        root.setPadding(dp(48), dp(24), dp(48), dp(24));

        TextView title = new TextView(this);
        title.setText("NOBLE ARCHIMEDES");
        title.setTextColor(Color.parseColor("#D8A84E"));
        title.setTextSize(28);
        title.setTypeface(Typeface.SERIF, Typeface.BOLD);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        status = new TextView(this);
        status.setText("Choose your game");
        status.setTextColor(Color.WHITE);
        status.setTextSize(20);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(20), 0, dp(8));
        root.addView(status);

        detail = new TextView(this);
        detail.setText("Select your Castlevania: Curse of Darkness disc image (.iso or .xiso) "
                + "from this device. It is copied once into the app's storage; no game data is included in this app.");
        detail.setTextColor(Color.parseColor("#B8C4C6"));
        detail.setTextSize(15);
        detail.setGravity(Gravity.CENTER);
        detail.setMaxWidth(dp(640));
        root.addView(detail);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(dp(480), dp(10));
        plp.topMargin = dp(20);
        root.addView(progress, plp);

        pick = new Button(this);
        pick.setText("Choose game file");
        pick.setAllCaps(false);
        pick.setTextSize(18);
        pick.setFocusable(true);
        pick.setOnClickListener(v -> openPicker());
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(dp(320), dp(56));
        blp.topMargin = dp(24);
        root.addView(pick, blp);

        setContentView(root);
        pick.requestFocus();
        // Skip the empty-state tap: show the picker straight away on first run.
        ui.postDelayed(() -> { if (!copying && !isFinishing()) openPicker(); }, 400);
    }

    private void openPicker() {
        if (copying) return;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // .xiso has no registered MIME type, so accept every file and check it ourselves.
        intent.setType("*/*");
        try {
            startActivityForResult(intent, REQUEST_PICK);
        } catch (Exception e) {
            fail("No file picker is available on this device (" + e.getMessage() + ").");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            status.setText("No file chosen");
            return;
        }
        copyGame(data.getData());
    }

    private void fail(String message) {
        copying = false;
        progress.setVisibility(View.GONE);
        pick.setEnabled(true);
        pick.setText("Choose game file");
        status.setText("Could not use that file");
        detail.setText(message);
    }

    private static String queryName(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last != null ? last : "game.iso";
    }

    private static long querySize(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) {
        }
        return -1;
    }

    private static String size(long bytes) {
        return String.format(Locale.US, "%.2f GB", bytes / 1073741824.0);
    }

    private void copyGame(Uri uri) {
        final ContentResolver cr = getContentResolver();
        final String name = queryName(cr, uri);
        final long total = querySize(cr, uri);
        final File assets = assetsDir();
        final String lower = name.toLowerCase(Locale.ROOT);
        if (!lower.endsWith(".iso") && !lower.endsWith(".xiso")) {
            fail(name + " is not an .iso or .xiso disc image. Pick your Castlevania: Curse of Darkness disc image.");
            return;
        }
        if (total > 0) {
            long free = new StatFs(assets.getPath()).getAvailableBytes();
            if (free < total + 64L * 1024 * 1024) {
                fail("Not enough free space: the image needs " + size(total) + " and the device has " + size(free) + ".");
                return;
            }
        }
        copying = true;
        pick.setEnabled(false);
        pick.setText("Copying...");
        progress.setProgress(0);
        progress.setVisibility(View.VISIBLE);
        status.setText("Copying " + name);
        detail.setText("This takes a few minutes. Keep the app open.");

        new Thread(() -> {
            // Always stored as .iso: the native loader looks for that extension, and
            // a .xiso is the same XDVDFS image.
            String base = lower.endsWith(".xiso") ? name.substring(0, name.length() - 5) : name.substring(0, name.length() - 4);
            File part = new File(assets, "game.iso.part");
            File dest = new File(assets, base.replaceAll("[\\\\/:*?\"<>|]", "_") + ".iso");
            try (InputStream in = cr.openInputStream(uri);
                 FileOutputStream out = new FileOutputStream(part)) {
                if (in == null) throw new IOException("The file could not be opened.");
                byte[] buf = new byte[1 << 20];
                long done = 0, lastUi = 0;
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    done += n;
                    long now = System.nanoTime();
                    if (now - lastUi > 200_000_000L) {
                        lastUi = now;
                        final long d = done;
                        ui.post(() -> {
                            if (total > 0) progress.setProgress((int) (d * 1000 / total));
                            detail.setText(size(d) + (total > 0 ? " of " + size(total) : "") + " copied");
                        });
                    }
                }
                out.getFD().sync();
            } catch (Exception e) {
                //noinspection ResultOfMethodCallIgnored
                part.delete();
                final String msg = e.getMessage() != null ? e.getMessage() : e.toString();
                ui.post(() -> fail("Copying failed: " + msg));
                return;
            }
            if (!looksLikeXboxDisc(part)) {
                //noinspection ResultOfMethodCallIgnored
                part.delete();
                ui.post(() -> fail(name + " does not look like an Xbox disc image."));
                return;
            }
            //noinspection ResultOfMethodCallIgnored
            dest.delete();
            if (!part.renameTo(dest)) {
                //noinspection ResultOfMethodCallIgnored
                part.delete();
                ui.post(() -> fail("The copied image could not be saved."));
                return;
            }
            ui.post(() -> {
                status.setText("Game ready");
                detail.setText("Starting...");
                startGame();
            });
        }, "noble-copy").start();
    }

    private static boolean looksLikeXboxDisc(File f) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            byte[] got = new byte[MAGIC.length];
            for (long off : VOLUME_OFFSETS) {
                long pos = off + 0x10000L;
                if (pos + got.length > r.length()) continue;
                r.seek(pos);
                r.readFully(got);
                if (java.util.Arrays.equals(got, MAGIC)) return true;
            }
        } catch (IOException ignored) {
        }
        return false;
    }

    @Override
    public void onBackPressed() {
        if (copying) return;  // a half-copied image is useless; let it finish
        super.onBackPressed();
    }
}
