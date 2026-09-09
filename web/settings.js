/*
 * Bottom Screen -- the menu bar and everything it changes.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
/* ---------------------------------------------------------- controls */

const qualityEl = document.getElementById("quality");
/* Its own function because a screen change has to say it again: the
 * encoder behind each screen is separate, and the new one was built on
 * the server's defaults. */
function applyQuality() {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setUint32(0, parseInt(qualityEl.value, 10) || 0, true);
  /* Zero lets the server derive one from the size, which is what it
   * does when nobody has asked. */
  send(MSG.SET_QUALITY, b);
}
qualityEl.addEventListener("change", applyQuality);

const volumeEl = document.getElementById("volume");
const volumeVal = document.getElementById("volume-v");
const muteEl = document.getElementById("mute");
function applyVolume() {
  volumeVal.textContent = volumeEl.value + "%";
  if (gain) gain.gain.value = muteEl.checked ? 0 : volumeEl.value / 100;
}
volumeEl.addEventListener("input", applyVolume);
muteEl.addEventListener("change", applyVolume);

/*
 * Only a Wii U has two outputs to choose between, so the row appears
 * only there rather than offering a choice with one answer.
 */
const audioSourceEl = document.getElementById("audioSource");
/* Only a console with sticks has anywhere to put them. */
function showStickRows(id) {
  const has = id === 2 || id === 3;
  document.getElementById("stick-l-row").hidden = !has;
  document.getElementById("stick-r-row").hidden = !has;
}

function showAudioSource(id) {
  const wiiu = id === 3;
  document.getElementById("audioSourceRow").hidden = !wiiu;
  document.getElementById("audioSourceHint").hidden = !wiiu;
}
audioSourceEl.addEventListener("change", () => {
  const b = new Uint8Array(4);
  b[0] = parseInt(audioSourceEl.value, 10) || 0;
  send(MSG.SET_AUDIO_SOURCE, b);
});

/*
 * The other screen.
 *
 * The whole group stays hidden until the server says it has one, rather
 * than offering a choice that would do nothing -- a backend that only
 * hands over the bottom screen is a normal thing to be talking to, and
 * an older server says nothing at all and is treated the same way.
 *
 * Nothing is drawn differently on arrival: the picture changes size,
 * which comes through STREAM_INFO like any other change of shape and
 * goes through the same path. What does change is the touch layer,
 * which stops sending.
 */
const screenPickEl = document.getElementById("screenPick");

/*
 * Quality and size, one of each per screen.
 *
 * The server encodes the two separately -- that is what a second screen
 * costs -- so these are genuinely separate answers: a television picture
 * worth 8 Mbit/s beside a GamePad screen worth 2 is a normal pair, not a
 * contradiction. Held as the select's own values, because that is what
 * has to go back into it.
 *
 * Sound is not here. There is one set of speakers whatever is on screen.
 */
const qualityFor = ["0", "0"];
const scaleFor = ["0", "0"];

screenPickEl.addEventListener("change", () => {
  /* What was on the screen being left, remembered before leaving it. */
  qualityFor[screenShown] = qualityEl.value;
  scaleFor[screenShown] = sizeEl.value;

  screenShown = parseInt(screenPickEl.value, 10) || 0;
  if (touching) { touching = false; sendInput(INPUT.TOUCH_UP, 0, 0, 0); }
  const b = new Uint8Array(4);
  b[0] = screenShown;
  send(MSG.SET_SCREEN, b);

  /*
   * The screen just joined has its own encoder, built on the server's
   * defaults: it has never heard of the quality or the size chosen
   * here. Both are said again, or a stream asked for at a low bitrate
   * arrives at full quality on the other screen with nothing in the
   * menu to explain it.
   *
   * The ladder is rebuilt first, because its rungs are multiples of the
   * screen that is now on the wire and a 3DS changes shape between the
   * two. fullW is cleared so the "whatever is rendered" rung stops
   * quoting the old screen's size until the first STREAM_INFO says what
   * the new one is.
   */
  fullW = 0; fullH = 0;
  buildSizes(console_id);
  /* What this screen was last set to, put back before either is sent.
   * A rung that no longer exists -- the other screen renders less --
   * falls back to "whatever is rendered", which always does. */
  qualityEl.value = qualityFor[screenShown];
  sizeEl.value = scaleFor[screenShown];
  if (!sizeEl.value) sizeEl.value = "0";
  applyQuality();
  sizeEl.dispatchEvent(new Event("change"));
});

/* Remembered as they are changed, not only when a screen is left: a
 * client that never switches back still has the right answer stored for
 * the one it is on. */
qualityEl.addEventListener("change", () => { qualityFor[screenShown] = qualityEl.value; });
sizeEl.addEventListener("change", () => { scaleFor[screenShown] = sizeEl.value; });

/*
 * The pad's colour, chosen rather than fixed.
 *
 * These sit on top of whatever the emulator is showing, and there is no
 * one colour that works against all of it -- white vanishes on a bright
 * scene. The same short palette the Switch client offers, so the two
 * are the same pad. The label takes the colour too: the difference
 * between a label and its button is the strength, not the hue.
 */
