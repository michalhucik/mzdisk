# Changelog

## 2026-11-10

### FSMZ file type fixes, macOS build and CI

Bugfix release.

- **`mzdsk-fsmz chtype`** rejects type `00`. It used to be accepted
  and marked the entry as deleted: the file disappeared from the
  directory while its blocks stayed allocated. The valid range is
  `01`-`FF`, same as for `set --ftype`.
- **GUI, FSMZ directory:** file type `05` was shown as "BRD" (same as
  `04`); the correct mnemonic is "RB".
- **CLI documentation:** fixed the file type mnemonics and the `dir`
  output example for `mzdsk-fsmz`, the sector ID in the Lemmings
  example for `mzdsk-raw` and the `--format-basic` example; added the
  `--overwrite` and `--yes` / `-y` options for `mzdsk-fsmz`,
  `mzdsk-cpm` and `mzdsk-mrs`. The README said `--format json|csv`
  instead of the correct `--output json|csv`.
- **macOS:** the libraries, CLI tools and tests now build on macOS.
  The GUI is not built there yet.
- **CI:** every push to `main` on GitHub builds the project and runs
  the tests on Linux, macOS and Windows (GitHub Actions).
- The `sharpmz_ascii` library was upgraded to 2.2.1 (new KOI8-CS and
  display code character sets; mzdisk does not use them yet, tool
  behaviour is unchanged).

**Release:**

- **mzdisk GUI v0.8.2**
- **mzdisk CLI v0.8.2**

**Versions changed in this release:**

| Component             | Version          |
|-----------------------|------------------|
| `mzdsk-fsmz`          | 1.14.2 -> 1.14.3 |
| `sharpmz_ascii` (lib) | 2.1.1 -> 2.2.1   |

Other CLI tools and libraries unchanged.

## 2026-05-27

### Fix reading MRS disks with original layout (Veselý 1993)

Bugfix release - fixes reading of disks formatted by the original
MRS driver (typically 720 KB DSK images with 54+ files). Before the
fix, the directory listing contained bogus entries assembled from
FAT bytes, and files with data blocks >= 1024 were unreadable
("Format error" in the GUI on export).

**Release:**

- **mzdisk GUI v0.8.1**
- **mzdisk CLI v0.8.1**

**Versions changed in this release:**

| Component          | Version          |
|--------------------|------------------|
| `mzdsk-mrs`        | 2.12.0 -> 2.12.1 |
| `mzdsk_mrs` (lib)  | 2.5.0 -> 2.6.0   |

Other CLI tools and libraries unchanged.

## 2026-04-22

### Public release on GitHub

First public release of the **mzdisk** project - a set of tools
and a GUI application for managing disk images of Sharp MZ-700,
Sharp MZ-800 and Sharp MZ-1500 computers. The project is the
successor of the older **FSTOOLS** and fully replaces it.

**Release:**

- **mzdisk GUI v0.8.0**
- **mzdisk CLI v0.8.0**

**Versions of the individual CLI tools in this release:**

| Tool           | Version |
|----------------|---------|
| `mzdsk-info`   | 1.4.6   |
| `mzdsk-create` | 1.2.5   |
| `mzdsk-dsk`    | 1.5.7   |
| `mzdsk-fsmz`   | 1.14.2  |
| `mzdsk-cpm`    | 1.19.1  |
| `mzdsk-mrs`    | 2.12.0  |
| `mzdsk-raw`    | 2.2.10  |
