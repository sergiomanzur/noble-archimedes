package com.noblearchimedes;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.SparseArray;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Toast;

/**
 * Touch controls laid directly over SDL's SurfaceView. Draws a complete
 * Xbox-style pad (two sticks, d-pad, A/B/X/Y, shoulders, triggers, start/back,
 * L3/R3 and a settings button), a show/hide bubble, and an interactive WYSIWYG
 * layout & button-remapping editor.
 *
 * Touch is consumed only while the pad or editor is visible. When hidden,
 * events fall through to SDL so the ImGui settings menu can be used by touch.
 */
public class TouchControlsView extends View {
    private static final int PAD_UP = 0x01, PAD_DOWN = 0x02, PAD_LEFT = 0x04, PAD_RIGHT = 0x08;
    private static final int PAD_START = 0x10, PAD_BACK = 0x20, PAD_L3 = 0x40, PAD_R3 = 0x80;

    private static final float MIN_SCALE = 0.55f;
    private static final float MAX_SCALE = 1.65f;
    private static final long LONG_PRESS_EDIT_MS = 650;

    /** Logical Xbox action that any touch button can be mapped to. */
    public enum Action {
        A("A", "A (Jump / Confirm)", 0xFF69B96C),
        B("B", "B (Dodge / Cancel)", 0xFFD65C52),
        X("X", "X (Attack)", 0xFF5C93D6),
        Y("Y", "Y (Special / ID)", 0xFFD9B44E),
        LT("LT", "LT (Left Trigger)", 0xFF7FB3C8),
        RT("RT", "RT (Right Trigger)", 0xFF7FB3C8),
        LB("LB", "LB (White / L1)", 0xFFB7C6CC),
        RB("RB", "RB (Black / R1)", 0xFFB7C6CC),
        L3("L3", "L3 (Sprint)", 0xFFB7C6CC),
        R3("R3", "R3 (Speed-up)", 0xFFB7C6CC),
        BACK("BACK", "Back (Map)", 0xFFB7C6CC),
        START("START", "Start (Pause)", 0xFFB7C6CC),
        D_UP("▲", "D-Pad Up", 0xFFE3BE74),
        D_DOWN("▼", "D-Pad Down", 0xFFE3BE74),
        D_LEFT("◀", "D-Pad Left", 0xFFE3BE74),
        D_RIGHT("▶", "D-Pad Right", 0xFFE3BE74),
        MENU("MENU", "Settings Menu", 0xFFE3BE74),
        NONE("OFF", "Disabled (Hide)", 0xFF707C80);

        final String shortLabel;
        final String menuLabel;
        final int color;

        Action(String shortLabel, String menuLabel, int color) {
            this.shortLabel = shortLabel;
            this.menuLabel = menuLabel;
            this.color = color;
        }

        static Action fromName(String name, Action fallback) {
            if (name == null) return fallback;
            try {
                return Action.valueOf(name);
            } catch (IllegalArgumentException ignored) {
                return fallback;
            }
        }
    }

    // Control slot indices (17 individually positionable/resizable elements)
    private static final int S_A = 0, S_B = 1, S_X = 2, S_Y = 3;
    private static final int S_LT = 4, S_LB = 5, S_RB = 6, S_RT = 7;
    private static final int S_BACK = 8, S_MENU = 9, S_START = 10;
    private static final int S_L3 = 11, S_R3 = 12;
    private static final int S_LSTICK = 13, S_RSTICK = 14, S_DPAD = 15, S_BUBBLE = 16;
    private static final int SLOT_COUNT = 17;

    // Virtual hit IDs for gameplay (D-pad cells get distinct IDs so 4 directions track independently)
    private static final int H_DUP = 100, H_DDOWN = 101, H_DLEFT = 102, H_DRIGHT = 103;

    private static final String[] SLOT_KEYS = {
        "a", "b", "x", "y",
        "lt", "lb", "rb", "rt",
        "back", "menu", "start",
        "l3", "r3",
        "lstick", "rstick", "dpad", "bubble"
    };

    private static final String[] SLOT_NAMES = {
        "A Button", "B Button", "X Button", "Y Button",
        "LT Trigger", "LB Shoulder", "RB Shoulder", "RT Trigger",
        "BACK Button", "MENU Button", "START Button",
        "L3 Button", "R3 Button",
        "Left Stick", "Right Stick", "D-Pad", "HIDE / TOUCH Bubble"
    };

    private static final Action[] DEFAULT_SLOT_ACTIONS = {
        Action.A, Action.B, Action.X, Action.Y,
        Action.LT, Action.LB, Action.RB, Action.RT,
        Action.BACK, Action.MENU, Action.START,
        Action.L3, Action.R3,
        Action.NONE, Action.NONE, Action.NONE, Action.NONE
    };

    private static final Action[] DEFAULT_DPAD_ACTIONS = {
        Action.D_UP, Action.D_DOWN, Action.D_LEFT, Action.D_RIGHT
    };

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint small = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bubble = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint selectPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint gridPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    // Persisted per-slot normalized positions (NaN = use default), scale, and remapped actions.
    private final float[] customNx = new float[SLOT_COUNT];
    private final float[] customNy = new float[SLOT_COUNT];
    private final float[] slotScale = new float[SLOT_COUNT];
    private final Action[] slotAction = new Action[SLOT_COUNT];
    private final Action[] dpadAction = new Action[4];
    private boolean sticksSwapped;
    private int opacityPercent = 100;

    // Computed pixel geometry for current view width/height.
    private final float[] cx = new float[SLOT_COUNT];
    private final float[] cy = new float[SLOT_COUNT];
    private final float[] halfW = new float[SLOT_COUNT];
    private final float[] halfH = new float[SLOT_COUNT];
    private final RectF[] slotRect = new RectF[SLOT_COUNT];
    private final RectF[] dpad = {new RectF(), new RectF(), new RectF(), new RectF()};
    private float dpadButtonPx;
    private int viewW, viewH;
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
    private boolean bubbleLongFired;

    // Interactive layout & button-remapping editor state.
    private boolean editMode;
    private boolean visibleBeforeEdit;
    private int selectedSlot = S_A;
    private int dragPointerId = -1;
    private float dragStartX, dragStartY, dragOrigCx, dragOrigCy;
    private boolean dragging;
    private final RectF toolbarRect = new RectF();
    private final RectF btnSmaller = new RectF();
    private final RectF btnLarger = new RectF();
    private final RectF btnRemap = new RectF();
    private final RectF btnOpacity = new RectF();
    private final RectF btnReset = new RectF();
    private final RectF btnDone = new RectF();
    private boolean toolbarAtBottom;

