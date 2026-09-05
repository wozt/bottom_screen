#!/bin/sh
# Launches one of the three emulators from this tree.
#
# They live inside emulators/ rather than being installed system-wide,
# so a system package of the same name cannot be picked up by mistake --
# only melonDS carries the bottom_screen patches, and running the
# distribution's copy instead would silently stream nothing.
#
#   ./scripts/run-emulator.sh melonds [rom]
#   ./scripts/run-emulator.sh azahar  [rom]
#   ./scripts/run-emulator.sh cemu    [game]
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

case "$1" in
  melonds) BIN="$DIR/emulators/melonDS/build/melonDS" ;;
  azahar)  BIN="$DIR/emulators/azahar/build/bin/Release/azahar" ;;
  cemu)    BIN="$DIR/emulators/Cemu/build/bin/Cemu_release" ;;
  *) echo "usage: $0 melonds|azahar|cemu [fichier]" >&2; exit 1 ;;
esac
shift

if [ ! -x "$BIN" ]; then
    echo "pas encore compilé : $BIN" >&2
    exit 1
fi

exec "$BIN" "$@"
