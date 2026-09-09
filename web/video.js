/*
 * Bottom Screen -- WebCodecs, and the size asked of the server.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
/* --------------------------------------------------------------- video */

/*
 * The codec string, read out of the stream rather than assumed.
 *
 * WebCodecs takes the profile and level as part of the codec name, and
 * it means them: a hardcoded avc1.42E01E pins level 3.0, which covers a
 * DS screen and silently decodes nothing at all for a GamePad at
 * 854x480 -- or for anything rendered at a raised internal resolution.
 * The three bytes after the SPS NAL header are exactly profile,
 * constraints and level, so they are taken from there.
 */
function codecFromSps(data) {
  for (let i = 0; i + 4 < data.length; i++) {
    const isStart3 = data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1;
    const isStart4 = data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 0 &&
                     data[i + 3] === 1;
    if (!isStart3 && !isStart4) continue;
    const nal = i + (isStart4 ? 4 : 3);
    if (nal + 3 >= data.length) break;
    if ((data[nal] & 0x1f) !== 7) continue;      /* 7 is the SPS */
    const hex = (b) => b.toString(16).padStart(2, "0");
    return "avc1." + hex(data[nal + 1]) + hex(data[nal + 2]) + hex(data[nal + 3]);
  }
  return null;
}

function startDecoder(codec) {
  if (decoder) { try { decoder.close(); } catch (e) {} }
  decoder = new VideoDecoder({
    output: (frame) => {
      decoded++;
      ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
      frame.close();
    },
    error: (e) => problem("decoder: " + e.message),
  });
  /*
   * No description, which is what tells WebCodecs the stream is Annex B
   * -- and it is, deliberately: the server does not set
   * AV_CODEC_FLAG_GLOBAL_HEADER, so the parameter sets are repeated in
   * front of every keyframe and a client can start at any of them.
   */
  decoder.configure({ codec, optimizeForLatency: true });
}

function onVideo(payload) {
  const v = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const flags = v.getUint16(12, true);
  const keyframe = (flags & 0x01) !== 0;
  const data = payload.subarray(16);

  /*
   * Nothing until a keyframe: it carries the parameter sets the decoder
   * has to be configured from, and a correction to a picture that was
   * never there produces noise rather than an image.
   */
  if (!decoder) {
    if (!keyframe) return;
    const codec = codecFromSps(data);
    if (!codec) { problem("no parameter sets in the keyframe"); return; }
    try {
      startDecoder(codec);
    } catch (e) {
      problem("configure " + codec + ": " + e.message);
      decoder = null;
      return;
    }
    chosenCodec = codec;
  }
  if (decoder.state !== "configured") return;
  if (!keyframe && frames === 0) return;

  frames++;
  try {
    decoder.decode(new EncodedVideoChunk({
      type: keyframe ? "key" : "delta",
      timestamp: frames * 1000,
      data,
    }));
  } catch (e) {
    problem("decode: " + e.message);
  }
}

/* ---------------------------------------------------------------- size */

function resize(w, h) {
  width = w; height = h;
  canvas.width = w; canvas.height = h;
  fit();
}

/*
 * Nothing to do: the canvas carries its own aspect ratio and the two
 * maximums in the stylesheet do the fitting. What is never allowed is
 * stretching the axes apart, and a rule that only ever caps both cannot.
 */
function fit() {}
