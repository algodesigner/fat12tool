/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file vfs_winfsp.c
 * @brief WinFSP frontend for FAT12 filesystem mounting on Windows.
 */

#ifdef _WIN32

#define _GNU_SOURCE

#include <assert.h>
#include <stddef.h>
#include <windows.h>

/* WinFSP headers */
#include <winfsp/winfsp.h>

#define FUSE_USE_VERSION 28
#include <fuse/fuse.h>

#include "fat12_core.h"
#include "vfs_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* FUSE 2.8 Signatures for WinFSP */
#define FAT12_RENAME_SIG const char *from, const char *to
#define FAT12_TRUNCATE_SIG const char *path, fuse_off_t size
#define FAT12_UTIMENS_SIG const char *path, const struct fuse_timespec tv[2]
#define FAT12_GETATTR_SIG const char *path, struct fuse_stat *st

typedef struct {
    Fat12 fs;
    pthread_mutex_t lock;
    char mountpoint[MAX_PATH];
    char image_path[MAX_PATH];
    int partition;
} Fat12Ctx;

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

static Fat12Ctx *ctx_from_fuse(void)
{
    return (Fat12Ctx *)fuse_get_context()->private_data;
}

static fuse_ino_t fat12_hash_ino(const char *path)
{
    uint32_t h = 5381u;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        h = ((h << 5) + h) + *p;
    if (h == 0)
        h = 1;
    return (fuse_ino_t)h;
}

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

static int fat12fs_getattr(FAT12_GETATTR_SIG)
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
    st->st_mtim.tv_sec = ts.tv_sec;
    st->st_atim.tv_sec = ts.tv_sec;
    st->st_ctim.tv_sec = ts.tv_sec;
    return 0;
}

typedef struct {
    void *buf;
    fuse_fill_dir_t filler;
    fuse_off_t off_in;
    fuse_off_t next_off;
} ReaddirState;

static int fat12_readdir_add(
        ReaddirState *state, const char *name, const struct fuse_stat *st)
{
    struct fuse_stat tmp;
    const struct fuse_stat *st_use = st;
    if (st) {
        tmp = *st;
        if (tmp.st_ino == 0)
            tmp.st_ino = (fuse_ino_t)(state->next_off + 1);
        st_use = &tmp;
    }
    if (state->next_off < state->off_in) {
        state->next_off++;
        return 0;
    }
    if (state->filler(state->buf, name, st_use, state->next_off + 1) != 0)
        return 1;
    state->next_off++;
    return 0;
}

static int list_cb(const char *name, const Fat12Node *node, void *user)
{
    ReaddirState *state = user;
    struct fuse_stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = node->is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st.st_nlink = node->is_dir ? 2 : 1;
    st.st_size = (fuse_off_t)node->size;
    if (node->first_cluster != 0)
        st.st_ino = node->first_cluster;

    struct timespec ts;
    fat_time_to_timespec(node->wrt_time, node->wrt_date, &ts);
    st.st_mtim.tv_sec = ts.tv_sec;
    st.st_atim.tv_sec = ts.tv_sec;
    st.st_ctim.tv_sec = ts.tv_sec;

    if (fat12_readdir_add(state, name, &st) != 0)
        return 1;
    return 0;
}

static int fat12fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
        fuse_off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    Fat12Ctx *ctx = ctx_from_fuse();

    ReaddirState state = {buf, filler, off, 0};

    if (off == 0) {
        if (filler(buf, ".", NULL, 1) != 0)
            return 0;
        if (filler(buf, "..", NULL, 2) != 0)
            return 0;
        state.next_off = 2;
    }

    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_list(&ctx->fs, path, list_cb, &state);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

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
    return 0;
}

static int fat12fs_read(const char *path, char *buf, size_t size,
        fuse_off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    ssize_t n = fat12_read(&ctx->fs, path, buf, size, (off_t)off);
    pthread_mutex_unlock(&ctx->lock);
    if (n < 0)
        return (int)n;
    return (int)n;
}

static int fat12fs_write(const char *path, const char *buf, size_t size,
        fuse_off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    ssize_t n = fat12_write(&ctx->fs, path, buf, size, (off_t)off);
    pthread_mutex_unlock(&ctx->lock);
    if (n < 0)
        return (int)n;
    return (int)n;
}

