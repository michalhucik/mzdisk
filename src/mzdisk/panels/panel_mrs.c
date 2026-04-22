/**
 * @file panel_mrs.c
 * @brief Naplnění datového modelu MRS adresáře, FAT dat a souborové operace.
 *
 * Kopíruje surová FAT data a layout parametry z MRS konfigurace
 * pro vizualizaci FAT mapy v GUI.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "panel_mrs.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


void panel_mrs_load ( st_PANEL_MRS_DATA *data, st_MZDSK_DETECT_RESULT *detect )
{
    /* zachovat error stav */
    bool had_error = data->has_error;
    char saved_msg[512];
    if ( had_error ) strncpy ( saved_msg, data->error_msg, sizeof ( saved_msg ) );

    memset ( data, 0, sizeof ( *data ) );
    data->detail_index = -1;

    st_FSMRS_CONFIG *config = &detect->mrs_config;
    data->free_blocks = config->free_blocks;
    data->max_files = config->max_files;

    /* FAT vizualizace - zkopírovat surová FAT data a layout parametry */
    memcpy ( data->fat_raw, config->fat, sizeof ( data->fat_raw ) );
    data->total_blocks = config->total_blocks;
    data->fat_block = config->fat_block;
    data->fat_sectors = config->fat_sectors;
    data->dir_block = config->dir_block;
    data->dir_sectors = config->dir_sectors;
    data->data_block = config->data_block;

    /* spočítat statistiky přímo z FAT */
    memset ( &data->fat_stats, 0, sizeof ( data->fat_stats ) );
    data->fat_stats.total_blocks = FSMRS_COUNT_BLOCKS;
    for ( int b = 0; b < FSMRS_COUNT_BLOCKS; b++ ) {
        switch ( config->fat[b] ) {
            case FSMRS_FAT_FREE:   data->fat_stats.free_blocks++; break;
            case FSMRS_FAT_FAT:    data->fat_stats.fat_blocks++; break;
            case FSMRS_FAT_DIR:    data->fat_stats.dir_blocks++; break;
            case FSMRS_FAT_BAD:    data->fat_stats.bad_blocks++; break;
            case FSMRS_FAT_SYSTEM: data->fat_stats.sys_blocks++; break;
            default:               data->fat_stats.file_blocks++; break;
        }
    }

    for ( int i = 0; i < config->max_files && data->file_count < PANEL_MRS_MAX_FILES; i++ ) {
        st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, (uint16_t) i );
        if ( !item ) continue;
        if ( !fsmrs_is_dir_item_active ( item ) ) continue;

        st_PANEL_MRS_FILE *f = &data->files[data->file_count];
        f->index = (uint8_t) i;
        f->file_id = item->file_id;
        f->bsize = item->bsize;
        f->size_bytes = (uint32_t) item->bsize * FSMRS_SECTOR_SIZE;
        f->start_addr = item->fstrt;
        f->exec_addr = item->fexec;

        /* název - 8 znaků, pravostranně oříznout mezery, sanitizovat non-ASCII */
        int j = 0;
        for ( ; j < 8 && item->fname[j] > 0x20; j++ ) {
            f->name[j] = ( item->fname[j] <= 0x7E ) ? (char) item->fname[j] : ' ';
        }
        f->name[j] = '\0';

        /* přípona - 3 znaky, sanitizovat non-ASCII */
        for ( j = 0; j < 3; j++ ) {
            uint8_t c = item->ext[j];
            f->ext[j] = ( c >= 0x20 && c <= 0x7E ) ? (char) c : ' ';
        }
        f->ext[3] = '\0';

        data->file_count++;
    }

    data->is_loaded = true;

    if ( had_error ) {
        data->has_error = true;
        strncpy ( data->error_msg, saved_msg, sizeof ( data->error_msg ) );
    }
}


