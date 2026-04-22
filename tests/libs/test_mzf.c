/**
 * @file test_mzf.c
 * @brief Testy MZF formátu - header, body, validace, lifecycle.
 *
 * Testuje parsování a vytváření MZF souborů přes memory driver.
 * Pokrývá header validaci, endianitu, filename konverze a kompletní
 * load/save roundtrip.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "test_framework.h"

#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/* ===================================================================
 *  Pomocné funkce
 * =================================================================== */


/**
 * @brief Připraví memory handler s buffrem dané velikosti.
 *
 * @param h Handler (musí být předalokovaný).
 * @param size Počáteční velikost bufferu.
 */
static void setup_memory_handler ( st_HANDLER *h, uint32_t size )
{
    memset ( h, 0, sizeof ( st_HANDLER ) );
    h->driver = &g_memory_driver_realloc;
    h->spec.memspec.ptr = (uint8_t *) calloc ( 1, size > 0 ? size : 1 );
    h->spec.memspec.size = size > 0 ? size : 1;
    h->type = HANDLER_TYPE_MEMORY;
    h->status = HANDLER_STATUS_READY;
}


/**
 * @brief Uvolní buffer memory handleru.
 *
 * @param h Handler.
 */
static void cleanup_handler ( st_HANDLER *h )
{
    if ( h->spec.memspec.ptr ) {
        free ( h->spec.memspec.ptr );
        h->spec.memspec.ptr = NULL;
    }
}


/* ===================================================================
 *  Testy mzf_tools: header vytváření a jména
 * =================================================================== */


/** @brief Vytvoření headeru přes mzf_tools_create_mzfhdr(). */
static int test_header_create ( void )
{
    uint8_t fname[] = "TESTFILE";
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr (
        MZF_FTYPE_OBJ, 1024, 0x1200, 0x1200,
        fname, 8, NULL
    );
    TEST_ASSERT_NOT_NULL ( hdr );

    TEST_ASSERT_EQ_INT ( hdr->ftype, MZF_FTYPE_OBJ );
    TEST_ASSERT_EQ_INT ( hdr->fsize, 1024 );
    TEST_ASSERT_EQ_INT ( hdr->fstrt, 0x1200 );
    TEST_ASSERT_EQ_INT ( hdr->fexec, 0x1200 );
    TEST_ASSERT_EQ_INT ( hdr->fname.terminator, MZF_FNAME_TERMINATOR );

    /* prvních 8 bajtů jména */
    TEST_ASSERT_EQ_MEM ( hdr->fname.name, fname, 8 );
    /* zbytek vyplněn 0x0D */
    TEST_ASSERT_EQ_INT ( hdr->fname.name[8], MZF_FNAME_TERMINATOR );

    free ( hdr );
    return 0;
}


/** @brief set_fname + get_fname roundtrip. */
static int test_header_set_get_fname ( void )
{
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );

    mzf_tools_set_fname ( &hdr, "HELLO" );

    char ascii[18];
    mzf_tools_get_fname ( &hdr, ascii );

    TEST_ASSERT_EQ_STR ( ascii, "HELLO" );
    return 0;
}


/** @brief get_fname_length pro různé délky. */
static int test_header_fname_length ( void )
{
    st_MZF_HEADER hdr;

    /* délka 0: samé terminátory */
    memset ( &hdr, 0, sizeof ( hdr ) );
    memset ( hdr.fname.name, MZF_FNAME_TERMINATOR, MZF_FILE_NAME_LENGTH );
    hdr.fname.terminator = MZF_FNAME_TERMINATOR;
    TEST_ASSERT_EQ_INT ( mzf_tools_get_fname_length ( &hdr ), 0 );

    /* délka 5 */
    mzf_tools_set_fname ( &hdr, "ABCDE" );
    TEST_ASSERT_EQ_INT ( mzf_tools_get_fname_length ( &hdr ), 5 );

    /* délka 16 (maximální) */
    mzf_tools_set_fname ( &hdr, "1234567890123456" );
    TEST_ASSERT_EQ_INT ( mzf_tools_get_fname_length ( &hdr ), 16 );

    /* délka > 16 se ořízne na 16 */
    mzf_tools_set_fname ( &hdr, "12345678901234567890" );
    TEST_ASSERT_EQ_INT ( mzf_tools_get_fname_length ( &hdr ), 16 );
    return 0;
}


