#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMG_SRC="$ROOT_DIR/sample-fat12-p1.img"
FAT12MOUNT="$ROOT_DIR/fat12mount"

OS=$(uname -s)
UNMOUNT_CMD=""
if [ "$OS" = "Linux" ]; then
  UNMOUNT_CMD="fusermount3 -u"
elif [ "$OS" = "Darwin" ]; then
  UNMOUNT_CMD="umount"
elif echo "$OS" | grep -q "MINGW\|MSYS"; then
  OS="Windows"
  FAT12MOUNT="$ROOT_DIR/fat12mount.exe"
  UNMOUNT_CMD="$FAT12MOUNT -u"
fi

# Use unique but descriptive mount point
MNT_DIR="$ROOT_DIR/mnt-p1-test-$$"
LOG="fat12-fuse-p1-$$.log"
SUCCESS=0

cleanup() {
  if [ "$OS" = "Windows" ]; then
    $UNMOUNT_CMD "$MNT_DIR" >/dev/null 2>&1 || true
    sleep 1
    taskkill //F //IM fat12mount.exe >/dev/null 2>&1 || true
  else
    if mount | grep -q "on $MNT_DIR "; then
      $UNMOUNT_CMD "$MNT_DIR" >/dev/null 2>&1 || true
    fi
  fi
  if [ "$SUCCESS" -eq 1 ]; then
    rm -f "$LOG"
  else
    echo "Log preserved at $LOG"
  fi
  rmdir "$MNT_DIR" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# Ensure fresh start
[ "$OS" = "Windows" ] || mkdir -p "$MNT_DIR"

EXTRA_FUSE_ARGS=""
if [ "$OS" = "Windows" ]; then
  EXTRA_FUSE_ARGS="-f" # Stay in foreground for reliable PID/tracking
fi

"$FAT12MOUNT" --image "$IMG_SRC" --mount "$MNT_DIR" $EXTRA_FUSE_ARGS >"$LOG" 2>&1 &
MOUNT_PID=$!

mounted=0
i=0
while [ "$i" -lt 50 ]; do
  if ls "$MNT_DIR" 2>/dev/null | grep -q "HELLO.TXT"; then
    mounted=1
    break
  fi
  if ! kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
  i=$((i + 1))
done

if [ "$mounted" -ne 1 ]; then
  echo "Mount did not become ready" >&2
  cat "$LOG" >&2
  exit 1
fi

# Verify content
grep -q "hello from p1" "$MNT_DIR/HELLO.TXT"

SUCCESS=1
echo "PASS: fat12mount FUSE integration test"
