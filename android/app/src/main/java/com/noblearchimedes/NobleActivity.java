package com.noblearchimedes;

import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

/**
 * The Android host: SDL's activity plus Noble Archimedes' splash, touch controls
 * and Android lifecycle glue.
 */
public class NobleActivity extends SDLActivity {
    private SplashView splash;
    private TouchControlsView touchControls;
    private InputManager inputManager;
    private boolean controllerPresent;
    private boolean l3Down, r3Down, l3r3Latch;
    private boolean controllerHintShown;
    private int faceButtons;

    // Safety net: if the game surface still shows no picture a minute after launch, restart the app (twice at most).
    // The cause of black screens on Adreno (a driver bug, see present.cpp) is worked around, so this should never fire;
    // once a picture has been seen the watch ends for good, so play is never restarted.
    private final Handler bootHandler = new Handler(Looper.getMainLooper());
    private static final int kBlackBootRetries = 2;
    private static final long kBootWatchFirstMs = 10000;   // first look: the splash is still up
    private static final long kBootWatchStepMs = 3000;
    private static final long kBootNoPictureMs = 60000;    // a normal boot shows a picture within ~10 s
    private static final long kBootWatchEndMs = 120000;
    private static final int kVisibleMean = 4;            // 0..255 mean over the sampled surface
    private long bootStart;
    private int visibleSamples;
    private boolean bootWatchDone;

    private final Runnable bootWatch = new Runnable() {
        @Override public void run() {
            sampleBootScreen();
        }
    };

    private void sampleBootScreen() {
        if (bootWatchDone) return;
        long elapsed = SystemClock.uptimeMillis() - bootStart;
        if (elapsed > kBootWatchEndMs) {
            bootWatchDone = true;
            return;
        }
        if (mSurface == null || !mSurface.isAttachedToWindow() || !mSurface.getHolder().getSurface().isValid()) {
            bootHandler.postDelayed(bootWatch, kBootWatchStepMs);
            return;
        }
        final android.graphics.Bitmap bmp = android.graphics.Bitmap.createBitmap(
                64, 36, android.graphics.Bitmap.Config.ARGB_8888);
        try {
            android.view.PixelCopy.request(mSurface, bmp, result -> {
                if (bootWatchDone) return;
                if (result == android.view.PixelCopy.SUCCESS) {
                    int[] px = new int[bmp.getWidth() * bmp.getHeight()];
                    bmp.getPixels(px, 0, bmp.getWidth(), 0, 0, bmp.getWidth(), bmp.getHeight());
                    long sum = 0;
                    for (int c : px) sum += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
                    int mean = (int) (sum / (px.length * 3L));
                    android.util.Log.i("NobleBoot", "boot screen mean " + mean);
                    if (mean >= kVisibleMean && ++visibleSamples >= 2) {
                        bootWatchDone = true;
                        resetBlackBootCount();
                        return;
                    }
                    long now = SystemClock.uptimeMillis() - bootStart;
                    if (visibleSamples == 0 && now >= kBootNoPictureMs) {
                        restartForBlackBoot();
                        return;
                    }
                }
                bootHandler.postDelayed(bootWatch, kBootWatchStepMs);
            }, bootHandler);
        } catch (Throwable t) {
            android.util.Log.w("NobleBoot", "PixelCopy failed", t);
            bootWatchDone = true;
        }
    }

    private void restartForBlackBoot() {
        android.content.SharedPreferences prefs =
                getSharedPreferences("noble_android", MODE_PRIVATE);
        int attempts = prefs.getInt("black_boot_attempts", 0);
        bootWatchDone = true;
        if (attempts >= kBlackBootRetries) {
            android.util.Log.w("NobleBoot",
                    "no picture after " + attempts + " restarts; leaving it");
            return;
        }
        prefs.edit().putInt("black_boot_attempts", attempts + 1).commit();
        android.util.Log.i("NobleBoot", "no picture after boot; restarting (" + (attempts + 1) + ")");
        android.content.Intent intent = new android.content.Intent(this, SetupActivity.class);
        intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK
                | android.content.Intent.FLAG_ACTIVITY_CLEAR_TASK);
        android.app.PendingIntent pi = android.app.PendingIntent.getActivity(this, 42, intent,
                android.app.PendingIntent.FLAG_IMMUTABLE
                        | android.app.PendingIntent.FLAG_UPDATE_CURRENT);
        android.app.AlarmManager am =
                (android.app.AlarmManager) getSystemService(ALARM_SERVICE);
        if (am != null) {
            am.set(android.app.AlarmManager.ELAPSED_REALTIME,
                    SystemClock.elapsedRealtime() + 800, pi);
        }
        finishAffinity();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private void resetBlackBootCount() {
        getSharedPreferences("noble_android", MODE_PRIVATE).edit()
                .putInt("black_boot_attempts", 0).apply();
    }

