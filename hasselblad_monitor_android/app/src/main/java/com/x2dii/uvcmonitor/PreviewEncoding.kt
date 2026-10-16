package com.x2dii.uvcmonitor

internal enum class PreviewEncoding(val frameFormat: Int) {
    YUYV(0), MJPEG(1);

    val bandwidth: Float get() = 1.0f
}

/** Prefer the camera gadget's uncompressed stream; retry only unsupported formats. */
internal fun negotiatePreview(configure: (PreviewEncoding) -> Unit): PreviewEncoding {
    try {
        configure(PreviewEncoding.YUYV)
        return PreviewEncoding.YUYV
    } catch (unsupported: IllegalArgumentException) {
        try {
            configure(PreviewEncoding.MJPEG)
            return PreviewEncoding.MJPEG
        } catch (fallback: IllegalArgumentException) {
            fallback.addSuppressed(unsupported)
            throw fallback
        }
    }
}
