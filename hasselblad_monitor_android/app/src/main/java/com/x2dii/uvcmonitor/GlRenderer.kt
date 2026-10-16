package com.x2dii.uvcmonitor

import android.opengl.GLES20
import android.opengl.GLSurfaceView
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * OpenGL ES 2 渲染器：NV21 双平面 → RGB，单 pass 完成 数字变焦/平移、
 * 峰值对焦、黑白、斑马纹、假彩色（与 iOS Shaders.metal 同一套数学）。
 * NV21 = Y 平面 + VU 交错平面（V 在前，与 NV12 相反）。
 */
class MonitorGlRenderer : GLSurfaceView.Renderer {

    // UVC 回调线程写、GL 线程读
    private val frames = FrameMailbox()
    private var displayedFrame: Nv21Frame? = null // GL thread only
    private var displayedGeneration = 0L

    // UI 线程写的渲染参数
    @Volatile var zoom = 1f
    @Volatile var panX = 0f
    @Volatile var panY = 0f
    @Volatile var zebra = 0f
    @Volatile var falseColor = 0f
    @Volatile var peak = 0f
    @Volatile var mono = 0f

    private var program = 0
    private var locAPos = 0
    private var locScale = 0
    private var locUvScale = 0
    private var locUvOffset = 0
    private var locTexel = 0
    private var locZebra = 0
    private var locFalse = 0
    private var locPeak = 0
    private var locMono = 0
    private var locYTex = 0
    private var locVuTex = 0
    private var yTex = 0
    private var vuTex = 0
    private var texW = -1
    private var texH = -1
    private var surfaceW = 0
    private var surfaceH = 0

