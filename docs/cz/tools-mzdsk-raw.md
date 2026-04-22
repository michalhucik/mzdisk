# mzdsk-raw - Surový přístup k sektorům DSK obrazu

Verze nástroje: 2.2.10. Součást projektu [mzdisk](https://github.com/michalhucik/mzdisk/).

> **Upozornění:** Vždy pracujte s kopií DSK obrazu, ne s originálem.
> Autor projektu neposkytuje žádné záruky a neodpovídá za ztráty ani
> poškození dat způsobené používáním nástrojů projektu mzdisk. Projekt
> je distribuován pod licencí GPLv3 bez záruky.

Nástroj pro přímý nízkoúrovňový přístup k sektorům a blokům DSK diskového
obrazu. Umožňuje export/import dat do/ze souboru a hexdump výpis
s konfigurovatelnými režimy adresování (Track/Sector a Block), řazením
sektorů (ID/Phys), inverzí dat, byte/file offsety.

Dále umožňuje měnit geometrii jednotlivých stop, přidávat stopy na konec
obrazu a zmenšovat obraz odstraněním stop.

Nástroj pracuje přímo s DSK vrstvou bez znalosti souborového systému.

**Atomicita zápisu:** Zápisové operace (`put`, `change-track`, `append-tracks`,
`shrink`) pracují atomicky - celý DSK obraz se načte do paměti, operace se
provedou v RAM a výsledek se zapíše zpět jediným voláním až po úspěšném
dokončení. Pokud operace selže uprostřed, původní soubor zůstává nezměněn.
I tak doporučujeme pro kritická data udržovat zálohy - dokončené změny jsou
nevratné.

## Použití

```
mzdsk-raw [volby] <obraz.dsk> <příkaz> [argumenty...] [volby příkazu]
```

## Argumenty

| Argument | Popis |
|----------|-------|
| `<obraz.dsk>` | Vstupní/výstupní DSK diskový obraz |

## Příkazy

| Příkaz | Popis |
|--------|-------|
| `get <soubor> [volby]` | Export sektorů/bloků do souboru |
| `put <soubor> [volby]` | Import dat ze souboru na disk |
| `dump [volby]` | Hexdump sektorů/bloků na stdout |
| `change-track <T> <SECS> <SSIZE> <FILLER> [ORDER\|MAP]` | Změnit geometrii stopy T |
| `append-tracks <N> <SECS> <SSIZE> <FILLER> [ORDER\|MAP]` | Přidat stopy na konec obrazu |
| `shrink <N>` | Zmenšit obraz na zadaný počet absolutních stop |

## Globální volby

| Volba | Popis |
|-------|-------|
| `--inv` | Vynutit inverzi dat (XOR 0xFF) |
| `--noinv` | Zakázat inverzi dat |
| `--overwrite` | Povolit `get` přepsat existující výstupní soubor (výchozí: odmítnout s chybou; při `--file-offset > 0` se ignoruje - embed režim) |
| `--version` | Zobrazit verzi programu |
| `--lib-versions` | Zobrazit verze použitých knihoven |
| `-h`, `--help` | Zobrazit nápovědu |

---

## Adresovací režimy

### Track/Sector režim (výchozí)

Přímé adresování stopou a sektorem. Volby:

| Volba | Výchozí | Popis |
|-------|---------|-------|
| `--track T` | 0 | Počáteční stopa |
| `--sector S` | 1 | Počáteční sektor ID (1-based) |
| `--sectors N` | 1 | Počet sektorů |

### Block režim

Aktivuje se přítomností `--block`. Přepočítává číslo bloku na
track/sector pomocí konfigurace origin, first-block a sectors-per-block.

| Volba | Výchozí | Popis |
|-------|---------|-------|
| `--block B` | - | Počáteční blok (aktivuje blokový režim) |
| `--blocks N` | 1 | Počet bloků |
| `--origin-track T` | 0 | Origin stopa (kde začíná blok first-block) |
| `--origin-sector S` | 1 | Origin sektor (1-based) |
| `--first-block N` | 0 | Číslo prvního bloku na origin pozici |
| `--spb N` | 1 | Sectors per block |

**Poznámka:** `--block` a `--track`/`--sector`/`--sectors` se vzájemně vylučují.

---

## Pořadí sektorů

Volba `--order id|phys` řídí, jak se postupuje od jednoho sektoru k dalšímu:

- **id** (výchozí) - sekvenčně podle ID sektoru (1, 2, 3, ...). Na konci
  stopy přejde na další stopu od ID 1. Emuluje chování řadiče disketové
  jednotky. Sektory s nesekvenčním ID (např. LEMMINGS sektor 22 na stopě
  s ID 1-9) jsou přeskočeny.

- **phys** - podle fyzické pozice v DSK obrazu (pole sinfo[] v hlavičce
  stopy). Zachycuje všechny sektory včetně nesekvenčních.

Na standardních Sharp MZ discích (sekvenční ID 1..N) se obě varianty
chovají identicky. Rozdíl nastává u disků s nestandardním číslováním sektorů.

---

## Datové volby

| Volba | Výchozí | Popis |
|-------|---------|-------|
| `--byte-offset N` | 0 | Offset v prvním sektoru/bloku (bajty) |
| `--byte-count N` | 0 (vše) | Celkový počet bajtů k přenesení |
| `--file-offset N` | 0 | Offset v souboru (bajty) - viz poznámka níže |
| `--dump-charset MODE` | raw | Znaková sada v ASCII sloupci hexdumpu (pouze dump) |
| `--cnv` | - | Alias pro `--dump-charset eu` |

**--file-offset N** - Offset v souboru (bajty), od kterého se čte (při `put`)
nebo ve kterém začíná zápis (při `get`).

- Při `put`: pokud je `--file-offset` roven nebo větší než velikost
  vstupního souboru, operace se odmítne s chybou (exit != 0). Sektor na
  disku se nezmění. Pokud během čtení dojde k EOF uprostřed, zbytek
  cílového rozsahu se dopadne nulami a vypíše se varování.
- Při `get`: pokud je `--file-offset` za koncem souboru, mezera mezi
  koncem a offsetem se vyplní nulami (soubor se prodlouží).

**--dump-charset MODE** - Nastaví konverzi Sharp MZ znakové sady v ASCII sloupci hexdumpu.

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

**--cnv** - Alias pro `--dump-charset eu`.

---

## get - Export sektorů/bloků

Přečte data z disku a uloží je do souboru.

```
mzdsk-raw [globální volby] <obraz.dsk> get <soubor> [volby]
```

### Ochrana před přepsáním

Pokud výstupní soubor existuje, `get` standardně selže s chybou:

```
Error: Output file 'data.bin' already exists. Use --overwrite to replace it.
```

Přepis lze povolit globální volbou `--overwrite` (konzistentní
kontrakt s nástroji `mzdsk-fsmz`, `mzdsk-cpm`, `mzdsk-mrs`). Výjimka:
při `--file-offset N` s `N > 0` jde o embed režim (vložení dat do
již existujícího binárního souboru), kde je pre-existence očekávaná
a `--overwrite` se nevyžaduje.

### Příklady

Export 16 sektorů ze stopy 0:

```bash
mzdsk-raw disk.dsk get data.bin --track 0 --sector 1 --sectors 16
```

Export jednoho CP/M bloku (2 sektory od stopy 4):

```bash
mzdsk-raw disk.dsk get block0.bin --block 0 --origin-track 4 --origin-sector 1 --spb 2
```

Export s vynucenou inverzí:

```bash
mzdsk-raw --inv disk.dsk get sector.bin --track 1 --sector 1
```

Export s byte offsetem (přeskočí prvních 128 bajtů):

```bash
mzdsk-raw disk.dsk get data.bin --track 0 --sector 1 --byte-offset 128
```

Export do souboru od offsetu 1024 (embed - existující soubor OK):

```bash
mzdsk-raw disk.dsk get output.bin --track 0 --sector 1 --sectors 4 --file-offset 1024
```

Přepsat existující výstupní soubor:

```bash
mzdsk-raw --overwrite disk.dsk get data.bin --track 0 --sector 1
```

---

## put - Import dat na disk

Zapíše data ze souboru do sektorů na disku. Pro částečné zápisy
(byte-offset nebo byte-count menší než celý sektor) nejprve přečte
existující sektor, přepíše relevantní bajty a zapíše zpět.

```
mzdsk-raw <obraz.dsk> put <soubor> [volby]
```

### Příklady

Import souboru do 16 sektorů od stopy 0:

```bash
mzdsk-raw disk.dsk put data.bin --track 0 --sector 1 --sectors 16
```

Import do CP/M bloku:

```bash
mzdsk-raw disk.dsk put block0.bin --block 0 --origin-track 4 --origin-sector 1 --spb 2
```

Import s vynucenou inverzí:

```bash
mzdsk-raw --inv disk.dsk put data.bin --track 1 --sector 1
```

---

## dump - Hexdump na stdout

Vypíše formátovaný hexdump sektorů/bloků na standardní výstup.
Před každým sektorem vypíše hlavičku s číslem stopy, sektoru,
velikostí a stavem inverze.

```
mzdsk-raw <obraz.dsk> dump [volby]
```

### Příklady

Hexdump dvou sektorů ze stopy 0:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 2
```

Hexdump s inverzí a Sharp MZ ASCII konverzí:

```bash
mzdsk-raw --inv disk.dsk dump --track 0 --sector 1 --sectors 16 --cnv
```

Hexdump v block režimu:

```bash
mzdsk-raw disk.dsk dump --block 0 --blocks 2 --origin-track 4 --spb 2
```

Hexdump s fyzickým pořadím sektorů:

```bash
mzdsk-raw disk.dsk dump --track 0 --sector 1 --sectors 10 --order phys
```

---

## change-track - Změna geometrie stopy

Změní geometrii jedné stopy v existujícím DSK obrazu.
Pokud se změní velikost stopy, data následujících stop se přesunou.
Novou stopu vyplní filler bytem.

```
mzdsk-raw <obraz.dsk> change-track <stopa> <sektory> <velikost_sektoru> <filler> [order|map]
```

| Argument | Popis |
|----------|-------|
| `<stopa>` | Absolutní číslo stopy, kterou chceme změnit |
| `<sektory>` | Nový počet sektorů na stopě |
| `<velikost_sektoru>` | Velikost sektoru v bajtech (128, 256, 512 nebo 1024) |
| `<filler>` | Filler bajt pro nové sektory (např. 0xE5, 0xFF, 0x00) |
| `[order\|map]` | Volitelné pořadí sektorů (viz níže) |

### Pořadí sektorů

- `normal` - sekvenční číslování (1, 2, 3, ...)
- `lec` - prokládané pořadí pro standardní CP/M disky
- `lechd` - prokládané pořadí pro vysokohustotní CP/M disky
- čárkami oddělená ID - vlastní mapa sektorů (např. `1,3,5,2,4,6`)

### Příklady

```bash
mzdsk-raw disk.dsk change-track 5 9 512 0xE5
mzdsk-raw disk.dsk change-track 0 16 256 0xFF
mzdsk-raw disk.dsk change-track 2 9 512 0xE5 lec
mzdsk-raw disk.dsk change-track 3 6 512 0xE5 1,3,5,2,4,6
```

---

## append-tracks - Přidání stop

Přidá zadaný počet stop na konec existujícího DSK obrazu.

```
mzdsk-raw <obraz.dsk> append-tracks <počet> <sektory> <velikost_sektoru> <filler> [order|map]
```

| Argument | Popis |
|----------|-------|
| `<počet>` | Počet přidávaných stop |
| `<sektory>` | Počet sektorů na každé nové stopě |
| `<velikost_sektoru>` | Velikost sektoru v bajtech (128, 256, 512 nebo 1024) |
| `<filler>` | Filler bajt pro nové sektory |
| `[order\|map]` | Volitelné pořadí sektorů |

### Příklady

```bash
mzdsk-raw disk.dsk append-tracks 10 16 256 0xFF
mzdsk-raw disk.dsk append-tracks 40 9 512 0xE5 lec
```

---

## shrink - Zmenšení obrazu

Zmenší DSK obraz na zadaný celkový počet absolutních stop.

```
mzdsk-raw <obraz.dsk> shrink <celkový_počet_stop>
```

### Příklady

```bash
mzdsk-raw disk.dsk shrink 40
```

---

## Příklady

Záloha a obnova sektoru:

```bash
mzdsk-raw disk.dsk get backup.bin --track 5 --sector 3
mzdsk-raw disk.dsk put backup.bin --track 5 --sector 3
```

Roundtrip test:

```bash
mzdsk-raw disk.dsk get orig.bin --track 0 --sector 1 --sectors 16
mzdsk-raw disk.dsk put orig.bin --track 0 --sector 1 --sectors 16
```

Export celé stopy ve fyzickém pořadí:

```bash
mzdsk-raw disk.dsk get track0.bin --track 0 --sector 1 --sectors 16 --order phys
```

Zobrazení verze:

```bash
mzdsk-raw --version
mzdsk-raw --lib-versions
```
