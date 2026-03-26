#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMG_SRC="$ROOT_DIR/sample-fat12-p1.img"
FAT12MOUNT="$ROOT_DIR/fat12mount"

if [ ! -x "$FAT12MOUNT" ]; then
  echo "SKIP: fat12mount not built"
  exit 0
fi

if [ ! -f "$IMG_SRC" ]; then
  echo "Missing fixture: $IMG_SRC" >&2
  exit 1
fi

OS=$(uname -s)
UNMOUNT_CMD=""
if [ "$OS" = "Linux" ]; then
  if [ ! -c /dev/fuse ]; then
    echo "SKIP: /dev/fuse not available"
    exit 0
  fi
  UNMOUNT_CMD="fusermount3 -u"
elif [ "$OS" = "Darwin" ]; then
  if [ ! -d /Library/Filesystems/fuse-t.fs ] \
    && [ ! -f /usr/local/include/fuse/fuse.h ] \
    && [ ! -f /usr/local/lib/libfuse-t.dylib ] \
    && [ ! -f /opt/homebrew/include/fuse/fuse.h ] \
    && [ ! -f /opt/homebrew/lib/libfuse-t.dylib ]; then
    echo "SKIP: FUSE-T not installed"
    exit 0
  fi
  UNMOUNT_CMD="umount"
elif echo "$OS" | grep -q "MINGW\|MSYS"; then
  OS="Windows"
  FAT12MOUNT="$ROOT_DIR/fat12mount.exe"
  UNMOUNT_CMD="$FAT12MOUNT -u"
  export PATH="$PATH:/c/Program Files (x86)/WinFSP/bin"

  if ! "/c/Program Files (x86)/WinFSP/bin/fsptool-x64.exe" lsdrv 2>/dev/null | grep -q "WinFsp"; then
    echo "SKIP: WinFSP driver not loaded. Run: fsptool-x64.exe load"
    exit 0
  fi

  TEMP_TEST_DIR="$ROOT_DIR/fat12-launcher-test-$$"
  mkdir -p "$TEMP_TEST_DIR"
  LAUNCHER_OUTPUT=$("/c/Program Files (x86)/WinFSP/bin/launchctl-x64.exe" start TESTLAUNCHER "$TEMP_TEST_DIR" 2>&1)
  LAUNCHER_EXIT=$?
  echo "$LAUNCHER_OUTPUT" | grep -q "^OK" && LAUNCHER_OK=1 || LAUNCHER_OK=0
  if [ "$LAUNCHER_OK" -eq 1 ]; then
    "/c/Program Files (x86)/WinFSP/bin/launchctl-x64.exe" stop TESTLAUNCHER 2>/dev/null || true
  else
    rmdir "$TEMP_TEST_DIR" 2>/dev/null || true
  fi
  rmdir "$TEMP_TEST_DIR" 2>/dev/null || true

  MNT_DIR="$ROOT_DIR/fat12-fuse-mnt-$$"
  rm -rf "$MNT_DIR"
  MNT_TARGET="$MNT_DIR"
else
  echo "SKIP: unsupported OS $OS"
  exit 0
fi

MNT_DIR="$ROOT_DIR/fat12-fuse-mnt-$$"

if [ ! -x "$FAT12MOUNT" ]; then
  echo "SKIP: fat12mount not built"
  exit 0
fi

TMP_IMG="fat12-fuse-test-$$.img"
MNT_TARGET="$MNT_DIR"
LOG="fat12-fuse-$$.log"
SUCCESS=0

cleanup() {
  if [ "$OS" = "Windows" ]; then
    $UNMOUNT_CMD "$MNT_TARGET" >/dev/null 2>&1 || true
    sleep 2
    taskkill //F //IM fat12mount.exe >/dev/null 2>&1 || true
    sleep 1
  else
    if [ -n "${MOUNT_PID:-}" ]; then
      kill "$MOUNT_PID" >/dev/null 2>&1 || true
      sleep 0.5
      kill -9 "$MOUNT_PID" >/dev/null 2>&1 || true
    fi
    if mount | grep -q "on $MNT_DIR "; then
      $UNMOUNT_CMD "$MNT_DIR" >/dev/null 2>&1 || true
    fi
  fi
  sleep 1
  rm -f "$TMP_IMG" 2>/dev/null || true
  if [ "$SUCCESS" -eq 1 ]; then
    rm -f "$LOG"
  else
    echo "Log preserved at $LOG"
  fi
  rmdir "$MNT_DIR" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

cp "$IMG_SRC" "$TMP_IMG"

mkdir -p "$MNT_DIR"

EXTRA_FUSE_ARGS=""
if [ "$OS" = "Darwin" ]; then
  EXTRA_FUSE_ARGS="-d"
fi

"$FAT12MOUNT" --image "$TMP_IMG" --mount "$MNT_TARGET" $EXTRA_FUSE_ARGS >"$LOG" 2>&1 &
MOUNT_PID=$!

mounted=0
i=0
exit_status=0
while [ "$i" -lt 50 ]; do
  if [ "$OS" = "Windows" ]; then
    if ls "$MNT_DIR" >/dev/null 2>&1; then
      mounted=1
    fi
    # On Windows, also check if we can list any files
    if [ -d "$MNT_DIR" ] && ls "$MNT_DIR" 2>/dev/null | grep -q .; then
      mounted=1
    fi
    if [ "$mounted" -eq 1 ]; then
      break
    fi
  else
    if mount | grep -q "on $MNT_DIR "; then
      mounted=1
      break
    fi
  fi
  if ! kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
  i=$((i + 1))
done

if [ "$mounted" -ne 1 ]; then
  echo "Mount did not become ready" >&2
  echo "Mount process exit status: $exit_status" >&2
  cat "$LOG" >&2 || true
  exit 1
fi

# Verify filesystem has content
sleep 1
FILE_COUNT=$(ls -1 "$MNT_DIR" 2>/dev/null | wc -l)
if [ "$FILE_COUNT" -lt 1 ]; then
  echo "ERROR: Mounted filesystem is empty (expected files in image)" >&2
  cat "$LOG" >&2 || true
  exit 1
fi

if grep -q "mount point in use" "$LOG" 2>/dev/null; then
  echo "ERROR: Mount point already in use (stale mount?)" >&2
  cat "$LOG" >&2
  exit 1
fi

ready=0
i=0
while [ "$i" -lt 50 ]; do
  if [ -r "$MNT_DIR/HELLO.TXT" ] && grep -q "hello from p1" "$MNT_DIR/HELLO.TXT"; then
    ready=1
    break
  fi
  if ! kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
  i=$((i + 1))
done

if [ "$ready" -ne 1 ]; then
  echo "Mounted but HELLO.TXT not readable" >&2
  cat "$LOG" >&2 || true
  exit 1
fi

echo "from-fuse-test" > "$MNT_DIR/NEW.TXT"
grep -q "from-fuse-test" "$MNT_DIR/NEW.TXT"

if [ "$OS" = "Windows" ]; then
  echo "Testing unmount..."
  $UNMOUNT_CMD "$MNT_TARGET" 2>&1
  sleep 1
  if [ -d "$MNT_DIR" ]; then
    echo "WARNING: Mount directory still exists after unmount"
  else
    echo "Unmount successful - directory removed"
  fi
fi

SUCCESS=1
echo "PASS: fat12mount FUSE integration test"
