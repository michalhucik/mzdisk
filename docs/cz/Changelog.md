# Changelog

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
