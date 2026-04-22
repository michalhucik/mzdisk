/**
 * @file test_dragdrop.c
 * @brief Testy dnd_transfer_file - přenos souboru mezi sessions.
 *
 * Testuje same-FS (FSMZ->FSMZ) i cross-FS (FSMZ->CPM) přenos
 * přes dočasný MZF soubor, včetně copy a move variant.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "disk_session.h"
#include "dragdrop.h"
#include "config.h"
#include "panels/panel_fsmz.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzf/mzf.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


static char s_tmpdir[256];
static int setup_tmpdir ( void ) {
    strncpy ( s_tmpdir, "/tmp/mzdsk_test_dnd_XXXXXX", sizeof ( s_tmpdir ) );
    return mkdtemp ( s_tmpdir ) != NULL ? 0 : -1;
}


/**
 * @brief Vytvoří validní MZF soubor na cestě (128B hlavička + tělo).
 */
static int make_mzf_file ( const char *path, uint8_t ftype, const char *name,
                            const uint8_t *body, uint16_t body_len )
{
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    hdr.ftype = ftype;
    uint8_t *fname_buf = MZF_UINT8_FNAME ( hdr.fname );
    memset ( fname_buf, 0x0d, MZF_FILE_NAME_LENGTH );
    size_t nlen = strlen ( name );
    if ( nlen > MZF_FILE_NAME_LENGTH ) nlen = MZF_FILE_NAME_LENGTH;
    memcpy ( fname_buf, name, nlen );
    hdr.fsize = body_len;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;

    FILE *f = fopen ( path, "wb" );
    if ( !f ) return EXIT_FAILURE;
    if ( fwrite ( &hdr, sizeof ( hdr ), 1, f ) != 1 ) { fclose ( f ); return EXIT_FAILURE; }
    if ( body_len > 0 && fwrite ( body, 1, body_len, f ) != body_len ) {
        fclose ( f ); return EXIT_FAILURE;
    }
    fclose ( f );
    return EXIT_SUCCESS;
}


/**
 * @brief Vytvoří FSMZ DSK soubor s jedním MZF souborem uvnitř.
 *
 * Vytvoří prázdný FSMZ disk, otevře ho přes session API a vloží do něj
 * dočasný MZF soubor. Po zavření session se disk uloží.
 */
static int create_fsmz_with_file ( const char *dsk_path, const char *name,
                                    const uint8_t *body, uint16_t body_len )
{
    /* 1. formátovat prázdný FSMZ do souboru */
    st_HANDLER h = {0};
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    if ( mzdsk_tools_format_basic ( &h, 40, 1 ) != EXIT_SUCCESS ) {
        free ( h.spec.memspec.ptr );
        return EXIT_FAILURE;
    }
    st_HANDLER fh; st_DRIVER fd;
    generic_driver_file_init ( &fd );
    if ( !generic_driver_open_file ( &fh, &fd, (char *) dsk_path, FILE_DRIVER_OPMODE_W ) ) {
        free ( h.spec.memspec.ptr ); return EXIT_FAILURE;
    }
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );

    /* 2. vytvořit MZF soubor v /tmp */
    char mzf_path[512];
    snprintf ( mzf_path, sizeof ( mzf_path ), "%s.mzf", dsk_path );
    if ( make_mzf_file ( mzf_path, 0x01, name, body, body_len ) != EXIT_SUCCESS ) {
        return EXIT_FAILURE;
    }

    /* 3. otevřít DSK přes session, vložit MZF, uložit */
    /* static: nesmí být na stacku - dvě mgr naráz přetečou Windows 1MB stack */
    static st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    en_MZDSK_RES r = mzdisk_session_open ( &mgr, dsk_path );
    if ( r != MZDSK_RES_OK ) { remove ( mzf_path ); return EXIT_FAILURE; }

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );
    r = panel_fsmz_put_file ( &s->disc, mzf_path );
    remove ( mzf_path );
    if ( r != MZDSK_RES_OK ) {
        mzdisk_session_close_all ( &mgr );
        return EXIT_FAILURE;
    }
    r = mzdisk_session_save ( s );
    mzdisk_session_close_all ( &mgr );
    return ( r == MZDSK_RES_OK ) ? EXIT_SUCCESS : EXIT_FAILURE;
}


