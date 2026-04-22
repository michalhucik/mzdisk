/**
 * @file test_panel_cpm.c
 * @brief Testy GUI CP/M operací přes panel vrstvu.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "panels/panel_cpm.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


static char s_tmpdir[256];
static int setup_tmpdir ( void ) {
    strncpy ( s_tmpdir, "/tmp/mzdsk_test_gcpm_XXXXXX", sizeof ( s_tmpdir ) );
    return mkdtemp ( s_tmpdir ) != NULL ? 0 : -1;
}

static int create_cpm_disc ( st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect, const char *path )
{
    st_HANDLER h = {0};
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc(1,1);
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    if ( mzdsk_tools_format_cpm_sd ( &h, 160, 2 ) != EXIT_SUCCESS ) {
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
    mzdsk_detect_filesystem ( disc, detect );
    return ( detect->type == MZDSK_FS_CPM ) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/** @brief Vytvoří testovací binární soubor. */
static int create_raw_file ( const char *path, uint16_t size )
{
    FILE *f = fopen ( path, "wb" );
    if ( !f ) return EXIT_FAILURE;
    for ( uint16_t i = 0; i < size; i++ ) fputc ( i & 0xFF, f );
    fclose ( f );
    return EXIT_SUCCESS;
}


/** @brief Load adresáře + alloc mapy. */
static int test_gui_cpm_load ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512]; snprintf ( dp, sizeof(dp), "%s/c.dsk", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }

    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.is_loaded, 1 );
    TEST_ASSERT_EQ_INT ( data.file_count, 0 );

    mzdsk_disc_close ( &disc ); remove(dp); rmdir(s_tmpdir);
    return 0;
}

/** @brief Put -> load -> soubor v adresáři. */
static int test_gui_cpm_put ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512];
    snprintf ( dp, sizeof(dp), "%s/cp.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/TEST.COM", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_raw_file ( fp, 256 );

    en_MZDSK_RES res = panel_cpm_put_file ( &disc, &detect.cpm_dpb, fp, "TEST", "COM", 0 );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );

    mzdsk_disc_close ( &disc ); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}

/** @brief Delete -> soubor zmizí. */
static int test_gui_cpm_delete ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512];
    snprintf ( dp, sizeof(dp), "%s/cd.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/DEL.COM", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_raw_file ( fp, 256 );
    panel_cpm_put_file ( &disc, &detect.cpm_dpb, fp, "DEL", "COM", 0 );

    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );

    en_MZDSK_RES res = panel_cpm_delete_file ( &disc, &detect.cpm_dpb, &data.files[0] );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    panel_cpm_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 0 );

    mzdsk_disc_close ( &disc ); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}

/** @brief Rename soubor. */
static int test_gui_cpm_rename ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512];
    snprintf ( dp, sizeof(dp), "%s/cr.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/OLD.COM", s_tmpdir );
    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_raw_file ( fp, 128 );
    panel_cpm_put_file ( &disc, &detect.cpm_dpb, fp, "OLD", "COM", 0 );

    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );

    en_MZDSK_RES res = panel_cpm_rename_file ( &disc, &detect.cpm_dpb, &data.files[0], "NEW", "COM" );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    mzdsk_disc_close ( &disc ); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


/** @brief Inicializace MZF export parametrů na výchozí hodnoty. */
static int test_gui_cpm_mzf_export_init ( void )
{
    st_PANEL_CPM_MZF_EXPORT opts;
    memset ( &opts, 0xFF, sizeof ( opts ) );
    panel_cpm_mzf_export_init ( &opts );

    TEST_ASSERT_EQ_INT ( opts.ftype, 0x22 );
    TEST_ASSERT_EQ_INT ( opts.exec_addr, 0x0100 );
    TEST_ASSERT_EQ_INT ( opts.strt_addr, 0x0100 );
    TEST_ASSERT_EQ_INT ( opts.encode_attrs, 1 );
    TEST_ASSERT_EQ_INT ( opts.show_opts, 0 );
    TEST_ASSERT_EQ_INT ( opts.open_file_dlg, 0 );

    return 0;
}


