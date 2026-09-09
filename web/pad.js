/*
 * Bottom Screen -- the on-screen controls: drawing them, moving them,
 * and reflecting a real gamepad in them.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
/* ------------------------------------------------------------- buttons */

/*
 * Every control is registered as it is built, so a physical pad can be
 * shown on the on-screen ones. Pressing a button on a controller and
 * seeing nothing move is how you end up unsure whether it is connected
 * at all -- the Android client has always reflected it, and this did
 * not.
 */
const padButtons = new Map();
const padSticks = [];

function addButton(parent, label, code) {
  const b = document.createElement("button");
  b.className = "b";
  b.textContent = label;
  const down = (e) => { e.preventDefault(); sendInput(INPUT.BUTTON_DOWN, code, 0, 0); };
  const up   = (e) => { e.preventDefault(); sendInput(INPUT.BUTTON_UP, code, 0, 0); };
  b.addEventListener("pointerdown", down);
  b.addEventListener("pointerup", up);
  b.addEventListener("pointerleave", up);
  b.addEventListener("pointercancel", up);
  parent.appendChild(b);
  padButtons.set(code, b);
  return b;
}

/*
 * A stick: drag inside the circle, let go and it re-centres.
 *
 * Both axes are sent on every move rather than only the one that
 * changed, because a diagonal is two axes and sending one of them makes
 * the character walk along a wall. Releasing sends zero on both -- a
 * stick left off-centre keeps walking after the thumb has gone.
 */
function addStick(parent, spec) {
  const wrap = document.createElement("div");
  wrap.className = "stickwrap";
  const label = document.createElement("div");
  label.className = "sticklabel";
  label.textContent = spec.label;
  const el = document.createElement("div");
  el.className = "stick";
  const knob = document.createElement("div");
  knob.className = "knob";
  el.appendChild(knob);
  wrap.append(label, el);
  parent.appendChild(wrap);
  /* Kept so the gamepad poll can leave this one alone while a finger
   * is on it: two things moving one knob would fight. */
  const entry = { spec, el, knob, dragging: false };
  padSticks.push(entry);

  let held = null;
  const move = (ev) => {
    const r = el.getBoundingClientRect();
    const radius = r.width / 2;
    let dx = (ev.clientX - (r.left + radius)) / radius;
    let dy = (ev.clientY - (r.top + radius)) / radius;
    const m = Math.hypot(dx, dy);
    if (m > 1) { dx /= m; dy /= m; }
    knob.style.transform = `translate(${dx * radius * 0.6}px, ${dy * radius * 0.6}px)`;
    sendInput(INPUT.AXIS, spec.x, Math.round(dx * 32767), 0);
    /* Screen y grows downwards, sticks report up as positive. */
    sendInput(INPUT.AXIS, spec.y, Math.round(-dy * 32767), 0);
  };
  const release = () => {
    held = null;
    entry.dragging = false;
    knob.style.transform = "";
    sendInput(INPUT.AXIS, spec.x, 0, 0);
    sendInput(INPUT.AXIS, spec.y, 0, 0);
  };
  el.addEventListener("pointerdown", (ev) => {
    held = ev.pointerId;
    entry.dragging = true;
    el.setPointerCapture(ev.pointerId);
    move(ev);
    ev.preventDefault();
  });
  el.addEventListener("pointermove", (ev) => { if (held === ev.pointerId) move(ev); });
  for (const e of ["pointerup", "pointercancel"])
    el.addEventListener(e, (ev) => { if (held === ev.pointerId) release(); });
  return wrap;
}

/* An empty container -- a stick column on a DS, say -- is not a group.
 * Placing one would leave a draggable frame around nothing. */
function hasContent(el) { return el && el.childElementCount > 0; }

/*
 * Where the groups sit, as fractions of their column, per console.
 *
 * Kept per console because the three do not have the same controls: a
 * Wii U's arrangement has a HOME button in it and a DS's has no stick,
 * so one layout could not describe both. Three sets, saved separately,
 * which is what the other two clients do.
 */
