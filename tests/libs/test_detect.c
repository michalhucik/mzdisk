/**
 * @file test_detect.c
 * @brief Testy auto-detekce FS a stability detekce po geometrických změnách.
 *
 * Vytváří disky od nuly, formátuje je a ověřuje, že mzdsk_detect_filesystem()
 * správně identifikuje typ FS. Testuje stabilitu detekce po append/shrink.
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
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"


/* ====================================================================
 *  Pomocné funkce
 * ==================================================================== */

/**
 * @brief Vytvoří DSK v paměti a inicializuje st_MZDSK_DISC.
 *
 * @param disc Výstupní diskový obraz.
 * @param tracks_per_side Počet stop na stranu.
 * @param sides Počet stran.
 * @param sectors Počet sektorů na stopu.
 * @param ssize Kódovaná velikost sektoru.
 * @param filler Filler byte.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int create_disc ( st_MZDSK_DISC *disc, int tracks_per_side, int sides,
                          int sectors, en_DSK_SECTOR_SIZE ssize, uint8_t filler )
{
    memset ( disc, 0, sizeof ( st_MZDSK_DISC ) );

    st_HANDLER *h = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    if ( !h ) return EXIT_FAILURE;

    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;

    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) { free ( h ); return EXIT_FAILURE; }

    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = (uint8_t) tracks_per_side;
    desc->sides = (uint8_t) sides;
    dsk_tools_assign_description ( desc, 0, 0, (uint8_t) sectors, ssize,
                                    DSK_SEC_ORDER_NORMAL, NULL, filler );

    int res = dsk_tools_create_image ( h, desc );
    free ( desc );

    if ( res != EXIT_SUCCESS ) {
        free ( h->spec.memspec.ptr );
        free ( h );
        return EXIT_FAILURE;
    }

    disc->handler = h;
    disc->tracks_rules = dsk_tools_get_tracks_rules ( h );
    disc->format = dsk_tools_identformat_from_tracks_rules ( disc->tracks_rules );
    disc->sector_info_cb = mzdsk_sector_info_cb;
    disc->sector_info_cb_data = disc;
    disc->cache = (uint8_t *) malloc ( 1024 );
    if ( !disc->cache ) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}


/**
 * @brief Vytvoří disk s CP/M-like geometrií (boot track + data).
 *
 * Reálný MZ CP/M disk má abs track 0 = data (9x512B),
 * abs track 1 = boot (16x256B), abs tracks 2+ = data (sectors x ssize).
 * Tato funkce reprodukuje tuto geometrii.
 *
 * @param disc Výstupní disk.
 * @param tracks_per_side Počet stop na stranu.
 * @param sides Počet stran.
 * @param data_sectors Počet sektorů na datových stopách (9=SD, 18=HD).
 * @param data_ssize Kódovaná velikost datového sektoru.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int create_cpm_like_disc ( st_MZDSK_DISC *disc, int tracks_per_side,
                                    int sides, int data_sectors,
                                    en_DSK_SECTOR_SIZE data_ssize )
{
    memset ( disc, 0, sizeof ( st_MZDSK_DISC ) );

    st_HANDLER *h = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    if ( !h ) return EXIT_FAILURE;

    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h->spec.memspec.size = 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;

    /* 3 pravidla: abs 0 = data, abs 1 = boot (16x256B), abs 2+ = data */
    size_t desc_size = dsk_tools_compute_description_size ( 3 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) { free ( h ); return EXIT_FAILURE; }

    memset ( desc, 0, desc_size );
    desc->count_rules = 3;
    desc->tracks = (uint8_t) tracks_per_side;
    desc->sides = (uint8_t) sides;

    /* abs track 0: data sektory */
    dsk_tools_assign_description ( desc, 0, 0,
                                    (uint8_t) data_sectors, data_ssize,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    /* abs track 1: MZ boot track (16x256B) */
    dsk_tools_assign_description ( desc, 1, 1,
                                    16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    /* abs track 2+: data sektory */
    dsk_tools_assign_description ( desc, 2, 2,
                                    (uint8_t) data_sectors, data_ssize,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );

    int res = dsk_tools_create_image ( h, desc );
    free ( desc );

    if ( res != EXIT_SUCCESS ) {
        free ( h->spec.memspec.ptr );
        free ( h );
        return EXIT_FAILURE;
    }

    disc->handler = h;
    disc->tracks_rules = dsk_tools_get_tracks_rules ( h );
    disc->format = dsk_tools_identformat_from_tracks_rules ( disc->tracks_rules );
    disc->sector_info_cb = mzdsk_sector_info_cb;
    disc->sector_info_cb_data = disc;
    disc->cache = (uint8_t *) malloc ( 1024 );
    if ( !disc->cache ) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}


/**
 * @brief Zavře testovací disk a uvolní paměť.
 */
static void close_disc ( st_MZDSK_DISC *disc )
{
    if ( disc->cache ) { free ( disc->cache ); disc->cache = NULL; }
    if ( disc->tracks_rules ) {
        dsk_tools_destroy_track_rules ( disc->tracks_rules );
        disc->tracks_rules = NULL;
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
 */
static int reload_rules ( st_MZDSK_DISC *disc )
{
    if ( disc->tracks_rules ) {
        dsk_tools_destroy_track_rules ( disc->tracks_rules );
    }
    disc->tracks_rules = dsk_tools_get_tracks_rules ( disc->handler );
    if ( !disc->tracks_rules ) return EXIT_FAILURE;
    disc->format = dsk_tools_identformat_from_tracks_rules ( disc->tracks_rules );
    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Testy: detekce na čerstvě vytvořených discích
 * ==================================================================== */


/** @brief FSMZ disk (16x256B) po formátování je detekován jako FSMZ. */
static int test_detect_fsmz_formatted ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_disc ( &disc, 80, 1, 16, DSK_SECTOR_SIZE_256, 0x00 ) );

    /* Formátovat - inicializuje DINFO a adresář */
    TEST_ASSERT ( fsmz_tool_fast_format ( &disc ) == MZDSK_RES_OK,
                  "fsmz_tool_fast_format" );

    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK,
                  "mzdsk_detect_filesystem" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_FSMZ );

    close_disc ( &disc );
    return 0;
}


/** @brief Neformátovaný FSMZ disk (16x256B) je detekován jako FSMZ. */
static int test_detect_fsmz_unformatted ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_disc ( &disc, 80, 1, 16, DSK_SECTOR_SIZE_256, 0x00 ) );

    /* FSMZ se detekuje z geometrie, ne z obsahu */
    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK,
                  "mzdsk_detect_filesystem" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_FSMZ );

    close_disc ( &disc );
    return 0;
}


