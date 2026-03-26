/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file fat12tool.c
 * @brief Interactive FAT12 shell for direct image manipulation.
 */
#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include "fat12_core.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PATH_LEN 8192

typedef struct {
    char cwd_path[MAX_PATH_LEN];
} Session;

static void trim_newline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/**
 * @brief Normalise a path relative to the current working directory.
 * @param sess Current session with cwd.
 * @param in Input path (absolute or relative).
 * @param out Output buffer for normalised absolute path.
 * @param out_cap Output buffer capacity.
 */
static void normalize_path(
        Session *sess, const char *in, char *out, size_t out_cap)
{
    if (out_cap == 0)
        return;

    if (in[0] == '/') {
        strncpy(out, in, out_cap - 1);
        out[out_cap - 1] = '\0';
    } else {
        size_t cwd_len = strlen(sess->cwd_path);
        size_t in_len = strlen(in);

        if (strcmp(sess->cwd_path, "/") == 0) {
            size_t copy_len = in_len < out_cap - 1
                    ? in_len
                    : (out_cap > 1 ? out_cap - 1 : 0);
            out[0] = '/';
            memcpy(out + 1, in, copy_len);
            out[copy_len + 1] = '\0';
        } else {
            size_t avail = out_cap;
            size_t copy_in_len = in_len;

            if (cwd_len + 1 > avail) {
                copy_in_len = 0;
            } else {
                memcpy(out, sess->cwd_path, cwd_len);
                out[cwd_len] = '/';
                if (cwd_len + 1 + copy_in_len + 1 > avail) {
                    copy_in_len = avail > cwd_len + 1 ? avail - cwd_len - 1 : 0;
                }
            }
            memcpy(out + cwd_len + 1, in, copy_in_len);
            out[cwd_len + 1 + copy_in_len] = '\0';
        }
    }
}

static int list_cb(const char *name, int is_dir, uint32_t size, void *user)
{
    (void)user;
    printf("%-14s %s %10u\n", name, is_dir ? "<DIR>" : "     ", size);
    return 0;
}

static void print_help(void)
{
    printf("FAT12 Toolkit Shell\n\n");
    printf("Commands:\n");
    printf("  ls [path]               List directory entries\n");
    printf("  cd <path>               Change current working directory\n");
    printf("  pwd                     Print current working directory\n");
    printf("  cat <path>              Display file contents to stdout\n");
    printf("  read <img> <host>       Copy file from FAT12 image to host\n");
    printf("  write <host> <img>      Copy file from host to FAT12 image\n");
    printf("  touch <path>            Create an empty file\n");
    printf("  mkdir <path>            Create a new directory\n");
    printf("  rm <path>               Delete a file\n");
    printf("  rmdir <path>            Delete an empty directory\n");
    printf("  mv <from> <to>          Rename or move a file/directory\n");
    printf("  stat <path>             Show detailed entry metadata\n");
    printf("  help                    Show this help message\n");
    printf("  exit, quit              Exit the shell\n");
}

