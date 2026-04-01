/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file fat12_core.c
 * @brief Implementation of FAT12 core path-based filesystem operations.
 *
 * The implementation maintains an in-memory FAT copy for fast entry updates,
 * flushing to all FAT replicas after mutating operations. It intentionally
 * targets FAT12 short-name semantics and is shared by both CLI and FUSE
 * frontends.
 */
#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64

#include "fat12_core.h"

#ifdef _WIN32
#ifndef fseeko
#define fseeko _fseeki64
#endif
#ifndef ftello
#define ftello _ftelli64
#endif
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define FAT12_EOC 0x0FF8 /**< End of chain marker. */

#pragma pack(push, 1)
/**
 * @brief Represents the raw FAT12 boot sector layout.
 */
typedef struct {
    uint8_t jmp[3];              /**< Jump instruction. */
    uint8_t oem[8];              /**< OEM string. */
    uint16_t bytes_per_sector;   /**< Bytes per logical sector. */
    uint8_t sectors_per_cluster; /**< Sectors per cluster. */
    uint16_t reserved_sectors;   /**< Number of reserved sectors. */
    uint8_t fat_count;           /**< Number of FATs. */
    uint16_t root_entry_count;   /**< Root directory entry count. */
    uint16_t total_sectors_16;   /**< Total sectors (16-bit). */
    uint8_t media;               /**< Media descriptor. */
    uint16_t sectors_per_fat;    /**< Sectors per FAT. */
    uint16_t sectors_per_track;  /**< Sectors per track. */
    uint16_t heads;              /**< Number of heads. */
    uint32_t hidden_sectors;     /**< Hidden sectors. */
    uint32_t total_sectors_32;   /**< Total sectors (32-bit). */
} BootSector;

/**
 * @brief Represents a raw FAT directory entry.
 */
typedef struct {
    uint8_t name[11];          /**< Short name (8.3). */
    uint8_t attr;              /**< Attributes. */
    uint8_t ntres;             /**< Reserved. */
    uint8_t crt_time_tenth;    /**< Creation time (tenths). */
    uint16_t crt_time;         /**< Creation time. */
    uint16_t crt_date;         /**< Creation date. */
    uint16_t acc_date;         /**< Last access date. */
    uint16_t first_cluster_hi; /**< High word of first cluster. */
    uint16_t wrt_time;         /**< Last write time. */
    uint16_t wrt_date;         /**< Last write date. */
    uint16_t first_cluster_lo; /**< Low word of first cluster. */
    uint32_t file_size;        /**< File size in bytes. */
} DirEntry;
#pragma pack(pop)

/**
 * @brief Holds a reference to a directory entry and its location.
 */
typedef struct {
    DirEntry entry;  /**< Entry payload. */
    uint64_t offset; /**< Absolute byte offset. */
    int found;       /**< Non-zero if entry exists. */
} EntryRef;

#pragma pack(push, 1)
/**
 * @brief Represents an MBR partition table entry.
 */
typedef struct {
    uint8_t status;        /**< Partition status. */
    uint8_t chs_first[3];  /**< CHS start. */
    uint8_t type;          /**< Partition type. */
    uint8_t chs_last[3];   /**< CHS end. */
    uint32_t lba_start;    /**< LBA start. */
    uint32_t sector_count; /**< Sector count. */
} MbrPartition;
#pragma pack(pop)

/* Internal forward declarations */
static int read_at(FILE *fp, uint64_t off, void *buf, size_t len);
static int write_at(FILE *fp, uint64_t off, const void *buf, size_t len);
static uint64_t cluster_to_offset(const Fat12 *fs, uint16_t cluster);
static int flush_fat(Fat12 *fs);
static uint16_t fat_get(const Fat12 *fs, uint16_t cluster);
static void fat_set(Fat12 *fs, uint16_t cluster, uint16_t value);
static int is_eoc(uint16_t v);
static int alloc_cluster(Fat12 *fs, uint16_t *out);
static int free_chain(Fat12 *fs, uint16_t first);
static int dir_parent_cluster(Fat12 *fs, uint16_t cluster, uint16_t *parent);
static int find_in_dir(Fat12 *fs, uint16_t dir_cluster,
        const uint8_t name83[11], EntryRef *out);
static int read_dir_entry_at(Fat12 *fs, uint64_t off, DirEntry *e);
static void short_name_to_str(const uint8_t in[11], char *out, size_t n);
static int to_short_name(const char *in, uint8_t out[11]);
static void fat_time_date(uint16_t *out_time, uint16_t *out_date);
static int write_dir_entry_at(Fat12 *fs, uint64_t off, const DirEntry *e);

/*
 * @brief Parses MBR partition table and returns selected partition offset.
 *
 * @param img Path to disk image file.
 * @param partition_idx 1-based partition index; <=0 means offset 0.
 * @param offset_out Output byte offset for FAT volume.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_parse_partition_offset(
        const char *img, int partition_idx, uint64_t *offset_out)
{
    *offset_out = 0;
    if (partition_idx <= 0)
        return 0;

    FILE *fp = fopen(img, "rb");
    if (!fp)
        return -errno;
    uint8_t mbr[512];
    if (fread(mbr, 1, sizeof(mbr), fp) != sizeof(mbr)) {
        fclose(fp);
        return -EIO;
    }
    fclose(fp);

    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return -EINVAL;
    if (partition_idx > 4)
        return -EINVAL;

    MbrPartition p;
    memcpy(&p, mbr + 446 + (partition_idx - 1) * 16, sizeof(p));
    if (p.sector_count == 0)
        return -ENOENT;

    *offset_out = (uint64_t)p.lba_start * 512ULL;
    return 0;
}

/**
 * @brief Reads bytes from an absolute image offset.
 * @param fp Open image file handle.
 * @param off Absolute byte offset within the image.
 * @param buf Destination buffer.
 * @param len Number of bytes to read.
 * @return 0 on success, -1 on I/O failure.
 */
static int read_at(FILE *fp, uint64_t off, void *buf, size_t len)
{
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0)
        return -1;
    if (fread(buf, 1, len, fp) != len)
        return -1;
    return 0;
}

/**
 * @brief Writes bytes to an absolute image offset.
 * @param fp Open image file handle.
 * @param off Absolute byte offset within the image.
 * @param buf Source buffer.
 * @param len Number of bytes to write.
 * @return 0 on success, -1 on I/O failure.
 */
static int write_at(FILE *fp, uint64_t off, const void *buf, size_t len)
{
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0)
        return -1;
    if (fwrite(buf, 1, len, fp) != len)
        return -1;
    return 0;
}

/**
 * @brief Converts a cluster number to an absolute byte offset.
 * @param fs Open FAT12 context.
 * @param cluster FAT cluster number (>=2 for data clusters).
 * @return Absolute byte offset of cluster start.
 */
static uint64_t cluster_to_offset(const Fat12 *fs, uint16_t cluster)
{
    return fs->data_offset + (uint64_t)(cluster - 2) * fs->cluster_size;
}

/**
 * @brief Flushes in-memory FAT content to all on-disk FAT replicas.
 * @param fs Open FAT12 context.
 * @return 0 on success, -1 on write failure.
 */
static int flush_fat(Fat12 *fs)
{
    for (uint8_t i = 0; i < fs->bpb.fat_count; ++i) {
        uint64_t off = fs->fat_offset + (uint64_t)i * fs->fat_size_bytes;
        if (write_at(fs->fp, off, fs->fat, fs->fat_size_bytes) != 0)
            return -1;
    }
    fflush(fs->fp);
    return 0;
}

/**
 * @brief Reads a 12-bit FAT table entry.
 * @param fs Open FAT12 context.
 * @param cluster Cluster index to inspect.
 * @return FAT12 entry value (masked to 12 bits).
 */
static uint16_t fat_get(const Fat12 *fs, uint16_t cluster)
{
    uint32_t off = cluster + cluster / 2;
    uint16_t v;
    if ((cluster & 1) == 0) {
        v = fs->fat[off] | ((fs->fat[off + 1] & 0x0F) << 8);
    } else {
        v = (uint16_t)((fs->fat[off] >> 4) | (fs->fat[off + 1] << 4));
    }
    return v & 0x0FFF;
}

/**
 * @brief Writes a 12-bit FAT table entry.
 * @param fs Open FAT12 context.
 * @param cluster Cluster index to update.
 * @param value New FAT12 value (lower 12 bits are used).
 */
static void fat_set(Fat12 *fs, uint16_t cluster, uint16_t value)
{
    uint32_t off = cluster + cluster / 2;
    value &= 0x0FFF;
    if ((cluster & 1) == 0) {
        fs->fat[off] = (uint8_t)(value & 0xFF);
        fs->fat[off + 1] =
                (uint8_t)((fs->fat[off + 1] & 0xF0) | ((value >> 8) & 0x0F));
    } else {
        fs->fat[off] = (uint8_t)((fs->fat[off] & 0x0F) | ((value << 4) & 0xF0));
        fs->fat[off + 1] = (uint8_t)((value >> 4) & 0xFF);
    }
}

