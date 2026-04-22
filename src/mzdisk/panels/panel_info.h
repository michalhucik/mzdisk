/**
 * @file panel_info.h
 * @brief Informační panel - read-only zobrazení geometrie a vlastností disku.
 *
 * Ekvivalent CLI nástroje mzdsk-info (bez hexdump a raw přístupu).
 * Zobrazuje geometrii, identifikovaný formát, track rules a FS-specifické
 * statistiky (FSMZ bloky, CP/M alokace, MRS FAT).
 *
 * Architektura panel-split:
 *   panel_info.h/.c   - datový model + logika naplnění (čisté C)
 *   panel_info_imgui.cpp - ImGui rendering (C++)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_INFO_H
#define MZDISK_PANEL_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/dsk/dsk_tools.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "config.h"


/** @brief Maximální počet track rules pro zobrazení. */
#define PANEL_INFO_MAX_RULES 16


/**
 * @brief Informace o jednom track rule (pro zobrazení v GUI).
 */
typedef struct st_PANEL_INFO_TRACK_RULE {
    uint16_t from_track;        /**< První stopa pravidla. */
    uint16_t count_tracks;      /**< Počet stop v pravidle. */
    uint16_t sectors;           /**< Počet sektorů na stopu. */
    uint16_t sector_size;       /**< Velikost sektoru v bajtech (dekódováno). */
    bool is_inverted;           /**< Stopa má invertovaná data (FSMZ). */
} st_PANEL_INFO_TRACK_RULE;


/**
 * @brief Datový model informačního panelu.
 *
 * Obsahuje veškerá data potřebná pro zobrazení.
 * Naplní se voláním panel_info_load(), renderuje se v panel_info_render().
 *
 * @par Invarianty:
 * - Pokud is_loaded == true, data jsou platná a odpovídají otevřenému disku.
 * - Pokud is_loaded == false, obsah ostatních členů je nedefinovaný.
 */
typedef struct st_PANEL_INFO_DATA {
    bool is_loaded;                     /**< Data jsou naplněna. */

    /* geometrie */
    uint16_t total_tracks;              /**< Celkový počet stop. */
    uint8_t sides;                      /**< Počet stran disku. */
    int rule_count;                     /**< Počet track rules. */
    st_PANEL_INFO_TRACK_RULE rules[PANEL_INFO_MAX_RULES]; /**< Pravidla geometrie. */

    /* identifikace */
    en_DSK_TOOLS_IDENTFORMAT dsk_format;    /**< Formát identifikovaný DSK knihovnou. */
    en_MZDSK_FS_TYPE fs_type;               /**< Typ FS z auto-detekce. */

    /* celková velikost */
    uint32_t total_size_bytes;          /**< Celková velikost dat disku v bajtech. */

    /* FSMZ specifika (platné jen pokud fs_type == MZDSK_FS_FSMZ) */
    bool has_fsmz_info;                 /**< FSMZ info je k dispozici. */
    uint16_t fsmz_total_blocks;         /**< Celkový počet bloků. */
    uint16_t fsmz_used_blocks;          /**< Počet obsazených bloků. */
    uint16_t fsmz_free_blocks;          /**< Počet volných bloků. */

    /* bootstrap (IPLPRO) - platné i pro non-FSMZ disky s boot trackem */
    bool has_boot_info;                 /**< Bootstrap info je k dispozici. */
    char boot_name[18];                 /**< Název bootstrap programu (ASCII fallback). */
    uint8_t mz_boot_name[18];           /**< Originální jméno v Sharp MZ ASCII. */
    int mz_boot_name_len;               /**< Délka platných bajtů v mz_boot_name. */
    char boot_type[16];                 /**< Typ: "Normal", "Mini", "Over FSMZ". */
    uint16_t boot_blocks;               /**< Velikost bootstrapu v blocích. */
    uint16_t boot_size_bytes;           /**< Velikost bootstrapu v bajtech. */
    uint16_t boot_start_block;          /**< Počáteční blok bootstrapu. */

    /* FSMZ boot oblast na non-FSMZ discích (CP/M, MRS) */
    bool has_fsmz_boot_area;            /**< Disk má FSMZ boot stopy na začátku. */
    uint16_t fsmz_boot_tracks;          /**< Počet FSMZ boot stop. */
    uint16_t fsmz_boot_blocks;          /**< Počet bloků v FSMZ boot oblasti. */

    /* CP/M specifika (platné jen pokud fs_type == MZDSK_FS_CPM) */
    bool has_cpm_info;                  /**< CP/M info je k dispozici. */
    uint16_t cpm_block_size;            /**< Velikost alokačního bloku. */
    uint16_t cpm_total_blocks;          /**< Celkový počet bloků. */
    uint16_t cpm_dir_entries;           /**< Počet adresářových slotů. */
    uint16_t cpm_reserved_tracks;       /**< Počet rezervovaných stop. */
    char cpm_preset_name[32];           /**< Název CP/M presetu. */

} st_PANEL_INFO_DATA;


/**
 * @brief Naplní datový model informačního panelu z otevřeného disku.
 *
 * Čte geometrii, identifikaci a FS-specifické informace z disc a detect_result.
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 * @param detect Výsledek auto-detekce filesystému.
 *
 * @pre data != NULL, disc je platně otevřený, detect je naplněný.
 * @post data->is_loaded == true, všechna relevantní pole jsou naplněna.
 */
extern void panel_info_load ( st_PANEL_INFO_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect );


/**
 * @brief Vrátí textový popis identifikovaného DSK formátu.
 *
 * @param format Formát z disc->format.
 * @return Statický řetězec (např. "MZ-BASIC (FSMZ)", "CP/M SD", ...).
 */
extern const char* panel_info_format_str ( en_DSK_TOOLS_IDENTFORMAT format );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_INFO_H */
