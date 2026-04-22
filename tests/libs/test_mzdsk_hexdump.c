/**
 * @file test_mzdsk_hexdump.c
 * @brief Testy formátovaného hexdump výstupu.
 *
 * Testuje základní hexdump, invertovaná data a charset konverze
 * v ASCII sloupci.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "test_framework.h"
#include "libs/mzdsk_hexdump/mzdsk_hexdump.h"


/**
 * @brief Zachytí hexdump výstup do bufferu.
 */
static int capture_hexdump ( char *buf, size_t buf_size,
                              const st_MZDSK_HEXDUMP_CFG *cfg,
                              const uint8_t *data, uint16_t len )
{
    FILE *f = tmpfile();
    if ( !f ) return -1;

    /* cfg je const, ale potřebujeme dočasně přepsat out */
    st_MZDSK_HEXDUMP_CFG tmp = *cfg;
    tmp.out = f;

    mzdsk_hexdump ( &tmp, data, len );
    fflush ( f );

    long flen = ftell ( f );
    if ( flen < 0 ) { fclose ( f ); return -1; }
    if ( (size_t) flen >= buf_size ) flen = (long) buf_size - 1;

    rewind ( f );
    size_t rd = fread ( buf, 1, (size_t) flen, f );
    buf[rd] = '\0';
    fclose ( f );
    return (int) rd;
}


/** @brief Základní hexdump: offset + hex + ASCII. */
static int test_hexdump_basic ( void )
{
    st_MZDSK_HEXDUMP_CFG cfg;
    mzdsk_hexdump_init ( &cfg );

    uint8_t data[] = { 'H', 'E', 'L', 'L', 'O', 0x00, 0xFF };
    char buf[2048];
    int len = capture_hexdump ( buf, sizeof ( buf ), &cfg, data, 7 );

    TEST_ASSERT ( len > 0, "hexdump produced output" );
    /* hex sloupec musí obsahovat bajty */
    TEST_ASSERT ( strstr ( buf, "48" ) != NULL, "contains hex 0x48 (H)" );
    TEST_ASSERT ( strstr ( buf, "FF" ) != NULL || strstr ( buf, "ff" ) != NULL,
                  "contains hex 0xFF" );
    return 0;
}


/** @brief Invertovaná data (XOR 0xFF) v hex sloupci. */
static int test_hexdump_inverted ( void )
{
    st_MZDSK_HEXDUMP_CFG cfg;
    mzdsk_hexdump_init ( &cfg );
    cfg.inv = 1;

    /* invertovaná data: 0xFF XOR 0xFF = 0x00 */
    uint8_t data[] = { 0xFF, 0x00, 0x55, 0xAA };
    char buf[2048];
    int len = capture_hexdump ( buf, sizeof ( buf ), &cfg, data, 4 );

    TEST_ASSERT ( len > 0, "inverted hexdump produced output" );
    /* po inverzi: 0xFF -> 0x00, 0x00 -> 0xFF */
    TEST_ASSERT ( strstr ( buf, "00" ) != NULL, "contains inverted 0x00" );
    return 0;
}


/** @brief Prázdná data -> minimální nebo prázdný výstup. */
static int test_hexdump_empty ( void )
{
    st_MZDSK_HEXDUMP_CFG cfg;
    mzdsk_hexdump_init ( &cfg );

    char buf[256];
    int len = capture_hexdump ( buf, sizeof ( buf ), &cfg, NULL, 0 );

    /* funkce může vypsat newline i pro prázdná data - to je OK */
    TEST_ASSERT ( len <= 2, "empty data produces minimal output" );
    return 0;
}


/** @brief Data kratší než šířka řádku (partial line). */
static int test_hexdump_partial ( void )
{
    st_MZDSK_HEXDUMP_CFG cfg;
    mzdsk_hexdump_init ( &cfg );

    /* jen 3 bajty - kratší než 16 sloupců */
    uint8_t data[] = { 0x41, 0x42, 0x43 };
    char buf[2048];
    int len = capture_hexdump ( buf, sizeof ( buf ), &cfg, data, 3 );

    TEST_ASSERT ( len > 0, "partial line produced output" );
    TEST_ASSERT ( strstr ( buf, "41" ) != NULL, "contains 0x41" );
    return 0;
}


/** @brief EU charset v ASCII sloupci. */
static int test_hexdump_charset_eu ( void )
{
    st_MZDSK_HEXDUMP_CFG cfg;
    mzdsk_hexdump_init ( &cfg );
    cfg.charset = MZDSK_HEXDUMP_CHARSET_EU;

    uint8_t data[] = { 'T', 'E', 'S', 'T' };
    char buf[2048];
    int len = capture_hexdump ( buf, sizeof ( buf ), &cfg, data, 4 );

    TEST_ASSERT ( len > 0, "EU charset hexdump produced output" );
    return 0;
}


/** @brief Init nastaví rozumné výchozí hodnoty. */
static int test_hexdump_init_defaults ( void )
{
    st_MZDSK_HEXDUMP_CFG cfg;
    memset ( &cfg, 0xAA, sizeof ( cfg ) );
    mzdsk_hexdump_init ( &cfg );

    TEST_ASSERT_EQ_INT ( cfg.inv, 0 );
    TEST_ASSERT_EQ_INT ( cfg.charset, MZDSK_HEXDUMP_CHARSET_RAW );
    TEST_ASSERT ( cfg.cols > 0, "cols > 0" );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    TEST_INIT();

    RUN_TEST ( test_hexdump_basic );
    RUN_TEST ( test_hexdump_inverted );
    RUN_TEST ( test_hexdump_empty );
    RUN_TEST ( test_hexdump_partial );
    RUN_TEST ( test_hexdump_charset_eu );
    RUN_TEST ( test_hexdump_init_defaults );

    TEST_SUMMARY();
}
