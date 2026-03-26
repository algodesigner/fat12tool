/**
 * Copyright (c) 2026, Vlad Shurupov
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @file fat12_verify.c
 * @brief Simple CLI utility to verify FAT12 image integrity using test utils.
 */

#include "utils.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Main entry point for the verification tool.
 *
 * Checks FAT consistency and directory integrity of a given FAT12 image.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Returns 0 on success, non-zero on failure.
 */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image.img>\n", argv[0]);
        return 2;
    }

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
