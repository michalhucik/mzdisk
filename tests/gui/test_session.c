/**
 * @file test_session.c
 * @brief Testy session lifecycle - otevření, zavření, reload, přepínání.
 *
 * Kompiluje disk_session.c + všechny panel .c soubory.
 * Testuje celý GUI workflow bez ImGui závislostí.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "disk_session.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


static char s_tmpdir[256];
static int setup_tmpdir ( void ) {
    strncpy ( s_tmpdir, "/tmp/mzdsk_test_sess_XXXXXX", sizeof ( s_tmpdir ) );
    return mkdtemp ( s_tmpdir ) != NULL ? 0 : -1;
}

/**
 * @brief Vytvoří DSK soubor na disku z paměťového obrazu.
 */
static int create_dsk_file ( const char *path, int ( *format_fn )( st_HANDLER *, uint8_t, uint8_t ),
                              uint8_t tracks, uint8_t sides )
{
    st_HANDLER h = {0};
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    if ( format_fn ( &h, tracks, sides ) != EXIT_SUCCESS ) {
        free ( h.spec.memspec.ptr ); return EXIT_FAILURE;
    }
    st_HANDLER fh; st_DRIVER fd;
    generic_driver_file_init ( &fd );
    if ( !generic_driver_open_file ( &fh, &fd, (char *) path, FILE_DRIVER_OPMODE_W ) ) {
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


/** @brief Init: active_id = 0, count = 0, next_id = 1. */
static int test_session_manager_init ( void )
{
    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    TEST_ASSERT_EQ_INT ( (int) mgr.active_id, 0 );
    TEST_ASSERT_EQ_INT ( (int) mgr.next_id, 1 );
    TEST_ASSERT_EQ_INT ( mgr.count, 0 );
    TEST_ASSERT_NULL ( mzdisk_session_get_active ( &mgr ) );
    return 0;
}


/** @brief Open FSMZ disk -> close -> cleanup. */
static int test_session_open_close ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/s1.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_basic, 80, 2 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );

    en_MZDSK_RES res = mzdisk_session_open ( &mgr, p );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( mgr.count, 1 );

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );
    TEST_ASSERT_NOT_NULL ( s );
    TEST_ASSERT_EQ_INT ( s->is_open, 1 );
    TEST_ASSERT_EQ_INT ( s->has_disk, 1 );   /* LOADED stav */
    /* první otevřená session dostane id 1 a stane se primární */
    TEST_ASSERT_EQ_INT ( (int) s->id, 1 );
    TEST_ASSERT_EQ_INT ( (int) mgr.active_id, 1 );
    TEST_ASSERT_EQ_INT ( s->is_primary, 1 );

    uint64_t id = s->id;
    mzdisk_session_close_by_id ( &mgr, id );
    TEST_ASSERT_EQ_INT ( mgr.count, 0 );
    TEST_ASSERT_EQ_INT ( (int) mgr.active_id, 0 );

    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Open FSMZ -> detect_result.type = FSMZ. */
static int test_session_detect_fsmz ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/sf.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_basic, 80, 2 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, p );

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );
    TEST_ASSERT_EQ_INT ( s->detect_result.type, MZDSK_FS_FSMZ );

    /* panelová data naplněna */
    TEST_ASSERT_EQ_INT ( s->geometry_data.is_loaded, 1 );
    TEST_ASSERT_EQ_INT ( s->geometry_data.total_tracks, 160 );
    TEST_ASSERT_EQ_INT ( s->boot_data.is_loaded, 1 );
    TEST_ASSERT_EQ_INT ( s->fsmz_data.is_loaded, 1 );

    mzdisk_session_close_all ( &mgr );
    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Open CP/M -> detect_result.type = CP/M. */
static int test_session_detect_cpm ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/sc.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_cpm_sd, 160, 2 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, p );

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );
    TEST_ASSERT_EQ_INT ( s->detect_result.type, MZDSK_FS_CPM );
    TEST_ASSERT_EQ_INT ( s->cpm_data.is_loaded, 1 );

    mzdisk_session_close_all ( &mgr );
    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Otevřít 3 disky -> přepínat aktivní. */
