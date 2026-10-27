#!/usr/bin/env bash
# Real-interval scenarios (140 BPM / 32 BPI = 13.7 s): a user's channels and its
# video stay on the interval they were recorded in (39, 37). Hidden from `make test`
# because each case takes 5-8 minutes; the cases run in parallel, one server each.
# Run before a core release: `make test-real`. Logs in $NJ_TEST_LOG_DIR (default
# ./real-logs); NJ_REAL_ONLY="vm9 hm3" runs those cases only. Needs the nz-netem image for the jitter cases (built on first use).
set -uo pipefail
cd "$(dirname "$0")"
LOGDIR="${NJ_TEST_LOG_DIR:-real-logs}"
mkdir -p "$LOGDIR"
docker image inspect nz-netem:latest >/dev/null 2>&1 ||
  docker build -q -t nz-netem:latest -f docker/netem.Dockerfile docker >/dev/null

JITTER="delay 120ms 80ms distribution normal"
# name | test case | env (plan, receiver phase in ms, ...) | netem
CASES=(
  "vm3|39_multichannel_align|NJ_MC_PLAN=vcmute NJ_REAL_PHASE_MS=3000|"
  "vm7|39_multichannel_align|NJ_MC_PLAN=vcmute NJ_REAL_PHASE_MS=7000|"
  "vm9|39_multichannel_align|NJ_MC_PLAN=vcmute NJ_REAL_PHASE_MS=9000|"
  "vm11|39_multichannel_align|NJ_MC_PLAN=vcmute NJ_REAL_PHASE_MS=11000|"
  "vl9|39_multichannel_align|NJ_MC_PLAN=vcloud NJ_REAL_PHASE_MS=9000|"
  "vc7|39_multichannel_align|NJ_MC_PLAN=vc NJ_REAL_PHASE_MS=7000|"
  "mu7|39_multichannel_align|NJ_MC_PLAN=mute NJ_REAL_PHASE_MS=7000|"
  "mj0|39_multichannel_align|NJ_MC_PLAN=mute NJ_REAL_PHASE_MS=300|$JITTER"
  "mj7|39_multichannel_align|NJ_MC_PLAN=mute NJ_REAL_PHASE_MS=7000|$JITTER"
  "mx9|39_multichannel_align|NJ_MC_PLAN=mix NJ_REAL_PHASE_MS=9000|"
  "ul9|39_multichannel_align|NJ_MC_PLAN=vcloud NJ_MC_UNTAGGED=1 NJ_REAL_PHASE_MS=9000|"
  "fl3|39_multichannel_align|NJ_MC_PLAN=fill NJ_REAL_PHASE_MS=3000|"
  "fl7|39_multichannel_align|NJ_MC_PLAN=fill NJ_REAL_PHASE_MS=7000|"
  "fl11|39_multichannel_align|NJ_MC_PLAN=fill NJ_REAL_PHASE_MS=11000|"
  "fv7|39_multichannel_align|NJ_MC_PLAN=fillvc NJ_REAL_PHASE_MS=7000|"
  "fv11|39_multichannel_align|NJ_MC_PLAN=fillvc NJ_REAL_PHASE_MS=11000|"
  "hm3|37_no_voice_chat_jitter|NJ_CH0_MUTED=1 NJ_REAL_PHASE_MS=300|"
)

run_case() {
  local name="$1" tc="$2" envs="$3" netem="$4"
  local out
  out=$(NJ_TEST_NAME="nzs-real-$name-$$" NJ_TEST_CFG=test-multi.cfg ./docker/run-server.sh) || return 1
  local cname="${out%% *}" port="${out##* }"
  if [[ -n "$netem" ]]; then
    docker run --rm --net "container:$cname" --cap-add NET_ADMIN nz-netem:latest \
      tc qdisc add dev eth0 root netem $netem || { docker rm -f "$cname" >/dev/null; return 1; }
  fi
  env $envs NJ_TEST_HOST=127.0.0.1 NJ_TEST_PORT="$port" NJ_TEST_INTERVAL_MS=13714 \
    NJ_REAL_PREFIX="r$name" ./video_sync_tests "$tc" > "$LOGDIR/$name.log" 2>&1
  local rc=$?
  docker rm -f "$cname" >/dev/null 2>&1
  return $rc
}

# Ctrl-C or a failure: remove this run's servers (named nzs-real-<case>-<pid>).
cleanup() { docker ps -q --filter "name=^nzs-real-.*-$$\$" | xargs -r docker rm -f >/dev/null 2>&1; }
trap cleanup EXIT
trap 'exit 130' INT TERM

pids=()
for c in "${CASES[@]}"; do
  IFS='|' read -r name tc envs netem <<< "$c"
  [[ -n "${NJ_REAL_ONLY:-}" && " $NJ_REAL_ONLY " != *" $name "* ]] && continue
  run_case "$name" "$tc" "$envs" "$netem" &
  pids+=("$!:$name")
done
fail=0
for p in "${pids[@]}"; do
  if wait "${p%%:*}"; then echo "[real] ${p#*:} passed"; else echo "[real] ${p#*:} FAILED ($LOGDIR/${p#*:}.log)"; fail=1; fi
done
grep -h "misaligned\|content tags" "$LOGDIR"/*.log
exit $fail
