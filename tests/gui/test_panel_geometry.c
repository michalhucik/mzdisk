/**
 * @file test_panel_geometry.c
 * @brief Testy GUI vrstvy editace geometrie (panel_geometry).
 *
 * Testuje operace append/shrink/change-track přes GUI panel API,
 * které volají dsk_tools funkce s parametry sestavenými z UI stavu.
 *
 * Motivace: Původní bug v GUI append přepisoval DSK header místo
 * přičítání stop a nastavoval absolute_track=0, čímž ničil disk.
 * Knihovní testy (test_dsk_tools) testují dsk_tools přímo - tento
 * soubor testuje GUI wrapper, který bug obsahoval.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "test_framework.h"

#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "panels/panel_geometry.h"


/* ===================================================================
 *  Pomocné funkce
 * =================================================================== */


/**
 * @brief Vytvoří CP/M SD disk v paměti (160T, 2 strany).
 *
 * Geometrie: stopa 0 = 9x512B LEC, stopa 1 = 16x256B (boot),
 * stopy 2-159 = 9x512B LEC. Handler se alokuje na haldě.
 *
 * @param disc Výstupní diskový obraz.
 * @return EXIT_SUCCESS při úspěchu.
 */
static int create_cpm_sd_memory ( st_MZDSK_DISC *disc )
{
    memset ( disc, 0, sizeof ( *disc ) );

    /* alokovat handler na haldě (st_MZDSK_DISC drží ukazatel) */
    st_HANDLER *h = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    if ( !h ) return EXIT_FAILURE;

    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;

    disc->handler = h;
    disc->cache = (uint8_t *) malloc ( 1024 );
    if ( !disc->cache ) { free ( h ); return EXIT_FAILURE; }

    /* 3 pravidla: stopa 0 = 9x512 LEC, stopa 1 = 16x256 (boot), stopy 2+ = 9x512 LEC */
    size_t desc_size = dsk_tools_compute_description_size ( 3 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) return EXIT_FAILURE;
    memset ( desc, 0, desc_size );

    desc->tracks = 80;
    desc->sides = 2;
    desc->count_rules = 3;

    dsk_tools_assign_description ( desc, 0, 0, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_INTERLACED_LEC, NULL, 0xE5 );
    dsk_tools_assign_description ( desc, 1, 1, 16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xFF );
    dsk_tools_assign_description ( desc, 2, 2, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_INTERLACED_LEC, NULL, 0xE5 );

    int ret = dsk_tools_create_image ( h, desc );
    free ( desc );

    if ( ret != EXIT_SUCCESS ) return EXIT_FAILURE;

    /* naplnit tracks_rules */
    disc->tracks_rules = dsk_tools_get_tracks_rules ( h );
    if ( !disc->tracks_rules ) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}


/**
 * @brief Uvolní zdroje disku vytvořeného v paměti.
 *
 * @param disc Diskový obraz.
 */
static void cleanup_disc ( st_MZDSK_DISC *disc )
{
    if ( disc->tracks_rules ) {
        free ( disc->tracks_rules );
        disc->tracks_rules = NULL;
    }
    if ( disc->cache ) {
        free ( disc->cache );
        disc->cache = NULL;
    }
    if ( disc->handler ) {
        if ( disc->handler->spec.memspec.ptr ) {
            free ( disc->handler->spec.memspec.ptr );
        }
        free ( disc->handler );
        disc->handler = NULL;
    }
}


/**
 * @brief Znovu načte tracks_rules po geometrické změně.
 *
 * @param disc Diskový obraz.
 * @return Nový tracks_rules nebo NULL.
 */
static st_DSK_TOOLS_TRACKS_RULES_INFO* reload_tracks_rules ( st_MZDSK_DISC *disc )
{
    if ( disc->tracks_rules ) {
        free ( disc->tracks_rules );
    }
    disc->tracks_rules = dsk_tools_get_tracks_rules ( disc->handler );
    return disc->tracks_rules;
}


/* ===================================================================
 *  Testy panel_geometry_load
 * =================================================================== */


