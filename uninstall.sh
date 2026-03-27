#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -eu

echo "===================================================="
echo "    FAT12 Toolkit Uninstaller"
echo "===================================================="

# Detect OS and Extension
OS=$(uname -s)
EXE=""
case "$OS" in
    MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

# Define standard search prefixes
SYSTEM_PREFIX="/usr/local"
USER_PREFIX="$HOME/.local"

FOUND_SYSTEM=0
FOUND_USER=0

# Check for binaries
if [ -f "$SYSTEM_PREFIX/bin/fat12tool$EXE" ]; then
    FOUND_SYSTEM=1
fi

if [ -f "$USER_PREFIX/bin/fat12tool$EXE" ]; then
    FOUND_USER=1
fi

uninstall_at() {
    local prefix="$1"
    echo "Uninstalling from $prefix/bin..."
    
    # Check if we need sudo
    if [ ! -w "$prefix/bin" ]; then
        echo "Elevating privileges for uninstallation..."
        sudo make uninstall PREFIX="$prefix"
    else
        make uninstall PREFIX="$prefix"
    fi
}

if [ "$FOUND_SYSTEM" -eq 0 ] && [ "$FOUND_USER" -eq 0 ]; then
    echo "No installation detected in standard locations (/usr/local or ~/.local)."
    echo "If you used a custom prefix, run: make uninstall PREFIX=/your/path"
    exit 0
fi

if [ "$FOUND_SYSTEM" -eq 1 ] && [ "$FOUND_USER" -eq 1 ]; then
    echo "Multiple installations detected:"
    echo "1) System-wide ($SYSTEM_PREFIX/bin)"
    echo "2) User-local ($USER_PREFIX/bin)"
    printf "Which one would you like to remove? [1/2/both]: "
    read -r CHOICE
    case "$CHOICE" in
        1) uninstall_at "$SYSTEM_PREFIX" ;;
        2) uninstall_at "$USER_PREFIX" ;;
        both) 
            uninstall_at "$SYSTEM_PREFIX"
            uninstall_at "$USER_PREFIX"
            ;;
        *) echo "Invalid choice. Exiting."; exit 1 ;;
    esac
elif [ "$FOUND_SYSTEM" -eq 1 ]; then
    echo "Detected system-wide installation at $SYSTEM_PREFIX/bin."
    uninstall_at "$SYSTEM_PREFIX"
elif [ "$FOUND_USER" -eq 1 ]; then
    echo "Detected user-local installation at $USER_PREFIX/bin."
    uninstall_at "$USER_PREFIX"
fi

echo ""
echo "Uninstallation complete!"
