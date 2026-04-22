/**
 * @file   mzdsk_hexdump.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Knihovna pro formátovaný hexdump výpis binárních dat.
 *
 * Poskytuje konfigurovatelný hexdump renderer s podporou volitelné
 * inverze (XOR 0xFF) a konverze Sharp MZ znakové sady v ASCII sloupci.
 * Výstup směřuje na libovolný FILE* stream (stdout, stderr, soubor).
 *
 * Typické použití:
 * @code
 *   st_MZDSK_HEXDUMP_CFG cfg;
 *   mzdsk_hexdump_init ( &cfg );
 *   cfg.inv = 1;  // invertovat data před zobrazením
 *   cfg.charset = MZDSK_HEXDUMP_CHARSET_EU;  // Sharp MZ EU konverze
 *   mzdsk_hexdump ( &cfg, data, length );
 * @endcode
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MZDSK_HEXDUMP_H
#define MZDSK_HEXDUMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>


/** @brief Verze knihovny mzdsk_hexdump. */
#define MZDSK_HEXDUMP_VERSION "1.2.1"


/**
 * @brief Režim konverze znakové sady v ASCII sloupci hexdumpu.
 *
 * Určuje, jak se konvertují bajty na znaky v ASCII sloupci.
 * Nekonvertovatelné a netisknutelné znaky se ve všech režimech
 * kromě RAW zobrazují jako '.'.
 */
typedef enum en_MZDSK_HEXDUMP_CHARSET {
    MZDSK_HEXDUMP_CHARSET_RAW = 0,     /**< Standardní ASCII (0x20-0x7E), výchozí. */
    MZDSK_HEXDUMP_CHARSET_EU,          /**< Sharp MZ EU -> ASCII (jednobajtová konverze). */
    MZDSK_HEXDUMP_CHARSET_JP,          /**< Sharp MZ JP -> ASCII (jednobajtová konverze). */
    MZDSK_HEXDUMP_CHARSET_UTF8_EU,     /**< Sharp MZ EU -> UTF-8 (plná konverze). */
    MZDSK_HEXDUMP_CHARSET_UTF8_JP,     /**< Sharp MZ JP -> UTF-8 (plná konverze). */
} en_MZDSK_HEXDUMP_CHARSET;


/** @brief Výchozí počet sloupců v hexdump řádku. */
#define MZDSK_HEXDUMP_DEFAULT_COLS           16

/** @brief Výchozí interval pro vizuální oddělení skupin hex sloupců. */
#define MZDSK_HEXDUMP_DEFAULT_COL_SEPARATOR  8

/** @brief Výchozí počet mezer mezi hex a ASCII sloupci. */
#define MZDSK_HEXDUMP_DEFAULT_ASCII_SEPARATOR 3


/**
 * @brief Konfigurace hexdump výstupu.
 *
 * Struktura sdružuje všechny parametry ovlivňující formátování
 * hexdump výstupu. Před použitím ji inicializujte voláním
 * mzdsk_hexdump_init(), které nastaví rozumné výchozí hodnoty.
 * Poté můžete přepsat libovolné členy.
 *
 * @invariant cols musí být v rozsahu 1..256.
 * @invariant col_separator musí být >= 1.
 * @invariant ascii_separator musí být >= 0.
 * @invariant out nesmí být NULL v okamžiku volání mzdsk_hexdump().
 */
typedef struct st_MZDSK_HEXDUMP_CFG {
    FILE *out;              /**< Výstupní stream (výchozí: stdout). */
    uint16_t cols;          /**< Počet bajtů na řádek (výchozí: 16). */
    uint16_t col_separator; /**< Po kolika bajtech vložit mezeru navíc (výchozí: 8). */
    uint16_t ascii_separator; /**< Počet mezer mezi hex a ASCII sloupci (výchozí: 3). */
    int inv;                /**< 1 = XOR 0xFF před zobrazením, 0 = beze změny. */
    en_MZDSK_HEXDUMP_CHARSET charset; /**< Režim konverze znakové sady v ASCII sloupci. */
} st_MZDSK_HEXDUMP_CFG;


/**
 * @brief Inicializuje konfiguraci hexdumpu na výchozí hodnoty.
 *
 * Nastaví: out=stdout, cols=16, col_separator=8, ascii_separator=3,
 * inv=0, charset=MZDSK_HEXDUMP_CHARSET_RAW.
 *
 * @param[out] cfg Ukazatel na konfiguraci. Nesmí být NULL.
 *
 * @post Všechny členy cfg mají definované výchozí hodnoty.
 */
extern void mzdsk_hexdump_init ( st_MZDSK_HEXDUMP_CFG *cfg );


/**
 * @brief Vypíše hexdump binárních dat podle zadané konfigurace.
 *
 * Formát výstupu na každém řádku:
 *   offset: hex_bajty  ascii_sloupec
 *
 * Podporuje libovolnou délku dat včetně posledního neúplného řádku
 * (doplní mezerami). Pokud je cfg->inv nastaven, každý bajt se před
 * zobrazením invertuje (XOR 0xFF). Podle cfg->charset se v ASCII
 * sloupci provede konverze Sharp MZ znakové sady (RAW, EU, JP,
 * UTF8-EU, UTF8-JP).
 *
 * Po posledním řádku vypíše prázdný řádek (oddělovač).
 *
 * @param[in] cfg     Konfigurace hexdumpu. Nesmí být NULL.
 *                    cfg->out nesmí být NULL.
 * @param[in] data    Ukazatel na data k výpisu. Nesmí být NULL při len > 0.
 * @param[in] len     Počet bajtů k výpisu. Může být 0 (nevypíše nic).
 *
 * @pre cfg bylo inicializováno voláním mzdsk_hexdump_init() nebo ručně.
 */
extern void mzdsk_hexdump ( const st_MZDSK_HEXDUMP_CFG *cfg,
                             const uint8_t *data, uint16_t len );


/**
 * @brief Vrátí řetězec s verzí knihovny.
 *
 * @return Statický řetězec s verzí (např. "1.0.0").
 */
extern const char* mzdsk_hexdump_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_HEXDUMP_H */
