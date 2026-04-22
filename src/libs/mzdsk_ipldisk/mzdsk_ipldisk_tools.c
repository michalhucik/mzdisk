/**
 * @file   mzdsk_ipldisk_tools.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Implementace pomocných nástrojů pro FSMZ souborový systém.
 *
 * Obsahuje vyšší funkce pro práci s FSMZ:
 * - test platnosti IPLPRO hlavičky
 * - konverze ASCII jmen na Sharp MZ formát
 * - vyhledávání položek adresáře podle jména nebo ID
 * - alokační mapa (klasifikace bloků)
 * - rychlý formát, oprava DINFO, defragmentace
 *
 * Port z fstool/fsmz_tools.c.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mzdsk_ipldisk.h"
#include "mzdsk_ipldisk_tools.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/**
 * @brief Otestuje, zda IPLPRO blok obsahuje platnou hlavičku.
 *
 * Kontroluje dvě podmínky:
 * 1. ftype musí být 0x03
 * 2. prvních 6 bajtů pole iplpro musí být "IPLPRO"
 *
 * @param iplpro Ukazatel na IPLPRO blok k testování. Nesmí být NULL.
 * @return EXIT_SUCCESS pokud je hlavička platná, EXIT_FAILURE pokud ne.
 */
int fsmz_tool_test_iplpro_header ( st_FSMZ_IPLPRO_BLOCK *iplpro ) {
    if ( ( iplpro->ftype == 0x03 ) && ( 0 == strncmp ( (char*) iplpro->iplpro, "IPLPRO", 6 ) ) ) {
        return EXIT_SUCCESS;
    };
    return EXIT_FAILURE;
}


/**
 * @brief Konvertuje ASCII řetězec na Sharp MZ jméno souboru.
 *
 * Výstupní buffer se nejprve vyplní terminátory 0x0d. Pak se do něj
 * zkopíruje jméno s konverzí jednotlivých znaků přes sharpmz_cnv_to.
 * Kopírování končí při dosažení znaku < 0x20 nebo maximální délky
 * (length - 1).
 *
 * @param mz_fname Výstupní buffer pro Sharp MZ jméno.
 *                 Musí mít velikost alespoň FSMZ_IPLFNAME_LENGTH
 *                 resp. FSMZ_FNAME_LENGTH bajtů.
 * @param filename Vstupní ASCII řetězec (zakončen znakem < 0x20 nebo '\0').
 * @param is_iplpro_fname Nenulové = IPLPRO jméno (13 bajtů),
 *                        0 = standardní jméno (17 bajtů).
 */
void fsmz_tool_convert_ascii_to_mzfname ( uint8_t *mz_fname, char *filename, int is_iplpro_fname ) {
    int i;
    int length;


    if ( is_iplpro_fname ) {
        length = FSMZ_IPLFNAME_LENGTH;
    } else {
        length = FSMZ_FNAME_LENGTH;
    };

    memset ( mz_fname, 0x0d, length );

    for ( i = 0; i < length - 1; i++ ) {

        if ( filename [ i ] < 0x20 ) {
            break;
        };

        mz_fname [ i ] = sharpmz_cnv_to ( filename [ i ] );
    };
}


/**
 * @brief Najde položku adresáře podle ASCII jména souboru.
 *
 * Konvertuje ASCII jméno na Sharp MZ formát pomocí
 * fsmz_tool_convert_ascii_to_mzfname a pak ho vyhledá v adresáři
 * přes fsmz_search_file. Volitelně vrátí stav adresáře přes
 * dir_cache pro další operace nad nalezenou položkou.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param filename ASCII jméno souboru (zakončeno '\0' nebo znakem < 0x20).
 * @param dir_cache Struktura adresáře pro cachování (může být NULL -
 *                  použije se lokální proměnná).
 * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
 * @param[out] err Výstupní chybový kód: MZDSK_RES_OK = nalezeno,
 *                 MZDSK_RES_FILE_NFND = nenalezeno, záporné = chyba I/O.
 * @return Ukazatel na nalezenou položku adresáře, nebo NULL při chybě.
 *
 * @warning Vrácený ukazatel směřuje do disc->cache a je platný pouze
 *          do dalšího čtení sektoru.
 */
st_FSMZ_DIR_ITEM* fsmz_tool_get_diritem_pointer_and_dir_by_name ( st_MZDSK_DISC *disc, char *filename, st_FSMZ_DIR *dir_cache, uint8_t fsmz_dir_items, en_MZDSK_RES *err ) {

    st_FSMZ_DIR dir_local_cache;
    st_FSMZ_DIR *dir_p = &dir_local_cache;

    if ( dir_cache != NULL ) dir_p = dir_cache;

    uint8_t mz_fname [ FSMZ_FNAME_LENGTH ];
    st_FSMZ_DIR_ITEM *diritem;

    fsmz_tool_convert_ascii_to_mzfname ( mz_fname, filename, 0 );

    diritem = fsmz_search_file ( disc, mz_fname, dir_p, fsmz_dir_items, err );

    if ( *err != MZDSK_RES_OK ) diritem = NULL;

    return diritem;
}