/**
 * @brief Tests whether a FAT entry value is an end-of-chain marker.
 * @param v FAT12 entry value.
 * @return Non-zero when value is EOC, otherwise zero.
 */
static int is_eoc(uint16_t v)
{
    return v >= FAT12_EOC;
}

/**
 * @brief Converts text filename to FAT 8.3 raw name bytes.
 * @param in Input file name (`NAME.EXT` style).
 * @param out Output 11-byte FAT short name buffer.
 * @return 0 on success, -1 when the name is invalid for 8.3 format.
 */
static int to_short_name(const char *in, uint8_t out[11])
{
    memset(out, ' ', 11);
    const char *dot = strrchr(in, '.');
    size_t name_len;
    size_t ext_len = 0;

    if (!in || !*in || strcmp(in, ".") == 0 || strcmp(in, "..") == 0)
        return -1;

    if (dot && dot != in) {
        name_len = (size_t)(dot - in);
        ext_len = strlen(dot + 1);
    } else {
        name_len = strlen(in);
    }

    if (name_len < 1 || name_len > 8 || ext_len > 3)
        return -1;

    for (size_t i = 0; i < name_len; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c <= 0x20 || strchr("\"*+,./:;<=>?[\\]|", c))
            return -1;
        out[i] = (uint8_t)toupper(c);
    }

    if (ext_len > 0) {
        for (size_t i = 0; i < ext_len; ++i) {
            unsigned char c = (unsigned char)dot[1 + i];
            if (c <= 0x20 || strchr("\"*+,./:;<=>?[\\]|", c))
                return -1;
            out[8 + i] = (uint8_t)toupper(c);
        }
    }

    return 0;
}

/**
 * @brief Converts FAT 8.3 raw bytes into printable name text.
 * @param in Input 11-byte FAT short name.
 * @param out Destination character buffer.
 * @param n Destination capacity in bytes.
 */
static void short_name_to_str(const uint8_t in[11], char *out, size_t n)
{
    char name[9];
    char ext[4];
    size_t ni = 0;
    size_t ei = 0;
    for (int i = 0; i < 8 && in[i] != ' '; ++i)
        name[ni++] = (char)in[i];
    for (int i = 0; i < 3 && in[8 + i] != ' '; ++i)
        ext[ei++] = (char)in[8 + i];
    name[ni] = '\0';
    ext[ei] = '\0';
    if (ei > 0)
        snprintf(out, n, "%s.%s", name, ext);
    else
        snprintf(out, n, "%s", name);
}

/**
 * @brief Reads one directory entry from the image.
 * @param fs Open FAT12 context.
 * @param off Absolute byte offset of the directory entry.
 * @param e Output entry structure.
 * @return 0 on success, -1 on read failure.
 */
static int read_dir_entry_at(Fat12 *fs, uint64_t off, DirEntry *e)
{
    return read_at(fs->fp, off, e, sizeof(*e));
}

/**
 * @brief Writes one directory entry to the image.
 * @param fs Open FAT12 context.
 * @param off Absolute byte offset of the directory entry.
 * @param e Entry payload to write.
 * @return 0 on success, -1 on write failure.
 */
static int write_dir_entry_at(Fat12 *fs, uint64_t off, const DirEntry *e)
{
    if (write_at(fs->fp, off, e, sizeof(*e)) != 0)
        return -1;
    fflush(fs->fp);
    return 0;
}

/**
 * @brief Converts `time_t` to FAT date/time fields.
 * @param t_in Time to encode.
 * @param out_time Output FAT time field.
 * @param out_date Output FAT date field.
 */
void fat12_time_t_to_fat(time_t t_in, uint16_t *out_time, uint16_t *out_date)
{
    struct tm *tmv = localtime(&t_in);
    uint16_t t = 0;
    uint16_t d = 0;
    if (tmv) {
        t = (uint16_t)(((tmv->tm_hour & 0x1F) << 11) |
                ((tmv->tm_min & 0x3F) << 5) | ((tmv->tm_sec / 2) & 0x1F));
        int year = tmv->tm_year + 1900;
        if (year < 1980)
            year = 1980;
        d = (uint16_t)(((year - 1980) << 9) | ((tmv->tm_mon + 1) << 5) |
                (tmv->tm_mday & 0x1F));
    }
    *out_time = t;
    *out_date = d;
}

/**
 * @brief Converts FAT date/time fields to a standard `time_t`.
 * @param fat_time FAT encoded time.
 * @param fat_date FAT encoded date.
 * @return `time_t` representation.
 */
time_t fat12_fat_to_time_t(uint16_t fat_time, uint16_t fat_date)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));

    int year = ((fat_date >> 9) & 0x7F) + 1980;
    int mon = (fat_date >> 5) & 0x0F;
    int day = fat_date & 0x1F;
    int hour = (fat_time >> 11) & 0x1F;
    int min = (fat_time >> 5) & 0x3F;
    int sec = (fat_time & 0x1F) * 2;

    if (mon < 1)
        mon = 1;
    if (day < 1)
        day = 1;

    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1;

    return mktime(&tmv);
}

/**
 * @brief Encodes current local wall-clock time into FAT date/time fields.
 * @param out_time Output FAT time field.
 * @param out_date Output FAT date field.
 */
static void fat_time_date(uint16_t *out_time, uint16_t *out_date)
{
    fat12_time_t_to_fat(time(NULL), out_time, out_date);
}

/**
 * @brief Resolves a directory's parent cluster using its `..` entry.
 * @param fs Open FAT12 context.
 * @param cluster Directory cluster (0 for root).
 * @param parent Output parent cluster.
 * @return 0 on success, -1 on lookup failure.
 */
static int dir_parent_cluster(Fat12 *fs, uint16_t cluster, uint16_t *parent)
{
    if (cluster == 0) {
        *parent = 0;
        return 0;
    }
    uint64_t off = cluster_to_offset(fs, cluster) + sizeof(DirEntry);
    DirEntry dotdot;
    if (read_dir_entry_at(fs, off, &dotdot) != 0)
        return -1;
    *parent = dotdot.first_cluster_lo;
    return 0;
}

/**
 * @brief Looks up an 8.3 entry inside a directory.
 * @param fs Open FAT12 context.
 * @param dir_cluster Directory cluster (0 for root directory region).
 * @param name83 Target 11-byte short name.
 * @param out Result holder; `out->found` indicates whether match exists.
 * @return 0 on success, -1 on traversal failure.
 */
static int find_in_dir(Fat12 *fs, uint16_t dir_cluster,
        const uint8_t name83[11], EntryRef *out)
{
    memset(out, 0, sizeof(*out));

    if (dir_cluster == 0) {
        for (uint32_t i = 0; i < fs->bpb.root_entry_count; ++i) {
            uint64_t off = fs->root_offset + (uint64_t)i * sizeof(DirEntry);
            DirEntry e;
            if (read_dir_entry_at(fs, off, &e) != 0)
                return -1;
            if (e.name[0] == 0x00)
                break;
            if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
                    (e.attr & ATTR_VOLUME_ID))
                continue;
            if (memcmp(e.name, name83, 11) == 0) {
                out->entry = e;
                out->offset = off;
                out->found = 1;
                return 0;
            }
        }
        return 0;
    }

    uint16_t c = dir_cluster;
    while (c >= 2 && c < 0xFF7) {
        uint64_t base = cluster_to_offset(fs, c);
        uint32_t count = fs->cluster_size / sizeof(DirEntry);
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t off = base + (uint64_t)i * sizeof(DirEntry);
            DirEntry e;
            if (read_dir_entry_at(fs, off, &e) != 0)
                return -1;
            if (e.name[0] == 0x00)
                return 0;
            if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
                    (e.attr & ATTR_VOLUME_ID))
                continue;
            if (memcmp(e.name, name83, 11) == 0) {
                out->entry = e;
                out->offset = off;
                out->found = 1;
                return 0;
            }
        }
        uint16_t n = fat_get(fs, c);
        if (is_eoc(n))
            break;
        c = n;
    }

    return 0;
}

/**
 * @brief Finds a reusable directory slot (unused or deleted).
 * @param fs Open FAT12 context.
 * @param dir_cluster Directory cluster (0 for root).
 * @param off_out Output absolute offset of free slot.
 * @return 0 on success, -1 when no slot is available or on I/O error.
 */