let padLayout = {};
function layoutKey() { return "bs_pad_layout_" + (console_id || 1); }
function loadLayout() {
  try { padLayout = JSON.parse(localStorage.getItem(layoutKey())) || {}; }
  catch (e) { padLayout = {}; }
}
function saveLayout() {
  try { localStorage.setItem(layoutKey(), JSON.stringify(padLayout)); }
  catch (e) {}
}

/*
 * Stacked from the bottom, with the same small gap between groups the
 * Switch client uses, and any position somebody saved taking precedence
 * over the default.
 */
function placeGroups(column, side, groups, pinnedTop) {
  column.innerHTML = "";
  const gap = 14;
  let bottom = 26;
  groups.forEach((g, i) => {
    g.classList.add("pgroup");
    g.dataset.key = side + i;
    column.appendChild(g);
  });
  if (pinnedTop) {
    pinnedTop.classList.add("pgroup");
    pinnedTop.dataset.key = side + "top";
    column.appendChild(pinnedTop);
  }
  /* Measured after they are in the document, since a group's height is
   * whatever its contents came to. */
  requestAnimationFrame(() => {
    const box = column.getBoundingClientRect();
    if (!box.height) return;
    for (const g of groups) {
      const saved = padLayout[g.dataset.key];
      const h = g.getBoundingClientRect().height;
      if (saved) {
        g.style.left = (saved.x * 100) + "%";
        g.style.top = (saved.y * 100) + "%";
      } else {
        g.style.left = "50%";
        g.style.top = (((box.height - bottom - h / 2) / box.height) * 100) + "%";
      }
      bottom += h + gap;
    }
    if (pinnedTop) {
      const saved = padLayout[pinnedTop.dataset.key];
      const h = pinnedTop.getBoundingClientRect().height;
      pinnedTop.style.left = saved ? (saved.x * 100) + "%" : "50%";
      pinnedTop.style.top = saved
        ? (saved.y * 100) + "%"
        : (((26 + h / 2) / box.height) * 100) + "%";
    }
    fit();
  });
}

/* Dragging a group moves it, and only while arranging: a thumb on a
 * button during play must press it, not carry it off. */
function armDragging() {
  for (const column of [padLeftEl, padRightEl]) {
    column.addEventListener("pointerdown", (ev) => {
      if (!document.body.classList.contains("arranging")) return;
      const g = ev.target.closest(".pgroup");
      if (!g) return;
      const box = column.getBoundingClientRect();
      column.setPointerCapture(ev.pointerId);
      const move = (e) => {
        const x = Math.min(1, Math.max(0, (e.clientX - box.left) / box.width));
        const y = Math.min(1, Math.max(0, (e.clientY - box.top) / box.height));
        g.style.left = (x * 100) + "%";
        g.style.top = (y * 100) + "%";
        padLayout[g.dataset.key] = { x, y };
      };
      const up = () => {
        column.removeEventListener("pointermove", move);
        column.removeEventListener("pointerup", up);
        saveLayout();
      };
      column.addEventListener("pointermove", move);
      column.addEventListener("pointerup", up);
      ev.preventDefault();
    });
  }
}
armDragging();

