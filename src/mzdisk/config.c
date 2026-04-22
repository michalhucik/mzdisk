/**
 * @file config.c
 * @brief Implementace konfigurace - čtení/zápis mzdisk.ini.
 *
 * Jednoduchý INI parser ve formátu [sekce] + klíč=hodnota.
 * Nepoužívá externí knihovnu.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "panels/panel_settings.h"


/** @brief Klíče pro tab visibility v INI souboru (odpovídá en_MZDISK_TAB). */
static const char *s_tab_keys[MZDISK_TAB_COUNT] = {
    "tab_info",
    "tab_geometry",
    "tab_geometry_edit",
    "tab_boot_sector",
    "tab_block_map",
    "tab_hexdump",
    "tab_fs_dir",
    "tab_fs_maint",
};


void mzdisk_config_init ( st_MZDISK_CONFIG *cfg )
{
    if ( cfg == NULL ) return;  /* audit L-25: defenzivní NULL kontrola */

    strncpy ( cfg->last_open_dir, ".", MZDISK_CONFIG_PATH_MAX );
    strncpy ( cfg->last_get_dir, ".", MZDISK_CONFIG_PATH_MAX );
    strncpy ( cfg->last_put_dir, ".", MZDISK_CONFIG_PATH_MAX );
    strncpy ( cfg->last_create_dir, ".", MZDISK_CONFIG_PATH_MAX );

    /* výchozí: všechny taby viditelné */
    for ( int i = 0; i < MZDISK_TAB_COUNT; i++ ) {
        cfg->tab_visible[i] = true;
    }

    /* výchozí: prázdný seznam posledních souborů */
    cfg->recent_count = 0;
    for ( int i = 0; i < MZDISK_CONFIG_RECENT_MAX; i++ ) {
        cfg->recent_files[i][0] = '\0';
    }

    /* výchozí nastavení Settings */
    strncpy ( cfg->language, "auto", MZDISK_CONFIG_LANG_MAX );
    cfg->font_family_idx = 0;
    cfg->font_size = MZDISK_CONFIG_DEFAULT_FONT_SIZE;
    cfg->theme_idx = 0;

    /* výchozí kódování jmen: EU UTF-8 */
    cfg->boot_name_charset = MZDSK_NAME_CHARSET_EU_UTF8;
    cfg->fsmz_name_charset = MZDSK_NAME_CHARSET_EU_UTF8;

    /* výchozí režim duplicit: dotázat se uživatele */
    cfg->export_dup_mode = MZDSK_EXPORT_DUP_ASK;

    /* DnD má vlastní dup_mode (výchozí Ask) - odděleno od Export
     * protože kontext je jiný (DnD mezi FS sessions vs. Export do hostu). */
    cfg->dnd_dup_mode = MZDSK_EXPORT_DUP_ASK;

    /* výchozí šířky splitterů */
    cfg->create_splitter_w = 220.0f;
    cfg->filebrowser_splitter_w = 200.0f;

    /* multi-window: ve výchozím stavu nahrazujeme disk v aktuálním okně
     * (klasické chování) */
    cfg->open_in_new_window = false;
}


/**
 * @brief Odstraní koncový whitespace z řetězce (in-place).
 *
 * @param s Řetězec k oříznutí.
 */
