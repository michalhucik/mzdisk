# mzdsk-info - Inspekce DSK diskového obrazu

Verze nástroje: 1.4.6. Součást projektu [mzdisk](https://github.com/michalhucik/mzdisk/).

> **Upozornění:** Vždy pracujte s kopií DSK obrazu, ne s originálem.
> Autor projektu neposkytuje žádné záruky a neodpovídá za ztráty ani
> poškození dat způsobené používáním nástrojů projektu mzdisk. Projekt
> je distribuován pod licencí GPLv3 bez záruky.

Zobrazení informací o DSK diskovém obrazu Sharp MZ bez modifikace dat.
Nástroj umí zobrazit základní informace o geometrii disku, mapu stop
a sektorů s automatickou detekcí souborového systému (FSMZ, CP/M
SD/HD/SD2S/HD2S, MRS), obsah boot sektoru, surová data jednotlivých
sektorů a obsah FSMZ alokačních bloků. Podporuje automatickou detekci
inverze dat (FSMZ formát) a konverzi Sharp MZ znakové sady na ASCII.

## Použití

```
mzdsk-info <obraz.dsk> [volby]
mzdsk-info --version
mzdsk-info --lib-versions
```

Bez příkazu zobrazuje základní informace o obrazu (geometrii, formát,
počet stop, počet sektorů, velikost sektoru).

## Argumenty

| Argument | Popis |
|----------|-------|
| `<obraz.dsk>` | Vstupní DSK diskový obraz (Extended CPC DSK formát) |

## Příkazy

| Příkaz | Popis |
|--------|-------|
| (žádný) | Zobrazit geometrii disku, formát a FSMZ informace |
| `--map` | Zobrazit mapu využití disku s auto-detekcí souborového systému (FSMZ, CP/M SD/HD/SD2S/HD2S, MRS) |
| `--boot` | Zobrazit informace o bootstrapu (IPLPRO) |
| `--sector T S` | Zobrazit hex dump sektoru na stopě T, sektor s ID S (viz pozn.) |
| `--block N` | Zobrazit hex dump FSMZ bloku N |

## Volby

| Volba | Hodnoty | Výchozí | Popis |
|-------|---------|---------|-------|
| `--output FMT` | text\|json\|csv | text | Výstupní formát (text, json, csv) |
| `-o FMT` | text\|json\|csv | text | Zkratka pro --output |
| `--charset MODE` | eu\|jp\|utf8-eu\|utf8-jp | eu | Konverze Sharp MZ znakové sady pro jména souborů |
| `--dump-charset MODE` | raw\|eu\|jp\|utf8-eu\|utf8-jp | raw | Znaková sada v ASCII sloupci hexdumpu |
| `--cnv` | - | - | Alias pro `--dump-charset eu` |
| `--nocnv` | - | - | Vynutit vypnutí konverze Sharp MZ znaků |
| `--inv` | - | - | Vynutit inverzi dat při čtení |
| `--noinv` | - | - | Vynutit vypnutí inverze dat při čtení |
| `--version` | - | - | Zobrazit verzi programu |
| `--lib-versions` | - | - | Zobrazit verze použitých knihoven |
| `--help`, `-h` | - | - | Zobrazit nápovědu |

### Podrobnosti k volbám

**--charset MODE** - Nastaví konverzi Sharp MZ znakové sady pro jména souborů
v IPLPRO bootstrap hlavičce (--boot).

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

**--dump-charset MODE** - Nastaví konverzi Sharp MZ znakové sady v ASCII
sloupci hexdumpu. Význam režimů je shodný s volbou --charset, viz výše.

**--cnv / --nocnv** - Volba `--cnv` je alias pro `--dump-charset eu`.
Volba `--nocnv` vynutí zobrazení surových dat bez konverze.
Ve výchozím stavu se konverze aplikuje automaticky podle kontextu.

**--inv / --noinv** - Disky ve formátu FSMZ (MZ-BASIC) ukládají data
s bitovou inverzí (XOR 0xFF). Ve výchozím stavu se inverze detekuje
automaticky na základě geometrie stopy (16 sektorů x 256 B = FSMZ formát).
Volbou `--inv` lze inverzi vynutit, volbou `--noinv` zakázat.

## Příklady

Zobrazení základních informací o obrazu:

```bash
mzdsk-info disk.dsk
```

Příklad výstupu:

```
=== disk.dsk ===

Format     : Extended CPC DSK
Creator    : DSKLib v1.1
Tracks     : 40
Sides      : 1
Total trk  : 40
Ident      : MZ-BASIC

Track rules:
  [  0] Track  0..39 : 16 x 256 B
```

Zobrazení mapy stop a sektorů:

```bash
mzdsk-info disk.dsk --map
```

Zobrazení obsahu boot sektoru:

```bash
mzdsk-info disk.dsk --boot
```

Příklad výstupu:

```
=== Boot sector (IPLPRO) ===

Ftype      : 0x03
IPLPRO     : IPLPRO
Filename   : "BASIC MZ800"
Fsize      : 2048 (0x0800)
Fstrt      : 0x0000
Fexec      : 0x0000
Block      : 1

=== Disk info (DINFO) ===

Volume     : 0 (master)
Farea      : 48 (0x30)
Used       : 52
Blocks     : 639
Free       : 587
```

Zobrazení hex dumpu sektoru na stopě 1, sektor 1:

```bash
mzdsk-info disk.dsk --sector 1 1
```

**Poznámka k `--sector T S`:** parametr `S` je **ID sektoru** z IDAM
(Address Mark v track headeru), ne pořadový index. U běžných presetů
(basic, cpm-sd, cpm-hd, mrs) jsou ID 1-based sekvenční, takže `S`
odpovídá pořadí. U disků s custom sector mapou (např. preset
`lemmings`, track 16 má ID `{1,6,2,7,3,8,4,9,5,21}`) musí být zadáno
skutečné ID. Pokud ID na stopě neexistuje, `mzdsk-info` vypíše výčet
dostupných ID a skončí s chybou:

```
Error: sector ID 10 not found on track 16 (available IDs: 1 6 2 7 3 8 4 9 5 21)
```

Seznam ID pro libovolnou stopu lze získat přes
`mzdsk-dsk disk.dsk tracks --abstrack T`.

Zobrazení hex dumpu FSMZ bloku 15 (DINFO):

```bash
mzdsk-info disk.dsk --block 15
```

Zobrazení hex dumpu s vynucenou konverzí Sharp MZ znaků:

```bash
mzdsk-info disk.dsk --sector 2 1 --cnv
```

Zobrazení bez konverze Sharp MZ znaků:

```bash
mzdsk-info disk.dsk --boot --nocnv
```

Zobrazení s vynucenou inverzí dat:

```bash
mzdsk-info disk.dsk --sector 2 1 --inv
```

Zobrazení verzí knihoven:

```bash
mzdsk-info --lib-versions
```
