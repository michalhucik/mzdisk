/**
 * @file test_mzdsk_global.c
 * @brief Testy centrální diskové abstrakce (mzdsk_global).
 *
 * Testuje otevírání/zavírání disků do paměti, sektorové I/O
 * s auto-inverzí, pomocné funkce (invert, strncpy, strcmp)
 * a chybové hlášky.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "test_framework.h"

#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/* ===================================================================
 *  Testy pomocných funkcí
 * =================================================================== */


/** @brief XOR 0xFF inverze bloku dat. */
static int test_invert_data ( void )
{
    uint8_t data[] = { 0x00, 0xFF, 0x55, 0xAA };
    mzdsk_invert_data ( data, 4 );

    TEST_ASSERT_EQ_INT ( data[0], 0xFF );
    TEST_ASSERT_EQ_INT ( data[1], 0x00 );
    TEST_ASSERT_EQ_INT ( data[2], 0xAA );
    TEST_ASSERT_EQ_INT ( data[3], 0x55 );

    /* dvojitá inverze = identita */
    mzdsk_invert_data ( data, 4 );
    TEST_ASSERT_EQ_INT ( data[0], 0x00 );
    TEST_ASSERT_EQ_INT ( data[1], 0xFF );
    return 0;
}


/** @brief mzdsk_strncpy s paddingem na fixní délku. */
static int test_strncpy_padding ( void )
{
    uint8_t dst[10];
    memset ( dst, 0xAA, sizeof ( dst ) );

    mzdsk_strncpy ( dst, (const uint8_t *) "ABC", 8, 0x0D );

    TEST_ASSERT_EQ_INT ( dst[0], 'A' );
    TEST_ASSERT_EQ_INT ( dst[1], 'B' );
    TEST_ASSERT_EQ_INT ( dst[2], 'C' );
    /* ověřit, že zdroj byl zkopírován a zbytek vyplněn */
    TEST_ASSERT ( dst[3] != 0xAA, "padding applied after string" );
    return 0;
}


/** @brief Porovnání MZ formátových řetězců. */
static int test_mzstrcmp ( void )
{
    uint8_t a[] = { 'T', 'E', 'S', 'T', 0x0D, 0x0D };
    uint8_t b[] = { 'T', 'E', 'S', 'T', 0x0D, 0x0D };
    uint8_t c[] = { 'O', 'T', 'H', 'E', 'R', 0x0D };

    TEST_ASSERT_EQ_INT ( mzdsk_mzstrcmp ( a, b ), 0 );
    TEST_ASSERT ( mzdsk_mzstrcmp ( a, c ) != 0, "different strings" );
    return 0;
}


/** @brief mzdsk_get_error pro všechny en_MZDSK_RES kódy. */
static int test_error_messages ( void )
{
    /* OK */
    const char *msg = mzdsk_get_error ( MZDSK_RES_OK );
    TEST_ASSERT_NOT_NULL ( msg );
    TEST_ASSERT ( strlen ( msg ) > 0, "OK message non-empty" );

    /* chybové kódy - všechny musí vrátit neprázdný řetězec */
    en_MZDSK_RES codes[] = {
        MZDSK_RES_DSK_ERROR, MZDSK_RES_NO_SPACE,
        MZDSK_RES_WRITE_PROTECTED, MZDSK_RES_FILE_NOT_FOUND
    };
    for ( int i = 0; i < 4; i++ ) {
        msg = mzdsk_get_error ( codes[i] );
        TEST_ASSERT_NOT_NULL ( msg );
        TEST_ASSERT ( strlen ( msg ) > 0, "error message non-empty" );
    }
    return 0;
}


/* ===================================================================
 *  Testy diskových operací
 * =================================================================== */


