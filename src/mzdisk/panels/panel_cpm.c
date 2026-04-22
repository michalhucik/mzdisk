/**
 * @file panel_cpm.c
 * @brief Naplnění datového modelu CP/M adresáře, souborové operace a alloc mapa.
 *
 * Implementuje panel_cpm_load() - načtení directory listingu, sestavení
 * alokační bitmapy a per-blokového vlastnictví (block_owner[]). Dále
 * souborové operace Get/Put/Delete/Rename/atributy volající knihovnu
 * mzdsk_cpm.
 *
 * Per-blokové vlastnictví se sestavuje čtením raw adresářových položek
 * a parsováním jejich alokačních polí (8-bit nebo 16-bit čísla bloků
 * podle DSM). Adresářové bloky se identifikují z AL0/AL1 bitmapy v DPB.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "panel_cpm.h"
#include "libs/mzdsk_cpm/mzdsk_cpm_mzf.h"


/**
 * @brief Extrahuje jméno souboru z raw adresářové položky.
 *
 * Kopíruje fname[0..7] s ořezáním koncových mezer, null-terminuje.
 *
 * @param entry Raw adresářová položka.
 * @param name Výstupní buffer (min 9 bajtů).
 */
static void extract_raw_filename ( const st_MZDSK_CPM_DIRENTRY *entry, char *name )
{
    int len = 8;
    while ( len > 0 && entry->fname[len - 1] == 0x20 ) len--;
    for ( int i = 0; i < len; i++ ) {
        uint8_t c = entry->fname[i];
        name[i] = ( c >= 0x20 && c <= 0x7E ) ? (char) c : ' ';
    }
    name[len] = '\0';
}


/**
 * @brief Extrahuje příponu z raw adresářové položky.
 *
 * Kopíruje ext[0..2] s maskováním bitu 7 (atributy) a ořezáním
 * koncových mezer, null-terminuje.
 *
 * @param entry Raw adresářová položka.
 * @param ext Výstupní buffer (min 4 bajty).
 */
static void extract_raw_extension ( const st_MZDSK_CPM_DIRENTRY *entry, char *ext )
{
    int len = 3;
    while ( len > 0 && ( entry->ext[len - 1] & 0x7F ) == 0x20 ) len--;
    for ( int i = 0; i < len; i++ ) {
        uint8_t c = entry->ext[i] & 0x7F;
        ext[i] = ( c >= 0x20 && c <= 0x7E ) ? (char) c : ' ';
    }
    ext[len] = '\0';
}


/**
 * @brief Najde index souboru v poli files[] odpovídající raw dir entry.
 *
 * Porovnává user, filename a extension (po maskování atributových bitů).
 *
 * @param data Datový model s naplněným files[].
 * @param user Číslo uživatele z raw entry.
 * @param fname Extrahované jméno (null-terminated).
 * @param ext Extrahovaná přípona (null-terminated).
 * @return Index do files[] při nalezení, -1 pokud nenalezeno.
 */
static int find_file_index ( const st_PANEL_CPM_DATA *data, uint8_t user,
                              const char *fname, const char *ext )
{
    for ( int i = 0; i < data->file_count; i++ ) {
        if ( data->files[i].user == user &&
             strcmp ( data->files[i].filename, fname ) == 0 &&
             strcmp ( data->files[i].extension, ext ) == 0 ) {
            return i;
        }
    }
    return -1;
}


/**
 * @brief Sestaví block_owner[] pole z raw adresářových položek.
 *
 * Pro každý alokační blok určí vlastníka:
 * - PANEL_CPM_BLOCK_FREE pro neobsazené bloky
 * - PANEL_CPM_BLOCK_DIR pro adresářové bloky (z AL0/AL1)
 * - index do files[] pro souborové bloky
 *
 * @param data Datový model (vstup: files[], dpb; výstup: block_owner[]).
 * @param disc Otevřený disk pro čtení raw adresáře.
 */
