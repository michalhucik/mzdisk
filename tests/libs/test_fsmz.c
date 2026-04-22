/**
 * @file test_fsmz.c
 * @brief Testy FSMZ filesystem operací.
 *
 * Testuje formátování, repair, defrag a edge cases.
 * FSMZ má složité nízkoúrovňové API (blokové čtení adresáře),
 * proto testujeme hlavně tool-level funkce.
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
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"


/* ====================================================================
 *  Pomocné funkce
 * ==================================================================== */

static int create_formatted_fsmz ( st_MZDSK_DISC *disc, int tracks )
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
    desc->tracks = (uint8_t) tracks;
    desc->sides = 1;
    dsk_tools_assign_description ( desc, 0, 0, 16, DSK_SECTOR_SIZE_256,
                                    DSK_SEC_ORDER_NORMAL, NULL, 0x00 );

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

    en_MZDSK_RES mres = fsmz_tool_fast_format ( disc );
    if ( mres != MZDSK_RES_OK ) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}


static void close_disc ( st_MZDSK_DISC *disc )
{
    if ( disc->cache ) { free ( disc->cache ); disc->cache = NULL; }
    if ( disc->tracks_rules ) {
        dsk_tools_destroy_track_rules ( disc->tracks_rules );
        disc->tracks_rules = NULL;
    }
    if ( disc->handler ) {
        if ( disc->handler->spec.memspec.ptr )
            free ( disc->handler->spec.memspec.ptr );
        free ( disc->handler );
        disc->handler = NULL;
    }
}


/**
 * @brief Zapíše soubor na FSMZ disk.
 *
 * Používá správný FSMZ API pattern: fsmz_open_dir + fsmz_read_dir
 * pro nalezení volného slotu + fsmz_write_block pro data.
 */
static en_MZDSK_RES put_raw_fsmz_file ( st_MZDSK_DISC *disc, const char *name,
                                           uint16_t data_size, uint8_t fill )
{
    uint16_t num_blocks = ( data_size + FSMZ_SECTOR_SIZE - 1 ) / FSMZ_SECTOR_SIZE;
    if ( num_blocks == 0 ) num_blocks = 1;

    /* Najít volné bloky */
    uint16_t start_block;
    en_MZDSK_RES res = fsmz_check_free_blocks ( disc, num_blocks, &start_block );
    if ( res != MZDSK_RES_OK ) return res;

    /* Zapsat data */
    uint8_t block_data[FSMZ_SECTOR_SIZE];
    memset ( block_data, fill, FSMZ_SECTOR_SIZE );
    for ( uint16_t b = 0; b < num_blocks; b++ ) {
        res = fsmz_write_block ( disc, start_block + b, block_data );
        if ( res != MZDSK_RES_OK ) return res;
    }

    /* Najít volný slot přes fsmz_open_dir + fsmz_read_dir */
    st_FSMZ_DIR dir;
    res = fsmz_open_dir ( disc, &dir );
    if ( res != MZDSK_RES_OK ) return res;

    en_MZDSK_RES err = MZDSK_RES_OK;
    st_FSMZ_DIR_ITEM *diritem;
    while ( 1 ) {
        diritem = fsmz_read_dir ( disc, &dir, FSMZ_MAX_DIR_ITEMS, &err );
        if ( err != MZDSK_RES_OK ) break;
        if ( diritem->ftype == 0x00 ) break; /* volný slot */
    }

    if ( err == MZDSK_RES_FILE_NOT_FOUND ) return MZDSK_RES_DIR_FULL;
    if ( err != MZDSK_RES_OK ) return err;

    /* Naplnit dir entry */
    diritem->ftype = 0x01;
    memset ( diritem->fname, 0x0D, FSMZ_FNAME_LENGTH );
    size_t nlen = strlen ( name );
    if ( nlen > 16 ) nlen = 16;
    memcpy ( diritem->fname, name, nlen );
    diritem->fname[nlen] = 0x0D;
    diritem->fsize = data_size;
    diritem->fstrt = 0x1200;
    diritem->fexec = 0x1200;
    diritem->block = start_block;

    fsmz_write_dirblock ( disc, &dir, FSMZ_MAX_DIR_ITEMS );
    fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_SET,
                                      start_block, num_blocks );
    return MZDSK_RES_OK;
}


