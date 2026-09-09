/*
 * Bottom Screen -- the questions the machine asks.
 *
 * Plain scripts in one shared scope, not modules, loaded in the order
 * index.html lists them.
 *
 * A 3DS or a Wii U stops and asks for a name, a message or a Mii. The
 * emulator would answer that with a dialog on the desktop it happens to
 * be running on, which from a phone is somewhere nobody can see while
 * the game waits for ever. The server puts the question on the wire
 * instead; this is where it is answered.
 *
 * Every client watching gets the same question, whichever screen it is
 * on -- the question belongs to the machine, not to the picture. The
 * first answer wins and the rest are told it is over.
 */

const promptBox = document.getElementById("prompt");
const promptTitle = document.getElementById("promptTitle");
const promptField = document.getElementById("promptField");
const promptList = document.getElementById("promptList");
const promptOk = document.getElementById("promptOk");
const promptCancel = document.getElementById("promptCancel");

let promptId = 0;

function promptHide() {
  promptId = 0;
  promptBox.hidden = true;
  promptList.innerHTML = "";
}

/* id 0 answers nothing; the reply carries the id so a late answer to a
 * question the game has withdrawn is recognised and dropped. */
function promptAnswer(cancelled, choice, text) {
  if (!promptId) return;
  const body = new TextEncoder().encode(text || "");
  const b = new Uint8Array(4 + body.length);
  const v = new DataView(b.buffer);
  v.setUint16(0, promptId, true);
  b[2] = cancelled ? 1 : 0;
  b[3] = choice & 0xff;
  b.set(body, 4);
  send(MSG.PROMPT_REPLY, b);
  promptHide();
}

promptOk.addEventListener("click", () => promptAnswer(0, 0, promptField.value));
promptCancel.addEventListener("click", () => promptAnswer(1, 0, ""));
promptField.addEventListener("keydown", (e) => {
  /* Enter sends, Escape declines: a box you have to reach for the mouse
   * to leave is a box in the way. */
  if (e.key === "Enter" && !promptField.matches("textarea")) {
    promptAnswer(0, 0, promptField.value);
  } else if (e.key === "Escape") {
    promptAnswer(1, 0, "");
  }
  e.stopPropagation();
});

/* The payload is the header followed by NUL-separated UTF-8: the title,
 * then one label per choice. */
function onPrompt(payload) {
  const v = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const id = v.getUint16(0, true);
  if (id === 0) { promptHide(); return; }        /* withdrawn */

  const kind = payload[2];
  const choices = payload[3];
  const maxLen = v.getUint16(4, true);
  const multiline = payload[6];

  const text = new TextDecoder().decode(payload.subarray(8));
  const parts = text.split("\0");

  promptId = id;
  promptTitle.textContent = parts[0] || "";
  promptList.innerHTML = "";
  promptBox.hidden = false;

  if (kind === 2) {
    promptField.hidden = true;
    promptOk.hidden = true;
    for (let i = 0; i < choices; i++) {
      const b = document.createElement("button");
      b.textContent = parts[1 + i] || ("#" + i);
      b.addEventListener("click", () => promptAnswer(0, i, b.textContent));
      promptList.appendChild(b);
    }
  } else {
    promptField.hidden = false;
    promptOk.hidden = false;
    promptField.value = "";
    promptField.maxLength = maxLen > 0 ? maxLen : 255;
    promptField.placeholder = multiline ? "" : "";
    promptField.focus();
  }
}
