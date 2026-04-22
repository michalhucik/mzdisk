/**
 * @file test_config.c
 * @brief Testy konfiguračního systému mzdisk GUI.
 *
 * Testuje inicializaci, MRU seznam (přidávání, deduplikaci, přetečení)
 * a save/load roundtrip přes INI soubor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_framework.h"
#include "config.h"


/* ===================================================================
 *  Inicializace
 * =================================================================== */


/** @brief Ověří výchozí hodnoty po mzdisk_config_init(). */
static int test_config_init_defaults ( void )
{
    st_MZDISK_CONFIG cfg;
    memset ( &cfg, 0xAA, sizeof ( cfg ) );
    mzdisk_config_init ( &cfg );

    TEST_ASSERT_EQ_STR ( cfg.last_open_dir, "." );
    TEST_ASSERT_EQ_STR ( cfg.last_get_dir, "." );
    TEST_ASSERT_EQ_STR ( cfg.last_put_dir, "." );
    TEST_ASSERT_EQ_STR ( cfg.last_create_dir, "." );
    TEST_ASSERT_EQ_INT ( cfg.recent_count, 0 );
    TEST_ASSERT_EQ_STR ( cfg.language, "auto" );
    TEST_ASSERT_EQ_INT ( cfg.font_size, MZDISK_CONFIG_DEFAULT_FONT_SIZE );
    TEST_ASSERT_EQ_INT ( cfg.font_family_idx, 0 );
    TEST_ASSERT_EQ_INT ( cfg.theme_idx, 0 );
    TEST_ASSERT_EQ_INT ( cfg.boot_name_charset, MZDSK_NAME_CHARSET_EU_UTF8 );
    TEST_ASSERT_EQ_INT ( cfg.fsmz_name_charset, MZDSK_NAME_CHARSET_EU_UTF8 );
    TEST_ASSERT_EQ_INT ( cfg.export_dup_mode, MZDSK_EXPORT_DUP_ASK );
    TEST_ASSERT_EQ_INT ( cfg.dnd_dup_mode, MZDSK_EXPORT_DUP_ASK );
    TEST_ASSERT_EQ_INT ( cfg.open_in_new_window, 0 );

    for ( int i = 0; i < MZDISK_TAB_COUNT; i++ ) {
        TEST_ASSERT_EQ_INT ( cfg.tab_visible[i], 1 );
    }
    return 0;
}


/* ===================================================================
 *  MRU seznam
 * =================================================================== */


/** @brief Přidání jednoho souboru do MRU. */
static int test_config_add_recent_basic ( void )
{
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );

    mzdisk_config_add_recent ( &cfg, "/path/to/disk.dsk" );

    TEST_ASSERT_EQ_INT ( cfg.recent_count, 1 );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[0], "/path/to/disk.dsk" );
    return 0;
}


/** @brief Nový soubor se přidá na první pozici, starší se posunou. */
static int test_config_add_recent_pushes_front ( void )
{
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );

    mzdisk_config_add_recent ( &cfg, "first.dsk" );
    mzdisk_config_add_recent ( &cfg, "second.dsk" );
    mzdisk_config_add_recent ( &cfg, "third.dsk" );

    TEST_ASSERT_EQ_INT ( cfg.recent_count, 3 );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[0], "third.dsk" );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[1], "second.dsk" );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[2], "first.dsk" );
    return 0;
}


/** @brief Duplicitní cesta se přesune na začátek (deduplikace). */
static int test_config_add_recent_dedup ( void )
{
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );

    mzdisk_config_add_recent ( &cfg, "a.dsk" );
    mzdisk_config_add_recent ( &cfg, "b.dsk" );
    mzdisk_config_add_recent ( &cfg, "c.dsk" );

    mzdisk_config_add_recent ( &cfg, "a.dsk" );

    TEST_ASSERT_EQ_INT ( cfg.recent_count, 3 );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[0], "a.dsk" );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[1], "c.dsk" );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[2], "b.dsk" );
    return 0;
}


/** @brief Přidání prvku, který je již na první pozici, nic nezmění. */
static int test_config_add_recent_already_first ( void )
{
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );

    mzdisk_config_add_recent ( &cfg, "only.dsk" );
    mzdisk_config_add_recent ( &cfg, "only.dsk" );

    TEST_ASSERT_EQ_INT ( cfg.recent_count, 1 );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[0], "only.dsk" );
    return 0;
}


