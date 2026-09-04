package fr.wozt.bottomscreen

import android.app.AlertDialog
import android.content.res.Configuration
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
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
    private var decoder: BsVideoDecoder? = null
    private var surfaceReady = false
    private var profile = ConsoleProfile.DS
    private var ack: BsProtocol.HelloAck? = null
    private var quality = Quality.AUTO

    private var frames = 0
    private var lastReport = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
        }
        quality = Quality.byName(
            getSharedPreferences(PREFS, MODE_PRIVATE).getString("quality", null)
        )
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
        intent?.getStringExtra("host")?.let { h ->
            hostField.setText(h)
            intent.getIntExtra("port", BsProtocol.DEFAULT_PORT).let {
                portField.setText(it.toString())
            }
            root.post { connect() }
        }
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
        form.addView(hostField)

        portField = EditText(this).apply {
            hint = "port"
            setText(prefs.getInt("port", BsProtocol.DEFAULT_PORT).toString())
            setTextColor(Color.WHITE)
            setHintTextColor(Color.GRAY)
        }
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

    override fun onConnected(ack: BsProtocol.HelloAck) {
        this.ack = ack
        /* The server starts on its own default, so a saved preference
         * has to be re-sent on every connection or it silently does
         * nothing after the first one. */
        if (quality != Quality.AUTO) client?.setQuality(quality.bitrate)
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
        if (!d.decode(data, offset, length, keyframe)) return

        frames++
        val now = System.currentTimeMillis()
        if (now - lastReport >= 1000) {
            val fps = frames
            frames = 0
            lastReport = now
            runOnUiThread {
                title = "${profile.label}  $fps fps"
            }
        }
    }

    override fun onDisconnected(reason: String) {
        runOnUiThread {
            decoder?.release()
            decoder = null
            surfaceReady = false
            play?.let { root.removeView(it) }
            play = null
            screen = null
            pad = null
            if (form.parent == null) root.addView(form)
            form.visibility = View.VISIBLE
            status.text = "Disconnected: $reason"
        }
    }

    // --- the playing layout ------------------------------------------

    private fun buildPlayUi(ack: BsProtocol.HelloAck) {
        profile = ConsoleProfile.forConsole(ack.console)
        form.visibility = View.GONE

        val landscape =
            resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
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
        }
        screen = view

        val overlay = PadOverlay(this).apply {
            profile = this@MainActivity.profile
            onButton = { code, pressed ->
                client?.sendInput(
                    if (pressed) BsProtocol.INPUT_BUTTON_DOWN else BsProtocol.INPUT_BUTTON_UP,
                    code, 0, 0
                )
            }
        }
        pad = overlay

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
             * buttons cannot be squeezed into nothing by a wide screen. */
            val minBand = (availW * 0.17f).toInt()
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
            /* Picture on top, controls below. The ceiling on height
             * leaves the buttons somewhere to live. */
            val maxVideoH = (availH * 0.55f).toInt()
            var videoW = availW
            var videoH = availW * ack.height / ack.width
            if (videoH > maxVideoH) {
                videoH = maxVideoH
                videoW = maxVideoH * ack.width / ack.height
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
        val gear = TextView(this).apply {
            text = "\u2699"
            textSize = 22f
            setTextColor(0x80FFFFFF.toInt())
            setPadding(24, 12, 24, 24)
            setOnClickListener { showSettings() }
        }
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
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.END
            ))
        }
        root.addView(play)
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
    private fun showSettings() {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val pad = (resources.displayMetrics.density * 16).toInt()

        val hostEdit = EditText(this).apply {
            hint = "server address"
            setText(prefs.getString("host", ""))
        }
        val portEdit = EditText(this).apply {
            hint = "port"
            setText(prefs.getInt("port", BsProtocol.DEFAULT_PORT).toString())
        }
        val qualitySpinner = Spinner(this).apply {
            adapter = ArrayAdapter(
                this@MainActivity,
                android.R.layout.simple_spinner_dropdown_item,
                Quality.entries.map { it.label }
            )
            setSelection(Quality.entries.indexOf(quality))
        }

        val body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, 0)
            addView(TextView(this@MainActivity).apply { text = "Server" })
            addView(hostEdit)
            addView(portEdit)
            addView(TextView(this@MainActivity).apply {
                text = "Stream quality"
                setPadding(0, pad, 0, 0)
            })
            addView(qualitySpinner)
        }

        AlertDialog.Builder(this)
            .setTitle("Settings")
            .setView(body)
            .setPositiveButton("Apply") { _, _ ->
                val newQuality = Quality.entries[qualitySpinner.selectedItemPosition]
                val newHost = hostEdit.text.toString().trim()
                val newPort = portEdit.text.toString().trim().toIntOrNull()
                    ?: BsProtocol.DEFAULT_PORT

                if (newQuality != quality) {
                    quality = newQuality
                    prefs.edit().putString("quality", newQuality.name).apply()
                    client?.setQuality(newQuality.bitrate)
                }

                val hostChanged = newHost != prefs.getString("host", "")
                val portChanged = newPort != prefs.getInt("port", BsProtocol.DEFAULT_PORT)
                if (newHost.isNotEmpty() && (hostChanged || portChanged)) {
                    prefs.edit().putString("host", newHost).putInt("port", newPort).apply()
                    hostField.setText(newHost)
                    portField.setText(newPort.toString())
                    /* A new address is a new connection: drop this one
                     * and let the usual disconnect path rebuild. */
                    client?.stop()
                }
            }
            .setNegativeButton("Cancel", null)
            .setNeutralButton("Disconnect") { _, _ -> client?.stop() }
            .show()
    }

    // --- surface lifecycle -------------------------------------------

    override fun surfaceCreated(h: SurfaceHolder) {
        val s = screen ?: return
        val d = BsVideoDecoder(h.surface, s.nativeWidth, s.nativeHeight)
        if (d.start()) {
            decoder = d
            surfaceReady = true
            /* A new decoder has no reference picture, so it draws
             * nothing until a keyframe arrives -- a second of black
             * after every rotation if we just wait for the next one. */
            client?.requestKeyframe()
        } else {
            runOnUiThread { status.text = "The decoder would not start" }
        }
    }

    override fun surfaceChanged(h: SurfaceHolder, format: Int, w: Int, height: Int) {}

    override fun surfaceDestroyed(h: SurfaceHolder) {
        surfaceReady = false
        decoder?.release()
        decoder = null
    }

    override fun onDestroy() {
        super.onDestroy()
        client?.stop()
        decoder?.release()
    }

    companion object {
        private const val PREFS = "bottom_screen"
    }
}
