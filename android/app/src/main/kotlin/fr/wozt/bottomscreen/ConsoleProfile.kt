package fr.wozt.bottomscreen

/**
 * What each machine actually has.
 *
 * The protocol is deliberately abstract -- the client says A, the
 * server's backend decides what A means on that console -- so the only
 * thing that changes per machine is which controls to draw. A DS has no
 * ZL, and drawing one would be offering something that goes nowhere.
 */
data class ConsoleProfile(
    val console: Int,
    val label: String,
    val width: Int,
    val height: Int,
    val faceButtons: List<PadButton>,
    val shoulders: List<PadButton>,
    val menuButtons: List<PadButton>,
    val sticks: List<PadStick> = emptyList(),
    val hasDpad: Boolean = true
) {
    companion object {
        private val A = PadButton(BsProtocol.BTN_A, "A")
        private val B = PadButton(BsProtocol.BTN_B, "B")
        private val X = PadButton(BsProtocol.BTN_X, "X")
        private val Y = PadButton(BsProtocol.BTN_Y, "Y")
        private val L = PadButton(BsProtocol.BTN_L, "L")
        private val R = PadButton(BsProtocol.BTN_R, "R")
        private val ZL = PadButton(BsProtocol.BTN_ZL, "ZL")
        private val ZR = PadButton(BsProtocol.BTN_ZR, "ZR")
        private val START = PadButton(BsProtocol.BTN_START, "START")
        private val SELECT = PadButton(BsProtocol.BTN_SELECT, "SELECT")
        private val HOME = PadButton(BsProtocol.BTN_HOME, "HOME")

        private val LEFT_STICK = PadStick(
            BsProtocol.AXIS_LEFT_X, BsProtocol.AXIS_LEFT_Y, "L", left = true)
        private val RIGHT_STICK = PadStick(
            BsProtocol.AXIS_RIGHT_X, BsProtocol.AXIS_RIGHT_Y, "R", left = false)
        private val CIRCLE_PAD = PadStick(
            BsProtocol.AXIS_LEFT_X, BsProtocol.AXIS_LEFT_Y, "\u25CB", left = true)
        private val C_STICK = PadStick(
            BsProtocol.AXIS_RIGHT_X, BsProtocol.AXIS_RIGHT_Y, "C", left = false)

        /* The DS has no analog stick at all, so it gets none -- drawing
         * one would offer a control that goes nowhere. */
        val DS = ConsoleProfile(
            BsProtocol.CONSOLE_DS, "Nintendo DS", 256, 192,
            faceButtons = listOf(A, B, X, Y),
            shoulders = listOf(L, R),
            menuButtons = listOf(SELECT, START)
        )

        /* Circle pad on the left, C-stick on the right. The C-stick is
         * a nub on real hardware rather than a full stick, but it
         * reports the same two axes, so it is drawn the same. */
        val N3DS = ConsoleProfile(
            BsProtocol.CONSOLE_3DS, "Nintendo 3DS", 320, 240,
            faceButtons = listOf(A, B, X, Y),
            shoulders = listOf(L, R, ZL, ZR),
            menuButtons = listOf(SELECT, START),
            sticks = listOf(CIRCLE_PAD, C_STICK)
        )

        val WIIU = ConsoleProfile(
            BsProtocol.CONSOLE_WIIU, "Wii U GamePad", 854, 480,
            faceButtons = listOf(A, B, X, Y),
            shoulders = listOf(L, R, ZL, ZR),
            menuButtons = listOf(SELECT, START, HOME),
            sticks = listOf(LEFT_STICK, RIGHT_STICK)
        )

        /* The server announces which console it is serving, so the
         * interface follows the stream rather than a setting the person
         * has to keep in step with it. */
        fun forConsole(console: Int): ConsoleProfile = when (console) {
            BsProtocol.CONSOLE_3DS -> N3DS
            BsProtocol.CONSOLE_WIIU -> WIIU
            else -> DS
        }
    }
}

data class PadButton(val code: Int, val label: String)

/**
 * An analog stick: two axes and a side of the screen.
 *
 * [left] decides which thumb it sits under, which is the only thing the
 * layout needs to know -- the axis codes carry the rest, and the
 * server's backend decides what they mean on that machine.
 */
data class PadStick(
    val axisX: Int,
    val axisY: Int,
    val label: String,
    val left: Boolean
)
