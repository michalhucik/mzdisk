/**
 * @file dragdrop.c
 * @brief Implementace DnD přenosu souborů mezi sessions přes MZF.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "dragdrop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "panels/panel_fsmz.h"
#include "panels/panel_cpm.h"
#include "panels/panel_mrs.h"
#include "libs/mzf/mzf.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_cpm/mzdsk_cpm_mzf.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "config.h"


/** @brief Zaregistrovaný session manager (viz dnd_init). */
static st_MZDISK_SESSION_MANAGER *g_dnd_mgr = NULL;


/**
 * @brief Pending stav pro ASK dialog během DnD.
 *
 * Při dup_mode=ASK a prvním konfliktu jmen se iterace zastaví a stav
 * se uloží sem. Hlavní smyčka zavolá render_dnd_ask_dialog, který
 * zobrazí modal. Uživatelská volba uloží applied_dup_mode a iterace
 * pokračuje od uloženého k v dalším frame.
 */
typedef struct st_DND_ASK_STATE {
    bool pending;                                  /**< true = čeká na user input */
    st_DND_FILE_PAYLOAD payload;                   /**< kopie původního payloadu */
    uint64_t dst_session_id;                        /**< cíl */
    bool is_move;                                   /**< kopie/přesun */
    int next_k;                                     /**< první neprovedený index v payload->file_indices */
    int ok_count;                                   /**< kolik už proběhlo OK */
    int fail_count;                                 /**< kolik selhalo */
    int target_cpm_user;                            /**< cílový CP/M user (-1=auto, 0-15=explicit) */
    char conflict_name[128];                        /**< jméno konfliktního souboru (UTF-8, Sharp MZ → UTF-8 konverze) */
    char pending_tmp[512];                          /**< zachovaný temp MZF soubor */
    int pending_src_index;                          /**< index v src panelu pro retry */
} st_DND_ASK_STATE;

static st_DND_ASK_STATE g_dnd_ask = { 0 };


bool dnd_has_pending_ask ( void )
{
    return g_dnd_ask.pending;
}


const char* dnd_get_ask_conflict_name ( void )
{
    return g_dnd_ask.pending ? g_dnd_ask.conflict_name : "";
}


int dnd_get_ask_remaining_count ( void )
{
    if ( !g_dnd_ask.pending ) return 0;
    /* Zbývající = zbytek payloadu od next_k (včetně current). */
    return g_dnd_ask.payload.count - g_dnd_ask.next_k;
}


void dnd_init ( st_MZDISK_SESSION_MANAGER *mgr )
{
    g_dnd_mgr = mgr;
}


void dnd_fill_payload ( st_DND_FILE_PAYLOAD *payload,
                         uint64_t session_id,
                         en_MZDSK_FS_TYPE fs_type,
                         int clicked_index,
                         const bool *selected,
                         int file_count )
{
    if ( !payload ) return;
    payload->source_session_id = session_id;
    payload->fs_type = fs_type;
    payload->count = 0;

    /* Pokud je startovní řádek mezi vybranými, přenášíme celou selekci. */
    bool in_selection = ( selected && clicked_index >= 0 &&
                          clicked_index < file_count && selected[clicked_index] );

    if ( in_selection ) {
        for ( int i = 0; i < file_count && payload->count < DND_MAX_FILES; i++ ) {
            if ( selected[i] ) {
                payload->file_indices[payload->count++] = i;
            }
        }
    } else if ( clicked_index >= 0 ) {
        payload->file_indices[0] = clicked_index;
        payload->count = 1;
    }
}


/* Forward declaration - implementace v dragdrop.c po try_overwrite_in_dst. */
static en_MZDSK_RES dnd_run_iteration (
    st_MZDISK_SESSION *src, st_MZDISK_SESSION *dst,
    const st_DND_FILE_PAYLOAD *payload,
    uint64_t dst_session_id, bool is_move,
    int dup_mode,
    int target_cpm_user,
    int start_k, int ok_count_in, int fail_count_in,
    en_MZDSK_RES first_err_in,
    char *err_msg, size_t err_msg_size );


en_MZDSK_RES dnd_handle_drop (
    const st_DND_FILE_PAYLOAD *payload,
    uint64_t dst_session_id,
    bool is_move,
    int dup_mode,
    int target_cpm_user,
    char *err_msg, size_t err_msg_size )
{
    if ( err_msg && err_msg_size > 0 ) err_msg[0] = '\0';

    if ( !g_dnd_mgr || !payload || dst_session_id == 0 ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size, "DnD not initialized or invalid drop" );
        return MZDSK_RES_INVALID_PARAM;
    }

    st_MZDISK_SESSION *src = mzdisk_session_get_by_id ( g_dnd_mgr, payload->source_session_id );
    if ( !src ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                  "Source session is no longer available" );
        return MZDSK_RES_INVALID_PARAM;
    }

    st_MZDISK_SESSION *dst = mzdisk_session_get_by_id ( g_dnd_mgr, dst_session_id );
    if ( !dst ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                  "Target session is no longer available" );
        return MZDSK_RES_INVALID_PARAM;
    }

    if ( payload->count <= 0 || payload->count > DND_MAX_FILES ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                  "Invalid payload: count out of range (%d)",
                                  payload->count );
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Iterate. Pro ASK mode může iterace při prvním konfliktu suspendnout. */
    return dnd_run_iteration ( src, dst, payload, dst_session_id, is_move,
                                dup_mode, target_cpm_user, 0, 0, 0, MZDSK_RES_OK,
                                err_msg, err_msg_size );
}


