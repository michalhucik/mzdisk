/**
 * @file test_dsk_tools.c
 * @brief Testy geometrických operací nad DSK obrazy.
 *
 * Všechny testy si vytvářejí disky od nuly v paměti.
 * Testuje: create, add_tracks, shrink, change_track.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
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


/* ====================================================================
 *  Pomocné funkce
 * ==================================================================== */

/**
 * @brief Vytvoří prázdný DSK obraz v paměti.
 *
 * Inicializuje handler s memory driverem (realloc variantou),
 * sestaví description z parametrů a zavolá dsk_tools_create_image().
 *
 * @param h Handler (musí být předalokovaný, nulovaný).
 * @param tracks_per_side Počet stop na stranu.
 * @param sides Počet stran (1 nebo 2).
 * @param sectors Počet sektorů na stopu.
 * @param ssize Kódovaná velikost sektoru.
 * @param filler Filler byte.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int create_test_disk ( st_HANDLER *h,
                               int tracks_per_side, int sides,
                               int sectors, en_DSK_SECTOR_SIZE ssize,
                               uint8_t filler )
{
    memset ( h, 0, sizeof ( st_HANDLER ) );
    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;

    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) return EXIT_FAILURE;

    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = (uint8_t) tracks_per_side;
    desc->sides = (uint8_t) sides;

    dsk_tools_assign_description ( desc, 0, 0,
                                    (uint8_t) sectors, ssize,
                                    DSK_SEC_ORDER_NORMAL, NULL, filler );

    int res = dsk_tools_create_image ( h, desc );
    free ( desc );
    return res;
}


/**
 * @brief Uvolní paměťový buffer handleru.
 *
 * @param h Handler s paměťovým driverem.
 */
static void free_test_disk ( st_HANDLER *h )
{
    if ( h->spec.memspec.ptr ) {
        free ( h->spec.memspec.ptr );
        h->spec.memspec.ptr = NULL;
        h->spec.memspec.size = 0;
    }
}


/**
 * @brief Přečte DSK header z handleru.
 *
 * @param h Handler.
 * @param hdr Výstupní buffer pro header.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int read_header ( st_HANDLER *h, st_DSK_HEADER *hdr )
{

    return generic_driver_read ( h, 0, hdr, sizeof ( st_DSK_HEADER ) );
}


/* ====================================================================
 *  Testy: vytváření DSK
 * ==================================================================== */


/** @brief Vytvoření 1-sided disku (80T, 9s, 512B). */
static int test_create_1sided ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 80, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );

    TEST_ASSERT_EQ_INT ( hdr.tracks, 80 );
    TEST_ASSERT_EQ_INT ( hdr.sides, 1 );

    /* Ověř fileinfo */
    TEST_ASSERT_EQ_MEM ( hdr.file_info, DSK_DEFAULT_FILEINFO, DSK_FILEINFO_FIELD_LENGTH );

    /* Ověř tsize - všech 80 stop musí mít nenulový tsize */
    for ( int i = 0; i < 80; i++ ) {
        TEST_ASSERT ( hdr.tsize[i] != 0, "tsize must be non-zero for existing tracks" );
    }
    /* Stopy za koncem musí být 0 */
    for ( int i = 80; i < DSK_MAX_TOTAL_TRACKS; i++ ) {
        TEST_ASSERT_EQ_INT ( hdr.tsize[i], 0 );
    }

    free_test_disk ( &h );
    return 0;
}


/** @brief Vytvoření 2-sided disku (80T x 2, 9s, 512B). */
static int test_create_2sided ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 80, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );

    TEST_ASSERT_EQ_INT ( hdr.tracks, 80 );
    TEST_ASSERT_EQ_INT ( hdr.sides, 2 );

    /* 160 absolutních stop (80 x 2 strany) */
    for ( int i = 0; i < 160; i++ ) {
        TEST_ASSERT ( hdr.tsize[i] != 0, "tsize must be non-zero" );
    }
    for ( int i = 160; i < DSK_MAX_TOTAL_TRACKS; i++ ) {
        TEST_ASSERT_EQ_INT ( hdr.tsize[i], 0 );
    }

    free_test_disk ( &h );
    return 0;
}


/** @brief Vytvoření FSMZ-like disku (80T, 1s, 16s x 256B). */
static int test_create_fsmz_geometry ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 80, 1, 16, DSK_SECTOR_SIZE_256, 0x00 ) );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );

    TEST_ASSERT_EQ_INT ( hdr.tracks, 80 );
    TEST_ASSERT_EQ_INT ( hdr.sides, 1 );

    /* track rules musí ukazovat 16 sektorů x 256B */
    st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( rules );
    TEST_ASSERT_EQ_INT ( rules->total_tracks, 80 );
    TEST_ASSERT_EQ_INT ( rules->count_rules, 1 );
    TEST_ASSERT_EQ_INT ( rules->rule[0].sectors, 16 );
    TEST_ASSERT_EQ_INT ( rules->rule[0].ssize, DSK_SECTOR_SIZE_256 );
    dsk_tools_destroy_track_rules ( rules );

    free_test_disk ( &h );
    return 0;
}


