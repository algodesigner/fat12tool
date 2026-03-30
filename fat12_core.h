/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file fat12_core.h
 * @brief Portable FAT12 read/write engine used by CLI and FUSE frontends.
 */

#ifndef FAT12_CORE_H
#define FAT12_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

/**
 * @brief BIOS Parameter Block (BPB) subset used by FAT12.
 *
 * This structure is read directly from disk, so packed layout is required.
 */
#pragma pack(push, 1)
typedef struct {
    /** Jump instruction and NOP in boot sector. */
    uint8_t jmp[3];
    /** OEM identifier string (not trusted for logic). */
    uint8_t oem[8];
    /** Logical sector size in bytes (usually 512). */
    uint16_t bytes_per_sector;
    /** Sectors per allocation unit (cluster). */
    uint8_t sectors_per_cluster;
    /** Count of reserved sectors before first FAT. */
    uint16_t reserved_sectors;
    /** Number of FAT copies (typically 2). */
    uint8_t fat_count;
    /** Maximum root directory entries (fixed for FAT12/16). */
    uint16_t root_entry_count;
    /** Total sectors for small volumes; if 0 use total_sectors_32. */
    uint16_t total_sectors_16;
    /** Media descriptor byte. */
    uint8_t media;
    /** Sectors occupied by one FAT table. */
    uint16_t sectors_per_fat;
    /** Disk geometry hint: sectors per track. */
    uint16_t sectors_per_track;
    /** Disk geometry hint: number of heads. */
    uint16_t heads;
    /** Hidden sectors before this volume (important in partitioned images). */
    uint32_t hidden_sectors;
    /** Total sectors for large volumes. */
    uint32_t total_sectors_32;
} Fat12BootSector;
#pragma pack(pop)

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

/**
 * @brief Runtime FAT12 filesystem handle.
 *
 * The handle stores both on-disk geometry and in-memory cached FAT state.
 * Call @ref fat12_open before use and @ref fat12_close when finished.
 */
typedef struct Fat12 {
    /** Backing image file, opened read-write (`rb+`). */
    FILE *fp;
    /** Start offset (bytes) of the FAT12 volume within `fp`. */
    uint64_t base_offset;
    /** Parsed BIOS Parameter Block fields. */
    Fat12BootSector bpb;
    /** Total logical sectors in this volume. */
    uint32_t total_sectors;
    /** Root directory size in sectors. */
    uint32_t root_dir_sectors;
    /** Absolute byte offset of first FAT copy. */
    uint64_t fat_offset;
    /** Absolute byte offset of root directory region. */
    uint64_t root_offset;
    /** Absolute byte offset of data region (cluster #2 base). */
    uint64_t data_offset;
    /** Cluster size in bytes. */
    uint32_t cluster_size;
    /** Count of addressable data clusters in this volume. */
    uint32_t total_clusters;
    /** In-memory FAT copy used for entry updates and flushes. */
    uint8_t *fat;
    /** Size of one FAT copy in bytes. */
    uint32_t fat_size_bytes;
} Fat12;

/**
 * @brief Lightweight file/directory metadata returned by @ref fat12_stat.
 */
typedef struct {
    /** Non-zero if node is a directory, zero for regular file. */
    int is_dir;
    /** POSIX-like mode synthesized by the implementation. */
    mode_t mode;
    /** File size in bytes (0 for directories). */
    uint32_t size;
    /** First data cluster (0 for empty file or root). */
    uint16_t first_cluster;
    /** FAT encoded write time field. */
    uint16_t wrt_time;
    /** FAT encoded write date field. */
    uint16_t wrt_date;
    /** FAT attribute byte. */
    uint8_t attr;
} Fat12Node;

/**
 * @brief Callback signature used by @ref fat12_list.
 *
 * @param name    Entry short name (8.3 text form).
 * @param is_dir  Non-zero if directory.
 * @param size    File size in bytes (directory entries report 0).
 * @param user    Opaque caller context.
 * @return 0 to continue iteration, non-zero to stop early.
 */
typedef int (*fat12_list_cb)(
        const char *name, const Fat12Node *node, void *user);

/**
 * @brief Opens a FAT12 volume from an image path.
 *
 * @param fs                     Output filesystem handle.
 * @param image_path             Backing image path.
 * @param partition_offset_bytes Byte offset of FAT12 volume in `image_path`.
 *                               Use 0 for standalone partition images.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_open(
        Fat12 *fs, const char *image_path, uint64_t partition_offset_bytes);

/**
 * @brief Releases all resources associated with a FAT12 handle.
 *
 * Safe to call on partially initialized handles.
 *
 * @param fs Filesystem handle previously opened by @ref fat12_open.
 */
