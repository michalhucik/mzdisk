/**
 * @file panel_raw_io.h
 * @brief Raw I/O okno - export/import sektorů/bloků do/ze souboru.
 *
 * Nemodální okno otevírané z hexdump panelu tlačítky Get/Put.
 * Umožňuje export (Get) nebo import (Put) libovolného rozsahu
 * sektorů/bloků do/ze souboru s volitelnou inverzí (XOR 0xFF),
 * byte offsetem/počtem a file offsetem.
 *
 * Architektura panel-split:
 *   panel_raw_io.h/.c          - datový model a logika (čisté C)
 *   panel_raw_io_imgui.cpp     - ImGui rendering (C++)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_RAW_IO_H
#define MZDISK_PANEL_RAW_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "panels/panel_hexdump.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "config.h"


/**
 * @brief Akce Raw I/O operace.
 */
typedef enum en_RAW_IO_ACTION {
    RAW_IO_ACTION_GET = 0,  /**< Export z disku do souboru. */
    RAW_IO_ACTION_PUT,      /**< Import ze souboru na disk. */
} en_RAW_IO_ACTION;


/**
 * @brief Datový model Raw I/O okna.
 *
 * Obsahuje veškerý stav potřebný pro definici rozsahu
 * sektorů/bloků, cesty k souboru a UI stav (dialogy, chyby).
 *
 * @par Invarianty:
 * - V režimu HEXDUMP_ADDR_TRACK_SECTOR: sector_count >= 1.
 * - V režimu HEXDUMP_ADDR_BLOCK: block_count >= 1.
 * - start_sector je 1-based (sector ID).
 * - byte_count == 0 znamená "vše" (žádné omezení).
 */
typedef struct st_PANEL_RAW_IO_DATA {
    bool is_open;                               /**< Okno je otevřené. */
    en_RAW_IO_ACTION action;                    /**< Typ operace (Get/Put). */

    /* adresace */
    en_HEXDUMP_ADDR_MODE addr_mode;             /**< Režim adresování (T/S nebo Block). */
    en_HEXDUMP_SECTOR_ORDER sector_order;       /**< Pořadí procházení sektorů (ID/Phys). */

    /* T/S režim */
    uint16_t start_track;                       /**< Počáteční stopa. */
    uint16_t start_sector;                      /**< Počáteční sektor (1-based sector ID). */
    int32_t sector_count;                       /**< Počet sektorů k přenesení. */

    /* Block režim */
    st_HEXDUMP_BLOCK_CONFIG block_config;       /**< Konfigurace blokového adresování. */
    int32_t start_block;                        /**< Počáteční blok. */
    int32_t block_count;                        /**< Počet bloků k přenesení. */

    /* data */
    bool invert;                                /**< Invertovat data (XOR 0xFF). */
    int32_t byte_offset;                        /**< Offset v prvním sektoru/bloku (bajty). */
    int32_t byte_count;                         /**< Počet bajtů k přenesení (0 = vše). */

    /* soubor */
    char filepath[MZDISK_CONFIG_PATH_MAX];      /**< Cesta k souboru pro export/import. */
    int64_t file_offset;                        /**< Offset v souboru (byte). */
    bool put_whole_file;                        /**< Put režim: importovat celý soubor od file_offset;
                                                     pokrývá tolik sektorů, kolik je potřeba.
                                                     Při true se sector_count/block_count a byte_count
                                                     v UI ignorují a v execute_put se přepočítají
                                                     podle skutečné velikosti souboru. */

    /* UI stav */
    bool show_put_confirm;                      /**< Zobrazit potvrzovací dialog pro Put. */
    bool show_error;                            /**< Zobrazit chybovou hlášku. */
    char error_msg[512];                        /**< Text chybové hlášky. */
    bool show_success;                          /**< Zobrazit zprávu o úspěchu. */
    char success_msg[256];                      /**< Text zprávy o úspěchu. */
    uint16_t max_track;                         /**< Maximální číslo stopy (z disku). */
} st_PANEL_RAW_IO_DATA;