/** @brief Filler byte musí být v datech nových sektorů. */
static int test_create_filler ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 1, 1, 1, DSK_SECTOR_SIZE_256, 0xAB ) );

    /* Přečti první sektor (za hlavičkou DSK + hlavičkou stopy) */
    uint8_t sector[256];

    uint32_t offset = sizeof ( st_DSK_HEADER ) + sizeof ( st_DSK_TRACK_INFO );
    TEST_ASSERT_OK ( generic_driver_read ( &h, offset, sector, 256 ) );

    /* Všechny bajty musí být 0xAB */
    for ( int i = 0; i < 256; i++ ) {
        TEST_ASSERT_BYTE ( sector, i, 0xAB );
    }

    free_test_disk ( &h );
    return 0;
}


/* ====================================================================
 *  Testy: add_tracks
 * ==================================================================== */


/** @brief Append 2 stop na 1-sided disk (80 -> 82). */
static int test_append_1sided ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 80, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    uint32_t size_before = h.spec.memspec.size;

    /* Sestavit description pro append: nový celkový počet = 82 */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = 82;  /* nový celkový počet stop na stranu */
    desc->sides = 1;
    dsk_tools_assign_description ( desc, 0, 80,  /* first new abs track */
                                    9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );

    TEST_ASSERT_OK ( dsk_tools_add_tracks ( &h, desc ) );
    free ( desc );

    /* Buffer se musel zvětšit */
    TEST_ASSERT ( h.spec.memspec.size > size_before, "buffer must grow" );

    /* Header musí mít 82 stop */
    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 82 );
    TEST_ASSERT_EQ_INT ( hdr.sides, 1 );

    /* tsize pro 80..81 musí být nenulové */
    TEST_ASSERT ( hdr.tsize[80] != 0, "new track 80 tsize" );
    TEST_ASSERT ( hdr.tsize[81] != 0, "new track 81 tsize" );
    TEST_ASSERT_EQ_INT ( hdr.tsize[82], 0 );

    /* Track rules musí potvrzovat 82 stop */
    st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( rules );
    TEST_ASSERT_EQ_INT ( rules->total_tracks, 82 );
    dsk_tools_destroy_track_rules ( rules );

    free_test_disk ( &h );
    return 0;
}


/** @brief Append 2 stop na 2-sided disk (80x2=160 -> 81x2=162). */
static int test_append_2sided ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 80, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* description: nový celkový = 81 stop na stranu, first new abs track = 160 */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = 81;
    desc->sides = 2;
    dsk_tools_assign_description ( desc, 0, 160,
                                    9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );

    TEST_ASSERT_OK ( dsk_tools_add_tracks ( &h, desc ) );
    free ( desc );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 81 );
    TEST_ASSERT_EQ_INT ( hdr.sides, 2 );

    /* tsize[160] a tsize[161] musí být nenulové */
    TEST_ASSERT ( hdr.tsize[160] != 0, "new track 160 tsize" );
    TEST_ASSERT ( hdr.tsize[161] != 0, "new track 161 tsize" );
    TEST_ASSERT_EQ_INT ( hdr.tsize[162], 0 );

    st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( rules );
    TEST_ASSERT_EQ_INT ( rules->total_tracks, 162 );
    TEST_ASSERT_EQ_INT ( rules->sides, 2 );
    dsk_tools_destroy_track_rules ( rules );

    free_test_disk ( &h );
    return 0;
}


/** @brief Append nesmí poškodit existující data na starých stopách. */
static int test_append_preserves_old_data ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 2, 1, 1, DSK_SECTOR_SIZE_256, 0x42 ) );

    /* Zapamatuj si header a první stopu před appendem */
    uint32_t track0_offset = sizeof ( st_DSK_HEADER );
    st_DSK_TRACK_INFO trk0;

    TEST_ASSERT_OK ( generic_driver_read ( &h, track0_offset, &trk0,
                                            sizeof ( st_DSK_TRACK_INFO )) );

    /* Append 1 stopu */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = 3;
    desc->sides = 1;
    dsk_tools_assign_description ( desc, 0, 2, 1, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xFF );
    TEST_ASSERT_OK ( dsk_tools_add_tracks ( &h, desc ) );
    free ( desc );

    /* Stará stopa 0 musí mít zachovaná data */
    st_DSK_TRACK_INFO trk0_after;
    TEST_ASSERT_OK ( generic_driver_read ( &h, track0_offset, &trk0_after,
                                            sizeof ( st_DSK_TRACK_INFO )) );
    TEST_ASSERT_EQ_MEM ( &trk0, &trk0_after, sizeof ( st_DSK_TRACK_INFO ) );

    /* Starý sektor musí mít filler 0x42 */
    uint8_t sector[256];
    TEST_ASSERT_OK ( generic_driver_read ( &h, track0_offset + sizeof ( st_DSK_TRACK_INFO ),
                                            sector, 256) );
    TEST_ASSERT_BYTE ( sector, 0, 0x42 );
    TEST_ASSERT_BYTE ( sector, 255, 0x42 );

    /* Nová stopa musí mít filler 0xFF */
    uint32_t new_track_offset = dsk_compute_track_offset ( 2, NULL );
    /* Potřebujeme přečíst tsize z headeru */
    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    new_track_offset = dsk_compute_track_offset ( 2, hdr.tsize );
    uint8_t new_sector[256];
    TEST_ASSERT_OK ( generic_driver_read ( &h, new_track_offset + sizeof ( st_DSK_TRACK_INFO ),
                                            new_sector, 256) );
    TEST_ASSERT_BYTE ( new_sector, 0, 0xFF );

    free_test_disk ( &h );
    return 0;
}