/** @brief Přetečení MRU seznamu - nejstarší se zahodí. */
static int test_config_add_recent_overflow ( void )
{
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );

    char name[32];
    for ( int i = 0; i < MZDISK_CONFIG_RECENT_MAX; i++ ) {
        snprintf ( name, sizeof ( name ), "file%d.dsk", i );
        mzdisk_config_add_recent ( &cfg, name );
    }

    TEST_ASSERT_EQ_INT ( cfg.recent_count, MZDISK_CONFIG_RECENT_MAX );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[0], "file9.dsk" );

    mzdisk_config_add_recent ( &cfg, "overflow.dsk" );

    TEST_ASSERT_EQ_INT ( cfg.recent_count, MZDISK_CONFIG_RECENT_MAX );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[0], "overflow.dsk" );

    for ( int i = 0; i < cfg.recent_count; i++ ) {
        TEST_ASSERT_NEQ_STR ( cfg.recent_files[i], "file0.dsk" );
    }
    return 0;
}


/** @brief Vymazání MRU seznamu. */
static int test_config_clear_recent ( void )
{
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );

    mzdisk_config_add_recent ( &cfg, "a.dsk" );
    mzdisk_config_add_recent ( &cfg, "b.dsk" );
    TEST_ASSERT_EQ_INT ( cfg.recent_count, 2 );

    mzdisk_config_clear_recent ( &cfg );
    TEST_ASSERT_EQ_INT ( cfg.recent_count, 0 );
    return 0;
}


/* ===================================================================
 *  Save/Load roundtrip
 * =================================================================== */


/** @brief Uložení a načtení konfigurace přes INI soubor. */
static int test_config_save_load_roundtrip ( void )
{
    char orig_dir[1024];
    if ( getcwd ( orig_dir, sizeof ( orig_dir ) ) == NULL ) {
        TEST_FAIL ( "getcwd failed" );
    }

    char tmpdir[] = "/tmp/mzdsk_test_cfg_XXXXXX";
    if ( mkdtemp ( tmpdir ) == NULL ) {
        TEST_FAIL ( "mkdtemp failed" );
    }

    if ( chdir ( tmpdir ) != 0 ) {
        chdir ( orig_dir );
        TEST_FAIL ( "chdir to tmpdir failed" );
    }

    st_MZDISK_CONFIG cfg_out;
    mzdisk_config_init ( &cfg_out );
    strncpy ( cfg_out.last_open_dir, "/my/open/dir", MZDISK_CONFIG_PATH_MAX );
    strncpy ( cfg_out.last_get_dir, "/my/get/dir", MZDISK_CONFIG_PATH_MAX );
    strncpy ( cfg_out.language, "cs", MZDISK_CONFIG_LANG_MAX );
    cfg_out.font_size = 32;
    cfg_out.theme_idx = 2;
    cfg_out.tab_visible[MZDISK_TAB_HEXDUMP] = false;
    cfg_out.boot_name_charset = MZDSK_NAME_CHARSET_JP_UTF8;
    cfg_out.export_dup_mode = MZDSK_EXPORT_DUP_RENAME;
    cfg_out.dnd_dup_mode = MZDSK_EXPORT_DUP_OVERWRITE;
    cfg_out.open_in_new_window = true;

    mzdisk_config_add_recent ( &cfg_out, "recent1.dsk" );
    mzdisk_config_add_recent ( &cfg_out, "recent2.dsk" );

    bool saved = mzdisk_config_save ( &cfg_out );
    if ( !saved ) {
        remove ( "mzdisk.ini" );
        chdir ( orig_dir );
        rmdir ( tmpdir );
        TEST_FAIL ( "config save failed" );
    }

    st_MZDISK_CONFIG cfg_in;
    mzdisk_config_init ( &cfg_in );
    bool loaded = mzdisk_config_load ( &cfg_in );

    /* úklid před asserty, aby se vždy vrátili */
    remove ( "mzdisk.ini" );
    chdir ( orig_dir );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( loaded, 1 );
    TEST_ASSERT_EQ_STR ( cfg_in.last_open_dir, "/my/open/dir" );
    TEST_ASSERT_EQ_STR ( cfg_in.last_get_dir, "/my/get/dir" );
    TEST_ASSERT_EQ_STR ( cfg_in.language, "cs" );
    TEST_ASSERT_EQ_INT ( cfg_in.font_size, 32 );
    TEST_ASSERT_EQ_INT ( cfg_in.theme_idx, 2 );
    TEST_ASSERT_EQ_INT ( cfg_in.tab_visible[MZDISK_TAB_HEXDUMP], 0 );
    TEST_ASSERT_EQ_INT ( cfg_in.boot_name_charset, MZDSK_NAME_CHARSET_JP_UTF8 );
    TEST_ASSERT_EQ_INT ( cfg_in.export_dup_mode, MZDSK_EXPORT_DUP_RENAME );
    TEST_ASSERT_EQ_INT ( cfg_in.dnd_dup_mode, MZDSK_EXPORT_DUP_OVERWRITE );
    TEST_ASSERT_EQ_INT ( cfg_in.open_in_new_window, 1 );
    TEST_ASSERT_EQ_INT ( cfg_in.recent_count, 2 );
    TEST_ASSERT_EQ_STR ( cfg_in.recent_files[0], "recent2.dsk" );
    TEST_ASSERT_EQ_STR ( cfg_in.recent_files[1], "recent1.dsk" );
    return 0;
}


