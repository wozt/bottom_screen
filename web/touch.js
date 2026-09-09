/*
 * Bottom Screen -- the stylus, and where a tap lands.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
/* --------------------------------------------------------------- touch */

/*
 * Coordinates go in the space the server announced, not the console's
 * native size: the stream is larger whenever the emulator renders at a
 * higher internal resolution, and dividing by the native size instead
 * puts every tap wrong by exactly that scale.
 */
function pointTo(ev) {
  /*
   * The element fills the stage but the picture does not fill the
   * element: object-fit leaves a band on two sides. Measuring against
   * the element's box instead of the drawing would put every touch
   * short by however wide those bands are -- which on a 4:3 screen in a
   * wide window is most of the width.
   */
  const r = canvas.getBoundingClientRect();
  const scale = Math.min(r.width / width, r.height / height);
  const drawW = width * scale, drawH = height * scale;
  const left = r.left + (r.width - drawW) / 2;
  const top = r.top + (r.height - drawH) / 2;

  const x = Math.round((ev.clientX - left) / scale);
  const y = Math.round((ev.clientY - top) / scale);
  return [Math.max(0, Math.min(width - 1, x)),
          Math.max(0, Math.min(height - 1, y))];
}

/*
 * Which picture is on the wire. Only the bottom one has a touch panel
 * behind it, so this gates every tap below -- the server drops them
 * anyway, and sending them regardless would put a press into the game
 * from a screen that cannot be pressed.
 */
let screenShown = 0;

/* How many are watching each screen, as the server last said. Worth
 * saying out loud: three people on the top screen and none on the
 * bottom explains a great deal about why a picture looks as it does. */
let watchingBottom = 0, watchingTop = 0;

let touching = false;
canvas.addEventListener("pointerdown", (e) => {
  if (screenShown !== 0) return;
  canvas.setPointerCapture(e.pointerId);
  touching = true;
  const [x, y] = pointTo(e);
  sendInput(INPUT.TOUCH_DOWN, 0, x, y);
  e.preventDefault();
});
canvas.addEventListener("pointermove", (e) => {
  if (!touching || screenShown !== 0) return;
  const [x, y] = pointTo(e);
  sendInput(INPUT.TOUCH_MOVE, 0, x, y);
});
function liftTouch(e) {
  if (!touching) return;
  touching = false;
  sendInput(INPUT.TOUCH_UP, 0, 0, 0);
}
canvas.addEventListener("pointerup", liftTouch);
canvas.addEventListener("pointercancel", liftTouch);