static int find_free_entry_slot(
        Fat12 *fs, uint16_t dir_cluster, uint64_t *off_out)
{
    if (dir_cluster == 0) {
        for (uint32_t i = 0; i < fs->bpb.root_entry_count; ++i) {
            uint64_t off = fs->root_offset + (uint64_t)i * sizeof(DirEntry);
            uint8_t first;
            if (read_at(fs->fp, off, &first, 1) != 0)
                return -1;
            if (first == 0x00 || first == 0xE5) {
                *off_out = off;
                return 0;
            }
        }
        return -1;
    }

    uint16_t c = dir_cluster;
    uint16_t last = c;
    while (1) {
        uint64_t base = cluster_to_offset(fs, c);
        uint32_t count = fs->cluster_size / sizeof(DirEntry);
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t off = base + (uint64_t)i * sizeof(DirEntry);
            uint8_t first;
            if (read_at(fs->fp, off, &first, 1) != 0)
                return -1;
            if (first == 0x00 || first == 0xE5) {
                *off_out = off;
                return 0;
            }
        }
        last = c;
        uint16_t n = fat_get(fs, c);
        if (is_eoc(n))
            break;
        c = n;
    }

    /* Grow directory if possible */
    uint16_t next;
    if (alloc_cluster(fs, &next) != 0)
        return -1;

    fat_set(fs, last, next);
    if (flush_fat(fs) != 0)
        return -1;

    *off_out = cluster_to_offset(fs, next);
    return 0;
}

/**
 * @brief Resolves an absolute path for lookup or creation.
 * @param fs Open FAT12 context.
 * @param path Absolute path (must start with `/`).
 * @param out Resolved entry information (`found=0` when leaf does not exist).
 * @param parent_cluster_out Optional output parent directory cluster.
 * @param leaf_out Optional output final component text.
 * @param leaf_cap Capacity of `leaf_out`.
 * @return 0 on success, -1 on parse or traversal failure.
 */
static int resolve_abs_path(Fat12 *fs, const char *path, EntryRef *out,
        uint16_t *parent_cluster_out, char *leaf_out, size_t leaf_cap)
{
    char temp[1024];
    if (!path || path[0] != '/')
        return -1;
    if (strlen(path) >= sizeof(temp))
        return -1;
    strcpy(temp, path);

    uint16_t current = 0;
    char *save = NULL;
    char *token = strtok_r(temp, "/", &save);
    char *components[256];
    int count = 0;
    while (token && count < 256) {
        if (*token)
            components[count++] = token;
        token = strtok_r(NULL, "/", &save);
    }

    if (count == 0) {
        memset(out, 0, sizeof(*out));
        out->found = 1;
        out->entry.attr = ATTR_DIRECTORY;
        out->entry.first_cluster_lo = 0;
        if (parent_cluster_out)
            *parent_cluster_out = 0;
        if (leaf_out && leaf_cap > 0)
            leaf_out[0] = '\0';
        return 0;
    }

    for (int i = 0; i < count; ++i) {
        const char *part = components[i];
        if (strcmp(part, ".") == 0)
            continue;
        if (strcmp(part, "..") == 0) {
            uint16_t p;
            if (dir_parent_cluster(fs, current, &p) != 0)
                return -1;
            current = p;
            continue;
        }

        uint8_t n83[11];
        if (to_short_name(part, n83) != 0)
            return -1;
        EntryRef ref;
        if (find_in_dir(fs, current, n83, &ref) != 0)
            return -1;

        if (i == count - 1) {
            if (parent_cluster_out)
                *parent_cluster_out = current;
            if (leaf_out && leaf_cap > 0) {
                strncpy(leaf_out, part, leaf_cap - 1);
                leaf_out[leaf_cap - 1] = '\0';
            }
            *out = ref;
            return 0;
        }

        if (!ref.found || !(ref.entry.attr & ATTR_DIRECTORY))
            return -1;
        current = ref.entry.first_cluster_lo;
    }

    return -1;
}

/**
 * @brief Materialises a FAT cluster chain into a heap buffer.
 * @param fs Open FAT12 context.
 * @param first First cluster in chain (or <2 for empty chain).
 * @param arr Output pointer to allocated cluster array.
 * @param n Output cluster count.
 * @return 0 on success, -1 on allocation failure.
 */
static int collect_chain(Fat12 *fs, uint16_t first, uint16_t **arr, size_t *n)
{
    *arr = NULL;
    *n = 0;
    if (first < 2)
        return 0;

    size_t cap = 8;
    uint16_t *v = (uint16_t *)malloc(cap * sizeof(uint16_t));
    if (!v)
        return -1;

    uint16_t c = first;
    while (c >= 2 && c < 0xFF7) {
        if (*n == cap) {
            cap *= 2;
            uint16_t *nv = (uint16_t *)realloc(v, cap * sizeof(uint16_t));
            if (!nv) {
                free(v);
                return -1;
            }
            v = nv;
        }
        v[(*n)++] = c;
        uint16_t nx = fat_get(fs, c);
        if (is_eoc(nx))
            break;
        c = nx;
    }

    *arr = v;
    return 0;
}

/**
 * @brief Allocates one free cluster and initialises it to zero.
 * @param fs Open FAT12 context.
 * @param out Output allocated cluster number.
 * @return 0 on success, -1 when full or on I/O error.
 */
static int alloc_cluster(Fat12 *fs, uint16_t *out)
{
    for (uint16_t c = 2; c < fs->total_clusters + 2; ++c) {
        if (fat_get(fs, c) == 0x000) {
            fat_set(fs, c, 0xFFF);
            uint8_t *zero = (uint8_t *)calloc(1, fs->cluster_size);
            if (!zero)
                return -1;
            int ok = write_at(
                    fs->fp, cluster_to_offset(fs, c), zero, fs->cluster_size);
            free(zero);
            if (ok != 0)
                return -1;
            if (flush_fat(fs) != 0)
                return -1;
            *out = c;
            return 0;
        }
    }
    return -1;
}

/**
 * @brief Releases all clusters in a chain.
 * @param fs Open FAT12 context.
 * @param first First cluster in chain.
 * @return 0 on success, -1 on FAT flush failure.
 */
static int free_chain(Fat12 *fs, uint16_t first)
{
    uint16_t c = first;
    while (c >= 2 && c < 0xFF7) {
        uint16_t nx = fat_get(fs, c);
        fat_set(fs, c, 0x000);
        if (is_eoc(nx))
            break;
        c = nx;
    }
    return flush_fat(fs);
}

/**
 * @brief Resizes a cluster chain to an exact cluster count.
 * @param fs Open FAT12 context.
 * @param first_cluster In/out first cluster of the chain.
 * @param need_clusters Target chain length.
 * @return 0 on success, -1 on allocation or FAT update failure.
 */
static int ensure_chain_size(
        Fat12 *fs, uint16_t *first_cluster, size_t need_clusters)
{
    uint16_t *chain = NULL;
    size_t n = 0;
    if (collect_chain(fs, *first_cluster, &chain, &n) != 0)
        return -1;

    if (need_clusters == 0) {
        if (*first_cluster >= 2) {
            if (free_chain(fs, *first_cluster) != 0) {
                free(chain);
                return -1;
            }
        }
        *first_cluster = 0;
        free(chain);
        return 0;
    }

    if (n == 0) {
        uint16_t c;
        if (alloc_cluster(fs, &c) != 0) {
            free(chain);
            return -1;
        }
        *first_cluster = c;
        chain = (uint16_t *)malloc(sizeof(uint16_t));
        if (!chain)
            return -1;
        chain[0] = c;
        n = 1;
    }

    while (n < need_clusters) {
        uint16_t c;
        if (alloc_cluster(fs, &c) != 0) {
            free(chain);
            return -1;
        }
        fat_set(fs, chain[n - 1], c);
        fat_set(fs, c, 0xFFF);
        if (flush_fat(fs) != 0) {
            free(chain);
            return -1;
        }
        uint16_t *nv = (uint16_t *)realloc(chain, (n + 1) * sizeof(uint16_t));
        if (!nv) {
            free(chain);
            return -1;
        }
        chain = nv;
        chain[n++] = c;
    }

    if (n > need_clusters) {
        uint16_t keep_last = chain[need_clusters - 1];
        uint16_t free_first = chain[need_clusters];
        fat_set(fs, keep_last, 0xFFF);
        if (free_chain(fs, free_first) != 0) {
            free(chain);
            return -1;
        }
    } else {
        fat_set(fs, chain[n - 1], 0xFFF);
        if (flush_fat(fs) != 0) {
            free(chain);
            return -1;
        }
    }

    free(chain);
    return 0;
}

/**
 * @brief Creates a new directory entry in a target directory.
 * @param fs Open FAT12 context.
 * @param dir_cluster Parent directory cluster (0 for root).
 * @param name File or directory name (8.3 compatible text).
 * @param attr FAT attribute byte.
 * @param first_cluster First cluster for the entry payload.
 * @param size Initial file size in bytes.
 * @param new_off Optional output absolute directory-entry offset.
 * @return 0 on success, -1 on validation, space, or I/O failure.
 */
