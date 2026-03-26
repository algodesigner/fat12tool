#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMG_SRC="$ROOT_DIR/sample-fat12-p1.img"

if [ ! -f "$IMG_SRC" ]; then
  echo "Missing fixture: $IMG_SRC" >&2
  exit 1
fi

TMP_IMG="fat12-cli-test-$$.img"
cp "$IMG_SRC" "$TMP_IMG"

cleanup() {
  rm -f "$TMP_IMG" "fat12-cli-host-src-$$.txt" "fat12-cli-host-out-$$.txt" "fat12-cli-out-$$.log"
}
trap cleanup EXIT INT TERM

HOST_SRC="fat12-cli-host-src-$$.txt"
HOST_OUT="fat12-cli-host-out-$$.txt"
LOG="fat12-cli-out-$$.log"

echo "from-host-write" > "$HOST_SRC"

{
  echo "pwd"
  echo "ls"
  echo "cat /HELLO.TXT"
  echo "touch /CLI.TXT"
  echo "write $HOST_SRC /CLI.TXT"
  echo "read /CLI.TXT $HOST_OUT"
  echo "mkdir /CLIDIR"
  echo "touch /CLIDIR/NEST.TXT"
  echo "rm /CLIDIR/NEST.TXT"
  echo "rmdir /CLIDIR"
  echo "stat /CLI.TXT"
  echo "exit"
} | "$ROOT_DIR/fat12tool" "$TMP_IMG" > "$LOG" 2>&1

grep -q "fat12:/>" "$LOG"
grep -q "hello from p1" "$LOG"
grep -q "name=CLI.TXT" "$LOG"

grep -q "from-host-write" "$HOST_OUT"

echo "PASS: fat12tool CLI test"
