/**
 * @file test_memory_driver.c
 * @brief Testy paměťového driveru (realloc varianta).
 *
 * Testuje: prepare+write s rozšířením bufferu, truncate,
 * read-only ochrana, základní čtení/zápis.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "test_framework.h"

#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/* ====================================================================
 *  Pomocné funkce
 * ==================================================================== */

/**
 * @brief Vytvoří testovací handler s paměťovým driverem.
 *
 * @param h Handler (předalokovaný).
 * @param initial_size Počáteční velikost bufferu.
 * @param fill Filler byte pro počáteční data.
 */
static void setup_handler ( st_HANDLER *h, uint32_t initial_size, uint8_t fill )
{
    memset ( h, 0, sizeof ( st_HANDLER ) );
    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) malloc ( initial_size );
    h->spec.memspec.size = initial_size;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;
    memset ( h->spec.memspec.ptr, fill, initial_size );
}


/**
 * @brief Uvolní handler.
 *
 * @param h Handler.
 */
static void teardown_handler ( st_HANDLER *h )
{
    if ( h->spec.memspec.ptr ) {
        free ( h->spec.memspec.ptr );
        h->spec.memspec.ptr = NULL;
        h->spec.memspec.size = 0;
    }
}


/* ====================================================================
 *  Testy: základní čtení a zápis
 * ==================================================================== */


/** @brief Zápis a čtení v rámci existujícího bufferu. */
static int test_write_read_basic ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 256, 0x00 );

    uint8_t data[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    TEST_ASSERT_OK ( generic_driver_ppwrite ( &h, 10, data, 4 ) );

    uint8_t buf[4] = { 0 };
    TEST_ASSERT_OK ( generic_driver_read ( &h, 10, buf, 4 ) );
    TEST_ASSERT_EQ_MEM ( buf, data, 4 );

    /* Velikost se nemění */
    TEST_ASSERT_EQ_UINT ( h.spec.memspec.size, 256 );

    teardown_handler ( &h );
    return 0;
}


/* ====================================================================
 *  Testy: rozšíření bufferu přes prepare + ppwrite
 * ==================================================================== */


/** @brief ppwrite za konec bufferu selže (rozšíření je jen přes prepare). */
static int test_ppwrite_beyond_buffer_fails ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 100, 0x00 );

    uint8_t data[50];
    memset ( data, 0xAA, 50 );
    /* Offset 80 + 50 bajtů = 130, ale buffer je jen 100 */
    TEST_ASSERT_FAIL ( generic_driver_ppwrite ( &h, 80, data, 50 ) );

    /* Velikost se nesmí změnit */
    TEST_ASSERT_EQ_UINT ( h.spec.memspec.size, 100 );

    teardown_handler ( &h );
    return 0;
}


/** @brief prepare rozšíří buffer a ppwrite pak uspěje. */
static int test_prepare_extends_then_write ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 100, 0x00 );

    /* prepare na offset 80, 50 bajtů -> rozšíří buffer na 130 */
    uint8_t local_buf[50];
    uint8_t *direct_ptr = NULL;
    TEST_ASSERT_OK ( generic_driver_prepare ( &h, 80,
                     (void **) &direct_ptr, &local_buf, 50 ) );

    TEST_ASSERT ( h.spec.memspec.size >= 130, "buffer must grow to at least 130" );

    /* Zapsat přes přímý ukazatel */
    memset ( direct_ptr, 0xBB, 50 );

    /* ppwrite teď uspěje (buffer je dostatečně velký) */
    TEST_ASSERT_OK ( generic_driver_ppwrite ( &h, 80, direct_ptr, 50 ) );

    /* Ověřit data */
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 80, 0xBB );
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 129, 0xBB );

    /* Původní data zachována */
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 0, 0x00 );
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 79, 0x00 );

    teardown_handler ( &h );
    return 0;
}