/* ====================================================================
 *  Testy: shrink
 * ==================================================================== */


/** @brief Shrink 1-sided disku z 80 na 40 stop. */
static int test_shrink_1sided ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 80, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    uint32_t size_before = h.spec.memspec.size;

    TEST_ASSERT_OK ( dsk_tools_shrink_image ( &h, NULL, 40 ) );

    /* Buffer se musel zmenšit */
    TEST_ASSERT ( h.spec.memspec.size < size_before, "buffer must shrink" );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 40 );
    TEST_ASSERT_EQ_INT ( hdr.sides, 1 );

    /* tsize za stopou 39 musí být 0 */
    for ( int i = 40; i < DSK_MAX_TOTAL_TRACKS; i++ ) {
        TEST_ASSERT_EQ_INT ( hdr.tsize[i], 0 );
    }

    free_test_disk ( &h );
    return 0;
}


/** @brief Shrink 2-sided disku z 160 na 80 stop. */
static int test_shrink_2sided ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 80, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    TEST_ASSERT_OK ( dsk_tools_shrink_image ( &h, NULL, 80 ) );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 40 );  /* 80 abs / 2 sides = 40 per side */
    TEST_ASSERT_EQ_INT ( hdr.sides, 2 );

    st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( rules );
    TEST_ASSERT_EQ_INT ( rules->total_tracks, 80 );
    dsk_tools_destroy_track_rules ( rules );

    free_test_disk ( &h );
    return 0;
}


/** @brief Shrink na 0 stop musí selhat. */
static int test_shrink_to_zero_fails ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    TEST_ASSERT_FAIL ( dsk_tools_shrink_image ( &h, NULL, 0 ) );

    free_test_disk ( &h );
    return 0;
}


/** @brief Shrink na lichý počet stop u 2-sided disku musí selhat. */
static int test_shrink_odd_2sided_fails ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    TEST_ASSERT_FAIL ( dsk_tools_shrink_image ( &h, NULL, 5 ) );

    free_test_disk ( &h );
    return 0;
}


/* ====================================================================
 *  Testy: append + shrink roundtrip
 * ==================================================================== */


/** @brief Append a pak shrink zpět na původní velikost. */
static int test_append_then_shrink_roundtrip ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 40, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Append 10 stop (40 -> 50) */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = 50;
    desc->sides = 1;
    dsk_tools_assign_description ( desc, 0, 40, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    TEST_ASSERT_OK ( dsk_tools_add_tracks ( &h, desc ) );
    free ( desc );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 50 );

    /* Shrink zpět na 40 */
    TEST_ASSERT_OK ( dsk_tools_shrink_image ( &h, NULL, 40 ) );

    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 40 );

    st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( rules );
    TEST_ASSERT_EQ_INT ( rules->total_tracks, 40 );
    dsk_tools_destroy_track_rules ( rules );

    free_test_disk ( &h );
    return 0;
}


/* ====================================================================
 *  Testy: edge cases - maximální a minimální velikosti
 * ==================================================================== */


/** @brief Append až na DSK_MAX_TOTAL_TRACKS (204) stop. */
static int test_append_to_max_tracks ( void )
{
    st_HANDLER h;
    /* 1-sided, 100 stop, malé sektory pro rychlost */
    TEST_ASSERT_OK ( create_test_disk ( &h, 100, 1, 1, DSK_SECTOR_SIZE_128, 0x00 ) );

    /* Append 104 stop (100 -> 204 = DSK_MAX_TOTAL_TRACKS) */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = DSK_MAX_TOTAL_TRACKS;
    desc->sides = 1;
    dsk_tools_assign_description ( desc, 0, 100, 1, DSK_SECTOR_SIZE_128,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0x00 );
    TEST_ASSERT_OK ( dsk_tools_add_tracks ( &h, desc ) );
    free ( desc );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, DSK_MAX_TOTAL_TRACKS );

    st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( rules );
    TEST_ASSERT_EQ_INT ( rules->total_tracks, DSK_MAX_TOTAL_TRACKS );
    dsk_tools_destroy_track_rules ( rules );

    free_test_disk ( &h );
    return 0;
}


