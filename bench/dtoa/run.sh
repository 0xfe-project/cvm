#!/bin/sh
#
# Build and run the f64 <-> text comparison.
#
# Both implementations are compiled into one binary, at the same optimisation
# level, and run in one process, so the two rows are comparable.
#
#   sh bench/dtoa/run.sh                 the default corpus
#   sh bench/dtoa/run.sh 1000000 4       a bigger corpus, fewer repeats
#
# See README.md in this directory for what the columns mean and what the
# numbers came out as.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cc=${CC:-cc}

qs="$root/refs/quickjs-0.17.0"
lj="$root/refs/json-5.0.3"

# Into bench/build/, which is ignored: a build product is not history.
out="$root/bench/build/dtoa/dtoa-bench"
mkdir -p "$(dirname "$out")"

"$cc" -O2 -std=c11 -o "$out" \
    -I "$root/rt" -I "$qs" -I "$lj" \
    "$here/dtoa-bench.c" \
    "$qs/dtoa.c" \
    "$lj/jnum.c"

exec "$out" "$@"
