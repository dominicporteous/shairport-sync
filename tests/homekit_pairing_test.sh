#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
MRP_TEST_SOURCE=tests/homekit_pairing_test.c sh tests/mrp_channel_test.sh "${1:-build}"