/* ===================================================================
 *  Watch (detekce externí změny mzdisk.ini)
 * =================================================================== */


/** @brief Vrátí směr pro práci - přepne CWD do tmpdir. */
static int watch_tmpdir_enter ( char *tmpdir, size_t size, char *orig_dir, size_t orig_size )
{
    if ( getcwd ( orig_dir, orig_size ) == NULL ) return -1;
    strncpy ( tmpdir, "/tmp/mzdsk_test_watch_XXXXXX", size );
    tmpdir[size - 1] = '\0';
    if ( mkdtemp ( tmpdir ) == NULL ) return -1;
    if ( chdir ( tmpdir ) != 0 ) { rmdir ( tmpdir ); return -1; }
    return 0;
}


/** @brief Úklid tmpdir - CWD zpět + smazání obsahu. */
static void watch_tmpdir_leave ( const char *tmpdir, const char *orig_dir )
{
    remove ( "mzdisk.ini" );
    chdir ( orig_dir );
    rmdir ( tmpdir );
}


/** @brief Watch init nad neexistujícím souborem - poll vrátí false bez pádu. */
static int test_watch_init_on_missing_file ( void )
{
    char tmpdir[64], orig_dir[1024];
    if ( watch_tmpdir_enter ( tmpdir, sizeof ( tmpdir ), orig_dir, sizeof ( orig_dir ) ) != 0 ) {
        TEST_FAIL ( "tmpdir setup failed" );
    }

    st_MZDISK_CONFIG_WATCH w;
    mzdisk_config_watch_init ( &w, "mzdisk.ini" );
    mzdisk_config_watch_mark_saved ( &w );  /* soubor neexistuje */

    /* první poll nic nedetekuje (neexistuje) */
    bool changed = mzdisk_config_watch_poll ( &w, 10000 );

    watch_tmpdir_leave ( tmpdir, orig_dir );

    TEST_ASSERT_EQ_INT ( changed, 0 );
    TEST_ASSERT ( w.last_size == -1, "baseline zůstává neznámá" );
    return 0;
}


/** @brief Watch detekuje externí změnu (simulováno druhým save s jiným obsahem). */
static int test_watch_detects_external_change ( void )
{
    char tmpdir[64], orig_dir[1024];
    if ( watch_tmpdir_enter ( tmpdir, sizeof ( tmpdir ), orig_dir, sizeof ( orig_dir ) ) != 0 ) {
        TEST_FAIL ( "tmpdir setup failed" );
    }

    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    cfg.font_size = 20;
    bool ok = mzdisk_config_save ( &cfg );

    st_MZDISK_CONFIG_WATCH w;
    mzdisk_config_watch_init ( &w, "mzdisk.ini" );
    mzdisk_config_watch_mark_saved ( &w );

    /* poll hned po save - žádná změna */
    bool ch1 = mzdisk_config_watch_poll ( &w, 10000 );

    /* externí zápis - jiná instance přepíše INI jinými hodnotami.
     * Sleep(1) zajistí rozdíl v mtime na FS s rozlišením 1s. */
    sleep ( 1 );
    cfg.font_size = 36;
    strncpy ( cfg.language, "cs", MZDISK_CONFIG_LANG_MAX );
    bool ok2 = mzdisk_config_save ( &cfg );

    /* posuneme čas přes throttling okno (500 ms) */
    bool ch2 = mzdisk_config_watch_poll ( &w, 10700 );

    /* ověření, že reload dostane novou hodnotu */
    st_MZDISK_CONFIG cfg_in;
    mzdisk_config_init ( &cfg_in );
    bool loaded = mzdisk_config_load ( &cfg_in );

    watch_tmpdir_leave ( tmpdir, orig_dir );

    TEST_ASSERT_EQ_INT ( ok, 1 );
    TEST_ASSERT_EQ_INT ( ok2, 1 );
    TEST_ASSERT_EQ_INT ( ch1, 0 );
    TEST_ASSERT_EQ_INT ( ch2, 1 );
    TEST_ASSERT_EQ_INT ( loaded, 1 );
    TEST_ASSERT_EQ_INT ( cfg_in.font_size, 36 );
    TEST_ASSERT_EQ_STR ( cfg_in.language, "cs" );
    return 0;
}


