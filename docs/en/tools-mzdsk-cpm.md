# mzdsk-cpm - CP/M Filesystem Tool

Tool version: 1.19.1. Part of the [mzdisk](https://github.com/michalhucik/mzdisk/) project.

> **Disclaimer:** Always work on a copy of your DSK image, never on the
> original. The author provides no warranty and accepts no liability for
> data loss or damage resulting from use of the mzdisk project tools.
> Distributed under the GPLv3 license, without warranty.

Performs CP/M filesystem operations on Sharp MZ DSK disk images. Supports
CP/M formats used by the LEC CP/M system: SD (single density, 9x512B
sectors) and HD (high density, 18x512B sectors). Both variants can be
1-sided or 2-sided - the geometry is autodetected from the DSK image.

CP/M uses 128-byte logical sectors and allocation blocks (2 KB for SD,
4 KB for HD). The boot track (absolute track 1) uses MZ-BASIC geometry
(16 x 256 B) and is not part of the CP/M filesystem. The disk format is
autodetected from DSK image geometry (sectors per track, number of sides).

When opening a DSK image, the tool automatically repairs incorrect tsize
values in the header (e.g. wrong boot track size).

**Atomic writes:** Write operations (`put`, `era`, `ren`, `attr`, `format`,
`defrag`, `put-block`) are atomic - the entire DSK image is loaded into
memory, operations run in RAM, and the result is flushed back in a single
write only after successful completion. If an operation fails midway, the
original file is left unchanged. Keeping backups for critical data is still
recommended - completed changes are irreversible.

## Usage

```
mzdsk-cpm [options] <image.dsk> <command> [arguments...]
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<image.dsk>` | DSK disk image file |

## Commands

### Directory operations

| Command | Description |
|---------|-------------|
| `dir` | List CP/M directory contents |
| `dir --ex` | Extended listing with attributes, extent count and dir index |
| `dir --raw` | Raw 32-byte directory entries (including deleted) |
| `file <name.ext>` | Show file details (including extents and blocks) |

### File operations

| Command | Description |
|---------|-------------|
| `get <name.ext> <output>` | Extract file from CP/M disk (raw binary) |
| `get --mzf <name.ext> <output.mzf> [options]` | Extract as MZF (default: CPM-IC, type $22) |
| `get --all <dir> [options]` | Extract all files to directory |
| `put <input> <name.ext>` | Write file to CP/M disk (raw binary) |
| `put <input> --name NAME.EXT` | Write file with strict name validation (no truncation) |
| `put --mzf <input.mzf> [name.ext]` | Write MZF file to CP/M disk |
| `put --mzf <input.mzf> --name NAME.EXT` | Write MZF with strict name override |
| `era <name.ext>` | Delete file from CP/M disk |
| `ren <old.ext> <new.ext>` | Rename file on CP/M disk |
| `chuser <name.ext> <new-user>` | Change user number (0-15) of existing file |

#### get --mzf

Extracts a file from the CP/M disk into an MZF (SharpMZ tape format)
frame. The output is a single MZF frame - 128B header plus a data body
of the file's size. Defaults follow the SOKODI CMT.COM convention for
CP/M export: type 0x22, load 0x0100, exec 0x0100, attribute encoding on.

```
get --mzf <name.ext> <output.mzf> [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--ftype HH` | 22 | MZF file type (hex, 0x00-0xFF). Known values: 01=OBJ, 02=BTX, 03=BSD, 04=BRD, 05=RB, 22=SOKODI CMT.COM convention for CP/M export. |
| `--exec-addr N` | 0x0100 | Exec address (0x0000-0xFFFF) written to the MZF `fexec` field. |
| `--addr N` | 0x0100 | Alias for `--exec-addr` (backward compatibility). |
| `--strt-addr N` | 0x0100 | Load address (0x0000-0xFFFF) written to the MZF `fstrt` field. |
| `--no-attrs` | off | Suppress encoding of CP/M R/O, SYS, ARC attributes into bit 7 of `fname[9..11]`. Only applies to `--ftype 22` (SOKODI CMT.COM convention); other types never encode attributes. |

Note: the MZF `fstrt` and `fexec` fields originate from the SharpMZ tape
format, where they describe the program's load and exec addresses on CMT.
CP/M itself doesn't use load/exec addresses (all programs load and run
from TPA 0x0100), which is why the defaults are 0x0100.