    private final InputManager.InputDeviceListener inputDeviceListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int deviceId) { updateControllerPresence(); }
        @Override public void onInputDeviceRemoved(int deviceId) { updateControllerPresence(); }
        @Override public void onInputDeviceChanged(int deviceId) { updateControllerPresence(); }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        enterImmersiveMode();

        // SDLActivity puts its SurfaceView in this RelativeLayout. The splash
        // covers the black start-up, and the touch controls sit above the game.
        if (mLayout != null) {
            touchControls = new TouchControlsView(this);
            mLayout.addView(touchControls, new android.widget.RelativeLayout.LayoutParams(
                    android.widget.RelativeLayout.LayoutParams.MATCH_PARENT,
                    android.widget.RelativeLayout.LayoutParams.MATCH_PARENT));

            // Last, so the branded splash covers the touch bubble while the game boots.
            splash = new SplashView(this);
            mLayout.addView(splash, new android.widget.RelativeLayout.LayoutParams(
                    android.widget.RelativeLayout.LayoutParams.MATCH_PARENT,
                    android.widget.RelativeLayout.LayoutParams.MATCH_PARENT));
        }

        controllerPresent = TouchControlsView.hasPhysicalGamepad();
        inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(inputDeviceListener, null);

        bootStart = SystemClock.uptimeMillis();
        bootHandler.postDelayed(bootWatch, kBootWatchFirstMs);
    }

    @Override
    protected void onResume() {
        super.onResume();
        enterImmersiveMode();
        updateControllerPresence();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int key = event.getKeyCode();
        if (event.getAction() == KeyEvent.ACTION_DOWN || event.getAction() == KeyEvent.ACTION_UP) {
            int mask = 0;
            if (key == KeyEvent.KEYCODE_BUTTON_A) mask = 0x1000;
            else if (key == KeyEvent.KEYCODE_BUTTON_B) mask = 0x2000;
            else if (key == KeyEvent.KEYCODE_BUTTON_X) mask = 0x4000;
            else if (key == KeyEvent.KEYCODE_BUTTON_Y) mask = 0x8000;
            if (mask != 0) {
                boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
                faceButtons = down ? (faceButtons | mask) : (faceButtons & ~mask);
                if (touchControls != null) touchControls.setFaceButtons(faceButtons);
            }
        }
        if (key == KeyEvent.KEYCODE_BUTTON_THUMBL || key == KeyEvent.KEYCODE_BUTTON_THUMBR) {
            boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
            if (key == KeyEvent.KEYCODE_BUTTON_THUMBL) l3Down = down;
            else r3Down = down;
            // L3 + R3: show or hide the touch pad with a physical controller. The
            // events still go to SDL so the pad's own L3/R3 stay usable.
            if (down && l3Down && r3Down && !l3r3Latch) {
                l3r3Latch = true;
                if (touchControls != null) touchControls.toggleControlsForController();
            }
            if (!down) l3r3Latch = false;
        }
        return super.dispatchKeyEvent(event);
    }

    private void updateControllerPresence() {
        boolean present = TouchControlsView.hasPhysicalGamepad();
        if (present == controllerPresent) return;
        boolean wasVisible = touchControls != null && touchControls.isControlsVisible();
        controllerPresent = present;
        if (touchControls != null) touchControls.setControllerPresent(present);
        if (present && wasVisible && !controllerHintShown) {
            controllerHintShown = true;
            Toast.makeText(this, "Touch controls hidden: press L3 + R3 to show them",
                    Toast.LENGTH_LONG).show();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) enterImmersiveMode();
    }

    private void enterImmersiveMode() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
        View decor = window.getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        // Back never kills a running session: it opens/closes the game's
        // settings menu, as the controller's menu combo does.
        if (nativeMenuOpen()) {
            nativeMenuToggle();
            if (touchControls != null) touchControls.onMenuClosed();
        } else {
            nativeMenuToggle();
            if (touchControls != null) touchControls.onMenuOpened();
        }
    }

    @Override
    protected void onPause() {
        if (touchControls != null) touchControls.releaseAll();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (inputManager != null) inputManager.unregisterInputDeviceListener(inputDeviceListener);
        bootHandler.removeCallbacksAndMessages(null);
        if (splash != null) splash.stop();
        super.onDestroy();
    }

    public void showTouchControls(boolean visible) {
        if (touchControls != null) touchControls.setControlsVisible(visible, true);
    }

    // Implemented in android_native.cpp.
    public static native void nativeSetTouchPad(
            int buttons,
            int a, int b, int x, int y,
            int black, int white,
            int lt, int rt,
            int lx, int ly, int rx, int ry,
            boolean active);

    public static native void nativeMenuToggle();
    public static native boolean nativeMenuOpen();
    public static native boolean nativeIsReady();
    public static native void nativeTouchVisibilityChanged(boolean visible);
}
