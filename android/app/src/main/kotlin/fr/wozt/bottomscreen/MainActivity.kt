package fr.wozt.bottomscreen

import android.app.AlertDialog
import android.content.Intent
import android.content.res.Configuration
import android.hardware.input.InputManager
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.CheckBox
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.ArrayAdapter
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * Connect, then play.
 *
 * The interface follows the stream rather than a setting: the server
 * says which console it is serving and how big its screen is, and the
 * layout and the button set come from that. Nothing here has to be kept
 * in step with the emulator by hand.
 */
class MainActivity : AppCompatActivity(), BsClient.Listener, SurfaceHolder.Callback {

    private lateinit var root: LinearLayout
    private lateinit var form: LinearLayout
    private lateinit var status: TextView
    private lateinit var hostField: EditText
    private lateinit var portField: EditText

    /* Vertical in portrait, a frame with the picture on top of the
     * pad in landscape, so the type is the common one. */
    private var play: ViewGroup? = null
    private var screen: ScreenView? = null
    private var pad: PadOverlay? = null

    private var client: BsClient? = null
    /*
     * Written on the UI thread in surfaceCreated, read on the network
     * thread in onFrame. Without @Volatile the reader is entitled never
     * to see the write: it keeps its cached null, drops every frame at
     * "val d = decoder ?: return", and the picture stays black for good
     * -- no decode is even attempted, so nothing is logged and forcing a
     * keyframe from the server changes nothing. A rotation repaired it
     * because the rebuild wrote the field again. Intermittent, which is
     * what a memory-visibility bug looks like from the outside.
     */
    @Volatile private var decoder: BsVideoDecoder? = null
    /* Which surface the decoder was built for, so a late callback about
     * an older one cannot tear down the current picture. */
    @Volatile private var decoderHolder: SurfaceHolder? = null
    private var audio: BsAudioPlayer? = null
    private var volume = 1f
    private var muted = false
    @Volatile private var surfaceReady = false
    private var profile = ConsoleProfile.DS
    private var panel: View? = null
    private var settingsGear: TextView? = null

    /* The size the emulator actually renders, remembered before anybody
     * asks for less: the fractions below are fractions of that, not of
     * each other. */
    private var fullWidth = 0
    private var fullHeight = 0
    /* 0 = whatever the emulator renders, N = N times the console's own
     * screen, -2 = half of it (the Wii U only). */
    private var receiveScale = 0
    private var fps = 0

    /* Set while leaving a server on purpose, so the closing socket does
     * not report itself as a failure on the way out. */
    private var leavingOnPurpose = false

    /*
     * When to draw the on-screen buttons. Automatic means "unless a pad
     * is connected", which is what somebody who has just plugged one in
     * wants without being asked.
     */
    private var padVisibility = PadVisibility.AUTOMATIC
    private var gamepadPresent = false

    /* What the pad is holding, so a disconnection mid-press does not
     * leave a button down for good -- the same reason the server tracks
     * it per client. */
    private val padHeld = HashSet<Int>()
    private var padHat = emptySet<Int>()
    private var ack: BsProtocol.HelloAck? = null
    private var quality = Quality.AUTO
    private var buttonScale = 1f
    private var fullscreen = false
    /* Which of a Wii U's two outputs to hear; ignored elsewhere. */
    private var audioSource = BsProtocol.AUDIO_BOTH

    /*
     * Which screen is being watched, and which ones this server has.
     *
     * Deliberately not remembered between sessions. The bottom screen is
     * what this is for, and coming back to find the television picture
     * because of something chosen days ago is a worse surprise than
     * having to pick it again. The mask starts empty so the choice stays
     * hidden until a server says it has one -- an older server never
     * says anything, and then there is nothing to offer.
     */
    @Volatile private var shownScreen = BsProtocol.SCREEN_BOTTOM
    @Volatile private var screensMask = 1 shl BsProtocol.SCREEN_BOTTOM
    private var padColour = 0
    private var stickBelow = booleanArrayOf(false, true)
    private var startInEdit = false

