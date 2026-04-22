/**
 * @file test_panel_create.c
 * @brief Testy GUI panelu pro vytváření nových disků.
 *
 * Testuje načítání předdefinovaných presetů (geometrie, pravidla)
 * a kompletní workflow vytvoření DSK souboru s formátováním.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_framework.h"

#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "panels/panel_create.h"


/* ===================================================================
 *  Testy načítání presetů
 * =================================================================== */


/** @brief MZ-BASIC preset: 160T, 2 sides, 1 pravidlo (16x256B). */
static int test_preset_basic ( void )
{
    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    /* výchozí preset je MZ-BASIC (index 1) */
    TEST_ASSERT_EQ_INT ( data.preset_idx, 1 );
    TEST_ASSERT_EQ_INT ( data.sides, 2 );
    TEST_ASSERT_EQ_INT ( data.tracks, 160 );
    TEST_ASSERT_EQ_INT ( data.count_rules, 1 );
    TEST_ASSERT_EQ_INT ( data.format_filesystem, 1 );

    /* pravidlo: 16 sektorů, 256B, Normal */
    TEST_ASSERT_EQ_INT ( data.rules[0].from_track, 0 );
    TEST_ASSERT_EQ_INT ( data.rules[0].sectors, 16 );
    TEST_ASSERT_EQ_INT ( data.rules[0].sector_size_idx, 1 ); /* 256B */
    return 0;
}


/** @brief CP/M SD preset: 3 pravidla (data, boot, data). */
static int test_preset_cpm_sd ( void )
{
    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    panel_create_load_preset ( &data, 2 ); /* CP/M SD */

    TEST_ASSERT_EQ_INT ( data.sides, 2 );
    TEST_ASSERT_EQ_INT ( data.tracks, 160 );
    TEST_ASSERT_EQ_INT ( data.count_rules, 3 );
    TEST_ASSERT_EQ_INT ( data.format_filesystem, 1 );

    /* pravidlo 0: stopa 0 = 9x512B */
    TEST_ASSERT_EQ_INT ( data.rules[0].from_track, 0 );
    TEST_ASSERT_EQ_INT ( data.rules[0].sectors, 9 );
    TEST_ASSERT_EQ_INT ( data.rules[0].sector_size_idx, 2 ); /* 512B */

    /* pravidlo 1: stopa 1 = 16x256B (boot) */
    TEST_ASSERT_EQ_INT ( data.rules[1].from_track, 1 );
    TEST_ASSERT_EQ_INT ( data.rules[1].sectors, 16 );
    TEST_ASSERT_EQ_INT ( data.rules[1].sector_size_idx, 1 ); /* 256B */

    /* pravidlo 2: stopy 2+ = 9x512B */
    TEST_ASSERT_EQ_INT ( data.rules[2].from_track, 2 );
    TEST_ASSERT_EQ_INT ( data.rules[2].sectors, 9 );
    return 0;
}


/** @brief CP/M HD preset: 18 sektorů na datových stopách. */
static int test_preset_cpm_hd ( void )
{
    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    panel_create_load_preset ( &data, 3 ); /* CP/M HD */

    TEST_ASSERT_EQ_INT ( data.count_rules, 3 );
    TEST_ASSERT_EQ_INT ( data.rules[0].sectors, 18 );
    TEST_ASSERT_EQ_INT ( data.rules[2].sectors, 18 );
    return 0;
}


/** @brief MRS preset: stejná geometrie jako CP/M SD. */
static int test_preset_mrs ( void )
{
    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    panel_create_load_preset ( &data, 4 ); /* MRS */

    TEST_ASSERT_EQ_INT ( data.count_rules, 3 );
    TEST_ASSERT_EQ_INT ( data.format_filesystem, 1 );
    TEST_ASSERT_EQ_INT ( data.rules[0].sectors, 9 );
    TEST_ASSERT_EQ_INT ( data.rules[1].sectors, 16 );
    TEST_ASSERT_EQ_INT ( data.rules[2].sectors, 9 );
    return 0;
}