/** @brief Načtení geometrie z CP/M SD disku. */
static int test_geom_load_cpm_sd ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOMETRY_DATA data;
    panel_geometry_load ( &data, &disc );

    TEST_ASSERT_EQ_INT ( data.is_loaded, 1 );
    TEST_ASSERT_EQ_INT ( data.total_tracks, 160 );
    TEST_ASSERT_EQ_INT ( data.sides, 2 );

    /* stopa 0: 9 sektorů x 512B */
    TEST_ASSERT_EQ_INT ( data.tracks[0].sectors, 9 );
    TEST_ASSERT_EQ_INT ( data.tracks[0].sector_size, 512 );

    /* stopa 1: 16 sektorů x 256B (boot track, invertovaná) */
    TEST_ASSERT_EQ_INT ( data.tracks[1].sectors, 16 );
    TEST_ASSERT_EQ_INT ( data.tracks[1].sector_size, 256 );
    TEST_ASSERT_EQ_INT ( data.tracks[1].is_inverted, 1 );

    /* stopy 2+: 9 sektorů x 512B */
    TEST_ASSERT_EQ_INT ( data.tracks[2].sectors, 9 );
    TEST_ASSERT_EQ_INT ( data.tracks[2].sector_size, 512 );
    TEST_ASSERT_EQ_INT ( data.tracks[159].sectors, 9 );

    cleanup_disc ( &disc );
    return 0;
}


/* ===================================================================
 *  Testy panel_geom_edit_append_tracks (REGRESE PRO PŮVODNÍ BUG!)
 * =================================================================== */


/** @brief Append 2 stop přes GUI vrstvu - header.tracks se správně zvýší. */
static int test_geom_append_basic ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );

    edit.at_count = 2;
    edit.at_sectors = 9;
    edit.at_ssize_idx = 2;     /* 512 B */
    edit.at_order_idx = 0;     /* Normal */

    int ret = panel_geom_edit_append_tracks ( &edit, &disc );
    TEST_ASSERT_OK ( ret );
    TEST_ASSERT_EQ_INT ( edit.is_error, 0 );

    TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );

    /* KLÍČOVÁ KONTROLA: total_tracks = 162, ne 2! (původní bug) */
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 162 );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->sides, 2 );

    /* ověřit, že nové stopy mají správnou geometrii */
    st_PANEL_GEOMETRY_DATA geom;
    panel_geometry_load ( &geom, &disc );
    TEST_ASSERT_EQ_INT ( geom.tracks[160].sectors, 9 );
    TEST_ASSERT_EQ_INT ( geom.tracks[160].sector_size, 512 );
    TEST_ASSERT_EQ_INT ( geom.tracks[161].sectors, 9 );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Append zachová data na původních stopách. */
static int test_geom_append_preserves_data ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    /* zapsat vzor do sektoru 1 stopy 2 (datová stopa 9x512B, LEC ID od 1) */
    uint8_t marker[512];
    memset ( marker, 0xAB, sizeof ( marker ) );
    TEST_ASSERT_OK ( dsk_write_sector ( disc.handler, 2, 1, marker ) );

    /* ověřit, že zápis funguje */
    uint8_t check[512];
    TEST_ASSERT_OK ( dsk_read_sector ( disc.handler, 2, 1, check ) );
    TEST_ASSERT_EQ_MEM ( check, marker, sizeof ( marker ) );

    /* append */
    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.at_count = 2;
    edit.at_sectors = 9;
    edit.at_ssize_idx = 2;
    edit.at_order_idx = 0;
    TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &edit, &disc ) );

    /* ověřit, že data na stopě 2 jsou netknutá */
    uint8_t readback[512];
    TEST_ASSERT_OK ( dsk_read_sector ( disc.handler, 2, 1, readback ) );
    TEST_ASSERT_EQ_MEM ( readback, marker, sizeof ( marker ) );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Append lichého počtu stop na dvoustranný disk selže. */
static int test_geom_append_odd_2sided_fails ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.at_count = 3; /* lichý - musí selhat na 2-sided disku */
    edit.at_sectors = 9;
    edit.at_ssize_idx = 2;

    int ret = panel_geom_edit_append_tracks ( &edit, &disc );
    TEST_ASSERT_FAIL ( ret );
    TEST_ASSERT_EQ_INT ( edit.is_error, 1 );

    cleanup_disc ( &disc );
    return 0;
}


/* ===================================================================
 *  Testy panel_geom_edit_shrink
 * =================================================================== */


/** @brief Shrink přes GUI vrstvu - zmenšení počtu stop. */
static int test_geom_shrink_basic ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.sh_new_total = 80;

    int ret = panel_geom_edit_shrink ( &edit, &disc );
    TEST_ASSERT_OK ( ret );

    TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 80 );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Shrink na lichý počet na 2-sided disku selže. */
