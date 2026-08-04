#!/bin/sh
# ema-pelt-trace: (a) samples /proc/<pid>/infinity across a sustained
# single burn and verifies the EMA-vs-PELT divergence stays within 50
# percentage points -- a sustained-burn task must NOT be flagged as
# divergent (guards against false-positive divergence bugs); (b) if root
# and perf are available, records sched wakeups/switches and reports the
# wakeup-to-run P50/P99 latency histogram.

# constants
BURN_SECS=20              # sustained single-burn duration (s)
DIVERGENCE_PP=50          # |ema_pct - util_pct| threshold
P99_FLOOR=500             # us, sanity floor

INF_FILE=/proc/self/infinity

# --- (a) EMA vs PELT divergence ------------------------------------------
# child: sustained single burn -- one continuous CPU spin for ~BURN_SECS.
# EMA and PELT both converge to ~100% within ~200ms, so every sample
# lands within the 50pp threshold and the gate deterministically passes.
(
	end=$(( $(date +%s 2>/dev/null || date +%s) + BURN_SECS ))
	while [ "$(date +%s 2>/dev/null || date +%s)" -lt "$end" ]; do
		j=0
		while [ "$j" -lt 1000000 ]; do j=$((j + 1)); done
	done
) &
child=$!

sleep 1
diverged=0
samples=0
while kill -0 "$child" 2>/dev/null; do
	# sample /proc/<child>/infinity (self file of the child path is not
	# usable cross-process; use the child pid's file)
	ema=$(awk -F'\t' '/^ema:/{print $NF}' /proc/$child/infinity 2>/dev/null)
	util=$(awk -F'\t' '/^util_avg:/{print $NF}' /proc/$child/infinity 2>/dev/null)
	if [ -n "$ema" ] && [ -n "$util" ]; then
		ema_pct=$(( ema * 100 / 6000000 ))
		util_pct=$(( util * 100 / 1024 ))
		diff=$(( ema_pct - util_pct ))
		[ "$diff" -lt 0 ] && diff=$(( -diff ))
		samples=$((samples + 1))
		[ "$diff" -gt "$DIVERGENCE_PP" ] && diverged=$((diverged + 1))
	fi
	sleep 0.2
done
wait "$child" 2>/dev/null

if [ "$samples" -eq 0 ]; then
	echo "RESULT ema-pelt-trace WARN no-samples 0 /proc/<pid>/infinity unavailable (stock kernel?)"
	diverged=0
elif [ "$diverged" -gt 0 ]; then
	echo "RESULT ema-pelt-trace FAIL diverged "$diverged"/"$samples" <=$DIVERGENCE_PP pp"
	exit 1
else
	echo "RESULT ema-pelt-trace PASS diverged 0/$samples <=$DIVERGENCE_PP pp"
fi

# --- (b) wakeup latency histogram (needs root + perf) ---------------------
[ "$(id -u)" -eq 0 ] || exit 0
command -v perf >/dev/null 2>&1 || exit 0

perf record -a -e sched:sched_wakeup,sched:sched_switch -o /tmp/ema-pelt-perf.data \
	-- sleep 10 >/dev/null 2>&1 || { echo "RESULT ema-pelt-trace WARN perf-failed 0"; exit 0; }

perf script -i /tmp/ema-pelt-perf.data 2>/dev/null | awk '
	/wakeup/ { split($0,a,"pid="); split(a[2],b,","); wake[b[1]]=$4 }
	/switch/ { split($0,a,"next_pid="); split(a[2],b,","); n=b[1];
		   if (wake[n] != "") { lat[n]=$4-wake[n]; wake[n]=""; cnt++ } }
	END { for (k in lat) print lat[k] > "/tmp/ema-pelt-lats.txt"; print cnt }
' > /tmp/ema-pelt-count.txt

cnt=$(cat /tmp/ema-pelt-count.txt)
if [ -z "$cnt" ] || [ "$cnt" -eq 0 ]; then
	echo "RESULT ema-pelt-trace WARN no-traces 0 perf-record-empty"
	exit 0
fi

sort -n /tmp/ema-pelt-lats.txt > /tmp/ema-pelt-sorted.txt
total=$(wc -l < /tmp/ema-pelt-sorted.txt)
p50=$(awk -v n="$total" 'NR==int(n*0.5)+1{print $1; exit}' /tmp/ema-pelt-sorted.txt)
p99=$(awk -v n="$total" 'NR==int(n*0.99)+1{print $1; exit}' /tmp/ema-pelt-sorted.txt)
rm -f /tmp/ema-pelt-perf.data /tmp/ema-pelt-lats.txt /tmp/ema-pelt-sorted.txt /tmp/ema-pelt-count.txt

if [ "${p99:-999999}" -lt "$P99_FLOOR" ]; then
	echo "RESULT ema-pelt-trace PASS wake-p99-us "$p99" <"$P99_FLOOR" (p50 "$p50", n "$total")"
else
	echo "RESULT ema-pelt-trace FAIL wake-p99-us "$p99" <"$P99_FLOOR" (p50 "$p50", n "$total")"
	exit 1
fi
