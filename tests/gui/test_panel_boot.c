/**
 * @file test_panel_boot.c
 * @brief Testy GUI panelu bootstrap managementu.
 *
 * Testuje načítání IPLPRO/DINFO dat, instalaci a odstranění bootstrapu
 * a CP/M systémové stopy. Používá temp soubory pro MZF a DSK.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "panels/panel_boot.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/* ===================================================================
 *  Pomocné funkce
 * =================================================================== */


static char s_tmpdir[256];

static int setup_tmpdir ( void )
{
    strncpy ( s_tmpdir, "/tmp/mzdsk_test_boot_XXXXXX", sizeof ( s_tmpdir ) );
    return mkdtemp ( s_tmpdir ) != NULL ? 0 : -1;
}


/**
 * @brief Vytvoří FSMZ disk jako soubor a otevře ho do paměti.
 */
static int create_fsmz_disc ( st_MZDSK_DISC *disc, const char *filepath )
{
    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;

    if ( mzdsk_tools_format_basic ( &h, 80, 2 ) != EXIT_SUCCESS ) {
        free ( h.spec.memspec.ptr );
        return EXIT_FAILURE;
    }

    st_HANDLER fh;
    st_DRIVER fd;
    generic_driver_file_init ( &fd );
    if ( !generic_driver_open_file ( &fh, &fd, (char *) filepath, FILE_DRIVER_OPMODE_W ) ) {
        free ( h.spec.memspec.ptr );
        return EXIT_FAILURE;
    }
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );

    en_MZDSK_RES res = mzdsk_disc_open_memory ( disc, (char *) filepath, FILE_DRIVER_OPMODE_RW );
    return ( res == MZDSK_RES_OK ) ? EXIT_SUCCESS : EXIT_FAILURE;
}


/**
 * @brief Vytvoří testovací MZF soubor (bootstrap typ 0x03).
 */
static int create_test_boot_mzf ( const char *path, uint16_t body_size )
{
    uint8_t fname[] = "MYBOOT";
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr (
        0x03, body_size, 0x1200, 0x1200, fname, 6, NULL
    );
    if ( !hdr ) return EXIT_FAILURE;

    st_MZF mzf;
    mzf.header = *hdr;
    free ( hdr );

    mzf.body = (uint8_t *) malloc ( body_size );
    if ( !mzf.body ) return EXIT_FAILURE;
    for ( uint16_t i = 0; i < body_size; i++ ) mzf.body[i] = (uint8_t) ( i & 0xFF );
    mzf.body_size = body_size;

    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;

    en_MZF_ERROR err = mzf_save ( &h, &mzf );
    free ( mzf.body );

    if ( err != MZF_OK ) {
        free ( h.spec.memspec.ptr );
        return EXIT_FAILURE;
    }

    int ret = generic_driver_save_memory ( &h, (char *) path );
    free ( h.spec.memspec.ptr );
    return ret;
}


/* ===================================================================
 *  Testy
 * =================================================================== */


