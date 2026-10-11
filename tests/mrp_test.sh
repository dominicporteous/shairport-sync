#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
testdir=$(mktemp -d)
trap 'rm -rf "$testdir"' EXIT HUP INT TERM
${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:-} -I. tests/mrp_test.c mrp/state.c mrp/messages.pb-c.c \
  $(pkg-config --cflags --libs libplist-2.0 libprotobuf-c) ${LDFLAGS:-} -pthread -lm \
  -o "$testdir/mrp_test"
"$testdir/mrp_test"