    private final SparseArray<Active> active = new SparseArray<>();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final SharedPreferences prefs;

    private int buttons;
    private final int[] analog = new int[8];
    private int lx, ly, rx, ry;
    private int extraFaceButtons;

    private static final class Active {
        int hitId;  // 0..SLOT_COUNT-1 or H_DUP..H_DRIGHT
        long downAt;
    }

    private final Runnable bubbleLongPress = new Runnable() {
        @Override public void run() {
            if (bubblePressed && !menuOpen && !editMode) {
                bubbleLongFired = true;
                bubblePressed = false;
                haptic();
                enterEditMode();
            }
        }
    };

    // Poll native menu state and touch-editor launch requests from the ImGui Controls tab.
    private final Runnable pollMenu = new Runnable() {
        @Override public void run() {
            boolean open = false;
            boolean wantEdit = false;
            try {
                open = NobleActivity.nativeMenuOpen();
                wantEdit = NobleActivity.nativeConsumeTouchEditorRequest();
            } catch (UnsatisfiedLinkError ignored) {
            }
            if (wantEdit) {
                menuOpen = false;
                enterEditMode();
            } else if (!editMode) {
                if (open && !menuOpen) onMenuOpened();
                else if (!open && menuOpen) onMenuClosed();
            }
            handler.postDelayed(this, 200);
        }
    };

    public TouchControlsView(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        prefs = context.getSharedPreferences("noble_android", Context.MODE_PRIVATE);

        for (int i = 0; i < SLOT_COUNT; i++) slotRect[i] = new RectF();

        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(2.0f * density);
        label.setTextAlign(Paint.Align.CENTER);
        label.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        small.setTextAlign(Paint.Align.CENTER);
        small.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        bubble.setStyle(Paint.Style.FILL);

        selectPaint.setStyle(Paint.Style.STROKE);
        selectPaint.setColor(0xFFFFD86B);
        selectPaint.setStrokeWidth(2.4f * density);
        selectPaint.setPathEffect(new DashPathEffect(new float[]{8f * density, 5f * density}, 0f));

        gridPaint.setStyle(Paint.Style.STROKE);
        gridPaint.setColor(0x22E3BE74);
        gridPaint.setStrokeWidth(1.0f * density);

        loadConfig();

        controllerPresent = hasPhysicalGamepad();
        controlsVisible = !controllerPresent && prefs.getBoolean("touch_visible", true);
        bubbleVisible = !controllerPresent;
        autoHiddenForController = controllerPresent;
        setWillNotDraw(false);
        setClickable(false);
        setFocusable(false);
        setLayerType(View.LAYER_TYPE_HARDWARE, null);
    }

    private void loadConfig() {
        for (int i = 0; i < SLOT_COUNT; i++) {
            customNx[i] = prefs.getFloat("tc_nx_" + SLOT_KEYS[i], Float.NaN);
            customNy[i] = prefs.getFloat("tc_ny_" + SLOT_KEYS[i], Float.NaN);
            slotScale[i] = clamp(prefs.getFloat("tc_sc_" + SLOT_KEYS[i], 1.0f), MIN_SCALE, MAX_SCALE);
            slotAction[i] = Action.fromName(prefs.getString("tc_act_" + SLOT_KEYS[i], null), DEFAULT_SLOT_ACTIONS[i]);
        }
        for (int i = 0; i < 4; i++) {
            dpadAction[i] = Action.fromName(prefs.getString("tc_dpad_act_" + i, null), DEFAULT_DPAD_ACTIONS[i]);
        }
        sticksSwapped = prefs.getBoolean("tc_sticks_swapped", false);
        opacityPercent = Math.max(30, Math.min(100, prefs.getInt("tc_alpha", 100)));
    }

    private void saveSlot(int slot) {
        SharedPreferences.Editor ed = prefs.edit();
        if (Float.isNaN(customNx[slot]) || Float.isNaN(customNy[slot])) {
            ed.remove("tc_nx_" + SLOT_KEYS[slot]).remove("tc_ny_" + SLOT_KEYS[slot]);
        } else {
            ed.putFloat("tc_nx_" + SLOT_KEYS[slot], customNx[slot]);
            ed.putFloat("tc_ny_" + SLOT_KEYS[slot], customNy[slot]);
        }
        ed.putFloat("tc_sc_" + SLOT_KEYS[slot], slotScale[slot]);
        if (isRemappableButtonSlot(slot)) {
            ed.putString("tc_act_" + SLOT_KEYS[slot], slotAction[slot].name());
        }
        ed.apply();
    }

    private void saveDpadActions() {
        SharedPreferences.Editor ed = prefs.edit();
        for (int i = 0; i < 4; i++) ed.putString("tc_dpad_act_" + i, dpadAction[i].name());
        ed.apply();
    }

    private void resetSlotToDefault(int slot) {
        customNx[slot] = Float.NaN;
        customNy[slot] = Float.NaN;
        slotScale[slot] = 1.0f;
        slotAction[slot] = DEFAULT_SLOT_ACTIONS[slot];
        saveSlot(slot);
        if (slot == S_DPAD) {
            System.arraycopy(DEFAULT_DPAD_ACTIONS, 0, dpadAction, 0, 4);
            saveDpadActions();
        } else if (slot == S_LSTICK || slot == S_RSTICK) {
            sticksSwapped = false;
            prefs.edit().putBoolean("tc_sticks_swapped", false).apply();
        }
        layoutControls();
        invalidate();
    }

    private void resetAllToDefault() {
        SharedPreferences.Editor ed = prefs.edit();
        for (int i = 0; i < SLOT_COUNT; i++) {
            customNx[i] = Float.NaN;
            customNy[i] = Float.NaN;
            slotScale[i] = 1.0f;
            slotAction[i] = DEFAULT_SLOT_ACTIONS[i];
            ed.remove("tc_nx_" + SLOT_KEYS[i])
                    .remove("tc_ny_" + SLOT_KEYS[i])
                    .remove("tc_sc_" + SLOT_KEYS[i])
                    .remove("tc_act_" + SLOT_KEYS[i]);
        }
        for (int i = 0; i < 4; i++) {
            dpadAction[i] = DEFAULT_DPAD_ACTIONS[i];
            ed.remove("tc_dpad_act_" + i);
        }
        sticksSwapped = false;
        opacityPercent = 100;
        ed.remove("tc_sticks_swapped").remove("tc_alpha").apply();
        layoutControls();
        invalidate();
    }

