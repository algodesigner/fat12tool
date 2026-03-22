/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file fat12_fuse.c
 * @brief FUSE frontend that exposes a FAT12 image as a mountable filesystem.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _FILE_OFFSET_BITS 64

#include "fat12_core.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__linux__)
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#define FAT12_RENAME_SIG const char *from, const char *to, unsigned int flags
#define FAT12_FILLER_FLAGS , 0, (enum fuse_fill_dir_flags)0
#define FAT12_TRUNCATE_SIG \
    const char *path, off_t size, struct fuse_file_info *fi
#define FAT12_UTIMENS_SIG \
    const char *path, const struct timespec tv[2], struct fuse_file_info *fi
#else
#define FUSE_USE_VERSION 26
#include <fuse.h>
#define FAT12_RENAME_SIG const char *from, const char *to
#define FAT12_FILLER_FLAGS , 0
#define FAT12_TRUNCATE_SIG const char *path, off_t size
#define FAT12_UTIMENS_SIG const char *path, const struct timespec tv[2]
#endif

#pragma pack(push, 1)
typedef struct {
    Fat12 fs;
    /** Global lock protecting non-thread-safe core operations. */
    pthread_mutex_t lock;
    /** Track last directory read on macOS to avoid readdir loops. */
    char readdir_path[256];
    int readdir_ready;
} Fat12Ctx;
#pragma pack(pop)

static int fat12_fill_dir(void *buf, fuse_fill_dir_t filler, const char *name,
        const struct stat *st, off_t off)
{
#if defined(__linux__)
    return filler(buf, name, st, off, (enum fuse_fill_dir_flags)0);
#else
    (void)off;
    return filler(buf, name, st, 0);
#endif
}

static uint16_t fat12_fat_get(const Fat12 *fs, uint16_t cluster)
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
 * @brief Resolve process-global FUSE private data into typed context.
 * @return Pointer to mount context stored in FUSE private data.
 */
static Fat12Ctx *ctx_from_fuse(void)
{
    return (Fat12Ctx *)fuse_get_context()->private_data;
}

static ino_t fat12_hash_ino(const char *path)
{
    uint32_t h = 5381u;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        h = ((h << 5) + h) + *p;
    if (h == 0)
        h = 1;
    return (ino_t)h;
}

/**
 * @brief Convert FAT encoded date/time fields into POSIX timespec.
 * @param fat_time FAT time field.
 * @param fat_date FAT date field.
 * @param ts Output timespec structure.
 */
static void fat_time_to_timespec(
        uint16_t fat_time, uint16_t fat_date, struct timespec *ts)
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

    ts->tv_sec = mktime(&tmv);
    ts->tv_nsec = 0;
}

/**
 * @brief FUSE getattr callback backed by fat12_stat.
 * @param path Absolute virtual path.
 * @param st Output stat structure.
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_getattr(const char *path, struct stat *st)
{
    Fat12Ctx *ctx = ctx_from_fuse();
    memset(st, 0, sizeof(*st));

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_ino = 1;
        return 0;
    }

    pthread_mutex_lock(&ctx->lock);
    Fat12Node node;
    int rc = fat12_stat(&ctx->fs, path, &node);
    pthread_mutex_unlock(&ctx->lock);
    if (rc < 0)
        return rc;

    st->st_mode = node.mode;
    st->st_nlink = node.is_dir ? 2 : 1;
    st->st_size = node.size;
    if (node.first_cluster != 0)
        st->st_ino = node.first_cluster;
    else
        st->st_ino = fat12_hash_ino(path);
    struct timespec ts;
    fat_time_to_timespec(node.wrt_time, node.wrt_date, &ts);
#if defined(__linux__)
    st->st_mtim = ts;
    st->st_atim = ts;
    st->st_ctim = ts;
#elif defined(__APPLE__)
    st->st_mtimespec = ts;
    st->st_atimespec = ts;
    st->st_ctimespec = ts;
#else
    st->st_mtime = ts.tv_sec;
    st->st_atime = ts.tv_sec;
    st->st_ctime = ts.tv_sec;
#endif
    return 0;
}

typedef struct {
    void *buf;
    fuse_fill_dir_t filler;
    off_t off_in;
    off_t next_off;
} ReaddirState;

static int fat12_readdir_add(
        ReaddirState *state, const char *name, const struct stat *st)
{
    struct stat tmp;
    const struct stat *st_use = st;
    if (st) {
        tmp = *st;
        if (tmp.st_ino == 0)
            tmp.st_ino = (ino_t)(state->next_off + 1);
        st_use = &tmp;
    }
    if (state->next_off < state->off_in) {
        state->next_off++;
        return 0;
    }
    int rc = fat12_fill_dir(
            state->buf, state->filler, name, st_use, state->next_off + 1);
    state->next_off++;
    if (rc != 0)
        return 1;
    return 0;
}

/**
 * @brief Adapter callback forwarding core list entries to FUSE filler.
 * @param name Entry name.
 * @param is_dir Non-zero if directory.
 * @param size Entry size in bytes.
 * @param user Opaque pointer carrying buffer and filler callback.
 * @return 0 to continue iteration, non-zero to stop.
 */
