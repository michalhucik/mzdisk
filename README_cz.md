<div align="right">
<a href="README.md">🇬🇧 English</a> | <b>🇨🇿 Čeština</b>
</div>

<p align="center">
  <img src="screenshot/mzdisk-logo.png" alt="mzdisk logo" width="320">
</p>

<h1 align="center">mzdisk</h1>

<p align="center">
  <b>Nástroje a GUI pro správu diskových obrazů počítačů Sharp MZ</b><br>
  <sub>MZ-700 &middot; MZ-800 &middot; MZ-1500</sub>
</p>

<p align="center">
  <a href="docs/cz/Changelog.md"><img src="https://img.shields.io/badge/GUI%20ver.-0.8.0-brightgreen" alt="GUI 0.8.0"></a>
  <a href="docs/cz/Changelog.md"><img src="https://img.shields.io/badge/CLI%20ver.-0.8.0-brightgreen" alt="CLI 0.8.0"></a>
  <img src="https://img.shields.io/badge/licence-GPLv3-blue" alt="GPLv3">
  <img src="https://img.shields.io/badge/platformy-Linux%20%7C%20Windows-lightgrey" alt="platformy">
  <img src="https://img.shields.io/badge/Sharp%20MZ-700%20%7C%20800%20%7C%201500-orange" alt="Sharp MZ">
  <img src="https://img.shields.io/badge/jazyk-C11%20%2F%20C%2B%2B17-purple" alt="C11 / C++17">
</p>

---

## Co je mzdisk

**mzdisk** je sada nástrojů pro práci s diskovými obrazy (DSK) počítačů Sharp MZ.
Projekt poskytuje:

- **GUI aplikaci** (`mzdisk`) pro pohodlnou interaktivní správu disků,
- **sadu sedmi CLI nástrojů** (`mzdsk-*`) pro skriptování a dávkové operace,
- **sdílené C knihovny** pro integraci do vlastních projektů.

Podporované souborové systémy:

- **FSMZ** - původní MZ-BASIC formát s invertovanými daty a IPLPRO bootstrapem,
- **CP/M 2.x** - presety SD Jiří Lamač - LEC (1988, 89), HD LuckySoft (1993), SD2S, HD2S i vlastní DPB,
- **MRS** - FAT-based systém Vlastimila Veselého - TRIPS (1993).

> **Historická poznámka**
>
> mzdisk je pokračovatel a plný nástupce mého staršího projektu **FSTOOLS**,
> který byl více než deset let nevyvíjený. Pokud jste dosud používali FSTOOLS,
> mzdisk jej ve všech ohledech nahrazuje - přináší podporu dalších filesystemů,
> grafické rozhraní i moderní kódovou základnu.

---

## GUI ve zkratce

<table>
<tr>
<td width="50%"><img src="screenshot/mzdisk-dir-mzfs.png" alt="FSMZ directory"></td>
<td width="50%"><img src="screenshot/mzdisk-dir-map-cpm.png" alt="CP/M directory + alloc map"></td>
</tr>
<tr>
<td align="center"><sub>FSMZ directory s konverzí znakové sady</sub></td>
<td align="center"><sub>CP/M - directory, DPB a block map</sub></td>
</tr>
<tr>
<td width="50%"><img src="screenshot/mzdisk-dir-fatmap-mrs.png" alt="MRS directory + FAT map"></td>
<td width="50%"><img src="screenshot/mzdisk-hexview-hexedit.png" alt="Hexdump s CG-ROM glyfy"></td>
</tr>
<tr>
<td align="center"><sub>MRS directory a FAT mapa</sub></td>
<td align="center"><sub>Hex editor s auto-inverzí a CG-ROM glyfy</sub></td>
</tr>
</table>

---

## Hlavní funkce

### Podporované formáty a disky
- DSK kontejner: Extended CPC DSK formát
- Filesystemy: FSMZ, CP/M 2.x (SD/HD/SD2S/HD2S + custom DPB), MRS
- Automatická detekce FS z geometrie a obsahu
- Automatická detekce a zpracování inverze dat (FSMZ, MRS)