/** @brief get_fname_ex s různými kódováními. */
static int test_header_fname_encoding ( void )
{
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );

    /* nastavit MZ ASCII jméno přímo (bez konverze) */
    uint8_t mz_name[] = "TEST";
    memcpy ( hdr.fname.name, mz_name, 4 );
    memset ( hdr.fname.name + 4, MZF_FNAME_TERMINATOR, MZF_FILE_NAME_LENGTH - 4 );
    hdr.fname.terminator = MZF_FNAME_TERMINATOR;

    char buf[MZF_FNAME_UTF8_BUF_SIZE];

    /* EU ASCII */
    mzf_tools_get_fname_ex ( &hdr, buf, sizeof ( buf ), MZF_NAME_ASCII_EU );
    TEST_ASSERT_EQ_STR ( buf, "TEST" );

    /* JP ASCII - pro ASCII rozsah stejný výsledek */
    mzf_tools_get_fname_ex ( &hdr, buf, sizeof ( buf ), MZF_NAME_ASCII_JP );
    TEST_ASSERT_EQ_STR ( buf, "TEST" );

    /* UTF-8 EU - pro ASCII rozsah stejný výsledek */
    mzf_tools_get_fname_ex ( &hdr, buf, sizeof ( buf ), MZF_NAME_UTF8_EU );
    TEST_ASSERT_EQ_STR ( buf, "TEST" );
    return 0;
}


/* ===================================================================
 *  Testy validace
 * =================================================================== */


/** @brief Validace korektního headeru. */
static int test_header_validate_ok ( void )
{
    uint8_t fname[] = "VALID";
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr (
        MZF_FTYPE_OBJ, 100, 0x1200, 0x1200, fname, 5, NULL
    );
    TEST_ASSERT_NOT_NULL ( hdr );

    en_MZF_ERROR err = mzf_header_validate ( hdr );
    TEST_ASSERT_EQ_INT ( err, MZF_OK );

    free ( hdr );
    return 0;
}


/** @brief Validace bez terminátoru. */
static int test_header_validate_no_term ( void )
{
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    hdr.ftype = MZF_FTYPE_OBJ;

    /* vyplnit celé jméno bez 0x0D terminátoru */
    memset ( hdr.fname.name, 'A', MZF_FILE_NAME_LENGTH );
    hdr.fname.terminator = 'X'; /* ne 0x0D! */

    en_MZF_ERROR err = mzf_header_validate ( &hdr );
    TEST_ASSERT_EQ_INT ( err, MZF_ERROR_NO_FNAME_TERMINATOR );
    return 0;
}


/* ===================================================================
 *  Testy I/O přes memory driver
 * =================================================================== */


/** @brief Zápis + čtení headeru přes memory driver. */
static int test_header_write_read ( void )
{
    st_HANDLER h;
    setup_memory_handler ( &h, 256 );

    /* vytvořit header */
    uint8_t fname[] = "IOTEST";
    st_MZF_HEADER *hdr_out = mzf_tools_create_mzfhdr (
        MZF_FTYPE_BTX, 512, 0x2000, 0x2000, fname, 6, NULL
    );
    TEST_ASSERT_NOT_NULL ( hdr_out );

    /* zapsat */
    TEST_ASSERT_OK ( mzf_write_header ( &h, hdr_out ) );

    /* přečíst zpět */
    st_MZF_HEADER hdr_in;
    TEST_ASSERT_OK ( mzf_read_header ( &h, &hdr_in ) );

    /* porovnat */
    TEST_ASSERT_EQ_INT ( hdr_in.ftype, MZF_FTYPE_BTX );
    TEST_ASSERT_EQ_INT ( hdr_in.fsize, 512 );
    TEST_ASSERT_EQ_INT ( hdr_in.fstrt, 0x2000 );
    TEST_ASSERT_EQ_INT ( hdr_in.fexec, 0x2000 );
    TEST_ASSERT_EQ_INT ( hdr_in.fname.terminator, MZF_FNAME_TERMINATOR );

    char name[18];
    mzf_tools_get_fname ( &hdr_in, name );
    TEST_ASSERT_EQ_STR ( name, "IOTEST" );

    free ( hdr_out );
    cleanup_handler ( &h );
    return 0;
}


