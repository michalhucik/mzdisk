/**
 * @file panel_map.c
 * @brief Naplnění datového modelu blokové mapy z otevřeného disku.
 *
 * Unifikuje tři různá API (FSMZ, CP/M, MRS) do společného pole en_MAP_BLOCK_TYPE.
 * Pro CP/M disky navíc sestavuje disk layout (proporcionální přehled oblastí disku).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <string.h>
#include <stdio.h>
#include "panel_map.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"


/**
 * @brief Naplní mapu z FSMZ blokové mapy.
 *
 * Převádí en_FSMZ_BLOCK_TYPE -> en_MAP_BLOCK_TYPE.
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 */
static void load_fsmz_map ( st_PANEL_MAP_DATA *data, st_MZDSK_DISC *disc )
{
    en_FSMZ_BLOCK_TYPE fsmz_map[PANEL_MAP_MAX_BLOCKS];
    st_FSMZ_MAP_STATS stats;

    if ( fsmz_tool_get_block_map ( disc, fsmz_map, PANEL_MAP_MAX_BLOCKS, &stats ) != EXIT_SUCCESS ) {
        return;
    }

    data->block_count = stats.total_blocks;
    if ( data->block_count > PANEL_MAP_MAX_BLOCKS ) {
        data->block_count = PANEL_MAP_MAX_BLOCKS;
    }

    for ( int i = 0; i < data->block_count; i++ ) {
        switch ( fsmz_map[i] ) {
            case FSMZ_BLOCK_FREE:       data->blocks[i] = MAP_BLOCK_FREE; data->free_count++; break;
            case FSMZ_BLOCK_IPLPRO:     data->blocks[i] = MAP_BLOCK_SYSTEM; data->system_count++; break;
            case FSMZ_BLOCK_META:       data->blocks[i] = MAP_BLOCK_META; data->meta_count++; break;
            case FSMZ_BLOCK_DIR:        data->blocks[i] = MAP_BLOCK_DIR; data->dir_count++; break;
            case FSMZ_BLOCK_USED:       data->blocks[i] = MAP_BLOCK_FILE; data->file_count++; break;
            case FSMZ_BLOCK_BOOTSTRAP:  data->blocks[i] = MAP_BLOCK_BOOTSTRAP; data->system_count++; break;
            case FSMZ_BLOCK_OVER_FAREA: data->blocks[i] = MAP_BLOCK_OVER; break;
            default:                    data->blocks[i] = MAP_BLOCK_FREE; data->free_count++; break;
        }
    }

    data->is_loaded = true;
}


/**
 * @brief Naplní boot track mapu (16 bloků stopy 0) pro non-FSMZ systémy.
 *
 * Čte IPLPRO hlavičku a na jejím základě klasifikuje bloky boot stopy:
 * - Blok 0: IPLPRO hlavička (pokud platná) nebo volný
 * - Bootstrap bloky: dle rozsahu system_start..system_end z IPLPRO
 * - Ostatní: volné
 *
 * @param data Výstupní datový model (naplní boot_blocks[], has_boot_map).
 * @param disc Otevřený diskový obraz.
 *
 * @pre data != NULL, disc je platně otevřený.
 * @post Při úspěchu data->has_boot_map == true, boot_block_count == 16.
 */
static void load_boot_track_map ( st_PANEL_MAP_DATA *data, st_MZDSK_DISC *disc )
{
    st_FSMZ_IPLPRO_BLOCK iplpro;

    if ( fsmz_read_iplpro ( disc, &iplpro ) != MZDSK_RES_OK ) {
        return;
    }

    data->boot_block_count = PANEL_MAP_BOOT_BLOCKS;

    /* výchozí: všechny bloky volné */
    for ( int i = 0; i < PANEL_MAP_BOOT_BLOCKS; i++ ) {
        data->boot_blocks[i] = MAP_BLOCK_FREE;
    }

    /* pokud je IPLPRO hlavička platná, označíme blok 0 a bootstrap rozsah */
    if ( fsmz_tool_test_iplpro_header ( &iplpro ) == EXIT_SUCCESS ) {
        data->boot_blocks[0] = MAP_BLOCK_SYSTEM;

        uint16_t system_start = iplpro.block;
        uint16_t system_end = iplpro.block + ( iplpro.fsize / FSMZ_SECTOR_SIZE ) - 1;
        if ( iplpro.fsize % FSMZ_SECTOR_SIZE ) system_end++;

        for ( int i = 0; i < PANEL_MAP_BOOT_BLOCKS; i++ ) {
            if ( i >= system_start && i <= system_end ) {
                data->boot_blocks[i] = MAP_BLOCK_BOOTSTRAP;
            }
        }
    }

    data->has_boot_map = true;
}


