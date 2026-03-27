/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file test_fat12_core_unit.c
 * @brief Unit tests for internal helper functions of FAT12 core.
 */

#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64

#include "../fat12_core.h"
#include "utils.h"

/* Include the source file to access internal functions */
#include "../fat12_core.c"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <stdio.h>
#include <string.h>

static void make_temp_path(char *out, const char *prefix)
{
    static int counter = 0;
    sprintf(out, "%s-%d-%d.img", prefix, (int)getpid(), counter++);
}

/**
 * @brief Verifies name conversion between printable strings and FAT 8.3 format.
 *
 * This function tests to_short_name and short_name_to_str with various valid
 * and invalid inputs, including edge cases like names with exactly 8 characters.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_name_conversion(void)
{
    uint8_t n83[11];
    char back[13];

    /* Valid 8.3 name */
    TEST_CHECK(to_short_name("README.TXT", n83) == 0, "README.TXT should convert");
    short_name_to_str(n83, back, sizeof(back));
    TEST_CHECK(strcmp(back, "README.TXT") == 0, "roundtrip README.TXT");

    /* Valid name without extension */
    TEST_CHECK(to_short_name("HELLO", n83) == 0, "HELLO should convert");
    short_name_to_str(n83, back, sizeof(back));
    TEST_CHECK(strcmp(back, "HELLO") == 0, "roundtrip HELLO");

    /* Valid exactly 8-character name */
    TEST_CHECK(to_short_name("LONGNAME.TXT", n83) == 0, "LONGNAME.TXT should convert");
    short_name_to_str(n83, back, sizeof(back));
    TEST_CHECK(strcmp(back, "LONGNAME.TXT") == 0, "roundtrip LONGNAME.TXT");

    /* Invalid: name too long (>8) */
    TEST_CHECK(to_short_name("VERYLONGNAME.TXT", n83) != 0, "VERYLONGNAME.TXT should fail");

    /* Invalid: extension too long (>3) */
    TEST_CHECK(to_short_name("FILE.LONG", n83) != 0, "FILE.LONG should fail");

    /* Invalid: empty name */
    TEST_CHECK(to_short_name("", n83) != 0, "empty name should fail");

    /* Invalid: dot at start */
    TEST_CHECK(to_short_name(".HIDDEN", n83) != 0, ".HIDDEN should fail");

    /* Invalid: invalid characters */
    TEST_CHECK(to_short_name("BAD*FILE.TXT", n83) != 0, "BAD*FILE.TXT should fail");

    return 0;
}

/**
 * @brief Verifies low-level FAT table operations.
 *
 * This function tests fat_get, fat_set, and is_eoc by creating a temporary
 * filesystem image and manipulating FAT entries.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_fat_operations(void)
{
    char path[256];
    make_temp_path(path, "fat12-unit-test");

    /* Create a minimal FAT12 image (1MB, 1 sector per cluster) */
    TEST_CHECK(test_create_fat12_image(path, 1, 1) == 0, "create test image failed");

    Fat12 fs;
    int rc = fat12_open(&fs, path, 0);
    TEST_CHECK(rc == 0, "fat12_open failed");

    /* Test fat_get and fat_set on cluster 2 (first data cluster) */
    uint16_t val = fat_get(&fs, 2);
    /* Initially cluster 2 should be free (0) */
    TEST_CHECK(val == 0, "cluster 2 should be free");

    /* Set cluster 2 to EOC */
    fat_set(&fs, 2, FAT12_EOC);
    val = fat_get(&fs, 2);
    TEST_CHECK(is_eoc(val), "cluster 2 should be EOC after set");

    /* Set cluster 2 to link to cluster 3 */
    fat_set(&fs, 2, 3);
    val = fat_get(&fs, 2);
    TEST_CHECK(val == 3, "cluster 2 should point to cluster 3");

    /* Test is_eoc */
    TEST_CHECK(!is_eoc(0), "0 is not EOC");
    TEST_CHECK(!is_eoc(3), "3 is not EOC");
    TEST_CHECK(is_eoc(FAT12_EOC), "FAT12_EOC is EOC");
    TEST_CHECK(is_eoc(0xFFF), "0xFFF is EOC");

    fat12_close(&fs);
    unlink(path);
    return 0;
}

/**
 * @brief Verifies directory entry operations.
 *
 * This function tests add_entry, find_in_dir, and remove_entry by
 * programmatically adding and removing entries in a test image.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_directory_operations(void)
{
    char path[256];
    make_temp_path(path, "fat12-dir-unit-test");

    TEST_CHECK(test_create_fat12_image(path, 1, 1) == 0, "create test image failed");

    Fat12 fs;
    TEST_CHECK(fat12_open(&fs, path, 0) == 0, "fat12_open failed");

    /* Test add_entry */
    uint64_t off;
    TEST_CHECK(add_entry(&fs, 0, "TEST.TXT", 0x20, 0, 0, &off) == 0, "add_entry failed");

    /* Test find_in_dir */
    uint8_t n83[11];
    to_short_name("TEST.TXT", n83);
    EntryRef ref;
    TEST_CHECK(find_in_dir(&fs, 0, n83, &ref) == 0, "find_in_dir failed");
    TEST_CHECK(ref.found, "entry should be found");
    TEST_CHECK(ref.offset == off, "offset mismatch");
    TEST_CHECK(memcmp(ref.entry.name, n83, 11) == 0, "name mismatch");

    /* Test remove_entry */
    TEST_CHECK(remove_entry(&fs, off) == 0, "remove_entry failed");
    TEST_CHECK(find_in_dir(&fs, 0, n83, &ref) == 0, "find_in_dir after remove failed");
    TEST_CHECK(!ref.found, "entry should not be found after remove");

    fat12_close(&fs);
    unlink(path);
    return 0;
}