/** @brief Lemmings preset: 5 pravidel, custom mapa na stopě 16. */
static int test_preset_lemmings ( void )
{
    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    panel_create_load_preset ( &data, 5 ); /* Lemmings */

    TEST_ASSERT_EQ_INT ( data.count_rules, 5 );
    TEST_ASSERT_EQ_INT ( data.format_filesystem, 0 ); /* neformátovatelný */

    /* pravidlo 3: stopa 16 = 10 sektorů, custom mapa */
    TEST_ASSERT_EQ_INT ( data.rules[3].from_track, 16 );
    TEST_ASSERT_EQ_INT ( data.rules[3].sectors, 10 );
    return 0;
}


/** @brief Custom preset: neformátovatelný. */
static int test_preset_custom ( void )
{
    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    panel_create_load_preset ( &data, 0 ); /* Custom */

    TEST_ASSERT_EQ_INT ( data.format_filesystem, 0 );
    TEST_ASSERT_EQ_INT ( data.count_rules, 1 );
    return 0;
}


/** @brief Neplatný index presetu se tiše ignoruje. */
static int test_preset_invalid_index ( void )
{
    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    /* zapamatovat stav po init (preset 1 = MZ-BASIC) */
    int orig_tracks = data.tracks;

    panel_create_load_preset ( &data, -1 );
    TEST_ASSERT_EQ_INT ( data.tracks, orig_tracks ); /* nezměněno */

    panel_create_load_preset ( &data, 99 );
    TEST_ASSERT_EQ_INT ( data.tracks, orig_tracks ); /* nezměněno */
    return 0;
}


/* ===================================================================
 *  Testy execute (vytvoření DSK souborů)
 * =================================================================== */


/** @brief Execute MZ-BASIC preset -> vytvoří DSK s FSMZ. */
static int test_execute_basic ( void )
{
    char orig_dir[1024];
    if ( getcwd ( orig_dir, sizeof ( orig_dir ) ) == NULL ) {
        TEST_FAIL ( "getcwd failed" );
    }

    char tmpdir[] = "/tmp/mzdsk_test_create_XXXXXX";
    if ( mkdtemp ( tmpdir ) == NULL ) {
        TEST_FAIL ( "mkdtemp failed" );
    }

    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    /* preset MZ-BASIC (index 1) */
    panel_create_load_preset ( &data, 1 );
    strncpy ( data.directory, tmpdir, sizeof ( data.directory ) - 1 );
    strncpy ( data.filename, "test_basic", sizeof ( data.filename ) - 1 );

    int ret = panel_create_execute ( &data );

    if ( ret != EXIT_SUCCESS ) {
        /* úklid */
        char path[2048];
        snprintf ( path, sizeof ( path ), "%s/test_basic.dsk", tmpdir );
        remove ( path );
        rmdir ( tmpdir );
        fprintf ( stderr, "    error_msg: %s\n", data.error_msg );
        TEST_FAIL ( "panel_create_execute failed" );
    }

    TEST_ASSERT_EQ_INT ( data.created, 1 );

    /* ověřit, že soubor existuje a má správnou geometrii */
    st_MZDSK_DISC disc;
    en_MZDSK_RES res = mzdsk_disc_open_memory ( &disc, data.created_filepath,
                                                  FILE_DRIVER_OPMODE_RO );
    if ( res != MZDSK_RES_OK ) {
        remove ( data.created_filepath );
        rmdir ( tmpdir );
        TEST_FAIL ( "cannot reopen created DSK" );
    }

    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 160 );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->sides, 2 );

    /* detekce musí najít FSMZ */
    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );
    TEST_ASSERT_EQ_INT ( detect.type, MZDSK_FS_FSMZ );

    mzdsk_disc_close ( &disc );
    remove ( data.created_filepath );
    rmdir ( tmpdir );
    return 0;
}