/**
 * @brief Naplní mapu z CP/M alokační bitmapy.
 *
 * CP/M má jen dvě hodnoty: obsazeno/volno. Bloky odpovídající
 * directory (odvozeno z AL0/AL1) označíme jako DIR.
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 * @param detect Výsledek auto-detekce (obsahuje DPB).
 */
static void load_cpm_map ( st_PANEL_MAP_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect )
{
    st_MZDSK_CPM_ALLOC_MAP alloc_map;

    if ( mzdsk_cpm_get_alloc_map ( disc, &detect->cpm_dpb, &alloc_map ) != MZDSK_RES_OK ) {
        return;
    }

    data->block_count = alloc_map.total_blocks;
    if ( data->block_count > PANEL_MAP_MAX_BLOCKS ) {
        data->block_count = PANEL_MAP_MAX_BLOCKS;
    }

    /* zjistit directory bloky z AL0/AL1 */
    uint16_t al = ( (uint16_t) detect->cpm_dpb.al0 << 8 ) | detect->cpm_dpb.al1;

    for ( int i = 0; i < data->block_count; i++ ) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        bool is_used = ( alloc_map.map[byte_idx] & ( 1 << bit_idx ) ) != 0;

        if ( is_used ) {
            /* directory bloky: první bity v AL0/AL1 (bit 15 = blok 0, bit 14 = blok 1, ...) */
            if ( i < 16 && ( al & ( 0x8000 >> i ) ) ) {
                data->blocks[i] = MAP_BLOCK_DIR;
                data->dir_count++;
            } else {
                data->blocks[i] = MAP_BLOCK_FILE;
                data->file_count++;
            }
        } else {
            data->blocks[i] = MAP_BLOCK_FREE;
            data->free_count++;
        }
    }

    data->is_loaded = true;
}


/**
 * @brief Naplní mapu z MRS FAT mapy.
 *
 * Převádí en_FSMRS_BLOCK_TYPE -> en_MAP_BLOCK_TYPE.
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 * @param detect Výsledek auto-detekce (obsahuje MRS config).
 */
static void load_mrs_map ( st_PANEL_MAP_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect )
{
    en_FSMRS_BLOCK_TYPE mrs_map[FSMRS_COUNT_BLOCKS];
    st_FSMRS_MAP_STATS stats;

    fsmrs_get_block_map ( &detect->mrs_config, mrs_map, FSMRS_COUNT_BLOCKS, &stats );

    (void) disc;

    data->block_count = stats.total_blocks;
    if ( data->block_count > PANEL_MAP_MAX_BLOCKS ) {
        data->block_count = PANEL_MAP_MAX_BLOCKS;
    }

    for ( int i = 0; i < data->block_count; i++ ) {
        switch ( mrs_map[i] ) {
            case FSMRS_BLOCK_FREE:      data->blocks[i] = MAP_BLOCK_FREE; data->free_count++; break;
            case FSMRS_BLOCK_FAT:       data->blocks[i] = MAP_BLOCK_META; data->meta_count++; break;
            case FSMRS_BLOCK_DIR:       data->blocks[i] = MAP_BLOCK_DIR; data->dir_count++; break;
            case FSMRS_BLOCK_SYSTEM:    data->blocks[i] = MAP_BLOCK_SYSTEM; data->system_count++; break;
            case FSMRS_BLOCK_BAD:       data->blocks[i] = MAP_BLOCK_BAD; data->bad_count++; break;
            case FSMRS_BLOCK_FILE:      data->blocks[i] = MAP_BLOCK_FILE; data->file_count++; break;
            default:                    data->blocks[i] = MAP_BLOCK_FREE; data->free_count++; break;
        }
    }

    data->is_loaded = true;
}


/**
 * @brief Sestaví disk layout segmenty pro CP/M disk.
 *
 * Vytvoří proporcionální přehled oblastí disku:
 * 1. Boot Track (abs. stopa 1, 16x256B = 4096 B)
 * 2. System Tracks (abs. stopy 0, 2..off-1 - CCP+BDOS+BIOS)
 * 3. Directory (adresářové bloky z AL0/AL1)
 * 4. File Data (obsazené datové bloky)
 * 5. Free Space (volné bloky)
 *
 * Velikosti se počítají z DPB parametrů a blokových statistik.
 *
 * @param data   Datový model mapy (musí mít naplněné dir_count, file_count, free_count).
 * @param detect Výsledek auto-detekce (obsahuje cpm_dpb).
 *
 * @pre data->block_count > 0, detect->type == MZDSK_FS_CPM
 * @post data->has_disk_layout == true, layout_segments[] naplněné.
 */
