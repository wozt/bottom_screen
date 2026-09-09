package fr.wozt.bottomscreen

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView

/**
 * The settings, as a panel rather than a dialog.
 *
 * Dressed like capture2cloud's, deliberately: these are two halves of
 * the same habit and somebody moving between them should not have to
 * learn a second set of manners. Dark blue ground, a cyan heading for
 * whatever is open, readouts in a muted blue-grey, and categories that
 * are either laid out in columns or opened one at a time depending on
 * how much room there is.
 *
 * It replaced an AlertDialog, which had a worse failing than looking
 * different: in landscape it was shorter than its own contents, so
 * everything from the volume down was unreachable with nothing on
 * screen to suggest it was there.
 */
class SettingsPanel(
    context: Context,
    private val settings: PanelState,
    private val actions: Actions,
) : LinearLayout(context) {

    /** What the panel reads and writes. The activity owns all of it. */
    interface PanelState {
        var host: String
        var port: Int
        /* Per screen, because the two are separate encoders on the
         * server and so genuinely separate settings: a television
         * picture worth 8 Mbit/s and a GamePad screen worth 2 are a
         * normal pair of answers, not a contradiction. Sound is not
         * here, because there is one set of speakers whatever is being
         * watched. */
        fun qualityOf(screen: Int): Quality
        fun setQuality(screen: Int, q: Quality)
        fun scaleOf(screen: Int): Int
        fun setScale(screen: Int, v: Int)
        var quality: Quality
        var volume: Float
        var muted: Boolean
        var buttonScale: Float
        var padVisibility: PadVisibility
        var fullscreen: Boolean
        var editMode: Boolean
        /**
         * What to receive, as a multiple of the console's own screen:
         * 1 is native, 2 is twice that, and so on. 0 means whatever the
         * emulator happens to render.
         *
         * -2 is the one exception, and only the Wii U has it: half of
         * 854x480. Below native is otherwise not offered at all -- a
         * fraction of a DS screen is smaller than any decoder will
         * produce frames from, and asking for one silently hands you a
         * black screen.
         */
        var receiveScale: Int
        var menuColumns: Boolean
        /** BsProtocol.AUDIO_BOTH / AUDIO_TV / AUDIO_PAD. Only a Wii U
         *  has two outputs, so only there is it offered. */
        /** Index into PadOverlay.PALETTE. */
        var padColour: Int
        /** Per side: is the stick below its thumb control? 0 is the left. */
        var stickBelow: BooleanArray
        val hasSticks: Boolean
        var audioSource: Int
        /** BsProtocol.SCREEN_BOTTOM or SCREEN_TOP. */
        var screenShown: Int
        /** Whether this server has a second screen to offer at all. */
        val hasTopScreen: Boolean
        val hasAudio: Boolean
        val isWiiU: Boolean
        val streamLine: String
        val savedServers: List<Profile>

        /** The console's own screen, which every offered size is a
         *  multiple of. */
        fun nativeWidthOf(screen: Int): Int
        fun nativeHeightOf(screen: Int): Int
        val nativeWidth: Int
        val nativeHeight: Int

        /** What the emulator is rendering, which is the ceiling: asking
         *  for more than exists would only upscale. */
        val sourceWidth: Int
        val sourceHeight: Int
    }

    interface Actions {
        fun onClose()
        fun onApply()
        fun onReconnect(host: String, port: Int)
        fun onSaveServer()
        fun onForgetServer(profile: Profile)
        fun onResetLayout()
    }

    private var body = LinearLayout(context)
    private var open: String? = "connection"

    init {
        setBackgroundColor(0xF00B1A26.toInt())
        orientation = VERTICAL
        build()
    }

    fun rebuild() {
        removeAllViews()
        build()
    }

    private fun build() {
        if (settings.menuColumns) buildColumns() else buildAccordion()
    }

    private fun buildColumns() {
        val outer = LinearLayout(context).apply {
            orientation = VERTICAL
            setPadding(20, 10, 20, 30)
        }
        body = outer
        header()

        val row = LinearLayout(context).apply { orientation = HORIZONTAL }
        val cols = (0..1).map {
            LinearLayout(context).apply {
                orientation = VERTICAL
                layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
                setPadding(8, 0, 8, 0)
            }
        }
        cols.forEach { row.addView(it) }
        outer.addView(row)

        /* Categories are kept whole. A heading in one column with half
         * its rows in the next is worse than an uneven column. */
        body = cols[0]; connection(); stream()
        body = cols[1]; sound(); controls(); diagnostics()

        addView(outer)
    }

    private fun buildAccordion() {
        val single = LinearLayout(context).apply {
            orientation = VERTICAL
            setPadding(24, 12, 24, 40)
        }
        body = single
        header()
        category("connection") { connection() }
        category("stream") { stream() }
        category("sound") { sound() }
        category("controls") { controls() }
        category("diagnostics") { diagnostics() }
        addView(single)
    }

    private fun header() {
        val top = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        /* Pinned, because a menu with no way back to the picture is a
         * trap on a device whose back button leaves the app. */
        top.addView(Button(context).apply {
            text = "back to the screen"
            textSize = 13f
            layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
            setOnClickListener { actions.onClose() }
        })
        /*
         * Fullscreen sits here rather than under "controls", where it
         * was the last row of the longer column and fell off the bottom
         * of the screen -- present, and unreachable without scrolling a
         * menu you opened to press one thing. It is an action on the
         * screen, like going back to it, so it belongs beside it.
         */
        lateinit var fs: Button
        fs = Button(context).apply {
            text = fullscreenLabel()
            textSize = 12f
            setOnClickListener {
                settings.fullscreen = !settings.fullscreen
                fs.text = fullscreenLabel()
                actions.onApply()
            }
        }
        top.addView(fs)
        top.addView(Button(context).apply {
            text = if (settings.menuColumns) "columns" else "one at a time"
            textSize = 12f
            setOnClickListener { settings.menuColumns = !settings.menuColumns; rebuild() }
        })
        body.addView(top)

        body.addView(TextView(context).apply {
            text = settings.streamLine
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 11f
            setTextColor(0xFF39FF14.toInt())
            setPadding(0, 8, 0, 4)
        })
    }

    /* ------------------------------------------------------ categories */

    private fun connection() {
        val hostField = field(settings.host, android.text.InputType.TYPE_CLASS_TEXT,
                              "server address")
        val portField = field(settings.port.toString(),
                              android.text.InputType.TYPE_CLASS_NUMBER, "port")
        row("address", hostField)
        row("port", portField)
        button("connect") {
            val p = portField.text.toString().trim().toIntOrNull()
                ?: BsProtocol.DEFAULT_PORT
            actions.onReconnect(hostField.text.toString().trim(), p)
        }
        button("save this server") { actions.onSaveServer(); rebuild() }

        if (settings.savedServers.isNotEmpty()) {
            group("saved")
            for (profile in settings.savedServers) {
                val line = LinearLayout(context).apply {
                    orientation = HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                }
                line.addView(Button(context).apply {
                    text = "${profile.name}\n${profile.where}"
                    textSize = 12f
                    layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
                    setOnClickListener { actions.onReconnect(profile.host, profile.port) }
                })
                line.addView(Button(context).apply {
                    text = "×"
                    textSize = 12f
                    setOnClickListener { actions.onForgetServer(profile); rebuild() }
                })
                body.addView(line)
            }
        }
    }

    private fun stream() {
        /*
         * One section per screen, because the server encodes them
         * separately and they are worth different things: a television
         * picture at 8 Mbit/s beside a GamePad screen at 2 is a normal
         * pair of answers. Only the screen being watched is applied
         * straight away; the other is remembered and sent on arrival.
         *
         * With no second screen on offer there is one section and no
         * heading telling somebody which screen it is, because there is
         * only one.
         */
        if (settings.hasTopScreen) {
            group("screen")
            choice(
                listOf("bottom" to BsProtocol.SCREEN_BOTTOM,
                       "top" to BsProtocol.SCREEN_TOP),
                settings.screenShown
            ) { settings.screenShown = it; rebuild() }
            hint("The top screen has no touch panel, so taps do nothing " +
                 "there. The buttons still work.")

            screenSection("bottom screen", BsProtocol.SCREEN_BOTTOM)
            screenSection("top screen", BsProtocol.SCREEN_TOP)
        } else {
            screenSection(null, BsProtocol.SCREEN_BOTTOM)
        }
    }

    /* Quality and size for one screen. `title` is null when there is
     * only one and naming it would be noise. */
    private fun screenSection(title: String?, screen: Int) {
        group(if (title == null) "quality" else "$title \u2014 quality")
        for (q in Quality.entries) {
            check(q.label, settings.qualityOf(screen) == q) {
                settings.setQuality(screen, q)
                actions.onApply()
                rebuild()
            }
        }

        group(if (title == null) "size received" else "$title \u2014 size received")
        /*
         * Multiples of that screen's own size, never below it.
         *
         * An emulator at a raised internal resolution puts far more on
         * the wire than a phone can show, so asking for less is worth
         * having -- but an arbitrary fraction is not: a quarter of a DS
         * screen is 64x48, which a hardware decoder configures happily
         * and then produces nothing from at all. Native is the floor and
         * every step above it is a whole multiple, so what arrives is
         * always a size the console itself could have produced.
         *
         * The Wii U is the exception, because 854x480 has room to give:
         * half of it is still larger than a DS screen.
         */
        val nw = settings.nativeWidthOf(screen)
        val nh = settings.nativeHeightOf(screen)
        val current = settings.scaleOf(screen)
        val srcW = if (screen == settings.screenShown) settings.sourceWidth else 0
        if (nw >= 640) sizeOption("half of native  ${nw / 2}x${nh / 2}", -2, screen, current)
        var f = 1
        while (nw * f <= maxOf(srcW, nw) && nh * f <= BsProtocol.MAX_STREAM_HEIGHT) {
            sizeOption(if (f == 1) "native  ${nw}x${nh}"
                       else "${f}x native  ${nw * f}x${nh * f}", f, screen, current)
            f++
        }
        sizeOption("whatever is rendered", 0, screen, current)
    }

    private fun sizeOption(label: String, value: Int, screen: Int, current: Int) {
        check(label, current == value) {
            settings.setScale(screen, value)
            actions.onApply()
            rebuild()
        }
    }

    private fun sound() {
        if (!settings.hasAudio) {
            body.addView(TextView(context).apply {
                text = "this stream carries no sound"
                textSize = 11f
                setTextColor(DIM)
            })
            return
        }
        slider("volume", 0, 100, (settings.volume * 100).toInt(), "%") {
            settings.volume = it / 100f
            actions.onApply()
        }
        check("mute", settings.muted) { settings.muted = it; actions.onApply() }

        /* Only a Wii U has two outputs, so only there is there anything
         * to choose. Offering it elsewhere would be a question with one
         * answer. */
        if (settings.isWiiU) {
            group("which output")
            choice(
                listOf("both, summed" to BsProtocol.AUDIO_BOTH,
                       "television" to BsProtocol.AUDIO_TV,
                       "GamePad" to BsProtocol.AUDIO_PAD),
                settings.audioSource
            ) { settings.audioSource = it; actions.onApply() }
        }
    }

    /* A row of tick boxes behaving as one choice, which is how quality
     * and the button modes are already offered here. */
    private fun choice(options: List<Pair<String, Int>>, current: Int,
                       onPick: (Int) -> Unit) {
        val boxes = mutableListOf<CheckBox>()
        options.forEachIndexed { i, (label, value) ->
            val cb = CheckBox(context).apply {
                text = label
                textSize = 12f
                setTextColor(Color.WHITE)
                isChecked = value == current
            }
            cb.setOnClickListener {
                boxes.forEachIndexed { j, b -> b.isChecked = j == i }
                onPick(value)
            }
            boxes.add(cb)
            body.addView(cb)
        }
    }

    private fun controls() {
        group("on-screen buttons")
        for (v in PadVisibility.entries) {
            check(v.label, settings.padVisibility == v) {
                settings.padVisibility = v
                actions.onApply()
                rebuild()
            }
        }
        slider("size", 50, 150, (settings.buttonScale * 100).toInt(), "%") {
            settings.buttonScale = it / 100f
            actions.onApply()
        }
        check("move them  (drag where your thumbs are)", settings.editMode) {
            settings.editMode = it
            actions.onApply()
        }
        button("reset their positions") { actions.onResetLayout() }
        /* Drawn over whatever the emulator is showing, so no single
         * colour works against all of it. */
        choice(
            PadOverlay.PALETTE_NAMES.mapIndexed { i, n -> n to i },
            settings.padColour
        ) { settings.padColour = it; actions.onApply() }

        /* Only a console with sticks has anywhere to put them. */
        if (settings.hasSticks) {
            group("left stick")
            choice(listOf("above the d-pad" to 0, "below the d-pad" to 1),
                   if (settings.stickBelow[0]) 1 else 0) {
                settings.stickBelow = booleanArrayOf(it == 1, settings.stickBelow[1])
                actions.onApply()
            }
            group("right stick")
            choice(listOf("above the buttons" to 0, "below the buttons" to 1),
                   if (settings.stickBelow[1]) 1 else 0) {
                settings.stickBelow = booleanArrayOf(settings.stickBelow[0], it == 1)
                actions.onApply()
            }
        }

    }

    /*
     * A button, not a tick box: a box makes you read its state before
     * you know what a tap will do. The label says what the tap does, and
     * changes as soon as it has done it.
     */
    private fun fullscreenLabel() =
        if (settings.fullscreen) "leave fullscreen" else "fullscreen"


    private fun diagnostics() {
        body.addView(TextView(context).apply {
            text = settings.streamLine
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 11f
            setTextColor(0xFF39FF14.toInt())
        })
        body.addView(TextView(context).apply {
            text = "A pad plugged into this phone works alongside these " +
                   "buttons: the emulator merges the two rather than " +
                   "swapping one for the other."
            textSize = 11f
            setTextColor(0xFF8AA0B4.toInt())
            setPadding(0, 8, 0, 0)
        })
    }

    /* --------------------------------------------------------- pieces */

    private fun category(title: String, contents: () -> Unit) {
        val isOpen = open == title
        body.addView(TextView(context).apply {
            text = (if (isOpen) "▾  " else "▸  ") + title
            setTextColor(if (isOpen) ACCENT else Color.WHITE)
            textSize = 14f
            setPadding(0, 14, 0, 8)
            setOnClickListener { open = if (isOpen) null else title; rebuild() }
        })
        if (isOpen) contents()
    }

    /* A line of explanation under a control, in the same dim grey the
     * headings use. Several places built one of these inline; the top
     * screen needed a third, which is where a helper earns itself. */
    private fun hint(text: String) {
        body.addView(TextView(context).apply {
            this.text = text
            textSize = 11f
            setTextColor(DIM)
            setPadding(0, 0, 0, 6)
        })
    }

    private fun group(title: String) {
        body.addView(TextView(context).apply {
            text = title
            textSize = 12f
            setTextColor(DIM)
            setPadding(0, 10, 0, 4)
        })
    }

    private fun field(value: String, type: Int, hintText: String? = null) =
        EditText(context).apply {
            setText(value)
            inputType = type
            textSize = 13f
            hint = hintText
            setTextColor(Color.WHITE)
            setHintTextColor(DIM)
        }

    private fun row(label: String, control: View) {
        val line = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        line.addView(TextView(context).apply {
            text = label
            textSize = 12f
            setTextColor(DIM)
            minWidth = 140
        })
        control.layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
        line.addView(control)
        body.addView(line)
    }

    private fun button(label: String, onClick: () -> Unit): Button {
        val b = Button(context).apply {
            text = label
            textSize = 13f
            setOnClickListener { onClick() }
        }
        body.addView(b)
        return b
    }

    private fun check(label: String, value: Boolean, onChange: (Boolean) -> Unit) {
        body.addView(CheckBox(context).apply {
            text = label
            textSize = 12f
            isChecked = value
            setTextColor(Color.WHITE)
            setOnCheckedChangeListener { _, v -> onChange(v) }
        })
    }

    private fun slider(label: String, min: Int, max: Int, value: Int, unit: String,
                       onChange: (Int) -> Unit) {
        val readout = TextView(context).apply {
            text = "$value$unit"
            textSize = 11f
            setTextColor(DIM)
            minWidth = 90
            gravity = Gravity.END
        }
        val bar = SeekBar(context).apply {
            this.max = max - min
            progress = value - min
            layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar?, p: Int, fromUser: Boolean) {
                    val v = p + min
                    readout.text = "$v$unit"
                    if (fromUser) onChange(v)
                }
                override fun onStartTrackingTouch(s: SeekBar?) {}
                override fun onStopTrackingTouch(s: SeekBar?) {}
            })
        }
        val line = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        line.addView(TextView(context).apply {
            text = label
            textSize = 12f
            setTextColor(DIM)
            minWidth = 120
        })
        line.addView(bar)
        line.addView(readout)
        body.addView(line)
    }

    companion object {
        const val ACCENT = 0xFF5CE8FF.toInt()
        const val DIM = 0xFF8FA8B8.toInt()
    }
}