static int list_cb(const char *name, int is_dir, uint32_t size, void *user)
{
    ReaddirState *state = user;
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = (is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    st.st_nlink = is_dir ? 2 : 1;
    st.st_size = (off_t)size;
    if (fat12_readdir_add(state, name, &st) != 0)
        return 1;
    return 0;
}

#if defined(__linux__)
/**
 * @brief FUSE readdir callback (Linux signature).
 * @param path Directory path.
 * @param buf FUSE directory buffer.
 * @param filler FUSE filler callback.
 * @param off Directory offset (unused).
 * @param fi FUSE file info (unused).
 * @param flags Readdir flags (unused).
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
        off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    (void)off;
    (void)fi;
    (void)flags;
#else
/**
 * @brief FUSE readdir callback (macOS / FUSE2 signature).
 * @param path Directory path.
 * @param buf FUSE directory buffer.
 * @param filler FUSE filler callback.
 * @param off Directory offset (unused).
 * @param fi FUSE file info (unused).
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
        off_t off, struct fuse_file_info *fi)
{
    (void)fi;
#endif
    Fat12Ctx *ctx = ctx_from_fuse();

#if !defined(__linux__)
    if (off > 0)
        return 0;
    if (ctx->readdir_ready && strcmp(path, ctx->readdir_path) == 0)
        return 0;
    ctx->readdir_ready = 1;
    snprintf(ctx->readdir_path, sizeof(ctx->readdir_path), "%s", path);
#endif

    ReaddirState state = {buf, filler, off, 0};
    if (fat12_readdir_add(&state, ".", NULL) != 0)
        return 0;
    if (fat12_readdir_add(&state, "..", NULL) != 0)
        return 0;

    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_list(&ctx->fs, path, list_cb, &state);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

/**
 * @brief FUSE open callback validating path and access mode.
 * @param path File path.
 * @param fi FUSE file info containing requested open flags.
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_open(const char *path, struct fuse_file_info *fi)
{
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    Fat12Node node;
    int rc = fat12_stat(&ctx->fs, path, &node);
    pthread_mutex_unlock(&ctx->lock);
    if (rc < 0)
        return rc;
    if (node.is_dir)
        return -EISDIR;

    int mode = fi->flags & O_ACCMODE;
    if (mode != O_RDONLY && mode != O_WRONLY && mode != O_RDWR)
        return -EACCES;
    return 0;
}

static int fat12fs_opendir(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    Fat12Node node;
    int rc = fat12_stat(&ctx->fs, path, &node);
    pthread_mutex_unlock(&ctx->lock);
    if (rc < 0)
        return rc;
    if (!node.is_dir)
        return -ENOTDIR;
#if !defined(__linux__)
    ctx->readdir_ready = 0;
    snprintf(ctx->readdir_path, sizeof(ctx->readdir_path), "%s", path);
#endif
    return 0;
}

/**
 * @brief FUSE read callback delegating to fat12_read.
 * @param path File path.
 * @param buf Destination buffer.
 * @param size Maximum bytes to read.
 * @param off File offset.
 * @param fi FUSE file info (unused).
 * @return Bytes read or negative errno-style code.
 */
static int fat12fs_read(const char *path, char *buf, size_t size, off_t off,
        struct fuse_file_info *fi)
{
    (void)fi;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    ssize_t n = fat12_read(&ctx->fs, path, buf, size, off);
    pthread_mutex_unlock(&ctx->lock);
    if (n < 0)
        return (int)n;
    return (int)n;
}

/**
 * @brief FUSE write callback delegating to fat12_write.
 * @param path File path.
 * @param buf Source buffer.
 * @param size Number of bytes to write.
 * @param off File offset.
 * @param fi FUSE file info (unused).
 * @return Bytes written or negative errno-style code.
 */
static int fat12fs_write(const char *path, const char *buf, size_t size,
        off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    ssize_t n = fat12_write(&ctx->fs, path, buf, size, off);
    pthread_mutex_unlock(&ctx->lock);
    if (n < 0)
        return (int)n;
    return (int)n;
}

/**
 * @brief FUSE create callback creating empty regular files.
 * @param path File path to create.
 * @param mode Requested mode (ignored; FAT permissions are fixed).
 * @param fi FUSE file info (unused).
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_create(
        const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    (void)fi;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_create(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

/**
 * @brief FUSE mkdir callback.
 * @param path Directory path to create.
 * @param mode Requested mode (ignored; FAT permissions are fixed).
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_mkdir(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

/**
 * @brief FUSE unlink callback.
 * @param path File path to remove.
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_unlink(const char *path)
{
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_unlink(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

/**
 * @brief FUSE rmdir callback.
 * @param path Directory path to remove.
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_rmdir(const char *path)
{
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_rmdir(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

/**
 * @brief FUSE truncate callback.
 * @param path File path to resize.
 * @param size Target file size in bytes.
 * @param fi FUSE file info (unused).
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_truncate(FAT12_TRUNCATE_SIG)
{
#if defined(__linux__)
    (void)fi;
#endif
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_truncate(&ctx->fs, path, size);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

/**
 * @brief FUSE utimens callback mapping to current-time metadata update.
 * @param path Target path.
 * @param tv Requested times (ignored; current time is applied).
 * @param fi FUSE file info (unused).
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_utimens(FAT12_UTIMENS_SIG)
{
    (void)tv;
#if defined(__linux__)
    (void)fi;
#endif
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_utimens_now(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int fat12fs_statfs(const char *path, struct statvfs *st)
{
    (void)path;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    const Fat12 *fs = &ctx->fs;
    memset(st, 0, sizeof(*st));
    st->f_bsize = fs->bpb.bytes_per_sector;
    st->f_frsize = fs->cluster_size;
    st->f_blocks = fs->total_clusters;
    uint32_t free_clusters = 0;
    for (uint32_t i = 2; i < fs->total_clusters + 2; ++i) {
        if (fat12_fat_get(fs, (uint16_t)i) == 0)
            free_clusters++;
    }
    st->f_bfree = free_clusters;
    st->f_bavail = free_clusters;
    st->f_namemax = 12;
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static int fat12fs_releasedir(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return 0;
}

/**
 * @brief FUSE rename callback.
 * @param from Source path.
 * @param to Destination path.
#if defined(__linux__)
 * @param flags Linux rename flags (must be zero).
#endif
 * @return 0 on success, negative errno-style code on failure.
 */
static int fat12fs_rename(FAT12_RENAME_SIG)
{
#if defined(__linux__)
    if (flags != 0)
        return -EINVAL;
#endif
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_rename(&ctx->fs, from, to);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static const struct fuse_operations fat12_ops = {
        .getattr = fat12fs_getattr,
        .statfs = fat12fs_statfs,
        .opendir = fat12fs_opendir,
        .readdir = fat12fs_readdir,
        .releasedir = fat12fs_releasedir,
        .open = fat12fs_open,
        .read = fat12fs_read,
        .write = fat12fs_write,
        .create = fat12fs_create,
        .mkdir = fat12fs_mkdir,
        .unlink = fat12fs_unlink,
        .rmdir = fat12fs_rmdir,
        .truncate = fat12fs_truncate,
        .utimens = fat12fs_utimens,
        .rename = fat12fs_rename,
};

/**
 * @brief Print command usage for mount mode.
 * @param argv0 Program name for usage text.
 */
static void usage(const char *argv0)
{
    fprintf(stderr, "FAT12 FUSE Mounter\n\n");
    fprintf(stderr, "Usage: %s [OPTIONS] [FUSE_OPTIONS]\n", argv0);
    fprintf(stderr, "       %s --unmount <mountpoint>\n\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr,
            "  --image PATH      Path to FAT12 image or full MBR disk image\n");
    fprintf(stderr, "  --mount PATH      Directory to use as mount point\n");
    fprintf(stderr,
            "  --partition N     Partition index (1-4) for MBR images "
            "(default: 0/image start)\n");
    fprintf(stderr, "  --unmount, -u     Unmount the specified directory\n");
    fprintf(stderr, "  --help, -h        Show this help message\n\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s --image disk.img --mount ./mnt -f\n", argv0);
    fprintf(stderr, "  %s --image disk.img --partition 1 --mount ./mnt -f\n",
            argv0);
    fprintf(stderr, "  %s -u ./mnt\n", argv0);
}

/**
 * @brief Entry point for FAT12 FUSE mount executable.
 *
 * Parses tool-specific options, opens FAT12 core, then transfers control to
 * `fuse_main()` until unmount.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Process exit status.
 */
int main(int argc, char **argv)
{
    const char *image = NULL;
    const char *mountpoint = NULL;
    int partition = 0;
    int unmount = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if ((strcmp(argv[i], "--unmount") == 0 || strcmp(argv[i], "-u") == 0) &&
                i + 1 < argc) {
            unmount = 1;
            mountpoint = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            image = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) {
            mountpoint = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--partition") == 0 && i + 1 < argc) {
            partition = atoi(argv[++i]);
            continue;
        }
    }

    if (unmount) {
        if (!mountpoint) {
            usage(argv[0]);
            return 1;
        }
        char cmd[1024];
#if defined(__APPLE__)
        snprintf(cmd, sizeof(cmd), "umount \"%s\"", mountpoint);
#else
        snprintf(cmd, sizeof(cmd), "fusermount3 -u \"%s\"", mountpoint);
#endif
        return system(cmd);
    }

    if (!image || !mountpoint) {
        usage(argv[0]);
        return 1;
    }

    struct stat st;
    if (stat(image, &st) != 0) {
        fprintf(stderr, "Error: Image file '%s' not found or inaccessible.\n",
                image);
        return 1;
    }
    if (stat(mountpoint, &st) != 0) {
        fprintf(stderr, "Error: Mount point '%s' does not exist.\n",
                mountpoint);
        fprintf(stderr, "Please create it first: mkdir -p %s\n", mountpoint);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Mount point '%s' is not a directory.\n",
                mountpoint);
        return 1;
    }

    char **fuse_argv = (char **)calloc((size_t)argc + 4, sizeof(char *));
    if (!fuse_argv)
        return 1;

    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = strdup(argv[0]);
    fuse_argv[fuse_argc++] = strdup(mountpoint);
    if (!fuse_argv[0] || !fuse_argv[1]) {
        for (int j = 0; j < fuse_argc; ++j)
            free(fuse_argv[j]);
        free(fuse_argv);
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            ++i;
            continue;
        }
        if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) {
            ++i;
            continue;
        }
        if (strcmp(argv[i], "--partition") == 0 && i + 1 < argc) {
            ++i;
            continue;
        }
        fuse_argv[fuse_argc] = strdup(argv[i]);
        if (!fuse_argv[fuse_argc]) {
            for (int j = 0; j < fuse_argc; ++j)
                free(fuse_argv[j]);
            free(fuse_argv);
            return 1;
        }
        fuse_argc++;
    }

