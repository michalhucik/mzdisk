/**
 * @file   mzdsk_ipldisk_tools.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Pomocné nástroje pro práci se souborovým systémem FSMZ.
 *
 * Knihovna poskytuje vyšší funkce pro práci s FSMZ adresářem:
 * test IPLPRO hlavičky, konverzi ASCII jmen na Sharp MZ formát,
 * vyhledávání položek adresáře podle jména nebo ID, alokační mapu,
 * rychlý formát, opravu DINFO a defragmentaci disku.
 *
 * Port z fstool/fsmz_tools.h.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2018-2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef MZDSK_IPLDISK_TOOLS_H
#define MZDSK_IPLDISK_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "mzdsk_ipldisk.h"

    /* =====================================================================
     * Typy pro alokační mapu FSMZ
     * ===================================================================== */

    /**
     * @brief Typ FSMZ alokačního bloku v mapě.
     *
     * Určuje roli každého alokačního bloku na FSMZ disku.
     * Hodnoty jsou vzájemně exkluzivní - každý blok má právě jeden typ.
     */
    typedef enum en_FSMZ_BLOCK_TYPE {
        FSMZ_BLOCK_FREE = 0,      /**< Volný blok */
        FSMZ_BLOCK_IPLPRO,        /**< IPLPRO hlavička (blok 0) */
        FSMZ_BLOCK_META,           /**< DINFO metadata (blok 15) */
        FSMZ_BLOCK_DIR,            /**< Adresářový blok (16-23) */
        FSMZ_BLOCK_USED,           /**< Obsazený souborový blok */
        FSMZ_BLOCK_BOOTSTRAP,      /**< Bootstrap loader data */
        FSMZ_BLOCK_OVER_FAREA,     /**< Blok za hranicí souborové oblasti */
    } en_FSMZ_BLOCK_TYPE;


    /**
     * @brief Statistiky FSMZ alokační mapy.
     *
     * Souhrnné informace o obsazenosti disku, počítané z klasifikace
     * jednotlivých bloků. Hodnoty jsou platné po úspěšném volání
     * fsmz_tool_get_block_map().
     *
     * @invariant farea_used <= (total_blocks - farea_start)
     */
    typedef struct st_FSMZ_MAP_STATS {
        int total_blocks;             /**< Celkový počet bloků na disku */
        int farea_start;              /**< První blok souborové oblasti (dinfo.farea) */
        int farea_used;               /**< Počet obsazených bloků ve FAREA */
        int over_farea;               /**< Počet bloků za hranicí FAREA (za dinfo.blocks) */
        int bitmap_inconsistencies;   /**< Počet nalezených nekonzistencí v bitmapě
                                       *   (např. bootstrap blok není alokován, nebo
                                       *   bootstrap v rezervované oblasti). Volající
                                       *   může na základě tohoto čítače rozhodnout,
                                       *   zda a jak reportovat chybu. */
    } st_FSMZ_MAP_STATS;


    /* =====================================================================
     * Veřejné funkce
     * ===================================================================== */

    /**
     * @brief Otestuje, zda IPLPRO blok obsahuje platnou hlavičku.
     *
     * Kontroluje, že ftype == 0x03 a pole iplpro obsahuje řetězec "IPLPRO".
     *
     * @param iplpro Ukazatel na IPLPRO blok k testování. Nesmí být NULL.
     * @return EXIT_SUCCESS pokud je hlavička platná, EXIT_FAILURE pokud ne.
     */
    extern int fsmz_tool_test_iplpro_header ( st_FSMZ_IPLPRO_BLOCK *iplpro );

    /**
     * @brief Konvertuje ASCII řetězec na Sharp MZ jméno souboru.
     *
     * Výstupní buffer se nejprve vyplní terminátory 0x0d, pak se do něj
     * zkopíruje jméno s konverzí jednotlivých znaků přes sharpmz_cnv_to.
     * Délka výstupu závisí na is_iplpro_fname:
     * - 1: FSMZ_IPLFNAME_LENGTH (13 bajtů, pro IPLPRO blok)
     * - 0: FSMZ_FNAME_LENGTH (17 bajtů, pro standardní adresář)
     *
     * @param mz_fname Výstupní buffer pro Sharp MZ jméno.
     * @param filename Vstupní ASCII řetězec (zakončen znakem < 0x20).
     * @param is_iplpro_fname Nenulové = IPLPRO jméno (kratší), 0 = standardní.
     */
    extern void fsmz_tool_convert_ascii_to_mzfname ( uint8_t *mz_fname, char *filename, int is_iplpro_fname );

    /**
     * @brief Najde položku adresáře podle ASCII jména souboru.
     *
     * Konvertuje ASCII jméno na Sharp MZ formát a vyhledá ho v adresáři.
     * Volitelně vrátí stav adresáře přes dir_cache pro další operace.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param filename ASCII jméno souboru.
     * @param dir_cache Struktura adresáře pro cachování (může být NULL).
     * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
     * @param[out] err Výstupní chybový kód.
     * @return Ukazatel na nalezenou položku, nebo NULL při chybě.
     *
     * @warning Vrácený ukazatel směřuje do disc->cache.
     */
    extern st_FSMZ_DIR_ITEM* fsmz_tool_get_diritem_pointer_and_dir_by_name ( st_MZDSK_DISC *disc, char *filename, st_FSMZ_DIR *dir_cache, uint8_t fsmz_dir_items, en_MZDSK_RES *err );

    /**
     * @brief Najde položku adresáře podle pořadového čísla (ID).
     *
     * ID je 0-based index (interně se přičte 1, protože první položka
     * adresáře je rezervovaná).
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param search_item_id Pořadové číslo položky (0-based).
     * @param dir_cache Struktura adresáře pro cachování (může být NULL).
     * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
     * @param[out] err Výstupní chybový kód.
     * @return Ukazatel na nalezenou položku, nebo NULL při chybě.
     *
     * @warning Vrácený ukazatel směřuje do disc->cache.
     */
    extern st_FSMZ_DIR_ITEM* fsmz_tool_get_diritem_pointer_and_dir_by_id ( st_MZDSK_DISC *disc, uint8_t search_item_id, st_FSMZ_DIR *dir_cache, uint8_t fsmz_dir_items, en_MZDSK_RES *err );

    /**
     * @brief Sestaví alokační mapu FSMZ disku.
     *
     * Pro každý blok na disku určí jeho typ (IPLPRO, META, DIR, USED,
     * FREE, BOOTSTRAP, OVER_FAREA) na základě IPLPRO hlavičky a DINFO
     * bitmapy. Vrátí pole typů a volitelně souhrnné statistiky.
     *
     * Klasifikační pravidla:
     * - Blok 0: pokud má platnou IPLPRO hlavičku -> FSMZ_BLOCK_IPLPRO,
     *   jinak FSMZ_BLOCK_FREE.
     * - Blok 15: FSMZ_BLOCK_META (DINFO metadata).
     * - Bloky 16-23: FSMZ_BLOCK_DIR (adresář).
     * - Bloky v souborové oblasti (farea .. blocks): podle bitmapy
     *   FSMZ_BLOCK_USED nebo FSMZ_BLOCK_FREE. Bootstrap bloky
     *   (system_start .. system_end z IPLPRO) mají FSMZ_BLOCK_BOOTSTRAP.
     * - Bloky za hranicí farea (> dinfo.blocks): FSMZ_BLOCK_OVER_FAREA,
     *   případně FSMZ_BLOCK_BOOTSTRAP pokud spadají do bootstrap rozsahu.
     * - Bloky v rezervované oblasti (1-14, mimo 15): pokud spadají
     *   do bootstrap rozsahu -> FSMZ_BLOCK_BOOTSTRAP, jinak FSMZ_BLOCK_FREE.
     *
     * @param[in]  disc      Otevřený disk v FSMZ formátu. Nesmí být NULL.
     * @param[out] map       Pole o velikosti alespoň total_tracks * 16.
     *                       Nesmí být NULL. Po návratu obsahuje typ
     *                       pro každý blok.
     * @param[in]  map_size  Velikost pole map (počet prvků).
     * @param[out] stats     Statistiky alokační mapy (může být NULL).
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return Chybový kód při selhání čtení IPLPRO nebo DINFO.
     *
     * @pre disc musí být otevřený disk s plným FSMZ formátem
     *      (geometrie 16x256B na všech stopách).
     * @pre map_size musí být >= total_tracks * FSMZ_SECTORS_ON_TRACK.
     *
     * @post Při úspěchu je map naplněna klasifikací a stats (pokud != NULL)
     *       obsahuje souhrnné statistiky.
     *
     * @note Funkce nepíše na stdout ani na stderr. Případné nekonzistence
     *       bitmapy (např. nealokovaný bootstrap blok, bootstrap v rezervované
     *       oblasti) jsou jen počítány v `stats->bitmap_inconsistencies` -
     *       volající si výpis řídí sám.
     */
    extern en_MZDSK_RES fsmz_tool_get_block_map ( st_MZDSK_DISC *disc,
                                                    en_FSMZ_BLOCK_TYPE *map,
                                                    int map_size,
                                                    st_FSMZ_MAP_STATS *stats );


    /**
     * @brief Provede rychlý formát FSMZ disku.
     *
     * Vymaže IPLPRO hlavičku, inicializuje DINFO blok (prázdná bitmapa,
     * farea, blocks) a vynuluje adresářové bloky. Datová oblast se
     * nemaže - po formátu mohou datové bloky obsahovat staré soubory,
     * ale bitmapa je prázdná.
     *
     * Adresář se inicializuje s hlavičkovými bajty 0x80, 0x01 na
     * začátku prvního bloku (standard FSMZ).
     *
     * @param[in] disc Otevřený disk v FSMZ formátu (RW). Nesmí být NULL.
     *
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre Disk musí být v plném FSMZ formátu (DSK_TOOLS_IDENTFORMAT_MZBASIC).
     * @post IPLPRO, DINFO a adresář jsou vymazány/inicializovány.
     *       volume_number = FSMZ_DINFO_SLAVE.
     */
    extern en_MZDSK_RES fsmz_tool_fast_format ( st_MZDSK_DISC *disc );


    /**
     * @brief Provede formát souborové oblasti FSMZ disku se zachováním bootstrapu.
     *
     * Vyčistí adresář a reinicializuje DINFO blok (prázdná bitmapa, farea, blocks).
     * Na rozdíl od fsmz_tool_fast_format() zachovává IPLPRO hlavičku a bootstrap
     * data. Pokud na disku existuje platný bootstrap (Normal i Bottom), jeho bloky
     * se alokují v nové bitmapě.
     *
     * @param[in] disc Otevřený disk v FSMZ formátu (RW). Nesmí být NULL.
     *
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre Disk musí být v plném FSMZ formátu (DSK_TOOLS_IDENTFORMAT_MZBASIC).
     * @post DINFO a adresář jsou reinicializovány. IPLPRO a bootstrap data zůstávají.
     *       volume_number = FSMZ_DINFO_MASTER pokud bootstrap existuje.
     *
     * @note Normal bootstrap (data ve FAREA) se zachová - jeho bloky jsou
     *       alokovány v nové bitmapě.
     */
    extern en_MZDSK_RES fsmz_tool_format_file_area ( st_MZDSK_DISC *disc );


    /**
     * @brief Opraví DINFO blok přepočítáním bitmapy a počítadel.
     *
     * Projde celý adresář a bootstrap, přepočítá bitmapu souborové oblasti
     * a čítač obsazených bloků (dinfo.used). Opravenou DINFO zapíše zpět
     * na disk.
     *
     * Pokud je na disku platný bootstrap (IPLPRO hlavička), nastaví
     * volume_number na FSMZ_DINFO_MASTER (0). Jinak nastaví
     * FSMZ_DINFO_SLAVE (1).
     *
     * @param[in] disc           Otevřený disk v FSMZ formátu (RW). Nesmí být NULL.
     * @param[in] fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
     *
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre Disk musí být v plném FSMZ formátu (DSK_TOOLS_IDENTFORMAT_MZBASIC).
     * @post DINFO blok na disku odpovídá aktuálnímu obsahu adresáře a bootstrapu.
     */
    extern en_MZDSK_RES fsmz_tool_repair_dinfo ( st_MZDSK_DISC *disc,
                                                    uint8_t fsmz_dir_items );


    /* =====================================================================
     * Defragmentace FSMZ
     * ===================================================================== */

    /**
     * @brief Typ callbacku pro hlášení průběhu defragmentace.
     *
     * Knihovní funkce fsmz_tool_defrag() volá tento callback s informačními
     * zprávami o průběhu operace. Zprávy jsou v angličtině a zakončené '\n'.
     *
     * @param message Textová zpráva o průběhu (nulou zakončený řetězec).
     * @param user_data Uživatelská data předaná při volání fsmz_tool_defrag().
     */
    typedef void (*fsmz_tool_defrag_cb_t) ( const char *message, void *user_data );


    /**
     * @brief Provede defragmentaci FSMZ disku.
     *
     * Načte všechny soubory (a volitelně bootstrap) z FSMZ disku do paměti,
     * provede rychlý formát a znovu zapíše vše sekvenčně od začátku
     * souborové oblasti bez mezer.
     *
     * Algoritmus:
     * 1. Přečte DINFO a IPLPRO hlavičku z disku.
     * 2. Pokud existuje platný bootstrap v souborové oblasti, načte ho
     *    do paměťového MZF handleru.
     * 3. Projde celý adresář a načte všechny existující soubory
     *    do paměťových MZF handlerů.
     * 4. Provede rychlý formát disku (fsmz_tool_fast_format).
     * 5. Zapíše bootstrap (pokud existoval) na první volné místo.
     * 6. Sekvenčně zapíše všechny soubory - výsledkem jsou soubory
     *    uložené bez fragmentace od začátku souborové oblasti.
     *
     * Průběh operace je hlášen přes progress_cb (pokud není NULL).
     *
     * @param[in] disc           Otevřený disk v FSMZ formátu (RW). Nesmí být NULL.
     * @param[in] fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
     * @param[in] progress_cb    Callback pro hlášení průběhu (může být NULL).
     * @param[in] cb_data        Uživatelská data pro callback (může být NULL).
     *
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre Disk musí být v plném FSMZ formátu (DSK_TOOLS_IDENTFORMAT_MZBASIC).
     * @pre disc nesmí být NULL.
     *
     * @post Při úspěchu jsou všechny soubory na disku uloženy sekvenčně
     *       bez mezer od začátku souborové oblasti. Bootstrap (pokud existoval)
     *       je zachován.
     *
     * @warning Defragmentace je destruktivní operace - při chybě uprostřed
     *          procesu mohou být data na disku nekonzistentní nebo ztracena.
     *
     * @note Funkce nepíše na stdout ani stderr. Veškeré informační zprávy
     *       jsou předávány přes progress_cb.
     */
    extern en_MZDSK_RES fsmz_tool_defrag ( st_MZDSK_DISC *disc,
                                             uint8_t fsmz_dir_items,
                                             fsmz_tool_defrag_cb_t progress_cb,
                                             void *cb_data );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_IPLDISK_TOOLS_H */