const PAD_COLOURS = [
  ["white",   255, 255, 255], ["black",     0,   0,   0],
  ["red",     255,  70,  70], ["orange",  255, 150,  40],
  ["yellow",  255, 225,  60], ["green",    90, 220, 120],
  ["cyan",     80, 220, 235], ["blue",     90, 150, 255],
  ["magenta", 240, 100, 230],
];
const padColourEl = document.getElementById("pad-colour");
PAD_COLOURS.forEach(([name], i) => {
  const o = document.createElement("option");
  o.value = i; o.textContent = name;
  padColourEl.appendChild(o);
});

function applyPadColour(i) {
  const [, r, g, b] = PAD_COLOURS[i] || PAD_COLOURS[0];
  const st = document.body.style;
  st.setProperty("--btn-bg", `rgba(${r},${g},${b},.18)`);
  st.setProperty("--btn-border", `rgba(${r},${g},${b},.60)`);
  st.setProperty("--btn-pressed", `rgba(${r},${g},${b},.55)`);
  st.setProperty("--btn-label", `rgba(${r},${g},${b},.92)`);
  try { localStorage.setItem("bs_pad_colour", String(i)); } catch (e) {}
}
padColourEl.addEventListener("change",
  () => applyPadColour(parseInt(padColourEl.value, 10) || 0));
{
  let saved = 0;
  try { saved = parseInt(localStorage.getItem("bs_pad_colour"), 10) || 0; }
  catch (e) { saved = 0; }
  if (saved < 0 || saved >= PAD_COLOURS.length) saved = 0;
  padColourEl.value = saved;
  applyPadColour(saved);
}

/*
 * Where each side's stick sits relative to its thumb control.
 *
 * A Switch is asymmetric -- stick above the d-pad on the left, face
 * buttons above the stick on the right -- and a PlayStation is not.
 * Which one a hand expects is not something this can know, so it is a
 * choice, one per side, with the same defaults the Switch client uses.
 */
const stickLEl = document.getElementById("stick-l");
const stickREl = document.getElementById("stick-r");
function stickBelow(side) {
  const el = side ? stickREl : stickLEl;
  return el.value === "1";
}
for (const [el, key, dflt] of [[stickLEl, "bs_stick_l", "0"],
                               [stickREl, "bs_stick_r", "1"]]) {
  let v = dflt;
  try { v = localStorage.getItem(key) ?? dflt; } catch (e) {}
  el.value = v;
  el.addEventListener("change", () => {
    try { localStorage.setItem(key, el.value); } catch (e) {}
    if (console_id) buildPad(console_id);
  });
}

const arrangeEl = document.getElementById("arrange-pad");
arrangeEl.addEventListener("change", () => {
  document.body.classList.toggle("arranging", arrangeEl.checked);
});
document.getElementById("reset-pad").addEventListener("click", () => {
  padLayout = {};
  saveLayout();
  if (console_id) buildPad(console_id);
});

/*
 * Three states, as the Android client has: the useful default is
 * neither on nor off. Somebody who plugs a controller in wants the
 * buttons out of the way without being asked, and somebody who unplugs
 * it wants them back.
 */
const padShownEl = document.getElementById("pad-shown");
function padWanted() {
  if (padShownEl.value === "never") return false;
  if (padShownEl.value === "always") return true;
  return !(navigator.getGamepads && [...navigator.getGamepads()].some((g) => g));
}
function setPadShown(shown) {
  for (const el of [padLeftEl, padRightEl])
    el.style.display = shown && console_id ? "block" : "none";
}
function applyPadVisibility() { setPadShown(padWanted()); }
padShownEl.addEventListener("change", () => {
  try { localStorage.setItem("bs_pad_shown", padShownEl.value); } catch (e) {}
  applyPadVisibility();
});
try {
  const v = localStorage.getItem("bs_pad_shown");
  if (v) padShownEl.value = v;
} catch (e) {}

/* The buttons' size, since a thumb and a mouse do not want the same
 * one. The whole pad scales together, as it does on the other two. */
const padSizeEl = document.getElementById("pad-size");
function applyPadSize() {
  const pct = parseInt(padSizeEl.value, 10) || 100;
  document.getElementById("pad-size-v").textContent = pct + "%";
  document.body.style.setProperty(
    "--btn", `clamp(${Math.round(24 * pct / 100)}px, ${(6.2 * pct / 100).toFixed(2)}vh, ${Math.round(64 * pct / 100)}px)`);
  try { localStorage.setItem("bs_pad_size", String(pct)); } catch (e) {}
  if (console_id) buildPad(console_id);
}
padSizeEl.addEventListener("change", applyPadSize);
try {
  const v = parseInt(localStorage.getItem("bs_pad_size"), 10);
  if (v >= 60 && v <= 160) padSizeEl.value = v;
} catch (e) {}
applyPadSize();

document.getElementById("fullscreen-toggle").addEventListener("click", () => {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen().catch(() => {});
});