/** @brief Spočte počet souborů v FSMZ adresáři. */
static int count_fsmz_files ( st_MZDSK_DISC *disc )
{
    st_FSMZ_DIR dir;
    en_MZDSK_RES res = fsmz_open_dir ( disc, &dir );
    if ( res != MZDSK_RES_OK ) return -1;

    int count = 0;
    en_MZDSK_RES err = MZDSK_RES_OK;
    while ( 1 ) {
        st_FSMZ_DIR_ITEM *item = fsmz_read_dir ( disc, &dir,
                                                    FSMZ_MAX_DIR_ITEMS, &err );
        if ( err != MZDSK_RES_OK ) break;
        if ( item->ftype != 0x00 ) count++;
    }
    return count;
}


/* ====================================================================
 *  Testy: základní operace
 * ==================================================================== */


/** @brief Naformátovaný FSMZ disk má prázdný adresář. */
static int test_fsmz_format_empty ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );
    TEST_ASSERT_EQ_INT ( count_fsmz_files ( &disc ), 0 );

    st_FSMZ_DINFO_BLOCK dinfo;
    TEST_ASSERT ( fsmz_read_dinfo ( &disc, &dinfo ) == MZDSK_RES_OK, "read dinfo" );
    TEST_ASSERT ( dinfo.blocks > 0, "blocks > 0" );

    close_disc ( &disc );
    return 0;
}


/** @brief Put soubor a najít v adresáři. */
static int test_fsmz_put_file ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "HELLO", 256, 0xAA ) == MZDSK_RES_OK, "put" );
    TEST_ASSERT_EQ_INT ( count_fsmz_files ( &disc ), 1 );

    close_disc ( &disc );
    return 0;
}


/** @brief Put + read roundtrip. */
static int test_fsmz_roundtrip ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "ROUND", 512, 0xBB ) == MZDSK_RES_OK, "put" );

    /* Najít v adresáři */
    st_FSMZ_DIR dir;
    fsmz_open_dir ( &disc, &dir );
    en_MZDSK_RES err;
    st_FSMZ_DIR_ITEM *item = fsmz_read_dir ( &disc, &dir, FSMZ_MAX_DIR_ITEMS, &err );
    TEST_ASSERT ( err == MZDSK_RES_OK, "read dir" );
    TEST_ASSERT ( item->ftype != 0x00, "file exists" );

    /* Přečíst data bloků */
    uint8_t buf[256];
    fsmz_read_block ( &disc, item->block, buf );
    TEST_ASSERT_BYTE ( buf, 0, 0xBB );
    TEST_ASSERT_BYTE ( buf, 255, 0xBB );

    fsmz_read_block ( &disc, item->block + 1, buf );
    TEST_ASSERT_BYTE ( buf, 0, 0xBB );

    close_disc ( &disc );
    return 0;
}


/** @brief Defrag zachová soubory. */
static int test_fsmz_defrag ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "AAA", 256, 0x11 ) == MZDSK_RES_OK, "A" );
    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "BBB", 256, 0x22 ) == MZDSK_RES_OK, "B" );
    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "CCC", 256, 0x33 ) == MZDSK_RES_OK, "C" );
    TEST_ASSERT_EQ_INT ( count_fsmz_files ( &disc ), 3 );

    /* Smazat prostřední (BBB) přes fsmz_save_diritem_to_position */
    {
        st_FSMZ_DIR dir;
        fsmz_open_dir ( &disc, &dir );
        en_MZDSK_RES err;
        /* Najít BBB (druhý soubor) */
        fsmz_read_dir ( &disc, &dir, FSMZ_MAX_DIR_ITEMS, &err ); /* AAA */
        st_FSMZ_DIR_ITEM *bbb = fsmz_read_dir ( &disc, &dir, FSMZ_MAX_DIR_ITEMS, &err ); /* BBB */
        TEST_ASSERT ( err == MZDSK_RES_OK, "find BBB" );
        uint16_t block = bbb->block;
        bbb->ftype = 0x00;
        fsmz_write_dirblock ( &disc, &dir, FSMZ_MAX_DIR_ITEMS );
        fsmz_update_dinfo_farea_bitmap ( &disc, FSMZ_DINFO_BITMAP_RESET, block, 1 );
    }
    TEST_ASSERT_EQ_INT ( count_fsmz_files ( &disc ), 2 );

    TEST_ASSERT ( fsmz_tool_defrag ( &disc, FSMZ_MAX_DIR_ITEMS, NULL, NULL ) == MZDSK_RES_OK,
                  "defrag" );
    TEST_ASSERT_EQ_INT ( count_fsmz_files ( &disc ), 2 );

    /* Data prvního souboru zachována */
    st_FSMZ_DIR dir;
    fsmz_open_dir ( &disc, &dir );
    en_MZDSK_RES err;
    st_FSMZ_DIR_ITEM *first = fsmz_read_dir ( &disc, &dir, FSMZ_MAX_DIR_ITEMS, &err );
    TEST_ASSERT ( err == MZDSK_RES_OK && first->ftype != 0x00, "first file after defrag" );
    uint8_t buf[256];
    fsmz_read_block ( &disc, first->block, buf );
    TEST_ASSERT_BYTE ( buf, 0, 0x11 );

    close_disc ( &disc );
    return 0;
}


