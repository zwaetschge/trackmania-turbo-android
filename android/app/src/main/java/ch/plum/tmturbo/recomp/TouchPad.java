package ch.plum.tmturbo.recomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen gamepad for devices without one (tablets, phones): a floating analog stick on
 * the left, GAS / BRAKE and A, B, Y, START on the right. It reports an XInput state
 * (buttons, triggers, left stick) through {@link Listener}. Touches that hit no control
 * fall through to the game surface below (= the mouse cursor, for tapping menu entries).
 */
public class TouchPad extends View {
    public interface Listener { void onPad(int buttons, int lt, int rt, int lx, int ly); }

    static final int A = 0x1000, B = 0x2000, Y = 0x8000, START = 0x0010;
    static final int GAS = -1, BRAKE = -2;

    /** A round button: centre and radius as fractions of the view height (x from the right edge). */
    static final class Btn {
        final String label; final int bit; final float fx, fy, fr; float cx, cy, r; int pointer = -1;
        Btn(String label, int bit, float fx, float fy, float fr) { this.label = label; this.bit = bit; this.fx = fx; this.fy = fy; this.fr = fr; }
    }
    private final Btn[] btns = {
            // a column at the right edge (the game's menus sit centre-right) and GAS/BREMSE below
            new Btn("GAS", GAS, 0.17f, 0.80f, 0.14f),
            new Btn("BREMSE", BRAKE, 0.44f, 0.87f, 0.09f),
            new Btn("A", A, 0.09f, 0.53f, 0.068f),
            new Btn("B", B, 0.09f, 0.36f, 0.062f),
            new Btn("Y", Y, 0.09f, 0.20f, 0.055f),
            new Btn("☰", START, 0.26f, 0.08f, 0.05f),
    };
    private final Listener listener;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), ring = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int stickPointer = -1;
    private float stickX0, stickY0, stickX, stickY;
    private int lastButtons, lastLt, lastRt, lastLx, lastLy;

    public TouchPad(Context c, Listener l) {
        super(c);
        listener = l;
        ring.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create(Typeface.DEFAULT_BOLD, Typeface.BOLD));
        setHapticFeedbackEnabled(true);
    }

    private float stickRadius() { return getHeight() * 0.14f; }
    /** The stick may start anywhere in the lower left part of the screen. */
    private boolean inStickZone(float x, float y) { return x < getWidth() * 0.40f && y > getHeight() * 0.40f; }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        for (Btn b : btns) { b.r = b.fr * h; b.cx = w - b.fx * h; b.cy = b.fy * h; }
        ring.setStrokeWidth(Math.max(2f, h * 0.004f));
    }

    private Btn hit(float x, float y) {
        for (Btn b : btns) { float dx = x - b.cx, dy = y - b.cy, rr = b.r * 1.25f; if (dx * dx + dy * dy <= rr * rr) return b; }
        return null;
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int act = e.getActionMasked(), idx = e.getActionIndex(), id = e.getPointerId(idx);
        switch (act) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(idx), y = e.getY(idx);
                Btn b = hit(x, y);
                if (b != null && b.pointer < 0) { b.pointer = id; performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY); }
                else if (b == null && stickPointer < 0 && inStickZone(x, y)) { stickPointer = id; stickX0 = stickX = x; stickY0 = stickY = y; }
                else if (act == MotionEvent.ACTION_DOWN) return false;   // not ours: the surface gets it (mouse)
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int pid = e.getPointerId(i);
                    if (pid == stickPointer) { stickX = e.getX(i); stickY = e.getY(i); }
                    else {   // a finger may slide from one button to another (e.g. GAS -> BRAKE)
                        Btn now = hit(e.getX(i), e.getY(i));
                        for (Btn b : btns) if (b.pointer == pid && b != now && now != null && now.pointer < 0) { b.pointer = -1; now.pointer = pid; }
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (act == MotionEvent.ACTION_CANCEL || act == MotionEvent.ACTION_UP) { stickPointer = -1; for (Btn b : btns) b.pointer = -1; }
                else { if (id == stickPointer) stickPointer = -1; for (Btn b : btns) if (b.pointer == id) b.pointer = -1; }
                break;
        }
        report();
        invalidate();
        return true;
    }

    private void report() {
        int buttons = 0, lt = 0, rt = 0, lx = 0, ly = 0;
        for (Btn b : btns) if (b.pointer >= 0) { if (b.bit == GAS) rt = 255; else if (b.bit == BRAKE) lt = 255; else buttons |= b.bit; }
        if (stickPointer >= 0) {
            float r = stickRadius(), dx = (stickX - stickX0) / r, dy = (stickY - stickY0) / r;
            float len = (float) Math.sqrt(dx * dx + dy * dy);
            if (len > 1f) { dx /= len; dy /= len; }
            lx = Math.round(dx * 32767f); ly = Math.round(-dy * 32767f);
        }
        if (buttons != lastButtons || lt != lastLt || rt != lastRt || lx != lastLx || ly != lastLy) {
            lastButtons = buttons; lastLt = lt; lastRt = rt; lastLx = lx; lastLy = ly;
            listener.onPad(buttons, lt, rt, lx, ly);
        }
    }

    /** Releases everything (e.g. when the view is hidden while a finger is down). */
    public void reset() { stickPointer = -1; for (Btn b : btns) b.pointer = -1; report(); invalidate(); }

    @Override protected void onDraw(Canvas c) {
        int h = getHeight();
        // stick: where it is held, or a faint hint in its corner
        float r = stickRadius();
        float bx = stickPointer >= 0 ? stickX0 : h * 0.30f, by = stickPointer >= 0 ? stickY0 : h * 0.72f;
        fill.setColor(Color.argb(stickPointer >= 0 ? 60 : 30, 255, 255, 255)); c.drawCircle(bx, by, r, fill);
        ring.setColor(Color.argb(stickPointer >= 0 ? 150 : 70, 255, 255, 255)); c.drawCircle(bx, by, r, ring);
        float kx = bx, ky = by;
        if (stickPointer >= 0) {
            float dx = stickX - stickX0, dy = stickY - stickY0, len = (float) Math.sqrt(dx * dx + dy * dy);
            if (len > r) { dx = dx * r / len; dy = dy * r / len; }
            kx += dx; ky += dy;
        }
        fill.setColor(Color.argb(stickPointer >= 0 ? 170 : 70, 255, 255, 255)); c.drawCircle(kx, ky, r * 0.42f, fill);
        for (Btn b : btns) {
            boolean on = b.pointer >= 0;
            int base = b.bit == GAS ? Color.rgb(255, 196, 0) : b.bit == BRAKE ? Color.rgb(230, 70, 60) : Color.WHITE;
            fill.setColor((on ? 150 : 45) << 24 | (base & 0xFFFFFF)); c.drawCircle(b.cx, b.cy, b.r, fill);
            ring.setColor((on ? 230 : 110) << 24 | (base & 0xFFFFFF)); c.drawCircle(b.cx, b.cy, b.r, ring);
            text.setColor(Color.argb(on ? 255 : 190, 255, 255, 255));
            text.setTextSize(b.label.length() > 2 ? b.r * 0.42f : b.r * 0.8f);
            c.drawText(b.label, b.cx, b.cy - (text.descent() + text.ascent()) / 2, text);
        }
    }
}