/**
 * @brief Vytvoří unikátní dočasný soubor pro MZF výměnu.
 *
 * Používá mkstemp s templatem v systémovém temp adresáři. Vzniklý
 * file descriptor se zavírá hned po vytvoření - nám zajímá jen
 * unikátní cesta, obsah zapisuje/čte panel API.
 *
 * Pokus o vytvoření: nejprve TMPDIR, pak /tmp, pak .  (Windows:
 * TMP, TEMP, pak .). POSIX mkstemp: template musí končit "XXXXXX".
 *
 * @param[out] out_path Buffer pro výslednou cestu (min. 512 bajtů).
 * @param path_size     Velikost bufferu.
 * @return true při úspěchu, false při chybě.
 */
static bool make_temp_mzf_path ( char *out_path, size_t path_size )
{
    const char *tmpdir = getenv ( "TMPDIR" );
    if ( !tmpdir || !tmpdir[0] ) tmpdir = getenv ( "TMP" );
    if ( !tmpdir || !tmpdir[0] ) tmpdir = getenv ( "TEMP" );
    if ( !tmpdir || !tmpdir[0] ) tmpdir = "/tmp";

    int n = snprintf ( out_path, path_size, "%s/mzdisk_dnd_XXXXXX.mzf", tmpdir );
    if ( n <= 0 || (size_t) n >= path_size ) return false;

    int fd = -1;

#if !defined(__MINGW32__) && !defined(__MINGW64__)
    /* mkstemps podporuje .mzf suffix (6 X před sufixem délky 4) */
    fd = mkstemps ( out_path, 4 );
#endif

    if ( fd < 0 ) {
        /* fallback: mkstemp bez sufixu (MinGW nemá mkstemps) */
        n = snprintf ( out_path, path_size, "%s/mzdisk_dnd_XXXXXX", tmpdir );
        if ( n <= 0 || (size_t) n >= path_size ) return false;
        fd = mkstemp ( out_path );
        if ( fd < 0 ) return false;
    }
    close ( fd );
    return true;
}


/**
 * @brief Volání panel_*_get_file_mzf podle FS typu session.
 */
static en_MZDSK_RES get_file_via_mzf ( st_MZDISK_SESSION *s, int idx, const char *path )
{
    switch ( s->detect_result.type ) {
        case MZDSK_FS_FSMZ:
            if ( idx < 0 || idx >= (int) s->fsmz_data.file_count ) return MZDSK_RES_INVALID_PARAM;
            return panel_fsmz_get_file ( &s->disc, &s->fsmz_data.files[idx], path );
        case MZDSK_FS_CPM:
            if ( idx < 0 || idx >= s->cpm_data.file_count ) return MZDSK_RES_INVALID_PARAM;
            return panel_cpm_get_file_mzf ( &s->disc, &s->cpm_data.dpb,
                                             &s->cpm_data.files[idx], path );
        case MZDSK_FS_MRS:
            if ( idx < 0 || idx >= s->mrs_data.file_count ) return MZDSK_RES_INVALID_PARAM;
            return panel_mrs_get_file_mzf ( &s->detect_result.mrs_config,
                                             &s->mrs_data.files[idx], path );
        default:
            break;
    }
    return MZDSK_RES_INVALID_PARAM;
}


/**
 * @brief Volání panel_*_put_file_mzf podle FS typu session.
 *
 * @param cpm_user Cílová CP/M user oblast (0-15). Použije se jen pokud
 *                 je cíl CP/M; pro FSMZ/MRS se ignoruje.
 */
static en_MZDSK_RES put_file_via_mzf ( st_MZDISK_SESSION *s, const char *path,
                                         uint8_t cpm_user )
{
    switch ( s->detect_result.type ) {
        case MZDSK_FS_FSMZ:
            return panel_fsmz_put_file ( &s->disc, path );
        case MZDSK_FS_CPM:
            return panel_cpm_put_file_mzf ( &s->disc, &s->cpm_data.dpb, path, cpm_user );
        case MZDSK_FS_MRS:
            return panel_mrs_put_file_mzf ( &s->detect_result.mrs_config, path );
        default:
            break;
    }
    return MZDSK_RES_INVALID_PARAM;
}


/**
 * @brief Vyřeší target CP/M user pro konkrétní přenos.
 *
 * @param target_cpm_user Požadovaná hodnota z volajícího (-1 = auto, 0-15 = explicit).
 * @param src Zdrojová session (pro auto režim při src=CPM vezme file user).
 * @param src_index Index souboru ve zdroji.
 * @return Finální 0-15 user.
 */
static uint8_t resolve_cpm_user ( int target_cpm_user,
                                    st_MZDISK_SESSION *src, int src_index )
{
    if ( target_cpm_user >= 0 && target_cpm_user <= 15 ) {
        return (uint8_t) target_cpm_user;
    }
    /* -1 = auto: pokud zdroj je CP/M, zachovej source user; jinak 0. */
    if ( src != NULL && src->detect_result.type == MZDSK_FS_CPM
         && src_index >= 0 && src_index < src->cpm_data.file_count ) {
        return src->cpm_data.files[src_index].user;
    }
    return 0;
}


/**
 * @brief Volání panel_*_delete_file podle FS typu session.
 */