/** @brief Po vlastním save + mark_saved watch nedetekuje false-positive. */
static int test_watch_ignores_own_save ( void )
{
    char tmpdir[64], orig_dir[1024];
    if ( watch_tmpdir_enter ( tmpdir, sizeof ( tmpdir ), orig_dir, sizeof ( orig_dir ) ) != 0 ) {
        TEST_FAIL ( "tmpdir setup failed" );
    }

    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    mzdisk_config_save ( &cfg );

    st_MZDISK_CONFIG_WATCH w;
    mzdisk_config_watch_init ( &w, "mzdisk.ini" );
    mzdisk_config_watch_mark_saved ( &w );

    /* vlastní save + mark_saved - poll nesmí hlásit změnu */
    sleep ( 1 );
    cfg.font_size = 24;
    mzdisk_config_save ( &cfg );
    mzdisk_config_watch_mark_saved ( &w );

    bool changed = mzdisk_config_watch_poll ( &w, 10000 );

    watch_tmpdir_leave ( tmpdir, orig_dir );

    TEST_ASSERT_EQ_INT ( changed, 0 );
    return 0;
}


/** @brief Throttling: poll v intervalu < poll_interval_ms vrátí false bez stat(). */
static int test_watch_throttle ( void )
{
    char tmpdir[64], orig_dir[1024];
    if ( watch_tmpdir_enter ( tmpdir, sizeof ( tmpdir ), orig_dir, sizeof ( orig_dir ) ) != 0 ) {
        TEST_FAIL ( "tmpdir setup failed" );
    }

    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    mzdisk_config_save ( &cfg );

    st_MZDISK_CONFIG_WATCH w;
    mzdisk_config_watch_init ( &w, "mzdisk.ini" );
    mzdisk_config_watch_mark_saved ( &w );

    /* první poll - provede skutečný stat(), aktualizuje last_check_ms */
    mzdisk_config_watch_poll ( &w, 10000 );

    /* následující poll v rámci throttle okna (< 500 ms) */
    bool ch = mzdisk_config_watch_poll ( &w, 10300 );

    watch_tmpdir_leave ( tmpdir, orig_dir );

    TEST_ASSERT_EQ_INT ( ch, 0 );
    return 0;
}


/** @brief Atomický save nenechá za sebou *.tmp.* soubory v úspěšném scénáři. */
static int test_atomic_save_no_partial ( void )
{
    char tmpdir[64], orig_dir[1024];
    if ( watch_tmpdir_enter ( tmpdir, sizeof ( tmpdir ), orig_dir, sizeof ( orig_dir ) ) != 0 ) {
        TEST_FAIL ( "tmpdir setup failed" );
    }

    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );
    bool ok = mzdisk_config_save ( &cfg );

    /* ověřit, že mzdisk.ini existuje a žádné *.tmp.* zbytky nejsou.
     * Použijeme fopen test na předpokládaný název. */
    char expected_tmp[128];
    snprintf ( expected_tmp, sizeof ( expected_tmp ), "mzdisk.ini.tmp.%d", (int) getpid () );
    FILE *ftmp = fopen ( expected_tmp, "rb" );
    bool tmp_present = ( ftmp != NULL );
    if ( ftmp ) fclose ( ftmp );

    FILE *fini = fopen ( "mzdisk.ini", "rb" );
    bool ini_present = ( fini != NULL );
    if ( fini ) fclose ( fini );

    if ( tmp_present ) remove ( expected_tmp );
    watch_tmpdir_leave ( tmpdir, orig_dir );

    TEST_ASSERT_EQ_INT ( ok, 1 );
    TEST_ASSERT_EQ_INT ( ini_present, 1 );
    TEST_ASSERT_EQ_INT ( tmp_present, 0 );
    return 0;
}


