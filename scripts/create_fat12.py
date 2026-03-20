#!/usr/bin/env python3
# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import sys
import struct
import math

def create_fat12(filename, size_kb=1440, sectors_per_cluster=None):
    """
    Creates a blank FAT12 image of a specified size.
    """
    sector_size = 512
    
    # Calculate total sectors
    total_sectors = (size_kb * 1024 + sector_size - 1) // sector_size
    
    # FAT12 limit: < 4085 clusters
    if sectors_per_cluster is None:
        # Try to find a power of 2 that keeps clusters under 4085
        spc = 1
        while (total_sectors / spc) >= 4085:
            spc *= 2
        sectors_per_cluster = spc
    
    total_clusters = total_sectors // sectors_per_cluster
    if total_clusters >= 4085:
        print(f"Error: FAT12 is limited to < 4085 clusters. At {sectors_per_cluster} sectors per cluster, "
              f"the image is too large ({total_clusters} clusters).")
        sys.exit(1)

    reserved_sectors = 1
    fat_count = 2
    root_entries = 224
    media_byte = 0xF8  # Fixed disk (standard for non-floppy)
    if size_kb <= 1440:
        media_byte = 0xF0 # Floppy

    # Calculate FAT size
    # Each cluster needs 1.5 bytes in FAT12. 
    # Plus 2 reserved entries at the start of FAT (clusters 0 and 1).
    fat_entries = total_clusters + 2
    fat_size_bytes = math.ceil(fat_entries * 1.5)
    sectors_per_fat = (fat_size_bytes + sector_size - 1) // sector_size

    # Re-calculate data area to be precise
    root_dir_sectors = (root_entries * 32 + sector_size - 1) // sector_size
    
    # Final adjustment of total sectors to match the sum of parts if needed, 
    # but usually we just fill up to total_sectors.
    
    # Boot Sector / BPB (512 bytes)
    boot = bytearray(sector_size)
    boot[0:3] = b'\xEB\x3C\x90'  # Jump to boot code
    boot[3:11] = b'FAT12GEN'     # OEM Name

    # BPB fields
    struct.pack_into('<H', boot, 11, sector_size)
    struct.pack_into('<B', boot, 13, sectors_per_cluster)
    struct.pack_into('<H', boot, 14, reserved_sectors)
    struct.pack_into('<B', boot, 16, fat_count)
    struct.pack_into('<H', boot, 17, root_entries)
    
    if total_sectors < 65536:
        struct.pack_into('<H', boot, 19, total_sectors)
        struct.pack_into('<I', boot, 32, 0)
    else:
        struct.pack_into('<H', boot, 19, 0)
        struct.pack_into('<I', boot, 32, total_sectors)
        
    boot[21] = media_byte
    struct.pack_into('<H', boot, 22, sectors_per_fat)
    struct.pack_into('<H', boot, 24, 32)   # Sectors per track (dummy)
    struct.pack_into('<H', boot, 26, 64)   # Number of heads (dummy)
    struct.pack_into('<I', boot, 28, 0)    # Hidden sectors

    # Extended BPB
    struct.pack_into('<B', boot, 36, 0x80) # Drive number
    struct.pack_into('<B', boot, 37, 0)    # Reserved
    struct.pack_into('<B', boot, 38, 0x29) # Extended signature
    struct.pack_into('<I', boot, 39, 0x12345678) # Volume ID
    boot[43:54] = b'NO NAME    '           # Volume Label
    boot[54:62] = b'FAT12   '              # File System Type

    # Boot Signature
    struct.pack_into('<H', boot, 510, 0xAA55)

    with open(filename, 'wb') as f:
        f.write(boot)
        
        # FATs
        fat = bytearray(sectors_per_fat * sector_size)
        fat[0] = media_byte
        fat[1] = 0xFF
        fat[2] = 0xFF
        for _ in range(fat_count):
            f.write(fat)
        
        # Root Directory
        f.write(bytearray(root_dir_sectors * sector_size))
        
        # Data Area
        current_pos = f.tell()
        target_size = total_sectors * sector_size
        if target_size > current_pos:
            f.write(bytearray(target_size - current_pos))

    print(f"Created {filename} ({size_kb}KB, {sectors_per_cluster} sectors/cluster, {total_clusters} clusters)")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: create_fat12.py <filename.img> [size_kb]")
        print("Example: create_fat12.py large.img 10240  # 10MB image")
        sys.exit(1)
    
    fname = sys.argv[1]
    size = 1440
    if len(sys.argv) >= 3:
        size = int(sys.argv[2])
    
    create_fat12(fname, size)
