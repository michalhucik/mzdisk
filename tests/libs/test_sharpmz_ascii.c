/**
 * @file test_sharpmz_ascii.c
 * @brief Testy konverze Sharp MZ ASCII <-> standard ASCII / UTF-8.
 *
 * Testuje jednobajtové konverze (EU/JP), UTF-8 konverze speciálních
 * znaků (přehlásky, šipky, katakana, kanji) a dávkové řetězcové funkce.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "test_framework.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"


/* ===================================================================
 *  EU jednobajtová konverze
 * =================================================================== */


/** @brief EU: roundtrip cnv_to(cnv_from(x)) pro ASCII-kompatibilní rozsah. */
static int test_eu_cnv_roundtrip ( void )
{
    /* Sharp MZ ASCII je kompatibilní se standardní ASCII v rozsahu 0x20-0x5D */
    for ( uint8_t c = 0x20; c <= 0x5D; c++ ) {
        uint8_t ascii = sharpmz_cnv_from ( c );
        uint8_t back = sharpmz_cnv_to ( ascii );
        if ( back != c ) {
            fprintf ( stderr, "    FAIL: roundtrip 0x%02X -> 0x%02X -> 0x%02X\n", c, ascii, back );
            return 1;
        }
    }
    return 0;
}


/** @brief EU: speciální znaky ('}' <-> 0x80, '^' <-> 0x8B). */
static int test_eu_cnv_special ( void )
{
    TEST_ASSERT_EQ_INT ( sharpmz_cnv_from ( 0x80 ), '}' );
    TEST_ASSERT_EQ_INT ( sharpmz_cnv_from ( 0x8B ), '^' );
    TEST_ASSERT_EQ_INT ( sharpmz_cnv_to ( '}' ), 0x80 );
    TEST_ASSERT_EQ_INT ( sharpmz_cnv_to ( '^' ), 0x8B );
    return 0;
}


/** @brief EU: neznámý kód se konvertuje na mezeru. */
static int test_eu_cnv_unknown ( void )
{
    /* grafické kódy bez ASCII ekvivalentu -> mezera */
    uint8_t result = sharpmz_cnv_from ( 0xE0 );
    TEST_ASSERT_EQ_INT ( result, ' ' );

    /* neznámý ASCII znak -> mezera */
    result = sharpmz_cnv_to ( 0x7F ); /* DEL */
    TEST_ASSERT_EQ_INT ( result, ' ' );
    return 0;
}


/* ===================================================================
 *  JP jednobajtová konverze
 * =================================================================== */


/** @brief JP: roundtrip pro ASCII-kompatibilní rozsah (0x20-0x5D). */
static int test_jp_cnv_roundtrip ( void )
{
    for ( uint8_t c = 0x20; c <= 0x5D; c++ ) {
        uint8_t ascii = sharpmz_jp_cnv_from ( c );
        uint8_t back = sharpmz_jp_cnv_to ( ascii );
        if ( back != c ) {
            fprintf ( stderr, "    FAIL: JP roundtrip 0x%02X -> 0x%02X -> 0x%02X\n", c, ascii, back );
            return 1;
        }
    }
    return 0;
}


/** @brief JP: kódy > 0x5D (katakana/grafika) -> mezera. */
static int test_jp_cnv_above_range ( void )
{
    TEST_ASSERT_EQ_INT ( sharpmz_jp_cnv_from ( 0x86 ), ' ' ); /* katakana ヲ */
    TEST_ASSERT_EQ_INT ( sharpmz_jp_cnv_from ( 0xB1 ), ' ' ); /* katakana ア */
    TEST_ASSERT_EQ_INT ( sharpmz_jp_cnv_from ( 0xFF ), ' ' );
    return 0;
}


/* ===================================================================
 *  EU UTF-8 konverze
 * =================================================================== */


