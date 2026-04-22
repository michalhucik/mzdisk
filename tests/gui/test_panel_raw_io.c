/**
 * @file test_panel_raw_io.c
 * @brief Testy GUI panelu pro surový sektorový/blokový I/O.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "panels/panel_raw_io.h"
#include "panels/panel_hexdump.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


static char s_tmpdir[256];
static int setup_tmpdir ( void ) {
    strncpy ( s_tmpdir, "/tmp/mzdsk_test_rio_XXXXXX", sizeof ( s_tmpdir ) );
    return mkdtemp ( s_tmpdir ) != NULL ? 0 : -1;
}

static int create_fsmz_disc ( st_MZDSK_DISC *disc, const char *path )
{
    st_HANDLER h = {0};
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    if ( mzdsk_tools_format_basic ( &h, 40, 1 ) != EXIT_SUCCESS ) {
        free ( h.spec.memspec.ptr ); return EXIT_FAILURE;
    }
    st_HANDLER fh; st_DRIVER fd;
    generic_driver_file_init ( &fd );
    generic_driver_open_file ( &fh, &fd, (char *) path, FILE_DRIVER_OPMODE_W );
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );
    return ( mzdsk_disc_open_memory ( disc, (char *) path, FILE_DRIVER_OPMODE_RW ) == MZDSK_RES_OK )
           ? EXIT_SUCCESS : EXIT_FAILURE;
}


/** @brief Inicializace datového modelu. */
static int test_raw_io_init ( void )
{
    st_PANEL_RAW_IO_DATA data;
    panel_raw_io_init ( &data );
    TEST_ASSERT_EQ_INT ( data.is_open, 0 );
    return 0;
}

/** @brief Export sektorů -> import -> data zachována. */
static int test_raw_io_get_put_roundtrip ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512];
    snprintf ( dp, sizeof ( dp ), "%s/rio.dsk", s_tmpdir );
    snprintf ( fp, sizeof ( fp ), "%s/sector.bin", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dp ) ) { remove ( dp ); rmdir ( s_tmpdir ); TEST_FAIL ( "disc" ); }

    /* zapsat marker do sektoru T0/S1 */
    uint8_t marker[256];
    memset ( marker, 0xAB, sizeof ( marker ) );
    dsk_write_sector ( disc.handler, 0, 1, marker );

    /* get: export T0/S1 */
    st_PANEL_RAW_IO_DATA data;
    panel_raw_io_init ( &data );
    data.addr_mode = HEXDUMP_ADDR_TRACK_SECTOR;
    data.start_track = 0;
    data.start_sector = 1;
    data.sector_count = 1;
    strncpy ( data.filepath, fp, sizeof ( data.filepath ) - 1 );

    en_MZDSK_RES res = panel_raw_io_execute_get ( &data, &disc );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    /* přepsat sektor nulami */
    uint8_t zeros[256];
    memset ( zeros, 0x00, sizeof ( zeros ) );
    dsk_write_sector ( disc.handler, 0, 1, zeros );

    /* put: importovat zpět */
    res = panel_raw_io_execute_put ( &data, &disc );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    /* ověřit, že data jsou zpět */
    uint8_t readback[256];
    dsk_read_sector ( disc.handler, 0, 1, readback );
    TEST_ASSERT_EQ_MEM ( readback, marker, sizeof ( marker ) );

    mzdsk_disc_close ( &disc );
    remove ( fp ); remove ( dp ); rmdir ( s_tmpdir );
    return 0;
}

/**
 * @brief Put s put_whole_file=true zapíše celý soubor bez ohledu na
 *        sector_count / byte_count.
 *
 * Regrese: UI checkbox "Whole file" je defaultně zapnutý, což musí
 * i v C vrstvě override sector_count (malá hodnota, třeba 1) tak, aby
 * se zapsal celý soubor.
 */
static int test_raw_io_put_whole_file ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512];
    snprintf ( dp, sizeof ( dp ), "%s/rio_wf.dsk", s_tmpdir );
    snprintf ( fp, sizeof ( fp ), "%s/whole.bin", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dp ) ) { remove ( dp ); rmdir ( s_tmpdir ); TEST_FAIL ( "disc" ); }

    /* Připravit soubor velikosti 3 FSMZ sektorů (256*3 = 768 B).
       Každý sektor vyplníme jiným markerem, ať poznáme pozici. */
    uint8_t file_data[768];
    for ( int s = 0; s < 3; s++ ) {
        memset ( file_data + s * 256, (uint8_t) ( 0x10 + s ), 256 );
    }
    FILE *fw = fopen ( fp, "wb" );
    TEST_ASSERT ( fw != NULL, "create test file" );
    fwrite ( file_data, 1, sizeof ( file_data ), fw );
    fclose ( fw );

    /* put s whole_file=true a sector_count=1 (schválně podhodnocený) */
    st_PANEL_RAW_IO_DATA data;
    panel_raw_io_init ( &data );  /* default put_whole_file = true */
    TEST_ASSERT_EQ_INT ( data.put_whole_file, 1 );
    data.addr_mode = HEXDUMP_ADDR_TRACK_SECTOR;
    data.start_track = 0;
    data.start_sector = 1;
    data.sector_count = 1;  /* override přes put_whole_file */
    data.byte_count = 0;
    strncpy ( data.filepath, fp, sizeof ( data.filepath ) - 1 );

    en_MZDSK_RES res = panel_raw_io_execute_put ( &data, &disc );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    /* Ověřit, že všechny 3 sektory obsahují odpovídající markery. */
    uint8_t rb[256];
    dsk_read_sector ( disc.handler, 0, 1, rb );
    TEST_ASSERT_EQ_INT ( rb[0], 0x10 );
    dsk_read_sector ( disc.handler, 0, 2, rb );
    TEST_ASSERT_EQ_INT ( rb[0], 0x11 );
    dsk_read_sector ( disc.handler, 0, 3, rb );
    TEST_ASSERT_EQ_INT ( rb[0], 0x12 );

    mzdsk_disc_close ( &disc );
    remove ( fp ); remove ( dp ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Open from hexdump context. */
static int test_raw_io_open_from_hexdump ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512]; snprintf ( dp, sizeof ( dp ), "%s/rioh.dsk", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dp ) ) { remove ( dp ); rmdir ( s_tmpdir ); TEST_FAIL ( "disc" ); }

    st_PANEL_HEXDUMP_DATA hd;
    panel_hexdump_init ( &hd, &disc );

    st_PANEL_RAW_IO_DATA data;
    panel_raw_io_init ( &data );
    panel_raw_io_open_from_hexdump ( &data, &hd, RAW_IO_ACTION_GET );

    TEST_ASSERT_EQ_INT ( data.is_open, 1 );
    TEST_ASSERT_EQ_INT ( data.action, RAW_IO_ACTION_GET );

    mzdsk_disc_close ( &disc );
    remove ( dp ); rmdir ( s_tmpdir );
    return 0;
}


int main ( void )
{
    memory_driver_init();
    TEST_INIT();
    RUN_TEST ( test_raw_io_init );
    RUN_TEST ( test_raw_io_get_put_roundtrip );
    RUN_TEST ( test_raw_io_put_whole_file );
    RUN_TEST ( test_raw_io_open_from_hexdump );
    TEST_SUMMARY();
}