/**
 * @brief Audit H-5: defrag musí vrátit INVALID_PARAM u non-memory handleru.
 */
static int test_fsmz_defrag_rejects_file_handler ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    en_HANDLER_TYPE orig_type = disc.handler->type;
    disc.handler->type = HANDLER_TYPE_FILE;

    en_MZDSK_RES res = fsmz_tool_defrag ( &disc, FSMZ_MAX_DIR_ITEMS, NULL, NULL );
    TEST_ASSERT ( res == MZDSK_RES_INVALID_PARAM,
                  "defrag must reject non-memory handler" );

    disc.handler->type = orig_type;

    close_disc ( &disc );
    return 0;
}


/** @brief Repair opraví poškozenou bitmapu. */
static int test_fsmz_repair ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "FILE1", 256, 0xAA ) == MZDSK_RES_OK, "put" );

    /* Poškodit DINFO - vynulovat bitmapu */
    st_FSMZ_DINFO_BLOCK dinfo;
    fsmz_read_dinfo ( &disc, &dinfo );
    memset ( dinfo.map, 0, sizeof ( dinfo.map ) );
    dinfo.used = 0;
    fsmz_write_dinfo ( &disc, &dinfo );

    /* Repair */
    TEST_ASSERT ( fsmz_tool_repair_dinfo ( &disc, FSMZ_MAX_DIR_ITEMS ) == MZDSK_RES_OK,
                  "repair" );

    fsmz_read_dinfo ( &disc, &dinfo );
    TEST_ASSERT ( dinfo.used > 0, "used blocks restored" );

    close_disc ( &disc );
    return 0;
}


/**
 * @brief Repair opraví i poškozená pole farea a blocks (BUG 11).
 *
 * Simuluje stav, kdy DINFO byl přepsán náhodnými daty (např. chybou
 * uživatele nebo poškozením sektoru). Repair musí obnovit pole @c farea
 * a @c blocks na hodnoty odvozené z geometrie disku, ne je ponechat.
 */
static int test_fsmz_repair_dinfo_geometry ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "FILE1", 256, 0xAA ) == MZDSK_RES_OK, "put" );

    /* Zapamatovat si správné hodnoty pro ověření po repair */
    st_FSMZ_DINFO_BLOCK dinfo_good;
    fsmz_read_dinfo ( &disc, &dinfo_good );

    /* Poškodit DINFO - celý blok náhodnými daty */
    st_FSMZ_DINFO_BLOCK dinfo_bad;
    memset ( &dinfo_bad, 0x5A, sizeof ( dinfo_bad ) );
    dinfo_bad.blocks = 0x1DE5;       /* 7653 - stejně jako v bug reportu */
    dinfo_bad.farea = 0xAA;
    dinfo_bad.used = 0xFFFF;
    fsmz_write_dinfo ( &disc, &dinfo_bad );

    /* Repair */
    TEST_ASSERT ( fsmz_tool_repair_dinfo ( &disc, FSMZ_MAX_DIR_ITEMS ) == MZDSK_RES_OK,
                  "repair" );

    st_FSMZ_DINFO_BLOCK dinfo;
    fsmz_read_dinfo ( &disc, &dinfo );
    TEST_ASSERT_EQ_INT ( dinfo.blocks, dinfo_good.blocks );
    TEST_ASSERT_EQ_INT ( dinfo.farea, dinfo_good.farea );
    TEST_ASSERT ( dinfo.used > 0 && dinfo.used < dinfo.blocks, "used in range" );

    close_disc ( &disc );
    return 0;
}