static void build_block_ownership ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc )
{
    uint16_t total = data->dpb.dsm + 1;
    if ( total > PANEL_CPM_MAX_BLOCKS ) total = PANEL_CPM_MAX_BLOCKS;

    /* inicializace na FREE */
    for ( uint16_t i = 0; i < total; i++ ) {
        data->block_owner[i] = PANEL_CPM_BLOCK_FREE;
    }

    /* označit adresářové bloky z AL0/AL1 */
    uint16_t al = ( (uint16_t) data->dpb.al0 << 8 ) | data->dpb.al1;
    for ( int b = 0; b < 16 && b < (int) total; b++ ) {
        if ( al & ( 0x8000u >> b ) ) {
            data->block_owner[b] = PANEL_CPM_BLOCK_DIR;
        }
    }

    /* přečíst raw adresář a mapovat bloky na soubory */
    st_MZDSK_CPM_DIRENTRY raw_entries[256];
    int raw_count = mzdsk_cpm_read_raw_directory ( disc, &data->dpb, raw_entries, 256 );
    if ( raw_count <= 0 ) return;

    for ( int i = 0; i < raw_count; i++ ) {
        if ( raw_entries[i].user == MZDSK_CPM_DELETED_ENTRY ) continue;
        if ( raw_entries[i].user > 15 ) continue;

        /* extrahovat jméno a příponu */
        char fname[9], ext[4];
        extract_raw_filename ( &raw_entries[i], fname );
        extract_raw_extension ( &raw_entries[i], ext );

        int file_idx = find_file_index ( data, raw_entries[i].user, fname, ext );
        if ( file_idx < 0 ) continue;

        /* zpracovat alokační bloky */
        if ( data->dpb.dsm <= 255 ) {
            /* 8-bitová čísla bloků (16 slotů) */
            for ( int b = 0; b < 16; b++ ) {
                uint8_t blk = raw_entries[i].alloc[b];
                if ( blk == 0 ) continue;
                if ( blk <= data->dpb.dsm && blk < total ) {
                    data->block_owner[blk] = (uint16_t) file_idx;
                }
            }
        } else {
            /* 16-bitová čísla bloků (8 slotů, LE) */
            for ( int b = 0; b < 8; b++ ) {
                uint16_t blk = (uint16_t) raw_entries[i].alloc[b * 2]
                             | ( (uint16_t) raw_entries[i].alloc[b * 2 + 1] << 8 );
                if ( blk == 0 ) continue;
                if ( blk <= data->dpb.dsm && blk < total ) {
                    data->block_owner[blk] = (uint16_t) file_idx;
                }
            }
        }
    }
}


void panel_cpm_load ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect )
{
    /* zachovat error stav a filter_user přes reload */
    bool had_error = data->has_error;
    char saved_msg[512];
    if ( had_error ) strncpy ( saved_msg, data->error_msg, sizeof ( saved_msg ) );
    int saved_filter_user = data->filter_user;

    memset ( data, 0, sizeof ( *data ) );
    data->detail_index = -1;
    data->filter_user = saved_filter_user;

    /* uložit kopii DPB a formátu pro souborové operace */
    memcpy ( &data->dpb, &detect->cpm_dpb, sizeof ( st_MZDSK_CPM_DPB ) );
    data->cpm_format = detect->cpm_format;

    data->file_count = mzdsk_cpm_read_directory_ex (
        disc, &detect->cpm_dpb, data->files, PANEL_CPM_MAX_FILES
    );

    if ( data->file_count < 0 ) {
        data->file_count = 0;
        return;
    }

    /* sanitizace jmen - nahradit znaky mimo 0x20-0x7E mezerou */
    for ( int i = 0; i < data->file_count; i++ ) {
        for ( int j = 0; data->files[i].filename[j]; j++ ) {
            uint8_t c = (uint8_t) data->files[i].filename[j];
            if ( c < 0x20 || c > 0x7E ) data->files[i].filename[j] = ' ';
        }
        for ( int j = 0; data->files[i].extension[j]; j++ ) {
            uint8_t c = (uint8_t) data->files[i].extension[j];
            if ( c < 0x20 || c > 0x7E ) data->files[i].extension[j] = ' ';
        }
    }

    switch ( detect->cpm_format ) {
        case MZDSK_CPM_FORMAT_SD:
            strncpy ( data->preset_name, "SD (Lamac)", sizeof ( data->preset_name ) );
            break;
        case MZDSK_CPM_FORMAT_HD:
            strncpy ( data->preset_name, "HD (Lucky-Soft)", sizeof ( data->preset_name ) );
            break;
        default:
            strncpy ( data->preset_name, "Custom", sizeof ( data->preset_name ) );
            break;
    }

    /* sestavit alokační mapu a block ownership */
    en_MZDSK_RES alloc_res = mzdsk_cpm_get_alloc_map ( disc, &data->dpb, &data->alloc_map );
    if ( alloc_res == MZDSK_RES_OK ) {
        build_block_ownership ( data, disc );
        data->alloc_loaded = true;
    }

    /* Statistika obsazení directory slotů (total/used/free/blocked). */
    if ( mzdsk_cpm_get_dir_stats ( disc, &data->dpb, &data->dir_stats ) == MZDSK_RES_OK ) {
        data->dir_stats_loaded = true;
    }

    data->is_loaded = true;

    /* obnovit error stav */
    if ( had_error ) {
        data->has_error = true;
        strncpy ( data->error_msg, saved_msg, sizeof ( data->error_msg ) );
    }
}