static int add_entry(Fat12 *fs, uint16_t dir_cluster, const char *name,
        uint8_t attr, uint16_t first_cluster, uint32_t size, uint64_t *new_off)
{
    uint8_t n83[11];
    if (to_short_name(name, n83) != 0)
        return -1;

    EntryRef existing;
    if (find_in_dir(fs, dir_cluster, n83, &existing) != 0)
        return -1;
    if (existing.found)
        return -1;

    uint64_t off;
    if (find_free_entry_slot(fs, dir_cluster, &off) != 0)
        return -1;

    DirEntry e;
    memset(&e, 0, sizeof(e));
    memcpy(e.name, n83, 11);
    e.attr = attr;
    e.first_cluster_lo = first_cluster;
    e.file_size = size;
    fat_time_date(&e.wrt_time, &e.wrt_date);
    e.crt_time = e.wrt_time;
    e.crt_date = e.wrt_date;
    e.acc_date = e.wrt_date;

    if (write_dir_entry_at(fs, off, &e) != 0)
        return -1;
    if (new_off)
        *new_off = off;
    return 0;
}

/**
 * @brief Marks a directory entry as deleted.
 * @param fs Open FAT12 context.
 * @param off Absolute offset of the directory entry.
 * @return 0 on success, -1 on write failure.
 */
static int remove_entry(Fat12 *fs, uint64_t off)
{
    uint8_t mark = 0xE5;
    if (write_at(fs->fp, off, &mark, 1) != 0)
        return -1;
    fflush(fs->fp);
    return 0;
}

/**
 * @brief Checks whether a directory contains no live entries.
 * @param fs Open FAT12 context.
 * @param cluster Directory cluster (0 for root).
 * @return Non-zero when directory is empty, otherwise zero.
 */
static int is_dir_empty(Fat12 *fs, uint16_t cluster)
{
    if (cluster == 0) {
        for (uint32_t i = 0; i < fs->bpb.root_entry_count; ++i) {
            DirEntry e;
            if (read_dir_entry_at(fs,
                        fs->root_offset + (uint64_t)i * sizeof(DirEntry),
                        &e) != 0)
                return 0;
            if (e.name[0] == 0x00)
                break;
            if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
                    (e.attr & ATTR_VOLUME_ID))
                continue;
            return 0;
        }
        return 1;
    }

    uint16_t c = cluster;
    while (c >= 2 && c < 0xFF7) {
        uint64_t base = cluster_to_offset(fs, c);
        uint32_t cnt = fs->cluster_size / sizeof(DirEntry);
        for (uint32_t i = 0; i < cnt; ++i) {
            DirEntry e;
            if (read_dir_entry_at(
                        fs, base + (uint64_t)i * sizeof(DirEntry), &e) != 0)
                return 0;
            if (e.name[0] == 0x00)
                return 1;
            if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
                    (e.attr & ATTR_VOLUME_ID))
                continue;
            if (i < 2 && e.name[0] == '.')
                continue;
            return 0;
        }
        uint16_t nx = fat_get(fs, c);
        if (is_eoc(nx))
            break;
        c = nx;
    }
    return 1;
}

/*
 * @brief Opens a FAT12 filesystem from an image path and base offset.
 *
 * @param fs Filesystem handle output.
 * @param image_path Backing image path.
 * @param partition_offset_bytes Byte offset where FAT12 boot sector starts.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_open(
        Fat12 *fs, const char *image_path, uint64_t partition_offset_bytes)
{
    memset(fs, 0, sizeof(*fs));
    fs->fp = fopen(image_path, "rb+");
    if (!fs->fp)
        return -errno;
    fs->base_offset = partition_offset_bytes;

    if (read_at(fs->fp, fs->base_offset, &fs->bpb, sizeof(fs->bpb)) != 0) {
        fat12_close(fs);
        return -EIO;
    }

    if (fs->bpb.bytes_per_sector == 0 || fs->bpb.sectors_per_cluster == 0) {
        fat12_close(fs);
        return -EINVAL;
    }

    fs->total_sectors = fs->bpb.total_sectors_16 ? fs->bpb.total_sectors_16
                                                 : fs->bpb.total_sectors_32;
    fs->root_dir_sectors = ((uint32_t)fs->bpb.root_entry_count * 32 +
                                   (fs->bpb.bytes_per_sector - 1)) /
            fs->bpb.bytes_per_sector;
    fs->fat_offset = fs->base_offset +
            (uint64_t)fs->bpb.reserved_sectors * fs->bpb.bytes_per_sector;
    fs->fat_size_bytes =
            (uint32_t)fs->bpb.sectors_per_fat * fs->bpb.bytes_per_sector;
    fs->root_offset = fs->base_offset +
            ((uint64_t)fs->bpb.reserved_sectors +
                    (uint64_t)fs->bpb.fat_count * fs->bpb.sectors_per_fat) *
                    fs->bpb.bytes_per_sector;
    fs->data_offset = fs->base_offset +
            ((uint64_t)fs->bpb.reserved_sectors +
                    (uint64_t)fs->bpb.fat_count * fs->bpb.sectors_per_fat +
                    fs->root_dir_sectors) *
                    fs->bpb.bytes_per_sector;
    fs->cluster_size =
            (uint32_t)fs->bpb.sectors_per_cluster * fs->bpb.bytes_per_sector;

    fs->total_clusters = (fs->total_sectors -
                                 (fs->bpb.reserved_sectors +
                                         (uint32_t)fs->bpb.fat_count *
                                                 fs->bpb.sectors_per_fat +
                                         fs->root_dir_sectors)) /
            fs->bpb.sectors_per_cluster;
    if (fs->total_clusters < 1 || fs->total_clusters >= 4085) {
        fat12_close(fs);
        return -EINVAL;
    }

    fs->fat = (uint8_t *)malloc(fs->fat_size_bytes);
    if (!fs->fat) {
        fat12_close(fs);
        return -ENOMEM;
    }

    if (read_at(fs->fp, fs->fat_offset, fs->fat, fs->fat_size_bytes) != 0) {
        fat12_close(fs);
        return -EIO;
    }

    return 0;
}

/*
 * @brief Closes an open FAT12 handle and releases resources.
 *
 * @param fs Open FAT12 context.
 */
void fat12_close(Fat12 *fs)
{
    if (fs->fat)
        free(fs->fat);
    if (fs->fp)
        fclose(fs->fp);
    memset(fs, 0, sizeof(*fs));
}

/*
 * @brief Fetches metadata for a file or directory.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute path.
 * @param out Output metadata.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_stat(Fat12 *fs, const char *path, Fat12Node *out)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;

    memset(out, 0, sizeof(*out));
    out->is_dir = (r.entry.attr & ATTR_DIRECTORY) != 0;
    if (out->is_dir) {
        out->mode = S_IFDIR | 0755;
    } else {
        out->mode = S_IFREG | 0644;
    }
    if (r.entry.attr & ATTR_READ_ONLY) {
        out->mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    }
    out->size = r.entry.file_size;
    out->first_cluster = r.entry.first_cluster_lo;
    out->wrt_time = r.entry.wrt_time;
    out->wrt_date = r.entry.wrt_date;
    out->attr = r.entry.attr;
    return 0;
}

/*
 * @brief Enumerates entries in a directory.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute directory path.
 * @param cb Callback invoked per entry.
 * @param user Opaque callback context.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_list(Fat12 *fs, const char *path, fat12_list_cb cb, void *user)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;

    uint16_t dir_cluster = 0;
    if (strcmp(path, "/") == 0)
        dir_cluster = 0;
    else {
        if (!(r.entry.attr & ATTR_DIRECTORY))
            return -ENOTDIR;
        dir_cluster = r.entry.first_cluster_lo;
    }

    if (dir_cluster == 0) {
        for (uint32_t i = 0; i < fs->bpb.root_entry_count; ++i) {
            DirEntry e;
            if (read_dir_entry_at(fs,
                        fs->root_offset + (uint64_t)i * sizeof(DirEntry),
                        &e) != 0)
                return -EIO;
            if (e.name[0] == 0x00)
                break;
            if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
                    (e.attr & ATTR_VOLUME_ID))
                continue;
            char name[20];
            short_name_to_str(e.name, name, sizeof(name));
            Fat12Node node;
            memset(&node, 0, sizeof(node));
            node.is_dir = (e.attr & ATTR_DIRECTORY) != 0;
            if (node.is_dir) {
                node.mode = S_IFDIR | 0755;
            } else {
                node.mode = S_IFREG | 0644;
            }
            if (e.attr & ATTR_READ_ONLY) {
                node.mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
            }
            node.size = e.file_size;
            node.first_cluster = e.first_cluster_lo;
            node.wrt_time = e.wrt_time;
            node.wrt_date = e.wrt_date;
            node.attr = e.attr;
            if (cb(name, &node, user) != 0)
                break;
        }
        return 0;
    }

    uint16_t c = dir_cluster;
    while (c >= 2 && c < 0xFF7) {
        uint64_t base = cluster_to_offset(fs, c);
        uint32_t cnt = fs->cluster_size / sizeof(DirEntry);
        for (uint32_t i = 0; i < cnt; ++i) {
            DirEntry e;
            if (read_dir_entry_at(
                        fs, base + (uint64_t)i * sizeof(DirEntry), &e) != 0)
                return -EIO;
            if (e.name[0] == 0x00)
                return 0;
            if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
                    (e.attr & ATTR_VOLUME_ID))
                continue;
            char name[20];
            short_name_to_str(e.name, name, sizeof(name));
            Fat12Node node;
            memset(&node, 0, sizeof(node));
            node.is_dir = (e.attr & ATTR_DIRECTORY) != 0;
            if (node.is_dir) {
                node.mode = S_IFDIR | 0755;
            } else {
                node.mode = S_IFREG | 0644;
            }
            if (e.attr & ATTR_READ_ONLY) {
                node.mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
            }
            node.size = e.file_size;
            node.first_cluster = e.first_cluster_lo;
            node.wrt_time = e.wrt_time;
            node.wrt_date = e.wrt_date;
            node.attr = e.attr;
            if (cb(name, &node, user) != 0)
                return 0;
        }
        uint16_t nx = fat_get(fs, c);
        if (is_eoc(nx))
            break;
        c = nx;
    }
    return 0;
}

/*
 * @brief Reads bytes from a file.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute file path.
 * @param buf Destination buffer.
 * @param size Maximum bytes to read.
 * @param offset Starting file offset.
 * @return Bytes read (>=0) or negative errno-style code.
 */