/**
 * @brief P2.6: Repair DINFO kde blocks=0 a farea=0 (úplné vynulování polí).
 *
 * Specifický edge: `dinfo.blocks = 0` by vedlo k nedefinovanému dělení
 * v mnoha operacích, proto repair musí toto pole jednoznačně přepsat
 * hodnotou odvozenou z geometrie, ne nechat na nule.
 */
static int test_fsmz_repair_dinfo_zero_blocks ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    /* Ulož si správné hodnoty pro porovnání */
    st_FSMZ_DINFO_BLOCK dinfo_good;
    fsmz_read_dinfo ( &disc, &dinfo_good );

    /* Poškodit: jen blocks a farea na nulu, zbytek nechat */
    st_FSMZ_DINFO_BLOCK dinfo_bad;
    fsmz_read_dinfo ( &disc, &dinfo_bad );
    dinfo_bad.blocks = 0;
    dinfo_bad.farea = 0;
    fsmz_write_dinfo ( &disc, &dinfo_bad );

    /* Repair */
    TEST_ASSERT ( fsmz_tool_repair_dinfo ( &disc, FSMZ_MAX_DIR_ITEMS ) == MZDSK_RES_OK,
                  "repair" );

    st_FSMZ_DINFO_BLOCK dinfo;
    fsmz_read_dinfo ( &disc, &dinfo );
    TEST_ASSERT_EQ_INT ( dinfo.blocks, dinfo_good.blocks );
    TEST_ASSERT_EQ_INT ( dinfo.farea, dinfo_good.farea );
    TEST_ASSERT ( dinfo.blocks > 0, "blocks not zero" );

    close_disc ( &disc );
    return 0;
}


/**
 * @brief P2.6: Repair DINFO, kde blocks je nesmyslně velké (přesah geometrie).
 *
 * Pokud blocks > fyzický počet bloků, repair musí hodnotu umenšit na
 * skutečnou kapacitu.
 */
static int test_fsmz_repair_dinfo_oversize_blocks ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    st_FSMZ_DINFO_BLOCK dinfo_good;
    fsmz_read_dinfo ( &disc, &dinfo_good );

    /* Nastavit blocks na nesmyslně velkou hodnotu */
    st_FSMZ_DINFO_BLOCK dinfo_bad;
    fsmz_read_dinfo ( &disc, &dinfo_bad );
    dinfo_bad.blocks = 0xFFFF;
    dinfo_bad.farea = 0xFE;
    fsmz_write_dinfo ( &disc, &dinfo_bad );

    TEST_ASSERT ( fsmz_tool_repair_dinfo ( &disc, FSMZ_MAX_DIR_ITEMS ) == MZDSK_RES_OK,
                  "repair" );

    st_FSMZ_DINFO_BLOCK dinfo;
    fsmz_read_dinfo ( &disc, &dinfo );
    TEST_ASSERT_EQ_INT ( dinfo.blocks, dinfo_good.blocks );
    TEST_ASSERT_EQ_INT ( dinfo.farea, dinfo_good.farea );

    close_disc ( &disc );
    return 0;
}


/* ====================================================================
 *  Edge cases
 * ==================================================================== */


/** @brief Soubor o velikosti 1 bajt. */
static int test_fsmz_one_byte_file ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "TINY", 1, 0xFF ) == MZDSK_RES_OK, "put" );
    TEST_ASSERT_EQ_INT ( count_fsmz_files ( &disc ), 1 );

    close_disc ( &disc );
    return 0;
}


