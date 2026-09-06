package fr.wozt.bottomscreen

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PointF
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot

/**
 * The on-screen controls.
 *
 * Multitouch throughout, and every control tracks which pointer id holds
 * it. Tracking pressed/not-pressed alone breaks the moment two thumbs
 * are down: lifting one would release a button the other is still on.
 *
 * The d-pad reports real diagonals -- a finger between up and right
 * sends both -- because a d-pad that cannot walk diagonally is useless
 * for most of what a DS asks of it.
 *
 * Three things are the person's to decide, because no default fits every
 * hand or every phone: how big the controls are, where they sit, and
 * whether they are being played or being arranged. The first two are
 * remembered per console and per orientation, since a layout that works
 * in landscape is not the one that works in portrait.
 */
class PadOverlay(context: Context) : View(context) {

    var onButton: ((code: Int, pressed: Boolean) -> Unit)? = null

    /** Axis value in the protocol's range, -32768..32767. */
    var onAxis: ((code: Int, value: Int) -> Unit)? = null

    /** A control was dragged to a new home. Coordinates are fractions of
     *  the view, so they survive a different screen or a rotation. */
    var onMoved: ((code: Int, fx: Float, fy: Float) -> Unit)? = null

    /** Long press outside any control: the way to the settings when the
     *  corner button is awkward to reach with a thumb. */
    var onLongPress: (() -> Unit)? = null

    var profile: ConsoleProfile = ConsoleProfile.DS
        set(value) { field = value; layoutControls(); invalidate() }

    /** Width of the band beside the picture in landscape; 0 = portrait. */
    var sideBand: Float = 0f
        set(value) { field = value; layoutControls(); invalidate() }

    /** 1.0 is the default size. Smaller thumbs, bigger phones, personal
     *  taste -- there is no single right answer, so it is a setting. */
    var buttonScale: Float = 1f
        set(value) { field = value; layoutControls(); invalidate() }

    /** Space kept clear at the top for the settings button, which lives
     *  in that corner and would otherwise sit on top of a shoulder. */
    var topReserve: Float = 0f
        set(value) { field = value; layoutControls(); invalidate() }

    /** In edit mode a drag moves a control instead of pressing it. */
    var editMode: Boolean = false
        set(value) {
            field = value
            releaseEverything()
            invalidate()
        }

    /** Where the person put things, as fractions of the view. */
    private val overrides = HashMap<Int, PointF>()

    fun setOverride(code: Int, fx: Float, fy: Float) {
        overrides[code] = PointF(fx, fy)
        layoutControls()
        invalidate()
    }

