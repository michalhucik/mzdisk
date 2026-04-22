/**
 * @file test_mrs.c
 * @brief Testy MRS filesystem operací.
 *
 * Vytváří MRS disky od nuly, inicializuje FAT/DIR a testuje
 * souborové operace (put, get, delete, rename, disk full).
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
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"


/* ====================================================================
 *  Pomocné funkce
 * ==================================================================== */

/**
 * @brief Vytvoří MRS disk s inicializovanou FAT a adresářem.
 *
 * Geometrie: CP/M-like (boot track + 9x512B), 80T, 2 sides.
 * FAT blok 36 (standardní MRS pozice).
 */
static int create_formatted_mrs ( st_MZDSK_DISC *disc, st_FSMRS_CONFIG *config )
{
    memset ( disc, 0, sizeof ( st_MZDSK_DISC ) );

    st_HANDLER *h = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    if ( !h ) return EXIT_FAILURE;

    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;

    /* CP/M-like geometrie s boot trackem (MRS potřebuje 9x512B + boot) */
    size_t desc_size = dsk_tools_compute_description_size ( 3 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) { free ( h ); return EXIT_FAILURE; }

    memset ( desc, 0, desc_size );
    desc->count_rules = 3;
    desc->tracks = 80;
    desc->sides = 2;
    dsk_tools_assign_description ( desc, 0, 0, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0x00 );
    dsk_tools_assign_description ( desc, 1, 1, 16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0x00 );
    dsk_tools_assign_description ( desc, 2, 2, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0x00 );

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

    /* Inicializovat MRS FAT a adresář */
    en_MZDSK_RES mres = fsmrs_format_fs ( disc, MZDSK_DETECT_MRS_DEFAULT_FAT_BLOCK );
    if ( mres != MZDSK_RES_OK ) return EXIT_FAILURE;

    /* Načíst config */
    mres = fsmrs_init ( disc, MZDSK_DETECT_MRS_DEFAULT_FAT_BLOCK, config );
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
        if ( disc->handler->spec.memspec.ptr )
            free ( disc->handler->spec.memspec.ptr );
        free ( disc->handler );
        disc->handler = NULL;
    }
}


/* ====================================================================
 *  Testy: základní operace
 * ==================================================================== */


/** @brief Inicializovaný MRS disk má prázdný adresář. */
static int test_mrs_init_empty ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    TEST_ASSERT_EQ_INT ( config.used_files, 0 );
    TEST_ASSERT ( config.free_blocks > 0, "free blocks > 0" );
    TEST_ASSERT ( config.total_blocks > 0, "total blocks > 0" );

    close_disc ( &disc );
    return 0;
}


/** @brief Put soubor -> soubor v adresáři. */
static int test_mrs_put_file ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    uint8_t data[512];
    for ( int i = 0; i < 512; i++ ) data[i] = (uint8_t) ( i & 0xFF );

    TEST_ASSERT ( fsmrs_write_file ( &config,
                                      (const uint8_t *) "TEST    ",
                                      (const uint8_t *) "DAT",
                                      0x1200, 0x1200,
                                      data, 512 ) == MZDSK_RES_OK,
                  "write_file" );

    /* Musí být v adresáři */
    st_FSMRS_DIR_ITEM *item = fsmrs_search_file ( &config,
                                                    (const uint8_t *) "TEST    ",
                                                    (const uint8_t *) "DAT" );
    TEST_ASSERT_NOT_NULL ( item );

    close_disc ( &disc );
    return 0;
}


/** @brief Put + get roundtrip - binární shoda. */
static int test_mrs_roundtrip ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    /* 1024B testovací data */
    uint8_t data_in[1024];
    for ( int i = 0; i < 1024; i++ ) data_in[i] = (uint8_t) ( i & 0xFF );

    TEST_ASSERT ( fsmrs_write_file ( &config,
                                      (const uint8_t *) "ROUND   ",
                                      (const uint8_t *) "TST",
                                      0x1200, 0x1200,
                                      data_in, 1024 ) == MZDSK_RES_OK,
                  "write" );

    /* Najít soubor a přečíst */
    st_FSMRS_DIR_ITEM *item = fsmrs_search_file ( &config,
                                                    (const uint8_t *) "ROUND   ",
                                                    (const uint8_t *) "TST" );
    TEST_ASSERT_NOT_NULL ( item );

    /* MRS ukládá po 512B blocích - 1024B = 2 bloky */
    uint8_t data_out[1024];
    memset ( data_out, 0, sizeof ( data_out ) );
    TEST_ASSERT ( fsmrs_read_file ( &config, item, data_out, 1024 ) == MZDSK_RES_OK,
                  "read" );

    TEST_ASSERT_EQ_MEM ( data_in, data_out, 1024 );

    close_disc ( &disc );
    return 0;
}