uint32_t panel_cpm_get_file_buffer_size ( const st_MZDSK_CPM_DPB *dpb )
{
    if ( dpb == NULL ) return 0;
    return (uint32_t) ( dpb->dsm + 1 ) * dpb->block_size;
}


en_MZDSK_RES panel_cpm_get_file_with_buffer ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                               const st_MZDSK_CPM_FILE_INFO_EX *file,
                                               const char *output_path,
                                               uint8_t *buffer, uint32_t buf_size )
{
    if ( disc == NULL || dpb == NULL || file == NULL ||
         output_path == NULL || buffer == NULL ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    if ( buf_size < panel_cpm_get_file_buffer_size ( dpb ) ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    uint32_t bytes_read = 0;
    en_MZDSK_RES res = mzdsk_cpm_read_file ( disc, dpb,
        file->filename, file->extension, file->user,
        buffer, buf_size, &bytes_read );
    if ( res != MZDSK_RES_OK ) return res;

    /* zapsat na lokální disk */
    FILE *fp = fopen ( output_path, "wb" );
    if ( !fp ) return MZDSK_RES_DSK_ERROR;

    size_t written = fwrite ( buffer, 1, bytes_read, fp );
    fclose ( fp );

    return ( written == bytes_read ) ? MZDSK_RES_OK : MZDSK_RES_DSK_ERROR;
}


en_MZDSK_RES panel_cpm_get_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                   const st_MZDSK_CPM_FILE_INFO_EX *file, const char *output_path )
{
    /* Audit M-36: jednorázová alokace bufferu pro single-file export.
       Pro bulk export (multi-select) volající alokuje buffer jednou
       přes panel_cpm_get_file_buffer_size() a reusuje přes
       panel_cpm_get_file_with_buffer(). */
    uint32_t buf_size = panel_cpm_get_file_buffer_size ( dpb );
    if ( buf_size == 0 ) return MZDSK_RES_UNKNOWN_ERROR;

    uint8_t *buffer = (uint8_t *) malloc ( buf_size );
    if ( buffer == NULL ) return MZDSK_RES_UNKNOWN_ERROR;

    en_MZDSK_RES res = panel_cpm_get_file_with_buffer ( disc, dpb, file,
                                                        output_path, buffer, buf_size );
    free ( buffer );
    return res;
}


en_MZDSK_RES panel_cpm_put_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                   const char *input_path,
                                   const char *cpm_name, const char *cpm_ext, uint8_t user )
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

    uint8_t *buffer = (uint8_t *) malloc ( (size_t) file_size );
    if ( !buffer ) {
        fclose ( fp );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    size_t read_count = fread ( buffer, 1, (size_t) file_size, fp );
    fclose ( fp );

    if ( (long) read_count != file_size ) {
        free ( buffer );
        return MZDSK_RES_DSK_ERROR;
    }

    en_MZDSK_RES res = mzdsk_cpm_write_file ( disc, dpb, cpm_name, cpm_ext, user,
                                                buffer, (uint32_t) file_size );
    free ( buffer );
    return res;
}


