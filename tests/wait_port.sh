#!/bin/sh
# Prints the port a just-started server announced, or fails.
# Usage: wait_port.sh <server log> <server pid>
LOG=$1
PID=$2
i=0
while [ "$i" -lt 100 ]; do
    PORT=$(sed -n 's/.*listening on port \([0-9][0-9]*\).*/\1/p' "$LOG" | head -1)
    if [ -n "$PORT" ]; then
        echo "$PORT"
        exit 0
    fi
    kill -0 "$PID" 2>/dev/null || break
    i=$((i + 1))
    # No sleep binary assumptions: read with a timeout idles just as well.
    read -t 0.1 _ignored < /dev/null 2>/dev/null || :
done
echo "le serveur n'a jamais annonce de port" >&2
exit 1
