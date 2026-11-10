package com.noblearchimedes;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.AttributeSet;
import android.view.View;

/**
 * A self-contained launch splash: the astrolabe emblem, the project name and a
 * quiet progress shimmer. It stays up until native code reports its first
 * usable frame, then fades out. Android 12+ also shows its own icon splash
 * before this view exists, so the two use the same artwork and background.
 */
public class SplashView extends View {
    private static final long MIN_VISIBLE_MS = 9000;
    private static final long MAX_VISIBLE_MS = 25000;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint glowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint logoPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final Paint titlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint subtitlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint rulePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint shimmerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private Bitmap logo;
    private long started;
    private boolean stopping;
    private float shimmer;

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (stopping) return;
            long elapsed = SystemClock.uptimeMillis() - started;
            boolean ready = false;
            try {
                ready = NobleActivity.nativeIsReady();
            } catch (UnsatisfiedLinkError ignored) {
                // Library still loading; keep the splash up.
            }
            if ((ready && elapsed >= MIN_VISIBLE_MS) || elapsed >= MAX_VISIBLE_MS) {
                fadeOut();
            } else {
                shimmer = (shimmer + 0.02f) % 1.0f;
                invalidate();
                handler.postDelayed(this, 16);
            }
        }
    };

    public SplashView(Context context) {
        super(context);
        init(context);
    }

    public SplashView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context);
    }

    private void init(Context context) {
        started = SystemClock.uptimeMillis();
        try {
            logo = BitmapFactory.decodeResource(context.getResources(), R.drawable.noble_splash_logo);
        } catch (Throwable ignored) {
            logo = null;
        }
        titlePaint.setColor(0xFFE3BE74);
        titlePaint.setTextAlign(Paint.Align.CENTER);
        titlePaint.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        titlePaint.setLetterSpacing(0.28f);

        subtitlePaint.setColor(0xFF8FA5A8);
        subtitlePaint.setTextAlign(Paint.Align.CENTER);
        subtitlePaint.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        subtitlePaint.setLetterSpacing(0.20f);

        rulePaint.setColor(0x66D8A84E);
        rulePaint.setStrokeWidth(1.5f);
        shimmerPaint.setColor(0x33D8A84E);
        setClickable(true);  // swallow taps while loading
    }

    public void stop() {
        stopping = true;
        handler.removeCallbacksAndMessages(null);
    }

    private void fadeOut() {
        stopping = true;
        handler.removeCallbacksAndMessages(null);
        animate().alpha(0f).setDuration(420).withEndAction(() -> {
            if (getParent() instanceof android.view.ViewGroup) {
                ((android.view.ViewGroup) getParent()).removeView(this);
            }
        }).start();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        started = SystemClock.uptimeMillis();
        handler.postDelayed(tick, 250);
    }

    @Override
    protected void onDetachedFromWindow() {
        stop();
        super.onDetachedFromWindow();
    }

    @Override
    protected void onDraw(Canvas c) {
        int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;

        bgPaint.setShader(new LinearGradient(0, 0, 0, h,
                new int[]{0xFF07181C, 0xFF050E11, 0xFF020708},
                new float[]{0f, 0.55f, 1f}, Shader.TileMode.CLAMP));
        c.drawRect(0, 0, w, h, bgPaint);

        float cx = w * 0.5f;
        float cy = h * 0.40f;
        glowPaint.setShader(new RadialGradient(cx, cy, Math.min(w, h) * 0.42f,
                new int[]{0x33298A91, 0x111B656C, 0x00000000},
                new float[]{0f, 0.55f, 1f}, Shader.TileMode.CLAMP));
        c.drawCircle(cx, cy, Math.min(w, h) * 0.42f, glowPaint);

        float side = Math.min(w, h) * 0.40f;
        if (logo != null) {
            float left = cx - side / 2f;
            float top = cy - side / 2f;
            c.drawBitmap(logo, null, new android.graphics.RectF(left, top, left + side, top + side), logoPaint);
        }

        titlePaint.setTextSize(Math.min(w, h) * 0.050f);
        c.drawText("NOBLE ARCHIMEDES", cx, h * 0.70f, titlePaint);

        float ruleW = Math.min(w * 0.36f, h * 0.62f);
        c.drawLine(cx - ruleW / 2f, h * 0.725f, cx + ruleW / 2f, h * 0.725f, rulePaint);

        subtitlePaint.setTextSize(Math.min(w, h) * 0.020f);
        c.drawText("CASTLEVANIA  \u00B7  CURSE OF DARKNESS", cx, h * 0.765f, subtitlePaint);

        // A small travelling glint under the title reads as progress without a bar.
        float sw = Math.min(w * 0.22f, h * 0.38f);
        float sx = cx - sw / 2f + sw * shimmer;
        c.drawCircle(sx, h * 0.825f, 2.0f * getResources().getDisplayMetrics().density, shimmerPaint);
    }
}