static int fat12fs_create(
        const char *path, fuse_mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    (void)fi;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_create(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int fat12fs_mkdir(const char *path, fuse_mode_t mode)
{
    (void)mode;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_mkdir(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int fat12fs_unlink(const char *path)
{
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_unlink(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int fat12fs_rmdir(const char *path)
{
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_rmdir(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int fat12fs_truncate(FAT12_TRUNCATE_SIG)
{
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_truncate(&ctx->fs, path, (off_t)size);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int fat12fs_utimens(FAT12_UTIMENS_SIG)
{
    (void)tv;
    Fat12Ctx *ctx = ctx_from_fuse();
    pthread_mutex_lock(&ctx->lock);
    int rc = fat12_utimens_now(&ctx->fs, path);
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

static int fat12fs_statfs(const char *path, struct fuse_statvfs *st)
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

static int fat12fs_rename(FAT12_RENAME_SIG)
{
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

static int vfs_winfs_init(VfsContext *ctx, const char *image,
        const char *mountpoint, int partition)
{
    Fat12Ctx *fctx = (Fat12Ctx *)ctx->platform_data;
    strncpy(fctx->mountpoint, mountpoint, MAX_PATH - 1);
    fctx->mountpoint[MAX_PATH - 1] = '\0';
    strncpy(fctx->image_path, image, MAX_PATH - 1);
    fctx->image_path[MAX_PATH - 1] = '\0';
    fctx->partition = partition;

    uint64_t offset;
    int rc = fat12_parse_partition_offset(image, partition, &offset);
    if (rc < 0) {
        return rc;
    }

    rc = fat12_open(&fctx->fs, image, offset);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

static void vfs_winfs_cleanup(VfsContext *ctx)
{
    Fat12Ctx *fctx = (Fat12Ctx *)ctx->platform_data;
    fat12_close(&fctx->fs);
}

static int vfs_winfs_main_loop(VfsContext *ctx, int argc, char *argv[])
{
    Fat12Ctx *fctx = (Fat12Ctx *)ctx->platform_data;

    int foreground = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-f") == 0) {
            foreground = 1;
            break;
        }
    }

#if defined(_WIN32)
    if (!foreground) {
        STARTUPINFO si = {0};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {0};
        char cmdline[1024];
        snprintf(cmdline, sizeof(cmdline),
                "\"%s\" --image \"%s\" --mount \"%s\" -f", argv[0],
                fctx->image_path, fctx->mountpoint);
        if (fctx->partition > 0) {
            char part[16];
            snprintf(part, sizeof(part), " --partition %d", fctx->partition);
            strncat(cmdline, part, sizeof(cmdline) - strlen(cmdline) - 1);
        }
        if (!CreateProcess(NULL, cmdline, NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
                    NULL, NULL, &si, &pi)) {
            fprintf(stderr, "Failed to spawn mount process: %lu\n",
                    GetLastError());
        } else {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        return 0;
    }
#endif

    char **fuse_argv = (char **)calloc((size_t)argc + 8, sizeof(char *));
    if (!fuse_argv)
        return 1;

    // Convert mountpoint to Windows absolute path
    char win_mountpoint[MAX_PATH];
    if (fctx->mountpoint[0] == '/' ||
            (fctx->mountpoint[0] == '.' && fctx->mountpoint[1] == '/')) {
        // Relative path - get absolute
        char cwd[MAX_PATH];
        if (GetCurrentDirectoryA(sizeof(cwd), cwd)) {
            if (fctx->mountpoint[0] == '.') {
                snprintf(win_mountpoint, sizeof(win_mountpoint), "%s%s", cwd,
                        fctx->mountpoint + 1);
            } else {
                // Unix-style absolute path
                snprintf(win_mountpoint, sizeof(win_mountpoint), "%s%s", cwd,
                        fctx->mountpoint);
            }
        } else {
            strncpy(win_mountpoint, fctx->mountpoint, MAX_PATH - 1);
            win_mountpoint[MAX_PATH - 1] = '\0';
        }
    } else {
        strncpy(win_mountpoint, fctx->mountpoint, MAX_PATH - 1);
        win_mountpoint[MAX_PATH - 1] = '\0';
    }
    // Convert forward slashes to backslashes for Windows API and WinFSP
    for (char *p = win_mountpoint; *p; ++p) {
        if (*p == '/')
            *p = '\\';
    }

    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = strdup(argv[0]);
    fuse_argv[fuse_argc++] = strdup(win_mountpoint);

    fuse_argv[fuse_argc++] = strdup("-i");
    fuse_argv[fuse_argc++] = strdup("-f");
    fuse_argv[fuse_argc++] = strdup("-o");
    fuse_argv[fuse_argc++] = strdup("uid=-1,gid=-1,umask=0");

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
        if (strcmp(argv[i], "-f") == 0) {
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

    fuse_argv[fuse_argc] = NULL;

    int ret = fuse_main(fuse_argc, fuse_argv, &fat12_ops, fctx);

    for (int i = 0; i < fuse_argc; ++i)
        free(fuse_argv[i]);
    free(fuse_argv);

    return ret;
}

static int vfs_winfs_unmount(const char *mountpoint)
{
    char win_mountpoint[MAX_PATH];
    strncpy(win_mountpoint, mountpoint, MAX_PATH - 1);
    win_mountpoint[MAX_PATH - 1] = '\0';
    for (char *p = win_mountpoint; *p; ++p) {
        if (*p == '/')
            *p = '\\';
    }

    fuse_unmount(win_mountpoint, NULL);
#if defined(_WIN32)
    if (RemoveDirectoryA(win_mountpoint)) {
        vfs_info("Unmounted and removed '%s'\n", mountpoint);
    } else {
        vfs_info(
                "Unmounted '%s' (directory may already be removed or not "
                "empty)\n",
                mountpoint);
    }
#else
    vfs_info("Successfully unmounted '%s'\n", mountpoint);
#endif
    return 0;
}

static VfsOps vfs_winfs_ops_instance = {
        .init = vfs_winfs_init,
        .cleanup = vfs_winfs_cleanup,
        .main_loop = vfs_winfs_main_loop,
        .unmount = vfs_winfs_unmount,
        .platform_name = "WinFSP",
};

VfsOps *vfs_winfsp_ops(void)
{
    return &vfs_winfs_ops_instance;
}

VfsContext *vfs_context_create(void)
{
    VfsContext *ctx = (VfsContext *)calloc(1, sizeof(VfsContext));
    if (!ctx)
        return NULL;

    Fat12Ctx *fctx = (Fat12Ctx *)calloc(1, sizeof(Fat12Ctx));
    if (!fctx) {
        free(ctx);
        return NULL;
    }

    if (pthread_mutex_init(&fctx->lock, NULL) != 0) {
        free(fctx);
        free(ctx);
        return NULL;
    }

    ctx->platform_data = fctx;
    return ctx;
}

void vfs_context_destroy(VfsContext *ctx)
{
    if (ctx) {
        Fat12Ctx *fctx = (Fat12Ctx *)ctx->platform_data;
        if (fctx) {
            pthread_mutex_destroy(&fctx->lock);
            free(fctx);
        }
        free(ctx);
    }
}

#endif /* _WIN32 */