/** @brief CP/M SD disk po formátování je detekován jako CP/M. */
static int test_detect_cpm_sd_formatted ( void )
{
    /* CP/M SD geometrie: abs track 0 = 9x512B, abs track 1 = 16x256B (boot),
       abs tracks 2-159 = 9x512B. Detect vyžaduje boot track pro identformat. */
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_like_disc ( &disc, 80, 2, 9, DSK_SECTOR_SIZE_512 ) );

    /* Inicializovat DPB a formátovat adresář */
    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, MZDSK_CPM_FORMAT_SD );
    TEST_ASSERT ( mzdsk_cpm_format_directory ( &disc, &dpb ) == MZDSK_RES_OK,
                  "mzdsk_cpm_format_directory" );

    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK,
                  "mzdsk_detect_filesystem" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_CPM );

    close_disc ( &disc );
    return 0;
}


/** @brief MRS disk po inicializaci je detekován jako MRS. */
static int test_detect_mrs_initialized ( void )
{
    /* MRS má stejnou geometrii jako CP/M SD - boot track + 9x512B */
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_like_disc ( &disc, 80, 2, 9, DSK_SECTOR_SIZE_512 ) );

    /* fsmrs_format_fs inicializuje FAT a adresář */
    TEST_ASSERT ( fsmrs_format_fs ( &disc, MZDSK_DETECT_MRS_DEFAULT_FAT_BLOCK ) == MZDSK_RES_OK,
                  "fsmrs_format_fs" );

    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK,
                  "mzdsk_detect_filesystem" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_MRS );

    close_disc ( &disc );
    return 0;
}


/** @brief Prázdný disk (9x512B) bez FS je UNKNOWN. */
static int test_detect_unknown_empty ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_disc ( &disc, 40, 1, 9, DSK_SECTOR_SIZE_512, 0x00 ) );

    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK,
                  "mzdsk_detect_filesystem" );
    /* Bez boot track a bez FS -> UNKNOWN */
    TEST_ASSERT ( result.type == MZDSK_FS_UNKNOWN || result.type == MZDSK_FS_BOOT_ONLY,
                  "empty disk should be UNKNOWN or BOOT_ONLY" );

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  Testy: stabilita detekce po geometrických změnách
 * ==================================================================== */