/**
 * @brief Sestaví alokační mapu FSMZ disku.
 *
 * Přečte IPLPRO hlavičku a DINFO blok z disku, poté projde všechny
 * bloky a klasifikuje je podle jejich role. Logika klasifikace:
 *
 * 1. Blok 0: kontrola IPLPRO hlavičky - platná -> IPLPRO, neplatná -> FREE.
 * 2. Bloky v souborové oblasti (farea .. blocks): test bitmapy
 *    DINFO -> USED nebo FREE. Bootstrap bloky mají přednost -> BOOTSTRAP.
 * 3. Bloky mimo farea ale <= blocks: blok 15 -> META, bloky 16-23 -> DIR,
 *    ostatní jsou FREE, pokud nespadají do bootstrap rozsahu.
 * 4. Bloky za hranicí (> blocks): OVER_FAREA, případně BOOTSTRAP.
 *
 * Případné nekonzistence (bootstrap blok není alokován, bootstrap
 * v rezervované oblasti) jsou započítány do
 * `stats->bitmap_inconsistencies`. Volající si reporting řídí sám -
 * knihovna nepíše na stderr ani stdout.
 *
 * @param[in]  disc      Otevřený disk v FSMZ formátu. Nesmí být NULL.
 * @param[out] map       Pole o velikosti alespoň total_tracks * 16. Nesmí být NULL.
 * @param[in]  map_size  Velikost pole map (počet prvků).
 * @param[out] stats     Statistiky alokační mapy (může být NULL).
 *
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání čtení.
 *
 * @pre disc != NULL, map != NULL
 * @pre map_size >= total_tracks * FSMZ_SECTORS_ON_TRACK
 */
