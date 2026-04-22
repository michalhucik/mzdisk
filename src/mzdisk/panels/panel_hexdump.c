/**
 * @file panel_hexdump.c
 * @brief Logika hexdump panelu - čtení sektorů/bloků a navigace.
 *
 * Čte raw sektorová data přímo přes dsk_read_sector() (bez auto-inverze).
 * Inverze a kódování znaků jsou čistě zobrazovací volby v rendereru.
 *
 * Podporuje dva režimy adresování:
 * - Track/Sector: čte jeden sektor na dané stopě dle sector ID.
 * - Block: na základě blokové konfigurace (origin, first_block,
 *   sectors_per_block, sector_order) přepočítá číslo bloku na
 *   track/sector a načte odpovídající počet po sobě jdoucích sektorů.
 *
 * Pořadí procházení sektorů v blokovém režimu (sector_order):
 * - ID: sekvenčně podle sector ID, přeskakuje nesekvenční ID.
 *   Čte sector ID mapu stopy přes dsk_read_short_track_info().
 * - Phys: podle fyzické pozice v sinfo[] poli DSK stopy,
 *   zachycuje všechny sektory včetně nesekvenčních.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <string.h>
#include "panel_hexdump.h"
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"


/**
 * @brief Zjistí počet sektorů a velikost sektoru na dané stopě.
 *
 * @param disc Otevřený disk.
 * @param track Číslo stopy.
 * @param[out] sectors Počet sektorů.
 * @param[out] sector_size Velikost sektoru v bajtech.
 *
 * @pre disc != NULL, sectors != NULL, sector_size != NULL.
 * @post *sectors a *sector_size obsahují parametry stopy,
 *       nebo 0/256 pokud stopa neexistuje.
 */
void panel_hexdump_get_track_params ( st_MZDSK_DISC *disc, uint16_t track,
                                       uint16_t *sectors, uint16_t *sector_size )
{
    *sectors = 0;
    *sector_size = 256;

    st_DSK_TOOLS_TRACKS_RULES_INFO *tr = disc->tracks_rules;
    if ( !tr ) return;

    for ( int i = 0; i < (int) tr->count_rules; i++ ) {
        st_DSK_TOOLS_TRACK_RULE_INFO *r = &tr->rule[i];
        if ( track >= r->from_track && track < r->from_track + r->count_tracks ) {
            *sectors = r->sectors;
            *sector_size = (uint16_t) ( 128 << (uint8_t) r->ssize );
            return;
        }
    }
}


/**
 * @brief Načte informace o stopě (sector ID mapa, velikosti).
 *
 * Wrapper nad dsk_read_short_track_info() pro pohodlnější použití
 * v rámci hexdump panelu.
 *
 * @param disc Otevřený disk.
 * @param track Číslo stopy.
 * @param[out] tinfo Výstupní informace o stopě.
 * @return true pokud stopa existuje a byla načtena, false při chybě.
 *
 * @pre disc != NULL, tinfo != NULL.
 */
static bool read_track_info ( st_MZDSK_DISC *disc, uint16_t track,
                              st_DSK_SHORT_TRACK_INFO *tinfo )
{
    return dsk_read_short_track_info ( disc->handler, NULL, (uint8_t) track, tinfo ) == EXIT_SUCCESS;
}


/**
 * @brief Najde fyzickou pozici sektoru s daným ID na stopě.
 *
 * Prohledá pole sinfo[] a vrátí index (0-based) sektoru
 * s hledaným ID.
 *
 * @param tinfo Informace o stopě.
 * @param sector_id Hledané ID sektoru.
 * @return Fyzická pozice (0-based), nebo -1 pokud ID neexistuje.
 *
 * @pre tinfo != NULL.
 */
static int find_sector_position ( const st_DSK_SHORT_TRACK_INFO *tinfo, uint8_t sector_id )
{
    for ( int i = 0; i < tinfo->sectors; i++ ) {
        if ( tinfo->sinfo[i] == sector_id ) return i;
    }
    return -1;
}


/**
 * @brief Spočítá kolik po sobě jdoucích ID sektorů existuje na stopě.
 *
 * Od sektoru s ID start_id+1 hledá souvislou řadu existujících ID.
 * Např. pro stopu s ID [1,2,3,4,5,6,7,8,9,22] a start_id=1
 * vrátí 8 (ID 2..9 existují, ID 10 ne).
 *
 * @param tinfo Informace o stopě.
 * @param start_id ID sektoru, od kterého se počítá (nezapočítává se).
 * @return Počet po sobě jdoucích následujících ID.
 *
 * @pre tinfo != NULL.
 */
