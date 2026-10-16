package com.x2dii.uvcmonitor

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AspectRatio
import androidx.compose.material.icons.outlined.BarChart
import androidx.compose.material.icons.outlined.CenterFocusStrong
import androidx.compose.material.icons.outlined.Contrast
import androidx.compose.material.icons.outlined.GridView
import androidx.compose.material.icons.outlined.LightMode
import androidx.compose.material.icons.outlined.Palette
import androidx.compose.material.icons.outlined.Pause
import androidx.compose.material.icons.outlined.PhotoCamera
import androidx.compose.material3.Icon
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import android.opengl.GLSurfaceView
import kotlinx.coroutines.delay

val HasselbladOrange = Color(0xFFFF6B00)

/** aspect-fit：视频完整显示在 size 内的矩形（与 GL 信箱公式一致） */
fun fittedVideoRect(aspect: Float, size: Size): Rect {
    if (!aspect.isFinite() || aspect <= 0f || size.width <= 0f || size.height <= 0f) return Rect.Zero
    val w = size.width
    val h = w / aspect
    return if (h <= size.height) {
        Rect(0f, (size.height - h) / 2f, w, (size.height + h) / 2f)
    } else {
        val hh = size.height
        val ww = hh * aspect
        Rect((size.width - ww) / 2f, 0f, (size.width + ww) / 2f, hh)
    }
}

fun inscribedRect(aspect: Float, r: Rect): Rect {
    var w = r.width
    var h = w / aspect
    if (h > r.height) {
        h = r.height
        w = h * aspect
    }
    return Rect(r.center.x - w / 2f, r.center.y - h / 2f, r.center.x + w / 2f, r.center.y + h / 2f)
}

