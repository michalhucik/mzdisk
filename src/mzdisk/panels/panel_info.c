/**
 * @file panel_info.c
 * @brief Naplnění datového modelu informačního panelu z otevřeného disku.
 *
 * Čte geometrii, identifikaci formátu a FS-specifické statistiky.
 * Ekvivalent logiky z CLI nástroje mzdsk-info (cmd_show_info).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <string.h>
#include "panel_info.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"


/**
 * @brief Dekóduje velikost sektoru z en_DSK_SECTOR_SIZE na bajty.
 */
static uint16_t decode_sector_size ( en_DSK_SECTOR_SIZE ssize )
{
    return (uint16_t) ( 128 << (uint8_t) ssize );
}


/**
 * @brief Naplní geometrii a track rules z disc->tracks_rules.
 */
static void load_geometry ( st_PANEL_INFO_DATA *data, st_MZDSK_DISC *disc )
{
    st_DSK_TOOLS_TRACKS_RULES_INFO *tr = disc->tracks_rules;
    if ( !tr ) return;

    data->total_tracks = tr->total_tracks;
    data->sides = tr->sides;
    data->rule_count = ( tr->count_rules > PANEL_INFO_MAX_RULES )
                       ? PANEL_INFO_MAX_RULES
                       : (int) tr->count_rules;

    data->total_size_bytes = 0;

    for ( int i = 0; i < data->rule_count; i++ ) {
        st_DSK_TOOLS_TRACK_RULE_INFO *r = &tr->rule[i];
        data->rules[i].from_track = r->from_track;
        data->rules[i].count_tracks = r->count_tracks;
        data->rules[i].sectors = r->sectors;
        data->rules[i].sector_size = decode_sector_size ( r->ssize );
        data->rules[i].is_inverted = ( r->sectors == 16 && r->ssize == DSK_SECTOR_SIZE_256 );

        data->total_size_bytes += (uint32_t) r->count_tracks * r->sectors * data->rules[i].sector_size;
    }
}


/**
 * @brief Konvertuje IPLPRO fname z Sharp MZ ASCII do data->boot_name.
 */
static void convert_iplpro_name ( st_PANEL_INFO_DATA *data, const st_FSMZ_IPLPRO_BLOCK *iplpro )
{
    int j = 0;
    while ( j < (int) sizeof ( iplpro->fname ) && iplpro->fname[j] >= 0x20 ) {
        data->boot_name[j] = (char) sharpmz_cnv_from ( iplpro->fname[j] );
        data->mz_boot_name[j] = iplpro->fname[j];
        j++;
    }
    data->boot_name[j] = '\0';
    data->mz_boot_name[j] = 0;
    data->mz_boot_name_len = j;
}


/**
 * @brief Naplní informace o bootstrapu (IPLPRO) včetně typu a velikosti.
 *
 * Typ bootstrapu:
 * - Mini: bloky 1-14 (FSMZ kompatibilní - DINFO/dir nedotčeny)
 * - Bottom: blok >= 1, blok < 16, ale block_end > 14 (přesahuje do DINFO/dir)
 * - Normal: blok >= farea (v souborové části)
 * - Over FSMZ: blok > dinfo.blocks (nad file area)
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 * @param have_dinfo True pokud DINFO blok byl úspěšně přečten.
 * @param dinfo Ukazatel na DINFO blok (platný jen pokud have_dinfo == true).
 */
static void load_boot_info ( st_PANEL_INFO_DATA *data, st_MZDSK_DISC *disc,
                             bool have_dinfo, const st_FSMZ_DINFO_BLOCK *dinfo )
{
    st_FSMZ_IPLPRO_BLOCK iplpro;
    if ( fsmz_read_iplpro ( disc, &iplpro ) != MZDSK_RES_OK ) return;
    if ( fsmz_tool_test_iplpro_header ( &iplpro ) != EXIT_SUCCESS ) return;

    data->has_boot_info = true;
    convert_iplpro_name ( data, &iplpro );

    data->boot_size_bytes = iplpro.fsize;
    data->boot_start_block = iplpro.block;

    /* počet bloků (zaokrouhlení nahoru) */
    data->boot_blocks = iplpro.fsize / FSMZ_SECTOR_SIZE;
    if ( iplpro.fsize % FSMZ_SECTOR_SIZE ) data->boot_blocks++;

    /* koncový blok bootstrapu */
    uint16_t block_end = ( data->boot_blocks > 0 )
                         ? ( iplpro.block + data->boot_blocks - 1 )
                         : iplpro.block;

    /* určení typu bootstrapu */
    if ( iplpro.block >= 1 && block_end <= 14 ) {
        strncpy ( data->boot_type, "Mini", sizeof ( data->boot_type ) );
    } else if ( iplpro.block >= 1 && iplpro.block < 16 && block_end > 14 ) {
        strncpy ( data->boot_type, "Bottom", sizeof ( data->boot_type ) );
    } else if ( have_dinfo && iplpro.block > dinfo->blocks ) {
        strncpy ( data->boot_type, "Over FSMZ", sizeof ( data->boot_type ) );
    } else {
        strncpy ( data->boot_type, "Normal", sizeof ( data->boot_type ) );
    }
}


