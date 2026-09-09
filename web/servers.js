/*
 * Bottom Screen -- the saved list of servers.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
/* ------------------------------------------------------------ servers */

/*
 * The other emulators.
 *
 * Each one serves this page on its own port, so moving from the DS to
 * the Wii U is a different address rather than a different setting --
 * and typing one on a phone is exactly the friction worth removing.
 *
 * Kept in this browser, because the list is about which machines you
 * use rather than anything the server knows: an emulator has no
 * business holding a list of its neighbours.
 */
const KEY = "bottom_screen.servers";
const serversEl = document.getElementById("servers");

function loadServers() {
  try {
    const raw = localStorage.getItem(KEY);
    return raw ? JSON.parse(raw) : [];
  } catch (e) {
    /* A list that cannot be read is treated as an empty one: nothing
     * here is worth failing a page load over. */
    return [];
  }
}

function saveServers(list) {
  try { localStorage.setItem(KEY, JSON.stringify(list)); } catch (e) {}
}

/*
 * The list travels with the link.
 *
 * Each emulator serves its page on its own port, and a different port is
 * a different origin, so the browser gives each one its own storage. A
 * list saved on the DS page is invisible from the Wii U page -- which
 * makes a list of the others useless exactly where it was meant to
 * help.
 *
 * So it rides along in the fragment, which the browser never sends to
 * the server, and the page it lands on merges it. After one hop every
 * page knows all of them.
 */
function encodeList(list) {
  try { return btoa(unescape(encodeURIComponent(JSON.stringify(list)))); }
  catch (e) { return ""; }
}

function mergeFromHash() {
  if (!location.hash.startsWith("#s=")) return;
  try {
    const incoming = JSON.parse(
      decodeURIComponent(escape(atob(location.hash.slice(3)))));
    const mine = loadServers();
    const known = new Set(mine.map((s) => s.host));
    /* Names already chosen here win: the person renamed it for a
     * reason. */
    for (const entry of incoming)
      if (entry && entry.host && !known.has(entry.host)) mine.push(entry);
    saveServers(mine);
  } catch (e) {
    /* A malformed fragment is somebody else's link, not a reason to
     * stop. */
  }
  history.replaceState(null, "", location.pathname);
}
mergeFromHash();

function rememberThisOne() {
  const list = loadServers().filter((s) => s.host !== location.host);
  /* Named after the console, which is only known once the server has
   * answered -- "Wii U (5092)" is worth more than the address it
   * replaces. */
  const name = console_id
    ? consoleName() + " (" + (location.port || "80") + ")"
    : location.host;
  list.push({ name, host: location.host });
  saveServers(list);
  drawServers();
}

function drawServers() {
  const list = loadServers();
  serversEl.innerHTML = "";

  if (!list.length) {
    const hint = document.createElement("div");
    hint.className = "hint";
    hint.textContent = "Nothing saved yet.";
    serversEl.appendChild(hint);
  }

  for (const entry of list) {
    const row = document.createElement("div");
    row.className = "srv";

    const link = document.createElement("a");
    link.href = location.protocol + "//" + entry.host + "/#s=" +
                encodeURIComponent(encodeList(list));
    link.textContent = entry.name +
      (entry.host === location.host ? "  (this one)" : "");
    row.appendChild(link);

    const drop = document.createElement("button");
    drop.textContent = "\u00d7";
    drop.title = "Forget";
    drop.addEventListener("click", (e) => {
      e.preventDefault();
      saveServers(loadServers().filter((s) => s.host !== entry.host));
      drawServers();
    });
    row.appendChild(drop);
    serversEl.appendChild(row);
  }

  const add = document.createElement("button");
  add.textContent = loadServers().some((s) => s.host === location.host)
    ? "Rename this one" : "Save this one";
  add.addEventListener("click", rememberThisOne);
  serversEl.appendChild(add);

  /*
   * Adding one you are not on, which is the only way a list ever gets a
   * second entry: "save this one" can only ever describe the page you
   * are reading.
   *
   * A bare number is a port on this machine, because that is the case
   * every time -- three emulators side by side, 5090, 5091, 5092.
   */
  const field = document.createElement("input");
  field.placeholder = "another port, or host:port";
  field.addEventListener("keydown", (e) => {
    if (e.key !== "Enter") return;
    const typed = field.value.trim();
    if (!typed) return;
    const host = /^\d+$/.test(typed)
      ? location.hostname + ":" + typed
      : typed;
    const list = loadServers().filter((s) => s.host !== host);
    list.push({ name: host, host });
    saveServers(list);
    field.value = "";
    drawServers();
  });
  serversEl.appendChild(field);

  const hint = document.createElement("div");
  hint.className = "hint";
  hint.textContent = "Following a link carries the list along.";
  serversEl.appendChild(hint);
}

document.getElementById("group-servers").addEventListener("toggle", (e) => {
  if (e.target.open) drawServers();
});
drawServers();