static en_MZDSK_RES delete_file_via_fs ( st_MZDISK_SESSION *s, int idx )
{
    switch ( s->detect_result.type ) {
        case MZDSK_FS_FSMZ:
            if ( idx < 0 || idx >= (int) s->fsmz_data.file_count ) return MZDSK_RES_INVALID_PARAM;
            return panel_fsmz_delete_file ( &s->disc, &s->fsmz_data.files[idx] );
        case MZDSK_FS_CPM:
            if ( idx < 0 || idx >= s->cpm_data.file_count ) return MZDSK_RES_INVALID_PARAM;
            return panel_cpm_delete_file ( &s->disc, &s->cpm_data.dpb,
                                            &s->cpm_data.files[idx] );
        case MZDSK_FS_MRS:
            if ( idx < 0 || idx >= s->mrs_data.file_count ) return MZDSK_RES_INVALID_PARAM;
            return panel_mrs_delete_file ( &s->detect_result.mrs_config,
                                            &s->mrs_data.files[idx] );
        default:
            break;
    }
    return MZDSK_RES_INVALID_PARAM;
}


/**
 * @brief Modifikuje jméno v MZF hlavičce souboru přidáním ~N suffixu.
 *
 * Otevře MZF soubor, najde první 0x0d (terminátor) v hlavice.fname a
 * přidá "~N" bezprostředně před něj (nebo na konec pokud terminátor
 * není). Pokud výsledek přeteče MZF_FILE_NAME_LENGTH, zkrátí od zadu.
 *
 * Soubor se přepíše in-place (jen hlavička, tělo zůstává).
 *
 * @param mzf_path Cesta k MZF souboru.
 * @param suffix_n Číslo suffixu (1..99).
 * @return true při úspěchu, false při IO chybě.
 */
static bool mzf_rename_with_suffix ( const char *mzf_path, int suffix_n )
{
    FILE *f = fopen ( mzf_path, "r+b" );
    if ( !f ) return false;

    st_MZF_HEADER hdr;
    if ( fread ( &hdr, sizeof ( hdr ), 1, f ) != 1 ) {
        fclose ( f ); return false;
    }

    /* Najdi délku platného jména (do prvního 0x0d nebo konce). */
    uint8_t *fname = MZF_UINT8_FNAME ( hdr.fname );
    int valid_len = MZF_FILE_NAME_LENGTH;
    for ( int i = 0; i < MZF_FILE_NAME_LENGTH; i++ ) {
        if ( fname[i] == MZF_FNAME_TERMINATOR ) {
            valid_len = i;
            break;
        }
    }

    /* Vytvoř suffix "~N" (ASCII 0x7e + číslice). */
    char suf[8];
    int suf_len = snprintf ( suf, sizeof ( suf ), "~%d", suffix_n );
    if ( suf_len < 1 ) { fclose ( f ); return false; }

    /* Spočítej novou délku - pokud přeteče, ořež původní jméno. */
    int new_valid = valid_len + suf_len;
    if ( new_valid > MZF_FILE_NAME_LENGTH ) {
        valid_len = MZF_FILE_NAME_LENGTH - suf_len;
        if ( valid_len < 0 ) { fclose ( f ); return false; }
        new_valid = MZF_FILE_NAME_LENGTH;
    }

    /* Přepiš jméno - vyplň paddingem 0x0d až do konce. */
    uint8_t new_fname[MZF_FILE_NAME_LENGTH];
    memcpy ( new_fname, fname, valid_len );
    memcpy ( new_fname + valid_len, suf, suf_len );
    for ( int i = new_valid; i < MZF_FILE_NAME_LENGTH; i++ ) {
        new_fname[i] = MZF_FNAME_TERMINATOR;
    }
    memcpy ( fname, new_fname, MZF_FILE_NAME_LENGTH );

    /* Zapiš hlavičku zpět. */
    fseek ( f, 0, SEEK_SET );
    if ( fwrite ( &hdr, sizeof ( hdr ), 1, f ) != 1 ) {
        fclose ( f ); return false;
    }
    fclose ( f );
    return true;
}


/**
 * @brief Zálohuje původní hlavičku MZF souboru do bufferu.
 */
static bool mzf_save_header ( const char *mzf_path, st_MZF_HEADER *out )
{
    FILE *f = fopen ( mzf_path, "rb" );
    if ( !f ) return false;
    bool ok = ( fread ( out, sizeof ( *out ), 1, f ) == 1 );
    fclose ( f );
    return ok;
}


/**
 * @brief Obnoví hlavičku MZF souboru ze zálohy.
 */
static bool mzf_restore_header ( const char *mzf_path, const st_MZF_HEADER *hdr )
{
    FILE *f = fopen ( mzf_path, "r+b" );
    if ( !f ) return false;
    fseek ( f, 0, SEEK_SET );
    bool ok = ( fwrite ( hdr, sizeof ( *hdr ), 1, f ) == 1 );
    fclose ( f );
    return ok;
}


/**
 * @brief Pokusí se přemazat existující soubor v cíli jménem z MZF hlavičky.
 *
 * Pro FSMZ: vyčte mz_fname z MZF hlavičky a zavolá fsmz_unlink_file.
 * Pro CP/M a MRS: zatím není podporováno - vrací FILE_EXISTS, aby
 * volající použil RENAME path jako fallback. (CP/M a MRS vyžadují
 * konverzi MZF jména do FS-specifického kódování, což je mimo MVP.)
 *
 * @param dst Cílová session.
 * @param mzf_path Cesta k temp MZF souboru (pro čtení hlavičky).
 * @return MZDSK_RES_OK při úspěšném smazání, MZDSK_RES_FILE_EXISTS pokud
 *         FS typ nepodporuje OVERWRITE (RENAME fallback),
 *         jinak chybový kód z fsmz_unlink_file.
 */
