package com.serenegiant.utils;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;

/**
 * 补丁类：AndroidUSBCamera 的 USBMonitor 运行时依赖 common 库里的这个类，
 * 该库未发布到任何仓（exclude 掉了传递依赖），按原语义同包名补齐。
 */
public class HandlerThreadHandler extends Handler {

    private HandlerThreadHandler(final HandlerThread thread) {
        super(thread.getLooper());
    }

    public static final HandlerThreadHandler createHandler(final String name) {
        return createHandler(name, Process.THREAD_PRIORITY_DEFAULT);
    }

    public static final HandlerThreadHandler createHandler(final String name, final int priority) {
        final HandlerThread thread = new HandlerThread(name, priority);
        thread.start();
        return new HandlerThreadHandler(thread);
    }
}