static int test_geom_shrink_odd_2sided_fails ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.sh_new_total = 79;

    int ret = panel_geom_edit_shrink ( &edit, &disc );
    TEST_ASSERT_FAIL ( ret );
    TEST_ASSERT_EQ_INT ( edit.is_error, 1 );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Shrink na větší nebo stejný počet stop selže. */
static int test_geom_shrink_no_increase ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.sh_new_total = 160;

    int ret = panel_geom_edit_shrink ( &edit, &disc );
    TEST_ASSERT_FAIL ( ret );
    TEST_ASSERT_EQ_INT ( edit.is_error, 1 );

    cleanup_disc ( &disc );
    return 0;
}


/* ===================================================================
 *  Testy panel_geom_edit_change_track
 * =================================================================== */


/** @brief Change track přes GUI vrstvu. */
static int test_geom_change_track ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );

    edit.ct_track = 5;
    edit.ct_sectors = 18;
    edit.ct_ssize_idx = 2;
    edit.ct_order_idx = 0;
    edit.ct_filler = 0xE5;

    int ret = panel_geom_edit_change_track ( &edit, &disc );
    TEST_ASSERT_OK ( ret );

    TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );

    st_PANEL_GEOMETRY_DATA geom;
    panel_geometry_load ( &geom, &disc );
    TEST_ASSERT_EQ_INT ( geom.tracks[5].sectors, 18 );
    TEST_ASSERT_EQ_INT ( geom.tracks[5].sector_size, 512 );

    /* okolní stopy nedotčeny */
    TEST_ASSERT_EQ_INT ( geom.tracks[4].sectors, 9 );
    TEST_ASSERT_EQ_INT ( geom.tracks[6].sectors, 9 );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Change track na neexistující stopu selže. */
static int test_geom_change_track_out_of_range ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.ct_track = 200;
    edit.ct_sectors = 9;

    int ret = panel_geom_edit_change_track ( &edit, &disc );
    TEST_ASSERT_FAIL ( ret );
    TEST_ASSERT_EQ_INT ( edit.is_error, 1 );

    cleanup_disc ( &disc );
    return 0;
}


/* ===================================================================
 *  Append + shrink roundtrip přes GUI
 * =================================================================== */


/** @brief Append a pak shrink zpět na původní velikost. */
static int test_geom_append_shrink_roundtrip ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    /* append 4 stopy */
    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.at_count = 4;
    edit.at_sectors = 9;
    edit.at_ssize_idx = 2;
    edit.at_order_idx = 0;
    TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &edit, &disc ) );

    TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 164 );

    /* shrink zpět na 160 */
    panel_geom_edit_init ( &edit, &disc );
    edit.sh_new_total = 160;
    TEST_ASSERT_OK ( panel_geom_edit_shrink ( &edit, &disc ) );

    TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 160 );

    cleanup_disc ( &disc );
    return 0;
}


/* ===================================================================
 *  Append na FSMZ disku (jiná geometrie: 16x256B)
 * =================================================================== */


/**
 * @brief Vytvoří FSMZ disk v paměti (80T, 1 strana, 16x256B).
 *
 * @param disc Výstupní diskový obraz.
 * @return EXIT_SUCCESS při úspěchu.
 */
static int create_fsmz_memory ( st_MZDSK_DISC *disc )
{
    memset ( disc, 0, sizeof ( *disc ) );

    st_HANDLER *h = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    if ( !h ) return EXIT_FAILURE;

    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;

    disc->handler = h;
    disc->cache = (uint8_t *) malloc ( 1024 );
    if ( !disc->cache ) { free ( h ); return EXIT_FAILURE; }

    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) return EXIT_FAILURE;
    memset ( desc, 0, desc_size );

    desc->tracks = 80;
    desc->sides = 1;
    desc->count_rules = 1;

    dsk_tools_assign_description ( desc, 0, 0, 16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xFF );

    int ret = dsk_tools_create_image ( h, desc );
    free ( desc );

    if ( ret != EXIT_SUCCESS ) return EXIT_FAILURE;

    disc->tracks_rules = dsk_tools_get_tracks_rules ( h );
    if ( !disc->tracks_rules ) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}


