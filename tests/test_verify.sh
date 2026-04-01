#!/usr/bin/env sh
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMG_SRC="$ROOT_DIR/sample-fat12-p1.img"

if [ ! -f "$IMG_SRC" ]; then
  echo "Missing fixture: $IMG_SRC" >&2
  exit 1
fi

# Build test utilities if needed
if [ ! -f "$ROOT_DIR/tests/fat12_verify" ]; then
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
} | "$ROOT_DIR/fat12tool" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Total issues: 0" verify-output.txt; then
  echo "✓ Test 1 passed: Clean image has no issues"
else
  echo "✗ Test 1 failed: Clean image should have no issues"
  cat verify-output.txt
  exit 1
fi

# Test 2: Create cross-link and verify detection
echo -e "\nTest 2: Cross-link detection"
cp "$IMG_SRC" "$TMP_IMG"

# Create a cross-link between clusters 2 and 3
if ! "$ROOT_DIR/tests/fat12_verify" corrupt-crosslink "$TMP_IMG" 2 3; then
  echo "✗ Failed to create cross-link"
  exit 1
fi

{
  echo "verify"
  echo "exit"
} | "$ROOT_DIR/fat12tool" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Cross-linked clusters: ✗ 2" verify-output.txt; then
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
} | "$ROOT_DIR/fat12tool" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Applied 1 fix" verify-output.txt && grep -q "Cross-linked clusters: ✓ None" verify-output.txt; then
  echo "✓ Test 3 passed: Cross-link repaired"
else
  echo "✗ Test 3 failed: Cross-link not repaired"
  cat verify-output.txt
  exit 1
fi

# Test 4: Create orphaned cluster and verify detection
echo -e "\nTest 4: Orphaned cluster detection"
cp "$IMG_SRC" "$TMP_IMG"

# Create an orphaned cluster (cluster 2 not referenced by any file)
if ! "$ROOT_DIR/tests/fat12_verify" corrupt-orphan "$TMP_IMG" 2; then
  echo "✗ Failed to create orphaned cluster"
  exit 1
fi

{
  echo "verify"
  echo "exit"
} | "$ROOT_DIR/fat12tool" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Orphaned clusters: ✗ 1" verify-output.txt; then
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
} | "$ROOT_DIR/fat12tool" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Applied 1 fix" verify-output.txt && grep -q "Orphaned clusters: ✓ None" verify-output.txt; then
  echo "✓ Test 5 passed: Orphaned cluster repaired"
else
  echo "✗ Test 5 failed: Orphaned cluster not repaired"
  cat verify-output.txt
  exit 1
fi

# Cleanup
rm -f "$TMP_IMG" verify-output.txt
echo -e "\n=== All verify tests passed! ==="