/** @brief Soubory přesně na hranicích bloků. */
static int test_fsmz_block_boundaries ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    /* 256B = 1 blok, 512B = 2 bloky, 257B = 2 bloky */
    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "B256", 256, 0xAA ) == MZDSK_RES_OK, "256B" );
    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "B512", 512, 0xBB ) == MZDSK_RES_OK, "512B" );
    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "B257", 257, 0xCC ) == MZDSK_RES_OK, "257B" );
    TEST_ASSERT_EQ_INT ( count_fsmz_files ( &disc ), 3 );

    close_disc ( &disc );
    return 0;
}


/** @brief Plný disk - malý disk, mnoho souborů. */
static int test_fsmz_disk_full ( void )
{
    /* 10 stop = 160 bloků, minus ~48 systémových = ~112 datových */
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 10 ) );

    char name[17];
    int i;
    en_MZDSK_RES res;
    for ( i = 0; i < 130; i++ ) {
        snprintf ( name, sizeof ( name ), "F%03d", i );
        res = put_raw_fsmz_file ( &disc, name, 256, (uint8_t) i );
        if ( res != MZDSK_RES_OK ) break;
    }
    TEST_ASSERT ( i > 0, "at least one file" );
    TEST_ASSERT ( i < 130, "must fail (disk or dir full)" );

    close_disc ( &disc );
    return 0;
}


/** @brief Put na read-only disk selže. */
static int test_fsmz_readonly_fails ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    generic_driver_set_handler_readonly_status ( disc.handler, 1 );

    en_MZDSK_RES res = put_raw_fsmz_file ( &disc, "RO", 256, 0x00 );
    TEST_ASSERT ( res != MZDSK_RES_OK, "put on RO must fail" );

    close_disc ( &disc );
    return 0;
}


/** @brief Format file area zachová bootstrap (IPLPRO). */
static int test_fsmz_format_preserves_bootstrap ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    /* Zapamatovat IPLPRO */
    st_FSMZ_IPLPRO_BLOCK iplpro_before;
    fsmz_read_iplpro ( &disc, &iplpro_before );

    TEST_ASSERT ( put_raw_fsmz_file ( &disc, "TEMP", 256, 0xAA ) == MZDSK_RES_OK, "put" );

    TEST_ASSERT ( fsmz_tool_format_file_area ( &disc ) == MZDSK_RES_OK, "format" );
    TEST_ASSERT_EQ_INT ( count_fsmz_files ( &disc ), 0 );

    /* IPLPRO zachován */
    st_FSMZ_IPLPRO_BLOCK iplpro_after;
    fsmz_read_iplpro ( &disc, &iplpro_after );
    TEST_ASSERT_EQ_MEM ( &iplpro_before, &iplpro_after, sizeof ( st_FSMZ_IPLPRO_BLOCK ) );

    close_disc ( &disc );
    return 0;
}


/** @brief FSMZ detekce. */
static int test_fsmz_detect ( void )
{
    st_MZDSK_DISC disc;
    TEST_ASSERT_OK ( create_formatted_fsmz ( &disc, 80 ) );

    st_MZDSK_DETECT_RESULT result;
    TEST_ASSERT ( mzdsk_detect_filesystem ( &disc, &result ) == MZDSK_RES_OK, "detect" );
    TEST_ASSERT_EQ_INT ( result.type, MZDSK_FS_FSMZ );

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

    /* základní operace */
    RUN_TEST ( test_fsmz_format_empty );
    RUN_TEST ( test_fsmz_put_file );
    RUN_TEST ( test_fsmz_roundtrip );
    RUN_TEST ( test_fsmz_defrag );
    RUN_TEST ( test_fsmz_defrag_rejects_file_handler );
    RUN_TEST ( test_fsmz_repair );
    RUN_TEST ( test_fsmz_repair_dinfo_geometry );
    RUN_TEST ( test_fsmz_repair_dinfo_zero_blocks );
    RUN_TEST ( test_fsmz_repair_dinfo_oversize_blocks );
    RUN_TEST ( test_fsmz_detect );

    /* edge cases */
    RUN_TEST ( test_fsmz_one_byte_file );
    RUN_TEST ( test_fsmz_block_boundaries );
    RUN_TEST ( test_fsmz_disk_full );
    RUN_TEST ( test_fsmz_readonly_fails );
    RUN_TEST ( test_fsmz_format_preserves_bootstrap );

    TEST_SUMMARY ();
}
