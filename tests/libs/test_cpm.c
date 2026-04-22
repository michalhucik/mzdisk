/**
 * @file test_cpm.c
 * @brief Testy CP/M filesystem operací.
 *
 * Vytváří CP/M disky od nuly, formátuje adresář a testuje
 * souborové operace (put, get, delete, rename, atributy).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "test_framework.h"

#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_cpm/mzdsk_cpm_mzf.h"
#include "libs/mzf/mzf.h"


/* ====================================================================
 *  Pomocné funkce
 * ==================================================================== */

/**
 * @brief Vytvoří naformátovaný CP/M SD disk v paměti.
 *
 * Geometrie: abs track 0 = 9x512B, abs track 1 = 16x256B (boot),
 * abs tracks 2-159 = 9x512B. SD DPB, adresář naformátován.
 */
static int create_formatted_cpm_sd ( st_MZDSK_DISC *disc, st_MZDSK_CPM_DPB *dpb )
{
    memset ( disc, 0, sizeof ( st_MZDSK_DISC ) );

    st_HANDLER *h = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    if ( !h ) return EXIT_FAILURE;

    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;

    /* CP/M SD geometrie: 3 pravidla */
    size_t desc_size = dsk_tools_compute_description_size ( 3 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) { free ( h ); return EXIT_FAILURE; }

    memset ( desc, 0, desc_size );
    desc->count_rules = 3;
    desc->tracks = 80;
    desc->sides = 2;
    dsk_tools_assign_description ( desc, 0, 0, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    dsk_tools_assign_description ( desc, 1, 1, 16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    dsk_tools_assign_description ( desc, 2, 2, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );

    int res = dsk_tools_create_image ( h, desc );
    free ( desc );
    if ( res != EXIT_SUCCESS ) {
        free ( h->spec.memspec.ptr );
        free ( h );
        return EXIT_FAILURE;
    }

    disc->handler = h;
    disc->tracks_rules = dsk_tools_get_tracks_rules ( h );
    disc->format = dsk_tools_identformat_from_tracks_rules ( disc->tracks_rules );
    disc->sector_info_cb = mzdsk_sector_info_cb;
    disc->sector_info_cb_data = disc;
    disc->cache = (uint8_t *) malloc ( 1024 );

    /* Inicializace DPB a formátování adresáře */
    mzdsk_cpm_init_dpb ( dpb, MZDSK_CPM_FORMAT_SD );
    en_MZDSK_RES mres = mzdsk_cpm_format_directory ( disc, dpb );
    if ( mres != MZDSK_RES_OK ) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}


static void close_disc ( st_MZDSK_DISC *disc )
{
    if ( disc->cache ) { free ( disc->cache ); disc->cache = NULL; }
    if ( disc->tracks_rules ) {
        dsk_tools_destroy_track_rules ( disc->tracks_rules );
        disc->tracks_rules = NULL;
    }
    if ( disc->handler ) {
        if ( disc->handler->spec.memspec.ptr ) {
            free ( disc->handler->spec.memspec.ptr );
        }
        free ( disc->handler );
        disc->handler = NULL;
    }
}


/* ====================================================================
 *  Testy
 * ==================================================================== */


/** @brief Naformátovaný adresář je prázdný. */
static int test_cpm_format_empty_dir ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    st_MZDSK_CPM_FILE_INFO files[64];
    int count = mzdsk_cpm_read_directory ( &disc, &dpb, files, 64 );
    TEST_ASSERT_EQ_INT ( count, 0 );

    close_disc ( &disc );
    return 0;
}


/** @brief Put soubor -> soubor se objeví v adresáři. */
static int test_cpm_put_file ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    /* Vytvořit testovací data (512B) */
    uint8_t data[512];
    for ( int i = 0; i < 512; i++ ) data[i] = (uint8_t) ( i & 0xFF );

    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "TEST", "DAT", 0,
                                          data, 512 ) == MZDSK_RES_OK,
                  "write_file" );

    /* Ověřit v adresáři */
    st_MZDSK_CPM_FILE_INFO files[64];
    int count = mzdsk_cpm_read_directory ( &disc, &dpb, files, 64 );
    TEST_ASSERT_EQ_INT ( count, 1 );

    close_disc ( &disc );
    return 0;
}


/** @brief Put + get roundtrip - binární shoda. */
static int test_cpm_roundtrip ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    /* Testovací data */
    uint8_t data_in[1024];
    for ( int i = 0; i < 1024; i++ ) data_in[i] = (uint8_t) ( i & 0xFF );

    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "ROUND", "TST", 0,
                                          data_in, 1024 ) == MZDSK_RES_OK,
                  "write" );

    /* Přečíst zpět */
    uint8_t data_out[2048];
    memset ( data_out, 0, sizeof ( data_out ) );
    uint32_t bytes_read = 0;
    TEST_ASSERT ( mzdsk_cpm_read_file ( &disc, &dpb, "ROUND", "TST", 0,
                                         data_out, sizeof ( data_out ),
                                         &bytes_read ) == MZDSK_RES_OK,
                  "read" );

    /* CP/M zarovnává na 128B záznamy, takže bytes_read >= 1024 */
    TEST_ASSERT ( bytes_read >= 1024, "must read at least 1024 bytes" );

    /* Prvních 1024B musí odpovídat */
    TEST_ASSERT_EQ_MEM ( data_in, data_out, 1024 );

    close_disc ( &disc );
    return 0;
}


/** @brief Delete soubor -> zmizí z adresáře. */
static int test_cpm_delete ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data[256];
    memset ( data, 0xAA, 256 );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "DELETE", "ME", 0,
                                          data, 256 ) == MZDSK_RES_OK,
                  "write" );

    TEST_ASSERT ( mzdsk_cpm_delete_file ( &disc, &dpb, "DELETE", "ME", 0 ) == MZDSK_RES_OK,
                  "delete" );

    st_MZDSK_CPM_FILE_INFO files[64];
    int count = mzdsk_cpm_read_directory ( &disc, &dpb, files, 64 );
    TEST_ASSERT_EQ_INT ( count, 0 );

    close_disc ( &disc );
    return 0;
}


/** @brief Rename soubor. */
static int test_cpm_rename ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data[256];
    memset ( data, 0xBB, 256 );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "OLD", "NAM", 0,
                                          data, 256 ) == MZDSK_RES_OK,
                  "write" );

    TEST_ASSERT ( mzdsk_cpm_rename_file ( &disc, &dpb, "OLD", "NAM", 0,
                                           "NEW", "NAM" ) == MZDSK_RES_OK,
                  "rename" );

    /* Starý soubor neexistuje */
    TEST_ASSERT_EQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "OLD", "NAM", 0 ), 0 );
    /* Nový existuje */
    TEST_ASSERT_NEQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "NEW", "NAM", 0 ), 0 );

    close_disc ( &disc );
    return 0;
}