/** @brief Shrink na 1 stopu (minimum). */
static int test_shrink_to_minimum ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 1, DSK_SECTOR_SIZE_128, 0x00 ) );

    TEST_ASSERT_OK ( dsk_tools_shrink_image ( &h, NULL, 1 ) );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 1 );

    free_test_disk ( &h );
    return 0;
}


/** @brief Shrink na 2 stopy u 2-sided disku (minimum). */
static int test_shrink_2sided_to_minimum ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 2, 1, DSK_SECTOR_SIZE_128, 0x00 ) );

    TEST_ASSERT_OK ( dsk_tools_shrink_image ( &h, NULL, 2 ) );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 1 );  /* 2 abs / 2 sides = 1 per side */
    TEST_ASSERT_EQ_INT ( hdr.sides, 2 );

    free_test_disk ( &h );
    return 0;
}


/** @brief Vytvoření disku se všemi podporovanými velikostmi sektorů. */
static int test_create_all_sector_sizes ( void )
{
    en_DSK_SECTOR_SIZE sizes[] = {
        DSK_SECTOR_SIZE_128, DSK_SECTOR_SIZE_256,
        DSK_SECTOR_SIZE_512, DSK_SECTOR_SIZE_1024
    };
    int expected_bytes[] = { 128, 256, 512, 1024 };

    for ( int i = 0; i < 4; i++ ) {
        st_HANDLER h;
        TEST_ASSERT_OK ( create_test_disk ( &h, 2, 1, 4, sizes[i], 0xAA ) );

        st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( &h );
        TEST_ASSERT_NOT_NULL ( rules );
        TEST_ASSERT_EQ_INT ( rules->rule[0].ssize, sizes[i] );
        TEST_ASSERT_EQ_INT ( dsk_decode_sector_size ( rules->rule[0].ssize ),
                              expected_bytes[i] );
        dsk_tools_destroy_track_rules ( rules );

        free_test_disk ( &h );
    }
    return 0;
}


/** @brief Append s jinou velikostí sektorů než původní stopy. */
static int test_append_different_sector_size ( void )
{
    st_HANDLER h;
    /* Původní: 9x512B */
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Append: 16x256B (jiná geometrie) */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = 12;
    desc->sides = 1;
    dsk_tools_assign_description ( desc, 0, 10, 16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0x00 );
    TEST_ASSERT_OK ( dsk_tools_add_tracks ( &h, desc ) );
    free ( desc );

    st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( &h );
    TEST_ASSERT_NOT_NULL ( rules );
    TEST_ASSERT_EQ_INT ( rules->total_tracks, 12 );
    /* Musí být 2 pravidla (9x512B + 16x256B) */
    TEST_ASSERT ( rules->count_rules >= 2, "at least 2 rules after mixed append" );
    dsk_tools_destroy_track_rules ( rules );

    free_test_disk ( &h );
    return 0;
}


/* ====================================================================
 *  set_sector_id / set_sector_ids
 * ==================================================================== */


/** @brief Změna jednoho sector ID na stopě (idx=0, new_id=42). */
static int test_set_sector_id_basic ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Změnit sector ID na indexu 0 z výchozího (1) na 42 */
    TEST_ASSERT_OK ( dsk_tools_set_sector_id ( &h, 0, 0, 42 ) );

    /* Zpětně přečíst a ověřit */
    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    TEST_ASSERT_EQ_INT ( info.sinfo[0].sector, 42 );
    /* Ostatní sektory nedotčeny */
    TEST_ASSERT_EQ_INT ( info.sinfo[1].sector, 2 );
    TEST_ASSERT_EQ_INT ( info.sinfo[8].sector, 9 );

    free_test_disk ( &h );
    return 0;
}


/** @brief Změna posledního sektoru na stopě (idx=sectors-1). */
static int test_set_sector_id_last ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    TEST_ASSERT_OK ( dsk_tools_set_sector_id ( &h, 0, 8, 99 ) );

    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    TEST_ASSERT_EQ_INT ( info.sinfo[8].sector, 99 );
    /* Prvních 8 sektorů nedotčeno */
    TEST_ASSERT_EQ_INT ( info.sinfo[0].sector, 1 );

    free_test_disk ( &h );
    return 0;
}


/** @brief sector_idx >= sectors -> EXIT_FAILURE. */
static int test_set_sector_id_out_of_range ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    TEST_ASSERT_FAIL ( dsk_tools_set_sector_id ( &h, 0, 9, 42 ) );
    TEST_ASSERT_FAIL ( dsk_tools_set_sector_id ( &h, 0, 255, 42 ) );

    free_test_disk ( &h );
    return 0;
}