static void trim_trailing ( char *s )
{
    int len = (int) strlen ( s );
    while ( len > 0 && ( s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t' ) ) {
        s[--len] = '\0';
    }
}


bool mzdisk_config_load ( st_MZDISK_CONFIG *cfg )
{
    FILE *f = fopen ( MZDISK_CONFIG_FILENAME, "r" );
    if ( !f ) return false;

    char line[MZDISK_CONFIG_PATH_MAX + 64];
    char current_section[64] = "";
    int recent_idx = 0;

    while ( fgets ( line, sizeof ( line ), f ) ) {
        trim_trailing ( line );

        /* přeskočit prázdné řádky a komentáře */
        if ( line[0] == '\0' || line[0] == '#' ) continue;

        /* detekce sekce */
        if ( line[0] == '[' ) {
            char *end = strchr ( line, ']' );
            if ( end ) {
                *end = '\0';
                strncpy ( current_section, line + 1, sizeof ( current_section ) - 1 );
                current_section[sizeof ( current_section ) - 1] = '\0';
            }
            continue;
        }

        char *eq = strchr ( line, '=' );
        if ( !eq ) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        /* sekce [recent] - klíče recent_0 .. recent_9 */
        if ( strcmp ( current_section, "recent" ) == 0 ) {
            if ( strncmp ( key, "recent_", 7 ) == 0 && val[0] != '\0' ) {
                if ( recent_idx < MZDISK_CONFIG_RECENT_MAX ) {
                    strncpy ( cfg->recent_files[recent_idx], val, MZDISK_CONFIG_PATH_MAX - 1 );
                    cfg->recent_files[recent_idx][MZDISK_CONFIG_PATH_MAX - 1] = '\0';
                    recent_idx++;
                }
            }
            continue;
        }

        /* ostatní klíče (paths, tabs, settings) */
        if ( strcmp ( key, "last_open_dir" ) == 0 ) {
            strncpy ( cfg->last_open_dir, val, MZDISK_CONFIG_PATH_MAX - 1 );
            cfg->last_open_dir[MZDISK_CONFIG_PATH_MAX - 1] = '\0';
        } else if ( strcmp ( key, "last_get_dir" ) == 0 ) {
            strncpy ( cfg->last_get_dir, val, MZDISK_CONFIG_PATH_MAX - 1 );
            cfg->last_get_dir[MZDISK_CONFIG_PATH_MAX - 1] = '\0';
        } else if ( strcmp ( key, "last_put_dir" ) == 0 ) {
            strncpy ( cfg->last_put_dir, val, MZDISK_CONFIG_PATH_MAX - 1 );
            cfg->last_put_dir[MZDISK_CONFIG_PATH_MAX - 1] = '\0';
        } else if ( strcmp ( key, "last_create_dir" ) == 0 ) {
            strncpy ( cfg->last_create_dir, val, MZDISK_CONFIG_PATH_MAX - 1 );
            cfg->last_create_dir[MZDISK_CONFIG_PATH_MAX - 1] = '\0';
        } else if ( strcmp ( key, "language" ) == 0 ) {
            strncpy ( cfg->language, val, MZDISK_CONFIG_LANG_MAX - 1 );
            cfg->language[MZDISK_CONFIG_LANG_MAX - 1] = '\0';
        } else if ( strcmp ( key, "font_family" ) == 0 ) {
            /* Audit M-32: clamp do platného rozsahu; ručně upravené INI
             * s font_family=99 by vedlo na OOB v s_font_file_paths[]. */
            int v = atoi ( val );
            if ( v >= 0 && v < PANEL_SETTINGS_FONT_COUNT ) cfg->font_family_idx = v;
        } else if ( strcmp ( key, "font_size" ) == 0 ) {
            int v = atoi ( val );
            if ( v >= PANEL_SETTINGS_FONT_SIZE_MIN && v <= PANEL_SETTINGS_FONT_SIZE_MAX )
                cfg->font_size = v;
        } else if ( strcmp ( key, "theme" ) == 0 ) {
            int v = atoi ( val );
            if ( v >= 0 && v < PANEL_SETTINGS_THEME_COUNT ) cfg->theme_idx = v;
        } else if ( strcmp ( key, "boot_name_charset" ) == 0 ) {
            int v = atoi ( val );
            if ( v >= 0 && v < MZDSK_NAME_CHARSET_COUNT ) cfg->boot_name_charset = v;
        } else if ( strcmp ( key, "fsmz_name_charset" ) == 0 ) {
            int v = atoi ( val );
            if ( v >= 0 && v < MZDSK_NAME_CHARSET_COUNT ) cfg->fsmz_name_charset = v;
        } else if ( strcmp ( key, "export_dup_mode" ) == 0 ) {
            int v = atoi ( val );
            if ( v >= 0 && v < MZDSK_EXPORT_DUP_COUNT ) cfg->export_dup_mode = v;
        } else if ( strcmp ( key, "dnd_dup_mode" ) == 0 ) {
            int v = atoi ( val );
            if ( v >= 0 && v < MZDSK_EXPORT_DUP_COUNT ) cfg->dnd_dup_mode = v;
        } else if ( strcmp ( key, "create_splitter_w" ) == 0 ) {
            /* Sjednocený min 10.0f pro splitter (audit L-18). Kontrola
             * isfinite odmítne NaN/Inf vstup z poškozeného INI. */
            float v = (float) atof ( val );
            if ( isfinite ( v ) && v >= 10.0f && v <= 2000.0f ) cfg->create_splitter_w = v;
        } else if ( strcmp ( key, "filebrowser_splitter_w" ) == 0 ) {
            float v = (float) atof ( val );
            if ( isfinite ( v ) && v >= 10.0f && v <= 2000.0f ) cfg->filebrowser_splitter_w = v;
        } else if ( strcmp ( key, "open_in_new_window" ) == 0 ) {
            cfg->open_in_new_window = ( atoi ( val ) != 0 );
        } else {
            /* tab visibility klíče */
            for ( int i = 0; i < MZDISK_TAB_COUNT; i++ ) {
                if ( strcmp ( key, s_tab_keys[i] ) == 0 ) {
                    cfg->tab_visible[i] = ( atoi ( val ) != 0 );
                    break;
                }
            }
        }
    }

    cfg->recent_count = recent_idx;

    fclose ( f );
    return true;
}


bool mzdisk_config_save ( const st_MZDISK_CONFIG *cfg )
{
    /* Atomický zápis: nejprve do dočasného souboru s PID suffixem,
     * pak rename() na cílové jméno. PID suffix zabrání kolizi, pokud
     * současně zapisuje více instancí mzdisk. Čtenář (watch poll
     * v jiné instanci) vidí vždy buď starý nebo nový soubor, nikdy
     * částečně zapsaný. */
    char tmp[MZDISK_CONFIG_PATH_MAX + 32];
    snprintf ( tmp, sizeof ( tmp ), "%s.tmp.%d",
               MZDISK_CONFIG_FILENAME, (int) getpid () );

    FILE *f = fopen ( tmp, "w" );
    if ( !f ) return false;

    fprintf ( f, "[paths]\n" );
    fprintf ( f, "last_open_dir=%s\n", cfg->last_open_dir );
    fprintf ( f, "last_get_dir=%s\n", cfg->last_get_dir );
    fprintf ( f, "last_put_dir=%s\n", cfg->last_put_dir );
    fprintf ( f, "last_create_dir=%s\n", cfg->last_create_dir );
    fprintf ( f, "\n[tabs]\n" );
    for ( int i = 0; i < MZDISK_TAB_COUNT; i++ ) {
        fprintf ( f, "%s=%d\n", s_tab_keys[i], cfg->tab_visible[i] ? 1 : 0 );
    }

    /* sekce recent - uložit jen neprázdné položky */
    if ( cfg->recent_count > 0 ) {
        fprintf ( f, "\n[recent]\n" );
        for ( int i = 0; i < cfg->recent_count; i++ ) {
            fprintf ( f, "recent_%d=%s\n", i, cfg->recent_files[i] );
        }
    }

    fprintf ( f, "\n[settings]\n" );
    fprintf ( f, "language=%s\n", cfg->language );
    fprintf ( f, "font_family=%d\n", cfg->font_family_idx );
    fprintf ( f, "font_size=%d\n", cfg->font_size );
    fprintf ( f, "theme=%d\n", cfg->theme_idx );
    fprintf ( f, "boot_name_charset=%d\n", cfg->boot_name_charset );
    fprintf ( f, "fsmz_name_charset=%d\n", cfg->fsmz_name_charset );
    fprintf ( f, "export_dup_mode=%d\n", cfg->export_dup_mode );
    fprintf ( f, "dnd_dup_mode=%d\n", cfg->dnd_dup_mode );

    fprintf ( f, "\n[layout]\n" );
    fprintf ( f, "create_splitter_w=%.0f\n", cfg->create_splitter_w );
    fprintf ( f, "filebrowser_splitter_w=%.0f\n", cfg->filebrowser_splitter_w );

    fprintf ( f, "\n[window]\n" );
    fprintf ( f, "open_in_new_window=%d\n", cfg->open_in_new_window ? 1 : 0 );

    if ( fclose ( f ) != 0 ) {
        remove ( tmp );
        return false;
    }

    /* POSIX rename() cíl atomicky nahradí. MSVCRT rename() selže,
     * pokud cíl existuje - fallback přes remove() + rename(). Malé
     * okno mezi remove a rename, kdy soubor neexistuje, je přijatelné:
     * čtenář v druhé instanci detekuje změnu v dalším pollu. */
    if ( rename ( tmp, MZDISK_CONFIG_FILENAME ) != 0 ) {
        remove ( MZDISK_CONFIG_FILENAME );
        if ( rename ( tmp, MZDISK_CONFIG_FILENAME ) != 0 ) {
            remove ( tmp );
            return false;
        }
    }
    return true;
}


void mzdisk_config_add_recent ( st_MZDISK_CONFIG *cfg, const char *filepath )
{
    if ( !cfg || !filepath || filepath[0] == '\0' ) return;

    /* zjistit, zda cesta už v seznamu je */
    int existing = -1;
    for ( int i = 0; i < cfg->recent_count; i++ ) {
        if ( strcmp ( cfg->recent_files[i], filepath ) == 0 ) {
            existing = i;
            break;
        }
    }

    if ( existing == 0 ) {
        /* už je na prvním místě - nic nedělat */
        return;
    }

    if ( existing > 0 ) {
        /* posunout položky 0..existing-1 o jednu dolů.
         * Audit M-41: strncpy nezajišťuje NUL terminaci pokud zdroj
         * přesně vyplní buffer. Explicitní NUL na posledním bajtu. */
        char tmp[MZDISK_CONFIG_PATH_MAX];
        strncpy ( tmp, cfg->recent_files[existing], MZDISK_CONFIG_PATH_MAX - 1 );
        tmp[MZDISK_CONFIG_PATH_MAX - 1] = '\0';
        for ( int i = existing; i > 0; i-- ) {
            strncpy ( cfg->recent_files[i], cfg->recent_files[i - 1], MZDISK_CONFIG_PATH_MAX - 1 );
            cfg->recent_files[i][MZDISK_CONFIG_PATH_MAX - 1] = '\0';
        }
        strncpy ( cfg->recent_files[0], tmp, MZDISK_CONFIG_PATH_MAX - 1 );
        cfg->recent_files[0][MZDISK_CONFIG_PATH_MAX - 1] = '\0';
    } else {
        /* nová položka - posunout vše o jednu dolů */
        int count = cfg->recent_count;
        if ( count >= MZDISK_CONFIG_RECENT_MAX ) {
            count = MZDISK_CONFIG_RECENT_MAX - 1;
        }
        for ( int i = count; i > 0; i-- ) {
            strncpy ( cfg->recent_files[i], cfg->recent_files[i - 1], MZDISK_CONFIG_PATH_MAX );
        }
        strncpy ( cfg->recent_files[0], filepath, MZDISK_CONFIG_PATH_MAX - 1 );
        cfg->recent_files[0][MZDISK_CONFIG_PATH_MAX - 1] = '\0';
        if ( cfg->recent_count < MZDISK_CONFIG_RECENT_MAX ) {
            cfg->recent_count++;
        }
    }
}


void mzdisk_config_clear_recent ( st_MZDISK_CONFIG *cfg )
{
    if ( !cfg ) return;
    cfg->recent_count = 0;
    for ( int i = 0; i < MZDISK_CONFIG_RECENT_MAX; i++ ) {
        cfg->recent_files[i][0] = '\0';
    }
}


/**
 * @brief Načte signaturu (mtime + size) zadaného souboru.
 *
 * Pokud soubor neexistuje nebo jej nelze zjistit přes `stat()`,
 * výstupní parametry se nastaví na hodnoty indikující "neznámo"
 * (size = -1, mtime = 0).
 *
 * @param[in]  path      Cesta k souboru.
 * @param[out] mtime_sec Výstup: sekundy mtime, 0 pokud neexistuje.
 * @param[out] mtime_nsec Výstup: nanosekundy mtime (0 na FS bez podpory).
 * @param[out] size      Výstup: velikost v bajtech, -1 pokud neexistuje.
 */
static void config_stat_file ( const char *path, long long *mtime_sec,
                                long long *mtime_nsec, long long *size )
{
    struct stat st;
    if ( stat ( path, &st ) != 0 ) {
        *mtime_sec = 0;
        *mtime_nsec = 0;
        *size = -1;
        return;
    }
    *mtime_sec = (long long) st.st_mtime;
#if defined(__linux__) || defined(__APPLE__)
    /* struct timespec st_mtim dostupný na Linuxu. */
    *mtime_nsec = (long long) st.st_mtim.tv_nsec;
#else
    /* MinGW/MSYS2: stat nemá nanosekundovou složku - nulujeme. */
    *mtime_nsec = 0;
#endif
    *size = (long long) st.st_size;
}


void mzdisk_config_watch_init ( st_MZDISK_CONFIG_WATCH *w, const char *ini_path )
{
    if ( !w || !ini_path ) return;
    strncpy ( w->path, ini_path, MZDISK_CONFIG_PATH_MAX - 1 );
    w->path[MZDISK_CONFIG_PATH_MAX - 1] = '\0';
    w->last_mtime_sec = 0;
    w->last_mtime_nsec = 0;
    w->last_size = -1;
    w->last_check_ms = 0;
    w->poll_interval_ms = 500;
}


void mzdisk_config_watch_mark_saved ( st_MZDISK_CONFIG_WATCH *w )
{
    if ( !w ) return;
    config_stat_file ( w->path, &w->last_mtime_sec, &w->last_mtime_nsec, &w->last_size );
}


bool mzdisk_config_watch_poll ( st_MZDISK_CONFIG_WATCH *w, unsigned now_ms )
{
    if ( !w ) return false;

    /* throttling - vyhnout se stat() při každém frame.
     * Rozdíl (now_ms - last_check_ms) používá unsigned wraparound
     * korektně i při přetečení 32-bit SDL_GetTicks() počítadla. */
    if ( w->last_check_ms != 0 &&
         ( now_ms - w->last_check_ms ) < w->poll_interval_ms ) {
        return false;
    }
    w->last_check_ms = now_ms;

    long long cur_mtime_sec, cur_mtime_nsec, cur_size;
    config_stat_file ( w->path, &cur_mtime_sec, &cur_mtime_nsec, &cur_size );

    /* soubor neexistuje - žádná změna k hlášení */
    if ( cur_size < 0 ) return false;

    /* baseline ještě neznámá - zachytit a neohlašovat změnu */
    if ( w->last_size < 0 ) {
        w->last_mtime_sec = cur_mtime_sec;
        w->last_mtime_nsec = cur_mtime_nsec;
        w->last_size = cur_size;
        return false;
    }

    /* porovnání podpisu - změna v mtime nebo velikosti */
    if ( cur_mtime_sec != w->last_mtime_sec ||
         cur_mtime_nsec != w->last_mtime_nsec ||
         cur_size != w->last_size ) {
        w->last_mtime_sec = cur_mtime_sec;
        w->last_mtime_nsec = cur_mtime_nsec;
        w->last_size = cur_size;
        return true;
    }
    return false;
}


/**
 * @brief Ověří existenci souboru na hostitelském FS.
 *
 * @param[in] path  Cesta k souboru. Nesmí být NULL.
 * @return 1 pokud soubor existuje, 0 pokud ne.
 */
static int config_file_exists ( const char *path )
{
    FILE *f = fopen ( path, "rb" );
    if ( f ) { fclose ( f ); return 1; }
    return 0;
}


int mzdisk_config_resolve_export_dup ( char *path, int path_size, int dup_mode )
{
    if ( !config_file_exists ( path ) ) return 0;

    switch ( dup_mode ) {
        case MZDSK_EXPORT_DUP_OVERWRITE:
            return 0;
        case MZDSK_EXPORT_DUP_SKIP:
            return 1;
        case MZDSK_EXPORT_DUP_RENAME: {
            /* Najít suffix ~N, který nevede ke kolizi */
            char orig[MZDISK_CONFIG_PATH_MAX];
            strncpy ( orig, path, sizeof ( orig ) - 1 );
            orig[sizeof ( orig ) - 1] = '\0';

            /* Najít pozici poslední tečky (přípona) a posledního '/' (adresář) */
            const char *last_slash = strrchr ( orig, '/' );
            const char *last_bslash = strrchr ( orig, '\\' );
            if ( last_bslash && ( !last_slash || last_bslash > last_slash ) ) last_slash = last_bslash;

            const char *last_dot = strrchr ( orig, '.' );
            /* Tečka musí být za posledním separátorem (jinak je v adresáři) */
            if ( last_dot && last_slash && last_dot < last_slash ) last_dot = NULL;

            for ( int n = 2; n < 1000; n++ ) {
                if ( last_dot ) {
                    /* NAME.EXT -> NAME~N.EXT */
                    int prefix_len = (int) ( last_dot - orig );
                    if ( snprintf ( path, path_size, "%.*s~%d%s",
                                    prefix_len, orig, n, last_dot ) >= path_size ) return -1;
                } else {
                    /* NAME -> NAME~N */
                    if ( snprintf ( path, path_size, "%s~%d", orig, n ) >= path_size ) return -1;
                }
                if ( !config_file_exists ( path ) ) return 0;
            }
            return -1;
        }
        default:
            return 0;
    }
}