/**
 * @brief Vytvoří prázdný FSMZ DSK (bez souborů).
 */
static int create_empty_fsmz ( const char *dsk_path )
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
    if ( !generic_driver_open_file ( &fh, &fd, (char *) dsk_path, FILE_DRIVER_OPMODE_W ) ) {
        free ( h.spec.memspec.ptr ); return EXIT_FAILURE;
    }
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );
    return EXIT_SUCCESS;
}


/**
 * @brief Vytvoří prázdný CP/M SD DSK.
 */
static int create_empty_cpm ( const char *dsk_path )
{
    st_HANDLER h = {0};
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    if ( mzdsk_tools_format_cpm_sd ( &h, 160, 2 ) != EXIT_SUCCESS ) {
        free ( h.spec.memspec.ptr ); return EXIT_FAILURE;
    }
    st_HANDLER fh; st_DRIVER fd;
    generic_driver_file_init ( &fd );
    if ( !generic_driver_open_file ( &fh, &fd, (char *) dsk_path, FILE_DRIVER_OPMODE_W ) ) {
        free ( h.spec.memspec.ptr ); return EXIT_FAILURE;
    }
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );
    return EXIT_SUCCESS;
}


/* ===================================================================
 *  Testy
 * =================================================================== */


/** @brief NULL arguments - ochrana. */
static int test_dnd_null_args ( void )
{
    char err[256];
    en_MZDSK_RES r = dnd_transfer_file ( NULL, 0, NULL, false,
                                          MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_INVALID_PARAM );
    TEST_ASSERT ( err[0] != '\0', "err_msg should be populated" );
    return 0;
}


/** @brief Same-FS FSMZ -> FSMZ copy. */
static int test_dnd_fsmz_to_fsmz_copy ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/src.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/dst.dsk", s_tmpdir );

    uint8_t body[] = { 0xde, 0xad, 0xbe, 0xef, 0x42, 0x00, 0xff };
    if ( create_fsmz_with_file ( src, "HELLO", body, sizeof ( body ) ) != EXIT_SUCCESS )
        TEST_FAIL ( "create src" );
    if ( create_empty_fsmz ( dst ) != EXIT_SUCCESS )
        TEST_FAIL ( "create dst" );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    TEST_ASSERT_EQ_INT ( mzdisk_session_open ( &mgr, src ), MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( mzdisk_session_open ( &mgr, dst ), MZDSK_RES_OK );

    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );
    TEST_ASSERT_NOT_NULL ( s_src );
    TEST_ASSERT_NOT_NULL ( s_dst );
    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 1 );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 0 );

    /* copy */
    char err[256];
    en_MZDSK_RES r = dnd_transfer_file ( s_src, 0, s_dst, false,
                                          MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( s_dst->is_dirty, 1 );
    TEST_ASSERT_EQ_INT ( s_src->is_dirty, 0 );  /* copy nemění zdroj */

    /* reload panelů a ověřit, že cíl má soubor a zdroj ho zachoval */
    mzdisk_session_reload_panels ( s_dst );
    mzdisk_session_reload_panels ( s_src );
    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 1 );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 1 );

    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Same-FS FSMZ -> FSMZ move (soubor se ze zdroje smaže). */
static int test_dnd_fsmz_to_fsmz_move ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/src.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/dst.dsk", s_tmpdir );

    uint8_t body[] = { 0x01, 0x02, 0x03, 0x04 };
    if ( create_fsmz_with_file ( src, "MOVE", body, sizeof ( body ) ) != EXIT_SUCCESS )
        TEST_FAIL ( "create src" );
    if ( create_empty_fsmz ( dst ) != EXIT_SUCCESS )
        TEST_FAIL ( "create dst" );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );

    char err[256];
    en_MZDSK_RES r = dnd_transfer_file ( s_src, 0, s_dst, true,
                                          MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( s_src->is_dirty, 1 );  /* move změní zdroj */
    TEST_ASSERT_EQ_INT ( s_dst->is_dirty, 1 );

    mzdisk_session_reload_panels ( s_src );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 0 );  /* zdroj prázdný po move */
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 1 );

    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Cross-FS FSMZ -> CP/M copy. */