    private val quad: FloatBuffer = ByteBuffer.allocateDirect(4 * 2 * 4)
        .order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
            put(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f))
            position(0)
        }

    fun submitFrame(data: ByteArray, w: Int, h: Int) {
        frames.submit(Nv21Frame(data, w, h))
    }
    fun clearFrame() { frames.clear() }

    /** UI 重组时同步设置（在 AndroidView.update 里调用） */
    fun apply(s: MonitorSettings) {
        zoom = s.zoom.coerceIn(1f, 8f)
        val half = (1f - 1f / zoom) / 2f
        panX = s.panX.coerceIn(-half, half)
        panY = s.panY.coerceIn(-half, half)
        zebra = if (s.zebraOn) s.zebraThreshold else 0f
        falseColor = if (s.falseColorOn) 1f else 0f
        peak = if (s.peakOn) s.peakThreshold else 0f
        mono = if (s.monoOn) 1f else 0f
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        program = buildProgram(VS, FS)
        locAPos = GLES20.glGetAttribLocation(program, "aPos")
        locScale = GLES20.glGetUniformLocation(program, "uScale")
        locUvScale = GLES20.glGetUniformLocation(program, "uUvScale")
        locUvOffset = GLES20.glGetUniformLocation(program, "uUvOffset")
        locTexel = GLES20.glGetUniformLocation(program, "uTexel")
        locZebra = GLES20.glGetUniformLocation(program, "uZebra")
        locFalse = GLES20.glGetUniformLocation(program, "uFalse")
        locPeak = GLES20.glGetUniformLocation(program, "uPeak")
        locMono = GLES20.glGetUniformLocation(program, "uMono")
        locYTex = GLES20.glGetUniformLocation(program, "yTex")
        locVuTex = GLES20.glGetUniformLocation(program, "vuTex")

        val tex = IntArray(2)
        GLES20.glGenTextures(2, tex, 0)
        yTex = tex[0]; vuTex = tex[1]
        for (t in tex) {
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, t)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        }
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        texW = -1; texH = -1
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        surfaceW = width
        surfaceH = height
        GLES20.glViewport(0, 0, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        val generation = frames.generation
        if (generation != displayedGeneration) {
            displayedFrame = null
            displayedGeneration = generation
        }
        val next = frames.take()
        val current = next ?: displayedFrame ?: return
        val data = current.bytes
        val w = current.width
        val h = current.height
        if (w <= 0 || h <= 0 || data.size < w * h * 3 / 2) return

        if (next != null || texW != w || texH != h) {
            upload(data, w, h)
        }
        displayedFrame = current

        GLES20.glUseProgram(program)

        // aspect-fit 信箱（顶点空间），overlay 的 Canvas 用同一公式
        val videoA = w.toFloat() / h.toFloat()
        val dA = surfaceW.toFloat() / surfaceH.coerceAtLeast(1).toFloat()
        var sx = 1f; var sy = 1f
        if (videoA > dA) sy = dA / videoA else sx = videoA / dA
        GLES20.glUniform2f(locScale, sx, sy)

        val z = zoom.coerceIn(1f, 8f)
        val half = (1f - 1f / z) / 2f
        GLES20.glUniform2f(locUvScale, 1f / z, 1f / z)
        GLES20.glUniform2f(locUvOffset, panX.coerceIn(-half, half), panY.coerceIn(-half, half))
        GLES20.glUniform2f(locTexel, 1f / w, 1f / h)
        GLES20.glUniform1f(locZebra, zebra)
        GLES20.glUniform1f(locFalse, falseColor)
        GLES20.glUniform1f(locPeak, peak)
        GLES20.glUniform1f(locMono, mono)
        GLES20.glUniform1i(locYTex, 0)
        GLES20.glUniform1i(locVuTex, 1)

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, yTex)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, vuTex)

        quad.position(0)
        GLES20.glEnableVertexAttribArray(locAPos)
        GLES20.glVertexAttribPointer(locAPos, 2, GLES20.GL_FLOAT, false, 0, quad)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(locAPos)
    }

    private fun upload(data: ByteArray, w: Int, h: Int) {
        val ySize = w * h
        val cw = w / 2
        val ch = h / 2
        val realloc = texW != w || texH != h

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, yTex)
        if (realloc) {
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE, w, h, 0,
                GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, ByteBuffer.wrap(data, 0, ySize))
        } else {
            GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, w, h,
                GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, ByteBuffer.wrap(data, 0, ySize))
        }

        GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, vuTex)
        if (realloc) {
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE_ALPHA, cw, ch, 0,
                GLES20.GL_LUMINANCE_ALPHA, GLES20.GL_UNSIGNED_BYTE,
                ByteBuffer.wrap(data, ySize, cw * ch * 2))
            texW = w; texH = h
        } else {
            GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, cw, ch,
                GLES20.GL_LUMINANCE_ALPHA, GLES20.GL_UNSIGNED_BYTE,
                ByteBuffer.wrap(data, ySize, cw * ch * 2))
        }
    }

    private fun buildProgram(vsSrc: String, fsSrc: String): Int {
        val vs = compile(GLES20.GL_VERTEX_SHADER, vsSrc)
        val fs = compile(GLES20.GL_FRAGMENT_SHADER, fsSrc)
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, vs)
        GLES20.glAttachShader(p, fs)
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        check(ok[0] == GLES20.GL_TRUE) { "link: " + GLES20.glGetProgramInfoLog(p) }
        return p
    }

    private fun compile(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
        check(ok[0] == GLES20.GL_TRUE) { "shader: " + GLES20.glGetShaderInfoLog(s) }
        return s
    }

    companion object {
        private val VS = """
            attribute vec2 aPos;
            uniform vec2 uScale;
            varying vec2 vUv;
            void main() {
                gl_Position = vec4(aPos * uScale, 0.0, 1.0);
                vUv = vec2((aPos.x + 1.0) * 0.5, 1.0 - (aPos.y + 1.0) * 0.5);
            }
        """.trimIndent()

        private val FS = """
            precision mediump float;
            varying vec2 vUv;
            uniform sampler2D yTex;
            uniform sampler2D vuTex;   /* NV21: .r=V, .a=U */
            uniform vec2 uUvScale;
            uniform vec2 uUvOffset;
            uniform vec2 uTexel;
            uniform float uZebra;
            uniform float uFalse;
            uniform float uPeak;
            uniform float uMono;

            vec3 yuvToRgb(float y, float v, float u) {
                float cb = u - 0.50196078;
                float cr = v - 0.50196078;
                float yy = (y - 0.06274509) * 1.164;
                return clamp(vec3(yy + 1.596 * cr, yy - 0.391 * cb - 0.813 * cr, yy + 2.018 * cb), 0.0, 1.0);
            }

            void main() {
                vec2 uv = (vUv - 0.5) * uUvScale + 0.5 + uUvOffset;
                if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
                    return;
                }
                float y = texture2D(yTex, uv).r;
                vec2 vu = texture2D(vuTex, uv).ra;
                vec3 rgb = yuvToRgb(y, vu.r, vu.g);
                float l = dot(rgb, vec3(0.2126, 0.7152, 0.0722));

                /* 峰值对焦：亮度 3x3 Sobel 染红 */
                if (uPeak > 0.0) {
                    float tl = texture2D(yTex, uv + vec2(-uTexel.x, -uTexel.y)).r;
                    float tc = texture2D(yTex, uv + vec2(0.0, -uTexel.y)).r;
                    float tr = texture2D(yTex, uv + vec2(uTexel.x, -uTexel.y)).r;
                    float ml = texture2D(yTex, uv + vec2(-uTexel.x, 0.0)).r;
                    float mr = texture2D(yTex, uv + vec2(uTexel.x, 0.0)).r;
                    float bl = texture2D(yTex, uv + vec2(-uTexel.x, uTexel.y)).r;
                    float bc = texture2D(yTex, uv + vec2(0.0, uTexel.y)).r;
                    float br = texture2D(yTex, uv + vec2(uTexel.x, uTexel.y)).r;
                    float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
                    float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
                    float on = smoothstep(uPeak, uPeak * 1.6, length(vec2(gx, gy)));
                    rgb = mix(rgb, vec3(1.0, 0.18, 0.1), on * 0.85);
                }

                if (uMono > 0.5) rgb = vec3(l);

                /* 斑马纹 */
                if (uZebra > 0.0) {
                    float over = step(uZebra, l);
                    float stripe = step(0.5, fract((gl_FragCoord.x + gl_FragCoord.y) * 0.06));
                    rgb = mix(rgb, vec3(1.0, 0.1, 0.1), over * stripe * 0.85);
                }

                /* 假彩色 9 段 */
                if (uFalse > 0.5) {
                    vec3 c;
                    if      (l < 0.04) c = vec3(0.00, 0.00, 0.00);
                    else if (l < 0.13) c = vec3(0.09, 0.09, 0.44);
                    else if (l < 0.27) c = vec3(0.18, 0.27, 0.83);
                    else if (l < 0.40) c = vec3(0.30, 0.78, 0.90);
                    else if (l < 0.53) c = vec3(0.22, 0.71, 0.29);
                    else if (l < 0.65) c = vec3(0.98, 0.93, 0.25);
                    else if (l < 0.77) c = vec3(0.96, 0.60, 0.13);
                    else if (l < 0.90) c = vec3(0.86, 0.11, 0.11);
                    else if (l < 0.96) c = vec3(1.00, 1.00, 1.00);
                    else               c = vec3(0.60, 0.00, 0.60);
                    rgb = c;
                }
                gl_FragColor = vec4(rgb, 1.0);
            }
        """.trimIndent()
    }
}