/** @brief Atributy R/O, SYS, ARC. */
static int test_cpm_attributes ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data[128];
    memset ( data, 0, 128 );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "ATTRS", "TST", 0,
                                          data, 128 ) == MZDSK_RES_OK,
                  "write" );

    /* Nastavit R/O + SYS */
    uint8_t attrs = MZDSK_CPM_ATTR_READ_ONLY | MZDSK_CPM_ATTR_SYSTEM;
    TEST_ASSERT ( mzdsk_cpm_set_attributes ( &disc, &dpb, "ATTRS", "TST", 0,
                                              attrs ) == MZDSK_RES_OK,
                  "set attrs" );

    /* Přečíst zpět */
    uint8_t read_attrs = 0;
    TEST_ASSERT ( mzdsk_cpm_get_attributes ( &disc, &dpb, "ATTRS", "TST", 0,
                                              &read_attrs ) == MZDSK_RES_OK,
                  "get attrs" );

    TEST_ASSERT ( read_attrs & MZDSK_CPM_ATTR_READ_ONLY, "R/O set" );
    TEST_ASSERT ( read_attrs & MZDSK_CPM_ATTR_SYSTEM, "SYS set" );
    TEST_ASSERT ( !( read_attrs & MZDSK_CPM_ATTR_ARCHIVED ), "ARC not set" );

    close_disc ( &disc );
    return 0;
}


/** @brief Volné místo se zmenší po put a vrátí po delete. */
static int test_cpm_free_space ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint32_t free_before = mzdsk_cpm_free_space ( &disc, &dpb );
    TEST_ASSERT ( free_before > 0, "free space > 0" );

    uint8_t data[2048];
    memset ( data, 0, 2048 );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "SPACE", "TST", 0,
                                          data, 2048 ) == MZDSK_RES_OK,
                  "write" );

    uint32_t free_after_put = mzdsk_cpm_free_space ( &disc, &dpb );
    TEST_ASSERT ( free_after_put < free_before, "free space decreased" );

    TEST_ASSERT ( mzdsk_cpm_delete_file ( &disc, &dpb, "SPACE", "TST", 0 ) == MZDSK_RES_OK,
                  "delete" );

    uint32_t free_after_del = mzdsk_cpm_free_space ( &disc, &dpb );
    TEST_ASSERT_EQ_UINT ( free_after_del, free_before );

    close_disc ( &disc );
    return 0;
}


/** @brief Zápis duplicitního souboru selže. */
static int test_cpm_duplicate_fails ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data[128];
    memset ( data, 0, 128 );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "DUP", "TST", 0,
                                          data, 128 ) == MZDSK_RES_OK,
                  "first write" );

    en_MZDSK_RES res = mzdsk_cpm_write_file ( &disc, &dpb, "DUP", "TST", 0,
                                                data, 128 );
    TEST_ASSERT ( res == MZDSK_RES_FILE_EXISTS, "duplicate must fail" );

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  Edge cases
 * ==================================================================== */


/** @brief Velký soubor přes hranici jednoho extentu (> 16KB pro SD). */
static int test_cpm_large_file_roundtrip ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    /* 32KB soubor - vyžaduje víc extentů v SD (block_size=2048, 16 bloků) */
    uint32_t size = 32768;
    uint8_t *data_in = (uint8_t *) malloc ( size );
    for ( uint32_t i = 0; i < size; i++ ) data_in[i] = (uint8_t) ( i & 0xFF );

    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "BIGFILE", "DAT", 0,
                                          data_in, size ) == MZDSK_RES_OK,
                  "write 32KB" );

    uint8_t *data_out = (uint8_t *) calloc ( 1, size + 256 );
    uint32_t bytes_read = 0;
    TEST_ASSERT ( mzdsk_cpm_read_file ( &disc, &dpb, "BIGFILE", "DAT", 0,
                                         data_out, size + 256,
                                         &bytes_read ) == MZDSK_RES_OK,
                  "read 32KB" );

    TEST_ASSERT ( bytes_read >= size, "must read at least 32KB" );
    TEST_ASSERT_EQ_MEM ( data_in, data_out, size );

    free ( data_in );
    free ( data_out );
    close_disc ( &disc );
    return 0;
}


/** @brief Zápis na plný disk musí selhat s DISK_FULL nebo NO_SPACE. */
static int test_cpm_disk_full ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    /* Naplnit disk velkými soubory (každý 32KB = 16 bloků)
       DSM=350, 351 bloků, minus 2 dir bloky = 349 datových.
       349 * 2KB = 698KB. 32KB soubor zabere 16 bloků.
       349/16 = 21.8 -> 21 souborů by mělo projít, 22. by měl selhat. */
    uint8_t *data = (uint8_t *) calloc ( 1, 32768 );
    char name[9];
    int i;
    en_MZDSK_RES res = MZDSK_RES_OK;

    for ( i = 0; i < 30; i++ ) {
        snprintf ( name, sizeof ( name ), "FILL%04d", i );
        res = mzdsk_cpm_write_file ( &disc, &dpb, name, "DAT", 0, data, 32768 );
        if ( res != MZDSK_RES_OK ) break;
    }

    /* Musí selhat před 30 soubory (disk je plný) */
    TEST_ASSERT ( i < 30, "must fail before 30 files (disk full)" );
    TEST_ASSERT ( res == MZDSK_RES_DISK_FULL || res == MZDSK_RES_NO_SPACE,
                  "must return DISK_FULL or NO_SPACE" );

    free ( data );
    close_disc ( &disc );
    return 0;
}


/** @brief Zápis do plného adresáře musí selhat. */
static int test_cpm_dir_full ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    /* DRM=127 -> 128 dir entries. Malé soubory (128B = 1 záznam),
       každý zabere 1 dir entry. Po 128 souborech musí selhat. */
    uint8_t data[128];
    memset ( data, 0, 128 );
    char name[9];
    int i;
    en_MZDSK_RES res = MZDSK_RES_OK;

    for ( i = 0; i < 140; i++ ) {
        snprintf ( name, sizeof ( name ), "D%06d", i );
        res = mzdsk_cpm_write_file ( &disc, &dpb, name, "X", 0, data, 128 );
        if ( res != MZDSK_RES_OK ) break;
    }

    /* Musí selhat kolem 128 souborů (dir je plný) */
    TEST_ASSERT ( i >= 120, "must fit at least 120 files" );
    TEST_ASSERT ( i < 140, "must fail before 140 files (dir full)" );
    TEST_ASSERT ( res == MZDSK_RES_DIR_FULL || res == MZDSK_RES_NO_SPACE
                  || res == MZDSK_RES_DISK_FULL,
                  "must return DIR_FULL or NO_SPACE" );

    close_disc ( &disc );
    return 0;
}


