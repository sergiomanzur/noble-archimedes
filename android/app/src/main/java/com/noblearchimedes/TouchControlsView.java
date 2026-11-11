package com.noblearchimedes;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.SparseArray;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

/**
 * Touch controls laid directly over SDL's SurfaceView. It draws a complete
 * Xbox-style pad (two sticks, d-pad, A/B/X/Y, shoulders, triggers, start/back,
 * L3/R3 and a settings button) and a small always-available show/hide bubble.
 *
 * Touch is consumed only while the pad is visible. When it is hidden the view
 * lets events fall through to SDL so the game's own ImGui settings menu can be
 * used by touch. The pad automatically steps aside while that menu is open.
 */
public class TouchControlsView extends View {
    private static final int PAD_UP = 0x01, PAD_DOWN = 0x02, PAD_LEFT = 0x04, PAD_RIGHT = 0x08;
    private static final int PAD_START = 0x10, PAD_BACK = 0x20, PAD_L3 = 0x40, PAD_R3 = 0x80;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint small = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bubble = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final RectF bubbleRect = new RectF();
    private final RectF ltRect = new RectF(), lbRect = new RectF(), rbRect = new RectF(), rtRect = new RectF();
    private final RectF backRect = new RectF(), menuRect = new RectF(), startRect = new RectF();
    private final RectF[] dpad = {new RectF(), new RectF(), new RectF(), new RectF()};
    private final float[] ax = new float[4], ay = new float[4];

    private float leftStickX, leftStickY, leftStickR;
    private float rightStickX, rightStickY, rightStickR;
    private float abxyX, abxyY, abxyR, abxyButtonR;
    private float dpadX, dpadY, dpadButton;
    private float l3X, l3Y, l3R, r3X, r3Y, r3R;
    private float density;

    private boolean controlsVisible;
    private boolean menuOpen;
    private boolean menuAutoHidden;
    private boolean visibleBeforeMenu;
    private boolean bubbleVisible = true;
    private boolean controllerPresent;
    private boolean autoHiddenForController;
    private long bubbleDownAt;
    private boolean bubblePressed;

    private final SparseArray<Active> active = new SparseArray<>();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final SharedPreferences prefs;

    private int buttons;
    private int[] analog = new int[8];
    private int lx, ly, rx, ry;
    private int extraFaceButtons;

    private static final class Active {
        int control;   // 0 none, 1..N buttons, 100 left stick, 101 right stick, 200 bubble
        long downAt;
    }

    private static final int C_A = 1, C_B = 2, C_X = 3, C_Y = 4;
    private static final int C_LT = 5, C_LB = 6, C_RB = 7, C_RT = 8;
    private static final int C_BACK = 9, C_MENU = 10, C_START = 11;
    private static final int C_DUP = 12, C_DDOWN = 13, C_DLEFT = 14, C_DRIGHT = 15;
    private static final int C_L3 = 16, C_R3 = 17;
    private static final int C_LSTICK = 100, C_RSTICK = 101, C_BUBBLE = 200;

    // The native menu can open from Start + Back, the system Back key or this view's MENU button. Poll it while the
    // game is up so the pad steps aside whichever way it was opened, and comes back when it closes.
    private final Runnable pollMenu = new Runnable() {
        @Override public void run() {
            boolean open = false;
            try {
                open = NobleActivity.nativeMenuOpen();
            } catch (UnsatisfiedLinkError ignored) {
            }
            if (open && !menuOpen) onMenuOpened();
            else if (!open && menuOpen) onMenuClosed();
            handler.postDelayed(this, 250);
        }
    };

    public TouchControlsView(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        prefs = context.getSharedPreferences("noble_android", Context.MODE_PRIVATE);

        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(2.0f * density);
        label.setTextAlign(Paint.Align.CENTER);
        label.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        small.setTextAlign(Paint.Align.CENTER);
        small.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        bubble.setStyle(Paint.Style.FILL);

        controllerPresent = hasPhysicalGamepad();
        // A physical pad takes over automatically: the touch pad and its show/hide
        // bubble stay out of the picture. L3 + R3 brings the touch pad back.
        controlsVisible = !controllerPresent && prefs.getBoolean("touch_visible", true);
        bubbleVisible = !controllerPresent;
        autoHiddenForController = controllerPresent;
        setWillNotDraw(false);
        setClickable(false);
        setFocusable(false);
        setLayerType(View.LAYER_TYPE_HARDWARE, null);
    }

