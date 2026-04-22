/**
 * @file    mzdisk_cli_version.h
 * @brief   Společná release verze sady CLI nástrojů mzdisk.
 *
 * Sada CLI nástrojů mzdisk (mzdsk-info, mzdsk-create, mzdsk-fsmz,
 * mzdsk-cpm, mzdsk-mrs, mzdsk-raw, mzdsk-dsk) se releasuje jako jeden
 * celek pod společnou release verzí, která je nezávislá na release
 * verzi GUI aplikace mzdisk.
 *
 * Každá binárka si zároveň drží svou vlastní sémantickou verzi
 * (například @c MZDSK_FSMZ_VERSION), která odráží změny v daném
 * konkrétním nástroji. Při @c --version se proto vypisuje dvojice
 * "tool version" a "CLI release", aby bylo v hlášeních o chybách
 * jednoznačné, o kterou kombinaci jde.
 *
 * Při bumpu release verze CLI upravte @c MZDISK_CLI_RELEASE_VERSION
 * v tomto souboru a doplňte záznam do @c docs/cz/Changelog.md a
 * @c docs/en/Changelog.md.
 */

#ifndef MZDISK_CLI_VERSION_H
#define MZDISK_CLI_VERSION_H

/**
 * @brief Release verze celé sady CLI nástrojů mzdisk.
 *
 * Sdílená napříč všemi mzdsk-* binárkami. Nesouvisí s verzí GUI
 * aplikace mzdisk ani s individuálními verzemi jednotlivých nástrojů.
 */
#define MZDISK_CLI_RELEASE_VERSION "0.8.0"

/**
 * @brief Jméno release celé sady CLI nástrojů mzdisk.
 *
 * Používá se v hlavičkách výpisů --version / --lib-versions a při
 * generování jména portable archivu (mzdisk-cli-X.Y.Z.zip).
 */
#define MZDISK_CLI_RELEASE_NAME "mzdisk-cli"

#endif /* MZDISK_CLI_VERSION_H */