static int test_session_multiple ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p1[512], p2[512], p3[512];
    snprintf ( p1, sizeof ( p1 ), "%s/m1.dsk", s_tmpdir );
    snprintf ( p2, sizeof ( p2 ), "%s/m2.dsk", s_tmpdir );
    snprintf ( p3, sizeof ( p3 ), "%s/m3.dsk", s_tmpdir );
    create_dsk_file ( p1, mzdsk_tools_format_basic, 40, 1 );
    create_dsk_file ( p2, mzdsk_tools_format_basic, 80, 2 );
    create_dsk_file ( p3, mzdsk_tools_format_cpm_sd, 160, 2 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, p1 );
    mzdisk_session_open ( &mgr, p2 );
    mzdisk_session_open ( &mgr, p3 );

    TEST_ASSERT_EQ_INT ( mgr.count, 3 );
    /* poslední otevřený dostal id 3 a je aktivní */
    TEST_ASSERT_EQ_INT ( (int) mgr.active_id, 3 );

    /* id jsou monotónní a unikátní */
    TEST_ASSERT_EQ_INT ( (int) mgr.sessions[0].id, 1 );
    TEST_ASSERT_EQ_INT ( (int) mgr.sessions[1].id, 2 );
    TEST_ASSERT_EQ_INT ( (int) mgr.sessions[2].id, 3 );

    /* jen první se stala primární, ostatní jsou detached */
    TEST_ASSERT_EQ_INT ( mgr.sessions[0].is_primary, 1 );
    TEST_ASSERT_EQ_INT ( mgr.sessions[1].is_primary, 0 );
    TEST_ASSERT_EQ_INT ( mgr.sessions[2].is_primary, 0 );

    /* přepnout na první (id = 1) */
    mzdisk_session_set_active_by_id ( &mgr, 1 );
    TEST_ASSERT_EQ_INT ( (int) mgr.active_id, 1 );

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );
    TEST_ASSERT_EQ_INT ( s->geometry_data.total_tracks, 40 );

    /* get_by_id najde libovolnou otevřenou */
    TEST_ASSERT_NOT_NULL ( mzdisk_session_get_by_id ( &mgr, 2 ) );
    TEST_ASSERT_NULL ( mzdisk_session_get_by_id ( &mgr, 999 ) );

    /* get_primary vrací stabilně první (id = 1) */
    st_MZDISK_SESSION *primary = mzdisk_session_get_primary ( &mgr );
    TEST_ASSERT_NOT_NULL ( primary );
    TEST_ASSERT_EQ_INT ( (int) primary->id, 1 );

    mzdisk_session_close_all ( &mgr );
    remove ( p1 ); remove ( p2 ); remove ( p3 ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Zavření aktivní -> výběr jiné session. */
static int test_session_close_selects_next ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p1[512], p2[512];
    snprintf ( p1, sizeof ( p1 ), "%s/n1.dsk", s_tmpdir );
    snprintf ( p2, sizeof ( p2 ), "%s/n2.dsk", s_tmpdir );
    create_dsk_file ( p1, mzdsk_tools_format_basic, 40, 1 );
    create_dsk_file ( p2, mzdsk_tools_format_basic, 80, 2 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, p1 );
    mzdisk_session_open ( &mgr, p2 );

    /* zavřít aktivní (id = 2) */
    mzdisk_session_close_by_id ( &mgr, 2 );
    TEST_ASSERT_EQ_INT ( mgr.count, 1 );
    TEST_ASSERT_EQ_INT ( (int) mgr.active_id, 1 ); /* přepnul na session id 1 */

    mzdisk_session_close_all ( &mgr );
    remove ( p1 ); remove ( p2 ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Append -> reload -> geometrie aktualizována. */
static int test_session_reload_after_append ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/rl.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_basic, 40, 1 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, p );

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );
    TEST_ASSERT_EQ_INT ( s->geometry_data.total_tracks, 40 );

    /* append 5 stop přes panel API */
    s->geom_edit_data.at_count = 5;
    s->geom_edit_data.at_sectors = 16;
    s->geom_edit_data.at_ssize_idx = 1; /* 256B */
    s->geom_edit_data.at_order_idx = 0;
    TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &s->geom_edit_data, &s->disc ) );

    /* reload panelů (jako v reálném GUI workflow) */
    mzdisk_session_reload_panels ( s );

    TEST_ASSERT_EQ_INT ( s->geometry_data.total_tracks, 45 );
    TEST_ASSERT_EQ_INT ( s->detect_result.type, MZDSK_FS_FSMZ );

    mzdisk_session_close_all ( &mgr );
    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Save -> reopen -> data zachována. */
