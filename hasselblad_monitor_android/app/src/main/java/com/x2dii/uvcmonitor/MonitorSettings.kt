package com.x2dii.uvcmonitor

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** 监视器设置（Compose 快照状态，UI 与渲染参数同源） */
class MonitorSettings {
    var uiVisible by mutableStateOf(true)
    var gridMode by mutableStateOf(GridMode.Off)
    var aspectMode by mutableStateOf(AspectMode.Off)
    var zebraOn by mutableStateOf(false)
    var zebraThreshold by mutableStateOf(0.95f)
    var falseColorOn by mutableStateOf(false)
    var peakOn by mutableStateOf(false)
    var peakThreshold by mutableStateOf(0.12f)
    var monoOn by mutableStateOf(false)
    var histogramOn by mutableStateOf(false)
    var frozen by mutableStateOf(false)
    var zoom by mutableStateOf(1f)
    var panX by mutableStateOf(0f)
    var panY by mutableStateOf(0f)
}

enum class GridMode(val label: String) {
    Off("关"), Thirds("三分"), Diagonals("对角"), Center("中心"), Golden("黄金");
    fun next(): GridMode = entries[(ordinal + 1) % entries.size]
}

enum class AspectMode(val ratio: Float?, val label: String) {
    Off(null, "关"),
    R169(16f / 9f, "16:9"),
    R43(4f / 3f, "4:3"),
    R11(1f, "1:1"),
    R6524(65f / 24f, "65:24"),   // XPan
    R32(3f / 2f, "3:2");
    fun next(): AspectMode = entries[(ordinal + 1) % entries.size]
}

class HistogramData {
    val r = FloatArray(256)
    val g = FloatArray(256)
    val b = FloatArray(256)
    val l = FloatArray(256)
}