/* ===================================================================
 *  Edge cases
 * =================================================================== */


/** @brief NULL a prázdný řetězec se ignorují bez pádu. */
static int test_config_add_recent_null_empty ( void )
{
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );

    mzdisk_config_add_recent ( &cfg, "valid.dsk" );
    mzdisk_config_add_recent ( &cfg, NULL );
    mzdisk_config_add_recent ( &cfg, "" );
    mzdisk_config_add_recent ( NULL, "crash.dsk" );

    /* žádný pád, stav nezměněn */
    TEST_ASSERT_EQ_INT ( cfg.recent_count, 1 );
    TEST_ASSERT_EQ_STR ( cfg.recent_files[0], "valid.dsk" );
    return 0;
}


/** @brief Cesta blížící se MZDISK_CONFIG_PATH_MAX se ořízne. */
static int test_config_add_recent_long_path ( void )
{
    st_MZDISK_CONFIG cfg;
    mzdisk_config_init ( &cfg );

    /* cesta o délce MZDISK_CONFIG_PATH_MAX + 100 */
    char long_path[MZDISK_CONFIG_PATH_MAX + 100];
    memset ( long_path, 'A', sizeof ( long_path ) - 1 );
    long_path[sizeof ( long_path ) - 1] = '\0';

    mzdisk_config_add_recent ( &cfg, long_path );

    TEST_ASSERT_EQ_INT ( cfg.recent_count, 1 );
    /* cesta musí být oříznutá - nesmí přetéct buffer */
    TEST_ASSERT ( strlen ( cfg.recent_files[0] ) < MZDISK_CONFIG_PATH_MAX,
                  "path truncated to fit buffer" );
    return 0;
}


/** @brief clear_recent s NULL nespadne. */
static int test_config_clear_recent_null ( void )
{
    mzdisk_config_clear_recent ( NULL );
    /* žádný pád = OK */
    return 0;
}


/* ===================================================================
 *  Resolve export duplicate (P1.2)
 *
 *  Pracuje s hostitelským FS - každý test si vytvoří unikátní tmpdir.
 * =================================================================== */


/** @brief Vytvoří prázdný soubor na zadané cestě (pro detekci kolize). */
static void touch_file ( const char *path )
{
    FILE *f = fopen ( path, "wb" );
    if ( f ) fclose ( f );
}


/** @brief Připraví unikátní tmpdir pro test, vrátí jeho cestu (statický buf). */
static const char *make_tmpdir ( void )
{
    static char tmpdir[64];
    strncpy ( tmpdir, "/tmp/mzdsk_test_dup_XXXXXX", sizeof ( tmpdir ) );
    if ( mkdtemp ( tmpdir ) == NULL ) return NULL;
    return tmpdir;
}


/** @brief Neexistující soubor - funkce vrátí 0 a path se nezmění. */
static int test_resolve_dup_nonexistent ( void )
{
    const char *tmpdir = make_tmpdir();
    if ( !tmpdir ) TEST_FAIL ( "mkdtemp failed" );

    char path[512];
    snprintf ( path, sizeof ( path ), "%s/nope.txt", tmpdir );

    int r = mzdisk_config_resolve_export_dup ( path, sizeof ( path ), MZDSK_EXPORT_DUP_RENAME );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( r, 0 );
    /* path končí na "nope.txt" - nezměněno */
    TEST_ASSERT ( strstr ( path, "/nope.txt" ) != NULL, "path not changed for nonexistent file" );
    return 0;
}


/** @brief Existující soubor + OVERWRITE → 0, path se nezmění. */
static int test_resolve_dup_overwrite ( void )
{
    const char *tmpdir = make_tmpdir();
    if ( !tmpdir ) TEST_FAIL ( "mkdtemp failed" );

    char path[512];
    snprintf ( path, sizeof ( path ), "%s/exists.txt", tmpdir );
    touch_file ( path );

    char saved[512];
    strncpy ( saved, path, sizeof ( saved ) );

    int r = mzdisk_config_resolve_export_dup ( path, sizeof ( path ), MZDSK_EXPORT_DUP_OVERWRITE );

    remove ( saved );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( r, 0 );
    TEST_ASSERT_EQ_STR ( path, saved );
    return 0;
}