/** @brief Append na FSMZ disk (16x256B) - jiná geometrie než CP/M. */
static int test_geom_append_fsmz ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_fsmz_memory ( &disc ) );

    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 80 );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->sides, 1 );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.at_count = 5;
    edit.at_sectors = 16;
    edit.at_ssize_idx = 1;     /* 256B */
    edit.at_order_idx = 0;

    TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &edit, &disc ) );

    TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 85 );

    /* nové stopy mají FSMZ geometrii */
    st_PANEL_GEOMETRY_DATA geom;
    panel_geometry_load ( &geom, &disc );
    TEST_ASSERT_EQ_INT ( geom.tracks[80].sectors, 16 );
    TEST_ASSERT_EQ_INT ( geom.tracks[80].sector_size, 256 );
    TEST_ASSERT_EQ_INT ( geom.tracks[84].sectors, 16 );

    cleanup_disc ( &disc );
    return 0;
}


/* ===================================================================
 *  Vícenásobný append bez reloadu (simulace chybného UI workflow)
 * =================================================================== */


/** @brief Dva po sobě jdoucí appendy s reloadem pravidel mezi nimi. */
static int test_geom_multiple_appends ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    /* 1. append: 160 -> 162 */
    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.at_count = 2;
    edit.at_sectors = 9;
    edit.at_ssize_idx = 2;
    edit.at_order_idx = 0;
    TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &edit, &disc ) );
    TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 162 );

    /* 2. append: 162 -> 164 */
    panel_geom_edit_init ( &edit, &disc );
    edit.at_count = 2;
    edit.at_sectors = 18;      /* jiný počet sektorů */
    edit.at_ssize_idx = 2;
    edit.at_order_idx = 0;
    TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &edit, &disc ) );
    TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 164 );

    /* ověřit, že nové stopy mají správnou geometrii */
    st_PANEL_GEOMETRY_DATA geom;
    panel_geometry_load ( &geom, &disc );
    TEST_ASSERT_EQ_INT ( geom.tracks[160].sectors, 9 );   /* 1. append */
    TEST_ASSERT_EQ_INT ( geom.tracks[162].sectors, 18 );   /* 2. append */

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Tři appendy za sebou - kumulativní. */
static int test_geom_triple_append ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    for ( int i = 0; i < 3; i++ ) {
        st_PANEL_GEOM_EDIT_DATA edit;
        panel_geom_edit_init ( &edit, &disc );
        edit.at_count = 2;
        edit.at_sectors = 9;
        edit.at_ssize_idx = 2;
        edit.at_order_idx = 0;
        TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &edit, &disc ) );
        TEST_ASSERT_NOT_NULL ( reload_tracks_rules ( &disc ) );
    }

    /* 160 + 3*2 = 166 */
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 166 );

    cleanup_disc ( &disc );
    return 0;
}


/* ===================================================================
 *  Simulace session reload workflow
 * =================================================================== */


/**
 * @brief Simuluje mzdisk_session_reload_panels() bez celé session infrastruktury.
 *
 * Provede stejnou sekvenci jako disk_session.c:
 * 1. Zruší a znovu načte tracks_rules
 * 2. Znovu načte panel_geometry_load
 * 3. Aktualizuje geom_edit_init
 *
 * @param disc Diskový obraz.
 * @param geom Výstupní geometrie.
 * @param edit Výstupní editační data.
 */
static void simulate_session_reload ( st_MZDSK_DISC *disc,
                                       st_PANEL_GEOMETRY_DATA *geom,
                                       st_PANEL_GEOM_EDIT_DATA *edit )
{
    if ( disc->tracks_rules ) {
        free ( disc->tracks_rules );
        disc->tracks_rules = NULL;
    }
    disc->tracks_rules = dsk_tools_get_tracks_rules ( disc->handler );

    panel_geometry_load ( geom, disc );
    panel_geom_edit_init ( edit, disc );
}


