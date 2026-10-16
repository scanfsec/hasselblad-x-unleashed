package com.x2dii.uvcmonitor;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.Bundle;
import java.io.FileOutputStream;
import java.util.Arrays;
import com.serenegiant.usb.UVCCamera;

/** Exercises real receiver teardown, including the Activity's onDestroy callback. */
public class LifecycleInstrumentation extends Instrumentation {
    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            // JNI_OnLoad must register the replacement library against the
            // actual bundled Java class, including its complete method table.
            UVCCamera nativeCamera = new UVCCamera();
            nativeCamera.destroy();
            for (int round = 0; round < 2; round++) {
                Intent intent = new Intent(getTargetContext(), MainActivity.class);
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                Activity activity = startActivitySync(intent);
                if (round == 0) verifyRotationAndFrameClear((MainActivity) activity);
                runOnMainSync(activity::finish);
                for (int wait = 0; wait < 50 && !activity.isDestroyed(); wait++)
                    Thread.sleep(100);
                if (!activity.isDestroyed()) throw new AssertionError("Activity did not destroy");
                for (int wait = 0; wait < 50 && hasUsbWorker(); wait++) Thread.sleep(100);
                if (hasUsbWorker()) throw new AssertionError("USB worker survived Activity destroy");
            }
            result.putString("result", "PASS: portrait/landscape live GL frame, stale-frame clear, JNI registration, two Activity teardown cycles");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable failure) {
            result.putString("result", "FAIL: " + failure);
            finish(Activity.RESULT_CANCELED, result);
        }
    }

    private boolean hasUsbWorker() {
        for (Thread thread : Thread.getAllStackTraces().keySet())
            if (thread.isAlive() && thread.getName().equals("UVC-connect")) return true;
        return false;
    }

    private void verifyRotationAndFrameClear(MainActivity activity) throws Exception {
        java.lang.reflect.Field field = MainActivity.class.getDeclaredField("renderer");
        field.setAccessible(true);
        MonitorGlRenderer renderer = (MonitorGlRenderer) field.get(activity);
        byte[] green = new byte[16 * 16 * 3 / 2];
        Arrays.fill(green, 0, 16 * 16, (byte) 145);
        for (int i = 16 * 16; i < green.length; i += 2) {
            green[i] = 34; green[i + 1] = 54; // NV21 V,U
        }
        runOnMainSync(() -> activity.onConnected("Test frame", "16×16", 1f));
        renderer.submitFrame(green, 16, 16);
        for (int orientation : new int[]{ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
                                        ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE}) {
            runOnMainSync(() -> activity.setRequestedOrientation(orientation));
            int expected = orientation == ActivityInfo.SCREEN_ORIENTATION_PORTRAIT ?
                Configuration.ORIENTATION_PORTRAIT : Configuration.ORIENTATION_LANDSCAPE;
            for (int wait = 0; wait < 50 && activity.getResources().getConfiguration().orientation != expected; wait++)
                Thread.sleep(100);
            if (activity.isDestroyed() || activity.getResources().getConfiguration().orientation != expected)
                throw new AssertionError("Rotation destroyed Activity or did not resize it");
            Thread.sleep(500);
            Bitmap image = getUiAutomation().takeScreenshot();
            int pixel = image.getPixel(image.getWidth() / 2, image.getHeight() / 2);
            if (Color.green(pixel) < Color.red(pixel) + 80 || Color.green(pixel) < Color.blue(pixel) + 80)
                throw new AssertionError("Rotated GL surface lost the green test frame: " + Integer.toHexString(pixel));
            save(image, expected == Configuration.ORIENTATION_PORTRAIT ? "portrait" : "landscape");
            image.recycle();
            if (!hasUsbWorker()) throw new AssertionError("Rotation lost USB worker");
        }
        renderer.clearFrame();
        Thread.sleep(500);
        Bitmap image = getUiAutomation().takeScreenshot();
        int pixel = image.getPixel(image.getWidth() / 2, image.getHeight() / 2);
        if (Color.red(pixel) > 10 || Color.green(pixel) > 10 || Color.blue(pixel) > 10)
            throw new AssertionError("Old frame survived disconnect clear: " + Integer.toHexString(pixel));
        save(image, "cleared");
        image.recycle();
        runOnMainSync(() -> activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR));
    }

    private void save(Bitmap bitmap, String suffix) throws Exception {
        try (FileOutputStream output = new FileOutputStream("/sdcard/Download/uvc27-" + suffix + ".png")) {
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, output))
                throw new AssertionError("Could not save render evidence");
        }
    }
}