/** @brief LE konverze uint16 polí (fsize, fstrt, fexec). */
static int test_header_endianity ( void )
{
    st_HANDLER h;
    setup_memory_handler ( &h, 256 );

    uint8_t fname[] = "END";
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr (
        MZF_FTYPE_OBJ, 0x1234, 0xABCD, 0xEF01, fname, 3, NULL
    );
    TEST_ASSERT_NOT_NULL ( hdr );

    TEST_ASSERT_OK ( mzf_write_header ( &h, hdr ) );

    /* přečíst surové bajty z bufferu - fsize je na offsetu 18-19 (po ftype + fname) */
    uint8_t *buf = h.spec.memspec.ptr;
    /* LE: low byte first */
    TEST_ASSERT_EQ_INT ( buf[18], 0x34 ); /* fsize low */
    TEST_ASSERT_EQ_INT ( buf[19], 0x12 ); /* fsize high */

    /* přečíst zpět přes API */
    st_MZF_HEADER hdr_in;
    TEST_ASSERT_OK ( mzf_read_header ( &h, &hdr_in ) );
    TEST_ASSERT_EQ_INT ( hdr_in.fsize, 0x1234 );
    TEST_ASSERT_EQ_INT ( hdr_in.fstrt, 0xABCD );
    TEST_ASSERT_EQ_INT ( hdr_in.fexec, 0xEF01 );

    free ( hdr );
    cleanup_handler ( &h );
    return 0;
}


/** @brief Zápis + čtení těla přes memory driver. */
static int test_body_write_read ( void )
{
    st_HANDLER h;
    setup_memory_handler ( &h, 512 );

    /* vzorová data */
    uint8_t body_out[128];
    for ( int i = 0; i < 128; i++ ) body_out[i] = (uint8_t) ( i ^ 0x55 );

    TEST_ASSERT_OK ( mzf_write_body ( &h, body_out, 128 ) );

    uint8_t body_in[128];
    TEST_ASSERT_OK ( mzf_read_body ( &h, body_in, 128 ) );
    TEST_ASSERT_EQ_MEM ( body_in, body_out, 128 );

    cleanup_handler ( &h );
    return 0;
}


/* ===================================================================
 *  Testy lifecycle (load/save/free)
 * =================================================================== */


/** @brief Kompletní lifecycle: save -> load -> porovnat. */
static int test_mzf_load_save ( void )
{
    /* vytvořit MZF v paměti */
    st_MZF mzf_out;
    uint8_t fname[] = "ROUNDTRIP";
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr (
        MZF_FTYPE_OBJ, 256, 0x1200, 0x1200, fname, 9, NULL
    );
    TEST_ASSERT_NOT_NULL ( hdr );
    mzf_out.header = *hdr;
    free ( hdr );

    mzf_out.body = (uint8_t *) malloc ( 256 );
    TEST_ASSERT_NOT_NULL ( mzf_out.body );
    for ( int i = 0; i < 256; i++ ) mzf_out.body[i] = (uint8_t) i;
    mzf_out.body_size = 256;

    /* uložit do memory handleru */
    st_HANDLER h;
    setup_memory_handler ( &h, 512 );

    en_MZF_ERROR err = mzf_save ( &h, &mzf_out );
    TEST_ASSERT_EQ_INT ( err, MZF_OK );

    /* načíst zpět */
    en_MZF_ERROR load_err;
    st_MZF *mzf_in = mzf_load ( &h, &load_err );
    TEST_ASSERT_NOT_NULL ( mzf_in );
    TEST_ASSERT_EQ_INT ( load_err, MZF_OK );

    /* porovnat header */
    TEST_ASSERT_EQ_INT ( mzf_in->header.ftype, MZF_FTYPE_OBJ );
    TEST_ASSERT_EQ_INT ( mzf_in->header.fsize, 256 );
    TEST_ASSERT_EQ_INT ( mzf_in->header.fstrt, 0x1200 );

    char name[18];
    mzf_tools_get_fname ( &mzf_in->header, name );
    TEST_ASSERT_EQ_STR ( name, "ROUNDTRIP" );

    /* porovnat body */
    TEST_ASSERT_NOT_NULL ( mzf_in->body );
    TEST_ASSERT_EQ_INT ( mzf_in->body_size, 256 );
    TEST_ASSERT_EQ_MEM ( mzf_in->body, mzf_out.body, 256 );

    mzf_free ( mzf_in );
    free ( mzf_out.body );
    cleanup_handler ( &h );
    return 0;
}


