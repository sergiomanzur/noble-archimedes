package com.noblearchimedes;

import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
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
