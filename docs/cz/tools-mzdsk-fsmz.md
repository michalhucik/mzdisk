# mzdsk-fsmz - Práce s FSMZ souborovým systémem (MZ-BASIC)

Verze nástroje: 1.14.2. Součást projektu [mzdisk](https://github.com/michalhucik/mzdisk/).

> **Upozornění:** Vždy pracujte s kopií DSK obrazu, ne s originálem.
> Autor projektu neposkytuje žádné záruky a neodpovídá za ztráty ani
> poškození dat způsobené používáním nástrojů projektu mzdisk. Projekt
> je distribuován pod licencí GPLv3 bez záruky.

Nástroj pro správu souborů na disketách s FSMZ souborovým systémem,
který používá Sharp MZ-800 Disc BASIC a program IPLDISK. Podporuje
výpis adresáře, čtení a zápis souborů, mazání, přejmenovávání,
zamykání souborů, změnu typu, práci s boot sektorem, formátování
a opravu/defragmentaci disku.

FSMZ používá alokační bloky o velikosti 256 bajtů (1 blok = 1 sektor).
Data jsou uložena s bitovou inverzí (XOR 0xFF). Disk má strukturu:
blok 0 = IPLPRO (bootstrap), blok 15 = DINFO (informace o disku),
bloky 16-23 = adresář (63 položek), od bloku 48 = souborová data.

**Atomicita zápisu:** Zápisové operace (`put`, `era`, `ren`, `lock`, `chtype`,
`set`, `format`, `repair`, `defrag`, `put-block`, `boot put/clear/bottom/mini/over`,
`boot` s `--fstrt`/`--fexec`/`--ftype`/`--name`)
pracují atomicky - celý DSK obraz se načte do paměti, operace se provedou
v RAM a výsledek se zapíše zpět jediným voláním až po úspěšném dokončení.
Pokud operace selže uprostřed, původní soubor zůstává nezměněn. I tak
doporučujeme pro kritická data udržovat zálohy - dokončené změny jsou
nevratné.

## Použití

```
mzdsk-fsmz [volby] <obraz.dsk> <příkaz> [argumenty...]
```

Volby mohou být před DSK souborem i mezi DSK souborem a příkazem.

## Argumenty

| Argument | Popis |
|----------|-------|
| `<obraz.dsk>` | Vstupní/výstupní DSK diskový obraz |

## Příkazy

| Příkaz | Popis |
|--------|-------|
| `dir` | Zobrazit všechny soubory |
| `dir --type T` | Zobrazit soubory filtrované podle ftype (T je hex, např. 01) |
| `file <název>` | Zobrazit informace o souboru podle názvu |
| `file --id N` | Zobrazit informace o souboru podle ID |
| `get <název> <mzf>` | Exportovat soubor do MZF podle názvu |
| `get --id N <mzf>` | Exportovat soubor do MZF podle ID |
| `get --all <adresář>` | Exportovat všechny soubory do adresáře |
| `put <mzf>` | Importovat MZF soubor (128 B hlavička + data) |
| `era <název>` | Smazat soubor podle názvu |
| `era --id N` | Smazat soubor podle ID |
| `ren <název> <nový>` | Přejmenovat soubor podle názvu |
| `ren --id N <nový>` | Přejmenovat soubor podle ID |
| `lock <název> 0\|1` | Zamknout (1) nebo odemknout (0) soubor podle názvu |
| `lock --id N 0\|1` | Zamknout (1) nebo odemknout (0) soubor podle ID |
| `chtype --id N T` | Změnit ftype souboru podle ID (T je hex) |
| `set <název> [--fstrt HEX] [--fexec HEX] [--ftype HEX]` | Aktualizovat STRT/EXEC/ftype existujícího souboru podle názvu |
| `set --id N [--fstrt HEX] [--fexec HEX] [--ftype HEX]` | Aktualizovat STRT/EXEC/ftype existujícího souboru podle ID |
| `boot` | Zobrazit informace o bootstrapu (včetně klasifikace typu) |
| `boot [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--name NAME]` | Aktualizovat pole bootstrap hlavičky a zobrazit info |
| `boot put <mzf>` | Nainstalovat normální bootstrap z MZF |
| `boot get <mzf>` | Extrahovat bootstrap do MZF |
| `boot clear` | Vymazat bootstrap |
| `boot bottom <mzf>` | Nainstalovat bottom bootstrap z MZF |
| `boot mini <mzf>` | Alias pro `boot bottom` |
| `boot bottom --no-fsmz-compat <mzf>` | Bottom bootstrap s povolením přepisu FSMZ struktur |
| `boot over <mzf>` | Nainstalovat over-FAREA bootstrap (experimentální) |
| `dump-block N [bajty] [--cnv] [--dump-charset MODE]` | Hexdump FSMZ alokačního bloku |
| `get-block N <soubor> [bajty] [--noinv]` | Extrahovat FSMZ blok(y) do souboru |
| `put-block N <soubor> [bajty] [offset] [--noinv]` | Zapsat soubor do FSMZ bloku |
| `format` | Rychlé FSMZ formátování |
| `repair` | Opravit DINFO blok |
| `defrag` | Defragmentovat souborový systém |

## Volby

| Volba | Hodnoty | Výchozí | Popis |
|-------|---------|---------|-------|
| `--ipldisk` | - | - | Použít rozšířený adresář IPLDISK (127 položek místo 63) |
| `--ro` | - | - | Otevřít obraz pouze pro čtení |
| `--output FMT` | text\|json\|csv | text | Výstupní formát (text, json, csv) |
| `-o FMT` | text\|json\|csv | text | Zkratka pro --output |
| `--charset MODE` | eu\|jp\|utf8-eu\|utf8-jp | eu | Konverze Sharp MZ znakové sady |
| `-C MODE` | eu\|jp\|utf8-eu\|utf8-jp | eu | Zkratka pro --charset |
| `--version` | - | - | Zobrazit verzi programu |
| `--lib-versions` | - | - | Zobrazit verze použitých knihoven |
| `--help` | - | - | Zobrazit nápovědu |
| `-h` | - | - | Zkratka pro --help |

Volba `--dump-charset MODE` (hodnoty `raw`\|`eu`\|`jp`\|`utf8-eu`\|`utf8-jp`,
výchozí `raw`) se používá jen u podpříkazu `dump-block` a řídí konverzi
Sharp MZ znakové sady v ASCII sloupci hexdumpu (viz sekce
[dump-block](#dump-block---hexdump-fsmz-bloku)).

### Podrobnosti k volbám

**--ipldisk** - Program IPLDISK rozšiřuje adresář z 8 sektorů (bloky 16-23)
na 16 sektorů (bloky 16-31), čímž zvyšuje maximální počet položek adresáře
ze 63 na 127. Tato volba aktivuje podporu rozšířeného adresáře.
Pozor: rozšířený adresář není plně čitelný ze standardního Disc BASICu.

**--charset MODE** - Nastaví konverzi Sharp MZ znakové sady pro jména souborů
v FSMZ adresáři a bootstrap hlavičce.

Sharp MZ počítače používají vlastní znakovou sadu (Sharp MZ ASCII), která
existuje ve dvou variantách: evropské (EU) a japonské (JP). Se standardní
ASCII je kompatibilní pouze v rozsahu 0x20-0x5D (velká písmena, číslice
a základní interpunkce). Znaky nad 0x5D (malá písmena, speciální symboly)
jsou v Sharp MZ ASCII umístěny na jiných pozicích než ve standardní ASCII.

Režimy `eu` a `jp` provádí jednobajtovou konverzi do standardní ASCII.
Japonská varianta je oproti evropské ochuzena - nemá malá písmena ani
některé další znaky, proto se nekonvertovatelné znaky nahrazují mezerou.

Režimy `utf8-eu` a `utf8-jp` konvertují do UTF-8 a snaží se zobrazit
co nejvíce znaků včetně grafických symbolů a speciálních znaků, které
v jednobajtové ASCII konverzi nemají ekvivalent.

Dostupné režimy:
- `eu` (výchozí) - Sharp MZ EU -> ASCII (jednobajtová konverze, evropská znaková sada)
- `jp` - Sharp MZ JP -> ASCII (jednobajtová konverze, japonská znaková sada)
- `utf8-eu` - Sharp MZ EU -> UTF-8 (evropská varianta s grafickými symboly)
- `utf8-jp` - Sharp MZ JP -> UTF-8 (japonská varianta s katakana a symboly)

**--dump-charset MODE** - Volba per-command pouze pro podpříkaz `dump-block`.
Nastaví konverzi Sharp MZ znakové sady v ASCII sloupci hexdumpu. Význam
režimů je shodný s volbou --charset, viz výše.

**--ro** - Otevře diskový obraz pouze pro čtení. Operace modifikující
disk (put, era, ren, lock, chtype, set, format, repair, defrag, boot put/clear/bottom/mini/over,
boot s --fstrt/--fexec/--ftype/--name) budou odmítnuté.

---

## dir - Výpis adresáře

Zobrazí seznam souborů na disketě včetně typu, velikosti, adresy načtení,
adresy spuštění, čísla počátečního bloku a příznaku zamčení.

```
mzdsk-fsmz [volby] <obraz.dsk> dir [--type T]
```

Nepovinný filtr `--type T` zobrazí pouze soubory s daným ftype
(hex hodnota, např. 01 pro OBJ).

### Příklad

```bash
mzdsk-fsmz disk.dsk dir
```

Výstup:

```
=== disk.dsk - FSMZ directory ===

 ID  Type  Filename           Size   Load   Exec   Block  Lock
 --  ----  -----------------  -----  -----  -----  -----  ----
  0  OBJ   MONITOR            4096   1200   1200    0030
  1  BTM   BASIC PROG         2048   7F00   7F00    0040
  2  BSD   DATA FILE           512   0000   0000    0048

Files: 3, Used: 52 blocks, Free: 587 blocks
```

Filtrování podle typu souboru:

```bash
mzdsk-fsmz disk.dsk dir --type 01
```

Rozšířený adresář IPLDISK:

```bash
mzdsk-fsmz --ipldisk disk.dsk dir
```

---

## file - Informace o souboru

Zobrazí podrobné informace o konkrétním souboru podle názvu nebo ID.

```
mzdsk-fsmz [volby] <obraz.dsk> file <název>
mzdsk-fsmz [volby] <obraz.dsk> file --id N
```

### Příklad

```bash
mzdsk-fsmz disk.dsk file "MONITOR"
mzdsk-fsmz disk.dsk file --id 0
```

---

## get - Export souboru

Exportuje soubor z FSMZ diskety do hostitelského souboru ve formátu MZF
(128 B hlavička + data). Soubor lze identifikovat názvem nebo ID.

```
mzdsk-fsmz [volby] <obraz.dsk> get <název> <mzf>
mzdsk-fsmz [volby] <obraz.dsk> get --id N <mzf>
```

Výstupní MZF soubor je povinný argument.

### Příklady

```bash
mzdsk-fsmz disk.dsk get "MONITOR" monitor.mzf
mzdsk-fsmz disk.dsk get --id 5 game.mzf
```

### get --all - Export všech souborů

Exportuje všechny soubory z FSMZ diskety do zadaného adresáře.
Pokud adresář neexistuje, vytvoří se automaticky. Výstupní soubory
jsou ve formátu MZF, pojmenovány podle názvů na disketě s příponou .mzf.
Znaky neplatné na Windows (*, ?, ", <, >, |, :, \, /) se v názvech
souborů nahradí podtržítkem '_'.

```
mzdsk-fsmz [volby] <obraz.dsk> get --all <adresář> [--on-duplicate MODE]
```

#### Volby

| Volba | Výchozí | Popis |
|-------|---------|-------|
| `--on-duplicate MODE` | rename | Chování při duplicitním názvu výstupního souboru: `rename` (přidá `~2`, `~3`, ... před `.mzf`), `overwrite` (přepíše), `skip` (přeskočí s varováním na stderr). |

FSMZ filesystém povoluje dva soubory se stejným názvem v adresáři (viz
např. `refdata/dsk/basic.dsk`). Bez explicitní strategie by se duplicitní
název v hostitelském adresáři přepsal. Výchozí `rename` zachová všechny
soubory s číselným suffixem.

#### Příklady

```bash
mzdsk-fsmz disk.dsk get --all export/
mzdsk-fsmz disk.dsk get --all export/ --on-duplicate skip
mzdsk-fsmz disk.dsk get --all export/ --on-duplicate overwrite
```

Na stdout se po skončení vypíše statistika: počet extrahovaných,
přejmenovaných a přeskočených souborů.

---

## put - Import souboru

Importuje MZF soubor (128 B hlavička + data) na FSMZ disketu.
Název a metadata souboru (typ, load adresa, exec adresa) se přebírají
z MZF hlavičky. Soubor je uložen jako souvislá sekvence alokačních bloků.

```
mzdsk-fsmz [volby] <obraz.dsk> put <mzf>
```

### Validace vstupu

Před zápisem se provede validace MZF souboru:

- Minimální velikost 128 B (velikost MZF hlavičky). Menší soubor je
  odmítnut s hláškou "too small to be an MZF file".
- Kontrola terminátoru jména (0x0D) v poli fname.
- Shoda `fsize` v hlavičce s dostupnou velikostí dat (hlavička + tělo
  musí být <= velikost souboru). Jinak "File size does not match
  fsize field".

Při neúspěchu validace operace končí s exit kódem != 0 a disk zůstává
nedotčen.

### Příklady

```bash
mzdsk-fsmz disk.dsk put monitor.mzf
mzdsk-fsmz disk.dsk put game.mzf
```

---

## era - Smazání souboru

Smaže soubor z diskety. Nastaví ftype na 0 v adresáři a uvolní
alokační bloky v DINFO bitmapě.

Pokud má soubor nastavený příznak `locked`, operace standardně
selže s chybou "File is locked". Pro smazání uzamčeného souboru
je nutné použít volbu `--force` (nebo soubor nejprve odemknout
příkazem `lock --id N 0`).

```
mzdsk-fsmz [volby] <obraz.dsk> era <název> [--force]
mzdsk-fsmz [volby] <obraz.dsk> era --id N [--force]
```

### Příklad

```bash
mzdsk-fsmz disk.dsk era "MONITOR"
mzdsk-fsmz disk.dsk era --id 0
mzdsk-fsmz disk.dsk era --id 0 --force     # ignoruje lock
```

---

## ren - Přejmenování souboru

Přejmenuje soubor na disketě. Nový název nesmí kolidovat
s existujícím souborem.

Pokud má soubor nastavený příznak `locked`, operace standardně
selže s chybou "File is locked". Pro přejmenování uzamčeného
souboru je nutné použít volbu `--force`.

```
mzdsk-fsmz [volby] <obraz.dsk> ren <název> <nový_název> [--force]
mzdsk-fsmz [volby] <obraz.dsk> ren --id N <nový_název> [--force]
```

### Příklad

```bash
mzdsk-fsmz disk.dsk ren "MONITOR" "MON V2"
mzdsk-fsmz disk.dsk ren --id 0 "MON V2"
mzdsk-fsmz disk.dsk ren --id 0 "MON V2" --force   # ignoruje lock
```

---

## lock - Zamknutí/odemknutí souboru

Nastaví příznak zamčení souboru. Hodnota 1 soubor zamkne, 0 odemkne.
Zamčený soubor nelze smazat ze standardního Disc BASICu.

```
mzdsk-fsmz [volby] <obraz.dsk> lock <název> 0|1
mzdsk-fsmz [volby] <obraz.dsk> lock --id N 0|1
```

### Příklad

```bash
mzdsk-fsmz disk.dsk lock "MONITOR" 1
mzdsk-fsmz disk.dsk lock --id 0 0
```

---

## chtype - Změna typu souboru

Změní typ souboru v adresářové položce. Soubor se identifikuje
pouze podle ID (--id je povinný).

```
mzdsk-fsmz [volby] <obraz.dsk> chtype --id N T
```

T je hex hodnota typu souboru:
- `01` - OBJ (binární program)
- `02` - BTM (bootstrap)
- `03` - BSD (BASIC data)
- `05` - BAS (BASIC program)

### Příklad

```bash
mzdsk-fsmz disk.dsk chtype --id 0 01
```

---

## set - Aktualizace STRT/EXEC/ftype existujícího souboru

Upraví adresářovou položku bez reuploadu dat. Každý z parametrů
`--fstrt`, `--fexec` a `--ftype` je volitelný, ale musí být zadán
alespoň jeden. Data souboru ani jeho alokační bloky se nemění.

```
mzdsk-fsmz [volby] <obraz.dsk> set <název> [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--force]
mzdsk-fsmz [volby] <obraz.dsk> set --id N [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--force]
```

| Volba | Popis |
|-------|-------|
| `--fstrt HEX` | Nová startovací (load) adresa Z80, rozsah `0x0000-0xFFFF` |
| `--fexec HEX` | Nová spouštěcí (exec) adresa Z80, rozsah `0x0000-0xFFFF` |
| `--ftype HEX` | Nový typ souboru, rozsah `0x01-0xFF` (hodnota `0x00` je rezervovaná pro smazanou položku a není povolená) |
| `--force` | Ignorovat příznak `locked` - bez něj vrátí zamčený soubor chybu |

Při uzamčeném souboru bez `--force` program vypíše `File is locked`
a nic neprovede.

### Příklady

```bash
# Nastavit jen load adresu podle názvu
mzdsk-fsmz disk.dsk set "MONITOR" --fstrt 1200

# Změnit load i exec adresu podle ID
mzdsk-fsmz disk.dsk set --id 3 --fstrt 1200 --fexec 120B

# Změnit load adresu a zároveň typ (BSD -> OBJ), ignorovat lock
mzdsk-fsmz disk.dsk set --id 0 --fstrt 1200 --ftype 01 --force
```

Očekávaný výstup:

```
Set address fields for MONITOR.
  fstrt: 0x1200
```

---

## boot - Boot sektor

Zobrazí nebo nastaví obsah boot sektoru (IPLPRO blok 0) a DINFO bloku (blok 15).

| Podpříkaz | Popis |
|-----------|-------|
| `boot` | Zobrazit informace o bootstrapu (včetně klasifikace typu) |
| `boot [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--name NAME]` | Aktualizovat pole bootstrap hlavičky a zobrazit info |
| `boot put <mzf>` | Nainstalovat normální bootstrap z MZF |
| `boot get <mzf>` | Extrahovat bootstrap do MZF |
| `boot clear` | Vymazat bootstrap |
| `boot bottom <mzf>` | Nainstalovat bottom bootstrap z MZF |
| `boot mini <mzf>` | Alias pro `boot bottom` (zpětná kompatibilita) |
| `boot bottom --no-fsmz-compat <mzf>` | Bottom bootstrap s povolením přepisu FSMZ struktur |
| `boot over <mzf>` | Nainstalovat over-FAREA bootstrap (experimentální) |

```
mzdsk-fsmz [volby] <obraz.dsk> boot
mzdsk-fsmz [volby] <obraz.dsk> boot [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--name NAME]
mzdsk-fsmz [volby] <obraz.dsk> boot put <mzf>
mzdsk-fsmz [volby] <obraz.dsk> boot get <mzf>
mzdsk-fsmz [volby] <obraz.dsk> boot clear
mzdsk-fsmz [volby] <obraz.dsk> boot bottom <mzf>
mzdsk-fsmz [volby] <obraz.dsk> boot bottom --no-fsmz-compat <mzf>
mzdsk-fsmz [volby] <obraz.dsk> boot mini <mzf>
mzdsk-fsmz [volby] <obraz.dsk> boot over <mzf>
```

### Klasifikace typu bootstrapu

Příkaz `boot` (bez argumentů) zobrazuje typ bootstrapu:

| Typ | Popis |
|-----|-------|
| **Mini** | Bloky 1-14, FSMZ kompatibilní (DINFO a adresář nedotčeny) |
| **Bottom** | Blok >= 1, ale přesahuje blok 14 (FSMZ struktury přepsány) |
| **Normal** | Bootstrap v souborové oblasti FAREA |
| **Over FSMZ** | Bootstrap za hranicí souborové oblasti |

### Volba --no-fsmz-compat

Ve výchozím stavu `boot bottom` a `boot mini` na plném FSMZ disku omezují
bootstrap na bloky 1-14 (zachování DINFO na bloku 15 a adresáře od bloku 16).
S volbou `--no-fsmz-compat` je limit zvýšen na celý disk kromě IPLPRO bloku,
ale FSMZ struktury budou přepsány a disk nebude fungovat jako FSMZ.

Na non-FSMZ discích (CP/M, MRS) nemá `--no-fsmz-compat` efekt - limit je
vždy 15 bloků (boot track bez IPLPRO).

### Aktualizace bootstrap hlavičky

Příkaz `boot` s některou z voleb `--fstrt`, `--fexec`, `--ftype`, `--name`
upraví odpovídající pole v IPLPRO bloku (jméno, startovací adresa,
spouštěcí adresa, typ) a poté zobrazí aktuální stav bootstrapu.
Data bootstrapu samotná se nemění - jde o metadata-only update hlavičky,
analogicky k příkazu `set` pro soubory v adresáři.

| Volba | Popis |
|-------|-------|
| `--fstrt HEX` | Nová startovací adresa Z80 (0x0000-0xFFFF) |
| `--fexec HEX` | Nová spouštěcí adresa Z80 (0x0000-0xFFFF) |
| `--ftype HEX` | Nový typ bootstrapu (0x01-0xFF; 0x00 není povoleno) |
| `--name NAME` | Nové jméno bootstrapu (max 12 znaků, ASCII) |

Alespoň jedna volba musí být zadaná; jinak příkaz jen zobrazí info.

Aktualizace funguje pouze na disku, který již obsahuje platný IPLPRO
bootstrap (ftype `0x03` s magickým řetězcem `"IPLPRO"`). Pokud disk
bootstrap neobsahuje (např. po `mzdsk-create --format-basic` bez
následného `boot put`, nebo po `boot clear`), příkaz skončí chybou
bez modifikace disku:

```
Error: No valid IPLPRO header on disk - cannot edit bootstrap fields.
       Install a bootstrap first (e.g. 'boot put <mzf>').
```

Pro instalaci nového bootstrapu použijte `boot put <mzf>` (případně
`boot bottom` nebo `boot over`).

### Příklad

Zobrazení informací o boot sektoru:

```bash
mzdsk-fsmz disk.dsk boot
```

Instalace bottom bootstrapu:

```bash
mzdsk-fsmz disk.dsk boot bottom bootstrap.mzf
```

Instalace bottom bootstrapu s přepisem FSMZ struktur:

```bash
mzdsk-fsmz disk.dsk boot bottom --no-fsmz-compat bootstrap.mzf
```

Aktualizace jména a startovací adresy existujícího bootstrapu:

```bash
mzdsk-fsmz disk.dsk boot --name MYBOOT --fstrt 1200
```

Instalace normálního bootstrapu z MZF souboru:

```bash
mzdsk-fsmz disk.dsk boot put bootstrap.mzf
```

Extrakce aktuálního bootstrapu do MZF souboru:

```bash
mzdsk-fsmz disk.dsk boot get bootstrap.mzf
```

Vymazání bootstrapu:

```bash
mzdsk-fsmz disk.dsk boot clear
```

---

## format - Formátování disku

Inicializuje FSMZ struktury na disku: IPLPRO blok, DINFO blok
a adresář. Všechna existující data budou ztracena.

```
mzdsk-fsmz [volby] <obraz.dsk> format
```

### Příklad

```bash
mzdsk-fsmz disk.dsk format
```

---

## repair - Oprava DINFO bloku

Opraví DINFO blok jeho kompletní reinicializací:

- `farea` se nastaví na výchozí hodnotu (`FSMZ_DEFAULT_FAREA_BLOCK`),
- `blocks` (celkový počet bloků na disku) se spočítá z geometrie
  DSK obrazu,
- alokační bitmapa i čítač `used` se zrekonstruují z aktuálního obsahu
  adresáře a případného bootstrapu.

Po `repair` je DINFO konzistentní s DSK geometrií i tehdy, když byl
celý DINFO blok přepsán náhodnými daty.

```
mzdsk-fsmz [volby] <obraz.dsk> repair
```

### Příklad

```bash
mzdsk-fsmz disk.dsk repair
```

---

## defrag - Defragmentace

Defragmentuje souborovou oblast - přesune soubory tak, aby
zabíraly souvislý prostor bez mezer. Zvýší úspěšnost zápisu
nových souborů na fragmentovaný disk.

```
mzdsk-fsmz [volby] <obraz.dsk> defrag
```

### Příklad

```bash
mzdsk-fsmz disk.dsk defrag
```

---

## dump-block - Hexdump FSMZ bloku

Zobrazí hexdump obsahu FSMZ alokačního bloku (256 B). Data jsou
automaticky deinvertována. Při zadání velikosti větší než jeden
blok se čte více po sobě jdoucích bloků.

```
mzdsk-fsmz [volby] <obraz.dsk> dump-block <blok> [bajty] [--cnv] [--dump-charset MODE]
```

Volba `--cnv` je alias pro `--dump-charset eu`. Volba `--dump-charset MODE`
umožňuje výběr konkrétního konverzního režimu pro ASCII sloupec hexdumpu.
Bez těchto voleb se zobrazí standardní ASCII.

### Příklady

```bash
# Hexdump bloku 16 (první adresářový blok)
mzdsk-fsmz disk.dsk dump-block 16

# S konverzí Sharp MZ ASCII (jména souborů budou čitelná)
mzdsk-fsmz disk.dsk dump-block 16 --cnv

# Hexdump 512 B (2 bloky) od bloku 16
mzdsk-fsmz disk.dsk dump-block 16 512 --cnv
```

---

## get-block - Extrakce FSMZ bloku

Přečte data FSMZ alokačního bloku (nebo série bloků) a uloží
je do lokálního souboru. Data jsou automaticky deinvertována.

```
mzdsk-fsmz [volby] <obraz.dsk> get-block <blok> <soubor> [bajty] [--noinv]
```

Volba `--noinv` vypne automatickou deinverzi - data se uloží
v invertovaném stavu tak, jak jsou na disku.

### Příklady

```bash
mzdsk-fsmz disk.dsk get-block 0 iplpro.bin
mzdsk-fsmz disk.dsk get-block 15 dinfo.bin
mzdsk-fsmz disk.dsk get-block 16 dir.bin 2048
```

---

## put-block - Zápis do FSMZ bloku

Zapíše data z lokálního souboru do FSMZ alokačního bloku (nebo
série bloků). Data se automaticky invertují při zápisu.

```
mzdsk-fsmz [volby] <obraz.dsk> put-block <blok> <soubor> [bajty] [offset] [--noinv]
```

Volba `--noinv` vypne automatickou inverzi - data se zapíšou
přímo tak, jak jsou v souboru.

### Příklady

```bash
mzdsk-fsmz disk.dsk put-block 0 custom_iplpro.bin
mzdsk-fsmz disk.dsk put-block 15 fixed_dinfo.bin
```

---

## Příklady

Kompletní workflow - vytvoření diskety a naplnění soubory:

```bash
mzdsk-create --format-basic disk.dsk
mzdsk-fsmz disk.dsk put loader.mzf
mzdsk-fsmz disk.dsk put game.mzf
mzdsk-fsmz disk.dsk dir
```

Práce s IPLDISK rozšířeným adresářem:

```bash
mzdsk-fsmz --ipldisk disk.dsk dir
mzdsk-fsmz --ipldisk disk.dsk put program.mzf
```

Export všech souborů z diskety:

```bash
mzdsk-fsmz disk.dsk get --all export/
```

Export jednotlivých souborů:

```bash
mzdsk-fsmz disk.dsk get "LOADER" loader.mzf
mzdsk-fsmz disk.dsk get --id 0 game.mzf
```

Zamykání a odemykání souborů:

```bash
mzdsk-fsmz disk.dsk lock "LOADER" 1
mzdsk-fsmz disk.dsk lock "LOADER" 0
```

Správa bootstrapu:

```bash
mzdsk-fsmz disk.dsk boot
mzdsk-fsmz disk.dsk boot put bootstrap.mzf
mzdsk-fsmz disk.dsk boot get bootstrap.mzf
mzdsk-fsmz disk.dsk boot clear
```

Hexdump bloku adresáře s konverzí Sharp MZ ASCII:

```bash
mzdsk-fsmz disk.dsk dump-block 16 --cnv
mzdsk-fsmz disk.dsk dump-block 16 512 --cnv
```

Extrakce a zápis surových FSMZ bloků:

```bash
mzdsk-fsmz disk.dsk get-block 0 iplpro.bin
mzdsk-fsmz disk.dsk get-block 15 dinfo.bin
mzdsk-fsmz disk.dsk put-block 0 custom_iplpro.bin
```

Oprava poškozeného disku:

```bash
mzdsk-fsmz disk.dsk repair
mzdsk-fsmz disk.dsk defrag
```

---

## Nesekvenční sektory

Blokové operace (get-block, put-block) postupují sekvenčně podle ID sektorů
(jako řadič disketové jednotky: ID 1, 2, 3, ...). Sektory s nesekvenčním ID
(např. LEMMINGS disk se sektory 1-9 + 22 na jedné stopě) nejsou přes
blokové příkazy dostupné.

Pro přístup k těmto sektorům použijte `mzdsk-raw` s volbou `--order phys`,
která postupuje podle fyzické pozice sektorů v DSK obrazu:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 10 --order phys
mzdsk-raw disk.dsk get data.bin --track 0 --sector 1 --sectors 10 --order phys
```
