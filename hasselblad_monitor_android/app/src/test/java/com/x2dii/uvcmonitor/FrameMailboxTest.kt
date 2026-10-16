package com.x2dii.uvcmonitor

import org.junit.Assert.*
import org.junit.Test

class FrameMailboxTest {
    private fun frame(id: Int) = Nv21Frame(byteArrayOf(id.toByte()), id * 2, id * 4)

    @Test fun emptyMailboxHasNoFrame() {
        assertNull(FrameMailbox().take())
    }

    @Test fun newestFrameKeepsItsOwnDimensions() {
        val mailbox = FrameMailbox()
        mailbox.submit(frame(1))
        val newest = frame(2)
        mailbox.submit(newest)
        assertSame(newest, mailbox.take())
        assertNull(mailbox.take())
    }

    @Test fun arrivalWhilePreviousFrameIsUploadingRemainsPending() {
        val mailbox = FrameMailbox()
        val uploading = frame(1)
        mailbox.submit(uploading)
        assertSame(uploading, mailbox.take())
        val arriving = frame(2)
        Thread { mailbox.submit(arriving) }.also { it.start(); it.join() }
        // The GL thread completes its old upload without clearing this update.
        assertSame(arriving, mailbox.take())
        assertNull(mailbox.take())
    }

    @Test fun disconnectClearsPendingFrameAndChangesDisplayGeneration() {
        val mailbox = FrameMailbox()
        val original = mailbox.generation
        mailbox.submit(frame(1))
        mailbox.clear()
        assertNull(mailbox.take())
        assertEquals(original + 1, mailbox.generation)
        val next = frame(2)
        mailbox.submit(next)
        assertSame(next, mailbox.take())
    }
}