en_MZDSK_RES fsmz_tool_get_block_map ( st_MZDSK_DISC *disc,
                                         en_FSMZ_BLOCK_TYPE *map,
                                         int map_size,
                                         st_FSMZ_MAP_STATS *stats ) {

    en_MZDSK_RES err;
    st_FSMZ_DINFO_BLOCK dinfo;
    st_FSMZ_IPLPRO_BLOCK iplpro;

    /* Přečtení IPLPRO hlavičky z bloku 0 */
    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    /* Určení, zda je na bloku 0 platný IPLPRO zavaděč */
    int has_iplpro = 0;
    uint16_t system_start = 0;
    uint16_t system_end = 0;

    if ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        has_iplpro = 1;
        if ( iplpro.fsize > 0 ) {
            system_start = iplpro.block;
            system_end = iplpro.block + ( iplpro.fsize / FSMZ_SECTOR_SIZE ) - 1;
            if ( iplpro.fsize % FSMZ_SECTOR_SIZE ) system_end++;
        }
        /* fsize == 0: prázdný IPLPRO (zformátovaný disk bez bootstrap kódu) -
           system_start/system_end zůstávají 0, žádné bootstrap bloky se nekontrolují */
    }

    /* Přečtení DINFO bloku s bitmapou a informacemi o souborové oblasti */
    err = fsmz_read_dinfo ( disc, &dinfo );
    if ( err ) return err;

    uint8_t tracks = disc->tracks_rules->total_tracks / disc->tracks_rules->sides;
    uint8_t sides = disc->tracks_rules->sides;
    int total_blocks = tracks * sides * FSMZ_SECTORS_ON_TRACK;

    /* Horní hranice bloku adresáře (standardní FSMZ: bloky 16-23) */
    int diritems_top_block = 23;

    int farea_used = 0;
    int over_farea = 0;
    int bitmap_inconsistencies = 0;

    for ( int i = 0; i < total_blocks && i < map_size; i++ ) {

        en_FSMZ_BLOCK_TYPE type = FSMZ_BLOCK_FREE;

        if ( i == 0 ) {
            /* Blok 0: IPLPRO hlavička nebo volný */
            type = has_iplpro ? FSMZ_BLOCK_IPLPRO : FSMZ_BLOCK_FREE;
        } else {
            if ( ( i >= dinfo.farea ) && ( i <= dinfo.blocks ) ) {
                /* Souborová oblast - kontrola bitmapy */
                int j = i - dinfo.farea;
                int is_used = ( dinfo.map [ j / 8 ] >> ( j % 8 ) ) & 1;
                type = is_used ? FSMZ_BLOCK_USED : FSMZ_BLOCK_FREE;
                if ( is_used ) farea_used++;

                /* Bootstrap bloky mají přednost */
                if ( ( i >= system_start ) && ( i <= system_end ) ) {
                    if ( !is_used ) {
                        /* Nekonzistence: bootstrap blok není alokován v bitmapě.
                         * Pouze započítáme, reporting je na volajícím (audit L-2). */
                        bitmap_inconsistencies++;
                        farea_used++;
                    }
                    type = FSMZ_BLOCK_BOOTSTRAP;
                }
            } else if ( i <= dinfo.blocks ) {
                /* Rezervovaná oblast (bloky 1-14, 15, 16-23, 24..farea-1) */
                if ( i == 15 ) {
                    type = FSMZ_BLOCK_META;
                } else if ( ( i >= 16 ) && ( i <= diritems_top_block ) ) {
                    type = FSMZ_BLOCK_DIR;
                }
                /* Bootstrap bloky v rezervované oblasti */
                if ( ( i >= system_start ) && ( i <= system_end ) ) {
                    if ( type != FSMZ_BLOCK_FREE ) {
                        /* Nekonzistence: bootstrap blok spadá do metadat/adresáře.
                         * Jen započítáme, reporting je na volajícím (audit L-2). */
                        bitmap_inconsistencies++;
                    }
                    type = FSMZ_BLOCK_BOOTSTRAP;
                }
            } else {
                /* Blok za hranicí souborové oblasti */
                over_farea++;
                type = FSMZ_BLOCK_OVER_FAREA;
                if ( ( i >= system_start ) && ( i <= system_end ) ) {
                    type = FSMZ_BLOCK_BOOTSTRAP;
                }
            }
        }

        map[i] = type;
    }

    /* Vyplnění statistik */
    if ( stats != NULL ) {
        stats->total_blocks = total_blocks;
        stats->farea_start = dinfo.farea;
        stats->farea_used = farea_used;
        stats->over_farea = over_farea;
        stats->bitmap_inconsistencies = bitmap_inconsistencies;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Provede rychlý formát FSMZ disku.
 *
 * Vymaže IPLPRO hlavičku, inicializuje DINFO blok a vynuluje adresářové
 * bloky. Datová oblast se nemaže.
 *
 * @param[in] disc Otevřený disk v FSMZ formátu (RW). Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu (DSK_TOOLS_IDENTFORMAT_MZBASIC).
 */
en_MZDSK_RES fsmz_tool_fast_format ( st_MZDSK_DISC *disc ) {

    en_MZDSK_RES err;
    uint8_t dma[FSMZ_SECTOR_SIZE];
    st_FSMZ_IPLPRO_BLOCK *iplpro = (st_FSMZ_IPLPRO_BLOCK *) dma;
    st_FSMZ_DINFO_BLOCK *dinfo = (st_FSMZ_DINFO_BLOCK *) dma;

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* Vymazání IPLPRO */
    memset ( iplpro, 0x00, sizeof ( st_FSMZ_IPLPRO_BLOCK ) );
    err = fs_mz_write_iplpro ( disc, iplpro );
    if ( err ) return err;

    /* Inicializace DINFO */
    memset ( dinfo, 0x00, sizeof ( st_FSMZ_DINFO_BLOCK ) );
    dinfo->volume_number = FSMZ_DINFO_SLAVE;
    dinfo->farea = FSMZ_DEFAULT_FAREA_BLOCK;
    dinfo->used = dinfo->farea;
    uint16_t sectors_on_disc = disc->tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK;
    uint16_t max_adresable_blocks = ( FSMZ_FAREA_BITMAP_SIZE * 8 ) + dinfo->farea;
    dinfo->blocks = ( sectors_on_disc >= max_adresable_blocks )
                    ? ( max_adresable_blocks - 1 ) : ( sectors_on_disc - 1 );
    err = fsmz_write_dinfo ( disc, dinfo );
    if ( err ) return err;

    /* Vymazání adresáře */
    memset ( &dma, 0x00, sizeof ( dma ) );

    int i;
    for ( i = FSMZ_ALOCBLOCK_DIR + 15; i > FSMZ_ALOCBLOCK_DIR; i-- ) {
        err = fsmz_write_blocks ( disc, i, FSMZ_SECTOR_SIZE, &dma );
        if ( err ) return err;
    }

    /* Dva bajty na začátku prvního bloku adresáře (standard FSMZ) */
    dma[0] = 0x80;
    dma[1] = 0x01;
    err = fsmz_write_blocks ( disc, i, FSMZ_SECTOR_SIZE, &dma );
    return err;
}


/**
 * @brief Provede formát souborové oblasti FSMZ disku se zachováním bootstrapu.
 *
 * Vyčistí adresář a reinicializuje DINFO blok (prázdná bitmapa, farea, blocks).
 * Na rozdíl od fsmz_tool_fast_format() zachovává IPLPRO hlavičku a bootstrap
 * data. Pokud na disku existuje platný bootstrap (Normal i Bottom), jeho bloky
 * se alokují v nové bitmapě.
 *
 * Algoritmus:
 * 1. Přečte IPLPRO hlavičku a zjistí, zda existuje platný bootstrap.
 * 2. Inicializuje DINFO s prázdnou bitmapou.
 * 3. Pokud bootstrap existuje a jeho data leží v souborové oblasti
 *    (blok >= farea), alokuje příslušné bloky v bitmapě.
 * 4. Vymaže adresářové bloky (16-31) a zapíše inicializační bajty.
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
 *       alokovány v nové bitmapě. Bottom bootstrap (bloky 1-14) se zachová
 *       automaticky, protože leží mimo souborovou oblast.
 */
en_MZDSK_RES fsmz_tool_format_file_area ( st_MZDSK_DISC *disc ) {

    en_MZDSK_RES err;
    uint8_t dma[FSMZ_SECTOR_SIZE];
    st_FSMZ_IPLPRO_BLOCK iplpro;
    st_FSMZ_DINFO_BLOCK *dinfo = (st_FSMZ_DINFO_BLOCK *) dma;

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* Přečíst IPLPRO hlavičku - zjistit, zda existuje bootstrap */
    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    int has_bootstrap = ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro ) );

    /* Inicializace DINFO */
    memset ( dinfo, 0x00, sizeof ( st_FSMZ_DINFO_BLOCK ) );
    dinfo->farea = FSMZ_DEFAULT_FAREA_BLOCK;
    dinfo->used = dinfo->farea;
    uint16_t sectors_on_disc = disc->tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK;
    uint16_t max_adresable_blocks = ( FSMZ_FAREA_BITMAP_SIZE * 8 ) + dinfo->farea;
    dinfo->blocks = ( sectors_on_disc >= max_adresable_blocks )
                    ? ( max_adresable_blocks - 1 ) : ( sectors_on_disc - 1 );

    if ( has_bootstrap ) {
        dinfo->volume_number = FSMZ_DINFO_MASTER;
        /* Pokud bootstrap data leží v souborové oblasti, alokovat v bitmapě */
        if ( iplpro.block >= dinfo->farea ) {
            uint16_t count_blocks = fsmz_blocks_from_size ( iplpro.fsize );
            dinfo->used += count_blocks;
            fsmz_update_farea_bitmap ( (uint8_t *) &dinfo->map,
                                        FSMZ_DINFO_BITMAP_SET,
                                        iplpro.block - dinfo->farea,
                                        count_blocks );
        }
    } else {
        dinfo->volume_number = FSMZ_DINFO_SLAVE;
    }

    err = fsmz_write_dinfo ( disc, dinfo );
    if ( err ) return err;

    /* Vymazání adresáře */
    memset ( &dma, 0x00, sizeof ( dma ) );

    int i;
    for ( i = FSMZ_ALOCBLOCK_DIR + 15; i > FSMZ_ALOCBLOCK_DIR; i-- ) {
        err = fsmz_write_blocks ( disc, i, FSMZ_SECTOR_SIZE, &dma );
        if ( err ) return err;
    }

    /* Dva bajty na začátku prvního bloku adresáře (standard FSMZ) */
    dma[0] = 0x80;
    dma[1] = 0x01;
    err = fsmz_write_blocks ( disc, i, FSMZ_SECTOR_SIZE, &dma );
    return err;
}


