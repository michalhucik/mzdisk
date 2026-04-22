/**
 * @file panel_geometry.c
 * @brief Naplnění datového modelu geometrie + logika editace geometrie.
 *
 * Čte disc->tracks_rules a převádí pravidla geometrie na per-track
 * pole st_PANEL_GEOMETRY_TRACK. Detekuje FSMZ inverzi (16x256B).
 * Čte ID sektorů z track headerů v DSK obrazu.
 *
 * Editace geometrie: Change Track, Append Tracks, Shrink.
 * Wrappuje funkce z dsk_tools.h s validací a chybovým hlášením.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "panel_geometry.h"
#include "libs/dsk/dsk_tools.h"


/**
 * @brief Načte ID sektorů z track headerů pro všechny absolutní stopy.
 *
 * Pro každou stopu přečte st_DSK_SHORT_TRACK_INFO a zkopíruje
 * pole sinfo[] (sector IDs) do data->tracks[].sector_ids[].
 *
 * @param data Datový model s naplněnými tracks (sectors, sector_size, is_inverted).
 * @param disc Otevřený diskový obraz.
 *
 * @pre data->total_tracks a data->tracks[].sectors jsou naplněné.
 */
static void load_sector_ids ( st_PANEL_GEOMETRY_DATA *data, st_MZDSK_DISC *disc )
{
    st_DSK_SHORT_IMAGE_INFO sii;
    if ( dsk_read_short_image_info ( disc->handler, &sii ) != EXIT_SUCCESS ) return;

    for ( int i = 0; i < (int) data->total_tracks; i++ ) {
        st_DSK_SHORT_TRACK_INFO sti;
        if ( dsk_read_short_track_info ( disc->handler, &sii, (uint8_t) i, &sti ) != EXIT_SUCCESS ) {
            continue;
        }
        int nsec = data->tracks[i].sectors;
        if ( nsec > PANEL_GEOMETRY_MAX_SECTORS ) nsec = PANEL_GEOMETRY_MAX_SECTORS;
        for ( int s = 0; s < nsec; s++ ) {
            data->tracks[i].sector_ids[s] = sti.sinfo[s];
        }
    }
}


void panel_geometry_load ( st_PANEL_GEOMETRY_DATA *data, st_MZDSK_DISC *disc )
{
    memset ( data, 0, sizeof ( *data ) );

    st_DSK_TOOLS_TRACKS_RULES_INFO *tr = disc->tracks_rules;
    if ( !tr ) return;

    data->total_tracks = tr->total_tracks;
    data->sides = tr->sides;

    uint16_t track_idx = 0;

    for ( int i = 0; i < (int) tr->count_rules; i++ ) {
        st_DSK_TOOLS_TRACK_RULE_INFO *r = &tr->rule[i];
        uint16_t sector_size = dsk_decode_sector_size ( r->ssize );
        bool inverted = ( r->sectors == 16 && r->ssize == DSK_SECTOR_SIZE_256 );

        for ( int j = 0; j < (int) r->count_tracks; j++ ) {
            if ( track_idx >= PANEL_GEOMETRY_MAX_TRACKS ) break;

            data->tracks[track_idx].sectors = r->sectors;
            data->tracks[track_idx].sector_size = sector_size;
            data->tracks[track_idx].is_inverted = inverted;
            track_idx++;
        }

        if ( r->sectors > data->max_sectors ) {
            data->max_sectors = r->sectors;
        }
    }

    /* načíst ID sektorů z track headerů */
    load_sector_ids ( data, disc );

    data->is_loaded = true;
}


/* =====================================================================
 *  Editace geometrie
 * ===================================================================== */


/**
 * @brief Mapování indexu combo boxu pořadí sektorů na en_DSK_SECTOR_ORDER_TYPE.
 *
 * Index 0 = Normal, 1 = LEC, 2 = LEC HD, 3 = Custom.
 */