/** @brief Load na disku bez bootstrapu: has_iplpro ale iplpro_valid = false. */
static int test_boot_load_empty ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dsk_path[512];
    snprintf ( dsk_path, sizeof ( dsk_path ), "%s/empty.dsk", s_tmpdir );

    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dsk_path ) != EXIT_SUCCESS ) {
        remove ( dsk_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create_fsmz_disc" );
    }

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );

    st_PANEL_BOOT_DATA data;
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );

    TEST_ASSERT_EQ_INT ( data.is_loaded, 1 );
    /* IPLPRO hlavička existuje (přečtena), ale není validní (prázdný disk) */
    TEST_ASSERT_EQ_INT ( data.iplpro_valid, 0 );
    TEST_ASSERT_EQ_INT ( data.has_dinfo, 1 );
    TEST_ASSERT_EQ_INT ( data.is_full_fsmz, 1 );

    mzdsk_disc_close ( &disc );
    remove ( dsk_path ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Put bottom bootstrap -> load -> správné pole. */
static int test_boot_put_bottom_load ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dsk_path[512], mzf_path[512];
    snprintf ( dsk_path, sizeof ( dsk_path ), "%s/boot.dsk", s_tmpdir );
    snprintf ( mzf_path, sizeof ( mzf_path ), "%s/boot.mzf", s_tmpdir );

    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dsk_path ) != EXIT_SUCCESS ) {
        remove ( dsk_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create disc" );
    }

    /* vytvořit MZF bootstrap (256B tělo = 1 FSMZ blok) */
    if ( create_test_boot_mzf ( mzf_path, 256 ) != EXIT_SUCCESS ) {
        mzdsk_disc_close ( &disc );
        remove ( dsk_path ); remove ( mzf_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create MZF" );
    }

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );

    st_PANEL_BOOT_DATA data;
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );

    /* put bottom */
    en_MZDSK_RES res = panel_boot_put_bottom ( &data, &disc, mzf_path );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    /* reload */
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );

    TEST_ASSERT_EQ_INT ( data.iplpro_valid, 1 );
    TEST_ASSERT_EQ_INT ( data.fsize, 256 );
    TEST_ASSERT_EQ_INT ( data.fstrt, 0x1200 );
    TEST_ASSERT_EQ_INT ( data.block, 1 );

    mzdsk_disc_close ( &disc );
    remove ( mzf_path ); remove ( dsk_path ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Put bottom -> reload -> metadata odpovídají vstupu. */
static int test_boot_put_metadata ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dsk_path[512], mzf_path[512];
    snprintf ( dsk_path, sizeof ( dsk_path ), "%s/meta.dsk", s_tmpdir );
    snprintf ( mzf_path, sizeof ( mzf_path ), "%s/meta.mzf", s_tmpdir );

    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dsk_path ) != EXIT_SUCCESS ) {
        remove ( dsk_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create disc" );
    }
    create_test_boot_mzf ( mzf_path, 512 );

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );

    st_PANEL_BOOT_DATA data;
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );

    TEST_ASSERT_EQ_INT ( panel_boot_put_bottom ( &data, &disc, mzf_path ), MZDSK_RES_OK );

    /* reload a ověření metadat */
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );

    TEST_ASSERT_EQ_INT ( data.iplpro_valid, 1 );
    TEST_ASSERT_EQ_INT ( data.fsize, 512 );
    TEST_ASSERT_EQ_INT ( data.fstrt, 0x1200 );
    TEST_ASSERT_EQ_INT ( data.fexec, 0x1200 );
    TEST_ASSERT_EQ_INT ( data.block, 1 );
    TEST_ASSERT_EQ_INT ( data.block_count, 2 ); /* 512B = 2 bloky po 256B */
    TEST_ASSERT ( strlen ( data.name ) > 0, "bootstrap name non-empty" );
    TEST_ASSERT ( strlen ( data.boot_type ) > 0, "boot type classified" );

    mzdsk_disc_close ( &disc );
    remove ( mzf_path ); remove ( dsk_path ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Clear bootstrap -> iplpro_valid = false. */
static int test_boot_clear ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dsk_path[512], mzf_path[512];
    snprintf ( dsk_path, sizeof ( dsk_path ), "%s/clr.dsk", s_tmpdir );
    snprintf ( mzf_path, sizeof ( mzf_path ), "%s/clr.mzf", s_tmpdir );

    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dsk_path ) != EXIT_SUCCESS ) {
        remove ( dsk_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create disc" );
    }
    create_test_boot_mzf ( mzf_path, 256 );

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );

    st_PANEL_BOOT_DATA data;
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );

    /* put + clear */
    panel_boot_put_bottom ( &data, &disc, mzf_path );
    TEST_ASSERT_EQ_INT ( panel_boot_clear ( &disc ), MZDSK_RES_OK );

    /* reload - bootstrap by měl být pryč */
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.iplpro_valid, 0 );

    mzdsk_disc_close ( &disc );
    remove ( mzf_path ); remove ( dsk_path ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief DINFO blok: volume_number, farea, blocks. */
static int test_boot_dinfo ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dsk_path[512];
    snprintf ( dsk_path, sizeof ( dsk_path ), "%s/dinfo.dsk", s_tmpdir );

    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dsk_path ) != EXIT_SUCCESS ) {
        remove ( dsk_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create disc" );
    }

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );

    st_PANEL_BOOT_DATA data;
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );

    TEST_ASSERT_EQ_INT ( data.has_dinfo, 1 );
    TEST_ASSERT ( data.farea > 0, "farea > 0" );
    TEST_ASSERT ( data.dinfo_blocks > 0, "dinfo blocks > 0" );

    mzdsk_disc_close ( &disc );
    remove ( dsk_path ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Bootstrap příliš velký -> chyba NO_SPACE. */
static int test_boot_too_large ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dsk_path[512], mzf_path[512];
    snprintf ( dsk_path, sizeof ( dsk_path ), "%s/big.dsk", s_tmpdir );
    snprintf ( mzf_path, sizeof ( mzf_path ), "%s/big.mzf", s_tmpdir );

    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dsk_path ) != EXIT_SUCCESS ) {
        remove ( dsk_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create disc" );
    }

    /* MZF s tělem 16*256 = 4096B = 16 bloků (max je 14 v mini režimu) */
    create_test_boot_mzf ( mzf_path, 4096 );

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );

    st_PANEL_BOOT_DATA data;
    memset ( &data, 0, sizeof ( data ) );
    data.preserve_fsmz = true;
    panel_boot_load ( &data, &disc, &detect );

    en_MZDSK_RES res = panel_boot_put_bottom ( &data, &disc, mzf_path );
    TEST_ASSERT ( res != MZDSK_RES_OK, "too large bootstrap should fail" );

    mzdsk_disc_close ( &disc );
    remove ( mzf_path ); remove ( dsk_path ); rmdir ( s_tmpdir );
    return 0;
}


