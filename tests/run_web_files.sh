#!/bin/sh
# The browser's files, over HTTP, exactly as a browser would ask.
#
# The page used to be one index.html with its stylesheet and fifty
# kilobytes of script inside it, so there was nothing to get wrong:
# whatever was served was the whole client. Now it is eleven files, and
# a broken split is invisible everywhere else -- the WebSocket tests
# never fetch them, the emulators only embed them, and the failure shows
# up as a blank page in a browser with a message in a console nobody has
# open.
#
# So: every file the page asks for is fetched and checked, and every
# script is parsed. Parsing is the part that earns its keep, because the
# files share one scope in a fixed order -- a brace left on the wrong
# side of a cut is a syntax error in one file and a page that does
# nothing at all.
set -e

if [ -z "$TMPDIR" ] && [ -d /dev/shm ]; then
    TMPDIR=/dev/shm
    export TMPDIR
fi

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
command -v curl >/dev/null || { echo "SKIP WEB FILES (no curl)"; exit 0; }

OUT=$(mktemp -d)
"$DIR/bottom_screen_server" --console ds --port "${PORT:-5096}" \
    >"$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$OUT"' EXIT

P=$("$DIR/tests/wait_port.sh" "$OUT/server.log" "$SERVER_PID") || {
    cat "$OUT/server.log"; exit 1; }

fail=0
get() {
    curl -s -D "$OUT/head" "http://127.0.0.1:$P$1" -o "$OUT/body"
}
type_of() {
    grep -i "^content-type:" "$OUT/head" | tr -d '\r' | cut -d' ' -f2
}

# --- the page, and the scripts it names --------------------------------
get / || { echo "  the page could not be fetched"; exit 1; }
cp "$OUT/body" "$OUT/index.html"
case "$(type_of)" in
    text/html*) ;;
    *) echo "  the page came back as $(type_of)"; fail=1 ;;
esac

scripts=$(grep -oE 'src="/[a-z]+\.js"' "$OUT/index.html" |
          sed 's/src="\///;s/"//' | tr '\n' ' ')
[ -n "$scripts" ] || { echo "  the page names no scripts at all"; fail=1; }
echo "  the page asks for: $scripts"

# --- each script: served, typed, and parseable -------------------------
have_node=0
command -v node >/dev/null && have_node=1

for js in $scripts; do
    get "/$js" || { echo "  /$js could not be fetched"; fail=1; continue; }
    size=$(wc -c < "$OUT/body")
    case "$(type_of)" in
        text/javascript*) ;;
        *) echo "  /$js came back as $(type_of)"; fail=1 ;;
    esac
    if [ "$size" -lt 100 ]; then
        echo "  /$js is only $size bytes"
        fail=1
        continue
    fi
    if [ "$have_node" = 1 ]; then
        if ! node --check "$OUT/body" 2>"$OUT/err"; then
            echo "  /$js does not parse:"
            head -3 "$OUT/err" | sed 's/^/    /'
            fail=1
        fi
    fi
done
[ "$have_node" = 1 ] || echo "  (no node, so the scripts were not parsed)"

# --- the stylesheet ----------------------------------------------------
get /app.css
case "$(type_of)" in
    text/css*) ;;
    *) echo "  /app.css came back as $(type_of)"; fail=1 ;;
esac
grep -q "#bar" "$OUT/body" || { echo "  /app.css is not the stylesheet"; fail=1; }

# --- and a path nobody defined -----------------------------------------
#
# The page, not an error: a browser asks for /favicon.ico on its own,
# and a 404 body is something it will happily render if it ever ends up
# in a frame.
get /favicon.ico
case "$(type_of)" in
    text/html*) ;;
    *) echo "  an unknown path came back as $(type_of)"; fail=1 ;;
esac

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