en_MZDSK_RES panel_mrs_get_file ( st_FSMRS_CONFIG *config,
                                   const st_PANEL_MRS_FILE *file, const char *output_path )
{
    /* najít dir item v MRS konfiguraci */
    st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, file->index );
    if ( !item ) return MZDSK_RES_UNKNOWN_ERROR;

    /* přečíst data souboru */
    uint32_t data_size = (uint32_t) file->bsize * FSMRS_SECTOR_SIZE;
    uint8_t *buffer = (uint8_t *) malloc ( data_size );
    if ( !buffer ) return MZDSK_RES_UNKNOWN_ERROR;

    en_MZDSK_RES res = fsmrs_read_file ( config, item, buffer, data_size );
    if ( res != MZDSK_RES_OK ) {
        free ( buffer );
        return res;
    }

    /* zapsat na lokální disk */
    FILE *fp = fopen ( output_path, "wb" );
    if ( !fp ) {
        free ( buffer );
        return MZDSK_RES_DSK_ERROR;
    }

    size_t written = fwrite ( buffer, 1, data_size, fp );
    fclose ( fp );
    free ( buffer );

    return ( written == data_size ) ? MZDSK_RES_OK : MZDSK_RES_DSK_ERROR;
}


en_MZDSK_RES panel_mrs_get_file_mzf_ex ( st_FSMRS_CONFIG *config,
                                          const st_PANEL_MRS_FILE *file,
                                          const char *output_path,
                                          const char *override_name,
                                          uint16_t fstrt, uint16_t fexec )
{
    st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, file->index );
    if ( !item ) return MZDSK_RES_UNKNOWN_ERROR;

    uint32_t data_size = (uint32_t) file->bsize * FSMRS_SECTOR_SIZE;
    if ( data_size > MZF_MAX_BODY_SIZE || data_size == 0 ) return MZDSK_RES_INVALID_PARAM;

    uint8_t *buffer = (uint8_t *) malloc ( data_size );
    if ( !buffer ) return MZDSK_RES_UNKNOWN_ERROR;

    en_MZDSK_RES res = fsmrs_read_file ( config, item, buffer, data_size );
    if ( res != MZDSK_RES_OK ) {
        free ( buffer );
        return res;
    }

    /* Jméno pro MZF hlavičku: override, nebo z MRS directory entry
       (formát "fname.ext"). */
    char ascii_name[18];
    if ( override_name != NULL && override_name[0] != '\0' ) {
        size_t len = strlen ( override_name );
        if ( len >= sizeof ( ascii_name ) ) len = sizeof ( ascii_name ) - 1;
        memcpy ( ascii_name, override_name, len );
        ascii_name[len] = '\0';
    } else {
        int j = 0;
        for ( int i = 0; i < 8 && item->fname[i] > 0x20; i++ ) {
            ascii_name[j++] = (char) item->fname[i];
        }
        ascii_name[j++] = '.';
        for ( int i = 0; i < 3 && item->ext[i] > 0x20; i++ ) {
            ascii_name[j++] = (char) item->ext[i];
        }
        ascii_name[j] = '\0';
    }

    /* MZF hlavička */
    st_MZF_HEADER mzfhdr;
    memset ( &mzfhdr, 0, sizeof ( mzfhdr ) );
    mzfhdr.ftype = MZF_FTYPE_OBJ;
    mzf_tools_set_fname ( &mzfhdr, ascii_name );
    mzfhdr.fsize = (uint16_t) data_size;
    mzfhdr.fstrt = fstrt;
    mzfhdr.fexec = fexec;

    /* zapsat MZF: hlavička + tělo */
    st_HANDLER *handler = generic_driver_open_memory ( NULL, &g_memory_driver_realloc, 1 );
    if ( !handler ) {
        free ( buffer );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    handler->spec.memspec.swelling_enabled = 1;

    if ( EXIT_SUCCESS != mzf_write_header ( handler, &mzfhdr ) ||
         EXIT_SUCCESS != mzf_write_body ( handler, buffer, (uint16_t) data_size ) ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( buffer );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    free ( buffer );

    if ( EXIT_SUCCESS != generic_driver_save_memory ( handler, (char *) output_path ) ) {
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_DSK_ERROR;
    }

    generic_driver_close ( handler );
    free ( handler );
    return MZDSK_RES_OK;
}


en_MZDSK_RES panel_mrs_get_file_mzf ( st_FSMRS_CONFIG *config,
                                       const st_PANEL_MRS_FILE *file, const char *output_path )
{
    /* Default: jméno + adresy z MRS entry. */
    st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, file->index );
    if ( !item ) return MZDSK_RES_UNKNOWN_ERROR;
    return panel_mrs_get_file_mzf_ex ( config, file, output_path, NULL,
                                         item->fstrt, item->fexec );
}


