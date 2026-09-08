package fr.wozt.bottomscreen

import android.util.Log
import java.io.DataInputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicInteger

/**
 * The network side: one connection, one reader thread, one writer thread.
 *
 * Input goes out on its own thread rather than from the touch handler,
 * because a socket write from the UI thread is both illegal on Android
 * and a good way to make the interface stutter when the network hiccups.
 * The handoff through a queue costs microseconds and buys a touch
 * handler that never blocks.
 */
class BsClient(
    private val host: String,
    private val port: Int,
    private val listener: Listener
) {
    interface Listener {
        fun onConnected(ack: BsProtocol.HelloAck)
        fun onFrame(data: ByteArray, offset: Int, length: Int, keyframe: Boolean, timestampUs: Int)
        fun onAudio(data: ByteArray, offset: Int, length: Int)
        fun onStreamInfo(info: BsProtocol.StreamInfo)
        /**
         * Which screens this server has, as a bit per BsProtocol.SCREEN_*.
         *
         * Never called by a server built before the top screen existed,
         * which is why the choice starts hidden rather than starting
         * shown and being taken away.
         */
        fun onScreens(mask: Int)
        fun onDisconnected(reason: String)
    }

    private var socket: Socket? = null
    private var reader: Thread? = null
    private var writer: Thread? = null
    private val outQueue = LinkedBlockingQueue<ByteArray>(256)
    private val sequence = AtomicInteger(0)
    @Volatile private var running = false

    fun start() {
        if (running) return
        running = true
        reader = Thread({ readLoop() }, "bs-reader").apply { start() }
    }

    fun stop() {
        running = false
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        /* A sentinel, so the writer wakes from take() instead of waiting
         * for an event that will never come. */
        outQueue.offer(ByteArray(0))
    }

    fun sendInput(type: Int, code: Int, x: Int, y: Int) {
        if (!running) return
        val msg = BsProtocol.inputMessage(
            sequence.getAndIncrement(),
            (System.nanoTime() / 1000L).toInt(),
            type, code, x, y
        )
        /* offer, not put: if the queue is full the link is already in
         * trouble, and blocking the caller would only make it worse. An
         * input event is worth less than a responsive interface. */
        outQueue.offer(msg)
    }

    /** Asks the server to re-encode at a different bitrate. */
    fun sendQuality(bitrate: Int) {
        if (running) outQueue.offer(BsProtocol.qualityMessage(bitrate))
    }

    /** Zero for both means "whatever the source produces". */
    fun sendSize(width: Int, height: Int) {
        if (running) outQueue.offer(BsProtocol.sizeMessage(width, height))
    }


    /** Picks which of a Wii U's two audio outputs to receive. */
    fun sendAudioSource(source: Int) {
        if (running) outQueue.offer(BsProtocol.audioSourceMessage(source))
    }

    /** Picks which of the machine's two screens to receive. */
    fun sendScreen(screen: Int) {
        if (running) outQueue.offer(BsProtocol.screenMessage(screen))
    }

    private fun readLoop() {
        var reason = "disconnected"
        try {
            val s = Socket()
            s.tcpNoDelay = true
            s.connect(InetSocketAddress(host, port), 5000)
            socket = s

            val out = s.getOutputStream()
            val input = DataInputStream(s.getInputStream())

            out.write(BsProtocol.hello())
            out.flush()

            val ackBytes = ByteArray(BsProtocol.HELLO_ACK_SIZE)
            input.readFully(ackBytes)
            val ack = BsProtocol.parseHelloAck(ackBytes)
                ?: throw IllegalStateException("bad handshake")
            if (!ack.accepted) throw IllegalStateException("the server is full")
            if (ack.codec != BsProtocol.CODEC_H264)
                throw IllegalStateException("unexpected codec ${ack.codec}")
            if (ack.extradataSize > 0) {
                /* The server sends parameter sets inline instead, so
                 * there is normally nothing here. Read it anyway rather
                 * than desynchronising the stream. */
                input.readFully(ByteArray(ack.extradataSize))
            }

            listener.onConnected(ack)

            writer = Thread({ writeLoop(out) }, "bs-writer").apply { start() }

            val header = ByteArray(BsProtocol.MSG_HEADER_SIZE)
            val payload = ByteArray(BsProtocol.MAX_PAYLOAD)

            while (running) {
                input.readFully(header)
                val h = BsProtocol.parseMsgHeader(header)
                if (h.payloadSize < 0 || h.payloadSize > BsProtocol.MAX_PAYLOAD)
                    throw IllegalStateException("payload of ${h.payloadSize} bytes")
                if (h.payloadSize > 0) input.readFully(payload, 0, h.payloadSize)

                when (h.type) {
                    BsProtocol.MSG_VIDEO -> {
                        if (h.payloadSize > BsProtocol.VIDEO_HEADER_SIZE) {
                            val vh = BsProtocol.parseVideoHeader(payload)
                            /* TCP never fragments, so anything else means
                             * a UDP stream reached a client that cannot
                             * reassemble it yet. Better to say so than to
                             * feed the decoder half a picture. */
                            if (vh.fragmentCount == 1) {
                                listener.onFrame(
                                    payload,
                                    BsProtocol.VIDEO_HEADER_SIZE,
                                    h.payloadSize - BsProtocol.VIDEO_HEADER_SIZE,
                                    vh.isKeyframe,
                                    vh.timestampUs
                                )
                            }
                        }
                    }
                    BsProtocol.MSG_STREAM_INFO -> {
                        if (h.payloadSize >= BsProtocol.STREAM_INFO_SIZE)
                            listener.onStreamInfo(BsProtocol.parseStreamInfo(payload))
                    }
                    BsProtocol.MSG_AUDIO -> {
                        if (h.payloadSize > BsProtocol.AUDIO_HEADER_SIZE) {
                            listener.onAudio(
                                payload,
                                BsProtocol.AUDIO_HEADER_SIZE,
                                h.payloadSize - BsProtocol.AUDIO_HEADER_SIZE
                            )
                        }
                    }
                    BsProtocol.MSG_SCREENS -> {
                        if (h.payloadSize >= 1) listener.onScreens(payload[0].toInt() and 0xFF)
                    }
                    BsProtocol.MSG_PING -> outQueue.offer(BsProtocol.emptyMessage(BsProtocol.MSG_PONG))
                }
            }
        } catch (e: Exception) {
            reason = e.message ?: e.javaClass.simpleName
            Log.w(TAG, "connection ended: $reason")
        } finally {
            running = false
            outQueue.offer(ByteArray(0))
            try {
                socket?.close()
            } catch (_: Exception) {
            }
            listener.onDisconnected(reason)
        }
    }

    private fun writeLoop(out: OutputStream) {
        try {
            while (running) {
                val msg = outQueue.take()
                if (msg.isEmpty()) break
                out.write(msg)
                out.flush()
            }
        } catch (e: Exception) {
            Log.w(TAG, "writer ended: ${e.message}")
        }
    }

    companion object {
        private const val TAG = "BsClient"
    }
}