### Práce se soubory
- Procházení obsahu všech podporovaných FS
- Import a export souborů, MZF kontejnery
- Mazání, přejmenování, uzamčení (attributy u CP/M)
- Drag &amp; drop souborů **mezi okny** (včetně cross-FS konverze přes MZF)
- Formátování, defragmentace a repair nástroje

### Nízkoúrovňové operace
- Editace DSK kontejneru (tracks, sector IDs, gap, filler, FDC status)
- Úpravy geometrie disku, úpravy bootstrapu (IPLPRO)
- Surový přístup k sektorům a blokům
- Hex editor s volitelnou inverzí a šesti režimy zobrazení znaků
  (Raw, Sharp MZ EU/JP ASCII, UTF-8 EU/JP, CG-ROM glyfy)

### Sharp MZ znakové sady
- Konverze mezi Sharp MZ ASCII a UTF-8 (varianty EU a JP)
- Zobrazení pixel-art glyfů z CG-ROM (font `mzglyphs.ttf`,
  čtyři znakové sady EU1/EU2/JP1/JP2)
- Editace textu v Sharp MZ kódování přímo v hex editoru

### GUI specifika
- Multi-window: až **16 disků otevřených současně** v jedné instanci
- Každé okno má vlastní menubar, toolbar a stavový řádek
- Samostatné OS okno pro každou session (multi-viewport)
- Nezávislé Open/Save/Close operace na úrovni okna

---

## GUI aplikace mzdisk

Hlavní okno Info tab otevřeného disku a dialog pro vytvoření nového disku:

<p align="center">
  <img src="screenshot/mzdisk-info.png" alt="Info panel" width="48%">
  &nbsp;
  <img src="screenshot/mzdisk-new-disk.png" alt="Nový disk" width="48%">
</p>

Editace geometrie a bootstrap sektoru:

<p align="center">
  <img src="screenshot/mzdisk-geometry-edit.png" alt="Editor geometrie" width="48%">
  &nbsp;
  <img src="screenshot/mzdisk-bootstrap.png" alt="Bootstrap editor" width="48%">
</p>

Vizualizace obsazení disku (CP/M block map):

<p align="center">
  <img src="screenshot/mzdisk-blockmap-cpm.png" alt="CP/M block map" width="80%">
</p>

Další screenshoty najdete v adresáři [screenshot/](screenshot/).

### Technologie GUI
- **SDL3** pro okna a vstup
- **Dear ImGui** s multi-viewport a docking větví
- **OpenGL** backend
- Plně lokalizovatelné rozhraní (locales)

---

## CLI nástroje

Sedm specializovaných binárek, každá zaměřená na jednu vrstvu
nebo jeden filesystem. Všechny jsou součástí portable distribuce.

| Nástroj | Popis | Dokumentace |
|---|---|---|
| `mzdsk-info` | Read-only inspekce DSK - geometrie, mapa, boot sektor, hexdump | [docs/cz/tools-mzdsk-info.md](docs/cz/tools-mzdsk-info.md) |
| `mzdsk-create` | Tvorba nových DSK obrazů (presety basic / cpm-sd / cpm-hd / mrs / lemmings + custom geometrie) | [docs/cz/tools-mzdsk-create.md](docs/cz/tools-mzdsk-create.md) |
| `mzdsk-dsk` | Diagnostika a editace DSK kontejneru (info, tracks, check, repair, edit-header, edit-track) | [docs/cz/tools-mzdsk-dsk.md](docs/cz/tools-mzdsk-dsk.md) |
| `mzdsk-fsmz` | Kompletní správa FSMZ (dir, get/put, bootstrap, format, repair, defrag) | [docs/cz/tools-mzdsk-fsmz.md](docs/cz/tools-mzdsk-fsmz.md) |
| `mzdsk-cpm` | Kompletní správa CP/M (dir, get/put/era/ren, attr, map, dpb, format) | [docs/cz/tools-mzdsk-cpm.md](docs/cz/tools-mzdsk-cpm.md) |
| `mzdsk-mrs` | Operace s MRS (info, dir, fat, init, defrag) | [docs/cz/tools-mzdsk-mrs.md](docs/cz/tools-mzdsk-mrs.md) |
| `mzdsk-raw` | Surový přístup k sektorům a blokům, modifikace geometrie | [docs/cz/tools-mzdsk-raw.md](docs/cz/tools-mzdsk-raw.md) |

