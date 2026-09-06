package fr.wozt.bottomscreen

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The wire format, mirroring bs_protocol.h.
 *
 * The C header is the definition; this is the same thing said in Kotlin,
 * and the two have to be changed together. The struct sizes are asserted
 * in the unit tests for exactly that reason -- a field added on one side
 * and forgotten on the other produces a stream that connects and then
 * makes no sense, which is a miserable thing to debug from a phone.
 *
 * Everything is little-endian, which is what the server sends and what
 * every ARM device here runs natively.
 */
object BsProtocol {

    const val MAGIC = 0x31435342          // "BSC1"
    const val VERSION = 1
    const val DEFAULT_PORT = 5090
    const val MAX_PAYLOAD = 1 * 1024 * 1024

    // Consoles
    const val CONSOLE_DS = 1
    const val CONSOLE_3DS = 2
    const val CONSOLE_WIIU = 3

    const val CODEC_H264 = 1

    /** Sound codec. */
    const val ACODEC_OPUS = 1

    // Message types
    const val MSG_VIDEO = 1
    const val MSG_AUDIO = 2
    const val MSG_INPUT = 16
    const val MSG_PING = 17
    const val MSG_PONG = 18
    const val MSG_REQUEST_KEYFRAME = 19
    const val MSG_SET_QUALITY = 20

    // Video flags
    const val VFLAG_KEYFRAME = 0x01
    const val VFLAG_END_OF_FRAME = 0x02

    // Input event types
    const val INPUT_TOUCH_DOWN = 1
    const val INPUT_TOUCH_MOVE = 2
    const val INPUT_TOUCH_UP = 3
    const val INPUT_BUTTON_DOWN = 4
    const val INPUT_BUTTON_UP = 5
    const val INPUT_AXIS = 6

    // Buttons, in the order BsButton declares them
    const val BTN_A = 1
    const val BTN_B = 2
    const val BTN_X = 3
    const val BTN_Y = 4
    const val BTN_L = 5
    const val BTN_R = 6
    const val BTN_ZL = 7
    const val BTN_ZR = 8
    const val BTN_START = 9
    const val BTN_SELECT = 10
    const val BTN_UP = 11
    const val BTN_DOWN = 12
    const val BTN_LEFT = 13
    const val BTN_RIGHT = 14
    const val BTN_HOME = 15

    // Axes
    const val AXIS_LEFT_X = 1
    const val AXIS_LEFT_Y = 2
    const val AXIS_RIGHT_X = 3
    const val AXIS_RIGHT_Y = 4

    const val HELLO_SIZE = 8
    const val HELLO_ACK_SIZE = 20
    const val MSG_HEADER_SIZE = 8
    const val VIDEO_HEADER_SIZE = 16
    const val AUDIO_HEADER_SIZE = 8
    const val INPUT_EVENT_SIZE = 16