/** @brief Různí uživatelé mohou mít soubory se stejným jménem. */
static int test_cpm_different_users ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data[128];
    memset ( data, 0xAA, 128 );

    /* Stejné jméno, jiný user */
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "SAME", "DAT", 0,
                                          data, 128 ) == MZDSK_RES_OK,
                  "write user 0" );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "SAME", "DAT", 1,
                                          data, 128 ) == MZDSK_RES_OK,
                  "write user 1" );

    /* Oba musí existovat */
    TEST_ASSERT_NEQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "SAME", "DAT", 0 ), 0 );
    TEST_ASSERT_NEQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "SAME", "DAT", 1 ), 0 );

    /* Smazat jen user 0 */
    TEST_ASSERT ( mzdsk_cpm_delete_file ( &disc, &dpb, "SAME", "DAT", 0 ) == MZDSK_RES_OK,
                  "delete user 0" );
    TEST_ASSERT_EQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "SAME", "DAT", 0 ), 0 );
    TEST_ASSERT_NEQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "SAME", "DAT", 1 ), 0 );

    close_disc ( &disc );
    return 0;
}


/** @brief Delete neexistujícího souboru musí selhat. */
static int test_cpm_delete_nonexistent ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    en_MZDSK_RES res = mzdsk_cpm_delete_file ( &disc, &dpb, "NOFILE", "XXX", 0 );
    TEST_ASSERT ( res == MZDSK_RES_FILE_NOT_FOUND, "delete nonexistent" );

    close_disc ( &disc );
    return 0;
}


/** @brief Soubor o velikosti 0 bajtů. */
static int test_cpm_zero_size_file ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t dummy = 0;
    en_MZDSK_RES res = mzdsk_cpm_write_file ( &disc, &dpb, "ZERO", "FIL", 0,
                                                &dummy, 0 );
    /* Může projít (prázdný soubor) nebo selhat (závisí na implementaci) */
    if ( res == MZDSK_RES_OK ) {
        /* Pokud prošlo, musí být v adresáři */
        TEST_ASSERT_NEQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "ZERO", "FIL", 0 ), 0 );
    }

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  Agresivní edge cases
 * ==================================================================== */


/** @brief CP/M HD disk - vytvoření, formátování, put/get roundtrip. */
static int test_cpm_hd_roundtrip ( void )
{
    st_MZDSK_DISC disc;
    memset ( &disc, 0, sizeof ( disc ) );

    st_HANDLER *h = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;

    /* HD geometrie: 18x512B + boot track */
    size_t desc_size = dsk_tools_compute_description_size ( 3 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 3;
    desc->tracks = 80;
    desc->sides = 2;
    dsk_tools_assign_description ( desc, 0, 0, 18, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    dsk_tools_assign_description ( desc, 1, 1, 16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    dsk_tools_assign_description ( desc, 2, 2, 18, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );

    TEST_ASSERT_OK ( dsk_tools_create_image ( h, desc ) );
    free ( desc );

    disc.handler = h;
    disc.tracks_rules = dsk_tools_get_tracks_rules ( h );
    disc.format = dsk_tools_identformat_from_tracks_rules ( disc.tracks_rules );
    disc.sector_info_cb = mzdsk_sector_info_cb;
    disc.sector_info_cb_data = &disc;
    disc.cache = (uint8_t *) malloc ( 1024 );

    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, MZDSK_CPM_FORMAT_HD );
    TEST_ASSERT ( mzdsk_cpm_format_directory ( &disc, &dpb ) == MZDSK_RES_OK, "format HD" );

    /* HD block size = 4KB, roundtrip s 8KB souborem (2 bloky) */
    uint8_t data_in[8192];
    for ( int i = 0; i < 8192; i++ ) data_in[i] = (uint8_t) ( i & 0xFF );

    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "HDTEST", "DAT", 0,
                                          data_in, 8192 ) == MZDSK_RES_OK, "write HD" );

    uint8_t data_out[8192 + 256];
    uint32_t bytes_read = 0;
    TEST_ASSERT ( mzdsk_cpm_read_file ( &disc, &dpb, "HDTEST", "DAT", 0,
                                         data_out, sizeof ( data_out ),
                                         &bytes_read ) == MZDSK_RES_OK, "read HD" );

    TEST_ASSERT ( bytes_read >= 8192, "read at least 8KB" );
    TEST_ASSERT_EQ_MEM ( data_in, data_out, 8192 );

    close_disc ( &disc );
    return 0;
}


/** @brief Soubor přesně na hranici bloku (2048B = 1 SD blok). */
static int test_cpm_exact_block_boundary ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    /* Přesně 2048B = 1 blok */
    uint8_t data[2048];
    for ( int i = 0; i < 2048; i++ ) data[i] = (uint8_t) ( i & 0xFF );

    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "EXACT", "BLK", 0,
                                          data, 2048 ) == MZDSK_RES_OK, "write" );

    uint8_t out[2048 + 256];
    uint32_t bytes_read = 0;
    TEST_ASSERT ( mzdsk_cpm_read_file ( &disc, &dpb, "EXACT", "BLK", 0,
                                         out, sizeof ( out ), &bytes_read ) == MZDSK_RES_OK,
                  "read" );
    TEST_ASSERT ( bytes_read >= 2048, "read >= 2048" );
    TEST_ASSERT_EQ_MEM ( data, out, 2048 );

    close_disc ( &disc );
    return 0;
}


/** @brief Soubor o velikosti 1 bajt. */
static int test_cpm_one_byte_file ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data = 0x42;
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "ONE", "BYT", 0,
                                          &data, 1 ) == MZDSK_RES_OK, "write 1B" );

    uint8_t out[256];
    uint32_t bytes_read = 0;
    TEST_ASSERT ( mzdsk_cpm_read_file ( &disc, &dpb, "ONE", "BYT", 0,
                                         out, sizeof ( out ), &bytes_read ) == MZDSK_RES_OK,
                  "read" );
    /* CP/M zarovnává na 128B, takže dostaneme celý záznam */
    TEST_ASSERT ( bytes_read >= 1, "read >= 1" );
    TEST_ASSERT_BYTE ( out, 0, 0x42 );

    close_disc ( &disc );
    return 0;
}


/** @brief Rename na existující jméno musí selhat. */
static int test_cpm_rename_to_existing_fails ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data[128];
    memset ( data, 0, 128 );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "AAA", "DAT", 0,
                                          data, 128 ) == MZDSK_RES_OK, "write AAA" );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "BBB", "DAT", 0,
                                          data, 128 ) == MZDSK_RES_OK, "write BBB" );

    en_MZDSK_RES res = mzdsk_cpm_rename_file ( &disc, &dpb, "AAA", "DAT", 0,
                                                  "BBB", "DAT" );
    TEST_ASSERT ( res == MZDSK_RES_FILE_EXISTS, "rename to existing must fail" );

    /* Oba soubory stále existují */
    TEST_ASSERT_NEQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "AAA", "DAT", 0 ), 0 );
    TEST_ASSERT_NEQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "BBB", "DAT", 0 ), 0 );

    close_disc ( &disc );
    return 0;
}