en_MZDSK_RES panel_cpm_get_file_mzf ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                       const st_MZDSK_CPM_FILE_INFO_EX *file, const char *output_path )
{
    /* přečíst data souboru */
    uint32_t buf_size = (uint32_t) ( dpb->dsm + 1 ) * dpb->block_size;
    uint8_t *buffer = (uint8_t *) malloc ( buf_size );
    if ( !buffer ) return MZDSK_RES_UNKNOWN_ERROR;

    uint32_t bytes_read = 0;
    en_MZDSK_RES res = mzdsk_cpm_read_file ( disc, dpb,
        file->filename, file->extension, file->user,
        buffer, buf_size, &bytes_read );

    if ( res != MZDSK_RES_OK ) {
        free ( buffer );
        return res;
    }

    /* zjistit atributy */
    uint8_t attrs = 0;
    if ( file->read_only ) attrs |= MZDSK_CPM_ATTR_READ_ONLY;
    if ( file->system )    attrs |= MZDSK_CPM_ATTR_SYSTEM;
    if ( file->archived )  attrs |= MZDSK_CPM_ATTR_ARCHIVED;

    /* zakódovat do MZF CPM-IC */
    uint8_t *mzf_data = NULL;
    uint32_t mzf_size = 0;
    res = mzdsk_cpm_mzf_encode ( buffer, bytes_read,
        file->filename, file->extension, attrs,
        MZDSK_CPM_MZF_DEFAULT_ADDR, &mzf_data, &mzf_size );
    free ( buffer );

    if ( res != MZDSK_RES_OK ) return res;

    /* zapsat na lokální disk */
    FILE *fp = fopen ( output_path, "wb" );
    if ( !fp ) {
        free ( mzf_data );
        return MZDSK_RES_DSK_ERROR;
    }

    size_t written = fwrite ( mzf_data, 1, mzf_size, fp );
    fclose ( fp );
    free ( mzf_data );

    return ( written == mzf_size ) ? MZDSK_RES_OK : MZDSK_RES_DSK_ERROR;
}


en_MZDSK_RES panel_cpm_get_file_mzf_ex ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                           const st_MZDSK_CPM_FILE_INFO_EX *file,
                                           const char *output_path,
                                           const st_PANEL_CPM_MZF_EXPORT *opts )
{
    if ( !opts ) return MZDSK_RES_INVALID_PARAM;

    /* přečíst data souboru */
    uint32_t buf_size = (uint32_t) ( dpb->dsm + 1 ) * dpb->block_size;
    uint8_t *buffer = (uint8_t *) malloc ( buf_size );
    if ( !buffer ) return MZDSK_RES_UNKNOWN_ERROR;

    uint32_t bytes_read = 0;
    en_MZDSK_RES res = mzdsk_cpm_read_file ( disc, dpb,
        file->filename, file->extension, file->user,
        buffer, buf_size, &bytes_read );

    if ( res != MZDSK_RES_OK ) {
        free ( buffer );
        return res;
    }

    /* zjistit atributy */
    uint8_t attrs = 0;
    if ( file->read_only ) attrs |= MZDSK_CPM_ATTR_READ_ONLY;
    if ( file->system )    attrs |= MZDSK_CPM_ATTR_SYSTEM;
    if ( file->archived )  attrs |= MZDSK_CPM_ATTR_ARCHIVED;

    /* zakódovat do MZF s rozšířenými volbami */
    uint8_t *mzf_data = NULL;
    uint32_t mzf_size = 0;
    res = mzdsk_cpm_mzf_encode_ex ( buffer, bytes_read,
        file->filename, file->extension, attrs,
        (uint8_t) opts->ftype, (uint16_t) opts->exec_addr,
        (uint16_t) opts->strt_addr,
        ( opts->ftype == MZDSK_CPM_MZF_FTYPE && opts->encode_attrs ) ? 1 : 0,
        &mzf_data, &mzf_size );
    free ( buffer );

    if ( res != MZDSK_RES_OK ) return res;

    /* zapsat na lokální disk */
    FILE *fp = fopen ( output_path, "wb" );
    if ( !fp ) {
        free ( mzf_data );
        return MZDSK_RES_DSK_ERROR;
    }

    size_t written = fwrite ( mzf_data, 1, mzf_size, fp );
    fclose ( fp );
    free ( mzf_data );

    return ( written == mzf_size ) ? MZDSK_RES_OK : MZDSK_RES_DSK_ERROR;
}


void panel_cpm_mzf_export_init ( st_PANEL_CPM_MZF_EXPORT *opts )
{
    if ( !opts ) return;
    memset ( opts, 0, sizeof ( *opts ) );
    opts->ftype = MZDSK_CPM_MZF_FTYPE;
    opts->exec_addr = MZDSK_CPM_MZF_DEFAULT_ADDR;
    opts->strt_addr = MZDSK_CPM_MZF_DEFAULT_ADDR;
    opts->encode_attrs = true;
}


