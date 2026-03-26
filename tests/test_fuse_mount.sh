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

if [ ! -x "$FAT12MOUNT" ]; then
  echo "SKIP: fat12mount not built"
  exit 0
fi

TMP_IMG="fat12-fuse-test-$$.img"
MNT_TARGET="$MNT_DIR"
LOG="fat12-fuse-$$.log"
SUCCESS=0

cleanup() {
  if [ -n "${MOUNT_PID:-}" ]; then
    kill "$MOUNT_PID" >/dev/null 2>&1 || true
    sleep 0.5
    kill -9 "$MOUNT_PID" >/dev/null 2>&1 || true
  fi
  if [ "$OS" = "Windows" ]; then
    $UNMOUNT_CMD "$MNT_TARGET" >/dev/null 2>&1 || true
  else
    if mount | grep -q "on $MNT_DIR "; then
      $UNMOUNT_CMD "$MNT_DIR" >/dev/null 2>&1 || true
    fi
  fi
  rm -f "$TMP_IMG"
  if [ "$SUCCESS" -eq 1 ]; then
    rm -f "$LOG"
  else
    echo "Log preserved at $LOG"
  fi
  rmdir "$MNT_DIR" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

cp "$IMG_SRC" "$TMP_IMG"

EXTRA_FUSE_ARGS=""
if [ "$OS" = "Darwin" ]; then
  EXTRA_FUSE_ARGS="-d"
fi

"$FAT12MOUNT" --image "$TMP_IMG" --mount "$MNT_TARGET" -f $EXTRA_FUSE_ARGS >"$LOG" 2>&1 &
MOUNT_PID=$!

mounted=0
i=0
exit_status=0
while [ "$i" -lt 50 ]; do
  if [ "$OS" = "Windows" ]; then
    if ls "$MNT_DIR" >/dev/null 2>&1; then
      mounted=1
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
  if [ "$exit_status" -eq 133 ]; then
    echo "fat12mount exited with SIGTRAP (133). On macOS this usually" >&2
    echo "means FUSE-T could not initialize; ensure this terminal/app has" >&2
    echo "Network Volumes permission in Privacy & Security." >&2
  fi
  tail -n 50 "$LOG" >&2 || true
  if [ "$OS" = "Darwin" ] && [ -f "$HOME/Library/Logs/fuse-t/fuse-t.err" ]; then
    echo "---- fuse-t.err (tail) ----" >&2
    tail -n 50 "$HOME/Library/Logs/fuse-t/fuse-t.err" >&2 || true
  fi
  if [ "$OS" = "Darwin" ] && [ -f "$HOME/Library/Logs/fuse-t/fuse-t.log" ]; then
    echo "---- fuse-t.log (tail) ----" >&2
    tail -n 50 "$HOME/Library/Logs/fuse-t/fuse-t.log" >&2 || true
  fi
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
  tail -n 50 "$LOG" >&2 || true
  if [ "$OS" = "Darwin" ] && [ -f "$HOME/Library/Logs/fuse-t/fuse-t.err" ]; then
    echo "---- fuse-t.err (tail) ----" >&2
    tail -n 50 "$HOME/Library/Logs/fuse-t/fuse-t.err" >&2 || true
  fi
  if [ "$OS" = "Darwin" ] && [ -f "$HOME/Library/Logs/fuse-t/fuse-t.log" ]; then
    echo "---- fuse-t.log (tail) ----" >&2
    tail -n 50 "$HOME/Library/Logs/fuse-t/fuse-t.log" >&2 || true
  fi
  exit 1
fi

echo "from-fuse-test" > "$MNT_DIR/NEW.TXT"
grep -q "from-fuse-test" "$MNT_DIR/NEW.TXT"

SUCCESS=1
echo "PASS: fat12mount FUSE integration test"
