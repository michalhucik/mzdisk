/**
 * @file test_mzdsk_tools.c
 * @brief Testy formátovacích funkcí (mzdsk_tools).
 *
 * Testuje vytváření disků z presetů a formátování s auto-detekcí FS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "test_framework.h"

#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/* ===================================================================
 *  Pomocné funkce
 * =================================================================== */


static void setup_handler ( st_HANDLER *h )
{
    memset ( h, 0, sizeof ( st_HANDLER ) );
    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;
}

static void cleanup_handler ( st_HANDLER *h )
{
    if ( h->spec.memspec.ptr ) {
        free ( h->spec.memspec.ptr );
        h->spec.memspec.ptr = NULL;
    }
}


/* ===================================================================
 *  Testy presetů
 * =================================================================== */


/** @brief mzdsk_tools_preset_name pro všechny presety. */
static int test_preset_name ( void )
{
    for ( int i = 0; i < MZDSK_PRESET_COUNT; i++ ) {
        const char *name = mzdsk_tools_preset_name ( (en_MZDSK_PRESET) i );
        TEST_ASSERT_NOT_NULL ( name );
        TEST_ASSERT ( strlen ( name ) > 0, "preset name non-empty" );
    }
    return 0;
}


/** @brief Vytvoření disku z každého presetu. */
static int test_create_from_preset ( void )
{
    for ( int i = 0; i < MZDSK_PRESET_COUNT; i++ ) {
        st_HANDLER h;
        setup_handler ( &h );

        int ret = mzdsk_tools_create_from_preset ( &h, (en_MZDSK_PRESET) i, 2 );
        TEST_ASSERT_OK ( ret );

        /* ověřit, že disk má nějaké stopy */
        st_DSK_TOOLS_TRACKS_RULES_INFO *tr = dsk_tools_get_tracks_rules ( &h );
        TEST_ASSERT_NOT_NULL ( tr );
        TEST_ASSERT ( tr->total_tracks > 0, "preset has tracks" );
        free ( tr );

        cleanup_handler ( &h );
    }
    return 0;
}


/* ===================================================================
 *  Testy formátovacích funkcí
 * =================================================================== */


/** @brief format_basic -> FSMZ detekce. */
static int test_format_basic ( void )
{
    st_HANDLER h;
    setup_handler ( &h );

    TEST_ASSERT_OK ( mzdsk_tools_format_basic ( &h, 80, 2 ) );

    st_DSK_TOOLS_TRACKS_RULES_INFO *tr = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( tr );
    TEST_ASSERT_EQ_INT ( tr->total_tracks, 160 );
    TEST_ASSERT_EQ_INT ( tr->sides, 2 );
    free ( tr );

    cleanup_handler ( &h );
    return 0;
}


/** @brief format_cpm_sd -> CP/M detekce. */
static int test_format_cpm_sd ( void )
{
    st_HANDLER h;
    setup_handler ( &h );

    TEST_ASSERT_OK ( mzdsk_tools_format_cpm_sd ( &h, 160, 2 ) );

    st_DSK_TOOLS_TRACKS_RULES_INFO *tr = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( tr );
    TEST_ASSERT_EQ_INT ( tr->total_tracks, 160 );
    free ( tr );

    cleanup_handler ( &h );
    return 0;
}


/** @brief format_cpm_hd -> CP/M HD geometrie (18 sektorů). */
static int test_format_cpm_hd ( void )
{
    st_HANDLER h;
    setup_handler ( &h );

    TEST_ASSERT_OK ( mzdsk_tools_format_cpm_hd ( &h, 160, 2 ) );

    st_DSK_TOOLS_TRACKS_RULES_INFO *tr = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( tr );
    TEST_ASSERT_EQ_INT ( tr->total_tracks, 160 );
    free ( tr );

    cleanup_handler ( &h );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    memory_driver_init();
    TEST_INIT();

    RUN_TEST ( test_preset_name );
    RUN_TEST ( test_create_from_preset );
    RUN_TEST ( test_format_basic );
    RUN_TEST ( test_format_cpm_sd );
    RUN_TEST ( test_format_cpm_hd );

    TEST_SUMMARY();
}