function buildPad(id) {
  loadLayout();
  padLeftEl.innerHTML = "";
  padRightEl.innerHTML = "";
  padButtons.clear();
  padSticks.length = 0;
  const profile = CONSOLES[id] || CONSOLES[1];

  /*
   * Each side is laid out the way the console holds it: triggers at the
   * top, the thumb control in the middle, a menu button at the bottom.
   * Everything used to live in the right-hand column, which made that
   * column tall enough to be pressed against the bottom edge while the
   * left one held nothing but the d-pad. This also matches the Android
   * client, which has always split the two sides.
   */
  const shoulderSide = (near, far) =>
    [far, near].filter((n) => profile.shoulders.includes(n));

  const left = document.createElement("div");
  left.className = "cluster";
  const lShoulders = document.createElement("div");
  lShoulders.className = "stack";
  shoulderSide("L", "ZL").forEach(
    (n) => addButton(lShoulders, n, BTN[n]).classList.add("wide"));
  const dpad = document.createElement("div");
  dpad.id = "dpad";
  const cells = [null, BTN.UP, null, BTN.LEFT, null, BTN.RIGHT, null, BTN.DOWN, null];
  const names = ["", "▲", "", "◀", "", "▶", "", "▼", ""];
  cells.forEach((code, i) => {
    if (code === null) { dpad.appendChild(document.createElement("div")); return; }
    addButton(dpad, names[i], code);
  });
  const lStick = document.createElement("div");
  const rStick = document.createElement("div");
  (profile.sticks || []).forEach((sp) => addStick(sp.left ? lStick : rStick, sp));

  /* Menu buttons alternate sides, as they do on Android: a Wii U has
   * three and there are two places to put them. */
  const lMenu = document.createElement("div");
  lMenu.className = "row";
  const rMenu = document.createElement("div");
  rMenu.className = "row";
  /* HOME on its own, pinned to the top of its column: it stops the
   * game, so it should take a deliberate reach rather than sit under a
   * thumb that is already resting somewhere. */
  const home = document.createElement("div");
  home.className = "row";
  (profile.menu || ["SELECT", "START"]).forEach((n, i) => {
    const into = n === "HOME" ? home : (i % 2 ? rMenu : lMenu);
    addButton(into, MENU_LABEL[n] || n, BTN[n]).classList.add("wide");
  });


  const right = document.createElement("div");
  right.className = "cluster";
  const shoulders = document.createElement("div");
  shoulders.className = "stack";
  shoulderSide("R", "ZR").forEach(
    (n) => addButton(shoulders, n, BTN[n]).classList.add("wide"));

  /* X on top, Y left, A right, B bottom -- the layout every console
     here uses, rather than the order they happen to be listed in. */
  const face = document.createElement("div");
  face.className = "diamond";
  addButton(face, "X", BTN.X).classList.add("top");
  addButton(face, "Y", BTN.Y).classList.add("left");
  addButton(face, "A", BTN.A).classList.add("right");
  addButton(face, "B", BTN.B).classList.add("bottom");

  /*
   * Each group placed in its column, stacked from the bottom -- which
   * is the end a thumb reaches -- in the order that side asked for.
   * "left" and "right" are gone: there is nothing between a column and
   * the groups in it any more.
   */
  const leftGroups = stickBelow(0)
    ? [lMenu, lStick, dpad, lShoulders]
    : [lMenu, dpad, lStick, lShoulders];
  const rightGroups = stickBelow(1)
    ? [rMenu, rStick, face, shoulders]
    : [rMenu, face, rStick, shoulders];

  placeGroups(padLeftEl, "l", leftGroups.filter(hasContent),
              hasContent(home) ? home : null);
  placeGroups(padRightEl, "r", rightGroups.filter(hasContent), null);
  applyPadVisibility();
  /* The stage only knows its height once the pad has taken its own, so
   * the picture is sized after the layout has settled. */
  requestAnimationFrame(fit);
}

/* A physical pad, if one is paired: the same codes the on-screen
 * buttons send, so the server cannot tell them apart. */
const GAMEPAD_MAP = [BTN.B, BTN.A, BTN.Y, BTN.X, BTN.L, BTN.R, BTN.ZL, BTN.ZR,
                     BTN.SELECT, BTN.START, 0, 0,
                     BTN.UP, BTN.DOWN, BTN.LEFT, BTN.RIGHT];
let padState = [];
let axisState = [0, 0, 0, 0];
let padPresent = false;

/* Below this a stick is at rest. Analogue sticks do not return exactly
 * to centre, and forwarding that would walk the character slowly into a
 * wall while nobody is touching anything. */
const DEADZONE = 0.12;

