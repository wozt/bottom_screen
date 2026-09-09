# The top screen: what is left to do

The second screen works end to end -- server, three clients, three
emulators. What follows is the list of things that are wrong with it, or
missing from it, written down because it is longer than a conversation
can hold.

Ordered by what blocks somebody using it, not by what is interesting.

## The rule everything here is measured against

One sentence, because every item below is a consequence of it:

> The picture takes the largest area it can without being distorted, the
> controls take what is left, both adapt the moment anything changes, and
> touch lands where it looks like it lands.

Three clients, one rule. Android first, because that is where it is
being used.

### What that means in practice

- **Largest undistorted area.** For every shape: 4:3 (DS, 3DS bottom),
  5:3 (3DS top), 16:9 (both Wii U screens). Never a ratio taken from the
  console when the screen on the wire has its own.
- **Touch at the true size.** Coordinates go in the space the server
  announced. When there is no touch panel -- the top screen -- there is
  no touch, and the client does not send any.
- **Controls fit the band they are given.** The side bands are whatever
  the picture leaves. They get narrower as the picture gets wider, so
  the pad shrinks into them rather than overlapping itself or hanging
  over the picture. The arrangement -- which control sits where, and
  which way it is pulled -- is kept; only the size gives.
- **Live.** Screen change, rotation, internal resolution moved in the
  emulator: all three are the same event, and none of them is a reason
  to reconnect or restart.

## Open faults

### 1. Android: the geometry does not follow the screen  ~~*(blocking)*~~

**Done.** The cause was in the server: the size is announced when it
changes, which says nothing to a client arriving on a screen somebody
else is already watching. That client kept the size from its handshake
-- the bottom screen's -- and drew a 5:3 picture as though it were 4:3.
It now gets told the shape of the screen it joins. Rotation keeps the
settings panel in front of the layout it rebuilds.

### 2. Android: the pad is wrong at 16:9  ~~*(blocking)*~~

**Done.** The unit came from one divisor chosen when a DS was the only
console. It is solved now: each band's demand is counted off the console
profile -- how tall its contents are, how wide the widest is, what is
already spoken for at the top -- and the unit is the largest at which
both bands still hold everything. Shoulders pin to the top, menus to the
bottom, the arm floats in what is left, and each stick is measured into
the gap on its own side. The right-hand shoulders no longer start as low
as the left-hand ones, which only gave room to a button that is not on
their side.

### 3. Switch: coming back from the top screen freezes  *(believed fixed)*

Going to the top screen from the menu works. Coming back does not: the
last top picture stays on screen.

Cause, believed: the client is not told the bottom screen's shape when
it returns, so it never rebuilds its decoder, and the console decodes in
hardware, which does not follow a resolution change. The server-side fix
for this exists but `bs_server.c` is compiled into each emulator, so the
emulators have to be rebuilt for it to take effect. Not yet confirmed on
the console.

### 4. The size ladder stopped short  ~~*(functional)*~~

**Done, and the ceiling reaches further than it looked.** Measured with
Azahar's internal resolution at six: the 3DS top screen streams at
2400x1440, which is six times its own 400x240 and exactly the ceiling.
Nothing in the chain was in the way -- the emulator was set to three, so
three was all there was to offer.

Two real faults were in the way of ever seeing it. The client recorded
what was rendered once, at the first connection, so after changing
screens the ladder was still capped by the screen that had been left.
And the cap was taken from the stream as it stands, which answers a
different question: choose one times native and the stream becomes
native, so the cap becomes native, so one times is the only rung left
and there is no way back up. Both clients now remember the largest
picture each screen has been seen to carry, which never shrinks while
the connection lasts.

Cemu's television picture measured 1920x1080; 1024x768 was melonDS at
four times a DS screen, not Cemu.

### 5. Cemu will not serve until its GamePad window is open  ~~*(functional)*~~

**Done.** Either view opens the server now, and only one of them counts
frames towards the rate -- both are drawn once per frame, so letting
both count would report twice the real one. A client watching the bottom
screen before that window exists sees nothing, which is the truth: Cemu
is not drawing it.

## Wanted

### 6. Separate menus for the two screens

**Done on Android**, still to do in the browser and on the Switch. Each
screen has its own quality and its own size, remembered separately and
sent when that screen is joined. Sound is not duplicated: there is one
set of speakers whatever is being watched.

### 7. An Xbox-shaped d-pad

**Done on Android**, still to do in the browser and on the Switch. Eight
sectors of the circle, so each direction gets the same 45 degrees and
the diagonals sit where the circle's diagonals are. The old two-threshold
square did produce diagonals, but only in its corners -- where the thumb
has already left the cross.

## Testing

### 8. A run through every mode

**Done**: `tests/run_emulator_matrix.sh`. Measured, all six: a DS is
1024x768 on both screens at four times native; Azahar is 320x240 below
and 1200x720 above, which is 5:3 exactly; Cemu is 854x480 below and
1920x1080 above.

**The emulators, and therefore the servers, are restarted between
tests.** `bs_server.c` is linked into each emulator, so a server left
running from before a change is the old code wearing the new binary's
name, and it produces a pass that means nothing.

### 9. The browser's menu gets out of the way

**Done.** The bar hides itself, and comes back only when the pointer is
in the top tenth of the window *and* over the black beside the picture
-- the picture is the touch area and belongs to the game. A 16:9 picture
filling a fullscreen window with the buttons turned off leaves no black
at all, so an eight-pixel corner works everywhere as the way back.

## Working

Kept here so it is clear what must not regress.

- All three emulators, all three clients, switching either way between
  the two screens: correct.
- The pad packs upward from the floor of its band, bounded by a thumb's
  reach: measured on a 2400x1080 phone, the top of the stack sits
  between 31% and 44% down, where it used to reach 11%.