static int count_consecutive_ids ( const st_DSK_SHORT_TRACK_INFO *tinfo, uint8_t start_id )
{
    int count = 0;
    for ( uint8_t next = start_id + 1; next > start_id; next++ ) { /* overflow ochrana */
        if ( find_sector_position ( tinfo, next ) < 0 ) break;
        count++;
    }
    return count;
}


/**
 * @brief Posune pozici (track, sector_id) o zadaný počet sektorů vpřed.
 *
 * Postup závisí na zvoleném pořadí sektorů:
 *
 * - ID režim: hledá sektor s ID+1 na aktuální stopě. Pokud neexistuje,
 *   přejde na další stopu a začne od ID 1. Sektory s nesekvenčním ID
 *   (např. LEMMINGS sektor 22) jsou přeskočeny, pokud na ně nenavazuje
 *   počáteční pozice.
 *
 * - Phys režim: postupuje podle fyzické pozice v sinfo[] poli stopy.
 *   Na konci stopy přejde na další stopu a začne od fyzické pozice 0
 *   (tj. sinfo[0]). Zachycuje všechny sektory včetně nesekvenčních.
 *
 * @param disc Otevřený disk.
 * @param order Pořadí procházení sektorů.
 * @param[in,out] track Aktuální stopa, aktualizuje se.
 * @param[in,out] sector_id Aktuální sector ID, aktualizuje se.
 * @param count Počet sektorů k přeskočení.
 * @param max_track Maximální číslo stopy.
 * @return true pokud je výsledná pozice platná, false pokud přetekla konec disku.
 *
 * @pre disc != NULL, track != NULL, sector_id != NULL, count >= 0.
 * @post Při úspěchu: *track a *sector_id ukazují na platný sektor.
 */
bool panel_hexdump_advance_sectors ( st_MZDSK_DISC *disc, en_HEXDUMP_SECTOR_ORDER order,
                                      uint16_t *track, uint16_t *sector_id,
                                      int count, uint16_t max_track )
{
    uint16_t t = *track;
    uint16_t s = *sector_id;

    while ( count > 0 ) {
        st_DSK_SHORT_TRACK_INFO tinfo;
        if ( !read_track_info ( disc, t, &tinfo ) ) return false;
        if ( tinfo.sectors == 0 ) return false;

        if ( order == HEXDUMP_SECTOR_ORDER_PHYS ) {
            /* fyzické pořadí: postup podle pozice v sinfo[] */
            int pos = find_sector_position ( &tinfo, (uint8_t) s );
            if ( pos < 0 ) return false;

            int remaining = tinfo.sectors - pos - 1;

            if ( count <= remaining ) {
                s = tinfo.sinfo[pos + count];
                count = 0;
            } else {
                count -= ( remaining + 1 );
                t++;
                if ( t > max_track ) return false;

                /* první fyzický sektor na další stopě */
                st_DSK_SHORT_TRACK_INFO next;
                if ( !read_track_info ( disc, t, &next ) ) return false;
                if ( next.sectors == 0 ) return false;
                s = next.sinfo[0];
            }

        } else {
            /* ID pořadí: postup podle sekvenčních ID */
            int remaining = count_consecutive_ids ( &tinfo, (uint8_t) s );

            if ( count <= remaining ) {
                s = (uint16_t) ( s + count );
                count = 0;
            } else {
                count -= ( remaining + 1 );
                t++;
                if ( t > max_track ) return false;
                s = 1;
            }
        }
    }

    *track = t;
    *sector_id = s;
    return true;
}


void panel_hexdump_init ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc )
{
    memset ( hd, 0, sizeof ( *hd ) );

    if ( disc->tracks_rules ) {
        hd->max_track = disc->tracks_rules->total_tracks - 1;
    }

    hd->track = 0;
    hd->sector = 1;
    hd->invert = false;
    hd->charset = HEXDUMP_CHARSET_RAW;

    /* výchozí blokový režim: celý disk, 1 sektor/blok, ID pořadí */
    hd->addr_mode = HEXDUMP_ADDR_TRACK_SECTOR;
    hd->block = 0;
    hd->block_config.origin_track = 0;
    hd->block_config.origin_sector = 1;
    hd->block_config.first_block = 0;
    hd->block_config.sectors_per_block = 1;
    hd->block_config.sector_order = HEXDUMP_SECTOR_ORDER_ID;

    panel_hexdump_get_track_params ( disc, hd->track, &hd->max_sector, &hd->sector_size );

    panel_hexdump_read_sector ( hd, disc );
}