    private static boolean isRemappableButtonSlot(int slot) {
        return slot >= S_A && slot <= S_R3;
    }

    private static boolean isFaceSlot(int slot) {
        return slot >= S_A && slot <= S_Y;
    }

    private static boolean isRectSlot(int slot) {
        return slot >= S_LT && slot <= S_START;
    }

    private static boolean isStickClickSlot(int slot) {
        return slot == S_L3 || slot == S_R3;
    }

    public boolean isEditMode() {
        return editMode;
    }

    public void enterEditMode() {
        if (editMode) return;
        visibleBeforeEdit = controlsVisible;
        editMode = true;
        controlsVisible = true;
        dragPointerId = -1;
        dragging = false;
        releaseAll();
        layoutControls();
        invalidate();
    }

    public void exitEditMode() {
        if (!editMode) return;
        editMode = false;
        dragPointerId = -1;
        dragging = false;
        controlsVisible = visibleBeforeEdit || !controllerPresent;
        prefs.edit().putBoolean("touch_visible", controlsVisible).apply();
        releaseAll();
        layoutControls();
        invalidate();
        Toast.makeText(getContext(), "Touch layout saved", Toast.LENGTH_SHORT).show();
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
        if (editMode) return;
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
        handler.removeCallbacks(bubbleLongPress);
        bubblePressed = false;
        bubbleLongFired = false;
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
        handler.postDelayed(pollMenu, 200);
    }

    @Override protected void onDetachedFromWindow() {
        handler.removeCallbacksAndMessages(null);
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        viewW = w;
        viewH = h;
        layoutControls();
        lx = ly = rx = ry = 0;
        send();
    }

    private void layoutControls() {
        if (viewW <= 0 || viewH <= 0) return;
        float w = viewW, h = viewH;
        float min = Math.min(w, h);
        float m = 10f * density;

        // Default base dimensions before per-slot scale.
        float bs = Math.max(42f * density, min * 0.052f);
        float top = m + bs + 8f * density;
        float bw = Math.max(54f * density, min * 0.070f);
        float bh = Math.max(30f * density, min * 0.040f);
        float cw = Math.max(48f * density, min * 0.058f);
        float topCy = top + bh * 0.5f;

        float defLStickX = w * 0.145f, defLStickY = h * 0.665f, baseLStickR = min * 0.105f;
        float defRStickX = w * 0.585f, defRStickY = h * 0.685f, baseRStickR = min * 0.100f;
        float defDpadX = w * 0.325f, defDpadY = h * 0.800f, baseDpadBtn = min * 0.074f;
        float defAbxyX = w * 0.835f, defAbxyY = h * 0.705f, abxyR = min * 0.130f, baseAbxyBtnR = min * 0.074f;
        float baseL3R = min * 0.036f;

        float[] defCx = new float[SLOT_COUNT];
        float[] defCy = new float[SLOT_COUNT];
        float[] baseHW = new float[SLOT_COUNT];
        float[] baseHH = new float[SLOT_COUNT];

        // Face buttons (individual centers so each can be dragged & resized separately)
        defCx[S_A] = defAbxyX;         defCy[S_A] = defAbxyY + abxyR; baseHW[S_A] = baseHH[S_A] = baseAbxyBtnR;
        defCx[S_B] = defAbxyX + abxyR; defCy[S_B] = defAbxyY;         baseHW[S_B] = baseHH[S_B] = baseAbxyBtnR;
        defCx[S_X] = defAbxyX - abxyR; defCy[S_X] = defAbxyY;         baseHW[S_X] = baseHH[S_X] = baseAbxyBtnR;
        defCx[S_Y] = defAbxyX;         defCy[S_Y] = defAbxyY - abxyR; baseHW[S_Y] = baseHH[S_Y] = baseAbxyBtnR;

        // Shoulders & triggers
        float ltLeft = m + 4f * density;
        float lbLeft = ltLeft + bw + 6f * density;
        float rEdge = w - m - 4f * density;
        float rtLeft = rEdge - bw;
        float rbLeft = rtLeft - 6f * density - bw;
        defCx[S_LT] = ltLeft + bw * 0.5f; defCy[S_LT] = topCy; baseHW[S_LT] = bw * 0.5f; baseHH[S_LT] = bh * 0.5f;
        defCx[S_LB] = lbLeft + bw * 0.5f; defCy[S_LB] = topCy; baseHW[S_LB] = bw * 0.5f; baseHH[S_LB] = bh * 0.5f;
        defCx[S_RB] = rbLeft + bw * 0.5f; defCy[S_RB] = topCy; baseHW[S_RB] = bw * 0.5f; baseHH[S_RB] = bh * 0.5f;
        defCx[S_RT] = rtLeft + bw * 0.5f; defCy[S_RT] = topCy; baseHW[S_RT] = bw * 0.5f; baseHH[S_RT] = bh * 0.5f;

        // System buttons
        defCx[S_BACK] = w * 0.5f - cw * 1.55f; defCy[S_BACK] = topCy; baseHW[S_BACK] = cw * 0.5f; baseHH[S_BACK] = bh * 0.5f;
        defCx[S_MENU] = w * 0.5f;              defCy[S_MENU] = topCy; baseHW[S_MENU] = cw * 0.5f; baseHH[S_MENU] = bh * 0.5f;
        defCx[S_START] = w * 0.5f + cw * 1.55f; defCy[S_START] = topCy; baseHW[S_START] = cw * 0.5f; baseHH[S_START] = bh * 0.5f;

        // L3 / R3
        defCx[S_L3] = defLStickX; defCy[S_L3] = defLStickY - baseLStickR - baseL3R * 1.8f; baseHW[S_L3] = baseHH[S_L3] = baseL3R;
        defCx[S_R3] = defRStickX; defCy[S_R3] = defRStickY - baseRStickR - baseL3R * 1.8f; baseHW[S_R3] = baseHH[S_R3] = baseL3R;

        // Sticks
        defCx[S_LSTICK] = defLStickX; defCy[S_LSTICK] = defLStickY; baseHW[S_LSTICK] = baseHH[S_LSTICK] = baseLStickR;
        defCx[S_RSTICK] = defRStickX; defCy[S_RSTICK] = defRStickY; baseHW[S_RSTICK] = baseHH[S_RSTICK] = baseRStickR;

        // D-pad cluster
        float dpadExtent = baseDpadBtn * 1.64f;
        defCx[S_DPAD] = defDpadX; defCy[S_DPAD] = defDpadY; baseHW[S_DPAD] = baseHH[S_DPAD] = dpadExtent;

        // HIDE / TOUCH bubble
        float bubbleY = h * 0.44f;
        defCx[S_BUBBLE] = m + bs * 0.975f; defCy[S_BUBBLE] = bubbleY + bs * 0.5f;
        baseHW[S_BUBBLE] = bs * 0.975f;    baseHH[S_BUBBLE] = bs * 0.5f;

        for (int i = 0; i < SLOT_COUNT; i++) {
            float sc = slotScale[i];
            halfW[i] = baseHW[i] * sc;
            halfH[i] = baseHH[i] * sc;
            float x = Float.isNaN(customNx[i]) ? defCx[i] : customNx[i] * w;
            float y = Float.isNaN(customNy[i]) ? defCy[i] : customNy[i] * h;
            float padX = Math.min(halfW[i] + 4f * density, w * 0.45f);
            float padY = Math.min(halfH[i] + 4f * density, h * 0.45f);
            cx[i] = clamp(x, padX, w - padX);
            cy[i] = clamp(y, padY, h - padY);
            slotRect[i].set(cx[i] - halfW[i], cy[i] - halfH[i], cx[i] + halfW[i], cy[i] + halfH[i]);
        }

        // Compute the 4 D-pad cells around cx[S_DPAD], cy[S_DPAD].
        dpadButtonPx = baseDpadBtn * slotScale[S_DPAD];
        float g = dpadButtonPx * 0.14f;
        float half = dpadButtonPx * 0.5f;
        float dx = cx[S_DPAD], dy = cy[S_DPAD];
        dpad[0].set(dx - half, dy - half - dpadButtonPx - g, dx + half, dy - half - g);
        dpad[1].set(dx - half, dy + half + g, dx + half, dy + half + dpadButtonPx + g);
        dpad[2].set(dx - half - dpadButtonPx - g, dy - half, dx - half - g, dy + half);
        dpad[3].set(dx + half + g, dy - half, dx + half + dpadButtonPx + g, dy + half);

        layoutEditorToolbar();
    }