/**
 * @brief Opraví DINFO blok přepočítáním bitmapy a počítadel.
 *
 * Projde celý adresář a bootstrap, přepočítá bitmapu souborové oblasti
 * a čítač obsazených bloků. Pole @c farea a @c blocks přepíše hodnotami
 * odvozenými z geometrie disku - starý obsah se nepoužívá, takže repair
 * opraví i DINFO poškozený náhodnými daty. Opravenou DINFO zapíše
 * zpět na disk.
 *
 * @param[in] disc           Otevřený disk v FSMZ formátu (RW). Nesmí být NULL.
 * @param[in] fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu (DSK_TOOLS_IDENTFORMAT_MZBASIC).
 */
en_MZDSK_RES fsmz_tool_repair_dinfo ( st_MZDSK_DISC *disc,
                                        uint8_t fsmz_dir_items ) {

    en_MZDSK_RES err;
    st_FSMZ_DINFO_BLOCK dinfo;
    st_FSMZ_IPLPRO_BLOCK iplpro;
    st_FSMZ_DIR dir;
    st_FSMZ_DIR_ITEM *diritem;

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* Reinicializujeme DINFO od nuly - farea a blocks odvodíme z geometrie
       disku, takže opravíme i případ, kdy byl celý DINFO blok poškozen. */
    memset ( &dinfo, 0x00, sizeof ( dinfo ) );
    dinfo.farea = FSMZ_DEFAULT_FAREA_BLOCK;
    uint16_t sectors_on_disc = disc->tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK;
    uint16_t max_adresable_blocks = ( FSMZ_FAREA_BITMAP_SIZE * 8 ) + dinfo.farea;
    dinfo.blocks = ( sectors_on_disc >= max_adresable_blocks )
                    ? ( max_adresable_blocks - 1 ) : ( sectors_on_disc - 1 );
    dinfo.used = dinfo.farea;

    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    if ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        dinfo.volume_number = FSMZ_DINFO_MASTER;
        if ( iplpro.block >= dinfo.farea ) {
            uint16_t count_blocks = fsmz_blocks_from_size ( iplpro.fsize );
            dinfo.used += count_blocks;
            fsmz_update_farea_bitmap ( (uint8_t *) &dinfo.map,
                                        FSMZ_DINFO_BITMAP_SET,
                                        iplpro.block - dinfo.farea,
                                        count_blocks );
        }
    } else {
        dinfo.volume_number = FSMZ_DINFO_SLAVE;
    }

    err = fsmz_open_dir ( disc, &dir );
    if ( err ) return err;

    while ( 1 ) {
        diritem = fsmz_read_dir ( disc, &dir, fsmz_dir_items, &err );
        if ( err ) break;

        /* ignorujeme smazané položky */
        if ( diritem->ftype ) {
            uint16_t count_blocks = fsmz_blocks_from_size ( diritem->fsize );
            dinfo.used += count_blocks;
            fsmz_update_farea_bitmap ( (uint8_t *) &dinfo.map,
                                        FSMZ_DINFO_BITMAP_SET,
                                        diritem->block - dinfo.farea,
                                        count_blocks );
        }
    }
    if ( err != MZDSK_RES_FILE_NOT_FOUND ) return err;

    return fsmz_write_dinfo ( disc, &dinfo );
}


