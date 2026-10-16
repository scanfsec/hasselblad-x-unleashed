package com.x2dii.uvcmonitor

import java.util.concurrent.atomic.AtomicReference
import java.util.concurrent.atomic.AtomicLong

/** Bytes are owned by the callback copy and remain unchanged after publication. */
internal data class Nv21Frame(val bytes: ByteArray, val width: Int, val height: Int)

/** Keep the newest complete frame; taking one cannot erase a later update. */
internal class FrameMailbox {
    private val pending = AtomicReference<Nv21Frame?>(null)
    private val epoch = AtomicLong()
    val generation: Long get() = epoch.get()

    fun submit(frame: Nv21Frame) { pending.set(frame) }
    fun take(): Nv21Frame? = pending.getAndSet(null)
    fun clear() { pending.set(null); epoch.incrementAndGet() }
}