function pollGamepad() {
  const gp = navigator.getGamepads && navigator.getGamepads()[0];
  if (gp) {
    for (let i = 0; i < GAMEPAD_MAP.length && i < gp.buttons.length; i++) {
      const code = GAMEPAD_MAP[i];
      if (!code) continue;
      const now = gp.buttons[i].pressed;
      if (now !== padState[i]) {
        padState[i] = now;
        sendInput(now ? INPUT.BUTTON_DOWN : INPUT.BUTTON_UP, code, 0, 0);
        const el = padButtons.get(code);
        if (el) el.classList.toggle("held", now);
      }
    }

    /*
     * The sticks, which were not forwarded at all: a pad drove every
     * button and nothing that moved. Axes 0 and 1 are the left stick,
     * 2 and 3 the right, which is what the standard mapping says and
     * what the profiles here already call LEFT and RIGHT.
     */
    for (let a = 0; a < 4 && a < gp.axes.length; a++) {
      let v = gp.axes[a];
      if (Math.abs(v) < DEADZONE) v = 0;
      const q = Math.round(v * 32767);
      if (q !== axisState[a]) {
        axisState[a] = q;
        /* Odd indices are the vertical ones. A pad reports down as
         * positive, like a screen; the protocol reports up as positive,
         * like a stick. Without the flip, forward walks backwards. */
        sendInput(INPUT.AXIS, a + 1, a % 2 ? -q : q, 0);
      }
    }
    for (const st of padSticks) {
      const ax = st.spec.x - 1, ay = st.spec.y - 1;
      const r = st.el.getBoundingClientRect().width / 2;
      const dx = axisState[ax] / 32767, dy = axisState[ay] / 32767;
      if (dx || dy)
        st.knob.style.transform =
          `translate(${dx * r * 0.6}px, ${dy * r * 0.6}px)`;
      else if (!st.dragging)
        st.knob.style.transform = "";
    }
  }
  /* A pad plugged in or unplugged changes what should be on screen,
   * and the browser only tells us by the list changing. */
  {
    const now = !!(navigator.getGamepads &&
                   [...navigator.getGamepads()].some((g) => g));
    if (now !== padPresent) { padPresent = now; applyPadVisibility(); }
  }
  requestAnimationFrame(pollGamepad);
}
requestAnimationFrame(pollGamepad);

/*
 * The bar shows itself when you move and gets out of the way when you
 * stop -- it sits on top of what you are watching. An open menu holds
 * it, because having a panel vanish mid-decision would be maddening.
 */
let barTimer = 0;
function showBar() {
  barEl.classList.add("visible");
  clearTimeout(barTimer);
  barTimer = setTimeout(() => {
    if (!barEl.querySelector("details[open]")) barEl.classList.remove("visible");
  }, 2500);
}

/*
 * Where the pointer has to be for the bar to come back: one corner.
 *
 * The bar sits over the top of the picture, and on a screen you are
 * drawing on with a stylus a bar that reappears whenever the pen moves
 * is a bar permanently in the way. The first attempt at this took the
 * top tenth of the window wherever there was black beside the picture,
 * which sounded narrow and was not -- with the on-screen buttons shown
 * that black is a whole column, so the bar came back on almost any
 * movement at all.
 *
 * Sixteen pixels of the top-left corner instead. Findable with a mouse,
 * small enough that nothing else lands there, and the same answer in
 * every mode -- including the one that has no black to aim at, which is
 * a 16:9 picture filling a fullscreen window with the buttons turned
 * off.
 */
const BAR_CORNER = 16;

function barZone(x, y) {
  return x < BAR_CORNER && y < BAR_CORNER;
}

window.addEventListener("pointermove", (e) => {
  /* Moving within the bar itself keeps it, or it would fade out from
   * under the hand reaching for it. */
  if (barEl.contains(e.target) || barZone(e.clientX, e.clientY)) showBar();
});
window.addEventListener("pointerdown", (e) => {
  if (barEl.contains(e.target) || barZone(e.clientX, e.clientY)) showBar();
});
/* A keyboard has no pointer to put anywhere. */
window.addEventListener("keydown", showBar);
showBar();

/* One menu at a time: two panels open at once overlap each other. */
barEl.addEventListener("toggle", (e) => {
  if (!(e.target instanceof HTMLDetailsElement) || !e.target.open) return;
  for (const d of barEl.querySelectorAll("details[open]"))
    if (d !== e.target) d.open = false;
}, true);
