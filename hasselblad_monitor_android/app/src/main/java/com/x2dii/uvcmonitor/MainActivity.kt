package com.x2dii.uvcmonitor

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

class MainActivity : ComponentActivity(), UvcEngine.Listener {

    private val settings = MonitorSettings()
    private val renderer = MonitorGlRenderer()
    private val engine = UvcEngine()

    private var connected by mutableStateOf(false)
    private var statusText by mutableStateOf("正在检测设备…")
    private var deviceName by mutableStateOf("")
    private var formatDesc by mutableStateOf("")
    private var fps by mutableIntStateOf(0)
    private var videoAspect by mutableFloatStateOf(0f)
    private var histogram by mutableStateOf<HistogramData?>(null)
    private var toast by mutableStateOf<String?>(null)
    private var toastSeq = 0
    private val cameraPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) startMonitor()
        else statusText = "需要相机权限才能访问外接设备\n\n请在应用设置中允许相机权限后重新打开"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        engine.listener = this
        engine.renderer = renderer
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startMonitor()
        } else {
            cameraPermission.launch(Manifest.permission.CAMERA)
        }

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                MonitorScreen(
                    settings = settings,
                    engine = engine,
                    renderer = renderer,
                    connected = connected,
                    statusText = statusText,
                    deviceName = deviceName,
                    formatDesc = formatDesc,
                    fps = fps,
                    videoAspect = videoAspect,
                    histogram = histogram,
                    toast = toast,
                    onCapture = { engine.capture() }
                )
            }
        }
    }

    private fun startMonitor() {
        try {
            engine.init(this)
            statusText = "未检测到外接设备\n\n插入后自动连接"
        } catch (e: Exception) {
            connected = false
            statusText = "USB 初始化失败：${e.message}"
        }
    }

    override fun onDestroy() {
        engine.close()
        super.onDestroy()
    }

    // ---- UvcEngine.Listener（主线程回调）----

    override fun onStatus(text: String?) {
        if (text == null) {
            connected = true
            statusText = ""
        } else {
            connected = false
            statusText = text
        }
    }

    override fun onConnected(name: String, format: String, aspect: Float) {
        connected = true
        statusText = ""
        deviceName = name
        formatDesc = format
        videoAspect = aspect
        fps = 0
        histogram = null
    }

    override fun onFps(value: Int) {
        fps = value
    }

    override fun onHistogram(h: HistogramData) {
        histogram = h
    }

    override fun onToast(msg: String) {
        val seq = ++toastSeq
        toast = msg
        android.os.Handler(mainLooper).postDelayed({
            if (seq == toastSeq) toast = null
        }, 1600)
    }
}
