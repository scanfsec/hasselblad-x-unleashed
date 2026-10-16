package com.serenegiant.utils;

import android.os.Build;

/**
 * 补丁类：同 HandlerThreadHandler——USBMonitor 需要 isAndroid5/isMarshmallow
 * 两个版本闸门（分别守 getSerialNumber/getVersion 的调用），按原语义补齐。
 */
public final class BuildCheck {

    private BuildCheck() {
    }

    public static final boolean isAndroid5() {
        return Build.VERSION.SDK_INT >= 21;   // LOLLIPOP
    }

    public static final boolean isLollipop() {
        return isAndroid5();
    }

    public static final boolean isMarshmallow() {
        return Build.VERSION.SDK_INT >= 23;
    }
}
