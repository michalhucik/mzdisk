# mzdsk-mrs - MRS Filesystem Tool

Tool version: 2.12.0. Part of the [mzdisk](https://github.com/michalhucik/mzdisk/) project.

> **Disclaimer:** Always work on a copy of your DSK image, never on the
> original. The author provides no warranty and accepts no liability for
> data loss or damage resulting from use of the mzdisk project tools.
> Distributed under the GPLv3 license, without warranty.

Tool for complete management of the MRS (Memory Resident System)
filesystem on Sharp MZ-800 floppy disks. MRS is a development
environment by Vlastimil Veselý (1993) containing an editor,
assembler and linker for Z80 program development.

MRS uses 720 KB disks with 2 sides, 80 tracks, and 9 sectors of 512 B
per track. All on-disk data is bit-inverted (XOR 0xFF). The filesystem
has its own FAT (1 byte per allocation block) and a 6-sector directory
(96 entries of 32 B each).

**Atomic writes:** Write operations (`put`, `era`, `ren`, `set-addr`,
`init`, `defrag`, `put-block`) are atomic - the entire DSK image is loaded
into memory,
operations run in RAM, and the result is flushed back in a single write only
after successful completion. If an operation fails midway, the original file
is left unchanged. Keeping backups for critical data is still recommended -
completed changes are irreversible.

## Usage

```
mzdsk-mrs [options] <image.dsk> <command> [arguments...]
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<image.dsk>` | Input/output DSK disk image (Extended CPC DSK format) |

## Commands

| Command | Description |
|---------|-------------|
| `info` | Show MRS filesystem information |
| `dir` | List directory (active files only) |
| `dir --raw` | List all directory slots (including empty and padding) |
| `fat` | Show FAT table contents (non-zero entries only) |
| `get <name[.ext]> <file>` | Extract file from disk (raw data) |
| `get --id N <file>` | Extract file by number (raw data) |
| `get --mzf <name[.ext]> <file>` | Extract file as MZF |
| `get --mzf --id N <file>` | Extract by number as MZF |
| `get --all <dir>` | Export all files to directory (raw) |
| `get --all --mzf <dir>` | Export all files to directory (MZF) |
| `put <file> <name.ext> [options]` | Store raw file to disk |
| `put <file> --name NAME.EXT [options]` | Store raw file with strict name validation |
| `put --mzf <file> [name.ext]` | Store MZF file to disk |
| `put --mzf <file> --name NAME.EXT` | Store MZF with strict name override |
| `era <name[.ext]>` | Erase file by name |
| `era --id N` | Erase file by number |
| `ren <name[.ext]> <new[.ext]>` | Rename file |
| `ren --id N <new[.ext]>` | Rename file by number |
| `set-addr <name[.ext]> [--fstrt HEX] [--fexec HEX]` | Update STRT/EXEC addresses of existing file |
| `set-addr --id N [--fstrt HEX] [--fexec HEX]` | Update addresses by file number |
| `file <name[.ext]>` | Show file details (metadata + block list) |
| `file --id N` | Show file details by number |
| `dump-block N [size] [--noinv] [--dump-charset MODE]` | Hexdump of MRS block(s) |
| `get-block N <file> [size] [--noinv]` | Extract MRS block(s) to file |
| `put-block N <file> [size] [offset] [--noinv]` | Write file into MRS block(s) |
| `init [--force]` | Create empty MRS filesystem (FAT + directory) |
| `defrag` | Defragment filesystem |

## Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--fat-block N` | number | 36 | Starting block of the FAT |
| `--ro` | - | - | Force read-only mode |
| `--output FMT` | text\|json\|csv | text | Output format (text, json, csv) |
| `-o FMT` | text\|json\|csv | text | Short for --output |
| `--version` | - | - | Show program version |
| `--lib-versions` | - | - | Show library versions |
| `--help`, `-h` | - | - | Show usage help |

The `--dump-charset MODE` option is only available on the `dump-block`
command (see the `dump-block` section).

### Option details

**--fat-block N** - The MRS driver uses a hardcoded FAT location which
for standard 720 KB disks starts at allocation block 36 (equivalent
to track 4, sector 1). For non-standard layouts this can be overridden.

**--ro** - Forces read-only mode. Commands `put`, `era`, `ren`,
`set-addr`, `init`, `defrag` and `put-block` will fail with an error
because they require write access.

---

## Filenames

MRS filenames have the format **8 characters + 3 character extension**,
are **case-sensitive**, and are space-padded (0x20) to full length on disk.

On the command line they are specified as `name.ext`:
- `util+.DAT` - matches both filename "util+" and extension "DAT" exactly
- `util+` - matches filename "util+" only (any extension)
- `nu2.MRS` - source text "nu2" with extension MRS

MRS file extensions:
- **MRS** - Z80 assembler source text (editor)
- **DAT** - binary data or machine code
- **RAM** - RAMdisk page
- **SCR** - screen dump

---

## info - Filesystem information

Shows the layout of FAT and directory, total and free capacity, and
the number of used files. All values are derived from the FAT table
contents.

```
mzdsk-mrs disk.dsk info
```

### Example output

```
MRS Filesystem info:

  Layout:
    FAT block:      36
    FAT sectors:    2
    DIR block:      38
    DIR sectors:    7
    Data block:     45

  Capacity:
    Total blocks:   1404 (702 KB)
    Free blocks:    1262 (631 KB)
    Used blocks:    142 (71 KB)
      FAT:            2
      Directory:      7
      Bad:            0
      Files (FAT):    133
      Files (dir):    133

  Directory:
    Total slots:    112
    Usable slots:   105
    Used files:     14
```

### Used blocks breakdown and inconsistency detection

`Used blocks` (= `total - free`) includes both system blocks (FAT,
Directory) and file blocks. The tool therefore shows a detailed
breakdown under the aggregate value:

- **FAT** - number of blocks marked in the FAT with value `0xFA`.
- **Directory** - number of blocks marked `0xFD` (directory sectors).
- **Bad** - number of blocks marked `0xFE` (bad blocks).
- **Files (FAT)** - number of blocks whose FAT entry is a `file_id`
  in the range 1..249 (sum of all blocks that the FAT assigns to files).
- **Files (dir)** - sum of the `bsize` field over all active directory
  entries (as seen in `dir`).

Normally `Files (FAT) == Files (dir)`. If the two values differ
(typically after non-standard filesystem manipulation, or on historical
disks where `bsize` does not exactly reflect the FAT state), the tool
appends a warning:

```
(!) FAT file blocks differ from dir bsize sum by +32 - run `defrag` to reconcile.
```

The `defrag` operation rewrites the files sequentially, the FAT chain
is rebuilt from the actually read data and `bsize` in the directory is
brought in line with the FAT. After `defrag` the `info` output shows
consistent numbers and the warning goes away.

### JSON / CSV output

In `--output json` / `--output csv` mode the breakdown is exposed via
keys `fat_blocks`, `dir_blocks`, `bad_blocks`, `file_blocks` and
`dir_bsize_sum` - useful for scripted disk-integrity checks.

### Layout note

The tool automatically detects FAT and directory size from markers
in the FAT table (0xFA = FAT sector, 0xFD = directory sector). For
disks initialized by the original MRS driver this detection yields
2 FAT sectors + 7 DIR sectors, which differs from the actual MRS
convention of 3 FAT sectors + 6 DIR sectors (see
`devdocs/MZ_MRS_poznamky.txt`). The difference is cosmetic - file
reading works correctly in both cases.

---

## dir - Directory listing

Lists active files on the MRS disk. For each file it shows file
number, name, extension, start address (fstrt), exec address (fexec),
number of allocated blocks, and size in bytes.

```
mzdsk-mrs disk.dsk dir
mzdsk-mrs disk.dsk dir --raw
```

The `--raw` option also shows empty and deleted slots and padding.

### Example output

```
MRS Directory:

  ID  Name       Ext  fstrt   fexec   Blocks    Size
  --  ---------  ---  ------  ------  ------  ---------
   1  util+     DAT  0x1f00  0x1f00       9     4608 B
   2  nu2       MRS  0xa4fa  0x0000      24    12288 B
   3  sada      DAT  0x5000  0x0000       4     2048 B
  ...

  14 entries, 1262 free blocks (631 KB free)
```

### Columns

- **ID** - file_id (directory slot number, 1-89)
- **Name** - filename (8 characters, space-padded)
- **Ext** - extension: `MRS` (source text), `DAT` (data), `RAM`
  (RAMdisk page), `SCR` (screen dump from MZpaint)
- **fstrt** - start address in RAM. For MRS files has the form
  0xHHFA where HH = 0xD4 - 2 x blocks (position in editor text buffer).
  For DAT files it is the actual load address.
- **fexec** - exec address (for the `run` command). 0x0000 for source texts.
- **Blocks** - number of allocated 512 B blocks
- **Size** - size in bytes (Blocks x 512)

---

## fat - FAT table contents

Shows a hex dump of the FAT table, but only rows containing at least
one non-zero value. Useful for quickly inspecting disk usage.

```
mzdsk-mrs disk.dsk fat
```

### Example output

```
MRS FAT table (non-zero entries):

  Offset   00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F
  ------   -----------------------------------------------
     0:   ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff
    16:   ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff
    32:   ff ff ff ff fa fa fd fd  fd fd fd fd fd 01 01 01
    48:   01 01 01 01 01 01 02 02  02 02 02 02 02 02 02 02
  ...

  Legend: 00=free, FA=FAT, FD=DIR, FE=bad, FF=system, other=file_id
```

### FAT value meanings

| Value | Meaning |
|-------|---------|
| `0x00` | Free block |
| `0x01-0x59` | File number (file_id from directory) |
| `0xFA` | Block belongs to FAT table |
| `0xFD` | Block belongs to directory |
| `0xFE` | Bad sector |
| `0xFF` | System/reserved block (boot area) |

---

## get - Extract file (raw)

Extracts a file from the MRS disk to a local file. Data is read from
all blocks identified by the file_id value in the FAT table, in
ascending block order, and automatically de-inverted.

```
mzdsk-mrs disk.dsk get <name[.ext]> <output_file>
mzdsk-mrs disk.dsk get --id N <output_file>
```

The output file contains raw data - blocks x 512 B without any header.
MRS does not store exact byte-level file size, so the output is always
aligned to whole blocks.

### Example

```bash
# By name and extension
mzdsk-mrs disk.dsk get util+.DAT util_plus.bin

# By name only (finds first match with any extension)
mzdsk-mrs disk.dsk get util+ util_plus.bin

# By file_id
mzdsk-mrs disk.dsk get --id 1 util_plus.bin
```

---

## get --mzf - Extract file as MZF

Extracts a file from the MRS disk in MZF format (128 B header + data).
The MZF header contains metadata from the directory entry:
- ftype = 0x01 (OBJ)
- filename = "fname.ext" from MRS directory
- fsize = bsize x 512
- fstrt, fexec = from directory entry

```
mzdsk-mrs disk.dsk get --mzf <name[.ext]> <output.mzf>
mzdsk-mrs disk.dsk get --mzf --id N <output.mzf>
```

MZF format preserves metadata, so re-importing via `put --mzf`
automatically restores fstrt and fexec.

### Example

```bash
mzdsk-mrs disk.dsk get --mzf util+.DAT util_plus.mzf
mzdsk-mrs disk.dsk get --mzf --id 1 output.mzf
```

### Limitation

MZF fsize is uint16_t (max 65535 B). Files larger than 65535 B
cannot be exported to MZF format.

---

## get --all - Export all files

Exports all files from the MRS disk into a directory on the host
filesystem. Each file is saved as a separate file. Filenames are
sanitized for the host OS (characters `* ? " < > | : \ /` are
replaced with `_`).

```
mzdsk-mrs disk.dsk get --all <dir>
mzdsk-mrs disk.dsk get --all --mzf <dir>
```

### Get --all options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--mzf` | - | - | Export as MZF (128 B header + data) |
| `--on-duplicate` | rename/overwrite/skip | rename | Duplicate file handling |
| `--ftype HH` | 0x00-0xFF | 0x01 | MZF file type (only with --mzf) |
| `--exec-addr N` | 0x0000-0xFFFF | from dir | Override exec address (only with --mzf) |
| `--strt-addr N` | 0x0000-0xFFFF | from dir | Override start address (only with --mzf) |

### Duplicate handling modes

- **rename** (default) - appends `~2`, `~3`, etc. before the extension
- **overwrite** - overwrites existing files without warning
- **skip** - skips existing files with a warning on stderr

### Examples

```bash
# Export all files as raw data
mzdsk-mrs disk.dsk get --all ./export

# Export all files as MZF
mzdsk-mrs disk.dsk get --all --mzf ./export-mzf

# Export as MZF with custom file type
mzdsk-mrs disk.dsk get --all --mzf --ftype 0x22 ./export

# Export with address overrides
mzdsk-mrs disk.dsk get --all --mzf --exec-addr 0x2000 --strt-addr 0x3000 ./export

# Skip existing files
mzdsk-mrs disk.dsk get --all --on-duplicate skip ./export
```

### Notes

- By default, MZF ftype is 0x01 (OBJ) and start/exec addresses are
  taken from the MRS directory entry. Use `--ftype`, `--exec-addr`
  and `--strt-addr` to override.
- Files larger than 65535 B are skipped in MZF mode (with a warning).
  Raw mode has no size limit.
- The output directory is created automatically if it does not exist.
- **Exit code:** 0 on success (including files skipped via
  `--on-duplicate skip`), 1 if at least one file could not be
  extracted due to an actual error (e.g. inconsistent FAT, I/O error).
  This lets scripts detect partial failures - the summary
  `Extracted N files (M errors)` still reports both counts.

---

## put - Store file (raw)

Stores a local file to the MRS disk. Data is aligned to whole blocks
(512 B), any remainder is zero-padded.

```
mzdsk-mrs disk.dsk put <input_file> <name.ext> [--fstrt ADDR] [--fexec ADDR]
```

### Put command options

| Option | Default | Description |
|--------|---------|-------------|
| `--fstrt ADDR` | 0x0000 | Start address (load address) |
| `--fexec ADDR` | 0x0000 | Exec address (execution address) |
| `--name NAME.EXT` | - | Strict name validation (no silent truncation) |

Addresses can be specified in decimal or hexadecimal (with 0x prefix).

### Example

```bash
# Store binary file with address settings
mzdsk-mrs disk.dsk put program.bin myfile.DAT --fstrt 0x2000 --fexec 0x2000

# Store without specifying addresses (both will be 0x0000)
mzdsk-mrs disk.dsk put source.bin text.MRS
```

### Notes

- Filename must be unique (MRS checks only the 8-character name part).
- If the requested name is longer than 8 characters (or the extension
  longer than 3), MRS truncates it and prints a warning on stderr
  `Warning: Name 'LONGNAMETEST.DAT' was truncated to 'LONGNAME.DAT'`.
  If the truncated name collides with an existing file, `put` fails
  with "File exists".
- The `--name NAME.EXT` option enables strict mode without silent
  truncation - see section below.
- Maximum number of files per disk is 88-89 (depends on MRS version).
- File size must fit within available free blocks.
- Blocks are allocated sequentially from the lowest free block in the
  data area.

### Strict name validation via `--name`

The `--name NAME.EXT` option on `put` and `put --mzf` enables strict
mode - no silent truncation, no normalization. If the name violates
MRS rules, the operation fails with an error.

Validation rules:
- name up to 8 characters,
- extension up to 3 characters,
- empty filename rejected,
- forbidden characters: `< > , ; : = ? * [ ] | /`, plus dot inside the
  name or extension, space, and control characters (0x00-0x1F, 0x7F).

Error messages on stderr (exit code 1):

```
Error: Name 'LONGNAMETEST.DAT' exceeds 8.3 limit (no truncation with --name)
Error: Name 'A?B.DAT' contains forbidden character '?' (no truncation with --name)
Error: Name '.DAT' has empty filename (no truncation with --name)
```

The `--name` option replaces the 2nd positional argument. Supplying
both forms simultaneously returns
`Error: --name conflicts with positional <name.ext>`.

---

## put --mzf - Store MZF file

Stores an MZF file to the MRS disk. Metadata (fstrt, fexec) is
automatically taken from the MZF header. The on-disk filename can
be specified on the command line or derived from the MZF header.

```
mzdsk-mrs disk.dsk put --mzf <input.mzf> [name.ext] [--charset eu|jp]
```

If no name is given, it is derived from the MZF header. If the MZF
name does not contain a dot, the extension defaults to "DAT".

### fname conversion - `--charset` switch

MZF files store their name in Sharp MZ ASCII. The `--charset` switch
selects the variant used for conversion to standard ASCII:

- `--charset eu` (default) - European variant (MZ-700/MZ-800)
- `--charset jp` - Japanese variant (MZ-1500)

If no usable MRS filename can be derived (e.g. the MZF fname starts
with a space or consists entirely of non-printable characters), the
CLI prints an informative error and suggests an explicit override via
the positional argument.

### Example

```bash
# Import with metadata from MZF header (name and addresses)
mzdsk-mrs disk.dsk put --mzf util_plus.mzf

# Import with name override
mzdsk-mrs disk.dsk put --mzf util_plus.mzf copy.DAT

# Japanese character set variant (MZ-1500)
mzdsk-mrs disk.dsk put --mzf japan.mzf --charset jp
```

---

## file - Detailed file information

Displays file metadata (filename, extension, file_id, start/exec
address, size) and a complete list of all blocks belonging to the
file from the FAT table, including track:sector address for each block.

```
mzdsk-mrs disk.dsk file <name[.ext]>
mzdsk-mrs disk.dsk file --id N
```

### Example

```bash
mzdsk-mrs disk.dsk file util+.DAT
mzdsk-mrs disk.dsk file --id 3
```

---

## dump-block - Hexdump MRS Block

Displays hexdump of an MRS block (512 B). Data is automatically
de-inverted. The `--noinv` option shows raw (inverted) data.

```
mzdsk-mrs disk.dsk dump-block <block> [size] [--noinv] [--dump-charset MODE]
```

### Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--noinv` | - | - | Show raw (inverted) data without de-inversion |
| `--dump-charset MODE` | raw\|eu\|jp\|utf8-eu\|utf8-jp | raw | Character set for hexdump ASCII column |

### The --dump-charset option

Selects the Sharp MZ character set conversion for the hexdump ASCII column.

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

### Examples

```bash
mzdsk-mrs disk.dsk dump-block 36           # FAT block
mzdsk-mrs disk.dsk dump-block 36 1024      # 2 blocks
mzdsk-mrs disk.dsk dump-block 36 --noinv   # raw data
mzdsk-mrs disk.dsk dump-block 45 --dump-charset utf8-eu
```

---

## get-block - Extract MRS Block

Extracts MRS block data to a local file. Data is automatically
de-inverted. The `--noinv` option saves raw (inverted) data.

```
mzdsk-mrs disk.dsk get-block <block> <output> [size] [--noinv]
```

### Examples

```bash
mzdsk-mrs disk.dsk get-block 36 fat.bin
mzdsk-mrs disk.dsk get-block 45 data.bin 4096
```

---

## put-block - Write to MRS Block

Writes data from a local file into an MRS block. Data is automatically
inverted before writing. The `--noinv` option writes data as-is.

```
mzdsk-mrs disk.dsk put-block <block> <input> [size] [offset] [--noinv]
```

### Examples

```bash
mzdsk-mrs disk.dsk put-block 45 data.bin
mzdsk-mrs disk.dsk put-block 45 data.bin 1024 512
```

---

## era - Erase file

Erases a file from the MRS disk. Frees blocks in the FAT and clears
the directory entry (filename is overwritten with spaces). Physical
block data remains on disk, but without a directory entry it becomes
inaccessible.

```
mzdsk-mrs disk.dsk era <name[.ext]>
mzdsk-mrs disk.dsk era --id N
```

### Example

```bash
mzdsk-mrs disk.dsk era util+.DAT
mzdsk-mrs disk.dsk era --id 1
```

---

## ren - Rename file

Renames a file on the MRS disk. Checks that the new name does not
already exist. If the new name includes an extension (dot), the
extension is also changed. Without an extension, only the filename
part changes, the extension remains unchanged.

```
mzdsk-mrs disk.dsk ren <name[.ext]> <new[.ext]>
mzdsk-mrs disk.dsk ren --id N <new[.ext]>
```

### Example

```bash
# Rename including extension
mzdsk-mrs disk.dsk ren util+.DAT loader.DAT

# Change name only, keep extension
mzdsk-mrs disk.dsk ren util+ loader

# By file_id
mzdsk-mrs disk.dsk ren --id 1 loader.DAT
```

---

## set-addr - Update STRT/EXEC addresses of existing file

Updates the start (`fstrt`) or exec (`fexec`) address in the MRS
directory entry. **Only metadata is modified; file data is untouched.**
Analogous to `ren`, but for address fields instead of the name.

At least one of `--fstrt` / `--fexec` must be supplied. Values are
hexadecimal in the range `0x0000`-`0xFFFF`.

```
mzdsk-mrs disk.dsk set-addr <name[.ext]> [--fstrt HEX] [--fexec HEX]
mzdsk-mrs disk.dsk set-addr --id N [--fstrt HEX] [--fexec HEX]
```

### Example

```bash
# Change both STRT and EXEC
mzdsk-mrs disk.dsk set-addr util+.DAT --fstrt 0x1234 --fexec 0xABCD

# EXEC only
mzdsk-mrs disk.dsk set-addr loram.DAT --fexec 0x1F00

# By file_id
mzdsk-mrs disk.dsk set-addr --id 1 --fstrt 0x2000 --fexec 0x276B
```

### Example output

```
  set-addr util+.DAT:  strt 0x1f00 -> 0x1234,  exec 0x1f00 -> 0xabcd
```

---

## init - Initialize empty filesystem

Creates a new empty FAT and directory on the disk. Does not touch
the system area (blocks 0-35, containing boot and MRS system) or
the data area beyond the directory.

```
mzdsk-mrs disk.dsk init [--force]
```

### The --force option

If the disk already contains a valid MRS filesystem (FAT with
a matching signature), the `init` command without `--force` will
fail with an error and display the number of existing files. This
prevents accidental overwriting of data. The `--force` option
skips this check and forces initialization.

### What init does exactly

- **Writes 3 FAT sectors** (blocks 36-38) with contents:
  - Blocks 0-35: `0xFF` (system area)
  - Blocks 36, 37: `0xFA` (FAT markers)
  - Blocks 38-44: `0xFD` (DIR markers, including the 3rd FAT sector)
  - Blocks 45-1439: `0x00` (free)

- **Writes 6 directory sectors** (blocks 39-44):
  - 89 empty slots with incrementing file_id (1-89)
  - Empty slots contain space-padded name and initialization pattern
    `0xCD490221` at offset 28-31
  - The last 7 slots are filled with `0x1A` (inaccessible)

- All data is inverted (XOR 0xFF) before writing.

### Warning

The `init` command **overwrites** the existing FAT and directory.
All information about files on the disk will be lost (the file data
itself remains, but without directory entries it will be inaccessible).

For safety, you can first use `--ro` to verify the disk state with
`info` / `dir`.

### Example

```bash
# Initialize an empty disk
mzdsk-mrs disk.dsk init

# Force re-initialization of a disk that already contains MRS filesystem
mzdsk-mrs disk.dsk init --force
```

### Example output

```
Initializing MRS filesystem at fat_block=36...
  FAT written:       3 sectors at blocks 36-38
  Directory written: 6 sectors at blocks 39-44
  89 empty directory slots, 7 inaccessible slots

MRS filesystem initialized successfully.
```

---

## defrag - Defragment filesystem

Reads all files from the disk into memory, formats the FAT and
directory, and writes everything back sequentially from the start
of the data area with no gaps. The result is contiguously stored
files without fragmentation.

```
mzdsk-mrs disk.dsk defrag
```

### What defrag does exactly

1. Initializes MRS configuration from FAT contents.
2. Reads all active files from the disk into memory (data + metadata).
3. Formats the FAT and directory (fsmrs_format_fs).
4. Re-initializes configuration from the freshly formatted disk.
5. Sequentially writes all files from the start of the data area.

After defragmentation:
- All files are stored contiguously from the data block.
- File order matches the original directory order.
- File metadata (name, extension, fstrt, fexec, bsize) is preserved.
- File IDs (file_id) may change (assigned sequentially).

### Warning

Defragmentation is a **destructive operation**. If an error occurs
mid-process (e.g., disk full, I/O error), data on the disk may become
inconsistent or lost. Always create a backup of the image before
defragmenting.

### Hidden 3rd FAT sector

On disks with the original MRS format, the 3rd FAT sector is marked
in the FAT as DIR (0xFD), even though it physically contains FAT data
for blocks 1024-1439. Defragmentation automatically reads this hidden
sector, so files with blocks in the 1024-1439 range are correctly
processed. After defragmentation, all files are relocated to the
45-1023 range (within the 2-sector FAT coverage).

### Example

```bash
# Defragment an MRS disk
mzdsk-mrs disk.dsk defrag
```

### Example output

```
Run defragmentation MRS

Defrag: reading files
  Read: util+.DAT (9 blocks)
  Read: DOS1.MRS (12 blocks)
  Read: PASIANS.DAT (50 blocks)
Defrag: formatting disk
Defrag: writing files
  Written: util+.DAT (9 blocks)
  Written: DOS1.MRS (12 blocks)
  Written: PASIANS.DAT (50 blocks)
Defrag: done (3 files defragmented)

Done.
```

---

## Examples

Basic exploration of an MRS disk:

```bash
mzdsk-mrs disk.dsk info
mzdsk-mrs disk.dsk dir
mzdsk-mrs disk.dsk fat
```

Extract a file and store it back:

```bash
mzdsk-mrs disk.dsk get util+.DAT util_plus.bin
mzdsk-mrs disk.dsk era util+.DAT
mzdsk-mrs disk.dsk put util_plus.bin util+.DAT --fstrt 0x1F00 --fexec 0x1F00
```

Rename a file:

```bash
mzdsk-mrs disk.dsk ren util+.DAT loader.DAT
```

Browse all directory slots including deleted ones:

```bash
mzdsk-mrs disk.dsk dir --raw
```

Safe preview (read-only):

```bash
mzdsk-mrs --ro disk.dsk dir
```

Initialize MRS filesystem (forced, if it already exists):

```bash
mzdsk-mrs disk.dsk init --force
```

## Limitations

- **720 KB disks only** with 1440 block layout (9 x 160).
- **FAT/DIR layout auto-detection** may differ from the original MRS
  driver by one sector (see `devdocs/MZ_MRS_poznamky.txt`).
- **MRS does not store byte-level file size** - only block count.
  Extracted files are always aligned to whole 512 B blocks.
- **FAT coverage**: On disks with 2 FAT sectors (older format) only
  blocks 0-1023 can be allocated (501 KB of data). Blocks 1024-1439
  are beyond FAT reach.

---

## Non-sequential Sectors

Block operations traverse sectors sequentially by sector ID (like an FDC
controller). Sectors with non-sequential IDs are not accessible through
block commands.

To access these sectors, use `mzdsk-raw` with the `--order phys` option:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 10 --order phys
```
