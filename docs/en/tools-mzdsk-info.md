# mzdsk-info - DSK Disk Image Inspector

Tool version: 1.4.6. Part of the [mzdisk](https://github.com/michalhucik/mzdisk/) project.

> **Disclaimer:** Always work on a copy of your DSK image, never on the
> original. The author provides no warranty and accepts no liability for
> data loss or damage resulting from use of the mzdisk project tools.
> Distributed under the GPLv3 license, without warranty.

Displays detailed information about Sharp MZ DSK disk images in the Extended
CPC DSK format. The tool is read-only and does not modify the image. It
automatically identifies the disk format (MZ-BASIC, CP/M SD, CP/M HD, MRS,
boot-only) and shows geometry, track layout, sector map with filesystem
auto-detection (FSMZ, CP/M SD/HD/SD2S/HD2S, MRS), boot sector contents,
and raw sector or FSMZ block data.

Without any subcommand, displays basic image information including format
identification, geometry (tracks, sides, sectors, sector size), and track
layout rules.

## Usage

```
mzdsk-info <image.dsk> [options]
mzdsk-info --version
mzdsk-info --lib-versions
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<image.dsk>` | Input DSK disk image file (Extended CPC DSK format) |

## Commands

| Command | Description |
|---------|-------------|
| *(none)* | Show disk geometry, format and FSMZ info |
| `--map` | Show disk usage map with filesystem auto-detection (FSMZ, CP/M SD/HD/SD2S/HD2S, MRS) |
| `--boot` | Show bootstrap (IPLPRO) info |
| `--sector T S` | Hexdump of sector at track T, sector ID S (see note below) |
| `--block N` | Hexdump of FSMZ block N |

## Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--output FMT` | text\|json\|csv | text | Output format (text, json, csv) |
| `-o FMT` | text\|json\|csv | text | Short for --output |
| `--charset MODE` | eu\|jp\|utf8-eu\|utf8-jp | eu | Sharp MZ character set conversion for filenames |
| `--dump-charset MODE` | raw\|eu\|jp\|utf8-eu\|utf8-jp | raw | Character set for hexdump ASCII column |
| `--cnv` | - | - | Alias for `--dump-charset eu` |
| `--nocnv` | - | - | Force disable Sharp ASCII conversion |
| `--inv` | - | - | Force enable data inversion |
| `--noinv` | - | - | Force disable data inversion |
| `--version` | - | - | Display program version |
| `--lib-versions` | - | - | Display library versions |
| `--help`, `-h` | - | - | Display help |

### Option Details

**--charset MODE** - selects the Sharp MZ character set conversion for filenames
in the IPLPRO bootstrap header (--boot).

Sharp MZ computers use their own character set (Sharp MZ ASCII), which exists
in two variants: European (EU) and Japanese (JP). It is compatible with
standard ASCII only in the range 0x20-0x5D (uppercase letters, digits, and
basic punctuation). Characters above 0x5D (lowercase letters, special symbols)
are placed at different positions than in standard ASCII.

The `eu` and `jp` modes perform single-byte conversion to standard ASCII.
The Japanese variant is more limited than the European one - it lacks lowercase
letters and some other characters, so unconvertible characters are replaced
with spaces.

The `utf8-eu` and `utf8-jp` modes convert to UTF-8 and attempt to display
as many characters as possible, including graphic symbols and special
characters that have no equivalent in single-byte ASCII conversion.

Available modes:
- `eu` (default) - Sharp MZ EU -> ASCII (single-byte conversion, European character set)
- `jp` - Sharp MZ JP -> ASCII (single-byte conversion, Japanese character set)
- `utf8-eu` - Sharp MZ EU -> UTF-8 (European variant with graphic symbols)
- `utf8-jp` - Sharp MZ JP -> UTF-8 (Japanese variant with katakana and symbols)

**--dump-charset MODE** - selects the Sharp MZ character set conversion for
the hexdump ASCII column. Mode meanings are the same as for --charset, see above.

**--cnv / --nocnv** - `--cnv` is an alias for `--dump-charset eu`.
With `--nocnv`, conversion is force-disabled and raw byte values are shown.
By default, conversion is applied automatically based on context.

**--inv / --noinv** - controls data inversion when reading sectors. The FSMZ
filesystem (MZ-BASIC) stores data with bitwise inversion (XOR 0xFF) on tracks
with 16 sectors of 256 bytes. By default, inversion is applied automatically
based on detected track geometry. Use `--inv` to force inversion on all
sectors, or `--noinv` to read raw uninverted data.

**--sector T S** - displays a hex dump (16 bytes per line with offset, hex
values, and ASCII) of the sector identified by absolute track number T and
sector ID S. Track numbering is absolute (both sides counted sequentially).

**--block N** - displays a hex dump of FSMZ block N. The block-to-sector
mapping is based on the FSMZ filesystem layout.

## Displayed Information

### Basic Info (no command)

- Image format: MZ-BASIC, CP/M SD, CP/M HD, MRS, Boot, or Unknown
- Geometry: tracks per side, sides, total absolute tracks
- Total data size and image file size
- Track layout rules (groups of tracks with same geometry)
- For each rule: track range, sector count, sector size

### Disk Usage Map (--map)

- Filesystem auto-detection (FSMZ, CP/M SD/HD/SD2S/HD2S, MRS)
- Block numbers with usage indicators
- Summary: total blocks, used blocks, free blocks

### Boot Sector (--boot)

- IPLPRO block (block 0): file type, name, size, load/exec addresses
- DINFO block (block 15): volume number, farea start, used/total blocks

## Examples

Display basic information about a DSK image:

```bash
mzdsk-info disk.dsk
```

Example output:

```
=== disk.dsk ===

Format     : Extended CPC DSK
Creator    : DSKLib v1.1
Tracks     : 40
Sides      : 1
Total trk  : 40
Ident      : MZ-BASIC

Track rules:
  [  0] Track  0..39 : 16 x 256 B
```

Display the disk usage map:

```bash
mzdsk-info disk.dsk --map
```

Display boot sector and disk information:

```bash
mzdsk-info disk.dsk --boot
```

Example output:

```
=== Boot sector (IPLPRO) ===

Ftype      : 0x03
IPLPRO     : IPLPRO
Filename   : "BASIC MZ800"
Fsize      : 2048 (0x0800)
Fstrt      : 0x0000
Fexec      : 0x0000
Block      : 1

=== Disk info (DINFO) ===

Volume     : 0 (master)
Farea      : 48 (0x30)
Used       : 52
Blocks     : 639
Free       : 587
```

Display hex dump of sector 1 on track 0:

```bash
mzdsk-info disk.dsk --sector 0 1
```

**Note on `--sector T S`:** `S` is the **sector ID** from the IDAM
(address mark in the track header), not the positional index. For
standard presets (basic, cpm-sd, cpm-hd, mrs) IDs are 1-based
sequential so `S` matches the position. For disks with a custom sector
map (e.g. the `lemmings` preset, track 16 has IDs
`{1,6,2,7,3,8,4,9,5,21}`) the actual ID must be given. If the ID does
not exist on the track, `mzdsk-info` lists the available IDs and exits
with an error:

```
Error: sector ID 10 not found on track 16 (available IDs: 1 6 2 7 3 8 4 9 5 21)
```

The list of IDs for any track can be obtained via
`mzdsk-dsk disk.dsk tracks --abstrack T`.

Display hex dump of FSMZ block 48:

```bash
mzdsk-info disk.dsk --block 48
```

Read a sector without automatic data inversion:

```bash
mzdsk-info disk.dsk --noinv --sector 0 1
```

Display hex dump with forced Sharp ASCII conversion:

```bash
mzdsk-info disk.dsk --cnv --sector 0 1
```

Display boot info without character conversion:

```bash
mzdsk-info disk.dsk --nocnv --boot
```

Display library versions:

```bash
mzdsk-info --lib-versions
```
