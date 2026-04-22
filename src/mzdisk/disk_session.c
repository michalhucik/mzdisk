/**
 * @file disk_session.c
 * @brief Implementace správy diskových sessions pro mzdisk GUI.
 *
 * Řeší otevírání DSK souborů do paměti (memory driver), auto-detekci
 * filesystému, ukládání změn a zavírání.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <string.h>
#include <stdio.h>
#include "disk_session.h"
#include "app.h"
#include "libs/dsk/dsk_tools.h"


/**
 * @brief Extrahuje název souboru z cesty pro zobrazení v tabu.
 *
 * Hledá poslední oddělovač cesty ('/' nebo '\\') a vrací řetězec za ním.
 * Pokud oddělovač nenajde, vrátí celou cestu.
 *
 * @param filepath Plná cesta k souboru.
 * @return Ukazatel do filepath na začátek názvu souboru.
 */
static const char* extract_filename ( const char *filepath )
{
    const char *name = filepath;
    const char *p = filepath;

    while ( *p ) {
        if ( *p == '/' || *p == '\\' ) {
            name = p + 1;
        }
        p++;
    }

    return name;
}


void mzdisk_session_manager_init ( st_MZDISK_SESSION_MANAGER *mgr )
{
    if ( mgr == NULL ) return;  /* audit L-25: defenzivní NULL kontrola */

    memset ( mgr, 0, sizeof ( *mgr ) );
    mgr->active_id = 0;
    mgr->next_id = 1;  /* 0 je rezervované jako "neplatné id" */
}


/**
 * @brief Najde session podle id - interní helper, vrací slot index.
 *
 * @param mgr Session manager.
 * @param id Hledané id (0 = nic, vrací -1).
 * @return Index slotu v sessions[], nebo -1 pokud id neexistuje nebo je 0.
 */
static int find_slot_by_id ( const st_MZDISK_SESSION_MANAGER *mgr, uint64_t id )
{
    if ( id == 0 ) return -1;
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        if ( mgr->sessions[i].is_open && mgr->sessions[i].id == id ) {
            return i;
        }
    }
    return -1;
}


/**
 * @brief Najde první volný slot v poli sessions.
 *
 * @param mgr Session manager.
 * @return Index volného slotu, nebo -1 pokud jsou všechny obsazené.
 */
static int find_free_slot ( st_MZDISK_SESSION_MANAGER *mgr )
{
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        if ( !mgr->sessions[i].is_open ) {
            return i;
        }
    }
    return -1;
}


st_MZDISK_SESSION* mzdisk_session_create_empty ( st_MZDISK_SESSION_MANAGER *mgr,
                                                  bool allow_primary )
{
    if ( !mgr ) return NULL;

    int slot = find_free_slot ( mgr );
    if ( slot < 0 ) return NULL;

    /* zjistit, zda je už nějaká primární session */
    bool has_primary = false;
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        if ( mgr->sessions[i].is_open && mgr->sessions[i].is_primary ) {
            has_primary = true;
            break;
        }
    }

    st_MZDISK_SESSION *session = &mgr->sessions[slot];
    memset ( session, 0, sizeof ( *session ) );

    /* přidělit monotónní id */
    if ( mgr->next_id == 0 ) mgr->next_id = 1;  /* obrana proti wrap na 0 */
    session->id = mgr->next_id++;

    session->is_open = true;
    session->has_disk = false;
    session->is_dirty = false;
    /* primární jen pokud volající to povolí A žádná primární ještě není. */
    session->is_primary = ( allow_primary && !has_primary );

    /* Uživatelské číslo okna: primární = 0, detached = nejnižší volné
       v rozsahu 1..MZDISK_MAX_SESSIONS-1 (vzhledem k aktuálně otevřeným).
       Pole is_open už má session nastavený (je_open=true), ale number
       ještě není přiřazen, takže při vyhledávání skip tuhle session. */
    if ( session->is_primary ) {
        session->window_number = 0;
    } else {
        bool used[MZDISK_MAX_SESSIONS] = { false };
        for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
            st_MZDISK_SESSION *o = &mgr->sessions[i];
            if ( o == session ) continue;
            if ( !o->is_open ) continue;
            if ( o->window_number >= 0 && o->window_number < MZDISK_MAX_SESSIONS ) {
                used[o->window_number] = true;
            }
        }
        /* 0 je rezervované pro primární i kdyby teď žádná nebyla. */
        int wn = 1;
        while ( wn < MZDISK_MAX_SESSIONS && used[wn] ) wn++;
        if ( wn >= MZDISK_MAX_SESSIONS ) wn = MZDISK_MAX_SESSIONS - 1;  /* fallback */
        session->window_number = wn;
    }
    session->window_open = true;
    session->pending_close = false;
    session->pending_reload = false;
    session->pending_open_path[0] = '\0';
    session->last_save_error[0] = '\0';

    /* EMPTY stav - bez souboru, zobrazí se welcome screen */
    session->filepath[0] = '\0';
    snprintf ( session->display_name, sizeof ( session->display_name ),
               "%s", "New Window" );

    /* CP/M filter user: -1 = all (memset 0 by jinak znamenalo "jen user 0"). */
    session->cpm_data.filter_user = -1;

    mgr->active_id = session->id;
    mgr->count++;

    return session;
}


