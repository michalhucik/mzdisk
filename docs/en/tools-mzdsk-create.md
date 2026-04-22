# mzdsk-create - Create DSK Disk Image

Tool version: 1.2.5. Part of the [mzdisk](https://github.com/michalhucik/mzdisk/) project.

> **Disclaimer:** Always work on a copy of your DSK image, never on the
> original. The author provides no warranty and accepts no liability for
> data loss or damage resulting from use of the mzdisk project tools.
> Distributed under the GPLv3 license, without warranty.

Creates a new empty DSK disk image in Extended CPC DSK format for
Sharp MZ computers. Supports predefined presets for the most common
disk formats (MZ-BASIC, CP/M SD, CP/M HD, MRS, Lemmings), parametric
filesystem formatting with a specified track count, and custom
geometry with all parameters.

## Usage

```
mzdsk-create <image.dsk> --preset <name> [--sides N] [--overwrite]
mzdsk-create <image.dsk> --format-basic <tracks> [--sides N] [--overwrite]
mzdsk-create <image.dsk> --format-cpm <tracks> [--sides N] [--overwrite]
mzdsk-create <image.dsk> --format-cpmhd <tracks> [--sides N] [--overwrite]
mzdsk-create <image.dsk> --format-mrs <tracks> [--sides N] [--overwrite]
mzdsk-create <image.dsk> --custom <tracks> <sectors> <ssize> <filler> [order] [--sides N] [--overwrite]
mzdsk-create --version
mzdsk-create --lib-versions
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<image.dsk>` | Path to the output DSK file |

## Commands

| Command | Description |
|---------|-------------|
| `--preset <name>` | Create DSK from a predefined preset |
| `--format-basic <T>` | Create and format an MZ-BASIC disk with T tracks per side |
| `--format-cpm <T>` | Create and format a CP/M SD disk with T tracks per side |
| `--format-cpmhd <T>` | Create and format a CP/M HD disk with T tracks per side |
| `--format-mrs <T>` | Create and format an MRS disk with T tracks per side |
| `--custom <T> <S> <SS> <F> [O]` | Create DSK with custom geometry |

## Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--overwrite` | - | - | Overwrite existing output file |
| `--sides` | 1, 2 | 2 | Number of sides (with all commands) |
| `--version` | - | - | Show program version |
| `--lib-versions` | - | - | Show library versions |
| `--help` | - | - | Show help and exit |

### Preset details

The `--preset <name>` option creates a DSK image according to one
of these fixed formats:

| Preset | Tracks/side | Geometry | Notes |
|--------|-------------|----------|-------|
| `basic` | 80 | all tracks 16×256 B, normal order, filler 0xFF | Sharp MZ-800 Disc BASIC |
| `cpm-sd` | 80 | track 1 (boot) 16×256 B; others 9×512 B, LEC order, filler 0xE5 | LEC CP/M 2.2 SD |
| `cpm-hd` | 80 | track 1 (boot) 16×256 B; others 18×512 B, LEC HD order, filler 0xE5 | LEC CP/M 2.2 HD |
| `mrs` | 80 | track 1 (boot) 16×256 B; others 9×512 B, LEC order, filler 0xE5 | Vlastimil Veselý's MRS |
| `lemmings` | 80 | track 1 (boot) 16×256 B; others 9×512 B, LEC order, filler 0xE5 | Lemmings game special format |

All presets default to 2 sides (160 absolute tracks). Use `--sides 1`
to create a single-sided disk (80 absolute tracks).

Presets only create the DSK structure (geometry and filler byte fill)
- they do NOT create filesystem content (FAT, directory, boot code).
For a complete create-with-system use the `--format-*` commands.

### `--format-*` details

The `--format-basic`, `--format-cpm`, `--format-cpmhd` and `--format-mrs`
commands create a DSK image with the specified number of tracks per side
AND initialize the filesystem. Number of sides is selected with `--sides
N` (default 2).

**The argument `<T>` means tracks per side.** The total absolute track
count on the disk is `T × sides`. This matches `--preset` semantics
where each preset uses T=80 tracks per side. Examples:

- `--format-basic 80 --sides 2` → 80×2 = 160 absolute tracks
  (same as `--preset basic --sides 2`).
- `--format-basic 80 --sides 1` → 80 absolute tracks
  (same as `--preset basic --sides 1`).

Historical note: mzdsk-create < 1.2.0 interpreted `<T>` as total tracks
for `--format-*`, which was inconsistent with `--preset` and `--sides`
had no effect on file size. Since 1.2.0 the semantics are unified.

- **`--format-basic <T>`** - Creates an MZ-BASIC disk with T tracks per side
  (16×256 B on all tracks) and initializes the FSMZ filesystem:
  IPLPRO block, DINFO block, empty directory.

- **`--format-cpm <T>`** - Creates a LEC CP/M SD disk with T tracks per side
  (9×512 B with boot track) and initializes an empty CP/M directory.

- **`--format-cpmhd <T>`** - Creates a LEC CP/M HD disk with T tracks per side
  (18×512 B with boot track) and initializes an empty CP/M directory.

- **`--format-mrs <T>`** - Creates an MRS disk with T tracks per side
  (9×512 B with boot track) and initializes the MRS filesystem:
  FAT table and empty directory.

### `--custom` details

The `--custom` command takes 4 required and 1 optional positional
argument:

```
--custom <T> <S> <SS> <F> [O]
```

| Argument | Range | Description |
|----------|-------|-------------|
| `T` | 1-204 | Total number of absolute tracks (unlike `--format-*`, here T is total, not per-side) |
| `S` | 1-29 | Sectors per track |
| `SS` | 128, 256, 512, 1024 | Sector size in bytes |
| `F` | 0-255 | Filler byte (sector fill) |
| `O` (optional) | see below | Sector order |

**Sector order (`O`):**

| Value | Description |
|-------|-------------|
| `normal` | Sequential 1, 2, 3, ... (default) |
| `lec` | Interlaced with interval 2 (LEC SD format) |
| `lechd` | Interlaced with interval 3 (LEC HD format) |
| `1,3,5,2,4,6` | Custom sector map (comma-separated IDs) |

The number of sides is selected with the `--sides N` option (1 or 2,
default 2). When `--sides 2` is used, the total track count `T` must
be even.

`--custom` does not create a filesystem, only the DSK structure. The
resulting image is a "clean" disk with the requested geometry filled
with the filler byte.

### `--overwrite` details

Without this option the program refuses to overwrite an existing
file and exits with an error. With `--overwrite` the existing file
is replaced.

## Examples

Create an MZ-BASIC disk from preset:

```bash
mzdsk-create basic.dsk --preset basic
```

Create a CP/M SD disk from preset:

```bash
mzdsk-create cpm.dsk --preset cpm-sd
```

Create an MRS disk from preset:

```bash
mzdsk-create mrs.dsk --preset mrs
```

Create a formatted MZ-BASIC disk with 40 tracks:

```bash
mzdsk-create basic40.dsk --format-basic 40
```

Create a formatted CP/M HD disk with 80 tracks:

```bash
mzdsk-create cpmhd.dsk --format-cpmhd 80
```

Create a custom single-sided disk (40 tracks, 16×256 B, filler 0xE5,
normal order):

```bash
mzdsk-create custom.dsk --custom 40 16 256 0xe5 normal --sides 1
```

Create a custom double-sided disk with LEC HD order:

```bash
mzdsk-create hd.dsk --custom 80 18 512 0xe5 lechd --sides 2
```

Create a disk with a custom sector map:

```bash
mzdsk-create custom.dsk --custom 80 9 512 0xe5 1,3,5,7,9,2,4,6,8
```

Overwrite an existing file:

```bash
mzdsk-create basic.dsk --preset basic --overwrite
```

Create an MRS disk and then initialize the filesystem with mzdsk-mrs:

```bash
mzdsk-create mrs.dsk --preset mrs
mzdsk-mrs mrs.dsk init
mzdsk-mrs mrs.dsk info
```
