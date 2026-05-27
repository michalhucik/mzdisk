# Changelog

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
