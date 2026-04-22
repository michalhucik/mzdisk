/**
 * @file panel_fsmz.c
 * @brief Naplnění datového modelu FSMZ adresáře a souborové operace.
 *
 * Čte adresářové položky přes fsmz_read_dir() iterátor a konvertuje
 * Sharp MZ ASCII názvy souborů na standardní ASCII. Implementuje
 * Get (export do MZF) a Put (import z MZF) operace.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdlib.h>
#include <string.h>
#include "panel_fsmz.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


void panel_fsmz_load ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc )
{
    memset ( data, 0, sizeof ( *data ) );
    data->detail_index = -1;

    st_FSMZ_DIR dir;
    if ( fsmz_open_dir ( disc, &dir ) != MZDSK_RES_OK ) {
        return;
    }

    en_MZDSK_RES res;
    st_FSMZ_DIR_ITEM *item;

    while ( ( item = fsmz_read_dir ( disc, &dir, FSMZ_MAX_DIR_ITEMS, &res ) ) != NULL ) {
        if ( res != MZDSK_RES_OK ) break;

        /* přeskočit smazané položky (ftype == 0x00) */
        if ( item->ftype == 0x00 ) continue;

        if ( data->file_count >= PANEL_FSMZ_MAX_FILES ) break;

        st_PANEL_FSMZ_FILE *f = &data->files[data->file_count];
        /* Audit L-28: dir.position vždy >= 2 (po IPLPRO bloku), ale pojistka
           proti nekonzistentnímu stavu iterátoru. */
        f->index = ( dir.position >= 2 ) ? (uint8_t) ( dir.position - 2 ) : 0;
        f->ftype = item->ftype;
        f->size = item->fsize;
        f->start_addr = item->fstrt;
        f->exec_addr = item->fexec;
        f->block = item->block;
        f->locked = ( item->locked != 0 );

        /* uložit originální Sharp MZ jméno (pro get/put operace) */
        memcpy ( f->mz_fname, item->fname, FSMZ_FNAME_LENGTH );

        /* konverze Sharp MZ ASCII -> ASCII bajt po bajtu */
        int j = 0;
        while ( j < FSMZ_FNAME_LENGTH && item->fname[j] != FSMZ_FNAME_TERMINATOR && item->fname[j] >= 0x20 ) {
            f->name[j] = (char) sharpmz_cnv_from ( item->fname[j] );
            j++;
        }
        f->name[j] = '\0';

        data->file_count++;
    }

    data->is_loaded = true;
}