en_MZDSK_RES mzdisk_session_load ( st_MZDISK_SESSION *session, const char *filepath )
{
    if ( !session || !session->is_open || !filepath ) return MZDSK_RES_DSK_ERROR;

    /* pokud session už má disk, nejprve ho uklidit */
    if ( session->has_disk ) {
        mzdsk_disc_close ( &session->disc );
        session->has_disk = false;
        session->is_dirty = false;
    }

    /* otevřít disk do paměti (memory driver) */
    en_MZDSK_RES res = mzdsk_disc_open_memory (
        &session->disc,
        (char *) filepath,
        FILE_DRIVER_OPMODE_RW
    );
    if ( res != MZDSK_RES_OK ) {
        return res;
    }

    /* auto-detekce filesystému */
    mzdsk_detect_filesystem ( &session->disc, &session->detect_result );

    /* naplnit panelová data */
    panel_info_load ( &session->info_data, &session->disc, &session->detect_result );
    panel_map_load ( &session->map_data, &session->disc, &session->detect_result );
    panel_hexdump_init ( &session->hexdump_data, &session->disc );
    panel_geometry_load ( &session->geometry_data, &session->disc );
    panel_boot_load ( &session->boot_data, &session->disc, &session->detect_result );

    /* FS-specifická data */
    if ( session->detect_result.type == MZDSK_FS_FSMZ ) {
        panel_fsmz_load ( &session->fsmz_data, &session->disc );
    }
    if ( session->detect_result.type == MZDSK_FS_CPM ) {
        panel_cpm_load ( &session->cpm_data, &session->disc, &session->detect_result );
    }
    if ( session->detect_result.type == MZDSK_FS_MRS ) {
        panel_mrs_load ( &session->mrs_data, &session->detect_result );
    }

    /* editace geometrie */
    panel_geom_edit_init ( &session->geom_edit_data, &session->disc );

    session->has_disk = true;
    session->is_dirty = false;
    session->last_save_error[0] = '\0';

    strncpy ( session->filepath, filepath, MZDISK_MAX_PATH - 1 );
    session->filepath[MZDISK_MAX_PATH - 1] = '\0';

    /* přesměrovat disc->filename na stabilní kopii v session
       (mzdsk_disc_open_memory ukládá jen pointer na předaný string,
       který může být dočasný - např. z ImGuiFileDialog) */
    session->disc.filename = session->filepath;

    const char *name = extract_filename ( filepath );
    snprintf ( session->display_name, sizeof ( session->display_name ),
               "%s (%s)", name, mzdisk_session_fs_type_str_detail ( &session->detect_result ) );

    return MZDSK_RES_OK;
}


en_MZDSK_RES mzdisk_session_open ( st_MZDISK_SESSION_MANAGER *mgr, const char *filepath )
{
    /* Open Disk smí naplnit primární slot, pokud zatím žádný není
     * (kompat s původní single-window sémantikou mzdisk_session_open). */
    st_MZDISK_SESSION *session = mzdisk_session_create_empty ( mgr, true );
    if ( !session ) return MZDSK_RES_NO_SPACE;

    en_MZDSK_RES res = mzdisk_session_load ( session, filepath );
    if ( res != MZDSK_RES_OK ) {
        /* load selhal - uvolnit vytvořený slot */
        session->is_open = false;
        session->id = 0;
        mgr->count--;
        mgr->active_id = 0;
        return res;
    }
    return MZDSK_RES_OK;
}