/**
 * @brief Najde položku adresáře podle pořadového čísla (ID).
 *
 * ID je 0-based index v adresáři. Interně se přičte 1, protože
 * první položka adresáře (index 0) je rezervovaná systémem
 * (obsahuje 0x80, 0x01, 0x00...).
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param search_item_id Pořadové číslo položky (0-based, tj. 0 = první
 *                       uživatelská položka).
 * @param dir_cache Struktura adresáře pro cachování (může být NULL -
 *                  použije se lokální proměnná).
 * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
 * @param[out] err Výstupní chybový kód: MZDSK_RES_OK = úspěch,
 *                 MZDSK_RES_FILE_NFND = pozice mimo rozsah, záporné = chyba.
 * @return Ukazatel na položku adresáře, nebo NULL při chybě.
 *
 * @warning Vrácený ukazatel směřuje do disc->cache a je platný pouze
 *          do dalšího čtení sektoru.
 */
st_FSMZ_DIR_ITEM* fsmz_tool_get_diritem_pointer_and_dir_by_id ( st_MZDSK_DISC *disc, uint8_t search_item_id, st_FSMZ_DIR *dir_cache, uint8_t fsmz_dir_items, en_MZDSK_RES *err ) {

    st_FSMZ_DIR dir_local_cache;
    st_FSMZ_DIR *dir_p = &dir_local_cache;
    st_FSMZ_DIR_ITEM *diritem;

    search_item_id += 1;

    if ( dir_cache != NULL ) dir_p = dir_cache;

    *err = fsmz_read_dirblock_with_diritem_position ( disc, dir_p, search_item_id, fsmz_dir_items );
    if ( *err ) return NULL;
    diritem = &dir_p->dir_bl->item [ ( search_item_id ) & 0x07 ];
    return diritem;
}


/* =========================================================================
 * Defragmentace FSMZ - interní pomocné funkce
 * ========================================================================= */


/**
 * @brief Načte tělo souboru z FSMZ bloků a zapíše ho jako MZF tělo do paměťového handleru.
 *
 * Alokuje dočasný buffer o velikosti fsize, přečte data z FSMZ bloků
 * počínaje zadaným blokem a zapíše je do handleru jako MZF body.
 *
 * @param[in] disc    Otevřený disk v FSMZ formátu. Nesmí být NULL.
 * @param[in] block   Číslo počátečního alokačního bloku dat.
 * @param[in] fsize   Velikost dat v bajtech.
 * @param[in] handler Paměťový handler pro zápis MZF body. Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_UNKNOWN_ERROR při selhání
 *         alokace nebo zápisu MZF těla, jinak chybový kód z fsmz_read_blocks.
 *
 * @pre handler musí mít již zapsanou MZF hlavičku (zápis body probíhá
 *      na offset MZF_HEADER_SIZE).
 */