static const en_DSK_SECTOR_ORDER_TYPE s_order_map[] = {
    DSK_SEC_ORDER_NORMAL,
    DSK_SEC_ORDER_INTERLACED_LEC,
    DSK_SEC_ORDER_INTERLACED_LEC_HD,
    DSK_SEC_ORDER_CUSTOM,
};


/**
 * @brief Parsuje řetězec custom sektorové mapy "1,2,3,..." do pole uint8_t.
 *
 * @param str Vstupní řetězec (např. "1,3,5,2,4,6").
 * @param out Výstupní pole ID sektorů.
 * @param max_count Maximální počet prvků ve výstupním poli.
 * @return Počet naparsovaných sektorů, nebo -1 při chybě formátu.
 */
static int parse_custom_sector_map ( const char *str, uint8_t *out, int max_count )
{
    if ( !str || !out || max_count <= 0 ) return -1;
    if ( str[0] == '\0' ) return -1;

    int count = 0;
    const char *p = str;

    while ( *p != '\0' && count < max_count ) {
        /* přeskočit mezery */
        while ( *p == ' ' ) p++;
        if ( *p == '\0' ) break;

        /* parsovat číslo */
        char *end = NULL;
        long val = strtol ( p, &end, 10 );
        if ( end == p ) return -1; /* žádné číslo */
        if ( val < 0 || val > 255 ) return -1;

        out[count++] = (uint8_t) val;
        p = end;

        /* přeskočit čárku a mezery */
        while ( *p == ' ' ) p++;
        if ( *p == ',' ) p++;
    }

    return count;
}


void panel_geom_edit_init ( st_PANEL_GEOM_EDIT_DATA *data,
                             const st_MZDSK_DISC *disc )
{
    if ( !data ) return;
    memset ( data, 0, sizeof ( *data ) );

    /* výchozí hodnoty pro Change Track */
    data->ct_sectors = 9;
    data->ct_ssize_idx = 2;   /* 512 B */
    data->ct_order_idx = 0;   /* Normal */

    /* výchozí hodnoty pro Append Tracks */
    data->at_count = 1;
    data->at_sectors = 9;
    data->at_ssize_idx = 2;   /* 512 B */
    data->at_order_idx = 0;   /* Normal */

    /* výchozí hodnota pro Shrink */
    if ( disc && disc->tracks_rules ) {
        int total = (int) disc->tracks_rules->total_tracks;
        data->sh_new_total = ( total > 1 ) ? total - 1 : 1;
    } else {
        data->sh_new_total = 1;
    }
}


