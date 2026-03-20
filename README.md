# FAT12 Toolkit (CLI + FUSE Mount)

This project provides two ways to work with FAT12 images:

- `fat12tool`: interactive command shell for direct FAT12 read/write operations.
- `fat12mount`: FUSE filesystem driver that mounts a FAT12 image to a regular directory so any app can use it.

The code is split into:

- `fat12_core.c/.h`: portable FAT12 engine (allocation, directory operations, file IO).
- `fat12tool.c`: interactive shell frontend.
- `fat12_fuse.c`: FUSE frontend for Linux/macOS.

## Features

- Read/write FAT12 files and directories
- Create/remove files and directories
- Truncate files
- Persist metadata updates (write/access timestamps)
- Open FAT12 at offset `0` (partition image) or at MBR partition offset (`--partition N` in FUSE tool)

## Current Limitations

- FAT12 only (not FAT16/FAT32/exFAT)
- 8.3 short filenames only (no long filename/LFN support)
- No journaling/crash recovery
- Root directory expansion is limited by FAT12 root entry table constraints
- `rename` in FUSE is implemented as copy+unlink for files, and directories are currently not supported for rename

## Repository Layout

- `/Users/vlad/Documents/Playground/fat12_core.h`
- `/Users/vlad/Documents/Playground/fat12_core.c`
- `/Users/vlad/Documents/Playground/fat12tool.c`
- `/Users/vlad/Documents/Playground/fat12_fuse.c`
- `/Users/vlad/Documents/Playground/tests/test_fat12_core.c`
- `/Users/vlad/Documents/Playground/tests/test_cli.sh`
- `/Users/vlad/Documents/Playground/sample-fat12-p1.img`
- `/Users/vlad/Documents/Playground/sample-fat12-p2.img`
- `/Users/vlad/Documents/Playground/sample-fat12-2part.img`

## Build

From `/Users/vlad/Documents/Playground`:

```sh
make
```

What `make` does:

- Always builds `fat12tool`
- Builds `fat12mount` only when FUSE headers/libraries are available

### FUSE dependencies

Install prerequisites via script:

```sh
./scripts/install_deps.sh
```

Include Doxygen as well:

```sh
./scripts/install_deps.sh --with-docs
```

Linux (Ubuntu/Debian):

```sh
sudo apt-get update
sudo apt-get install -y libfuse3-dev fuse3 pkg-config
```

Linux (Fedora):

```sh
sudo dnf install -y fuse3-devel fuse3 pkgconf-pkg-config
```

macOS:

1. Install FUSE-T (Homebrew cask `macos-fuse-t/homebrew-cask/fuse-t`).
2. Ensure FUSE headers are available (`/usr/local/include/fuse/fuse.h`) and `libfuse-t` is present (`/usr/local/lib/libfuse-t.dylib`).

Then rebuild:

```sh
make clean
make
```

## Creating a New Image

To create a new blank FAT12 image, use the provided Python script (defaults to 1.44MB):

```sh
# Create standard 1.44MB image
python3 scripts/create_fat12.py floppy.img

# Create a 10MB image
python3 scripts/create_fat12.py large.img 10240
```

Note: FAT12 is technically limited to < 4085 clusters. The script automatically adjusts sectors per cluster to accommodate larger sizes (up to ~128MB with 64 sectors per cluster).

Alternatively, you can use system tools:

- **macOS**: `hdiutil create -size 1.44m -fs "MS-DOS FAT12" -volname "MYFAT" myfat.img`
- **Linux**: `mkfs.fat -F 12 -C myfat.img 1440` (requires `dosfstools`)

## Using `fat12tool` (Interactive Shell)

```sh
./fat12tool sample-fat12-p1.img
```

Commands:

- `ls [path]`
- `cd <path>`
- `pwd`
- `cat <path>`
- `read <img_path> <host_path>`
- `write <host_path> <img_path>`
- `touch <path>`
- `mkdir <path>`
- `rm <path>`
- `rmdir <path>`
- `stat <path>`
- `help`
- `exit`

Example session:

```text
fat12:/> ls
fat12:/> cat /README.TXT
fat12:/> write ./local.txt /LOCAL.TXT
fat12:/> mkdir /DOCS
fat12:/> read /LOCAL.TXT ./copy.txt
fat12:/> exit
```

## Using `fat12mount` (FUSE)

### 1) Mount a partition image directly

```sh
mkdir -p ./mnt
./fat12mount --image sample-fat12-p1.img --mount ./mnt -f
```

In another shell:

```sh
ls -la ./mnt
cat ./mnt/README.TXT
echo "hello" > ./mnt/NEW.TXT
```

Unmount:

Linux:

```sh
fusermount3 -u ./mnt
```

macOS:

```sh
umount ./mnt
```

### 2) Mount a partition inside an MBR disk image

```sh
mkdir -p ./mnt
./fat12mount --image sample-fat12-2part.img --partition 1 --mount ./mnt -f
```

Use `--partition 2` for the second partition.

## Test Suite

Run full tests:

```sh
make test
```

The suite includes a FUSE integration test (`tests/test_fuse_mount.sh`) that
mounts a sample image and verifies reads/writes. It skips automatically if
FUSE is unavailable.

### `test-core` (C API tests)

Binary: `fat12_core_test`
Source: `/Users/vlad/Documents/Playground/tests/test_fat12_core.c`

Coverage includes:

- Open and stat root
- List root directory and validate fixture entries
- Read existing file contents
- Create new file
- Sparse write behaviour (`offset > file_size` zero-fills gap)
- Truncate shrink behaviour
- Create/remove nested directories and files
- Proper error on `rmdir` non-empty directory
- Timestamp update call
- Persistence across close/reopen
- Open FAT12 partition from MBR image offset and read expected content

### `test-cli` (shell smoke test)

Script: `/Users/vlad/Documents/Playground/tests/test_cli.sh`

Coverage includes:

- Launch `fat12tool`
- `pwd`, `ls`, `cat`
- `touch`, `write`, `read`
- `mkdir`, nested file create/remove, `rmdir`
- `stat`
- Host roundtrip verification for written/read file content

## Sample Images

Provided fixtures:

- `sample-fat12-p1.img`: standalone FAT12 partition image
- `sample-fat12-p2.img`: standalone FAT12 partition image
- `sample-fat12-2part.img`: MBR disk image with two FAT12 partitions

These include sample files for immediate testing.

## Troubleshooting

### `fat12mount` not built

If `make` prints:

```text
Skipping fat12mount (FUSE not available in build environment).
```

Install FUSE development headers/libraries for your platform and rerun `make`.

### Cannot mount on macOS

- Confirm FUSE-T is installed (Homebrew cask `macos-fuse-t/homebrew-cask/fuse-t`).
- If prompted, allow access to Network Volumes in Privacy & Security.
- If `fat12mount` exits immediately with SIGTRAP, double-check that your
  terminal app has Network Volumes permission.
- Ensure the mountpoint directory exists and is writable.

### File names rejected

Current implementation enforces FAT 8.3 names. Use names up to `8.3` format (`NAME.TXT`, `DATA.BIN`, etc.).

## Development Notes

The FAT12 core is written to be embeddable and used by both frontends. If you add features (LFN, FAT16/32, better rename semantics), implement in `fat12_core` first, then expose via `fat12tool` and `fat12mount`.

## License

This project is licensed under the 3-Clause BSD License. See the [LICENSE](LICENSE) file for details.
Copyright (c) 2026, Vlad Shurupov.
