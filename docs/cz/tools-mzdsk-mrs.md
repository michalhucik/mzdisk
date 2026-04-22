# mzdsk-mrs - Práce s MRS souborovým systémem

Verze nástroje: 2.12.0. Součást projektu [mzdisk](https://github.com/michalhucik/mzdisk/).

> **Upozornění:** Vždy pracujte s kopií DSK obrazu, ne s originálem.
> Autor projektu neposkytuje žádné záruky a neodpovídá za ztráty ani
> poškození dat způsobené používáním nástrojů projektu mzdisk. Projekt
> je distribuován pod licencí GPLv3 bez záruky.

Nástroj pro kompletní správu MRS souborového systému (Memory Resident
System) na disketách Sharp MZ-800. MRS je vývojové prostředí autora
Vlastimila Veselého (1993) obsahující editor, assembler a linker pro
vývoj Z80 programů.

MRS používá 720K disketu se dvěma stranami, 80 stopami a 9 sektory
po 512 B na stopu. Data jsou na disku bitově invertována (XOR 0xFF).
Souborový systém má vlastní FAT (1 bajt na alokační blok) a adresář
o velikosti 6 sektorů (96 položek po 32 B).

**Atomicita zápisu:** Zápisové operace (`put`, `era`, `ren`, `set-addr`,
`init`, `defrag`, `put-block`) pracují atomicky - celý DSK obraz se načte do paměti,
operace se provedou v RAM a výsledek se zapíše zpět jediným voláním až
po úspěšném dokončení. Pokud operace selže uprostřed, původní soubor
zůstává nezměněn. I tak doporučujeme pro kritická data udržovat zálohy -
dokončené změny jsou nevratné.

## Použití

```
mzdsk-mrs [volby] <obraz.dsk> <příkaz> [argumenty...]
```

## Argumenty

| Argument | Popis |
|----------|-------|
| `<obraz.dsk>` | Vstupní/výstupní DSK diskový obraz (Extended CPC DSK) |

## Příkazy

| Příkaz | Popis |
|--------|-------|
| `info` | Zobrazit informace o MRS souborovém systému |
| `dir` | Vypsat adresář (aktivní soubory) |
| `dir --raw` | Vypsat všechny sloty adresáře (včetně prázdných a padding) |
| `fat` | Zobrazit obsah FAT tabulky (pouze nenulové záznamy) |
| `get <jméno[.ext]> <soubor>` | Extrahovat soubor z disku (raw data) |
| `get --id N <soubor>` | Extrahovat soubor podle čísla (raw data) |
| `get --mzf <jméno[.ext]> <soubor>` | Extrahovat soubor jako MZF |
| `get --mzf --id N <soubor>` | Extrahovat podle čísla jako MZF |
| `get --all <adresář>` | Exportovat všechny soubory do adresáře (raw) |
| `get --all --mzf <adresář>` | Exportovat všechny soubory do adresáře (MZF) |
| `put <soubor> <jméno.ext> [volby]` | Vložit raw soubor na disk |
| `put <soubor> --name JMÉNO.EXT [volby]` | Vložit raw soubor se striktní validací jména |
| `put --mzf <soubor> [jméno.ext]` | Vložit MZF soubor na disk |
| `put --mzf <soubor> --name JMÉNO.EXT` | Vložit MZF se striktní validací cílového jména |
| `era <jméno[.ext]>` | Smazat soubor podle jména |
| `era --id N` | Smazat soubor podle čísla |
| `ren <jméno[.ext]> <nové[.ext]>` | Přejmenovat soubor |
| `ren --id N <nové[.ext]>` | Přejmenovat soubor podle čísla |
| `set-addr <jméno[.ext]> [--fstrt HEX] [--fexec HEX]` | Změnit STRT/EXEC adresy existujícího souboru |
| `set-addr --id N [--fstrt HEX] [--fexec HEX]` | Změnit adresy podle čísla souboru |
| `file <jméno[.ext]>` | Zobrazit detaily souboru (metadata + seznam bloků) |
| `file --id N` | Zobrazit detaily souboru podle čísla |
| `dump-block N [size] [--noinv] [--dump-charset MODE]` | Hexdump MRS bloku |
| `get-block N <soubor> [size] [--noinv]` | Extrahovat MRS blok(y) do souboru |
| `put-block N <soubor> [size] [offset] [--noinv]` | Zapsat soubor do MRS bloku |
| `init [--force]` | Vytvořit prázdný MRS souborový systém (FAT + adresář) |
| `defrag` | Defragmentovat souborový systém |

## Volby

| Volba | Hodnoty | Výchozí | Popis |
|-------|---------|---------|-------|
| `--fat-block N` | číslo | 36 | Číslo bloku, kde začíná FAT |
| `--ro` | - | - | Vynutit režim pouze pro čtení |
| `--output FMT` | text\|json\|csv | text | Výstupní formát (text, json, csv) |
| `-o FMT` | text\|json\|csv | text | Zkratka pro --output |
| `--version` | - | - | Zobrazit verzi programu |
| `--lib-versions` | - | - | Zobrazit verze použitých knihoven |
| `--help`, `-h` | - | - | Zobrazit nápovědu |

Volba `--dump-charset MODE` je dostupná pouze u příkazu `dump-block`
(viz sekce `dump-block`).

### Podrobnosti k volbám

**--fat-block N** - MRS driver používá hardcoded pozici FAT, která
pro standardní 720K disketu začíná na alokačním bloku 36 (odpovídá
4. stopě, 1. sektoru). Pro nestandardní rozložení lze pozici změnit.

**--ro** - Vynutí otevření disku v režimu pouze pro čtení. Příkazy
`put`, `era`, `ren`, `set-addr`, `init`, `defrag` a `put-block` pak selžou
s chybou, protože vyžadují zápis.

---

## Jména souborů

MRS jména souborů mají formát **8 znaků + 3 znaky přípona**, jsou
**case-sensitive** a na disku doplněna mezerami (0x20) na plnou délku.

Na příkazovém řádku se zadávají ve formátu `jméno.ext`:
- `util+.DAT` - hledá přesnou shodu jména "util+" a přípony "DAT"
- `util+` - hledá jen podle jména "util+" (libovolná přípona)
- `nu2.MRS` - zdrojový text "nu2" s příponou MRS

Přípony souborů v MRS:
- **MRS** - zdrojový text Z80 assembleru (editor)
- **DAT** - binární data nebo strojový kód
- **RAM** - stránka RAMdisku
- **SCR** - screen dump (obrazovka)

---

## info - Informace o souborovém systému

Zobrazí rozložení FAT a adresáře, celkovou a volnou kapacitu a počet
obsazených souborů. Všechny údaje jsou odvozené z obsahu FAT tabulky.

```
mzdsk-mrs disk.dsk info
```

### Příklad výstupu

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

### Rozpad Used blocks a detekce nekonzistence

`Used blocks` (= `total - free`) zahrnuje jak systémové bloky (FAT,
Directory), tak souborové bloky. Nástroj proto zobrazuje pod souhrnnou
hodnotou detailní rozpad:

- **FAT** - počet bloků označených v FAT hodnotou `0xFA`.
- **Directory** - počet bloků označených `0xFD` (DIR sektory).
- **Bad** - počet bloků označených `0xFE` (vadné).
- **Files (FAT)** - počet bloků s `file_id` v rozsahu 1-249 (součet
  všech bloků, které FAT přiřadila souborům).
- **Files (dir)** - součet pole `bsize` ze všech aktivních položek
  adresáře (tak, jak je vidí `dir`).

Za normálních okolností platí `Files (FAT) == Files (dir)`. Pokud se
údaje liší (typicky po nestandardní manipulaci s filesystemem, nebo
u historických disků, kde bsize neodráží přesně FAT stav), nástroj
připojí varovnou hlášku:

```
(!) FAT file blocks differ from dir bsize sum by +32 - run `defrag` to reconcile.
```

Operace `defrag` soubory přepíše sekvenčně, FAT chain se přebuduje
podle skutečně přečtených dat a bsize v adresáři se sjednotí s FAT.
Po `defrag` vrací `info` konzistentní čísla a varování zmizí.

### JSON / CSV výstup

V režimu `--output json` / `--output csv` nese rozpad klíče
`fat_blocks`, `dir_blocks`, `bad_blocks`, `file_blocks` a
`dir_bsize_sum` - užitečné pro skriptované ověření integrity disku.

### Poznámka k rozložení

Nástroj automaticky detekuje velikost FAT a adresáře z markerů ve
FAT tabulce (0xFA = FAT sektor, 0xFD = adresářový sektor). U disket
inicializovaných původním MRS driverem dává tato detekce výsledek
2 FAT sektory + 7 DIR sektorů, což se liší od skutečné MRS konvence
3 FAT sektory + 6 DIR sektorů (viz `devdocs/MZ_MRS_poznamky.txt`).
Rozdíl je kosmetický - čtení souborů funguje správně v obou případech.

---

## dir - Výpis adresáře

Zobrazí seznam aktivních souborů na MRS disketě. Pro každý soubor
vypíše číslo souboru, jméno, příponu, start adresu (fstrt), exec
adresu (fexec), počet alokovaných bloků a velikost v bajtech.

```
mzdsk-mrs disk.dsk dir
mzdsk-mrs disk.dsk dir --raw
```

Volba `--raw` zobrazí také prázdné a smazané sloty a padding.

### Příklad výstupu

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

### Sloupce

- **ID** - file_id (pořadové číslo souboru v adresáři, 1-89)
- **Name** - jméno souboru (8 znaků, doplněno mezerami)
- **Ext** - přípona: `MRS` (zdrojový text), `DAT` (data), `RAM`
  (stránka RAMdisku), `SCR` (screen dump z MZpaint)
- **fstrt** - start adresa v RAM. U MRS souborů má tvar 0xHHFA,
  kde HH = 0xD4 - 2 x blocks (pozice v textovém bufferu editoru).
  U DAT souborů je to skutečná load adresa.
- **fexec** - exec adresa (pro příkaz `run`). 0x0000 u zdrojových textů.
- **Blocks** - počet alokovaných 512B bloků
- **Size** - velikost v bajtech (Blocks x 512)

---

## fat - Obsah FAT tabulky

Zobrazí hex dump FAT tabulky, ale pouze řádky obsahující alespoň
jednu nenulovou hodnotu. Vhodné pro rychlou inspekci obsazenosti.

```
mzdsk-mrs disk.dsk fat
```

### Příklad výstupu

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

### Významy hodnot

| Hodnota | Význam |
|---------|--------|
| `0x00` | Volný blok |
| `0x01-0x59` | Číslo souboru (file_id z adresáře) |
| `0xFA` | Blok patří FAT tabulce |
| `0xFD` | Blok patří adresáři |
| `0xFE` | Vadný sektor |
| `0xFF` | Systémový/rezervovaný blok (boot oblast) |

---

## get - Extrakce souboru (raw)

Extrahuje soubor z MRS disku do lokálního souboru. Data jsou čtena
ze všech bloků identifikovaných hodnotou file_id ve FAT tabulce,
ve vzestupném pořadí bloků, a automaticky deinvertována.

```
mzdsk-mrs disk.dsk get <jméno[.ext]> <výstupní_soubor>
mzdsk-mrs disk.dsk get --id N <výstupní_soubor>
```

Výstupní soubor obsahuje surová (raw) data - bloky x 512 B bez
jakékoliv hlavičky. MRS neukládá přesnou velikost v bajtech, takže
výstup je vždy zarovnán na celé bloky.

### Příklad

```bash
# Podle jména a přípony
mzdsk-mrs disk.dsk get util+.DAT util_plus.bin

# Pouze podle jména (najde první shodu s libovolnou příponou)
mzdsk-mrs disk.dsk get util+ util_plus.bin

# Podle file_id
mzdsk-mrs disk.dsk get --id 1 util_plus.bin
```

---

## get --mzf - Extrakce souboru jako MZF

Extrahuje soubor z MRS disku do MZF formátu (128B hlavička + data).
MZF hlavička obsahuje metadata z adresářové položky:
- ftype = 0x01 (OBJ)
- jméno = "fname.ext" z MRS adresáře
- fsize = bsize x 512
- fstrt, fexec = z adresářové položky

```
mzdsk-mrs disk.dsk get --mzf <jméno[.ext]> <výstup.mzf>
mzdsk-mrs disk.dsk get --mzf --id N <výstup.mzf>
```

MZF formát zachová metadata, takže při zpětném importu přes
`put --mzf` se fstrt a fexec automaticky obnoví.

### Příklad

```bash
mzdsk-mrs disk.dsk get --mzf util+.DAT util_plus.mzf
mzdsk-mrs disk.dsk get --mzf --id 1 output.mzf
```

### Omezení

MZF fsize je uint16_t (max 65535 B). Soubory větší než 65535 B
nelze exportovat do MZF formátu.

---

## get --all - Export všech souborů

Exportuje všechny soubory z MRS disku do adresáře na hostitelském
souborovém systému. Každý soubor se uloží jako samostatný soubor.
Jména souborů jsou sanitizována pro hostitelský OS (znaky
`* ? " < > | : \ /` se nahradí `_`).

```
mzdsk-mrs disk.dsk get --all <adresář>
mzdsk-mrs disk.dsk get --all --mzf <adresář>
```

### Volby get --all

| Volba | Hodnoty | Výchozí | Popis |
|-------|---------|---------|-------|
| `--mzf` | - | - | Export jako MZF (128B hlavička + data) |
| `--on-duplicate` | rename/overwrite/skip | rename | Řešení duplicitních jmen |
| `--ftype HH` | 0x00-0xFF | 0x01 | MZF typ souboru (jen s --mzf) |
| `--exec-addr N` | 0x0000-0xFFFF | z dir | Override exec adresy (jen s --mzf) |
| `--strt-addr N` | 0x0000-0xFFFF | z dir | Override start adresy (jen s --mzf) |

### Režimy řešení duplicit

- **rename** (výchozí) - přidá `~2`, `~3` atd. před příponu
- **overwrite** - přepíše existující soubory bez varování
- **skip** - přeskočí existující soubory s varováním na stderr

### Příklady

```bash
# Export všech souborů jako raw data
mzdsk-mrs disk.dsk get --all ./export

# Export všech souborů jako MZF
mzdsk-mrs disk.dsk get --all --mzf ./export-mzf

# Export jako MZF s vlastním typem souboru
mzdsk-mrs disk.dsk get --all --mzf --ftype 0x22 ./export

# Export s override adres
mzdsk-mrs disk.dsk get --all --mzf --exec-addr 0x2000 --strt-addr 0x3000 ./export

# Přeskočení existujících souborů
mzdsk-mrs disk.dsk get --all --on-duplicate skip ./export
```

### Poznámky

- Výchozí MZF ftype je 0x01 (OBJ) a start/exec adresy se berou
  z adresářové položky MRS. Volbami `--ftype`, `--exec-addr`
  a `--strt-addr` lze hodnoty přepsat.
- Soubory větší než 65535 B se v MZF režimu přeskočí (s varováním).
  Raw režim nemá omezení velikosti.
- Cílový adresář se vytvoří automaticky pokud neexistuje.
- **Exit code:** 0 při úspěchu (včetně souborů přeskočených přes
  `--on-duplicate skip`), 1 pokud alespoň jeden soubor nebylo možné
  extrahovat kvůli skutečné chybě (např. nekonzistentní FAT, I/O chyba).
  Skript tak pozná částečné selhání - výstup přitom hlásí souhrn
  `Extracted N files (M errors)` s počtem úspěšných i chybných.

---

## put - Vložení souboru (raw)

Vloží lokální soubor na MRS disk. Data se zarovnají na celé bloky
(512 B), případný zbytek se doplní nulami.

```
mzdsk-mrs disk.dsk put <vstupní_soubor> <jméno.ext> [--fstrt ADDR] [--fexec ADDR]
```

### Volby příkazu put

| Volba | Výchozí | Popis |
|-------|---------|-------|
| `--fstrt ADDR` | 0x0000 | Start adresa (load address) |
| `--fexec ADDR` | 0x0000 | Exec adresa (execution address) |
| `--name JMÉNO.EXT` | - | Striktní validace jména (bez tichého zkracování) |

Adresy lze zadávat dekadicky nebo hexadecimálně (s prefixem 0x).

### Příklad

```bash
# Vložení binárního souboru s nastavením adres
mzdsk-mrs disk.dsk put program.bin myfile.DAT --fstrt 0x2000 --fexec 0x2000

# Vložení bez zadání adres (obě budou 0x0000)
mzdsk-mrs disk.dsk put source.bin text.MRS
```

### Poznámky

- Jméno souboru musí být unikátní (MRS kontroluje jen 8 znaků jména).
- Pokud je zadané jméno delší než 8 znaků (nebo přípona delší než 3),
  MRS jméno zkrátí a na stderr vypíše varování
  `Warning: Name 'LONGNAMETEST.DAT' was truncated to 'LONGNAME.DAT'`.
  Pokud zkrácené jméno koliduje s existujícím souborem, `put` selže
  s chybou "File exists".
- Volba `--name JMÉNO.EXT` aktivuje striktní režim bez tichého
  zkracování - viz sekce níže.
- Maximální počet souborů na disku je 88-89 (závisí na verzi MRS).
- Soubor musí být v přípustné velikosti vzhledem k volným blokům.
- Bloky se alokují v datové oblasti sekvenčně od nejnižšího volného.

### Striktní validace jména pomocí `--name`

Volba `--name JMÉNO.EXT` u příkazů `put` a `put --mzf` aktivuje
striktní režim - žádné tiché zkracování, žádná normalizace. Pokud
jméno porušuje MRS pravidla, operace selže s chybovým kódem.

Validační pravidla:
- délka jména maximálně 8 znaků,
- délka přípony maximálně 3 znaky,
- jméno nesmí být prázdné,
- zakázané znaky: `< > , ; : = ? * [ ] | /`, dále tečka uvnitř jména
  nebo přípony, mezera a řídicí znaky (0x00-0x1F, 0x7F).

Chybové hlášky na stderr (exit code 1):

```
Error: Name 'LONGNAMETEST.DAT' exceeds 8.3 limit (no truncation with --name)
Error: Name 'A?B.DAT' contains forbidden character '?' (no truncation with --name)
Error: Name '.DAT' has empty filename (no truncation with --name)
```

Volba `--name` nahrazuje 2. poziční argument u `put` a `put --mzf`.
Současné použití obou variant vrátí chybu
`Error: --name conflicts with positional <name.ext>`.

---

## put --mzf - Vložení MZF souboru

Vloží MZF soubor na MRS disk. Metadata (fstrt, fexec) se automaticky
převezmou z MZF hlavičky. Jméno na disku lze zadat z příkazového
řádku, nebo se odvodí z MZF hlavičky.

```
mzdsk-mrs disk.dsk put --mzf <vstup.mzf> [jméno.ext] [--charset eu|jp]
```

Pokud jméno není zadáno, odvodí se z MZF hlavičky. Pokud MZF jméno
neobsahuje tečku, přípona se nastaví na "DAT".

### Konverze fname - přepínač `--charset`

MZF soubory mají jméno uložené v Sharp MZ ASCII. Pro správný převod
do standardního ASCII se používá přepínač `--charset`:

- `--charset eu` (výchozí) - evropská varianta (MZ-700/MZ-800)
- `--charset jp` - japonská varianta (MZ-1500)

Pokud nelze odvodit použitelné MRS jméno (např. MZF fname začíná
mezerou nebo samými netisknutelnými znaky), CLI vypíše informativní
chybu a doporučí explicitní override pozičním argumentem.

### Příklad

```bash
# Import s metadaty z MZF hlavičky (jméno i adresy)
mzdsk-mrs disk.dsk put --mzf util_plus.mzf

# Import s přepsáním jména
mzdsk-mrs disk.dsk put --mzf util_plus.mzf copy.DAT

# Japonská varianta znakové sady (MZ-1500)
mzdsk-mrs disk.dsk put --mzf japan.mzf --charset jp
```

---

## file - Detailní informace o souboru

Zobrazí metadata souboru (jméno, přípona, file_id, start/exec adresa,
velikost) a kompletní seznam všech bloků patřících souboru z FAT
tabulky, včetně adresy track:sector pro každý blok.

```
mzdsk-mrs disk.dsk file <jméno[.ext]>
mzdsk-mrs disk.dsk file --id N
```

### Příklad

```bash
mzdsk-mrs disk.dsk file util+.DAT
mzdsk-mrs disk.dsk file --id 3
```

---

## dump-block - Hexdump MRS bloku

Zobrazí hexdump MRS bloku (512 B). Data jsou automaticky
deinvertována. S volbou `--noinv` se zobrazí surová data.

```
mzdsk-mrs disk.dsk dump-block <blok> [velikost] [--noinv] [--dump-charset MODE]
```

### Volby

| Volba | Hodnoty | Výchozí | Popis |
|-------|---------|---------|-------|
| `--noinv` | - | - | Zobrazit surová (invertovaná) data bez deinverze |
| `--dump-charset MODE` | raw\|eu\|jp\|utf8-eu\|utf8-jp | raw | Znaková sada v ASCII sloupci hexdumpu |

### Volba --dump-charset

Nastaví konverzi Sharp MZ znakové sady v ASCII sloupci hexdumpu.

Sharp MZ počítače používají vlastní znakovou sadu (Sharp MZ ASCII), která
existuje ve dvou variantách: evropské (EU) a japonské (JP). Se standardní
ASCII je kompatibilní pouze v rozsahu 0x20-0x5D (velká písmena, číslice
a základní interpunkce). Znaky nad 0x5D (malá písmena, speciální symboly)
jsou v Sharp MZ ASCII umístěny na jiných pozicích než ve standardní ASCII.

Režimy `eu` a `jp` provádí jednobajtovou konverzi do standardní ASCII.
Japonská varianta je oproti evropské ochuzena - nemá malá písmena ani
některé další znaky, proto se nekonvertovatelné znaky nahrazují tečkou '.'.

Režimy `utf8-eu` a `utf8-jp` konvertují do UTF-8 a snaží se zobrazit
co nejvíce znaků včetně grafických symbolů a speciálních znaků, které
v jednobajtové ASCII konverzi nemají ekvivalent.

Dostupné režimy:
- `raw` (výchozí) - standardní ASCII (0x20-0x7E), ostatní jako '.'
- `eu` - Sharp MZ EU -> ASCII (jednobajtová konverze, evropská znaková sada)
- `jp` - Sharp MZ JP -> ASCII (jednobajtová konverze, japonská znaková sada)
- `utf8-eu` - Sharp MZ EU -> UTF-8 (evropská varianta s grafickými symboly)
- `utf8-jp` - Sharp MZ JP -> UTF-8 (japonská varianta s katakana a symboly)

### Příklady

```bash
mzdsk-mrs disk.dsk dump-block 36           # FAT blok
mzdsk-mrs disk.dsk dump-block 36 1024      # 2 bloky
mzdsk-mrs disk.dsk dump-block 36 --noinv   # surová data
mzdsk-mrs disk.dsk dump-block 45 --dump-charset utf8-eu
```

---

## get-block - Extrakce MRS bloku

Extrahuje data MRS bloku do lokálního souboru. Data jsou automaticky
deinvertována. S volbou `--noinv` se uloží surová data.

```
mzdsk-mrs disk.dsk get-block <blok> <výstup> [velikost] [--noinv]
```

### Příklady

```bash
mzdsk-mrs disk.dsk get-block 36 fat.bin
mzdsk-mrs disk.dsk get-block 45 data.bin 4096
```

---

## put-block - Zápis do MRS bloku

Zapíše data z lokálního souboru do MRS bloku. Data jsou automaticky
invertována před zápisem. S volbou `--noinv` se zapíšou přímo.

```
mzdsk-mrs disk.dsk put-block <blok> <vstup> [velikost] [offset] [--noinv]
```

### Příklady

```bash
mzdsk-mrs disk.dsk put-block 45 data.bin
mzdsk-mrs disk.dsk put-block 45 data.bin 1024 512
```

---

## era - Smazání souboru

Smaže soubor z MRS disku. Uvolní bloky ve FAT a vymaže adresářovou
položku (jméno se přepíše mezerami). Fyzická data bloků na disku
zůstávají, ale bez adresářové položky jsou nedostupná.

```
mzdsk-mrs disk.dsk era <jméno[.ext]>
mzdsk-mrs disk.dsk era --id N
```

### Příklad

```bash
mzdsk-mrs disk.dsk era util+.DAT
mzdsk-mrs disk.dsk era --id 1
```

---

## ren - Přejmenování souboru

Přejmenuje soubor na MRS disku. Kontroluje, že nové jméno neexistuje.
Pokud nové jméno obsahuje příponu (tečku), změní se i přípona.
Bez přípony se změní jen jméno, přípona zůstane původní.

```
mzdsk-mrs disk.dsk ren <jméno[.ext]> <nové[.ext]>
mzdsk-mrs disk.dsk ren --id N <nové[.ext]>
```

### Příklad

```bash
# Přejmenování včetně přípony
mzdsk-mrs disk.dsk ren util+.DAT loader.DAT

# Pouze změna jména, přípona zůstane
mzdsk-mrs disk.dsk ren util+ loader

# Podle file_id
mzdsk-mrs disk.dsk ren --id 1 loader.DAT
```

---

## set-addr - Změna STRT/EXEC adres existujícího souboru

Aktualizuje start (`fstrt`) nebo exec (`fexec`) adresu v adresářové
položce MRS souboru. **Mění pouze metadata, data souboru se nedotýká.**
Analogie `ren`, ale pro adresní pole místo jména.

Alespoň jedna z voleb `--fstrt` a `--fexec` musí být zadaná. Volby
přijímají hexadecimální hodnoty v rozsahu `0x0000`-`0xFFFF`.

```
mzdsk-mrs disk.dsk set-addr <jméno[.ext]> [--fstrt HEX] [--fexec HEX]
mzdsk-mrs disk.dsk set-addr --id N [--fstrt HEX] [--fexec HEX]
```

### Příklad

```bash
# Změna STRT i EXEC
mzdsk-mrs disk.dsk set-addr util+.DAT --fstrt 0x1234 --fexec 0xABCD

# Pouze EXEC
mzdsk-mrs disk.dsk set-addr loram.DAT --fexec 0x1F00

# Podle file_id
mzdsk-mrs disk.dsk set-addr --id 1 --fstrt 0x2000 --fexec 0x276B
```

### Příklad výstupu

```
  set-addr util+.DAT:  strt 0x1f00 -> 0x1234,  exec 0x1f00 -> 0xabcd
```

---

## init - Inicializace prázdného souborového systému

Vytvoří novou prázdnou FAT a adresář na disketě. Nezasahuje do
systémové oblasti (bloky 0-35, obsahující boot a MRS systém) ani
do datové oblasti za adresářem.

```
mzdsk-mrs disk.dsk init [--force]
```

### Volba --force

Pokud na disketě již existuje platný MRS souborový systém (FAT
s odpovídající signaturou), příkaz `init` bez `--force` skončí
s chybou a vypíše počet existujících souborů. Tím se zabrání
nechtěnému přepsání dat. Volba `--force` tuto kontrolu přeskočí
a vynutí inicializaci.

### Co přesně init udělá

- **Zapíše 3 FAT sektory** (bloky 36-38) s obsahem:
  - Bloky 0-35: `0xFF` (systémová oblast)
  - Bloky 36, 37: `0xFA` (FAT markery)
  - Bloky 38-44: `0xFD` (DIR markery, včetně 3. FAT sektoru)
  - Bloky 45-1439: `0x00` (volné)

- **Zapíše 6 adresářových sektorů** (bloky 39-44):
  - 89 prázdných slotů s postupně rostoucím file_id (1-89)
  - Prázdné sloty obsahují jméno z mezer a inicializační vzor
    `0xCD490221` na offsetu 28-31
  - Posledních 7 slotů vyplněno hodnotou `0x1A` (nedostupné)

- Veškerá data jsou před zápisem invertována (XOR 0xFF).

### Upozornění

Příkaz `init` **přepíše** existující FAT a adresář. Všechny informace
o souborech na disketě budou ztraceny (samotná data souborů zůstanou,
ale bez položek v adresáři budou nedostupná).

Pro bezpečnost lze použít nejprve `--ro` a ověřit si stav diskety
pomocí `info` / `dir`.

### Příklad

```bash
# Inicializace prázdného disku
mzdsk-mrs disk.dsk init

# Vynucená reinicializace disku, který již obsahuje MRS filesystem
mzdsk-mrs disk.dsk init --force
```

### Příklad výstupu

```
Initializing MRS filesystem at fat_block=36...
  FAT written:       3 sectors at blocks 36-38
  Directory written: 6 sectors at blocks 39-44
  89 empty directory slots, 7 inaccessible slots

MRS filesystem initialized successfully.
```

---

## defrag - Defragmentace souborového systému

Načte všechny soubory z disku do paměti, provede formát FAT a adresáře
a znovu zapíše vše sekvenčně od začátku datové oblasti bez mezer.
Výsledkem jsou soubory uložené souvisle za sebou bez fragmentace.

```
mzdsk-mrs disk.dsk defrag
```

### Co přesně defrag udělá

1. Inicializuje konfiguraci MRS z obsahu FAT.
2. Načte všechny aktivní soubory z disku do paměti (data + metadata).
3. Provede formát FAT a adresáře (fsmrs_format_fs).
4. Re-inicializuje konfiguraci z čerstvě naformátovaného disku.
5. Sekvenčně zapíše všechny soubory od začátku datové oblasti.

Po defragmentaci:
- Všechny soubory jsou souvisle uloženy od datového bloku.
- Pořadí souborů odpovídá původnímu pořadí v adresáři.
- Metadata souborů (jméno, přípona, fstrt, fexec, bsize) jsou zachována.
- Čísla souborů (file_id) se mohou změnit (přiřazují se sekvenčně).

### Upozornění

Defragmentace je **destruktivní operace**. Při chybě uprostřed procesu
(např. disk full, I/O chyba) mohou být data na disku nekonzistentní
nebo ztracena. Před defragmentací si vždy vytvořte zálohu obrazu.

### Skrytý 3. FAT sektor

Na discích s originálním MRS formátem je 3. FAT sektor označen ve FAT
jako DIR (0xFD), přestože fyzicky obsahuje FAT data pro bloky 1024-1439.
Defragmentace automaticky přečte tento skrytý sektor, takže soubory
s bloky v rozsahu 1024-1439 jsou správně zpracovány. Po defragmentaci
jsou všechny soubory přesunuty do rozsahu 45-1023 (v rámci pokrytí
2 FAT sektorů).

### Příklad

```bash
# Defragmentace MRS disku
mzdsk-mrs disk.dsk defrag
```

### Příklad výstupu

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

## Příklady

Základní průzkum MRS diskety:

```bash
mzdsk-mrs disk.dsk info
mzdsk-mrs disk.dsk dir
mzdsk-mrs disk.dsk fat
```

Extrakce souboru a následné vložení zpět:

```bash
mzdsk-mrs disk.dsk get util+.DAT util_plus.bin
mzdsk-mrs disk.dsk era util+.DAT
mzdsk-mrs disk.dsk put util_plus.bin util+.DAT --fstrt 0x1F00 --fexec 0x1F00
```

Přejmenování souboru:

```bash
mzdsk-mrs disk.dsk ren util+.DAT loader.DAT
```

Prohlížení všech slotů adresáře včetně smazaných:

```bash
mzdsk-mrs disk.dsk dir --raw
```

Bezpečný náhled (jen pro čtení):

```bash
mzdsk-mrs --ro disk.dsk dir
```

Inicializace MRS souborového systému (vynucená, pokud již existuje):

```bash
mzdsk-mrs disk.dsk init --force
```

## Omezení

- **Pouze 720K diskety** s layoutem 1440 bloků (9 x 160).
- **Auto-detekce layoutu FAT/DIR** se může lišit od původního MRS
  driveru o jeden sektor (viz `devdocs/MZ_MRS_poznamky.txt`).
- **MRS neukládá velikost souboru v bajtech** - pouze počet bloků.
  Extrahovaný soubor je vždy zarovnán na celé 512B bloky.
- **FAT pokrytí**: Na discích se 2 FAT sektory (starší formát) lze
  alokovat pouze bloky 0-1023 (501 KB dat). Bloky 1024-1439 jsou
  mimo dosah FAT.

---

## Nesekvenční sektory

Blokové operace postupují sekvenčně podle ID sektorů (jako řadič disketové
jednotky). Sektory s nesekvenčním ID nejsou přes blokové příkazy dostupné.

Pro přístup k těmto sektorům použijte `mzdsk-raw` s volbou `--order phys`:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 10 --order phys
```
