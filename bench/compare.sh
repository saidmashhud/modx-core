#!/bin/sh
# Benchmark several revisions back to back under the same conditions: each
# revision's examples/todo server is built in a temporary worktree and driven
# by the current bench/run.mjs + loadgen.
#   bench/compare.sh [profile] name=rev [name=rev ...]
#   bench/compare.sh full 0-baseline=227586f 7-final=HEAD
# Results land in bench/results/<prefix><name>.json (prefix: $BENCH_PREFIX).
set -eu
profile=$1
shift
root=$(cd "$(dirname "$0")/.." && pwd)
make -s -C "$root" build/loadgen
for pair in "$@"; do
  name=${pair%%=*}
  rev=${pair#*=}
  wt=$(mktemp -d)
  git -C "$root" worktree add -q --detach "$wt" "$rev"
  nice -n 15 make -s -C "$wt" build/todo
  echo "== $name ($rev)"
  nice -n 15 node "$root/bench/run.mjs" --profile "$profile" --label "${BENCH_PREFIX:-}$name" \
    --todo "$wt/build/todo" --loadgen "$root/build/loadgen" ${BENCH_ARGS:-}
  git -C "$root" worktree remove --force "$wt"
  sleep "${BENCH_PAUSE:-30}"
done