    private void layoutEditorToolbar() {
        if (viewW <= 0 || viewH <= 0) return;
        // Place the toolbar in the clear band just below the top button row (BACK/MENU/START)
        // so all 17 controls remain visible and tappable; flip it to the bottom whenever the
        // selected control is in the upper half of the screen.
        toolbarAtBottom = (selectedSlot >= 0 && selectedSlot < SLOT_COUNT && cy[selectedSlot] < viewH * 0.46f);
        float barW = Math.min(viewW - 24f * density, 610f * density);
        float barH = 68f * density;
        float left = (viewW - barW) * 0.5f;
        float topRowEnd = Math.max(slotRect[S_BACK].bottom,
                Math.max(slotRect[S_MENU].bottom, slotRect[S_START].bottom));
        float upperTop = clamp(topRowEnd + 10f * density, 12f * density, viewH * 0.26f);
        float top = toolbarAtBottom ? (viewH - barH - 12f * density) : upperTop;
        toolbarRect.set(left, top, left + barW, top + barH);

        float pad = 8f * density;
        float rowTop = top + 26f * density;
        float rowBot = top + barH - 7f * density;
        float gap = 6f * density;
        float avail = barW - pad * 2f - gap * 5f;
        float[] weights = {0.70f, 0.70f, 1.45f, 1.25f, 1.00f, 1.10f};
        float sum = 0f;
        for (float wt : weights) sum += wt;
        RectF[] btns = {btnSmaller, btnLarger, btnRemap, btnOpacity, btnReset, btnDone};
        float x = left + pad;
        for (int i = 0; i < btns.length; i++) {
            float bw = avail * (weights[i] / sum);
            btns[i].set(x, rowTop, x + bw, rowBot);
            x += bw + gap;
        }
    }

    private int applyAlpha(int argb, boolean dimmed) {
        int a = (argb >>> 24) & 0xFF;
        float factor = (opacityPercent / 100f) * (dimmed ? 0.35f : 1.0f);
        int outA = Math.max(12, Math.min(255, Math.round(a * factor)));
        return (outA << 24) | (argb & 0x00FFFFFF);
    }

    @Override protected void onDraw(Canvas c) {
        if (menuOpen && !editMode) return;

        if (editMode) {
            // Subtle dim + alignment guide grid
            fill.setColor(0x55000000);
            c.drawRect(0, 0, viewW, viewH, fill);
            int cols = 12, rows = 6;
            for (int i = 1; i < cols; i++) {
                float gx = viewW * (i / (float) cols);
                c.drawLine(gx, 0, gx, viewH, gridPaint);
            }
            for (int j = 1; j < rows; j++) {
                float gy = viewH * (j / (float) rows);
                c.drawLine(0, gy, viewW, gy, gridPaint);
            }
        }

        drawBubble(c);
        if (!controlsVisible && !editMode) return;

        // Sticks
        drawStick(c, S_LSTICK, !sticksSwapped ? lx : rx, !sticksSwapped ? ly : ry, sticksSwapped);
        drawStick(c, S_RSTICK, !sticksSwapped ? rx : lx, !sticksSwapped ? ry : ly, !sticksSwapped);

        // L3 / R3
        drawSlotControl(c, S_L3);
        drawSlotControl(c, S_R3);

        // D-pad
        drawDpad(c);

        // Shoulders, triggers, system buttons
        for (int s = S_LT; s <= S_START; s++) drawSlotControl(c, s);

        // Face buttons
        for (int s = S_A; s <= S_Y; s++) drawSlotControl(c, s);

        if (editMode) {
            drawSelectionHalo(c, selectedSlot);
            drawEditorToolbar(c);
        }
    }