/** @brief Otevření DSK do paměti a zavření. */
static int test_disc_open_close ( void )
{
    /* nejdřív vytvořit DSK soubor */
    char tmpdir[] = "/tmp/mzdsk_test_global_XXXXXX";
    if ( mkdtemp ( tmpdir ) == NULL ) TEST_FAIL ( "mkdtemp" );

    char filepath[512];
    snprintf ( filepath, sizeof ( filepath ), "%s/test.dsk", tmpdir );

    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;

    int ret = mzdsk_tools_format_basic ( &h, 40, 1 );
    if ( ret != EXIT_SUCCESS ) {
        free ( h.spec.memspec.ptr );
        rmdir ( tmpdir );
        TEST_FAIL ( "format_basic" );
    }

    /* uložit do souboru */
    st_HANDLER fh;
    st_DRIVER fd;
    generic_driver_file_init ( &fd );
    generic_driver_open_file ( &fh, &fd, filepath, FILE_DRIVER_OPMODE_W );
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );

    /* otevřít přes mzdsk_disc_open_memory */
    st_MZDSK_DISC disc;
    en_MZDSK_RES res = mzdsk_disc_open_memory ( &disc, filepath, FILE_DRIVER_OPMODE_RO );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    TEST_ASSERT_NOT_NULL ( disc.handler );
    TEST_ASSERT_NOT_NULL ( disc.tracks_rules );

    mzdsk_disc_close ( &disc );

    remove ( filepath );
    rmdir ( tmpdir );
    return 0;
}


/**
 * @brief Audit M-5: mzdsk_disc_close je bezpečný i pro nekompletně
 * inicializovanou strukturu (partial cleanup invariant).
 */
static int test_disc_close_partial_init ( void )
{
    /* Nulovaná struktura - vše NULL */
    st_MZDSK_DISC disc_zero;
    memset ( &disc_zero, 0, sizeof ( disc_zero ) );
    mzdsk_disc_close ( &disc_zero ); /* nesmí crashnout */

    /* Struktura s alokovaným handlerem + driverem, ale bez tracks_rules
       a bez cache - simulace failure uprostřed mzdsk_disc_open */
    st_MZDSK_DISC disc_partial;
    memset ( &disc_partial, 0, sizeof ( disc_partial ) );
    disc_partial.handler = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    TEST_ASSERT_NOT_NULL ( disc_partial.handler );
    st_DRIVER *drv = (st_DRIVER *) calloc ( 1, sizeof ( st_DRIVER ) );
    TEST_ASSERT_NOT_NULL ( drv );
    generic_driver_file_init ( drv );
    disc_partial.handler->driver = drv;
    /* tracks_rules, cache, format, sector_info_cb zůstávají nulové */

    mzdsk_disc_close ( &disc_partial ); /* nesmí crashnout, nesmí leaknout */

    /* Volat znovu na již vynulovanou strukturu - NOP */
    mzdsk_disc_close ( &disc_partial );

    /* NULL parametr - NOP */
    mzdsk_disc_close ( NULL );

    return 0;
}


/** @brief Save -> reopen -> data zachována. */
static int test_disc_save_reopen ( void )
{
    char tmpdir[] = "/tmp/mzdsk_test_save_XXXXXX";
    if ( mkdtemp ( tmpdir ) == NULL ) TEST_FAIL ( "mkdtemp" );

    char filepath[512];
    snprintf ( filepath, sizeof ( filepath ), "%s/save.dsk", tmpdir );

    /* vytvořit FSMZ disk */
    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    mzdsk_tools_format_basic ( &h, 40, 1 );

    st_HANDLER fh;
    st_DRIVER fd;
    generic_driver_file_init ( &fd );
    generic_driver_open_file ( &fh, &fd, filepath, FILE_DRIVER_OPMODE_W );
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );

    /* otevřít, zapsat marker, uložit */
    st_MZDSK_DISC disc;
    en_MZDSK_RES res = mzdsk_disc_open_memory ( &disc, filepath, FILE_DRIVER_OPMODE_RW );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );

    uint16_t total = disc.tracks_rules->total_tracks;
    TEST_ASSERT ( total > 0, "has tracks" );

    res = mzdsk_disc_save ( &disc );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    mzdsk_disc_close ( &disc );

    /* znovu otevřít a ověřit */
    st_MZDSK_DISC disc2;
    res = mzdsk_disc_open_memory ( &disc2, filepath, FILE_DRIVER_OPMODE_RO );
    TEST_ASSERT_EQ_INT ( res, MZDSK_RES_OK );
    TEST_ASSERT_EQ_INT ( disc2.tracks_rules->total_tracks, total );

    mzdsk_disc_close ( &disc2 );
    remove ( filepath );
    rmdir ( tmpdir );
    return 0;
}