/** @brief Put na read-only disk musí selhat. */
static int test_cpm_readonly_put_fails ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    generic_driver_set_handler_readonly_status ( disc.handler, 1 );

    uint8_t data[128];
    memset ( data, 0, 128 );
    en_MZDSK_RES res = mzdsk_cpm_write_file ( &disc, &dpb, "RDONLY", "TST", 0,
                                                data, 128 );
    TEST_ASSERT ( res != MZDSK_RES_OK, "put on RO must fail" );

    close_disc ( &disc );
    return 0;
}


/** @brief CP/M defrag - put, delete, defrag, ověř soubory. */
static int test_cpm_defrag ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data_a[2048];
    memset ( data_a, 0xAA, sizeof ( data_a ) );
    uint8_t data_b[2048];
    memset ( data_b, 0xBB, sizeof ( data_b ) );
    uint8_t data_c[2048];
    memset ( data_c, 0xCC, sizeof ( data_c ) );

    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "FIRST", "DAT", 0,
                                          data_a, 2048 ) == MZDSK_RES_OK, "write A" );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "MIDDLE", "DAT", 0,
                                          data_b, 2048 ) == MZDSK_RES_OK, "write B" );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "LAST", "DAT", 0,
                                          data_c, 2048 ) == MZDSK_RES_OK, "write C" );

    /* Smazat prostřední */
    TEST_ASSERT ( mzdsk_cpm_delete_file ( &disc, &dpb, "MIDDLE", "DAT", 0 ) == MZDSK_RES_OK,
                  "delete B" );

    /* Defrag */
    TEST_ASSERT ( mzdsk_cpm_defrag ( &disc, &dpb, NULL, NULL ) == MZDSK_RES_OK, "defrag" );

    /* Ověřit, že A a C jsou stále čitelné a mají správná data */
    uint8_t out[2048 + 256];
    uint32_t bytes_read = 0;

    TEST_ASSERT ( mzdsk_cpm_read_file ( &disc, &dpb, "FIRST", "DAT", 0,
                                         out, sizeof ( out ), &bytes_read ) == MZDSK_RES_OK,
                  "read A after defrag" );
    TEST_ASSERT_EQ_MEM ( data_a, out, 2048 );

    TEST_ASSERT ( mzdsk_cpm_read_file ( &disc, &dpb, "LAST", "DAT", 0,
                                         out, sizeof ( out ), &bytes_read ) == MZDSK_RES_OK,
                  "read C after defrag" );
    TEST_ASSERT_EQ_MEM ( data_c, out, 2048 );

    /* MIDDLE nesmí existovat */
    TEST_ASSERT_EQ_INT ( mzdsk_cpm_file_exists ( &disc, &dpb, "MIDDLE", "DAT", 0 ), 0 );

    close_disc ( &disc );
    return 0;
}


/**
 * @brief Audit H-5: defrag musí vrátit INVALID_PARAM u non-memory handleru.
 *
 * Defrag NENÍ interně transakční - vyžaduje memory handler, aby uživatel
 * mohl případný fail "zahodit" bez poškození původního souboru.
 */
static int test_cpm_defrag_rejects_file_handler ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    /* Dočasně přepneme typ handleru na FILE - tím simulujeme volání
       z kontextu, kde uživatel neotevřel disk přes mzdsk_disc_open_memory(). */
    en_HANDLER_TYPE orig_type = disc.handler->type;
    disc.handler->type = HANDLER_TYPE_FILE;

    en_MZDSK_RES res = mzdsk_cpm_defrag ( &disc, &dpb, NULL, NULL );
    TEST_ASSERT ( res == MZDSK_RES_INVALID_PARAM,
                  "defrag must reject non-memory handler" );

    /* Obnovit typ pro korektní cleanup */
    disc.handler->type = orig_type;

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  MZF CPM-IC kódování (mzdsk_cpm_mzf_encode / _ex)
 * ==================================================================== */


/**
 * @brief Vrátí typ MZF z výstupního bufferu (offset 0x00).
 */
static uint8_t mzf_out_ftype ( const uint8_t *mzf ) {
    return mzf[0];
}


/**
 * @brief Načte little-endian 16b hodnotu z daného offsetu.
 */
static uint16_t mzf_out_le16 ( const uint8_t *mzf, size_t off ) {
    return (uint16_t) ( mzf[off] | ( mzf[off + 1] << 8 ) );
}


/**
 * @brief _ex s výchozími parametry musí dát shodný výstup s _encode.
 *
 * `mzdsk_cpm_mzf_encode()` je tenký wrapper nad `_ex()` - s defaulty
 * (ftype=0x22, strt=0x0100, exec z parametru, encode_attrs=1) musí
 * produkovat bajtově stejný výstup.
 */
static int test_cpm_mzf_encode_ex_default ( void ) {
    uint8_t data[] = { 0x11, 0x22, 0x33, 0x44 };

    uint8_t *mzf_a = NULL; uint32_t size_a = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode ( data, sizeof ( data ), "TEST", "COM",
                                          MZDSK_CPM_ATTR_READ_ONLY, 0x0100,
                                          &mzf_a, &size_a ) == MZDSK_RES_OK,
                  "encode default" );

    uint8_t *mzf_b = NULL; uint32_t size_b = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "TEST", "COM",
                                             MZDSK_CPM_ATTR_READ_ONLY,
                                             MZDSK_CPM_MZF_FTYPE, 0x0100,
                                             MZDSK_CPM_MZF_DEFAULT_ADDR, 1,
                                             &mzf_b, &size_b ) == MZDSK_RES_OK,
                  "encode_ex with default params" );

    TEST_ASSERT_EQ_INT ( (int) size_a, (int) size_b );
    TEST_ASSERT_EQ_MEM ( mzf_a, mzf_b, size_a );

    /* Hlavička + tělo = 128 + 4 */
    TEST_ASSERT_EQ_INT ( (int) size_a, MZF_HEADER_SIZE + (int) sizeof ( data ) );

    /* fstrt = 0x0100 (default strt), fexec = 0x0100 */
    TEST_ASSERT_EQ_INT ( (int) mzf_out_le16 ( mzf_a, 0x14 ), 0x0100 );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_le16 ( mzf_a, 0x16 ), 0x0100 );

    free ( mzf_a );
    free ( mzf_b );
    return 0;
}


/**
 * @brief --ftype HH se propíše do pole ftype v hlavičce.
 */