en_MZDSK_RES mzdisk_session_save ( st_MZDISK_SESSION *session )
{
    en_MZDSK_RES res = mzdsk_disc_save ( &session->disc );
    if ( res == MZDSK_RES_OK ) {
        session->is_dirty = false;
    }
    return res;
}


void mzdisk_session_close_by_id ( st_MZDISK_SESSION_MANAGER *mgr, uint64_t id )
{
    int slot = find_slot_by_id ( mgr, id );
    if ( slot < 0 ) return;

    st_MZDISK_SESSION *session = &mgr->sessions[slot];

    if ( session->has_disk ) {
        mzdsk_disc_close ( &session->disc );
    }
    session->is_open = false;
    session->has_disk = false;
    session->is_dirty = false;
    session->is_primary = false;
    session->window_open = false;
    session->id = 0;
    mgr->count--;

    /* pokud jsme zavřeli aktivní session, najít jinou otevřenou */
    if ( mgr->active_id == id ) {
        mgr->active_id = 0;
        for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
            if ( mgr->sessions[i].is_open ) {
                mgr->active_id = mgr->sessions[i].id;
                break;
            }
        }
    }
}


void mzdisk_session_close_all ( st_MZDISK_SESSION_MANAGER *mgr )
{
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        if ( mgr->sessions[i].is_open ) {
            if ( mgr->sessions[i].has_disk ) {
                mzdsk_disc_close ( &mgr->sessions[i].disc );
            }
            mgr->sessions[i].is_open = false;
            mgr->sessions[i].has_disk = false;
            mgr->sessions[i].is_dirty = false;
            mgr->sessions[i].is_primary = false;
            mgr->sessions[i].window_open = false;
            mgr->sessions[i].id = 0;
        }
    }
    mgr->count = 0;
    mgr->active_id = 0;
}


st_MZDISK_SESSION* mzdisk_session_get_active ( st_MZDISK_SESSION_MANAGER *mgr )
{
    int slot = find_slot_by_id ( mgr, mgr->active_id );
    if ( slot < 0 ) return NULL;
    return &mgr->sessions[slot];
}


st_MZDISK_SESSION* mzdisk_session_get_by_id ( st_MZDISK_SESSION_MANAGER *mgr, uint64_t id )
{
    int slot = find_slot_by_id ( mgr, id );
    if ( slot < 0 ) return NULL;
    return &mgr->sessions[slot];
}


st_MZDISK_SESSION* mzdisk_session_get_primary ( st_MZDISK_SESSION_MANAGER *mgr )
{
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        if ( mgr->sessions[i].is_open && mgr->sessions[i].is_primary ) {
            return &mgr->sessions[i];
        }
    }
    return NULL;
}


void mzdisk_session_set_active_by_id ( st_MZDISK_SESSION_MANAGER *mgr, uint64_t id )
{
    if ( find_slot_by_id ( mgr, id ) >= 0 ) {
        mgr->active_id = id;
    }
}