static int test_dnd_fsmz_to_cpm_copy ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/src.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/dst.dsk", s_tmpdir );

    uint8_t body[] = { 'H', 'i', '!', '\n' };
    if ( create_fsmz_with_file ( src, "X", body, sizeof ( body ) ) != EXIT_SUCCESS )
        TEST_FAIL ( "create src" );
    if ( create_empty_cpm ( dst ) != EXIT_SUCCESS )
        TEST_FAIL ( "create dst" );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );
    TEST_ASSERT_EQ_INT ( s_src->detect_result.type, MZDSK_FS_FSMZ );
    TEST_ASSERT_EQ_INT ( s_dst->detect_result.type, MZDSK_FS_CPM );

    char err[256];
    en_MZDSK_RES r = dnd_transfer_file ( s_src, 0, s_dst, false,
                                          MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( s_dst->is_dirty, 1 );

    /* Po reloadu má CP/M cíl 1 soubor */
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->cpm_data.file_count, 1 );

    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Index mimo rozsah = INVALID_PARAM, cíl beze změny. */
static int test_dnd_invalid_index ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/s.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/d.dsk", s_tmpdir );

    uint8_t body[] = { 1, 2, 3 };
    create_fsmz_with_file ( src, "A", body, sizeof ( body ) );
    create_empty_fsmz ( dst );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );

    char err[256];
    en_MZDSK_RES r = dnd_transfer_file ( s_src, 99, s_dst, false,
                                          MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_INVALID_PARAM );
    TEST_ASSERT_EQ_INT ( s_dst->is_dirty, 0 );  /* cíl se nezměnil */

    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief fill_payload z single-klik (clicked není v selected). */
static int test_dnd_fill_payload_single ( void )
{
    st_DND_FILE_PAYLOAD p;
    bool sel[5] = { false, false, false, false, false };

    /* ne-selected klik -> jen clicked index */
    dnd_fill_payload ( &p, 42, MZDSK_FS_FSMZ, 3, sel, 5 );
    TEST_ASSERT_EQ_INT ( (int) p.source_session_id, 42 );
    TEST_ASSERT_EQ_INT ( p.fs_type, MZDSK_FS_FSMZ );
    TEST_ASSERT_EQ_INT ( p.count, 1 );
    TEST_ASSERT_EQ_INT ( p.file_indices[0], 3 );
    return 0;
}


/** @brief fill_payload z multi-select (clicked je v selected). */
static int test_dnd_fill_payload_multi ( void )
{
    st_DND_FILE_PAYLOAD p;
    bool sel[6] = { true, false, true, false, true, false };

    /* clicked=2 je vybraný -> všechny selected (0, 2, 4) */
    dnd_fill_payload ( &p, 7, MZDSK_FS_CPM, 2, sel, 6 );
    TEST_ASSERT_EQ_INT ( p.count, 3 );
    TEST_ASSERT_EQ_INT ( p.file_indices[0], 0 );
    TEST_ASSERT_EQ_INT ( p.file_indices[1], 2 );
    TEST_ASSERT_EQ_INT ( p.file_indices[2], 4 );

    /* clicked=1 není vybraný -> jen clicked */
    dnd_fill_payload ( &p, 7, MZDSK_FS_CPM, 1, sel, 6 );
    TEST_ASSERT_EQ_INT ( p.count, 1 );
    TEST_ASSERT_EQ_INT ( p.file_indices[0], 1 );
    return 0;
}


