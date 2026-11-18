#!/bin/sh
# Replays dumps into a NEW directory and compares decoded pixels and the exact frame set.
# Older-build references prove regression stability, not hardware correctness.
#   compare.sh <refdir> <outdir> <dump>...
set -eu
here="$(dirname "$0")"
replay="${REPLAY:-$here/build/gpu_replay}"
exec "${PYTHON:-python3}" "$here/compare.py" "$@" --replay "$replay"
