# FAT12 Toolkit (CLI + Filesystem Mount)

This project provides two ways to work with FAT12 images:

- `fat12tool`: interactive command shell for direct FAT12 read/write operations.
- `fat12mount`: filesystem driver that mounts a FAT12 image to a regular directory so any application can use it.

The code is split into:

- `fat12_core.c/.h`: portable FAT12 engine (allocation, directory operations, file IO).
- `fat12tool.c`: interactive shell frontend.
- `fat12mount.c`, `vfs_ops.h`, `vfs_fuse.c`, `vfs_winfsp.c`: cross-platform mounting via FUSE (Linux/macOS) or WinFSP (Windows).

## Features

- Read/write FAT12 files and directories
- Create/remove files and directories
- Truncate files
- Persist metadata updates (write/access timestamps)
- Preserve file attributes (timestamps, read-only, hidden) on host-image roundtrips
- Open FAT12 at offset `0` (partition image) or at MBR partition offset (`--partition N`)
- Cross-platform: works on Linux, macOS, and Windows

## Current Limitations

- FAT12 only (not FAT16/FAT32/exFAT)
- 8.3 short filenames only (no long filename/LFN support)
- No journaling/crash recovery
- Root directory expansion is limited by FAT12 root entry table constraints
- `rename` in mounting mode is implemented as copy+unlink for files, and directories are currently not supported for rename

## Build

### Linux

```sh
make
```

Install FUSE dependencies if needed:

```sh
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y libfuse3-dev fuse3 pkg-config

# Fedora
sudo dnf install -y fuse3-devel fuse3 pkgconf-pkg-config
```

### macOS

```sh
make
```

Install FUSE-T:

1. Install FUSE-T (Homebrew cask `macos-fuse-t/homebrew-cask/fuse-t`).
2. Ensure FUSE headers are available (`/usr/local/include/fuse/fuse.h`) and `libfuse-t` is present (`/usr/local/lib/libfuse-t.dylib`).

### Windows (MinGW + WinFSP)

1. **Install WinFSP**: Download the installer from the [official website](https://winfsp.dev/). 
   * **Important**: During installation, ensure you tick the **"Developer"** section (it is unticked by default). This is absolutely essential as it contains the header files required for the build.
2. **Configure PATH**: The WinFSP installer does not automatically add its binary directory to your system `PATH`. You must manually add `C:\Program Files (x86)\WinFSP\bin` (or your custom install path) to your environment variables. This is essential for the application to load `winfsp-x64.dll` at runtime.
3. Install make via [Scoop](https://scoop.sh/):
```sh
scoop install make
```
4. Install MinGW-w64 via Scoop:
```sh
scoop install mingw
```
5. Build:
```sh
make fat12mount.exe
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

- `ls [path]` (displays entries with size, date and time)
- `cd <path>`
- `pwd`
- `cat <path>`
- `read <img_path> <host_path>` (preserves timestamps and read-only status)
- `write <host_path> <img_path>` (preserves timestamps, read-only status, and hidden status for dot-files)
- `touch <path>`
- `mkdir <path>`
- `rm <path>`
- `rmdir <path>`
- `stat <path>` (displays detailed metadata including attribute bits)
- `help`
- `exit`

Example session:

```text
fat12:/> ls
README.TXT                   62 2026-03-09 00:02:12
HELLO.TXT                    14 2026-03-09 00:02:12
TESTDIR        <DIR>          0 2026-03-09 00:02:12
fat12:/> cat /README.TXT
fat12:/> write ./local.txt /LOCAL.TXT
fat12:/> mkdir /DOCS
fat12:/> read /LOCAL.TXT ./copy.txt
fat12:/> exit
```

## Using `fat12mount`

The mount point directory is created automatically (Windows) or must be created first (Linux/macOS). On all platforms, the directory is automatically removed after unmount.

### Mount a partition image directly

```sh
# Windows: directory is created automatically
# Linux/macOS: create directory first
mkdir -p ./mnt

./fat12mount --image sample-fat12-p1.img --mount ./mnt
```

In another shell:

```sh
ls -la ./mnt
cat ./mnt/README.TXT
echo "hello" > ./mnt/NEW.TXT
```

### Mount a partition inside an MBR disk image

```sh
mkdir -p ./mnt
./fat12mount --image sample-fat12-2part.img --partition 1 --mount ./mnt
```

Use `--partition 2` for the second partition.

### Unmount

```sh
./fat12mount -u ./mnt
```

## Test Suite

Run full tests:

```sh
make test
```

### `test-core` (C API tests)

Binary: `fat12_core_test`  
Source: `tests/test_fat12_core.c`

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

Script: `tests/test_cli.sh`

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

### `fat12mount` not built on Linux/macOS

If `make` prints:

```text
Skipping fat12mount (FUSE not available in build environment).
```

Install FUSE development headers/libraries and rerun `make`.

### Cannot mount on macOS

- Confirm FUSE-T is installed (Homebrew cask `macos-fuse-t/homebrew-cask/fuse-t`).
- If prompted, allow access to Network Volumes in Privacy & Security.
- If `fat12mount` exits immediately with SIGTRAP, double-check that your
  terminal app has Network Volumes permission.
- Ensure the mountpoint directory exists and is writable.

### Cannot mount on Windows

- **WinFSP Installation**: Confirm WinFSP is installed. Download from [https://winfsp.dev/](https://winfsp.dev/).
- **Missing Headers**: Ensure you ticked the **"Developer"** section during WinFSP installation. Without it, the build will fail as headers like `winfsp/winfsp.h` won't be found.
- **Missing DLL**: If the application fails to start with a "DLL not found" error, ensure `C:\Program Files (x86)\WinFSP\bin` is added to your system `PATH`. This is essential for loading `winfsp-x64.dll`.
- **WinFSP Launcher**: Confirm the "Launcher" component was included during installation. Run the installer as Administrator to ensure the WinFsp.Launcher service is registered.
- If you installed WinFSP without Administrator privileges, you can manually register the service:
  ```cmd
  sc create WinFsp.Launcher binPath= "C:\Program Files (x86)\WinFSP\bin\launcher-x64.exe" DisplayName= "WinFsp Launcher" start= demand
  sc start WinFsp.Launcher
  ```
- If unmount fails, check that no other applications are using the mounted volume.

### File names rejected

Current implementation enforces FAT 8.3 names. Use names up to `8.3` format (`NAME.TXT`, `DATA.BIN`, etc.).

## Development Notes

The FAT12 core is written to be embeddable and used by all frontends. If you add features (LFN, FAT16/32, better rename semantics), implement in `fat12_core` first, then expose via `fat12tool` and `fat12mount`.

The cross-platform mounting layer uses an abstraction via `vfs_ops.h`:
- `vfs_fuse.c`: FUSE backend for Linux/macOS
- `vfs_winfsp.c`: WinFSP backend for Windows

## License

This project is licensed under the 3-Clause BSD License. See the [LICENSE](LICENSE) file for details.
Copyright (c) 2026, Vlad Shurupov.