/** @brief Multi-file transfer: 2 FSMZ soubory -> prázdný FSMZ. */
static int test_dnd_multi_transfer ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/ms.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/md.dsk", s_tmpdir );

    uint8_t b1[] = { 0xaa };
    uint8_t b2[] = { 0xbb, 0xcc };
    create_fsmz_with_file ( src, "A", b1, sizeof ( b1 ) );
    /* do téhož souboru dopíšeme další MZF */
    {
        /* static: nesmí být na stacku vedle vnější mgr */
        static st_MZDISK_SESSION_MANAGER m; mzdisk_session_manager_init ( &m );
        mzdisk_session_open ( &m, src );
        char mp[600];
        snprintf ( mp, sizeof ( mp ), "%s.2.mzf", src );
        make_mzf_file ( mp, 0x01, "B", b2, sizeof ( b2 ) );
        panel_fsmz_put_file ( &mzdisk_session_get_active ( &m )->disc, mp );
        mzdisk_session_save ( mzdisk_session_get_active ( &m ) );
        mzdisk_session_close_all ( &m );
        remove ( mp );
    }
    create_empty_fsmz ( dst );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );
    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 2 );

    /* zavolat dnd_handle_drop přímo s multi-payloadem */
    dnd_init ( &mgr );
    st_DND_FILE_PAYLOAD payload;
    payload.source_session_id = s_src->id;
    payload.fs_type = MZDSK_FS_FSMZ;
    payload.count = 2;
    payload.file_indices[0] = 0;
    payload.file_indices[1] = 1;

    char err[256];
    en_MZDSK_RES r = dnd_handle_drop ( &payload, s_dst->id, false,
                                        MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 2 );  /* oba zkopírovány */
    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 2 );  /* copy - zdroj zachován */

    dnd_init ( NULL );
    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/**
 * @brief End-to-end multi-select: selected[] -> dnd_fill_payload -> dnd_handle_drop.
 *
 * Simuluje přesně to, co dělá GUI: uživatel označí checkboxem víc souborů
 * (selected[]), pustí drag, payload se naplní přes dnd_fill_payload a drop
 * projde handle_drop. Tento test odhalí potenciální rozpor mezi
 * fill_payload a skutečným chováním drop handleru.
 */
static int test_dnd_multi_select_end_to_end ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/es.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/ed.dsk", s_tmpdir );

    /* vytvořit FSMZ src se 3 soubory */
    uint8_t b1[] = { 1 };
    create_fsmz_with_file ( src, "A", b1, sizeof ( b1 ) );
    {
        static st_MZDISK_SESSION_MANAGER m;
        mzdisk_session_manager_init ( &m );
        mzdisk_session_open ( &m, src );
        char mp[600];
        for ( int k = 0; k < 2; k++ ) {
            snprintf ( mp, sizeof ( mp ), "%s.%d.mzf", src, k );
            uint8_t body[2] = { (uint8_t) ( 0xB0 + k ), 0 };
            const char *n = ( k == 0 ) ? "B" : "C";
            make_mzf_file ( mp, 0x01, n, body, sizeof ( body ) );
            panel_fsmz_put_file ( &mzdisk_session_get_active ( &m )->disc, mp );
            remove ( mp );
        }
        mzdisk_session_save ( mzdisk_session_get_active ( &m ) );
        mzdisk_session_close_all ( &m );
    }
    create_empty_fsmz ( dst );

    static st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );
    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 3 );

    dnd_init ( &mgr );

    /* Uživatel označí všechny 3 soubory checkboxem */
    s_src->fsmz_data.selected[0] = true;
    s_src->fsmz_data.selected[1] = true;
    s_src->fsmz_data.selected[2] = true;

    /* Drag-startuje z řádku 0 (který je vybrán) */
    st_DND_FILE_PAYLOAD payload;
    dnd_fill_payload ( &payload, s_src->id, MZDSK_FS_FSMZ,
                        0, s_src->fsmz_data.selected,
                        s_src->fsmz_data.file_count );

    /* Ověř, že payload obsahuje všechny 3 */
    TEST_ASSERT_EQ_INT ( payload.count, 3 );
    TEST_ASSERT_EQ_INT ( payload.file_indices[0], 0 );
    TEST_ASSERT_EQ_INT ( payload.file_indices[1], 1 );
    TEST_ASSERT_EQ_INT ( payload.file_indices[2], 2 );

    /* Drop */
    char err[256];
    en_MZDSK_RES r = dnd_handle_drop ( &payload, s_dst->id, false,
                                        MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );

    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 3 );

    dnd_init ( NULL );
    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/**
 * @brief Cross-FS multi-select: FSMZ -> prázdný CP/M, 3 soubory najednou.
 */
