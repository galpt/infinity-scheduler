#!/bin/sh
# v4.8 regression runner: executes every scenario with a per-scenario
# timeout, aggregates the results table, and exits 1 on any FAIL.
# Usage: run-v48-regression.sh   (as root for the perf/cyclictest parts)

cd "$(dirname "$0")" || exit 1

[ "$(id -u)" -eq 0 ] || echo "warning: not root; perf and cyclictest scenarios will skip"

uname -r | grep -q infinity || \
	echo "warning: kernel does not look like an infinity build ($(uname -r))"

SCENARIOS="alt-tab wakeup-latency socket-latency rt fork ema-pelt-trace"
TIMEOUT=300
RESULT_FILE=$(mktemp)
trap 'rm -f "$RESULT_FILE"' EXIT

for s in $SCENARIOS; do
	[ -x "scenarios/$s.sh" ] || { echo "RESULT $s FAIL script-missing 0 missing"; continue; }
	out=$(timeout $TIMEOUT "scenarios/$s.sh" 2>&1)
	rc=$?
	lines=$(printf '%s\n' "$out" | grep '^RESULT ')
	if [ -z "$lines" ]; then
		lines="RESULT $s $( [ "$rc" -eq 0 ] && echo WARN || echo FAIL ) no-result rc=$rc no-metric-line"
	fi
	printf '%s\n' "$lines" | tee -a "$RESULT_FILE"
done

echo
echo "Summary:"
cat "$RESULT_FILE" | awk '{printf "  %-16s %-5s %s\n", $2, $3, $4}'
grep -q ' FAIL ' "$RESULT_FILE" && exit 1
exit 0
