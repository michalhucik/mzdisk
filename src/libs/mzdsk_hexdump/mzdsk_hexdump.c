/**
 * @file   mzdsk_hexdump.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Implementace konfigurovatelného hexdump výpisu.
 *
 * Poskytuje formátovaný hexdump s volitelnou inverzi (XOR 0xFF)
 * a konverzí Sharp MZ znakové sady. Výstup směřuje na konfigurovatelný
 * FILE* stream.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */


#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "libs/mzdsk_hexdump/mzdsk_hexdump.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"


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
void mzdsk_hexdump_init ( st_MZDSK_HEXDUMP_CFG *cfg ) {
    cfg->out = stdout;
    cfg->cols = MZDSK_HEXDUMP_DEFAULT_COLS;
    cfg->col_separator = MZDSK_HEXDUMP_DEFAULT_COL_SEPARATOR;
    cfg->ascii_separator = MZDSK_HEXDUMP_DEFAULT_ASCII_SEPARATOR;
    cfg->inv = 0;
    cfg->charset = MZDSK_HEXDUMP_CHARSET_RAW;
}


/**
 * @brief Vypíše hexdump binárních dat podle zadané konfigurace.
 *
 * Formát výstupu na každém řádku:
 *   "0xOOOO:  HH HH HH ...   ascii"
 *
 * Hex bajty jsou seskupeny po cfg->col_separator sloupcích s extra
 * mezerou. Mezi hex a ASCII částí je cfg->ascii_separator mezer.
 * Poslední neúplný řádek je doplněn mezerami pro zarovnání.
 *
 * Pokud je cfg->inv nastaven, každý bajt se před zobrazením
 * invertuje (XOR 0xFF) - to odpovídá zobrazení "logických" dat
 * na FSMZ discích, kde jsou data fyzicky uložena invertovaně.
 *
 * Podle cfg->charset se v ASCII sloupci provede konverze:
 * - RAW: standardní ASCII (0x20-0x7E), ostatní jako '.'
 * - EU: Sharp MZ EU -> ASCII, nekonvertovatelné/netisknutelné jako '.'
 * - JP: Sharp MZ JP -> ASCII, nekonvertovatelné/netisknutelné jako '.'
 * - UTF8_EU: Sharp MZ EU -> UTF-8, nekonvertovatelné/netisknutelné jako '.'
 * - UTF8_JP: Sharp MZ JP -> UTF-8, nekonvertovatelné/netisknutelné jako '.'
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
void mzdsk_hexdump ( const st_MZDSK_HEXDUMP_CFG *cfg,
                      const uint8_t *data, uint16_t len ) {

    FILE *out = cfg->out;
    uint16_t cols = cfg->cols;
    uint16_t i = 0;

    /* Ochrana proti OOB zápisu do `ascii` bufferu: buffer má kapacitu
     * pro 256 znaků (× max 4 B UTF-8). Pokud volající nastaví cols > 256,
     * padding smyčka i cyklus hex výstupu by přetekly. */
    if ( cols > 256 ) cols = 256;

    while ( i < len ) {
        fprintf ( out, "0x%04x: ", i );

        uint16_t row_len = ( (uint16_t) ( len - i ) < cols )
                           ? (uint16_t) ( len - i ) : cols;

        /* ASCII sloupec - buffer dostatečný i pro UTF-8 (max 4 B na znak) */
        char ascii[257 * 4];
        size_t ascii_pos = 0;

        /* Hex bajty */
        for ( uint16_t j = 0; j < row_len; j++ ) {
            if ( j % cfg->col_separator == 0 ) fprintf ( out, " " );
            uint8_t c = cfg->inv ? (uint8_t) ~data[i + j] : data[i + j];
            fprintf ( out, " %02X", c );

            switch ( cfg->charset ) {

                case MZDSK_HEXDUMP_CHARSET_RAW:
                    ascii[ascii_pos++] = ( c >= 0x20 && c <= 0x7e ) ? (char) c : '.';
                    break;

                case MZDSK_HEXDUMP_CHARSET_EU: {
                    int converted, printable;
                    uint8_t c_ascii = sharpmz_convert_to_ASCII ( c, &converted, &printable );
                    ascii[ascii_pos++] = ( converted && printable ) ? (char) c_ascii : '.';
                    break;
                }

                case MZDSK_HEXDUMP_CHARSET_JP: {
                    int converted, printable;
                    uint8_t c_ascii = sharpmz_jp_convert_to_ASCII ( c, &converted, &printable );
                    ascii[ascii_pos++] = ( converted && printable ) ? (char) c_ascii : '.';
                    break;
                }

                case MZDSK_HEXDUMP_CHARSET_UTF8_EU: {
                    int converted, printable;
                    const char *utf8 = sharpmz_eu_convert_to_UTF8 ( c, &converted, &printable );
                    if ( converted && printable ) {
                        size_t ulen = strlen ( utf8 );
                        memcpy ( ascii + ascii_pos, utf8, ulen );
                        ascii_pos += ulen;
                    } else {
                        ascii[ascii_pos++] = '.';
                    }
                    break;
                }

                case MZDSK_HEXDUMP_CHARSET_UTF8_JP: {
                    int converted, printable;
                    const char *utf8 = sharpmz_jp_convert_to_UTF8 ( c, &converted, &printable );
                    if ( converted && printable ) {
                        size_t ulen = strlen ( utf8 );
                        memcpy ( ascii + ascii_pos, utf8, ulen );
                        ascii_pos += ulen;
                    } else {
                        ascii[ascii_pos++] = '.';
                    }
                    break;
                }
            }
        }

        /* Padding neúplného řádku */
        for ( uint16_t j = row_len; j < cols; j++ ) {
            if ( j % cfg->col_separator == 0 ) fprintf ( out, " " );
            fprintf ( out, "   " );
            ascii[ascii_pos++] = ' ';
        }

        ascii[ascii_pos] = '\0';
        i += row_len;

        /* Oddělovač hex / ASCII */
        for ( int k = 0; k < cfg->ascii_separator; k++ ) fprintf ( out, " " );
        fprintf ( out, "%s\n", ascii );
    }

    fprintf ( out, "\n" );
}


/**
 * @brief Vrátí řetězec s verzí knihovny.
 *
 * @return Statický řetězec s verzí (např. "1.0.0").
 */
const char* mzdsk_hexdump_version ( void ) {
    return MZDSK_HEXDUMP_VERSION;
}