static en_MZDSK_RES try_overwrite_in_dst ( st_MZDISK_SESSION *dst, const char *mzf_path )
{
    if ( dst->detect_result.type == MZDSK_FS_FSMZ ) {
        st_MZF_HEADER hdr;
        if ( !mzf_save_header ( mzf_path, &hdr ) ) return MZDSK_RES_DSK_ERROR;
        uint8_t mz_fname[FSMZ_FNAME_LENGTH];
        /* FSMZ jméno má stejný formát jako MZF fname (Sharp MZ ASCII +
         * 0x0d terminátor). Délky se mohou lišit - kopírujeme min. */
        size_t copy_len = ( FSMZ_FNAME_LENGTH < MZF_FILE_NAME_LENGTH )
                          ? FSMZ_FNAME_LENGTH : MZF_FILE_NAME_LENGTH;
        memcpy ( mz_fname, MZF_UINT8_FNAME ( hdr.fname ), copy_len );
        if ( FSMZ_FNAME_LENGTH > copy_len ) {
            memset ( mz_fname + copy_len, 0x0d, FSMZ_FNAME_LENGTH - copy_len );
        }
        /* force=1: přemazat i uzamčené soubory (OVERWRITE semantics). */
        return fsmz_unlink_file ( &dst->disc, mz_fname, FSMZ_MAX_DIR_ITEMS, 1 );
    }

    if ( dst->detect_result.type == MZDSK_FS_CPM ) {
        /* Načti MZF do paměti a dekóduj CP/M jméno. */
        FILE *fp = fopen ( mzf_path, "rb" );
        if ( !fp ) return MZDSK_RES_DSK_ERROR;
        fseek ( fp, 0, SEEK_END );
        long file_size = ftell ( fp );
        fseek ( fp, 0, SEEK_SET );
        if ( file_size < 128 ) { fclose ( fp ); return MZDSK_RES_FORMAT_ERROR; }
        uint8_t *mzf_data = (uint8_t *) malloc ( (size_t) file_size );
        if ( !mzf_data ) { fclose ( fp ); return MZDSK_RES_UNKNOWN_ERROR; }
        size_t rc = fread ( mzf_data, 1, (size_t) file_size, fp );
        fclose ( fp );
        if ( (long) rc != file_size ) { free ( mzf_data ); return MZDSK_RES_DSK_ERROR; }

        char cpm_name[9], cpm_ext[4];
        uint8_t attrs = 0;
        uint8_t *body = NULL;
        uint32_t body_size = 0;
        en_MZDSK_RES r = mzdsk_cpm_mzf_decode ( mzf_data, (uint32_t) file_size,
                                                 cpm_name, cpm_ext, &attrs,
                                                 NULL, &body, &body_size );
        free ( mzf_data );
        if ( body ) free ( body );
        if ( r != MZDSK_RES_OK ) return r;

        /* CP/M put_file_mzf používá user=0 natvrdo, takže i delete = user 0. */
        return mzdsk_cpm_delete_file ( &dst->disc, &dst->cpm_data.dpb,
                                        cpm_name, cpm_ext, 0 );
    }

    if ( dst->detect_result.type == MZDSK_FS_MRS ) {
        /* MRS: načti MZF hlavičku a extrahuj fname/ext stejným způsobem
         * jako panel_mrs_put_file_mzf_ex. Pak fsmrs_search_file +
         * fsmrs_delete_file. */
        st_MZF_HEADER hdr;
        if ( !mzf_save_header ( mzf_path, &hdr ) ) return MZDSK_RES_DSK_ERROR;

        /* Rozparsování 8.3 z MZF fname (stejně jako panel_mrs_put_file_mzf_ex
         * v non-override větvi). */
        char ascii_name[18];
        int len = mzf_tools_get_fname_length ( &hdr );
        if ( len > 16 ) len = 16;
        for ( int i = 0; i < len; i++ ) {
            ascii_name[i] = (char) MZF_UINT8_FNAME ( hdr.fname )[i];
        }
        ascii_name[len] = '\0';

        uint8_t fname[8], ext[3];
        memset ( fname, 0x20, 8 );
        memset ( ext, 0x20, 3 );
        const char *dot = strchr ( ascii_name, '.' );
        if ( dot ) {
            int nlen = (int) ( dot - ascii_name );
            if ( nlen > 8 ) nlen = 8;
            for ( int i = 0; i < nlen; i++ ) fname[i] = (uint8_t) ascii_name[i];
            int elen = (int) strlen ( dot + 1 );
            if ( elen > 3 ) elen = 3;
            for ( int i = 0; i < elen; i++ ) ext[i] = (uint8_t) dot[1 + i];
        } else {
            int nlen = (int) strlen ( ascii_name );
            if ( nlen > 8 ) nlen = 8;
            for ( int i = 0; i < nlen; i++ ) fname[i] = (uint8_t) ascii_name[i];
        }

        st_FSMRS_DIR_ITEM *item = fsmrs_search_file ( &dst->detect_result.mrs_config,
                                                        fname, ext );
        if ( !item ) return MZDSK_RES_FILE_NOT_FOUND;
        return fsmrs_delete_file ( &dst->detect_result.mrs_config, item );
    }

    return MZDSK_RES_FILE_EXISTS;
}