/** @brief Append stop na CP/M SD disk nezmění detekci. */
static int test_detect_cpm_after_append ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_like_disc ( &disc, 80, 2, 9, DSK_SECTOR_SIZE_512 ) );

    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, MZDSK_CPM_FORMAT_SD );
    TEST_ASSERT ( mzdsk_cpm_format_directory ( &disc, &dpb ) == MZDSK_RES_OK,
                  "format" );

    /* Ověřit CP/M detekci před appendem */
    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect before" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_CPM );

    /* Append 2 stopy (160 -> 162) */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = 81;
    desc->sides = 2;
    dsk_tools_assign_description ( desc, 0, 160, 9, DSK_SECTOR_SIZE_512,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0xE5 );
    TEST_ASSERT_OK ( dsk_tools_add_tracks ( disc.handler, desc ) );
    free ( desc );

    /* Reload pravidel */
    TEST_ASSERT_OK ( reload_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 162 );

    /* Detekce musí stále najít CP/M */
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect after" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_CPM );

    close_disc ( &disc );
    return 0;
}


/** @brief Append stop na FSMZ disk nezmění detekci. */
static int test_detect_fsmz_after_append ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_disc ( &disc, 80, 1, 16, DSK_SECTOR_SIZE_256, 0x00 ) );
    TEST_ASSERT ( fsmz_tool_fast_format ( &disc ) == MZDSK_RES_OK, "format" );

    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect before" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_FSMZ );

    /* Append 5 stop se stejnou geometrií */
    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->count_rules = 1;
    desc->tracks = 85;
    desc->sides = 1;
    dsk_tools_assign_description ( desc, 0, 80, 16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0x00 );
    TEST_ASSERT_OK ( dsk_tools_add_tracks ( disc.handler, desc ) );
    free ( desc );

    TEST_ASSERT_OK ( reload_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 85 );

    /* Stále FSMZ */
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect after" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_FSMZ );

    close_disc ( &disc );
    return 0;
}


/** @brief Shrink CP/M disku zachová detekci. */
static int test_detect_cpm_after_shrink ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_like_disc ( &disc, 80, 2, 9, DSK_SECTOR_SIZE_512 ) );

    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, MZDSK_CPM_FORMAT_SD );
    TEST_ASSERT ( mzdsk_cpm_format_directory ( &disc, &dpb ) == MZDSK_RES_OK, "format" );

    /* Shrink ze 160 na 100 absolutních stop */
    TEST_ASSERT_OK ( dsk_tools_shrink_image ( disc.handler, NULL, 100 ) );
    TEST_ASSERT_OK ( reload_rules ( &disc ) );
    TEST_ASSERT_EQ_INT ( disc.tracks_rules->total_tracks, 100 );

    /* Stále CP/M */
    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_CPM );

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  Testy: cross-FS reformat regresse (P2.3)
 *
 *  CP/M a MRS sdílí stejnou geometrii (9x512B data + boot track 16x256B),
 *  proto přechod mezi nimi je skutečný "reformat" bez změny DSK kontejneru.
 *  Testy ověřují, že detekce po přeformátování nevrátí starý FS kvůli
 *  reziduálním datům (např. FAT bajty na bloku 36 u MRS vs. CP/M dir).
 * ==================================================================== */


/** @brief MRS disk po přeformátování na CP/M → detekce vrátí CP/M, ne MRS. */
static int test_detect_after_mrs_to_cpm_format ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_like_disc ( &disc, 80, 2, 9, DSK_SECTOR_SIZE_512 ) );

    /* Inicializovat jako MRS */
    TEST_ASSERT ( fsmrs_format_fs ( &disc, MZDSK_DETECT_MRS_DEFAULT_FAT_BLOCK ) == MZDSK_RES_OK,
                  "mrs init" );

    /* Ověřit MRS detekci */
    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect mrs" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_MRS );

    /* Reformát na CP/M (přepíše adresář a implicitně MRS FAT) */
    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, MZDSK_CPM_FORMAT_SD );
    TEST_ASSERT ( mzdsk_cpm_format_directory ( &disc, &dpb ) == MZDSK_RES_OK,
                  "cpm format" );

    /* Detekce po reformátu musí vrátit CP/M, NE MRS.
     * MRS probe bude hledat FAT marker na bloku 36, ale CP/M format
     * změní obsah bloku 36 tak, aby signatura 0xFA (po inverzi) neseděla.
     */
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect cpm" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_CPM );

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  Testy: tolerance částečně "divokých" adresářových položek
 *
 *  CP/M 2.2 BDOS (a kompatibilní, např. nipos BDOS - viz nipbdos1.s)
 *  považuje za volný slot výhradně user == 0xE5. Cokoli jiného je pro
 *  BDOS alokovaná entry; pokud user > 15, BDOS ji nezahrne do výpisu,
 *  ale počítá ji mezi obsazené. Detekce CP/M musí takové disky poznat.
 * ==================================================================== */


