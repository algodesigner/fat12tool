/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file fat12_verify.c
 * @brief CLI utility to verify and corrupt FAT12 images for testing.
 */

#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <image.img>               Verify image integrity\n", prog);
    fprintf(stderr, "  %s corrupt-crosslink <img> <c1> <c2>   Create cross-link\n", prog);
    fprintf(stderr, "  %s corrupt-orphan <img> <cluster>      Create orphaned cluster\n", prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (argc == 2) {
        const char *img = argv[1];
        int failed = 0;

        printf("Verifying FAT consistency for %s...\n", img);
        if (test_verify_fat_consistency(img) != 0) {
            fprintf(stderr, "ERROR: FAT consistency check failed (replicas differ)\n");
            failed = 1;
        } else {
            printf("OK: FAT replicas are consistent.\n");
        }

        printf("Verifying directory integrity for %s...\n", img);
        if (test_verify_directory_integrity(img) != 0) {
            fprintf(stderr, "ERROR: Directory integrity check failed\n");
            failed = 1;
        } else {
            printf("OK: Directory structure is listable.\n");
        }

        return failed;
    }

    if (strcmp(argv[1], "corrupt-crosslink") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s corrupt-crosslink <image> <cluster1> <cluster2>\n", argv[0]);
            return 2;
        }
        return test_corrupt_fat_crosslink(argv[2], (uint16_t)atoi(argv[3]), (uint16_t)atoi(argv[4]));
    }

    if (strcmp(argv[1], "corrupt-orphan") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s corrupt-orphan <image> <cluster>\n", argv[0]);
            return 2;
        }
        return test_corrupt_orphan(argv[2], (uint16_t)atoi(argv[3]));
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    usage(argv[0]);
    return 2;
}