/**
 * @brief Získá UTF-8 reprezentaci jména z MZF hlavičky na disku.
 *
 * Čte prvních 128 bajtů z mzf_path a extrahuje `fname` (Sharp MZ ASCII),
 * konvertuje každý znak přes sharpmz_to_utf8 (SHARPMZ_CHARSET_EU) pro
 * správné zobrazení malých písmen a akcentovaných znaků v UI dialogech.
 *
 * Bez konverze by malá písmena (které mají v Sharp MZ ASCII hodnoty
 * nad 0x7E) byly filtrovány jako non-printable a zobrazené jako '?'.
 *
 * @param mzf_path Cesta k MZF souboru.
 * @param[out] out Buffer pro výslednou UTF-8 hlášku (1 MZ znak = až 4 B UTF-8).
 * @param out_size Velikost bufferu (doporučeně ≥ 128 B).
 * @return true při úspěchu.
 */
static bool extract_mzf_ascii_name ( const char *mzf_path, char *out, size_t out_size )
{
    if ( !mzf_path || !out || out_size == 0 ) return false;
    st_MZF_HEADER hdr;
    if ( !mzf_save_header ( mzf_path, &hdr ) ) {
        snprintf ( out, out_size, "?" );
        return false;
    }
    int len = mzf_tools_get_fname_length ( &hdr );
    if ( len > MZF_FILE_NAME_LENGTH ) len = MZF_FILE_NAME_LENGTH;
    const uint8_t *fname = MZF_UINT8_FNAME ( hdr.fname );

    size_t pos = 0;
    out[0] = '\0';
    for ( int i = 0; i < len; i++ ) {
        const char *utf = sharpmz_to_utf8 ( fname[i], SHARPMZ_CHARSET_EU );
        if ( !utf ) continue;
        size_t ulen = strlen ( utf );
        if ( pos + ulen + 1 >= out_size ) break;
        memcpy ( out + pos, utf, ulen );
        pos += ulen;
    }
    out[pos] = '\0';
    return true;
}


/**
 * @brief Provede get + put + (volitelně) delete src pro jeden item.
 *
 * Bez dup_mode logiky - čistě get/put. Při FILE_EXISTS vrátí chybu.
 * Volající rozhodne o dalším postupu (rename/overwrite/skip/ask).
 *
 * @param keep_temp_on_conflict true = při FILE_EXISTS ponechat temp
 *        soubor na disku (pro retry) a vrátit cestu v out_temp.
 * @param[out] out_temp Buffer pro temp cestu (min 512 B). Naplněno
 *        pokud FILE_EXISTS a keep_temp_on_conflict.
 * @param[out] out_conflict Buffer pro jméno konfliktního souboru
 *        (ASCII reprezentace). Naplněno při FILE_EXISTS.
 */
static en_MZDSK_RES dnd_try_raw_transfer (
    st_MZDISK_SESSION *src, int src_index,
    st_MZDISK_SESSION *dst,
    bool keep_temp_on_conflict,
    int target_cpm_user,
    char *out_temp, size_t out_temp_size,
    char *out_conflict, size_t out_conflict_size,
    char *err_msg, size_t err_msg_size )
{
    char tmp_path[512];
    if ( !make_temp_mzf_path ( tmp_path, sizeof ( tmp_path ) ) ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size, "Failed to create temp file" );
        return MZDSK_RES_DSK_ERROR;
    }
    en_MZDSK_RES r = get_file_via_mzf ( src, src_index, tmp_path );
    if ( r != MZDSK_RES_OK ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size, "Get failed: %s",
                                  mzdsk_get_error ( r ) );
        remove ( tmp_path );
        return r;
    }
    uint8_t resolved_user = resolve_cpm_user ( target_cpm_user, src, src_index );
    r = put_file_via_mzf ( dst, tmp_path, resolved_user );
    if ( r == MZDSK_RES_FILE_EXISTS && keep_temp_on_conflict ) {
        if ( out_temp && out_temp_size > 0 ) {
            strncpy ( out_temp, tmp_path, out_temp_size - 1 );
            out_temp[out_temp_size - 1] = '\0';
        }
        if ( out_conflict && out_conflict_size > 0 ) {
            extract_mzf_ascii_name ( tmp_path, out_conflict, out_conflict_size );
        }
        /* Temp zachován - volající si ho odstraní po resume. */
        return r;
    }
    remove ( tmp_path );
    return r;
}