void fat12_close(Fat12 *fs);

/**
 * @brief Retrieves metadata for a file or directory by absolute FAT path.
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute path (must start with `/`).
 * @param out  Output metadata structure.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_stat(Fat12 *fs, const char *path, Fat12Node *out);

/**
 * @brief Enumerates directory entries.
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute directory path.
 * @param cb   Callback invoked for each visible entry.
 * @param user Opaque callback context.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_list(Fat12 *fs, const char *path, fat12_list_cb cb, void *user);

/**
 * @brief Reads bytes from a file.
 *
 * @param fs     Open filesystem handle.
 * @param path   Absolute file path.
 * @param buf    Destination buffer.
 * @param size   Maximum bytes to read.
 * @param offset Byte offset in file.
 * @return Number of bytes read (>=0), or negative errno-style code.
 */
ssize_t fat12_read(
        Fat12 *fs, const char *path, void *buf, size_t size, off_t offset);

/**
 * @brief Writes bytes into a file at the given offset.
 *
 * Gaps introduced by writing past end-of-file are zero-filled.
 *
 * @param fs     Open filesystem handle.
 * @param path   Absolute file path.
 * @param buf    Source bytes.
 * @param size   Number of bytes to write.
 * @param offset Byte offset in file.
 * @return Number of bytes written (>=0), or negative errno-style code.
 */
ssize_t fat12_write(Fat12 *fs, const char *path, const void *buf, size_t size,
        off_t offset);

/**
 * @brief Creates an empty file.
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute path of new file.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_create(Fat12 *fs, const char *path);

/**
 * @brief Creates a new directory (including `.` and `..` initialization).
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute path of new directory.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_mkdir(Fat12 *fs, const char *path);

/**
 * @brief Removes a regular file.
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute file path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_unlink(Fat12 *fs, const char *path);

/**
 * @brief Removes an empty directory.
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute directory path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_rmdir(Fat12 *fs, const char *path);

/**
 * @brief Resizes a file to an exact length.
 *
 * If extended, new region is zero-filled. If shrunk, excess clusters are freed.
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute file path.
 * @param size Target file size in bytes.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_truncate(Fat12 *fs, const char *path, off_t size);

/**
 * @brief Updates access/write timestamps to the current local time.
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute file or directory path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_utimens_now(Fat12 *fs, const char *path);

/**
 * @brief Updates access/write timestamps to a specific local time.
 *
 * @param fs    Open filesystem handle.
 * @param path  Absolute file or directory path.
 * @param mtime Modification time to set.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_utimens(Fat12 *fs, const char *path, time_t mtime);

/**
 * @brief Sets directory entry attributes.
 *
 * @param fs   Open filesystem handle.
 * @param path Absolute file or directory path.
 * @param attr Attribute byte to set.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_set_attr(Fat12 *fs, const char *path, uint8_t attr);

/**
 * @brief Converts FAT date/time fields to a standard `time_t`.
 *
 * @param fat_time FAT encoded time.
 * @param fat_date FAT encoded date.
 * @return `time_t` representation.
 */
time_t fat12_fat_to_time_t(uint16_t fat_time, uint16_t fat_date);

/**
 * @brief Converts `time_t` to FAT date/time fields.
 *
 * @param t_in     Input time.
 * @param fat_time Output FAT encoded time.
 * @param fat_date Output FAT encoded date.
 */
void fat12_time_t_to_fat(time_t t_in, uint16_t *fat_time, uint16_t *fat_date);


/**
 * @brief Renames or moves a file or directory.
 *
 * @param fs   Open filesystem handle.
 * @param from Source absolute path.
 * @param to   Destination absolute path.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_rename(Fat12 *fs, const char *from, const char *to);

/**
 * @brief Parses MBR partition table and returns selected partition offset.
 *
 * @param img           Path to disk image file.
 * @param partition_idx 1-based partition index; <=0 means offset 0.
 * @param offset_out    Output byte offset for FAT volume.
 * @return 0 on success, negative errno-style code on failure.
 */
int fat12_parse_partition_offset(
        const char *img, int partition_idx, uint64_t *offset_out);

#endif