    /** Whether Android currently reports a gamepad or joystick. */
    public static boolean hasPhysicalGamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null) continue;
            int src = d.getSources();
            if ((src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                return true;
            }
        }
        return false;
    }

    /** A pad was connected or removed: hide the touch pad for a pad, restore it without one. */
    public void setControllerPresent(boolean present) {
        if (present == controllerPresent) return;
        controllerPresent = present;
        if (present) {
            if (controlsVisible) autoHiddenForController = true;
            controlsVisible = false;
            bubbleVisible = false;
            releaseAll();
        } else {
            bubbleVisible = true;
            if (autoHiddenForController) {
                controlsVisible = true;
                autoHiddenForController = false;
            }
        }
        try {
            NobleActivity.nativeTouchVisibilityChanged(controlsVisible);
        } catch (UnsatisfiedLinkError ignored) {
        }
        invalidate();
    }

    /** L3 + R3 on a physical controller: show or hide the touch pad without the bubble. */
    public void toggleControlsForController() {
        controlsVisible = !controlsVisible;
        autoHiddenForController = false;
        if (!controlsVisible) releaseAll();
        prefs.edit().putBoolean("touch_visible", controlsVisible).apply();
        try {
            NobleActivity.nativeTouchVisibilityChanged(controlsVisible);
        } catch (UnsatisfiedLinkError ignored) {
        }
        haptic();
        invalidate();
    }

    public boolean isControlsVisible() {
        return controlsVisible;
    }

    public void setControlsVisible(boolean visible, boolean persist) {
        controlsVisible = visible;
        if (!visible) releaseAll();
        if (persist) prefs.edit().putBoolean("touch_visible", visible).apply();
        invalidate();
    }

    public void onMenuOpened() {
        if (menuOpen) return;
        menuOpen = true;
        menuAutoHidden = true;
        visibleBeforeMenu = controlsVisible;
        releaseAll();
        invalidate();
    }

    public void onMenuClosed() {
        if (!menuOpen) return;
        menuOpen = false;
        if (menuAutoHidden) controlsVisible = visibleBeforeMenu;
        invalidate();
    }

    public void releaseAll() {
        active.clear();
        buttons = 0;
        extraFaceButtons = 0;
        for (int i = 0; i < analog.length; i++) analog[i] = 0;
        lx = ly = rx = ry = 0;
        send();
        invalidate();
    }

    /** A/B/X/Y as XInput's digital masks, merged with the pad's analog buttons. */
    public void setFaceButtons(int mask) {
        if (mask == extraFaceButtons) return;
        extraFaceButtons = mask;
        send();
        invalidate();
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        send();
        handler.removeCallbacks(pollMenu);
        handler.postDelayed(pollMenu, 250);
    }

    @Override protected void onDetachedFromWindow() {
        handler.removeCallbacksAndMessages(null);
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        float min = Math.min(w, h);
        float m = 10f * density;

        // The bubble: left edge, above the movement stick, clear of the game's top-left HUD.
        float bs = Math.max(42f * density, min * 0.052f);
        float bubbleY = h * 0.44f;
        bubbleRect.set(m, bubbleY, m + bs * 1.95f, bubbleY + bs);

        // Shoulder and system buttons along the top.
        float top = m + bs + 8f * density;
        float bw = Math.max(54f * density, min * 0.070f);
        float bh = Math.max(30f * density, min * 0.040f);
        ltRect.set(m + 4f * density, top, m + 4f * density + bw, top + bh);
        lbRect.set(ltRect.right + 6f * density, top, ltRect.right + 6f * density + bw, top + bh);
        float rEdge = w - m - 4f * density;
        rtRect.set(rEdge - bw, top, rEdge, top + bh);
        rbRect.set(rtRect.left - 6f * density - bw, top, rtRect.left - 6f * density, top + bh);

        float cw = Math.max(48f * density, min * 0.058f);
        float cy = top + bh * 0.5f;
        backRect.set(w * 0.5f - cw * 2.05f, cy - bh * 0.5f, w * 0.5f - cw * 1.05f, cy + bh * 0.5f);
        menuRect.set(w * 0.5f - cw * 0.5f, cy - bh * 0.5f, w * 0.5f + cw * 0.5f, cy + bh * 0.5f);
        startRect.set(w * 0.5f + cw * 1.05f, cy - bh * 0.5f, w * 0.5f + cw * 2.05f, cy + bh * 0.5f);

        // Sticks.
        leftStickX = w * 0.145f;
        leftStickY = h * 0.665f;
        leftStickR = min * 0.105f;
        rightStickX = w * 0.585f;
        rightStickY = h * 0.685f;
        rightStickR = min * 0.100f;

        // D-pad.
        dpadX = w * 0.325f;
        dpadY = h * 0.800f;
        dpadButton = min * 0.074f;
        float g = dpadButton * 0.14f;
        float half = dpadButton * 0.5f;
        dpad[0].set(dpadX - half, dpadY - half - dpadButton - g, dpadX + half, dpadY - half - g);
        dpad[1].set(dpadX - half, dpadY + half + g, dpadX + half, dpadY + half + dpadButton + g);
        dpad[2].set(dpadX - half - dpadButton - g, dpadY - half, dpadX - half - g, dpadY + half);
        dpad[3].set(dpadX + half + g, dpadY - half, dpadX + half + dpadButton + g, dpadY + half);

        // A/B/X/Y diamond.
        abxyX = w * 0.835f;
        abxyY = h * 0.705f;
        abxyR = min * 0.130f;
        abxyButtonR = min * 0.074f;
        ax[0] = abxyX;                 ay[0] = abxyY + abxyR;  // A south
        ax[1] = abxyX + abxyR;         ay[1] = abxyY;          // B east
        ax[2] = abxyX - abxyR;         ay[2] = abxyY;          // X west
        ax[3] = abxyX;                 ay[3] = abxyY - abxyR;  // Y north

        // L3 / R3: small click buttons sitting above their sticks.
        l3R = r3R = min * 0.036f;
        l3X = leftStickX;   l3Y = leftStickY - leftStickR - l3R * 1.8f;
        r3X = rightStickX;  r3Y = rightStickY - rightStickR - r3R * 1.8f;

        // The stick knobs restart at centre when the layout changes.
        lx = ly = rx = ry = 0;
        send();
    }

    @Override protected void onDraw(Canvas c) {
        if (menuOpen) return;

        drawBubble(c);
        if (!controlsVisible) return;

        // sticks
        drawStick(c, leftStickX, leftStickY, leftStickR, lx, ly, false);
        drawStick(c, rightStickX, rightStickY, rightStickR, rx, ry, true);
        drawRoundButton(c, l3X, l3Y, l3R, "L3", 0, isHeld(C_L3));
        drawRoundButton(c, r3X, r3Y, r3R, "R3", 0, isHeld(C_R3));

        // d-pad
        drawDpad(c);

        // shoulders and triggers
        drawRectButton(c, ltRect, "LT", isHeld(C_LT), 0xFF7FB3C8);
        drawRectButton(c, lbRect, "LB", isHeld(C_LB), 0xFFB7C6CC);
        drawRectButton(c, rbRect, "RB", isHeld(C_RB), 0xFFB7C6CC);
        drawRectButton(c, rtRect, "RT", isHeld(C_RT), 0xFF7FB3C8);

        // start / back / settings
        drawRectButton(c, backRect, "BACK", isHeld(C_BACK), 0xFFB7C6CC);
        drawRectButton(c, menuRect, "MENU", isHeld(C_MENU), 0xFFE3BE74);
        drawRectButton(c, startRect, "START", isHeld(C_START), 0xFFB7C6CC);

        // face buttons
        drawFaceButton(c, ax[0], ay[0], abxyButtonR, "A", 0xFF69B96C, isHeld(C_A));
        drawFaceButton(c, ax[1], ay[1], abxyButtonR, "B", 0xFFD65C52, isHeld(C_B));
        drawFaceButton(c, ax[2], ay[2], abxyButtonR, "X", 0xFF5C93D6, isHeld(C_X));
        drawFaceButton(c, ax[3], ay[3], abxyButtonR, "Y", 0xFFD9B44E, isHeld(C_Y));
    }

    private void drawBubble(Canvas c) {
        if (!bubbleVisible) return;
        float r = bubbleRect.height() * 0.5f;
        bubble.setColor(bubblePressed ? 0xB8D8A84E : 0x8C0A2025);
        c.drawRoundRect(bubbleRect, r, r, bubble);
        stroke.setColor(0xB0D8A84E);
        stroke.setStrokeWidth(1.6f * density);
        c.drawRoundRect(bubbleRect, r, r, stroke);

        // a tiny controller glyph
        float cx = bubbleRect.left + r;
        float cy = bubbleRect.centerY();
        fill.setColor(0xFFE3BE74);
        RectF body = new RectF(cx - r * 0.46f, cy - r * 0.24f, cx + r * 0.46f, cy + r * 0.30f);
        c.drawRoundRect(body, r * 0.22f, r * 0.22f, fill);
        fill.setColor(0xFF0A2025);
        c.drawCircle(cx - r * 0.20f, cy + r * 0.02f, r * 0.075f, fill);
        c.drawCircle(cx + r * 0.20f, cy + r * 0.02f, r * 0.075f, fill);

        label.setColor(0xFFE3BE74);
        label.setTextSize(r * 0.44f);
        c.drawText(controlsVisible ? "HIDE" : "TOUCH",
                bubbleRect.left + r * 2.05f, cy + r * 0.16f, label);
    }

    private void drawStick(Canvas c, float cx, float cy, float r, int dx, int dy, boolean right) {
        fill.setColor(0x3D0A2A30);
        c.drawCircle(cx, cy, r, fill);
        stroke.setColor(0x99D8A84E);
        stroke.setStrokeWidth(1.8f * density);
        c.drawCircle(cx, cy, r, stroke);

        // faint crosshair
        stroke.setColor(0x33D8A84E);
        stroke.setStrokeWidth(1.0f * density);
        c.drawLine(cx - r * 0.7f, cy, cx + r * 0.7f, cy, stroke);
        c.drawLine(cx, cy - r * 0.7f, cx, cy + r * 0.7f, stroke);

        float kx = cx + (dx / 32767f) * r * 0.58f;
        float ky = cy - (dy / 32767f) * r * 0.58f;
        fill.setColor(0x8CE3BE74);
        c.drawCircle(kx, ky, r * 0.36f, fill);
        stroke.setColor(0xCC071417);
        stroke.setStrokeWidth(1.4f * density);
        c.drawCircle(kx, ky, r * 0.36f, stroke);

        small.setColor(0x99E3BE74);
        small.setTextSize(r * 0.20f);
        c.drawText(right ? "CAMERA" : "MOVE", cx, cy + r * 1.20f, small);
    }

    private void drawDpad(Canvas c) {
        drawDpadCell(c, dpad[0], "▲", isHeld(C_DUP));
        drawDpadCell(c, dpad[1], "▼", isHeld(C_DDOWN));
        drawDpadCell(c, dpad[2], "◀", isHeld(C_DLEFT));
        drawDpadCell(c, dpad[3], "▶", isHeld(C_DRIGHT));
    }

    private void drawDpadCell(Canvas c, RectF r, String glyph, boolean held) {
        fill.setColor(held ? 0x99D8A84E : 0x4D0A2A30);
        c.drawRoundRect(r, dpadButton * 0.28f, dpadButton * 0.28f, fill);
        stroke.setColor(held ? 0xFFE3BE74 : 0x99D8A84E);
        stroke.setStrokeWidth(1.5f * density);
        c.drawRoundRect(r, dpadButton * 0.28f, dpadButton * 0.28f, stroke);
        label.setColor(held ? 0xFF071417 : 0xFFE3BE74);
        label.setTextSize(r.height() * 0.44f);
        c.drawText(glyph, r.centerX(), r.centerY() + r.height() * 0.16f, label);
    }

    private void drawRectButton(Canvas c, RectF r, String text, boolean held, int accent) {
        fill.setColor(held ? 0x99D8A84E : 0x4D0A2A30);
        c.drawRoundRect(r, r.height() * 0.30f, r.height() * 0.30f, fill);
        stroke.setColor(held ? 0xFFE3BE74 : 0x88D8A84E);
        stroke.setStrokeWidth(1.4f * density);
        c.drawRoundRect(r, r.height() * 0.30f, r.height() * 0.30f, stroke);
        label.setColor(held ? 0xFF071417 : accent);
        label.setTextSize(r.height() * 0.38f);
        c.drawText(text, r.centerX(), r.centerY() + r.height() * 0.14f, label);
    }

    private void drawFaceButton(Canvas c, float cx, float cy, float r, String text, int accent, boolean held) {
        fill.setColor(held ? 0xD8E3BE74 : 0x5A0A2A30);
        c.drawCircle(cx, cy, r, fill);
        stroke.setColor(held ? 0xFFE3BE74 : accent);
        stroke.setStrokeWidth(2.0f * density);
        c.drawCircle(cx, cy, r, stroke);
        label.setColor(held ? 0xFF071417 : accent);
        label.setTextSize(r * 0.68f);
        c.drawText(text, cx, cy + r * 0.24f, label);
    }

    private void drawRoundButton(Canvas c, float cx, float cy, float r, String text, int unused, boolean held) {
        fill.setColor(held ? 0x99D8A84E : 0x400A2A30);
        c.drawCircle(cx, cy, r, fill);
        stroke.setColor(held ? 0xFFE3BE74 : 0x88D8A84E);
        stroke.setStrokeWidth(1.4f * density);
        c.drawCircle(cx, cy, r, stroke);
        label.setColor(held ? 0xFF071417 : 0xFFB7C6CC);
        label.setTextSize(r * 0.62f);
        c.drawText(text, cx, cy + r * 0.22f, label);
    }

    private boolean isHeld(int control) {
        for (int i = 0; i < active.size(); i++) {
            if (active.valueAt(i).control == control) return true;
        }
        return false;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (menuOpen) return false;

        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        float x = e.getX(index), y = e.getY(index);

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            Active a = new Active();
            a.control = hit(x, y);
            a.downAt = System.currentTimeMillis();
            if (a.control != 0) {
                if (a.control == C_BUBBLE) {
                    bubblePressed = true;
                    bubbleDownAt = a.downAt;
                    invalidate();
                } else if (a.control == C_MENU) {
                    try {
                        NobleActivity.nativeMenuToggle();
                    } catch (UnsatisfiedLinkError ignored) {
                    }
                    onMenuOpened();
                    haptic();
                } else {
                    active.put(id, a);
                    updateControl(a.control, x, y, true);
                    haptic();
                }
                return true;
            }
            // Controls visible: swallow touches that miss so the game never sees
            // a stray tap. Hidden: let SDL have the whole screen.
            return controlsVisible || (bubbleVisible && bubbleRect.contains(x, y));
        }

        if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); i++) {
                int pid = e.getPointerId(i);
                Active a = active.get(pid);
                if (a == null) continue;
                updateControl(a.control, e.getX(i), e.getY(i), false);
            }
            return true;
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            Active a = active.get(id);
            if (a != null) {
                if (a.control == C_BUBBLE) {
                    if (bubblePressed && System.currentTimeMillis() - bubbleDownAt < 650) {
                        toggleControls();
                    }
                    bubblePressed = false;
                } else {
                    clearControl(a.control);
                    active.remove(id);
                    send();
                }
                invalidate();
                return true;
            }
            if (action == MotionEvent.ACTION_UP && bubblePressed) {
                bubblePressed = false;
                if (System.currentTimeMillis() - bubbleDownAt < 650) toggleControls();
                invalidate();
                return true;
            }
            return controlsVisible;
        }

        if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            return true;
        }
        return controlsVisible;
    }

    private void toggleControls() {
        controlsVisible = !controlsVisible;
        prefs.edit().putBoolean("touch_visible", controlsVisible).apply();
        if (!controlsVisible) releaseAll();
        try {
            NobleActivity.nativeTouchVisibilityChanged(controlsVisible);
        } catch (UnsatisfiedLinkError ignored) {
        }
        invalidate();
    }

    private int hit(float x, float y) {
        if (bubbleVisible && bubbleRect.contains(x, y)) return C_BUBBLE;
        if (ltRect.contains(x, y)) return C_LT;
        if (lbRect.contains(x, y)) return C_LB;
        if (rbRect.contains(x, y)) return C_RB;
        if (rtRect.contains(x, y)) return C_RT;
        if (backRect.contains(x, y)) return C_BACK;
        if (menuRect.contains(x, y)) return C_MENU;
        if (startRect.contains(x, y)) return C_START;
        for (int i = 0; i < 4; i++) {
            if (dpad[i].contains(x, y)) {
                return new int[]{C_DUP, C_DDOWN, C_DLEFT, C_DRIGHT}[i];
            }
        }
        if (dist(x, y, l3X, l3Y) <= l3R * 1.2f) return C_L3;
        if (dist(x, y, r3X, r3Y) <= r3R * 1.2f) return C_R3;
        if (dist(x, y, ax[0], ay[0]) <= abxyButtonR * 1.2f) return C_A;
        if (dist(x, y, ax[1], ay[1]) <= abxyButtonR * 1.2f) return C_B;
        if (dist(x, y, ax[2], ay[2]) <= abxyButtonR * 1.2f) return C_X;
        if (dist(x, y, ax[3], ay[3]) <= abxyButtonR * 1.2f) return C_Y;
        if (dist(x, y, leftStickX, leftStickY) <= leftStickR * 1.25f) return C_LSTICK;
        if (dist(x, y, rightStickX, rightStickY) <= rightStickR * 1.25f) return C_RSTICK;
        return 0;
    }

    private static float dist(float x, float y, float cx, float cy) {
        float dx = x - cx, dy = y - cy;
        return (float) Math.sqrt(dx * dx + dy * dy);
    }

    private void updateControl(int control, float x, float y, boolean down) {
        switch (control) {
            case C_LSTICK: {
                float dx = (x - leftStickX) / (leftStickR * 0.75f);
                float dy = (y - leftStickY) / (leftStickR * 0.75f);
                dx = Math.max(-1f, Math.min(1f, dx));
                dy = Math.max(-1f, Math.min(1f, dy));
                lx = (int) (dx * 32767f);
                ly = (int) (-dy * 32767f);
                break;
            }
            case C_RSTICK: {
                float dx = (x - rightStickX) / (rightStickR * 0.75f);
                float dy = (y - rightStickY) / (rightStickR * 0.75f);
                dx = Math.max(-1f, Math.min(1f, dx));
                dy = Math.max(-1f, Math.min(1f, dy));
                rx = (int) (dx * 32767f);
                ry = (int) (-dy * 32767f);
                break;
            }
            default:
                break;
        }
        send();
        invalidate();
    }

    private void clearControl(int control) {
        switch (control) {
            case C_LSTICK: lx = ly = 0; break;
            case C_RSTICK: rx = ry = 0; break;
            default: break;
        }
    }

    private void send() {
        int b = extraFaceButtons, a = 0, bb = 0, x = 0, y = 0, black = 0, white = 0, lt = 0, rt = 0;
        for (int i = 0; i < active.size(); i++) {
            switch (active.valueAt(i).control) {
                case C_DUP: b |= PAD_UP; break;
                case C_DDOWN: b |= PAD_DOWN; break;
                case C_DLEFT: b |= PAD_LEFT; break;
                case C_DRIGHT: b |= PAD_RIGHT; break;
                case C_START: b |= PAD_START; break;
                case C_BACK: b |= PAD_BACK; break;
                case C_L3: b |= PAD_L3; break;
                case C_R3: b |= PAD_R3; break;
                case C_A: a = 255; b |= 0x1000; break;
                case C_B: bb = 255; b |= 0x2000; break;
                case C_X: x = 255; b |= 0x4000; break;
                case C_Y: y = 255; b |= 0x8000; break;
                case C_LB: white = 255; break;
                case C_RB: black = 255; break;
                case C_LT: lt = 255; break;
                case C_RT: rt = 255; break;
                case C_MENU: break;  // handled on press, not as a held pad button
                default: break;
            }
        }
        buttons = b;
        analog[0] = a; analog[1] = bb; analog[2] = x; analog[3] = y;
        analog[4] = black; analog[5] = white; analog[6] = lt; analog[7] = rt;
        try {
            NobleActivity.nativeSetTouchPad(b, a, bb, x, y, black, white, lt, rt, lx, ly, rx, ry,
                    (controlsVisible && !menuOpen) || extraFaceButtons != 0);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private void haptic() {
        if (prefs.getBoolean("touch_haptics", true)) {
            performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec));
    }
}