static int test_session_save_reopen ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/sv.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_basic, 80, 2 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, p );

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );
    s->is_dirty = true;

    en_MZDSK_RES res = mzdisk_session_save ( s );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( s->is_dirty, 0 );

    mzdisk_session_close_all ( &mgr );

    /* znovu otevřít */
    mzdisk_session_manager_init ( &mgr );
    res = mzdisk_session_open ( &mgr, p );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    s = mzdisk_session_get_active ( &mgr );
    TEST_ASSERT_EQ_INT ( s->geometry_data.total_tracks, 160 );

    mzdisk_session_close_all ( &mgr );
    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Textové popisy všech FS typů. */
static int test_session_fs_type_str ( void )
{
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str ( MZDSK_FS_FSMZ ), "FSMZ" );
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str ( MZDSK_FS_CPM ), "CP/M" );
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str ( MZDSK_FS_MRS ), "MRS" );
    TEST_ASSERT_NOT_NULL ( mzdisk_session_fs_type_str ( MZDSK_FS_UNKNOWN ) );
    TEST_ASSERT_NOT_NULL ( mzdisk_session_fs_type_str ( MZDSK_FS_BOOT_ONLY ) );
    return 0;
}


/** @brief Detailní FS typ s CP/M SD/HD rozlišením. */
static int test_session_fs_type_str_detail ( void )
{
    st_MZDSK_DETECT_RESULT r;
    memset ( &r, 0, sizeof ( r ) );

    /* FSMZ */
    r.type = MZDSK_FS_FSMZ;
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str_detail ( &r ), "FSMZ" );

    /* CP/M SD */
    r.type = MZDSK_FS_CPM;
    r.cpm_format = MZDSK_CPM_FORMAT_SD;
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str_detail ( &r ), "CP/M SD" );

    /* CP/M HD */
    r.cpm_format = MZDSK_CPM_FORMAT_HD;
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str_detail ( &r ), "CP/M HD" );

    /* MRS */
    r.type = MZDSK_FS_MRS;
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str_detail ( &r ), "MRS" );

    /* Boot only */
    r.type = MZDSK_FS_BOOT_ONLY;
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str_detail ( &r ), "Boot only" );

    /* Unknown */
    r.type = MZDSK_FS_UNKNOWN;
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str_detail ( &r ), "Unknown" );

    /* NULL -> Unknown */
    TEST_ASSERT_EQ_STR ( mzdisk_session_fs_type_str_detail ( NULL ), "Unknown" );

    return 0;
}


/** @brief Nastavení poslední operace. */
static int test_session_set_last_op ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/lo.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_basic, 40, 1 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, p );

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );

    /* počáteční stav: žádná operace */
    TEST_ASSERT_EQ_INT ( s->last_op_status, 0 );

    /* nastavit OK */
    mzdisk_session_set_last_op ( s, 1, "Save" );
    TEST_ASSERT_EQ_INT ( s->last_op_status, 1 );
    TEST_ASSERT_EQ_STR ( s->last_op_msg, "Save" );

    /* nastavit FAILED */
    mzdisk_session_set_last_op ( s, 2, "Put" );
    TEST_ASSERT_EQ_INT ( s->last_op_status, 2 );
    TEST_ASSERT_EQ_STR ( s->last_op_msg, "Put" );

    mzdisk_session_close_all ( &mgr );
    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief create_empty vytvoří EMPTY session (okno bez disku). */