en_MZDSK_RES dnd_transfer_file (
    st_MZDISK_SESSION *src, int src_index,
    st_MZDISK_SESSION *dst,
    bool is_move,
    int dup_mode,
    int target_cpm_user,
    char *err_msg, size_t err_msg_size )
{
    if ( err_msg && err_msg_size > 0 ) err_msg[0] = '\0';

    if ( !src || !dst || !src->is_open || !dst->is_open ||
         !src->has_disk || !dst->has_disk ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                  "Source or target session is not available" );
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Vytvoř temp MZF */
    char tmp_path[512];
    if ( !make_temp_mzf_path ( tmp_path, sizeof ( tmp_path ) ) ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                  "Failed to create temporary file" );
        return MZDSK_RES_DSK_ERROR;
    }

    /* Get ze zdroje do temp MZF */
    en_MZDSK_RES r = get_file_via_mzf ( src, src_index, tmp_path );
    if ( r != MZDSK_RES_OK ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                  "Get from source failed: %s",
                                  mzdsk_get_error ( r ) );
        remove ( tmp_path );
        return r;
    }

    uint8_t resolved_user = resolve_cpm_user ( target_cpm_user, src, src_index );

    /* Put z temp MZF do cíle. Při FILE_EXISTS aplikuj dup_mode politiku. */
    r = put_file_via_mzf ( dst, tmp_path, resolved_user );
    if ( r == MZDSK_RES_FILE_EXISTS ) {
        if ( dup_mode == MZDSK_EXPORT_DUP_SKIP ) {
            /* Skip: neuloží se, ale není to chyba - vrátíme OK s msg. */
            if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                      "Skipped: target already has the file" );
            remove ( tmp_path );
            return MZDSK_RES_OK;
        }
        if ( dup_mode == MZDSK_EXPORT_DUP_OVERWRITE ) {
            /* Overwrite: najdi a smaž existující soubor v cíli, pak retry put.
             * Pro FSMZ implementováno; pro CPM/MRS fallback na RENAME. */
            en_MZDSK_RES over = try_overwrite_in_dst ( dst, tmp_path );
            if ( over == MZDSK_RES_OK ) {
                r = put_file_via_mzf ( dst, tmp_path, resolved_user );
                if ( r == MZDSK_RES_OK ) {
                    /* Úspěch - pokračuj níže do společného úspěchu */
                } else if ( r != MZDSK_RES_FILE_EXISTS ) {
                    if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                              "Put after overwrite failed: %s",
                                              mzdsk_get_error ( r ) );
                    remove ( tmp_path );
                    return r;
                }
                /* Pokud retry stále FILE_EXISTS, padá na RENAME níže. */
            }
            /* Pokud overwrite vrátí FILE_EXISTS (CPM/MRS unsupported),
             * padá na RENAME path níže. */
        }
    }
    if ( r == MZDSK_RES_FILE_EXISTS ) {
        /* RENAME, ASK nebo OVERWRITE fallback: zkoušej ~1 .. ~99. */
        st_MZF_HEADER orig_hdr;
        if ( !mzf_save_header ( tmp_path, &orig_hdr ) ) {
            if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                      "Failed to read temp MZF header for rename" );
            remove ( tmp_path );
            return MZDSK_RES_DSK_ERROR;
        }
        bool renamed_ok = false;
        for ( int n = 1; n <= 99; n++ ) {
            /* Restoruj hlavičku (rename vychází z originálu ne z předchozího pokusu). */
            mzf_restore_header ( tmp_path, &orig_hdr );
            if ( !mzf_rename_with_suffix ( tmp_path, n ) ) continue;
            r = put_file_via_mzf ( dst, tmp_path, resolved_user );
            if ( r == MZDSK_RES_OK ) { renamed_ok = true; break; }
            if ( r != MZDSK_RES_FILE_EXISTS ) break;  /* jiná chyba */
        }
        if ( !renamed_ok ) {
            if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                      "Put failed after rename attempts: %s",
                                      mzdsk_get_error ( r ) );
            remove ( tmp_path );
            return r;
        }
    } else if ( r != MZDSK_RES_OK ) {
        if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                  "Put to target failed: %s",
                                  mzdsk_get_error ( r ) );
        remove ( tmp_path );
        return r;
    }

    /* Úspěch put - cíl je dirty, reload panelu cíle */
    dst->is_dirty = true;

    /* Move: smazat zdroj */
    if ( is_move ) {
        r = delete_file_via_fs ( src, src_index );
        if ( r != MZDSK_RES_OK ) {
            if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                      "Copied OK, but delete from source failed: %s",
                                      mzdsk_get_error ( r ) );
            remove ( tmp_path );
            return r;
        }
        src->is_dirty = true;
    }

    remove ( tmp_path );
    return MZDSK_RES_OK;
}


/**
 * @brief Finalizuje drop iteraci - reload panels a nastaví last_op.
 */
static en_MZDSK_RES dnd_finalize_drop (
    st_MZDISK_SESSION *src, st_MZDISK_SESSION *dst,
    const st_DND_FILE_PAYLOAD *payload,
    bool is_move, int ok_count, int fail_count,
    en_MZDSK_RES first_err, char *err_msg, size_t err_msg_size )
{
    if ( ok_count > 0 ) {
        mzdisk_session_reload_panels ( dst );
        if ( is_move && src != dst ) {
            mzdisk_session_reload_panels ( src );
        }
    }
    char op_msg[64];
    const char *action = is_move ? "DnD Move" : "DnD Copy";
    if ( payload->count == 1 ) {
        snprintf ( op_msg, sizeof ( op_msg ), "%s", action );
    } else {
        snprintf ( op_msg, sizeof ( op_msg ), "%s (%d/%d)",
                   action, ok_count, payload->count );
    }
    if ( fail_count == 0 ) {
        mzdisk_session_set_last_op ( dst, LAST_OP_OK, op_msg );
        if ( is_move && src != dst && ok_count > 0 ) {
            mzdisk_session_set_last_op ( src, LAST_OP_OK, op_msg );
        }
        return MZDSK_RES_OK;
    }
    mzdisk_session_set_last_op ( dst, LAST_OP_FAILED, op_msg );
    if ( err_msg && err_msg[0] == '\0' ) {
        snprintf ( err_msg, err_msg_size,
                   "%d of %d transfers failed", fail_count, payload->count );
    }
    return first_err;
}


