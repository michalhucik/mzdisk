/**
 * @file test_output_format.c
 * @brief Testy JSON/CSV streaming výstupu.
 *
 * CLI testy závisí na JSON výstupu pro asserty (grep na "key": value).
 * Chyba zde = falešně procházející CLI testy.
 *
 * Výstup se zachytává přes tmpfile() a čte zpět do bufferu.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_framework.h"
#include "libs/output_format/output_format.h"


/* ===================================================================
 *  Pomocné funkce
 * =================================================================== */


/**
 * @brief Zachytí výstup formátovacích funkcí do řetězce.
 *
 * Přesměruje ctx->out na tmpfile, provede callback, přečte výsledek.
 *
 * @param buf Výstupní buffer.
 * @param buf_size Velikost bufferu.
 * @param ctx Kontext (bude modifikován - nastaví se out).
 * @param callback Funkce generující výstup.
 */
typedef void ( *outfmt_test_cb ) ( st_OUTFMT_CTX *ctx );

static int capture_output ( char *buf, size_t buf_size,
                             st_OUTFMT_CTX *ctx, outfmt_test_cb callback )
{
    FILE *f = tmpfile();
    if ( !f ) return -1;

    ctx->out = f;
    callback ( ctx );
    fflush ( f );

    long len = ftell ( f );
    if ( len < 0 ) { fclose ( f ); return -1; }
    if ( (size_t) len >= buf_size ) len = (long) buf_size - 1;

    rewind ( f );
    size_t rd = fread ( buf, 1, (size_t) len, f );
    buf[rd] = '\0';
    fclose ( f );
    return (int) rd;
}


/* ===================================================================
 *  Testy parsování
 * =================================================================== */


/** @brief outfmt_parse("text") = TEXT. */
static int test_parse_text ( void )
{
    en_OUTFMT fmt;
    TEST_ASSERT_EQ_INT ( outfmt_parse ( "text", &fmt ), 0 );
    TEST_ASSERT_EQ_INT ( fmt, OUTFMT_TEXT );
    return 0;
}


/** @brief outfmt_parse("json") = JSON. */
static int test_parse_json ( void )
{
    en_OUTFMT fmt;
    TEST_ASSERT_EQ_INT ( outfmt_parse ( "json", &fmt ), 0 );
    TEST_ASSERT_EQ_INT ( fmt, OUTFMT_JSON );
    return 0;
}


/** @brief outfmt_parse("csv") = CSV. */
static int test_parse_csv ( void )
{
    en_OUTFMT fmt;
    TEST_ASSERT_EQ_INT ( outfmt_parse ( "csv", &fmt ), 0 );
    TEST_ASSERT_EQ_INT ( fmt, OUTFMT_CSV );
    return 0;
}


/** @brief outfmt_parse neznámého formátu vrátí -1. */
static int test_parse_unknown ( void )
{
    en_OUTFMT fmt;
    TEST_ASSERT_EQ_INT ( outfmt_parse ( "xml", &fmt ), -1 );
    TEST_ASSERT_EQ_INT ( outfmt_parse ( "", &fmt ), -1 );
    return 0;
}


/* ===================================================================
 *  Testy JSON výstupu
 * =================================================================== */


static void cb_json_kv_str ( st_OUTFMT_CTX *ctx )
{
    outfmt_doc_begin ( ctx );
    outfmt_kv_str ( ctx, "name", "test.dsk" );
    outfmt_doc_end ( ctx );
}

/** @brief JSON klíč-hodnota řetězec. */
static int test_json_kv_str ( void )
{
    st_OUTFMT_CTX ctx;
    outfmt_init ( &ctx, OUTFMT_JSON );

    char buf[512];
    capture_output ( buf, sizeof ( buf ), &ctx, cb_json_kv_str );

    TEST_ASSERT ( strstr ( buf, "\"name\"" ) != NULL, "contains key" );
    TEST_ASSERT ( strstr ( buf, "\"test.dsk\"" ) != NULL, "contains value" );
    TEST_ASSERT ( buf[0] == '{', "starts with {" );
    TEST_ASSERT ( strstr ( buf, "}" ) != NULL, "contains }" );
    return 0;
}


static void cb_json_kv_int ( st_OUTFMT_CTX *ctx )
{
    outfmt_doc_begin ( ctx );
    outfmt_kv_int ( ctx, "tracks", 160 );
    outfmt_doc_end ( ctx );
}

/** @brief JSON klíč-hodnota číslo. */
static int test_json_kv_int ( void )
{
    st_OUTFMT_CTX ctx;
    outfmt_init ( &ctx, OUTFMT_JSON );

    char buf[512];
    capture_output ( buf, sizeof ( buf ), &ctx, cb_json_kv_int );

    TEST_ASSERT ( strstr ( buf, "\"tracks\"" ) != NULL, "contains key" );
    TEST_ASSERT ( strstr ( buf, "160" ) != NULL, "contains value" );
    return 0;
}


static void cb_json_array ( st_OUTFMT_CTX *ctx )
{
    outfmt_doc_begin ( ctx );
    outfmt_array_begin ( ctx, "files" );
    outfmt_item_begin ( ctx );
    outfmt_field_str ( ctx, "name", "A.COM" );
    outfmt_field_int ( ctx, "size", 256 );
    outfmt_item_end ( ctx );
    outfmt_item_begin ( ctx );
    outfmt_field_str ( ctx, "name", "B.COM" );
    outfmt_field_int ( ctx, "size", 512 );
    outfmt_item_end ( ctx );
    outfmt_array_end ( ctx );
    outfmt_doc_end ( ctx );
}