/** @brief EU UTF-8: šipky. */
static int test_eu_utf8_arrows ( void )
{
    int converted, printable;

    const char *up = sharpmz_eu_convert_to_UTF8 ( 0x5E, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT_EQ_INT ( printable, 1 );
    TEST_ASSERT ( strlen ( up ) > 0, "arrow should be non-empty UTF-8" );

    const char *left = sharpmz_eu_convert_to_UTF8 ( 0x5F, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT ( strlen ( left ) > 0, "left arrow non-empty" );
    return 0;
}


/** @brief EU UTF-8: německé přehlásky. */
static int test_eu_utf8_umlauts ( void )
{
    int converted, printable;

    const char *oe_upper = sharpmz_eu_convert_to_UTF8 ( 0xA8, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT_EQ_STR ( oe_upper, "Ö" );

    const char *ue_lower = sharpmz_eu_convert_to_UTF8 ( 0xAD, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT_EQ_STR ( ue_lower, "ü" );

    const char *ss = sharpmz_eu_convert_to_UTF8 ( 0xAE, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT_EQ_STR ( ss, "ß" );
    return 0;
}


/** @brief EU UTF-8: roundtrip convert_to_UTF8 -> convert_UTF8_to. */
static int test_eu_utf8_roundtrip ( void )
{
    /* speciální znaky, které mají EU UTF-8 mapování */
    uint8_t test_codes[] = { 0xA8, 0xAD, 0xAE, 0xB2, 0xB9, 0xBA, 0xBB, 0xFB, 0xFF };
    int count = sizeof ( test_codes ) / sizeof ( test_codes[0] );

    for ( int i = 0; i < count; i++ ) {
        int converted, printable;
        const char *utf8 = sharpmz_eu_convert_to_UTF8 ( test_codes[i], &converted, &printable );
        if ( !converted ) continue;

        uint8_t back = sharpmz_eu_convert_UTF8_to ( utf8 );
        if ( back != test_codes[i] ) {
            fprintf ( stderr, "    FAIL: EU UTF-8 roundtrip 0x%02X -> \"%s\" -> 0x%02X\n",
                      test_codes[i], utf8, back );
            return 1;
        }
    }
    return 0;
}


/* ===================================================================
 *  JP UTF-8 konverze
 * =================================================================== */


/** @brief JP UTF-8: katakana konverze. */
static int test_jp_utf8_katakana ( void )
{
    int converted, printable;

    const char *wo = sharpmz_jp_convert_to_UTF8 ( 0x86, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT_EQ_STR ( wo, "ヲ" );

    /* 0xB1 = ム (ne ア - MZ kódová tabulka se liší od JIS) */
    const char *mu = sharpmz_jp_convert_to_UTF8 ( 0xB1, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT ( strlen ( mu ) > 0, "katakana should produce non-empty UTF-8" );
    return 0;
}


/** @brief JP UTF-8: kanji dnů v týdnu. */
static int test_jp_utf8_kanji ( void )
{
    int converted, printable;

    const char *nichi = sharpmz_jp_convert_to_UTF8 ( 0x70, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT_EQ_STR ( nichi, "日" );

    const char *getsu = sharpmz_jp_convert_to_UTF8 ( 0x71, &converted, &printable );
    TEST_ASSERT_EQ_INT ( converted, 1 );
    TEST_ASSERT_EQ_STR ( getsu, "月" );
    return 0;
}


/** @brief JP UTF-8: roundtrip pro katakana a kanji. */
static int test_jp_utf8_roundtrip ( void )
{
    /* katakana: 0x86 (ヲ), 0xB1 (ア), 0xBD (ン) */
    uint8_t test_codes[] = { 0x70, 0x71, 0x72, 0x86, 0xB1, 0xB5, 0xBD };
    int count = sizeof ( test_codes ) / sizeof ( test_codes[0] );

    for ( int i = 0; i < count; i++ ) {
        int converted, printable;
        const char *utf8 = sharpmz_jp_convert_to_UTF8 ( test_codes[i], &converted, &printable );
        if ( !converted ) continue;

        uint8_t back = sharpmz_jp_convert_UTF8_to ( utf8 );
        if ( back != test_codes[i] ) {
            fprintf ( stderr, "    FAIL: JP UTF-8 roundtrip 0x%02X -> \"%s\" -> 0x%02X\n",
                      test_codes[i], utf8, back );
            return 1;
        }
    }
    return 0;
}


/* ===================================================================
 *  Řetězcové konverze
 * =================================================================== */


/** @brief EU: dávková konverze MZ řetězce -> UTF-8. */
static int test_str_to_utf8_eu ( void )
{
    /* "HELLO" v Sharp MZ ASCII = shodné se standardní ASCII (0x20-0x5D kompatibilní) */
    uint8_t mz_str[] = { 'H', 'E', 'L', 'L', 'O' }; /* 0x48, 0x45, 0x4C, 0x4C, 0x4F */
    char utf8[64];

    int ret = sharpmz_str_to_utf8 ( mz_str, 5, utf8, sizeof ( utf8 ), SHARPMZ_CHARSET_EU );
    TEST_ASSERT ( ret > 0, "str_to_utf8 returned > 0" );
    TEST_ASSERT_EQ_STR ( utf8, "HELLO" );
    return 0;
}


/** @brief EU: dávková konverze UTF-8 -> MZ řetězec. */
static int test_str_from_utf8_eu ( void )
{
    uint8_t mz_out[64];

    int ret = sharpmz_str_from_utf8 ( "HELLO", mz_out, sizeof ( mz_out ), SHARPMZ_CHARSET_EU );
    TEST_ASSERT ( ret > 0, "str_from_utf8 returned > 0" );
    TEST_ASSERT_EQ_INT ( mz_out[0], 'H' ); /* 0x48 - MZ ASCII = standard ASCII pro tento rozsah */
    TEST_ASSERT_EQ_INT ( mz_out[1], 'E' ); /* 0x45 */
    TEST_ASSERT_EQ_INT ( mz_out[2], 'L' ); /* 0x4C */
    return 0;
}


/** @brief Řetězcová konverze tam a zpět zachová ASCII obsah. */
static int test_str_roundtrip ( void )
{
    const char *original = "TEST123";
    uint8_t mz_buf[32];
    char ascii_buf[32];

    int n1 = sharpmz_str_from_utf8 ( original, mz_buf, sizeof ( mz_buf ), SHARPMZ_CHARSET_EU );
    TEST_ASSERT ( n1 > 0, "from_utf8" );

    int n2 = sharpmz_str_to_utf8 ( mz_buf, (size_t) n1, ascii_buf, sizeof ( ascii_buf ), SHARPMZ_CHARSET_EU );
    TEST_ASSERT ( n2 > 0, "to_utf8" );
    TEST_ASSERT_EQ_STR ( ascii_buf, original );
    return 0;
}


/** @brief Příliš malý výstupní buffer se nepoškodí. */
static int test_str_buffer_overflow ( void )
{
    uint8_t mz_str[] = { 0x28, 0x25, 0x2C, 0x2C, 0x2F }; /* HELLO */
    char tiny[3]; /* moc malý pro "HELLO" */

    int ret = sharpmz_str_to_utf8 ( mz_str, 5, tiny, sizeof ( tiny ), SHARPMZ_CHARSET_EU );
    /* musí vrátit >= 0, nesmí přetéct buffer */
    TEST_ASSERT ( ret >= 0, "no crash on small buffer" );
    /* výstup musí být null-terminated */
    TEST_ASSERT ( tiny[sizeof ( tiny ) - 1] == '\0' || strlen ( tiny ) < sizeof ( tiny ),
                  "output null-terminated" );
    return 0;
}


/** @brief NULL vstupy se ošetří bez pádu. */
static int test_str_null_edge ( void )
{
    char buf[32];
    uint8_t mz_buf[32];

    int ret1 = sharpmz_str_to_utf8 ( NULL, 5, buf, sizeof ( buf ), SHARPMZ_CHARSET_EU );
    TEST_ASSERT_EQ_INT ( ret1, -1 );

    int ret2 = sharpmz_str_from_utf8 ( NULL, mz_buf, sizeof ( mz_buf ), SHARPMZ_CHARSET_EU );
    TEST_ASSERT_EQ_INT ( ret2, -1 );
    return 0;
}


/** @brief Charset dispatch: sharpmz_to_utf8/from_utf8 s oběma variantami. */
static int test_charset_dispatch ( void )
{
    /* EU: 0xA8 = "Ö" */
    const char *eu = sharpmz_to_utf8 ( 0xA8, SHARPMZ_CHARSET_EU );
    TEST_ASSERT_EQ_STR ( eu, "Ö" );

    int back_eu = sharpmz_from_utf8 ( "Ö", SHARPMZ_CHARSET_EU );
    TEST_ASSERT_EQ_INT ( back_eu, 0xA8 );

    /* JP: 0x70 = "日" */
    const char *jp = sharpmz_to_utf8 ( 0x70, SHARPMZ_CHARSET_JP );
    TEST_ASSERT_EQ_STR ( jp, "日" );

    int back_jp = sharpmz_from_utf8 ( "日", SHARPMZ_CHARSET_JP );
    TEST_ASSERT_EQ_INT ( back_jp, 0x70 );

    /* neznámý UTF-8 -> -1 */
    int unknown = sharpmz_from_utf8 ( "Ж", SHARPMZ_CHARSET_EU );
    TEST_ASSERT_EQ_INT ( unknown, -1 );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    TEST_INIT();

    RUN_TEST ( test_eu_cnv_roundtrip );
    RUN_TEST ( test_eu_cnv_special );
    RUN_TEST ( test_eu_cnv_unknown );
    RUN_TEST ( test_jp_cnv_roundtrip );
    RUN_TEST ( test_jp_cnv_above_range );
    RUN_TEST ( test_eu_utf8_arrows );
    RUN_TEST ( test_eu_utf8_umlauts );
    RUN_TEST ( test_eu_utf8_roundtrip );
    RUN_TEST ( test_jp_utf8_katakana );
    RUN_TEST ( test_jp_utf8_kanji );
    RUN_TEST ( test_jp_utf8_roundtrip );
    RUN_TEST ( test_str_to_utf8_eu );
    RUN_TEST ( test_str_from_utf8_eu );
    RUN_TEST ( test_str_roundtrip );
    RUN_TEST ( test_str_buffer_overflow );
    RUN_TEST ( test_str_null_edge );
    RUN_TEST ( test_charset_dispatch );

    TEST_SUMMARY();
}