ssize_t fat12_read(
        Fat12 *fs, const char *path, void *buf, size_t size, off_t offset)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;
    if (r.entry.attr & ATTR_DIRECTORY)
        return -EISDIR;
    if (offset < 0)
        return -EINVAL;

    if ((uint64_t)offset >= r.entry.file_size)
        return 0;
    size_t can = (size_t)(r.entry.file_size - (uint32_t)offset);
    if (can > size)
        can = size;

    if (can == 0)
        return 0;

    uint32_t cluster_idx = (uint32_t)offset / fs->cluster_size;
    uint32_t cluster_off = (uint32_t)offset % fs->cluster_size;
    uint16_t c = r.entry.first_cluster_lo;

    // Skip clusters until we reach the starting one
    for (uint32_t i = 0; i < cluster_idx; ++i) {
        if (c < 2 || is_eoc(c))
            return -EIO;
        c = fat_get(fs, c);
    }

    uint32_t read_bytes = 0;
    while (read_bytes < can) {
        if (c < 2 || is_eoc(c))
            break;
        uint32_t in_cluster = fs->cluster_size - cluster_off;
        uint32_t chunk = (can - read_bytes) < in_cluster ? (can - read_bytes)
                                                         : in_cluster;

        if (read_at(fs->fp, cluster_to_offset(fs, c) + cluster_off,
                    (uint8_t *)buf + read_bytes, chunk) != 0)
            return -EIO;

        read_bytes += chunk;
        cluster_off = 0;
        if (read_bytes < can) {
            c = fat_get(fs, c);
        }
    }

    return (ssize_t)read_bytes;
}

/*
 * @brief Writes bytes into a file.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute file path.
 * @param buf Source data.
 * @param size Bytes to write.
 * @param offset Starting file offset.
 * @return Bytes written (>=0) or negative errno-style code.
 */
ssize_t fat12_write(
        Fat12 *fs, const char *path, const void *buf, size_t size, off_t offset)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;
    if (r.entry.attr & ATTR_DIRECTORY)
        return -EISDIR;
    if (offset < 0)
        return -EINVAL;

    uint32_t new_len = (uint32_t)(offset + size);
    if (new_len > r.entry.file_size) {
        if (fat12_truncate(fs, path, (off_t)new_len) != 0)
            return -ENOSPC;
        // Reload entry after extension
        if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0)
            return -EIO;
    }

    if (size > 0) {
        uint32_t cluster_idx = (uint32_t)offset / fs->cluster_size;
        uint32_t cluster_off = (uint32_t)offset % fs->cluster_size;
        uint16_t c = r.entry.first_cluster_lo;

        // Skip clusters until we reach the starting one
        for (uint32_t i = 0; i < cluster_idx; ++i) {
            if (c < 2 || is_eoc(c))
                return -EIO;
            c = fat_get(fs, c);
        }

        uint32_t written_bytes = 0;
        while (written_bytes < (uint32_t)size) {
            if (c < 2 || is_eoc(c))
                return -EIO;
            uint32_t in_cluster = fs->cluster_size - cluster_off;
            uint32_t chunk = ((uint32_t)size - written_bytes) < in_cluster
                    ? ((uint32_t)size - written_bytes)
                    : in_cluster;

            if (write_at(fs->fp, cluster_to_offset(fs, c) + cluster_off,
                        (const uint8_t *)buf + written_bytes, chunk) != 0)
                return -EIO;

            written_bytes += chunk;
            cluster_off = 0;
            if (written_bytes < (uint32_t)size) {
                c = fat_get(fs, c);
            }
        }
    }

    fat_time_date(&r.entry.wrt_time, &r.entry.wrt_date);
    r.entry.acc_date = r.entry.wrt_date;
    r.entry.attr |= ATTR_ARCHIVE;
    if (write_dir_entry_at(fs, r.offset, &r.entry) != 0)
        return -EIO;

    return (ssize_t)size;
}

/*
 * @brief Creates an empty regular file.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute file path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_create(Fat12 *fs, const char *path)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    uint16_t parent;
    char leaf[256];
    if (resolve_abs_path(fs, path, &r, &parent, leaf, sizeof(leaf)) != 0)
        return -EINVAL;
    if (r.found)
        return -EEXIST;
    uint8_t attr = ATTR_ARCHIVE;
    if (leaf[0] == '.') {
        attr |= ATTR_HIDDEN;
    }
    if (add_entry(fs, parent, leaf, attr, 0, 0, NULL) != 0)
        return -ENOSPC;
    return 0;
}

/*
 * @brief Initialises a new directory cluster with dot entries.
 *
 * @param fs Open FAT12 context.
 * @param cluster Newly allocated directory cluster.
 * @param parent_cluster Parent directory cluster.
 * @return 0 on success, -1 on allocation or write failure.
 */
static int mkdir_init(Fat12 *fs, uint16_t cluster, uint16_t parent_cluster)
{
    uint8_t *buf = (uint8_t *)calloc(1, fs->cluster_size);
    if (!buf)
        return -1;

    DirEntry dot;
    DirEntry dotdot;
    memset(&dot, 0, sizeof(dot));
    memset(&dotdot, 0, sizeof(dotdot));

    memset(dot.name, ' ', 11);
    dot.name[0] = '.';
    dot.attr = ATTR_DIRECTORY;
    dot.first_cluster_lo = cluster;
    fat_time_date(&dot.wrt_time, &dot.wrt_date);
    dot.crt_time = dot.wrt_time;
    dot.crt_date = dot.wrt_date;
    dot.acc_date = dot.wrt_date;

    memset(dotdot.name, ' ', 11);
    dotdot.name[0] = '.';
    dotdot.name[1] = '.';
    dotdot.attr = ATTR_DIRECTORY;
    dotdot.first_cluster_lo = parent_cluster;
    fat_time_date(&dotdot.wrt_time, &dotdot.wrt_date);
    dotdot.crt_time = dotdot.wrt_time;
    dotdot.crt_date = dotdot.wrt_date;
    dotdot.acc_date = dotdot.wrt_date;

    memcpy(buf, &dot, sizeof(dot));
    memcpy(buf + sizeof(dot), &dotdot, sizeof(dotdot));

    int rc = write_at(
            fs->fp, cluster_to_offset(fs, cluster), buf, fs->cluster_size);
    free(buf);
    fflush(fs->fp);
    return rc;
}