en_MZDSK_RES panel_mrs_set_addr ( st_FSMRS_CONFIG *config,
                                   const st_PANEL_MRS_FILE *file,
                                   uint16_t fstrt, uint16_t fexec )
{
    if ( !config || !file ) return MZDSK_RES_INVALID_PARAM;
    st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, file->index );
    if ( !item ) return MZDSK_RES_FILE_NOT_FOUND;
    return fsmrs_set_addr ( config, item, fstrt, fexec );
}


/**
 * @brief Pomocná funkce - C-string jméno doplní na 8/3 pole paddované 0x20.
 *
 * @param[in]  name   C-string jméno (max 8 znaků + NUL). Nesmí být NULL.
 * @param[in]  ext    C-string přípona (max 3 znaky + NUL), nebo NULL.
 * @param[out] fname  Výstupní pole 8 bajtů paddované 0x20.
 * @param[out] ext_out Výstupní pole 3 bajtů paddované 0x20.
 */
static void pad_mrs_name ( const char *name, const char *ext,
                            uint8_t fname[8], uint8_t ext_out[3] )
{
    memset ( fname, 0x20, 8 );
    memset ( ext_out, 0x20, 3 );
    size_t nlen = strlen ( name );
    if ( nlen > 8 ) nlen = 8;
    memcpy ( fname, name, nlen );
    if ( ext != NULL ) {
        size_t elen = strlen ( ext );
        if ( elen > 3 ) elen = 3;
        memcpy ( ext_out, ext, elen );
    }
}


en_MZDSK_RES panel_mrs_put_file ( st_FSMRS_CONFIG *config, const char *input_path,
                                    const char *name, const char *ext,
                                    uint16_t fstrt, uint16_t fexec )
{
    /* přečíst vstupní soubor */
    FILE *fp = fopen ( input_path, "rb" );
    if ( !fp ) return MZDSK_RES_DSK_ERROR;

    fseek ( fp, 0, SEEK_END );
    long file_size = ftell ( fp );
    fseek ( fp, 0, SEEK_SET );

    if ( file_size < 0 || file_size > 0xFFFF ) {
        fclose ( fp );
        return MZDSK_RES_INVALID_PARAM;
    }

    uint8_t *buffer = NULL;
    if ( file_size > 0 ) {
        buffer = (uint8_t *) malloc ( (size_t) file_size );
        if ( !buffer ) {
            fclose ( fp );
            return MZDSK_RES_UNKNOWN_ERROR;
        }
        size_t read_count = fread ( buffer, 1, (size_t) file_size, fp );
        if ( (long) read_count != file_size ) {
            free ( buffer );
            fclose ( fp );
            return MZDSK_RES_DSK_ERROR;
        }
    }
    fclose ( fp );

    uint8_t fname_pad[8], ext_pad[3];
    pad_mrs_name ( name, ext, fname_pad, ext_pad );

    en_MZDSK_RES res = fsmrs_write_file ( config, fname_pad, ext_pad,
        fstrt, fexec, buffer, (uint32_t) file_size );

    if ( buffer ) free ( buffer );
    return res;
}


