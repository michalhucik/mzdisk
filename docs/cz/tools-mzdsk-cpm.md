# mzdsk-cpm - Práce s CP/M souborovým systémem

Verze nástroje: 1.19.1. Součást projektu [mzdisk](https://github.com/michalhucik/mzdisk/).

> **Upozornění:** Vždy pracujte s kopií DSK obrazu, ne s originálem.
> Autor projektu neposkytuje žádné záruky a neodpovídá za ztráty ani
> poškození dat způsobené používáním nástrojů projektu mzdisk. Projekt
> je distribuován pod licencí GPLv3 bez záruky.

Nástroj pro správu souborů na disketách s CP/M souborovým systémem
pro počítače Sharp MZ. Podporuje formáty používané s LEC CP/M:
SD (single density, 9x512B sektory) a HD (high density, 18x512B sektory).
Obě varianty mohou být 1-stranné i 2-stranné - geometrie se detekuje
automaticky z DSK obrazu.

CP/M používá 128B logické sektory a alokační bloky (2 KB pro SD,
4 KB pro HD). Boot stopa (absolutní stopa 1) je ve formátu FSMZ
(16x256 B) a CP/M ji nepoužívá. Formát disku se detekuje automaticky
z geometrie DSK obrazu (počet sektorů na stopu, počet stran).

Při otevírání DSK obrazu se automaticky opravují chybné tsize hodnoty
v hlavičce (např. nesprávná velikost boot stopy).

**Atomicita zápisu:** Zápisové operace (`put`, `era`, `ren`, `attr`,
`format`, `defrag`, `put-block`) pracují atomicky - celý DSK obraz se načte
do paměti, operace se provedou v RAM a výsledek se zapíše zpět jediným
voláním až po úspěšném dokončení. Pokud operace selže uprostřed, původní
soubor zůstává nezměněn. I tak doporučujeme pro kritická data udržovat
zálohy - dokončené změny jsou nevratné.

## Použití

```
mzdsk-cpm [volby] <obraz.dsk> <příkaz> [argumenty...]
```

## Argumenty

| Argument | Popis |
|----------|-------|
| `<obraz.dsk>` | Vstupní/výstupní DSK diskový obraz |

## Příkazy

### Adresářové operace

| Příkaz | Popis |
|--------|-------|
| `dir` | Zobrazit obsah CP/M adresáře |
| `dir --ex` | Rozšířený výpis s atributy, počtem extentů a dir indexem |
| `dir --raw` | Surový výpis 32B adresářových položek (včetně smazaných) |
| `file <název.ext>` | Zobrazit podrobnosti o souboru (včetně extentů a bloků) |

### Souborové operace

| Příkaz | Popis |
|--------|-------|
| `get <název.ext> <výstup>` | Exportovat soubor z CP/M diskety (raw binární) |
| `get --mzf <název.ext> <výstup.mzf> [volby]` | Exportovat jako MZF (výchozí: CPM-IC $22) |
| `get --all <adresář> [volby]` | Exportovat všechny soubory do adresáře |
| `put <soubor> <název.ext>` | Importovat soubor na CP/M disketu (raw binární) |
| `put <soubor> --name NÁZEV.EXT` | Importovat se striktní validací jména (bez tichého zkracování) |
| `put --mzf <vstup.mzf> [název.ext]` | Importovat MZF s CPM-IC hlavičkou na CP/M disketu |
| `put --mzf <vstup.mzf> --name NÁZEV.EXT` | Importovat MZF se striktní validací cílového jména |
| `era <název.ext>` | Smazat soubor z CP/M diskety |
| `ren <starý.ext> <nový.ext>` | Přejmenovat soubor na CP/M disketě |
| `chuser <název.ext> <nový-user>` | Změnit user number (0-15) existujícího souboru |

#### get --mzf

Exportuje soubor z CP/M diskety ve formátu MZF (SharpMZ tape formát).
Výstupem je jeden MZF rámec - 128B hlavička + tělo dat o velikosti souboru.
Výchozí hodnoty odpovídají konvenci utility SOKODI CMT.COM pro export
z CP/M: typ 0x22, load 0x0100, exec 0x0100, zapnuté kódování atributů.

```
get --mzf <název.ext> <výstup.mzf> [volby]
```

| Volba | Výchozí | Popis |
|-------|---------|-------|
| `--ftype HH` | 22 | MZF typ souboru (hex, 0x00-0xFF). Známé hodnoty: 01=OBJ, 02=BTX, 03=BSD, 04=BRD, 05=RB, 22=konvence SOKODI CMT.COM pro CP/M export. |
| `--exec-addr N` | 0x0100 | Exec adresa (0x0000-0xFFFF) zapsaná do pole `fexec`. |
| `--addr N` | 0x0100 | Alias pro `--exec-addr` (zpětná kompatibilita). |
| `--strt-addr N` | 0x0100 | Load adresa (0x0000-0xFFFF) zapsaná do pole `fstrt`. |
| `--no-attrs` | vypnuto | Potlačí kódování CP/M atributů R/O, SYS, ARC do bitů 7 v `fname[9..11]`. Platí pouze pro `--ftype 22` (konvence SOKODI CMT.COM); pro ostatní typy se atributy nikdy nekódují. |

Pozn.: MZF pole `fstrt` a `fexec` pocházejí ze SharpMZ tape formátu,
kde popisují load a exec adresu programu na CMT. CP/M tyto adresy sám
neřeší (všechny programy se nahrávají a spouštějí z TPA 0x0100),
proto jsou výchozí hodnoty 0x0100.

Název souboru v MZF hlavičce se přebírá z CP/M adresáře.

Limit velikosti: pole `fsize` v MZF je 16bitové, takže tělo rámce
nemůže překročit 65535 B. Větší soubory `get --mzf` odmítne.

#### get --all

Exportuje všechny soubory z CP/M diskety do zadaného adresáře.

```
get --all <adresář> [--mzf] [--by-user] [--on-duplicate MODE]
                    [--ftype HH] [--exec-addr N] [--strt-addr N] [--no-attrs]
```

| Volba | Výchozí | Popis |
|-------|---------|-------|
| `--mzf` | - | Exportovat jako MZF (výchozí CPM-IC $22) |
| `--by-user` | - | Vytvořit podadresáře pro každého uživatele (user00-user15) |
| `--on-duplicate MODE` | rename | Chování při duplicitním názvu: rename, overwrite, skip |
| `--ftype HH` | 22 | MZF typ souboru (hex, viz `get --mzf`) |
| `--exec-addr N` | 0x0100 | Exec adresa pro MZF (`fexec`). `--addr N` zachován jako alias. |
| `--strt-addr N` | 0x0100 | Load adresa pro MZF (`fstrt`). |
| `--no-attrs` | vypnuto | Nepřekládat CP/M atributy do bitů 7 `fname` (platí jen pro `--ftype 22`). |

Režimy `--on-duplicate`:
- `rename` - přejmenuje soubor přidáním čísla (výchozí)
- `overwrite` - přepíše existující soubor
- `skip` - přeskočí soubor

#### Validace kompatibility voleb u příkazu `get`

Nástroj striktně kontroluje, že zadané volby odpovídají aktivnímu
režimu. Nekompatibilní kombinace skončí chybou `exit 1` místo tichého
ignorování. Toto chování je konzistentní s `mzdsk-mrs`.

| Pravidlo | Chybová hláška |
|----------|-----------------|
| `--ftype`, `--exec-addr`, `--strt-addr`, `--no-attrs` vyžadují `--mzf` | `--ftype/--exec-addr/--strt-addr/--no-attrs require --mzf` |
| `--by-user` vyžaduje `--all` | `--by-user can only be used with --all` |
| `--on-duplicate` vyžaduje `--all` | `--on-duplicate can only be used with --all` |

Platné kombinace:

```bash
get <jméno> <výstup>                    # čistý raw export
get --mzf <jméno> <out.mzf>             # single-file MZF, volby --ftype atd. OK
get --all <dir>                         # batch raw export
get --all --mzf <dir>                   # batch MZF export, volby --ftype atd. OK
get --all --by-user <dir>               # batch s podadresáři user00/..
get --all --on-duplicate overwrite <dir>  # batch s přepsáním
```

#### put --mzf

Importuje MZF soubor na CP/M disketu. Z MZF hlavičky se načte jméno,
velikost a data. Atributy R/O, SYS, ARC se extrahují z bitů 7 znaků
přípony pouze pokud má soubor `ftype == 0x22` (konvence SOKODI CMT.COM);
u jiných typů se atributy nepřevádějí. Pole `fstrt` a `fexec` nejsou
při importu do CP/M potřeba a ignorují se.

```
put --mzf <vstup.mzf> [název.ext] [--charset eu|jp]
                                   [--force-charset] [--no-attrs]
```

Pokud je zadán `název.ext`, přepíše název souboru z MZF hlavičky.
Pokud není zadán, odvozený název se tvoří z `fname` v MZF hlavičce.

#### Konverze fname - přepínač `--charset`

MZF soubory s `ftype != 0x22` (typicky nativní MZ-700/MZ-800 programy:
OBJ, BTX, BSD, BRD) mají jméno uložené v **Sharp MZ ASCII**, nikoliv
ve standardním ASCII. Rozdíl se projeví u Sharp-specifických znaků
(zejména malá písmena v kódech 0x80-0xFF), které po naivním maskování
bitu 7 vyprodukují netisknutelné znaky.

Přepínač `--charset` určuje variantu Sharp MZ znakové sady použitou
při konverzi:

- `--charset eu` (výchozí) - evropská varianta (MZ-700/MZ-800)
- `--charset jp` - japonská varianta (MZ-1500)

Po konverzi se řetězec trimuje od mezer, rozdělí na 8.3 podle poslední
tečky a sanitizuje - netisknutelné znaky a mezery uvnitř jména se
nahradí `_`, aby výsledná CP/M directory entry splňovala kritéria
auto-detekce souborového systému.

Pro `ftype == 0x22` (CPM-IC export vytvořený našimi nástroji) se
přepínač ignoruje - `fname` je tam již v ASCII a interpretuje se přímo
včetně atributů v bitu 7.

#### Přepínače pro cizí MZF s `ftype == 0x22`

Drtivá většina MZF s `ftype == 0x22` je naše vlastní CPM-IC export,
kde je `fname` v ASCII a bit 7 přípony kóduje atributy. Pro edge
case scénáře (ručně editovaný MZF, MZF z cizího nástroje, forenzní
analýza) nabízí `put --mzf` dva doplňkové přepínače:

- `--force-charset` - i pro `ftype == 0x22` konvertuje `fname` přes
  `--charset` (Sharp MZ EU/JP) místo CPM-IC ASCII masky. Vhodné
  pokud je `fname` ve skutečnosti v Sharp MZ ASCII (ne v CPM-IC
  ASCII), například po ruční úpravě `ftype` bez překonvertování
  jména.
- `--no-attrs` - potlačí dekódování atributů R/O, SYS, ARC z bitu 7
  přípony. Vhodné pro cizí MZF, kde bit 7 není CP/M atribut a
  dekódování by na disk zapsalo nechtěné atributy.

Oba přepínače lze kombinovat. `--no-attrs` ovlivňuje jen chování
pro `ftype == 0x22` (u jiných typů se atributy nedekódují nikdy).
`--force-charset` bez `--mzf` skončí chybou, stejně jako `--no-attrs`.

Příklady:

```
# MZF s ftype=0x22 ale Sharp-kódovaným fname - vynutit Sharp EU dekódování
mzdsk-cpm disk.dsk put --mzf weird.mzf --force-charset --charset eu

# MZF z cizího nástroje - bit 7 přípony nejsou atributy
mzdsk-cpm disk.dsk put --mzf foreign.mzf --no-attrs

# Oba edge cases najednou
mzdsk-cpm disk.dsk put --mzf unknown.mzf --force-charset --charset jp --no-attrs
```

#### Zkrácení dlouhých jmen

Pokud je zadané jméno delší než 8 znaků (nebo přípona delší než 3),
`put` a `put --mzf` jméno zkrátí na CP/M limit 8.3 a na stderr vypíší
varování ve tvaru:

```
Warning: Name 'TOOLONGNAME.EXT' was truncated to 'TOOLONGN.EXT'
Written '/cesta/soubor.bin' -> 'TOOLONGN.EXT' (1024 bytes)
```

Výsledné `Written` ukazuje skutečné on-disk jméno. Pokud zkrácené jméno
koliduje s existujícím souborem (např. dva různé vstupy zkrácené na
stejný 8.3 tvar), druhý `put` selže s chybou "File exists".

#### Striktní validace jména pomocí `--name`

Volba `--name NÁZEV.EXT` u příkazů `put` a `put --mzf` aktivuje
striktní režim - žádné tiché zkracování, žádná normalizace. Pokud
jméno porušuje CP/M pravidla, operace selže s chybovým kódem.

```
put <soubor> --name NÁZEV.EXT
put --mzf <vstup.mzf> --name NÁZEV.EXT
```

Validační pravidla:
- délka jména maximálně 8 znaků,
- délka přípony maximálně 3 znaky,
- jméno nesmí být prázdné (vstup jako `.TXT` se odmítne),
- zakázané znaky: `< > , ; : = ? * [ ] | /`, dále tečka uvnitř jména
  nebo přípony, mezera a řídicí znaky (0x00-0x1F, 0x7F).

Chybové hlášky na stderr (exit code 1):

```
Error: Name 'TOOLONGNAME.EXT' exceeds 8.3 limit (no truncation with --name)
Error: Name 'A*B.TXT' contains forbidden character '*' (no truncation with --name)
Error: Name '.TXT' has empty filename (no truncation with --name)
```

Volba `--name` nahrazuje 2. poziční argument u `put` a `put --mzf`.
Současné použití obou variant (poziční i `--name`) vrátí chybu
`Error: --name conflicts with positional <name.ext>`.

Malá písmena jsou povolena a knihovna je interně převede na velká
(CP/M konvence). Jméno `my.txt` je validní a uloží se jako `MY.TXT`.

#### chuser - změna user number existujícího souboru

Změní user number všech extentů již existujícího souboru. Zdrojový
user se bere z globální volby `--user N` (výchozí 0); cílový user
je druhý poziční argument (0-15).

```
[--user N] chuser <název.ext> <nový-user>
```

Příklady:

```
# přesun souboru z user 0 do user 3
mzdsk-cpm disk.dsk chuser HELLO.TXT 3

# přesun z user 2 do user 7 (zdroj nastaven přes --user)
mzdsk-cpm --user 2 disk.dsk chuser HELLO.TXT 7
```

Pravidla CP/M 2.2 (P-CP/M na Sharp MZ):

- User 0-15 jsou izolované namespacy; stejné jméno může existovat
  v různých user oblastech současně.
- Pokud v cílové user oblasti už soubor stejného jména existuje,
  operace skončí chybou `File exists` a disk zůstane beze změny.
- Pokud zdrojový a cílový user jsou stejné, operace je no-op
  a vrací úspěch.
- Přepisuje se pouze user byte v adresáři - datové bloky, extent
  čísla, RC ani atributy se nemění.

### Atributy

| Příkaz | Popis |
|--------|-------|
| `attr <název.ext>` | Zobrazit atributy souboru |
| `attr <název.ext> [+\|-][RSA]` | Nastavit/zrušit atributy (+R/-R, +S/-S, +A/-A) |

Atributy lze kombinovat: `+RSA`, `+S+A`, `+R-S` (nastaví R/O, zruší System).

### Blokové operace

| Příkaz | Popis |
|--------|-------|
| `dump-block N [bytes]` | Hexdump CP/M alokačního bloku |
| `get-block N <soubor> [bytes]` | Extrahovat CP/M blok(y) do souboru |
| `put-block N <soubor> [bytes] [offset]` | Zapsat soubor do CP/M bloku |

Parametr `bytes` určuje počet bajtů ke zpracování. Pokud není zadán,
zpracuje se celý blok. Parametr `offset` u `put-block` určuje počáteční
offset v rámci bloku.

Volba `dump-block`:

| Volba | Hodnoty | Výchozí | Popis |
|-------|---------|---------|-------|
| `--dump-charset MODE` | raw\|eu\|jp\|utf8-eu\|utf8-jp | raw | Znaková sada v ASCII sloupci hexdumpu |

### Diskové operace

| Příkaz | Popis |
|--------|-------|
| `dpb` | Zobrazit aktivní DPB parametry |
| `map` | Zobrazit alokační mapu bloků |
| `free` | Zobrazit volné místo na disku |
| `format` | Inicializovat prázdný adresář (vyplnit 0xE5) |
| `defrag` | Defragmentovat disk (přepsat soubory sekvenčně bez mezer) |

## Volby

| Volba | Hodnoty | Výchozí | Popis |
|-------|---------|---------|-------|
| `--format` | sd\|hd | autodetekce | Formát CP/M disku |
| `--user` | 0-15 | 0 | Číslo CP/M uživatele (dir: všichni pokud neuvedeno) |
| `--ro` | - | - | Vynucený read-only režim |
| `--force` | - | - | Obejít kontrolu, že disk je CP/M filesystém (viz níže) |
| `--output FMT` | text\|json\|csv | text | Výstupní formát (text, json, csv) |
| `-o FMT` | text\|json\|csv | text | Zkratka pro --output |
| `--version` | - | - | Zobrazit verzi programu |
| `--lib-versions` | - | - | Zobrazit verze použitých knihoven |
| `--help`, `-h` | - | - | Zobrazit nápovědu |

### Ochrana proti operacím na cizím souborovém systému

Před zahájením jakékoli operace se ověří typ souborového systému na
disku (přes `mzdsk_detect_filesystem`). Pokud disk není rozpoznán jako
CP/M (tedy je to FSMZ, MRS, jen boot track nebo neznámý formát), nástroj
operaci odmítne s chybou "disk is not a CP/M filesystem" a exit kódem 1.

Tato ochrana platí i pro čtecí příkazy (`dir`, `file`, `dump-block` atd.),
protože na cizím disku by vracely nesmyslná čísla, a pro destruktivní
příkazy je kritická (`format` by tiše přepsala 2048 B v oblasti FSMZ
FAREA, `put`/`era`/`defrag` by podobně poškozovaly data).

Pro explicitní operaci na cizím disku (např. `format` pro přeformátování
na CP/M, forenzní náhled, diagnostika) použijte `--force`. Nástroj vypíše
varování a pokračuje. `--force` musí stát před subpříkazem.

### Custom DPB parametry

Přepíšou odpovídající hodnoty z presetu zvoleného formátu.
Odvozené hodnoty (BLM, block_size, CKS) se přepočtou automaticky.

| Volba | Popis |
|-------|-------|
| `--spt N` | Logické (128B) sektory na stopu |
| `--bsh N` | Block shift factor (3-7), block_size = 128 << BSH |
| `--exm N` | Extent mask |
| `--dsm N` | Celkový počet bloků - 1 |
| `--drm N` | Počet adresářových položek - 1 |
| `--al0 N` | Alokační bitmapa adresáře - byte 0 (hex: 0xC0) |
| `--al1 N` | Alokační bitmapa adresáře - byte 1 (hex: 0x00) |
| `--off N` | Počet rezervovaných stop |

### Podrobnosti k volbám

**--format** - určuje formát CP/M disku. Výchozí chování je autodetekce
z geometrie DSK obrazu (počet sektorů na stopu a počet stran).

| Formát | Sektorů | Velikost bloku | Adresář | OFF | Popis |
|--------|---------|----------------|---------|-----|-------|
| sd | 9x512B | 2048 B | 128 | 4 | SD (1-stranný i 2-stranný) |
| hd | 18x512B | 4096 B | 128 | 4 | HD (1-stranný i 2-stranný) |

**--user N** - CP/M podporuje 16 uživatelů (0-15). Každý uživatel má
vlastní pohled na adresář. Volba ovlivňuje všechny operace. U příkazu
`dir` se bez zadání `--user` zobrazí soubory všech uživatelů.

**--dump-charset MODE** - volba příkazu `dump-block`. Nastaví konverzi Sharp MZ znakové sady v ASCII sloupci hexdumpu.

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

**Custom DPB** - umožňuje experimentování s nestandardními formáty.
Příklad: `--spt 36 --bsh 4 --dsm 350 --drm 127 --al0 0xC0 --off 4`

---

## Příklady

Výpis adresáře 2-stranné SD diskety (autodetekce formátu):

```bash
mzdsk-cpm disk.dsk dir
```

Rozšířený výpis s atributy:

```bash
mzdsk-cpm disk.dsk dir --ex
```

Export a import souborů (raw binární):

```bash
mzdsk-cpm disk.dsk get PROGRAM.COM program.com
mzdsk-cpm disk.dsk put data.txt DATA.TXT
mzdsk-cpm disk.dsk put data.txt --name DATA.TXT   # striktní validace
```

Export a import ve formátu MZF s výchozími hodnotami
(typ 0x22, load/exec 0x0100, zapnuté kódování atributů):

```bash
mzdsk-cpm disk.dsk get --mzf PROGRAM.COM program.mzf
mzdsk-cpm disk.dsk get --mzf PROGRAM.COM program.mzf --exec-addr 0x0200
mzdsk-cpm disk.dsk put --mzf program.mzf
mzdsk-cpm disk.dsk put --mzf program.mzf NEWNAME.COM
```

Export s vlastním typem a adresami:

```bash
mzdsk-cpm disk.dsk get --mzf PROGRAM.COM program.mzf \
    --ftype 01 --strt-addr 0x1200 --exec-addr 0x1200
mzdsk-cpm disk.dsk get --mzf TEXT.TXT text.mzf --ftype 02 --no-attrs
```

Export všech souborů:

```bash
mzdsk-cpm disk.dsk get --all ./output
mzdsk-cpm disk.dsk get --all ./output --mzf --by-user
mzdsk-cpm disk.dsk get --all ./output --mzf --exec-addr 0x0200 --on-duplicate skip
mzdsk-cpm disk.dsk get --all ./output --mzf --ftype 01 --strt-addr 0x1200
```

**Exit code `get --all`:** 0 při úspěchu (včetně souborů přeskočených
přes `--on-duplicate skip`), 1 pokud alespoň jeden soubor nebylo možné
extrahovat kvůli skutečné chybě (např. I/O chyba, poškozený blok).
Skript tak pozná částečné selhání.

Zobrazení DPB parametrů:

```bash
mzdsk-cpm disk.dsk dpb
```

Práce s custom DPB parametry:

```bash
mzdsk-cpm --spt 36 --bsh 4 --dsm 350 --off 4 disk.dsk dir
```

Správa atributů:

```bash
mzdsk-cpm disk.dsk attr PROGRAM.COM +R +S
mzdsk-cpm disk.dsk attr PROGRAM.COM -R
```

Alokační mapa a volné místo:

```bash
mzdsk-cpm disk.dsk map
mzdsk-cpm disk.dsk free
```

Defragmentace disku:

```bash
mzdsk-cpm disk.dsk defrag
```

Blokové operace:

```bash
mzdsk-cpm disk.dsk dump-block 5
mzdsk-cpm disk.dsk dump-block 5 512
mzdsk-cpm disk.dsk get-block 10 block10.bin
mzdsk-cpm disk.dsk get-block 10 block10.bin 1024
mzdsk-cpm disk.dsk put-block 10 data.bin
mzdsk-cpm disk.dsk put-block 10 data.bin 512 128
```

---

## Nesekvenční sektory

Blokové operace (get-block, put-block, dump-block) postupují sekvenčně
podle ID sektorů (jako řadič disketové jednotky). Sektory s nesekvenčním
ID nejsou přes blokové příkazy dostupné.

Pro přístup k těmto sektorům použijte `mzdsk-raw` s volbou `--order phys`:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 10 --order phys
```
