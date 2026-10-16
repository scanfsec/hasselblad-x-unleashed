package com.x2dii.uvcmonitor

import org.junit.Assert.*
import org.junit.Test

class PreviewEncodingTest {
    @Test fun uncompressedCameraUsesYuyvWithoutMjpegRequest() {
        val calls = mutableListOf<PreviewEncoding>()
        assertEquals(PreviewEncoding.YUYV, negotiatePreview { calls.add(it) })
        assertEquals(listOf(PreviewEncoding.YUYV), calls)
    }

    @Test fun unsupportedYuyvFallsBackToMjpeg() {
        val calls = mutableListOf<PreviewEncoding>()
        val selected = negotiatePreview {
            calls.add(it)
            if (it == PreviewEncoding.YUYV) throw IllegalArgumentException("unsupported")
        }
        assertEquals(PreviewEncoding.MJPEG, selected)
        assertEquals(listOf(PreviewEncoding.YUYV, PreviewEncoding.MJPEG), calls)
    }

    @Test fun bothUnsupportedPreservesBothErrors() {
        val first = IllegalArgumentException("YUYV")
        val second = IllegalArgumentException("MJPEG")
        val actual = assertThrows(IllegalArgumentException::class.java) {
            negotiatePreview { throw if (it == PreviewEncoding.YUYV) first else second }
        }
        assertSame(second, actual)
        assertSame(first, actual.suppressed.single())
    }

    @Test fun permissionFailureDoesNotRetryFormat() {
        var calls = 0
        assertThrows(SecurityException::class.java) {
            negotiatePreview { calls++; throw SecurityException("permission denied") }
        }
        assertEquals(1, calls)
    }

    @Test fun yuyvHasFullUncompressedBandwidth() {
        assertEquals(0, PreviewEncoding.YUYV.frameFormat)
        assertEquals(1, PreviewEncoding.MJPEG.frameFormat)
        assertEquals(1.0f, PreviewEncoding.YUYV.bandwidth, 0f)
    }
}
