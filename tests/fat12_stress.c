/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * @file fat12_stress.c
 * @brief High-performance integration stress test for FAT12 mount points.
 *
 * This tool performs bulk file operations against a mounted directory
 * without spawning external processes, significantly speeding up tests on Windows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif
#else
#define O_BINARY 0
#endif

static int test_dir_stress(const char *mnt) {
    size_t mnt_len = strlen(mnt);
    if (mnt_len > 240) {
        fprintf(stderr, "Mount path too long for test buffers\n");
        return -1;
    }
    
    char sub[256];  /* mnt (240) + /STRESS (7) + NUL = 248 */
    char path[512];  /* sub + /F_XXX.TXT (11) + NUL = 259, compiler warning is overly cautious */
    
    snprintf(sub, sizeof(sub), "%s/STRESS", mnt);
    
    printf("  Testing directory visibility stress (100 files) at %s...\n", sub);
    if (mkdir(sub, 0755) != 0 && errno != EEXIST) {
        perror("mkdir STRESS");
        return -1;
    }

    for (int i = 1; i <= 100; ++i) {
        snprintf(path, sizeof(path), "%s/F_%03d.TXT", sub, i);
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "Failed to create %s: %s\n", path, strerror(errno));
            return -1;
        }
        if (fwrite("test", 1, 4, f) != 4) {
            perror("fwrite");
            fclose(f);
            return -1;
        }
        fclose(f);
    }

    DIR *d = opendir(sub);
    if (!d) {
        perror("opendir STRESS");
        return -1;
    }
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "F_", 2) == 0) count++;
    }
    closedir(d);

    if (count != 100) {
        fprintf(stderr, "Error: Expected 100 files, found %d\n", count);
        return -1;
    }
    return 0;
}

static int test_interleaved(const char *mnt) {
    size_t mnt_len = strlen(mnt);
    if (mnt_len + 9 > 256 || mnt_len + 11 > 512) {
        fprintf(stderr, "Mount path too long for test buffers\n");
        return -1;
    }
    
    char sub[256];
    char path[512];
    
    snprintf(sub, sizeof(sub), "%s/CACHE", mnt);
    
    printf("  Testing interleaved modifications (50 files)...\n");
    if (mkdir(sub, 0755) != 0 && errno != EEXIST) {
        perror("mkdir CACHE");
        return -1;
    }

    for (int i = 1; i <= 50; ++i) {
        snprintf(path, sizeof(path), "%s/FILE_%02d", sub, i);
        FILE *f = fopen(path, "wb");
        if (!f) return -1;
        fclose(f);
        if (access(path, F_OK) != 0) {
            fprintf(stderr, "File %s not visible immediately after creation\n", path);
            return -1;
        }
    }

    for (int i = 1; i <= 50; i += 2) {
        snprintf(path, sizeof(path), "%s/FILE_%02d", sub, i);
        if (unlink(path) != 0) {
            perror("unlink");
            return -1;
        }
        if (access(path, F_OK) == 0) {
            fprintf(stderr, "File %s still visible after deletion\n", path);
            return -1;
        }
    }
    return 0;
}

static int test_rename_truncate(const char *mnt) {
    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "%s/REN_ME.TXT", mnt);
    snprintf(p2, sizeof(p2), "%s/RENAMED.TXT", mnt);

    printf("  Testing rename and truncate...\n");
    FILE *f = fopen(p1, "wb");
    if (!f) { perror("fopen p1"); return -1; }
    if (fwrite("original", 1, 8, f) != 8) { perror("fwrite p1"); fclose(f); return -1; }
    fclose(f);

    if (rename(p1, p2) != 0) {
        perror("rename");
        return -1;
    }

    if (access(p1, F_OK) == 0) { fprintf(stderr, "Source still exists after rename\n"); return -1; }
    if (access(p2, F_OK) != 0) { fprintf(stderr, "Dest does not exist after rename\n"); return -1; }

    f = fopen(p2, "wb");
    if (!f) { perror("fopen p2 truncate"); return -1; }
    if (fwrite("short", 1, 5, f) != 5) { perror("fwrite p2"); fclose(f); return -1; }
    fclose(f);

    struct stat st;
    if (stat(p2, &st) != 0) { perror("stat p2"); return -1; }
    if (st.st_size != 5) { fprintf(stderr, "Truncate failed, size is %ld\n", (long)st.st_size); return -1; }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <mount-point>\n", argv[0]);
        return 1;
    }

    const char *mnt = argv[1];
    if (test_dir_stress(mnt) != 0) return 1;
    if (test_interleaved(mnt) != 0) return 1;
    if (test_rename_truncate(mnt) != 0) return 1;

    printf("PASS: fat12_stress C-based integration checks\n");
    return 0;
}