static int test_dnd_multi_select_cross_fs ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/xs.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/xd.dsk", s_tmpdir );

    uint8_t b1[] = { 1 };
    create_fsmz_with_file ( src, "AAA", b1, sizeof ( b1 ) );
    {
        static st_MZDISK_SESSION_MANAGER m;
        mzdisk_session_manager_init ( &m );
        mzdisk_session_open ( &m, src );
        char mp[600];
        const char *names[] = { "BBB", "CCC" };
        for ( int k = 0; k < 2; k++ ) {
            snprintf ( mp, sizeof ( mp ), "%s.%d.mzf", src, k );
            uint8_t body[2] = { (uint8_t) ( 0xC0 + k ), 0 };
            make_mzf_file ( mp, 0x01, names[k], body, sizeof ( body ) );
            panel_fsmz_put_file ( &mzdisk_session_get_active ( &m )->disc, mp );
            remove ( mp );
        }
        mzdisk_session_save ( mzdisk_session_get_active ( &m ) );
        mzdisk_session_close_all ( &m );
    }
    create_empty_cpm ( dst );

    static st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );
    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 3 );
    TEST_ASSERT_EQ_INT ( s_dst->detect_result.type, MZDSK_FS_CPM );

    dnd_init ( &mgr );

    /* Vybrat všechny 3 */
    for ( int i = 0; i < 3; i++ ) s_src->fsmz_data.selected[i] = true;

    st_DND_FILE_PAYLOAD payload;
    dnd_fill_payload ( &payload, s_src->id, MZDSK_FS_FSMZ,
                        0, s_src->fsmz_data.selected,
                        s_src->fsmz_data.file_count );
    TEST_ASSERT_EQ_INT ( payload.count, 3 );

    char err[256];
    en_MZDSK_RES r = dnd_handle_drop ( &payload, s_dst->id, false,
                                        MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );

    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->cpm_data.file_count, 3 );

    dnd_init ( NULL );
    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/**
 * @brief Multi-select MOVE: všechny vybrané soubory přeneseny a smazány ze zdroje.
 */
static int test_dnd_multi_select_move ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/mvs.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/mvd.dsk", s_tmpdir );

    uint8_t b1[] = { 1 };
    create_fsmz_with_file ( src, "ONE", b1, sizeof ( b1 ) );
    {
        static st_MZDISK_SESSION_MANAGER m;
        mzdisk_session_manager_init ( &m );
        mzdisk_session_open ( &m, src );
        char mp[600];
        snprintf ( mp, sizeof ( mp ), "%s.x.mzf", src );
        uint8_t b2[] = { 2 };
        make_mzf_file ( mp, 0x01, "TWO", b2, sizeof ( b2 ) );
        panel_fsmz_put_file ( &mzdisk_session_get_active ( &m )->disc, mp );
        mzdisk_session_save ( mzdisk_session_get_active ( &m ) );
        mzdisk_session_close_all ( &m );
        remove ( mp );
    }
    create_empty_fsmz ( dst );

    static st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );
    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 2 );

    dnd_init ( &mgr );

    /* Vybrat oba soubory */
    s_src->fsmz_data.selected[0] = true;
    s_src->fsmz_data.selected[1] = true;

    st_DND_FILE_PAYLOAD payload;
    dnd_fill_payload ( &payload, s_src->id, MZDSK_FS_FSMZ,
                        1, s_src->fsmz_data.selected,
                        s_src->fsmz_data.file_count );
    TEST_ASSERT_EQ_INT ( payload.count, 2 );

    /* Move */
    char err[256];
    en_MZDSK_RES r = dnd_handle_drop ( &payload, s_dst->id, true,
                                        MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );

    TEST_ASSERT_EQ_INT ( s_src->fsmz_data.file_count, 0 );   /* zdroj prázdný */
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 2 );   /* cíl má oba */

    dnd_init ( NULL );
    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief CP/M drop se stejným jménem - RENAME přidá ~1 suffix. */