/**
 * @brief Disk s ne-displayed user bajty v prvních entries se detekuje jako CP/M.
 *
 * Reprodukuje refdata/dsk/Chaky/DISK05_Artex.DSK: validní CP/M SD
 * geometrie, ale prvních několik directory entries má user bajt mimo
 * <0,15> a != 0xE5 (např. 0x5B). nipos BDOS to akceptuje. Detekce to
 * dnes také musí přijmout jako CP/M.
 */
static int test_detect_cpm_with_nondisplayed_user ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_like_disc ( &disc, 80, 2, 9, DSK_SECTOR_SIZE_512 ) );

    /* Inicializovat jako prázdný CP/M SD */
    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, MZDSK_CPM_FORMAT_SD );
    TEST_ASSERT ( mzdsk_cpm_format_directory ( &disc, &dpb ) == MZDSK_RES_OK,
                  "cpm format" );

    /* Ověřit CP/M detekci na čistém disku */
    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect clean" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_CPM );

    /* Zapsat 3 entries s user = 0x5B (out-of-range pro CP/M 2.2 user areas).
     * Zbytek ponecháme prázdný (0xE5). */
    st_MZDSK_CPM_DIRENTRY entry;
    memset ( &entry, 0xAA, sizeof ( entry ) );
    entry.user = 0x5B;
    entry.rc = 0x40;
    entry.extent = 0x00;
    entry.s1 = 0x00;
    entry.s2 = 0x00;

    for ( int i = 0; i < 3; i++ ) {
        TEST_ASSERT ( mzdsk_cpm_write_dir_entry ( &disc, &dpb, i, &entry ) == MZDSK_RES_OK,
                      "write garbage entry" );
    }

    /* Detekce musí stále vrátit CP/M. */
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect with garbage" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_CPM );

    close_disc ( &disc );
    return 0;
}


/** @brief CP/M disk po přeformátování na MRS → detekce vrátí MRS, ne CP/M. */
static int test_detect_after_cpm_to_mrs_format ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_cpm_like_disc ( &disc, 80, 2, 9, DSK_SECTOR_SIZE_512 ) );

    /* Inicializovat jako CP/M */
    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, MZDSK_CPM_FORMAT_SD );
    TEST_ASSERT ( mzdsk_cpm_format_directory ( &disc, &dpb ) == MZDSK_RES_OK,
                  "cpm format" );

    /* Ověřit CP/M detekci */
    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect cpm" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_CPM );

    /* Reformát na MRS */
    TEST_ASSERT ( fsmrs_format_fs ( &disc, MZDSK_DETECT_MRS_DEFAULT_FAT_BLOCK ) == MZDSK_RES_OK,
                  "mrs init" );

    /* Detekce po reformátu musí vrátit MRS.
     * MRS probe má přednost před CP/M probe (viz mzdsk_detect pořadí kroků:
     * FSMZ → MRS → CP/M), takže MRS FAT marker přepsaný v bloku 36 jasně
     * vítězí, i kdyby CPM dir zbytky zůstávaly.
     */
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect mrs" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_MRS );

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  main
 * ==================================================================== */


int main ( void )
{
    memory_driver_init ();

    TEST_INIT ();

    /* detekce na čerstvých discích */
    RUN_TEST ( test_detect_fsmz_formatted );
    RUN_TEST ( test_detect_fsmz_unformatted );
    RUN_TEST ( test_detect_cpm_sd_formatted );
    RUN_TEST ( test_detect_mrs_initialized );
    RUN_TEST ( test_detect_unknown_empty );

    /* stabilita po geometrických změnách */
    RUN_TEST ( test_detect_cpm_after_append );
    RUN_TEST ( test_detect_fsmz_after_append );
    RUN_TEST ( test_detect_cpm_after_shrink );

    /* cross-FS reformat regresse (P2.3) */
    RUN_TEST ( test_detect_after_mrs_to_cpm_format );
    RUN_TEST ( test_detect_after_cpm_to_mrs_format );

    /* tolerance ne-displayed user entries (DISK05_Artex) */
    RUN_TEST ( test_detect_cpm_with_nondisplayed_user );

    TEST_SUMMARY ();
}
