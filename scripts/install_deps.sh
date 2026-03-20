#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -eu

usage() {
  cat <<'USAGE'
Usage: ./scripts/install_deps.sh [--with-docs]

Installs build dependencies for FAT12 toolkit.
- macOS: FUSE-T (cask), pkg-config, optional doxygen
- Linux: fuse3 dev packages + pkg-config, optional doxygen
USAGE
}

WITH_DOCS=0
if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  usage
  exit 0
fi
if [ "${1:-}" = "--with-docs" ]; then
  WITH_DOCS=1
elif [ -n "${1:-}" ]; then
  echo "Unknown option: $1" >&2
  usage >&2
  exit 2
fi

OS=$(uname -s)

echo "Detected OS: $OS"

if [ "$OS" = "Darwin" ]; then
  if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required on macOS. Install from https://brew.sh" >&2
    exit 1
  fi

  if ! brew list --cask fuse-t >/dev/null 2>&1; then
    echo "Installing FUSE-T..."
    brew install --cask macos-fuse-t/homebrew-cask/fuse-t
  else
    echo "FUSE-T already installed."
  fi

  if ! brew list pkg-config >/dev/null 2>&1; then
    echo "Installing pkg-config..."
    brew install pkg-config
  else
    echo "pkg-config already installed."
  fi

  if [ "$WITH_DOCS" -eq 1 ]; then
    if ! brew list doxygen >/dev/null 2>&1; then
      echo "Installing doxygen..."
      brew install doxygen
    else
      echo "doxygen already installed."
    fi
  fi

  cat <<'NOTE'
macOS note: After installing FUSE-T, allow access to Network Volumes
in Privacy & Security if prompted by the OS.
NOTE
  exit 0
fi

if [ "$OS" = "Linux" ]; then
  if command -v apt-get >/dev/null 2>&1; then
    PKGS="libfuse3-dev fuse3 pkg-config"
    [ "$WITH_DOCS" -eq 1 ] && PKGS="$PKGS doxygen"
    sudo apt-get update
    sudo apt-get install -y $PKGS
    exit 0
  fi

  if command -v dnf >/dev/null 2>&1; then
    PKGS="fuse3-devel fuse3 pkgconf-pkg-config"
    [ "$WITH_DOCS" -eq 1 ] && PKGS="$PKGS doxygen"
    sudo dnf install -y $PKGS
    exit 0
  fi

  if command -v pacman >/dev/null 2>&1; then
    PKGS="fuse3 pkgconf"
    [ "$WITH_DOCS" -eq 1 ] && PKGS="$PKGS doxygen"
    sudo pacman -Sy --noconfirm $PKGS
    exit 0
  fi

  echo "Unsupported Linux package manager (need apt-get, dnf, or pacman)." >&2
  exit 1
fi

echo "Unsupported OS: $OS" >&2
exit 1