bool panel_hexdump_block_to_track_sector ( const st_PANEL_HEXDUMP_DATA *hd,
                                            st_MZDSK_DISC *disc,
                                            uint16_t *out_track,
                                            uint16_t *out_sector )
{
    const st_HEXDUMP_BLOCK_CONFIG *cfg = &hd->block_config;

    /* kolik sektorů od originu ke začátku bloku */
    int32_t sector_offset = ( hd->block - cfg->first_block ) * (int32_t) cfg->sectors_per_block;

    if ( sector_offset < 0 ) return false;

    uint16_t t = cfg->origin_track;
    uint16_t s = cfg->origin_sector;

    if ( sector_offset > 0 ) {
        if ( !panel_hexdump_advance_sectors ( disc, cfg->sector_order, &t, &s,
                                (int) sector_offset, hd->max_track ) ) {
            return false;
        }
    }

    /* ověřit, že stopa je platná */
    if ( t > hd->max_track ) return false;

    *out_track = t;
    *out_sector = s;
    return true;
}


void panel_hexdump_read_sector ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc )
{
    memset ( hd->data, 0, sizeof ( hd->data ) );
    hd->read_error = false;
    hd->data_size = 0;

    if ( hd->addr_mode == HEXDUMP_ADDR_BLOCK ) {
        /* blokový režim: přepočítat blok na track/sector */
        uint16_t t, s;
        if ( !panel_hexdump_block_to_track_sector ( hd, disc, &t, &s ) ) {
            hd->read_error = true;
            hd->is_loaded = false;
            return;
        }

        hd->track = t;
        hd->sector = s;

        /* načíst sectors_per_block po sobě jdoucích sektorů */
        uint16_t offset = 0;
        uint16_t ct = t;
        uint16_t cs = s;

        for ( uint16_t i = 0; i < hd->block_config.sectors_per_block; i++ ) {
            uint16_t trk_sectors, trk_ssize;
            panel_hexdump_get_track_params ( disc, ct, &trk_sectors, &trk_ssize );

            if ( trk_sectors == 0 ) {
                hd->read_error = true;
                hd->is_loaded = false;
                return;
            }

            /* Audit M-39: dříve jen `break` bez indikace chyby -
             * uživatel viděl uříznutý dump jako kdyby byl celý blok.
             * Nyní se nastaví `read_error` a přeruší načítání, UI
             * zobrazí hlášku místo nekompletních dat. */
            if ( offset + trk_ssize > PANEL_HEXDUMP_MAX_DATA ) {
                hd->read_error = true;
                hd->is_loaded = false;
                return;
            }

            /* Audit L-27: runtime kontrola ct/cs > 255 před oříznutím
               do uint8_t pro dsk_read_sector(). DSK_MAX_TOTAL_TRACKS je
               aktuálně 255 (H-11/M-31 refactor pendoval), ale při budoucím
               rozšíření by tiché oříznutí vedlo k čtení jiné stopy. */
            if ( ct > 255 || cs > 255 ) {
                hd->read_error = true;
                hd->is_loaded = false;
                return;
            }

            int res = dsk_read_sector ( disc->handler, (uint8_t) ct, (uint8_t) cs,
                                         hd->data + offset );
            if ( res != EXIT_SUCCESS ) {
                hd->read_error = true;
                hd->is_loaded = false;
                return;
            }

            offset += trk_ssize;

            /* posun na další sektor dle zvoleného pořadí */
            if ( i + 1 < hd->block_config.sectors_per_block ) {
                if ( !panel_hexdump_advance_sectors ( disc, hd->block_config.sector_order,
                                        &ct, &cs, 1, hd->max_track ) ) {
                    hd->read_error = true;
                    hd->is_loaded = false;
                    return;
                }
            }
        }

        hd->data_size = offset;
        hd->sector_size = offset; /* pro render_hex_content */
        hd->is_loaded = true;

        /* aktualizovat max_sector pro info zobrazení */
        panel_hexdump_get_track_params ( disc, hd->track, &hd->max_sector, &hd->sector_size );
        hd->data_size = offset;

    } else {
        /* režim track/sector: čtení jednoho sektoru (beze změny) */
        panel_hexdump_get_track_params ( disc, hd->track, &hd->max_sector, &hd->sector_size );

        if ( hd->sector < 1 || hd->sector > hd->max_sector ) {
            hd->read_error = true;
            hd->is_loaded = false;
            return;
        }

        int res = dsk_read_sector ( disc->handler, (uint8_t) hd->track,
                                     (uint8_t) hd->sector, hd->data );
        if ( res != EXIT_SUCCESS ) {
            hd->read_error = true;
            hd->is_loaded = false;
        } else {
            hd->is_loaded = true;
            hd->data_size = hd->sector_size;
        }
    }
}


