package fr.wozt.bottomscreen

import android.content.Context
import android.view.MotionEvent
import android.view.SurfaceView

/**
 * The console's screen, at a whole-number multiple of its real size.
 *
 * The view is sized to exactly native x k, so the decoder's output lands
 * on it one source pixel to k by k screen pixels with nothing to
 * resample. Letting it stretch to fill the phone would resample pixel
 * art into a smeared mess, and on a 256x192 screen there is nothing to
 * hide it behind.
 *
 * Because the view is exactly native x k, turning a touch into a console
 * coordinate is a division and nothing else.
 */
class ScreenView(context: Context) : SurfaceView(context) {

    var onTouch: ((type: Int, x: Int, y: Int) -> Unit)? = null

    var nativeWidth = 256
    var nativeHeight = 192

    /* The stylus is one point: the DS digitiser cannot report two, so
     * neither do we. The first finger down owns the screen until it
     * lifts, which also stops a stray palm from yanking the stylus
     * across the screen mid-drag. */
    private var activePointer = -1

    /**
     * False while the top screen is being shown.
     *
     * There is no touch panel up there, so a tap is not a tap that
     * missed -- it is one that should never have been sent. The server
     * drops them as well, but a press arriving from a screen that
     * cannot be pressed is worth stopping at both ends.
     */
    var touchEnabled = true

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!touchEnabled) {
            if (activePointer != -1) {
                activePointer = -1
                onTouch?.invoke(BsProtocol.INPUT_TOUCH_UP, 0, 0)
            }
            return true
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                if (activePointer == -1) {
                    val i = event.actionIndex
                    activePointer = event.getPointerId(i)
                    report(BsProtocol.INPUT_TOUCH_DOWN, event.getX(i), event.getY(i))
                }
            }
            MotionEvent.ACTION_MOVE -> {
                val i = event.findPointerIndex(activePointer)
                if (i >= 0) report(BsProtocol.INPUT_TOUCH_MOVE, event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                if (event.getPointerId(event.actionIndex) == activePointer) {
                    activePointer = -1
                    onTouch?.invoke(BsProtocol.INPUT_TOUCH_UP, 0, 0)
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                if (activePointer != -1) {
                    activePointer = -1
                    onTouch?.invoke(BsProtocol.INPUT_TOUCH_UP, 0, 0)
                }
            }
        }
        return true
    }

    private fun report(type: Int, px: Float, py: Float) {
        if (width <= 0 || height <= 0) return
        var cx = (px * nativeWidth / width).toInt()
        var cy = (py * nativeHeight / height).toInt()
        /* A finger that slides off the edge mid-drag should stay at the
         * edge, the way a stylus does, rather than report a coordinate
         * off the screen. */
        if (cx < 0) cx = 0
        if (cx >= nativeWidth) cx = nativeWidth - 1
        if (cy < 0) cy = 0
        if (cy >= nativeHeight) cy = nativeHeight - 1
        onTouch?.invoke(type, cx, cy)
    }
}