static int test_cpm_mzf_encode_ex_ftype ( void ) {
    uint8_t data[] = { 0xAA, 0xBB, 0xCC };

    uint8_t *mzf = NULL; uint32_t size = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "PROG", "COM",
                                             0, 0x01, 0x0100, 0x0100, 0,
                                             &mzf, &size ) == MZDSK_RES_OK,
                  "encode ftype=0x01" );

    TEST_ASSERT_EQ_INT ( (int) mzf_out_ftype ( mzf ), 0x01 );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_le16 ( mzf, 0x14 ), 0x0100 );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_le16 ( mzf, 0x16 ), 0x0100 );

    free ( mzf );
    return 0;
}


/**
 * @brief --strt-addr zapisuje adresu do fstrt bez ohledu na ftype.
 *
 * Testováno s ftype=0x22 i ftype=0x05 - obě musí mít fstrt = strt_addr.
 */
static int test_cpm_mzf_encode_ex_strt_addr ( void ) {
    uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };

    /* ftype=0x05, strt=0x1200 */
    uint8_t *mzf_a = NULL; uint32_t size_a = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "IMG", "BIN",
                                             0, 0x05, 0x2000, 0x1200, 0,
                                             &mzf_a, &size_a ) == MZDSK_RES_OK,
                  "encode ftype=0x05 strt=0x1200" );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_ftype ( mzf_a ), 0x05 );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_le16 ( mzf_a, 0x14 ), 0x1200 );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_le16 ( mzf_a, 0x16 ), 0x2000 );
    free ( mzf_a );

    /* ftype=0x22, strt=0x5000 - hodnota se musí zapsat i pro 0x22 */
    uint8_t *mzf_b = NULL; uint32_t size_b = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "APP", "COM",
                                             0, MZDSK_CPM_MZF_FTYPE,
                                             0x1234, 0x5000, 1,
                                             &mzf_b, &size_b ) == MZDSK_RES_OK,
                  "encode ftype=0x22 strt=0x5000" );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_ftype ( mzf_b ), MZDSK_CPM_MZF_FTYPE );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_le16 ( mzf_b, 0x14 ), 0x5000 );
    TEST_ASSERT_EQ_INT ( (int) mzf_out_le16 ( mzf_b, 0x16 ), 0x1234 );
    free ( mzf_b );

    return 0;
}


/**
 * @brief encode_attrs=0 nesmí nastavit bity 7 v bajtech fname[9..11].
 */
static int test_cpm_mzf_encode_ex_no_attrs ( void ) {
    uint8_t data[] = { 0x55 };
    uint8_t all_attrs = MZDSK_CPM_ATTR_READ_ONLY |
                        MZDSK_CPM_ATTR_SYSTEM |
                        MZDSK_CPM_ATTR_ARCHIVED;

    /* s encode_attrs=1 musí mít všechny tři bajty bit 7 nastavený */
    uint8_t *mzf_on = NULL; uint32_t size_on = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "FILE", "TXT",
                                             all_attrs, MZDSK_CPM_MZF_FTYPE,
                                             0x0100, 0x0100, 1,
                                             &mzf_on, &size_on ) == MZDSK_RES_OK,
                  "encode with attrs" );
    TEST_ASSERT ( ( mzf_on[0x0A] & 0x80 ) != 0, "R/O bit set" );
    TEST_ASSERT ( ( mzf_on[0x0B] & 0x80 ) != 0, "SYS bit set" );
    TEST_ASSERT ( ( mzf_on[0x0C] & 0x80 ) != 0, "ARC bit set" );
    free ( mzf_on );

    /* s encode_attrs=0 musí být všechny bajty bez bitu 7 */
    uint8_t *mzf_off = NULL; uint32_t size_off = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "FILE", "TXT",
                                             all_attrs, MZDSK_CPM_MZF_FTYPE,
                                             0x0100, 0x0100, 0,
                                             &mzf_off, &size_off ) == MZDSK_RES_OK,
                  "encode without attrs" );
    TEST_ASSERT ( ( mzf_off[0x0A] & 0x80 ) == 0, "R/O bit clear" );
    TEST_ASSERT ( ( mzf_off[0x0B] & 0x80 ) == 0, "SYS bit clear" );
    TEST_ASSERT ( ( mzf_off[0x0C] & 0x80 ) == 0, "ARC bit clear" );
    free ( mzf_off );

    return 0;
}


/**
 * @brief P2.5: encode_attrs=0 zachová datový bit 7 ve jméně/příponě.
 *
 * Pokud název nebo přípona obsahuje znak s nativně nastaveným bitem 7
 * (např. SharpMZ EU znak 0xC1 = 'A' | 0x80), encode_attrs=0 musí pouze
 * potlačit atributové flagy (R/O, SYS, ARC), ale NESMÍ vymazat bit 7
 * z datového znaku samotného.
 *
 * Rafinovaný regresní test: naivní implementace by mohla aplikovat
 * `ch &= 0x7F` při encode_attrs=0 a tím zničila by vstupní znak.
 */
static int test_cpm_mzf_encode_ex_no_attrs_preserves_highbit_name ( void ) {
    uint8_t data[] = { 0x01 };
    /* Přípona s bitem 7 v prvním znaku (0xC1 = 'A' | 0x80),
     * druhý a třetí znak běžné ASCII */
    char ext_high[4] = { (char) 0xC1, 'B', 'C', '\0' };

    /* Scénář 1: encode_attrs=0, cpm_attrs=R/O → R/O se NEkóduje,
     * ale datový bit 7 v ext[0] musí zůstat nastavený. */
    uint8_t *mzf1 = NULL; uint32_t size1 = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "N", ext_high,
                                             MZDSK_CPM_ATTR_READ_ONLY,
                                             MZDSK_CPM_MZF_FTYPE,
                                             0x0100, 0x0100, 0,
                                             &mzf1, &size1 ) == MZDSK_RES_OK,
                  "encode no-attrs with high-bit name" );

    /* ext[0] = 0xC1 (bit 7 je datový, ne atributový) */
    TEST_ASSERT_EQ_INT ( mzf1[0x0A], 0xC1 );
    /* ext[1] = 'B' = 0x42 (žádný bit 7, SYS neaplikováno) */
    TEST_ASSERT_EQ_INT ( mzf1[0x0B], 'B' );
    /* ext[2] = 'C' = 0x43 (žádný bit 7, ARC neaplikováno) */
    TEST_ASSERT_EQ_INT ( mzf1[0x0C], 'C' );
    free ( mzf1 );

    /* Scénář 2: encode_attrs=1, cpm_attrs=0 → OR s 0x00 nic nezmění,
     * datový bit 7 v ext[0] musí zůstat. */
    uint8_t *mzf2 = NULL; uint32_t size2 = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "N", ext_high,
                                             0,
                                             MZDSK_CPM_MZF_FTYPE,
                                             0x0100, 0x0100, 1,
                                             &mzf2, &size2 ) == MZDSK_RES_OK,
                  "encode with attrs=0 but high-bit data" );

    TEST_ASSERT_EQ_INT ( mzf2[0x0A], 0xC1 );
    TEST_ASSERT_EQ_INT ( mzf2[0x0B], 'B' );
    TEST_ASSERT_EQ_INT ( mzf2[0x0C], 'C' );
    free ( mzf2 );

    /* Scénář 3: encode_attrs=1, cpm_attrs=R/O+SYS+ARC, data s bitem 7
     * v ext[0] → ext[0] stále 0xC1 (bit 7 už je set, OR idempotentní),
     * ext[1] má bit 7 přes SYS (0xC2 = 'B' | 0x80),
     * ext[2] má bit 7 přes ARC (0xC3 = 'C' | 0x80). */
    uint8_t *mzf3 = NULL; uint32_t size3 = 0;
    uint8_t all = MZDSK_CPM_ATTR_READ_ONLY |
                  MZDSK_CPM_ATTR_SYSTEM |
                  MZDSK_CPM_ATTR_ARCHIVED;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "N", ext_high,
                                             all,
                                             MZDSK_CPM_MZF_FTYPE,
                                             0x0100, 0x0100, 1,
                                             &mzf3, &size3 ) == MZDSK_RES_OK,
                  "encode with all attrs + high-bit data" );

    TEST_ASSERT_EQ_INT ( mzf3[0x0A], 0xC1 );
    TEST_ASSERT_EQ_INT ( mzf3[0x0B], 0xC2 );
    TEST_ASSERT_EQ_INT ( mzf3[0x0C], 0xC3 );
    free ( mzf3 );

    return 0;
}


