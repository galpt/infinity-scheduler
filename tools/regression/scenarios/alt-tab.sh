#!/bin/sh
# alt-tab: issue-#16 proxy. CPU hogs compete with a wakeup-latency probe;
# the interactive task must keep low wakeup latency under saturation.

# constants
HOGS=$(( $(nproc) - 2 ))   # leave headroom for the probe
[ "$HOGS" -lt 1 ] && HOGS=1   # small machines: keep at least one hog
RUNTIME=60
# Sanity floor only (see wakeup-latency.sh); deltas vs the v4.7 baseline
# are the real signal.
P99_FLOOR=10000            # us

command -v stress-ng >/dev/null 2>&1 || { echo "RESULT alt-tab WARN missing stress-ng 0"; exit 0; }
command -v schbench >/dev/null 2>&1 || { echo "RESULT alt-tab WARN missing schbench 0"; exit 0; }

stress-ng --cpu "$HOGS" --timeout "$RUNTIME" >/dev/null 2>&1 &
hog=$!
sleep 2

out=$(schbench -m 2 -t 8 -r "$RUNTIME" 2>&1)
kill "$hog" 2>/dev/null
wait "$hog" 2>/dev/null

p99=$(printf '%s\n' "$out" | awk '/Wakeup Latencies/{w=1} w && /99\.0th:/{print $3; exit}')

if [ -z "$p99" ]; then
	echo "RESULT alt-tab FAIL no-p99 0 schbench-parse-error"
	exit 1
fi
if [ "$p99" -lt "$P99_FLOOR" ]; then
	echo "RESULT alt-tab PASS p99-us "$p99" <"$P99_FLOOR
else
	echo "RESULT alt-tab FAIL p99-us "$p99" <"$P99_FLOOR
	exit 1
fi
