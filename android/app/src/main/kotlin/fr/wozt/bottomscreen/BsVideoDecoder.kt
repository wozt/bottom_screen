package fr.wozt.bottomscreen

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Hardware H.264 decoding straight onto a Surface.
 *
 * Decoding to a Surface rather than to a ByteBuffer is the whole reason
 * this is worth doing on a phone: the decoded picture never touches the
 * CPU, never gets copied, and never gets converted from YUV. The SoC
 * hands it to the compositor and the compositor draws it.
 *
 * The server sends SPS/PPS inline before every keyframe rather than out
 * of band, so there is no csd-0/csd-1 to configure with -- MediaCodec
 * picks the parameter sets out of the stream. That is what lets this
 * connect to a stream already in progress and start at the next
 * keyframe, which is exactly the case when a phone joins a running game.
 */
class BsVideoDecoder(
    private val surface: Surface,
    private val width: Int,
    private val height: Int
) {
    private var codec: MediaCodec? = null
    private val info = MediaCodec.BufferInfo()

    /** Frames dropped because no input buffer was free. */
    var starved = 0
        private set

    private var announcedFirstFrame = false

    fun start(): Boolean {
        return try {
            val format = MediaFormat.createVideoFormat(
                MediaFormat.MIMETYPE_VIDEO_AVC, width, height
            )
            /* Generous: a keyframe on a busy screen is far below this,
             * and a buffer too small is a decoder that fails on the one
             * frame that mattered. */
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 512 * 1024)

            /*
             * Ask the decoder not to buffer ahead. Without this a decoder
             * is free to hold several pictures back to keep its pipeline
             * full, which is sensible for video playback and exactly
             * wrong for something you are playing on.
             */
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }

            val c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            c.configure(format, surface, null, 0)
            c.start()
            codec = c
            true
        } catch (e: Exception) {
            Log.e(TAG, "cannot start the decoder", e)
            release()
            false
        }
    }

    /**
     * Feeds one encoded frame and draws whatever comes out.
     * Returns false if the decoder has failed and the caller should stop.
     */
    fun decode(data: ByteArray, offset: Int, length: Int, isKeyframe: Boolean): Boolean {
        val c = codec ?: return false
        try {
            /*
             * A short wait, not an indefinite one. If the decoder has no
             * free input buffer it is behind, and blocking here would
             * stall the socket read and turn a hiccup into a growing
             * backlog. Dropping the frame lets the stream catch up on
             * its own, and the next keyframe repairs the picture.
             */
            val inIndex = c.dequeueInputBuffer(2000)
            if (inIndex >= 0) {
                val buf: ByteBuffer? = c.getInputBuffer(inIndex)
                if (buf != null) {
                    buf.clear()
                    buf.put(data, offset, length)
                    val flags = if (isKeyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                    c.queueInputBuffer(inIndex, 0, length, 0, flags)
                }
            } else {
                starved++
            }

            /* Drain everything ready, so a picture is never left sitting
             * in the decoder waiting for the next frame to push it out. */
            while (true) {
                val outIndex = c.dequeueOutputBuffer(info, 0)
                if (outIndex < 0) break
                /* true: hand it to the surface, which is the draw. */
                c.releaseOutputBuffer(outIndex, true)

                // Said once. A black window has three quite different
                // causes -- nothing arriving, nothing decoding, or
                // nothing reaching the surface -- and they are
                // indistinguishable from the outside.
                if (!announcedFirstFrame) {
                    announcedFirstFrame = true
                    Log.i(TAG, "first frame decoded and handed to the surface")
                }
            }
            return true
        } catch (e: Exception) {
            Log.e(TAG, "decode failed", e)
            return false
        }
    }

    fun release() {
        try {
            codec?.stop()
        } catch (_: Exception) {
        }
        try {
            codec?.release()
        } catch (_: Exception) {
        }
        codec = null
    }

    companion object {
        private const val TAG = "BsVideoDecoder"
    }
}
