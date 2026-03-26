/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file vfs_ops.h
 * @brief VFS abstraction layer for cross-platform filesystem mounting.
 *
 * This header defines the VFS operations interface used by fat12mount to
 * support multiple backends: FUSE on Linux/macOS and WinFSP on Windows.
 */

#ifndef VFS_OPS_H
#define VFS_OPS_H

#include <stddef.h>
#include "fat12_core.h"

/**
 * @brief VFS context containing filesystem state and platform data.
 */
typedef struct VfsContext {
    Fat12 fs;
    void *platform_data;
    void *mutex;
    int mount_fd;
} VfsContext;

#define vfs_error(...) fprintf(stderr, "Error: " __VA_ARGS__)
#define vfs_error_plat(p, ...) fprintf(stderr, "Error: [" p "] " __VA_ARGS__)
#define vfs_info(...) fprintf(stdout, __VA_ARGS__)

/**
 * @brief VFS operations vtable.
 *
 * Each platform implements these operations and returns a pointer to this
 * structure via its *_ops() function.
 */
typedef struct VfsOps {
    int (*init)(VfsContext *ctx, const char *image, const char *mountpoint,
            int partition);
    void (*cleanup)(VfsContext *ctx);
    int (*main_loop)(VfsContext *ctx, int argc, char *argv[]);
    int (*unmount)(const char *mountpoint);
    const char *platform_name;
} VfsOps;

VfsContext *vfs_context_create(void);
void vfs_context_destroy(VfsContext *ctx);

#if defined(_WIN32)
VfsOps *vfs_winfsp_ops(void);
#else
VfsOps *vfs_fuse_ops(void);
#endif

int vfs_parse_args(int argc, char *argv[], char **image, char **mountpoint,
        int *partition, int *unmount);

void vfs_usage(const char *argv0);

#endif