/** @brief Delete soubor -> zmizel. */
static int test_mrs_delete ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    uint8_t data[512];
    memset ( data, 0xAA, 512 );
    TEST_ASSERT ( fsmrs_write_file ( &config,
                                      (const uint8_t *) "DELME   ",
                                      (const uint8_t *) "DAT",
                                      0x1200, 0x1200,
                                      data, 512 ) == MZDSK_RES_OK, "write" );

    st_FSMRS_DIR_ITEM *item = fsmrs_search_file ( &config,
                                                    (const uint8_t *) "DELME   ",
                                                    (const uint8_t *) "DAT" );
    TEST_ASSERT_NOT_NULL ( item );

    uint16_t free_before = config.free_blocks;

    TEST_ASSERT ( fsmrs_delete_file ( &config, item ) == MZDSK_RES_OK, "delete" );

    /* Soubor už nenajdu */
    item = fsmrs_search_file ( &config,
                                (const uint8_t *) "DELME   ",
                                (const uint8_t *) "DAT" );
    TEST_ASSERT_NULL ( item );

    /* Volné bloky se vrátily */
    TEST_ASSERT ( config.free_blocks > free_before, "free blocks increased" );

    close_disc ( &disc );
    return 0;
}


/** @brief Volné místo se zmenší po put a vrátí po delete. */
static int test_mrs_free_blocks ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    uint16_t free_init = config.free_blocks;

    uint8_t data[2048]; /* 4 bloky */
    memset ( data, 0, sizeof ( data ) );
    TEST_ASSERT ( fsmrs_write_file ( &config,
                                      (const uint8_t *) "SPACE   ",
                                      (const uint8_t *) "TST",
                                      0x1200, 0x1200,
                                      data, 2048 ) == MZDSK_RES_OK, "write" );

    TEST_ASSERT ( config.free_blocks < free_init, "free decreased" );

    st_FSMRS_DIR_ITEM *item = fsmrs_search_file ( &config,
                                                    (const uint8_t *) "SPACE   ",
                                                    (const uint8_t *) "TST" );
    TEST_ASSERT_NOT_NULL ( item );
    TEST_ASSERT ( fsmrs_delete_file ( &config, item ) == MZDSK_RES_OK, "delete" );

    TEST_ASSERT_EQ_INT ( config.free_blocks, free_init );

    close_disc ( &disc );
    return 0;
}


/** @brief MRS detekce na inicializovaném disku. */
static int test_mrs_detect ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_MRS );

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  Edge cases
 * ==================================================================== */


/** @brief Velký soubor (10KB = 20 bloků). */
static int test_mrs_large_file ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    uint32_t size = 10240; /* 10KB = 20 bloků po 512B */
    uint8_t *data_in = (uint8_t *) malloc ( size );
    for ( uint32_t i = 0; i < size; i++ ) data_in[i] = (uint8_t) ( i & 0xFF );

    TEST_ASSERT ( fsmrs_write_file ( &config,
                                      (const uint8_t *) "BIGFILE ",
                                      (const uint8_t *) "DAT",
                                      0x1200, 0x1200,
                                      data_in, size ) == MZDSK_RES_OK,
                  "write 10KB" );

    st_FSMRS_DIR_ITEM *item = fsmrs_search_file ( &config,
                                                    (const uint8_t *) "BIGFILE ",
                                                    (const uint8_t *) "DAT" );
    TEST_ASSERT_NOT_NULL ( item );

    uint8_t *data_out = (uint8_t *) calloc ( 1, size );
    TEST_ASSERT ( fsmrs_read_file ( &config, item, data_out, size ) == MZDSK_RES_OK,
                  "read 10KB" );
    TEST_ASSERT_EQ_MEM ( data_in, data_out, size );

    free ( data_in );
    free ( data_out );
    close_disc ( &disc );
    return 0;
}


/** @brief Naplnění disku - zápis dokud neselže. */
static int test_mrs_disk_full ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    /* MRS má ~1440 bloků, systémové zabírají ~45.
       Maximální soubor ~717KB. Budeme plnit 10KB soubory. */
    uint8_t *data = (uint8_t *) calloc ( 1, 10240 );
    char name[9];
    int i;
    en_MZDSK_RES res = MZDSK_RES_OK;

    for ( i = 0; i < 200; i++ ) {
        snprintf ( name, sizeof ( name ), "F%06d ", i );
        /* MRS jméno musí být 8 znaků, doplněno mezerami */
        res = fsmrs_write_file ( &config,
                                  (const uint8_t *) name,
                                  (const uint8_t *) "DAT",
                                  0x1200, 0x1200,
                                  data, 10240 );
        if ( res != MZDSK_RES_OK ) break;
    }

    TEST_ASSERT ( i > 0, "at least one file must fit" );
    TEST_ASSERT ( i < 200, "must fail before 200 files (disk full)" );

    free ( data );
    close_disc ( &disc );
    return 0;
}


/** @brief Delete neexistujícího souboru. */
static int test_mrs_delete_nonexistent ( void )
{
    st_MZDSK_DISC disc;
    st_FSMRS_CONFIG config;
    TEST_ASSERT_OK ( create_formatted_mrs ( &disc, &config ) );

    st_FSMRS_DIR_ITEM *item = fsmrs_search_file ( &config,
                                                    (const uint8_t *) "NOFILE  ",
                                                    (const uint8_t *) "XXX" );
    TEST_ASSERT_NULL ( item );

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
    RUN_TEST ( test_mrs_init_empty );
    RUN_TEST ( test_mrs_put_file );
    RUN_TEST ( test_mrs_roundtrip );
    RUN_TEST ( test_mrs_delete );
    RUN_TEST ( test_mrs_free_blocks );
    RUN_TEST ( test_mrs_detect );

    /* edge cases */
    RUN_TEST ( test_mrs_large_file );
    RUN_TEST ( test_mrs_disk_full );
    RUN_TEST ( test_mrs_delete_nonexistent );

    TEST_SUMMARY ();
}