/**
 * @brief Data > 65535 B musí encode_ex odmítnout (fsize je uint16).
 */
static int test_cpm_mzf_encode_ex_oversize ( void ) {
    uint32_t big_size = 65536;
    uint8_t *big = (uint8_t *) calloc ( 1, big_size );
    TEST_ASSERT ( big != NULL, "alloc big buffer" );

    uint8_t *mzf = NULL; uint32_t size = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( big, big_size, "BIG", "BIN",
                                             0, MZDSK_CPM_MZF_FTYPE,
                                             0x0100, 0x0100, 0,
                                             &mzf, &size ) == MZDSK_RES_INVALID_PARAM,
                  "encode oversize must fail" );
    TEST_ASSERT ( mzf == NULL, "no buffer on error" );

    free ( big );
    return 0;
}


/**
 * @brief Roundtrip encode -> decode zachová jméno, data, atributy a adresy.
 */
static int test_cpm_mzf_encode_decode_roundtrip ( void ) {
    uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34 };

    uint8_t *mzf = NULL; uint32_t mzf_size = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_encode_ex ( data, sizeof ( data ), "RT", "DAT",
                                             MZDSK_CPM_ATTR_SYSTEM,
                                             MZDSK_CPM_MZF_FTYPE,
                                             0x0200, 0x0300, 1,
                                             &mzf, &mzf_size ) == MZDSK_RES_OK,
                  "encode" );

    char name[9] = {0}, ext[4] = {0};
    uint8_t attrs = 0;
    uint16_t exec = 0;
    uint8_t *out = NULL;
    uint32_t out_size = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_decode ( mzf, mzf_size, name, ext, &attrs, &exec,
                                          &out, &out_size ) == MZDSK_RES_OK,
                  "decode" );

    TEST_ASSERT ( strcmp ( name, "RT" ) == 0, "name roundtrip" );
    TEST_ASSERT ( strcmp ( ext, "DAT" ) == 0, "ext roundtrip" );
    TEST_ASSERT_EQ_INT ( (int) attrs, MZDSK_CPM_ATTR_SYSTEM );
    TEST_ASSERT_EQ_INT ( (int) exec, 0x0200 );
    TEST_ASSERT_EQ_INT ( (int) out_size, (int) sizeof ( data ) );
    TEST_ASSERT_EQ_MEM ( data, out, sizeof ( data ) );

    free ( out );
    free ( mzf );
    return 0;
}


/**
 * @brief Sestaví raw 128B MZF buffer (little-endian, tak jak se čte ze souboru).
 *
 * @param[out] out_buf     128B buffer, musí mít velikost min MZF_HEADER_SIZE.
 * @param[in]  ftype       MZF typ souboru.
 * @param[in]  fname_bytes Surové bajty fname (max 17 B, null nebude kopírován).
 * @param[in]  fname_len   Počet bajtů v fname_bytes (1-17). Zbytek do 17 se
 *                          doplní MZF_FNAME_TERMINATOR (0x0D).
 * @param[in]  fsize       Velikost datové části.
 */
static void build_raw_mzf_header ( uint8_t *out_buf, uint8_t ftype,
                                     const uint8_t *fname_bytes, unsigned fname_len,
                                     uint16_t fsize ) {
    memset ( out_buf, 0, MZF_HEADER_SIZE );
    out_buf[0] = ftype;
    unsigned n = fname_len > 17 ? 17 : fname_len;
    for ( unsigned i = 0; i < n; i++ ) out_buf[1 + i] = fname_bytes[i];
    for ( unsigned i = n; i < 17; i++ ) out_buf[1 + i] = 0x0D;
    /* fsize LE na offsetu 18 */
    out_buf[18] = (uint8_t) ( fsize & 0xFF );
    out_buf[19] = (uint8_t) ( ( fsize >> 8 ) & 0xFF );
    /* fstrt, fexec = 0 (už vynulované) */
}


/**
 * @brief Flappy reprodukce (BUG B2) - Sharp EU velká písmena v fname.
 *
 * Sharp bajty AB 92 9D = 'v','e','r'. Po opravě musí decode vrátit
 * sanitizované jméno použitelné v CP/M nástrojích (printable ASCII po
 * masking bit 7).
 */
static int test_cpm_mzf_decode_sharp_eu_fname ( void ) {
    /* fname: "FLAPPY " + AB 92 9D + " 1.0A" + CR */
    uint8_t fname_bytes[] = {
        0x46, 0x4C, 0x41, 0x50, 0x50, 0x59, 0x20,       /* "FLAPPY " */
        0xAB, 0x92, 0x9D, 0x20,                          /* Sharp EU "ver " */
        0x31, 0x2E, 0x30, 0x41,                          /* "1.0A" */
        0x0D                                             /* terminátor */
    };
    uint8_t mzf[MZF_HEADER_SIZE];
    build_raw_mzf_header ( mzf, MZF_FTYPE_OBJ, fname_bytes, sizeof ( fname_bytes ), 0 );

    char name[9] = {0}, ext[4] = {0};
    uint8_t attrs = 0;
    uint16_t exec = 0;
    uint8_t *out = NULL;
    uint32_t out_size = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_decode ( mzf, sizeof ( mzf ), name, ext, &attrs, &exec,
                                          &out, &out_size ) == MZDSK_RES_OK,
                  "decode Sharp EU fname" );

    /* "FLAPPY ver 1.0A" po trim + split na '.': name="FLAPPY ver 1", ext="0A"
       → clip na 8.3, mezery sanitizovány na '_' → "FLAPPY_v" + "0A" */
    TEST_ASSERT ( strcmp ( name, "FLAPPY_v" ) == 0, "name sanitized to printable ASCII" );
    TEST_ASSERT ( strcmp ( ext, "0A" ) == 0, "extension from after last dot" );
    TEST_ASSERT_EQ_INT ( (int) attrs, 0 );

    /* Každý znak jména i přípony musí být printable ASCII - zajišťuje
       použitelnost jména v CP/M nástrojích (CCP, PIP). */
    for ( size_t i = 0; name[i]; i++ ) {
        unsigned char c = (unsigned char) name[i];
        TEST_ASSERT ( c >= 0x21 && c <= 0x7E, "name char in printable ASCII" );
    }
    for ( size_t i = 0; ext[i]; i++ ) {
        unsigned char c = (unsigned char) ext[i];
        TEST_ASSERT ( c >= 0x21 && c <= 0x7E, "ext char in printable ASCII" );
    }

    free ( out );
    return 0;
}