/** @brief Přepsání všech ID najednou (9 sektorů). */
static int test_set_sector_ids_all ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    uint8_t new_ids[] = { 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    TEST_ASSERT_OK ( dsk_tools_set_sector_ids ( &h, 0, new_ids, 9 ) );

    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    for ( int i = 0; i < 9; i++ ) {
        TEST_ASSERT_EQ_INT ( info.sinfo[i].sector, new_ids[i] );
    }

    free_test_disk ( &h );
    return 0;
}


/** @brief count != skutečný počet sektorů -> EXIT_FAILURE. */
static int test_set_sector_ids_count_mismatch ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    uint8_t ids5[] = { 1, 2, 3, 4, 5 };
    TEST_ASSERT_FAIL ( dsk_tools_set_sector_ids ( &h, 0, ids5, 5 ) );

    uint8_t ids10[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    TEST_ASSERT_FAIL ( dsk_tools_set_sector_ids ( &h, 0, ids10, 10 ) );

    free_test_disk ( &h );
    return 0;
}


/** @brief Po změně ID zůstanou sektorová data beze změny. */
static int test_set_sector_ids_preserves_data ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Zapsat vzorek dat do prvního sektoru (offset = track header + 0) */
    uint8_t marker[512];
    memset ( marker, 0xAB, sizeof ( marker ) );
    /* offset stopy 0 = sizeof(st_DSK_HEADER) = 256, data za track headerem (+256) */
    uint32_t data_offset = 256 + sizeof ( st_DSK_TRACK_INFO );
    TEST_ASSERT_OK ( dsk_write_on_offset ( &h, data_offset, marker, sizeof ( marker ) ) );

    /* Změnit sector IDs */
    uint8_t new_ids[] = { 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    TEST_ASSERT_OK ( dsk_tools_set_sector_ids ( &h, 0, new_ids, 9 ) );

    /* Ověřit, že data jsou nedotčena */
    uint8_t readback[512];
    TEST_ASSERT_OK ( dsk_read_on_offset ( &h, data_offset, readback, sizeof ( readback ) ) );
    TEST_ASSERT_EQ_MEM ( readback, marker, sizeof ( marker ) );

    free_test_disk ( &h );
    return 0;
}


/** @brief Po změně ID zůstanou FDC status registry beze změny. */
static int test_set_sector_ids_preserves_fdc ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Nastavit FDC status na sektoru 0 */
    TEST_ASSERT_OK ( dsk_tools_set_sector_fdc_status ( &h, 0, 0, 0x40, 0x20 ) );

    /* Ověřit nastavení */
    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    TEST_ASSERT_EQ_INT ( info.sinfo[0].fdc_sts1, 0x40 );
    TEST_ASSERT_EQ_INT ( info.sinfo[0].fdc_sts2, 0x20 );

    /* Změnit sector IDs */
    uint8_t new_ids[] = { 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    TEST_ASSERT_OK ( dsk_tools_set_sector_ids ( &h, 0, new_ids, 9 ) );

    /* Ověřit, že FDC status zůstal */
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    TEST_ASSERT_EQ_INT ( info.sinfo[0].fdc_sts1, 0x40 );
    TEST_ASSERT_EQ_INT ( info.sinfo[0].fdc_sts2, 0x20 );

    free_test_disk ( &h );
    return 0;
}


/**
 * @brief Shrink zachová custom sector IDs na zůstávajících stopách.
 *
 * Regrese: shrink se týká konce disku (odřezává stopy), takže custom
 * IDs zapsaná na začátku musí zůstat beze změny.
 */
static int test_shrink_preserves_sector_ids ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 80, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Přepsat IDs na stopě 0 na obrácené pořadí */
    uint8_t new_ids[] = { 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    TEST_ASSERT_OK ( dsk_tools_set_sector_ids ( &h, 0, new_ids, 9 ) );

    /* Shrink z 80 na 40 stop */
    TEST_ASSERT_OK ( dsk_tools_shrink_image ( &h, NULL, 40 ) );

    st_DSK_HEADER hdr;
    TEST_ASSERT_OK ( read_header ( &h, &hdr ) );
    TEST_ASSERT_EQ_INT ( hdr.tracks, 40 );

    /* Stopa 0 musí mít stále upravená IDs */
    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    for ( int i = 0; i < 9; i++ ) {
        TEST_ASSERT_EQ_INT ( info.sinfo[i].sector, new_ids[i] );
    }

    free_test_disk ( &h );
    return 0;
}


/**
 * @brief Append nových stop nezmění custom sector IDs na existujících stopách.
 *
 * Regrese: add_tracks smí ovlivnit jen nově přidané stopy, staré IDs
 * musí zůstat beze změny.
 */
