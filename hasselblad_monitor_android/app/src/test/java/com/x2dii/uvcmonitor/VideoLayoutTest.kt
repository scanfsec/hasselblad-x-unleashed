package com.x2dii.uvcmonitor

import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import org.junit.Assert.*
import org.junit.Test

class VideoLayoutTest {
    @Test fun portraitUsesFullWidthWithoutStretchingVideo() {
        val r = fittedVideoRect(4f / 3f, Size(1080f, 2400f))
        assertEquals(1080f, r.width, .01f)
        assertEquals(810f, r.height, .01f)
        assertEquals(1200f, r.center.y, .01f)
    }

    @Test fun landscapeUsesFullHeightWithoutCroppingVideo() {
        val r = fittedVideoRect(4f / 3f, Size(2400f, 1080f))
        assertEquals(1440f, r.width, .01f)
        assertEquals(1080f, r.height, .01f)
        assertEquals(1200f, r.center.x, .01f)
    }

    @Test fun invalidOrUnmeasuredSurfaceHasNoVideoRect() {
        assertEquals(Rect.Zero, fittedVideoRect(4f / 3f, Size(1080f, 0f)))
        assertEquals(Rect.Zero, fittedVideoRect(Float.NaN, Size(1080f, 2400f)))
    }
}
