/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file utils.c
 * @brief Test utilities for FAT12 test suite.
 */

#define _GNU_SOURCE
#include "utils.h"
#include "../fat12_core.h"

#ifdef _WIN32
#include <process.h>
#ifndef fseeko
#define fseeko _fseeki64
#endif
#ifndef ftello
#define ftello _ftelli64
#endif
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

/* Internal structures matching fat12_core.c */
#pragma pack(push, 1)
typedef struct {
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} BootSector;

typedef struct {
    uint8_t name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} DirEntry;
#pragma pack(pop)

/**
 * @brief Reads bytes from an absolute offset.
 * @param fp Open file pointer.
 * @param off Byte offset.
 * @param buf Destination buffer.
 * @param len Number of bytes to read.
 * @return Returns 0 on success, -1 on failure.
 */
static int read_at(FILE *fp, uint64_t off, void *buf, size_t len)
{
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0)
        return -1;
    return fread(buf, 1, len, fp) == len ? 0 : -1;
}

/**
 * @brief Writes bytes at an absolute offset.
 * @param fp Open file pointer.
 * @param off Byte offset.
 * @param buf Source buffer.
 * @param len Number of bytes to write.
 * @return Returns 0 on success, -1 on failure.
 */
static int write_at(FILE *fp, uint64_t off, const void *buf, size_t len)
{
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0)
        return -1;
    return fwrite(buf, 1, len, fp) == len ? 0 : -1;
}

#if defined(_WIN32) && !defined(__MINGW32__)
static int asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = _vscprintf(fmt, ap);
    if (len < 0) return -1;
    *strp = (char *)malloc(len + 1);
    if (!*strp) return -1;
    int r = vsprintf(*strp, fmt, ap);
    va_end(ap);
    return r;
}
#endif

/**
 * @brief Calculates the number of sectors required for the FAT.
 * @param total_clusters Total data clusters.
 * @param sector_size Bytes per sector.
 * @return Returns the FAT size in sectors.
 */
static uint32_t calculate_fat_sectors(uint32_t total_clusters, uint16_t sector_size)
{
    /* Each cluster needs 1.5 bytes in FAT12, plus 2 reserved entries */
    uint32_t fat_entries = total_clusters + 2;
    uint32_t fat_size_bytes = (fat_entries * 3 + 1) / 2; /* ceil(fat_entries * 1.5) */
    return (fat_size_bytes + sector_size - 1) / sector_size;
}

