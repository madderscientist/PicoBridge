package com.madderscientist.picobridge;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.RectF;
import android.util.Log;

/**
 * VR 内的配置面板。
 * 画在一张 ARGB_8888 Bitmap 上，native 侧锁像素上传成 GL 纹理后当作四边形画进投影层。
 * 不走 XR_KHR_android_surface_swapchain：PICO 对任何 format/usage 组合都返回 VALIDATION_FAILURE。
 * 纯 Canvas 绘制 + 自己做命中测试：不接 View 框架，因而不需要 Looper / UI 线程 / 窗口。
 */
public final class VrPanel {

    private static final String TAG = "PicoBridge/panel";
    private static final String REPO = "github.com/madderscientist/PicoBridge";
    private static final String DEFAULT_HOST = "192.168.137.63:8000";

    // 每页 4 行，各行列数可以不同，按行宽均分
    private static final String[][] DIGITS = {
            {"1", "2", "3", "⌫"},
            {"4", "5", "6", "."},
            {"7", "8", "9", ":"},
            {"ABC", "0", "默认", "连接"}};

    private static final String[][] LETTERS = {
            {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
            {"a", "s", "d", "f", "g", "h", "j", "k", "l", "-"},
            {"z", "x", "c", "v", "b", "n", "m", ".", ":", "⌫"},
            {"123", "清空", "默认", "连接"}};

    private static Bitmap bitmap;
    private static Canvas canvas;
    private static int width, height;
    private static String host = DEFAULT_HOST;
    private static boolean connected = false;
    private static boolean letterPage = false;
    private static int pressedRow = -1, pressedCol = -1;

    private static RectF[][] rects;
    private static final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private VrPanel() {}

    public static Bitmap create(int w, int h, String initialHost) {
        width = w;
        height = h;
        bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        canvas = new Canvas(bitmap);
        if (initialHost != null && !initialHost.isEmpty()) host = initialHost;
        layout();
        Log.i(TAG, "panel created " + w + "x" + h + " host=" + host);
        return bitmap;
    }

    private static String[][] page() {
        return letterPage ? LETTERS : DIGITS;
    }

    /** 键盘占下半部分；每行按自己的列数均分，所以末行可以是宽键。 */
    private static void layout() {
        final String[][] p = page();
        final float top = height * 0.42f, pad = width * 0.012f;
        final float rowH = (height - top - pad * (p.length + 1)) / p.length;
        rects = new RectF[p.length][];
        for (int r = 0; r < p.length; r++) {
            rects[r] = new RectF[p[r].length];
            float cw = (width - pad * (p[r].length + 1)) / p[r].length;
            float y = top + pad + r * (rowH + pad);
            for (int c = 0; c < p[r].length; c++) {
                float x = pad + c * (cw + pad);
                rects[r][c] = new RectF(x, y, x + cw, y + rowH);
            }
        }
    }

    public static String url() {
        return "ws://" + host + "/ws/device";
    }

    /** action: 0=DOWN 1=MOVE 2=UP，坐标为位图像素。 */
    public static void pointer(float x, float y, int action) {
        if (action == 0) {
            pressedRow = -1;
            pressedCol = -1;
            for (int r = 0; r < rects.length; r++) {
                for (int c = 0; c < rects[r].length; c++) {
                    if (rects[r][c].contains(x, y)) {
                        pressedRow = r;
                        pressedCol = c;
                    }
                }
            }
        } else if (action == 2) {
            if (pressedRow >= 0 && pressedCol >= 0 && rects[pressedRow][pressedCol].contains(x, y)) {
                apply(page()[pressedRow][pressedCol]);
            }
            pressedRow = -1;
            pressedCol = -1;
        }
    }

    private static void apply(String key) {
        switch (key) {
            case "⌫":
                if (!host.isEmpty()) host = host.substring(0, host.length() - 1);
                break;
            case "清空":
                host = "";
                break;
            case "默认":
                host = DEFAULT_HOST;
                break;
            case "ABC":
            case "123":
                letterPage = !letterPage;
                layout();
                break;
            case "连接":
                nativeOnConnect(url());
                break;
            default:
                if (host.length() < 32) host += key;
                break;
        }
    }

    public static void render(boolean isConnected) {
        connected = isConnected;
        if (canvas == null) return;
        try {
            draw(canvas);
        } catch (Exception e) {
            Log.w(TAG, "draw failed: " + e);
        }
    }

    private static void draw(Canvas c) {
        c.drawColor(0, PorterDuff.Mode.CLEAR);

        final float r = width * 0.02f;
        final float pad = width * 0.04f;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xF01A1D23);
        c.drawRoundRect(new RectF(0, 0, width, height), r, r, paint);

        paint.setColor(Color.WHITE);
        paint.setTextSize(height * 0.070f);
        c.drawText("PicoBridge", pad, height * 0.095f, paint);

        paint.setColor(0xFF6AA9FF);
        paint.setTextSize(height * 0.036f);
        c.drawText(REPO, pad, height * 0.150f, paint);

        paint.setColor(connected ? 0xFF3DD68C : 0xFFE05252);
        c.drawCircle(width - pad - height * 0.028f, height * 0.082f, height * 0.020f, paint);
        paint.setTextSize(height * 0.036f);
        String status = connected ? "已连接" : "未连接";
        c.drawText(status, width - pad - height * 0.065f - paint.measureText(status),
                height * 0.095f, paint);

        RectF box = new RectF(pad, height * 0.21f, width - pad, height * 0.32f);
        paint.setColor(0xFF0D0F12);
        c.drawRoundRect(box, r * 0.6f, r * 0.6f, paint);
        paint.setColor(Color.WHITE);
        paint.setTextSize(height * 0.052f);
        c.drawText(host.isEmpty() ? "_" : host, box.left + pad * 0.5f, box.bottom - height * 0.032f,
                paint);

        paint.setColor(0xFF8B95A3);
        paint.setTextSize(height * 0.032f);
        c.drawText(url(), pad, height * 0.375f, paint);

        final String[][] p = page();
        for (int row = 0; row < p.length; row++) {
            for (int col = 0; col < p[row].length; col++) {
                final String key = p[row][col];
                final RectF rc = rects[row][col];
                final boolean isGo = "连接".equals(key);
                final boolean down = row == pressedRow && col == pressedCol;
                paint.setColor(down ? 0xFF4A5160 : (isGo ? 0xFF2F6FEB : 0xFF2B303A));
                c.drawRoundRect(rc, r * 0.4f, r * 0.4f, paint);

                paint.setColor(Color.WHITE);
                paint.setTextSize(rc.height() * 0.40f);
                Paint.FontMetrics fm = paint.getFontMetrics();
                c.drawText(key, rc.centerX() - paint.measureText(key) / 2f,
                        rc.centerY() - (fm.ascent + fm.descent) / 2f, paint);
            }
        }
    }

    private static native void nativeOnConnect(String url);
}
