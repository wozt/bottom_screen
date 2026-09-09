/*
 * Bottom Screen -- putting messages on the wire.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
/* ------------------------------------------------------------- sending */

function send(type, payload) {
  if (!socket || socket.readyState !== 1) return;
  const body = payload || new Uint8Array(0);
  const buf = new Uint8Array(8 + body.length);
  const view = new DataView(buf.buffer);
  buf[0] = type;
  view.setUint32(4, body.length, true);
  buf.set(body, 8);
  socket.send(buf);
}

function sendInput(type, code, x, y) {
  const b = new Uint8Array(16);
  const v = new DataView(b.buffer);
  v.setUint32(0, sequence++, true);
  v.setUint32(4, (performance.now() * 1000) & 0xffffffff, true);
  b[8] = type;
  b[9] = code;
  v.setInt16(10, x, true);
  v.setInt16(12, y, true);
  send(MSG.INPUT, b);
}

/*
 * Ask for less than the emulator renders.
 *
 * An emulator at four times its internal resolution puts 1280x960 on
 * the wire for a screen that is 320x240, and a phone showing it in part
 * of its display gains nothing from the extra pixels but pays for all
 * of them.
 *
 * Shared with anyone else watching, because there is one encoder: a
 * size each would mean an encoder each. The server answers with the
 * size it settled on, which is not always the one asked for.
 */
/*
 * Sizes as multiples of the console's own screen, which is how the
 * other two clients put it.
 *
 * This used to divide whatever the emulator happened to be rendering --
 * full, half, quarter -- and those mean nothing fixed: half of a 4x
 * internal resolution is twice a 2x one. A multiple of the console's
 * own screen means the same thing whatever the emulator is set to, and
 * it is the number the other clients show.
 *
 * The ceiling is the server's own: nothing taller than 1440 is offered,
 * because nothing taller would be encoded.
 */
const MAX_STREAM_HEIGHT = 1440;
const sizeEl = document.getElementById("size");

/*
 * The screen on the wire, at its own native size.
 *
 * Not the console's, which means the bottom screen's and is the wrong
 * answer for a 3DS: 320x240 below, 400x240 above -- 4:3 against 5:3.
 * Offering multiples of the bottom screen while the top one is being
 * sent asks the server to squeeze a 5:3 picture into a 4:3 box, and it
 * obliges. A DS has two screens the same size and a Wii U's are both
 * 16:9, so the 3DS is the one where it shows.
 */
function nativeSize(id, screen) {
  if ((screen !== undefined ? screen : screenShown) === 1)
    return id === 3 ? [1280, 720] : id === 2 ? [400, 240] : [256, 192];
  return id === 3 ? [854, 480] : id === 2 ? [320, 240] : [256, 192];
}

/*
 * Whether the address we dialled is on this network or through a tunnel.
 *
 * It matters because the two do not carry the same amount: a LAN will
 * take the emulator's full internal resolution and a link over the
 * internet will not, and the default here is "whatever is rendered",
 * which on a raised internal resolution is a great deal. Tailscale hands
 * out addresses in 100.64.0.0/10, the carrier-grade NAT range, and
 * anything that is not a private address is not on this network either.
 *
 * The size is not changed on anyone's behalf, because it is shared: one
 * client quietly lowering it would lower it for everybody watching. It
 * is said instead, next to the control, and the choice stays where it
 * belongs.
 */
function linkLooksRemote() {
  const h = location.hostname;
  const m = h.match(/^(\d+)\.(\d+)\.\d+\.\d+$/);
  if (!m) return false;                       /* a name says nothing */
  const a = +m[1], b = +m[2];
  if (a === 127 || a === 10) return false;
  if (a === 192 && b === 168) return false;
  if (a === 172 && b >= 16 && b <= 31) return false;
  if (a === 169 && b === 254) return false;
  return true;                                /* 100.64/10 included */
}

function buildSizes(id) {
  const [nw, nh] = nativeSize(id);
  sizeEl.innerHTML = "";
  const add = (label, value) => {
    const o = document.createElement("option");
    o.value = value; o.textContent = label;
    sizeEl.appendChild(o);
  };
  /* Below native only where there is enough to lose: a fraction of a DS
   * screen is smaller than a decoder will take. */
  if (nw >= 640) add(`half of native  ${nw / 2}x${nh / 2}`, -2);
  for (let f = 1; nw * f <= (fullW || nw) && nh * f <= MAX_STREAM_HEIGHT; f++)
    add(f === 1 ? `native  ${nw}x${nh}` : `${f}x native  ${nw * f}x${nh * f}`, f);
  add(`whatever is rendered  ${fullW || nw}x${fullH || nh}`, 0);
  sizeEl.value = "0";

  const warn = document.getElementById("remote-hint");
  warn.hidden = !linkLooksRemote();
}

sizeEl.addEventListener("change", () => {
  const factor = parseInt(sizeEl.value, 10);
  const [nw, nh] = nativeSize(console_id);   /* the screen now shown */
  const b = new Uint8Array(4);
  const v = new DataView(b.buffer);
  if (factor === -2) {
    v.setUint16(0, nw / 2, true);
    v.setUint16(2, nh / 2, true);
  } else if (factor > 0) {
    v.setUint16(0, nw * factor, true);
    v.setUint16(2, nh * factor, true);
  }
  /* Zero means "follow the source", which is how a client stops
   * asking rather than guessing at the original numbers. */
  send(MSG.SET_SIZE, b);
});