/**
 * @brief Inicializuje Raw I/O data na výchozí hodnoty.
 *
 * @param data Ukazatel na datový model.
 *
 * @pre data != NULL.
 * @post data je vynulovaný, sector_count=1, block_count=1.
 */
extern void panel_raw_io_init ( st_PANEL_RAW_IO_DATA *data );


/**
 * @brief Otevře Raw I/O okno s parametry z hexdumpu.
 *
 * Převezme aktuální pozici, režim adresování, blokovou konfiguraci
 * a inverzi z hexdump panelu. Nastaví is_open na true.
 *
 * @param data Ukazatel na Raw I/O datový model.
 * @param hd Ukazatel na hexdump datový model (zdrojové parametry).
 * @param action Typ operace (Get nebo Put).
 *
 * @pre data != NULL, hd != NULL.
 * @post data->is_open == true, parametry převzaté z hd.
 */
extern void panel_raw_io_open_from_hexdump ( st_PANEL_RAW_IO_DATA *data,
                                              const st_PANEL_HEXDUMP_DATA *hd,
                                              en_RAW_IO_ACTION action );


/**
 * @brief Provede Get (export) - čtení sektorů z disku do souboru.
 *
 * Na základě režimu adresování (T/S nebo Block) přepočítá startovní
 * pozici, iteruje přes sektory a zapisuje data do souboru.
 * Respektuje byte_offset, byte_count, invert a file_offset.
 *
 * @param data Datový model s parametry operace.
 * @param disc Otevřený disk.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DSK_ERROR při chybě.
 *
 * @pre data != NULL, disc != NULL a otevřený.
 * @post Při úspěchu: soubor na filepath obsahuje exportovaná data,
 *       data->show_success == true.
 * @post Při chybě: data->show_error == true, data->error_msg naplněný.
 */
extern en_MZDSK_RES panel_raw_io_execute_get ( st_PANEL_RAW_IO_DATA *data,
                                                st_MZDSK_DISC *disc );


/**
 * @brief Provede Put (import) - zápis dat ze souboru na disk.
 *
 * Na základě režimu adresování přepočítá startovní pozici,
 * iteruje přes sektory a čte data ze souboru na disk.
 * Respektuje byte_offset, byte_count, invert a file_offset.
 *
 * @param data Datový model s parametry operace.
 * @param disc Otevřený disk.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DSK_ERROR při chybě.
 *
 * @pre data != NULL, disc != NULL a otevřený.
 * @post Při úspěchu: sektory na disku obsahují importovaná data,
 *       data->show_success == true.
 * @post Při chybě: data->show_error == true, data->error_msg naplněný.
 */
extern en_MZDSK_RES panel_raw_io_execute_put ( st_PANEL_RAW_IO_DATA *data,
                                                st_MZDSK_DISC *disc );


/**
 * @brief Vykreslí Raw I/O okno (ImGui rendering).
 *
 * Implementováno v panel_raw_io_imgui.cpp.
 *
 * @param data Datový model Raw I/O okna.
 * @param disc Otevřený disk.
 * @param hd Data hexdump panelu sesterské session; po úspěšné Put operaci
 *           se na nich zavolá panel_hexdump_read_sector(), aby hexview
 *           zobrazil aktuální obsah sektoru. Může být NULL - pak se refresh
 *           přeskočí.
 * @param is_dirty Příznak neuložených změn (Put operace ho nastaví na true).
 * @param cfg Konfigurace aplikace (pro IGFD cesty).
 * @param owner_id Stabilní id vlastníka (session id) pro unikátní ImGui
 *                 window ID - nutné při multi-window, kdy víc sessions
 *                 může mít Raw I/O otevřený paralelně. 0 = ignorovat,
 *                 chovat se jako single-instance.
 */
extern void panel_raw_io_render ( st_PANEL_RAW_IO_DATA *data, st_MZDSK_DISC *disc,
                                   st_PANEL_HEXDUMP_DATA *hd,
                                   bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                   uint64_t owner_id );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_RAW_IO_H */