/** @brief Existující soubor + SKIP → 1. */
static int test_resolve_dup_skip ( void )
{
    const char *tmpdir = make_tmpdir();
    if ( !tmpdir ) TEST_FAIL ( "mkdtemp failed" );

    char path[512];
    snprintf ( path, sizeof ( path ), "%s/exists.txt", tmpdir );
    touch_file ( path );

    int r = mzdisk_config_resolve_export_dup ( path, sizeof ( path ), MZDSK_EXPORT_DUP_SKIP );

    remove ( path );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( r, 1 );
    return 0;
}


/** @brief RENAME: NAME.EXT existuje → NAME~2.EXT. */
static int test_resolve_dup_rename_basic ( void )
{
    const char *tmpdir = make_tmpdir();
    if ( !tmpdir ) TEST_FAIL ( "mkdtemp failed" );

    char path[512];
    snprintf ( path, sizeof ( path ), "%s/FILE.EXT", tmpdir );
    touch_file ( path );

    char expected[512];
    snprintf ( expected, sizeof ( expected ), "%s/FILE~2.EXT", tmpdir );

    char orig[512];
    snprintf ( orig, sizeof ( orig ), "%s/FILE.EXT", tmpdir );

    int r = mzdisk_config_resolve_export_dup ( path, sizeof ( path ), MZDSK_EXPORT_DUP_RENAME );

    remove ( orig );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( r, 0 );
    TEST_ASSERT_EQ_STR ( path, expected );
    return 0;
}


/** @brief RENAME: NAME.EXT i NAME~2.EXT existují → NAME~3.EXT. */
static int test_resolve_dup_rename_multiple ( void )
{
    const char *tmpdir = make_tmpdir();
    if ( !tmpdir ) TEST_FAIL ( "mkdtemp failed" );

    char p1[512], p2[512];
    snprintf ( p1, sizeof ( p1 ), "%s/FILE.EXT", tmpdir );
    snprintf ( p2, sizeof ( p2 ), "%s/FILE~2.EXT", tmpdir );
    touch_file ( p1 );
    touch_file ( p2 );

    char path[512];
    strncpy ( path, p1, sizeof ( path ) );

    char expected[512];
    snprintf ( expected, sizeof ( expected ), "%s/FILE~3.EXT", tmpdir );

    int r = mzdisk_config_resolve_export_dup ( path, sizeof ( path ), MZDSK_EXPORT_DUP_RENAME );

    remove ( p1 );
    remove ( p2 );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( r, 0 );
    TEST_ASSERT_EQ_STR ( path, expected );
    return 0;
}


/** @brief RENAME: jméno bez přípony (README → README~2). */
static int test_resolve_dup_rename_no_ext ( void )
{
    const char *tmpdir = make_tmpdir();
    if ( !tmpdir ) TEST_FAIL ( "mkdtemp failed" );

    char path[512];
    snprintf ( path, sizeof ( path ), "%s/README", tmpdir );
    touch_file ( path );

    char orig[512];
    strncpy ( orig, path, sizeof ( orig ) );

    char expected[512];
    snprintf ( expected, sizeof ( expected ), "%s/README~2", tmpdir );

    int r = mzdisk_config_resolve_export_dup ( path, sizeof ( path ), MZDSK_EXPORT_DUP_RENAME );

    remove ( orig );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( r, 0 );
    TEST_ASSERT_EQ_STR ( path, expected );
    return 0;
}


/**
 * @brief RENAME: tečka je v adresářové cestě, ne v názvu souboru.
 *
 * Např. `/tmp/mzdsk_test_dup_XXXXXX.dir/FILE` - funkce nesmí
 * považovat tečku v adresářové cestě za oddělovač přípony.
 */