/** @brief Append -> reload -> append -> reload (celý GUI workflow). */
static int test_geom_append_reload_append ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOMETRY_DATA geom;
    st_PANEL_GEOM_EDIT_DATA edit;

    /* 1. append 2 stop */
    panel_geom_edit_init ( &edit, &disc );
    edit.at_count = 2;
    edit.at_sectors = 9;
    edit.at_ssize_idx = 2;
    edit.at_order_idx = 0;
    TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &edit, &disc ) );

    /* reload (jako session_reload_panels) */
    simulate_session_reload ( &disc, &geom, &edit );

    TEST_ASSERT_EQ_INT ( geom.total_tracks, 162 );
    TEST_ASSERT_EQ_INT ( geom.is_loaded, 1 );

    /* sh_new_total by měl být 161 (total - 1) */
    TEST_ASSERT_EQ_INT ( edit.sh_new_total, 161 );

    /* 2. append dalších 4 stop */
    edit.at_count = 4;
    edit.at_sectors = 9;
    edit.at_ssize_idx = 2;
    edit.at_order_idx = 0;
    TEST_ASSERT_OK ( panel_geom_edit_append_tracks ( &edit, &disc ) );

    /* reload */
    simulate_session_reload ( &disc, &geom, &edit );

    TEST_ASSERT_EQ_INT ( geom.total_tracks, 166 );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 166 );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Change track -> reload -> ověřit panel geometrie. */
static int test_geom_change_track_reload ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOMETRY_DATA geom;
    st_PANEL_GEOM_EDIT_DATA edit;

    /* change track 10 na 18 sektorů */
    panel_geom_edit_init ( &edit, &disc );
    edit.ct_track = 10;
    edit.ct_sectors = 18;
    edit.ct_ssize_idx = 2;
    edit.ct_order_idx = 0;
    edit.ct_filler = 0xE5;
    TEST_ASSERT_OK ( panel_geom_edit_change_track ( &edit, &disc ) );

    /* reload */
    simulate_session_reload ( &disc, &geom, &edit );

    /* geometrie musí reflektovat změnu */
    TEST_ASSERT_EQ_INT ( geom.tracks[10].sectors, 18 );
    TEST_ASSERT_EQ_INT ( geom.tracks[9].sectors, 9 );   /* okolní nedotčeny */
    TEST_ASSERT_EQ_INT ( geom.tracks[11].sectors, 9 );
    TEST_ASSERT_EQ_INT ( geom.total_tracks, 160 );       /* počet stop nezměněn */

    cleanup_disc ( &disc );
    return 0;
}


/* ===================================================================
 *  Testy panel_geom_edit_load/apply_sector_ids
 * =================================================================== */


/** @brief Load sector IDs z track headeru naplní si_ids[]. */
static int test_geom_load_sector_ids ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );

    /* Stopa 0 = 9x512B s LEC interleave */
    edit.si_track = 0;
    panel_geom_edit_load_sector_ids ( &edit, &disc );

    TEST_ASSERT_EQ_INT ( edit.si_loaded, 1 );
    TEST_ASSERT_EQ_INT ( edit.si_count, 9 );
    /* LEC pořadí: 1, 6, 2, 7, 3, 8, 4, 9, 5 */
    TEST_ASSERT_EQ_INT ( edit.si_ids[0], 1 );
    TEST_ASSERT_EQ_INT ( edit.si_ids[1], 6 );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Apply sector IDs zapíše nová ID a zpětné čtení ověří. */
static int test_geom_apply_sector_ids ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );

    /* Načíst aktuální IDs */
    edit.si_track = 0;
    panel_geom_edit_load_sector_ids ( &edit, &disc );
    TEST_ASSERT_EQ_INT ( edit.si_loaded, 1 );

    /* Změnit na reverzní pořadí */
    for ( int i = 0; i < edit.si_count; i++ ) {
        edit.si_ids[i] = (uint8_t) ( 9 - i );
    }

    /* Aplikovat */
    TEST_ASSERT_OK ( panel_geom_edit_apply_sector_ids ( &edit, &disc ) );
    TEST_ASSERT_EQ_INT ( edit.is_error, 0 );

    /* Zpětně načíst a ověřit */
    panel_geom_edit_load_sector_ids ( &edit, &disc );
    TEST_ASSERT_EQ_INT ( edit.si_ids[0], 9 );
    TEST_ASSERT_EQ_INT ( edit.si_ids[1], 8 );
    TEST_ASSERT_EQ_INT ( edit.si_ids[8], 1 );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Apply -> session reload -> geometrie reflektuje nová ID. */
