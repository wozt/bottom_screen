package fr.wozt.bottomscreen

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import java.nio.ByteBuffer

/**
 * Opus decoding and playback.
 *
 * The buffer is deliberately small. A media player would queue a
 * comfortable second of sound to ride out any hiccup; here that second
 * would be a second of delay between pressing a button and hearing it,
 * which is the one thing this must not have. Two of the server's 20 ms
 * blocks is the target, and an underrun is a click -- an acceptable
 * price, and recoverable, where constant lag is neither.
 *
 * Playback runs on the network thread, like the video decode: MediaCodec
 * and AudioTrack both do their work elsewhere, and a hop through another
 * queue would only add somewhere for latency to hide.
 */
class BsAudioPlayer(
    private val rate: Int,
    private val channels: Int
) {
    private var codec: MediaCodec? = null
    private var track: AudioTrack? = null
    private val info = MediaCodec.BufferInfo()

    var dropped = 0
        private set

    private var announcedFirst = false

    /** 0..1, applied by AudioTrack rather than to the samples. */
    var volume: Float = 1f
        set(value) {
            field = value.coerceIn(0f, 1f)
            try {
                track?.setVolume(field)
            } catch (_: Exception) {
            }
        }

    fun start(): Boolean {
        return try {
            val channelMask =
                if (channels >= 2) AudioFormat.CHANNEL_OUT_STEREO
                else AudioFormat.CHANNEL_OUT_MONO

            val minBuf = AudioTrack.getMinBufferSize(
                rate, channelMask, AudioFormat.ENCODING_PCM_16BIT
            )
            /* The floor Android gives is already tuned for latency on
             * this device; asking for less is refused, and asking for
             * much more is how a media player behaves. */
            val bufSize = maxOf(minBuf, rate * channels * 2 * 40 / 1000)

            val t = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(rate)
                        .setChannelMask(channelMask)
                        .build()
                )
                .setBufferSizeInBytes(bufSize)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            t.play()
            track = t

            val format = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_OPUS, rate, channels
            )
            /*
             * Opus in MediaCodec wants three csd buffers: the identification
             * header, then the pre-skip and seek pre-roll as 64-bit
             * nanosecond values. The server sends raw packets with no
             * header of their own, so it is built here from what the
             * handshake already told us.
             */
            format.setByteBuffer("csd-0", opusIdHeader(rate, channels))
            format.setByteBuffer("csd-1", longLe(PRE_SKIP_NS))
            format.setByteBuffer("csd-2", longLe(SEEK_PREROLL_NS))

            val c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
            c.configure(format, null, null, 0)
            c.start()
            codec = c
            true
        } catch (e: Exception) {
            Log.e(TAG, "cannot start audio", e)
            release()
            false
        }
    }

    fun decode(data: ByteArray, offset: Int, length: Int) {
        val c = codec ?: return
        val t = track ?: return
        try {
            val inIndex = c.dequeueInputBuffer(1000)
            if (inIndex >= 0) {
                c.getInputBuffer(inIndex)?.apply {
                    clear()
                    put(data, offset, length)
                }
                c.queueInputBuffer(inIndex, 0, length, 0, 0)
            } else {
                /* Behind: drop rather than wait. Sound that arrives late
                 * is worse than sound that is missing. */
                dropped++
            }

            while (true) {
                val outIndex = c.dequeueOutputBuffer(info, 0)
                if (outIndex < 0) break
                val buf: ByteBuffer? = c.getOutputBuffer(outIndex)
                if (buf != null && info.size > 0) {
                    val pcm = ByteArray(info.size)
                    buf.position(info.offset)
                    buf.get(pcm, 0, info.size)
                    val written = t.write(pcm, 0, pcm.size, AudioTrack.WRITE_NON_BLOCKING)

                    // Said once, like the video's. Silence has three
                    // causes -- nothing arriving, nothing decoding,
                    // nothing reaching the track -- and from outside
                    // they look identical.
                    if (!announcedFirst) {
                        announcedFirst = true
                        Log.i(TAG, "first audio decoded, $written of ${pcm.size} bytes written")
                    }
                }
                c.releaseOutputBuffer(outIndex, false)
            }
        } catch (e: Exception) {
            Log.w(TAG, "audio decode failed: ${e.message}")
        }
    }

    fun release() {
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        try { track?.stop() } catch (_: Exception) {}
        try { track?.release() } catch (_: Exception) {}
        codec = null
        track = null
    }

    private fun opusIdHeader(rate: Int, channels: Int): ByteBuffer {
        val b = ByteBuffer.allocate(19).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        b.put("OpusHead".toByteArray(Charsets.US_ASCII))
        b.put(1)                       // version
        b.put(channels.toByte())
        b.putShort(0)                  // pre-skip, in samples
        b.putInt(rate)
        b.putShort(0)                  // output gain
        b.put(0)                       // channel mapping family
        b.flip()
        return b
    }

    private fun longLe(v: Long): ByteBuffer {
        val b = ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        b.putLong(v)
        b.flip()
        return b
    }

    companion object {
        private const val TAG = "BsAudioPlayer"
        private const val PRE_SKIP_NS = 0L
        private const val SEEK_PREROLL_NS = 80_000_000L   // 80 ms, the Opus default
    }
}
