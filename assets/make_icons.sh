#!/bin/sh
# Regenerates every icon from assets/icon.svg.
#
# One master, many outputs: an icon redrawn by hand per size drifts, and
# the drift shows up as a launcher icon that does not match the one in
# the settings list.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SVG="$DIR/assets/icon.svg"
RES="$DIR/android/app/src/main/res"
BG="#12161C"

# Adaptive icon foreground: full 108-unit canvas, launcher applies the mask.
for entry in mdpi:108 hdpi:162 xhdpi:216 xxhdpi:324 xxxhdpi:432; do
    d=${entry%%:*}; px=${entry##*:}
    mkdir -p "$RES/mipmap-$d"
    rsvg-convert -w "$px" -h "$px" "$SVG" -o "$RES/mipmap-$d/ic_launcher_foreground.png"
done

# Legacy square icon for launchers older than adaptive icons: the same
# art composited on the background colour, inset so it is not flush to
# the edge the way the adaptive foreground is.
for entry in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    d=${entry%%:*}; px=${entry##*:}
    rsvg-convert -w "$px" -h "$px" "$SVG" -o "/dev/shm/bs_icon_$px.png"
    magick -size "${px}x${px}" "xc:$BG" "/dev/shm/bs_icon_$px.png" \
        -gravity center -composite \
        "$RES/mipmap-$d/ic_launcher.png"
    cp "$RES/mipmap-$d/ic_launcher.png" "$RES/mipmap-$d/ic_launcher_round.png"
    rm -f "/dev/shm/bs_icon_$px.png"
done

# A big one for anything that wants artwork rather than a launcher icon.
rsvg-convert -w 512 -h 512 "$SVG" -o "$DIR/assets/icon-512.png"

# The Switch homebrew menu, which wants exactly 256x256 JPEG. No alpha
# in a JPEG, so the same background the launcher icons use is painted
# behind it rather than left to whatever the menu happens to show.
rsvg-convert -w 256 -h 256 "$SVG" -o "/dev/shm/bs_icon_switch.png"
magick -size 256x256 "xc:$BG" "/dev/shm/bs_icon_switch.png" \
    -gravity center -composite -quality 92 "$DIR/switch/icon.jpg"
rm -f "/dev/shm/bs_icon_switch.png"

echo "icons regenerated from $SVG"