    private void drawSlotControl(Canvas c, int slot) {
        Action act = slotAction[slot];
        if (act == Action.NONE && !editMode) return;
        boolean dimmed = (act == Action.NONE);
        boolean held = !editMode && isHeld(slot);
        if (isFaceSlot(slot)) {
            drawFaceButton(c, cx[slot], cy[slot], halfW[slot], act.shortLabel, act.color, held, dimmed);
        } else if (isStickClickSlot(slot)) {
            drawRoundButton(c, cx[slot], cy[slot], halfW[slot], act.shortLabel, act.color, held, dimmed);
        } else {
            drawRectButton(c, slotRect[slot], act.shortLabel, held, act.color, dimmed);
        }
    }

    private void drawBubble(Canvas c) {
        if (!bubbleVisible && !editMode) return;
        RectF rRect = slotRect[S_BUBBLE];
        float r = rRect.height() * 0.5f;
        bubble.setColor(applyAlpha(bubblePressed ? 0xB8D8A84E : 0x8C0A2025, false));
        c.drawRoundRect(rRect, r, r, bubble);
        stroke.setColor(applyAlpha(0xB0D8A84E, false));
        stroke.setStrokeWidth(1.6f * density);
        c.drawRoundRect(rRect, r, r, stroke);

        float gx = rRect.left + r;
        float gy = rRect.centerY();
        fill.setColor(applyAlpha(0xFFE3BE74, false));
        RectF body = new RectF(gx - r * 0.46f, gy - r * 0.24f, gx + r * 0.46f, gy + r * 0.30f);
        c.drawRoundRect(body, r * 0.22f, r * 0.22f, fill);
        fill.setColor(applyAlpha(0xFF0A2025, false));
        c.drawCircle(gx - r * 0.20f, gy + r * 0.02f, r * 0.075f, fill);
        c.drawCircle(gx + r * 0.20f, gy + r * 0.02f, r * 0.075f, fill);

        label.setColor(applyAlpha(0xFFE3BE74, false));
        label.setTextSize(r * 0.44f);
        String text = editMode ? "HIDE" : (controlsVisible ? "HIDE" : "TOUCH");
        c.drawText(text, rRect.left + r * 2.05f, gy + r * 0.16f, label);
    }

    private void drawStick(Canvas c, int slot, int dx, int dy, boolean cameraRole) {
        float x = cx[slot], y = cy[slot], r = halfW[slot];
        fill.setColor(applyAlpha(0x3D0A2A30, false));
        c.drawCircle(x, y, r, fill);
        stroke.setColor(applyAlpha(0x99D8A84E, false));
        stroke.setStrokeWidth(1.8f * density);
        c.drawCircle(x, y, r, stroke);

        stroke.setColor(applyAlpha(0x33D8A84E, false));
        stroke.setStrokeWidth(1.0f * density);
        c.drawLine(x - r * 0.7f, y, x + r * 0.7f, y, stroke);
        c.drawLine(x, y - r * 0.7f, x, y + r * 0.7f, stroke);

        float kx = x + (dx / 32767f) * r * 0.58f;
        float ky = y - (dy / 32767f) * r * 0.58f;
        fill.setColor(applyAlpha(0x8CE3BE74, false));
        c.drawCircle(kx, ky, r * 0.36f, fill);
        stroke.setColor(applyAlpha(0xCC071417, false));
        stroke.setStrokeWidth(1.4f * density);
        c.drawCircle(kx, ky, r * 0.36f, stroke);

        small.setColor(applyAlpha(0x99E3BE74, false));
        small.setTextSize(r * 0.20f);
        c.drawText(cameraRole ? "CAMERA" : "MOVE", x, y + r * 1.20f, small);
    }

    private void drawDpad(Canvas c) {
        int[] hits = {H_DUP, H_DDOWN, H_DLEFT, H_DRIGHT};
        for (int i = 0; i < 4; i++) {
            Action act = dpadAction[i];
            if (act == Action.NONE && !editMode) continue;
            drawDpadCell(c, dpad[i], act.shortLabel, act.color, !editMode && isHeld(hits[i]), act == Action.NONE);
        }
    }

    private void drawDpadCell(Canvas c, RectF r, String glyph, int accent, boolean held, boolean dimmed) {
        fill.setColor(applyAlpha(held ? 0x99D8A84E : 0x4D0A2A30, dimmed));
        c.drawRoundRect(r, dpadButtonPx * 0.28f, dpadButtonPx * 0.28f, fill);
        stroke.setColor(applyAlpha(held ? 0xFFE3BE74 : 0x99D8A84E, dimmed));
        stroke.setStrokeWidth(1.5f * density);
        c.drawRoundRect(r, dpadButtonPx * 0.28f, dpadButtonPx * 0.28f, stroke);
        label.setColor(applyAlpha(held ? 0xFF071417 : accent, dimmed));
        label.setTextSize(r.height() * (glyph.length() > 2 ? 0.30f : 0.44f));
        c.drawText(glyph, r.centerX(), r.centerY() + r.height() * 0.14f, label);
    }

    private void drawRectButton(Canvas c, RectF r, String text, boolean held, int accent, boolean dimmed) {
        fill.setColor(applyAlpha(held ? 0x99D8A84E : 0x4D0A2A30, dimmed));
        c.drawRoundRect(r, r.height() * 0.30f, r.height() * 0.30f, fill);
        stroke.setColor(applyAlpha(held ? 0xFFE3BE74 : 0x88D8A84E, dimmed));
        stroke.setStrokeWidth(1.4f * density);
        c.drawRoundRect(r, r.height() * 0.30f, r.height() * 0.30f, stroke);
        label.setColor(applyAlpha(held ? 0xFF071417 : accent, dimmed));
        label.setTextSize(r.height() * 0.38f);
        c.drawText(text, r.centerX(), r.centerY() + r.height() * 0.14f, label);
    }

    private void drawFaceButton(Canvas c, float x, float y, float r, String text, int accent, boolean held, boolean dimmed) {
        fill.setColor(applyAlpha(held ? 0xD8E3BE74 : 0x5A0A2A30, dimmed));
        c.drawCircle(x, y, r, fill);
        stroke.setColor(applyAlpha(held ? 0xFFE3BE74 : accent, dimmed));
        stroke.setStrokeWidth(2.0f * density);
        c.drawCircle(x, y, r, stroke);
        label.setColor(applyAlpha(held ? 0xFF071417 : accent, dimmed));
        label.setTextSize(r * (text.length() > 2 ? 0.40f : 0.68f));
        c.drawText(text, x, y + r * (text.length() > 2 ? 0.14f : 0.24f), label);
    }