static int test_session_create_empty ( void )
{
    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );

    /* allow_primary=true: první session se stane primární */
    st_MZDISK_SESSION *s = mzdisk_session_create_empty ( &mgr, true );
    TEST_ASSERT_NOT_NULL ( s );
    TEST_ASSERT_EQ_INT ( s->is_open, 1 );     /* slot obsazený */
    TEST_ASSERT_EQ_INT ( s->has_disk, 0 );    /* ale bez disku */
    TEST_ASSERT_EQ_INT ( s->is_primary, 1 );  /* první se stala primární */
    TEST_ASSERT_EQ_INT ( (int) s->id, 1 );
    TEST_ASSERT_EQ_INT ( (int) mgr.active_id, 1 );
    TEST_ASSERT_EQ_INT ( mgr.count, 1 );

    /* další create_empty s allow_primary=true, ale primární už je -> detached */
    st_MZDISK_SESSION *s2 = mzdisk_session_create_empty ( &mgr, true );
    TEST_ASSERT_NOT_NULL ( s2 );
    TEST_ASSERT_EQ_INT ( s2->is_open, 1 );
    TEST_ASSERT_EQ_INT ( s2->has_disk, 0 );
    TEST_ASSERT_EQ_INT ( s2->is_primary, 0 );
    TEST_ASSERT_EQ_INT ( (int) s2->id, 2 );

    mzdisk_session_close_all ( &mgr );
    return 0;
}


/** @brief allow_primary=false: new session je vždy detached, i bez primární. */
static int test_session_create_empty_force_detached ( void )
{
    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );

    /* Bez primární, ale allow_primary=false -> přesto detached. */
    st_MZDISK_SESSION *s = mzdisk_session_create_empty ( &mgr, false );
    TEST_ASSERT_NOT_NULL ( s );
    TEST_ASSERT_EQ_INT ( s->is_primary, 0 );

    mzdisk_session_close_all ( &mgr );
    return 0;
}


/** @brief load načte DSK do existující EMPTY session. */
static int test_session_load_into_empty ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/le.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_basic, 80, 2 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );

    st_MZDISK_SESSION *s = mzdisk_session_create_empty ( &mgr, true );
    TEST_ASSERT_NOT_NULL ( s );
    TEST_ASSERT_EQ_INT ( s->has_disk, 0 );

    en_MZDSK_RES res = mzdisk_session_load ( s, p );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( s->has_disk, 1 );    /* LOADED stav */
    TEST_ASSERT_EQ_INT ( s->detect_result.type, MZDSK_FS_FSMZ );
    TEST_ASSERT_EQ_INT ( s->geometry_data.total_tracks, 160 );

    mzdisk_session_close_all ( &mgr );
    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief load nad session s diskem nahradí disk (replace). */
static int test_session_load_replace ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p1[512], p2[512];
    snprintf ( p1, sizeof ( p1 ), "%s/r1.dsk", s_tmpdir );
    snprintf ( p2, sizeof ( p2 ), "%s/r2.dsk", s_tmpdir );
    create_dsk_file ( p1, mzdsk_tools_format_basic, 40, 1 );
    create_dsk_file ( p2, mzdsk_tools_format_cpm_sd, 160, 2 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );
    mzdisk_session_open ( &mgr, p1 );

    st_MZDISK_SESSION *s = mzdisk_session_get_active ( &mgr );
    TEST_ASSERT_EQ_INT ( s->detect_result.type, MZDSK_FS_FSMZ );
    uint64_t orig_id = s->id;

    /* load do stejné session - disk se zamění, session zůstává */
    en_MZDSK_RES res = mzdisk_session_load ( s, p2 );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( (int) s->id, (int) orig_id );  /* id stabilní */
    TEST_ASSERT_EQ_INT ( s->detect_result.type, MZDSK_FS_CPM );
    TEST_ASSERT_EQ_INT ( mgr.count, 1 );  /* pořád 1 session */

    mzdisk_session_close_all ( &mgr );
    remove ( p1 ); remove ( p2 ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Překročení MZDISK_MAX_SESSIONS: 17. otevření vrátí NO_SPACE. */
static int test_session_max_boundary ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/mx.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_basic, 40, 1 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );

    /* otevřít max slotů */
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        en_MZDSK_RES res = mzdisk_session_open ( &mgr, p );
        TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    }
    TEST_ASSERT_EQ_INT ( mgr.count, MZDISK_MAX_SESSIONS );

    /* další otevření musí selhat */
    en_MZDSK_RES res = mzdisk_session_open ( &mgr, p );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_NO_SPACE );
    TEST_ASSERT_EQ_INT ( mgr.count, MZDISK_MAX_SESSIONS );

    /* stejně tak create_empty */
    st_MZDISK_SESSION *s = mzdisk_session_create_empty ( &mgr, false );
    TEST_ASSERT_NULL ( s );
    TEST_ASSERT_EQ_INT ( mgr.count, MZDISK_MAX_SESSIONS );

    /* zavření uvolní slot a další open funguje */
    st_MZDISK_SESSION *first = mzdisk_session_get_by_id ( &mgr, 1 );
    TEST_ASSERT_NOT_NULL ( first );
    mzdisk_session_close_by_id ( &mgr, 1 );
    TEST_ASSERT_EQ_INT ( mgr.count, MZDISK_MAX_SESSIONS - 1 );

    res = mzdisk_session_open ( &mgr, p );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( mgr.count, MZDISK_MAX_SESSIONS );

    mzdisk_session_close_all ( &mgr );
    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