#if defined(__APPLE__)
    fuse_argv[fuse_argc] = strdup("-o");
    if (!fuse_argv[fuse_argc]) {
        for (int j = 0; j < fuse_argc; ++j)
            free(fuse_argv[j]);
        free(fuse_argv);
        return 1;
    }
    fuse_argc++;
    fuse_argv[fuse_argc] =
            strdup("no_remote_lock,no_remote_flock,no_remote_posix_lock");
    if (!fuse_argv[fuse_argc]) {
        for (int j = 0; j < fuse_argc; ++j)
            free(fuse_argv[j]);
        free(fuse_argv);
        return 1;
    }
    fuse_argc++;
#endif

    uint64_t offset;
    int rc = fat12_parse_partition_offset(image, partition, &offset);
    if (rc < 0) {
        fprintf(stderr, "Failed to parse partition offset: %s\n",
                strerror(-rc));
        free(fuse_argv);
        return 1;
    }

    Fat12Ctx *ctx = (Fat12Ctx *)calloc(1, sizeof(Fat12Ctx));
    if (!ctx) {
        free(fuse_argv);
        return 1;
    }

    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        free(ctx);
        free(fuse_argv);
        return 1;
    }

    rc = fat12_open(&ctx->fs, image, offset);
    if (rc < 0) {
        fprintf(stderr, "Cannot open FAT12 filesystem: %s\n", strerror(-rc));
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        free(fuse_argv);
        return 1;
    }

    fuse_argv[fuse_argc] = NULL;

    int ret = 0;
#if defined(__linux__)
    ret = fuse_main(fuse_argc, fuse_argv, &fat12_ops, ctx);
#else
    char *fuse_mountpoint = NULL;
    int multithreaded = 0;
    struct fuse *fuse = fuse_setup(fuse_argc, fuse_argv, &fat12_ops,
            sizeof(fat12_ops), &fuse_mountpoint, &multithreaded, ctx);
    if (!fuse) {
        ret = 1;
    } else {
        if (multithreaded)
            ret = fuse_loop_mt(fuse);
        else
            ret = fuse_loop(fuse);
        fuse_teardown(fuse, fuse_mountpoint);
        if (ret != 0)
            ret = 1;
    }
#endif

    fat12_close(&ctx->fs);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    for (int i = 0; i < fuse_argc; ++i)
        free(fuse_argv[i]);
    free(fuse_argv);
    return ret;
}