Podporované volby napříč nástroji:

- `--format json|csv` - strojově čitelný výstup (pro skriptování)
- `--charset eu|jp|utf8-eu|utf8-jp` - konverze jmen souborů
- `--dump-charset raw|eu|jp|utf8-eu|utf8-jp` - znakový režim v hexdumpu

---

## Rychlý start

```bash
# Vytvoření nového CP/M SD disku
mzdsk-create --preset cpm-sd moje.dsk

# Inspekce obsahu s auto-detekcí FS
mzdsk-info moje.dsk --map

# Import MZF souboru do FSMZ disku
mzdsk-fsmz basic.dsk put program.mzf

# Spuštění GUI (s volitelným vstupním souborem)
mzdisk basic.dsk
```

---

## Instalace a build

### Binárky (Windows)

Předkompilované portable ZIP archivy pro Windows jsou k dispozici
v [GitHub Releases](https://github.com/michalhucik/mzdisk/releases).

Portable distribuce obsahuje dvě samostatné složky:

- `mzdisk/` - GUI aplikace + všechna potřebná DLL a UI resources
- `mzdisk-cli/` - CLI nástroje + dokumentace (`.md`, volitelně i `.html`)

### Build ze zdrojových kódů

Předpoklady:

- CMake 3.16+
- C kompilátor s podporou C11 (GCC 10+ nebo Clang 12+)
- Pro GUI navíc: SDL3 a OpenGL
- Linux nebo Windows (MSYS2 / MinGW-w64)

```bash
# Sdílené knihovny
make libs

# CLI nástroje
make cli

# GUI aplikace
make gui

# Portable distribuce (GUI + CLI, jen Markdown dokumentace)
make portable

# Portable distribuce včetně HTML dokumentace
# (vyžaduje python3 a Python modul `markdown`)
make portable-full

# Samostatná HTML dokumentace (volitelně, generovaná z docs/*/*.md)
make docs-html

# Testy (488 testů - knihovny, GUI logika, CLI integrační)
make test
```

> **Poznámka:** Generování HTML dokumentace je volitelný krok. `make portable`
> a `make portable-cli` fungují i bez Pythonu a HTML soubory se do balíku
> přidají, pouze pokud už adresář `docs-html/` existuje (typicky po
> `make docs-html` nebo `make portable-full`).

Build výstupy:

- `build-libs/` - statické knihovny `.a`
- `build-cli/` - sedm CLI binárek
- `build-gui/mzdisk` (Linux) nebo `build-gui/mzdisk.exe` (Windows)
- `build-tests/` - testovací binárky
- `portable/` - distribuční složky

---

## Dokumentace

- [Dokumentace CLI nástrojů (cz)](docs/cz/) - `tools-mzdsk-*.md`
- [Dokumentace CLI nástrojů (en)](docs/en/)
- [Changelog (cz)](docs/cz/Changelog.md) &middot; [Changelog (en)](docs/en/Changelog.md)

---

## Licence a upozornění

Projekt je distribuován pod licencí **GPLv3** bez jakékoliv záruky.

> ⚠️ **Vždy pracujte s kopií DSK obrazu, ne s originálem.**
> Autor projektu neposkytuje žádné záruky a neodpovídá za ztráty
> ani poškození dat způsobené používáním nástrojů projektu mzdisk.

---

## Autor

**Michal Hučík** - [github.com/michalhucik](https://github.com/michalhucik)

Poděkování patří všem autorům historických filesystem specifikací
počítačů Sharp MZ a komunitě retro nadšenců, kteří tyto stroje
udržují při životě.
