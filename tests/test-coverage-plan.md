# Test Coverage Plan: FAT12 Toolkit

## Overview
This plan outlines the systematic improvement of test coverage for the FAT12 Toolkit, focusing on the core library, CLI shell, and FUSE/WinFSP mount layer.

## Phase 1: Core Unit Test Expansion
**Status: COMPLETED**
- [x] Create `tests/utils.c/h` for image generation and error simulation.
- [x] Implement `tests/test_fat12_core_unit.c` covering all ~24 internal static functions.
- [x] Expand `tests/test_fat12_core.c` with boundary conditions and systematic input validation.
- [x] Result: 100% coverage of internal helpers and significantly improved API robustness.

## Phase 2: Mount Layer & Integration Testing
**Status: COMPLETED**
- [x] Create `tests/test_mount_robust.sh` to target directory visibility and unmounting regressions.
- [x] **Regression Identified & Fixed**: Resolved critical bug where subdirectories were restricted to a single cluster.
- [x] Implement `tests/fat12_verify.c` for automated post-operation integrity checks.
- [x] Verify path traversal, deep nesting, and "busy" unmount scenarios.
- [x] Reorganized `Makefile` to output all test binaries into the `tests/` directory.

## Phase 3: CLI Test Expansion
**Status: IN PROGRESS**
- [ ] Extend `tests/test_cli.sh` to exhaustively cover all interactive flags and command combinations.
- [ ] Create `tests/test_cli_error.sh` for failure scenarios (invalid arguments, bad permissions).
- [ ] Test edge cases in the shell parser (e.g., handling of spaces in host paths vs. FAT paths).

## Phase 4: Advanced Integrity & Error Condition Testing
**Status: PENDING**
- [ ] **Torture Tests**: High-frequency create/delete cycles to verify no cluster leaks in the FAT.
- [ ] **Recovery Scenarios**: Handling of cross-linked clusters and "orphaned" cluster chains.
- [ ] **Fuzzing-lite**: Random input to `resolve_abs_path` to ensure absolute stability.
