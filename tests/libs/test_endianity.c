/**
 * @file test_endianity.c
 * @brief Testy byte swap konverzí (LE/BE).
 *
 * Na x86 (little-endian) platformě LE funkce vrací vstup beze změny,
 * BE funkce prohodí bajty. Testuje se i roundtrip a hraniční hodnoty.
 */

#include <stdio.h>
#include <stdint.h>
#include "test_framework.h"
#include "libs/endianity/endianity.h"


/** @brief LE 16-bit: na LE platformě beze změny. */
static int test_bswap16_le ( void )
{
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_LE ( 0x1234 ), 0x1234 );
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_LE ( 0x0001 ), 0x0001 );
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_LE ( 0xABCD ), 0xABCD );
    return 0;
}


/** @brief BE 16-bit: prohodí bajty. */
static int test_bswap16_be ( void )
{
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_BE ( 0x1234 ), 0x3412 );
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_BE ( 0x0001 ), 0x0100 );
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_BE ( 0xFF00 ), 0x00FF );
    return 0;
}


/** @brief LE 32-bit konverze. */
static int test_bswap32_le ( void )
{
    TEST_ASSERT_EQ_UINT ( endianity_bswap32_LE ( 0x12345678 ), 0x12345678 );
    return 0;
}


/** @brief BE 32-bit: prohodí bajty. */
static int test_bswap32_be ( void )
{
    TEST_ASSERT_EQ_UINT ( endianity_bswap32_BE ( 0x12345678 ), 0x78563412 );
    TEST_ASSERT_EQ_UINT ( endianity_bswap32_BE ( 0x000000FF ), 0xFF000000 );
    return 0;
}


/** @brief 64-bit konverze. */
static int test_bswap64 ( void )
{
    uint64_t val = 0x0102030405060708ULL;
    TEST_ASSERT ( endianity_bswap64_LE ( val ) == val, "LE 64-bit identity" );
    TEST_ASSERT ( endianity_bswap64_BE ( val ) == 0x0807060504030201ULL, "BE 64-bit swap" );
    return 0;
}


/** @brief Dvojité volání = identita. */
static int test_bswap_roundtrip ( void )
{
    uint16_t v16 = 0xCAFE;
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_BE ( endianity_bswap16_BE ( v16 ) ), v16 );

    uint32_t v32 = 0xDEADBEEF;
    TEST_ASSERT_EQ_UINT ( endianity_bswap32_BE ( endianity_bswap32_BE ( v32 ) ), v32 );
    return 0;
}


/** @brief Nula zůstane nulou. */
static int test_bswap_zero ( void )
{
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_BE ( 0 ), 0 );
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_LE ( 0 ), 0 );
    TEST_ASSERT_EQ_UINT ( endianity_bswap32_BE ( 0 ), 0 );
    TEST_ASSERT_EQ_UINT ( endianity_bswap32_LE ( 0 ), 0 );
    return 0;
}


/** @brief 0xFFFF / 0xFFFFFFFF zůstane stejný po swapu. */
static int test_bswap_max ( void )
{
    TEST_ASSERT_EQ_UINT ( endianity_bswap16_BE ( 0xFFFF ), 0xFFFF );
    TEST_ASSERT_EQ_UINT ( endianity_bswap32_BE ( 0xFFFFFFFF ), 0xFFFFFFFF );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    TEST_INIT();

    RUN_TEST ( test_bswap16_le );
    RUN_TEST ( test_bswap16_be );
    RUN_TEST ( test_bswap32_le );
    RUN_TEST ( test_bswap32_be );
    RUN_TEST ( test_bswap64 );
    RUN_TEST ( test_bswap_roundtrip );
    RUN_TEST ( test_bswap_zero );
    RUN_TEST ( test_bswap_max );

    TEST_SUMMARY();
}