/** @brief Id jsou monotónní i napříč open/close cyklemy. */
static int test_session_id_monotonic ( void )
{
    if ( setup_tmpdir() ) TEST_FAIL ( "tmpdir" );
    char p[512]; snprintf ( p, sizeof ( p ), "%s/id.dsk", s_tmpdir );
    create_dsk_file ( p, mzdsk_tools_format_basic, 40, 1 );

    st_MZDISK_SESSION_MANAGER mgr;
    mzdisk_session_manager_init ( &mgr );

    mzdisk_session_open ( &mgr, p );
    mzdisk_session_open ( &mgr, p );
    mzdisk_session_open ( &mgr, p );
    TEST_ASSERT_EQ_INT ( (int) mgr.sessions[0].id, 1 );
    TEST_ASSERT_EQ_INT ( (int) mgr.sessions[1].id, 2 );
    TEST_ASSERT_EQ_INT ( (int) mgr.sessions[2].id, 3 );

    /* zavřít prostřední (id=2), slot [1] uvolněn */
    mzdisk_session_close_by_id ( &mgr, 2 );
    TEST_ASSERT_EQ_INT ( mgr.sessions[1].is_open, 0 );

    /* nová session recykluje slot [1], ale dostane nové id 4 */
    mzdisk_session_open ( &mgr, p );
    TEST_ASSERT_EQ_INT ( mgr.sessions[1].is_open, 1 );
    TEST_ASSERT_EQ_INT ( (int) mgr.sessions[1].id, 4 );

    /* get_by_id ignoruje staré id */
    TEST_ASSERT_NULL ( mzdisk_session_get_by_id ( &mgr, 2 ) );
    TEST_ASSERT_NOT_NULL ( mzdisk_session_get_by_id ( &mgr, 4 ) );

    mzdisk_session_close_all ( &mgr );
    remove ( p ); rmdir ( s_tmpdir );
    return 0;
}


int main ( void )
{
    memory_driver_init();
    TEST_INIT();

    RUN_TEST ( test_session_manager_init );
    RUN_TEST ( test_session_open_close );
    RUN_TEST ( test_session_detect_fsmz );
    RUN_TEST ( test_session_detect_cpm );
    RUN_TEST ( test_session_multiple );
    RUN_TEST ( test_session_close_selects_next );
    RUN_TEST ( test_session_reload_after_append );
    RUN_TEST ( test_session_save_reopen );
    RUN_TEST ( test_session_fs_type_str );
    RUN_TEST ( test_session_fs_type_str_detail );
    RUN_TEST ( test_session_set_last_op );
    RUN_TEST ( test_session_create_empty );
    RUN_TEST ( test_session_create_empty_force_detached );
    RUN_TEST ( test_session_load_into_empty );
    RUN_TEST ( test_session_load_replace );
    RUN_TEST ( test_session_max_boundary );
    RUN_TEST ( test_session_id_monotonic );

    TEST_SUMMARY();
}