int test_create_fat12_image(const char *path, int size_mb, int cluster_sectors)
{
    const uint16_t sector_size = 512;
    uint32_t total_sectors = (size_mb * 1024 * 1024 + sector_size - 1) / sector_size;

    /* FAT12 limit: < 4085 clusters */
    uint32_t total_clusters = total_sectors / cluster_sectors;
    if (total_clusters >= 4085) {
        fprintf(stderr, "Error: FAT12 limited to < 4085 clusters\n");
        return -1;
    }

    uint16_t reserved_sectors = 1;
    uint8_t fat_count = 2;
    uint16_t root_entries = 224;
    uint8_t media_byte = (size_mb <= 1) ? 0xF0 : 0xF8; /* Floppy vs fixed */

    uint16_t sectors_per_fat = calculate_fat_sectors(total_clusters, sector_size);
    uint16_t root_dir_sectors = (root_entries * 32 + sector_size - 1) / sector_size;

    /* Create boot sector */
    BootSector boot = {0};
    boot.jmp[0] = 0xEB;
    boot.jmp[1] = 0x3C;
    boot.jmp[2] = 0x90;
    memcpy(boot.oem, "FAT12GEN", 8);
    boot.bytes_per_sector = sector_size;
    boot.sectors_per_cluster = cluster_sectors;
    boot.reserved_sectors = reserved_sectors;
    boot.fat_count = fat_count;
    boot.root_entry_count = root_entries;

    if (total_sectors < 65536) {
        boot.total_sectors_16 = total_sectors;
        boot.total_sectors_32 = 0;
    } else {
        boot.total_sectors_16 = 0;
        boot.total_sectors_32 = total_sectors;
    }

    boot.media = media_byte;
    boot.sectors_per_fat = sectors_per_fat;
    boot.sectors_per_track = 32;   /* dummy */
    boot.heads = 64;               /* dummy */
    boot.hidden_sectors = 0;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    /* Write boot sector */
    if (fwrite(&boot, sizeof(boot), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    /* Pad to sector size */
    uint8_t zero = 0;
    for (size_t i = sizeof(boot); i < sector_size; i++) {
        if (fwrite(&zero, 1, 1, fp) != 1) {
            fclose(fp);
            return -1;
        }
    }

    /* Create FAT */
    size_t fat_size_bytes = sectors_per_fat * sector_size;
    uint8_t *fat = calloc(1, fat_size_bytes);
    if (!fat) {
        fclose(fp);
        return -1;
    }

    /* FAT[0] = media byte, FAT[1] = 0xFF */
    fat[0] = media_byte;
    fat[1] = 0xFF;
    fat[2] = 0xFF;  /* FAT12: first 3 bytes contain first 2 entries */

    /* Write FAT copies */
    for (int i = 0; i < fat_count; i++) {
        if (fwrite(fat, 1, fat_size_bytes, fp) != fat_size_bytes) {
            free(fat);
            fclose(fp);
            return -1;
        }
    }

    free(fat);

    /* Write root directory (zero-filled) */
    size_t root_size = root_dir_sectors * sector_size;
    uint8_t *root_zeros = calloc(1, root_size);
    if (!root_zeros) {
        fclose(fp);
        return -1;
    }
    if (fwrite(root_zeros, 1, root_size, fp) != root_size) {
        free(root_zeros);
        fclose(fp);
        return -1;
    }
    free(root_zeros);

    /* Write data area (zero-filled) */
    uint64_t current_pos = ftell(fp);
    uint64_t target_size = (uint64_t)total_sectors * sector_size;
    if (target_size > current_pos) {
        size_t data_size = target_size - current_pos;
        uint8_t *data_zeros = calloc(1, data_size);
        if (!data_zeros) {
            fclose(fp);
            return -1;
        }
        if (fwrite(data_zeros, 1, data_size, fp) != data_size) {
            free(data_zeros);
            fclose(fp);
            return -1;
        }
        free(data_zeros);
    }

    fclose(fp);
    return 0;
}

int test_create_standard_fat12_image(const char *path)
{
    /* Create a 6MB image (similar to sample-fat12-p1.img) */
    if (test_create_fat12_image(path, 6, 4) != 0)
        return -1;

    Fat12 fs;
    if (fat12_open(&fs, path, 0) != 0)
        return -1;

    /* Create standard structure */
    fat12_create(&fs, "/README.TXT");
    const char *readme = "This is a standard test image.\n";
    fat12_write(&fs, "/README.TXT", readme, strlen(readme), 0);

    fat12_create(&fs, "/HELLO.TXT");
    const char *hello = "hello from p1\n";
    fat12_write(&fs, "/HELLO.TXT", hello, strlen(hello), 0);

    fat12_mkdir(&fs, "/TESTDIR");

    fat12_close(&fs);
    return 0;
}

int test_simulate_disk_full(const char *fs_path)
{
    FILE *fp = fopen(fs_path, "rb+");
    if (!fp)
        return -1;

    BootSector boot;
    if (read_at(fp, 0, &boot, sizeof(boot)) != 0) {
        fclose(fp);
        return -1;
    }

    uint16_t sector_size = boot.bytes_per_sector;
    uint16_t sectors_per_fat = boot.sectors_per_fat;
    uint8_t fat_count = boot.fat_count;
    uint64_t fat_offset = (uint64_t)boot.reserved_sectors * sector_size;

    size_t fat_size_bytes = (size_t)sectors_per_fat * sector_size;
    uint8_t *fat = malloc(fat_size_bytes);
    if (!fat) {
        fclose(fp);
        return -1;
    }

    /* Read first FAT */
    if (read_at(fp, fat_offset, fat, fat_size_bytes) != 0) {
        free(fat);
        fclose(fp);
        return -1;
    }

    /* Mark all clusters as allocated (0xFFF) */
    uint32_t total_clusters = (boot.total_sectors_16 ? boot.total_sectors_16 : boot.total_sectors_32)
                               / boot.sectors_per_cluster;

    for (uint32_t cluster = 2; cluster < total_clusters + 2; cluster++) {
        uint32_t fat_index = cluster + cluster / 2;
        if (fat_index + 1 >= fat_size_bytes) break;
        if (cluster & 1) {
            /* Odd cluster: high 12 bits of 16-bit word */
            fat[fat_index] = (uint8_t)((fat[fat_index] & 0x0F) | 0xF0);
            fat[fat_index + 1] = 0xFF;
        } else {
            /* Even cluster: low 12 bits of 16-bit word */
            fat[fat_index] = 0xFF;
            fat[fat_index + 1] = (uint8_t)((fat[fat_index + 1] & 0xF0) | 0x0F);
        }
    }

    /* Write back all FAT copies */
    for (int i = 0; i < fat_count; i++) {
        if (write_at(fp, fat_offset + (uint64_t)i * fat_size_bytes, fat, fat_size_bytes) != 0) {
            free(fat);
            fclose(fp);
            return -1;
        }
    }

    free(fat);
    fclose(fp);
    return 0;
}

int test_corrupt_boot_sector(const char *fs_path)
{
    FILE *fp = fopen(fs_path, "rb+");
    if (!fp)
        return -1;

    /* Write invalid bytes per sector (0) */
    uint16_t zero = 0;
    if (write_at(fp, 11, &zero, sizeof(zero)) != 0) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/**
 * @brief Internal helper to set a 12-bit FAT entry in a buffer.
 * @param fat Pointer to the FAT buffer.
 * @param fat_size Size of the FAT buffer in bytes.
 * @param cluster Cluster index to update.
 * @param value 12-bit value to set.
 */
static void set_fat_entry_raw(uint8_t *fat, size_t fat_size, uint16_t cluster, uint16_t value) {
    uint32_t off = cluster + cluster / 2;
    if (off + 1 >= fat_size) return;
    value &= 0x0FFF;
    if ((cluster & 1) == 0) {
        fat[off] = (uint8_t)(value & 0xFF);
        fat[off + 1] = (uint8_t)((fat[off + 1] & 0xF0) | ((value >> 8) & 0x0F));
    } else {
        fat[off] = (uint8_t)((fat[off] & 0x0F) | ((value << 4) & 0xF0));
        fat[off + 1] = (uint8_t)((value >> 4) & 0xFF);
    }
}

/**
 * @brief Utility callback for counting directory entries.
 * @param name Entry name.
 * @param is_dir Whether the entry is a directory.
 * @param size File size.
 * @param user Pointer to an integer counter.
 * @return Returns 0 to continue enumeration.
 */
static int count_cb(const char *name, const Fat12Node *node, void *user) {
    (void)name; (void)node;
    (*(int *)user)++;
    return 0;
}

int test_corrupt_fat_crosslink(const char *fs_path, uint16_t cluster1, uint16_t cluster2)
{
    FILE *fp = fopen(fs_path, "rb+");
    if (!fp)
        return -1;

    BootSector boot;
    if (read_at(fp, 0, &boot, sizeof(boot)) != 0) {
        fclose(fp);
        return -1;
    }

    uint16_t sector_size = boot.bytes_per_sector;
    uint16_t sectors_per_fat = boot.sectors_per_fat;
    uint64_t fat_offset = (uint64_t)boot.reserved_sectors * sector_size;

    size_t fat_size_bytes = (size_t)sectors_per_fat * sector_size;
    uint8_t *fat = malloc(fat_size_bytes);
    if (!fat) {
        fclose(fp);
        return -1;
    }

    if (read_at(fp, fat_offset, fat, fat_size_bytes) != 0) {
        free(fat);
        fclose(fp);
        return -1;
    }

    /* Cross-link: cluster1 points to cluster2, cluster2 points to cluster1 */
    set_fat_entry_raw(fat, fat_size_bytes, cluster1, cluster2);
    set_fat_entry_raw(fat, fat_size_bytes, cluster2, cluster1);

    for (int i = 0; i < boot.fat_count; i++) {
        if (write_at(fp, fat_offset + (uint64_t)i * fat_size_bytes, fat, fat_size_bytes) != 0) {
            free(fat);
            fclose(fp);
            return -1;
        }
    }

    free(fat);
    fclose(fp);
    return 0;
}

int test_corrupt_directory_entry(const char *fs_path, const char *path)
{
    Fat12 fs;
    if (fat12_open(&fs, fs_path, 0) != 0)
        return -1;

    /* For now, just corrupt the first root entry if path is "/" or something. */
    (void)path;
    uint64_t off = fs.root_offset;
    uint8_t corrupt_name[11] = "CORRUPTED  ";
    if (write_at(fs.fp, off, corrupt_name, 11) != 0) {
        fat12_close(&fs);
        return -1;
    }

    fat12_close(&fs);
    return 0;
}

char *test_create_temp_copy(const char *src_path)
{
    char *tmp_path = NULL;
    if (asprintf(&tmp_path, "fat12-test-%d.img", (int)getpid()) < 0)
        return NULL;

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        free(tmp_path);
        return NULL;
    }

    FILE *dst = fopen(tmp_path, "wb");
    if (!dst) {
        fclose(src);
        free(tmp_path);
        return NULL;
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            unlink(tmp_path);
            free(tmp_path);
            return NULL;
        }
    }

    fclose(src);
    fclose(dst);
    return tmp_path;
}

int test_verify_fat_consistency(const char *fs_path)
{
    FILE *fp = fopen(fs_path, "rb");
    if (!fp)
        return -1;

    BootSector boot;
    if (read_at(fp, 0, &boot, sizeof(boot)) != 0) {
        fclose(fp);
        return -1;
    }

    uint16_t sector_size = boot.bytes_per_sector;
    uint16_t sectors_per_fat = boot.sectors_per_fat;
    uint8_t fat_count = boot.fat_count;
    uint64_t fat_offset = (uint64_t)boot.reserved_sectors * sector_size;
    size_t fat_size_bytes = (size_t)sectors_per_fat * sector_size;

    if (fat_count < 1) {
        fclose(fp);
        return -1;
    }

    uint8_t *fat1 = malloc(fat_size_bytes);
    if (!fat1) {
        fclose(fp);
        return -1;
    }
    if (read_at(fp, fat_offset, fat1, fat_size_bytes) != 0) {
        free(fat1);
        fclose(fp);
        return -1;
    }

    for (int i = 1; i < fat_count; i++) {
        uint8_t *fati = malloc(fat_size_bytes);
        if (!fati) {
            free(fat1);
            fclose(fp);
            return -1;
        }
        if (read_at(fp, fat_offset + (uint64_t)i * fat_size_bytes, fati, fat_size_bytes) != 0) {
            free(fat1);
            free(fati);
            fclose(fp);
            return -1;
        }
        if (memcmp(fat1, fati, fat_size_bytes) != 0) {
            free(fat1);
            free(fati);
            fclose(fp);
            return -1; /* FAT copies differ */
        }
        free(fati);
    }

    free(fat1);
    fclose(fp);
    return 0;
}

int test_verify_directory_integrity(const char *fs_path)
{
    /* Basic check: root directory can be opened and listed */
    Fat12 fs;
    if (fat12_open(&fs, fs_path, 0) != 0)
        return -1;

    int count = 0;
    if (fat12_list(&fs, "/", count_cb, &count) != 0) {
        fat12_close(&fs);
        return -1;
    }

    fat12_close(&fs);
    return 0;
}

int test_count_free_clusters(const char *fs_path)
{
    Fat12 fs;
    if (fat12_open(&fs, fs_path, 0) != 0)
        return -1;

    int free_clusters = 0;
    for (uint16_t i = 2; i < fs.total_clusters + 2; i++) {
        uint32_t off = i + i / 2;
        uint16_t v;
        if ((i & 1) == 0) {
            v = fs.fat[off] | ((fs.fat[off + 1] & 0x0F) << 8);
        } else {
            v = (uint16_t)((fs.fat[off] >> 4) | (fs.fat[off + 1] << 4));
        }
        if ((v & 0x0FFF) == 0) {
            free_clusters++;
        }
    }

    fat12_close(&fs);
    return free_clusters;
}
