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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static int root_list_cb(const char *name, int is_dir, uint32_t size, void *user)
{
    (void)size;
    RootScan *scan = (RootScan *)user;
    if (strcmp(name, "README.TXT") == 0)
        scan->found_readme = 1;
    if (strcmp(name, "HELLO.TXT") == 0)
        scan->found_hello = 1;
    if (strcmp(name, "TESTDIR") == 0 && is_dir)
        scan->found_testdir = 1;
    return 0;
}

#define CHECK_RC(rc, msg)                                                   \
    do {                                                                    \
        if ((rc) != 0) {                                                    \
            fprintf(stderr, "FAIL: %s (rc=%d, line %d)\n", (msg), (int)(rc), __LINE__); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/**
 * @brief Validate primary core operations on a temporary writable image copy.
 */
static int run_core_ops_test(const char *fixture_p1_img)
{
    char tmp_template[256];
    snprintf(tmp_template, sizeof(tmp_template), "fat12-core-test-%d.img", (int)getpid());

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

    printf("PASS: fat12_core tests\n");
    return 0;
}