    private var frames = 0
    private var lastReport = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
        }
        val prefs0 = getSharedPreferences(PREFS, MODE_PRIVATE)
        quality = Quality.byName(prefs0.getString("quality", null))
        volume = prefs0.getFloat("volume", 1f)
        muted = prefs0.getBoolean("muted", false)
        buttonScale = prefs0.getFloat("pad_scale", 1f)
        fullscreen = prefs0.getBoolean("fullscreen", false)
        audioSource = prefs0.getInt("audio_source", BsProtocol.AUDIO_BOTH)
        padColour = prefs0.getInt("pad_colour", 0)
        stickBelow = booleanArrayOf(prefs0.getBoolean("stick_l_below", false),
                                    prefs0.getBoolean("stick_r_below", true))
        padVisibility = PadVisibility.byName(prefs0.getString("pad_visibility", null))
        receiveScale = prefs0.getInt("receive_scale", 0)
        gamepadPresent = Gamepad.anyConnected()
        applyFullscreen()
        buildForm()
        setContentView(root)

        /*
         * Let a launch carry the address, so a development cycle is one
         * adb command rather than typing an IP on a phone keyboard every
         * time. Over USB this pairs with "adb reverse tcp:5090 tcp:5090",
         * which makes the phone's own localhost reach the PC.
         *
         *   am start -n fr.wozt.bottomscreen/.MainActivity \
         *     --es host 127.0.0.1 --ei port 5090
         */
        intent?.let { applyLaunchIntent(it, running = false) }
    }

    /*
     * The same address may arrive at an app that is already open.
     *
     * A launcher aimed at another emulator sends a second intent, and
     * without this it does nothing at all: the extras land on an intent
     * onCreate has already read and will not read again, so the app just
     * comes to the front still showing the console you were leaving.
     */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        applyLaunchIntent(intent, running = true)
    }

    private fun applyLaunchIntent(i: Intent, running: Boolean) {
        if (i.getBooleanExtra("fullscreen", false)) {
            fullscreen = true
            applyFullscreen()
        }
        /* The pad does not exist until a connection brings one, so this
         * is remembered and applied when it is built. */
        startInEdit = i.getBooleanExtra("edit", false)

        val host = i.getStringExtra("host") ?: return
        val port = i.getIntExtra("port", BsProtocol.DEFAULT_PORT)

        /* Already playing somewhere else: put that down first, or the
         * old stream keeps painting over the new one. */
        if (running) disconnectToForm()

        hostField.setText(host)
        portField.setText(port.toString())
        root.post { connect() }
    }

    /*
     * A physical controller, mapped onto the same button codes the
     * on-screen buttons send. The protocol is already abstract, so the
     * server cannot tell which of the two a press came from -- and the
     * host's own pad keeps working either way, because the emulator
     * merges rather than replaces.
     */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (client == null || !Gamepad.isFromGamepad(event) || event.repeatCount > 0)
            return super.dispatchKeyEvent(event)

        val code = Gamepad.buttonFor(event.keyCode)
        if (code == 0) return super.dispatchKeyEvent(event)

        when (event.action) {
            KeyEvent.ACTION_DOWN -> sendPadButton(code, true)
            KeyEvent.ACTION_UP -> sendPadButton(code, false)
            else -> return super.dispatchKeyEvent(event)
        }
        return true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (client == null || !Gamepad.isFromJoystick(event) ||
            event.action != MotionEvent.ACTION_MOVE) {
            return super.onGenericMotionEvent(event)
        }

        client?.sendInput(BsProtocol.INPUT_AXIS, BsProtocol.AXIS_LEFT_X,
            Gamepad.axis(event, MotionEvent.AXIS_X), 0)
        /* Screen coordinates run down, sticks run up: the sign is
         * flipped here so a push forward is a push forward. */
        client?.sendInput(BsProtocol.INPUT_AXIS, BsProtocol.AXIS_LEFT_Y,
            -Gamepad.axis(event, MotionEvent.AXIS_Y), 0)
        client?.sendInput(BsProtocol.INPUT_AXIS, BsProtocol.AXIS_RIGHT_X,
            Gamepad.axis(event, MotionEvent.AXIS_Z), 0)
        client?.sendInput(BsProtocol.INPUT_AXIS, BsProtocol.AXIS_RIGHT_Y,
            -Gamepad.axis(event, MotionEvent.AXIS_RZ), 0)

        /* And shown on the drawn sticks, the way a press already lights
         * the drawn button. */
        pad?.showPadAxes(
            Gamepad.axis(event, MotionEvent.AXIS_X),
            -Gamepad.axis(event, MotionEvent.AXIS_Y),
            Gamepad.axis(event, MotionEvent.AXIS_Z),
            -Gamepad.axis(event, MotionEvent.AXIS_RZ))

        /* Most pads report their d-pad as a hat rather than as keys. */
        val hat = Gamepad.hatButtons(event)
        for (code in padHat - hat) sendPadButton(code, false)
        for (code in hat - padHat) sendPadButton(code, true)
        padHat = hat
        return true
    }

    private fun sendPadButton(code: Int, pressed: Boolean) {
        if (pressed) padHeld.add(code) else padHeld.remove(code)
        client?.sendInput(
            if (pressed) BsProtocol.INPUT_BUTTON_DOWN else BsProtocol.INPUT_BUTTON_UP,
            code, 0, 0)
    }

    /* Somebody unplugged a pad. Whatever it was holding has to come up,
     * or the console keeps seeing a button that nothing is pressing. */
    private fun releasePadButtons() {
        for (code in padHeld.toList())
            client?.sendInput(BsProtocol.INPUT_BUTTON_UP, code, 0, 0)
        padHeld.clear()
        padHat = emptySet()
    }

    private fun showPadOverlay(): Boolean = when (padVisibility) {
        PadVisibility.ALWAYS -> true
        PadVisibility.NEVER -> false
        PadVisibility.AUTOMATIC -> !gamepadPresent
    }

    private fun applyPadVisibility() {
        val overlay = pad ?: return
        overlay.visibility = if (showPadOverlay()) View.VISIBLE else View.GONE
        /* In landscape the buttons hold a band down each side. Giving it
         * back to the picture is the point of hiding them, so the
         * geometry is recomputed rather than left with an empty margin. */
        val view = screen
        val a = ack
        val container = play
        if (view != null && a != null && container != null) {
            val landscape =
                resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
            container.post { sizeVideo(container, view, overlay, a, landscape) }
        }
    }

    private val deviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = padsChanged()
        override fun onInputDeviceRemoved(deviceId: Int) {
            releasePadButtons()
            padsChanged()
        }
        override fun onInputDeviceChanged(deviceId: Int) = padsChanged()
    }

    private fun padsChanged() {
        val present = Gamepad.anyConnected()
        if (present == gamepadPresent) return
        gamepadPresent = present
        applyPadVisibility()
        if (padVisibility == PadVisibility.AUTOMATIC) {
            val name = if (present) Gamepad.nameOfFirst() else null
            status.text = if (name != null) "$name connected — on-screen buttons hidden"
                          else "No pad — on-screen buttons back"
        }
    }

    override fun onResume() {
        super.onResume()
        audio?.volume = if (muted) 0f else volume
        (getSystemService(INPUT_SERVICE) as InputManager)
            .registerInputDeviceListener(deviceListener, null)
        padsChanged()
    }

    override fun onPause() {
        super.onPause()
        /* Silence while it is not on screen. A stream carries on
         * arriving when the app is behind something else, and sound with
         * no picture coming out of a pocket is not a feature. */
        audio?.volume = 0f
        (getSystemService(INPUT_SERVICE) as InputManager)
            .unregisterInputDeviceListener(deviceListener)
    }

    private fun buildForm() {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val pad = (resources.displayMetrics.density * 16).toInt()

        form = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 3, pad, pad)
            gravity = Gravity.CENTER_HORIZONTAL
        }

        form.addView(TextView(this).apply {
            text = "Bottom Screen"
            textSize = 24f
            setTextColor(Color.WHITE)
            setPadding(0, 0, 0, pad)
        })

        hostField = EditText(this).apply {
            hint = "server address"
            setText(prefs.getString("host", ""))
            setTextColor(Color.WHITE)
            setHintTextColor(Color.GRAY)
        }
        portField = EditText(this).apply {
            hint = "port"
            setText(prefs.getInt("port", BsProtocol.DEFAULT_PORT).toString())
            setTextColor(Color.WHITE)
            setHintTextColor(Color.GRAY)
        }

        /* The known servers come first, because the whole point of
         * keeping them is not to type an address again. */
        addSavedServers(pad)

        form.addView(hostField)
        form.addView(portField)

        form.addView(Button(this).apply {
            text = "Connect"
            setOnClickListener { connect() }
        })

        status = TextView(this).apply {
            setTextColor(Color.GRAY)
            setPadding(0, pad, 0, 0)
        }
        form.addView(status)

        root.addView(form)
    }

    /*
     * One tap per server this phone already knows.
     *
     * Three emulators run at once, each on its own port -- and a server
     * whose port was taken moves to the next one and says so -- which
     * makes the address the thing you would otherwise retype every time
     * you change console.
     */
    private fun addSavedServers(pad: Int) {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val saved = Profiles.load(prefs)
        if (saved.isEmpty()) return

        form.addView(TextView(this).apply {
            text = "Saved servers"
            setTextColor(Color.GRAY)
            setPadding(0, 0, 0, pad / 2)
        })
        for (p in saved) {
            form.addView(Button(this).apply {
                text = "${p.name}\n${p.where}"
                setOnClickListener {
                    hostField.setText(p.host)
                    portField.setText(p.port.toString())
                    connect()
                }
                /* Deleting is a long press with a confirmation: these
                 * buttons are meant to be tapped in a hurry, and a
                 * delete that shares that gesture would be tapped by
                 * accident. */
                setOnLongClickListener {
                    AlertDialog.Builder(this@MainActivity)
                        .setTitle("Forget ${p.name}?")
                        .setMessage(p.where)
                        .setPositiveButton("Forget") { _, _ ->
                            Profiles.remove(prefs, p)
                            rebuildForm()
                        }
                        .setNegativeButton("Keep", null)
                        .show()
                    true
                }
            })
        }
    }

    private fun rebuildForm() {
        root.removeView(form)
        buildForm()
    }

    /*
     * Offered once connected rather than before, because that is when
     * the console is known -- the server announces it -- and a name like
     * "Wii U (5092)" is worth more than the address it replaces.
     */
    private fun promptSaveProfile() {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val host = prefs.getString("host", "") ?: ""
        val port = prefs.getInt("port", BsProtocol.DEFAULT_PORT)
        if (host.isEmpty()) return

        val nameEdit = EditText(this).apply {
            setText(Profiles.suggestName(profile.label, host, port))
            setSelectAllOnFocus(true)
        }
        AlertDialog.Builder(this)
            .setTitle("Save this server")
            .setMessage("$host:$port")
            .setView(nameEdit)
            .setPositiveButton("Save") { _, _ ->
                val name = nameEdit.text.toString().trim()
                Profiles.upsert(prefs, Profile(
                    name = name.ifEmpty { "$host:$port" },
                    host = host,
                    port = port))
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun connect() {
        val host = hostField.text.toString().trim()
        if (host.isEmpty()) {
            status.text = "Enter the address of the machine running the server"
            return
        }
        val port = portField.text.toString().trim().toIntOrNull() ?: BsProtocol.DEFAULT_PORT

        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString("host", host).putInt("port", port).apply()

        status.text = "Connecting to $host:$port…"
        client?.stop()
        client = BsClient(host, port, this).also { it.start() }
    }

    // --- BsClient.Listener, all on the network thread ----------------

    /*
     * The picture changed shape, because someone moved the emulator's
     * internal resolution. Rebuilding the whole play layout is the
     * simplest correct answer: the surface, the decoder and the geometry
     * all depend on the size, and this happens when a slider moves, not
     * every frame.
     */
    override fun onStreamInfo(info: BsProtocol.StreamInfo) {
        val old = ack ?: return
        if (info.width <= 0 || info.height <= 0) return
        if (info.width == old.width && info.height == old.height) return

        val updated = old.copy(width = info.width, height = info.height,
                               fps = if (info.fps > 0) info.fps else old.fps)
        ack = updated
        runOnUiThread {
            /* The panel's own heading is built once, so a size that
             * changes while it is open would keep reading the old one --
             * which is exactly when somebody is looking at it. */
            (panel as? android.widget.ScrollView)
                ?.getChildAt(0)?.let { (it as? SettingsPanel)?.rebuild() }
            decoder?.release()
            decoder = null
            surfaceReady = false
            play?.let { root.removeView(it) }
            play = null
            buildPlayUi(updated)
        }
    }

    override fun onScreens(mask: Int) {
        screensMask = mask
        /* The settings may be open on the very row this adds. */
        runOnUiThread {
            (panel as? android.widget.ScrollView)
                ?.getChildAt(0)?.let { (it as? SettingsPanel)?.rebuild() }
        }
    }

    override fun onAudio(data: ByteArray, offset: Int, length: Int) {
        audio?.decode(data, offset, length)
    }

    override fun onConnected(ack: BsProtocol.HelloAck) {
        this.ack = ack
        /* Started here rather than with the video: a server with no
         * sound reports rate 0, and then there is nothing to start and
         * no volume control to draw. */
        if (ack.hasAudio) {
            val p = BsAudioPlayer(ack.audioRate, ack.audioChannels)
            if (p.start()) {
                p.volume = if (muted) 0f else volume
                audio = p
            }
        }
        /* The server starts on its own default, so a saved preference
         * has to be re-sent on every connection or it silently does
         * nothing after the first one. */
        if (quality != Quality.AUTO) client?.sendQuality(quality.bitrate)
        if (ack.console == BsProtocol.CONSOLE_WIIU &&
            audioSource != BsProtocol.AUDIO_BOTH)
            client?.sendAudioSource(audioSource)
        /* A fresh connection is on the bottom screen whatever this
         * activity last showed, so the two are put back in step rather
         * than left disagreeing. */
        shownScreen = BsProtocol.SCREEN_BOTTOM
        runOnUiThread { buildPlayUi(ack) }
    }

    /*
     * Rotation is handled here rather than by letting Android recreate
     * the activity, which would drop the socket and reconnect on every
     * turn of the phone. Only the views are rebuilt; the connection
     * carries straight on.
     */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val a = ack ?: return
        play?.let { root.removeView(it) }
        play = null
        buildPlayUi(a)
    }

    override fun onFrame(
        data: ByteArray, offset: Int, length: Int, keyframe: Boolean, timestampUs: Int
    ) {
        /* Decoded on the reader thread rather than posted to the UI
         * thread: MediaCodec draws to the surface itself, so there is
         * nothing for the UI thread to do, and a hop through its queue
         * would only add the wait for whatever it is already doing. */
        val d = decoder ?: return
        /*
         * A dropped frame used to ask the server for a new keyframe.
         * It does not any more: there is one encoder for everyone, so
         * the repair was billed to every other client -- and a decoder
         * that is behind drops frames in bursts, which is exactly when
         * the others can least afford it. The next scheduled keyframe
         * is a second away at worst.
         */
        if (!d.decode(data, offset, length, keyframe)) return

        frames++
        val now = System.currentTimeMillis()
        if (now - lastReport >= 1000) {
            val measured = frames
            frames = 0
            lastReport = now
            fps = measured
            runOnUiThread {
                title = "${profile.label}  $measured fps"
            }
        }
    }

    override fun onDisconnected(reason: String) {
        runOnUiThread {
            if (leavingOnPurpose) {
                /* Already torn down by disconnectToForm, which left a
                 * message of its own. */
                leavingOnPurpose = false
                return@runOnUiThread
            }
            decoder?.release()
            decoder = null
            audio?.release()
            audio = null
            surfaceReady = false
            play?.let { root.removeView(it) }
            play = null
            screen = null
            pad = null
            /* Rebuilt rather than merely shown again, so a server saved
             * during this session is in the list when we land back on
             * it. */
            rebuildForm()
            status.text = "Disconnected: $reason"
        }
    }

    /*
     * Back out to the server list without leaving the app.
     *
     * The back button quits altogether, so until this the only way to
     * reach a different emulator was to close the app and start it
     * again -- which is exactly what keeping a list of servers was
     * supposed to spare you.
     */
    private fun disconnectToForm() {
        leavingOnPurpose = true
        client?.stop()
        client = null
        decoder?.release()
        decoder = null
        audio?.release()
        audio = null
        surfaceReady = false
        play?.let { root.removeView(it) }
        play = null
        screen = null
        pad = null
        rebuildForm()
        status.text = "Choose a server"
    }

    // --- the playing layout ------------------------------------------

    private fun buildPlayUi(ack: BsProtocol.HelloAck) {
        profile = ConsoleProfile.forConsole(ack.console)
        if (fullWidth == 0) { fullWidth = ack.width; fullHeight = ack.height }
        form.visibility = View.GONE

        val landscape =
            resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
        /*
         * displayMetrics is the whole panel, including the strips the
         * status and navigation bars take. Sizing the picture from it
         * made the bottom of the screen fall off the edge in landscape.
         * These are a first guess; sizeVideo re-does it against the
         * container's real size once there is one.
         */
        val availW = resources.displayMetrics.widthPixels
        val availH = resources.displayMetrics.heightPixels

        val view = ScreenView(this).apply {
            nativeWidth = ack.width
            nativeHeight = ack.height
            holder.addCallback(this@MainActivity)
            /* Tell the surface the exact buffer size, so the compositor
             * is handed native pixels and does the scaling itself
             * instead of the decoder scaling first. */
            holder.setFixedSize(ack.width, ack.height)
            onTouch = { type, x, y -> client?.sendInput(type, 0, x, y) }
            /* Rebuilt on every change of shape, and a change of screen
             * is one, so this has to be set here rather than once. */
            touchEnabled = shownScreen == BsProtocol.SCREEN_BOTTOM
        }
        screen = view

        val overlay = PadOverlay(this).apply {
            profile = this@MainActivity.profile
            buttonScale = this@MainActivity.buttonScale
            onButton = { code, pressed ->
                client?.sendInput(
                    if (pressed) BsProtocol.INPUT_BUTTON_DOWN else BsProtocol.INPUT_BUTTON_UP,
                    code, 0, 0
                )
            }
            /*
             * In landscape the settings button sits in the top-left
             * corner, which is the top of the left band -- exactly where
             * the shoulders start. The reserve is the gear's own extent
             * (8dp inset + 40dp box) plus a little, so the two cannot
             * meet; a flat 20dp let it sit on top of L. In portrait the
             * gear is below the picture and 20dp is only breathing room.
             */
            topReserve = resources.displayMetrics.density *
                         (if (landscape) 52f else 20f)
            onAxis = { code, value ->
                client?.sendInput(BsProtocol.INPUT_AXIS, code, value, 0)
            }
            colourIndex = padColour
            stickBelow = this@MainActivity.stickBelow
            onMoved = { code, fx, fy -> savePosition(code, fx, fy, landscape) }
        }
        loadPositions(overlay, landscape)
        if (startInEdit) overlay.editMode = true
        pad = overlay
        applyPadVisibility()

        /*
         * The picture keeps its aspect ratio exactly -- a DS screen is
         * 4:3 and stays 4:3 -- but the zoom is whatever fraction fits
         * rather than a whole number. Whole multiples keep every source
         * pixel square, and on a phone that costs a third of the screen
         * to do it. What is never allowed is stretching the two axes
         * independently, which is what makes a picture fat or tall.
         */
        if (landscape) {
            /* Picture in the middle, controls down each side, the way
             * they sit on the console. The bands get a floor so the
             * buttons cannot be squeezed into nothing -- and none at all
             * when there are no buttons to keep room for, which is the
             * whole point of hiding them.
             *
             * The floor is a width, not a fraction of the screen. At 17%
             * a 16:9 GamePad could not reach the full height of a
             * 2400x1080 phone: it needed 12.4% and was given 17, which
             * left a band of black above and below the picture in order
             * to keep buttons bigger than they need to be. */
            val minBand = if (showPadOverlay()) minBandPx() else 0
            var videoH = availH
            var videoW = availH * ack.width / ack.height
            if (availW - videoW < minBand * 2) {
                videoW = availW - minBand * 2
                videoH = videoW * ack.height / ack.width
            }
            overlay.sideBand = ((availW - videoW) / 2f)

            play = FrameLayout(this).apply {
                setBackgroundColor(Color.BLACK)
                layoutParams = FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT
                )
                /* The pad goes in first so the picture sits on top of
                 * it: a touch inside the picture then reaches the screen
                 * rather than the pad underneath. */
                addView(overlay, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT
                ))
                addView(view, FrameLayout.LayoutParams(videoW, videoH, Gravity.CENTER))
            }
            title = "${profile.label}  ${ack.width}x${ack.height} \u2192 ${videoW}x${videoH}"
        } else {
            /* Picture on top, controls below. The band under it is
             * kept for the buttons, and only while there are buttons --
             * the same rule as the side bands above. */
            val minBand = if (showPadOverlay()) (availH * 0.30f).toInt() else 0
            var videoW = availW
            var videoH = availW * ack.height / ack.width
            if (availH - videoH < minBand) {
                videoH = availH - minBand
                videoW = videoH * ack.width / ack.height
            }
            overlay.sideBand = 0f

            val holder = FrameLayout(this).apply {
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, videoH
                )
                setBackgroundColor(Color.BLACK)
                addView(view, FrameLayout.LayoutParams(videoW, videoH, Gravity.CENTER))
            }
            overlay.layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
            )
            play = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setBackgroundColor(Color.BLACK)
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.MATCH_PARENT
                )
                addView(holder)
                addView(overlay)
            }
            title = "${profile.label}  ${ack.width}x${ack.height} \u2192 ${videoW}x${videoH}"
        }

        /* The settings button rides on top of whichever layout was
         * built, so there is one of it rather than one per orientation. */
        val content = play!!
        /*
         * A 48dp target, inset from the corner.
         *
         * The first version was a 22sp glyph flush against the edge,
         * which is both under the smallest comfortable touch target and,
         * in landscape, underneath the navigation bar -- so it could be
         * seen and not pressed.
         */
        /*
         * Small, dim, and never on top of the picture.
         *
         * It used to be a large tile in the top-right corner, which in
         * portrait is squarely over the screen you are playing on --
         * chrome sitting on the one thing it exists to get out of the
         * way of. sizeVideo puts it in the black beside or below the
         * picture instead, wherever that happens to be.
         */
        val touch = (resources.displayMetrics.density * 40).toInt()
        val inset = (resources.displayMetrics.density * 8).toInt()
        val gear = TextView(this).apply {
            text = "\u2699"
            textSize = 15f
            setTextColor(0x77FFFFFF)
            gravity = Gravity.CENTER
            setOnClickListener { showSettings() }
        }
        settingsGear = gear
        play = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
            addView(content, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            ))
            addView(gear, FrameLayout.LayoutParams(
                touch, touch, Gravity.TOP or Gravity.START
            ).apply { setMargins(inset, inset, inset, inset) })
            /* Keep it clear of the status and navigation bars, which in
             * landscape sit exactly where a top-right corner is. */
            setOnApplyWindowInsetsListener { v, insets ->
                val sb = insets.systemWindowInsetTop
                val se = insets.systemWindowInsetRight
                (gear.layoutParams as FrameLayout.LayoutParams)
                    .setMargins(0, sb + inset, se + inset, 0)
                gear.requestLayout()
                v.onApplyWindowInsets(insets)
            }
        }
        root.addView(play)

        /* Now that a real container exists, size the picture against
         * what it actually got rather than against the panel. */
        val container = content
        container.post { sizeVideo(container, view, overlay, ack, landscape) }
    }

    /*
     * Fits the picture to the space there actually is, keeping its
     * aspect ratio exactly. Run after layout, because that is the first
     * moment the usable size -- panel minus system bars -- is known.
     */
    /*
     * The narrowest a side band is allowed to get. Below this the
     * buttons stop being pressable; above it the picture is being made
     * smaller to keep them comfortable, which is the wrong trade on a
     * screen whose whole purpose is the picture.
     */
    private fun minBandPx() = (resources.displayMetrics.density * 96).toInt()

    private fun sizeVideo(
        container: View,
        view: ScreenView,
        overlay: PadOverlay,
        ack: BsProtocol.HelloAck,
        landscape: Boolean
    ) {
        val w = container.width
        val h = container.height
        if (w <= 0 || h <= 0) return

        var videoW: Int
        var videoH: Int
        if (landscape) {
            val minBand = if (showPadOverlay()) minBandPx() else 0
            videoH = h
            videoW = h * ack.width / ack.height
            if (w - videoW < minBand * 2) {
                videoW = w - minBand * 2
                videoH = videoW * ack.height / ack.width
            }
            overlay.sideBand = ((w - videoW) / 2f)
        } else {
            /*
             * Across the width is already the largest a picture can be
             * drawn without stretching it, so the only reason to make it
             * smaller is to leave the buttons somewhere to go -- and that
             * is a reason only while the buttons are actually shown.
             *
             * This used to be a flat cap at 55% of the height, applied
             * whether or not there were any buttons: hiding them gave
             * nothing back, and the picture sat in the top half with an
             * empty half underneath it. The band is now conditional, the
             * same rule the side bands already follow in landscape.
             */
            val minBand = if (showPadOverlay()) (h * 0.30f).toInt() else 0
            videoW = w
            videoH = w * ack.height / ack.width
            if (h - videoH < minBand) {
                videoH = h - minBand
                videoW = videoH * ack.width / ack.height
            }
            overlay.sideBand = 0f
            (view.parent as? View)?.let { holder ->
                holder.layoutParams = holder.layoutParams.also { it.height = videoH }
                holder.requestLayout()
            }
        }

        view.layoutParams = (view.layoutParams as FrameLayout.LayoutParams).also {
            it.width = videoW
            it.height = videoH
            it.gravity = Gravity.CENTER
        }
        view.requestLayout()
        placeGear(w, h, videoW, videoH, landscape)
        title = "${profile.label}  ${ack.width}x${ack.height} \u2192 ${videoW}x${videoH}"
    }

    /*
     * Puts the settings button in the black, which is a different corner
     * depending on which way the phone is held: in landscape the picture
     * is centred with a band down each side, and in portrait it sits
     * across the top with everything below it empty until the shoulders.
     */
    private fun placeGear(w: Int, h: Int, videoW: Int, videoH: Int,
                          landscape: Boolean) {
        val gear = settingsGear ?: return
        val inset = (resources.displayMetrics.density * 8).toInt()
        val p = gear.layoutParams as? FrameLayout.LayoutParams ?: return

        if (landscape) {
            /* The left band, at the top: the shoulders sit lower down. */
            p.gravity = Gravity.TOP or Gravity.START
            p.setMargins(inset, inset, 0, 0)
        } else {
            /* Just under the picture, on the right, where nothing else
             * is until the shoulder row. */
            p.gravity = Gravity.TOP or Gravity.END
            p.setMargins(0, videoH + inset, inset, 0)
        }
        gear.layoutParams = p
        gear.requestLayout()
    }

    /* Saved per console and per orientation: a layout that works with
     * the phone on its side is not the one that works upright. */
    private fun posKey(code: Int, landscape: Boolean) =
        "pos_${profile.console}_${if (landscape) "L" else "P"}_$code"

    private fun savePosition(code: Int, fx: Float, fy: Float, landscape: Boolean) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(posKey(code, landscape), "$fx,$fy").apply()
    }

    private fun loadPositions(overlay: PadOverlay, landscape: Boolean) {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val codes = listOf(PadOverlay.DPAD, PadOverlay.FACE) + (-14..-11) + (1..15)
        for (code in codes) {
            val raw = prefs.getString(posKey(code, landscape), null) ?: continue
            val parts = raw.split(",")
            val fx = parts.getOrNull(0)?.toFloatOrNull() ?: continue
            val fy = parts.getOrNull(1)?.toFloatOrNull() ?: continue
            overlay.setOverride(code, fx, fy)
        }
    }

    private fun clearPositions() {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val edit = prefs.edit()
        for (land in listOf(true, false))
            for (code in listOf(PadOverlay.DPAD, PadOverlay.FACE) + (-14..-11) + (1..15))
                edit.remove(posKey(code, land))
        edit.apply()
        pad?.clearOverrides()
    }

    /*
     * Immersive, and sticky: a swipe brings the bars back for a moment
     * and they leave again on their own. Non-sticky immersive would put
     * the navigation bar back permanently the first time a thumb strayed
     * near the edge -- which, with controls along that edge, is
     * constantly.
     */
    private fun applyFullscreen() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = if (fullscreen) {
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        } else {
            View.SYSTEM_UI_FLAG_VISIBLE
        }
    }

    /*
     * Address, port and quality, reachable while playing.
     *
     * Quality is applied live -- the server rebuilds its encoder between
     * two frames and sends a keyframe -- because the person holding the
     * phone is the one who can see whether the picture is good enough
     * and whether the link is keeping up. Address and port cannot be;
     * changing those means a new connection, so the dialog says so
     * rather than appearing to do nothing.
     */
    /*
     * The settings, as a panel over the picture rather than a dialog.
     *
     * The dialog looked different from capture2cloud, but its real fault
     * was worse: in landscape it was shorter than its own contents, so
     * everything from the volume down was unreachable with nothing on
     * screen to suggest it was there.
     */
    private fun showSettings() {
        if (panel != null) return
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)

        val state = object : SettingsPanel.PanelState {
            override var host: String
                get() = prefs.getString("host", "") ?: ""
                set(v) { prefs.edit().putString("host", v).apply() }
            override var port: Int
                get() = prefs.getInt("port", BsProtocol.DEFAULT_PORT)
                set(v) { prefs.edit().putInt("port", v).apply() }
            override var quality: Quality
                get() = this@MainActivity.quality
                set(v) { this@MainActivity.quality = v }
            override var volume: Float
                get() = this@MainActivity.volume
                set(v) { this@MainActivity.volume = v }
            override var muted: Boolean
                get() = this@MainActivity.muted
                set(v) { this@MainActivity.muted = v }
            override var buttonScale: Float
                get() = this@MainActivity.buttonScale
                set(v) { this@MainActivity.buttonScale = v }
            override var padVisibility: PadVisibility
                get() = this@MainActivity.padVisibility
                set(v) { this@MainActivity.padVisibility = v }
            override var fullscreen: Boolean
                get() = this@MainActivity.fullscreen
                set(v) { this@MainActivity.fullscreen = v }
            override var editMode: Boolean
                get() = pad?.editMode == true
                set(v) { pad?.editMode = v }
            override var receiveScale: Int
                get() = this@MainActivity.receiveScale
                set(v) { this@MainActivity.receiveScale = v }
            override var menuColumns: Boolean
                get() = prefs.getBoolean("menu_columns", false)
                set(v) { prefs.edit().putBoolean("menu_columns", v).apply() }
            override val hasAudio: Boolean get() = ack?.hasAudio == true
            override val isWiiU: Boolean
                get() = ack?.console == BsProtocol.CONSOLE_WIIU
            override var stickBelow: BooleanArray
                get() = this@MainActivity.stickBelow
                set(v) { this@MainActivity.stickBelow = v }
            override val hasSticks: Boolean get() = profile.sticks.isNotEmpty()
            override var padColour: Int
                get() = this@MainActivity.padColour
                set(v) { this@MainActivity.padColour = v }
            override var audioSource: Int
                get() = this@MainActivity.audioSource
                set(v) { this@MainActivity.audioSource = v }
            override var screenShown: Int
                get() = this@MainActivity.shownScreen
                set(v) {
                    this@MainActivity.shownScreen = v
                    screen?.touchEnabled = v == BsProtocol.SCREEN_BOTTOM
                    client?.sendScreen(v)
                }
            override val hasTopScreen: Boolean
                get() = (screensMask and (1 shl BsProtocol.SCREEN_TOP)) != 0
            override val streamLine: String get() = statusLine()
            override val savedServers: List<Profile> get() = Profiles.load(prefs)
            override val nativeWidth: Int get() = profile.width
            override val nativeHeight: Int get() = profile.height
            override val sourceWidth: Int get() = fullWidth
            override val sourceHeight: Int get() = fullHeight
        }

        val actions = object : SettingsPanel.Actions {
            override fun onClose() = hideSettings()
            override fun onApply() = applySettings()
            override fun onReconnect(host: String, port: Int) {
                hideSettings()
                hostField.setText(host)
                portField.setText(port.toString())
                connect()
            }
            override fun onSaveServer() = promptSaveProfile()
            override fun onForgetServer(profile: Profile) {
                Profiles.remove(prefs, profile)
            }
            override fun onResetLayout() = clearPositions()
        }

        val view = SettingsPanel(this, state, actions)
        val scroller = ScrollView(this).apply {
            addView(view)
            setBackgroundColor(0xF00B1A26.toInt())
        }
        panel = scroller
        /*
         * addContentView, not root.addView: root is a vertical
         * LinearLayout, so a child added there lands underneath the
         * picture rather than over it -- which is to say off the bottom
         * of the screen, where the first attempt put the whole panel.
         */
        addContentView(scroller, ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT))
    }

    private fun hideSettings() {
        panel?.let { (it.parent as? ViewGroup)?.removeView(it) }
        panel = null
    }

    /** One line, the way capture2cloud reports a stream. */
    private fun statusLine(): String {
        val a = ack ?: return "not connected"
        return "${profile.label}  ${a.width}x${a.height}  ${fps} fps" +
               (if (a.hasAudio) "  sound" else "  no sound")
    }

    /*
     * Everything the panel can change, applied at once. It is a handful
     * of assignments, and doing them together means no path through the
     * panel can leave one of them behind.
     */
    private fun applySettings() {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        prefs.edit()
            .putFloat("volume", volume)
            .putBoolean("muted", muted)
            .putFloat("pad_scale", buttonScale)
            .putBoolean("fullscreen", fullscreen)
            .putInt("audio_source", audioSource)
            .putInt("pad_colour", padColour)
            .putBoolean("stick_l_below", stickBelow[0])
            .putBoolean("stick_r_below", stickBelow[1])
            .putString("pad_visibility", padVisibility.name)
            .putString("quality", quality.name)
            .putInt("receive_scale", receiveScale)
            .apply()

        audio?.volume = if (muted) 0f else volume
        pad?.buttonScale = buttonScale
        pad?.colourIndex = padColour
        pad?.stickBelow = stickBelow
        applyFullscreen()
        applyPadVisibility()

        client?.sendQuality(quality.bitrate)
        client?.sendAudioSource(audioSource)
        /*
         * Zero means "follow the source", which is how a client stops
         * asking rather than guessing at the original numbers. Every
         * other value is a whole multiple of the console's own screen,
         * or -- for the Wii U alone -- half of it.
         */
        if (ack != null) {
            when {
                receiveScale == 0 -> client?.sendSize(0, 0)
                receiveScale == -2 -> client?.sendSize(profile.width / 2,
                                                       profile.height / 2)
                else -> client?.sendSize(profile.width * receiveScale,
                                         profile.height * receiveScale)
            }
        }
    }


    // --- surface lifecycle -------------------------------------------

    override fun surfaceCreated(h: SurfaceHolder) {
        val s = screen ?: return
        val d = BsVideoDecoder(h.surface, s.nativeWidth, s.nativeHeight)
        if (d.start()) {
            /* Anything still here belongs to a surface being replaced. */
            decoder?.release()
            decoder = d
            decoderHolder = h
            surfaceReady = true
            /* A new decoder has no reference picture and draws nothing
             * until a keyframe arrives. It waits for the stream's own,
             * rather than asking: one client's rotation is not a reason
             * to spend everybody's bandwidth. */
        } else {
            runOnUiThread { status.text = "The decoder would not start" }
        }
    }

    override fun surfaceChanged(h: SurfaceHolder, format: Int, w: Int, height: Int) {}

    override fun surfaceDestroyed(h: SurfaceHolder) {
        /*
         * Rotation adds the new view before dropping the old one, so
         * this can arrive for a surface that has already been replaced.
         * Releasing whatever was current tore down the decoder built for
         * the NEW surface, and the picture stayed black until something
         * else happened to rebuild it -- intermittently, which is why it
         * looked like a capture artefact rather than a bug.
         */
        if (h !== decoderHolder) return
        surfaceReady = false
        decoder?.release()
        decoder = null
        decoderHolder = null
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        /* A dialog or the notification shade puts the bars back; this
         * takes them away again once they are gone. */
        if (hasFocus) applyFullscreen()
    }

    override fun onDestroy() {
        super.onDestroy()
        client?.stop()
        decoder?.release()
        audio?.release()
    }

    companion object {
        private const val PREFS = "bottom_screen"
    }
}