/**
 * @brief Export bootstrapu na lokální disk musí vrátit OK a vytvořit MZF.
 *
 * Regrese: panel_boot_get_bootstrap() dříve otevíral paměťový handler
 * pro MZF s count_bytes=0 a bez swelling_enabled, čímž mzf_write_header()
 * selhal i tam, kde byla IPLPRO hlavička platná (typicky CP/M disk
 * s P-CP/M80 boot sektorem). GUI pak hlásilo "Failed to export bootstrap."
 * i když v CLI stejný export procházel.
 */
static int test_boot_get_bootstrap_export ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dsk_path[512], in_mzf[512], out_mzf[512];
    snprintf ( dsk_path, sizeof ( dsk_path ), "%s/exp.dsk", s_tmpdir );
    snprintf ( in_mzf,   sizeof ( in_mzf ),   "%s/in.mzf",  s_tmpdir );
    snprintf ( out_mzf,  sizeof ( out_mzf ),  "%s/out.mzf", s_tmpdir );

    st_MZDSK_DISC disc;
    if ( create_fsmz_disc ( &disc, dsk_path ) != EXIT_SUCCESS ) {
        remove ( dsk_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create disc" );
    }

    /* Nainstalovat bootstrap (256 B = 1 blok), aby byl IPLPRO platný. */
    if ( create_test_boot_mzf ( in_mzf, 256 ) != EXIT_SUCCESS ) {
        mzdsk_disc_close ( &disc );
        remove ( dsk_path ); rmdir ( s_tmpdir );
        TEST_FAIL ( "create in MZF" );
    }

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );

    st_PANEL_BOOT_DATA data;
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( panel_boot_put_bottom ( &data, &disc, in_mzf ), MZDSK_RES_OK );

    /* Export do out.mzf - tohle byl hlášený bug. */
    en_MZDSK_RES res = panel_boot_get_bootstrap ( &disc, out_mzf );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    /* Výstupní soubor musí existovat a být neprázdný. */
    FILE *f = fopen ( out_mzf, "rb" );
    TEST_ASSERT ( f != NULL, "output MZF file exists" );
    fseek ( f, 0, SEEK_END );
    long sz = ftell ( f );
    fclose ( f );
    TEST_ASSERT ( sz >= (long) ( MZF_HEADER_SIZE + 256 ), "output MZF has header + body" );

    mzdsk_disc_close ( &disc );
    remove ( out_mzf ); remove ( in_mzf ); remove ( dsk_path ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief CP/M disk: system tracks info. */
static int test_boot_cpm_system_tracks ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dsk_path[512];
    snprintf ( dsk_path, sizeof ( dsk_path ), "%s/cpm.dsk", s_tmpdir );

    /* vytvořit CP/M SD disk */
    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    mzdsk_tools_format_cpm_sd ( &h, 160, 2 );

    st_HANDLER fh;
    st_DRIVER fd;
    generic_driver_file_init ( &fd );
    generic_driver_open_file ( &fh, &fd, dsk_path, FILE_DRIVER_OPMODE_W );
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );

    st_MZDSK_DISC disc;
    mzdsk_disc_open_memory ( &disc, dsk_path, FILE_DRIVER_OPMODE_RO );

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );
    TEST_ASSERT_EQ_INT ( detect.type, MZDSK_FS_CPM );

    st_PANEL_BOOT_DATA data;
    memset ( &data, 0, sizeof ( data ) );
    panel_boot_load ( &data, &disc, &detect );

    TEST_ASSERT_EQ_INT ( data.has_system_tracks, 1 );
    TEST_ASSERT ( data.system_tracks_count > 0, "has system tracks" );
    TEST_ASSERT ( data.system_tracks_size > 0, "system tracks size > 0" );

    mzdsk_disc_close ( &disc );
    remove ( dsk_path ); rmdir ( s_tmpdir );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    memory_driver_init();
    TEST_INIT();

    RUN_TEST ( test_boot_load_empty );
    RUN_TEST ( test_boot_put_bottom_load );
    RUN_TEST ( test_boot_put_metadata );
    RUN_TEST ( test_boot_clear );
    RUN_TEST ( test_boot_dinfo );
    RUN_TEST ( test_boot_too_large );
    RUN_TEST ( test_boot_get_bootstrap_export );
    RUN_TEST ( test_boot_cpm_system_tracks );

    TEST_SUMMARY();
}