    private void drawRoundButton(Canvas c, float x, float y, float r, String text, int accent, boolean held, boolean dimmed) {
        fill.setColor(applyAlpha(held ? 0x99D8A84E : 0x400A2A30, dimmed));
        c.drawCircle(x, y, r, fill);
        stroke.setColor(applyAlpha(held ? 0xFFE3BE74 : 0x88D8A84E, dimmed));
        stroke.setStrokeWidth(1.4f * density);
        c.drawCircle(x, y, r, stroke);
        label.setColor(applyAlpha(held ? 0xFF071417 : accent, dimmed));
        label.setTextSize(r * (text.length() > 2 ? 0.44f : 0.62f));
        c.drawText(text, x, y + r * 0.18f, label);
    }

    private void drawSelectionHalo(Canvas c, int slot) {
        if (slot < 0 || slot >= SLOT_COUNT) return;
        float pad = 6f * density;
        if (isFaceSlot(slot) || isStickClickSlot(slot) || slot == S_LSTICK || slot == S_RSTICK) {
            c.drawCircle(cx[slot], cy[slot], halfW[slot] + pad, selectPaint);
        } else {
            RectF r = new RectF(slotRect[slot].left - pad, slotRect[slot].top - pad,
                    slotRect[slot].right + pad, slotRect[slot].bottom + pad);
            c.drawRoundRect(r, 10f * density, 10f * density, selectPaint);
        }
    }

    private void drawEditorToolbar(Canvas c) {
        layoutEditorToolbar();
        float rr = 12f * density;
        fill.setColor(0xEB0B191D);
        c.drawRoundRect(toolbarRect, rr, rr, fill);
        stroke.setColor(0xFFD8A84E);
        stroke.setStrokeWidth(1.6f * density);
        c.drawRoundRect(toolbarRect, rr, rr, stroke);

        String info = selectedStatusText();
        small.setColor(0xFFE3BE74);
        small.setTextSize(12.5f * density);
        c.drawText(info, toolbarRect.centerX(), toolbarRect.top + 19f * density, small);

        drawToolBtn(c, btnSmaller, "Size -", 0xFF21363C, 0xFFE5ECEE);
        drawToolBtn(c, btnLarger, "Size +", 0xFF21363C, 0xFFE5ECEE);
        String remapLabel = (selectedSlot == S_LSTICK || selectedSlot == S_RSTICK)
                ? "Swap Sticks"
                : (selectedSlot == S_BUBBLE ? "Drag Only" : "Remap");
        int remapBg = (selectedSlot == S_BUBBLE) ? 0xFF1A262A : 0xFF5B181D;
        int remapFg = (selectedSlot == S_BUBBLE) ? 0xFF6B7B80 : 0xFFFFE5B0;
        drawToolBtn(c, btnRemap, remapLabel, remapBg, remapFg);
        drawToolBtn(c, btnOpacity, "Alpha " + opacityPercent + "%", 0xFF21363C, 0xFFE5ECEE);
        drawToolBtn(c, btnReset, "Reset", 0xFF3E1C1F, 0xFFF3B3AE);
        drawToolBtn(c, btnDone, "Done", 0xFF285E37, 0xFFFFFFFF);
    }

    private String selectedStatusText() {
        int pct = Math.round(slotScale[selectedSlot] * 100f);
        if (selectedSlot == S_LSTICK) {
            return "Drag to move  |  Left Stick (" + (sticksSwapped ? "CAMERA" : "MOVE") + ")  " + pct + "%";
        }
        if (selectedSlot == S_RSTICK) {
            return "Drag to move  |  Right Stick (" + (sticksSwapped ? "MOVE" : "CAMERA") + ")  " + pct + "%";
        }
        if (selectedSlot == S_DPAD) {
            return "Drag to move  |  D-Pad (" + dpadAction[0].shortLabel + "/" + dpadAction[1].shortLabel
                    + "/" + dpadAction[2].shortLabel + "/" + dpadAction[3].shortLabel + ")  " + pct + "%";
        }
        if (selectedSlot == S_BUBBLE) {
            return "Drag to move  |  HIDE/TOUCH Bubble  " + pct + "%";
        }
        return "Drag to move  |  " + SLOT_NAMES[selectedSlot] + " -> " + slotAction[selectedSlot].menuLabel + "  " + pct + "%";
    }

    private void drawToolBtn(Canvas c, RectF r, String text, int bg, int fg) {
        fill.setColor(bg);
        c.drawRoundRect(r, 7f * density, 7f * density, fill);
        stroke.setColor(0x66D8A84E);
        stroke.setStrokeWidth(1.1f * density);
        c.drawRoundRect(r, 7f * density, 7f * density, stroke);
        label.setColor(fg);
        label.setTextSize(12.0f * density);
        c.drawText(text, r.centerX(), r.centerY() + 4.5f * density, label);
    }