/*
 * @brief Creates a directory.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute directory path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_mkdir(Fat12 *fs, const char *path)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    uint16_t parent;
    char leaf[256];
    if (resolve_abs_path(fs, path, &r, &parent, leaf, sizeof(leaf)) != 0)
        return -EINVAL;
    if (r.found)
        return -EEXIST;

    uint16_t c;
    if (alloc_cluster(fs, &c) != 0)
        return -ENOSPC;

    uint8_t attr = ATTR_DIRECTORY;
    if (leaf[0] == '.') {
        attr |= ATTR_HIDDEN;
    }

    if (mkdir_init(fs, c, parent) != 0 ||
            add_entry(fs, parent, leaf, attr, c, 0, NULL) != 0) {
        free_chain(fs, c);
        return -ENOSPC;
    }
    return 0;
}

/*
 * @brief Removes a regular file.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute file path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_unlink(Fat12 *fs, const char *path)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;
    if (r.entry.attr & ATTR_DIRECTORY)
        return -EISDIR;

    if (r.entry.first_cluster_lo >= 2 &&
            free_chain(fs, r.entry.first_cluster_lo) != 0)
        return -EIO;
    if (remove_entry(fs, r.offset) != 0)
        return -EIO;
    return 0;
}

/*
 * @brief Removes an empty directory.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute directory path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_rmdir(Fat12 *fs, const char *path)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    if (strcmp(path, "/") == 0)
        return -EBUSY;
    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;
    if (!(r.entry.attr & ATTR_DIRECTORY))
        return -ENOTDIR;
    if (!is_dir_empty(fs, r.entry.first_cluster_lo))
        return -ENOTEMPTY;
    if (free_chain(fs, r.entry.first_cluster_lo) != 0)
        return -EIO;
    if (remove_entry(fs, r.offset) != 0)
        return -EIO;
    return 0;
}

/*
 * @brief Resizes a regular file.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute file path.
 * @param size Target file size in bytes.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_truncate(Fat12 *fs, const char *path, off_t size)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    if (size < 0)
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;
    if (r.entry.attr & ATTR_DIRECTORY)
        return -EISDIR;

    uint32_t new_size = (uint32_t)size;
    uint32_t old_size = r.entry.file_size;

    if (new_size == old_size)
        return 0;

    if (new_size > old_size) {
        // Zero the gap in the current last cluster if any
        if (old_size > 0 && (old_size % fs->cluster_size) != 0) {
            uint32_t cluster_idx = (old_size - 1) / fs->cluster_size;
            uint32_t cluster_off = old_size % fs->cluster_size;
            uint32_t chunk = fs->cluster_size - cluster_off;
            uint16_t c = r.entry.first_cluster_lo;
            for (uint32_t i = 0; i < cluster_idx; ++i) {
                if (c < 2 || is_eoc(c))
                    break;
                c = fat_get(fs, c);
            }
            if (c >= 2 && !is_eoc(c)) {
                uint8_t *zero = (uint8_t *)calloc(1, chunk);
                if (zero) {
                    write_at(fs->fp, cluster_to_offset(fs, c) + cluster_off,
                            zero, chunk);
                    free(zero);
                }
            }
        }
    }

    size_t need = (new_size + fs->cluster_size - 1) / fs->cluster_size;
    uint16_t first = r.entry.first_cluster_lo;
    if (ensure_chain_size(fs, &first, need) != 0)
        return -ENOSPC;

    r.entry.first_cluster_lo = first;
    r.entry.file_size = new_size;
    fat_time_date(&r.entry.wrt_time, &r.entry.wrt_date);
    r.entry.acc_date = r.entry.wrt_date;
    r.entry.attr |= ATTR_ARCHIVE;
    if (write_dir_entry_at(fs, r.offset, &r.entry) != 0)
        return -EIO;
    fflush(fs->fp);

    return 0;
}

/*
 * @brief Updates entry access and write dates to the current local time.
 *
 * @param fs Open FAT12 context.
 * @param path Absolute file or directory path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_utimens_now(Fat12 *fs, const char *path)
{
    return fat12_utimens(fs, path, time(NULL));
}

/*
 * @brief Updates access/write timestamps to a specific local time.
 */
int fat12_utimens(Fat12 *fs, const char *path, time_t mtime)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;

    fat12_time_t_to_fat(mtime, &r.entry.wrt_time, &r.entry.wrt_date);
    r.entry.acc_date = r.entry.wrt_date;
    if (write_dir_entry_at(fs, r.offset, &r.entry) != 0)
        return -EIO;
    return 0;
}

/*
 * @brief Sets directory entry attributes.
 */
int fat12_set_attr(Fat12 *fs, const char *path, uint8_t attr)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;

    r.entry.attr = attr;
    if (write_dir_entry_at(fs, r.offset, &r.entry) != 0)
        return -EIO;
    return 0;
}

/*
 * @brief Updates directory entry attributes from POSIX mode.
 */
int fat12_chmod(Fat12 *fs, const char *path, mode_t mode)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    EntryRef r;
    if (resolve_abs_path(fs, path, &r, NULL, NULL, 0) != 0 || !r.found)
        return -ENOENT;

    if (!(mode & S_IWUSR)) {
        r.entry.attr |= ATTR_READ_ONLY;
    } else {
        r.entry.attr &= ~ATTR_READ_ONLY;
    }

    if (write_dir_entry_at(fs, r.offset, &r.entry) != 0)
        return -EIO;
    return 0;
}

/*
 * @brief Renames or moves a file or directory.
 *
 * @param fs Open FAT12 context.
 * @param from Source absolute path.
 * @param to Target absolute path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_rename(Fat12 *fs, const char *from, const char *to)
{
    if (!from || from[0] != '/' || !to || to[0] != '/')
        return -EINVAL;

    EntryRef src_ref;
    if (resolve_abs_path(fs, from, &src_ref, NULL, NULL, 0) != 0 ||
            !src_ref.found)
        return -ENOENT;

    // Root cannot be renamed
    if (strcmp(from, "/") == 0)
        return -EBUSY;

    // Handle same path rename
    if (strcmp(from, to) == 0)
        return 0;

    EntryRef dst_ref;
    uint16_t dst_parent;
    char dst_leaf[256];
    if (resolve_abs_path(
                fs, to, &dst_ref, &dst_parent, dst_leaf, sizeof(dst_leaf)) != 0)
        return -EINVAL;

    if (dst_ref.found) {
        // If they are the same file/dir (e.g. symlink/hardlink case, though FAT
        // doesn't have them)
        if (src_ref.offset == dst_ref.offset && src_ref.offset != 0)
            return 0;

        if (src_ref.entry.attr & ATTR_DIRECTORY) {
            if (!(dst_ref.entry.attr & ATTR_DIRECTORY))
                return -ENOTDIR;
            if (!is_dir_empty(fs, dst_ref.entry.first_cluster_lo))
                return -ENOTEMPTY;
            // Overwrite existing empty directory
            if (fat12_rmdir(fs, to) != 0)
                return -EIO;
        } else {
            if (dst_ref.entry.attr & ATTR_DIRECTORY)
                return -EISDIR;
            // Overwrite existing file
            if (fat12_unlink(fs, to) != 0)
                return -EIO;
        }
    }

    // Add new entry in destination
    uint64_t new_off;
    if (add_entry(fs, dst_parent, dst_leaf, src_ref.entry.attr,
                src_ref.entry.first_cluster_lo, src_ref.entry.file_size,
                &new_off) != 0)
        return -ENOSPC;

    // If it's a directory, update its '..' entry
    if (src_ref.entry.attr & ATTR_DIRECTORY) {
        uint16_t dir_cluster = src_ref.entry.first_cluster_lo;
        if (dir_cluster != 0) {
            uint64_t dotdot_off =
                    cluster_to_offset(fs, dir_cluster) + sizeof(DirEntry);
            DirEntry dotdot;
            if (read_dir_entry_at(fs, dotdot_off, &dotdot) == 0) {
                dotdot.first_cluster_lo = dst_parent;
                write_dir_entry_at(fs, dotdot_off, &dotdot);
            }
        }
    }

    // Mark old entry as deleted
    if (remove_entry(fs, src_ref.offset) != 0)
        return -EIO;

    return 0;
}

/* ============================================================================
 * Integrity Verification Functions
 * ===========================================================================*/

/**
 * @brief Counts different types of clusters in FAT.
 */
static int count_cluster_types(
        Fat12 *fs, int *free_count, int *allocated_count, int *bad_count)
{
    if (!fs || !free_count || !allocated_count || !bad_count)
        return -EINVAL;

    *free_count = 0;
    *allocated_count = 0;
    *bad_count = 0;

    for (uint16_t cluster = 2; cluster < fs->total_clusters + 2; cluster++) {
        uint16_t value = fat_get(fs, cluster);
        if (value == 0) {
            (*free_count)++;
        } else if (value == 0xFF7) {
            (*bad_count)++;
        } else if (value < 0xFF8) {
            (*allocated_count)++;
        }
        // Values 0xFF8-0xFFF are EOC markers, counted as allocated
    }

    return 0;
}

/**
 * @brief Counts used entries in root directory.
 */
static int count_root_entries(Fat12 *fs, int *used, int *max)
{
    if (!fs || !used || !max)
        return -EINVAL;

    *max = fs->bpb.root_entry_count;
    *used = 0;

    for (uint32_t i = 0; i < fs->bpb.root_entry_count; ++i) {
        DirEntry e;
        if (read_dir_entry_at(fs,
                    fs->root_offset + (uint64_t)i * sizeof(DirEntry), &e) != 0)
            return -EIO;

        if (e.name[0] == 0x00)
            break;  // End of directory

        if (e.name[0] != 0xE5 && e.attr != ATTR_LFN &&
                !(e.attr & ATTR_VOLUME_ID)) {
            (*used)++;
        }
    }

    return 0;
}

