#!/bin/sh
# socket-latency: netperf TCP_RR covers the P0-1 wait_woken/socket path
# (hackbench uses pipes and exercises a different wait path). Skips
# gracefully when netperf is not installed.

command -v netperf >/dev/null 2>&1 || { echo "RESULT socket-latency WARN missing netperf 0"; exit 0; }
command -v netserver >/dev/null 2>&1 || { echo "RESULT socket-latency WARN missing netserver 0"; exit 0; }

netserver >/dev/null 2>&1 &
srv=$!
sleep 1

out=$(netperf -t TCP_RR -l 10 2>&1)
kill "$srv" 2>/dev/null
wait "$srv" 2>/dev/null

# netperf TCP_RR reports transactions/s in the last column of the result row
rate=$(printf '%s\n' "$out" | awk 'NR>5 && $1 ~ /^[0-9]/ {print $NF; exit}')
rate=${rate%%.*}   # netperf prints decimals; [ -gt ] needs an integer

if [ -z "$rate" ]; then
	echo "RESULT socket-latency FAIL no-rate 0 netperf-parse-error"
	exit 1
fi
# sanity floor: 1k trans/s is far below any real machine; deltas are the signal
if [ "$rate" -gt 1000 ]; then
	echo "RESULT socket-latency PASS trans-s "$rate" >1000"
else
	echo "RESULT socket-latency FAIL trans-s "$rate" >1000"
	exit 1
fi