static en_MZDSK_RES defrag_write_mzf_body ( st_MZDSK_DISC *disc, uint16_t block, uint16_t fsize, st_HANDLER *handler ) {

    uint8_t *body = malloc ( fsize );
    if ( !body ) return MZDSK_RES_UNKNOWN_ERROR;

    en_MZDSK_RES err = fsmz_read_blocks ( disc, block, fsize, body );
    if ( err ) {
        free ( body );
        return err;
    }

    if ( EXIT_FAILURE == mzf_write_body ( handler, body, fsize ) ) {
        free ( body );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    free ( body );
    return MZDSK_RES_OK;
}


/**
 * @brief Vytvoří MZF soubor v paměti z FSMZ bloku.
 *
 * Alokuje paměťový handler přes generic_driver_open_memory(), zapíše do něj
 * MZF hlavičku a tělo načtené z FSMZ bloků. Výsledný handler obsahuje
 * kompletní MZF soubor v paměti.
 *
 * @param[in]  disc   Otevřený disk v FSMZ formátu. Nesmí být NULL.
 * @param[in]  mzfhdr MZF hlavička k zápisu. Nesmí být NULL.
 * @param[in]  block  Číslo počátečního alokačního bloku dat.
 * @param[out] err    Výstupní chybový kód.
 *
 * @return Ukazatel na paměťový handler s kompletním MZF souborem,
 *         nebo NULL při chybě.
 *
 * @note Volající je zodpovědný za uvolnění handleru
 *       (generic_driver_close + free).
 */
static st_HANDLER* defrag_create_mem_mzf ( st_MZDSK_DISC *disc, st_MZF_HEADER *mzfhdr, uint16_t block, en_MZDSK_RES *err ) {

    st_HANDLER *handler = generic_driver_open_memory ( NULL, &g_memory_driver_realloc, 1 );
    *err = MZDSK_RES_UNKNOWN_ERROR;

    if ( !handler ) return NULL;

    handler->spec.memspec.swelling_enabled = 1;

    if ( EXIT_SUCCESS != mzf_write_header ( handler, mzfhdr ) ) {
        generic_driver_close ( handler );
        free ( handler );
        return NULL;
    }

    *err = defrag_write_mzf_body ( disc, block, mzfhdr->fsize, handler );
    if ( *err ) {
        generic_driver_close ( handler );
        free ( handler );
        return NULL;
    }

    *err = MZDSK_RES_OK;
    return handler;
}


/**
 * @brief Zapíše soubor na zformátovaný disk bez kontroly unikátnosti jména.
 *
 * Najde volné souvislé místo, zapíše data, vytvoří položku adresáře
 * na zadané pozici a aktualizuje DINFO bitmapu. Na rozdíl od fsmz_write_file()
 * nekontroluje, zda jméno souboru již existuje - to je v kontextu defragmentace
 * zbytečné, protože disk byl právě naformátován.
 *
 * @param[in] disc            Otevřený disk v FSMZ formátu (RW). Nesmí být NULL.
 * @param[in] ftype           Souborový typ.
 * @param[in] mz_fname        Jméno souboru v Sharp MZ ASCII. Nesmí být NULL.
 * @param[in] fsize           Velikost souboru v bajtech.
 * @param[in] fstrt           Startovací adresa v paměti Z80.
 * @param[in] fexec           Spouštěcí adresa v paměti Z80.
 * @param[in] locked          Příznak uzamčení souboru.
 * @param[in] src             Zdrojový buffer s daty souboru. Nesmí být NULL.
 * @param[in] dir_position    Pozice v adresáři (1-based).
 * @param[in] fsmz_dir_items  Maximální počet položek adresáře (63 nebo 127).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v FSMZ formátu se zformátovaným prázdným adresářem.
 */
static en_MZDSK_RES defrag_write_file ( st_MZDSK_DISC *disc, uint8_t ftype, uint8_t *mz_fname,
                                         uint16_t fsize, uint16_t fstrt, uint16_t fexec, uint8_t locked,
                                         void *src, uint8_t dir_position, uint8_t fsmz_dir_items ) {

    en_MZDSK_RES err;
    uint16_t start_block = 0;
    uint16_t num_blocks = fsmz_blocks_from_size ( fsize );

    err = fsmz_check_free_blocks ( disc, num_blocks, &start_block );
    if ( err ) return err;

    err = fsmz_write_blocks ( disc, start_block, fsize, src );
    if ( err ) return err;

    /* Připravíme položku adresáře */
    st_FSMZ_DIR_ITEM diritem;
    memset ( &diritem, 0x00, sizeof ( diritem ) );
    diritem.ftype = ftype;
    memcpy ( diritem.fname, mz_fname, FSMZ_FNAME_LENGTH );
    diritem.locked = locked;
    diritem.fsize = fsize;
    diritem.fstrt = fstrt;
    diritem.fexec = fexec;
    diritem.block = start_block;

    err = fsmz_save_diritem_to_position ( disc, &diritem, dir_position, fsmz_dir_items );
    if ( err ) return err;

    return fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_SET, start_block, num_blocks );
}


/**
 * @brief Provede defragmentaci FSMZ disku.
 *
 * Načte všechny soubory (a volitelně bootstrap) z FSMZ disku do paměti,
 * provede rychlý formát a znovu zapíše vše sekvenčně od začátku
 * souborové oblasti bez mezer.
 *
 * @param[in] disc           Otevřený disk v FSMZ formátu (RW). Nesmí být NULL.
 * @param[in] fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
 * @param[in] progress_cb    Callback pro hlášení průběhu (může být NULL).
 * @param[in] cb_data        Uživatelská data pro callback (může být NULL).
 *
 * @return MZDSK_RES_OK při úspěchu.
 * @return MZDSK_RES_FORMAT_ERROR pokud disc->format != DSK_TOOLS_IDENTFORMAT_MZBASIC.
 * @return MZDSK_RES_INVALID_PARAM pokud handler NENÍ typu
 *         HANDLER_TYPE_MEMORY (audit H-5).
 * @return Jiný chybový kód při selhání IO nebo zápisu.
 *
 * @pre Disk musí být v plném FSMZ formátu (DSK_TOOLS_IDENTFORMAT_MZBASIC).
 * @pre disc->handler->type == HANDLER_TYPE_MEMORY (audit H-5)
 *
 * @warning Defrag NENÍ interně transakční - selhání během zápisu souborů
 *          nechává disk v nekonzistentním stavu. Proto je vyžadován memory
 *          handler: volající otevře disk přes mzdsk_disc_open_memory(),
 *          po úspěšném defragu flushne přes mzdsk_disc_save(). Při chybě
 *          volající save nevolá a originální soubor zůstává nedotčený.
 */