en_MZDSK_RES panel_fsmz_get_file ( st_MZDSK_DISC *disc, const st_PANEL_FSMZ_FILE *file, const char *mzf_path )
{
    /* vytvoření MZF hlavičky z adresářové položky */
    st_MZF_HEADER *mzfhdr = mzf_tools_create_mzfhdr (
        file->ftype, file->size, file->start_addr, file->exec_addr,
        (const uint8_t *) file->mz_fname, FSMZ_FNAME_LENGTH - 1, NULL
    );
    if ( !mzfhdr ) return MZDSK_RES_UNKNOWN_ERROR;

    /* paměťový handler pro MZF soubor */
    st_HANDLER *handler = generic_driver_open_memory ( NULL, &g_memory_driver_realloc, 1 );
    if ( !handler ) {
        free ( mzfhdr );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    handler->spec.memspec.swelling_enabled = 1;

    /* zapsat MZF hlavičku */
    if ( EXIT_SUCCESS != mzf_write_header ( handler, mzfhdr ) ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Heap místo stacku - viz komentář v panel_boot.c. Audit H-24. */
    uint8_t *body = malloc ( 0xFFFF );
    if ( !body ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* přečíst data z FSMZ bloků a zapsat jako MZF tělo */
    en_MZDSK_RES err = fsmz_read_blocks ( disc, file->block, file->size, body );
    if ( err != MZDSK_RES_OK ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        free ( body );
        return err;
    }

    if ( EXIT_SUCCESS != mzf_write_body ( handler, body, file->size ) ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        free ( body );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* uložit na lokální disk */
    if ( EXIT_SUCCESS != generic_driver_save_memory ( handler, (char *) mzf_path ) ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        free ( body );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    generic_driver_close ( handler );
    free ( handler );
    free ( mzfhdr );
    free ( body );
    return MZDSK_RES_OK;
}


en_MZDSK_RES panel_fsmz_put_file ( st_MZDSK_DISC *disc, const char *mzf_path )
{
    /* načíst MZF soubor do paměti */
    st_HANDLER *handler = generic_driver_open_memory_from_file ( NULL, &g_memory_driver_realloc, mzf_path );
    if ( !handler ) return MZDSK_RES_UNKNOWN_ERROR;

    generic_driver_set_handler_readonly_status ( handler, 1 );

    /* přečíst MZF hlavičku */
    st_MZF_HEADER mzfhdr;
    if ( EXIT_SUCCESS != mzf_read_header ( handler, &mzfhdr ) ) {
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Heap místo stacku - viz komentář výše. Audit H-24. */
    uint8_t *data = malloc ( 0xFFFF );
    if ( !data ) {
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* přečíst MZF tělo */
    if ( EXIT_SUCCESS != mzf_read_body ( handler, data, mzfhdr.fsize ) ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( data );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    generic_driver_close ( handler );
    free ( handler );

    /* zapsat soubor na FSMZ disk */
    en_MZDSK_RES write_res = fsmz_write_file (
        disc, mzfhdr.ftype,
        MZF_UINT8_FNAME ( mzfhdr.fname ),
        mzfhdr.fsize, mzfhdr.fstrt, mzfhdr.fexec,
        data, FSMZ_MAX_DIR_ITEMS
    );
    free ( data );
    return write_res;
}


en_MZDSK_RES panel_fsmz_delete_file ( st_MZDSK_DISC *disc, const st_PANEL_FSMZ_FILE *file )
{
    /* fsmz_unlink_file očekává mutable pointer */
    uint8_t mz_fname[FSMZ_FNAME_LENGTH];
    memcpy ( mz_fname, file->mz_fname, FSMZ_FNAME_LENGTH );
    /* GUI ctí lock flag; force=0 znamená, že uzamčený soubor nelze smazat */
    return fsmz_unlink_file ( disc, mz_fname, FSMZ_MAX_DIR_ITEMS, 0 );
}


en_MZDSK_RES panel_fsmz_rename_file ( st_MZDSK_DISC *disc, const st_PANEL_FSMZ_FILE *file, const char *new_ascii_name )
{
    uint8_t old_mz[FSMZ_FNAME_LENGTH];
    uint8_t new_mz[FSMZ_FNAME_LENGTH];

    memcpy ( old_mz, file->mz_fname, FSMZ_FNAME_LENGTH );
    fsmz_tool_convert_ascii_to_mzfname ( new_mz, (char *) new_ascii_name, 0 );

    /* GUI ctí lock flag; force=0 znamená, že uzamčený soubor nelze přejmenovat */
    return fsmz_rename_file ( disc, old_mz, new_mz, FSMZ_MAX_DIR_ITEMS, 0 );
}


en_MZDSK_RES panel_fsmz_set_addr ( st_MZDSK_DISC *disc,
                                    const st_PANEL_FSMZ_FILE *file,
                                    uint16_t fstrt, uint16_t fexec,
                                    uint8_t ftype )
{
    if ( disc == NULL || file == NULL ) return MZDSK_RES_INVALID_PARAM;
    if ( ftype == 0x00 ) return MZDSK_RES_INVALID_PARAM;

    uint8_t mz_fname[FSMZ_FNAME_LENGTH];
    memcpy ( mz_fname, file->mz_fname, FSMZ_FNAME_LENGTH );

    /* GUI ctí lock flag (force=0) */
    return fsmz_set_addr ( disc, mz_fname, &fstrt, &fexec, &ftype,
                            FSMZ_MAX_DIR_ITEMS, 0 );
}


en_MZDSK_RES panel_fsmz_lock_file ( st_MZDSK_DISC *disc, const st_PANEL_FSMZ_FILE *file, bool locked )
{
    en_MZDSK_RES err;
    st_FSMZ_DIR dir;

    /* najít položku v adresáři podle ID */
    st_FSMZ_DIR_ITEM *item = fsmz_tool_get_diritem_pointer_and_dir_by_id (
        disc, file->index, &dir, FSMZ_MAX_DIR_ITEMS, &err
    );
    if ( !item || err != MZDSK_RES_OK ) return err ? err : MZDSK_RES_UNKNOWN_ERROR;

    /* nastavit příznak a zapsat blok zpět */
    item->locked = locked ? 1 : 0;
    return fsmz_write_dirblock ( disc, &dir, FSMZ_MAX_DIR_ITEMS );
}


const char* panel_fsmz_type_str ( uint8_t ftype )
{
    switch ( ftype ) {
        case 0x01: return "OBJ";
        case 0x02: return "BTX";
        case 0x03: return "BSD";
        case 0x04: return "BRD";
        case 0x05: return "BRD";
        default:   return "???";
    }
}