static int test_append_preserves_sector_ids ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 40, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Nastavit custom IDs na stopě 0 */
    uint8_t new_ids[] = { 5, 4, 3, 2, 1, 9, 8, 7, 6 };
    TEST_ASSERT_OK ( dsk_tools_set_sector_ids ( &h, 0, new_ids, 9 ) );

    /* Append 5 stop (40 -> 45) se stejnou geometrií */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = 45;
    desc->sides = 1;
    dsk_tools_assign_description ( desc, 0, 40, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    TEST_ASSERT_OK ( dsk_tools_add_tracks ( &h, desc ) );
    free ( desc );

    /* Stopa 0 musí mít stále upravená IDs */
    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    for ( int i = 0; i < 9; i++ ) {
        TEST_ASSERT_EQ_INT ( info.sinfo[i].sector, new_ids[i] );
    }

    /* Nové stopy mají výchozí IDs (1..9) */
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 44, &info ) );
    for ( int i = 0; i < 9; i++ ) {
        TEST_ASSERT_EQ_INT ( info.sinfo[i].sector, i + 1 );
    }

    free_test_disk ( &h );
    return 0;
}


/**
 * @brief Zápis duplicitních sector ID na stopě projde bez chyby.
 *
 * dsk_tools_set_sector_ids nevaliduje unikátnost - duplicita je
 * explicitně povolena (některé kopírky/chráněné disky ji používají).
 * Test dokumentuje, že zápis projde a data se přečtou tak, jak byla
 * zapsána.
 */
static int test_set_sector_ids_duplicate_allowed ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 10, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Duplicita ID 1 na prvních dvou slotech */
    uint8_t dup_ids[] = { 1, 1, 2, 3, 4, 5, 6, 7, 8 };
    TEST_ASSERT_OK ( dsk_tools_set_sector_ids ( &h, 0, dup_ids, 9 ) );

    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    for ( int i = 0; i < 9; i++ ) {
        TEST_ASSERT_EQ_INT ( info.sinfo[i].sector, dup_ids[i] );
    }

    free_test_disk ( &h );
    return 0;
}


/* ====================================================================
 *  Délka dat sektoru v Sector Info (bug 2026-11-10-003)
 * ==================================================================== */


/**
 * @brief Vynuluje pole délky dat u všech sektorů jedné stopy.
 *
 * Simuluje obraz vytvořený knihovnou dsk_tools < 3.4.0, která pole
 * délky dat nevyplňovala.
 *
 * @param h Handler s DSK obrazem.
 * @param abstrack Absolutní číslo stopy.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int zero_track_data_lengths ( st_HANDLER *h, uint8_t abstrack )
{
    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;
    uint32_t offset = dsk_compute_track_offset ( abstrack, iinfo.tsize );

    st_DSK_TRACK_INFO trk;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, offset, &trk, sizeof ( trk ) ) ) return EXIT_FAILURE;
    for ( int i = 0; i < trk.sectors; i++ ) {
        dsk_sector_info_set_data_length ( &trk.sinfo[i], 0 );
    }
    return dsk_write_on_offset ( h, offset, &trk, sizeof ( trk ) );
}


/** @brief Nově vytvořený obraz má u každého sektoru vyplněnou délku dat. */
static int test_create_sets_sector_data_length ( void )
{
    static const struct { en_DSK_SECTOR_SIZE ssize; int sectors; int bytes; } cases[] = {
        { DSK_SECTOR_SIZE_128,  9,  128 },
        { DSK_SECTOR_SIZE_256,  16, 256 },
        { DSK_SECTOR_SIZE_512,  9,  512 },
        { DSK_SECTOR_SIZE_1024, 5,  1024 },
    };

    for ( size_t c = 0; c < sizeof ( cases ) / sizeof ( cases[0] ); c++ ) {
        st_HANDLER h;
        TEST_ASSERT_OK ( create_test_disk ( &h, 2, 2, cases[c].sectors, cases[c].ssize, 0xE5 ) );

        for ( int t = 0; t < 4; t++ ) {
            st_DSK_TRACK_HEADER_INFO info;
            TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, (uint8_t) t, &info ) );
            for ( int i = 0; i < cases[c].sectors; i++ ) {
                TEST_ASSERT_EQ_INT ( dsk_sector_info_get_data_length ( &info.sinfo[i] ), cases[c].bytes );
            }
        }

        /* Uložení je little-endian: nižší bajt na offsetu 6, vyšší na 7 */
        st_DSK_TRACK_INFO trk;
        TEST_ASSERT_OK ( dsk_read_on_offset ( &h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );
        const uint8_t *raw = (const uint8_t *) &trk.sinfo[0];
        TEST_ASSERT_EQ_INT ( raw[6], cases[c].bytes & 0xff );
        TEST_ASSERT_EQ_INT ( raw[7], cases[c].bytes >> 8 );

        free_test_disk ( &h );
    }
    return 0;
}