/** @brief Default sector_info_cb detekuje invertované FSMZ sektory. */
static int test_sector_info_cb ( void )
{
    /* FSMZ stopa 0 (16x256B) by měla být INVERTED */
    /* Vytvořit minimální FSMZ disc pro test */
    char tmpdir[] = "/tmp/mzdsk_test_sicb_XXXXXX";
    if ( mkdtemp ( tmpdir ) == NULL ) TEST_FAIL ( "mkdtemp" );

    char filepath[512];
    snprintf ( filepath, sizeof ( filepath ), "%s/fsmz.dsk", tmpdir );

    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = &g_memory_driver_realloc;
    h.spec.memspec.ptr = (uint8_t *) calloc ( 1, 1 );
    h.spec.memspec.size = 1;
    h.type = HANDLER_TYPE_MEMORY;
    h.status = HANDLER_STATUS_READY;
    mzdsk_tools_format_basic ( &h, 40, 1 );

    st_HANDLER fh;
    st_DRIVER fd;
    generic_driver_file_init ( &fd );
    generic_driver_open_file ( &fh, &fd, filepath, FILE_DRIVER_OPMODE_W );
    generic_driver_write ( &fh, 0, h.spec.memspec.ptr, h.spec.memspec.size );
    generic_driver_close ( &fh );
    free ( h.spec.memspec.ptr );

    st_MZDSK_DISC disc;
    mzdsk_disc_open_memory ( &disc, filepath, FILE_DRIVER_OPMODE_RO );
    disc.sector_info_cb = mzdsk_sector_info_cb;
    disc.sector_info_cb_data = &disc;

    /* stopa 0 na FSMZ disku (16x256B) -> INVERTED | sector_size */
    uint8_t medium = mzdsk_sector_info_cb ( 0, 1, &disc );
    TEST_ASSERT ( ( medium & MZDSK_MEDIUM_INVERTED ) != 0,
                  "FSMZ track should be INVERTED" );

    mzdsk_disc_close ( &disc );
    remove ( filepath );
    rmdir ( tmpdir );
    return 0;
}


/* ===================================================================
 *  Testy striktní validace 8.3 jména
 * =================================================================== */


/** @brief Validní 8.3 jméno "HELLO.TXT" projde bez chyby. */
static int test_nameval_cpm_ok ( void )
{
    char name[9], ext[4];
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "HELLO.TXT",
        MZDSK_NAMEVAL_FLAVOR_CPM, name, ext, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_OK );
    TEST_ASSERT_EQ_STR ( name, "HELLO" );
    TEST_ASSERT_EQ_STR ( ext, "TXT" );
    return 0;
}


/** @brief Hraniční případ - přesně 8 znaků name + 3 ext. */
static int test_nameval_exactly_8_3 ( void )
{
    char name[9], ext[4];
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "ABCDEFGH.XYZ",
        MZDSK_NAMEVAL_FLAVOR_CPM, name, ext, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_OK );
    TEST_ASSERT_EQ_STR ( name, "ABCDEFGH" );
    TEST_ASSERT_EQ_STR ( ext, "XYZ" );
    return 0;
}


/** @brief 9. znak jména musí vyvolat NAME_TOO_LONG. */
static int test_nameval_name_too_long ( void )
{
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "ABCDEFGHI.XYZ",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_NAME_TOO_LONG );
    return 0;
}


/** @brief 4. znak přípony musí vyvolat EXT_TOO_LONG. */
static int test_nameval_ext_too_long ( void )
{
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "ABC.XYZW",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_EXT_TOO_LONG );
    return 0;
}


/** @brief Prázdné jméno před tečkou vyvolá EMPTY. */
static int test_nameval_empty_name_before_dot ( void )
{
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( ".TXT",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_EMPTY );
    return 0;
}


/** @brief Zcela prázdný vstup vyvolá EMPTY. */
static int test_nameval_empty_input ( void )
{
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_EMPTY );
    return 0;
}


/** @brief '*' uprostřed jména vyvolá BAD_CHAR s nahlášeným '*'. */
static int test_nameval_bad_char_asterisk ( void )
{
    char bad = 0;
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "A*B.TXT",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, &bad );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_BAD_CHAR );
    TEST_ASSERT_EQ_INT ( bad, '*' );
    return 0;
}


/** @brief Mezera uvnitř přípony vyvolá BAD_CHAR. */
static int test_nameval_bad_char_space ( void )
{
    char bad = 0;
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "ABC.T X",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, &bad );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_BAD_CHAR );
    TEST_ASSERT_EQ_INT ( bad, ' ' );
    return 0;
}


