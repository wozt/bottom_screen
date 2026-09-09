/*
 * Bottom Screen -- Opus in, speakers out.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
/* --------------------------------------------------------------- sound */

/*
 * Opus, decoded by the browser and handed straight to WebAudio.
 *
 * Each packet is scheduled after the last rather than mixed into a
 * continuous buffer: the server sends one packet per video frame and
 * they already arrive in order, so keeping a running start time is
 * enough and needs no worklet.
 */
function startAudio() {
  if (audioDecoder || !audioRate) return;

  /*
   * "interactive", not 0.
   *
   * latencyHint 0 asks for a 128-frame buffer -- 2.7ms -- and was tried
   * here on the strength of that number alone. It made things audibly
   * worse: a buffer that small underruns on any pause of the main
   * thread, and the canvas is drawn on that same thread, so the picture
   * suffered with the sound. The 18ms it saves are not worth having if
   * they are spent on glitches.
   */
  audioCtx = new (window.AudioContext || window.webkitAudioContext)(
    { sampleRate: 48000, latencyHint: "interactive" });
  /* Everything goes through one gain, so volume and mute are a single
   * number rather than a decision taken per packet. */
  gain = audioCtx.createGain();
  gain.connect(audioCtx.destination);
  applyVolume();
  nextAudioAt = 0;

  audioDecoder = new AudioDecoder({
    output: (data) => {
      audioFrames++;
      const channels = data.numberOfChannels;
      const buffer = audioCtx.createBuffer(channels, data.numberOfFrames, 48000);
      for (let c = 0; c < channels; c++) {
        const plane = new Float32Array(data.numberOfFrames);
        data.copyTo(plane, { planeIndex: c, format: "f32-planar" });
        buffer.copyToChannel(plane, c);
        /*
         * The loudest sample seen, for the same reason the video has a
         * frame count: a decoder can produce a perfectly well-formed
         * stream of silence, and "832 packets" says nothing about
         * whether any of them carried a sound.
         */
        if (c === 0) {
          for (let i = 0; i < plane.length; i += 16) {
            const v = Math.abs(plane[i]);
            if (v > audioPeak) audioPeak = v;
          }
        }
      }
      data.close();

      const src = audioCtx.createBufferSource();
      src.buffer = buffer;
      src.connect(gain);
      /*
       * A little ahead of now, so a packet that arrives fractionally
       * late still has somewhere to go -- and not much ahead, in either
       * direction. Behind, the schedule would stutter; ahead, it would
       * quietly become the latency.
       */
      const now = audioCtx.currentTime;
      if (nextAudioAt < now + 0.02 || nextAudioAt > now + AUDIO_MAX_LEAD)
        nextAudioAt = now + AUDIO_LEAD;
      audioLead = nextAudioAt - now;
      src.start(nextAudioAt);
      nextAudioAt += buffer.duration;
    },
    error: (e) => problem("audio: " + e.message),
  });

  /* No description: the server sends bare Opus packets rather than the
   * Ogg encapsulation, which is what WebCodecs takes when none is
   * given. */
  audioDecoder.configure({
    codec: "opus",
    sampleRate: 48000,
    numberOfChannels: audioChannels || 2,
  });
}

function onAudio(payload) {
  if (!audioDecoder || audioDecoder.state !== "configured") return;
  /* Browsers will not make a sound until somebody has touched the page,
   * so the context is nudged awake rather than left silent. */
  if (audioCtx && audioCtx.state === "suspended") audioCtx.resume();

  try {
    audioDecoder.decode(new EncodedAudioChunk({
      type: "key",           /* every Opus packet stands alone */
      timestamp: audioFrames * 20000,
      data: payload.subarray(8),
    }));
  } catch (e) {
    problem("audio decode: " + e.message);
  }
}

/* Some browsers only allow sound after a gesture, and this page is one
 * you are going to touch anyway. */
["pointerdown", "keydown"].forEach((ev) =>
  window.addEventListener(ev, () => { if (audioCtx) audioCtx.resume(); },
                          { once: false }));
