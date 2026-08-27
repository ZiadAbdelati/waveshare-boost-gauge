package com.boostgauge.app.data.transport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BleControlFramerTest {
    @Test
    fun reassemblesFragmentedResponse() {
        val framer = BleControlFramer()

        assertTrue(framer.append("{\"id\":7,\"body\":{\"text\":\"a".encodeToByteArray()).isEmpty())
        val frames = framer.append("}b\"},\"status\":200}".encodeToByteArray())

        assertEquals(listOf("{\"id\":7,\"body\":{\"text\":\"a}b\"},\"status\":200}"), frames)
    }

    @Test
    fun emitsConcatenatedResponsesIndividually() {
        val framer = BleControlFramer()
        val frames = framer.append("{\"id\":1,\"status\":200}{\"id\":2,\"status\":409}".encodeToByteArray())

        assertEquals(2, frames.size)
        assertTrue(frames[0].contains("\"id\":1"))
        assertTrue(frames[1].contains("\"id\":2"))
    }

    @Test
    fun clearDropsPartialResponse() {
        val framer = BleControlFramer()
        framer.append("{\"id\":1".encodeToByteArray())
        framer.clear()

        assertEquals(listOf("{\"id\":2}"), framer.append("{\"id\":2}".encodeToByteArray()))
    }

    @Test
    fun statusFragmentsReassembleIntoSingleObject() {
        val framer = BleControlFramer()
        val state = "{\"psi\":12.3,\"peak\":30.1,\"zone\":\"dyno-cell\",\"demo\":true,\"uptimeMs\":12345}"

        val emitted = mutableListOf<String>()
        val chunkSize = 20
        for (i in state.indices step chunkSize) {
            val chunk = state.substring(i, minOf(i + chunkSize, state.length)).encodeToByteArray()
            emitted += framer.append(chunk)
        }

        assertEquals(listOf(state), emitted)
    }
}