/** @brief dsk_tools_change_track zapíše délku dat podle nové velikosti sektoru. */
static int test_change_track_sets_sector_data_length ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 4, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    uint8_t map[16];
    for ( int i = 0; i < 16; i++ ) map[i] = (uint8_t) ( i + 1 );
    TEST_ASSERT_OK ( dsk_tools_change_track ( &h, NULL, 1, 16, DSK_SECTOR_SIZE_256, map, 0xFF ) );

    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 1, &info ) );
    for ( int i = 0; i < 16; i++ ) {
        TEST_ASSERT_EQ_INT ( dsk_sector_info_get_data_length ( &info.sinfo[i] ), 256 );
    }

    /* Sousední stopa zůstává 512 B */
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 2, &info ) );
    TEST_ASSERT_EQ_INT ( dsk_sector_info_get_data_length ( &info.sinfo[0] ), 512 );

    free_test_disk ( &h );
    return 0;
}


/** @brief Diagnostika nově vytvořeného obrazu nehlásí žádný problém. */
static int test_diag_new_image_is_clean ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 5, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( &h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQ_INT ( diag->image_flags, DSK_DIAG_IMAGE_OK );
    TEST_ASSERT_EQ_INT ( dsk_tools_diag_has_repairable_errors ( diag ), 0 );
    dsk_tools_destroy_diag_result ( diag );

    free_test_disk ( &h );
    return 0;
}


/**
 * @brief Nulová délka dat sektoru je detekována jako opravitelná chyba.
 *
 * Nesmí být fatální a nesmí nastavit souhrnný flag TRACK_ERRORS.
 */
static int test_diag_detects_zero_data_length ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 5, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );
    TEST_ASSERT_OK ( zero_track_data_lengths ( &h, 3 ) );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( &h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQ_INT ( diag->image_flags, DSK_DIAG_IMAGE_ZERO_DATALEN );
    TEST_ASSERT_EQ_INT ( diag->zero_datalen_tracks, 1 );
    for ( int t = 0; t < diag->count_tracks; t++ ) {
        TEST_ASSERT_EQ_INT ( diag->tracks[t].flags,
                             ( t == 3 ) ? DSK_DIAG_TRACK_ZERO_DATALEN : DSK_DIAG_TRACK_OK );
    }
    TEST_ASSERT_EQ_INT ( dsk_tools_diag_has_repairable_errors ( diag ), 1 );
    TEST_ASSERT_EQ_INT ( dsk_tools_diag_has_fatal_errors ( diag ), 0 );
    dsk_tools_destroy_diag_result ( diag );

    free_test_disk ( &h );
    return 0;
}


/**
 * @brief Repair doplní nulové délky dat a nic jiného nezmění.
 *
 * Kontroluje se celý obraz: po opravě musí být bajt po bajtu shodný
 * s kopií pořízenou před vynulováním délek (včetně vlastního FDC
 * statusu, ID sektoru a sektorových dat).
 */