static void load_cpm_disk_layout ( st_PANEL_MAP_DATA *data, st_MZDSK_DETECT_RESULT *detect )
{
    const st_MZDSK_CPM_DPB *dpb = &detect->cpm_dpb;
    int idx = 0;

    /* velikost jedné datové stopy */
    uint32_t data_track_bytes = (uint32_t) dpb->spt * MZDSK_CPM_RECORD_SIZE;
    /* boot track je vždy 16x256B */
    uint32_t boot_track_bytes = PANEL_MAP_BOOT_BLOCKS * 256;

    /* 1. Boot Track */
    st_PANEL_MAP_LAYOUT_SEGMENT *seg = &data->layout_segments[idx++];
    strncpy ( seg->label, "Boot Track", sizeof ( seg->label ) );
    snprintf ( seg->detail, sizeof ( seg->detail ),
               "Abs. track 1 (16x256B, IPLPRO + miniboot)" );
    seg->size_bytes = boot_track_bytes;
    seg->type = MAP_BLOCK_SYSTEM;

    /* 2. System Tracks (off - 1 stop)
     * Audit L-29: dpb->off == 0 by způsobil uint16_t wrap-around na 65535.
     * Normální CP/M DPB má off >= 1 (1 boot + N system), ale custom DPB
     * nebo poškozená data můžou obsahovat 0. */
    uint16_t sys_count = ( dpb->off > 0 ) ? ( dpb->off - 1 ) : 0;
    if ( sys_count > 0 ) {
        seg = &data->layout_segments[idx++];
        strncpy ( seg->label, "System Tracks", sizeof ( seg->label ) );
        if ( dpb->off == 2 ) {
            snprintf ( seg->detail, sizeof ( seg->detail ),
                       "Abs. track 0 - CCP + BDOS + BIOS (%u B)",
                       sys_count * data_track_bytes );
        } else if ( dpb->off == 3 ) {
            snprintf ( seg->detail, sizeof ( seg->detail ),
                       "Abs. tracks 0, 2 - CCP + BDOS + BIOS (%u B)",
                       sys_count * data_track_bytes );
        } else {
            snprintf ( seg->detail, sizeof ( seg->detail ),
                       "Abs. tracks 0, 2-%d - CCP + BDOS + BIOS (%u B)",
                       dpb->off - 1, sys_count * data_track_bytes );
        }
        seg->size_bytes = sys_count * data_track_bytes;
        seg->type = MAP_BLOCK_RESERVED;
    }

    /* 3. Directory */
    if ( data->dir_count > 0 ) {
        seg = &data->layout_segments[idx++];
        strncpy ( seg->label, "Directory", sizeof ( seg->label ) );
        snprintf ( seg->detail, sizeof ( seg->detail ),
                   "%d blocks, %d entries",
                   data->dir_count, dpb->drm + 1 );
        seg->size_bytes = (uint32_t) data->dir_count * dpb->block_size;
        seg->type = MAP_BLOCK_DIR;
    }

    /* 4. File Data */
    if ( data->file_count > 0 ) {
        seg = &data->layout_segments[idx++];
        strncpy ( seg->label, "File Data", sizeof ( seg->label ) );
        snprintf ( seg->detail, sizeof ( seg->detail ),
                   "%d blocks (%u B)",
                   data->file_count,
                   (uint32_t) data->file_count * dpb->block_size );
        seg->size_bytes = (uint32_t) data->file_count * dpb->block_size;
        seg->type = MAP_BLOCK_FILE;
    }

    /* 5. Free Space */
    if ( data->free_count > 0 ) {
        seg = &data->layout_segments[idx++];
        strncpy ( seg->label, "Free", sizeof ( seg->label ) );
        snprintf ( seg->detail, sizeof ( seg->detail ),
                   "%d blocks (%u B)",
                   data->free_count,
                   (uint32_t) data->free_count * dpb->block_size );
        seg->size_bytes = (uint32_t) data->free_count * dpb->block_size;
        seg->type = MAP_BLOCK_FREE;
    }

    data->layout_segment_count = idx;

    /* celková velikost */
    data->layout_total_bytes = 0;
    for ( int i = 0; i < idx; i++ ) {
        data->layout_total_bytes += data->layout_segments[i].size_bytes;
    }

    data->has_disk_layout = true;
}


void panel_map_load ( st_PANEL_MAP_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect )
{
    memset ( data, 0, sizeof ( *data ) );
    data->fs_type = detect->type;

    switch ( detect->type ) {
        case MZDSK_FS_FSMZ:
            load_fsmz_map ( data, disc );
            break;
        case MZDSK_FS_BOOT_ONLY:
            load_boot_track_map ( data, disc );
            data->is_loaded = data->has_boot_map;
            break;
        case MZDSK_FS_CPM:
            load_boot_track_map ( data, disc );
            load_cpm_map ( data, disc, detect );
            load_cpm_disk_layout ( data, detect );
            break;
        case MZDSK_FS_MRS:
            load_boot_track_map ( data, disc );
            load_mrs_map ( data, disc, detect );
            break;
        default:
            break;
    }
}