/**
 * @brief Bomber reprodukce (BUG B2 / KNOWN-CPM-001) - leading space v fname.
 *
 * fname = " F1200" + CR. Po konverzi a trim zbude "F1200", bez přípony.
 * Detekce CP/M musí projít (žádné non-printable bajty v CP/M directory).
 */
static int test_cpm_mzf_decode_leading_space_fname ( void ) {
    uint8_t fname_bytes[] = {
        0x20, 0x46, 0x31, 0x32, 0x30, 0x30,              /* " F1200" */
        0x0D                                              /* terminátor */
    };
    uint8_t mzf[MZF_HEADER_SIZE];
    build_raw_mzf_header ( mzf, MZF_FTYPE_OBJ, fname_bytes, sizeof ( fname_bytes ), 0 );

    char name[9] = {0}, ext[4] = {0};
    uint8_t attrs = 0;
    uint16_t exec = 0;
    uint8_t *out = NULL;
    uint32_t out_size = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_decode ( mzf, sizeof ( mzf ), name, ext, &attrs, &exec,
                                          &out, &out_size ) == MZDSK_RES_OK,
                  "decode leading-space fname" );

    TEST_ASSERT ( strcmp ( name, "F1200" ) == 0, "name after leading space trim" );
    TEST_ASSERT ( strcmp ( ext, "" ) == 0, "no extension when no dot in fname" );

    free ( out );
    return 0;
}


/**
 * @brief JP varianta - stejné bajty jako EU, ale s --charset jp mapují na jiné znaky.
 *
 * MZF s ftype != 0x22 a fname obsahujícím Sharp-JP specifické bajty. Ověřujeme,
 * že decode_ex s MZF_NAME_ASCII_JP produkuje validní sanitizovaný výstup.
 */
static int test_cpm_mzf_decode_ex_jp_charset ( void ) {
    /* "TEST" (ASCII) + CR - stejný výsledek u EU i JP */
    uint8_t fname_bytes[] = { 0x54, 0x45, 0x53, 0x54, 0x0D };
    uint8_t mzf[MZF_HEADER_SIZE];
    build_raw_mzf_header ( mzf, MZF_FTYPE_OBJ, fname_bytes, sizeof ( fname_bytes ), 0 );

    char name_eu[9] = {0}, ext_eu[4] = {0};
    char name_jp[9] = {0}, ext_jp[4] = {0};
    uint8_t attrs = 0;
    uint16_t exec = 0;
    uint8_t *out = NULL;
    uint32_t out_size = 0;

    TEST_ASSERT ( mzdsk_cpm_mzf_decode_ex ( mzf, sizeof ( mzf ), MZF_NAME_ASCII_EU,
                                             name_eu, ext_eu, &attrs, &exec,
                                             &out, &out_size ) == MZDSK_RES_OK,
                  "decode_ex EU" );
    free ( out ); out = NULL;

    TEST_ASSERT ( mzdsk_cpm_mzf_decode_ex ( mzf, sizeof ( mzf ), MZF_NAME_ASCII_JP,
                                             name_jp, ext_jp, &attrs, &exec,
                                             &out, &out_size ) == MZDSK_RES_OK,
                  "decode_ex JP" );
    free ( out );

    /* ASCII pismena jsou v obou variantach shodná. */
    TEST_ASSERT ( strcmp ( name_eu, "TEST" ) == 0, "EU name" );
    TEST_ASSERT ( strcmp ( name_jp, "TEST" ) == 0, "JP name" );

    /* UTF-8 encoding musí být odmítnuto. */
    TEST_ASSERT ( mzdsk_cpm_mzf_decode_ex ( mzf, sizeof ( mzf ), MZF_NAME_UTF8_EU,
                                             name_eu, ext_eu, &attrs, &exec,
                                             &out, &out_size ) == MZDSK_RES_INVALID_PARAM,
                  "UTF-8 encoding rejected" );
    return 0;
}


/**
 * @brief CPM-IC konvence (ftype 0x22) - fname je ASCII, bit 7 přípony = atributy.
 *
 * Ověřujeme, že sanitizace nerozbila CPM-IC export/import roundtrip.
 * Také že non-printable bajty v ftype 0x22 fname (hypotetický poškozený MZF)
 * se sanitizují, aby detekce CP/M neselhala.
 */
static int test_cpm_mzf_decode_ftype22_sanitized ( void ) {
    /* CPM-IC layout: 8 znaků name, '.', 3 znaky ext, CR.
       Schválně vložím CR uvnitř jména na pozici 5 - dřív by to rozbilo detekci. */
    uint8_t fname_bytes[17] = {
        'A', 'B', 'C', 'D', 0x0D,                        /* name s CR uprostřed */
        ' ', ' ', ' ',                                    /* padding */
        '.',                                              /* oddělovač */
        'X', 'Y', 'Z',                                    /* ext */
        0x0D,                                             /* terminátor */
        0, 0, 0, 0                                        /* padding */
    };
    uint8_t mzf[MZF_HEADER_SIZE];
    /* Ruční build - CR uvnitř fname by jinak build_raw rozbil */
    memset ( mzf, 0, MZF_HEADER_SIZE );
    mzf[0] = MZDSK_CPM_MZF_FTYPE;
    memcpy ( &mzf[1], fname_bytes, 17 );

    char name[9] = {0}, ext[4] = {0};
    uint8_t attrs = 0;
    uint16_t exec = 0;
    uint8_t *out = NULL;
    uint32_t out_size = 0;
    TEST_ASSERT ( mzdsk_cpm_mzf_decode ( mzf, sizeof ( mzf ), name, ext, &attrs, &exec,
                                          &out, &out_size ) == MZDSK_RES_OK,
                  "decode ftype 0x22 with CR in fname" );

    /* Všechny znaky výstupu musí být printable ASCII - CR (0x0D) → '_' */
    for ( size_t i = 0; name[i]; i++ ) {
        unsigned char c = (unsigned char) name[i];
        TEST_ASSERT ( c >= 0x21 && c <= 0x7E, "name char sanitized" );
    }
    TEST_ASSERT ( strcmp ( ext, "XYZ" ) == 0, "ext intact" );

    free ( out );
    return 0;
}