en_MZDSK_RES panel_cpm_put_file_mzf_ex ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                          const char *mzf_path,
                                          const char *override_name,
                                          const char *override_ext,
                                          uint8_t user )
{
    /* přečíst MZF soubor */
    FILE *fp = fopen ( mzf_path, "rb" );
    if ( !fp ) return MZDSK_RES_DSK_ERROR;

    fseek ( fp, 0, SEEK_END );
    long file_size = ftell ( fp );
    fseek ( fp, 0, SEEK_SET );

    if ( file_size < 128 ) {
        fclose ( fp );
        return MZDSK_RES_FORMAT_ERROR;
    }

    uint8_t *mzf_data = (uint8_t *) malloc ( (size_t) file_size );
    if ( !mzf_data ) {
        fclose ( fp );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    size_t read_count = fread ( mzf_data, 1, (size_t) file_size, fp );
    fclose ( fp );

    if ( (long) read_count != file_size ) {
        free ( mzf_data );
        return MZDSK_RES_DSK_ERROR;
    }

    /* dekódovat MZF CPM-IC */
    char cpm_name[9], cpm_ext[4];
    uint8_t cpm_attrs = 0;
    uint8_t *data = NULL;
    uint32_t data_size = 0;

    en_MZDSK_RES res = mzdsk_cpm_mzf_decode ( mzf_data, (uint32_t) file_size,
        cpm_name, cpm_ext, &cpm_attrs, NULL, &data, &data_size );
    free ( mzf_data );

    if ( res != MZDSK_RES_OK ) return res;

    /* Pokud volající poskytl override, přepíšeme jméno/příponu z MZF hlavičky.
       Volající (GUI dialog) je zodpovědný za validaci - tady jen kopírujeme. */
    if ( override_name != NULL ) {
        strncpy ( cpm_name, override_name, sizeof ( cpm_name ) - 1 );
        cpm_name[sizeof ( cpm_name ) - 1] = '\0';
        if ( override_ext != NULL ) {
            strncpy ( cpm_ext, override_ext, sizeof ( cpm_ext ) - 1 );
            cpm_ext[sizeof ( cpm_ext ) - 1] = '\0';
        } else {
            cpm_ext[0] = '\0';
        }
    }

    /* zapsat soubor na CP/M disk */
    res = mzdsk_cpm_write_file ( disc, dpb, cpm_name, cpm_ext, user, data, data_size );
    free ( data );

    if ( res != MZDSK_RES_OK ) return res;

    /* nastavit atributy pokud nějaké jsou */
    if ( cpm_attrs ) {
        mzdsk_cpm_set_attributes ( disc, dpb, cpm_name, cpm_ext, user, cpm_attrs );
    }

    return MZDSK_RES_OK;
}


en_MZDSK_RES panel_cpm_put_file_mzf ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                        const char *mzf_path, uint8_t user )
{
    return panel_cpm_put_file_mzf_ex ( disc, dpb, mzf_path, NULL, NULL, user );
}


en_MZDSK_RES panel_cpm_delete_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                      const st_MZDSK_CPM_FILE_INFO_EX *file )
{
    return mzdsk_cpm_delete_file ( disc, dpb, file->filename, file->extension, file->user );
}


en_MZDSK_RES panel_cpm_rename_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                      const st_MZDSK_CPM_FILE_INFO_EX *file,
                                      const char *new_name, const char *new_ext )
{
    return mzdsk_cpm_rename_file ( disc, dpb,
        file->filename, file->extension, file->user,
        new_name, new_ext );
}


en_MZDSK_RES panel_cpm_set_attrs ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                    const st_MZDSK_CPM_FILE_INFO_EX *file, uint8_t attributes )
{
    return mzdsk_cpm_set_attributes ( disc, dpb, file->filename, file->extension,
                                       file->user, attributes );
}


en_MZDSK_RES panel_cpm_set_user ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                   const st_MZDSK_CPM_FILE_INFO_EX *file, uint8_t new_user )
{
    return mzdsk_cpm_set_user ( disc, dpb, file->filename, file->extension,
                                 file->user, new_user );
}
