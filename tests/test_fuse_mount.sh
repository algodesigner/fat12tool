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
else
  echo "SKIP: unsupported OS $OS"
  exit 0
fi

TMP_IMG=$(mktemp /tmp/fat12-fuse-test-XXXXXX.img)
MNT_DIR=$(mktemp -d /tmp/fat12-fuse-mnt-XXXXXX)
LOG="/tmp/fat12-fuse-$$.log"
SUCCESS=0

cleanup() {
  if mount | grep -q "on $MNT_DIR "; then
    $UNMOUNT_CMD "$MNT_DIR" >/dev/null 2>&1 || true
  fi
  if [ -n "${MOUNT_PID:-}" ]; then
    kill "$MOUNT_PID" >/dev/null 2>&1 || true
    wait "$MOUNT_PID" >/dev/null 2>&1 || true
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

"$FAT12MOUNT" --image "$TMP_IMG" --mount "$MNT_DIR" -f $EXTRA_FUSE_ARGS >"$LOG" 2>&1 &
MOUNT_PID=$!

mounted=0
i=0
exit_status=0
while [ "$i" -lt 50 ]; do
  if mount | grep -q "on $MNT_DIR "; then
    mounted=1
    break
  fi
  if ls "$MNT_DIR" >/dev/null 2>&1; then
    mounted=1
    break
  fi
  if ! kill -0 "$MOUNT_PID" >/dev/null 2>&1; then
    wait "$MOUNT_PID" >/dev/null 2>&1 || exit_status=$?
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
    wait "$MOUNT_PID" >/dev/null 2>&1 || true
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
