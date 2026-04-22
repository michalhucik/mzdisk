# mzdsk-dsk - Diagnostika a editace DSK kontejneru

Verze nástroje: 1.5.7. Součást projektu [mzdisk](https://github.com/michalhucik/mzdisk/).

> **Upozornění:** Vždy pracujte s kopií DSK obrazu, ne s originálem.
> Autor projektu neposkytuje žádné záruky a neodpovídá za ztráty ani
> poškození dat způsobené používáním nástrojů projektu mzdisk. Projekt
> je distribuován pod licencí GPLv3 bez záruky.

Nástroj pro diagnostiku, inspekci, opravu a editaci Extended CPC DSK
diskových obrazů na úrovni kontejneru. Pracuje přímo s DSK hlavičkou
a track headery bez znalosti souborového systému a bez automatické
opravy při otevírání (na rozdíl od ostatních nástrojů mzdsk-*).

Nástroj je určen pro pokročilé uživatele, kteří potřebují analyzovat
nebo opravit poškozené DSK obrazy, editovat metadata (creator, GAP#3,
FDC statusy) nebo ověřit konzistenci obrazu.

**Atomicita zápisu:** Zápisové operace (`repair`, `edit-header`, `edit-track`)
pracují atomicky - celý DSK obraz se načte do paměti, operace se provedou
v RAM a výsledek se zapíše zpět jediným voláním až po úspěšném dokončení.
Pokud operace selže uprostřed, původní soubor zůstává nezměněn. I tak
doporučujeme pro kritická data udržovat zálohy - dokončené změny jsou
nevratné.

**Ochrana proti nevalidním vstupům:** Nástroj na začátku ověřuje DSK
identifikační řetězec (`EXTENDED CPC DSK File\r\nDisk-Info\r\n`). Pokud
soubor nemá tento magic, všechny příkazy kromě `check` skončí s chybou
a exit kódem 1, aby se zabránilo modifikaci libovolného souboru přes
`edit-header`/`edit-track` nebo nesmyslnému výstupu z `info`/`tracks`.
Příkaz `check` tento magic test vynechává úmyslně - hlásí problém přes
flag `BAD_FILEINFO` v diagnostickém výstupu.

## Použití

```
mzdsk-dsk [volby] <obraz.dsk> [volby] <příkaz> [argumenty...]
```

## Argumenty

| Argument | Popis |
|----------|-------|
| `<obraz.dsk>` | Vstupní/výstupní DSK diskový obraz |

## Příkazy

| Příkaz | Popis |
|--------|-------|
| `info` | Zobrazit informace z DSK hlavičky |
| `tracks [--abstrack T]` | Zobrazit detaily track headerů |
| `check` | Diagnostika bez opravy (exit code 0 = OK, 1 = chyby) |
| `repair` | Diagnostika s opravou opravitelných chyb |
| `edit-header --creator TEXT` | Editace DSK hlavičky |
| `edit-track T [volby]` | Editace track headeru |

## Volby

| Volba | Popis |
|-------|-------|
| `--version` | Zobrazit verzi programu |
| `--lib-versions` | Zobrazit verze použitých knihoven |
| `--help` | Zobrazit nápovědu |

---

## info - Informace o DSK hlavičce

Zobrazí raw obsah DSK hlavičky: identifikační řetězec (file_info),
creator, počet stop, počet stran, velikost souboru a kompletní
tabulku tsize pro všechny stopy. Pokud existují trailing data
za poslední stopou, zobrazí jejich offset a velikost.

```
mzdsk-dsk <obraz.dsk> info
```

### Příklady

Zobrazení informací o DSK obrazu:

```bash
mzdsk-dsk disk.dsk info
```

Ukázkový výstup:

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

## tracks - Detail track headerů

Zobrazí detailní informace z hlavičky jedné nebo všech stop:
číslo stopy, strana, velikost sektoru, počet sektorů, GAP#3,
filler byte a seznam sektorů včetně FDC stavových registrů.

```
mzdsk-dsk <obraz.dsk> tracks [--abstrack T]
```

| Volba | Popis |
|-------|-------|
| `--abstrack T` | Zobrazit pouze stopu T (absolutní číslo) |

### Příklady

Zobrazení všech stop:

```bash
mzdsk-dsk disk.dsk tracks
```

Zobrazení pouze stopy 0:

```bash
mzdsk-dsk disk.dsk tracks --abstrack 0
```

Ukázkový výstup:

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

## check - Diagnostika

Provede kompletní diagnostiku DSK obrazu bez jakékoliv modifikace.
Kontroluje platnost file_info identifikátoru, shodu počtu stop
v hlavičce se skutečností, konzistenci tsize polí, čísla stop
a stran v track headerech, platnost počtu sektorů a velikostí
sektorů, čitelnost sektorových dat a existenci trailing dat.

Vrací exit code 0 pokud je obraz v pořádku, 1 pokud nalezne chyby.

```
mzdsk-dsk <obraz.dsk> check
```

### Klasifikace chyb

**Opravitelné chyby** (příkaz `repair` je zvládne opravit):
- `BAD_TRACKCOUNT` - špatný počet stop v DSK hlavičce
- `BAD_TSIZE` - špatné tsize pole v DSK hlavičce
- `TRAILING_DATA` - data za poslední stopou

**Fatální chyby** (automatická oprava není možná):
- `BAD_FILEINFO` - neplatný file_info identifikátor
- `BAD_TRACK_NUM` - špatné číslo stopy v track headeru
- `BAD_SIDE_NUM` - špatné číslo strany v track headeru
- `BAD_SECTORS` - neplatný počet sektorů
- `BAD_SSIZE` - neplatná kódovaná velikost sektoru
- `DATA_UNREADABLE` - sektorová data nelze přečíst

**Informativní vlajky** (nejsou klasifikovány jako chyba - `check` vrátí 0):
- `ODD_DOUBLE` - 2-sided obraz má lichý počet stop (neobvyklá geometrie)
- `TRACK_ERRORS` - souhrnná vlajka indikující přítomnost per-track chyb
  (konkrétní chyby jsou vypsány v sekci "Track issues")

Per-track úrovně vlajek (vypisují se v sekci "Track issues" u poškozených stop):
`NO_TRACKINFO` (chybí Track-Info identifikátor), `READ_ERROR` (chyba čtení dat stopy),
`BAD_TRACK_NUM`, `BAD_SIDE_NUM`, `BAD_SECTORS`, `BAD_SSIZE`, `BAD_TSIZE`,
`DATA_UNREADABLE`.

### Příklady

Diagnostika DSK obrazu:

```bash
mzdsk-dsk disk.dsk check
```

Ukázkový výstup pro konzistentní obraz:

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

Ukázkový výstup s nalezenými chybami:

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

## repair - Oprava

Provede diagnostiku a následně opraví všechny opravitelné chyby:

- **BAD_TRACKCOUNT** - nesprávný počet stop v DSK hlavičce
- **BAD_TSIZE** - nesprávná hodnota tsize pro některou stopu
- **TRAILING_DATA** - data za poslední stopou; soubor je zkrácen na
  `expected_image_size` (hlavička + součet skutečných tsize všech stop)

Fatální chyby (špatná čísla stop/stran v track headerech, neplatné
sektory, příliš malý soubor) nelze opravit automaticky.

```
mzdsk-dsk <obraz.dsk> repair
```

Vrací exit code 0 pokud je obraz opraven nebo v pořádku,
1 pokud zůstávají neopravitelné chyby.

**Kaskádové opravy:** některé chyby se odhalí až po opravě jiné
(typicky `TRAILING_DATA` vyplyne až po opravě `BAD_TRACKCOUNT`,
protože se přepočítá `expected_image_size`). Jedno volání `repair`
proto iteruje dvojici diagnose/repair, dokud nezmizí všechny
opravitelné chyby (max 4 průchody). Uživatel nemusí `repair`
spouštět vícekrát - první výstup hlásí `Repair completed
successfully (N pass[es])` s uvedeným počtem iterací.

### Příklady

Oprava DSK obrazu:

```bash
mzdsk-dsk disk.dsk repair
```

---

## edit-header - Editace DSK hlavičky

Umožňuje změnit pole creator v DSK hlavičce. Creator je 14znakový
řetězec identifikující program, který obraz vytvořil.

```
mzdsk-dsk <obraz.dsk> edit-header --creator TEXT
```

| Volba | Popis |
|-------|-------|
| `--creator TEXT` | Nový creator řetězec (max 14 znaků) |

### Příklady

Změna creatoru:

```bash
mzdsk-dsk disk.dsk edit-header --creator "MyTool v1.0"
```

---

## edit-track - Editace track headeru

Umožňuje upravit metadata v hlavičce jedné stopy: číslo stopy,
stranu, GAP#3, filler byte nebo FDC stavové registry pro konkrétní
sektor. Hodnoty, které nejsou zadány, zůstanou nezměněny.

```
mzdsk-dsk <obraz.dsk> edit-track <stopa> [volby]
```

| Argument | Popis |
|----------|-------|
| `<stopa>` | Absolutní číslo stopy |

| Volba | Popis |
|-------|-------|
| `--track-num N` | Nastavit číslo stopy v track headeru (0-255) |
| `--side N` | Nastavit stranu (0 nebo 1) |
| `--gap HH` | Nastavit GAP#3 (hexadecimálně, plný rozsah 00-FF včetně FF) |
| `--filler HH` | Nastavit filler byte (hexadecimálně, plný rozsah 00-FF včetně FF) |
| `--fdc-status IDX STS1 STS2` | Nastavit FDC stavové registry pro sektor IDX |
| `--sector-ids ID,ID,...` | Nastavit všechna sektorová ID najednou (čárkami oddělená) |
| `--sector-id IDX:ID` | Nastavit jedno sektorové ID na indexu IDX (opakovatelné) |

### Příklady

Změna GAP#3 na stopě 0:

```bash
mzdsk-dsk disk.dsk edit-track 0 --gap 52
```

Oprava čísla stopy a strany:

```bash
mzdsk-dsk disk.dsk edit-track 5 --track-num 2 --side 1
```

Nastavení filler byte:

```bash
mzdsk-dsk disk.dsk edit-track 0 --filler e5
mzdsk-dsk disk.dsk edit-track 0 --filler ff   # typický filler 5.25" disket
```

Nastavení FDC stavových registrů pro sektor 3 na stopě 0:

```bash
mzdsk-dsk disk.dsk edit-track 0 --fdc-status 3 40 00
```

Nastavení všech sektorových ID na obrácené pořadí:

```bash
mzdsk-dsk disk.dsk edit-track 0 --sector-ids 9,8,7,6,5,4,3,2,1
```

**Poznámka:** `--sector-ids` vyžaduje přesně tolik ID, kolik má stopa
sektorů. Při neshodě nástroj selže a vypíše skutečný počet sektorů
i počet dodaných ID:

```
Error: Cannot set sector IDs for track 0: track has 16 sectors, but 3 IDs were provided
```

Počet sektorů stopy lze zjistit pomocí `mzdsk-dsk disk.dsk tracks
--abstrack T`.

Změna jednoho sektorového ID (index 0 na ID 42):

```bash
mzdsk-dsk disk.dsk edit-track 0 --sector-id 0:42
```

---

## Příklady

Kompletní kontrola DSK obrazu:

```bash
mzdsk-dsk disk.dsk check
```

Diagnostika a oprava:

```bash
mzdsk-dsk disk.dsk repair
```

Inspekce DSK hlavičky:

```bash
mzdsk-dsk disk.dsk info
```

Inspekce konkrétní stopy:

```bash
mzdsk-dsk disk.dsk tracks --abstrack 5
```

Změna creatoru a ověření:

```bash
mzdsk-dsk disk.dsk edit-header --creator "MZDisk v1.0"
mzdsk-dsk disk.dsk info
```

Zobrazení verze programu:

```bash
mzdsk-dsk --version
```

Zobrazení verzí knihoven:

```bash
mzdsk-dsk --lib-versions
```