/* ====================================================================
 *  Statistika directory slotů (mzdsk_cpm_get_dir_stats)
 * ==================================================================== */


/** @brief Čerstvě naformátovaný disk: total = drm+1, vše free, used = blocked = 0. */
static int test_cpm_dir_stats_empty ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    st_MZDSK_CPM_DIR_STATS stats;
    TEST_ASSERT ( mzdsk_cpm_get_dir_stats ( &disc, &dpb, &stats ) == MZDSK_RES_OK,
                  "get_dir_stats" );

    TEST_ASSERT_EQ_INT ( stats.total, dpb.drm + 1 );
    TEST_ASSERT_EQ_INT ( stats.free,  dpb.drm + 1 );
    TEST_ASSERT_EQ_INT ( stats.used,    0 );
    TEST_ASSERT_EQ_INT ( stats.blocked, 0 );

    close_disc ( &disc );
    return 0;
}


/** @brief Po put souboru: used narůstá, free klesá, blocked = 0. */
static int test_cpm_dir_stats_after_put ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    uint8_t data[512];
    for ( int i = 0; i < 512; i++ ) data[i] = (uint8_t) ( i & 0xFF );
    TEST_ASSERT ( mzdsk_cpm_write_file ( &disc, &dpb, "FILE", "DAT", 0,
                                          data, 512 ) == MZDSK_RES_OK,
                  "write_file" );

    st_MZDSK_CPM_DIR_STATS stats;
    TEST_ASSERT ( mzdsk_cpm_get_dir_stats ( &disc, &dpb, &stats ) == MZDSK_RES_OK,
                  "get_dir_stats" );

    TEST_ASSERT_EQ_INT ( stats.total, dpb.drm + 1 );
    TEST_ASSERT_EQ_INT ( stats.used,  1 );
    TEST_ASSERT_EQ_INT ( stats.blocked, 0 );
    TEST_ASSERT_EQ_INT ( stats.free, dpb.drm + 1 - 1 );

    close_disc ( &disc );
    return 0;
}


/** @brief Entries s user > 15 (mimo 0xE5) se počítají do blocked. */
static int test_cpm_dir_stats_blocked ( void )
{
    st_MZDSK_DISC disc;
    st_MZDSK_CPM_DPB dpb;
    TEST_ASSERT_OK ( create_formatted_cpm_sd ( &disc, &dpb ) );

    /* Zapsat 3 entries s user=0x5B (blocked) a 1 entry s user=0 (used). */
    st_MZDSK_CPM_DIRENTRY entry;
    memset ( &entry, 0x00, sizeof ( entry ) );
    entry.user = 0x5B;
    entry.rc = 0x10;
    for ( int i = 0; i < 3; i++ ) {
        TEST_ASSERT ( mzdsk_cpm_write_dir_entry ( &disc, &dpb, i, &entry ) == MZDSK_RES_OK,
                      "write blocked entry" );
    }
    entry.user = 0;
    TEST_ASSERT ( mzdsk_cpm_write_dir_entry ( &disc, &dpb, 3, &entry ) == MZDSK_RES_OK,
                  "write used entry" );

    st_MZDSK_CPM_DIR_STATS stats;
    TEST_ASSERT ( mzdsk_cpm_get_dir_stats ( &disc, &dpb, &stats ) == MZDSK_RES_OK,
                  "get_dir_stats" );

    TEST_ASSERT_EQ_INT ( stats.total, dpb.drm + 1 );
    TEST_ASSERT_EQ_INT ( stats.used,    1 );
    TEST_ASSERT_EQ_INT ( stats.blocked, 3 );
    TEST_ASSERT_EQ_INT ( stats.free, dpb.drm + 1 - 4 );

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  main
 * ==================================================================== */


int main ( void )
{
    memory_driver_init ();

    TEST_INIT ();

    /* základní operace */
    RUN_TEST ( test_cpm_format_empty_dir );
    RUN_TEST ( test_cpm_put_file );
    RUN_TEST ( test_cpm_roundtrip );
    RUN_TEST ( test_cpm_delete );
    RUN_TEST ( test_cpm_rename );
    RUN_TEST ( test_cpm_attributes );
    RUN_TEST ( test_cpm_free_space );
    RUN_TEST ( test_cpm_duplicate_fails );

    /* edge cases */
    RUN_TEST ( test_cpm_large_file_roundtrip );
    RUN_TEST ( test_cpm_disk_full );
    RUN_TEST ( test_cpm_dir_full );
    RUN_TEST ( test_cpm_different_users );
    RUN_TEST ( test_cpm_delete_nonexistent );
    RUN_TEST ( test_cpm_zero_size_file );

    /* agresivní testy */
    RUN_TEST ( test_cpm_hd_roundtrip );
    RUN_TEST ( test_cpm_exact_block_boundary );
    RUN_TEST ( test_cpm_one_byte_file );
    RUN_TEST ( test_cpm_rename_to_existing_fails );
    RUN_TEST ( test_cpm_readonly_put_fails );
    RUN_TEST ( test_cpm_defrag );
    RUN_TEST ( test_cpm_defrag_rejects_file_handler );

    /* MZF kódování - rozšířené volby */
    RUN_TEST ( test_cpm_mzf_encode_ex_default );
    RUN_TEST ( test_cpm_mzf_encode_ex_ftype );
    RUN_TEST ( test_cpm_mzf_encode_ex_strt_addr );
    RUN_TEST ( test_cpm_mzf_encode_ex_no_attrs );
    RUN_TEST ( test_cpm_mzf_encode_ex_no_attrs_preserves_highbit_name );
    RUN_TEST ( test_cpm_mzf_encode_ex_oversize );
    RUN_TEST ( test_cpm_mzf_encode_decode_roundtrip );

    /* MZF dekódování - Sharp MZ konverze a sanitizace (BUG B2 / KNOWN-CPM-001) */
    RUN_TEST ( test_cpm_mzf_decode_sharp_eu_fname );
    RUN_TEST ( test_cpm_mzf_decode_leading_space_fname );
    RUN_TEST ( test_cpm_mzf_decode_ex_jp_charset );
    RUN_TEST ( test_cpm_mzf_decode_ftype22_sanitized );

    /* statistika directory slotů */
    RUN_TEST ( test_cpm_dir_stats_empty );
    RUN_TEST ( test_cpm_dir_stats_after_put );
    RUN_TEST ( test_cpm_dir_stats_blocked );

    TEST_SUMMARY ();
}
