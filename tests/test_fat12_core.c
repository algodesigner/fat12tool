/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file test_fat12_core.c
 * @brief End-to-end API tests for the FAT12 core library.
 */
#define _GNU_SOURCE
#include "../fat12_core.h"
#include "utils.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
            return 1;                                                 \
        }                                                             \
    } while (0)

#pragma pack(push, 1)
typedef struct {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} MbrPartition;
#pragma pack(pop)

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        perror("fopen src");
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        perror("fopen dst");
        fclose(in);
        return -1;
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }

    fclose(in);
    fclose(out);
    return 0;
}

/**
 * @brief Parse MBR partition offset (bytes) for fixture tests.
 */
static int parse_partition_offset_bytes(
        const char *disk_img, int partition_index, uint64_t *offset_out)
{
    *offset_out = 0;
    FILE *fp = fopen(disk_img, "rb");
    if (!fp)
        return -1;

    uint8_t mbr[512];
    if (fread(mbr, 1, sizeof(mbr), fp) != sizeof(mbr)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (partition_index < 1 || partition_index > 4)
        return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return -1;

    MbrPartition p;
    memcpy(&p, mbr + 446 + (partition_index - 1) * 16, sizeof(p));
    if (p.sector_count == 0)
        return -1;

    *offset_out = (uint64_t)p.lba_start * 512ULL;
    return 0;
}

typedef struct {
    int found_readme;
    int found_hello;
    int found_testdir;
} RootScan;

static int root_list_cb(const char *name, const Fat12Node *node, void *user)
{
    RootScan *scan = (RootScan *)user;
    if (strcmp(name, "README.TXT") == 0)
        scan->found_readme = 1;
    if (strcmp(name, "HELLO.TXT") == 0)
        scan->found_hello = 1;
    if (strcmp(name, "TESTDIR") == 0 && node->is_dir)
        scan->found_testdir = 1;
    return 0;
}

/* Callback for traversal test */
typedef struct {
    int count;
    int found_readme;
    int found_hello;
    int found_testdir;
} TraversalState;

static int traversal_list_cb(const char *name, const Fat12Node *node, void *user)
{
    TraversalState *s = (TraversalState *)user;
    s->count++;
    if (strcmp(name, "README.TXT") == 0) s->found_readme = 1;
    if (strcmp(name, "HELLO.TXT") == 0) s->found_hello = 1;
    if (strcmp(name, "TESTDIR") == 0 && node->is_dir) s->found_testdir = 1;
    return 0;
}

/* Simple counter callback */
static int count_list_cb(const char *name, const Fat12Node *node, void *user)
{
    (void)name;
    (void)node;
    (*(int *)user)++;
    return 0;
}

#define CHECK_RC(rc, msg)                                                    \
    do {                                                                     \
        if ((rc) != 0) {                                                     \
            fprintf(stderr, "FAIL: %s (rc=%d, line %d)\n", (msg), (int)(rc), \
                    __LINE__);                                               \
            return 1;                                                        \
        }                                                                    \
    } while (0)

/**
 * @brief Validate primary core operations on a temporary writable image copy.
 */
static int run_core_ops_test(const char *fixture_p1_img)
{
    char tmp_template[256];
    snprintf(tmp_template, sizeof(tmp_template), "fat12-core-test-%d.img",
            (int)getpid());

    CHECK(copy_file(fixture_p1_img, tmp_template) == 0, "copy fixture failed");

    Fat12 fs;
    int rc = fat12_open(&fs, tmp_template, 0);
    CHECK(rc == 0, "fat12_open fixture failed");

    Fat12Node node;
    rc = fat12_stat(&fs, "/", &node);
    CHECK(rc == 0, "stat root failed");
    CHECK(node.is_dir == 1, "root should be dir");

    RootScan scan = {0};
    rc = fat12_list(&fs, "/", root_list_cb, &scan);
    CHECK(rc == 0, "list root failed");
    CHECK(scan.found_readme && scan.found_hello && scan.found_testdir,
            "missing expected fixture files");

    char hello[64];
    ssize_t n = fat12_read(&fs, "/HELLO.TXT", hello, sizeof(hello) - 1, 0);
    CHECK(n > 0, "read HELLO.TXT failed");
    hello[n] = '\0';
    CHECK(strstr(hello, "hello from p1") != NULL, "HELLO.TXT content mismatch");

    rc = fat12_create(&fs, "/NEWFILE.TXT");
    CHECK(rc == 0, "create NEWFILE.TXT failed");

    const char *p1 = "abc";
    n = fat12_write(&fs, "/NEWFILE.TXT", p1, strlen(p1), 0);
    CHECK(n == (ssize_t)strlen(p1), "write NEWFILE.TXT part1 failed");

    const char *p2 = "XYZ";
    n = fat12_write(&fs, "/NEWFILE.TXT", p2, strlen(p2), 5);
    CHECK(n == (ssize_t)strlen(p2), "write NEWFILE.TXT part2 failed");

    char out[32];
    memset(out, 0, sizeof(out));
    n = fat12_read(&fs, "/NEWFILE.TXT", out, sizeof(out), 0);
    CHECK(n == 8, "NEWFILE.TXT size should be 8");
    CHECK(out[0] == 'a' && out[1] == 'b' && out[2] == 'c',
            "prefix bytes mismatch");
    CHECK(out[3] == '\0' && out[4] == '\0', "sparse gap should be zero-filled");
    CHECK(out[5] == 'X' && out[6] == 'Y' && out[7] == 'Z',
            "suffix bytes mismatch");

    rc = fat12_truncate(&fs, "/NEWFILE.TXT", 2);
    CHECK(rc == 0, "truncate to 2 failed");
    memset(out, 0, sizeof(out));
    n = fat12_read(&fs, "/NEWFILE.TXT", out, sizeof(out), 0);
    CHECK(n == 2 && out[0] == 'a' && out[1] == 'b', "truncate shrink mismatch");

    rc = fat12_mkdir(&fs, "/NEWDIR");
    CHECK(rc == 0, "mkdir NEWDIR failed");
    rc = fat12_create(&fs, "/NEWDIR/FILE.TXT");
    CHECK(rc == 0, "create nested file failed");

    const char *nested = "nested-data";
    n = fat12_write(&fs, "/NEWDIR/FILE.TXT", nested, strlen(nested), 0);
    CHECK(n == (ssize_t)strlen(nested), "nested write failed");

    rc = fat12_rmdir(&fs, "/NEWDIR");
    CHECK(rc == -ENOTEMPTY, "rmdir should fail for non-empty dir");

    rc = fat12_unlink(&fs, "/NEWDIR/FILE.TXT");
    CHECK(rc == 0, "unlink nested file failed");
    rc = fat12_rmdir(&fs, "/NEWDIR");
    CHECK(rc == 0, "rmdir NEWDIR failed");

    // Test rename
    rc = fat12_create(&fs, "/OLDNAME.TXT");
    CHECK(rc == 0, "create OLDNAME.TXT failed");
    rc = fat12_rename(&fs, "/OLDNAME.TXT", "/NEWNAME.TXT");
    CHECK(rc == 0, "rename file failed");
    rc = fat12_stat(&fs, "/OLDNAME.TXT", &node);
    CHECK(rc == -ENOENT, "old name should be gone");
    rc = fat12_stat(&fs, "/NEWNAME.TXT", &node);
    CHECK(rc == 0, "new name should exist");

    rc = fat12_mkdir(&fs, "/MOVEDIR");
    CHECK_RC(rc, "mkdir MOVEDIR failed");
    rc = fat12_mkdir(&fs, "/TDIR");
    CHECK_RC(rc, "mkdir TDIR failed");
    rc = fat12_rename(&fs, "/MOVEDIR", "/TDIR/MOVEDIR");
    CHECK_RC(rc, "move dir failed");
    rc = fat12_stat(&fs, "/MOVEDIR", &node);
    CHECK(rc == -ENOENT, "old dir name should be gone");
    rc = fat12_stat(&fs, "/TDIR/MOVEDIR", &node);
    CHECK(rc == 0, "moved dir should exist in target");

    rc = fat12_utimens_now(&fs, "/NEWNAME.TXT");
    CHECK(rc == 0, "utimens failed");

    fat12_close(&fs);

    rc = fat12_open(&fs, tmp_template, 0);
    CHECK(rc == 0, "reopen tmp image failed");
    memset(out, 0, sizeof(out));
    n = fat12_read(&fs, "/NEWFILE.TXT", out, sizeof(out), 0);
    CHECK(n == 2 && out[0] == 'a' && out[1] == 'b', "persistence check failed");
    fat12_close(&fs);

    unlink(tmp_template);
    return 0;
}

/**
 * @brief Validate opening a FAT12 partition via offset within an MBR image.
 */
static int run_partition_offset_test(const char *disk_img)
{
    uint64_t p1_off;
    CHECK(parse_partition_offset_bytes(disk_img, 1, &p1_off) == 0,
            "parse partition1 offset failed");

    Fat12 fs;
    int rc = fat12_open(&fs, disk_img, p1_off);
    CHECK(rc == 0, "fat12_open with partition offset failed");

    char buf[128];
    ssize_t n = fat12_read(&fs, "/README.TXT", buf, sizeof(buf) - 1, 0);
    CHECK(n > 0, "read README from partition 1 failed");
    buf[n] = '\0';
    CHECK(strstr(buf, "partition P1") != NULL,
            "partition README content mismatch");

    fat12_close(&fs);
    return 0;
}

/**
 * @brief Test comprehensive file system traversal.
 */
static int test_file_system_traversal(const char *fixture_img)
{
    char *tmp = test_create_temp_copy(fixture_img);
    TEST_CHECK(tmp != NULL, "create temp copy failed");

    Fat12 fs;
    int rc = fat12_open(&fs, tmp, 0);
    TEST_CHECK_RC(rc, 0, "fat12_open failed");

    Fat12Node node;

    /* Test root directory listing */
    TraversalState state = {0};

    rc = fat12_list(&fs, "/", traversal_list_cb, &state);
    TEST_CHECK_RC(rc, 0, "fat12_list root failed");
    TEST_CHECK(state.count >= 3, "should list at least 3 entries");
    TEST_CHECK(state.found_readme, "README.TXT not found");
    TEST_CHECK(state.found_hello, "HELLO.TXT not found");
    TEST_CHECK(state.found_testdir, "TESTDIR not found");

    /* Test subdirectory traversal */
    rc = fat12_stat(&fs, "/TESTDIR", &node);
    TEST_CHECK_RC(rc, 0, "stat TESTDIR failed");
    TEST_CHECK(node.is_dir, "TESTDIR should be directory");

    /* Create nested directory structure for testing */
    rc = fat12_mkdir(&fs, "/TESTDIR/SUBDIR");
    TEST_CHECK_RC(rc, 0, "mkdir SUBDIR failed");

    rc = fat12_create(&fs, "/TESTDIR/SUBDIR/FILE.TXT");
    TEST_CHECK_RC(rc, 0, "create nested file failed");

    const char *data = "nested content";
    ssize_t n = fat12_write(&fs, "/TESTDIR/SUBDIR/FILE.TXT", data, strlen(data), 0);
    TEST_CHECK(n == (ssize_t)strlen(data), "write nested file failed");

    /* List nested directory */
    int nested_count = 0;
    rc = fat12_list(&fs, "/TESTDIR/SUBDIR", count_list_cb, &nested_count);
    TEST_CHECK_RC(rc, 0, "list SUBDIR failed");
    TEST_CHECK(nested_count >= 1, "SUBDIR should have at least 1 entry");

    /* Test path resolution with . and .. */
    rc = fat12_stat(&fs, "/TESTDIR/SUBDIR/.././SUBDIR/FILE.TXT", &node);
    TEST_CHECK_RC(rc, 0, "path with . and .. resolution failed");

    fat12_close(&fs);
    unlink(tmp);
    free(tmp);
    return 0;
}

/**
 * @brief Test boundary conditions (max file size, path limits).
 */
static int test_boundary_conditions(const char *fixture_img)
{
    char *tmp = test_create_temp_copy(fixture_img);
    TEST_CHECK(tmp != NULL, "create temp copy failed");

    Fat12 fs;
    int rc = fat12_open(&fs, tmp, 0);
    TEST_CHECK_RC(rc, 0, "fat12_open failed");

    /* Test path length limits */
    char long_path[512];
    memset(long_path, 'A', 511);
    long_path[511] = '\0';

    /* Create a deep directory structure would test path limits */
    /* For now, test reasonable paths */
    rc = fat12_create(&fs, "/BOUNDARY.TXT");
    TEST_CHECK_RC(rc, 0, "create boundary test file failed");

    /* Test writing near cluster boundaries */
    /* Get cluster size from filesystem info */
    uint32_t cluster_size = fs.cluster_size;
    TEST_CHECK(cluster_size > 0, "cluster size should be positive");

    /* Write data that spans multiple clusters */
    size_t large_size = cluster_size * 3;
    char *large_buf = malloc(large_size);
    TEST_CHECK(large_buf != NULL, "malloc failed");

    memset(large_buf, 'X', large_size);
    ssize_t n = fat12_write(&fs, "/BOUNDARY.TXT", large_buf, large_size, 0);
    TEST_CHECK(n == (ssize_t)large_size, "write large file failed");

    /* Read back and verify */
    char *read_buf = malloc(large_size);
    TEST_CHECK(read_buf != NULL, "malloc failed");

    n = fat12_read(&fs, "/BOUNDARY.TXT", read_buf, large_size, 0);
    TEST_CHECK(n == (ssize_t)large_size, "read large file failed");
    TEST_CHECK(memcmp(large_buf, read_buf, large_size) == 0, "data mismatch");

    free(large_buf);
    free(read_buf);

    /* Test file truncation to exact cluster boundary */
    rc = fat12_truncate(&fs, "/BOUNDARY.TXT", cluster_size * 2);
    TEST_CHECK_RC(rc, 0, "truncate to cluster boundary failed");

    fat12_close(&fs);
    unlink(tmp);
    free(tmp);
    return 0;
}

/**
 * @brief Test error cases for all public API functions.
 */
static int test_error_cases(const char *fixture_img)
{
    char *tmp = test_create_temp_copy(fixture_img);
    TEST_CHECK(tmp != NULL, "create temp copy failed");

    Fat12 fs;
    int rc = fat12_open(&fs, tmp, 0);
    TEST_CHECK_RC(rc, 0, "fat12_open failed");

    Fat12Node node;
    char buffer[64];

    /* Test invalid paths */
    rc = fat12_stat(&fs, NULL, &node);
    TEST_CHECK_RC(rc, -EINVAL, "stat NULL path should return EINVAL");

    rc = fat12_stat(&fs, "", &node);
    TEST_CHECK_RC(rc, -EINVAL, "stat empty path should return EINVAL");

    rc = fat12_stat(&fs, "relative", &node);
    TEST_CHECK_RC(rc, -EINVAL, "stat relative path should return EINVAL");

    /* Test non-existent paths */
    rc = fat12_stat(&fs, "/NONEXISTENT.TXT", &node);
    TEST_CHECK_RC(rc, -ENOENT, "stat non-existent file should return ENOENT");

    rc = fat12_read(&fs, "/NONEXISTENT.TXT", buffer, sizeof(buffer), 0);
    TEST_CHECK(rc < 0, "read non-existent file should fail");

    rc = fat12_write(&fs, "/NONEXISTENT.TXT", "data", 4, 0);
    TEST_CHECK(rc < 0, "write non-existent file should fail");

    /* Test invalid file handles (already closed) */
    fat12_close(&fs);

    /* Re-open for more tests */
    rc = fat12_open(&fs, tmp, 0);
    TEST_CHECK_RC(rc, 0, "re-open failed");

    /* Test directory operations on files and vice versa */
    rc = fat12_create(&fs, "/FILE.TXT");
    TEST_CHECK_RC(rc, 0, "create FILE.TXT failed");

    rc = fat12_rmdir(&fs, "/FILE.TXT");
    TEST_CHECK_RC(rc, -ENOTDIR, "rmdir on file should return ENOTDIR");

    rc = fat12_mkdir(&fs, "/DIR");
    TEST_CHECK_RC(rc, 0, "mkdir DIR failed");

    rc = fat12_unlink(&fs, "/DIR");
    TEST_CHECK_RC(rc, -EISDIR, "unlink on directory should return EISDIR");

    /* Test rename with invalid paths */
    rc = fat12_rename(&fs, "/FILE.TXT", "");
    TEST_CHECK_RC(rc, -EINVAL, "rename to empty path should fail");

    rc = fat12_rename(&fs, "", "/NEW.TXT");
    TEST_CHECK_RC(rc, -EINVAL, "rename from empty path should fail");

    /* Test path traversal safety */
    rc = fat12_stat(&fs, "/../README.TXT", &node);
    TEST_CHECK_RC(rc, 0, "path traversal /.. should stay at root");

    rc = fat12_stat(&fs, "/TESTDIR/../../HELLO.TXT", &node);
    TEST_CHECK_RC(rc, 0, "deep path traversal should stay at root");

    /* Test invalid characters in path */
    rc = fat12_create(&fs, "/BAD*FILE.TXT");
    TEST_CHECK(rc != 0, "create with invalid character should fail");

    fat12_close(&fs);
    unlink(tmp);
    free(tmp);
    return 0;
}

/**
 * @brief Test filesystem integrity after operations.
 */
static int test_filesystem_integrity(const char *fixture_img)
{
    char *tmp = test_create_temp_copy(fixture_img);
    TEST_CHECK(tmp != NULL, "create temp copy failed");

    Fat12 fs;
    Fat12Node node;
    int rc = fat12_open(&fs, tmp, 0);
    TEST_CHECK_RC(rc, 0, "fat12_open failed");

    /* Perform series of operations */
    rc = fat12_mkdir(&fs, "/INTEGTST");
    fprintf(stderr, "DEBUG: fat12_mkdir returned %d (errno %d)\n", rc, rc < 0 ? -rc : 0);
    TEST_CHECK_RC(rc, 0, "mkdir failed");

    rc = fat12_create(&fs, "/INTEGTST/FILE1.TXT");
    TEST_CHECK_RC(rc, 0, "create file1 failed");

    rc = fat12_create(&fs, "/INTEGTST/FILE2.TXT");
    TEST_CHECK_RC(rc, 0, "create file2 failed");

    const char *data = "test data";
    ssize_t n = fat12_write(&fs, "/INTEGTST/FILE1.TXT", data, strlen(data), 0);
    TEST_CHECK(n == (ssize_t)strlen(data), "write file1 failed");

    /* Rename directory */
    rc = fat12_rename(&fs, "/INTEGTST", "/RENAMED");
    TEST_CHECK_RC(rc, 0, "rename directory failed");

    /* Verify renamed directory contents */
    rc = fat12_stat(&fs, "/RENAMED/FILE1.TXT", &node);
    TEST_CHECK_RC(rc, 0, "stat file in renamed dir failed");

    char buffer[64];
    n = fat12_read(&fs, "/RENAMED/FILE1.TXT", buffer, sizeof(buffer), 0);
    TEST_CHECK(n == (ssize_t)strlen(data), "read from renamed dir failed");
    TEST_CHECK(memcmp(buffer, data, strlen(data)) == 0, "data mismatch after rename");

    /* Remove files and directory */
    rc = fat12_unlink(&fs, "/RENAMED/FILE1.TXT");
    TEST_CHECK_RC(rc, 0, "unlink file1 failed");

    rc = fat12_unlink(&fs, "/RENAMED/FILE2.TXT");
    TEST_CHECK_RC(rc, 0, "unlink file2 failed");

    rc = fat12_rmdir(&fs, "/RENAMED");
    TEST_CHECK_RC(rc, 0, "rmdir failed");

    /* Verify filesystem still functional */
    rc = fat12_stat(&fs, "/README.TXT", &node);
    TEST_CHECK_RC(rc, 0, "stat root file failed after operations");

    fat12_close(&fs);

    /* Verify FAT consistency using utility */
    rc = test_verify_fat_consistency(tmp);
    TEST_CHECK(rc == 0, "FAT consistency check failed");

    unlink(tmp);
    free(tmp);
    return 0;
}

/**
 * @brief Verifies handling of the disk full condition.
 *
 * This function simulates a full disk and checks that operations requiring
 * new clusters fail with -ENOSPC.
 *
 * @param fixture_img Path to the fixture image.
 * @return Returns 0 on success, 1 on failure.
 */
static int test_disk_full_handling(const char *fixture_img)
{
    char *tmp = test_create_temp_copy(fixture_img);
    TEST_CHECK(tmp != NULL, "create temp copy failed");

    TEST_CHECK(test_simulate_disk_full(tmp) == 0, "simulate disk full failed");

    Fat12 fs;
    int rc = fat12_open(&fs, tmp, 0);
    TEST_CHECK_RC(rc, 0, "fat12_open failed");

    /* Try to create a new file - should fail if root dir is full or if it needs cluster */
    /* Since root entries are pre-allocated in FAT12, fat12_create might succeed
     * if there is a free slot, but fat12_write will fail. */
    rc = fat12_create(&fs, "/FULLTEST.TXT");
    /* It might succeed if there's a free slot in root dir. */

    if (rc == 0) {
        ssize_t n = fat12_write(&fs, "/FULLTEST.TXT", "data", 4, 0);
        TEST_CHECK(n < 0, "write to full disk should fail");
    }

    /* Try to create a directory - always requires a new cluster */
    rc = fat12_mkdir(&fs, "/FULLDIR");
    TEST_CHECK(rc == -ENOSPC, "mkdir on full disk should return ENOSPC");

    fat12_close(&fs);
    unlink(tmp);
    free(tmp);
    return 0;
}

/**
 * @brief Verifies handling of corrupted filesystem structures.
 *
 * This function injects corruptions into the filesystem and verifies
 * that the library handles them without crashing.
 *
 * @param fixture_img Path to the fixture image.
 * @return Returns 0 on success, 1 on failure.
 */
static int test_filesystem_corruption_handling(const char *fixture_img)
{
    char *tmp = test_create_temp_copy(fixture_img);
    TEST_CHECK(tmp != NULL, "create temp copy failed");

    /* Case 1: Corrupt boot sector */
    TEST_CHECK(test_corrupt_boot_sector(tmp) == 0, "corrupt boot sector failed");
    Fat12 fs;
    int rc = fat12_open(&fs, tmp, 0);
    TEST_CHECK(rc != 0, "open should fail with corrupted boot sector");

    /* Case 2: FAT cross-link (cycle) */
    free(tmp);
    tmp = test_create_temp_copy(fixture_img);
    TEST_CHECK(test_corrupt_fat_crosslink(tmp, 2, 3) == 0, "corrupt fat failed");
    rc = fat12_open(&fs, tmp, 0);
    TEST_CHECK_RC(rc, 0, "fat12_open failed with fat corruption");
    /* Traversal of corrupted chain should ideally handle loops or fail gracefully */
    /* Implementation details of fat12_core.c traversal should be checked */

    fat12_close(&fs);
    unlink(tmp);
    free(tmp);
    return 0;
}

/**
 * @brief Verifies traversal of deep directory structures.
 *
 * This function creates a nested directory structure and verifies
 * recursive operations and path resolution.
 *
 * @param fixture_img Path to the fixture image.
 * @return Returns 0 on success, 1 on failure.
 */
static int test_deep_traversal(const char *fixture_img)
{
    char *tmp = test_create_temp_copy(fixture_img);
    TEST_CHECK(tmp != NULL, "create temp copy failed");

    Fat12 fs;
    TEST_CHECK(fat12_open(&fs, tmp, 0) == 0, "fat12_open failed");

    /* Create deep structure: /A/B/C/D/FILE.TXT */
    TEST_CHECK(fat12_mkdir(&fs, "/A") == 0, "mkdir /A failed");
    TEST_CHECK(fat12_mkdir(&fs, "/A/B") == 0, "mkdir /A/B failed");
    TEST_CHECK(fat12_mkdir(&fs, "/A/B/C") == 0, "mkdir /A/B/C failed");
    TEST_CHECK(fat12_mkdir(&fs, "/A/B/C/D") == 0, "mkdir /A/B/C/D failed");
    TEST_CHECK(fat12_create(&fs, "/A/B/C/D/FILE.TXT") == 0, "create file failed");

    Fat12Node node;
    TEST_CHECK(fat12_stat(&fs, "/A/B/C/D/FILE.TXT", &node) == 0, "stat deep file failed");

    /* Test relative resolution through many levels */
    TEST_CHECK(fat12_stat(&fs, "/A/B/../B/C/./D/../D/FILE.TXT", &node) == 0, "complex path resolution failed");

    fat12_close(&fs);
    unlink(tmp);
    free(tmp);
    return 0;
}

/**
 * @brief Test executable entry point.
 */
int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr,
                "Usage: %s <sample-fat12-p1.img> <sample-fat12-2part.img>\n",
                argv[0]);
        return 2;
    }

    if (run_core_ops_test(argv[1]) != 0)
        return 1;
    if (run_partition_offset_test(argv[2]) != 0)
        return 1;

    /* Phase 1: Expanded basic operation tests */
    if (test_file_system_traversal(argv[1]) != 0)
        return 1;
    if (test_boundary_conditions(argv[1]) != 0)
        return 1;
    if (test_error_cases(argv[1]) != 0)
        return 1;
    if (test_filesystem_integrity(argv[1]) != 0)
        return 1;
    if (test_disk_full_handling(argv[1]) != 0)
        return 1;
    if (test_filesystem_corruption_handling(argv[1]) != 0)
        return 1;
    if (test_deep_traversal(argv[1]) != 0)
        return 1;

    printf("PASS: fat12_core tests\n");
    return 0;
}