    fun buffer(size: Int): ByteBuffer =
        ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)

    fun wrap(bytes: ByteArray, offset: Int = 0, length: Int = bytes.size - offset): ByteBuffer =
        ByteBuffer.wrap(bytes, offset, length).order(ByteOrder.LITTLE_ENDIAN)

    /** The client's opening message. */
    fun hello(): ByteArray {
        val b = buffer(HELLO_SIZE)
        b.putInt(MAGIC)
        b.put(VERSION.toByte())
        b.put(0); b.put(0); b.put(0)
        return b.array()
    }

    data class HelloAck(
        val accepted: Boolean,
        val console: Int,
        val codec: Int,
        val width: Int,
        val height: Int,
        val fps: Int,
        val extradataSize: Int,
        /** 0 when the server sends no sound, which is a normal state
         *  rather than an error -- the UI then draws no volume control
         *  instead of one that does nothing. */
        val audioRate: Int,
        val audioChannels: Int,
        val audioCodec: Int
    ) {
        val hasAudio: Boolean get() = audioRate > 0
    }

    fun parseHelloAck(bytes: ByteArray): HelloAck? {
        if (bytes.size < HELLO_ACK_SIZE) return null
        val b = wrap(bytes)
        if (b.int != MAGIC) return null
        val version = b.get().toInt() and 0xFF
        if (version != VERSION) return null
        val accepted = (b.get().toInt() and 0xFF) != 0
        val console = b.get().toInt() and 0xFF
        val codec = b.get().toInt() and 0xFF
        val width = b.short.toInt() and 0xFFFF
        val height = b.short.toInt() and 0xFFFF
        val fps = b.short.toInt() and 0xFFFF
        val extra = b.short.toInt() and 0xFFFF
        val aCodec = b.get().toInt() and 0xFF
        val aChannels = b.get().toInt() and 0xFF
        val aRate = b.short.toInt() and 0xFFFF
        return HelloAck(accepted, console, codec, width, height, fps, extra,
                        aRate, aChannels, aCodec)
    }

    data class MsgHeader(val type: Int, val payloadSize: Int)

    fun parseMsgHeader(bytes: ByteArray): MsgHeader {
        val b = wrap(bytes)
        val type = b.get().toInt() and 0xFF
        b.get(); b.get(); b.get()
        return MsgHeader(type, b.int)
    }

    data class VideoHeader(
        val frameId: Int,
        val timestampUs: Int,
        val fragmentId: Int,
        val fragmentCount: Int,
        val flags: Int
    ) {
        val isKeyframe: Boolean get() = (flags and VFLAG_KEYFRAME) != 0
    }

    fun parseVideoHeader(bytes: ByteArray, offset: Int = 0): VideoHeader {
        val b = wrap(bytes, offset, VIDEO_HEADER_SIZE)
        val frameId = b.int
        val ts = b.int
        val fragId = b.short.toInt() and 0xFFFF
        val fragCount = b.short.toInt() and 0xFFFF
        val flags = b.get().toInt() and 0xFF
        return VideoHeader(frameId, ts, fragId, fragCount, flags)
    }

    /**
     * One input event, already framed with its message header so it goes
     * out in a single write. Two writes with TCP_NODELAY on would be two
     * packets on the wire for something this small.
     */
    fun inputMessage(
        sequence: Int,
        timestampUs: Int,
        type: Int,
        code: Int,
        x: Int,
        y: Int
    ): ByteArray {
        val b = buffer(MSG_HEADER_SIZE + INPUT_EVENT_SIZE)
        b.put(MSG_INPUT.toByte())
        b.put(0); b.put(0); b.put(0)
        b.putInt(INPUT_EVENT_SIZE)
        b.putInt(sequence)
        b.putInt(timestampUs)
        b.put(type.toByte())
        b.put(code.toByte())
        b.putShort(x.toShort())
        b.putShort(y.toShort())
        b.putShort(0)
        return b.array()
    }

    /** An empty message, for the types that carry no payload. */
    fun emptyMessage(type: Int): ByteArray {
        val b = buffer(MSG_HEADER_SIZE)
        b.put(type.toByte())
        b.put(0); b.put(0); b.put(0)
        b.putInt(0)
        return b.array()
    }

    /**
     * Asks the server to re-encode at [bitrate] bits per second. 0 lets
     * it derive one from the resolution.
     */
    fun qualityMessage(bitrate: Int, fps: Int = 0): ByteArray {
        val b = buffer(MSG_HEADER_SIZE + 8)
        b.put(MSG_SET_QUALITY.toByte())
        b.put(0); b.put(0); b.put(0)
        b.putInt(8)
        b.putInt(bitrate)
        b.putShort(fps.toShort())
        b.putShort(0)
        return b.array()
    }

    fun consoleName(console: Int): String = when (console) {
        CONSOLE_DS -> "Nintendo DS"
        CONSOLE_3DS -> "Nintendo 3DS"
        CONSOLE_WIIU -> "Wii U GamePad"
        else -> "unknown"
    }
}
