#!/bin/sh
#
# Build and run the JSON comparison.
#
# All three implementations are compiled into one binary and run in one process
# on one corpus, so the columns are comparable.
#
#   sh bench/json/run.sh            the default repeat count
#   sh bench/json/run.sh 10         10 print repeats
#
# See README.md in this directory for what the columns mean.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cc=${CC:-cc}

lj="$root/refs/json-5.0.3"
yy="$root/refs/yyjson-0.13.0/src"

# Into bench/build/, which is ignored: a build product is not history.
out="$root/bench/build/json/json-bench"
mkdir -p "$(dirname "$out")"

# The whole runtime (rt/ and vm/) plus the two libraries. The runtime is compiled
# as a hosted program here, which is what it is; its freestanding discipline is
# enforced by build.mjs, not by this.
"$cc" -O2 -std=c11 -o "$out" \
    -I "$root/rt" -I "$root/vm/include" -I "$root/vm" -I "$lj" -I "$yy" \
    "$here/json-bench.c" \
    $(find "$root/rt" -name '*.c') \
    $(find "$root/vm" -name '*.c') \
    "$lj/json.c" "$lj/jnum.c" \
    "$yy/yyjson.c" \
    -lpthread

exec "$out" "$@"
