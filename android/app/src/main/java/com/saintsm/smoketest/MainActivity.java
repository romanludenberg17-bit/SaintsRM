package com.saintsm.smoketest;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.TextView;
import android.widget.LinearLayout;
import android.graphics.Color;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("saintsrm_smoke");
    }

    private SurfaceView surfaceView;
    private TextView status;

    private static native void nativeSetSurface(Surface surface);
    private static native void nativeClearSurface();
    private static native String nativeRunSmokeTests();
    private static native void nativeOnTrimMemory(int level);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(14);
        status.setPadding(24, 16, 24, 16);
        status.setText("SaintsRM smoke-test: starting...");
        root.addView(status, new LinearLayout.LayoutParams(-1, -2));

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);

        String result = nativeRunSmokeTests();
        status.setText(result);
        writeSmokeReport(result);
    }

    private void writeSmokeReport(String result) {
        File report = new File(getFilesDir(), "saintsrm-smoke.txt");
        try (FileOutputStream out = new FileOutputStream(report, false)) {
            out.write(result.getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) {
            // The on-screen result and logcat remain available if file output fails.
        }
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        nativeSetSurface(holder.getSurface());
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSetSurface(holder.getSurface());
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        nativeClearSurface();
    }

    @Override protected void onPause() {
        nativeClearSurface();
        super.onPause();
    }

    @Override protected void onResume() {
        super.onResume();
        if (surfaceView.getHolder().getSurface().isValid()) {
            nativeSetSurface(surfaceView.getHolder().getSurface());
        }
    }

    @Override public void onTrimMemory(int level) {
        nativeOnTrimMemory(level);
        super.onTrimMemory(level);
    }
}
