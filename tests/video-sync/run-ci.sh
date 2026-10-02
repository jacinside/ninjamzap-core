#!/usr/bin/env bash
# CI entry point: runs the full suite, then reruns each failing scenario alone
# (twice). Several scenarios assert on wall-clock timing and occasionally fail
# under a loaded runner; a failure only counts if it reproduces in isolation.
# Flaky scenarios are reported but do not fail the job.
#
# Usage: ./run-ci.sh [extra Catch2 args for the full run]
set -uo pipefail
cd "$(dirname "$0")"

LOG="${NJ_TEST_LOG:-video-sync.log}"
make test TEST_ARGS="$*" 2>&1 | tee "$LOG"
status=${PIPESTATUS[0]}
[[ $status -eq 0 ]] && exit 0

failed_files=$(grep -oE 'scenarios/[0-9]+_[a-z0-9_]+\.cpp:[0-9]+: FAILED' "$LOG" \
               | cut -d: -f1 | sort -u)
if [[ -z "$failed_files" ]]; then
  echo "::error::suite failed without a scenario failure (build/server problem)"
  exit "$status"
fi

real=()
flaky=()
for file in $failed_files; do
  tag=$(grep -m1 -oE '\[scenario[0-9]+\]' "$file")
  ok=1
  for attempt in 1 2; do
    echo "[run-ci] rerun $tag ($file) attempt $attempt"
    if ! make test TEST_ARGS="\"$tag\"" > "rerun-${tag//[^a-z0-9]/}-$attempt.log" 2>&1; then
      ok=0
      break
    fi
  done
  if [[ $ok -eq 1 ]]; then flaky+=("$tag $file"); else real+=("$tag $file"); fi
done

for f in "${flaky[@]}"; do echo "::warning::flaky (passed alone): $f"; done
for f in "${real[@]}"; do echo "::error::reproduces in isolation: $f"; done
[[ ${#real[@]} -eq 0 ]]
