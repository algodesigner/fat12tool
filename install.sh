#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -eu

echo "===================================================="
echo "    FAT12 Toolkit Installation Bootstrap"
echo "===================================================="

# Detect OS
OS=$(uname -s)
IS_WINDOWS=0
case "$OS" in
    MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
esac

# 1. Check/Install Dependencies
if [ "$IS_WINDOWS" -eq 1 ]; then
    echo "Windows detected. Checking for build tools..."
    if ! command -v gcc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
        echo "Error: MinGW (gcc) and make are required."
        echo "Please install them via Scoop: 'scoop install mingw make'"
        exit 1
    fi
else
    echo "Installing system dependencies..."
    ./scripts/install_deps.sh
fi

# 2. Determine Installation Path
PREFIX="${PREFIX:-}"
CHOICE=1
if [ -z "$PREFIX" ]; then
    DEFAULT_PREFIX="/usr/local"
    echo ""
    echo "Where would you like to install the toolkit?"
    echo "1) System-wide ($DEFAULT_PREFIX/bin) - May require sudo"
    echo "2) User-local ($HOME/.local/bin)"
    printf "Choice [1/2]: "
    # Handle both interactive and non-interactive (defaulting to 1 if no input)
    if ! CHOICE=$(read -r choice_in && echo "$choice_in"); then
        CHOICE=1
    fi

    case "$CHOICE" in
        2) PREFIX="$HOME/.local" ;;
        *) PREFIX="$DEFAULT_PREFIX" ;;
    esac
fi

# 3. Build and Install
echo "Building toolkit..."
make clean
make all

echo "Installing to $PREFIX/bin..."
INSTALL_CMD="make install PREFIX=$PREFIX"

# Check if we need sudo for this prefix
if [ ! -w "$(dirname "$PREFIX")" ] && [ "$CHOICE" -ne 2 ]; then
    echo "Elevating privileges for installation..."
    sudo $INSTALL_CMD
else
    $INSTALL_CMD
fi

# 4. Post-installation Environment Setup
if [ "$IS_WINDOWS" -eq 1 ]; then
    WINFSP_BIN="C:\\Program Files (x86)\\WinFSP\\bin"
    if ! echo "$PATH" | grep -qi "WinFSP"; then
        echo ""
        echo "WinFSP was not detected in your PATH."
        echo "Would you like to add it to your User PATH automatically? [y/N]"
        read -r ADD_PATH
        if [ "$ADD_PATH" = "y" ] || [ "$ADD_PATH" = "Y" ]; then
            powershell.exe -Command "[Environment]::SetEnvironmentVariable('Path', [Environment]::GetEnvironmentVariable('Path', 'User') + ';$WINFSP_BIN', 'User')"
            echo "Added WinFSP to User PATH. Please restart your terminal."
        fi
    fi
fi

# Check if PREFIX/bin is in PATH
if ! echo "$PATH" | grep -q "$PREFIX/bin"; then
    echo ""
    echo "Warning: $PREFIX/bin is not in your PATH."
    echo "You may need to add it to your .bashrc, .zshrc, or profile."
fi

echo ""
echo "Installation complete!"
echo "Try running 'fat12tool --help' to get started."
