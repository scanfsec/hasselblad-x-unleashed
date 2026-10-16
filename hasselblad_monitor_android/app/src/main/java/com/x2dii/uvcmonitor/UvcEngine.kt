package com.x2dii.uvcmonitor

import android.content.ContentValues
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.ImageFormat
import android.graphics.PixelFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.media.ImageReader
import android.os.Environment
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.provider.MediaStore
import android.util.Log
import com.serenegiant.usb.IFrameCallback
import com.serenegiant.usb.Size
import com.serenegiant.usb.USBMonitor
import com.serenegiant.usb.UVCCamera
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer

/**
 * UVC 引擎：USBMonitor(权限/热插拔) + UVCCamera(libuvc, YUYV/MJPEG→NV21 帧回调)
 * 帧路由 = fps 计数 → 渲染器喂帧 → 直方图采样（0.5s 一更）；冻结时停喂。
 */
class UvcEngine {

    interface Listener {
        fun onStatus(text: String?)                 // null = 已连接
        fun onConnected(name: String, format: String, videoAspect: Float)
        fun onFps(fps: Int)
        fun onHistogram(h: HistogramData)
        fun onToast(msg: String)
    }

    var listener: Listener? = null
    var renderer: MonitorGlRenderer? = null
    @Volatile var frozen = false
    @Volatile var histogramOn = false
    var connected = false
        private set

    private var appContext: Context? = null
    private var usbMonitor: USBMonitor? = null
    private var camera: UVCCamera? = null
    private val main = Handler(Looper.getMainLooper())
    private var usbManager: UsbManager? = null
    private var usbReceiver: BroadcastReceiver? = null
    private var usbThread: HandlerThread? = null
    private var usbHandler: Handler? = null
    private var previewReader: ImageReader? = null
    private var previewThread: HandlerThread? = null
    private var controlBlock: USBMonitor.UsbControlBlock? = null
    private var activeDeviceName: String? = null
    private var permissionPending: String? = null
    @Volatile private var closed = false

    // 帧线程私有
    private var frameCount = 0
    private var windowStart = 0L
    private var lastHistNano = 0L
    @Volatile private var latestFrame: ByteArray? = null
    @Volatile private var frameW = 0
    @Volatile private var frameH = 0
    @Volatile private var lastFrameAt = 0L

    private val frameCallback = IFrameCallback { bb: ByteBuffer ->
        val now = System.nanoTime()
        if (frameW == 0 || frameH == 0) return@IFrameCallback
        lastFrameAt = now
        frameCount++
        if (now - windowStart >= 1_000_000_000L) {
            val fps = (frameCount * 1_000_000_000L / (now - windowStart)).toInt()
            frameCount = 0
            windowStart = now
            Log.i("UVCMonitor", "frames fps=$fps size=${frameW}x${frameH}")
            main.post { listener?.onFps(fps) }
        }
        val arr = ByteArray(bb.remaining()).also { bb.get(it) }   // buffer 复用，必须拷贝
        latestFrame = arr
        if (!frozen) {
            renderer?.submitFrame(arr, frameW, frameH)
            if (histogramOn && now - lastHistNano > 500_000_000L) {
                lastHistNano = now
                sampleHistogram(arr, frameW, frameH)?.let { h ->
                    main.post { listener?.onHistogram(h) }
                }
            }
        }
    }

