# mzdsk-dsk - DSK Container Diagnostics and Editing

Tool version: 1.5.7. Part of the [mzdisk](https://github.com/michalhucik/mzdisk/) project.

> **Disclaimer:** Always work on a copy of your DSK image, never on the
> original. The author provides no warranty and accepts no liability for
> data loss or damage resulting from use of the mzdisk project tools.
> Distributed under the GPLv3 license, without warranty.

Tool for diagnostics, inspection, repair and editing of Extended CPC DSK
disk images at the container level. Works directly with the DSK header
and track headers without any filesystem awareness and without automatic
repair on open (unlike other mzdsk-* tools).

This tool is intended for advanced users who need to analyze or repair
damaged DSK images, edit metadata (creator, GAP#3, FDC statuses) or
verify image consistency.

**Atomic writes:** Write operations (`repair`, `edit-header`, `edit-track`)
are atomic - the entire DSK image is loaded into memory, operations run in
RAM, and the result is flushed back in a single write only after successful
completion. If an operation fails midway, the original file is left
unchanged. Keeping backups for critical data is still recommended -
completed changes are irreversible.

**Protection against invalid input:** The tool validates the DSK
identifier string (`EXTENDED CPC DSK File\r\nDisk-Info\r\n`) on startup.
If the file lacks this magic, every subcommand except `check` aborts
with an error and exit code 1, to prevent accidental modification of
arbitrary files via `edit-header`/`edit-track` or nonsense output from
`info`/`tracks`. The `check` command intentionally skips the magic
check - it reports the problem via the `BAD_FILEINFO` flag in its
diagnostic output.

## Usage

```
mzdsk-dsk [options] <image.dsk> [options] <subcommand> [arguments...]
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<image.dsk>` | DSK disk image file |

## Subcommands

| Subcommand | Description |
|------------|-------------|
| `info` | Show DSK header information |
| `tracks [--abstrack T]` | Show track header details |
| `check` | Diagnose without repair (exit code 0 = OK, 1 = errors) |
| `repair` | Diagnose and repair fixable errors |
| `edit-header --creator TEXT` | Edit DSK header |
| `edit-track T [options]` | Edit track header |

## Options

| Option | Description |
|--------|-------------|
| `--version` | Display program version |
| `--lib-versions` | Display library versions |
| `--help` | Display help |

---

## info - DSK Header Information

Displays the raw contents of the DSK header: file_info identifier,
creator, number of tracks, number of sides, file size and a complete
tsize table for all tracks. If trailing data exists beyond the last
track, its offset and size are shown.

```
mzdsk-dsk <image.dsk> info
```

### Example

Display DSK image information:

```bash
mzdsk-dsk disk.dsk info
```

Sample output:

```
File info : EXTENDED CPC DSK File
Creator   : CPDRead v3.24
Tracks    : 80
Sides     : 2
File size : 696576 bytes

Track size table:
  AbsTrack  tsize(hex)  tsize(bytes)
  --------  ----------  ------------
  0         0x11        4352
  1         0x11        4352
  ...
```

---

## tracks - Track Header Details

Displays detailed information from one or all track headers: track
number, side, sector size, sector count, GAP#3, filler byte and a
list of sectors including FDC status registers.

```
mzdsk-dsk <image.dsk> tracks [--abstrack T]
```

| Option | Description |
|--------|-------------|
| `--abstrack T` | Show only track T (absolute number) |

### Example

Display all tracks:

```bash
mzdsk-dsk disk.dsk tracks
```

Display only track 0:

```bash
mzdsk-dsk disk.dsk tracks --abstrack 0
```

Sample output:

```
Track 0:
  Track number : 0
  Side         : 0
  Sector size  : 256 (0x01)
  Sectors      : 16
  GAP#3        : 0x4e
  Filler       : 0xe5
  Sectors detail:
    Idx   SectID  Track  Side   FDC_STS1  FDC_STS2
    ----  ------  -----  -----  --------  --------
    0     1       0      0      0x00      0x00
    1     9       0      0      0x00      0x00
    ...
```

---

## check - Diagnostics

Performs a complete diagnostics of the DSK image without any
modification. Checks the file_info identifier validity, track count
consistency between header and actual data, tsize field consistency,
track and side numbers in track headers, sector count and size
validity, sector data readability and trailing data presence.

Returns exit code 0 if the image is OK, 1 if errors are found.

```
mzdsk-dsk <image.dsk> check
```

### Error Classification

**Repairable errors** (the `repair` command can fix these):
- `BAD_TRACKCOUNT` - wrong track count in DSK header
- `BAD_TSIZE` - wrong tsize field(s) in DSK header
- `TRAILING_DATA` - data beyond the last track

**Fatal errors** (automatic repair is not possible):
- `BAD_FILEINFO` - invalid file_info identifier
- `BAD_TRACK_NUM` - wrong track number in track header
- `BAD_SIDE_NUM` - wrong side number in track header
- `BAD_SECTORS` - invalid sector count
- `BAD_SSIZE` - invalid encoded sector size
- `DATA_UNREADABLE` - sector data cannot be read

**Informational flags** (not classified as errors - `check` returns 0):
- `ODD_DOUBLE` - 2-sided image has an odd track count (unusual geometry)
- `TRACK_ERRORS` - summary flag indicating presence of per-track errors
  (specific issues are listed in the "Track issues" section)

Per-track flag levels (printed in the "Track issues" section for damaged tracks):
`NO_TRACKINFO` (missing Track-Info identifier), `READ_ERROR` (track data read failure),
`BAD_TRACK_NUM`, `BAD_SIDE_NUM`, `BAD_SECTORS`, `BAD_SSIZE`, `BAD_TSIZE`,
`DATA_UNREADABLE`.

### Example

Diagnose a DSK image:

```bash
mzdsk-dsk disk.dsk check
```

Sample output for a consistent image:

```
Creator        : CPDRead v3.24
Header tracks  : 160 (tracks*sides)
Actual tracks  : 160
Sides          : 2
tsize diffs    : 0
Expected size  : 696576 bytes
Actual size    : 696576 bytes

Image flags    : 0x0000 (OK)

Result: DSK is OK!
```

Sample output with errors:

```
Creator        : CPDRead v3.24
Header tracks  : 134 (tracks*sides)
Actual tracks  : 134
Sides          : 2
tsize diffs    : 0
Expected size  : 651520 bytes
Actual size    : 777984 bytes

Image flags    : 0x0010 TRAILING_DATA

Result: repairable errors found
```

---

## repair - Repair

Performs diagnostics and then repairs all repairable errors:

- **BAD_TRACKCOUNT** - wrong track count in the DSK header
- **BAD_TSIZE** - wrong tsize value for one or more tracks
- **TRAILING_DATA** - data beyond the last track; the file is truncated
  to `expected_image_size` (header + sum of actual tsize values of all
  tracks)

Fatal errors (wrong track/side numbers in track headers, invalid
sectors, file smaller than header) cannot be repaired automatically.

```
mzdsk-dsk <image.dsk> repair
```

Returns exit code 0 if the image is repaired or was already OK,
1 if non-repairable errors remain.

**Cascaded repairs:** some errors are only revealed after another is
fixed (typically `TRAILING_DATA` only surfaces after `BAD_TRACKCOUNT`
is fixed, because `expected_image_size` gets recomputed). A single
`repair` invocation therefore loops diagnose/repair internally until
all repairable errors are gone (capped at 4 passes). You don't need
to run `repair` multiple times - the output reports `Repair completed
successfully (N pass[es])` with the number of iterations taken.

### Example

Repair a DSK image:

```bash
mzdsk-dsk disk.dsk repair
```

---

## edit-header - Edit DSK Header

Allows changing the creator field in the DSK header. The creator is a
14-character string identifying the program that created the image.

```
mzdsk-dsk <image.dsk> edit-header --creator TEXT
```

| Option | Description |
|--------|-------------|
| `--creator TEXT` | New creator string (max 14 characters) |

### Example

Change the creator:

```bash
mzdsk-dsk disk.dsk edit-header --creator "MyTool v1.0"
```

---

## edit-track - Edit Track Header

Allows modifying metadata in a single track header: track number,
side, GAP#3, filler byte or FDC status registers for a specific
sector. Values that are not specified remain unchanged.

```
mzdsk-dsk <image.dsk> edit-track <track> [options]
```

| Argument | Description |
|----------|-------------|
| `<track>` | Absolute track number |

| Option | Description |
|--------|-------------|
| `--track-num N` | Set track number in track header (0-255) |
| `--side N` | Set side (0 or 1) |
| `--gap HH` | Set GAP#3 (hexadecimal, full range 00-FF including FF) |
| `--filler HH` | Set filler byte (hexadecimal, full range 00-FF including FF) |
| `--fdc-status IDX STS1 STS2` | Set FDC status registers for sector IDX |
| `--sector-ids ID,ID,...` | Set all sector IDs at once (comma-separated) |
| `--sector-id IDX:ID` | Set single sector ID at index IDX (repeatable) |

### Example

Change GAP#3 on track 0:

```bash
mzdsk-dsk disk.dsk edit-track 0 --gap 52
```

Fix track number and side:

```bash
mzdsk-dsk disk.dsk edit-track 5 --track-num 2 --side 1
```

Set filler byte:

```bash
mzdsk-dsk disk.dsk edit-track 0 --filler e5
mzdsk-dsk disk.dsk edit-track 0 --filler ff   # typical filler for 5.25" diskettes
```

Set FDC status registers for sector 3 on track 0:

```bash
mzdsk-dsk disk.dsk edit-track 0 --fdc-status 3 40 00
```

Set all sector IDs to reverse order:

```bash
mzdsk-dsk disk.dsk edit-track 0 --sector-ids 9,8,7,6,5,4,3,2,1
```

**Note:** `--sector-ids` requires exactly as many IDs as there are
sectors on the track. If the counts don't match, the tool fails and
prints both the actual sector count and the number of IDs provided:

```
Error: Cannot set sector IDs for track 0: track has 16 sectors, but 3 IDs were provided
```

The sector count of a track can be obtained via
`mzdsk-dsk disk.dsk tracks --abstrack T`.

Change a single sector ID (index 0 to ID 42):

```bash
mzdsk-dsk disk.dsk edit-track 0 --sector-id 0:42
```

---

## Examples

Complete DSK image check:

```bash
mzdsk-dsk disk.dsk check
```

Diagnose and repair:

```bash
mzdsk-dsk disk.dsk repair
```

Inspect DSK header:

```bash
mzdsk-dsk disk.dsk info
```

Inspect a specific track:

```bash
mzdsk-dsk disk.dsk tracks --abstrack 5
```

Change creator and verify:

```bash
mzdsk-dsk disk.dsk edit-header --creator "MZDisk v1.0"
mzdsk-dsk disk.dsk info
```

Display program version:

```bash
mzdsk-dsk --version
```

Display library versions:

```bash
mzdsk-dsk --lib-versions
```