The filename in the MZF header is taken from the CP/M directory.

Size limit: the MZF `fsize` field is 16 bits, so the data body cannot
exceed 65535 B. Larger files are rejected by `get --mzf`.

#### get --all

Extracts all files from the CP/M disk to the specified directory.

```
get --all <dir> [--mzf] [--by-user] [--on-duplicate MODE]
                [--ftype HH] [--exec-addr N] [--strt-addr N] [--no-attrs]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--mzf` | - | Extract as MZF (default: CPM-IC $22) |
| `--by-user` | - | Create subdirectories per user (user00-user15) |
| `--on-duplicate MODE` | rename | Handle duplicate names: rename, overwrite, skip |
| `--ftype HH` | 22 | MZF file type (hex, see `get --mzf`) |
| `--exec-addr N` | 0x0100 | MZF exec address (`fexec`). `--addr N` kept as an alias. |
| `--strt-addr N` | 0x0100 | MZF load address (`fstrt`). |
| `--no-attrs` | off | Don't encode CP/M attributes into bit 7 of `fname` (only applies to `--ftype 22`). |

`--on-duplicate` modes:
- `rename` - rename the file by appending a number (default)
- `overwrite` - overwrite existing file
- `skip` - skip the file

#### Option compatibility validation for `get`

The tool strictly validates that options match the active mode.
Incompatible combinations fail with `exit 1` instead of being silently
ignored. This is consistent with `mzdsk-mrs`.

| Rule | Error message |
|------|----------------|
| `--ftype`, `--exec-addr`, `--strt-addr`, `--no-attrs` require `--mzf` | `--ftype/--exec-addr/--strt-addr/--no-attrs require --mzf` |
| `--by-user` requires `--all` | `--by-user can only be used with --all` |
| `--on-duplicate` requires `--all` | `--on-duplicate can only be used with --all` |

Valid combinations:

```bash
get <name> <output>                       # plain raw export
get --mzf <name> <out.mzf>                # single-file MZF, --ftype etc. OK
get --all <dir>                           # batch raw export
get --all --mzf <dir>                     # batch MZF export, --ftype etc. OK
get --all --by-user <dir>                 # batch with user00/.. subdirs
get --all --on-duplicate overwrite <dir>  # batch with overwrite
```

#### put --mzf

Writes an MZF file to the CP/M disk. The name, size and data are taken
from the MZF header. The R/O, SYS, ARC attributes are extracted from bit 7
of the extension characters only if the file has `ftype == 0x22` (SOKODI
CMT.COM convention); for other types attributes are not carried. The
`fstrt` and `fexec` fields are not needed for CP/M import and are ignored.

```
put --mzf <input.mzf> [name.ext] [--charset eu|jp]
                                 [--force-charset] [--no-attrs]
```

If `name.ext` is specified, it overrides the filename from the MZF header.
If omitted, the filename is derived from `fname` in the MZF header.

#### fname conversion - `--charset` switch

MZF files with `ftype != 0x22` (typically native MZ-700/MZ-800 programs:
OBJ, BTX, BSD, BRD) store their name in **Sharp MZ ASCII**, not in
standard ASCII. The difference is noticeable for Sharp-specific
characters (especially lowercase letters in the 0x80-0xFF range), which
naive bit-7 masking turns into non-printable bytes.

The `--charset` switch selects the Sharp MZ character set variant used
for conversion:

- `--charset eu` (default) - European variant (MZ-700/MZ-800)
- `--charset jp` - Japanese variant (MZ-1500)

After conversion, the string is trimmed, split into 8.3 on the last
dot, and sanitized - non-printable bytes and internal spaces are
replaced with `_` so that the resulting CP/M directory entry satisfies
the filesystem auto-detection criteria.

