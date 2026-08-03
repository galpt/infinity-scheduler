#!/bin/sh
# fork: process-spawn throughput. Spawns TARGET subshells and times the
# spawn+exit round trip with date +%s%N (ns) when supported, else the
# POSIX seconds fallback. Deterministic and dependency-free.

# constants
FORK_FLOOR=2000           # forks/s, sanity floor
TARGET=20000

start=$(date +%s%N 2>/dev/null)
if [ -n "$start" ] && [ "$start" -gt 1000000000 ]; then
	n=0
	while [ "$n" -lt "$TARGET" ]; do
		( : ) &
		n=$((n + 1))
	done
	wait
	end=$(date +%s%N)
	ms=$(( (end - start) / 1000000 ))
	[ "$ms" -lt 1 ] && ms=1
	rate=$(( TARGET * 1000 / ms ))
else
	start=$(date +%s)
	n=0
	while [ "$n" -lt "$TARGET" ]; do
		( : ) &
		n=$((n + 1))
	done
	wait
	end=$(date +%s)
	[ "$end" -gt "$start" ] || end=$((start + 1))
	rate=$(( TARGET / (end - start) ))
fi

if [ "$rate" -gt "$FORK_FLOOR" ]; then
	echo "RESULT fork PASS forks-s "$rate" >"$FORK_FLOOR
else
	echo "RESULT fork FAIL forks-s "$rate" >"$FORK_FLOOR
	exit 1
fi
