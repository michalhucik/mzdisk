# mzdsk-create - Vytvoření DSK diskového obrazu

Verze nástroje: 1.2.5. Součást projektu [mzdisk](https://github.com/michalhucik/mzdisk/).

> **Upozornění:** Vždy pracujte s kopií DSK obrazu, ne s originálem.
> Autor projektu neposkytuje žádné záruky a neodpovídá za ztráty ani
> poškození dat způsobené používáním nástrojů projektu mzdisk. Projekt
> je distribuován pod licencí GPLv3 bez záruky.

Vytvoří nový prázdný DSK diskový obraz ve formátu Extended CPC DSK
pro počítače Sharp MZ. Podporuje předdefinované presety pro nejběžnější
diskové formáty (MZ-BASIC, CP/M SD, CP/M HD, MRS, Lemmings), parametrické
formátování souborového systému se zadaným počtem stop, a vlastní
geometrii se všemi parametry.

## Použití

```
mzdsk-create <obraz.dsk> --preset <název> [--sides N] [--overwrite]
mzdsk-create <obraz.dsk> --format-basic <stop> [--sides N] [--overwrite]
mzdsk-create <obraz.dsk> --format-cpm <stop> [--sides N] [--overwrite]
mzdsk-create <obraz.dsk> --format-cpmhd <stop> [--sides N] [--overwrite]
mzdsk-create <obraz.dsk> --format-mrs <stop> [--sides N] [--overwrite]
mzdsk-create <obraz.dsk> --custom <stop> <sektorů> <velikost> <filler> [řazení] [--sides N] [--overwrite]
mzdsk-create --version
mzdsk-create --lib-versions
```

## Argumenty

| Argument | Popis |
|----------|-------|
| `<obraz.dsk>` | Cesta k výstupnímu DSK souboru |

## Příkazy

| Příkaz | Popis |
|--------|-------|
| `--preset <název>` | Vytvořit DSK z předdefinovaného presetu |
| `--format-basic <T>` | Vytvořit a naformátovat MZ-BASIC disketu s T stopami na stranu |
| `--format-cpm <T>` | Vytvořit a naformátovat CP/M SD disketu s T stopami na stranu |
| `--format-cpmhd <T>` | Vytvořit a naformátovat CP/M HD disketu s T stopami na stranu |
| `--format-mrs <T>` | Vytvořit a naformátovat MRS disketu s T stopami na stranu |
| `--custom <T> <S> <SS> <F> [O]` | Vytvořit DSK s vlastní geometrií |

## Volby

| Volba | Hodnoty | Výchozí | Popis |
|-------|---------|---------|-------|
| `--overwrite` | - | - | Přepsat existující výstupní soubor |
| `--sides` | 1, 2 | 2 | Počet stran (u všech příkazů) |
| `--version` | - | - | Zobrazit verzi programu |
| `--lib-versions` | - | - | Zobrazit verze použitých knihoven |
| `--help` | - | - | Zobrazit nápovědu a skončit |

### Podrobnosti k presetům

Volba `--preset <název>` vytvoří DSK obraz podle jednoho z těchto
pevně definovaných formátů:

| Preset | Stop/stranu | Geometrie | Poznámka |
|--------|-------------|-----------|----------|
| `basic` | 80 | všechny stopy 16×256 B, normální řazení, filler 0xFF | Sharp MZ-800 Disc BASIC |
| `cpm-sd` | 80 | stopa 1 (boot) 16×256 B; ostatní 9×512 B, LEC řazení, filler 0xE5 | LEC CP/M 2.2 SD |
| `cpm-hd` | 80 | stopa 1 (boot) 16×256 B; ostatní 18×512 B, LEC HD řazení, filler 0xE5 | LEC CP/M 2.2 HD |
| `mrs` | 80 | stopa 1 (boot) 16×256 B; ostatní 9×512 B, LEC řazení, filler 0xE5 | MRS Vlastimila Veselého |
| `lemmings` | 80 | stopa 1 (boot) 16×256 B; ostatní 9×512 B, LEC řazení, filler 0xE5 | Speciální formát hry Lemmings |

Všechny presety mají výchozí 2 strany (160 absolutních stop). Volba `--sides 1`
vytvoří jednostranný disk (80 absolutních stop).

Presety vytváří pouze strukturu DSK (geometrii a výplň filler bytem) -
nevytvářejí obsah souborového systému (FAT, adresář, boot kód). Pro
kompletní vytvoření se systémem použijte `--format-*` příkazy.

### Podrobnosti k `--format-*`

Volby `--format-basic`, `--format-cpm`, `--format-cpmhd` a `--format-mrs`
vytvoří DSK obraz se zadaným počtem stop na stranu a zároveň inicializují
souborový systém. Počet stran se volí volbou `--sides N` (výchozí 2).

**Argument `<T>` znamená počet stop na jednu stranu.** Celkový počet
absolutních stop na disku je `T × sides`. Tato sémantika je shodná
s `--preset`, kde každý preset má T=80 stop/stranu. Příklady:

- `--format-basic 80 --sides 2` → 80×2 = 160 absolutních stop
  (odpovídá `--preset basic --sides 2`).
- `--format-basic 80 --sides 1` → 80 absolutních stop
  (odpovídá `--preset basic --sides 1`).

Historická poznámka: verze mzdsk-create < 1.2.0 interpretovala `<T>`
jako total tracks u `--format-*`, což bylo nekonzistentní s `--preset`
a `--sides` nemělo na velikost souboru vliv. Od verze 1.2.0 je sémantika
sjednocená.

- **`--format-basic <T>`** - Vytvoří MZ-BASIC disketu s T stopami na stranu
  (16×256 B na všech stopách) a inicializuje FSMZ souborový
  systém: IPLPRO blok, DINFO blok, prázdný adresář.

- **`--format-cpm <T>`** - Vytvoří LEC CP/M SD disketu s T stopami na stranu
  (9×512 B s boot stopou) a inicializuje prázdný CP/M adresář.

- **`--format-cpmhd <T>`** - Vytvoří LEC CP/M HD disketu s T stopami na stranu
  (18×512 B s boot stopou) a inicializuje prázdný CP/M adresář.

- **`--format-mrs <T>`** - Vytvoří MRS disketu s T stopami na stranu
  (9×512 B s boot stopou) a inicializuje MRS souborový systém:
  FAT tabulku a prázdný adresář.

### Podrobnosti k `--custom`

Volba `--custom` přijímá 4 povinné a 1 volitelný poziční argument:

```
--custom <T> <S> <SS> <F> [O]
```

| Argument | Rozsah | Popis |
|----------|--------|-------|
| `T` | 1-204 | Celkový počet absolutních stop (na rozdíl od `--format-*` je zde T total, ne per-side) |
| `S` | 1-29 | Počet sektorů na stopu |
| `SS` | 128, 256, 512, 1024 | Velikost sektoru v bajtech |
| `F` | 0-255 | Filler byte (výplň sektorů) |
| `O` (volitelné) | viz níže | Řazení sektorů |

**Řazení sektorů (`O`):**

| Hodnota | Popis |
|---------|-------|
| `normal` | Sekvenční 1, 2, 3, ... (výchozí) |
| `lec` | Prokládané s intervalem 2 (LEC SD formát) |
| `lechd` | Prokládané s intervalem 3 (LEC HD formát) |
| `1,3,5,2,4,6` | Vlastní mapa sektorů (čísla oddělená čárkou) |

**Počet stran** se volí volbou `--sides N` (1 nebo 2, výchozí 2).
Při `--sides 2` musí být celkový počet stop `T` sudý.

`--custom` nevytváří souborový systém, jen DSK strukturu. Vytvořený
obraz je "čistá" disketa s požadovanou geometrií vyplněná filler bytem.

### Podrobnosti k `--overwrite`

Bez této volby program odmítne přepsat existující soubor a skončí
s chybou. S volbou `--overwrite` je existující soubor nahrazen.

## Příklady

Vytvoření MZ-BASIC diskety z presetu:

```bash
mzdsk-create basic.dsk --preset basic
```

Vytvoření CP/M SD diskety z presetu:

```bash
mzdsk-create cpm.dsk --preset cpm-sd
```

Vytvoření MRS diskety z presetu:

```bash
mzdsk-create mrs.dsk --preset mrs
```

Vytvoření naformátované MZ-BASIC diskety se 40 stopami:

```bash
mzdsk-create basic40.dsk --format-basic 40
```

Vytvoření naformátované CP/M HD diskety s 80 stopami:

```bash
mzdsk-create cpmhd.dsk --format-cpmhd 80
```

Vytvoření vlastní jednostranné diskety (40 stop, 16×256 B, filler 0xE5,
normální řazení):

```bash
mzdsk-create custom.dsk --custom 40 16 256 0xe5 normal --sides 1
```

Vytvoření vlastní oboustranné diskety s LEC HD řazením:

```bash
mzdsk-create hd.dsk --custom 80 18 512 0xe5 lechd --sides 2
```

Vytvoření diskety s vlastní mapou sektorů:

```bash
mzdsk-create custom.dsk --custom 80 9 512 0xe5 1,3,5,7,9,2,4,6,8
```

Přepsání existujícího souboru:

```bash
mzdsk-create basic.dsk --preset basic --overwrite
```

Vytvoření MRS diskety a následná inicializace souborového systému
pomocí mzdsk-mrs:

```bash
mzdsk-create mrs.dsk --preset mrs
mzdsk-mrs mrs.dsk init
mzdsk-mrs mrs.dsk info
```