static int test_dnd_cpm_rename_on_conflict ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/rs.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/rd.dsk", s_tmpdir );

    uint8_t body[] = { 'A' };
    create_fsmz_with_file ( src, "DUP", body, sizeof ( body ) );
    create_empty_cpm ( dst );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );

    /* první transfer */
    char err[256];
    en_MZDSK_RES r = dnd_transfer_file ( s_src, 0, s_dst, false,
                                          MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->cpm_data.file_count, 1 );

    /* druhý transfer stejného souboru - FILE_EXISTS -> RENAME */
    r = dnd_transfer_file ( s_src, 0, s_dst, false,
                             MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->cpm_data.file_count, 2 );  /* oba souborky */

    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief CP/M drop se stejným jménem - SKIP vrátí OK bez změny. */
static int test_dnd_cpm_skip_on_conflict ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/ss.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/sd.dsk", s_tmpdir );

    uint8_t body[] = { 'A' };
    create_fsmz_with_file ( src, "DUP", body, sizeof ( body ) );
    create_empty_cpm ( dst );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );

    /* první transfer */
    char err[256];
    dnd_transfer_file ( s_src, 0, s_dst, false, MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->cpm_data.file_count, 1 );

    /* druhý transfer - SKIP */
    en_MZDSK_RES r = dnd_transfer_file ( s_src, 0, s_dst, false,
                                          MZDSK_EXPORT_DUP_SKIP, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );  /* SKIP = OK but noop */
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->cpm_data.file_count, 1 );  /* nepřibyl */

    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/**
 * @brief FSMZ OVERWRITE: druhý put se stejným jménem přepíše původní.
 */
static int test_dnd_fsmz_overwrite_on_conflict ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/os.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/od.dsk", s_tmpdir );

    uint8_t body[] = { 0x42 };
    create_fsmz_with_file ( src, "DUP", body, sizeof ( body ) );
    create_empty_fsmz ( dst );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );

    /* první transfer - čistý put */
    char err[256];
    en_MZDSK_RES r = dnd_transfer_file ( s_src, 0, s_dst, false,
                                          MZDSK_EXPORT_DUP_OVERWRITE, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 1 );

    /* druhý transfer se stejným jménem - OVERWRITE smaže existující a zapíše */
    r = dnd_transfer_file ( s_src, 0, s_dst, false,
                             MZDSK_EXPORT_DUP_OVERWRITE, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 1 );  /* stále 1 (přepsáno) */

    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/**
 * @brief CP/M OVERWRITE: druhý put se stejným jménem přepíše existující.
 */
static int test_dnd_cpm_overwrite_on_conflict ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/cos.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/cod.dsk", s_tmpdir );

    uint8_t body[] = { 0x11 };
    create_fsmz_with_file ( src, "XXX", body, sizeof ( body ) );
    create_empty_cpm ( dst );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );

    /* první put */
    char err[256];
    en_MZDSK_RES r = dnd_transfer_file ( s_src, 0, s_dst, false,
                                          MZDSK_EXPORT_DUP_OVERWRITE, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->cpm_data.file_count, 1 );

    /* druhý put - OVERWRITE smaže původní a zapíše znovu */
    r = dnd_transfer_file ( s_src, 0, s_dst, false,
                             MZDSK_EXPORT_DUP_OVERWRITE, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->cpm_data.file_count, 1 );  /* přepsáno, ne duplikováno */

    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/**
 * @brief ASK flow: první konflikt suspendne, user_chose_ask(OVERWRITE)
 *        přepíše a pokračuje.
 */
static int test_dnd_ask_overwrite ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/as.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/ad.dsk", s_tmpdir );

    uint8_t body[] = { 0x55 };
    create_fsmz_with_file ( src, "ASK", body, sizeof ( body ) );
    create_empty_fsmz ( dst );

    static st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );

    dnd_init ( &mgr );

    /* Nejdřív naplníme cíl souborem (bez ASK, standardní put). */
    char err[256];
    dnd_transfer_file ( s_src, 0, s_dst, false, MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 1 );

    /* Druhý drop s ASK mode na stejný soubor */
    st_DND_FILE_PAYLOAD p;
    p.source_session_id = s_src->id;
    p.fs_type = MZDSK_FS_FSMZ;
    p.count = 1;
    p.file_indices[0] = 0;

    en_MZDSK_RES r = dnd_handle_drop ( &p, s_dst->id, false,
                                        MZDSK_EXPORT_DUP_ASK, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( r, MZDSK_RES_OK );  /* ASK suspended = OK (pending) */
    TEST_ASSERT_EQ_INT ( dnd_has_pending_ask (), 1 );
    TEST_ASSERT_EQ_INT ( dnd_get_ask_remaining_count (), 1 );
    TEST_ASSERT_NOT_NULL ( dnd_get_ask_conflict_name () );

    /* User klikne Overwrite (apply_all=true = propagovat na zbytek). */
    dnd_user_chose_ask ( MZDSK_EXPORT_DUP_OVERWRITE, true );

    TEST_ASSERT_EQ_INT ( dnd_has_pending_ask (), 0 );
    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 1 );  /* přepsáno, ne duplikováno */

    dnd_init ( NULL );
    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