void panel_hexdump_enter_edit ( st_PANEL_HEXDUMP_DATA *hd )
{
    if ( !hd || !hd->is_loaded ) return;

    memcpy ( hd->undo_data, hd->data, hd->data_size );
    hd->edit_mode = true;
    hd->edit_dirty = false;
    hd->cursor_pos = 0;
    hd->cursor_in_ascii = false;
    hd->cursor_high_nibble = true;
    hd->pending_nav_discard = false;
    hd->edit_convert_error = false;
    hd->edit_convert_error_msg[0] = '\0';
}


void panel_hexdump_revert_edit ( st_PANEL_HEXDUMP_DATA *hd )
{
    if ( !hd ) return;

    if ( hd->edit_mode ) {
        memcpy ( hd->data, hd->undo_data, hd->data_size );
    }

    hd->edit_mode = false;
    hd->edit_dirty = false;
    hd->cursor_pos = 0;
    hd->cursor_in_ascii = false;
    hd->cursor_high_nibble = true;
    hd->pending_nav_discard = false;
    hd->edit_convert_error = false;
    hd->edit_convert_error_msg[0] = '\0';
}


en_MZDSK_RES panel_hexdump_write_data ( st_PANEL_HEXDUMP_DATA *hd,
                                          st_MZDSK_DISC *disc )
{
    if ( !hd || !disc || !hd->edit_mode || !hd->is_loaded ) {
        return MZDSK_RES_DSK_ERROR;
    }

    if ( hd->addr_mode == HEXDUMP_ADDR_BLOCK ) {
        /* blokový režim: zapsat všechny sektory bloku */
        uint16_t t, s;
        if ( !panel_hexdump_block_to_track_sector ( hd, disc, &t, &s ) ) {
            return MZDSK_RES_DSK_ERROR;
        }

        uint16_t offset = 0;

        for ( uint16_t i = 0; i < hd->block_config.sectors_per_block; i++ ) {
            uint16_t trk_sectors, trk_ssize;
            panel_hexdump_get_track_params ( disc, t, &trk_sectors, &trk_ssize );

            if ( trk_sectors == 0 ) return MZDSK_RES_DSK_ERROR;
            if ( offset + trk_ssize > hd->data_size ) break;

            int res = dsk_write_sector ( disc->handler, (uint8_t) t, (uint8_t) s,
                                           hd->data + offset );
            if ( res != EXIT_SUCCESS ) return MZDSK_RES_DSK_ERROR;

            offset += trk_ssize;

            /* posun na další sektor dle zvoleného pořadí */
            if ( i + 1 < hd->block_config.sectors_per_block ) {
                if ( !panel_hexdump_advance_sectors ( disc, hd->block_config.sector_order,
                                        &t, &s, 1, hd->max_track ) ) {
                    return MZDSK_RES_DSK_ERROR;
                }
            }
        }
    } else {
        /* režim track/sector: zapsat jeden sektor */
        int res = dsk_write_sector ( disc->handler, (uint8_t) hd->track,
                                       (uint8_t) hd->sector, hd->data );
        if ( res != EXIT_SUCCESS ) return MZDSK_RES_DSK_ERROR;
    }

    /* po úspěšném zápisu: aktualizovat undo buffer, smazat dirty */
    memcpy ( hd->undo_data, hd->data, hd->data_size );
    hd->edit_dirty = false;

    return MZDSK_RES_OK;
}


void panel_hexdump_preset_whole_disk ( st_PANEL_HEXDUMP_DATA *hd )
{
    hd->block_config.origin_track = 0;
    hd->block_config.origin_sector = 1;
    hd->block_config.first_block = 0;
    hd->block_config.sectors_per_block = 1;
    hd->block_config.sector_order = HEXDUMP_SECTOR_ORDER_ID;
}


void panel_hexdump_preset_cpm ( st_PANEL_HEXDUMP_DATA *hd,
                                 const st_MZDSK_CPM_DPB *dpb )
{
    hd->block_config.origin_track = dpb->off;
    hd->block_config.origin_sector = 1;
    hd->block_config.first_block = 0;
    /* CP/M fyzický sektor je 512B, block_size / 512 = sectors_per_block */
    hd->block_config.sectors_per_block = dpb->block_size / 512;
    if ( hd->block_config.sectors_per_block < 1 ) {
        hd->block_config.sectors_per_block = 1;
    }
    hd->block_config.sector_order = HEXDUMP_SECTOR_ORDER_ID;
}