en_MZDSK_RES fsmz_tool_defrag ( st_MZDSK_DISC *disc,
                                  uint8_t fsmz_dir_items,
                                  fsmz_tool_defrag_cb_t progress_cb,
                                  void *cb_data ) {

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* Audit H-5: defrag NENÍ transakční v rámci knihovny - zápis
     * IPLPRO + souborů probíhá přímo nad handlerem. Selhání u file
     * handleru = nenávratná ztráta dat. Vynucujeme memory handler -
     * volající musí disc otevřít přes mzdsk_disc_open_memory() a
     * po úspěchu flushnout mzdsk_disc_save(). Volající (mzdsk-fsmz
     * CLI + GUI session) tento pattern splňují. */
    if ( disc->handler == NULL || disc->handler->type != HANDLER_TYPE_MEMORY ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    en_MZDSK_RES err;
    st_FSMZ_DINFO_BLOCK dinfo;
    st_FSMZ_IPLPRO_BLOCK iplpro;
    st_MZF_HEADER *mzfhdr = NULL;
    st_HANDLER *iplpro_handler = NULL;
    st_HANDLER **file_handler = malloc ( sizeof ( void* ) * fsmz_dir_items );
    uint8_t *mz_fname = malloc ( sizeof ( uint8_t ) * FSMZ_FNAME_LENGTH * fsmz_dir_items );
    uint8_t *file_locked = malloc ( sizeof ( uint8_t ) * fsmz_dir_items );
    int files_count = 0;

    if ( !file_handler || !mz_fname || !file_locked ) {
        free ( file_handler );
        free ( mz_fname );
        free ( file_locked );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    err = fsmz_read_dinfo ( disc, &dinfo );
    if ( !err ) {
        err = fsmz_read_iplpro ( disc, &iplpro );
    }

    if ( !err ) {

        if ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro ) ) {
            st_MZF_HEADER *boot_mzfhdr = mzf_tools_create_mzfhdr ( iplpro.ftype, iplpro.fsize, iplpro.fstrt, iplpro.fexec, ( uint8_t* ) &iplpro.fname, FSMZ_IPLFNAME_LENGTH - 1, ( uint8_t* ) &iplpro.cmnt );
            if ( !boot_mzfhdr ) {
                err = MZDSK_RES_UNKNOWN_ERROR;
            } else {

                /* Načteme bootstrap, pokud je v souborové oblasti */
                if ( iplpro.block >= dinfo.farea && iplpro.block <= dinfo.blocks ) {

                    if ( progress_cb ) progress_cb ( "Defrag: bootstrap will be preserved\n", cb_data );

                    iplpro_handler = defrag_create_mem_mzf ( disc, boot_mzfhdr, iplpro.block, &err );
                }

                free ( boot_mzfhdr );
            }
        }

        if ( !err ) {
            int i;
            for ( i = 0; i < fsmz_dir_items; i++ ) {
                st_FSMZ_DIR_ITEM *diritem = fsmz_tool_get_diritem_pointer_and_dir_by_id ( disc, i, NULL, fsmz_dir_items, &err );
                if ( err != MZDSK_RES_FILE_NOT_FOUND ) {
                    if ( err ) break;

                    if ( !diritem->ftype ) continue;

                    mzfhdr = mzf_tools_create_mzfhdr ( diritem->ftype, diritem->fsize, diritem->fstrt, diritem->fexec, ( uint8_t* ) &diritem->fname, FSMZ_FNAME_LENGTH - 1, NULL );
                    if ( !mzfhdr ) {
                        err = MZDSK_RES_UNKNOWN_ERROR;
                        break;
                    }

                    /* Načteme všechny soubory - formát je vymaže */
                    memcpy ( &mz_fname[( files_count * FSMZ_FNAME_LENGTH )], diritem->fname, FSMZ_FNAME_LENGTH );
                    file_locked[files_count] = diritem->locked;
                    file_handler[files_count] = defrag_create_mem_mzf ( disc, mzfhdr, diritem->block, &err );
                    if ( !file_handler[files_count] ) {
                        free ( mzfhdr );
                        mzfhdr = NULL;
                        break;
                    }
                    files_count++;

                    free ( mzfhdr );
                    mzfhdr = NULL;
                    if ( err ) break;
                }
            }
        }
    }

    if ( mzfhdr != NULL ) free ( mzfhdr );

    /* Máme načteno - provedeme formát a znovu zapíšeme vše sekvenčně.
     * Místo mazání po jednom a přepisování provedeme čistý formát
     * celého disku a pak zapíšeme vše od začátku.
     */
    if ( !err ) {

        /* Formát disku - vynuluje DINFO/dir/IPLPRO */
        if ( progress_cb ) progress_cb ( "Defrag: formatting disk\n", cb_data );
        err = fsmz_tool_fast_format ( disc );

        /* Uložení bootstrapu */
        if ( !err && iplpro_handler != NULL ) {

            st_FSMZ_IPLPRO_BLOCK new_iplpro;
            st_MZF_HEADER boot_mzfhdr;

            if ( EXIT_SUCCESS != mzf_read_header ( iplpro_handler, &boot_mzfhdr ) ) {
                err = MZDSK_RES_UNKNOWN_ERROR;
            } else {
                new_iplpro.ftype = 0x03;
                memcpy ( &new_iplpro.iplpro, "IPLPRO", sizeof ( new_iplpro.iplpro ) );

                new_iplpro.fsize = boot_mzfhdr.fsize;
                new_iplpro.fstrt = boot_mzfhdr.fstrt;
                new_iplpro.fexec = boot_mzfhdr.fexec;

                memset ( new_iplpro.fname, 0x0d, FSMZ_IPLFNAME_LENGTH );
                memcpy ( &new_iplpro.fname, &boot_mzfhdr.fname, FSMZ_IPLFNAME_LENGTH - 1 );

                memset ( new_iplpro.cmnt, 0x00, FSMZ_IPLCMNT_LENGTH );
                memcpy ( &new_iplpro.cmnt, &boot_mzfhdr.cmnt, MZF_CMNT_LENGTH );

                uint16_t count_blocks = fsmz_blocks_from_size ( new_iplpro.fsize );

                /* Inicializace new_iplpro.block pro případ, že fsmz_check_free_blocks
                 * selže - bez toho by se pokračovalo s neinicializovanou hodnotou. */
                new_iplpro.block = 0;
                err = fsmz_check_free_blocks ( disc, count_blocks, &new_iplpro.block );

                if ( !err ) {

                    /* Alokovat přesně tolik, kolik je potřeba (fsize je uint16_t,
                     * max 65535 B). Dřívější malloc(0xffff) plýtval pamětí a nic
                     * negarantoval proti špatné fsize. Audit H-14. */
                    uint8_t *data = malloc ( new_iplpro.fsize );
                    if ( !data ) {
                        err = MZDSK_RES_UNKNOWN_ERROR;
                    } else {
                        if ( EXIT_SUCCESS != mzf_read_body ( iplpro_handler, data, new_iplpro.fsize ) ) {
                            err = MZDSK_RES_UNKNOWN_ERROR;
                        } else {
                            err = fsmz_write_blocks ( disc, new_iplpro.block, new_iplpro.fsize, data );
                            if ( !err ) {
                                err = fs_mz_write_iplpro ( disc, &new_iplpro );
                                if ( !err ) {
                                    err = fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_SET, new_iplpro.block, count_blocks );
                                    if ( !err ) {
                                        err = fsmz_update_dinfo_volume_number ( disc, FSMZ_DINFO_MASTER );
                                    }
                                }
                            }
                        }
                        free ( data );
                    }
                }
            }
        }

        /* Uložení všech souborů sekvenčně */
        if ( !err ) {

            if ( progress_cb && files_count > 0 ) progress_cb ( "Defrag: writing files\n", cb_data );

            int i;
            for ( i = 0; i < files_count; i++ ) {

                st_MZF_HEADER file_mzfhdr;
                if ( EXIT_SUCCESS != mzf_read_header ( file_handler[i], &file_mzfhdr ) ) {
                    err = MZDSK_RES_UNKNOWN_ERROR;
                } else {

                    /* Alokovat přesně tolik, kolik je potřeba (fsize je uint16_t).
                     * Audit H-14 - dřívější malloc(0xffff) plýtval pamětí. */
                    uint8_t *data = malloc ( file_mzfhdr.fsize );
                    if ( !data ) {
                        err = MZDSK_RES_UNKNOWN_ERROR;
                    } else {
                        if ( EXIT_SUCCESS != mzf_read_body ( file_handler[i], data, file_mzfhdr.fsize ) ) {
                            err = MZDSK_RES_UNKNOWN_ERROR;
                        } else {
                            /* dir pozice: 1-based (0 je rezervovaná), soubory začínají od 1 */
                            uint8_t dir_pos = (uint8_t) ( i + 1 );
                            err = defrag_write_file ( disc, file_mzfhdr.ftype, MZF_UINT8_FNAME ( file_mzfhdr.fname ),
                                                      file_mzfhdr.fsize, file_mzfhdr.fstrt, file_mzfhdr.fexec,
                                                      file_locked[i], data, dir_pos, fsmz_dir_items );
                        }
                        free ( data );
                    }
                }

                if ( err ) break;
            }
        }
    }

    if ( iplpro_handler != NULL ) {
        generic_driver_close ( iplpro_handler );
        free ( iplpro_handler );
    }

    int i;
    for ( i = 0; i < files_count; i++ ) {
        generic_driver_close ( file_handler[i] );
        free ( file_handler[i] );
    }

    free ( file_handler );
    free ( mz_fname );
    free ( file_locked );

    return err;
}