static int read_host_file(const char *path, uint8_t **buf, uint32_t *len)
{
    *buf = NULL;
    *len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    uint8_t *data = NULL;
    if (sz > 0) {
        data = (uint8_t *)malloc((size_t)sz);
        if (!data) {
            fclose(fp);
            return -1;
        }
        if (fread(data, 1, (size_t)sz, fp) != (size_t)sz) {
            free(data);
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    *buf = data;
    *len = (uint32_t)sz;
    return 0;
}

static int write_host_file(const char *path, const uint8_t *buf, uint32_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    if (len > 0 && fwrite(buf, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *image = NULL;
    int partition = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s <fat12-image-file> [--partition N]\n",
                    argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--partition") == 0 && i + 1 < argc) {
            partition = atoi(argv[++i]);
            continue;
        }
        if (!image) {
            image = argv[i];
        }
    }

    if (!image) {
        fprintf(stderr, "Usage: %s <fat12-image-file> [--partition N]\n",
                argv[0]);
        return 1;
    }

    uint64_t offset = 0;
    if (partition > 0) {
        if (fat12_parse_partition_offset(image, partition, &offset) != 0) {
            fprintf(stderr, "Failed to parse partition %d in %s\n", partition,
                    image);
            return 1;
        }
    }

    Fat12 fs;
    if (fat12_open(&fs, image, offset) != 0) {
        fprintf(stderr, "Cannot open FAT12 image: %s (offset %llu)\n", image,
                (unsigned long long)offset);
        if (offset == 0) {
            fprintf(stderr,
                    "Hint: If this is a partitioned disk image, try "
                    "--partition 1\n");
        }
        return 1;
    }

    Session sess;
    strcpy(sess.cwd_path, "/");

    printf("FAT12 shell opened: %s\n", argv[1]);
    print_help();

    char line[MAX_PATH_LEN];
    while (1) {
        printf("fat12:%s> ", sess.cwd_path);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;
        trim_newline(line);
        if (line[0] == '\0')
            continue;

        char *args[4] = {0};
        int ac = 0;
        char *save = NULL;
        char *tok = strtok_r(line, " \t", &save);
        while (tok && ac < 4) {
            args[ac++] = tok;
            tok = strtok_r(NULL, " \t", &save);
        }

        if (strcmp(args[0], "exit") == 0 || strcmp(args[0], "quit") == 0)
            break;
        if (strcmp(args[0], "help") == 0) {
            print_help();
            continue;
        }
        if (strcmp(args[0], "pwd") == 0) {
            printf("%s\n", sess.cwd_path);
            continue;
        }
        if (strcmp(args[0], "ls") == 0) {
            char path[MAX_PATH_LEN];
            if (ac >= 2) {
                normalize_path(&sess, args[1], path, sizeof(path));
            } else {
                snprintf(path, sizeof(path), "%s", sess.cwd_path);
            }
            if (fat12_list(&fs, path, list_cb, NULL) != 0) {
                fprintf(stderr, "ls: failed to list %s\n", path);
            }
            continue;
        }
        if (strcmp(args[0], "cd") == 0) {
            if (ac < 2) {
                fprintf(stderr, "cd: missing path\n");
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[1], path, sizeof(path));

            // Special case for .. which isn't handled by normalize_path well
            if (strcmp(args[1], "..") == 0) {
                if (strcmp(sess.cwd_path, "/") != 0) {
                    char *last = strrchr(sess.cwd_path, '/');
                    if (last && last != sess.cwd_path)
                        *last = '\0';
                    else
                        strcpy(sess.cwd_path, "/");
                }
                continue;
            } else if (strcmp(args[1], ".") == 0) {
                continue;
            }

            Fat12Node node;
            if (fat12_stat(&fs, path, &node) != 0 || !node.is_dir) {
                fprintf(stderr, "cd: directory not found: %s\n", path);
            } else {
                snprintf(sess.cwd_path, sizeof(sess.cwd_path), "%s", path);
            }
            continue;
        }
        if (strcmp(args[0], "stat") == 0) {
            if (ac < 2) {
                fprintf(stderr, "stat: missing path\n");
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[1], path, sizeof(path));
            Fat12Node node;
            if (fat12_stat(&fs, path, &node) != 0) {
                fprintf(stderr, "stat: not found: %s\n", path);
                continue;
            }

            // Get name from path for stat output
            char *name_ptr = strrchr(path, '/');
            if (name_ptr)
                name_ptr++;
            else
                name_ptr = path;
            if (*name_ptr == '\0')
                name_ptr = "/";

            printf("name=%s attr=0x%02X cluster=%u size=%u\n", name_ptr,
                    node.attr, node.first_cluster, node.size);
            continue;
        }
        if (strcmp(args[0], "cat") == 0) {
            if (ac < 2) {
                fprintf(stderr, "cat: missing path\n");
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[1], path, sizeof(path));
            Fat12Node node;
            if (fat12_stat(&fs, path, &node) != 0 || node.is_dir) {
                fprintf(stderr, "cat: file not found: %s\n", path);
                continue;
            }
            if (node.size > 0) {
                uint8_t *buf = (uint8_t *)malloc(node.size);
                if (!buf) {
                    fprintf(stderr, "cat: out of memory\n");
                    continue;
                }
                ssize_t n = fat12_read(&fs, path, buf, node.size, 0);
                if (n > 0) {
                    fwrite(buf, 1, (size_t)n, stdout);
                }
                printf("\n");
                free(buf);
            } else {
                printf("\n");
            }
            continue;
        }
        if (strcmp(args[0], "touch") == 0) {
            if (ac < 2) {
                fprintf(stderr, "touch: missing path\n");
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[1], path, sizeof(path));
            if (fat12_create(&fs, path) != 0) {
                fprintf(stderr, "touch: failed to create %s\n", path);
            }
            continue;
        }
        if (strcmp(args[0], "mkdir") == 0) {
            if (ac < 2) {
                fprintf(stderr, "mkdir: missing path\n");
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[1], path, sizeof(path));
            if (fat12_mkdir(&fs, path) != 0) {
                fprintf(stderr, "mkdir: failed to create directory %s\n", path);
            }
            continue;
        }
        if (strcmp(args[0], "rm") == 0) {
            if (ac < 2) {
                fprintf(stderr, "rm: missing path\n");
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[1], path, sizeof(path));
            if (fat12_unlink(&fs, path) != 0) {
                fprintf(stderr, "rm: failed to remove %s\n", path);
            }
            continue;
        }
        if (strcmp(args[0], "rmdir") == 0) {
            if (ac < 2) {
                fprintf(stderr, "rmdir: missing path\n");
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[1], path, sizeof(path));
            if (fat12_rmdir(&fs, path) != 0) {
                fprintf(stderr, "rmdir: failed to remove %s\n", path);
            }
            continue;
        }
        if (strcmp(args[0], "mv") == 0) {
            if (ac < 3) {
                fprintf(stderr, "mv: usage mv <from> <to>\n");
                continue;
            }
            char from[MAX_PATH_LEN], to[MAX_PATH_LEN];
            normalize_path(&sess, args[1], from, sizeof(from));
            normalize_path(&sess, args[2], to, sizeof(to));
            if (fat12_rename(&fs, from, to) != 0) {
                fprintf(stderr, "mv: failed to rename %s to %s\n", from, to);
            }
            continue;
        }
        if (strcmp(args[0], "read") == 0) {
            if (ac < 3) {
                fprintf(stderr, "read: usage read <img> <host>\n");
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[1], path, sizeof(path));
            Fat12Node node;
            if (fat12_stat(&fs, path, &node) != 0 || node.is_dir) {
                fprintf(stderr, "read: file not found: %s\n", path);
                continue;
            }
            uint8_t *buf = NULL;
            if (node.size > 0) {
                buf = (uint8_t *)malloc(node.size);
                if (!buf) {
                    fprintf(stderr, "read: out of memory\n");
                    continue;
                }
                if (fat12_read(&fs, path, buf, node.size, 0) !=
                        (ssize_t)node.size) {
                    fprintf(stderr, "read: failed to read from image\n");
                    free(buf);
                    continue;
                }
            }
            if (write_host_file(args[2], buf, node.size) != 0) {
                fprintf(stderr, "read: failed to write host file %s\n",
                        args[2]);
            }
            free(buf);
            continue;
        }
        if (strcmp(args[0], "write") == 0) {
            if (ac < 3) {
                fprintf(stderr, "write: usage write <host> <img>\n");
                continue;
            }
            uint8_t *data;
            uint32_t len;
            if (read_host_file(args[1], &data, &len) != 0) {
                fprintf(stderr, "write: cannot read host file %s\n", args[1]);
                continue;
            }
            char path[MAX_PATH_LEN];
            normalize_path(&sess, args[2], path, sizeof(path));

            Fat12Node node;
            if (fat12_stat(&fs, path, &node) != 0) {
                if (fat12_create(&fs, path) != 0) {
                    fprintf(stderr, "write: cannot create file %s\n", path);
                    free(data);
                    continue;
                }
            } else if (node.is_dir) {
                fprintf(stderr, "write: target %s is a directory\n", path);
                free(data);
                continue;
            }

            if (fat12_write(&fs, path, data, len, 0) != (ssize_t)len) {
                fprintf(stderr, "write: failed to write to image %s\n", path);
            }
            free(data);
            continue;
        }

        fprintf(stderr, "Unknown command: %s\n", args[0]);
    }

    fat12_close(&fs);
    return 0;
}