void mzdisk_session_reload_panels ( st_MZDISK_SESSION *session )
{
    if ( !session || !session->is_open ) return;

    /* 1. Obnovit tracks_rules z aktuálního stavu obrazu */
    if ( session->disc.tracks_rules ) {
        dsk_tools_destroy_track_rules ( session->disc.tracks_rules );
        session->disc.tracks_rules = NULL;
    }
    session->disc.tracks_rules = dsk_tools_get_tracks_rules ( session->disc.handler );

    /* 2. Obnovit identifikaci formátu */
    if ( session->disc.tracks_rules ) {
        session->disc.format = dsk_tools_identformat_from_tracks_rules (
            session->disc.tracks_rules );
    } else {
        session->disc.format = DSK_TOOLS_IDENTFORMAT_UNKNOWN;
    }

    /* 3. Znovu detekovat filesystém */
    mzdsk_detect_filesystem ( &session->disc, &session->detect_result );

    /* 4. Reload všech panelů */
    panel_info_load ( &session->info_data, &session->disc, &session->detect_result );
    panel_map_load ( &session->map_data, &session->disc, &session->detect_result );
    panel_hexdump_init ( &session->hexdump_data, &session->disc );
    panel_geometry_load ( &session->geometry_data, &session->disc );
    panel_boot_load ( &session->boot_data, &session->disc, &session->detect_result );

    /* FS-specifická data */
    if ( session->detect_result.type == MZDSK_FS_FSMZ ) {
        panel_fsmz_load ( &session->fsmz_data, &session->disc );
    }
    if ( session->detect_result.type == MZDSK_FS_CPM ) {
        panel_cpm_load ( &session->cpm_data, &session->disc, &session->detect_result );
    }
    if ( session->detect_result.type == MZDSK_FS_MRS ) {
        panel_mrs_load ( &session->mrs_data, &session->detect_result );
    }

    /* 5. Aktualizovat editaci geometrie (výchozí hodnoty z nové geometrie) */
    /* Zachováme UI stav (result_msg), ale aktualizujeme sh_new_total */
    if ( session->disc.tracks_rules ) {
        int total = (int) session->disc.tracks_rules->total_tracks;
        if ( session->geom_edit_data.sh_new_total >= total ) {
            session->geom_edit_data.sh_new_total = ( total > 1 ) ? total - 1 : 1;
        }
    }

    /* 5a. Reset Raw I/O okna: po Shrink/Append Tracks by zde zůstal
     * zastaralý max_track, block_config a start_sector. Pokud by uživatel
     * neotevřel Raw I/O znovu, jeho Put by se provedl na neplatnou
     * pozici staré geometrie. Audit H-21. */
    panel_raw_io_init ( &session->raw_io_data );

    /* 5b. Reset sector-ID loaded flagu v geom_edit_data: po změně
     * geometrie je cache z předchozí stopy neplatná. */
    session->geom_edit_data.si_loaded = false;

    /* 6. Aktualizovat display_name (FS typ se mohl změnit) */
    const char *name = extract_filename ( session->filepath );
    snprintf ( session->display_name, sizeof ( session->display_name ),
               "%s (%s)", name,
               mzdisk_session_fs_type_str_detail ( &session->detect_result ) );
}


const char* mzdisk_session_fs_type_str ( en_MZDSK_FS_TYPE type )
{
    switch ( type ) {
        case MZDSK_FS_FSMZ:        return "FSMZ";
        case MZDSK_FS_CPM:         return "CP/M";
        case MZDSK_FS_MRS:         return "MRS";
        case MZDSK_FS_BOOT_ONLY:   return "Boot only";
        case MZDSK_FS_UNKNOWN:
        default:                    return "Unknown";
    }
}


const char* mzdisk_session_fs_type_str_detail ( const st_MZDSK_DETECT_RESULT *result )
{
    if ( !result ) return "Unknown";

    switch ( result->type ) {
        case MZDSK_FS_FSMZ:        return "FSMZ";
        case MZDSK_FS_CPM:
            return ( result->cpm_format == MZDSK_CPM_FORMAT_HD ) ? "CP/M HD" : "CP/M SD";
        case MZDSK_FS_MRS:         return "MRS";
        case MZDSK_FS_BOOT_ONLY:   return "Boot only";
        case MZDSK_FS_UNKNOWN:
        default:                    return "Unknown";
    }
}


void mzdisk_session_set_last_op ( st_MZDISK_SESSION *session, en_LAST_OP_STATUS status, const char *msg )
{
    if ( !session || !msg ) return;
    session->last_op_status = status;
    strncpy ( session->last_op_msg, msg, sizeof ( session->last_op_msg ) - 1 );
    session->last_op_msg[sizeof ( session->last_op_msg ) - 1] = '\0';
}


void mzdisk_session_format_window_title ( const st_MZDISK_SESSION *session,
                                           char *buf, size_t size )
{
    if ( buf == NULL || size == 0 ) return;

    /* Prefix bez session: aplikace s verzí. */
    if ( session == NULL ) {
        snprintf ( buf, size, "mzdisk v%s", MZDISK_VERSION );
        return;
    }

    /* Primární má jen "mzdisk vX.Y.Z" (bez #0), detached má "mzdisk #N". */
    char prefix[64];
    if ( session->is_primary ) {
        snprintf ( prefix, sizeof ( prefix ), "mzdisk v%s", MZDISK_VERSION );
    } else {
        snprintf ( prefix, sizeof ( prefix ), "mzdisk #%d",
                   session->window_number );
    }

    /* Disk: prefix + " - name.dsk". Bez disku jen prefix. */
    if ( session->has_disk && session->filepath[0] != '\0' ) {
        const char *name = extract_filename ( session->filepath );
        snprintf ( buf, size, "%s - %s", prefix, name );
    } else {
        snprintf ( buf, size, "%s", prefix );
    }
}