/** @brief Jméno bez přípony je validní, ext je prázdný řetězec. */
static int test_nameval_no_extension ( void )
{
    char name[9], ext[4];
    ext[0] = 'X'; /* ověřit, že se přepíše */
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "HELLO",
        MZDSK_NAMEVAL_FLAVOR_CPM, name, ext, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_OK );
    TEST_ASSERT_EQ_STR ( name, "HELLO" );
    TEST_ASSERT_EQ_STR ( ext, "" );
    return 0;
}


/** @brief MRS flavor - validní jméno projde shodně s CP/M. */
static int test_nameval_mrs_ok ( void )
{
    char name[9], ext[4];
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "HELLO.TXT",
        MZDSK_NAMEVAL_FLAVOR_MRS, name, ext, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_OK );
    TEST_ASSERT_EQ_STR ( name, "HELLO" );
    TEST_ASSERT_EQ_STR ( ext, "TXT" );
    return 0;
}


/** @brief Malá písmena jsou povolená (CP/M knihovna je převede na velká). */
static int test_nameval_lowercase_allowed ( void )
{
    char name[9], ext[4];
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "my.txt",
        MZDSK_NAMEVAL_FLAVOR_CPM, name, ext, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_OK );
    TEST_ASSERT_EQ_STR ( name, "my" );
    TEST_ASSERT_EQ_STR ( ext, "txt" );
    return 0;
}


/** @brief '?' u MRS flavor vyvolá BAD_CHAR. */
static int test_nameval_mrs_question_mark ( void )
{
    char bad = 0;
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "ABC?.DAT",
        MZDSK_NAMEVAL_FLAVOR_MRS, NULL, NULL, &bad );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_BAD_CHAR );
    TEST_ASSERT_EQ_INT ( bad, '?' );
    return 0;
}


/** @brief Více teček v rámci přípony do 3 znaků - BAD_CHAR s '.'. */
static int test_nameval_multiple_dots ( void )
{
    /* "A.B.C" - split na první tečce: name="A", ext="B.C" (délka 3,
       v limitu). Druhá tečka v příponě = zakázaný znak. */
    char bad = 0;
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "A.B.C",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, &bad );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_BAD_CHAR );
    TEST_ASSERT_EQ_INT ( bad, '.' );
    return 0;
}


/** @brief Tab (0x09) jako control char vyvolá BAD_CHAR. */
static int test_nameval_control_char ( void )
{
    char bad = 0;
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "A\tB.TXT",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, &bad );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_BAD_CHAR );
    TEST_ASSERT_EQ_INT ( bad, '\t' );
    return 0;
}


/** @brief 9 znaků jména, 3 ext - preferuj NAME_TOO_LONG, ne BAD_CHAR. */
static int test_nameval_length_precedes_bad_char ( void )
{
    /* 9 znaků name, přitom obsahuje '*' - musí vrátit NAME_TOO_LONG */
    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( "TOOLONGN*.XYZ",
        MZDSK_NAMEVAL_FLAVOR_CPM, NULL, NULL, NULL );
    TEST_ASSERT_EQ_INT ( r, MZDSK_NAMEVAL_NAME_TOO_LONG );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    memory_driver_init();
    TEST_INIT();

    RUN_TEST ( test_invert_data );
    RUN_TEST ( test_strncpy_padding );
    RUN_TEST ( test_mzstrcmp );
    RUN_TEST ( test_error_messages );
    RUN_TEST ( test_disc_open_close );
    RUN_TEST ( test_disc_close_partial_init );
    RUN_TEST ( test_disc_save_reopen );
    RUN_TEST ( test_sector_info_cb );

    /* Striktní validátor 8.3 jména */
    RUN_TEST ( test_nameval_cpm_ok );
    RUN_TEST ( test_nameval_exactly_8_3 );
    RUN_TEST ( test_nameval_name_too_long );
    RUN_TEST ( test_nameval_ext_too_long );
    RUN_TEST ( test_nameval_empty_name_before_dot );
    RUN_TEST ( test_nameval_empty_input );
    RUN_TEST ( test_nameval_bad_char_asterisk );
    RUN_TEST ( test_nameval_bad_char_space );
    RUN_TEST ( test_nameval_no_extension );
    RUN_TEST ( test_nameval_mrs_ok );
    RUN_TEST ( test_nameval_lowercase_allowed );
    RUN_TEST ( test_nameval_mrs_question_mark );
    RUN_TEST ( test_nameval_multiple_dots );
    RUN_TEST ( test_nameval_control_char );
    RUN_TEST ( test_nameval_length_precedes_bad_char );

    TEST_SUMMARY();
}
