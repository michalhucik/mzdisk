/**
 * @file test_panel_mrs.c
 * @brief Testy GUI MRS operací přes panel vrstvu.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "panels/panel_mrs.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


static char s_tmpdir[256];
static int setup_tmpdir ( void ) {
    strncpy ( s_tmpdir, "/tmp/mzdsk_test_gmrs_XXXXXX", sizeof ( s_tmpdir ) );
    return mkdtemp ( s_tmpdir ) != NULL ? 0 : -1;
}

static int create_mrs_disc ( st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect, const char *path )
{
    /* vytvořit MRS disk přes preset + fsmrs_format_fs */
    st_HANDLER h = {0};
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc(1,1);
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    if ( mzdsk_tools_create_from_preset ( &h, MZDSK_PRESET_MRS, 2 ) != EXIT_SUCCESS ) {
        free(h.spec.memspec.ptr); return EXIT_FAILURE;
    }
    st_HANDLER fh; st_DRIVER fd;
    generic_driver_file_init ( &fd );
    generic_driver_open_file ( &fh, &fd, (char *)path, FILE_DRIVER_OPMODE_W );
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free(h.spec.memspec.ptr);

    if ( mzdsk_disc_open_memory ( disc, (char *)path, FILE_DRIVER_OPMODE_RW ) != MZDSK_RES_OK )
        return EXIT_FAILURE;

    /* inicializovat MRS FS */
    disc->sector_info_cb = fsmrs_sector_info_cb;
    disc->sector_info_cb_data = NULL;
    if ( fsmrs_format_fs ( disc, 36 ) != MZDSK_RES_OK ) return EXIT_FAILURE;

    mzdsk_detect_filesystem ( disc, detect );
    return ( detect->type == MZDSK_FS_MRS ) ? EXIT_SUCCESS : EXIT_FAILURE;
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


/** @brief Load adresáře + FAT vizualizace. */
static int test_gui_mrs_load ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512]; snprintf ( dp, sizeof(dp), "%s/m.dsk", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_mrs_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    st_PANEL_MRS_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_mrs_load ( &data, &detect );
    TEST_ASSERT_EQ_INT ( data.is_loaded, 1 );
    TEST_ASSERT_EQ_INT ( data.file_count, 0 );

    mzdsk_disc_close ( &disc ); remove(dp); rmdir(s_tmpdir);
    return 0;
}

/** @brief Put MZF -> load -> soubor v adresáři. */
static int test_gui_mrs_put ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], mp[512];
    snprintf ( dp, sizeof(dp), "%s/mp.dsk", s_tmpdir );
    snprintf ( mp, sizeof(mp), "%s/mp.mzf", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_mrs_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_test_mzf ( mp, "MRSPUT", 512 );

    en_MZDSK_RES res = panel_mrs_put_file_mzf ( &detect.mrs_config, mp );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    st_PANEL_MRS_DATA data;
    memset ( &data, 0, sizeof(data) );
    /* reload detekce pro aktuální stav */
    mzdsk_detect_filesystem ( &disc, &detect );
    panel_mrs_load ( &data, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );

    mzdsk_disc_close ( &disc ); remove(mp); remove(dp); rmdir(s_tmpdir);
    return 0;
}

/** @brief put_file (raw) - nová _ex varianta pro GUI. */
static int test_gui_mrs_put_raw ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512];
    snprintf ( dp, sizeof(dp), "%s/mraw.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/raw.bin", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_mrs_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    FILE *f = fopen ( fp, "wb" );
    for ( int i = 0; i < 256; i++ ) fputc ( i & 0xFF, f );
    fclose ( f );

    en_MZDSK_RES res = panel_mrs_put_file ( &detect.mrs_config, fp, "RAWIMP", "DAT", 0x1200, 0x1200 );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    mzdsk_detect_filesystem ( &disc, &detect );
    st_PANEL_MRS_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_mrs_load ( &data, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );
    TEST_ASSERT_EQ_STR ( data.files[0].name, "RAWIMP" );
    TEST_ASSERT_EQ_STR ( data.files[0].ext, "DAT" );

    mzdsk_disc_close ( &disc ); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


/** @brief put_file_mzf_ex s override přepíše jméno z MZF hlavičky. */
static int test_gui_mrs_put_mzf_ex_override ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], mp[512];
    snprintf ( dp, sizeof(dp), "%s/movr.dsk", s_tmpdir );
    snprintf ( mp, sizeof(mp), "%s/movr.mzf", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_mrs_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_test_mzf ( mp, "MZFSRC", 256 );

    /* Override jméno na OTHER.DAT */
    en_MZDSK_RES res = panel_mrs_put_file_mzf_ex ( &detect.mrs_config, mp, "OTHER", "DAT" );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    mzdsk_detect_filesystem ( &disc, &detect );
    st_PANEL_MRS_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_mrs_load ( &data, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );
    TEST_ASSERT_EQ_STR ( data.files[0].name, "OTHER" );
    TEST_ASSERT_EQ_STR ( data.files[0].ext, "DAT" );

    mzdsk_disc_close ( &disc ); remove(mp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


/** @brief put_file_mzf_ex s NULL override chová se jako put_file_mzf. */
static int test_gui_mrs_put_mzf_ex_null_override ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], mp[512];
    snprintf ( dp, sizeof(dp), "%s/mnl.dsk", s_tmpdir );
    snprintf ( mp, sizeof(mp), "%s/mnl.mzf", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_mrs_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_test_mzf ( mp, "KEEP", 256 );

    en_MZDSK_RES res = panel_mrs_put_file_mzf_ex ( &detect.mrs_config, mp, NULL, NULL );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    mzdsk_detect_filesystem ( &disc, &detect );
    st_PANEL_MRS_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_mrs_load ( &data, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );
    TEST_ASSERT_EQ_STR ( data.files[0].name, "KEEP" );

    mzdsk_disc_close ( &disc ); remove(mp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


/** @brief Delete -> soubor zmizí. */
static int test_gui_mrs_delete ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], mp[512];
    snprintf ( dp, sizeof(dp), "%s/md.dsk", s_tmpdir );
    snprintf ( mp, sizeof(mp), "%s/md.mzf", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_mrs_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_test_mzf ( mp, "MRSDEL", 512 );
    panel_mrs_put_file_mzf ( &detect.mrs_config, mp );

    mzdsk_detect_filesystem ( &disc, &detect );
    st_PANEL_MRS_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_mrs_load ( &data, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );

    en_MZDSK_RES res = panel_mrs_delete_file ( &detect.mrs_config, &data.files[0] );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    mzdsk_detect_filesystem ( &disc, &detect );
    panel_mrs_load ( &data, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 0 );

    mzdsk_disc_close ( &disc ); remove(mp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


int main ( void )
{
    memory_driver_init();
    TEST_INIT();
    RUN_TEST ( test_gui_mrs_load );
    RUN_TEST ( test_gui_mrs_put );
    RUN_TEST ( test_gui_mrs_put_raw );
    RUN_TEST ( test_gui_mrs_put_mzf_ex_override );
    RUN_TEST ( test_gui_mrs_put_mzf_ex_null_override );
    RUN_TEST ( test_gui_mrs_delete );
    TEST_SUMMARY();
}
