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

### 1. Android: the geometry does not follow the screen  *(blocking)*

In 3DS mode the top screen does not take the room it should and is not
drawn at the 3DS's real ratio. It does not correct itself when the
screen is changed, and it is worse across a rotation.

### 2. Android: the pad is wrong at 16:9  *(blocking)*

On both Wii U screens and the 3DS top screen -- the wide ones -- the
controls sit too high on the right: ZL, L, the left stick and the d-pad
are all pushed up. The gravity that decides where a control settles does
not react to the band changing width.

Two spacing faults come with it:

- an abnormally large gap between the d-pad and the face buttons,
- no gap at all between the settings button and ZL.

### 3. Switch: coming back from the top screen freezes  *(blocking)*

Going to the top screen from the menu works. Coming back does not: the
last top picture stays on screen.

Cause, believed: the client is not told the bottom screen's shape when
it returns, so it never rebuilds its decoder, and the console decodes in
hardware, which does not follow a resolution change. The server-side fix
for this exists but `bs_server.c` is compiled into each emulator, so the
emulators have to be rebuilt for it to take effect. Not yet confirmed on
the console.

### 4. The Wii U top screen offers no sizes  *(functional)*

Its picture is capped at 1024x768 and the size control offers no
multiples of the top screen's own 1280x720 -- only "whatever is
rendered". Both the Android client and the Switch are affected.

The ladder is built from what the emulator actually renders, so if
Cemu's television readback comes back smaller than 1280x720 there are no
rungs to offer. To be found out rather than assumed.

### 5. Cemu will not serve until its GamePad window is open  *(functional)*

The server is started from the GamePad view's own path, so nothing is
served until that window exists. It should not be a condition.

## Wanted

### 6. Separate menus for the two screens

In all three clients, the top and the bottom screen get their own
section, each with its own quality and internal resolution -- they are
separate encoders, so these are genuinely separate settings. Anything
that is shared between the two stays shared and is not duplicated.

### 7. An Xbox-shaped d-pad

DS, 3DS and Wii U alike: a cross inside a circle, with the diagonals
handled in the circle's diagonals rather than at the arms' corners.

## Testing

### 8. A run through every mode

Six combinations -- DS, 3DS and Wii U, each top and bottom -- against
the real emulators.

**The emulators, and therefore the servers, are restarted between
tests.** `bs_server.c` is linked into each emulator, so a server left
running from before a change is the old code wearing the new binary's
name, and it produces a pass that means nothing.

## Working

Kept here so it is clear what must not regress.

- DS, both screens, switching either way: correct.
- Cemu, both screens, switching either way: correct, subject to item 4.
- Azahar serves both screens; the browser and the Android client both
  show them.