/**
 * @brief Naplní FSMZ specifické informace (blokové statistiky + bootstrap).
 */
static void load_fsmz_info ( st_PANEL_INFO_DATA *data, st_MZDSK_DISC *disc )
{
    /* blokové statistiky z DINFO */
    /* Inicializace na 0 - pokud `fsmz_read_dinfo` selže, stále
     * posíláme `&dinfo` do `load_boot_info`, která nad ním provádí
     * read-path (i když pod `if (have_dinfo)`). Audit M-43. */
    st_FSMZ_DINFO_BLOCK dinfo = { 0 };
    bool have_dinfo = false;

    if ( fsmz_read_dinfo ( disc, &dinfo ) == MZDSK_RES_OK ) {
        have_dinfo = true;
        data->has_fsmz_info = true;
        data->fsmz_total_blocks = dinfo.blocks + 1;
        data->fsmz_used_blocks = dinfo.used;
        data->fsmz_free_blocks = data->fsmz_total_blocks - dinfo.used;
    }

    /* bootstrap info */
    load_boot_info ( data, disc, have_dinfo, &dinfo );
}


/**
 * @brief Detekuje FSMZ boot oblast na non-FSMZ discích.
 *
 * Spočítá po sobě jdoucí stopy s formátem 16x256B od začátku disku.
 * Pokud existují, přečte IPLPRO a zjistí bootstrap info.
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 */
static void load_fsmz_boot_area ( st_PANEL_INFO_DATA *data, st_MZDSK_DISC *disc )
{
    st_DSK_TOOLS_TRACKS_RULES_INFO *tr = disc->tracks_rules;
    if ( !tr || !tr->mzboot_track ) return;

    /* spočítej po sobě jdoucí FSMZ stopy od začátku */
    uint16_t fsmz_tracks = 0;
    for ( int i = 0; i < (int) tr->count_rules; i++ ) {
        st_DSK_TOOLS_TRACK_RULE_INFO *r = &tr->rule[i];
        if ( r->sectors == 16 && r->ssize == DSK_SECTOR_SIZE_256 ) {
            fsmz_tracks += r->count_tracks;
        } else {
            /* přerušení - FSMZ stopy musí být na začátku a po sobě */
            if ( fsmz_tracks > 0 ) break;
        }
    }

    if ( fsmz_tracks == 0 ) return;

    data->has_fsmz_boot_area = true;
    data->fsmz_boot_tracks = fsmz_tracks;
    data->fsmz_boot_blocks = fsmz_tracks * 16;  /* 16 bloků na stopu */

    /* zkusit přečíst IPLPRO z boot oblasti */
    load_boot_info ( data, disc, false, NULL );
}


/**
 * @brief Naplní CP/M specifické informace z DPB.
 */
static void load_cpm_info ( st_PANEL_INFO_DATA *data, st_MZDSK_DETECT_RESULT *detect )
{
    data->has_cpm_info = true;
    st_MZDSK_CPM_DPB *dpb = &detect->cpm_dpb;

    data->cpm_block_size = dpb->block_size;
    data->cpm_total_blocks = dpb->dsm + 1;
    data->cpm_dir_entries = dpb->drm + 1;
    data->cpm_reserved_tracks = dpb->off;

    switch ( detect->cpm_format ) {
        case MZDSK_CPM_FORMAT_SD:
            strncpy ( data->cpm_preset_name, "SD (Lamac)", sizeof ( data->cpm_preset_name ) );
            break;
        case MZDSK_CPM_FORMAT_HD:
            strncpy ( data->cpm_preset_name, "HD (Lucky-Soft)", sizeof ( data->cpm_preset_name ) );
            break;
        default:
            strncpy ( data->cpm_preset_name, "Custom", sizeof ( data->cpm_preset_name ) );
            break;
    }
}


void panel_info_load ( st_PANEL_INFO_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect )
{
    memset ( data, 0, sizeof ( *data ) );

    data->dsk_format = disc->format;
    data->fs_type = detect->type;

    load_geometry ( data, disc );

    switch ( detect->type ) {
        case MZDSK_FS_FSMZ:
        case MZDSK_FS_BOOT_ONLY:
            load_fsmz_info ( data, disc );
            break;
        case MZDSK_FS_CPM:
            load_cpm_info ( data, detect );
            load_fsmz_boot_area ( data, disc );
            break;
        case MZDSK_FS_MRS:
            load_fsmz_boot_area ( data, disc );
            break;
        default:
            load_fsmz_boot_area ( data, disc );
            break;
    }

    data->is_loaded = true;
}


const char* panel_info_format_str ( en_DSK_TOOLS_IDENTFORMAT format )
{
    switch ( format ) {
        case DSK_TOOLS_IDENTFORMAT_MZBASIC:    return "MZ-BASIC (FSMZ)";
        case DSK_TOOLS_IDENTFORMAT_MZCPM:      return "CP/M SD (9x512B)";
        case DSK_TOOLS_IDENTFORMAT_MZCPMHD:    return "CP/M HD (18x512B)";
        case DSK_TOOLS_IDENTFORMAT_MZBOOT:     return "MZ Boot only";
        case DSK_TOOLS_IDENTFORMAT_UNKNOWN:
        default:                                return "Unknown";
    }
}
