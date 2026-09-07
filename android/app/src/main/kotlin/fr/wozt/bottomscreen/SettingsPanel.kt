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
        val hasAudio: Boolean
        val streamLine: String
        val savedServers: List<Profile>

        /** The console's own screen, which every offered size is a
         *  multiple of. */
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
        group("quality")
        for (q in Quality.entries) {
            check(q.label, settings.quality == q) {
                settings.quality = q
                actions.onApply()
                rebuild()
            }
        }

        group("size received")
        /*
         * Multiples of the console's own screen, never below it.
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
         * half of it is still 427x240.
         *
         * Shared with anyone else watching, because there is one
         * encoder.
         */
        val nw = settings.nativeWidth
        val nh = settings.nativeHeight

        if (nw >= 640) {
            option("half of native  ${nw / 2}x${nh / 2}", -2)
        }
        var factor = 1
        while (nw * factor <= settings.sourceWidth) {
            option(if (factor == 1) "native  ${nw}x$nh"
                   else "${factor}x native  ${nw * factor}x${nh * factor}", factor)
            factor++
        }
        option("whatever is rendered  " +
               "${settings.sourceWidth}x${settings.sourceHeight}", 0)
    }

    private fun option(label: String, scale: Int) {
        check(label, settings.receiveScale == scale) {
            settings.receiveScale = scale
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

        group("screen")
        check("fullscreen", settings.fullscreen) {
            settings.fullscreen = it
            actions.onApply()
        }
    }

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

    private fun button(label: String, onClick: () -> Unit) {
        body.addView(Button(context).apply {
            text = label
            textSize = 13f
            setOnClickListener { onClick() }
        })
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
