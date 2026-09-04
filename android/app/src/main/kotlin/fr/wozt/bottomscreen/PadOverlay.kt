package fr.wozt.bottomscreen

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot

/**
 * The on-screen buttons.
 *
 * Multitouch throughout, and every control tracks which pointer id is
 * holding it. Tracking pressed/not-pressed alone breaks the moment two
 * thumbs are down: lifting one would release a button the other is still
 * on. That is the difference between a pad you can play with and a pad
 * that fights you.
 *
 * The d-pad reports real diagonals -- a finger between up and right
 * sends both -- because the alternative is a d-pad that cannot walk
 * diagonally, which every DS game asks for.
 */
class PadOverlay(context: Context) : View(context) {

    var onButton: ((code: Int, pressed: Boolean) -> Unit)? = null

    var profile: ConsoleProfile = ConsoleProfile.DS
        set(value) {
            field = value
            layoutControls()
            invalidate()
        }

    /*
     * In landscape the picture sits in the middle and the controls go
     * either side of it, the way they do on the console itself. This is
     * how wide each side band is; 0 means portrait, where the picture is
     * above and the controls have the whole width below it.
     */
    var sideBand: Float = 0f
        set(value) {
            field = value
            layoutControls()
            invalidate()
        }

    private class Control(
        val code: Int,
        val label: String,
        val rect: RectF,
        val round: Boolean
    ) {
        var pointerId = -1
        val pressed: Boolean get() = pointerId >= 0
    }

    private val controls = mutableListOf<Control>()