    private boolean isHeld(int hitId) {
        for (int i = 0; i < active.size(); i++) {
            if (active.valueAt(i).hitId == hitId) return true;
        }
        return false;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editMode) return handleEditTouch(e);
        if (menuOpen) return false;

        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        float x = e.getX(index), y = e.getY(index);

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int hitId = hitGameplay(x, y);
            if (hitId >= 0) {
                if (hitId == S_BUBBLE) {
                    bubblePressed = true;
                    bubbleLongFired = false;
                    bubbleDownAt = System.currentTimeMillis();
                    handler.removeCallbacks(bubbleLongPress);
                    handler.postDelayed(bubbleLongPress, LONG_PRESS_EDIT_MS);
                    invalidate();
                } else if (actionForHit(hitId) == Action.MENU) {
                    try {
                        NobleActivity.nativeMenuToggle();
                    } catch (UnsatisfiedLinkError ignored) {
                    }
                    onMenuOpened();
                    haptic();
                } else {
                    Active a = new Active();
                    a.hitId = hitId;
                    a.downAt = System.currentTimeMillis();
                    active.put(id, a);
                    updateControl(hitId, x, y);
                    haptic();
                }
                return true;
            }
            return controlsVisible || (bubbleVisible && slotRect[S_BUBBLE].contains(x, y));
        }

        if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); i++) {
                int pid = e.getPointerId(i);
                Active a = active.get(pid);
                if (a == null) continue;
                updateControl(a.hitId, e.getX(i), e.getY(i));
            }
            return true;
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            Active a = active.get(id);
            if (a != null) {
                clearControl(a.hitId);
                active.remove(id);
                send();
                invalidate();
                return true;
            }
            if (action == MotionEvent.ACTION_UP && bubblePressed) {
                handler.removeCallbacks(bubbleLongPress);
                bubblePressed = false;
                if (!bubbleLongFired && System.currentTimeMillis() - bubbleDownAt < LONG_PRESS_EDIT_MS) {
                    toggleControls();
                }
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

    private boolean handleEditTouch(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        float x = e.getX(index), y = e.getY(index);

        if (action == MotionEvent.ACTION_DOWN) {
            if (toolbarRect.contains(x, y)) {
                handleToolbarTap(x, y);
                return true;
            }
            int slot = hitEditSlot(x, y);
            if (slot >= 0) {
                selectedSlot = slot;
                dragPointerId = id;
                dragStartX = x;
                dragStartY = y;
                dragOrigCx = cx[slot];
                dragOrigCy = cy[slot];
                dragging = false;
                haptic();
                invalidate();
            }
            return true;
        }

        if (action == MotionEvent.ACTION_MOVE && dragPointerId >= 0) {
            int idx = e.findPointerIndex(dragPointerId);
            if (idx >= 0) {
                float mx = e.getX(idx), my = e.getY(idx);
                float dx = mx - dragStartX, dy = my - dragStartY;
                if (!dragging && Math.hypot(dx, dy) > 4f * density) dragging = true;
                if (dragging && viewW > 0 && viewH > 0) {
                    float padX = Math.min(halfW[selectedSlot] + 4f * density, viewW * 0.45f);
                    float padY = Math.min(halfH[selectedSlot] + 4f * density, viewH * 0.45f);
                    float nx = clamp(dragOrigCx + dx, padX, viewW - padX) / viewW;
                    float ny = clamp(dragOrigCy + dy, padY, viewH - padY) / viewH;
                    customNx[selectedSlot] = nx;
                    customNy[selectedSlot] = ny;
                    layoutControls();
                    invalidate();
                }
            }
            return true;
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            if (dragPointerId >= 0) {
                if (dragging) saveSlot(selectedSlot);
                dragPointerId = -1;
                dragging = false;
                invalidate();
            }
            return true;
        }
        return true;
    }

    private void handleToolbarTap(float x, float y) {
        haptic();
        if (btnSmaller.contains(x, y)) {
            slotScale[selectedSlot] = clamp(Math.round((slotScale[selectedSlot] - 0.10f) * 20f) / 20f, MIN_SCALE, MAX_SCALE);
            saveSlot(selectedSlot);
            layoutControls();
            invalidate();
        } else if (btnLarger.contains(x, y)) {
            slotScale[selectedSlot] = clamp(Math.round((slotScale[selectedSlot] + 0.10f) * 20f) / 20f, MIN_SCALE, MAX_SCALE);
            saveSlot(selectedSlot);
            layoutControls();
            invalidate();
        } else if (btnRemap.contains(x, y)) {
            if (selectedSlot == S_LSTICK || selectedSlot == S_RSTICK) {
                sticksSwapped = !sticksSwapped;
                prefs.edit().putBoolean("tc_sticks_swapped", sticksSwapped).apply();
                Toast.makeText(getContext(),
                        sticksSwapped ? "Sticks swapped (Left=Camera, Right=Move)"
                                      : "Sticks default (Left=Move, Right=Camera)",
                        Toast.LENGTH_SHORT).show();
                invalidate();
            } else if (selectedSlot == S_DPAD) {
                showDpadRemapPicker();
            } else if (isRemappableButtonSlot(selectedSlot)) {
                showActionPicker(SLOT_NAMES[selectedSlot], slotAction[selectedSlot], chosen -> {
                    slotAction[selectedSlot] = chosen;
                    saveSlot(selectedSlot);
                    invalidate();
                });
            }
        } else if (btnOpacity.contains(x, y)) {
            if (opacityPercent > 80) opacityPercent = 80;
            else if (opacityPercent > 60) opacityPercent = 60;
            else if (opacityPercent > 40) opacityPercent = 40;
            else opacityPercent = 100;
            prefs.edit().putInt("tc_alpha", opacityPercent).apply();
            invalidate();
        } else if (btnReset.contains(x, y)) {
            showResetDialog();
        } else if (btnDone.contains(x, y)) {
            exitEditMode();
        }
    }

    private interface ActionChoice {
        void onChosen(Action action);
    }

    private void showActionPicker(String title, Action current, ActionChoice cb) {
        final Action[] all = Action.values();
        String[] items = new String[all.length];
        int checked = 0;
        for (int i = 0; i < all.length; i++) {
            items[i] = all[i].menuLabel;
            if (all[i] == current) checked = i;
        }
        new AlertDialog.Builder(getContext())
                .setTitle("Remap " + title)
                .setSingleChoiceItems(items, checked, (dialog, which) -> {
                    cb.onChosen(all[which]);
                    dialog.dismiss();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void showDpadRemapPicker() {
        String[] dirs = {
            "D-Pad Up -> " + dpadAction[0].menuLabel,
            "D-Pad Down -> " + dpadAction[1].menuLabel,
            "D-Pad Left -> " + dpadAction[2].menuLabel,
            "D-Pad Right -> " + dpadAction[3].menuLabel
        };
        new AlertDialog.Builder(getContext())
                .setTitle("Choose D-Pad Direction to Remap")
                .setItems(dirs, (dialog, which) -> {
                    final String[] dirNames = {"D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"};
                    showActionPicker(dirNames[which], dpadAction[which], chosen -> {
                        dpadAction[which] = chosen;
                        saveDpadActions();
                        invalidate();
                    });
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void showResetDialog() {
        String[] opts = {
            "Reset " + SLOT_NAMES[selectedSlot] + " only",
            "Reset ALL layout, sizes & button remappings"
        };
        new AlertDialog.Builder(getContext())
                .setTitle("Reset Touch Controls")
                .setItems(opts, (dialog, which) -> {
                    if (which == 0) {
                        resetSlotToDefault(selectedSlot);
                        Toast.makeText(getContext(), SLOT_NAMES[selectedSlot] + " reset", Toast.LENGTH_SHORT).show();
                    } else {
                        resetAllToDefault();
                        Toast.makeText(getContext(), "All touch controls reset to default", Toast.LENGTH_SHORT).show();
                    }
                })
                .setNegativeButton("Cancel", null)
                .show();
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

    private Action actionForHit(int hitId) {
        if (hitId >= H_DUP && hitId <= H_DRIGHT) return dpadAction[hitId - H_DUP];
        if (hitId >= 0 && hitId < SLOT_COUNT && isRemappableButtonSlot(hitId)) return slotAction[hitId];
        return Action.NONE;
    }

    private int hitGameplay(float x, float y) {
        if (bubbleVisible && slotRect[S_BUBBLE].contains(x, y)) return S_BUBBLE;
        if (!controlsVisible) return -1;

        for (int s = S_LT; s <= S_START; s++) {
            if (slotAction[s] != Action.NONE && slotRect[s].contains(x, y)) return s;
        }
        int[] dpadHits = {H_DUP, H_DDOWN, H_DLEFT, H_DRIGHT};
        for (int i = 0; i < 4; i++) {
            if (dpadAction[i] != Action.NONE && dpad[i].contains(x, y)) return dpadHits[i];
        }
        if (slotAction[S_L3] != Action.NONE && dist(x, y, cx[S_L3], cy[S_L3]) <= halfW[S_L3] * 1.25f) return S_L3;
        if (slotAction[S_R3] != Action.NONE && dist(x, y, cx[S_R3], cy[S_R3]) <= halfW[S_R3] * 1.25f) return S_R3;
        for (int s = S_A; s <= S_Y; s++) {
            if (slotAction[s] != Action.NONE && dist(x, y, cx[s], cy[s]) <= halfW[s] * 1.20f) return s;
        }
        if (dist(x, y, cx[S_LSTICK], cy[S_LSTICK]) <= halfW[S_LSTICK] * 1.25f) return S_LSTICK;
        if (dist(x, y, cx[S_RSTICK], cy[S_RSTICK]) <= halfW[S_RSTICK] * 1.25f) return S_RSTICK;
        return -1;
    }

    private int hitEditSlot(float x, float y) {
        // Check smaller/top buttons first so they are easy to pick even near sticks.
        if (slotRect[S_BUBBLE].contains(x, y)) return S_BUBBLE;
        for (int s = S_LT; s <= S_START; s++) {
            if (slotRect[s].contains(x, y)) return s;
        }
        if (dist(x, y, cx[S_L3], cy[S_L3]) <= Math.max(halfW[S_L3] * 1.35f, 24f * density)) return S_L3;
        if (dist(x, y, cx[S_R3], cy[S_R3]) <= Math.max(halfW[S_R3] * 1.35f, 24f * density)) return S_R3;
        for (int s = S_A; s <= S_Y; s++) {
            if (dist(x, y, cx[s], cy[s]) <= halfW[s] * 1.15f) return s;
        }
        if (slotRect[S_DPAD].contains(x, y)) return S_DPAD;
        if (dist(x, y, cx[S_LSTICK], cy[S_LSTICK]) <= halfW[S_LSTICK] * 1.15f) return S_LSTICK;
        if (dist(x, y, cx[S_RSTICK], cy[S_RSTICK]) <= halfW[S_RSTICK] * 1.15f) return S_RSTICK;
        return -1;
    }

    private static float dist(float x, float y, float tx, float ty) {
        float dx = x - tx, dy = y - ty;
        return (float) Math.sqrt(dx * dx + dy * dy);
    }

    private static float clamp(float v, float lo, float hi) {
        return Math.max(lo, Math.min(hi, v));
    }

    private void updateControl(int hitId, float x, float y) {
        if (hitId == S_LSTICK || hitId == S_RSTICK) {
            float r = Math.max(halfW[hitId] * 0.75f, 1f);
            float dx = clamp((x - cx[hitId]) / r, -1f, 1f);
            float dy = clamp((y - cy[hitId]) / r, -1f, 1f);
            int sx = (int) (dx * 32767f);
            int sy = (int) (-dy * 32767f);
            boolean drivesLeft = (hitId == S_LSTICK) ^ sticksSwapped;
            if (drivesLeft) {
                lx = sx;
                ly = sy;
            } else {
                rx = sx;
                ry = sy;
            }
        }
        send();
        invalidate();
    }

    private void clearControl(int hitId) {
        if (hitId == S_LSTICK || hitId == S_RSTICK) {
            boolean drivesLeft = (hitId == S_LSTICK) ^ sticksSwapped;
            if (drivesLeft) {
                lx = ly = 0;
            } else {
                rx = ry = 0;
            }
        }
    }

    private void send() {
        int b = extraFaceButtons, a = 0, bb = 0, x = 0, y = 0, black = 0, white = 0, lt = 0, rt = 0;
        if (!editMode) {
            for (int i = 0; i < active.size(); i++) {
                Action act = actionForHit(active.valueAt(i).hitId);
                switch (act) {
                    case D_UP: b |= PAD_UP; break;
                    case D_DOWN: b |= PAD_DOWN; break;
                    case D_LEFT: b |= PAD_LEFT; break;
                    case D_RIGHT: b |= PAD_RIGHT; break;
                    case START: b |= PAD_START; break;
                    case BACK: b |= PAD_BACK; break;
                    case L3: b |= PAD_L3; break;
                    case R3: b |= PAD_R3; break;
                    case A: a = 255; b |= 0x1000; break;
                    case B: bb = 255; b |= 0x2000; break;
                    case X: x = 255; b |= 0x4000; break;
                    case Y: y = 255; b |= 0x8000; break;
                    case LB: white = 255; break;
                    case RB: black = 255; break;
                    case LT: lt = 255; break;
                    case RT: rt = 255; break;
                    default: break;
                }
            }
        }
        buttons = b;
        analog[0] = a; analog[1] = bb; analog[2] = x; analog[3] = y;
        analog[4] = black; analog[5] = white; analog[6] = lt; analog[7] = rt;
        try {
            NobleActivity.nativeSetTouchPad(b, a, bb, x, y, black, white, lt, rt, lx, ly, rx, ry,
                    (controlsVisible && !menuOpen && !editMode) || extraFaceButtons != 0);
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