    fun init(context: Context) {
        appContext = context.applicationContext
        closed = false
        usbManager = context.getSystemService(UsbManager::class.java)
        usbThread = HandlerThread("UVC-connect").also { it.start() }
        usbHandler = Handler(usbThread!!.looper)
        usbMonitor = USBMonitor(context, object : USBMonitor.OnDeviceConnectListener {
            override fun onAttach(device: UsbDevice) {}
            override fun onDettach(device: UsbDevice) {}
            override fun onConnect(device: UsbDevice, ctrl: USBMonitor.UsbControlBlock, createNew: Boolean) {}
            override fun onDisconnect(device: UsbDevice, ctrl: USBMonitor.UsbControlBlock) {}
            override fun onCancel(device: UsbDevice) {}
        })
        // Use Android's permission API before touching the old library. Its polling
        // reads device.serialNumber even when permission has not been granted.
        val action = context.packageName + ".USB_PERMISSION"
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
                usbHandler?.post {
                    if (closed) return@post
                    when (intent.action) {
                        UsbManager.ACTION_USB_DEVICE_ATTACHED -> requestDevice(device, action)
                        UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                            if (permissionPending == device.deviceName) permissionPending = null
                            if (activeDeviceName == device.deviceName) {
                                releaseDevice()
                                main.post { listener?.onStatus("未检测到外接设备\n\n插入后自动连接") }
                            }
                        }
                        action -> {
                            permissionPending = null
                            if (usbManager?.hasPermission(device) == true) connectDevice(device)
                            else main.post { listener?.onStatus("USB 访问未授权，请重新插入并允许访问") }
                        }
                    }
                }
            }
        }
        // Register and unregister with the same Context; Activity and application
        // maintain different receiver registrations.
        appContext!!.registerReceiver(receiver, IntentFilter().apply {
            addAction(action)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }, Context.RECEIVER_EXPORTED)
        usbReceiver = receiver
        usbHandler?.post { usbManager?.deviceList?.values?.forEach { requestDevice(it, action) } }
    }

    private fun requestDevice(device: UsbDevice, action: String) {
        if (closed || (0 until device.interfaceCount).none { device.getInterface(it).interfaceClass == 14 }) return
        if (activeDeviceName == device.deviceName || permissionPending == device.deviceName) return
        val manager = usbManager ?: return
        if (manager.hasPermission(device)) connectDevice(device)
        else {
            permissionPending = device.deviceName
            val ctx = appContext ?: return
            val permission = PendingIntent.getBroadcast(ctx, 0,
                Intent(action).setPackage(ctx.packageName), PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            manager.requestPermission(device, permission)
        }
    }

    private fun connectDevice(device: UsbDevice) {
        if (closed || activeDeviceName == device.deviceName) return
        // Ignore delayed permission broadcasts for a device that was unplugged.
        if (usbManager?.deviceList?.get(device.deviceName) == null) return
        try {
            releaseDevice()
            val ctrl = usbMonitor?.openDevice(device) ?: return
            controlBlock = ctrl
            activeDeviceName = device.deviceName
            openCamera(device, ctrl)
        } catch (e: Exception) {
            releaseDevice()
            Log.e("UVCMonitor", "USB open failed", e)
            main.post { listener?.onStatus("USB 打开失败：${e.message}") }
        }
    }

    fun close() {
        if (closed) return
        closed = true
        listener = null
        usbReceiver?.let { appContext?.unregisterReceiver(it) }
        usbReceiver = null
        main.removeCallbacksAndMessages(null)
        // Native open, disconnect, and destroy share one worker. Removing
        // queued tasks does not interrupt an open that is already running.
        val handler = usbHandler
        if (handler != null) {
            handler.post {
                releaseDevice()
                try {
                    usbMonitor?.unregister()
                    usbMonitor?.destroy()
                } catch (e: Exception) {
                    Log.w("UVCMonitor", "USB monitor teardown failed", e)
                }
                usbMonitor = null
                usbThread?.quitSafely()
                usbThread = null
                usbHandler = null
                Log.i("UVCMonitor", "USB worker teardown complete")
            }
        }
    }

    private fun releaseDevice() {
        closeCamera()
        try {
            controlBlock?.close()
        } catch (e: Exception) {
            Log.w("UVCMonitor", "USB control block close failed", e)
        }
        controlBlock = null
        activeDeviceName = null
        permissionPending = null
    }

    private fun openCamera(device: UsbDevice, ctrl: USBMonitor.UsbControlBlock) {
        try {
            val cam = UVCCamera()
            camera = cam
            cam.open(ctrl)
            val best: Size? = cam.supportedSizeList.maxWithOrNull(
                compareBy { it.width.toLong() * it.height * ((it.fps.maxOrNull() ?: 0f).toDouble().toLong().coerceAtLeast(1L)) }
            )
            val w = best?.width ?: 640
            val h = best?.height ?: 480
            val fps = best?.fps?.maxOrNull() ?: 30f
            val encoding = negotiatePreview {
                cam.setPreviewSize(w, h, 1, fps.toInt().coerceAtLeast(1), it.frameFormat, it.bandwidth)
            }
            frameW = w
            frameH = h
            windowStart = System.nanoTime()
            lastFrameAt = windowStart
            previewThread = HandlerThread("UVC-preview-drain").also { it.start() }
            previewReader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2).also { reader ->
                reader.setOnImageAvailableListener({ it.acquireLatestImage()?.close() }, Handler(previewThread!!.looper))
                cam.setPreviewDisplay(reader.surface)
            }
            // Bundled JNI's YUV420SP/4 label selects uvc_yuyv2iyuv420SP (actual VU/NV21).
            // Its NV21 label selects UV. Preserve real NV21 for GL and JPEG.
            cam.setFrameCallback(frameCallback, UVCCamera.PIXEL_FORMAT_YUV420SP)
            cam.startPreview()
            connected = true
            Log.i("UVCMonitor", "preview start requested $w x $h @ ${fps.toInt()} $encoding")
            val watchdog = object : Runnable {
                override fun run() {
                    if (closed || camera !== cam) return
                    val limit = if (latestFrame == null) 7_000_000_000L else 3_000_000_000L
                    if (System.nanoTime() - lastFrameAt > limit) {
                        Log.e("UVCMonitor", "Video callbacks stopped; closing stale session")
                        releaseDevice()
                        main.post {
                            listener?.onFps(0)
                            listener?.onStatus("视频流已中断，请重新连接相机")
                        }
                    } else usbHandler?.postDelayed(this, 1000)
                }
            }
            usbHandler?.postDelayed(watchdog, 1000)
            main.post {
                listener?.onConnected(
                    ctrl.productName ?: device.productName ?: "外接设备",
                    "$w×$h @ ${fps.toInt()} $encoding",
                    w.toFloat() / h.toFloat()
                )
                listener?.onStatus(null)
            }
        } catch (e: Exception) {
            Log.e("UVCMonitor", "Open failed", e)
            releaseDevice()
            main.post { listener?.onStatus("打开失败：${e.message}") }
        }
    }

    private fun closeCamera() {
        val cam = camera ?: return
        camera = null
        connected = false
        frameW = 0
        frameH = 0
        frameCount = 0
        latestFrame = null
        try {
            cam.setFrameCallback(null, 0)
        } catch (_: Exception) {
        }
        try {
            cam.stopPreview()
        } catch (_: Exception) {
        }
        try {
            cam.close()
        } catch (_: Exception) {
        }
        try {
            cam.destroy()
        } catch (_: Exception) {
        }
        previewReader?.close()
        previewReader = null
        previewThread?.quitSafely()
        previewThread = null
        renderer?.clearFrame()
        main.post { listener?.onFps(0) }
    }

    /** 截图：当前帧 NV21 → JPEG → MediaStore 相册（API29+ 免权限） */
    fun capture() {
        val arr = latestFrame
        val w = frameW
        val h = frameH
        val ctx = appContext
        if (arr == null || w == 0 || h == 0 || ctx == null) {
            main.post { listener?.onToast("暂无帧可截") }
            return
        }
        Thread {
            try {
                val yuv = YuvImage(arr, ImageFormat.NV21, w, h, null)
                val out = ByteArrayOutputStream()
                yuv.compressToJpeg(Rect(0, 0, w, h), 95, out)
                val bytes = out.toByteArray()
                val cv = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, "Monitor_${System.currentTimeMillis()}.jpg")
                    put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                    put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/Monitor")
                }
                val uri = ctx.contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, cv)
                    ?: throw IllegalStateException("MediaStore insert 失败")
                ctx.contentResolver.openOutputStream(uri)?.use { it.write(bytes) }
                main.post { listener?.onToast("已保存到相册") }
            } catch (e: Exception) {
                main.post { listener?.onToast("保存失败：${e.message}") }
            }
        }.start()
    }

    companion object {
        /** NV21 CPU 抽样直方图：Y 平面 + VU 交错平面（V 在前） */
        fun sampleHistogram(arr: ByteArray, w: Int, h: Int): HistogramData? {
            if (w < 2 || h < 2 || arr.size < w * h * 3 / 2) return null
            val hd = HistogramData()
            val stepX = maxOf(1, w / 96)
            val stepY = maxOf(1, h / 96)
            val yBase = 0
            val cBase = w * h
            var maxV = 1f
            var gy = 0
            while (gy < h) {
                val rowY = yBase + gy * w
                val rowC = cBase + (gy ushr 1) * w
                var gx = 0
                while (gx < w) {
                    val yv = arr[rowY + gx].toInt() and 0xFF
                    val c = rowC + (gx ushr 1) * 2
                    val v = arr[c].toInt() and 0xFF
                    val u = arr[c + 1].toInt() and 0xFF
                    val yy = (yv - 16) * 1.164f
                    val cb = u - 128f
                    val cr = v - 128f
                    val r = (yy + 1.596f * cr).toInt().coerceIn(0, 255)
                    val g = (yy - 0.391f * cb - 0.813f * cr).toInt().coerceIn(0, 255)
                    val b = (yy + 2.018f * cb).toInt().coerceIn(0, 255)
                    hd.r[r]++; hd.g[g]++; hd.b[b]++
                    hd.l[yy.toInt().coerceIn(0, 255)]++
                    if (hd.r[r] > maxV) maxV = hd.r[r]
                    if (hd.g[g] > maxV) maxV = hd.g[g]
                    if (hd.b[b] > maxV) maxV = hd.b[b]
                    gx += stepX
                }
                gy += stepY
            }
            for (i in 0 until 256) {
                hd.r[i] /= maxV; hd.g[i] /= maxV; hd.b[i] /= maxV; hd.l[i] /= maxV
            }
            return hd
        }
    }
}