int panel_geom_edit_change_track ( st_PANEL_GEOM_EDIT_DATA *data,
                                    st_MZDSK_DISC *disc )
{
    if ( !data || !disc || !disc->tracks_rules ) return EXIT_FAILURE;

    data->show_result = false;

    /* validace */
    int total = (int) disc->tracks_rules->total_tracks;
    if ( data->ct_track < 0 || data->ct_track >= total ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Track %d out of range (0-%d)", data->ct_track, total - 1 );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    if ( data->ct_sectors < 1 || data->ct_sectors > DSK_MAX_SECTORS ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Sector count %d out of range (1-%d)", data->ct_sectors, DSK_MAX_SECTORS );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    if ( data->ct_filler < 0 || data->ct_filler > 255 ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Filler byte %d out of range (0-255)", data->ct_filler );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    en_DSK_SECTOR_SIZE ssize = (en_DSK_SECTOR_SIZE) data->ct_ssize_idx;
    en_DSK_SECTOR_ORDER_TYPE order = s_order_map[data->ct_order_idx];

    /* sestavit sektorovou mapu */
    uint8_t sector_map[DSK_MAX_SECTORS];

    if ( order == DSK_SEC_ORDER_CUSTOM ) {
        int parsed = parse_custom_sector_map ( data->ct_custom_map, sector_map,
                                                DSK_MAX_SECTORS );
        if ( parsed < 0 || parsed != data->ct_sectors ) {
            snprintf ( data->result_msg, sizeof ( data->result_msg ),
                       "Custom sector map: expected %d values, got %d",
                       data->ct_sectors, parsed );
            data->is_error = true;
            data->show_result = true;
            return EXIT_FAILURE;
        }
    } else {
        dsk_tools_make_sector_map ( (uint8_t) data->ct_sectors, order, sector_map );
    }

    /* provést operaci */
    int res = dsk_tools_change_track ( disc->handler, NULL,
                                         (uint8_t) data->ct_track,
                                         (uint8_t) data->ct_sectors,
                                         ssize, sector_map,
                                         (uint8_t) data->ct_filler );
    if ( res != EXIT_SUCCESS ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Change track %d failed", data->ct_track );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    snprintf ( data->result_msg, sizeof ( data->result_msg ),
               "Track %d changed: %d sectors x %d B",
               data->ct_track, data->ct_sectors,
               (int) dsk_decode_sector_size ( ssize ) );
    data->is_error = false;
    data->show_result = true;
    return EXIT_SUCCESS;
}


int panel_geom_edit_append_tracks ( st_PANEL_GEOM_EDIT_DATA *data,
                                     st_MZDSK_DISC *disc )
{
    if ( !data || !disc || !disc->tracks_rules ) return EXIT_FAILURE;

    data->show_result = false;

    int total = (int) disc->tracks_rules->total_tracks;
    int sides = (int) disc->tracks_rules->sides;

    /* validace */
    if ( data->at_count < 1 ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Track count must be at least 1" );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    if ( sides == 2 && ( data->at_count % 2 ) != 0 ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "For 2-sided disks, track count must be even (got %d)",
                   data->at_count );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    if ( total + data->at_count > DSK_MAX_TOTAL_TRACKS ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Total tracks would be %d, maximum is %d",
                   total + data->at_count, DSK_MAX_TOTAL_TRACKS );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    if ( data->at_sectors < 1 || data->at_sectors > DSK_MAX_SECTORS ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Sector count %d out of range (1-%d)", data->at_sectors, DSK_MAX_SECTORS );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    en_DSK_SECTOR_SIZE ssize = (en_DSK_SECTOR_SIZE) data->at_ssize_idx;
    en_DSK_SECTOR_ORDER_TYPE order = s_order_map[data->at_order_idx];

    /* custom mapa pro append */
    uint8_t custom_map[DSK_MAX_SECTORS];
    uint8_t *map_ptr = NULL;

    if ( order == DSK_SEC_ORDER_CUSTOM ) {
        int parsed = parse_custom_sector_map ( data->at_custom_map, custom_map,
                                                DSK_MAX_SECTORS );
        if ( parsed < 0 || parsed != data->at_sectors ) {
            snprintf ( data->result_msg, sizeof ( data->result_msg ),
                       "Custom sector map: expected %d values, got %d",
                       data->at_sectors, parsed );
            data->is_error = true;
            data->show_result = true;
            return EXIT_FAILURE;
        }
        map_ptr = custom_map;
    }

    /* sestavit popis - 1 pravidlo */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Memory allocation failed" );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = (uint8_t) ( ( total + data->at_count ) / sides );
    desc->sides = (uint8_t) sides;

    dsk_tools_assign_description ( desc, 0, (uint8_t) total,
                                    (uint8_t) data->at_sectors, ssize,
                                    order, map_ptr,
                                    (uint8_t) data->at_filler );

    /* provést operaci */
    int res = dsk_tools_add_tracks ( disc->handler, desc );
    free ( desc );

    if ( res != EXIT_SUCCESS ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Append %d tracks failed", data->at_count );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    snprintf ( data->result_msg, sizeof ( data->result_msg ),
               "Appended %d tracks (%d sectors x %d B each)",
               data->at_count, data->at_sectors,
               (int) dsk_decode_sector_size ( ssize ) );
    data->is_error = false;
    data->show_result = true;
    return EXIT_SUCCESS;
}


int panel_geom_edit_shrink ( st_PANEL_GEOM_EDIT_DATA *data,
                              st_MZDSK_DISC *disc )
{
    if ( !data || !disc || !disc->tracks_rules ) return EXIT_FAILURE;

    data->show_result = false;

    int total = (int) disc->tracks_rules->total_tracks;
    int sides = (int) disc->tracks_rules->sides;

    /* validace */
    if ( data->sh_new_total <= 0 ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "New track count must be at least 1" );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    if ( data->sh_new_total >= total ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "New track count (%d) must be less than current (%d)",
                   data->sh_new_total, total );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    if ( sides == 2 && ( data->sh_new_total % 2 ) != 0 ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "For 2-sided disks, track count must be even (got %d)",
                   data->sh_new_total );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    /* provést operaci */
    int res = dsk_tools_shrink_image ( disc->handler, NULL,
                                         (uint8_t) data->sh_new_total );
    if ( res != EXIT_SUCCESS ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Shrink to %d tracks failed", data->sh_new_total );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    snprintf ( data->result_msg, sizeof ( data->result_msg ),
               "Disk shrunk from %d to %d total tracks",
               total, data->sh_new_total );
    data->is_error = false;
    data->show_result = true;
    return EXIT_SUCCESS;
}


/* =====================================================================
 *  Edit Sector IDs
 * ===================================================================== */


void panel_geom_edit_load_sector_ids ( st_PANEL_GEOM_EDIT_DATA *data,
                                        st_MZDSK_DISC *disc )
{
    if ( !data || !disc ) return;

    data->si_loaded = false;
    data->si_count = 0;
    memset ( data->si_ids, 0, sizeof ( data->si_ids ) );

    if ( !disc->tracks_rules ) return;

    int total = (int) disc->tracks_rules->total_tracks;
    if ( data->si_track < 0 || data->si_track >= total ) return;

    st_DSK_TRACK_HEADER_INFO info;
    if ( dsk_tools_read_track_header_info ( disc->handler,
                                             (uint8_t) data->si_track,
                                             &info ) != EXIT_SUCCESS ) {
        return;
    }

    int nsec = (int) info.sectors;
    if ( nsec > PANEL_GEOMETRY_MAX_SECTORS ) nsec = PANEL_GEOMETRY_MAX_SECTORS;

    for ( int i = 0; i < nsec; i++ ) {
        data->si_ids[i] = info.sinfo[i].sector;
    }

    data->si_count = nsec;
    data->si_loaded = true;
}


int panel_geom_edit_apply_sector_ids ( st_PANEL_GEOM_EDIT_DATA *data,
                                        st_MZDSK_DISC *disc )
{
    if ( !data || !disc || !disc->tracks_rules ) return EXIT_FAILURE;

    data->show_result = false;

    if ( !data->si_loaded || data->si_count <= 0 ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "No sector IDs loaded" );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    int total = (int) disc->tracks_rules->total_tracks;
    if ( data->si_track < 0 || data->si_track >= total ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Track %d out of range (0-%d)", data->si_track, total - 1 );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    int res = dsk_tools_set_sector_ids ( disc->handler,
                                          (uint8_t) data->si_track,
                                          data->si_ids,
                                          (uint8_t) data->si_count );
    if ( res != EXIT_SUCCESS ) {
        snprintf ( data->result_msg, sizeof ( data->result_msg ),
                   "Failed to set sector IDs for track %d", data->si_track );
        data->is_error = true;
        data->show_result = true;
        return EXIT_FAILURE;
    }

    snprintf ( data->result_msg, sizeof ( data->result_msg ),
               "Sector IDs for track %d changed successfully", data->si_track );
    data->is_error = false;
    data->show_result = true;
    return EXIT_SUCCESS;
}