static int test_repair_fills_zero_data_length ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 5, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    /* Vlastní FDC status, ID sektoru a data, aby bylo co poškodit */
    TEST_ASSERT_OK ( dsk_tools_set_sector_fdc_status ( &h, 3, 2, 0x40, 0x20 ) );
    TEST_ASSERT_OK ( dsk_tools_set_sector_id ( &h, 3, 4, 77 ) );
    uint8_t marker[512];
    memset ( marker, 0xAB, sizeof ( marker ) );
    st_DSK_SHORT_IMAGE_INFO iinfo;
    TEST_ASSERT_OK ( dsk_read_short_image_info ( &h, &iinfo ) );
    uint32_t t3 = dsk_compute_track_offset ( 3, iinfo.tsize );
    TEST_ASSERT_OK ( dsk_write_on_offset ( &h, t3 + sizeof ( st_DSK_TRACK_INFO ), marker, sizeof ( marker ) ) );

    /* Referenční kopie správného obrazu */
    uint32_t size = 0;
    TEST_ASSERT_OK ( generic_driver_get_size ( &h, &size ) );
    uint8_t *reference = (uint8_t *) malloc ( size );
    TEST_ASSERT_NOT_NULL ( reference );
    for ( uint32_t off = 0; off < size; off += 256 ) {
        TEST_ASSERT_OK ( dsk_read_on_offset ( &h, off, reference + off, 256 ) );
    }

    /* Poškození: nulové délky na dvou stopách */
    TEST_ASSERT_OK ( zero_track_data_lengths ( &h, 0 ) );
    TEST_ASSERT_OK ( zero_track_data_lengths ( &h, 3 ) );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( &h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQ_INT ( diag->zero_datalen_tracks, 2 );
    TEST_ASSERT_OK ( dsk_tools_repair ( &h, diag ) );
    dsk_tools_destroy_diag_result ( diag );

    /* Po opravě je obraz bajt po bajtu shodný s referencí */
    uint32_t size_after = 0;
    TEST_ASSERT_OK ( generic_driver_get_size ( &h, &size_after ) );
    TEST_ASSERT_EQ_UINT ( size_after, size );
    for ( uint32_t off = 0; off < size; off += 256 ) {
        uint8_t buf[256];
        TEST_ASSERT_OK ( dsk_read_on_offset ( &h, off, buf, sizeof ( buf ) ) );
        TEST_ASSERT_EQ_MEM ( buf, reference + off, sizeof ( buf ) );
    }
    free ( reference );

    diag = dsk_tools_diagnose ( &h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQ_INT ( diag->image_flags, DSK_DIAG_IMAGE_OK );
    dsk_tools_destroy_diag_result ( diag );

    free_test_disk ( &h );
    return 0;
}


/**
 * @brief Nenulová délka dat se při opravě nepřepisuje.
 *
 * Délka odlišná od velikosti dané polem N není diagnostikou hlášena
 * a repair se jí nedotkne, ani když na téže stopě opravuje jiný sektor.
 */
static int test_repair_keeps_nonzero_data_length ( void )
{
    st_HANDLER h;
    TEST_ASSERT_OK ( create_test_disk ( &h, 2, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 ) );

    st_DSK_TRACK_INFO trk;
    TEST_ASSERT_OK ( dsk_read_on_offset ( &h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );
    dsk_sector_info_set_data_length ( &trk.sinfo[1], 300 );
    TEST_ASSERT_OK ( dsk_write_on_offset ( &h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( &h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQ_INT ( diag->image_flags, DSK_DIAG_IMAGE_OK );
    dsk_tools_destroy_diag_result ( diag );

    TEST_ASSERT_OK ( dsk_read_on_offset ( &h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );
    dsk_sector_info_set_data_length ( &trk.sinfo[5], 0 );
    TEST_ASSERT_OK ( dsk_write_on_offset ( &h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );

    diag = dsk_tools_diagnose ( &h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_OK ( dsk_tools_repair ( &h, diag ) );
    dsk_tools_destroy_diag_result ( diag );

    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_OK ( dsk_tools_read_track_header_info ( &h, 0, &info ) );
    TEST_ASSERT_EQ_INT ( dsk_sector_info_get_data_length ( &info.sinfo[1] ), 300 );
    TEST_ASSERT_EQ_INT ( dsk_sector_info_get_data_length ( &info.sinfo[5] ), 512 );

    free_test_disk ( &h );
    return 0;
}


/* ====================================================================
 *  main
 * ==================================================================== */


int main ( void )
{
    memory_driver_init ();

    TEST_INIT ();

    /* vytváření DSK */
    RUN_TEST ( test_create_1sided );
    RUN_TEST ( test_create_2sided );
    RUN_TEST ( test_create_fsmz_geometry );
    RUN_TEST ( test_create_filler );
    RUN_TEST ( test_create_all_sector_sizes );

    /* add_tracks */
    RUN_TEST ( test_append_1sided );
    RUN_TEST ( test_append_2sided );
    RUN_TEST ( test_append_preserves_old_data );
    RUN_TEST ( test_append_to_max_tracks );
    RUN_TEST ( test_append_different_sector_size );

    /* shrink */
    RUN_TEST ( test_shrink_1sided );
    RUN_TEST ( test_shrink_2sided );
    RUN_TEST ( test_shrink_to_zero_fails );
    RUN_TEST ( test_shrink_odd_2sided_fails );
    RUN_TEST ( test_shrink_to_minimum );
    RUN_TEST ( test_shrink_2sided_to_minimum );

    /* roundtrip */
    RUN_TEST ( test_append_then_shrink_roundtrip );

    /* set_sector_id / set_sector_ids */
    RUN_TEST ( test_set_sector_id_basic );
    RUN_TEST ( test_set_sector_id_last );
    RUN_TEST ( test_set_sector_id_out_of_range );
    RUN_TEST ( test_set_sector_ids_all );
    RUN_TEST ( test_set_sector_ids_count_mismatch );
    RUN_TEST ( test_set_sector_ids_preserves_data );
    RUN_TEST ( test_set_sector_ids_preserves_fdc );

    /* interakce sector IDs s geometrickými operacemi (P2.1, P2.2) */
    RUN_TEST ( test_shrink_preserves_sector_ids );
    RUN_TEST ( test_append_preserves_sector_ids );
    RUN_TEST ( test_set_sector_ids_duplicate_allowed );

    /* délka dat sektoru v Sector Info (bug 2026-11-10-003) */
    RUN_TEST ( test_create_sets_sector_data_length );
    RUN_TEST ( test_change_track_sets_sector_data_length );
    RUN_TEST ( test_diag_new_image_is_clean );
    RUN_TEST ( test_diag_detects_zero_data_length );
    RUN_TEST ( test_repair_fills_zero_data_length );
    RUN_TEST ( test_repair_keeps_nonzero_data_length );

    TEST_SUMMARY ();
}