/** @brief Opakované rozšiřování bufferu přes prepare + ppwrite. */
static int test_multiple_extensions ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 10, 0x00 );

    for ( int i = 0; i < 100; i++ ) {
        uint8_t byte = (uint8_t) i;
        uint8_t local;
        uint8_t *ptr = NULL;
        uint32_t offset = (uint32_t) i * 10;
        TEST_ASSERT_OK ( generic_driver_prepare ( &h, offset,
                         (void **) &ptr, &local, 1 ) );
        *ptr = byte;
        TEST_ASSERT_OK ( generic_driver_ppwrite ( &h, offset, ptr, 1 ) );
    }

    TEST_ASSERT ( h.spec.memspec.size >= 991, "buffer must grow" );

    /* Ověřit první a poslední zapsaný byte */
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 0, 0 );
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 990, 99 );

    teardown_handler ( &h );
    return 0;
}


/* ====================================================================
 *  Testy: truncate
 * ==================================================================== */


/** @brief Truncate zvětší buffer. */
static int test_truncate_grow ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 100, 0xAB );

    TEST_ASSERT_OK ( generic_driver_truncate ( &h, 500 ) );
    TEST_ASSERT_EQ_UINT ( h.spec.memspec.size, 500 );

    /* Původní data musí být zachována */
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 0, 0xAB );
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 99, 0xAB );

    teardown_handler ( &h );
    return 0;
}


/** @brief Truncate zmenší buffer. */
static int test_truncate_shrink ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 500, 0xAB );

    TEST_ASSERT_OK ( generic_driver_truncate ( &h, 100 ) );
    TEST_ASSERT_EQ_UINT ( h.spec.memspec.size, 100 );

    /* Data v zachovaném rozsahu musí být stále OK */
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 0, 0xAB );
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 99, 0xAB );

    teardown_handler ( &h );
    return 0;
}


/** @brief Truncate na 0 musí selhat. */
static int test_truncate_to_zero_fails ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 100, 0x00 );

    TEST_ASSERT_FAIL ( generic_driver_truncate ( &h, 0 ) );

    /* Velikost se nesmí změnit */
    TEST_ASSERT_EQ_UINT ( h.spec.memspec.size, 100 );

    teardown_handler ( &h );
    return 0;
}


/* ====================================================================
 *  Testy: read-only ochrana
 * ==================================================================== */


/** @brief Zápis do read-only handleru musí selhat. */
static int test_readonly_write_fails ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 100, 0x00 );

    generic_driver_set_handler_readonly_status ( &h, 1 );

    uint8_t data = 0xFF;
    TEST_ASSERT_FAIL ( generic_driver_ppwrite ( &h, 0, &data, 1 ) );

    /* Data nesmí být změněna */
    TEST_ASSERT_BYTE ( h.spec.memspec.ptr, 0, 0x00 );

    teardown_handler ( &h );
    return 0;
}


/** @brief Truncate na read-only handleru musí selhat. */
static int test_readonly_truncate_fails ( void )
{
    st_HANDLER h;
    setup_handler ( &h, 100, 0x00 );

    generic_driver_set_handler_readonly_status ( &h, 1 );

    TEST_ASSERT_FAIL ( generic_driver_truncate ( &h, 200 ) );
    TEST_ASSERT_EQ_UINT ( h.spec.memspec.size, 100 );

    teardown_handler ( &h );
    return 0;
}


/* ====================================================================
 *  main
 * ==================================================================== */


int main ( void )
{
    memory_driver_init ();

    TEST_INIT ();

    /* základní čtení/zápis */
    RUN_TEST ( test_write_read_basic );

    /* rozšíření bufferu */
    RUN_TEST ( test_ppwrite_beyond_buffer_fails );
    RUN_TEST ( test_prepare_extends_then_write );
    RUN_TEST ( test_multiple_extensions );

    /* truncate */
    RUN_TEST ( test_truncate_grow );
    RUN_TEST ( test_truncate_shrink );
    RUN_TEST ( test_truncate_to_zero_fails );

    /* read-only */
    RUN_TEST ( test_readonly_write_fails );
    RUN_TEST ( test_readonly_truncate_fails );

    TEST_SUMMARY ();
}