/** @brief _ex s výchozími parametry dá shodný výstup jako starý _get_file_mzf. */
static int test_gui_cpm_get_mzf_ex_default ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512], mzf1[512], mzf2[512];
    snprintf ( dp, sizeof(dp), "%s/mzfex.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/EX.COM", s_tmpdir );
    snprintf ( mzf1, sizeof(mzf1), "%s/old.mzf", s_tmpdir );
    snprintf ( mzf2, sizeof(mzf2), "%s/new.mzf", s_tmpdir );

    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_raw_file ( fp, 512 );
    panel_cpm_put_file ( &disc, &detect.cpm_dpb, fp, "EX", "COM", 0 );

    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );

    /* Starý export */
    TEST_ASSERT_EQ_INT ( panel_cpm_get_file_mzf ( &disc, &detect.cpm_dpb, &data.files[0], mzf1 ), MZDSK_RES_OK );

    /* Nový export s výchozími parametry */
    st_PANEL_CPM_MZF_EXPORT opts;
    panel_cpm_mzf_export_init ( &opts );
    TEST_ASSERT_EQ_INT ( panel_cpm_get_file_mzf_ex ( &disc, &detect.cpm_dpb, &data.files[0], mzf2, &opts ), MZDSK_RES_OK );

    /* Porovnat výstupní soubory - musí být shodné */
    FILE *f1 = fopen ( mzf1, "rb" ); TEST_ASSERT_NOT_NULL ( f1 );
    FILE *f2 = fopen ( mzf2, "rb" ); TEST_ASSERT_NOT_NULL ( f2 );
    fseek ( f1, 0, SEEK_END ); long s1 = ftell ( f1 ); rewind ( f1 );
    fseek ( f2, 0, SEEK_END ); long s2 = ftell ( f2 ); rewind ( f2 );
    TEST_ASSERT_EQ_INT ( (int) s1, (int) s2 );

    uint8_t *b1 = (uint8_t *) malloc ( (size_t) s1 );
    uint8_t *b2 = (uint8_t *) malloc ( (size_t) s2 );
    fread ( b1, 1, (size_t) s1, f1 ); fclose ( f1 );
    fread ( b2, 1, (size_t) s2, f2 ); fclose ( f2 );
    TEST_ASSERT_EQ_MEM ( b1, b2, (size_t) s1 );
    free ( b1 ); free ( b2 );

    mzdsk_disc_close ( &disc ); remove(mzf1); remove(mzf2); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


/** @brief _ex s ftype=0x01 -> ftype v MZF hlavičce = 0x01, fstrt = strt_addr. */
static int test_gui_cpm_get_mzf_ex_ftype ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512], mzf[512];
    snprintf ( dp, sizeof(dp), "%s/mzfft.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/FT.COM", s_tmpdir );
    snprintf ( mzf, sizeof(mzf), "%s/ft.mzf", s_tmpdir );

    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_raw_file ( fp, 256 );
    panel_cpm_put_file ( &disc, &detect.cpm_dpb, fp, "FT", "COM", 0 );

    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );

    st_PANEL_CPM_MZF_EXPORT opts;
    panel_cpm_mzf_export_init ( &opts );
    opts.ftype = 0x01;
    opts.strt_addr = 0x1200;
    opts.exec_addr = 0x4000;
    TEST_ASSERT_EQ_INT ( panel_cpm_get_file_mzf_ex ( &disc, &detect.cpm_dpb, &data.files[0], mzf, &opts ), MZDSK_RES_OK );

    /* Přečíst hlavičku a ověřit */
    FILE *f = fopen ( mzf, "rb" ); TEST_ASSERT_NOT_NULL ( f );
    uint8_t hdr[128];
    TEST_ASSERT_EQ_INT ( (int) fread ( hdr, 1, 128, f ), 128 );
    fclose ( f );

    TEST_ASSERT_EQ_INT ( hdr[0], 0x01 ); /* ftype */
    /* fstrt (LE) na offsetu 0x14-0x15 = 0x1200 */
    TEST_ASSERT_EQ_INT ( hdr[0x14] | ( hdr[0x15] << 8 ), 0x1200 );
    /* fexec (LE) na offsetu 0x16-0x17 = 0x4000 */
    TEST_ASSERT_EQ_INT ( hdr[0x16] | ( hdr[0x17] << 8 ), 0x4000 );

    mzdsk_disc_close ( &disc ); remove(mzf); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


/** @brief put_file_mzf_ex s override přepíše jméno z MZF hlavičky. */
static int test_gui_cpm_put_mzf_ex_override ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512], mzf[512];
    snprintf ( dp, sizeof(dp), "%s/put_ov.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/SRC.BIN", s_tmpdir );
    snprintf ( mzf, sizeof(mzf), "%s/src.mzf", s_tmpdir );

    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_raw_file ( fp, 128 );

    /* Nejprve put a get-mzf aby vznikl validní MZF se jménem SRC.BIN. */
    panel_cpm_put_file ( &disc, &detect.cpm_dpb, fp, "SRC", "BIN", 0 );
    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );
    panel_cpm_get_file_mzf ( &disc, &detect.cpm_dpb, &data.files[0], mzf );

    /* Smažeme SRC.BIN z disku a naimportujeme MZF s override jménem. */
    panel_cpm_delete_file ( &disc, &detect.cpm_dpb, &data.files[0] );

    en_MZDSK_RES res = panel_cpm_put_file_mzf_ex ( &disc, &detect.cpm_dpb, mzf, "OTHER", "DAT", 0 );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    panel_cpm_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );
    TEST_ASSERT_EQ_STR ( data.files[0].filename, "OTHER" );
    TEST_ASSERT_EQ_STR ( data.files[0].extension, "DAT" );

    mzdsk_disc_close ( &disc ); remove(mzf); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


