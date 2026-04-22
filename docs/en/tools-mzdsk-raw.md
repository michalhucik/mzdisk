# mzdsk-raw - Raw Sector Access Tool

Tool version: 2.2.10. Part of the [mzdisk](https://github.com/michalhucik/mzdisk/) project.

> **Disclaimer:** Always work on a copy of your DSK image, never on the
> original. The author provides no warranty and accepts no liability for
> data loss or damage resulting from use of the mzdisk project tools.
> Distributed under the GPLv3 license, without warranty.

Provides low-level access to Sharp MZ DSK disk images at the sector and
block level. Supports export/import to/from files and hexdump output with
configurable addressing modes (Track/Sector and Block), sector ordering
(ID/Phys), data inversion, byte/file offsets.

Also allows modifying disk geometry by changing track layout, appending
tracks, or shrinking the image.

This tool operates directly on the Extended CPC DSK structure without any
filesystem awareness.

**Atomic writes:** Write operations (`put`, `change-track`, `append-tracks`,
`shrink`) are atomic - the entire DSK image is loaded into memory, operations
run in RAM, and the result is flushed back in a single write only after
successful completion. If an operation fails midway, the original file is
left unchanged. Keeping backups for critical data is still recommended -
completed changes are irreversible.

## Usage

```
mzdsk-raw [options] <image.dsk> <subcommand> [arguments...] [subcommand options]
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<image.dsk>` | DSK disk image file |

## Subcommands

| Subcommand | Description |
|------------|-------------|
| `get <file> [options]` | Export sectors/blocks to a file |
| `put <file> [options]` | Import data from a file into sectors/blocks |
| `dump [options]` | Hexdump sectors/blocks to stdout |
| `change-track <T> <SECS> <SSIZE> <FILLER> [ORDER\|MAP]` | Modify track geometry |
| `append-tracks <N> <SECS> <SSIZE> <FILLER> [ORDER\|MAP]` | Add new tracks |
| `shrink <N>` | Remove tracks from the end |

## Global Options

| Option | Description |
|--------|-------------|
| `--inv` | Force data inversion on (XOR 0xFF) |
| `--noinv` | Force data inversion off |
| `--overwrite` | Allow `get` to overwrite an existing output file (default: refuse with error; ignored when `--file-offset > 0` is used for embed mode) |
| `--version` | Display program version |
| `--lib-versions` | Display library versions |
| `-h`, `--help` | Display help |

---

## Addressing Modes

### Track/Sector Mode (default)

Direct addressing by track and sector. Options:

| Option | Default | Description |
|--------|---------|-------------|
| `--track T` | 0 | Start track |
| `--sector S` | 1 | Start sector ID (1-based) |
| `--sectors N` | 1 | Number of sectors |

### Block Mode

Activated by the presence of `--block`. Translates block numbers to
track/sector using the origin, first-block and sectors-per-block
configuration.

| Option | Default | Description |
|--------|---------|-------------|
| `--block B` | - | Start block (activates block mode) |
| `--blocks N` | 1 | Number of blocks |
| `--origin-track T` | 0 | Origin track (where first-block starts) |
| `--origin-sector S` | 1 | Origin sector (1-based) |
| `--first-block N` | 0 | First block number at origin position |
| `--spb N` | 1 | Sectors per block |

**Note:** `--block` and `--track`/`--sector`/`--sectors` are mutually exclusive.

---

## Sector Ordering

The `--order id|phys` option controls how sectors are traversed:

- **id** (default) - sequential by sector ID (1, 2, 3, ...). At the end
  of a track, moves to the next track starting from ID 1. Emulates floppy
  disk controller behavior. Sectors with non-sequential IDs (e.g. LEMMINGS
  sector 22 on a track with IDs 1-9) are skipped.

- **phys** - by physical position in the DSK image (sinfo[] array in the
  track header). Captures all sectors including non-sequential ones.

On standard Sharp MZ disks (sequential IDs 1..N) both modes behave
identically. The difference appears on disks with non-standard sector
numbering.

---

## Data Options

| Option | Default | Description |
|--------|---------|-------------|
| `--byte-offset N` | 0 | Offset in first sector/block (bytes) |
| `--byte-count N` | 0 (all) | Total number of bytes to transfer |
| `--file-offset N` | 0 | Offset in file (bytes) - see note below |
| `--dump-charset MODE` | raw | Character set for hexdump ASCII column (dump only) |
| `--cnv` | - | Alias for `--dump-charset eu` |

**--file-offset N** - offset in the file (bytes) from which data is read
(for `put`) or at which writing begins (for `get`).

- For `put`: if `--file-offset` is at or past the end of the input file,
  the operation aborts with an error (exit != 0) and the sector on disk
  is not modified. If EOF is hit partway through, the rest of the target
  range is padded with zeros and a warning is printed.
- For `get`: if `--file-offset` is past the end of the output file, the
  gap between the end of file and the offset is filled with zeros (the
  file is extended).

**--dump-charset MODE** - selects the Sharp MZ character set conversion for
the hexdump ASCII column.

Sharp MZ computers use their own character set (Sharp MZ ASCII), which exists
in two variants: European (EU) and Japanese (JP). It is compatible with
standard ASCII only in the range 0x20-0x5D (uppercase letters, digits, and
basic punctuation). Characters above 0x5D (lowercase letters, special symbols)
are placed at different positions than in standard ASCII.

The `eu` and `jp` modes perform single-byte conversion to standard ASCII.
The Japanese variant is more limited than the European one - it lacks lowercase
letters and some other characters, so unconvertible characters are replaced
with '.'.

The `utf8-eu` and `utf8-jp` modes convert to UTF-8 and attempt to display
as many characters as possible, including graphic symbols and special
characters that have no equivalent in single-byte ASCII conversion.

Available modes:
- `raw` (default) - standard ASCII (0x20-0x7E), others shown as '.'
- `eu` - Sharp MZ EU -> ASCII (single-byte conversion, European character set)
- `jp` - Sharp MZ JP -> ASCII (single-byte conversion, Japanese character set)
- `utf8-eu` - Sharp MZ EU -> UTF-8 (European variant with graphic symbols)
- `utf8-jp` - Sharp MZ JP -> UTF-8 (Japanese variant with katakana and symbols)

**--cnv** - Alias for `--dump-charset eu`.

---

## get - Export Sectors/Blocks

Reads data from the disk and saves it to a file.

```
mzdsk-raw [global options] <image.dsk> get <file> [options]
```

### Overwrite Protection

If the output file already exists, `get` fails with an error by default:

```
Error: Output file 'data.bin' already exists. Use --overwrite to replace it.
```

Use the global `--overwrite` flag to allow replacing an existing file
(consistent contract with `mzdsk-fsmz`, `mzdsk-cpm`, `mzdsk-mrs`).
Exception: when `--file-offset N` with `N > 0` is used, `get` runs in
embed mode (writing into an existing binary file at a specific offset),
so pre-existence is intentional and `--overwrite` is not required.

### Examples

Export 16 sectors from track 0:

```bash
mzdsk-raw disk.dsk get data.bin --track 0 --sector 1 --sectors 16
```

Export one CP/M block (2 sectors from track 4):

```bash
mzdsk-raw disk.dsk get block0.bin --block 0 --origin-track 4 --origin-sector 1 --spb 2
```

Export with forced inversion:

```bash
mzdsk-raw --inv disk.dsk get sector.bin --track 1 --sector 1
```

Export with byte offset (skip first 128 bytes):

```bash
mzdsk-raw disk.dsk get data.bin --track 0 --sector 1 --byte-offset 128
```

Export to file at offset 1024 (embed mode - existing file OK):

```bash
mzdsk-raw disk.dsk get output.bin --track 0 --sector 1 --sectors 4 --file-offset 1024
```

Replace an existing output file:

```bash
mzdsk-raw --overwrite disk.dsk get data.bin --track 0 --sector 1
```

---

## put - Import Data to Disk

Writes data from a file into sectors on the disk. For partial writes
(byte-offset or byte-count less than full sector), the existing sector
is read first, relevant bytes are overwritten, and the sector is written
back.

```
mzdsk-raw <image.dsk> put <file> [options]
```

### Examples

Import file into 16 sectors from track 0:

```bash
mzdsk-raw disk.dsk put data.bin --track 0 --sector 1 --sectors 16
```

Import into a CP/M block:

```bash
mzdsk-raw disk.dsk put block0.bin --block 0 --origin-track 4 --origin-sector 1 --spb 2
```

Import with forced inversion:

```bash
mzdsk-raw --inv disk.dsk put data.bin --track 1 --sector 1
```

---

## dump - Hexdump to stdout

Displays a formatted hexdump of sectors/blocks on standard output.
Each sector is preceded by a header showing track number, sector ID,
size and inversion status.

```
mzdsk-raw <image.dsk> dump [options]
```

### Examples

Hexdump two sectors from track 0:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 2
```

Hexdump with inversion and Sharp MZ ASCII conversion:

```bash
mzdsk-raw --inv disk.dsk dump --track 0 --sector 1 --sectors 16 --cnv
```

Hexdump in block mode:

```bash
mzdsk-raw disk.dsk dump --block 0 --blocks 2 --origin-track 4 --spb 2
```

Hexdump with physical sector ordering:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 10 --order phys
```

---

## change-track - Modify Track Geometry

Changes the geometry of an existing track in the DSK image. If the new
track size differs from the original, subsequent tracks are shifted.

```
mzdsk-raw <image.dsk> change-track <track> <sectors> <sector_size> <filler> [order|map]
```

| Argument | Description |
|----------|-------------|
| `<track>` | Absolute track number to modify |
| `<sectors>` | New number of sectors on the track |
| `<sector_size>` | Sector size in bytes (128, 256, 512, or 1024) |
| `<filler>` | Filler byte for new sectors (e.g. 0xE5, 0xFF, 0x00) |
| `[order\|map]` | Optional sector order (see below) |

### Sector order

- `normal` - sequential numbering (1, 2, 3, ...)
- `lec` - interleaved order for standard-density CP/M disks
- `lechd` - interleaved order for high-density CP/M disks
- comma-separated IDs - custom sector ID map (e.g. `1,3,5,2,4,6`)

### Examples

```bash
mzdsk-raw disk.dsk change-track 5 9 512 0xE5
mzdsk-raw disk.dsk change-track 0 16 256 0xFF
mzdsk-raw disk.dsk change-track 2 9 512 0xE5 lec
mzdsk-raw disk.dsk change-track 3 6 512 0xE5 1,3,5,2,4,6
```

---

## append-tracks - Add Tracks

Appends new tracks to the end of the DSK image.

```
mzdsk-raw <image.dsk> append-tracks <count> <sectors> <sector_size> <filler> [order|map]
```

| Argument | Description |
|----------|-------------|
| `<count>` | Number of tracks to append |
| `<sectors>` | Number of sectors per new track |
| `<sector_size>` | Sector size in bytes (128, 256, 512, or 1024) |
| `<filler>` | Filler byte for new sectors |
| `[order\|map]` | Optional sector order |

### Examples

```bash
mzdsk-raw disk.dsk append-tracks 10 16 256 0xFF
mzdsk-raw disk.dsk append-tracks 40 9 512 0xE5 lec
```

---

## shrink - Remove Tracks

Removes tracks from the end of the DSK image.

```
mzdsk-raw <image.dsk> shrink <total_tracks>
```

### Examples

```bash
mzdsk-raw disk.dsk shrink 40
```

---

## Examples

Backup and restore a sector:

```bash
mzdsk-raw disk.dsk get backup.bin --track 5 --sector 3
mzdsk-raw disk.dsk put backup.bin --track 5 --sector 3
```

Roundtrip test:

```bash
mzdsk-raw disk.dsk get orig.bin --track 0 --sector 1 --sectors 16
mzdsk-raw disk.dsk put orig.bin --track 0 --sector 1 --sectors 16
```

Export entire track in physical order:

```bash
mzdsk-raw disk.dsk get track0.bin --track 0 --sector 1 --sectors 16 --order phys
```

Display version:

```bash
mzdsk-raw --version
mzdsk-raw --lib-versions
```