For `ftype == 0x22` (CPM-IC export created by our tools) the switch is
ignored - `fname` is already ASCII there and is interpreted directly,
including attributes in bit 7.

#### Switches for foreign MZF with `ftype == 0x22`

The vast majority of MZF files with `ftype == 0x22` are our own
CPM-IC export, where `fname` is ASCII and bit 7 of the extension
encodes attributes. For edge cases (hand-edited MZF, MZF from a
foreign tool, forensic analysis), `put --mzf` offers two
additional switches:

- `--force-charset` - apply the `--charset` conversion (Sharp MZ
  EU/JP) even for `ftype == 0x22`, instead of the CPM-IC ASCII
  mask. Use when `fname` is actually encoded in Sharp MZ ASCII
  (not CPM-IC ASCII), e.g. after a manual `ftype` change without
  re-encoding the name.
- `--no-attrs` - suppress decoding of R/O, SYS, ARC attributes
  from bit 7 of the extension. Use for foreign MZF where bit 7 is
  not a CP/M attribute and decoding would write unintended
  attributes to the disk.

Both switches can be combined. `--no-attrs` only affects
behavior for `ftype == 0x22` (for other types attributes are
never decoded). Using `--force-charset` or `--no-attrs` without
`--mzf` fails with an error.

Examples:

```
# MZF with ftype=0x22 but Sharp-encoded fname - force Sharp EU decoding
mzdsk-cpm disk.dsk put --mzf weird.mzf --force-charset --charset eu

# MZF from a foreign tool - ext bit 7 is not attribute data
mzdsk-cpm disk.dsk put --mzf foreign.mzf --no-attrs

# Both edge cases at once
mzdsk-cpm disk.dsk put --mzf unknown.mzf --force-charset --charset jp --no-attrs
```

#### Long-name truncation

If the requested name exceeds 8 characters (or the extension exceeds 3),
`put` and `put --mzf` truncate it to the CP/M 8.3 limit and emit a
warning on stderr:

```
Warning: Name 'TOOLONGNAME.EXT' was truncated to 'TOOLONGN.EXT'
Written '/path/to/source.bin' -> 'TOOLONGN.EXT' (1024 bytes)
```

The final `Written` line shows the actual on-disk name. If the
truncated name collides with an existing file (e.g. two different long
names reducing to the same 8.3 form), the second `put` fails with a
"File exists" error.

#### Strict name validation via `--name`

The `--name NAME.EXT` option on `put` and `put --mzf` enables strict
mode - no silent truncation, no normalization. If the name violates
CP/M rules, the operation fails with an error.

```
put <input> --name NAME.EXT
put --mzf <input.mzf> --name NAME.EXT
```

Validation rules:
- name up to 8 characters,
- extension up to 3 characters,
- empty filename rejected (input like `.TXT`),
- forbidden characters: `< > , ; : = ? * [ ] | /`, plus dot inside the
  name or extension, space, and control characters (0x00-0x1F, 0x7F).

Error messages on stderr (exit code 1):

```
Error: Name 'TOOLONGNAME.EXT' exceeds 8.3 limit (no truncation with --name)
Error: Name 'A*B.TXT' contains forbidden character '*' (no truncation with --name)
Error: Name '.TXT' has empty filename (no truncation with --name)
```

The `--name` option replaces the 2nd positional argument. Supplying
both forms simultaneously returns
`Error: --name conflicts with positional <name.ext>`.

Lowercase letters are allowed; the library converts to uppercase
internally (CP/M convention). Input `my.txt` is valid and stored as
`MY.TXT`.

#### chuser - change user number of existing file

Changes the user number on all extents of an already existing file.
The source user comes from the global `--user N` option (default 0);
the target user is the second positional argument (0-15).

```
[--user N] chuser <name.ext> <new-user>
```

Examples:

```
# move file from user 0 to user 3
mzdsk-cpm disk.dsk chuser HELLO.TXT 3

# move from user 2 to user 7 (source given via --user)
mzdsk-cpm --user 2 disk.dsk chuser HELLO.TXT 7
```

