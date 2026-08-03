#!/bin/sh
# wakeup-latency: schbench wakeup latency percentile, 3 iterations.
# The sanity floor is loose; the real signal is the delta against the
# v4.7-gpu baseline run with the same method.

# constants
THREADS=8
ITERATIONS=3
# Sanity floor only: a loaded desktop (browser + compositor + editors) can
# push schbench P99 into the low ms range on any kernel; the real signal is
# the delta against the v4.7-gpu baseline with the same method.
P99_FLOOR=3000            # us

command -v schbench >/dev/null 2>&1 || { echo "RESULT wakeup-latency WARN missing schbench 0"; exit 0; }

best=999999
worst=0
i=0
while [ "$i" -lt "$ITERATIONS" ]; do
	out=$(schbench -m 2 -t "$THREADS" -r 10 2>&1)
	p99=$(printf '%s\n' "$out" | awk '/Wakeup Latencies/{w=1} w && /99\.0th:/{print $3; exit}')
	[ -z "$p99" ] && { echo "RESULT wakeup-latency FAIL no-p99 0 schbench-parse-error"; exit 1; }
	[ "$p99" -lt "$best" ] && best=$p99
	[ "$p99" -gt "$worst" ] && worst=$p99
	i=$((i + 1))
done

echo "RESULT wakeup-latency $( [ "$best" -lt "$P99_FLOOR" ] && echo PASS || echo FAIL ) p99-min-us "$best" <"$P99_FLOOR" (worst "$worst")"
[ "$best" -lt "$P99_FLOOR" ] || exit 1
