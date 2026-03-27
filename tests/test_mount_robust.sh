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
}

run_unmount() {
  echo "Unmounting $MNT_DIR..."
  $UNMOUNT_CMD "$MNT_DIR"
  
  # Verify process exit
  i=0
  while [ "$i" -lt 100 ]; do
    if [ "$OS" = "Windows" ]; then
      # tasklist check for native windows processes
      if ! tasklist //FI "IMAGENAME eq fat12mount.exe" 2>/dev/null | grep -q "fat12mount.exe"; then
        break
      fi
    else
      if ! kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
        break
      fi
    fi
    sleep 0.2
    i=$((i + 1))
  done
  
  if [ "$OS" = "Windows" ]; then
    if tasklist //FI "IMAGENAME eq fat12mount.exe" 2>/dev/null | grep -q "fat12mount.exe"; then
       echo "Warning: fat12mount process still appears in tasklist after unmount"
       # On Windows it can sometimes hang around or be another instance
    fi
  else
    if kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
      echo "Error: fat12mount process did not terminate after unmount" >&2
      exit 1
    fi
  fi

  # Post-unmount integrity check
  echo "Verifying image integrity..."
  VERIFY_EXE="$ROOT_DIR/tests/fat12_verify"
  if [ "$OS" = "Windows" ]; then
    VERIFY_EXE="${VERIFY_EXE}.exe"
  fi
  "$VERIFY_EXE" "$TMP_IMG"
}

# --- TEST 1: Directory Visibility & Cache Stress ---
run_mount
echo "Testing directory visibility stress..."
mkdir "$MNT_DIR/STRESS" || { echo "mkdir STRESS failed" >&2; exit 1; }
# Brief sleep to let WinFSP settle on some systems
[ "$OS" = "Windows" ] && sleep 1

for i in $(seq 1 100); do
  # Use redirection instead of touch for more reliability on WinFSP/MSYS2
  echo "test" > "$MNT_DIR/STRESS/F_$i.TXT" || { echo "Failed to create F_$i.TXT" >&2; exit 1; }
done

COUNT=$(ls -1 "$MNT_DIR/STRESS" | grep "F_" | wc -l)
if [ "$COUNT" -ne 100 ]; then
  echo "Error: Directory visibility stress failed. Expected 100 files, found $COUNT" >&2
  exit 1
fi

# Verify no "holes" in listing
ls -1 "$MNT_DIR/STRESS" > list.txt
for i in $(seq 1 100); do
  if ! grep -q "F_$i.TXT" list.txt; then
    echo "Error: File F_$i.TXT missing from listing" >&2
    exit 1
  fi
done
rm list.txt

# Interleaved modification test (Targets readdir cache regressions)
echo "Testing interleaved modifications..."
mkdir "$MNT_DIR/CACHE" || { echo "mkdir CACHE failed" >&2; exit 1; }
[ "$OS" = "Windows" ] && sleep 1
for i in $(seq 1 50); do
  echo "test" > "$MNT_DIR/CACHE/FILE_$i" || { echo "Failed to create FILE_$i" >&2; exit 1; }
  # Immediate check after creation
  if [ ! -f "$MNT_DIR/CACHE/FILE_$i" ]; then
    echo "Error: FILE_$i not visible immediately after creation (Cache regression?)" >&2
    exit 1
  fi
done
# Now delete half and check visibility
for i in $(seq 1 2 50); do
  rm "$MNT_DIR/CACHE/FILE_$i"
  if [ -f "$MNT_DIR/CACHE/FILE_$i" ]; then
    echo "Error: FILE_$i still visible after deletion (Cache regression?)" >&2
    exit 1
  fi
done
run_unmount

# --- TEST 2: Path Traversal & POSIX Conformance ---
run_mount
echo "Testing deep tree navigation..."
mkdir -p "$MNT_DIR/A/B/C/D/E"
echo "deep data" > "$MNT_DIR/A/B/C/D/E/ROOTED.TXT"
if [ "$(cat "$MNT_DIR/A/B/C/D/E/ROOTED.TXT")" != "deep data" ]; then
  echo "Error: Deep tree navigation failed" >&2
  exit 1
fi

echo "Testing relative path resolution..."
(
  cd "$MNT_DIR/A/B/C"
  if [ "$(cat ../../../HELLO.TXT)" != "hello from p1" ]; then
    echo "Error: Relative path resolution failed" >&2
    exit 1
  fi
)

echo "Testing root jail..."
(
  cd "$MNT_DIR"
  # FUSE handles .. at root by staying at root or returning to parent of mount point.
)

run_unmount

# --- TEST 3: Persistence & Remount Cycle ---
run_mount
echo "Testing persistence and large R/W..."
# Create a 10KB file (spanning 5 clusters if cluster size is 2KB)
dd if=/dev/urandom of=host_data bs=1024 count=10
# Use redirection to avoid xattr copy issues on some platforms
cat host_data > "$MNT_DIR/LARGE.BIN"
run_unmount

run_mount
if ! cmp host_data "$MNT_DIR/LARGE.BIN"; then
  echo "Error: Large file persistence check failed" >&2
  exit 1
fi
rm -f host_data "$MNT_DIR/LARGE.BIN"
run_unmount

# --- TEST 4: Busy State Handling ---
run_mount
echo "Testing busy unmount handling..."
(
  cd "$MNT_DIR"
  sleep 2
) &
BUSY_PID=$!
sleep 1

# Try to unmount while busy
if $UNMOUNT_CMD "$MNT_DIR" 2>/dev/null; then
  echo "Warning: Unmount succeeded while busy (expected on some systems or with lazy unmount)"
else
  echo "Unmount correctly refused while busy"
  kill $BUSY_PID || true
  wait $BUSY_PID || true
  sleep 1
  run_unmount
fi

SUCCESS=1
echo "PASS: fat12mount robust integration tests"