static int test_resolve_dup_rename_dot_in_dir ( void )
{
    char dirpath[256];
    strncpy ( dirpath, "/tmp/mzdsk_test_dup_XXXXXX", sizeof ( dirpath ) );
    if ( mkdtemp ( dirpath ) == NULL ) TEST_FAIL ( "mkdtemp failed" );

    /* přejmenovat tmpdir na "<puvodni>.dir" */
    char dirdot[320];
    snprintf ( dirdot, sizeof ( dirdot ), "%s.dir", dirpath );
    if ( rename ( dirpath, dirdot ) != 0 ) {
        rmdir ( dirpath );
        TEST_FAIL ( "rename tmpdir failed" );
    }

    char path[512];
    snprintf ( path, sizeof ( path ), "%s/FILE", dirdot );
    touch_file ( path );

    char orig[512];
    strncpy ( orig, path, sizeof ( orig ) );

    char expected[512];
    snprintf ( expected, sizeof ( expected ), "%s/FILE~2", dirdot );

    int r = mzdisk_config_resolve_export_dup ( path, sizeof ( path ), MZDSK_EXPORT_DUP_RENAME );

    remove ( orig );
    rmdir ( dirdot );

    TEST_ASSERT_EQ_INT ( r, 0 );
    TEST_ASSERT_EQ_STR ( path, expected );
    return 0;
}


/** @brief RENAME: malý buffer nepobere ~N suffix → -1. */
static int test_resolve_dup_rename_buffer_overflow ( void )
{
    const char *tmpdir = make_tmpdir();
    if ( !tmpdir ) TEST_FAIL ( "mkdtemp failed" );

    char real_path[512];
    snprintf ( real_path, sizeof ( real_path ), "%s/F.X", tmpdir );
    touch_file ( real_path );

    /* buffer je o jeden znak větší než původní cesta - "F.X" má délku
     * (tmpdir + "/F.X"), takže buffer by musel být o ~3 znaky větší,
     * aby se vešlo "F~2.X". Použijeme buffer přesně delku original+2. */
    int orig_len = (int) strlen ( real_path );
    int buf_size = orig_len + 2;    /* max "F~2.X" bez '\0' se nevejde */

    char path[512];
    strncpy ( path, real_path, sizeof ( path ) );

    int r = mzdisk_config_resolve_export_dup ( path, buf_size, MZDSK_EXPORT_DUP_RENAME );

    remove ( real_path );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( r, -1 );
    return 0;
}


/** @brief RENAME pro neexistující soubor: funkce vrátí 0 i bez kolize. */
static int test_resolve_dup_rename_nonexistent ( void )
{
    const char *tmpdir = make_tmpdir();
    if ( !tmpdir ) TEST_FAIL ( "mkdtemp failed" );

    char path[512];
    snprintf ( path, sizeof ( path ), "%s/nope.txt", tmpdir );

    char saved[512];
    strncpy ( saved, path, sizeof ( saved ) );

    int r = mzdisk_config_resolve_export_dup ( path, sizeof ( path ), MZDSK_EXPORT_DUP_RENAME );
    rmdir ( tmpdir );

    TEST_ASSERT_EQ_INT ( r, 0 );
    TEST_ASSERT_EQ_STR ( path, saved );
    return 0;
}


/* === Spuštění === */

int main ( void )
{
    TEST_INIT();

    RUN_TEST ( test_config_init_defaults );
    RUN_TEST ( test_config_add_recent_basic );
    RUN_TEST ( test_config_add_recent_pushes_front );
    RUN_TEST ( test_config_add_recent_dedup );
    RUN_TEST ( test_config_add_recent_already_first );
    RUN_TEST ( test_config_add_recent_overflow );
    RUN_TEST ( test_config_clear_recent );
    RUN_TEST ( test_config_save_load_roundtrip );
    RUN_TEST ( test_watch_init_on_missing_file );
    RUN_TEST ( test_watch_detects_external_change );
    RUN_TEST ( test_watch_ignores_own_save );
    RUN_TEST ( test_watch_throttle );
    RUN_TEST ( test_atomic_save_no_partial );
    RUN_TEST ( test_config_add_recent_null_empty );
    RUN_TEST ( test_config_add_recent_long_path );
    RUN_TEST ( test_config_clear_recent_null );

    RUN_TEST ( test_resolve_dup_nonexistent );
    RUN_TEST ( test_resolve_dup_overwrite );
    RUN_TEST ( test_resolve_dup_skip );
    RUN_TEST ( test_resolve_dup_rename_basic );
    RUN_TEST ( test_resolve_dup_rename_multiple );
    RUN_TEST ( test_resolve_dup_rename_no_ext );
    RUN_TEST ( test_resolve_dup_rename_dot_in_dir );
    RUN_TEST ( test_resolve_dup_rename_buffer_overflow );
    RUN_TEST ( test_resolve_dup_rename_nonexistent );

    TEST_SUMMARY();
}