static int test_geom_apply_sector_ids_reload ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOMETRY_DATA geom;
    st_PANEL_GEOM_EDIT_DATA edit;

    /* Načíst a změnit sector IDs na stopě 2 */
    panel_geom_edit_init ( &edit, &disc );
    edit.si_track = 2;
    panel_geom_edit_load_sector_ids ( &edit, &disc );
    TEST_ASSERT_EQ_INT ( edit.si_loaded, 1 );

    /* Nastavit sekvenční pořadí */
    for ( int i = 0; i < edit.si_count; i++ ) {
        edit.si_ids[i] = (uint8_t) ( i + 1 );
    }
    TEST_ASSERT_OK ( panel_geom_edit_apply_sector_ids ( &edit, &disc ) );

    /* Reload */
    simulate_session_reload ( &disc, &geom, &edit );

    /* Geometrie musí reflektovat nová ID */
    TEST_ASSERT_EQ_INT ( geom.tracks[2].sector_ids[0], 1 );
    TEST_ASSERT_EQ_INT ( geom.tracks[2].sector_ids[1], 2 );
    TEST_ASSERT_EQ_INT ( geom.tracks[2].sector_ids[8], 9 );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief Po apply zůstanou sektorová data beze změny. */
static int test_geom_apply_sector_ids_preserves_data ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    /* Zapsat vzorek dat na stopu 2, sektor 1 (LEC: první sektor) */
    uint8_t marker[512];
    memset ( marker, 0xCD, sizeof ( marker ) );
    TEST_ASSERT_OK ( dsk_write_sector ( disc.handler, 2, 1, marker ) );

    /* Změnit sector IDs */
    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );
    edit.si_track = 2;
    panel_geom_edit_load_sector_ids ( &edit, &disc );

    for ( int i = 0; i < edit.si_count; i++ ) {
        edit.si_ids[i] = (uint8_t) ( 9 - i );
    }
    TEST_ASSERT_OK ( panel_geom_edit_apply_sector_ids ( &edit, &disc ) );

    /* Ověřit data přes raw offset (sektor 1 je na pozici podle nového ID mapování) */
    /* Data na fyzické pozici zůstávají - čteme přes raw offset */
    st_DSK_SHORT_IMAGE_INFO sii;
    TEST_ASSERT_OK ( dsk_read_short_image_info ( disc.handler, &sii ) );
    uint32_t track_offset = dsk_compute_track_offset ( 2, sii.tsize );
    uint8_t readback[512];
    /* Fyzicky prvni sektor = offset track_header + sizeof(track_info) */
    TEST_ASSERT_OK ( dsk_read_on_offset ( disc.handler,
                                           track_offset + sizeof ( st_DSK_TRACK_INFO ),
                                           readback, sizeof ( readback ) ) );
    TEST_ASSERT_EQ_MEM ( readback, marker, sizeof ( marker ) );

    cleanup_disc ( &disc );
    return 0;
}


/** @brief si_track mimo rozsah -> si_loaded == false. */
static int test_geom_load_sector_ids_out_of_range ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_sd_memory ( &disc ) );

    st_PANEL_GEOM_EDIT_DATA edit;
    panel_geom_edit_init ( &edit, &disc );

    edit.si_track = 999;
    panel_geom_edit_load_sector_ids ( &edit, &disc );
    TEST_ASSERT_EQ_INT ( edit.si_loaded, 0 );

    cleanup_disc ( &disc );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    memory_driver_init();
    TEST_INIT();

    RUN_TEST ( test_geom_load_cpm_sd );
    RUN_TEST ( test_geom_append_basic );
    RUN_TEST ( test_geom_append_preserves_data );
    RUN_TEST ( test_geom_append_odd_2sided_fails );
    RUN_TEST ( test_geom_shrink_basic );
    RUN_TEST ( test_geom_shrink_odd_2sided_fails );
    RUN_TEST ( test_geom_shrink_no_increase );
    RUN_TEST ( test_geom_change_track );
    RUN_TEST ( test_geom_change_track_out_of_range );
    RUN_TEST ( test_geom_append_shrink_roundtrip );
    RUN_TEST ( test_geom_append_fsmz );
    RUN_TEST ( test_geom_multiple_appends );
    RUN_TEST ( test_geom_triple_append );
    RUN_TEST ( test_geom_append_reload_append );
    RUN_TEST ( test_geom_change_track_reload );

    /* edit sector IDs */
    RUN_TEST ( test_geom_load_sector_ids );
    RUN_TEST ( test_geom_apply_sector_ids );
    RUN_TEST ( test_geom_apply_sector_ids_reload );
    RUN_TEST ( test_geom_apply_sector_ids_preserves_data );
    RUN_TEST ( test_geom_load_sector_ids_out_of_range );

    TEST_SUMMARY();
}