en_MZDSK_RES panel_mrs_put_file_mzf_ex ( st_FSMRS_CONFIG *config, const char *mzf_path,
                                           const char *override_name,
                                           const char *override_ext )
{
    /* načíst MZF soubor */
    st_HANDLER *handler = generic_driver_open_memory_from_file ( NULL, &g_memory_driver_realloc, mzf_path );
    if ( !handler ) return MZDSK_RES_DSK_ERROR;

    generic_driver_set_handler_readonly_status ( handler, 1 );

    st_MZF_HEADER mzfhdr;
    if ( EXIT_SUCCESS != mzf_read_header ( handler, &mzfhdr ) ) {
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_FORMAT_ERROR;
    }

    uint8_t *data = NULL;
    if ( mzfhdr.fsize > 0 ) {
        data = (uint8_t *) malloc ( mzfhdr.fsize );
        if ( !data ) {
            generic_driver_close ( handler );
            free ( handler );
            return MZDSK_RES_UNKNOWN_ERROR;
        }
        if ( EXIT_SUCCESS != mzf_read_body ( handler, data, mzfhdr.fsize ) ) {
            free ( data );
            generic_driver_close ( handler );
            free ( handler );
            return MZDSK_RES_FORMAT_ERROR;
        }
    }

    generic_driver_close ( handler );
    free ( handler );

    /* rozparsovat NAME.EXT */
    uint8_t fname[8], ext[3];

    if ( override_name != NULL ) {
        /* Volající dodal override - použít ho bez změny. */
        pad_mrs_name ( override_name, override_ext, fname, ext );
    } else {
        /* Odvodit z MZF hlavičky (tolerantní, zkracuje na 8.3). */
        char ascii_name[18];
        int len = mzf_tools_get_fname_length ( &mzfhdr );
        for ( int i = 0; i < len && i < 16; i++ ) {
            ascii_name[i] = (char) MZF_UINT8_FNAME ( mzfhdr.fname )[i];
        }
        ascii_name[len] = '\0';

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
    }

    en_MZDSK_RES res = fsmrs_write_file ( config, fname, ext,
        mzfhdr.fstrt, mzfhdr.fexec, data, mzfhdr.fsize );

    if ( data ) free ( data );
    return res;
}


en_MZDSK_RES panel_mrs_put_file_mzf ( st_FSMRS_CONFIG *config, const char *mzf_path )
{
    return panel_mrs_put_file_mzf_ex ( config, mzf_path, NULL, NULL );
}


en_MZDSK_RES panel_mrs_delete_file ( st_FSMRS_CONFIG *config, const st_PANEL_MRS_FILE *file )
{
    st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, file->index );
    if ( !item ) return MZDSK_RES_UNKNOWN_ERROR;
    return fsmrs_delete_file ( config, item );
}


en_MZDSK_RES panel_mrs_rename_file ( st_FSMRS_CONFIG *config, const st_PANEL_MRS_FILE *file,
                                      const char *new_name, const char *new_ext )
{
    st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, file->index );
    if ( !item ) return MZDSK_RES_UNKNOWN_ERROR;

    /* převést ASCII jméno na MRS formát (8 bajtů, doplnit mezerami) */
    uint8_t fname[8];
    uint8_t ext[3];
    memset ( fname, 0x20, 8 );
    memset ( ext, 0x20, 3 );

    int len = (int) strlen ( new_name );
    if ( len > 8 ) len = 8;
    for ( int i = 0; i < len; i++ ) fname[i] = (uint8_t) new_name[i];

    if ( new_ext ) {
        len = (int) strlen ( new_ext );
        if ( len > 3 ) len = 3;
        for ( int i = 0; i < len; i++ ) ext[i] = (uint8_t) new_ext[i];
        return fsmrs_rename_file ( config, item, fname, ext );
    }

    return fsmrs_rename_file ( config, item, fname, NULL );
}