/**
 * @brief Checks if FAT replicas are consistent.
 */
static int check_fat_consistency(Fat12 *fs)
{
    if (!fs || fs->bpb.fat_count < 1)
        return -EINVAL;

    // Read first FAT
    uint8_t *fat1 = fs->fat;  // Already loaded in memory

    // Check other FATs against first one
    for (int i = 1; i < fs->bpb.fat_count; i++) {
        uint8_t *fati = malloc(fs->fat_size_bytes);
        if (!fati)
            return -ENOMEM;

        uint64_t offset = fs->fat_offset + (uint64_t)i * fs->fat_size_bytes;
        if (fseeko(fs->fp, (off_t)(fs->base_offset + offset), SEEK_SET) != 0) {
            free(fati);
            return -EIO;
        }

        if (fread(fati, 1, fs->fat_size_bytes, fs->fp) != fs->fat_size_bytes) {
            free(fati);
            return -EIO;
        }

        if (memcmp(fat1, fati, fs->fat_size_bytes) != 0) {
            free(fati);
            return -1;  // FAT copies differ
        }

        free(fati);
    }

    return 0;  // All FATs consistent
}

/**
 * @brief Marks a cluster chain in the reference bitmap.
 */
static void mark_cluster_chain(
        Fat12 *fs, uint16_t start_cluster, uint8_t *ref_bitmap)
{
    uint16_t current = start_cluster;

    while (current >= 2 && current < 0xFF7) {
        if (ref_bitmap[current]) {
            // Already marked, possible cross-link (will be detected separately)
            break;
        }
        ref_bitmap[current] = 1;

        uint16_t next = fat_get(fs, current);
        if (next >= 0xFF8)  // EOC
            break;

        current = next;
    }
}

/**
 * @brief Traverses directory and marks referenced clusters.
 */
static int traverse_directory(
        Fat12 *fs, uint16_t dir_cluster, uint8_t *ref_bitmap, int *entry_count)
{
    int local_count = 0;

    if (dir_cluster == 0) {
        // Root directory
        for (uint32_t i = 0; i < fs->bpb.root_entry_count; ++i) {
            DirEntry e;
            if (read_dir_entry_at(fs,
                        fs->root_offset + (uint64_t)i * sizeof(DirEntry),
                        &e) != 0)
                return -EIO;

            if (e.name[0] == 0x00)
                break;

            if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
                    (e.attr & ATTR_VOLUME_ID))
                continue;

            local_count++;

            uint16_t first_cluster = e.first_cluster_lo;
            if (first_cluster != 0) {
                mark_cluster_chain(fs, first_cluster, ref_bitmap);

                // If it's a directory, traverse it recursively
                if (e.attr & ATTR_DIRECTORY) {
                    if (traverse_directory(
                                fs, first_cluster, ref_bitmap, NULL) != 0) {
                        return -EIO;
                    }
                }
            }
        }
    } else {
        // Subdirectory
        uint16_t c = dir_cluster;
        while (c >= 2 && c < 0xFF7) {
            uint64_t base = cluster_to_offset(fs, c);
            uint32_t cnt = fs->cluster_size / sizeof(DirEntry);

            for (uint32_t i = 0; i < cnt; ++i) {
                DirEntry e;
                if (read_dir_entry_at(
                            fs, base + (uint64_t)i * sizeof(DirEntry), &e) != 0)
                    return -EIO;

                if (e.name[0] == 0x00)
                    return 0;

                if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
                        (e.attr & ATTR_VOLUME_ID))
                    continue;

                // Skip . and .. entries
                if (e.name[0] == '.' && (e.name[1] == ' ' || e.name[1] == '.'))
                    continue;

                local_count++;

                uint16_t first_cluster = e.first_cluster_lo;
                if (first_cluster != 0) {
                    mark_cluster_chain(fs, first_cluster, ref_bitmap);

                    // If it's a directory, traverse it recursively
                    if (e.attr & ATTR_DIRECTORY) {
                        if (traverse_directory(
                                    fs, first_cluster, ref_bitmap, NULL) != 0) {
                            return -EIO;
                        }
                    }
                }
            }

            uint16_t nx = fat_get(fs, c);
            if (nx >= 0xFF8)  // EOC
                break;
            c = nx;
        }
    }

    if (entry_count)
        *entry_count = local_count;

    return 0;
}

/**
 * @brief Detects cross-linked clusters.
 */
static int detect_cross_links(Fat12 *fs, Fat12CrossLink **links, int *count)
{
    if (!fs || !links || !count)
        return -EINVAL;

    *links = NULL;
    *count = 0;

    int max_clusters = fs->total_clusters + 2;
    uint8_t *visited = calloc(max_clusters, 1);
    uint8_t *in_current_chain = calloc(max_clusters, 1);

    if (!visited || !in_current_chain) {
        free(visited);
        free(in_current_chain);
        return -ENOMEM;
    }

    // Simple array for cross-links (we'll allocate as needed)
    int capacity = 10;
    Fat12CrossLink *cross_links = malloc(capacity * sizeof(Fat12CrossLink));
    if (!cross_links) {
        free(visited);
        free(in_current_chain);
        return -ENOMEM;
    }

    int cross_link_count = 0;

    for (uint16_t cluster = 2; cluster < max_clusters; cluster++) {
        uint16_t value = fat_get(fs, cluster);
        if (value == 0 || value >= 0xFF7)  // Free or bad
            continue;

        if (visited[cluster])
            continue;

        memset(in_current_chain, 0, max_clusters);
        uint16_t current = cluster;

        while (current >= 2 && current < 0xFF7) {
            if (in_current_chain[current]) {
                // Cross-link detected!
                if (cross_link_count >= capacity) {
                    capacity *= 2;
                    Fat12CrossLink *new_links = realloc(
                            cross_links, capacity * sizeof(Fat12CrossLink));
                    if (!new_links) {
                        free(cross_links);
                        free(visited);
                        free(in_current_chain);
                        return -ENOMEM;
                    }
                    cross_links = new_links;
                }

                cross_links[cross_link_count].cluster1 = cluster;
                cross_links[cross_link_count].cluster2 = current;
                cross_link_count++;
                break;
            }

            if (visited[current])
                break;  // Already checked this chain

            in_current_chain[current] = 1;
            visited[current] = 1;
            current = fat_get(fs, current);
        }

        // Mark entire chain as visited
        current = cluster;
        while (current >= 2 && current < 0xFF7) {
            visited[current] = 1;
            uint16_t next = fat_get(fs, current);
            if (next >= 0xFF8)
                break;
            current = next;
        }
    }

    free(visited);
    free(in_current_chain);

    *links = cross_links;
    *count = cross_link_count;

    return 0;
}

/**
 * @brief Detects orphaned clusters.
 */
static int detect_orphaned_clusters(Fat12 *fs, uint16_t **clusters, int *count)
{
    if (!fs || !clusters || !count)
        return -EINVAL;

    *clusters = NULL;
    *count = 0;

    int max_clusters = fs->total_clusters + 2;
    uint8_t *referenced = calloc(max_clusters, 1);

    if (!referenced)
        return -ENOMEM;

    // Traverse directory tree to mark referenced clusters
    if (traverse_directory(fs, 0, referenced, NULL) != 0) {
        free(referenced);
        return -EIO;
    }

    // Simple array for orphaned clusters
    int capacity = 10;
    uint16_t *orphaned = malloc(capacity * sizeof(uint16_t));
    if (!orphaned) {
        free(referenced);
        return -ENOMEM;
    }

    int orphaned_count = 0;

    // Find allocated but not referenced clusters
    for (uint16_t cluster = 2; cluster < max_clusters; cluster++) {
        uint16_t value = fat_get(fs, cluster);
        if (value != 0 && value < 0xFF7 && !referenced[cluster]) {
            if (orphaned_count >= capacity) {
                capacity *= 2;
                uint16_t *new_orphaned =
                        realloc(orphaned, capacity * sizeof(uint16_t));
                if (!new_orphaned) {
                    free(orphaned);
                    free(referenced);
                    return -ENOMEM;
                }
                orphaned = new_orphaned;
            }

            orphaned[orphaned_count++] = cluster;
        }
    }

    free(referenced);

    *clusters = orphaned;
    *count = orphaned_count;

    return 0;
}

/**
 * @brief Appends formatted error message to error buffer.
 */
