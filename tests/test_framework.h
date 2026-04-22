/**
 * @file test_framework.h
 * @brief Minimální single-header testovací framework.
 *
 * Poskytuje assert makra a test runner. Každý testovací soubor
 * definuje testovací funkce vracející 0 (OK) nebo 1 (FAIL)
 * a volá je přes RUN_TEST() v main().
 *
 * Výstup: jméno testu + OK/FAIL, na konci souhrn.
 *
 * @par Použití:
 * @code
 * #include "test_framework.h"
 *
 * static int test_foo ( void ) {
 *     TEST_ASSERT_EQ_INT ( 1 + 1, 2 );
 *     return 0;
 * }
 *
 * int main ( void ) {
 *     TEST_INIT ();
 *     RUN_TEST ( test_foo );
 *     TEST_SUMMARY ();
 * }
 * @endcode
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>

/** @brief Počítadla testů - deklarace v každém testovacím souboru. */
static int g_test_passed = 0;
static int g_test_failed = 0;
static int g_test_total = 0;

/** @brief Inicializace testovací sady (výpis hlavičky). */
#define TEST_INIT() \
    do { \
        g_test_passed = 0; g_test_failed = 0; g_test_total = 0; \
        printf ( "=== %s ===\n", __FILE__ ); \
    } while ( 0 )

/** @brief Souhrn a návratový kód. */
#define TEST_SUMMARY() \
    do { \
        printf ( "\n--- %d passed, %d failed, %d total ---\n", \
                 g_test_passed, g_test_failed, g_test_total ); \
        return ( g_test_failed > 0 ) ? 1 : 0; \
    } while ( 0 )

/** @brief Spustí jednu testovací funkci a aktualizuje počítadla. */
#define RUN_TEST(fn) \
    do { \
        printf ( "  %-55s", #fn ); \
        fflush ( stdout ); \
        if ( fn () == 0 ) { \
            printf ( "OK\n" ); \
            g_test_passed++; \
        } else { \
            printf ( "FAIL\n" ); \
            g_test_failed++; \
        } \
        g_test_total++; \
    } while ( 0 )


/* ====================================================================
 *  Assert makra - při selhání vypíší soubor:řádek a vrátí 1
 * ==================================================================== */

/** @brief Obecný assert s popisem. */
#define TEST_ASSERT(cond, msg) \
    do { \
        if ( !(cond) ) { \
            fprintf ( stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg) ); \
            return 1; \
        } \
    } while ( 0 )

/** @brief Assert rovnosti dvou int hodnot. */
#define TEST_ASSERT_EQ_INT(actual, expected) \
    do { \
        int _a = (int)(actual); \
        int _e = (int)(expected); \
        if ( _a != _e ) { \
            fprintf ( stderr, "    FAIL %s:%d: expected %d, got %d\n", \
                      __FILE__, __LINE__, _e, _a ); \
            return 1; \
        } \
    } while ( 0 )

/** @brief Assert rovnosti dvou uint32_t hodnot. */
#define TEST_ASSERT_EQ_UINT(actual, expected) \
    do { \
        unsigned int _a = (unsigned int)(actual); \
        unsigned int _e = (unsigned int)(expected); \
        if ( _a != _e ) { \
            fprintf ( stderr, "    FAIL %s:%d: expected %u, got %u\n", \
                      __FILE__, __LINE__, _e, _a ); \
            return 1; \
        } \
    } while ( 0 )

/** @brief Assert nerovnosti (hodnota != nežádoucí). */
#define TEST_ASSERT_NEQ_INT(actual, unwanted) \
    do { \
        int _a = (int)(actual); \
        int _u = (int)(unwanted); \
        if ( _a == _u ) { \
            fprintf ( stderr, "    FAIL %s:%d: got unwanted value %d\n", \
                      __FILE__, __LINE__, _u ); \
            return 1; \
        } \
    } while ( 0 )

/** @brief Assert rovnosti bloků paměti. */
#define TEST_ASSERT_EQ_MEM(actual, expected, size) \
    do { \
        if ( memcmp ( (actual), (expected), (size) ) != 0 ) { \
            fprintf ( stderr, "    FAIL %s:%d: memory mismatch (%d bytes)\n", \
                      __FILE__, __LINE__, (int)(size) ); \
            return 1; \
        } \
    } while ( 0 )

/** @brief Assert, že návratový kód je EXIT_SUCCESS (0). */
#define TEST_ASSERT_OK(expr) \
    TEST_ASSERT ( (expr) == 0, #expr " failed (expected EXIT_SUCCESS)" )

/** @brief Assert, že návratový kód je EXIT_FAILURE (ne 0). */
#define TEST_ASSERT_FAIL(expr) \
    TEST_ASSERT ( (expr) != 0, #expr " succeeded (expected EXIT_FAILURE)" )

/** @brief Assert, že ukazatel není NULL. */
#define TEST_ASSERT_NOT_NULL(ptr) \
    TEST_ASSERT ( (ptr) != NULL, #ptr " is NULL" )

/** @brief Assert, že ukazatel je NULL. */
#define TEST_ASSERT_NULL(ptr) \
    TEST_ASSERT ( (ptr) == NULL, #ptr " is not NULL" )

/** @brief Assert, že bajt na daném offsetu v bufferu má očekávanou hodnotu. */
#define TEST_ASSERT_BYTE(buffer, offset, expected) \
    do { \
        uint8_t _a = ((uint8_t*)(buffer))[(offset)]; \
        uint8_t _e = (uint8_t)(expected); \
        if ( _a != _e ) { \
            fprintf ( stderr, "    FAIL %s:%d: byte[%d] expected 0x%02X, got 0x%02X\n", \
                      __FILE__, __LINE__, (int)(offset), _e, _a ); \
            return 1; \
        } \
    } while ( 0 )

/** @brief Assert rovnosti dvou řetězců (strcmp). */
#define TEST_ASSERT_EQ_STR(actual, expected) \
    do { \
        const char *_a = (actual); \
        const char *_e = (expected); \
        if ( strcmp ( _a, _e ) != 0 ) { \
            fprintf ( stderr, "    FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
                      __FILE__, __LINE__, _e, _a ); \
            return 1; \
        } \
    } while ( 0 )

/** @brief Assert nerovnosti dvou řetězců (strcmp). */
#define TEST_ASSERT_NEQ_STR(actual, unwanted) \
    do { \
        const char *_a = (actual); \
        const char *_u = (unwanted); \
        if ( strcmp ( _a, _u ) == 0 ) { \
            fprintf ( stderr, "    FAIL %s:%d: got unwanted string \"%s\"\n", \
                      __FILE__, __LINE__, _u ); \
            return 1; \
        } \
    } while ( 0 )

/** @brief Bezpodmínečné selhání s popisem. */
#define TEST_FAIL(msg) \
    do { \
        fprintf ( stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg) ); \
        return 1; \
    } while ( 0 )

#endif /* TEST_FRAMEWORK_H */