/**
 * @brief ASK Cancel_all: pending se zruší, cíl beze změny pro zbytek.
 */
static int test_dnd_ask_cancel_all ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char src[512], dst[512];
    snprintf ( src, sizeof ( src ), "%s/cas.dsk", s_tmpdir );
    snprintf ( dst, sizeof ( dst ), "%s/cad.dsk", s_tmpdir );

    uint8_t body[] = { 0x77 };
    create_fsmz_with_file ( src, "CNC", body, sizeof ( body ) );
    create_empty_fsmz ( dst );

    static st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, src );
    mzdisk_session_open ( &mgr, dst );
    st_MZDISK_SESSION *s_src = mzdisk_session_get_by_id ( &mgr, 1 );
    st_MZDISK_SESSION *s_dst = mzdisk_session_get_by_id ( &mgr, 2 );

    dnd_init ( &mgr );

    /* Napřed naplnit cíl */
    char err[256];
    dnd_transfer_file ( s_src, 0, s_dst, false, MZDSK_EXPORT_DUP_RENAME, -1, err, sizeof ( err ) );

    /* ASK drop - dojde ke konfliktu */
    st_DND_FILE_PAYLOAD p;
    p.source_session_id = s_src->id;
    p.fs_type = MZDSK_FS_FSMZ;
    p.count = 1;
    p.file_indices[0] = 0;
    dnd_handle_drop ( &p, s_dst->id, false, MZDSK_EXPORT_DUP_ASK, -1, err, sizeof ( err ) );
    TEST_ASSERT_EQ_INT ( dnd_has_pending_ask (), 1 );

    /* Cancel - nová API (dříve reprezentováno MZDSK_EXPORT_DUP_ASK). */
    dnd_cancel_ask ();
    TEST_ASSERT_EQ_INT ( dnd_has_pending_ask (), 0 );

    mzdisk_session_reload_panels ( s_dst );
    TEST_ASSERT_EQ_INT ( s_dst->fsmz_data.file_count, 1 );  /* beze změny */

    dnd_init ( NULL );
    mzdisk_session_close_all ( &mgr );
    remove ( src ); remove ( dst ); rmdir ( s_tmpdir );
    return 0;
}


int main ( void )
{
    memory_driver_init();
    TEST_INIT();

    RUN_TEST ( test_dnd_null_args );
    RUN_TEST ( test_dnd_fsmz_to_fsmz_copy );
    RUN_TEST ( test_dnd_fsmz_to_fsmz_move );
    RUN_TEST ( test_dnd_fsmz_to_cpm_copy );
    RUN_TEST ( test_dnd_invalid_index );
    RUN_TEST ( test_dnd_fill_payload_single );
    RUN_TEST ( test_dnd_fill_payload_multi );
    RUN_TEST ( test_dnd_multi_transfer );
    RUN_TEST ( test_dnd_multi_select_end_to_end );
    RUN_TEST ( test_dnd_multi_select_cross_fs );
    RUN_TEST ( test_dnd_multi_select_move );
    RUN_TEST ( test_dnd_cpm_rename_on_conflict );
    RUN_TEST ( test_dnd_cpm_skip_on_conflict );
    RUN_TEST ( test_dnd_fsmz_overwrite_on_conflict );
    RUN_TEST ( test_dnd_cpm_overwrite_on_conflict );
    RUN_TEST ( test_dnd_ask_overwrite );
    RUN_TEST ( test_dnd_ask_cancel_all );

    TEST_SUMMARY();
}