static void append_error(char **error_buf, size_t *buf_size, size_t *buf_len,
        const char *format, ...)
{
    if (!error_buf || !buf_size || !buf_len)
        return;

    va_list args;
    va_start(args, format);

    // Calculate needed size
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    if (needed < 0)
        return;

    // Ensure buffer is large enough
    size_t new_len = *buf_len + (size_t)needed + 1;  // +1 for newline
    if (new_len > *buf_size) {
        size_t new_size = *buf_size == 0 ? 256 : *buf_size * 2;
        while (new_size < new_len)
            new_size *= 2;

        char *new_buf = realloc(*error_buf, new_size);
        if (!new_buf)
            return;

        *error_buf = new_buf;
        *buf_size = new_size;
    }

    // Append the formatted message
    int written = vsnprintf(
            *error_buf + *buf_len, *buf_size - *buf_len, format, args);
    if (written > 0)
        *buf_len += (size_t)written;

    // Add newline
    if (*buf_len < *buf_size - 1) {
        (*error_buf)[*buf_len] = '\n';
        (*buf_len)++;
        (*error_buf)[*buf_len] = '\0';
    }

    va_end(args);
}

/**
 * @brief Estimates memory usage for verification.
 */
static size_t estimate_memory_usage(const Fat12 *fs)
{
    if (!fs)
        return 0;

    // Bitmaps: 2 * (total_clusters + 2) bytes
    // Cross-link array: worst-case total_clusters * sizeof(Fat12CrossLink)
    // Orphaned cluster array: worst-case total_clusters * sizeof(uint16_t)
    // Total: ~ (total_clusters * 10) bytes

    size_t bitmap_size = 2 * (fs->total_clusters + 2);
    size_t array_size =
            fs->total_clusters * (sizeof(Fat12CrossLink) + sizeof(uint16_t));
    return bitmap_size + array_size;
}

/**
 * @brief Checks if memory usage might be excessive and warns.
 */
static void check_memory_warning(const Fat12 *fs, int verbose, char **error_buf,
        size_t *error_buf_size, size_t *error_buf_len)
{
    if (!fs)
        return;

    const size_t WARNING_THRESHOLD = 50 * 1024 * 1024;  // 50MB
    size_t estimated = estimate_memory_usage(fs);

    if (estimated > WARNING_THRESHOLD) {
        append_error(error_buf, error_buf_size, error_buf_len,
                "Warning: Verification may use ~%.1f MB of memory",
                estimated / (1024.0 * 1024.0));
        if (verbose) {
            append_error(error_buf, error_buf_size, error_buf_len,
                    "  Consider using alternative verification method if "
                    "memory constrained");
        }
    }
}

/**
 * @brief Main verification function implementation.
 */
int fat12_verify_integrity(Fat12 *fs, Fat12IntegrityReport *report, int verbose)
{
    if (!fs || !report)
        return -EINVAL;

    // Initialize report
    memset(report, 0, sizeof(*report));
    report->error_details = NULL;

    size_t error_buf_size = 0;
    size_t error_buf_len = 0;
    char *error_buf = NULL;

    // Check memory usage and warn if necessary
    check_memory_warning(
            fs, verbose, &error_buf, &error_buf_size, &error_buf_len);

    // Check FAT consistency
    report->fat_consistent = check_fat_consistency(fs);
    if (report->fat_consistent != 0 && verbose) {
        append_error(&error_buf, &error_buf_size, &error_buf_len,
                "FAT replicas are inconsistent");
    }

    // Count cluster types
    if (count_cluster_types(fs, &report->free_count, &report->allocated_count,
                &report->bad_count) != 0) {
        free(error_buf);
        return -EIO;
    }

    // Count root directory entries
    if (count_root_entries(fs, &report->root_entries_used,
                &report->root_entries_max) != 0) {
        free(error_buf);
        return -EIO;
    }

    // Detect cross-links
    Fat12CrossLink *cross_links = NULL;
    int cross_link_count = 0;
    if (detect_cross_links(fs, &cross_links, &cross_link_count) == 0) {
        report->cross_linked_count = cross_link_count;

        if (verbose && cross_link_count > 0) {
            for (int i = 0; i < cross_link_count; i++) {
                append_error(&error_buf, &error_buf_size, &error_buf_len,
                        "Cross-link detected: cluster %u ↔ cluster %u",
                        cross_links[i].cluster1, cross_links[i].cluster2);
            }
        }

        free(cross_links);
    }

    // Detect orphaned clusters
    uint16_t *orphaned_clusters = NULL;
    int orphaned_count = 0;
    if (detect_orphaned_clusters(fs, &orphaned_clusters, &orphaned_count) ==
            0) {
        report->orphaned_count = orphaned_count;

        if (verbose && orphaned_count > 0) {
            append_error(&error_buf, &error_buf_size, &error_buf_len,
                    "Found %d orphaned cluster%s:", orphaned_count,
                    orphaned_count == 1 ? "" : "s");

            for (int i = 0; i < orphaned_count; i++) {
                if (i == 0) {
                    append_error(&error_buf, &error_buf_size, &error_buf_len,
                            "  %u", orphaned_clusters[i]);
                } else if (i % 8 == 0) {
                    append_error(&error_buf, &error_buf_size, &error_buf_len,
                            "\n  %u", orphaned_clusters[i]);
                } else {
                    append_error(&error_buf, &error_buf_size, &error_buf_len,
                            " %u", orphaned_clusters[i]);
                }
            }
        }

        free(orphaned_clusters);
    }

    // Calculate total errors
    report->total_errors = 0;
    if (report->fat_consistent != 0)
        report->total_errors++;
    report->total_errors += report->cross_linked_count;
    report->total_errors += report->orphaned_count;

    // Store error details if any
    if (error_buf_len > 0) {
        report->error_details = error_buf;
    } else {
        free(error_buf);
    }

    return 0;
}

/**
 * @brief Creates a backup of the FAT12 image.
 */
static int create_backup(Fat12 *fs, const char *backup_path)
{
    if (!fs || !backup_path)
        return -EINVAL;

    // Get current position
    long original_pos = ftell(fs->fp);
    if (original_pos < 0)
        return -EIO;

    // Go to beginning
    if (fseeko(fs->fp, 0, SEEK_SET) != 0)
        return -EIO;

    // Open backup file
    FILE *backup_fp = fopen(backup_path, "wb");
    if (!backup_fp)
        return -EIO;

    // Copy entire file
    uint8_t buffer[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fs->fp)) > 0) {
        if (fwrite(buffer, 1, bytes_read, backup_fp) != bytes_read) {
            fclose(backup_fp);
            fseeko(fs->fp, original_pos, SEEK_SET);
            return -EIO;
        }
    }

    fclose(backup_fp);

    // Restore original position
    if (fseeko(fs->fp, original_pos, SEEK_SET) != 0)
        return -EIO;

    return 0;
}

/**
 * @brief Synchronizes FAT copies (writes primary FAT to all replicas).
 */
static int sync_fat_copies(Fat12 *fs)
{
    if (!fs || fs->bpb.fat_count < 1)
        return -EINVAL;

    // Write FAT to all copies
    for (int i = 0; i < fs->bpb.fat_count; i++) {
        uint64_t offset = fs->fat_offset + (uint64_t)i * fs->fat_size_bytes;
        if (fseeko(fs->fp, (off_t)(fs->base_offset + offset), SEEK_SET) != 0)
            return -EIO;

        if (fwrite(fs->fat, 1, fs->fat_size_bytes, fs->fp) !=
                fs->fat_size_bytes)
            return -EIO;
    }

    return 0;
}

/**
 * @brief Frees orphaned clusters (sets FAT entries to 0).
 */
/*
static int free_orphaned_clusters(Fat12 *fs, const uint16_t *clusters, int
count)
{
    if (!fs || !clusters || count <= 0)
        return 0;

    int freed = 0;
    for (int i = 0; i < count; i++) {
        uint16_t cluster = clusters[i];
        if (cluster >= 2 && cluster < fs->total_clusters + 2) {
            // Set FAT entry to 0 (free)
            fat_set(fs, cluster, 0);
            freed++;
        }
    }

    // Write updated FAT to disk
    if (freed > 0) {
        if (sync_fat_copies(fs) != 0)
            return -EIO;
    }

    return freed;
}
*/

/**
 * @brief Applies fixes based on verification report.
 */
int fat12_fix_integrity(Fat12 *fs, const Fat12IntegrityReport *report,
        const char *backup_path, int *fixes_applied)
{
    if (!fs || !report || !fixes_applied)
        return -EINVAL;

    *fixes_applied = 0;

    // Create backup if requested
    if (backup_path && backup_path[0] != '\0') {
        if (create_backup(fs, backup_path) != 0)
            return -EIO;
    }

    // Fix FAT inconsistencies
    if (report->fat_consistent != 0) {
        if (sync_fat_copies(fs) == 0)
            (*fixes_applied)++;
    }

    // Note: Cross-link fixing requires more complex logic
    // Orphaned cluster fixing would be implemented here when we have
    // the list of orphaned clusters

    return 0;
}
