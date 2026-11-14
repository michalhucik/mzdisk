# Changelog

## 2026-11-14

### Délka dat sektoru v DSK obrazech, make install

Opravné vydání.

- **DSK obrazy mají vyplněnou délku dat sektoru.** Formát Extended CPC
  DSK ukládá u každého sektoru v Sector Info skutečnou délku dat
  (offset 6-7). Nástroje mzdisk toto pole dosud nechávaly nulové, takže
  každý vytvořený obraz deklaroval sektory nulové délky. Nástroje
  mzdisk takové obrazy čtou bez potíží, programy, které se polem
  řídí, ale mohou sektory vidět jako prázdné. Nově se délka zapisuje
  při tvorbě obrazu i při změnách geometrie (CLI i GUI).
- **Oprava starších obrazů:** `mzdsk-dsk <obraz.dsk> repair` doplní
  chybějící délky. Sektorová data, ID sektorů ani FDC statusy se
  nemění.
- **`mzdsk-dsk check`** hlásí nulovou délku jako opravitelnou chybu
  `ZERO_DATALEN`. **Změna chování:** nad obrazem vytvořeným starší
  verzí proto `check` vrací 1, dokud se neprovede `repair`.
- **`mzdsk-dsk tracks`** vypisuje u sektorů nový sloupec `DataLen`.
- **`make install` a `make uninstall`:** souhrnné cíle pro instalaci
  CLI nástrojů a jejich dokumentace (`install-cli` +
  `install-cli-docs`). GUI se neinstaluje, distribuuje se jako
  adresář z `make portable-gui`.
- **Testy:** dočasné adresáře se berou z prostředí (`TMPDIR`, `TEMP`,
  `TMP`) místo natvrdo `/tmp`; na Windows už testy nevyžadují adresář
  `\tmp` na aktuálním disku.

**Release:**

- **mzdisk GUI v0.8.3**
- **mzdisk CLI v0.8.3**

**Změněné verze v tomto release:**

| Komponenta        | Verze          |
|-------------------|----------------|
| `mzdsk-dsk`       | 1.5.7 -> 1.6.0 |
| `dsk` (lib)       | 2.0.7 -> 2.1.0 |
| `dsk_tools` (lib) | 3.3.0 -> 3.4.0 |

Ostatní CLI nástroje a knihovny beze změny.

## 2026-11-10

### Opravy typů souborů FSMZ, sestavení na macOS a CI

Opravné vydání.

- **`mzdsk-fsmz chtype`** odmítne typ `00`. Dříve prošel a položku
  označil jako smazanou: soubor zmizel z adresáře, ale jeho bloky
  zůstaly obsazené. Platný rozsah je `01`-`FF`, stejně jako u
  `set --ftype`.
- **GUI, FSMZ adresář:** typ souboru `05` se zobrazoval jako "BRD"
  (stejně jako `04`), správně je "RB".
- **Dokumentace CLI:** opraveny zkratky typů souborů a příklad výstupu
  `dir` u `mzdsk-fsmz`, ID sektoru v příkladu Lemmings u `mzdsk-raw`
  a příklad `--format-basic`; doplněny volby `--overwrite` a
  `--yes` / `-y` u `mzdsk-fsmz`, `mzdsk-cpm` a `mzdsk-mrs`. README
  uvádělo `--format json|csv` místo správného `--output json|csv`.
- **macOS:** knihovny, CLI nástroje a testy jdou sestavit i na macOS.
  GUI se tam zatím nesestavuje.
- **CI:** každý push do `main` na GitHubu sestaví projekt a spustí
  testy na Linuxu, macOS a Windows (GitHub Actions).
- Knihovna `sharpmz_ascii` povýšena na 2.2.1 (nové znakové sady
  KOI8-CS a display kódy; mzdisk je zatím nepoužívá, chování nástrojů
  se nemění).

**Release:**

- **mzdisk GUI v0.8.2**
- **mzdisk CLI v0.8.2**

**Změněné verze v tomto release:**

| Komponenta            | Verze            |
|-----------------------|------------------|
| `mzdsk-fsmz`          | 1.14.2 -> 1.14.3 |
| `sharpmz_ascii` (lib) | 2.1.1 -> 2.2.1   |

Ostatní CLI nástroje a knihovny beze změny.

## 2026-05-27

### Oprava čtení MRS disků s originálním layoutem (Veselý 1993)

Bugfix release - opravuje čtení disků naformátovaných originálním
MRS driverem (typicky 720 KB DSK obrazy s 54+ soubory). Před opravou
se v adresáři objevovaly nesmyslné záznamy poskládané z FAT bajtů
a soubory s datovými bloky >= 1024 byly nečitelné ("Format error"
v GUI při exportu).

**Release:**

- **mzdisk GUI v0.8.1**
- **mzdisk CLI v0.8.1**

**Změněné verze v tomto release:**

| Komponenta         | Verze            |
|--------------------|------------------|
| `mzdsk-mrs`        | 2.12.0 -> 2.12.1 |
| `mzdsk_mrs` (lib)  | 2.5.0 -> 2.6.0   |

Ostatní CLI nástroje a knihovny beze změny.

## 2026-04-22

### Zveřejnění projektu na GitHubu

První veřejné vydání projektu **mzdisk** - sady nástrojů a GUI
aplikace pro správu diskových obrazů počítačů Sharp MZ-700,
Sharp MZ-800 a Sharp MZ-1500. Projekt navazuje na starší
**FSTOOLS** a nahrazuje jej.

**Release:**

- **mzdisk GUI v0.8.0**
- **mzdisk CLI v0.8.0**

**Verze jednotlivých CLI nástrojů v tomto release:**

| Nástroj        | Verze   |
|----------------|---------|
| `mzdsk-info`   | 1.4.6   |
| `mzdsk-create` | 1.2.5   |
| `mzdsk-dsk`    | 1.5.7   |
| `mzdsk-fsmz`   | 1.14.2  |
| `mzdsk-cpm`    | 1.19.1  |
| `mzdsk-mrs`    | 2.12.0  |
| `mzdsk-raw`    | 2.2.10  |