/**
 * @brief Iterace přes payload, zpracování každého souboru s dup_mode
 * logikou. Pro ASK se při prvním konfliktu suspendne (state v g_dnd_ask).
 *
 * @param dup_mode Initiální dup_mode. Po uživatelově volbě v ASK dialogu
 *                  se volá znovu s konkrétním mode (OVERWRITE/RENAME/SKIP).
 * @param start_k Začínající index v payload->file_indices[] (obvykle 0,
 *                  po resume = next_k+1).
 */
static en_MZDSK_RES dnd_run_iteration (
    st_MZDISK_SESSION *src, st_MZDISK_SESSION *dst,
    const st_DND_FILE_PAYLOAD *payload,
    uint64_t dst_session_id, bool is_move,
    int dup_mode,
    int target_cpm_user,
    int start_k, int ok_count_in, int fail_count_in,
    en_MZDSK_RES first_err_in,
    char *err_msg, size_t err_msg_size )
{
    int ok_count = ok_count_in;
    int fail_count = fail_count_in;
    en_MZDSK_RES first_err = first_err_in;
    char local_err[256] = {0};

    for ( int k = start_k; k < payload->count; k++ ) {
        int idx = payload->file_indices[k];

        if ( dup_mode == MZDSK_EXPORT_DUP_ASK ) {
            /* ASK mode: zkusit raw transfer, při konfliktu suspendnout. */
            char tmp_path[512] = {0};
            char conflict[128] = {0};
            en_MZDSK_RES r = dnd_try_raw_transfer (
                src, idx, dst, true,
                target_cpm_user,
                tmp_path, sizeof ( tmp_path ),
                conflict, sizeof ( conflict ),
                local_err, sizeof ( local_err ) );

            if ( r == MZDSK_RES_FILE_EXISTS ) {
                /* Suspend: uložit state pro resume po user choice. */
                memset ( &g_dnd_ask, 0, sizeof ( g_dnd_ask ) );
                g_dnd_ask.pending = true;
                g_dnd_ask.payload = *payload;
                g_dnd_ask.dst_session_id = dst_session_id;
                g_dnd_ask.is_move = is_move;
                g_dnd_ask.next_k = k;
                g_dnd_ask.ok_count = ok_count;
                g_dnd_ask.fail_count = fail_count;
                g_dnd_ask.target_cpm_user = target_cpm_user;
                strncpy ( g_dnd_ask.pending_tmp, tmp_path,
                          sizeof ( g_dnd_ask.pending_tmp ) - 1 );
                strncpy ( g_dnd_ask.conflict_name, conflict,
                          sizeof ( g_dnd_ask.conflict_name ) - 1 );
                g_dnd_ask.pending_src_index = idx;
                if ( err_msg ) snprintf ( err_msg, err_msg_size,
                                          "Awaiting user choice for '%s'", conflict );
                return MZDSK_RES_OK;  /* odloženo, dialog to pokračuje */
            }
            if ( r == MZDSK_RES_OK ) {
                dst->is_dirty = true;
                if ( is_move ) {
                    en_MZDSK_RES dr = delete_file_via_fs ( src, idx );
                    if ( dr != MZDSK_RES_OK ) r = dr;
                    else src->is_dirty = true;
                }
            }
            if ( r == MZDSK_RES_OK ) ok_count++;
            else {
                fail_count++;
                if ( first_err == MZDSK_RES_OK ) first_err = r;
                fprintf ( stderr, "DnD item %d failed: %s\n", idx, local_err );
            }
        } else {
            /* Non-ASK: klasický transfer_file s dup_mode logikou uvnitř. */
            en_MZDSK_RES r = dnd_transfer_file ( src, idx, dst,
                                                  is_move, dup_mode,
                                                  target_cpm_user,
                                                  local_err, sizeof ( local_err ) );
            if ( r == MZDSK_RES_OK ) ok_count++;
            else {
                fail_count++;
                if ( first_err == MZDSK_RES_OK ) first_err = r;
                fprintf ( stderr, "DnD item %d failed: %s\n", idx, local_err );
            }
        }
    }

    return dnd_finalize_drop ( src, dst, payload, is_move,
                                ok_count, fail_count, first_err,
                                err_msg, err_msg_size );
}


/**
 * @brief Aplikuje uživatelovu volbu na pending item a pokračuje iterace.
 *
 * Volá se z render_dnd_ask_dialog po kliknutí na tlačítko modálu.
 *
 * @param applied_mode MZDSK_EXPORT_DUP_OVERWRITE / RENAME / SKIP.
 * @param apply_all    true = apply to all remaining (propagovat mode
 *                     do zbytku bez dalšího dotazu); false = jen
 *                     aktuální soubor, zbytek znovu v ASK módu.
 */
