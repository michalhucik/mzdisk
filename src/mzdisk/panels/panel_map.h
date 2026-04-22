/**
 * @file panel_map.h
 * @brief Vizuální sektorová/bloková mapa disku.
 *
 * Zobrazuje grafickou mapu obsazenosti bloků pro všechny podporované FS.
 * Unifikuje tři různá API (FSMZ, CP/M, MRS) do společného datového modelu.
 *
 * Pro CP/M disky navíc poskytuje "Disk Layout" - proporcionální přehled
 * oblastí disku (Boot Track, System Tracks, Directory, File Data, Free).
 *
 * Architektura panel-split:
 *   panel_map.h/.c       - datový model + logika naplnění (čisté C)
 *   panel_map_imgui.cpp  - ImGui rendering (C++)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_MAP_H
#define MZDISK_PANEL_MAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"


/** @brief Maximální počet bloků v mapě (FSMZ: 160*16=2560, CP/M: 4096, MRS: 1440). */
#define PANEL_MAP_MAX_BLOCKS 4096

/** @brief Počet bloků v boot track mapě (stopa 0 = 16 FSMZ sektorů). */
#define PANEL_MAP_BOOT_BLOCKS 16

/** @brief Maximální počet segmentů v disk layout vizualizaci. */
#define PANEL_MAP_MAX_LAYOUT_SEGMENTS 8


/**
 * @brief Unifikovaný typ bloku pro vizualizaci.
 *
 * Mapuje specifické typy z FSMZ/CP/M/MRS do společné sady barev.
 */
typedef enum en_MAP_BLOCK_TYPE {
    MAP_BLOCK_FREE = 0,         /**< Volný blok (všechny FS). */
    MAP_BLOCK_FILE,             /**< Souborový blok (FSMZ USED, CP/M allocated, MRS FILE). */
    MAP_BLOCK_SYSTEM,           /**< Systémový/boot blok (FSMZ IPLPRO, MRS SYSTEM). */
    MAP_BLOCK_META,             /**< Metadata blok (FSMZ META/DINFO, MRS FAT). */
    MAP_BLOCK_DIR,              /**< Adresářový blok (FSMZ DIR, CP/M DIR, MRS DIR). */
    MAP_BLOCK_BOOTSTRAP,        /**< Bootstrap loader (FSMZ). */
    MAP_BLOCK_BAD,              /**< Vadný blok (MRS). */
    MAP_BLOCK_RESERVED,         /**< Rezervovaný blok (CP/M system tracks). */
    MAP_BLOCK_OVER,             /**< Blok za hranicí FS (FSMZ OVER_FAREA). */
} en_MAP_BLOCK_TYPE;


/**
 * @brief Jeden segment disk layout vizualizace.
 *
 * Reprezentuje jednu oblast na disku (boot track, systémové stopy,
 * adresář, souborová data, volné místo) s velikostí pro proporcionální
 * vykreslení a barvou podle typu.
 *
 * @par Invarianty:
 * - label[] je vždy null-terminated.
 * - detail[] je vždy null-terminated.
 * - size_bytes > 0.
 */
typedef struct st_PANEL_MAP_LAYOUT_SEGMENT {
    char label[64];             /**< Krátký popis segmentu (např. "Boot Track"). */
    char detail[128];           /**< Detailní popis pro tooltip (stopy, velikost). */
    uint32_t size_bytes;        /**< Velikost segmentu v bajtech. */
    en_MAP_BLOCK_TYPE type;     /**< Typ bloku pro určení barvy. */
} st_PANEL_MAP_LAYOUT_SEGMENT;


/**
 * @brief Datový model mapy bloků.
 *
 * Obsahuje unifikované pole typů bloků a statistiky.
 *
 * @par Invarianty:
 * - Pokud is_loaded == true, pole blocks[] je naplněné pro [0..block_count).
 */
typedef struct st_PANEL_MAP_DATA {
    bool is_loaded;                                 /**< Data jsou naplněna. */
    en_MZDSK_FS_TYPE fs_type;                       /**< Typ FS (pro popis v legendě). */
    int block_count;                                /**< Celkový počet bloků. */
    en_MAP_BLOCK_TYPE blocks[PANEL_MAP_MAX_BLOCKS]; /**< Typ každého bloku. */

    /* statistiky */
    int free_count;             /**< Počet volných bloků. */
    int file_count;             /**< Počet souborových bloků. */
    int system_count;           /**< Počet systémových bloků. */
    int dir_count;              /**< Počet adresářových bloků. */
    int meta_count;             /**< Počet metadata bloků. */
    int bad_count;              /**< Počet vadných bloků. */

    /* boot track mapa (pro non-FSMZ systémy: CP/M, MRS) */
    bool has_boot_map;                                      /**< Data boot tracku jsou naplněna. */
    int boot_block_count;                                   /**< Počet bloků v boot mapě (16). */
    en_MAP_BLOCK_TYPE boot_blocks[PANEL_MAP_BOOT_BLOCKS];   /**< Typ každého bloku v boot stopě. */

    /* disk layout vizualizace (proporcionální pruh oblastí disku) */
    bool has_disk_layout;                                                   /**< Zobrazit disk layout pruh. */
    int layout_segment_count;                                               /**< Počet segmentů. */
    st_PANEL_MAP_LAYOUT_SEGMENT layout_segments[PANEL_MAP_MAX_LAYOUT_SEGMENTS]; /**< Segmenty layoutu. */
    uint32_t layout_total_bytes;                                            /**< Celková velikost disku v bajtech. */
} st_PANEL_MAP_DATA;


/**
 * @brief Naplní datový model mapy bloků z otevřeného disku.
 *
 * Detekuje typ FS a zavolá příslušné API pro získání blokové mapy.
 * Výsledek unifikuje do pole en_MAP_BLOCK_TYPE.
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 * @param detect Výsledek auto-detekce filesystému.
 *
 * @pre data != NULL, disc je platně otevřený, detect je naplněný.
 * @post data->is_loaded == true při úspěchu.
 */
extern void panel_map_load ( st_PANEL_MAP_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect );


/**
 * @brief Vykreslí vizuální mapu bloků (ImGui rendering).
 *
 * @param data Naplněný datový model.
 */
extern void panel_map_render ( const st_PANEL_MAP_DATA *data );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_MAP_H */
