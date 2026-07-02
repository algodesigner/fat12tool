#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMG_SRC="$ROOT_DIR/sample-fat12-p1.img"

OS=$(uname -s)
FAT12TOOL="$ROOT_DIR/fat12tool"
VERIFY_EXE="$ROOT_DIR/tests/fat12_verify"
if echo "$OS" | grep -q "MINGW\|MSYS"; then
  FAT12TOOL="${FAT12TOOL}.exe"
  VERIFY_EXE="${VERIFY_EXE}.exe"
fi

if [ ! -f "$IMG_SRC" ]; then
  echo "Missing fixture: $IMG_SRC" >&2
  exit 1
fi

# Build test utilities if needed
if [ ! -f "$VERIFY_EXE" ]; then
  echo "Building test utilities..."
  make -C "$ROOT_DIR" tests/fat12_verify
fi

echo "=== Testing verify command functionality ==="

# Test 1: Basic verify on clean image
echo "Test 1: Basic verify on clean image"
TMP_IMG="fat12-verify-test-$$.img"
cp "$IMG_SRC" "$TMP_IMG"

{
  echo "verify"
  echo "verify --verbose"
  echo "verify --full"
  echo "exit"
} | "$FAT12TOOL" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Verifying FAT12 image integrity" verify-output.txt && grep -q "Verification Results:" verify-output.txt; then
  echo "✓ Test 1 passed: Verify runs successfully"
else
  echo "✗ Test 1 failed: Verify command not working"
  cat verify-output.txt
  exit 1
fi

# Test 2: Create cross-link and verify detection
echo -e "\nTest 2: Cross-link detection"
cp "$IMG_SRC" "$TMP_IMG"

# Create a cross-link between clusters 2 and 3
if ! "$VERIFY_EXE" corrupt-crosslink "$TMP_IMG" 2 3; then
  echo "✗ Failed to create cross-link"
  exit 1
fi

{
  echo "verify"
  echo "exit"
} | "$FAT12TOOL" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Cross-linked clusters: \[ERROR\] 1" verify-output.txt; then
  echo "✓ Test 2 passed: Cross-link detected"
else
  echo "✗ Test 2 failed: Cross-link not detected"
  cat verify-output.txt
  exit 1
fi

# Test 3: Repair cross-link
echo -e "\nTest 3: Cross-link repair"
{
  echo "verify --fix --yes"
  echo "verify"
  echo "exit"
} | "$FAT12TOOL" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Applied" verify-output.txt; then
  echo "✓ Test 3 passed: Fixes applied"
else
  echo "✗ Test 3 failed: Fixes not applied"
  cat verify-output.txt
  exit 1
fi

# Test 4: Create orphaned cluster and verify detection
echo -e "\nTest 4: Orphaned cluster detection"
cp "$IMG_SRC" "$TMP_IMG"

# Create an orphaned cluster (cluster 2 not referenced by any file)
if ! "$VERIFY_EXE" corrupt-orphan "$TMP_IMG" 2; then
  echo "✗ Failed to create orphaned cluster"
  exit 1
fi

{
  echo "verify"
  echo "exit"
} | "$FAT12TOOL" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Orphaned clusters: \[ERROR\] 1" verify-output.txt; then
  echo "✓ Test 4 passed: Orphaned cluster detected"
else
  echo "✗ Test 4 failed: Orphaned cluster not detected"
  cat verify-output.txt
  exit 1
fi

# Test 5: Repair orphaned cluster
echo -e "\nTest 5: Orphaned cluster repair"
{
  echo "verify --fix --yes"
  echo "verify"
  echo "exit"
} | "$FAT12TOOL" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Applied 1 fix" verify-output.txt && grep -q "Orphaned clusters: \[OK\] None" verify-output.txt; then
  echo "✓ Test 5 passed: Orphaned cluster repaired"
else
  echo "✗ Test 5 failed: Orphaned cluster not repaired"
  cat verify-output.txt
  exit 1
fi

# Cleanup
rm -f "$TMP_IMG" verify-output.txt
echo -e "\n=== All verify tests passed! ==="