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

        val DS = ConsoleProfile(
            BsProtocol.CONSOLE_DS, "Nintendo DS", 256, 192,
            faceButtons = listOf(A, B, X, Y),
            shoulders = listOf(L, R),
            menuButtons = listOf(SELECT, START)
        )

        val N3DS = ConsoleProfile(
            BsProtocol.CONSOLE_3DS, "Nintendo 3DS", 320, 240,
            faceButtons = listOf(A, B, X, Y),
            shoulders = listOf(L, R, ZL, ZR),
            menuButtons = listOf(SELECT, START)
        )

        val WIIU = ConsoleProfile(
            BsProtocol.CONSOLE_WIIU, "Wii U GamePad", 854, 480,
            faceButtons = listOf(A, B, X, Y),
            shoulders = listOf(L, R, ZL, ZR),
            menuButtons = listOf(SELECT, START)
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
