# mzdsk-fsmz - FSMZ Filesystem Tool (MZ-BASIC)

Tool version: 1.14.2. Part of the [mzdisk](https://github.com/michalhucik/mzdisk/) project.

> **Disclaimer:** Always work on a copy of your DSK image, never on the
> original. The author provides no warranty and accepts no liability for
> data loss or damage resulting from use of the mzdisk project tools.
> Distributed under the GPLv3 license, without warranty.

Complete filesystem management tool for the FSMZ (MZ-BASIC) disk format used
by Sharp MZ-800 computers with Disc BASIC (IPLDISK). Supports directory listing,
file extraction and insertion, deletion, renaming, locking, file type changes,
boot sector management, filesystem formatting, integrity repair, and
defragmentation.

FSMZ disks use 16 sectors of 256 bytes per track. Data is stored with bitwise
inversion (XOR 0xFF). Allocation blocks map 1:1 to physical sectors. The
directory occupies blocks 16-23 (standard) or 16-31 (IPLDISK extended), with
the file area starting at the block specified by DINFO (typically block 48).

**Atomic writes:** Write operations (`put`, `era`, `ren`, `lock`, `chtype`,
`set`, `format`, `repair`, `defrag`, `put-block`, `boot put/clear/bottom/mini/over`,
`boot` with `--fstrt`/`--fexec`/`--ftype`/`--name`)
are atomic - the entire DSK image is loaded into memory, operations run in
RAM, and the result is flushed back in a single write only after successful
completion. If an operation fails midway, the original file is left
unchanged. Keeping backups for critical data is still recommended -
completed changes are irreversible.

## Usage

```
mzdsk-fsmz [options] <image.dsk> <command> [arguments...]
```

Options can be placed before the DSK file or between the DSK file and the command.

## Arguments

| Argument | Description |
|----------|-------------|
| `<image.dsk>` | DSK disk image file |

## Commands

| Command | Description |
|---------|-------------|
| `dir` | List all files |
| `dir --type T` | List files filtered by ftype (T is hex, e.g. 01) |
| `file <name>` | Show file info by name |
| `file --id N` | Show file info by ID |
| `get <name> <mzf>` | Extract file to MZF by name |
| `get --id N <mzf>` | Extract file to MZF by ID |
| `get --all <dir>` | Extract all files to directory |
| `put <mzf>` | Insert MZF file (128 B header + data) |
| `era <name>` | Delete file by name |
| `era --id N` | Delete file by ID |
| `ren <name> <new>` | Rename file by name |
| `ren --id N <new>` | Rename file by ID |
| `lock <name> 0\|1` | Lock (1) or unlock (0) file by name |
| `lock --id N 0\|1` | Lock (1) or unlock (0) file by ID |
| `chtype --id N T` | Change ftype of file by ID (T is hex) |
| `set <name> [--fstrt HEX] [--fexec HEX] [--ftype HEX]` | Update STRT/EXEC/ftype of existing file by name |
| `set --id N [--fstrt HEX] [--fexec HEX] [--ftype HEX]` | Update STRT/EXEC/ftype of existing file by ID |
| `boot` | Show bootstrap info (with type classification) |
| `boot [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--name NAME]` | Update bootstrap header fields and show info |
| `boot put <mzf>` | Install normal bootstrap from MZF |
| `boot get <mzf>` | Extract bootstrap to MZF |
| `boot clear` | Clear bootstrap |
| `boot bottom <mzf>` | Install bottom bootstrap from MZF |
| `boot mini <mzf>` | Alias for `boot bottom` |
| `boot bottom --no-fsmz-compat <mzf>` | Bottom bootstrap allowing FSMZ structure overwrite |
| `boot over <mzf>` | Install over-FAREA bootstrap (experimental) |
| `dump-block N [bytes] [--cnv] [--dump-charset MODE]` | Hexdump of FSMZ allocation block(s) |
| `get-block N <file> [bytes] [--noinv]` | Extract FSMZ block(s) to file |
| `put-block N <file> [bytes] [offset] [--noinv]` | Write file into FSMZ block(s) |
| `format` | Quick FSMZ format |
| `repair` | Repair DINFO block |
| `defrag` | Defragment filesystem |

## Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--ipldisk` | - | off | Use IPLDISK extended directory (127 entries instead of 63) |
| `--ro` | - | off | Open disk image in read-only mode |
| `--output FMT` | text\|json\|csv | text | Output format (text, json, csv) |
| `-o FMT` | text\|json\|csv | text | Short for --output |
| `--charset MODE` | eu\|jp\|utf8-eu\|utf8-jp | eu | Sharp MZ character set conversion |
| `-C MODE` | eu\|jp\|utf8-eu\|utf8-jp | eu | Short for --charset |
| `--version` | - | - | Display program version |
| `--lib-versions` | - | - | Display library versions |
| `--help` | - | - | Display help |
| `-h` | - | - | Short for --help |

The `--dump-charset MODE` option (values `raw`\|`eu`\|`jp`\|`utf8-eu`\|`utf8-jp`,
default `raw`) is a per-command option used only by the `dump-block` subcommand;
it controls the Sharp MZ character set conversion in the hexdump ASCII column
(see [dump-block](#dump-block---hexdump-fsmz-block) section).

### Option Details

**--ipldisk** - enables the IPLDISK extended directory format. Standard
MZ-BASIC Disc BASIC uses 8 directory blocks (blocks 16-23) with a maximum
of 63 file entries. IPLDISK extends the directory to 16 blocks (blocks 16-31)
allowing up to 127 entries. Use this option when working with disks formatted
by the IPLDISK program.

**--charset MODE** - selects the Sharp MZ character set conversion for filenames
in the FSMZ directory and bootstrap header.

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

**--dump-charset MODE** - per-command option for the `dump-block` subcommand
only. Selects the Sharp MZ character set conversion for the hexdump ASCII
column. Mode meanings are the same as for --charset, see above.

**--ro** - opens the disk image in read-only mode. All write operations
(put, era, ren, lock, chtype, set, format, repair, defrag, boot put/clear/bottom/mini/over,
boot with --fstrt/--fexec/--ftype/--name) will be rejected.
Useful for safely inspecting disk contents without risk of modification.

---

## dir - Directory Listing

Lists all files in the FSMZ directory with their attributes.

```
mzdsk-fsmz [options] <image.dsk> dir [--type T]
```

The optional `--type T` filter shows only files with the given ftype
(hex value, e.g. 01 for OBJ).

### Example

```bash
mzdsk-fsmz mydisk.dsk dir
```

Output:

```
Directory of mydisk.dsk (MZ-BASIC)

 ID  Type  Name              Size    Load    Exec    Block  Locked
  0  0x01  GAME              4096    0x1200  0x1200  48     -
  1  0x05  BASIC PROG        2048    0x7F53  0x7F53  64     *
  2  0x01  LOADER            1024    0xC000  0xC000  72     -

  3 file(s), 7168 bytes used
  Free: 153600 bytes (600 blocks)
```

Filtering by file type:

```bash
mzdsk-fsmz mydisk.dsk dir --type 01
```

Using IPLDISK extended directory:

```bash
mzdsk-fsmz --ipldisk mydisk.dsk dir
```

---

## file - File Information

Displays detailed information about a single file identified by name or
directory ID.

```
mzdsk-fsmz [options] <image.dsk> file <name>
mzdsk-fsmz [options] <image.dsk> file --id N
```

### Example

```bash
mzdsk-fsmz mydisk.dsk file GAME
mzdsk-fsmz mydisk.dsk file --id 0
```

---

## get - Extract File

Extracts a file from the DSK image to the host filesystem as an MZF file
(128 B header + data). The file is identified by its name on the disk
or by directory ID.

```
mzdsk-fsmz [options] <image.dsk> get <name> <mzf>
mzdsk-fsmz [options] <image.dsk> get --id N <mzf>
```

The output MZF file is a required argument.

### Example

Extract a file by name:

```bash
mzdsk-fsmz mydisk.dsk get GAME game.mzf
```

Extract by directory ID:

```bash
mzdsk-fsmz mydisk.dsk get --id 0 game.mzf
```

### get --all - Extract All Files

Extracts all files from the FSMZ disk into a directory. If the target
directory does not exist, it is created automatically. Output files are
in MZF format, named after the disk filenames with .mzf extension.
Characters invalid on Windows (*, ?, ", <, >, |, :, \, /) are replaced
with underscore '_'.

```
mzdsk-fsmz [options] <image.dsk> get --all <directory> [--on-duplicate MODE]
```

#### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--on-duplicate MODE` | rename | Handling of duplicate output filenames: `rename` (append `~2`, `~3`, ... before `.mzf`), `overwrite` (overwrite existing), `skip` (skip with a warning on stderr). |

The FSMZ filesystem allows two files with the same name in the
directory (see e.g. `refdata/dsk/basic.dsk`). Without an explicit strategy
the second file would silently overwrite the first on the host. The
default `rename` keeps all files with a numeric suffix.

#### Examples

```bash
mzdsk-fsmz mydisk.dsk get --all export/
mzdsk-fsmz mydisk.dsk get --all export/ --on-duplicate skip
mzdsk-fsmz mydisk.dsk get --all export/ --on-duplicate overwrite
```

After completion, a summary is printed on stdout: number of extracted,
renamed and skipped files.

---

## put - Write File

Writes an MZF file (128 B header + data) onto the DSK image. The filename
and metadata (type, load address, exec address) are taken from the MZF
header. The file is stored as a contiguous sequence of allocation blocks.

```
mzdsk-fsmz [options] <image.dsk> put <mzf>
```

### Input validation

Before writing, the MZF file is validated:

- Minimum size 128 B (MZF header size). Smaller files are rejected
  with "too small to be an MZF file".
- Presence of the filename terminator (0x0D) in the fname field.
- Consistency of the `fsize` field with the available data size
  (header + body must not exceed file size). Otherwise "File size
  does not match fsize field".

If validation fails, the command aborts with a non-zero exit code and
the disk image is left untouched.

### Example

```bash
mzdsk-fsmz mydisk.dsk put game.mzf
mzdsk-fsmz mydisk.dsk put loader.mzf
```

---

## era - Delete File

Deletes a file from the disk image. Sets the directory entry's file type to 0
and releases allocated blocks in the DINFO bitmap.

If the file has the `locked` flag set, the operation fails with
"File is locked" by default. To delete a locked file, use the `--force`
option (or unlock it first using `lock --id N 0`).

```
mzdsk-fsmz [options] <image.dsk> era <name> [--force]
mzdsk-fsmz [options] <image.dsk> era --id N [--force]
```

### Example

```bash
mzdsk-fsmz mydisk.dsk era GAME
mzdsk-fsmz mydisk.dsk era --id 0
mzdsk-fsmz mydisk.dsk era --id 0 --force    # ignore lock flag
```

---

## ren - Rename File

Renames a file on the disk image. The new name must not already exist in
the directory.

If the file has the `locked` flag set, the operation fails with
"File is locked" by default. To rename a locked file, use the `--force`
option.

```
mzdsk-fsmz [options] <image.dsk> ren <name> <new_name> [--force]
mzdsk-fsmz [options] <image.dsk> ren --id N <new_name> [--force]
```

### Example

```bash
mzdsk-fsmz mydisk.dsk ren GAME "MY GAME"
mzdsk-fsmz mydisk.dsk ren --id 0 "MY GAME"
mzdsk-fsmz mydisk.dsk ren --id 0 "MY GAME" --force    # ignore lock flag
```

---

## lock - Lock/Unlock File

Sets the lock attribute on a file. The value 1 locks the file, 0 unlocks it.
Locked files are typically protected from accidental deletion in MZ-BASIC.

```
mzdsk-fsmz [options] <image.dsk> lock <name> 0|1
mzdsk-fsmz [options] <image.dsk> lock --id N 0|1
```

### Example

```bash
mzdsk-fsmz mydisk.dsk lock GAME 1
mzdsk-fsmz mydisk.dsk lock --id 0 0
```

---

## chtype - Change File Type

Changes the file type attribute of a directory entry. The file is identified
by directory ID only (--id is required).

```
mzdsk-fsmz [options] <image.dsk> chtype --id N T
```

T is a hex value for the file type:
- `01` - OBJ (machine code binary)
- `02` - BTM (bootstrap)
- `03` - BSD (BASIC data)
- `05` - BAS (BASIC program)

### Example

```bash
mzdsk-fsmz mydisk.dsk chtype --id 0 01
```

---

## set - Update STRT/EXEC/ftype of Existing File

Updates the directory entry without re-uploading the data. Each of
`--fstrt`, `--fexec` and `--ftype` is optional, but at least one must
be supplied. File data and allocation blocks are not modified.

```
mzdsk-fsmz [options] <image.dsk> set <name> [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--force]
mzdsk-fsmz [options] <image.dsk> set --id N [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--force]
```

| Option | Description |
|--------|-------------|
| `--fstrt HEX` | New load (start) Z80 address, range `0x0000-0xFFFF` |
| `--fexec HEX` | New exec Z80 address, range `0x0000-0xFFFF` |
| `--ftype HEX` | New file type, range `0x01-0xFF` (the value `0x00` is reserved for deleted entries and is rejected) |
| `--force` | Ignore the `locked` flag - without it, locked files return an error |

When the file is locked and `--force` is not given, the program prints
`File is locked` and makes no change.

### Examples

```bash
# Update the load address only, by name
mzdsk-fsmz mydisk.dsk set MONITOR --fstrt 1200

# Update load and exec addresses, by ID
mzdsk-fsmz mydisk.dsk set --id 3 --fstrt 1200 --fexec 120B

# Update load address and type (BSD -> OBJ), ignore lock
mzdsk-fsmz mydisk.dsk set --id 0 --fstrt 1200 --ftype 01 --force
```

Expected output:

```
Set address fields for MONITOR.
  fstrt: 0x1200
```

---

## boot - Boot Sector Management

Displays or modifies the IPLPRO block (block 0) and DINFO block (block 15).

| Subcommand | Description |
|------------|-------------|
| `boot` | Show bootstrap info (with type classification) |
| `boot [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--name NAME]` | Update bootstrap header fields and show info |
| `boot put <mzf>` | Install normal bootstrap from MZF |
| `boot get <mzf>` | Extract bootstrap to MZF |
| `boot clear` | Clear bootstrap |
| `boot bottom <mzf>` | Install bottom bootstrap from MZF |
| `boot mini <mzf>` | Alias for `boot bottom` (backward compatibility) |
| `boot bottom --no-fsmz-compat <mzf>` | Bottom bootstrap allowing FSMZ structure overwrite |
| `boot over <mzf>` | Install over-FAREA bootstrap (experimental) |

```
mzdsk-fsmz [options] <image.dsk> boot
mzdsk-fsmz [options] <image.dsk> boot [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--name NAME]
mzdsk-fsmz [options] <image.dsk> boot put <mzf>
mzdsk-fsmz [options] <image.dsk> boot get <mzf>
mzdsk-fsmz [options] <image.dsk> boot clear
mzdsk-fsmz [options] <image.dsk> boot bottom <mzf>
mzdsk-fsmz [options] <image.dsk> boot bottom --no-fsmz-compat <mzf>
mzdsk-fsmz [options] <image.dsk> boot mini <mzf>
mzdsk-fsmz [options] <image.dsk> boot over <mzf>
```

### Bootstrap Type Classification

The `boot` command (without arguments) displays the bootstrap type:

| Type | Description |
|------|-------------|
| **Mini** | Blocks 1-14, FSMZ compatible (DINFO and directory untouched) |
| **Bottom** | Block >= 1, but extends past block 14 (FSMZ structures overwritten) |
| **Normal** | Bootstrap in FAREA file area |
| **Over FSMZ** | Bootstrap beyond file area boundary |

### --no-fsmz-compat Flag

By default, `boot bottom` and `boot mini` on a full FSMZ disk limit the
bootstrap to blocks 1-14 (preserving DINFO on block 15 and directory from
block 16). With `--no-fsmz-compat`, the limit is raised to the entire disk
minus the IPLPRO block, but FSMZ structures will be overwritten and the disk
will no longer function as FSMZ.

On non-FSMZ disks (CP/M, MRS), `--no-fsmz-compat` has no effect - the limit
is always 15 blocks (boot track minus IPLPRO).

### Updating the Bootstrap Header

The `boot` command with any of the options `--fstrt`, `--fexec`, `--ftype`,
`--name` updates the corresponding field in the IPLPRO block (name, start
address, exec address, type) and then displays the current bootstrap state.
Bootstrap data itself is not changed - this is a metadata-only header
update, analogous to the `set` command for directory files.

| Option | Description |
|--------|-------------|
| `--fstrt HEX` | New Z80 start (load) address (0x0000-0xFFFF) |
| `--fexec HEX` | New Z80 exec address (0x0000-0xFFFF) |
| `--ftype HEX` | New bootstrap type (0x01-0xFF; 0x00 is rejected) |
| `--name NAME` | New bootstrap name (max 12 ASCII characters) |

At least one option must be supplied; otherwise the command only shows info.

Updates only work on a disk that already contains a valid IPLPRO bootstrap
(ftype `0x03` with the magic string `"IPLPRO"`). If the disk has no
bootstrap (e.g. after `mzdsk-create --format-basic` without a subsequent
`boot put`, or after `boot clear`), the command fails with an error and
does not modify the disk:

```
Error: No valid IPLPRO header on disk - cannot edit bootstrap fields.
       Install a bootstrap first (e.g. 'boot put <mzf>').
```

To install a new bootstrap, use `boot put <mzf>` (or `boot bottom` /
`boot over`).

### Example

Display boot sector information:

```bash
mzdsk-fsmz mydisk.dsk boot
```

Install a bottom bootstrap:

```bash
mzdsk-fsmz mydisk.dsk boot bottom bootstrap.mzf
```

Install a bottom bootstrap overwriting FSMZ structures:

```bash
mzdsk-fsmz mydisk.dsk boot bottom --no-fsmz-compat bootstrap.mzf
```

Update the bootstrap name and start address:

```bash
mzdsk-fsmz mydisk.dsk boot --name MYBOOT --fstrt 1200
```

Install a normal bootstrap from an MZF file:

```bash
mzdsk-fsmz mydisk.dsk boot put bootstrap.mzf
```

Extract the current bootstrap to an MZF file:

```bash
mzdsk-fsmz mydisk.dsk boot get bootstrap.mzf
```

Clear the bootstrap:

```bash
mzdsk-fsmz mydisk.dsk boot clear
```

---

## format - Format Filesystem

Initializes the FSMZ filesystem on an existing DSK image. Creates the IPLPRO
block, DINFO block with allocation bitmap, and empty directory. Existing data
on the disk is overwritten.

```
mzdsk-fsmz [options] <image.dsk> format
```

### Example

```bash
mzdsk-fsmz mydisk.dsk format
```

---

## repair - Filesystem Repair

Fully reinitializes the DINFO block:

- `farea` is reset to the default value (`FSMZ_DEFAULT_FAREA_BLOCK`),
- `blocks` (total block count) is derived from the DSK image geometry,
- the allocation bitmap and the `used` counter are rebuilt from the
  current directory contents and the bootstrap, if any.

After `repair` the DINFO is consistent with the DSK geometry even when
the whole DINFO block had been overwritten with random data.

```
mzdsk-fsmz [options] <image.dsk> repair
```

### Example

```bash
mzdsk-fsmz mydisk.dsk repair
```

---

## defrag - Defragment File Area

Defragments the file area by moving files so they occupy contiguous blocks
starting from the beginning of the file area. This maximizes the largest
available contiguous free space for new files.

```
mzdsk-fsmz [options] <image.dsk> defrag
```

### Example

```bash
mzdsk-fsmz mydisk.dsk defrag
```

---

## dump-block - Hexdump FSMZ Block

Displays hexdump of an FSMZ allocation block (256 B). Data is
automatically de-inverted. When size larger than one block is given,
multiple consecutive blocks are read.

```
mzdsk-fsmz [options] <image.dsk> dump-block <block> [bytes] [--cnv] [--dump-charset MODE]
```

The `--cnv` option is an alias for `--dump-charset eu`. The `--dump-charset MODE`
option allows selecting a specific conversion mode for the hexdump ASCII column.
Without these options, standard ASCII is shown.

### Examples

```bash
# Hexdump of block 16 (first directory block)
mzdsk-fsmz mydisk.dsk dump-block 16

# With Sharp MZ ASCII conversion (filenames become readable)
mzdsk-fsmz mydisk.dsk dump-block 16 --cnv

# Hexdump of 512 B (2 blocks) starting at block 16
mzdsk-fsmz mydisk.dsk dump-block 16 512 --cnv
```

---

## get-block - Extract FSMZ Block

Reads an FSMZ allocation block (or series of blocks) and saves
data to a local file. Data is automatically de-inverted.

```
mzdsk-fsmz [options] <image.dsk> get-block <block> <file> [bytes] [--noinv]
```

The `--noinv` option disables automatic de-inversion - data is saved
in the inverted state as it is on disk.

### Examples

```bash
mzdsk-fsmz mydisk.dsk get-block 0 iplpro.bin
mzdsk-fsmz mydisk.dsk get-block 15 dinfo.bin
mzdsk-fsmz mydisk.dsk get-block 16 dir.bin 2048
```

---

## put-block - Write to FSMZ Block

Writes data from a local file into an FSMZ allocation block (or
series of blocks). Data is automatically inverted on write.

```
mzdsk-fsmz [options] <image.dsk> put-block <block> <file> [bytes] [offset] [--noinv]
```

The `--noinv` option disables automatic inversion - data is written
directly as-is from the file.

### Examples

```bash
mzdsk-fsmz mydisk.dsk put-block 0 custom_iplpro.bin
mzdsk-fsmz mydisk.dsk put-block 15 fixed_dinfo.bin
```

---

## Examples

Full workflow - create, format, populate, and inspect a disk:

```bash
mzdsk-create --format-basic blank.dsk
mzdsk-fsmz blank.dsk put loader.mzf
mzdsk-fsmz blank.dsk put game.mzf
mzdsk-fsmz blank.dsk dir
```

Open a disk in read-only mode and list contents:

```bash
mzdsk-fsmz --ro mydisk.dsk dir
```

Extract all files from a disk:

```bash
mzdsk-fsmz mydisk.dsk get --all export/
```

Extract individual files:

```bash
mzdsk-fsmz mydisk.dsk get GAME game.mzf
mzdsk-fsmz mydisk.dsk get --id 0 game.mzf
```

Work with an IPLDISK-formatted disk:

```bash
mzdsk-fsmz --ipldisk mydisk.dsk dir
mzdsk-fsmz --ipldisk mydisk.dsk get "PROGRAM" output.mzf
```

Lock and unlock files:

```bash
mzdsk-fsmz mydisk.dsk lock GAME 1
mzdsk-fsmz mydisk.dsk lock GAME 0
```

Bootstrap management:

```bash
mzdsk-fsmz mydisk.dsk boot
mzdsk-fsmz mydisk.dsk boot put bootstrap.mzf
mzdsk-fsmz mydisk.dsk boot get bootstrap.mzf
mzdsk-fsmz mydisk.dsk boot clear
```

Hexdump of directory block with Sharp MZ ASCII conversion:

```bash
mzdsk-fsmz mydisk.dsk dump-block 16 --cnv
mzdsk-fsmz mydisk.dsk dump-block 16 512 --cnv
```

Extract and write raw FSMZ blocks:

```bash
mzdsk-fsmz mydisk.dsk get-block 0 iplpro.bin
mzdsk-fsmz mydisk.dsk get-block 15 dinfo.bin
mzdsk-fsmz mydisk.dsk put-block 0 custom_iplpro.bin
```

Repair and defragment a disk:

```bash
mzdsk-fsmz mydisk.dsk repair
mzdsk-fsmz mydisk.dsk defrag
```

---

## Non-sequential Sectors

Block operations (get-block, put-block) traverse sectors sequentially by
sector ID (like an FDC controller: ID 1, 2, 3, ...). Sectors with
non-sequential IDs (e.g. LEMMINGS disk with sectors 1-9 + 22 on a single
track) are not accessible through block commands.

To access these sectors, use `mzdsk-raw` with the `--order phys` option,
which traverses sectors by their physical position in the DSK image:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 10 --order phys
mzdsk-raw disk.dsk get data.bin --track 0 --sector 1 --sectors 10 --order phys
```