    fun clearOverrides() {
        overrides.clear()
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

    /* The d-pad is one control yielding four codes, not four controls: a
     * finger between two directions has to produce both, and four
     * separate rectangles cannot say that. It needs an id of its own for
     * the saved positions; the button codes start at 1. */
    private val dpadRect = RectF()

    /* The face buttons move as one diamond, never individually. Dragging
     * A away from B is not a layout anybody wants, and the relationship
     * between the four is the thing a thumb has learned. */
    private val faceCentre = PointF()

    /*
     * A stick is a base circle and a knob that follows the thumb inside
     * it. It keeps its own pointer id like every other control, so a
     * second thumb elsewhere cannot steal or release it.
     *
     * Released, the knob snaps back and both axes are sent as zero --
     * without that a stick left off-centre would keep walking the
     * character after the thumb has gone.
     */
    private class Stick(val spec: PadStick)
    {
        val centre = PointF()
        var radius = 0f
        val knob = PointF()
        var pointerId = -1
        val active: Boolean get() = pointerId >= 0
    }

    private val sticks = mutableListOf<Stick>()
    private var dpadPointer = -1
    private val dpadHeld = HashSet<Int>()

    private var dragging: Int? = null
    private var dragDx = 0f
    private var dragDy = 0f
    private var pressStart = 0L
    private var pressX = 0f
    private var pressY = 0f

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
    }
    private val frame = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = 0xFFFFD400.toInt()
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        super.onSizeChanged(w, h, ow, oh)
        layoutControls()
    }

    private fun unit(): Float {
        val h = height.toFloat()
        val base = if (sideBand > 0f) minOf(sideBand / 3.2f, h / 6.5f)
                   else minOf(width.toFloat(), h * 0.55f) * 0.13f
        return base * buttonScale
    }

    private fun layoutControls() {
        controls.clear()
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return

        sticks.clear()
        for (sp in profile.sticks) sticks.add(Stick(sp))

        if (sideBand > 0f) layoutBeside(w, h) else layoutBelow(w, h)
        applyOverrides(w, h)
        text.textSize = unit() * 0.42f
    }

    /* Portrait: three bands down the screen -- shoulders, then d-pad and
     * face buttons, then the menu row. Fixed offsets put a hole in the
     * middle of a tall phone and stacked B on START. */
    private fun layoutBelow(w: Float, h: Float) {
        val u = unit()
        val margin = u * 0.55f
        val shoulderH = u * 0.85f
        val menuH = u * 0.75f
        val top = margin + topReserve
        val midY = (top + shoulderH + (h - margin - menuH)) / 2f

        addShoulders(w, margin, top, u * 1.8f, shoulderH, false)

        val dpadSize = u * 3.0f
        dpadRect.set(margin, midY - dpadSize / 2f, margin + dpadSize, midY + dpadSize / 2f)

        addFaceDiamond(w - margin - u * 1.3f - u * 0.7f, midY, u)

        // Sticks sit below the thumb that uses them, between the middle
        // row and the menu row.
        val stickR = u * 1.15f
        val stickY = (midY + dpadSize / 2f + (h - margin - menuH)) / 2f
        placeSticks(w, stickY, margin + stickR, w - margin - stickR, stickR)

        val mw = u * 2.0f
        val total = profile.menuButtons.size * mw + (profile.menuButtons.size - 1) * margin
        var mx = (w - total) / 2f
        for (b in profile.menuButtons) {
            controls.add(Control(b.code, b.label,
                RectF(mx, h - margin - menuH, mx + mw, h - margin), false))
            mx += mw + margin
        }
    }

    /* Landscape: a band down each side of the picture, which is where a
     * console keeps them, so a thumb goes where it expects to. */
    private fun layoutBeside(w: Float, h: Float) {
        val u = unit()
        val band = sideBand
        val margin = u * 0.45f
        val shoulderH = u * 0.8f
        val menuH = u * 0.7f
        val midY = h / 2f

        addShoulders(w, margin, margin + topReserve,
                     minOf(u * 1.8f, band - margin * 2f), shoulderH, true)

        val dpadSize = minOf(u * 2.9f, band - margin * 2f)
        val dpadCx = band / 2f
        dpadRect.set(dpadCx - dpadSize / 2f, midY - dpadSize / 2f,
                     dpadCx + dpadSize / 2f, midY + dpadSize / 2f)

        addFaceDiamond(w - band / 2f, midY, u)

        val stickR = minOf(u * 1.05f, band / 2f - margin)
        val stickY = (midY + dpadSize / 2f + (h - margin - menuH)) / 2f
        placeSticks(w, stickY, band / 2f, w - band / 2f, stickR)

        val mw = minOf(u * 2.0f, band - margin * 2f)
        profile.menuButtons.forEachIndexed { i, b ->
            val centre = if (i == 0) band / 2f else w - band / 2f
            controls.add(Control(b.code, b.label,
                RectF(centre - mw / 2f, h - margin - menuH,
                      centre + mw / 2f, h - margin), false))
        }
    }

    private fun addShoulders(
        w: Float, x0: Float, y0: Float, sw: Float, sh: Float, stacked: Boolean
    ) {
        val left = profile.shoulders.filter {
            it.code == BsProtocol.BTN_L || it.code == BsProtocol.BTN_ZL
        }
        val right = profile.shoulders.filter {
            it.code == BsProtocol.BTN_R || it.code == BsProtocol.BTN_ZR
        }
        val step = if (stacked) sh + y0 * 0.7f else sw + x0
        left.forEachIndexed { i, b ->
            val x = if (stacked) x0 else x0 + i * step
            val y = if (stacked) y0 + i * step else y0
            controls.add(Control(b.code, b.label, RectF(x, y, x + sw, y + sh), false))
        }
        right.forEachIndexed { i, b ->
            val x = if (stacked) w - x0 - sw else w - x0 - sw - i * step
            val y = if (stacked) y0 + i * step else y0
            controls.add(Control(b.code, b.label, RectF(x, y, x + sw, y + sh), false))
        }
    }

    /* Y left, A right, X top, B bottom -- the diamond all three machines
     * use. */
    private fun addFaceDiamond(cx: Float, cy: Float, u: Float) {
        faceCentre.set(cx, cy)
        val r = u * 0.62f
        val spread = u * 1.2f
        fun face(code: Int, label: String, dx: Float, dy: Float) {
            controls.add(Control(code, label,
                RectF(cx + dx - r, cy + dy - r, cx + dx + r, cy + dy + r), true))
        }
        for (b in profile.faceButtons) {
            when (b.code) {
                BsProtocol.BTN_A -> face(b.code, b.label, spread, 0f)
                BsProtocol.BTN_B -> face(b.code, b.label, 0f, spread)
                BsProtocol.BTN_X -> face(b.code, b.label, 0f, -spread)
                BsProtocol.BTN_Y -> face(b.code, b.label, -spread, 0f)
            }
        }
    }

    private fun placeSticks(w: Float, y: Float, leftX: Float, rightX: Float, r: Float) {
        for (st in sticks) {
            st.radius = r
            val cx = if (st.spec.left) leftX else rightX
            st.centre.set(cx, y)
            st.knob.set(cx, y)
        }
    }

    private fun applyOverrides(w: Float, h: Float) {
        overrides[DPAD]?.let { recentre(dpadRect, it.x * w, it.y * h) }
        overrides[FACE]?.let { moveFaceTo(it.x * w, it.y * h) }
        for (st in sticks) {
            overrides[stickKey(st)]?.let {
                st.centre.set(it.x * w, it.y * h)
                if (!st.active) st.knob.set(st.centre)
            }
        }
        for (c in controls) {
            if (isFace(c.code)) continue      /* moved as a group, above */
            overrides[c.code]?.let { recentre(c.rect, it.x * w, it.y * h) }
        }
    }

    /* Saved positions are keyed by control id; sticks take negative ids
     * below the face group so they cannot collide with a button code. */
    private fun stickKey(st: Stick) = -10 - st.spec.axisX

    private fun isFace(code: Int) = code == BsProtocol.BTN_A ||
        code == BsProtocol.BTN_B || code == BsProtocol.BTN_X || code == BsProtocol.BTN_Y

    private fun moveFaceTo(cx: Float, cy: Float) {
        val dx = cx - faceCentre.x
        val dy = cy - faceCentre.y
        for (c in controls) if (isFace(c.code)) c.rect.offset(dx, dy)
        faceCentre.set(cx, cy)
    }

    /* The bounding box of the diamond, for the frame drawn round it in
     * edit mode -- one frame for the group, not four. */
    private fun faceBounds(): RectF? {
        var box: RectF? = null
        for (c in controls) if (isFace(c.code)) {
            box = box?.apply { union(c.rect) } ?: RectF(c.rect)
        }
        return box
    }

    private fun recentre(r: RectF, cx: Float, cy: Float) {
        val hw = r.width() / 2f
        val hh = r.height() / 2f
        r.set(cx - hw, cy - hh, cx + hw, cy + hh)
    }

    override fun onDraw(canvas: Canvas) {
        val u = unit()
        val pad = u * 0.22f

        val cx = dpadRect.centerX()
        val cy = dpadRect.centerY()
        val arm = dpadRect.width() / 3f
        fill.color = if (dpadHeld.isNotEmpty()) HELD else idle()
        stroke.color = edge()
        canvas.drawRect(cx - arm / 2f, dpadRect.top, cx + arm / 2f, dpadRect.bottom, fill)
        canvas.drawRect(dpadRect.left, cy - arm / 2f, dpadRect.right, cy + arm / 2f, fill)
        canvas.drawRect(cx - arm / 2f, dpadRect.top, cx + arm / 2f, dpadRect.bottom, stroke)
        canvas.drawRect(dpadRect.left, cy - arm / 2f, dpadRect.right, cy + arm / 2f, stroke)

        for (c in controls) {
            fill.color = if (c.pressed) HELD else idle()
            stroke.color = edge()
            if (c.round) {
                val r = c.rect.width() / 2f
                canvas.drawCircle(c.rect.centerX(), c.rect.centerY(), r, fill)
                canvas.drawCircle(c.rect.centerX(), c.rect.centerY(), r, stroke)
            } else {
                val r = u * 0.18f
                canvas.drawRoundRect(c.rect, r, r, fill)
                canvas.drawRoundRect(c.rect, r, r, stroke)
            }
            canvas.drawText(c.label, c.rect.centerX(),
                c.rect.centerY() - (text.descent() + text.ascent()) / 2f, text)
        }

        for (st in sticks) {
            fill.color = if (st.active) HELD else idle()
            stroke.color = edge()
            canvas.drawCircle(st.centre.x, st.centre.y, st.radius, stroke)
            canvas.drawCircle(st.knob.x, st.knob.y, st.radius * 0.45f, fill)
            canvas.drawCircle(st.knob.x, st.knob.y, st.radius * 0.45f, stroke)
            canvas.drawText(st.spec.label, st.centre.x,
                st.centre.y - st.radius - text.textSize * 0.35f, text)
        }

        if (!editMode) return

        /*
         * A yellow frame round everything that can be dragged, and one
         * frame round the whole diamond rather than four.
         *
         * Edit mode has to be unmistakable: a person who does not
         * realise they are in it will press a button, watch it slide,
         * and conclude the pad is broken.
         */
        frame.strokeWidth = maxOf(3f, u * 0.06f)
        drawFrame(canvas, dpadRect, pad)
        faceBounds()?.let { drawFrame(canvas, it, pad) }
        for (st in sticks)
            drawFrame(canvas, RectF(st.centre.x - st.radius, st.centre.y - st.radius,
                                    st.centre.x + st.radius, st.centre.y + st.radius), pad)
        for (c in controls) if (!isFace(c.code)) drawFrame(canvas, c.rect, pad)
    }

    private fun drawFrame(canvas: Canvas, r: RectF, pad: Float) {
        val box = RectF(r.left - pad, r.top - pad, r.right + pad, r.bottom + pad)
        canvas.drawRoundRect(box, pad, pad, frame)
    }

    /* Edit mode brightens everything, so it is obvious at a glance that a
     * drag will move a button rather than press it. */
    private fun idle() = if (editMode) 0x55FFFFFF else 0x30FFFFFF
    private fun edge() = if (editMode) 0xB0FFFFFF.toInt() else 0x60FFFFFF

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (editMode) return editTouch(event)

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                pressStart = System.currentTimeMillis()
                pressX = event.getX(i)
                pressY = event.getY(i)
                press(event.getPointerId(i), pressX, pressY)
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    val id = event.getPointerId(i)
                    if (id == dpadPointer) updateDpad(event.getX(i), event.getY(i))
                    for (st in sticks)
                        if (st.pointerId == id) updateStick(st, event.getX(i), event.getY(i))
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val i = event.actionIndex
                /* A long press on bare background is the other way into
                 * the settings, for when the corner button is not where
                 * a thumb wants to be. */
                if (event.actionMasked == MotionEvent.ACTION_UP &&
                    System.currentTimeMillis() - pressStart > 600 &&
                    hypot(event.getX(i) - pressX, event.getY(i) - pressY) < 40f &&
                    hitTest(pressX, pressY) == null && !nearDpad(pressX, pressY)
                ) {
                    onLongPress?.invoke()
                }
                release(event.getPointerId(i))
            }
            MotionEvent.ACTION_CANCEL -> releaseEverything()
        }
        invalidate()
        return true
    }

    private fun editTouch(event: MotionEvent): Boolean {
        val x = event.getX(0)
        val y = event.getY(0)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val stick = sticks.firstOrNull {
                    hypot(x - it.centre.x, y - it.centre.y) <= it.radius
                }
                if (stick != null) {
                    dragging = stickKey(stick)
                    dragDx = x - stick.centre.x
                    dragDy = y - stick.centre.y
                } else {
                val hit = hitTest(x, y)
                if (hit != null && isFace(hit.code)) {
                    dragging = FACE
                    dragDx = x - faceCentre.x
                    dragDy = y - faceCentre.y
                } else if (hit != null) {
                    dragging = hit.code
                    dragDx = x - hit.rect.centerX()
                    dragDy = y - hit.rect.centerY()
                } else if (nearDpad(x, y)) {
                    dragging = DPAD
                    dragDx = x - dpadRect.centerX()
                    dragDy = y - dpadRect.centerY()
                }
                }
            }
            MotionEvent.ACTION_MOVE -> {
                val code = dragging ?: return true
                val cx = (x - dragDx).coerceIn(0f, width.toFloat())
                val cy = (y - dragDy).coerceIn(0f, height.toFloat())
                val st = sticks.firstOrNull { stickKey(it) == code }
                when {
                    st != null -> { st.centre.set(cx, cy); st.knob.set(cx, cy) }
                    else -> when (code) {
                    DPAD -> recentre(dpadRect, cx, cy)
                    FACE -> moveFaceTo(cx, cy)
                    else -> controls.firstOrNull { it.code == code }
                        ?.let { recentre(it.rect, cx, cy) }
                    }
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val code = dragging
                dragging = null
                if (code != null && width > 0 && height > 0) {
                    val sk = sticks.firstOrNull { stickKey(it) == code }
                    val r = when {
                        sk != null -> RectF(sk.centre.x, sk.centre.y, sk.centre.x, sk.centre.y)
                        code == DPAD -> dpadRect
                        code == FACE -> RectF(faceCentre.x, faceCentre.y, faceCentre.x, faceCentre.y)
                        else -> controls.firstOrNull { it.code == code }?.rect
                    }
                    if (r != null) {
                        val fx = r.centerX() / width
                        val fy = r.centerY() / height
                        overrides[code] = PointF(fx, fy)
                        onMoved?.invoke(code, fx, fy)
                    }
                }
            }
        }
        invalidate()
        return true
    }

    private fun hitTest(x: Float, y: Float): Control? =
        controls.firstOrNull { it.rect.contains(x, y) }

    private fun press(pointerId: Int, x: Float, y: Float) {
        for (st in sticks) {
            if (st.active) continue
            if (hypot(x - st.centre.x, y - st.centre.y) <= st.radius) {
                st.pointerId = pointerId
                updateStick(st, x, y)
                return
            }
        }
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

    /* Slack around the d-pad, because a thumb that slides just off the
     * edge mid-movement should keep walking, not stop dead. */
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

    private fun release(pointerId: Int) {
        for (st in sticks) {
            if (st.pointerId == pointerId) {
                st.pointerId = -1
                st.knob.set(st.centre)
                // Zero both axes: a stick left off-centre would keep the
                // character walking after the thumb has left.
                onAxis?.invoke(st.spec.axisX, 0)
                onAxis?.invoke(st.spec.axisY, 0)
                return
            }
        }
        if (pointerId == dpadPointer) {
            for (code in dpadHeld) onButton?.invoke(code, false)
            dpadHeld.clear()
            dpadPointer = -1
            return
        }
        for (c in controls) {
            if (c.pointerId == pointerId) {
                c.pointerId = -1
                onButton?.invoke(c.code, false)
            }
        }
    }

    /*
     * Clamped to the base circle, then reported as a fraction of full
     * deflection. Clamping rather than letting the knob follow the thumb
     * outside keeps the maximum reachable in every direction, which is
     * what a real stick does at the edge of its gate.
     */
    private fun updateStick(st: Stick, x: Float, y: Float) {
        var dx = x - st.centre.x
        var dy = y - st.centre.y
        val dist = hypot(dx, dy)
        if (dist > st.radius && dist > 0f) {
            dx = dx / dist * st.radius
            dy = dy / dist * st.radius
        }
        st.knob.set(st.centre.x + dx, st.centre.y + dy)

        val fx = (dx / st.radius).coerceIn(-1f, 1f)
        val fy = (dy / st.radius).coerceIn(-1f, 1f)
        onAxis?.invoke(st.spec.axisX, (fx * 32767f).toInt())
        // Screen y grows downwards, sticks report up as positive.
        onAxis?.invoke(st.spec.axisY, (-fy * 32767f).toInt())
    }

    private fun releaseEverything() {
        for (c in controls) if (c.pressed) {
            c.pointerId = -1
            onButton?.invoke(c.code, false)
        }
        for (code in dpadHeld) onButton?.invoke(code, false)
        dpadHeld.clear()
        dpadPointer = -1
        dragging = null
        for (st in sticks) if (st.active) {
            st.pointerId = -1
            st.knob.set(st.centre)
            onAxis?.invoke(st.spec.axisX, 0)
            onAxis?.invoke(st.spec.axisY, 0)
        }
    }

    companion object {
        /** The d-pad's id for saved positions; button codes start at 1. */
        const val DPAD = 0

        /** The face diamond's id. It moves as one, so it saves one
         *  position rather than four. */
        const val FACE = -1
        private const val HELD = 0x90FFFFFF.toInt()
    }
}
