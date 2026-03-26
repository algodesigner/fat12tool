/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file utils.h
 * @brief Test utilities for FAT12 test suite.
 */

#ifndef FAT12_TEST_UTILS_H
#define FAT12_TEST_UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/**
 * @brief Creates a minimal FAT12 filesystem image for testing.
 *
 * This function generates a raw disk image with a valid FAT12 boot sector,
 * empty FAT tables, and an empty root directory.
 *
 * @param path Output image file path.
 * @param size_mb Size in megabytes (1-32).
 * @param cluster_sectors Sectors per cluster (1-8).
 * @return Returns 0 on success, -1 on error.
 */
int test_create_fat12_image(const char *path, int size_mb, int cluster_sectors);

/**
 * @brief Creates a FAT12 image with a specific file structure.
 *
 * This function creates a root directory containing README.TXT, HELLO.TXT,
 * and a TESTDIR subdirectory. It mirrors the structure of sample-fat12-p1.img
 * for compatibility with existing tests.
 *
 * @param path Output image file path.
 * @return Returns 0 on success, -1 on error.
 */
int test_create_standard_fat12_image(const char *path);

/**
 * @brief Simulates a disk full condition by marking all clusters as allocated.
 *
 * This function modifies the FAT tables to mark every data cluster as
 * end-of-chain (0xFFF), effectively preventing further allocations.
 *
 * @param fs_path Path to the FAT12 image.
 * @return Returns 0 on success, -1 on error.
 */
int test_simulate_disk_full(const char *fs_path);

/**
 * @brief Corrupts the boot sector with invalid values.
 *
 * This function writes zero to the bytes-per-sector field in the boot sector
 * to test filesystem mounting resilience.
 *
 * @param fs_path Path to the FAT12 image.
 * @return Returns 0 on success, -1 on error.
 */
int test_corrupt_boot_sector(const char *fs_path);

/**
 * @brief Injects bad FAT entries (cross-linked clusters).
 *
 * This function modifies the FAT to create a cycle or cross-link between
 * two clusters to test integrity checking.
 *
 * @param fs_path Path to the FAT12 image.
 * @param cluster1 First cluster to cross-link.
 * @param cluster2 Second cluster to cross-link.
 * @return Returns 0 on success, -1 on error.
 */
int test_corrupt_fat_crosslink(const char *fs_path, uint16_t cluster1, uint16_t cluster2);

/**
 * @brief Corrupts a directory entry with invalid data.
 *
 * This function overwrites a directory entry's name with invalid characters
 * to test directory traversal robustness.
 *
 * @param fs_path Path to the FAT12 image.
 * @param path File path within the filesystem (unused).
 * @return Returns 0 on success, -1 on error.
 */
int test_corrupt_directory_entry(const char *fs_path, const char *path);

/**
 * @brief Creates a temporary test image copy.
 *
 * This function creates a duplicate of an existing image file using a
 * process-unique filename to allow safe concurrent tests.
 *
 * @param src_path Source image path.
 * @return Returns a heap-allocated path to the temporary copy (must be freed by caller), or NULL on error.
 */
char *test_create_temp_copy(const char *src_path);

/**
 * @brief Verifies FAT consistency between replicas.
 *
 * This function compares all FAT copies stored on the disk to ensure they
 * are bit-for-bit identical, verifying that flushes work correctly.
 *
 * @param fs_path Path to the FAT12 image.
 * @return Returns 0 if all FAT copies are consistent, -1 if discrepancies are found.
 */
int test_verify_fat_consistency(const char *fs_path);

/**
 * @brief Verifies directory structure integrity.
 *
 * This function performs a basic sanity check on the root directory to
 * ensure it can still be listed after mutation operations.
 *
 * @param fs_path Path to the FAT12 image.
 * @return Returns 0 if structure is intact, -1 if errors are found.
 */
int test_verify_directory_integrity(const char *fs_path);

/**
 * @brief Counts free clusters in the filesystem.
 *
 * This function scans the primary FAT and tallies entries marked as zero.
 *
 * @param fs_path Path to the FAT12 image.
 * @return Returns the number of free clusters, or -1 on error.
 */
int test_count_free_clusters(const char *fs_path);

/**
 * @brief Test macro for checking conditions with descriptive failure messages.
 */
#define TEST_CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "TEST FAILED: %s (line %d)\n", (msg), __LINE__); \
            return 1; \
        } \
    } while (0)

/**
 * @brief Test macro for checking function return codes.
 */
#define TEST_CHECK_RC(rc, expected, msg) \
    do { \
        if ((rc) != (expected)) { \
            fprintf(stderr, "TEST FAILED: %s (got %d, expected %d, line %d)\n", \
                    (msg), (int)(rc), (int)(expected), __LINE__); \
            return 1; \
        } \
    } while (0)

#endif /* FAT12_TEST_UTILS_H */