CP/M 2.2 rules (P-CP/M on Sharp MZ):

- Users 0-15 are isolated namespaces; the same name may exist
  simultaneously in different user areas.
- If a file with the same name already exists in the target user
  area, the operation fails with `File exists` and the disk is
  left unchanged.
- If the source and target users are identical, the operation is
  a no-op and returns success.
- Only the user byte in the directory is rewritten - data blocks,
  extent numbers, RC and attributes are preserved.

### Attributes

| Command | Description |
|---------|-------------|
| `attr <name.ext>` | Show file attributes |
| `attr <name.ext> [+\|-][RSA]` | Set/clear attributes (+R/-R, +S/-S, +A/-A) |

Attributes can be combined: `+RSA`, `+S+A`, `+R-S` (set R/O, clear System).

### Block operations

| Command | Description |
|---------|-------------|
| `dump-block N [bytes]` | Hexdump of CP/M allocation block |
| `get-block N <file> [bytes]` | Extract CP/M block(s) to file |
| `put-block N <file> [bytes] [offset]` | Write file into CP/M block(s) |

The `bytes` parameter specifies the number of bytes to process. If omitted,
the entire block is processed. The `offset` parameter for `put-block`
specifies the starting offset within the block.

`dump-block` option:

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--dump-charset MODE` | raw\|eu\|jp\|utf8-eu\|utf8-jp | raw | Character set for hexdump ASCII column |

### Disk operations

| Command | Description |
|---------|-------------|
| `dpb` | Show active DPB (Disk Parameter Block) |
| `map` | Show block allocation map |
| `free` | Show free space |
| `format` | Initialize empty directory (fill with 0xE5) |
| `defrag` | Defragment disk (rewrite files sequentially without gaps) |

## Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--format` | sd\|hd | autodetect | CP/M disk format |
| `--user` | 0-15 | 0 | CP/M user number (dir: all if omitted) |
| `--ro` | - | - | Force read-only mode |
| `--force` | - | - | Bypass the CP/M filesystem check (see below) |
| `--output FMT` | text\|json\|csv | text | Output format (text, json, csv) |
| `-o FMT` | text\|json\|csv | text | Short for --output |
| `--version` | - | - | Display program version |
| `--lib-versions` | - | - | Display library versions |
| `--help`, `-h` | - | - | Show help |

### Protection against operations on a foreign filesystem

Before any operation, the tool verifies the filesystem type on the disk
(via `mzdsk_detect_filesystem`). If the disk is not recognized as CP/M
(e.g. it is FSMZ, MRS, a boot track only, or an unknown format), the
operation aborts with the error "disk is not a CP/M filesystem" and
exit code 1.

This protection also applies to read-only commands (`dir`, `file`,
`dump-block`, etc.), since they would otherwise print nonsense figures
on a foreign disk, and it is critical for destructive commands
(`format` would silently overwrite 2048 B inside the FSMZ FAREA area,
`put`/`era`/`defrag` would similarly corrupt data).

To explicitly operate on a non-CP/M disk (e.g. `format` to reformat it
as CP/M, forensic viewing, diagnostics) use `--force`. The tool prints
a warning and proceeds. `--force` must appear before the subcommand.

### Custom DPB parameters

Override specific values in the selected format preset. Derived values
(BLM, block_size, CKS) are recalculated automatically.

| Option | Description |
|--------|-------------|
| `--spt N` | Logical (128B) sectors per track |
| `--bsh N` | Block shift factor (3-7), block_size = 128 << BSH |
| `--exm N` | Extent mask |
| `--dsm N` | Total blocks - 1 |
| `--drm N` | Directory entries - 1 |
| `--al0 N` | Directory allocation byte 0 (hex: 0xC0) |
| `--al1 N` | Directory allocation byte 1 (hex: 0x00) |
| `--off N` | Reserved tracks count |

### Option details

**--format** - selects the CP/M disk format. Default is autodetection
from DSK image geometry (sectors per track and number of sides).

