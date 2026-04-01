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

# Test 2: Test with --fix flag but no issues (should do nothing)
echo -e "\nTest 2: Verify with --fix on clean image"
{
  echo "verify --fix --yes"
  echo "exit"
} | "$ROOT_DIR/fat12tool" "$TMP_IMG" > verify-output.txt 2>&1

if ! grep -q "Applied" verify-output.txt && ! grep -q "Error: Failed to apply fixes" verify-output.txt; then
  echo "✓ Test 2 passed: No fixes applied to clean image"
else
  echo "✗ Test 2 failed: Should not apply fixes to clean image"
  cat verify-output.txt
  exit 1
fi

# Test 3: Test that --fix without --yes asks for confirmation when there are errors
echo -e "\nTest 3: User confirmation prompt"
# We need to create an image with errors to test this
# For now, just verify the command works
cp "$IMG_SRC" "$TMP_IMG"

{
  echo "verify --fix"
  echo "exit"
} | "$ROOT_DIR/fat12tool" "$TMP_IMG" > verify-output.txt 2>&1

if grep -q "Verifying FAT12 image integrity" verify-output.txt; then
  echo "✓ Test 3 passed: Verify command works with --fix flag"
else
  echo "✗ Test 3 failed: Verify command not working"
  cat verify-output.txt
  exit 1
fi

# Cleanup
rm -f "$TMP_IMG" verify-output.txt
echo -e "\n=== Basic verify tests passed! ==="
echo "Note: For corruption tests, run 'make test' to run the full test suite"