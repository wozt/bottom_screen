/*
 * Bottom Screen -- the socket, the greeting, and the message loop.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
/* ---------------------------------------------------------- connection */

function connect() {
  if (!("VideoDecoder" in window)) {
    say("this browser has no WebCodecs, so it cannot decode the stream");
    return;
  }

  const url = (location.protocol === "https:" ? "wss://" : "ws://") +
              location.host + "/ws";
  socket = new WebSocket(url);
  socket.binaryType = "arraybuffer";

  socket.onopen = () => say("connected, waiting for the stream…");
  socket.onclose = () => say("disconnected");
  socket.onerror = () => say("connection failed");

  let greeted = false;
  socket.onmessage = (ev) => {
    const bytes = new Uint8Array(ev.data);

    if (!greeted) {
      /* The upgrade was the hello, so the first frame is the answer:
       * the ack, then the parameter sets the decoder wants up front. */
      greeted = true;
      const v = new DataView(bytes.buffer);
      console_id = bytes[6];
      resize(v.getUint16(8, true), v.getUint16(10, true));
      audioChannels = bytes[17];
      audioRate = v.getUint16(18, true);
      if (audioRate > 0 && "AudioDecoder" in window) startAudio();
      if (!fullW) { fullW = width; fullH = height; }
      buildPad(console_id);
      showAudioSource(console_id);
      showStickRows(console_id);
      buildSizes(console_id);
      /* The server starts on its own default, so a choice already made
       * has to be re-sent on every connection. */
      if (console_id === 3 && audioSourceEl.value !== "0")
        audioSourceEl.dispatchEvent(new Event("change"));
      say(consoleName() + "  " + width + "×" + height + ", waiting for a keyframe");
      return;
    }

    const type = bytes[0];
    const payload = bytes.subarray(8);
    if (type === MSG.VIDEO) {
      onVideo(payload);
      const now = performance.now();
      if (now - lastReport > 1000) {
        say(consoleName() + "  " + width + "×" + height + "  " +
            chosenCodec + "  " + decoded + "/" + frames + " frames" +
            "  " + (watchingBottom + watchingTop) + " watching" +
            (watchingTop ? " (" + watchingBottom + " bottom, " +
                           watchingTop + " top)" : "") +
            (audioRate ? "  " + audioFrames + " audio, peak " +
                         audioPeak.toFixed(2) +
                         ", lead " + Math.round(audioLead * 1000) + "ms" : "") +
            /* Decoded and audible are not the same thing: a browser
             * that has not been touched yet suspends the context, and
             * everything still schedules into silence. Saying so beats
             * leaving somebody to wonder. */
            "");
        problem(audioRate && audioCtx && audioCtx.state !== "running"
                  ? "tap to allow sound" : "");
        lastReport = now;
      }
    } else if (type === MSG.AUDIO) {
      onAudio(payload);
    } else if (type === MSG.STREAM_INFO) {
      const v = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
      resize(v.getUint16(4, true), v.getUint16(6, true));
      /* What the emulator renders, which is the ceiling for the
       * multiples offered. Only believed while nothing has been asked
       * for, since after that the number is our own doing. */
      if (sizeEl.value === "0") { fullW = width; fullH = height; }
      /* The size changed under the decoder, so its reference picture is
       * worthless whatever it was -- and so is its level, which is why
       * the next keyframe reconfigures rather than this line. */
      if (decoder) { try { decoder.close(); } catch (e) {} }
      decoder = null;
      /* Both counters, or the display reads "3876/454": one restarting
       * beside one that never did says nothing about either. */
      frames = 0;
      decoded = 0;
    } else if (type === MSG.SCREENS) {
      /* A bit per screen. The row appears only if there is a second
       * one to switch to; a server that never sends this leaves it
       * hidden, which is right -- it has nothing to offer. */
      document.getElementById("group-screen").hidden = !(payload[0] & 2);
      /* And who else is here. Sent again whenever somebody arrives,
       * leaves or moves, so the counters are current rather than a
       * memory of the moment this page connected. */
      watchingBottom = payload[1];
      watchingTop = payload[2];
    } else if (type === MSG.PROMPT) {
      /* The machine is asking for something the buttons cannot give. */
      onPrompt(payload);
    } else if (type === MSG.PING) {
      send(MSG.PONG);
    }
  };
}

connect();