@Composable
fun MonitorScreen(
    settings: MonitorSettings,
    engine: UvcEngine,
    renderer: MonitorGlRenderer,
    connected: Boolean,
    statusText: String,
    deviceName: String,
    formatDesc: String,
    fps: Int,
    videoAspect: Float,
    histogram: HistogramData?,
    toast: String?,
    onCapture: () -> Unit
) {
    LaunchedEffect(settings.frozen) { engine.frozen = settings.frozen }
    LaunchedEffect(settings.histogramOn) { engine.histogramOn = settings.histogramOn }

    var flash by remember { mutableStateOf(false) }
    LaunchedEffect(flash) {
        if (flash) {
            delay(120)
            flash = false
        }
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .pointerInput(Unit) {
                detectTapGestures(
                    onTap = { settings.uiVisible = !settings.uiVisible },
                    onDoubleTap = {
                        settings.zoom = 1f; settings.panX = 0f; settings.panY = 0f
                    }
                )
            }
            .pointerInput(Unit) {
                detectTransformGestures { _, pan, gestureZoom, _ ->
                    val nz = (settings.zoom * gestureZoom).coerceIn(1f, 8f)
                    settings.zoom = nz
                    val span = 1f - 1f / nz
                    settings.panX = (settings.panX - pan.x / 400f * span).coerceIn(-1f, 1f)
                    settings.panY = (settings.panY - pan.y / 400f * span).coerceIn(-1f, 1f)
                }
            }
    ) {
        // Keep one GL surface across disconnect/status transitions. Its frame
        // mailbox is cleared on native teardown, so old sessions cannot linger.
        AndroidView(
                factory = { ctx ->
                    GLSurfaceView(ctx).apply {
                        setEGLContextClientVersion(2)
                        setRenderer(renderer)
                        renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
                    }
                },
                update = { renderer.apply(settings) },
                modifier = Modifier.fillMaxSize()
        )
        if (connected) {
            // 网格 + 画幅遮罩（几何与 GL 信箱对齐）
            Canvas(Modifier.fillMaxSize()) {
                val video = fittedVideoRect(videoAspect, size)
                if (video.width > 2f) {
                    settings.aspectMode.ratio?.let { ratio ->
                        val t = inscribedRect(ratio, video)
                        val p = Path().apply {
                            fillType = PathFillType.EvenOdd
                            addRect(Rect(Offset.Zero, size))
                            addRect(t)
                        }
                        drawPath(p, Color.Black.copy(alpha = 0.65f))
                        drawRect(
                            Color.White.copy(alpha = 0.9f),
                            topLeft = t.topLeft, size = t.size,
                            style = Stroke(width = 1.dp.toPx())
                        )
                    }
                    if (settings.gridMode != GridMode.Off) {
                        val stroke = Color.White.copy(alpha = 0.55f)
                        val divisions = if (settings.gridMode == GridMode.Golden)
                            listOf(0.382f, 0.618f) else listOf(1f / 3f, 2f / 3f)
                        when (settings.gridMode) {
                            GridMode.Thirds, GridMode.Golden -> for (f in divisions) {
                                drawLine(stroke, Offset(video.left + video.width * f, video.top),
                                    Offset(video.left + video.width * f, video.bottom), 0.8f.dp.toPx())
                                drawLine(stroke, Offset(video.left, video.top + video.height * f),
                                    Offset(video.right, video.top + video.height * f), 0.8f.dp.toPx())
                            }
                            GridMode.Diagonals -> {
                                drawLine(stroke, video.topLeft, video.bottomRight, 0.8f.dp.toPx())
                                drawLine(stroke, Offset(video.right, video.top),
                                    Offset(video.left, video.bottom), 0.8f.dp.toPx())
                            }
                            GridMode.Center -> {
                                val c = video.center
                                val arm = 18.dp.toPx()
                                drawLine(stroke, Offset(c.x - arm, c.y), Offset(c.x + arm, c.y), 1f.dp.toPx())
                                drawLine(stroke, Offset(c.x, c.y - arm), Offset(c.x, c.y + arm), 1f.dp.toPx())
                                drawCircle(stroke, arm, c, style = Stroke(1f.dp.toPx()))
                            }
                            GridMode.Off -> {}
                        }
                    }
                }
            }
        } else {
            Text(
                statusText,
                color = Color.White.copy(alpha = 0.75f),
                fontSize = 13.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier.align(Alignment.Center).padding(32.dp)
            )
        }

        // 变焦倍率徽标
        if (connected && settings.zoom > 1.01f) {
            Text(
                String.format("%.1fx", settings.zoom),
                color = Color.White,
                fontSize = 11.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .padding(end = 14.dp, bottom = 120.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(Color.Black.copy(alpha = 0.5f))
                    .padding(horizontal = 8.dp, vertical = 3.dp)
            )
        }

        if (toast != null) {
            Text(
                toast,
                color = Color.White,
                fontSize = 12.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = 140.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(Color.Black.copy(alpha = 0.7f))
                    .padding(horizontal = 14.dp, vertical = 7.dp)
            )
        }

        if (flash) {
            Box(Modifier.fillMaxSize().background(Color.White))
        }

        if (connected && settings.uiVisible) {
            // targetSdk 35+ 强制 edge-to-edge：视频在系统栏后面全屏，
            // 顶栏/工具栏避开状态栏与手势条
            Column(
                Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding(),
                verticalArrangement = Arrangement.SpaceBetween
            ) {
                TopBar(settings, deviceName, formatDesc, fps, histogram)
                ControlBar(settings) {
                    flash = true
                    onCapture()
                }
            }
        }
    }
}

@Composable
private fun TopBar(
    settings: MonitorSettings,
    deviceName: String,
    formatDesc: String,
    fps: Int,
    histogram: HistogramData?
) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Color.Black.copy(alpha = 0.35f))
            .padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.Top
    ) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(7.dp)
                        .background(Color.Red, RoundedCornerShape(4.dp))
                )
                Spacer(Modifier.width(6.dp))
                Text(deviceName, color = Color.White, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                if (settings.frozen) {
                    Spacer(Modifier.width(6.dp))
                    Text(
                        "FRZ",
                        color = HasselbladOrange,
                        fontSize = 9.sp,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier
                            .clip(RoundedCornerShape(8.dp))
                            .background(HasselbladOrange.copy(alpha = 0.2f))
                            .padding(horizontal = 5.dp, vertical = 1.dp)
                    )
                }
            }
            Spacer(Modifier.height(3.dp))
            Text(formatDesc, color = Color.White.copy(alpha = 0.85f), fontSize = 11.sp, fontFamily = FontFamily.Monospace)
        }
        Spacer(Modifier.weight(1f))
        if (settings.histogramOn && histogram != null) {
            HistogramView(histogram, Modifier.width(118.dp).height(54.dp))
            Spacer(Modifier.width(10.dp))
        }
        Column(horizontalAlignment = Alignment.End) {
            Text("$fps fps", color = Color.White.copy(alpha = 0.9f), fontSize = 12.sp, fontFamily = FontFamily.Monospace)
        }
    }
}