    /* The d-pad is one control that yields four codes, not four
     * controls: a finger placed between two directions has to produce
     * both, and four separate rectangles cannot say that. */
    private val dpadRect = RectF()
    private var dpadPointer = -1
    private val dpadHeld = HashSet<Int>()

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        super.onSizeChanged(w, h, ow, oh)
        layoutControls()
    }

    /*
     * Three bands, so nothing can land on top of anything else.
     *
     *   top     shoulders
     *   middle  d-pad on the left, face buttons on the right
     *   bottom  SELECT and START
     *
     * The first version placed the shoulders at the top margin and
     * everything else against the bottom, which on a tall phone left a
     * hole in the middle and stacked B on START and Y on the d-pad. The
     * bands are measured from the height that is actually available
     * rather than from fixed offsets.
     */
    private fun layoutControls() {
        controls.clear()
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        if (sideBand > 0f) {
            layoutBeside(w, h)
            return
        }

        val unit = minOf(w, h * 0.55f) * 0.16f
        val margin = unit * 0.5f

        val shoulderH = unit * 0.8f
        val menuH = unit * 0.7f
        val topBand = margin + shoulderH
        val bottomBand = h - margin - menuH
        /* Where the thumbs sit: the middle of what is left once the
         * shoulders and the menu row have taken theirs. */
        val midY = (topBand + bottomBand) / 2f

        // --- shoulders, across the top
        val sw = unit * 1.6f
        val left = profile.shoulders.filter {
            it.code == BsProtocol.BTN_L || it.code == BsProtocol.BTN_ZL
        }
        val right = profile.shoulders.filter {
            it.code == BsProtocol.BTN_R || it.code == BsProtocol.BTN_ZR
        }
        left.forEachIndexed { i, b ->
            val x = margin + i * (sw + margin)
            controls.add(Control(b.code, b.label, RectF(x, margin, x + sw, margin + shoulderH), false))
        }
        right.forEachIndexed { i, b ->
            val x = w - margin - sw - i * (sw + margin)
            controls.add(Control(b.code, b.label, RectF(x, margin, x + sw, margin + shoulderH), false))
        }

        // --- d-pad, middle left
        val dpadSize = unit * 3.0f
        dpadRect.set(margin, midY - dpadSize / 2f, margin + dpadSize, midY + dpadSize / 2f)

        // --- face buttons, middle right, in the diamond these machines
        //     all use: Y left, A right, X top, B bottom.
        val r = unit * 0.66f
        val spread = unit * 1.25f
        val cx = w - margin - spread - r
        val cy = midY
        fun face(code: Int, label: String, dx: Float, dy: Float) {
            controls.add(
                Control(code, label,
                    RectF(cx + dx - r, cy + dy - r, cx + dx + r, cy + dy + r), true)
            )
        }
        for (b in profile.faceButtons) {
            when (b.code) {
                BsProtocol.BTN_A -> face(b.code, b.label, spread, 0f)
                BsProtocol.BTN_B -> face(b.code, b.label, 0f, spread)
                BsProtocol.BTN_X -> face(b.code, b.label, 0f, -spread)
                BsProtocol.BTN_Y -> face(b.code, b.label, -spread, 0f)
            }
        }

        // --- SELECT and START, centred along the bottom
        val mw = unit * 1.9f
        val gap = margin
        val total = profile.menuButtons.size * mw + (profile.menuButtons.size - 1) * gap
        var mx = (w - total) / 2f
        for (b in profile.menuButtons) {
            controls.add(Control(b.code, b.label, RectF(mx, bottomBand, mx + mw, bottomBand + menuH), false))
            mx += mw + gap
        }

        text.textSize = unit * 0.40f
    }

    /*
     * Landscape: one band down each side of the picture.
     *
     *   left    L on top, d-pad in the middle, SELECT below
     *   right   R on top, face buttons in the middle, START below
     *
     * Which is where they are on a DS, so a thumb goes where it expects
     * to rather than where a phone layout happened to put them.
     */
    private fun layoutBeside(w: Float, h: Float) {
        val band = sideBand
        val unit = minOf(band / 2.7f, h / 5.5f)
        val margin = unit * 0.42f

        val shoulderH = unit * 0.75f
        val menuH = unit * 0.65f
        val midY = h / 2f

        val sw = minOf(unit * 1.7f, band - margin * 2f)
        val leftShoulders = profile.shoulders.filter {
            it.code == BsProtocol.BTN_L || it.code == BsProtocol.BTN_ZL
        }
        val rightShoulders = profile.shoulders.filter {
            it.code == BsProtocol.BTN_R || it.code == BsProtocol.BTN_ZR
        }
        leftShoulders.forEachIndexed { i, b ->
            val y = margin + i * (shoulderH + margin * 0.6f)
            controls.add(Control(b.code, b.label,
                RectF(margin, y, margin + sw, y + shoulderH), false))
        }
        rightShoulders.forEachIndexed { i, b ->
            val y = margin + i * (shoulderH + margin * 0.6f)
            controls.add(Control(b.code, b.label,
                RectF(w - margin - sw, y, w - margin, y + shoulderH), false))
        }

        val dpadSize = minOf(unit * 2.8f, band - margin * 2f)
        val dpadCx = band / 2f
        dpadRect.set(dpadCx - dpadSize / 2f, midY - dpadSize / 2f,
                     dpadCx + dpadSize / 2f, midY + dpadSize / 2f)

        val r = unit * 0.6f
        val spread = unit * 1.15f
        val cx = w - band / 2f
        fun face(code: Int, label: String, dx: Float, dy: Float) {
            controls.add(Control(code, label,
                RectF(cx + dx - r, midY + dy - r, cx + dx + r, midY + dy + r), true))
        }
        for (b in profile.faceButtons) {
            when (b.code) {
                BsProtocol.BTN_A -> face(b.code, b.label, spread, 0f)
                BsProtocol.BTN_B -> face(b.code, b.label, 0f, spread)
                BsProtocol.BTN_X -> face(b.code, b.label, 0f, -spread)
                BsProtocol.BTN_Y -> face(b.code, b.label, -spread, 0f)
            }
        }

        /* SELECT goes under the d-pad and START under the face buttons,
         * one per band, rather than both crowding one side. */
        val mw = minOf(unit * 1.9f, band - margin * 2f)
        profile.menuButtons.forEachIndexed { i, b ->
            val centre = if (i == 0) band / 2f else w - band / 2f
            controls.add(Control(b.code, b.label,
                RectF(centre - mw / 2f, h - margin - menuH,
                      centre + mw / 2f, h - margin), false))
        }

        text.textSize = unit * 0.38f
    }

    override fun onDraw(canvas: Canvas) {
        val unit = if (sideBand > 0f) minOf(sideBand / 2.7f, height / 5.5f)
                   else minOf(width.toFloat(), height * 0.55f) * 0.16f

        // D-pad
        val cx = dpadRect.centerX()
        val cy = dpadRect.centerY()
        val arm = dpadRect.width() / 3f
        fill.color = if (dpadHeld.isNotEmpty()) HELD else IDLE
        stroke.color = EDGE
        canvas.drawRect(cx - arm / 2f, dpadRect.top, cx + arm / 2f, dpadRect.bottom, fill)
        canvas.drawRect(dpadRect.left, cy - arm / 2f, dpadRect.right, cy + arm / 2f, fill)
        canvas.drawRect(cx - arm / 2f, dpadRect.top, cx + arm / 2f, dpadRect.bottom, stroke)
        canvas.drawRect(dpadRect.left, cy - arm / 2f, dpadRect.right, cy + arm / 2f, stroke)

        for (c in controls) {
            fill.color = if (c.pressed) HELD else IDLE
            stroke.color = EDGE
            if (c.round) {
                val r = c.rect.width() / 2f
                canvas.drawCircle(c.rect.centerX(), c.rect.centerY(), r, fill)
                canvas.drawCircle(c.rect.centerX(), c.rect.centerY(), r, stroke)
            } else {
                val r = unit * 0.18f
                canvas.drawRoundRect(c.rect, r, r, fill)
                canvas.drawRoundRect(c.rect, r, r, stroke)
            }
            canvas.drawText(
                c.label, c.rect.centerX(),
                c.rect.centerY() - (text.descent() + text.ascent()) / 2f, text
            )
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                press(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    val id = event.getPointerId(i)
                    if (id == dpadPointer) updateDpad(event.getX(i), event.getY(i))
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                release(event.getPointerId(event.actionIndex))
            }
            MotionEvent.ACTION_CANCEL -> {
                for (c in controls) if (c.pressed) {
                    c.pointerId = -1
                    onButton?.invoke(c.code, false)
                }
                clearDpad()
            }
        }
        invalidate()
        return true
    }

    private fun press(pointerId: Int, x: Float, y: Float) {
        if (dpadRect.contains(x, y) || nearDpad(x, y)) {
            dpadPointer = pointerId
            updateDpad(x, y)
            return
        }
        for (c in controls) {
            if (c.pressed) continue
            if (c.rect.contains(x, y)) {
                c.pointerId = pointerId
                onButton?.invoke(c.code, true)
                return
            }
        }
    }

    /* A little slack around the d-pad, because a thumb that slides just
     * off the edge mid-movement should keep walking, not stop dead. */
    private fun nearDpad(x: Float, y: Float): Boolean {
        val r = dpadRect.width() * 0.6f
        return hypot(x - dpadRect.centerX(), y - dpadRect.centerY()) < r
    }

    private fun updateDpad(x: Float, y: Float) {
        val cx = dpadRect.centerX()
        val cy = dpadRect.centerY()
        val dead = dpadRect.width() * 0.14f

        val wanted = HashSet<Int>()
        if (x < cx - dead) wanted.add(BsProtocol.BTN_LEFT)
        if (x > cx + dead) wanted.add(BsProtocol.BTN_RIGHT)
        if (y < cy - dead) wanted.add(BsProtocol.BTN_UP)
        if (y > cy + dead) wanted.add(BsProtocol.BTN_DOWN)

        for (code in dpadHeld - wanted) onButton?.invoke(code, false)
        for (code in wanted - dpadHeld) onButton?.invoke(code, true)
        dpadHeld.clear()
        dpadHeld.addAll(wanted)
    }

    private fun clearDpad() {
        for (code in dpadHeld) onButton?.invoke(code, false)
        dpadHeld.clear()
        dpadPointer = -1
    }

    private fun release(pointerId: Int) {
        if (pointerId == dpadPointer) {
            clearDpad()
            return
        }
        for (c in controls) {
            if (c.pointerId == pointerId) {
                c.pointerId = -1
                onButton?.invoke(c.code, false)
            }
        }
    }

    companion object {
        private const val IDLE = 0x30FFFFFF
        private const val HELD = 0x90FFFFFF.toInt()
        private const val EDGE = 0x60FFFFFF
    }
}