static void dnd_apply_ask_choice ( int applied_mode, bool apply_all )
{
    if ( !g_dnd_ask.pending || !g_dnd_mgr ) return;

    st_MZDISK_SESSION *src = mzdisk_session_get_by_id ( g_dnd_mgr,
                                                         g_dnd_ask.payload.source_session_id );
    st_MZDISK_SESSION *dst = mzdisk_session_get_by_id ( g_dnd_mgr,
                                                         g_dnd_ask.dst_session_id );

    if ( !src || !dst ) {
        /* Zdroj/cíl zmizel - ukončit drop. */
        remove ( g_dnd_ask.pending_tmp );
        memset ( &g_dnd_ask, 0, sizeof ( g_dnd_ask ) );
        return;
    }

    uint8_t ask_user = resolve_cpm_user ( g_dnd_ask.target_cpm_user,
                                           src, g_dnd_ask.pending_src_index );

    /* Aplikuj volbu na aktuální pending item. */
    en_MZDSK_RES r = MZDSK_RES_FILE_EXISTS;
    if ( applied_mode == MZDSK_EXPORT_DUP_SKIP ) {
        r = MZDSK_RES_OK;  /* Skip = přeskoč, ale counted jako úspěch (no-op). */
    } else if ( applied_mode == MZDSK_EXPORT_DUP_OVERWRITE ) {
        en_MZDSK_RES o = try_overwrite_in_dst ( dst, g_dnd_ask.pending_tmp );
        if ( o == MZDSK_RES_OK ) {
            r = put_file_via_mzf ( dst, g_dnd_ask.pending_tmp, ask_user );
        } else if ( o == MZDSK_RES_FILE_EXISTS ) {
            /* OVERWRITE unsupported pro tento FS - fallback na RENAME. */
            applied_mode = MZDSK_EXPORT_DUP_RENAME;
        } else {
            r = o;
        }
    }
    if ( applied_mode == MZDSK_EXPORT_DUP_RENAME && r == MZDSK_RES_FILE_EXISTS ) {
        st_MZF_HEADER orig_hdr;
        if ( mzf_save_header ( g_dnd_ask.pending_tmp, &orig_hdr ) ) {
            for ( int n = 1; n <= 99; n++ ) {
                mzf_restore_header ( g_dnd_ask.pending_tmp, &orig_hdr );
                if ( !mzf_rename_with_suffix ( g_dnd_ask.pending_tmp, n ) ) continue;
                r = put_file_via_mzf ( dst, g_dnd_ask.pending_tmp, ask_user );
                if ( r == MZDSK_RES_OK ) break;
                if ( r != MZDSK_RES_FILE_EXISTS ) break;
            }
        }
    }

    int ok = g_dnd_ask.ok_count;
    int fail = g_dnd_ask.fail_count;
    if ( r == MZDSK_RES_OK ) {
        dst->is_dirty = true;
        if ( g_dnd_ask.is_move && applied_mode != MZDSK_EXPORT_DUP_SKIP ) {
            en_MZDSK_RES dr = delete_file_via_fs ( src, g_dnd_ask.pending_src_index );
            if ( dr == MZDSK_RES_OK ) src->is_dirty = true;
            else { fail++; }
        }
        if ( applied_mode != MZDSK_EXPORT_DUP_SKIP ) ok++;
        else ok++;  /* SKIP - považujeme za úspěch (no-op) */
    } else {
        fail++;
        fprintf ( stderr, "DnD resume failed: %s\n", mzdsk_get_error ( r ) );
    }

    remove ( g_dnd_ask.pending_tmp );

    /* Pokračuj iterace od next_k+1.
       - apply_all = true:  mode se propisuje (stávající chování).
       - apply_all = false: mode zpět na ASK, při další kolizi popup. */
    st_DND_FILE_PAYLOAD payload_copy = g_dnd_ask.payload;
    uint64_t dst_id = g_dnd_ask.dst_session_id;
    bool is_move = g_dnd_ask.is_move;
    int next_start = g_dnd_ask.next_k + 1;
    int remembered_cpm_user = g_dnd_ask.target_cpm_user;
    memset ( &g_dnd_ask, 0, sizeof ( g_dnd_ask ) );

    int next_mode = apply_all ? applied_mode : MZDSK_EXPORT_DUP_ASK;
    char err[256] = {0};
    dnd_run_iteration ( src, dst, &payload_copy, dst_id, is_move,
                         next_mode, remembered_cpm_user,
                         next_start, ok, fail, MZDSK_RES_OK,
                         err, sizeof ( err ) );
}


void dnd_user_chose_ask ( int applied_mode, bool apply_all )
{
    dnd_apply_ask_choice ( applied_mode, apply_all );
}


void dnd_cancel_ask ( void )
{
    if ( !g_dnd_ask.pending || !g_dnd_mgr ) return;
    st_MZDISK_SESSION *src = mzdisk_session_get_by_id ( g_dnd_mgr,
                                                         g_dnd_ask.payload.source_session_id );
    st_MZDISK_SESSION *dst = mzdisk_session_get_by_id ( g_dnd_mgr,
                                                         g_dnd_ask.dst_session_id );
    remove ( g_dnd_ask.pending_tmp );
    if ( src && dst && g_dnd_ask.ok_count > 0 ) {
        mzdisk_session_reload_panels ( dst );
        if ( g_dnd_ask.is_move && src != dst ) {
            mzdisk_session_reload_panels ( src );
        }
        char op_msg[64];
        snprintf ( op_msg, sizeof ( op_msg ), "DnD %s (%d/%d, cancelled)",
                   g_dnd_ask.is_move ? "Move" : "Copy",
                   g_dnd_ask.ok_count, g_dnd_ask.payload.count );
        mzdisk_session_set_last_op ( dst, LAST_OP_FAILED, op_msg );
    }
    memset ( &g_dnd_ask, 0, sizeof ( g_dnd_ask ) );
}
