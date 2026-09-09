/*
 * Bottom Screen -- the preamble: elements, protocol numbers, and the
 * things every other file needs to exist before it runs.
 *
 * Plain scripts in one shared scope, not modules, loaded in the
 * order index.html lists them. The page is one program built around
 * one set of elements and one connection; splitting it into modules
 * would mean exporting almost everything to almost everything else,
 * which is a lot of ceremony to describe a single program.
 */
"use strict";

/*
 * The same stream the phone and the Switch get, decoded by the browser.
 *
 * No transcoding and no WebRTC: the server already produces H.264, and
 * WebCodecs decodes exactly those bytes -- in hardware where the machine
 * has it. Re-encoding to VP8 for a browser would mean encoding the
 * picture a second time, in software, on the machine that is already
 * running an emulator.
 */

/*
 * SET_QUALITY was missing from this table while the code below used
 * MSG.SET_QUALITY, so every quality change sent a message with an
 * undefined type and the server dropped it without a word. The picture
 * never changed because nothing was ever asked. Adding a name here and
 * using it there are two edits, and only one of them was made.
 */
const MSG = { VIDEO: 1, AUDIO: 2, STREAM_INFO: 3, SCREENS: 24,
              INPUT: 16, PING: 17, PONG: 18, REQUEST_KEYFRAME: 19,
              SET_QUALITY: 20, SET_SIZE: 21, SET_AUDIO_SOURCE: 22,
              SET_SCREEN: 23, PROMPT: 25, PROMPT_REPLY: 26 };
const INPUT = { TOUCH_DOWN: 1, TOUCH_MOVE: 2, TOUCH_UP: 3,
                BUTTON_DOWN: 4, BUTTON_UP: 5, AXIS: 6 };
const BTN = { A:1, B:2, X:3, Y:4, L:5, R:6, ZL:7, ZR:8,
              START:9, SELECT:10, UP:11, DOWN:12, LEFT:13, RIGHT:14, HOME:15 };

/* The button set each console actually has. Drawing a ZL for a DS would
 * be offering a control that goes nowhere. */
/*
 * The same three profiles the Android client carries, which is the
 * reference: a 3DS has ZL and ZR and two sticks, a Wii U has a HOME
 * button and two sticks, and neither was drawn here. Offering a control
 * the machine has not got is one fault; leaving out one it has is the
 * worse one, because there is then no way to press it at all.
 */
const CONSOLES = {
  1: { name: "Nintendo DS",  shoulders: ["L","R"],
       menu: ["SELECT","START"], sticks: [] },
  2: { name: "Nintendo 3DS", shoulders: ["L","R","ZL","ZR"],
       menu: ["SELECT","START"],
       sticks: [{ label: "\u25CB", x: 1, y: 2, left: true },
                { label: "C",       x: 3, y: 4, left: false }] },
  3: { name: "Wii U",        shoulders: ["L","R","ZL","ZR"],
       menu: ["SELECT","START","HOME"],
       sticks: [{ label: "L", x: 1, y: 2, left: true },
                { label: "R", x: 3, y: 4, left: false }] },
};

/* Full words now that the buttons are wide enough to hold them. */
const MENU_LABEL = { SELECT: "SELECT", START: "START", HOME: "HOME" };

const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d", { alpha: false });
const statusEl = document.getElementById("info");
const warnEl = document.getElementById("warn");
const padLeftEl = document.getElementById("padLeft");
const padRightEl = document.getElementById("padRight");
const barEl = document.getElementById("bar");

let socket = null, decoder = null;
let width = 256, height = 192, console_id = 1;
let frames = 0, decoded = 0, lastReport = 0, chosenCodec = "";
let audioRate = 0, audioChannels = 0, audioDecoder = null, audioCtx = null;
let audioFrames = 0, nextAudioAt = 0, audioPeak = 0, gain = null;
let audioLead = 0;
/*
 * How far ahead of the clock the audio schedule may run.
 *
 * The old code corrected one direction only: falling behind reset the
 * schedule, but nothing stopped it running ahead. A stall followed by a
 * rush of packets got scheduled back to back into the future, and every
 * packet after it inherited that lead -- so one hiccup became a delay
 * that never went away. Correcting both ends costs a click where the
 * backlog is dropped; carrying it costs the whole point of the project.
 */
const AUDIO_LEAD = 0.04;
const AUDIO_MAX_LEAD = 0.15;
/* The size before anybody asked for less, so the fractions below mean
 * fractions of what the emulator renders rather than of each other. */
let fullW = 0, fullH = 0;
let sequence = 0;

function say(text) { statusEl.textContent = text; }
/*
 * Something went wrong and stays wrong. It gets its own place beside the
 * counters rather than replacing them: a frame counter ticking over an
 * error message hides the only thing worth reading, and an error that
 * erases the counters hides how far it got.
 */
function problem(text) { warnEl.textContent = text; }
function consoleName() { return (CONSOLES[console_id] || CONSOLES[1]).name; }
