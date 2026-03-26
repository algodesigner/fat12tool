/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file fat12mount.c
 * @brief Unified entry point for cross-platform FAT12 mounting.
 *
 * This file handles argument parsing and platform dispatch, delegating
 * to the appropriate VFS backend (FUSE or WinFSP).
 */

#include "vfs_ops.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int vfs_parse_args(int argc, char *argv[], char **image, char **mountpoint,
        int *partition, int *unmount)
{
    *image = NULL;
    *mountpoint = NULL;
    *partition = 0;
    *unmount = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            return 1;
        }
        if ((strcmp(argv[i], "--unmount") == 0 || strcmp(argv[i], "-u") == 0) &&
                i + 1 < argc) {
            *unmount = 1;
            *mountpoint = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            *image = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) {
            *mountpoint = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--partition") == 0 && i + 1 < argc) {
            *partition = atoi(argv[++i]);
            continue;
        }
    }

    return 0;
}

void vfs_usage(const char *argv0)
{
    fprintf(stderr, "FAT12 Filesystem Mounter\n\n");
    fprintf(stderr, "Usage: %s [OPTIONS] [PLATFORM_OPTIONS]\n", argv0);
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
    fprintf(stderr, "  %s --image disk.img --mount ./mnt\n", argv0);
    fprintf(stderr, "  %s --image disk.img --partition 1 --mount ./mnt\n",
            argv0);
    fprintf(stderr, "  %s -u ./mnt\n", argv0);
}

#if defined(_WIN32)
VfsOps *vfs_winfsp_ops(void);
#else
VfsOps *vfs_fuse_ops(void);
#endif

VfsContext *vfs_context_create(void);
void vfs_context_destroy(VfsContext *ctx);

int main(int argc, char *argv[])
{
    char *image = NULL, *mountpoint = NULL;
    int partition = 0, unmount_flag = 0;

#if defined(_WIN32)
    {
        HMODULE h = LoadLibrary("winfsp-x64.dll");
        if (!h) {
            char dll_path[MAX_PATH];
            GetModuleFileNameA(NULL, dll_path, sizeof(dll_path));
            char *lastbackslash = strrchr(dll_path, '\\');
            if (lastbackslash) {
                *lastbackslash = '\0';
                strncat(dll_path, "\\winfsp-x64.dll",
                        sizeof(dll_path) - strlen(dll_path) - 1);
                h = LoadLibraryA(dll_path);
            }
        }
        if (!h) {
            fprintf(stderr, "Error: WinFSP DLL not found.\n\n");
            fprintf(stderr,
                    "WinFSP is required to mount FAT12 images on Windows.\n");
            fprintf(stderr,
                    "Please install WinFSP from https://winfsp.dev/ or via:\n");
            fprintf(stderr, "    winget install WinFsp\n\n");
            fprintf(stderr,
                    "After installation, ensure WinFSP bin directory is in "
                    "your PATH.\n");
            return 1;
        }
    }
#endif

    if (vfs_parse_args(argc, argv, &image, &mountpoint, &partition,
                &unmount_flag) != 0) {
        vfs_usage(argv[0]);
        return 1;
    }

#if defined(_WIN32)
    VfsOps *ops = vfs_winfsp_ops();
#else
    VfsOps *ops = vfs_fuse_ops();
#endif

    if (unmount_flag) {
        if (!mountpoint) {
            vfs_error("Option '--unmount' requires a mount point path.\n");
            vfs_usage(argv[0]);
            return 1;
        }
        int rc = ops->unmount(mountpoint);
        return rc;
    }

    if (!image) {
        vfs_error("Option '--image' requires a value.\n");
        vfs_usage(argv[0]);
        return 1;
    }

    if (!mountpoint) {
        vfs_error("Option '--mount' requires a value.\n");
        vfs_usage(argv[0]);
        return 1;
    }

    struct stat st_img;
    if (stat(image, &st_img) != 0) {
        vfs_error("Image file '%s' not found or inaccessible.\n", image);
        return 1;
    }

#if defined(_WIN32)
    // On Windows with WinFSP, mountpoint can be:
    // 1. A drive letter like "X:" (no check needed)
    // 2. A folder path (WinFSP will create it if it doesn't exist, so we allow
    // non-existent paths) Only check if it exists and is not a directory, and
    // ensure it's empty
    if (!(strlen(mountpoint) == 2 && mountpoint[1] == ':')) {
        struct stat st;
        if (stat(mountpoint, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                vfs_error("Mount point '%s' exists but is not a directory.\n",
                        mountpoint);
                return 1;
            }
            // Check if directory is empty (only . and .. entries)
            DIR *d = opendir(mountpoint);
            if (d) {
                int has_entries = 0;
                struct dirent *entry;
                while ((entry = readdir(d)) != NULL) {
                    if (strcmp(entry->d_name, ".") != 0 &&
                            strcmp(entry->d_name, "..") != 0) {
                        has_entries = 1;
                        break;
                    }
                }
                closedir(d);
                if (has_entries) {
                    vfs_error(
                            "Mount point '%s' is not empty. Use an empty "
                            "directory.\n",
                            mountpoint);
                    return 1;
                }
            }
        }
    }
#else
    struct stat st;
    if (stat(mountpoint, &st) != 0) {
        vfs_error(
                "Mount point '%s' does not exist. Create it first: mkdir -p "
                "%s\n",
                mountpoint, mountpoint);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        vfs_error("Mount point '%s' is not a directory.\n", mountpoint);
        return 1;
    }
#endif

    VfsContext *ctx = vfs_context_create();
    if (!ctx) {
        vfs_error("Failed to allocate context.\n");
        return 1;
    }

    int rc = ops->init(ctx, image, mountpoint, partition);
    if (rc < 0) {
        vfs_error("Failed to initialise filesystem '%s': %s\n", image,
                strerror(-rc));
        vfs_context_destroy(ctx);
        return 1;
    }

    vfs_info("Mounting FAT12 image '%s' at '%s' using %s\n", image, mountpoint,
            ops->platform_name);

    rc = ops->main_loop(ctx, argc, argv);
    ops->cleanup(ctx);
    vfs_context_destroy(ctx);

    return rc;
}
