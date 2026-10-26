package com.saintsm.smoketest;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.graphics.Color;
import android.graphics.Typeface;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("saintsrm_smoke");
    }

    private SurfaceView surfaceView;
    private TextView status;
    private String reportText = "";

    private static native void nativeSetSurface(Surface surface);
    private static native void nativeClearSurface();
    private static native String nativeRunSmokeTests();
    private static native String nativeGetVulkanReport();
    private static native void nativeOnTrimMemory(int level);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);

        Button share = new Button(this);
        share.setText("Поделиться отчётом");
        share.setOnClickListener(v -> shareReport());
        root.addView(share, new LinearLayout.LayoutParams(-1, -2));

        ScrollView reportScroll = new ScrollView(this);
        reportScroll.setFillViewport(true);

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(14);
        status.setTypeface(Typeface.MONOSPACE);
        status.setPadding(24, 16, 24, 16);
        status.setTextIsSelectable(true);
        status.setText("SaintsRM smoke-test: starting...");
        reportScroll.addView(status, new ScrollView.LayoutParams(-1, -2));
        root.addView(reportScroll, new LinearLayout.LayoutParams(-1, 0, 1));

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new LinearLayout.LayoutParams(-1, 1));
        setContentView(root);

        String nativeReport = nativeRunSmokeTests();
        String vulkanReport = nativeGetVulkanReport();
        reportText = buildDeviceReport(nativeReport, vulkanReport);
        status.setText(reportText);
        writeSmokeReport(reportText);
    }

    private String buildDeviceReport(String nativeReport, String vulkanReport) {
        StringBuilder out = new StringBuilder();
        out.append("Android model: ").append(Build.MODEL).append('\n');
        out.append("Android manufacturer: ").append(Build.MANUFACTURER).append('\n');
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            out.append("SoC model: ").append(Build.SOC_MODEL).append('\n');
        } else {
            out.append("SoC model: unavailable (API < 31)\n");
        }
        out.append("Android release: ").append(Build.VERSION.RELEASE).append('\n');
        out.append("Android SDK_INT: ").append(Build.VERSION.SDK_INT).append('\n');
        out.append('\n');
        out.append(nativeReport);
        out.append('\n');
        out.append(vulkanReport);
        return out.toString();
    }

    private void shareReport() {
        if (reportText.isEmpty()) {
            reportText = status.getText().toString();
        }
        Intent intent = new Intent(Intent.ACTION_SEND);
        intent.setType("text/plain");
        intent.putExtra(Intent.EXTRA_SUBJECT, "SaintsRM smoke-test report");
        intent.putExtra(Intent.EXTRA_TEXT, reportText);
        startActivity(Intent.createChooser(intent, "Поделиться отчётом"));
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