/** @brief Execute CP/M SD preset -> vytvoří DSK s CP/M. */
static int test_execute_cpm_sd ( void )
{
    char tmpdir[] = "/tmp/mzdsk_test_create_XXXXXX";
    if ( mkdtemp ( tmpdir ) == NULL ) {
        TEST_FAIL ( "mkdtemp failed" );
    }

    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    panel_create_load_preset ( &data, 2 ); /* CP/M SD */
    strncpy ( data.directory, tmpdir, sizeof ( data.directory ) - 1 );
    strncpy ( data.filename, "test_cpm", sizeof ( data.filename ) - 1 );

    int ret = panel_create_execute ( &data );

    if ( ret != EXIT_SUCCESS ) {
        char path[2048];
        snprintf ( path, sizeof ( path ), "%s/test_cpm.dsk", tmpdir );
        remove ( path );
        rmdir ( tmpdir );
        fprintf ( stderr, "    error_msg: %s\n", data.error_msg );
        TEST_FAIL ( "panel_create_execute failed" );
    }

    TEST_ASSERT_EQ_INT ( data.created, 1 );

    st_MZDSK_DISC disc;
    en_MZDSK_RES res = mzdsk_disc_open_memory ( &disc, data.created_filepath,
                                                  FILE_DRIVER_OPMODE_RO );
    if ( res != MZDSK_RES_OK ) {
        remove ( data.created_filepath );
        rmdir ( tmpdir );
        TEST_FAIL ( "cannot reopen created DSK" );
    }

    st_MZDSK_DETECT_RESULT detect;
    mzdsk_detect_filesystem ( &disc, &detect );
    TEST_ASSERT_EQ_INT ( detect.type, MZDSK_FS_CPM );

    mzdsk_disc_close ( &disc );
    remove ( data.created_filepath );
    rmdir ( tmpdir );
    return 0;
}


/** @brief Execute Custom (neformátovaný) preset. */
static int test_execute_custom_unformatted ( void )
{
    char tmpdir[] = "/tmp/mzdsk_test_create_XXXXXX";
    if ( mkdtemp ( tmpdir ) == NULL ) {
        TEST_FAIL ( "mkdtemp failed" );
    }

    st_PANEL_CREATE_DATA data;
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    panel_create_init ( &data, &cfg );

    panel_create_load_preset ( &data, 0 ); /* Custom */
    data.sides = 1;
    data.tracks = 40;
    strncpy ( data.directory, tmpdir, sizeof ( data.directory ) - 1 );
    strncpy ( data.filename, "test_custom", sizeof ( data.filename ) - 1 );

    int ret = panel_create_execute ( &data );

    if ( ret != EXIT_SUCCESS ) {
        char path[2048];
        snprintf ( path, sizeof ( path ), "%s/test_custom.dsk", tmpdir );
        remove ( path );
        rmdir ( tmpdir );
        fprintf ( stderr, "    error_msg: %s\n", data.error_msg );
        TEST_FAIL ( "panel_create_execute failed" );
    }

    TEST_ASSERT_EQ_INT ( data.created, 1 );

    st_MZDSK_DISC disc;
    en_MZDSK_RES res = mzdsk_disc_open_memory ( &disc, data.created_filepath,
                                                  FILE_DRIVER_OPMODE_RO );
    if ( res != MZDSK_RES_OK ) {
        remove ( data.created_filepath );
        rmdir ( tmpdir );
        TEST_FAIL ( "cannot reopen created DSK" );
    }

    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 40 );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->sides, 1 );

    mzdsk_disc_close ( &disc );
    remove ( data.created_filepath );
    rmdir ( tmpdir );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    memory_driver_init();
    TEST_INIT();

    RUN_TEST ( test_preset_basic );
    RUN_TEST ( test_preset_cpm_sd );
    RUN_TEST ( test_preset_cpm_hd );
    RUN_TEST ( test_preset_mrs );
    RUN_TEST ( test_preset_lemmings );
    RUN_TEST ( test_preset_custom );
    RUN_TEST ( test_preset_invalid_index );
    RUN_TEST ( test_execute_basic );
    RUN_TEST ( test_execute_cpm_sd );
    RUN_TEST ( test_execute_custom_unformatted );

    TEST_SUMMARY();
}