/** @brief JSON pole s položkami. */
static int test_json_array ( void )
{
    st_OUTFMT_CTX ctx;
    outfmt_init ( &ctx, OUTFMT_JSON );

    char buf[1024];
    capture_output ( buf, sizeof ( buf ), &ctx, cb_json_array );

    TEST_ASSERT ( strstr ( buf, "\"files\"" ) != NULL, "contains array key" );
    TEST_ASSERT ( strstr ( buf, "[" ) != NULL, "contains [" );
    TEST_ASSERT ( strstr ( buf, "]" ) != NULL, "contains ]" );
    TEST_ASSERT ( strstr ( buf, "\"A.COM\"" ) != NULL, "contains first file" );
    TEST_ASSERT ( strstr ( buf, "\"B.COM\"" ) != NULL, "contains second file" );
    TEST_ASSERT ( strstr ( buf, "256" ) != NULL, "contains first size" );
    TEST_ASSERT ( strstr ( buf, "512" ) != NULL, "contains second size" );
    return 0;
}


static void cb_json_hex_bool ( st_OUTFMT_CTX *ctx )
{
    outfmt_doc_begin ( ctx );
    outfmt_array_begin ( ctx, "items" );
    outfmt_item_begin ( ctx );
    outfmt_field_hex16 ( ctx, "addr", 0x1200 );
    outfmt_field_hex8 ( ctx, "type", 0x03 );
    outfmt_field_bool ( ctx, "readonly", 1 );
    outfmt_item_end ( ctx );
    outfmt_array_end ( ctx );
    outfmt_doc_end ( ctx );
}

/** @brief JSON hex a bool pole. */
static int test_json_hex_bool ( void )
{
    st_OUTFMT_CTX ctx;
    outfmt_init ( &ctx, OUTFMT_JSON );

    char buf[512];
    capture_output ( buf, sizeof ( buf ), &ctx, cb_json_hex_bool );

    TEST_ASSERT ( strstr ( buf, "0x1200" ) != NULL, "contains hex16" );
    TEST_ASSERT ( strstr ( buf, "0x03" ) != NULL, "contains hex8" );
    TEST_ASSERT ( strstr ( buf, "true" ) != NULL, "contains bool true" );
    return 0;
}


/* ===================================================================
 *  Testy CSV výstupu
 * =================================================================== */


static void cb_csv_with_header ( st_OUTFMT_CTX *ctx )
{
    const char *hdrs[] = { "name", "size", "type" };
    outfmt_csv_header ( ctx, hdrs, 3 );
    outfmt_item_begin ( ctx );
    outfmt_field_str ( ctx, "name", "A.COM" );
    outfmt_field_int ( ctx, "size", 256 );
    outfmt_field_str ( ctx, "type", "OBJ" );
    outfmt_item_end ( ctx );
}

/** @brief CSV hlavička + řádek. */
static int test_csv_header_fields ( void )
{
    st_OUTFMT_CTX ctx;
    outfmt_init ( &ctx, OUTFMT_CSV );

    char buf[512];
    capture_output ( buf, sizeof ( buf ), &ctx, cb_csv_with_header );

    /* první řádek = hlavička */
    TEST_ASSERT ( strstr ( buf, "name" ) != NULL, "header contains name" );
    TEST_ASSERT ( strstr ( buf, "size" ) != NULL, "header contains size" );
    /* data */
    TEST_ASSERT ( strstr ( buf, "A.COM" ) != NULL, "contains file name" );
    TEST_ASSERT ( strstr ( buf, "256" ) != NULL, "contains size" );
    return 0;
}


/* ===================================================================
 *  Testy TEXT výstupu (NOP)
 * =================================================================== */


static void cb_text_nop ( st_OUTFMT_CTX *ctx )
{
    outfmt_doc_begin ( ctx );
    outfmt_kv_str ( ctx, "key", "value" );
    outfmt_array_begin ( ctx, "arr" );
    outfmt_item_begin ( ctx );
    outfmt_field_str ( ctx, "f", "v" );
    outfmt_item_end ( ctx );
    outfmt_array_end ( ctx );
    outfmt_doc_end ( ctx );
}

/** @brief TEXT formát: všechny funkce jsou NOP. */
static int test_text_nop ( void )
{
    st_OUTFMT_CTX ctx;
    outfmt_init ( &ctx, OUTFMT_TEXT );

    char buf[512];
    int len = capture_output ( buf, sizeof ( buf ), &ctx, cb_text_nop );

    /* TEXT formát negeneruje žádný výstup přes outfmt funkce */
    TEST_ASSERT_EQ_INT ( len, 0 );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    TEST_INIT();

    RUN_TEST ( test_parse_text );
    RUN_TEST ( test_parse_json );
    RUN_TEST ( test_parse_csv );
    RUN_TEST ( test_parse_unknown );
    RUN_TEST ( test_json_kv_str );
    RUN_TEST ( test_json_kv_int );
    RUN_TEST ( test_json_array );
    RUN_TEST ( test_json_hex_bool );
    RUN_TEST ( test_csv_header_fields );
    RUN_TEST ( test_text_nop );

    TEST_SUMMARY();
}