/** @brief put_file_mzf_ex s NULL override chová se jako put_file_mzf. */
static int test_gui_cpm_put_mzf_ex_null_override ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512], mzf[512];
    snprintf ( dp, sizeof(dp), "%s/put_null.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/KEEP.BIN", s_tmpdir );
    snprintf ( mzf, sizeof(mzf), "%s/keep.mzf", s_tmpdir );

    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_raw_file ( fp, 128 );
    panel_cpm_put_file ( &disc, &detect.cpm_dpb, fp, "KEEP", "BIN", 0 );
    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );
    panel_cpm_get_file_mzf ( &disc, &detect.cpm_dpb, &data.files[0], mzf );
    panel_cpm_delete_file ( &disc, &detect.cpm_dpb, &data.files[0] );

    /* NULL override -> jméno z MZF hlavičky (KEEP.BIN). */
    en_MZDSK_RES res = panel_cpm_put_file_mzf_ex ( &disc, &detect.cpm_dpb, mzf, NULL, NULL, 0 );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    panel_cpm_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.file_count, 1 );
    TEST_ASSERT_EQ_STR ( data.files[0].filename, "KEEP" );
    TEST_ASSERT_EQ_STR ( data.files[0].extension, "BIN" );

    mzdsk_disc_close ( &disc ); remove(mzf); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


/** @brief _ex s encode_attrs=false -> bity 7 v fname = 0. */
static int test_gui_cpm_get_mzf_ex_no_attrs ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char dp[512], fp[512], mzf[512];
    snprintf ( dp, sizeof(dp), "%s/mzfna.dsk", s_tmpdir );
    snprintf ( fp, sizeof(fp), "%s/NA.COM", s_tmpdir );
    snprintf ( mzf, sizeof(mzf), "%s/na.mzf", s_tmpdir );

    st_MZDSK_DISC disc; st_MZDSK_DETECT_RESULT detect;
    if ( create_cpm_disc ( &disc, &detect, dp ) ) { remove(dp); rmdir(s_tmpdir); TEST_FAIL("disc"); }
    create_raw_file ( fp, 128 );
    panel_cpm_put_file ( &disc, &detect.cpm_dpb, fp, "NA", "COM", 0 );

    /* Nastavit R/O atribut */
    st_PANEL_CPM_DATA data;
    memset ( &data, 0, sizeof(data) );
    panel_cpm_load ( &data, &disc, &detect );
    panel_cpm_set_attrs ( &disc, &detect.cpm_dpb, &data.files[0], MZDSK_CPM_ATTR_READ_ONLY );
    panel_cpm_load ( &data, &disc, &detect );
    TEST_ASSERT_EQ_INT ( data.files[0].read_only, 1 );

    st_PANEL_CPM_MZF_EXPORT opts;
    panel_cpm_mzf_export_init ( &opts );
    opts.encode_attrs = false;
    TEST_ASSERT_EQ_INT ( panel_cpm_get_file_mzf_ex ( &disc, &detect.cpm_dpb, &data.files[0], mzf, &opts ), MZDSK_RES_OK );

    /* Přečíst hlavičku - bity 7 na offsetech 0x0A-0x0C musí být 0 */
    FILE *f = fopen ( mzf, "rb" ); TEST_ASSERT_NOT_NULL ( f );
    uint8_t hdr[128];
    fread ( hdr, 1, 128, f ); fclose ( f );

    TEST_ASSERT_EQ_INT ( hdr[0x0A] & 0x80, 0 ); /* R/O bit = 0 i přesto, že soubor je R/O */
    TEST_ASSERT_EQ_INT ( hdr[0x0B] & 0x80, 0 );
    TEST_ASSERT_EQ_INT ( hdr[0x0C] & 0x80, 0 );

    mzdsk_disc_close ( &disc ); remove(mzf); remove(fp); remove(dp); rmdir(s_tmpdir);
    return 0;
}


int main ( void )
{
    memory_driver_init();
    TEST_INIT();
    RUN_TEST ( test_gui_cpm_load );
    RUN_TEST ( test_gui_cpm_put );
    RUN_TEST ( test_gui_cpm_delete );
    RUN_TEST ( test_gui_cpm_rename );

    /* MZF export */
    RUN_TEST ( test_gui_cpm_mzf_export_init );
    RUN_TEST ( test_gui_cpm_get_mzf_ex_default );
    RUN_TEST ( test_gui_cpm_get_mzf_ex_ftype );
    RUN_TEST ( test_gui_cpm_get_mzf_ex_no_attrs );

    /* Put MZF ex */
    RUN_TEST ( test_gui_cpm_put_mzf_ex_override );
    RUN_TEST ( test_gui_cpm_put_mzf_ex_null_override );

    TEST_SUMMARY();
}
