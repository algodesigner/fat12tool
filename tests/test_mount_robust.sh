#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.
#
# @file test_mount_robust.sh
# @brief Robust integration tests for FAT12 mount layer.

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

MNT_DIR="$ROOT_DIR/mnt-robust-$$"
LOG="fat12-robust-$$.log"
TMP_IMG="fat12-robust-test-$$.img"
SUCCESS=0

cleanup() {
  echo "Cleaning up..."
  if [ "$OS" = "Windows" ]; then
    $UNMOUNT_CMD "$MNT_DIR" >/dev/null 2>&1 || true
    sleep 1
    taskkill //F //IM fat12mount.exe >/dev/null 2>&1 || true
  else
    if [ "$OS" = "Darwin" ] || [ "$OS" = "Linux" ]; then
      if mount | grep -q "on $MNT_DIR "; then
        $UNMOUNT_CMD "$MNT_DIR" >/dev/null 2>&1 || true
        sleep 1
      fi
    fi
  fi
  
  if [ "$SUCCESS" -eq 1 ]; then
    rm -f "$LOG" "$TMP_IMG"
  else
    echo "Test failed. Log preserved at $LOG, Image at $TMP_IMG"
  fi
  rmdir "$MNT_DIR" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# Ensure fresh start
if [ "$OS" = "Windows" ]; then
  rmdir "$MNT_DIR" >/dev/null 2>&1 || true
else
  mkdir -p "$MNT_DIR"
fi
cp "$IMG_SRC" "$TMP_IMG"

run_mount() {
  echo "Mounting $TMP_IMG to $MNT_DIR..."
  EXTRA_FUSE_ARGS=""
  if [ "$OS" = "Windows" ]; then
    EXTRA_FUSE_ARGS="-f"
  fi
  "$FAT12MOUNT" --image "$TMP_IMG" --mount "$MNT_DIR" $EXTRA_FUSE_ARGS >"$LOG" 2>&1 &
  MOUNT_PID=$!
  
  # Wait for mount to be ready
  mounted=0
  i=0
  while [ "$i" -lt 500 ]; do
    if [ -f "$MNT_DIR/HELLO.TXT" ] 2>/dev/null; then
      mounted=1
      break
    fi
    if ! kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
      break
    fi
    sleep 0.01
    i=$((i + 1))
  done

  if [ "$mounted" -ne 1 ]; then
    echo "Mount did not become ready" >&2
    cat "$LOG" >&2
    exit 1
  fi
}

run_unmount() {
  echo "Unmounting $MNT_DIR..."
  $UNMOUNT_CMD "$MNT_DIR"
  
  # Verify process exit
  i=0
  while [ "$i" -lt 200 ]; do
    if ! kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
      break
    fi
    sleep 0.01
    i=$((i + 1))
  done
  
  if kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
    if [ "$OS" = "Windows" ]; then
      taskkill //F //PID "$MOUNT_PID" >/dev/null 2>&1 || true
    else
      echo "Error: fat12mount process did not terminate after unmount" >&2
      exit 1
    fi
  fi

  # Post-unmount integrity check
  VERIFY_EXE="$ROOT_DIR/tests/fat12_verify"
  [ "$OS" = "Windows" ] && VERIFY_EXE="${VERIFY_EXE}.exe"
  "$VERIFY_EXE" "$TMP_IMG" >/dev/null
}

# --- Single Consolidated Integration Phase ---
run_mount

echo "Verifying existing content..."
grep -q "hello from p1" "$MNT_DIR/HELLO.TXT"

echo "Testing directory visibility stress, rename, and truncate..."
STRESS_EXE="$ROOT_DIR/tests/fat12_stress"
[ "$OS" = "Windows" ] && STRESS_EXE="${STRESS_EXE}.exe"
"$STRESS_EXE" "$MNT_DIR"

echo "Testing deep tree navigation..."
mkdir -p "$MNT_DIR/A/B/C/D/E"
echo "deep data" > "$MNT_DIR/A/B/C/D/E/ROOTED.TXT"

echo "Testing relative path resolution..."
(
  cd "$MNT_DIR/A/B/C"
  [ "$(cat ../../../HELLO.TXT)" = "hello from p1" ]
)

echo "Testing large R/W..."
dd if=/dev/urandom of=host_data bs=1024 count=10 2>/dev/null
cat host_data > "$MNT_DIR/LARGE.BIN"
cmp host_data "$MNT_DIR/LARGE.BIN"

echo "Testing busy unmount handling..."
(
  cd "$MNT_DIR"
  sleep 0.5
) &
BUSY_PID=$!
sleep 0.1
if $UNMOUNT_CMD "$MNT_DIR" 2>/dev/null; then
  echo "Warning: Unmount succeeded while busy"
else
  echo "Unmount correctly refused while busy"
  kill $BUSY_PID || true
  wait $BUSY_PID || true
fi

run_unmount
rm -f host_data

SUCCESS=1
echo "PASS: fat12mount robust integration tests"