@Composable
private fun HistogramView(h: HistogramData, modifier: Modifier) {
    Canvas(modifier.background(Color.Black.copy(alpha = 0.45f))) {
        val scaleX = size.width / 255f
        fun channel(bins: FloatArray, color: Color) {
            val p = Path()
            p.moveTo(0f, size.height)
            for (i in 0 until 256) {
                p.lineTo(i * scaleX, size.height * (1f - bins[i].coerceIn(0f, 1f)))
            }
            p.lineTo(size.width, size.height)
            p.close()
            drawPath(p, color.copy(alpha = 0.45f))
        }
        channel(h.r, Color.Red)
        channel(h.g, Color.Green)
        channel(h.b, Color.Blue)
        val lp = Path()
        lp.moveTo(0f, size.height)
        for (i in 0 until 256) {
            lp.lineTo(i * scaleX, size.height * (1f - h.l[i].coerceIn(0f, 1f)))
        }
        drawPath(lp, Color.White.copy(alpha = 0.9f), style = Stroke(1.dp.toPx()))
    }
}

private data class ToolSpec(val icon: ImageVector, val label: String, val key: String)

@Composable
private fun ControlBar(
    settings: MonitorSettings,
    onCapture: () -> Unit
) {
    Column(Modifier.fillMaxWidth().background(Color.Black.copy(alpha = 0.55f))) {
        if (settings.zebraOn || settings.peakOn) {
            Column(Modifier.padding(horizontal = 12.dp, vertical = 2.dp)) {
                if (settings.zebraOn) {
                    SliderRow("斑马阈值", settings.zebraThreshold, 0.7f..1.0f) { settings.zebraThreshold = it }
                }
                if (settings.peakOn) {
                    SliderRow("峰值灵敏度", settings.peakThreshold, 0.02f..0.4f) { settings.peakThreshold = it }
                }
            }
        }
        Row(
            Modifier
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 8.dp, vertical = 6.dp)
        ) {
            ToolButton(Icons.Outlined.GridView, settings.gridMode.label, settings.gridMode != GridMode.Off) {
                settings.gridMode = settings.gridMode.next()
            }
            ToolButton(Icons.Outlined.AspectRatio, settings.aspectMode.label, settings.aspectMode != AspectMode.Off) {
                settings.aspectMode = settings.aspectMode.next()
            }
            ToolButton(Icons.Outlined.LightMode, "斑马", settings.zebraOn) { settings.zebraOn = !settings.zebraOn }
            ToolButton(Icons.Outlined.Palette, "假彩", settings.falseColorOn) { settings.falseColorOn = !settings.falseColorOn }
            ToolButton(Icons.Outlined.CenterFocusStrong, "峰值", settings.peakOn) { settings.peakOn = !settings.peakOn }
            ToolButton(Icons.Outlined.BarChart, "直方", settings.histogramOn) { settings.histogramOn = !settings.histogramOn }
            ToolButton(Icons.Outlined.Contrast, "黑白", settings.monoOn) { settings.monoOn = !settings.monoOn }
            ToolButton(Icons.Outlined.Pause, "冻结", settings.frozen) { settings.frozen = !settings.frozen }
            ToolButton(Icons.Outlined.PhotoCamera, "截图", false) { onCapture() }
        }
    }
}

@Composable
private fun SliderRow(title: String, value: Float, range: ClosedFloatingPointRange<Float>, onChange: (Float) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(vertical = 0.dp)) {
        Text(title, color = Color.White.copy(alpha = 0.75f), fontSize = 10.sp, fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(72.dp))
        Slider(value = value, onValueChange = onChange, valueRange = range, modifier = Modifier.weight(1f))
        Text(
            if (range.endInclusive > 0.5f) "${(value * 100).toInt()}%" else String.format("%.2f", value),
            color = Color.White.copy(alpha = 0.75f), fontSize = 10.sp, fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(40.dp)
        )
    }
}

@Composable
private fun ToolButton(icon: ImageVector, label: String, active: Boolean, onClick: () -> Unit) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier
            .padding(horizontal = 3.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(if (active) HasselbladOrange.copy(alpha = 0.18f) else Color.White.copy(alpha = 0.06f))
            .clickable(onClick = onClick)
            .width(52.dp)
            .height(44.dp)
    ) {
        Icon(icon, contentDescription = label,
            tint = if (active) HasselbladOrange else Color.White.copy(alpha = 0.85f),
            modifier = Modifier.size(16.dp))
        Spacer(Modifier.height(3.dp))
        Text(label, color = if (active) HasselbladOrange else Color.White.copy(alpha = 0.85f),
            fontSize = 8.sp, fontFamily = FontFamily.Monospace)
    }
}
