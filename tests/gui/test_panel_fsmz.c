/**
 * @file test_panel_fsmz.c
 * @brief Testy GUI FSMZ operací přes panel vrstvu.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "panels/panel_fsmz.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


static char s_tmpdir[256];
static int setup_tmpdir ( void ) {
    strncpy ( s_tmpdir, "/tmp/mzdsk_test_gfsmz_XXXXXX", sizeof ( s_tmpdir ) );
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
    if ( mzdsk_tools_format_basic ( &h, 80, 2 ) != EXIT_SUCCESS ) {
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

static int create_test_mzf ( const char *path, const char *name, uint16_t body_size )
{
    uint8_t fname[16];
    memset ( fname, 0x0D, 16 );
    memcpy ( fname, name, strlen(name) );
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr ( 0x01, body_size, 0x1200, 0x1200, fname, (unsigned)strlen(name), NULL );
    if ( !hdr ) return EXIT_FAILURE;
    st_MZF mzf; mzf.header = *hdr; free(hdr);
    mzf.body = (uint8_t *) malloc(body_size);
    for ( uint16_t i = 0; i < body_size; i++ ) mzf.body[i] = (uint8_t)(i & 0xFF);
    mzf.body_size = body_size;
    st_HANDLER h = {0};
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc(1,1);
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    mzf_save ( &h, &mzf );
    free(mzf.body);
    int ret = generic_driver_save_memory ( &h, (char *)path );
    free(h.spec.memspec.ptr);
    return ret;
}


/** @brief Load adresáře -> správný počet souborů. */
static int test_gui_fsmz_load ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512]; snprintf ( dp, sizeof(dp), "%s/f.dsk", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    st_PANEL_FSMZ_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_fsmz_load ( &data, &disc );
    TEST_ASSERT_EQ_INT ( data.is_loaded, 1 );
    TEST_ASSERT_EQ_INT ( data.file_count, 0 );

    mzdsk_disc_close ( &disc ); remove(dp); rmdir(s_tmpdir);
    return 0;
}

/** @brief Put MZF -> load -> soubor v adresáři. */
static int test_gui_fsmz_put ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], mp[512];
    snprintf ( dp, sizeof(dp), "%s/fp.dsk", s_tmpdir );
    snprintf ( mp, sizeof(mp), "%s/fp.mzf", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_test_mzf ( mp, "GUIPUT", 256 );

    en_MZDSK_RES res = panel_fsmz_put_file ( &disc, mp );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    st_PANEL_FSMZ_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_fsmz_load ( &data, &disc );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );

    mzdsk_disc_close ( &disc ); remove(mp); remove(dp); rmdir(s_tmpdir);
    return 0;
}

/** @brief Delete -> soubor zmizí z dat. modelu. */
static int test_gui_fsmz_delete ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], mp[512];
    snprintf ( dp, sizeof(dp), "%s/fd.dsk", s_tmpdir );
    snprintf ( mp, sizeof(mp), "%s/fd.mzf", s_tmpdir );
    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_test_mzf ( mp, "GUIDEL", 256 );
    panel_fsmz_put_file ( &disc, mp );

    st_PANEL_FSMZ_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_fsmz_load ( &data, &disc );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );

    en_MZDSK_RES res = panel_fsmz_delete_file ( &disc, &data.files[0] );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    panel_fsmz_load ( &data, &disc );
    TEST_ASSERT_EQ_INT ( data.file_count, 0 );

    mzdsk_disc_close ( &disc ); remove(mp); remove(dp); rmdir(s_tmpdir);
    return 0;
}

/** @brief Textový popis typu (OBJ, BTX, BSD). */
static int test_gui_fsmz_type_str ( void )
{
    TEST_ASSERT_NOT_NULL ( panel_fsmz_type_str ( 0x01 ) );
    TEST_ASSERT ( strlen ( panel_fsmz_type_str ( 0x01 ) ) > 0, "OBJ type non-empty" );
    TEST_ASSERT_NOT_NULL ( panel_fsmz_type_str ( 0x02 ) );
    return 0;
}


int main ( void )
{
    memory_driver_init();
    TEST_INIT();
    RUN_TEST ( test_gui_fsmz_load );
    RUN_TEST ( test_gui_fsmz_put );
    RUN_TEST ( test_gui_fsmz_delete );
    RUN_TEST ( test_gui_fsmz_type_str );
    TEST_SUMMARY();
}
