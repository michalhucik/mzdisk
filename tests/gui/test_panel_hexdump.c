/**
 * @file test_panel_hexdump.c
 * @brief Testy GUI hexdump vieweru/editoru.
 *
 * Testuje inicializaci, čtení sektorů, konverzi blok->track/sector,
 * editaci s undo a zápis dat.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "panels/panel_hexdump.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


static char s_tmpdir[256];
static int setup_tmpdir ( void ) {
    strncpy ( s_tmpdir, "/tmp/mzdsk_test_hd_XXXXXX", sizeof ( s_tmpdir ) );
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


/** @brief Inicializace na T0/S1. */
static int test_hexdump_init ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof(p), "%s/hd.dsk", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, p ) ) { remove(p); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    st_PANEL_HEXDUMP_DATA hd;
    panel_hexdump_init ( &hd, &disc );
    TEST_ASSERT_EQ_INT ( hd.is_loaded, 1 );

    mzdsk_disc_close ( &disc ); remove(p); rmdir(s_tmpdir);
    return 0;
}

/** @brief Čtení sektoru do bufferu. */
static int test_hexdump_read ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof(p), "%s/hd2.dsk", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, p ) ) { remove(p); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    st_PANEL_HEXDUMP_DATA hd;
    panel_hexdump_init ( &hd, &disc );
    panel_hexdump_read_sector ( &hd, &disc );
    TEST_ASSERT ( hd.data_size > 0, "data loaded" );

    mzdsk_disc_close ( &disc ); remove(p); rmdir(s_tmpdir);
    return 0;
}

/** @brief Edit mode -> revert -> data obnovena. */
static int test_hexdump_edit_revert ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof(p), "%s/hd3.dsk", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, p ) ) { remove(p); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    st_PANEL_HEXDUMP_DATA hd;
    panel_hexdump_init ( &hd, &disc );
    panel_hexdump_read_sector ( &hd, &disc );

    /* zapamatovat originální data */
    uint8_t orig[1024];
    memcpy ( orig, hd.data, hd.data_size );

    /* enter edit + modifikace */
    panel_hexdump_enter_edit ( &hd );
    TEST_ASSERT_EQ_INT ( hd.edit_mode, 1 );
    if ( hd.data_size > 0 ) hd.data[0] ^= 0xFF;

    /* revert -> data obnovena */
    panel_hexdump_revert_edit ( &hd );
    TEST_ASSERT_EQ_INT ( hd.edit_mode, 0 );
    TEST_ASSERT_EQ_MEM ( hd.data, orig, hd.data_size );

    mzdsk_disc_close ( &disc ); remove(p); rmdir(s_tmpdir);
    return 0;
}

/** @brief Edit -> write -> data persistována na disku. */
static int test_hexdump_write ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof(p), "%s/hd4.dsk", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, p ) ) { remove(p); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    st_PANEL_HEXDUMP_DATA hd;
    panel_hexdump_init ( &hd, &disc );
    panel_hexdump_read_sector ( &hd, &disc );

    panel_hexdump_enter_edit ( &hd );
    if ( hd.data_size > 0 ) hd.data[0] = 0xAB;

    en_MZDSK_RES res = panel_hexdump_write_data ( &hd, &disc );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    /* znovu přečíst a ověřit */
    panel_hexdump_read_sector ( &hd, &disc );
    TEST_ASSERT_EQ_INT ( hd.data[0], 0xAB );

    mzdsk_disc_close ( &disc ); remove(p); rmdir(s_tmpdir);
    return 0;
}

/** @brief Dotaz na sektory/velikost pro stopu. */
static int test_hexdump_track_params ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof(p), "%s/hd5.dsk", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, p ) ) { remove(p); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    uint16_t sectors = 0, sector_size = 0;
    panel_hexdump_get_track_params ( &disc, 0, &sectors, &sector_size );
    TEST_ASSERT_EQ_INT ( sectors, 16 );
    TEST_ASSERT_EQ_INT ( sector_size, 256 );

    mzdsk_disc_close ( &disc ); remove(p); rmdir(s_tmpdir);
    return 0;
}

/** @brief Whole-disk preset. */
static int test_hexdump_preset_whole ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof(p), "%s/hd6.dsk", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, p ) ) { remove(p); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    st_PANEL_HEXDUMP_DATA hd;
    panel_hexdump_init ( &hd, &disc );
    panel_hexdump_preset_whole_disk ( &hd );
    TEST_ASSERT_EQ_INT ( hd.block_config.sectors_per_block, 1 );

    mzdsk_disc_close ( &disc ); remove(p); rmdir(s_tmpdir);
    return 0;
}


int main ( void )
{
    memory_driver_init();
    TEST_INIT();
    RUN_TEST ( test_hexdump_init );
    RUN_TEST ( test_hexdump_read );
    RUN_TEST ( test_hexdump_edit_revert );
    RUN_TEST ( test_hexdump_write );
    RUN_TEST ( test_hexdump_track_params );
    RUN_TEST ( test_hexdump_preset_whole );
    TEST_SUMMARY();
}