| Format | Sectors | Block size | Dir entries | OFF | Description |
|--------|---------|------------|-------------|-----|-------------|
| sd | 9x512B | 2048 B | 128 | 4 | SD (1-sided and 2-sided) |
| hd | 18x512B | 4096 B | 128 | 4 | HD (1-sided and 2-sided) |

**--user N** - specifies the CP/M user number (0-15). Each user has an
independent file namespace. Affects all operations. For `dir`, all users
are shown if `--user` is not specified.

**--dump-charset MODE** - option of the `dump-block` command. Selects the
Sharp MZ character set conversion for the hexdump ASCII column.

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

**Custom DPB** - allows experimentation with non-standard formats.
Example: `--spt 36 --bsh 4 --dsm 350 --drm 127 --al0 0xC0 --off 4`

---

## Examples

List directory of a 2-sided SD disk (format autodetected):

```bash
mzdsk-cpm disk.dsk dir
```

Extended listing with attributes:

```bash
mzdsk-cpm disk.dsk dir --ex
```

Extract and import files (raw binary):

```bash
mzdsk-cpm disk.dsk get PROGRAM.COM program.com
mzdsk-cpm disk.dsk put data.txt DATA.TXT
mzdsk-cpm disk.dsk put data.txt --name DATA.TXT   # strict validation
```

Extract and import in MZF format with default values
(type 0x22, load/exec 0x0100, attribute encoding on):

```bash
mzdsk-cpm disk.dsk get --mzf PROGRAM.COM program.mzf
mzdsk-cpm disk.dsk get --mzf PROGRAM.COM program.mzf --exec-addr 0x0200
mzdsk-cpm disk.dsk put --mzf program.mzf
mzdsk-cpm disk.dsk put --mzf program.mzf NEWNAME.COM
```

Extract with custom type and addresses:

```bash
mzdsk-cpm disk.dsk get --mzf PROGRAM.COM program.mzf \
    --ftype 01 --strt-addr 0x1200 --exec-addr 0x1200
mzdsk-cpm disk.dsk get --mzf TEXT.TXT text.mzf --ftype 02 --no-attrs
```

Extract all files:

```bash
mzdsk-cpm disk.dsk get --all ./output
mzdsk-cpm disk.dsk get --all ./output --mzf --by-user
mzdsk-cpm disk.dsk get --all ./output --mzf --exec-addr 0x0200 --on-duplicate skip
mzdsk-cpm disk.dsk get --all ./output --mzf --ftype 01 --strt-addr 0x1200
```

**Exit code `get --all`:** 0 on success (including files skipped via
`--on-duplicate skip`), 1 if at least one file could not be extracted
due to an actual error (e.g. I/O error, corrupted block). This lets
scripts detect partial failures.

Show DPB parameters:

```bash
mzdsk-cpm disk.dsk dpb
```

Use custom DPB parameters:

```bash
mzdsk-cpm --spt 36 --bsh 4 --dsm 350 --off 4 disk.dsk dir
```

Manage attributes:

```bash
mzdsk-cpm disk.dsk attr PROGRAM.COM +R +S
mzdsk-cpm disk.dsk attr PROGRAM.COM -R
```

Allocation map and free space:

```bash
mzdsk-cpm disk.dsk map
mzdsk-cpm disk.dsk free
```

Defragment disk:

```bash
mzdsk-cpm disk.dsk defrag
```

Block operations:

```bash
mzdsk-cpm disk.dsk dump-block 5
mzdsk-cpm disk.dsk dump-block 5 512
mzdsk-cpm disk.dsk get-block 10 block10.bin
mzdsk-cpm disk.dsk get-block 10 block10.bin 1024
mzdsk-cpm disk.dsk put-block 10 data.bin
mzdsk-cpm disk.dsk put-block 10 data.bin 512 128
```

---

## Non-sequential Sectors

Block operations (get-block, put-block, dump-block) traverse sectors
sequentially by sector ID (like an FDC controller). Sectors with
non-sequential IDs are not accessible through block commands.

To access these sectors, use `mzdsk-raw` with the `--order phys` option:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 10 --order phys
```