/**
 * @brief Verifies absolute path resolution.
 *
 * This function tests resolve_abs_path with various scenarios including
 * nested directories, current/parent directory markers (. and ..), and
 * non-existent paths.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_path_resolution(void)
{
    char path[256];
    make_temp_path(path, "fat12-path-unit-test");

    TEST_CHECK(test_create_fat12_image(path, 1, 1) == 0, "create test image failed");

    Fat12 fs;
    TEST_CHECK(fat12_open(&fs, path, 0) == 0, "fat12_open failed");

    /* Setup: /DIR/SUB/FILE.TXT */
    TEST_CHECK(fat12_mkdir(&fs, "/DIR") == 0, "mkdir /DIR failed");
    TEST_CHECK(fat12_mkdir(&fs, "/DIR/SUB") == 0, "mkdir /DIR/SUB failed");
    TEST_CHECK(fat12_create(&fs, "/DIR/SUB/FILE.TXT") == 0, "create file failed");

    EntryRef ref;
    uint16_t parent;
    char leaf[256];

    /* Test absolute path */
    TEST_CHECK(resolve_abs_path(&fs, "/DIR/SUB/FILE.TXT", &ref, &parent, leaf, sizeof(leaf)) == 0, "resolve absolute failed");
    TEST_CHECK(ref.found, "should be found");
    TEST_CHECK(strcmp(leaf, "FILE.TXT") == 0, "leaf mismatch");

    /* Test . and .. */
    TEST_CHECK(resolve_abs_path(&fs, "/DIR/./SUB/../SUB/FILE.TXT", &ref, &parent, leaf, sizeof(leaf)) == 0, "resolve with dots failed");
    TEST_CHECK(ref.found, "should be found with dots");

    /* Test non-existent */
    TEST_CHECK(resolve_abs_path(&fs, "/DIR/MISSING.TXT", &ref, &parent, leaf, sizeof(leaf)) == 0, "resolve non-existent should succeed");
    TEST_CHECK(!ref.found, "should not be found");
    TEST_CHECK(strcmp(leaf, "MISSING.TXT") == 0, "leaf should be the non-existent part");

    fat12_close(&fs);
    unlink(path);
    return 0;
}

/**
 * @brief Verifies cluster allocation and chain management.
 *
 * This function tests alloc_cluster, free_chain, ensure_chain_size, and
 * collect_chain to ensure the filesystem correctly manages its data area.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_cluster_management(void)
{
    char path[256];
    make_temp_path(path, "fat12-cluster-unit-test");

    TEST_CHECK(test_create_fat12_image(path, 1, 1) == 0, "create test image failed");

    Fat12 fs;
    TEST_CHECK(fat12_open(&fs, path, 0) == 0, "fat12_open failed");

    /* Test alloc_cluster */
    uint16_t c1, c2;
    TEST_CHECK(alloc_cluster(&fs, &c1) == 0, "alloc_cluster 1 failed");
    TEST_CHECK(alloc_cluster(&fs, &c2) == 0, "alloc_cluster 2 failed");
    TEST_CHECK(c1 != c2, "different clusters should be allocated");
    TEST_CHECK(fat_get(&fs, c1) == 0xFFF, "c1 should be marked EOC");

    /* Test free_chain */
    fat_set(&fs, c1, c2);
    fat_set(&fs, c2, 0xFFF);
    TEST_CHECK(free_chain(&fs, c1) == 0, "free_chain failed");
    TEST_CHECK(fat_get(&fs, c1) == 0, "c1 should be free");
    TEST_CHECK(fat_get(&fs, c2) == 0, "c2 should be free");

    /* Test ensure_chain_size */
    uint16_t head = 0;
    TEST_CHECK(ensure_chain_size(&fs, &head, 3) == 0, "ensure_chain_size grow failed");
    TEST_CHECK(head >= 2, "head should be valid");
    
    uint16_t *chain = NULL;
    size_t n = 0;
    TEST_CHECK(collect_chain(&fs, head, &chain, &n) == 0, "collect_chain failed");
    TEST_CHECK(n == 3, "chain size mismatch");
    free(chain);

    TEST_CHECK(ensure_chain_size(&fs, &head, 1) == 0, "ensure_chain_size shrink failed");
    TEST_CHECK(collect_chain(&fs, head, &chain, &n) == 0, "collect_chain after shrink failed");
    TEST_CHECK(n == 1, "shrunk chain size mismatch");
    free(chain);

    TEST_CHECK(ensure_chain_size(&fs, &head, 0) == 0, "ensure_chain_size free failed");
    TEST_CHECK(head == 0, "head should be 0 after free");

    fat12_close(&fs);
    unlink(path);
    return 0;
}

/**
 * @brief Main entry point for the unit test suite.
 *
 * Runs all unit tests and returns a non-zero exit code if any test fails.
 *
 * @return 0 on success, non-zero on failure.
 */
int main(void)
{
    if (test_name_conversion() != 0) return 1;
    if (test_fat_operations() != 0) return 1;
    if (test_directory_operations() != 0) return 1;
    if (test_path_resolution() != 0) return 1;
    if (test_cluster_management() != 0) return 1;

    printf("PASS: fat12_core unit tests\n");
    return 0;
}