/** @brief MZF s fsize=0: body==NULL, body_size==0. */
static int test_mzf_load_zero_body ( void )
{
    uint8_t fname[] = "EMPTY";
    st_MZF mzf_out;
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr (
        MZF_FTYPE_BSD, 0, 0, 0, fname, 5, NULL
    );
    TEST_ASSERT_NOT_NULL ( hdr );
    mzf_out.header = *hdr;
    free ( hdr );
    mzf_out.body = NULL;
    mzf_out.body_size = 0;

    st_HANDLER h;
    setup_memory_handler ( &h, 256 );
    TEST_ASSERT_EQ_INT ( mzf_save ( &h, &mzf_out ), MZF_OK );

    en_MZF_ERROR load_err;
    st_MZF *mzf_in = mzf_load ( &h, &load_err );
    TEST_ASSERT_NOT_NULL ( mzf_in );
    TEST_ASSERT_EQ_INT ( load_err, MZF_OK );
    TEST_ASSERT_EQ_INT ( mzf_in->header.fsize, 0 );
    TEST_ASSERT_NULL ( mzf_in->body );
    TEST_ASSERT_EQ_INT ( mzf_in->body_size, 0 );

    mzf_free ( mzf_in );
    cleanup_handler ( &h );
    return 0;
}


/** @brief mzf_free(NULL) nespadne. */
static int test_mzf_free_null ( void )
{
    mzf_free ( NULL );
    return 0;
}


/** @brief mzf_file_validate na validním MZF v memory driveru. */
static int test_file_validate_ok ( void )
{
    uint8_t fname[] = "VALID";
    st_MZF mzf;
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr (
        MZF_FTYPE_OBJ, 64, 0x1200, 0x1200, fname, 5, NULL
    );
    TEST_ASSERT_NOT_NULL ( hdr );
    mzf.header = *hdr;
    free ( hdr );
    mzf.body = (uint8_t *) calloc ( 1, 64 );
    mzf.body_size = 64;

    st_HANDLER h;
    setup_memory_handler ( &h, 256 );
    TEST_ASSERT_EQ_INT ( mzf_save ( &h, &mzf ), MZF_OK );

    en_MZF_ERROR err = mzf_file_validate ( &h );
    TEST_ASSERT_EQ_INT ( err, MZF_OK );

    free ( mzf.body );
    cleanup_handler ( &h );
    return 0;
}


/** @brief mzf_file_validate na poškozeném MZF (chybí terminátor). */
static int test_file_validate_bad ( void )
{
    st_HANDLER h;
    setup_memory_handler ( &h, 256 );

    /* zapsat 128 bajtů samých 'A' - žádný 0x0D terminátor v fname */
    uint8_t garbage[128];
    memset ( garbage, 'A', sizeof ( garbage ) );
    generic_driver_write ( &h, 0, garbage, sizeof ( garbage ) );

    en_MZF_ERROR err = mzf_file_validate ( &h );
    TEST_ASSERT ( err != MZF_OK, "corrupted MZF should fail validation" );

    cleanup_handler ( &h );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    memory_driver_init();
    TEST_INIT();

    RUN_TEST ( test_header_create );
    RUN_TEST ( test_header_set_get_fname );
    RUN_TEST ( test_header_fname_length );
    RUN_TEST ( test_header_fname_encoding );
    RUN_TEST ( test_header_validate_ok );
    RUN_TEST ( test_header_validate_no_term );
    RUN_TEST ( test_header_write_read );
    RUN_TEST ( test_header_endianity );
    RUN_TEST ( test_body_write_read );
    RUN_TEST ( test_mzf_load_save );
    RUN_TEST ( test_mzf_load_zero_body );
    RUN_TEST ( test_mzf_free_null );
    RUN_TEST ( test_file_validate_ok );
    RUN_TEST ( test_file_validate_bad );

    TEST_SUMMARY();
}
