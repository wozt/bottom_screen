package fr.wozt.bottomscreen

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs

/**
 * A physical controller plugged into or paired with the phone.
 *
 * The protocol is already abstract -- the client says A, the server's
 * backend decides what A means on that console -- so a pad needs no new
 * messages, only a mapping onto the same button codes the on-screen
 * buttons send.
 */
object Gamepad {

    /**
     * Android names its buttons the way an Xbox pad is labelled, where A
     * is the bottom of the diamond. Every console here labels the right
     * one A. So the mapping is positional rather than by name: the
     * button under your thumb does what the same button does on the
     * console, which is what somebody holding the pad expects.
     *
     * A driver that presents a Nintendo pad already normalises it to
     * these names, so this holds for those too.
     */
    fun buttonFor(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> BsProtocol.BTN_B      // bottom
        KeyEvent.KEYCODE_BUTTON_B -> BsProtocol.BTN_A      // right
        KeyEvent.KEYCODE_BUTTON_X -> BsProtocol.BTN_Y      // left
        KeyEvent.KEYCODE_BUTTON_Y -> BsProtocol.BTN_X      // top
        KeyEvent.KEYCODE_BUTTON_L1 -> BsProtocol.BTN_L
        KeyEvent.KEYCODE_BUTTON_R1 -> BsProtocol.BTN_R
        KeyEvent.KEYCODE_BUTTON_L2 -> BsProtocol.BTN_ZL
        KeyEvent.KEYCODE_BUTTON_R2 -> BsProtocol.BTN_ZR
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> BsProtocol.BTN_START
        KeyEvent.KEYCODE_BUTTON_SELECT -> BsProtocol.BTN_SELECT
        KeyEvent.KEYCODE_BUTTON_MODE -> BsProtocol.BTN_HOME
        KeyEvent.KEYCODE_DPAD_UP -> BsProtocol.BTN_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> BsProtocol.BTN_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> BsProtocol.BTN_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> BsProtocol.BTN_RIGHT
        else -> 0
    }

    fun isFromGamepad(event: KeyEvent): Boolean =
        (event.source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
        (event.source and InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD ||
        (event.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

    fun isFromJoystick(event: MotionEvent): Boolean =
        (event.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

    /** True while at least one pad is present. */
    fun anyConnected(): Boolean = InputDevice.getDeviceIds().any { id ->
        val d = InputDevice.getDevice(id) ?: return@any false
        val sources = d.sources
        !d.isVirtual &&
            ((sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
             (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
    }

    fun nameOfFirst(): String? {
        for (id in InputDevice.getDeviceIds()) {
            val d = InputDevice.getDevice(id) ?: continue
            val sources = d.sources
            if (!d.isVirtual &&
                ((sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                 (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)) {
                return d.name
            }
        }
        return null
    }

    /**
     * Sticks rest slightly off centre on worn hardware, so anything
     * inside the pad's own reported flat zone is treated as centre. Sent
     * raw otherwise: the console decides what a small push means, and
     * squaring or scaling it here would be inventing a feel.
     */
    fun axis(event: MotionEvent, axis: Int): Int {
        val value = event.getAxisValue(axis)
        val range = event.device?.getMotionRange(axis, event.source)
        val flat = range?.flat ?: 0.05f
        if (abs(value) <= maxOf(flat, 0.05f)) return 0
        return (value.coerceIn(-1f, 1f) * 32767f).toInt()
    }

    /**
     * The d-pad reaches us as a hat on most pads and as key events on
     * some, so both are handled. Returns the four directions as a set of
     * protocol codes.
     */
    fun hatButtons(event: MotionEvent): Set<Int> {
        val x = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val y = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        val held = HashSet<Int>()
        if (x < -0.5f) held.add(BsProtocol.BTN_LEFT)
        if (x > 0.5f) held.add(BsProtocol.BTN_RIGHT)
        if (y < -0.5f) held.add(BsProtocol.BTN_UP)
        if (y > 0.5f) held.add(BsProtocol.BTN_DOWN)
        return held
    }
}
