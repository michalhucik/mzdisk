/**
 * @file   mzdsk_mrs.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  CLI nástroj pro práci se souborovým systémem MRS (Memory Resident System).
 *
 * Podporuje kompletní operace nad MRS souborovým systémem:
 * - Výpis informací o FS (info)
 * - Výpis adresáře (dir)
 * - Hexdump FAT tabulky (fat)
 * - Informace o souboru (file, file --id)
 * - Extrakce souboru (get, get --mzf, get --all)
 * - Vložení souboru (put, put --mzf)
 * - Smazání souboru (era)
 * - Přejmenování souboru (ren)
 * - Surový přístup k blokům (dump-block, get-block, put-block)
 * - Vytvoření prázdné FAT a adresáře (init)
 *
 * Globální volby:
 * - --fat-block N    - číslo bloku, kde začíná FAT (výchozí 36)
 * - --ro             - vynucený read-only režim
 * - --output FMT     - výstupní formát: text (výchozí), json, csv
 *
 * MRS je vývojové prostředí Vlastimila Veselého pro Sharp MZ-800
 * (editor, assembler, linker). Disketa používá vlastní souborový
 * systém s FAT tabulkou, kde jeden bajt = jeden alokační blok (512 B).
 *
 * Knihovna fsmrs_init() detekuje rozložení FAT a adresáře z obsahu
 * FAT markerů. Pro disketu DISK41_MRS.DSK a podobné formáty lze
 * použít fat_block = 36 (typická pozice FAT na 720K MRS disketě).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#else
#define _mkdir(path) mkdir ( (path), 0755 )
#endif

#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/mzdsk_hexdump/mzdsk_hexdump.h"
#include "libs/output_format/output_format.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "tools/common/mzdisk_cli_version.h"


/** @brief Verze nástroje mzdsk-mrs. */
#define MZDSK_MRS_TOOL_VERSION "2.12.0"


/** @brief Globální flag `--overwrite` (audit M-16). */
static int g_allow_overwrite = 0;


/** @brief Globální flag `--yes` (audit M-17). */
static int g_assume_yes = 0;


/**
 * @brief Interaktivní potvrzení destruktivní operace (audit M-17).
 *
 * @param op_name  Popis operace.
 * @return 1 pokud potvrzeno nebo --yes/non-TTY, 0 pokud odmítnuto.
 */
static int confirm_destructive_op ( const char *op_name ) {
    if ( g_assume_yes ) return 1;
    if ( !isatty ( fileno ( stdin ) ) ) return 1;
    fprintf ( stderr, "This will %s the disk. Are you sure? [y/N] ", op_name );
    fflush ( stderr );
    char buf[16];
    if ( fgets ( buf, sizeof ( buf ), stdin ) == NULL ) return 0;
    return ( buf[0] == 'y' || buf[0] == 'Y' );
}


/**
 * @brief Zkontroluje, zda lze bezpečně zapsat na výstupní cestu (audit M-16).
 *
 * @param path Cesta k výstupnímu souboru.
 * @return EXIT_SUCCESS pokud lze zapsat, EXIT_FAILURE pokud by došlo k přepisu.
 */
static int check_output_overwrite ( const char *path ) {
    if ( path == NULL || path[0] == '\0' ) return EXIT_SUCCESS;
    if ( g_allow_overwrite ) return EXIT_SUCCESS;
    if ( access ( path, F_OK ) == 0 ) {
        fprintf ( stderr,
                  "Error: Output file '%s' already exists. Use --overwrite to replace it.\n",
                  path );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/** @brief Výchozí číslo bloku, kde začíná FAT na 720K MRS disketě. */
#define FSMRS_DEFAULT_FAT_BLOCK 36

/** @brief Maximální délka výstupní cesty pro export.
 *  Sjednoceno s mzdsk-fsmz a mzdsk-cpm (audit L-13). */
#define MAX_PATH_LEN 1024


/**
 * @brief Režim řešení duplicitních jmen při dávkovém exportu.
 */
typedef enum en_DUPLICATE_MODE {
    DUP_RENAME    = 0,    /**< Přidá suffix ~N před příponu (výchozí, bezpečné) */
    DUP_OVERWRITE = 1,    /**< Přepíše existující soubor bez varování */
    DUP_SKIP      = 2,    /**< Přeskočí soubor s varováním na stderr */
} en_DUPLICATE_MODE;


/* =========================================================================
 * Pomocné funkce
 * ========================================================================= */

/**
 * @brief Vytiskne jméno souboru z adresářové položky.
 *
 * Vytiskne prvních 8 bajtů jména. Pokud je znak menší než 0x20
 * (nepoužitelné/padding), vytiskne se jako tečka.
 *
 * @param[in] name  Ukazatel na pole jména (8 bajtů). Nesmí být NULL.
 */
static void print_fname ( const uint8_t *name ) {
    for ( int i = 0; i < 8; i++ ) {
        uint8_t c = name[i];
        putchar ( ( c >= 0x20 && c < 0x7f ) ? c : '.' );
    }
}


/**
 * @brief Vytiskne příponu souboru z adresářové položky.
 *
 * @param[in] ext  Ukazatel na pole přípony (3 bajty). Nesmí být NULL.
 */
static void print_ext ( const uint8_t *ext ) {
    for ( int i = 0; i < 3; i++ ) {
        uint8_t c = ext[i];
        putchar ( ( c >= 0x20 && c < 0x7f ) ? c : '.' );
    }
}


/**
 * @brief Vytiskne jméno souboru ve formátu "fname.ext" (bez trailing mezer).
 *
 * @param[in] fname  Ukazatel na pole jména (8 bajtů). Nesmí být NULL.
 * @param[in] ext    Ukazatel na pole přípony (3 bajty). Nesmí být NULL.
 */
static void print_fname_dot_ext ( const uint8_t *fname, const uint8_t *ext ) {
    /* Zjisti délku jména bez trailing mezer */
    int name_len = 8;
    while ( name_len > 0 && fname[name_len - 1] == 0x20 ) name_len--;
    for ( int i = 0; i < name_len; i++ ) {
        putchar ( ( fname[i] >= 0x20 && fname[i] < 0x7f ) ? fname[i] : '.' );
    }

    /* Zjisti délku přípony bez trailing mezer */
    int ext_len = 3;
    while ( ext_len > 0 && ext[ext_len - 1] == 0x20 ) ext_len--;
    if ( ext_len > 0 ) {
        putchar ( '.' );
        for ( int i = 0; i < ext_len; i++ ) {
            putchar ( ( ext[i] >= 0x20 && ext[i] < 0x7f ) ? ext[i] : '.' );
        }
    }
}


/**
 * @brief Sestaví ASCII řetězec "fname.ext" z MRS adresářové položky.
 *
 * Trimuje trailing mezery ze jména i přípony. Výsledek je null-terminated.
 *
 * @param[in]  fname  Pole jména (8 bajtů). Nesmí být NULL.
 * @param[in]  ext    Pole přípony (3 bajty). Nesmí být NULL.
 * @param[out] buf    Výstupní buffer (min. 13 bajtů: 8 + '.' + 3 + '\0'). Nesmí být NULL.
 */
static void compose_ascii_name ( const uint8_t *fname, const uint8_t *ext, char *buf ) {
    int pos = 0;
    int nlen = 8;
    while ( nlen > 0 && fname[nlen - 1] == 0x20 ) nlen--;
    for ( int i = 0; i < nlen; i++ ) buf[pos++] = (char) fname[i];

    int elen = 3;
    while ( elen > 0 && ext[elen - 1] == 0x20 ) elen--;
    if ( elen > 0 ) {
        buf[pos++] = '.';
        for ( int i = 0; i < elen; i++ ) buf[pos++] = (char) ext[i];
    }
    buf[pos] = '\0';
}


/**
 * @brief Sanitizuje jméno souboru pro hostitelský souborový systém.
 *
 * Nahradí znaky nepovolené ve Windows jménech souborů podtržítkem.
 * Nahrazované znaky: * ? " < > | : \ /
 *
 * @param[in,out] name  Null-terminated řetězec k sanitizaci. Nesmí být NULL.
 */
static void sanitize_filename ( char *name ) {
    for ( char *p = name; *p; p++ ) {
        if ( *p == '*' || *p == '?' || *p == '"' || *p == '<' ||
             *p == '>' || *p == '|' || *p == ':' || *p == '\\' || *p == '/' ) {
            *p = '_';
        }
    }
    /* Ochrana proti path traversal (audit M-25): jméno tvořené jen
     * tečkami a mezerami (".", "..", "...", " .", " .. ") by vedlo
     * k cestě `dir/..` a zápisu do parent directory. Sanitizace
     * path separators výše toto sama nezastaví. */
    int only_dots_spaces = ( name[0] != '\0' );
    for ( char *p = name; *p; p++ ) {
        if ( *p != '.' && *p != ' ' ) { only_dots_spaces = 0; break; }
    }
    if ( only_dots_spaces && name[0] != '\0' ) name[0] = '_';
}


/**
 * @brief Ověří existenci souboru na hostitelském FS.
 *
 * @param[in] path  Cesta k souboru. Nesmí být NULL.
 * @return 1 pokud soubor existuje, 0 pokud ne.
 */
static int file_exists_on_host ( const char *path ) {
    FILE *f = fopen ( path, "rb" );
    if ( f ) { fclose ( f ); return 1; }
    return 0;
}


/**
 * @brief Sestaví výstupní cestu pro export MRS souboru s řešením duplicit.
 *
 * Vytvoří cestu ve formátu dir/NAME.EXT (raw) nebo dir/NAME.EXT.mzf (MZF).
 * Pokud soubor na cílové cestě již existuje, podle dup_mode buď přidá
 * suffix ~N (DUP_RENAME), přepíše (DUP_OVERWRITE), nebo přeskočí (DUP_SKIP).
 *
 * @param[out] path_buf       Výstupní buffer pro cestu. Nesmí být NULL.
 * @param[in]  path_buf_size  Velikost bufferu.
 * @param[in]  dir            Cílový adresář. Nesmí být NULL.
 * @param[in]  safe_name      Sanitizované jméno souboru (bez přípony, null-terminated). Nesmí být NULL.
 * @param[in]  safe_ext       Sanitizovaná přípona (null-terminated, může být prázdný řetězec). Nesmí být NULL.
 * @param[in]  is_mzf         1 = výstup je MZF (přidá .mzf suffix), 0 = raw.
 * @param[in]  dup_mode       Režim řešení duplicit.
 *
 * @return 0 = cesta připravena v path_buf, soubor se má extrahovat.
 * @return 1 = soubor se má přeskočit (DUP_SKIP a soubor existuje, nebo
 *             příliš mnoho duplicit při DUP_RENAME).
 * @return -1 = chyba (buffer overflow).
 */
static int build_mrs_output_path ( char *path_buf, size_t path_buf_size,
                                   const char *dir,
                                   const char *safe_name, const char *safe_ext,
                                   int is_mzf, en_DUPLICATE_MODE dup_mode ) {

    /* Složení základní cesty */
    if ( is_mzf ) {
        if ( safe_ext[0] != '\0' ) {
            if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s.%s.mzf",
                                     dir, safe_name, safe_ext ) >= path_buf_size ) return -1;
        } else {
            if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s.mzf",
                                     dir, safe_name ) >= path_buf_size ) return -1;
        }
    } else {
        if ( safe_ext[0] != '\0' ) {
            if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s.%s",
                                     dir, safe_name, safe_ext ) >= path_buf_size ) return -1;
        } else {
            if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s",
                                     dir, safe_name ) >= path_buf_size ) return -1;
        }
    }

    /* Kontrola duplicity */
    if ( !file_exists_on_host ( path_buf ) ) return 0;

    switch ( dup_mode ) {
        case DUP_OVERWRITE:
            return 0;
        case DUP_SKIP:
            fprintf ( stderr, "Warning: Skipping '%s' (file exists)\n", path_buf );
            return 1;
        case DUP_RENAME: {
            for ( int n = 2; n < 1000; n++ ) {
                if ( is_mzf ) {
                    if ( safe_ext[0] != '\0' ) {
                        if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s~%d.%s.mzf",
                                                 dir, safe_name, n, safe_ext ) >= path_buf_size ) return -1;
                    } else {
                        if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s~%d.mzf",
                                                 dir, safe_name, n ) >= path_buf_size ) return -1;
                    }
                } else {
                    if ( safe_ext[0] != '\0' ) {
                        if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s~%d.%s",
                                                 dir, safe_name, n, safe_ext ) >= path_buf_size ) return -1;
                    } else {
                        if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s~%d",
                                                 dir, safe_name, n ) >= path_buf_size ) return -1;
                    }
                }
                if ( !file_exists_on_host ( path_buf ) ) return 0;
            }
            fprintf ( stderr, "Warning: Skipping '%s' (too many duplicates)\n", safe_name );
            return 1;
        }
    }
    return 0;
}


/**
 * @brief Rozparsuje řetězec "name.ext" na MRS jméno (8 B) a příponu (3 B).
 *
 * Jméno i přípona se doplní mezerami (0x20) na plnou délku.
 * Pokud řetězec neobsahuje tečku, přípona zůstane prázdná (samé mezery).
 * Jméno i přípona se zkrátí na maximální délku (8 resp. 3 znaků).
 *
 * Jako oddělovač jména a přípony se použije poslední tečka v řetězci,
 * protože MRS jméno může obsahovat tečky (např. "kdos...DAT" → fname="kdos..",
 * ext="DAT"). Funkce compose_ascii_name() přidává oddělovací tečku za
 * trimnuté fname, takže poslední tečka je vždy oddělovač.
 *
 * @param[in]  input     Vstupní řetězec (null-terminated). Nesmí být NULL.
 * @param[out] fname     Výstupní pole jména (8 bajtů). Nesmí být NULL.
 * @param[out] ext       Výstupní pole přípony (3 bajty). Nesmí být NULL.
 * @param[out] has_ext   Výstupní příznak, zda vstup obsahoval tečku (1) nebo ne (0).
 *                       Může být NULL pokud informaci nepotřebujeme.
 * @param[out] truncated Výstupní příznak - nastavený na 1 pokud bylo jméno nebo
 *                       přípona zkrácena (vstup delší než 8/3 znaků).
 *                       Může být NULL.
 */
static void parse_mrs_filename ( const char *input, uint8_t *fname, uint8_t *ext,
                                  int *has_ext, int *truncated ) {
    memset ( fname, 0x20, 8 );
    memset ( ext, 0x20, 3 );
    int trunc = 0;

    const char *dot = strrchr ( input, '.' );
    if ( dot != NULL ) {
        size_t name_len = (size_t) ( dot - input );
        if ( name_len > 8 ) { name_len = 8; trunc = 1; }
        memcpy ( fname, input, name_len );

        size_t ext_len = strlen ( dot + 1 );
        if ( ext_len > 3 ) { ext_len = 3; trunc = 1; }
        memcpy ( ext, dot + 1, ext_len );

        if ( has_ext != NULL ) *has_ext = 1;
    } else {
        size_t name_len = strlen ( input );
        if ( name_len > 8 ) { name_len = 8; trunc = 1; }
        memcpy ( fname, input, name_len );

        if ( has_ext != NULL ) *has_ext = 0;
    }
    if ( truncated != NULL ) *truncated = trunc;
}


/**
 * @brief Zapíše do stderr warning o zkrácení jména v MRS.
 *
 * Vytvoří normalizovaný tvar "name.ext" ze skutečně uloženého 8.3 jména
 * a vypíše varování ve formátu
 * "Warning: Name 'ORIG' was truncated to 'name.ext'".
 *
 * Používá stejnou kompozici jako compose_ascii_name() - trimuje pouze
 * trailing mezery z fname i ext, ponechává interní mezery (MRS je fixní
 * 8.3 padding mezerami jen na konci, "FLAPPY v" je legální jméno).
 *
 * @param[in] orig   Původní jméno zadané uživatelem (null-terminated).
 * @param[in] fname  Zkrácené jméno (8 bajtů, padding mezerami).
 * @param[in] ext    Zkrácená přípona (3 bajty, padding mezerami).
 * @param[in] has_ext 1 pokud má výsledek příponu, 0 bez přípony.
 */
static void warn_mrs_truncation ( const char *orig, const uint8_t *fname,
                                   const uint8_t *ext, int has_ext ) {
    char composed[13];
    uint8_t empty_ext[3] = { 0x20, 0x20, 0x20 };
    compose_ascii_name ( fname, has_ext ? ext : empty_ext, composed );
    fprintf ( stderr, "Warning: Name '%s' was truncated to '%s'\n", orig, composed );
}


/**
 * @brief Přeloží en_MZDSK_NAMEVAL na stderr chybovou hlášku.
 *
 * Vypíše jednotný formát "Error: Name 'X' ..." odpovídající konkrétní
 * chybě validátoru. Použito v striktním režimu `--name` u put/put-mzf.
 *
 * @param[in] orig   Původní uživatelský vstup pro citaci v hlášce.
 * @param[in] code   Kód chyby vrácený z mzdsk_validate_83_name.
 * @param[in] bad    Při MZDSK_NAMEVAL_BAD_CHAR první zakázaný znak.
 *
 * @pre code != MZDSK_NAMEVAL_OK.
 */
static void report_strict_name_error ( const char *orig, en_MZDSK_NAMEVAL code, char bad ) {
    switch ( code ) {
        case MZDSK_NAMEVAL_EMPTY:
            fprintf ( stderr, "Error: Name '%s' has empty filename (no truncation with --name)\n", orig );
            break;
        case MZDSK_NAMEVAL_NAME_TOO_LONG:
        case MZDSK_NAMEVAL_EXT_TOO_LONG:
            fprintf ( stderr, "Error: Name '%s' exceeds 8.3 limit (no truncation with --name)\n", orig );
            break;
        case MZDSK_NAMEVAL_BAD_CHAR:
            fprintf ( stderr, "Error: Name '%s' contains forbidden character '%c' (no truncation with --name)\n",
                      orig, bad );
            break;
        default:
            fprintf ( stderr, "Error: Name '%s' is invalid\n", orig );
            break;
    }
}


/**
 * @brief Striktně validuje MRS jméno a naplní 8/3 pole paddovaná 0x20.
 *
 * Volá mzdsk_validate_83_name s flavor MRS. Výsledek (C-string name a ext)
 * zkopíruje do cílových 8 a 3 bajtových polí a zbytek doplní mezerami
 * (0x20), jak vyžaduje MRS adresářový záznam.
 *
 * @param[in]  input     Vstupní jméno ve formátu "NAME" nebo "NAME.EXT".
 * @param[out] fname     Výstupní pole 8 bajtů paddovaných 0x20.
 * @param[out] ext       Výstupní pole 3 bajtů paddovaných 0x20.
 * @param[out] has_ext   1 pokud vstup obsahoval tečku (pro warning), jinak 0.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE pokud validátor odmítl
 *         vstup - chybová hláška již byla vypsána na stderr.
 */
static int parse_mrs_filename_strict ( const char *input, uint8_t *fname,
                                        uint8_t *ext, int *has_ext ) {
    char name_str[9] = {0};
    char ext_str[4]  = {0};
    char bad = 0;

    en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( input,
        MZDSK_NAMEVAL_FLAVOR_MRS, name_str, ext_str, &bad );
    if ( r != MZDSK_NAMEVAL_OK ) {
        report_strict_name_error ( input, r, bad );
        return EXIT_FAILURE;
    }

    memset ( fname, 0x20, 8 );
    memset ( ext, 0x20, 3 );
    size_t name_len = strlen ( name_str );
    size_t ext_len  = strlen ( ext_str );
    memcpy ( fname, name_str, name_len );
    memcpy ( ext, ext_str, ext_len );
    if ( has_ext != NULL ) *has_ext = ( ext_len > 0 ) ? 1 : 0;
    return EXIT_SUCCESS;
}


/**
 * @brief Parsuje hexadecimální nebo dekadickou hodnotu z řetězce.
 *
 * Podporuje prefix "0x" pro hexadecimální zápis.
 *
 * @param[in]  str    Vstupní řetězec. Nesmí být NULL.
 * @param[out] value  Ukazatel na výstupní hodnotu. Nesmí být NULL.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při neplatném vstupu.
 */
static int parse_number ( const char *str, unsigned long *value ) {
    /* Audit M-15: odmítnout NULL, prázdný řetězec a záporné znaménko;
       strtoul(base=0) akceptuje '-1' → ULONG_MAX. */
    if ( str == NULL || *str == '\0' || str[0] == '-' ) return EXIT_FAILURE;
    char *endptr;
    errno = 0;
    *value = strtoul ( str, &endptr, 0 );
    if ( errno == ERANGE ) return EXIT_FAILURE;
    if ( endptr == str || *endptr != '\0' ) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Vyhledá soubor podle jména zadaného na příkazovém řádku.
 *
 * Rozparsuje "name.ext" a vyhledá v adresáři. Pokud vstup obsahuje
 * tečku, hledá se podle jména i přípony. Jinak jen podle jména.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci.
 * @param[in] name    Jméno souboru z příkazového řádku ("name.ext" nebo "name").
 *
 * @return Ukazatel na nalezenou položku, nebo NULL.
 */
static st_FSMRS_DIR_ITEM* find_file_by_name ( st_FSMRS_CONFIG *config, const char *name ) {
    uint8_t fname[8], ext[3];
    int has_ext = 0;
    parse_mrs_filename ( name, fname, ext, &has_ext, NULL );
    return fsmrs_search_file ( config, fname, has_ext ? ext : NULL );
}


/**
 * @brief Vytiskne informace o verzích použitých knihoven.
 */
static void print_lib_versions ( void ) {
    printf ( "mzdsk-mrs %s (%s %s)\n\n",
             MZDSK_MRS_TOOL_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
    printf ( "Library versions:\n" );
    printf ( "  mzdsk_mrs         %s\n", mzdsk_mrs_version() );
    printf ( "  mzdsk_hexdump     %s\n", mzdsk_hexdump_version() );
    printf ( "  output_format     %s\n", output_format_version() );
    printf ( "  mzdsk_global      %s\n", mzdsk_global_version() );
    printf ( "  generic_driver    %s\n", generic_driver_version() );
}


/**
 * @brief Vytiskne stručný návod k použití.
 *
 * @param[in] out Výstupní stream (stdout pro --help, stderr pro chybové cesty).
 */
static void print_usage ( FILE *out ) {
    fprintf ( out, "mzdsk-mrs %s - CLI tool for MRS filesystem on Sharp MZ-800\n\n", MZDSK_MRS_TOOL_VERSION );
    fprintf ( out, "Usage:\n" );
    fprintf ( out, "  mzdsk-mrs [options] <dsk_file> <command> [args...]\n\n" );
    fprintf ( out, "Global options:\n" );
    fprintf ( out, "  --version          Show tool version\n" );
    fprintf ( out, "  --lib-versions     Show library versions\n" );
    fprintf ( out, "  --fat-block N      Starting block of FAT (default: %d)\n", FSMRS_DEFAULT_FAT_BLOCK );
    fprintf ( out, "  --ro               Force read-only mode\n" );
    fprintf ( out, "  --overwrite        Allow 'get'/'get-block' to overwrite an existing output file\n" );
    fprintf ( out, "                     (default: refuse to overwrite)\n" );
    fprintf ( out, "  --yes, -y          Skip interactive confirmation prompt for 'init'/'defrag';\n" );
    fprintf ( out, "                     non-TTY (pipes, scripts) is always silent\n" );
    fprintf ( out, "  --output FMT       Output format: text (default), json, csv\n" );
    fprintf ( out, "  -o FMT             Same as --output\n\n" );
    fprintf ( out, "Commands:\n" );
    fprintf ( out, "  info                      Show filesystem information (FAT layout, usage)\n" );
    fprintf ( out, "  dir                       List directory contents\n" );
    fprintf ( out, "  dir --raw                 List all directory slots (including deleted/empty)\n" );
    fprintf ( out, "  fat                       Show FAT table contents (non-zero entries)\n" );
    fprintf ( out, "  get <name[.ext]> <file>   Extract file from disk (raw data)\n" );
    fprintf ( out, "  get --id N <file>         Extract file by ID (raw data)\n" );
    fprintf ( out, "  get --mzf <name[.ext]> <file>  Extract file as MZF (with header)\n" );
    fprintf ( out, "  get --mzf --id N <file>   Extract file by ID as MZF\n" );
    fprintf ( out, "  get --all <dir>           Export all files to directory (raw)\n" );
    fprintf ( out, "  get --all --mzf <dir>     Export all files to directory (MZF)\n" );
    fprintf ( out, "    --on-duplicate MODE     Duplicate handling: rename (default), overwrite, skip\n" );
    fprintf ( out, "    --ftype HH             MZF file type (default: 0x01=OBJ, with --mzf)\n" );
    fprintf ( out, "    --exec-addr N          Override exec address (with --mzf)\n" );
    fprintf ( out, "    --strt-addr N          Override start address (with --mzf)\n" );
    fprintf ( out, "  put <file> <name.ext> [options]  Store raw file to disk\n" );
    fprintf ( out, "    --fstrt ADDR            Set start address (default: 0x0000)\n" );
    fprintf ( out, "    --fexec ADDR            Set exec address (default: 0x0000)\n" );
    fprintf ( out, "    --name NAME.EXT         Strict name validation (no truncation; errors on 8.3 overflow or forbidden chars)\n" );
    fprintf ( out, "  put --mzf <file> [name.ext]  Store MZF file (metadata from header)\n" );
    fprintf ( out, "  put --mzf <file> --name NAME.EXT  Strict override (no truncation)\n" );
    fprintf ( out, "    --charset MODE          Sharp MZ variant for fname conversion (eu default, jp)\n" );
    fprintf ( out, "  era <name[.ext]>          Erase (delete) file by name\n" );
    fprintf ( out, "  era --id N                Erase (delete) file by ID\n" );
    fprintf ( out, "  ren <name[.ext]> <new[.ext]>  Rename file\n" );
    fprintf ( out, "  ren --id N <new[.ext]>    Rename file by ID\n" );
    fprintf ( out, "  set-addr <name[.ext]> [--fstrt HEX] [--fexec HEX]\n" );
    fprintf ( out, "                            Update start/exec address of existing file\n" );
    fprintf ( out, "                            (at least one of --fstrt, --fexec required)\n" );
    fprintf ( out, "  set-addr --id N [--fstrt HEX] [--fexec HEX]\n" );
    fprintf ( out, "                            Update addresses by file ID\n" );
    fprintf ( out, "  file <name[.ext]>         Show file details (metadata + block list)\n" );
    fprintf ( out, "  file --id N               Show file details by ID\n" );
    fprintf ( out, "  dump-block N [bytes]      Hexdump of MRS block(s)\n" );
    fprintf ( out, "    --noinv                 Show raw (inverted) data\n" );
    fprintf ( out, "    --dump-charset MODE     ASCII column charset: raw (default), eu, jp, utf8-eu, utf8-jp\n" );
    fprintf ( out, "  get-block N <file> [bytes] Extract MRS block(s) to file\n" );
    fprintf ( out, "    --noinv                 Extract raw (inverted) data\n" );
    fprintf ( out, "  put-block N <file> [bytes] [offset]  Write file into MRS block(s)\n" );
    fprintf ( out, "    --noinv                 Write without inversion\n" );
    fprintf ( out, "  init [--force]            Create empty MRS filesystem (FAT + directory)\n" );
    fprintf ( out, "                            --force required if MRS filesystem already exists\n" );
    fprintf ( out, "  defrag                    Defragment filesystem\n\n" );
    fprintf ( out, "Filenames:\n" );
    fprintf ( out, "  MRS names are 8 chars + 3 char extension, case-sensitive.\n" );
    fprintf ( out, "  Use \"name.ext\" format (e.g., \"util+.DAT\", \"nu2.MRS\").\n" );
    fprintf ( out, "  If no extension given, search matches any extension.\n\n" );
    fprintf ( out, "Examples:\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk info\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk dir\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk get util+.DAT util_plus.bin\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk get --mzf util+.DAT util_plus.mzf\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk get --mzf --id 1 output.mzf\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk put input.bin myfile.DAT --fstrt 0x2000 --fexec 0x2000\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk put --mzf input.mzf\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk put --mzf input.mzf myname.DAT\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk era util+.DAT\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk ren util+.DAT newname.DAT\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk get --all ./export\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk get --all --mzf ./export\n" );
    fprintf ( out, "  mzdsk-mrs disk.dsk get --all --mzf --ftype 0x22 ./export\n" );
    fprintf ( out, "  mzdsk-mrs --fat-block 36 disk.dsk init\n" );
}


/* =========================================================================
 * Příkazy - read-only
 * ========================================================================= */

/**
 * @brief Vytiskne souhrnné informace o MRS souborovém systému.
 *
 * Zobrazí rozložení FAT, adresáře a datové oblasti, celkovou
 * a volnou kapacitu a počet obsazených souborů.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci.
 */
static void cmd_info ( const st_FSMRS_CONFIG *config, en_OUTFMT output_format ) {

    uint32_t total_bytes = (uint32_t) config->total_blocks * FSMRS_SECTOR_SIZE;
    uint32_t free_bytes = (uint32_t) config->free_blocks * FSMRS_SECTOR_SIZE;
    uint32_t used_bytes = total_bytes - free_bytes;

    /* Detailní rozpad bloků z FAT (BUG 18): file_blocks z FAT umožní
       porovnat "Used = total - free" se skutečně označenými souborovými bloky
       a se součtem bsize z adresáře. Při nekonzistenci (soubor tvrdí v bsize
       víc bloků, než má skutečně v FAT) lze volat `defrag` pro sjednocení. */
    en_FSMRS_BLOCK_TYPE *block_map =
        (en_FSMRS_BLOCK_TYPE *) malloc ( sizeof ( en_FSMRS_BLOCK_TYPE ) * FSMRS_COUNT_BLOCKS );
    st_FSMRS_MAP_STATS stats;
    memset ( &stats, 0, sizeof ( stats ) );
    if ( block_map != NULL ) {
        fsmrs_get_block_map ( config, block_map, FSMRS_COUNT_BLOCKS, &stats );
        free ( block_map );
    }

    /* Součet bsize ze všech platných položek adresáře - porovnatelné s
       výstupem `dir` a s file_blocks z FAT. */
    uint32_t dir_bsize_sum = 0;
    for ( uint16_t i = 0; i < config->usable_files; i++ ) {
        st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( (st_FSMRS_CONFIG *) config, i );
        if ( item != NULL && fsmrs_is_dir_item_active ( item ) ) {
            dir_bsize_sum += item->bsize;
        }
    }

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nMRS Filesystem info:\n\n" );

        printf ( "  Layout:\n" );
        printf ( "    FAT block:      %u\n", config->fat_block );
        printf ( "    FAT sectors:    %u\n", config->fat_sectors );
        printf ( "    DIR block:      %u\n", config->dir_block );
        printf ( "    DIR sectors:    %u\n", config->dir_sectors );
        printf ( "    Data block:     %u\n", config->data_block );
        printf ( "\n" );

        printf ( "  Capacity:\n" );
        printf ( "    Total blocks:   %u (%u KB)\n", config->total_blocks, total_bytes / 1024 );
        printf ( "    Free blocks:    %u (%u KB)\n", config->free_blocks, free_bytes / 1024 );
        printf ( "    Used blocks:    %u (%u KB)\n",
                 config->total_blocks - config->free_blocks, used_bytes / 1024 );
        printf ( "      FAT:            %d\n", stats.fat_blocks );
        printf ( "      Directory:      %d\n", stats.dir_blocks );
        printf ( "      Bad:            %d\n", stats.bad_blocks );
        printf ( "      Files (FAT):    %d\n", stats.file_blocks );
        printf ( "      Files (dir):    %u\n", (unsigned) dir_bsize_sum );
        if ( (int) dir_bsize_sum != stats.file_blocks ) {
            printf ( "    (!) FAT file blocks differ from dir bsize sum by %+d - "
                     "run `defrag` to reconcile.\n",
                     (int) dir_bsize_sum - stats.file_blocks );
        }
        printf ( "\n" );

        printf ( "  Directory:\n" );
        printf ( "    Total slots:    %u\n", config->max_files );
        printf ( "    Usable slots:   %u\n", config->usable_files );
        printf ( "    Used files:     %u\n", config->used_files );
        printf ( "\n" );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "mrs" );
        outfmt_kv_uint ( &ctx, "fat_block", (unsigned long) config->fat_block );
        outfmt_kv_uint ( &ctx, "fat_sectors", (unsigned long) config->fat_sectors );
        outfmt_kv_uint ( &ctx, "dir_block", (unsigned long) config->dir_block );
        outfmt_kv_uint ( &ctx, "dir_sectors", (unsigned long) config->dir_sectors );
        outfmt_kv_uint ( &ctx, "data_block", (unsigned long) config->data_block );
        outfmt_kv_uint ( &ctx, "total_blocks", (unsigned long) config->total_blocks );
        outfmt_kv_uint ( &ctx, "free_blocks", (unsigned long) config->free_blocks );
        outfmt_kv_uint ( &ctx, "used_blocks", (unsigned long) ( config->total_blocks - config->free_blocks ) );
        outfmt_kv_uint ( &ctx, "fat_blocks", (unsigned long) stats.fat_blocks );
        outfmt_kv_uint ( &ctx, "dir_blocks", (unsigned long) stats.dir_blocks );
        outfmt_kv_uint ( &ctx, "bad_blocks", (unsigned long) stats.bad_blocks );
        outfmt_kv_uint ( &ctx, "file_blocks", (unsigned long) stats.file_blocks );
        outfmt_kv_uint ( &ctx, "dir_bsize_sum", (unsigned long) dir_bsize_sum );
        outfmt_kv_uint ( &ctx, "total_bytes", (unsigned long) total_bytes );
        outfmt_kv_uint ( &ctx, "free_bytes", (unsigned long) free_bytes );
        outfmt_kv_uint ( &ctx, "used_bytes", (unsigned long) used_bytes );
        outfmt_kv_uint ( &ctx, "total_slots", (unsigned long) config->max_files );
        outfmt_kv_uint ( &ctx, "usable_slots", (unsigned long) config->usable_files );
        outfmt_kv_uint ( &ctx, "used_files", (unsigned long) config->used_files );
        outfmt_doc_end ( &ctx );
    }
}


/**
 * @brief Vytiskne obsah adresáře.
 *
 * V režimu "raw=0" vypíše jen aktivní soubory ve formátu tabulky
 * s podrobnými metadaty. V režimu "raw=1" vypíše všechny sloty
 * včetně smazaných a prázdných.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci.
 * @param[in] raw     0 = jen aktivní soubory, 1 = všechny sloty.
 */
static void cmd_dir ( st_FSMRS_CONFIG *config, int raw, en_OUTFMT output_format ) {

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nMRS Directory:\n\n" );
        printf ( "  ID  Name       Ext  fstrt   fexec   Blocks    Size\n" );
        printf ( "  --  ---------  ---  ------  ------  ------  ---------\n" );
    }

    st_OUTFMT_CTX ctx;
    outfmt_init ( &ctx, output_format );

    if ( output_format != OUTFMT_TEXT ) {
        static const char *csv_hdr[] = {
            "id", "name", "ext", "fstrt", "fexec", "blocks", "size"
        };
        outfmt_csv_header ( &ctx, csv_hdr, 7 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "mrs" );
        outfmt_array_begin ( &ctx, "files" );
    }

    int count = 0;
    for ( uint16_t i = 0; i < config->max_files; i++ ) {
        st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, i );
        if ( item == NULL ) break;

        int active = fsmrs_is_dir_item_active ( item );

        if ( !raw && !active ) continue;

        uint32_t size_bytes = (uint32_t) item->bsize * FSMRS_SECTOR_SIZE;

        if ( output_format == OUTFMT_TEXT ) {
            printf ( "  %2u  ", item->file_id );

            if ( active ) {
                print_fname ( item->fname );
                printf ( "  " );
                print_ext ( item->ext );
            } else {
                printf ( "<empty>   ---" );
            }

            printf ( "  0x%04x  0x%04x  %6u  %7u B", item->fstrt, item->fexec, item->bsize, size_bytes );

            if ( !active ) {
                printf ( "  (slot %u)", i );
            }
            printf ( "\n" );
        } else {
            char name_buf[13];
            compose_ascii_name ( item->fname, item->ext, name_buf );

            /* Oddělíme jméno a příponu pro strukturovaný výstup */
            char fname_str[9], ext_str[4];
            int nlen = 8;
            while ( nlen > 0 && item->fname[nlen - 1] == 0x20 ) nlen--;
            for ( int j = 0; j < nlen; j++ ) fname_str[j] = (char) item->fname[j];
            fname_str[nlen] = '\0';

            int elen = 3;
            while ( elen > 0 && item->ext[elen - 1] == 0x20 ) elen--;
            for ( int j = 0; j < elen; j++ ) ext_str[j] = (char) item->ext[j];
            ext_str[elen] = '\0';

            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "id", item->file_id );
            outfmt_field_str ( &ctx, "name", active ? fname_str : "" );
            outfmt_field_str ( &ctx, "ext", active ? ext_str : "" );
            outfmt_field_hex16 ( &ctx, "fstrt", item->fstrt );
            outfmt_field_hex16 ( &ctx, "fexec", item->fexec );
            outfmt_field_uint ( &ctx, "blocks", (unsigned long) item->bsize );
            outfmt_field_uint ( &ctx, "size", (unsigned long) size_bytes );
            outfmt_item_end ( &ctx );
        }

        count++;
    }

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\n  %d entries, %u free blocks (%u KB free)\n\n",
                 count, config->free_blocks, (uint32_t) config->free_blocks * FSMRS_SECTOR_SIZE / 1024 );
    } else {
        outfmt_array_end ( &ctx );
        outfmt_kv_int ( &ctx, "total_files", count );
        outfmt_kv_uint ( &ctx, "free_blocks", (unsigned long) config->free_blocks );
        outfmt_kv_uint ( &ctx, "free_bytes", (unsigned long) config->free_blocks * FSMRS_SECTOR_SIZE );
        outfmt_doc_end ( &ctx );
    }
}


/**
 * @brief Vytiskne obsah FAT tabulky.
 *
 * Zobrazí pouze nenulové (obsazené) FAT záznamy ve formátu
 * hexdump po 16 bajtech na řádek.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci.
 */
static void cmd_fat ( const st_FSMRS_CONFIG *config, en_OUTFMT output_format ) {

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nMRS FAT table (non-zero entries):\n\n" );
        printf ( "  Offset   00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F\n" );
        printf ( "  ------   -----------------------------------------------\n" );

        for ( uint16_t row = 0; row < FSMRS_COUNT_BLOCKS; row += 16 ) {
            /* Přeskoč řádky kde jsou všechny hodnoty 0x00 */
            int any = 0;
            for ( int col = 0; col < 16 && row + col < FSMRS_COUNT_BLOCKS; col++ ) {
                if ( config->fat[row + col] != 0x00 ) {
                    any = 1;
                    break;
                }
            }
            if ( !any ) continue;

            printf ( "  %4u:   ", row );
            for ( int col = 0; col < 16; col++ ) {
                if ( row + col >= FSMRS_COUNT_BLOCKS ) {
                    printf ( "   " );
                } else {
                    printf ( "%02x ", config->fat[row + col] );
                }
                if ( col == 7 ) printf ( " " );
            }
            printf ( "\n" );
        }
        printf ( "\n" );

        printf ( "  Legend: 00=free, FA=FAT, FD=DIR, FE=bad, FF=system, other=file_id\n\n" );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = { "block", "value" };
        outfmt_csv_header ( &ctx, csv_hdr, 2 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "mrs" );

        uint16_t fat_coverage = (uint16_t) ( config->fat_sectors * FSMRS_SECTOR_SIZE );
        if ( fat_coverage > FSMRS_COUNT_BLOCKS ) fat_coverage = FSMRS_COUNT_BLOCKS;

        outfmt_array_begin ( &ctx, "fat" );
        for ( uint16_t b = 0; b < fat_coverage; b++ ) {
            if ( config->fat[b] == 0x00 ) continue;
            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "block", b );
            outfmt_field_hex8 ( &ctx, "value", config->fat[b] );
            outfmt_item_end ( &ctx );
        }
        outfmt_array_end ( &ctx );

        outfmt_doc_end ( &ctx );
    }
}


/* =========================================================================
 * Příkazy - souborové operace
 * ========================================================================= */

/**
 * @brief Extrahuje soubor z MRS disku do lokálního souboru.
 *
 * Přečte všechny bloky souboru identifikované file_id ve FAT
 * a zapíše je do výstupního souboru.
 *
 * @param[in] config       Ukazatel na inicializovanou konfiguraci.
 * @param[in] item         Ukazatel na adresářovou položku souboru.
 * @param[in] output_file  Cesta k výstupnímu souboru.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int cmd_get ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item, const char *output_file ) {

    /* Audit M-16: kontrola existence výstupního souboru před čtením z disku. */
    if ( check_output_overwrite ( output_file ) != EXIT_SUCCESS ) {
        return EXIT_FAILURE;
    }

    uint32_t data_size = (uint32_t) item->bsize * FSMRS_SECTOR_SIZE;

    if ( data_size == 0 ) {
        printf ( "  File has 0 blocks, nothing to extract.\n\n" );
        return EXIT_SUCCESS;
    }

    uint8_t *data = (uint8_t *) malloc ( data_size );
    if ( data == NULL ) {
        fprintf ( stderr, "Error: Out of memory (%u bytes)\n", data_size );
        return EXIT_FAILURE;
    }

    en_MZDSK_RES res = fsmrs_read_file ( config, item, data, data_size );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not read file: %s\n", mzdsk_get_error ( res ) );
        free ( data );
        return EXIT_FAILURE;
    }

    FILE *f = fopen ( output_file, "wb" );
    if ( f == NULL ) {
        fprintf ( stderr, "Error: Could not create file '%s': %s\n", output_file, strerror ( errno ) );
        free ( data );
        return EXIT_FAILURE;
    }

    if ( fwrite ( data, 1, data_size, f ) != data_size ) {
        fprintf ( stderr, "Error: Could not write file '%s': %s\n", output_file, strerror ( errno ) );
        fclose ( f );
        free ( data );
        return EXIT_FAILURE;
    }

    fclose ( f );
    free ( data );

    printf ( "  Extracted " );
    print_fname_dot_ext ( item->fname, item->ext );
    printf ( " -> %s (%u blocks, %u bytes)\n\n", output_file, item->bsize, data_size );

    return EXIT_SUCCESS;
}


/**
 * @brief Vloží lokální soubor na MRS disk.
 *
 * Přečte data z lokálního souboru, alokuje bloky v datové oblasti
 * a zapíše soubor na disk s aktualizací FAT a adresáře.
 *
 * @param[in] config      Ukazatel na inicializovanou konfiguraci.
 * @param[in] input_file  Cesta k vstupnímu souboru.
 * @param[in] mrs_name    Jméno souboru na disku ("name.ext").
 * @param[in] fstrt       Start adresa.
 * @param[in] fexec       Exec adresa.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int cmd_put ( st_FSMRS_CONFIG *config, const char *input_file, const char *mrs_name,
                     int strict_name, uint16_t fstrt, uint16_t fexec ) {
    /* Parsování MRS jména */
    uint8_t fname[8], ext[3];
    if ( strict_name ) {
        if ( parse_mrs_filename_strict ( mrs_name, fname, ext, NULL ) != EXIT_SUCCESS ) {
            return EXIT_FAILURE;
        }
    } else {
        int has_ext = 0, truncated = 0;
        parse_mrs_filename ( mrs_name, fname, ext, &has_ext, &truncated );
        if ( truncated ) {
            warn_mrs_truncation ( mrs_name, fname, ext, has_ext );
        }
    }

    /* Čtení vstupního souboru */
    FILE *f = fopen ( input_file, "rb" );
    if ( f == NULL ) {
        fprintf ( stderr, "Error: Could not open file '%s': %s\n", input_file, strerror ( errno ) );
        return EXIT_FAILURE;
    }

    fseek ( f, 0, SEEK_END );
    long fsize = ftell ( f );
    fseek ( f, 0, SEEK_SET );

    if ( fsize < 0 ) {
        fprintf ( stderr, "Error: Could not determine file size of '%s'\n", input_file );
        fclose ( f );
        return EXIT_FAILURE;
    }

    uint8_t *data = NULL;
    if ( fsize > 0 ) {
        data = (uint8_t *) malloc ( (size_t) fsize );
        if ( data == NULL ) {
            fprintf ( stderr, "Error: Out of memory (%ld bytes)\n", fsize );
            fclose ( f );
            return EXIT_FAILURE;
        }
        if ( fread ( data, 1, (size_t) fsize, f ) != (size_t) fsize ) {
            fprintf ( stderr, "Error: Could not read file '%s': %s\n", input_file, strerror ( errno ) );
            free ( data );
            fclose ( f );
            return EXIT_FAILURE;
        }
    }
    fclose ( f );

    /* Zápis na disk */
    en_MZDSK_RES res = fsmrs_write_file ( config, fname, ext, fstrt, fexec, data, (uint32_t) fsize );
    free ( data );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not write file: %s\n", mzdsk_get_error ( res ) );
        return EXIT_FAILURE;
    }

    uint16_t bsize = ( fsize == 0 ) ? 1 : (uint16_t) ( ( (uint32_t) fsize + FSMRS_SECTOR_SIZE - 1 ) / FSMRS_SECTOR_SIZE );
    printf ( "  Stored %s -> ", input_file );
    print_fname_dot_ext ( fname, ext );
    printf ( " (%u blocks, fstrt=0x%04x, fexec=0x%04x)\n\n", bsize, fstrt, fexec );

    return EXIT_SUCCESS;
}


/**
 * @brief Extrahuje soubor z MRS disku jako MZF (128B hlavička + data).
 *
 * Vytvoří MZF soubor s hlavičkou obsahující metadata z adresářové
 * položky (ftype=OBJ, jméno, fstrt, fexec, fsize). Data jsou čtena
 * ze všech bloků souboru a deinvertována.
 *
 * @param[in] config       Ukazatel na inicializovanou konfiguraci.
 * @param[in] item         Ukazatel na adresářovou položku souboru.
 * @param[in] output_file  Cesta k výstupnímu MZF souboru.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @note MZF fsize je uint16_t, takže soubory větší než 65535 B
 *       nelze exportovat do MZF formátu.
 */
static int cmd_get_mzf ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item, const char *output_file ) {

    /* Audit M-16: kontrola existence výstupního souboru před čtením z disku. */
    if ( check_output_overwrite ( output_file ) != EXIT_SUCCESS ) {
        return EXIT_FAILURE;
    }

    uint32_t data_size = (uint32_t) item->bsize * FSMRS_SECTOR_SIZE;

    if ( data_size > MZF_MAX_BODY_SIZE ) {
        fprintf ( stderr, "Error: File too large for MZF format (%u bytes, max %u)\n",
                  data_size, (unsigned) MZF_MAX_BODY_SIZE );
        return EXIT_FAILURE;
    }

    if ( data_size == 0 ) {
        printf ( "  File has 0 blocks, nothing to extract.\n\n" );
        return EXIT_SUCCESS;
    }

    uint8_t *data = (uint8_t *) malloc ( data_size );
    if ( data == NULL ) {
        fprintf ( stderr, "Error: Out of memory (%u bytes)\n", data_size );
        return EXIT_FAILURE;
    }

    en_MZDSK_RES res = fsmrs_read_file ( config, item, data, data_size );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not read file: %s\n", mzdsk_get_error ( res ) );
        free ( data );
        return EXIT_FAILURE;
    }

    /* Sestavíme ASCII jméno "fname.ext" pro MZF hlavičku */
    char ascii_name[13];
    compose_ascii_name ( item->fname, item->ext, ascii_name );

    /* Vytvoříme MZF hlavičku */
    st_MZF_HEADER mzfhdr;
    memset ( &mzfhdr, 0, sizeof ( mzfhdr ) );
    mzfhdr.ftype = MZF_FTYPE_OBJ;
    mzf_tools_set_fname ( &mzfhdr, ascii_name );
    mzfhdr.fsize = (uint16_t) data_size;
    mzfhdr.fstrt = item->fstrt;
    mzfhdr.fexec = item->fexec;

    /* Zapíšeme MZF do paměťového handleru a uložíme na disk */
    st_HANDLER *h = generic_driver_open_memory ( NULL, &g_memory_driver_realloc,
                                                  MZF_HEADER_SIZE + data_size );
    if ( h == NULL ) {
        fprintf ( stderr, "Error: Could not create memory handler\n" );
        free ( data );
        return EXIT_FAILURE;
    }

    if ( mzf_write_header ( h, &mzfhdr ) != EXIT_SUCCESS ||
         mzf_write_body ( h, data, (uint16_t) data_size ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Could not write MZF data\n" );
        generic_driver_close ( h );
        free ( h );
        free ( data );
        return EXIT_FAILURE;
    }

    int save_res = generic_driver_save_memory ( h, (char *) output_file );
    generic_driver_close ( h );
    free ( h );
    free ( data );

    if ( save_res != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Could not save file '%s'\n", output_file );
        return EXIT_FAILURE;
    }

    printf ( "  Extracted " );
    print_fname_dot_ext ( item->fname, item->ext );
    printf ( " -> %s (MZF, %u blocks, %u bytes)\n\n", output_file, item->bsize, data_size );

    return EXIT_SUCCESS;
}


/**
 * @brief Exportuje všechny soubory z MRS disku do adresáře.
 *
 * Iteruje přes všechny aktivní položky MRS adresáře, čte data
 * a ukládá je na hostitelský FS jako raw binárky nebo MZF soubory.
 * Jména souborů jsou sanitizována pro hostitelský FS (Windows-safe).
 *
 * Při MZF exportu se z adresářové položky přebírají metadata (fstrt, fexec),
 * pokud nejsou přepsány parametry ftype_override, exec_override, strt_override.
 * Soubory větší než 65535 B nelze exportovat jako MZF a jsou přeskočeny
 * s varováním (ale raw export projde vždy).
 *
 * @param[in] config          Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @param[in] dir             Cílový adresář na hostitelském FS. Nesmí být NULL.
 * @param[in] dup_mode        Režim řešení duplicitních jmen.
 * @param[in] is_mzf          1 = export jako MZF (128B hlavička + data), 0 = raw.
 * @param[in] ftype_override  Override MZF ftype: 0x00-0xFF, nebo -1 pro výchozí (OBJ = 0x01).
 * @param[in] exec_override   Override exec adresy: 0x0000-0xFFFF, nebo -1 pro hodnotu z dir entry.
 * @param[in] strt_override   Override start adresy: 0x0000-0xFFFF, nebo -1 pro hodnotu z dir entry.
 *
 * @return EXIT_SUCCESS při úspěchu (i když některé soubory byly přeskočeny
 *         přes --on-duplicate skip - to je uživatelská volba).
 * @return EXIT_FAILURE pokud došlo k jakékoli chybě při exportu souboru
 *         (errors > 0), i když se některé soubory povedlo extrahovat.
 *         Skript tak zjistí, že export byl jen částečný (BUG B7).
 *
 * @pre config musí být inicializovaná platná konfigurace (fsmrs_init proběhl úspěšně).
 * @post Cílový adresář dir je vytvořen pokud neexistoval.
 * @post Exportované soubory jsou zapsány na hostitelský FS.
 *
 * @note Vedlejší efekty: vytváří soubory a adresáře na hostitelském FS.
 * @note Funkce alokuje a uvolňuje paměť pro data souborů a MZF handler
 *       v každé iteraci zvlášť.
 */
static int cmd_get_all ( st_FSMRS_CONFIG *config, const char *dir,
                         en_DUPLICATE_MODE dup_mode, int is_mzf,
                         int ftype_override, long exec_override,
                         long strt_override ) {

    printf ( "\nExport all from MRS into dir '%s'\n\n", dir );

    /* Vytvoření cílového adresáře */
    if ( _mkdir ( dir ) != 0 && errno != EEXIST ) {
        fprintf ( stderr, "Error: Could not create directory '%s': %s\n", dir, strerror ( errno ) );
        return EXIT_FAILURE;
    }

    int extracted = 0;
    int skipped = 0;
    int renamed = 0;
    int errors = 0;

    for ( uint16_t i = 0; i < config->usable_files; i++ ) {
        st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, i );
        if ( item == NULL || !fsmrs_is_dir_item_active ( item ) ) continue;

        /* Sestavení ASCII jména a sanitizace */
        char ascii_name[13];
        compose_ascii_name ( item->fname, item->ext, ascii_name );

        /* Rozdělení na name a ext části pro build_mrs_output_path */
        char safe_name[9] = { 0 };
        char safe_ext[4] = { 0 };
        int nlen = 8;
        while ( nlen > 0 && item->fname[nlen - 1] == 0x20 ) nlen--;
        for ( int j = 0; j < nlen && j < 8; j++ ) safe_name[j] = (char) item->fname[j];
        safe_name[nlen] = '\0';

        int elen = 3;
        while ( elen > 0 && item->ext[elen - 1] == 0x20 ) elen--;
        for ( int j = 0; j < elen && j < 3; j++ ) safe_ext[j] = (char) item->ext[j];
        safe_ext[elen] = '\0';

        sanitize_filename ( safe_name );
        sanitize_filename ( safe_ext );

        /* Sestavení výstupní cesty */
        char path_buf[MAX_PATH_LEN];
        int path_res = build_mrs_output_path ( path_buf, sizeof ( path_buf ),
                                               dir, safe_name, safe_ext,
                                               is_mzf, dup_mode );
        if ( path_res < 0 ) {
            fprintf ( stderr, "Error: Output path too long for '%s'\n", ascii_name );
            errors++;
            continue;
        }
        if ( path_res > 0 ) {
            skipped++;
            continue;
        }

        /* Detekce přejmenování (pro statistiku) */
        char orig_path[MAX_PATH_LEN];
        if ( is_mzf ) {
            if ( safe_ext[0] != '\0' ) {
                snprintf ( orig_path, sizeof ( orig_path ), "%s/%s.%s.mzf", dir, safe_name, safe_ext );
            } else {
                snprintf ( orig_path, sizeof ( orig_path ), "%s/%s.mzf", dir, safe_name );
            }
        } else {
            if ( safe_ext[0] != '\0' ) {
                snprintf ( orig_path, sizeof ( orig_path ), "%s/%s.%s", dir, safe_name, safe_ext );
            } else {
                snprintf ( orig_path, sizeof ( orig_path ), "%s/%s", dir, safe_name );
            }
        }
        int was_renamed = ( strcmp ( path_buf, orig_path ) != 0 );

        /* Čtení dat souboru */
        uint32_t data_size = (uint32_t) item->bsize * FSMRS_SECTOR_SIZE;

        if ( data_size == 0 ) {
            printf ( "  Skipping '%s' (0 blocks)\n", ascii_name );
            skipped++;
            continue;
        }

        /* MZF limit kontrola */
        if ( is_mzf && data_size > MZF_MAX_BODY_SIZE ) {
            fprintf ( stderr, "Warning: Skipping '%s' (too large for MZF: %u bytes, max %u)\n",
                      ascii_name, data_size, (unsigned) MZF_MAX_BODY_SIZE );
            skipped++;
            continue;
        }

        uint8_t *data = (uint8_t *) malloc ( data_size );
        if ( data == NULL ) {
            fprintf ( stderr, "Error: Out of memory (%u bytes)\n", data_size );
            errors++;
            continue;
        }

        en_MZDSK_RES res = fsmrs_read_file ( config, item, data, data_size );
        if ( res != MZDSK_RES_OK ) {
            fprintf ( stderr, "Error: Could not read '%s': %s\n", ascii_name, mzdsk_get_error ( res ) );
            free ( data );
            errors++;
            continue;
        }

        if ( is_mzf ) {
            /* MZF export */
            st_MZF_HEADER mzfhdr;
            memset ( &mzfhdr, 0, sizeof ( mzfhdr ) );
            mzfhdr.ftype = ( ftype_override >= 0 ) ? (uint8_t) ftype_override : MZF_FTYPE_OBJ;
            mzf_tools_set_fname ( &mzfhdr, ascii_name );
            mzfhdr.fsize = (uint16_t) data_size;
            mzfhdr.fstrt = ( strt_override >= 0 ) ? (uint16_t) strt_override : item->fstrt;
            mzfhdr.fexec = ( exec_override >= 0 ) ? (uint16_t) exec_override : item->fexec;

            st_HANDLER *h = generic_driver_open_memory ( NULL, &g_memory_driver_realloc,
                                                          MZF_HEADER_SIZE + data_size );
            if ( h == NULL ) {
                fprintf ( stderr, "Error: Could not create memory handler for '%s'\n", ascii_name );
                free ( data );
                errors++;
                continue;
            }

            if ( mzf_write_header ( h, &mzfhdr ) != EXIT_SUCCESS ||
                 mzf_write_body ( h, data, (uint16_t) data_size ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: MZF encoding failed for '%s'\n", ascii_name );
                generic_driver_close ( h );
                free ( h );
                free ( data );
                errors++;
                continue;
            }

            int save_res = generic_driver_save_memory ( h, (char *) path_buf );
            generic_driver_close ( h );
            free ( h );
            free ( data );

            if ( save_res != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Could not save '%s'\n", path_buf );
                errors++;
                continue;
            }

            printf ( "  Extracted %s -> %s (MZF, %u blocks, %u bytes)\n",
                     ascii_name, path_buf, item->bsize, data_size );
        } else {
            /* Raw export */
            FILE *f = fopen ( path_buf, "wb" );
            if ( f == NULL ) {
                fprintf ( stderr, "Error: Could not create '%s': %s\n", path_buf, strerror ( errno ) );
                free ( data );
                errors++;
                continue;
            }

            if ( fwrite ( data, 1, data_size, f ) != data_size ) {
                fprintf ( stderr, "Error: Write error to '%s': %s\n", path_buf, strerror ( errno ) );
                fclose ( f );
                free ( data );
                errors++;
                continue;
            }

            fclose ( f );
            free ( data );

            printf ( "  Extracted %s -> %s (%u blocks, %u bytes)\n",
                     ascii_name, path_buf, item->bsize, data_size );
        }

        extracted++;
        if ( was_renamed ) renamed++;
    }

    /* Souhrn */
    if ( extracted == 0 && errors == 0 && skipped == 0 ) {
        printf ( "No files found.\n" );
        return EXIT_SUCCESS;
    }

    printf ( "\nExtracted %d files", extracted );
    if ( skipped > 0 || renamed > 0 || errors > 0 ) {
        printf ( " (" );
        int need_comma = 0;
        if ( skipped > 0 ) {
            printf ( "%d skipped", skipped );
            need_comma = 1;
        }
        if ( renamed > 0 ) {
            if ( need_comma ) printf ( ", " );
            printf ( "%d renamed", renamed );
            need_comma = 1;
        }
        if ( errors > 0 ) {
            if ( need_comma ) printf ( ", " );
            printf ( "%d errors", errors );
        }
        printf ( ")" );
    }
    printf ( "\n" );

    /* Jakákoli chyba při exportu souboru = částečné selhání, skripty
       musí mít šanci to detekovat (BUG B7). Skipped samo o sobě OK
       (uživatelská volba --on-duplicate skip). */
    return ( errors > 0 ) ? EXIT_FAILURE : EXIT_SUCCESS;
}


/**
 * @brief Vloží MZF soubor na MRS disk.
 *
 * Přečte MZF hlavičku a tělo ze souboru. Metadata (fstrt, fexec)
 * se převezmou z MZF hlavičky. Jméno na MRS disku lze zadat
 * z příkazového řádku, nebo se odvodí z MZF hlavičky.
 *
 * @param[in] config             Ukazatel na inicializovanou konfiguraci.
 * @param[in] input_file         Cesta k vstupnímu MZF souboru.
 * @param[in] mrs_name_override  Jméno na MRS disku ("name.ext"), nebo NULL
 *                               pro odvození z MZF hlavičky.
 * @param[in] strict_name        Pokud nenulové, `mrs_name_override` prochází
 *                               striktní validací bez tichého zkracování.
 * @param[in] charset            Varianta Sharp MZ znakové sady pro konverzi
 *                               fname odvozeného z MZF hlavičky (EU/JP).
 *                               Použije se jen když `mrs_name_override == NULL`.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int cmd_put_mzf ( st_FSMRS_CONFIG *config, const char *input_file,
                          const char *mrs_name_override, int strict_name,
                          en_MZF_NAME_ENCODING charset ) {
    /* Kontrola regular file (audit M-19) - adresář by prošel fopen
     * ale fread vrátil 0 a uživatel dostane matoucí "empty file" hlášku. */
    {
        struct stat st;
        if ( stat ( input_file, &st ) == 0 && !S_ISREG ( st.st_mode ) ) {
            fprintf ( stderr, "Error: '%s' is not a regular file\n", input_file );
            return EXIT_FAILURE;
        }
    }

    /* Otevřeme MZF soubor přes generic_driver */
    st_HANDLER *h = generic_driver_open_memory_from_file ( NULL, &g_memory_driver_realloc,
                                                            input_file );
    if ( h == NULL ) {
        fprintf ( stderr, "Error: Could not open file '%s': %s\n", input_file, strerror ( errno ) );
        return EXIT_FAILURE;
    }
    generic_driver_set_handler_readonly_status ( h, 1 );

    /* Přečteme MZF hlavičku */
    st_MZF_HEADER mzfhdr;
    if ( mzf_read_header ( h, &mzfhdr ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Could not read MZF header from '%s'\n", input_file );
        generic_driver_close ( h );
        free ( h );
        return EXIT_FAILURE;
    }

    /* Přečteme MZF tělo */
    uint8_t *data = NULL;
    if ( mzfhdr.fsize > 0 ) {
        data = (uint8_t *) malloc ( mzfhdr.fsize );
        if ( data == NULL ) {
            fprintf ( stderr, "Error: Out of memory (%u bytes)\n", mzfhdr.fsize );
            generic_driver_close ( h );
            free ( h );
            return EXIT_FAILURE;
        }
        if ( mzf_read_body ( h, data, mzfhdr.fsize ) != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Could not read MZF body from '%s'\n", input_file );
            free ( data );
            generic_driver_close ( h );
            free ( h );
            return EXIT_FAILURE;
        }
    }

    generic_driver_close ( h );
    free ( h );

    /* Určíme MRS jméno a příponu */
    uint8_t fname[8], ext[3];
    if ( mrs_name_override != NULL ) {
        if ( strict_name ) {
            if ( parse_mrs_filename_strict ( mrs_name_override, fname, ext, NULL ) != EXIT_SUCCESS ) {
                free ( data );
                return EXIT_FAILURE;
            }
        } else {
            int has_ext_ov = 0, trunc_ov = 0;
            parse_mrs_filename ( mrs_name_override, fname, ext, &has_ext_ov, &trunc_ov );
            if ( trunc_ov ) {
                warn_mrs_truncation ( mrs_name_override, fname, ext, has_ext_ov );
            }
        }
    } else {
        /* Odvodíme z MZF hlavičky s volitelnou variantou Sharp MZ znakové sady. */
        char ascii_name[MZF_FILE_NAME_LENGTH + 1];
        mzf_tools_get_fname_ex ( &mzfhdr, ascii_name, sizeof ( ascii_name ), charset );
        int has_ext = 0, trunc = 0;
        parse_mrs_filename ( ascii_name, fname, ext, &has_ext, &trunc );
        if ( !has_ext ) {
            memcpy ( ext, "DAT", 3 );
        }
        if ( trunc ) {
            warn_mrs_truncation ( ascii_name, fname, ext, 1 );
        }
    }

    /* Zapíšeme na MRS disk */
    en_MZDSK_RES res = fsmrs_write_file ( config, fname, ext,
                                           mzfhdr.fstrt, mzfhdr.fexec,
                                           data, (uint32_t) mzfhdr.fsize );
    free ( data );

    if ( res != MZDSK_RES_OK ) {
        if ( res == MZDSK_RES_BAD_NAME && mrs_name_override == NULL ) {
            /* Odvození z MZF selhalo (obvykle leading space nebo samé
               non-printable po konverzi) - poradit explicitní override. */
            fprintf ( stderr, "Error: Could not derive usable MRS filename from MZF header.\n" );
            fprintf ( stderr, "       Use 'put --mzf %s NAME.EXT' to specify name explicitly,\n", input_file );
            fprintf ( stderr, "       or try --charset jp if the MZF uses Japanese character set.\n" );
        } else {
            fprintf ( stderr, "Error: Could not write file: %s\n", mzdsk_get_error ( res ) );
        }
        return EXIT_FAILURE;
    }

    uint16_t bsize = ( mzfhdr.fsize == 0 ) ? 1
                     : (uint16_t) ( ( (uint32_t) mzfhdr.fsize + FSMRS_SECTOR_SIZE - 1 ) / FSMRS_SECTOR_SIZE );
    printf ( "  Stored %s -> ", input_file );
    print_fname_dot_ext ( fname, ext );
    printf ( " (MZF, %u blocks, fstrt=0x%04x, fexec=0x%04x)\n\n", bsize, mzfhdr.fstrt, mzfhdr.fexec );

    return EXIT_SUCCESS;
}


/**
 * @brief Smaže soubor z MRS disku.
 *
 * Uvolní bloky ve FAT, vymaže adresářovou položku a zapíše
 * změny na disk.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci.
 * @param[in] item    Ukazatel na adresářovou položku souboru.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int cmd_era ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item ) {
    /* Uložíme jméno pro výpis před smazáním */
    uint8_t fname_copy[8], ext_copy[3];
    memcpy ( fname_copy, item->fname, 8 );
    memcpy ( ext_copy, item->ext, 3 );
    uint8_t fid = item->file_id;

    en_MZDSK_RES res = fsmrs_delete_file ( config, item );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not delete file: %s\n", mzdsk_get_error ( res ) );
        return EXIT_FAILURE;
    }

    printf ( "  Erased file #%u: ", fid );
    print_fname_dot_ext ( fname_copy, ext_copy );
    printf ( "\n\n" );

    return EXIT_SUCCESS;
}


/**
 * @brief Přejmenuje soubor na MRS disku.
 *
 * Ověří unikátnost nového jména a aktualizuje adresářovou položku.
 *
 * @param[in] config    Ukazatel na inicializovanou konfiguraci.
 * @param[in] item      Ukazatel na adresářovou položku souboru.
 * @param[in] new_name  Nové jméno ("name.ext" nebo "name").
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int cmd_ren ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item, const char *new_name ) {
    uint8_t new_fname[8], new_ext[3];
    int has_ext = 0, truncated = 0;
    parse_mrs_filename ( new_name, new_fname, new_ext, &has_ext, &truncated );
    if ( truncated ) {
        warn_mrs_truncation ( new_name, new_fname, new_ext, has_ext );
    }

    /* Uložíme staré jméno pro výpis */
    uint8_t old_fname[8], old_ext[3];
    memcpy ( old_fname, item->fname, 8 );
    memcpy ( old_ext, item->ext, 3 );

    en_MZDSK_RES res = fsmrs_rename_file ( config, item, new_fname, has_ext ? new_ext : NULL );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not rename file: %s\n", mzdsk_get_error ( res ) );
        return EXIT_FAILURE;
    }

    printf ( "  Renamed " );
    print_fname_dot_ext ( old_fname, old_ext );
    printf ( " -> " );
    print_fname_dot_ext ( item->fname, item->ext );
    printf ( "\n\n" );

    return EXIT_SUCCESS;
}


/* =========================================================================
 * Příkaz set-addr - změna STRT/EXEC adres existujícího souboru
 * ========================================================================= */


/**
 * @brief Aktualizuje start a exec adresu existujícího souboru.
 *
 * Modifikuje pouze adresářovou položku (fields fstrt a fexec),
 * neodmazává ani nepřepisuje obsah souboru. Analog to mzdsk-fsmz
 * `chtype`, ale pro MRS adresní pole.
 *
 * @param[in] config Ukazatel na inicializovanou konfiguraci (musí být RW).
 * @param[in] item   Adresářová položka cílového souboru.
 * @param[in] fstrt  Nová start adresa (0x0000-0xFFFF).
 * @param[in] fexec  Nová exec adresa (0x0000-0xFFFF).
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě zápisu.
 */
static int cmd_set_addr ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item,
                           uint16_t fstrt, uint16_t fexec ) {
    uint16_t old_strt = item->fstrt;
    uint16_t old_exec = item->fexec;
    en_MZDSK_RES res = fsmrs_set_addr ( config, item, fstrt, fexec );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not set addresses: %s\n", mzdsk_get_error ( res ) );
        return EXIT_FAILURE;
    }
    printf ( "  set-addr " );
    print_fname_dot_ext ( item->fname, item->ext );
    printf ( ":  strt 0x%04x -> 0x%04x,  exec 0x%04x -> 0x%04x\n\n",
             old_strt, fstrt, old_exec, fexec );
    return EXIT_SUCCESS;
}


/* =========================================================================
 * Příkaz file - detailní informace o souboru
 * ========================================================================= */


/**
 * @brief Zobrazí detailní informace o souboru včetně seznamu bloků.
 *
 * Vypíše metadata z adresářové položky a projde FAT tabulku
 * pro sestavení seznamu všech bloků patřících souboru.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci.
 * @param[in] item    Ukazatel na adresářovou položku souboru.
 *
 * @return EXIT_SUCCESS.
 */
static int cmd_file ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item,
                      en_OUTFMT output_format ) {
    uint32_t data_size = (uint32_t) item->bsize * FSMRS_SECTOR_SIZE;

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nFile: " );
        print_fname_dot_ext ( item->fname, item->ext );
        printf ( " (ID %u)\n", item->file_id );
        printf ( "\tSize:       %u B (%u blocks x %u B)\n", data_size, item->bsize, FSMRS_SECTOR_SIZE );
        printf ( "\tStart addr: 0x%04x\n", item->fstrt );
        printf ( "\tExec addr:  0x%04x\n", item->fexec );

        /* Projdi FAT a vypiš všechny bloky patřící souboru */
        uint16_t fat_coverage = (uint16_t) ( config->fat_sectors * FSMRS_SECTOR_SIZE );
        if ( fat_coverage > FSMRS_COUNT_BLOCKS ) fat_coverage = FSMRS_COUNT_BLOCKS;

        printf ( "\n\tBlocks (%u):", item->bsize );

        uint16_t count = 0;
        for ( uint16_t b = 0; b < fat_coverage; b++ ) {
            if ( config->fat[b] == item->file_id ) {
                uint16_t trsec = fsmrs_block2trsec ( b );
                if ( count % 6 == 0 ) printf ( "\n\t  " );
                printf ( "%4u (T%u:S%u)  ", b, ( trsec >> 8 ) & 0xff, trsec & 0xff );
                count++;
            }
        }

        if ( count != item->bsize ) {
            printf ( "\n\n\tWARNING: FAT contains %u blocks, directory says %u!", count, item->bsize );
        }

        printf ( "\n\n" );
    } else {
        char fname_str[9], ext_str[4];
        int nlen = 8;
        while ( nlen > 0 && item->fname[nlen - 1] == 0x20 ) nlen--;
        for ( int j = 0; j < nlen; j++ ) fname_str[j] = (char) item->fname[j];
        fname_str[nlen] = '\0';

        int elen = 3;
        while ( elen > 0 && item->ext[elen - 1] == 0x20 ) elen--;
        for ( int j = 0; j < elen; j++ ) ext_str[j] = (char) item->ext[j];
        ext_str[elen] = '\0';

        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = {
            "id", "name", "ext", "fstrt", "fexec", "blocks", "size"
        };
        outfmt_csv_header ( &ctx, csv_hdr, 7 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "mrs" );

        outfmt_array_begin ( &ctx, "files" );
        outfmt_item_begin ( &ctx );
        outfmt_field_int ( &ctx, "id", item->file_id );
        outfmt_field_str ( &ctx, "name", fname_str );
        outfmt_field_str ( &ctx, "ext", ext_str );
        outfmt_field_hex16 ( &ctx, "fstrt", item->fstrt );
        outfmt_field_hex16 ( &ctx, "fexec", item->fexec );
        outfmt_field_uint ( &ctx, "blocks", (unsigned long) item->bsize );
        outfmt_field_uint ( &ctx, "size", (unsigned long) data_size );
        outfmt_item_end ( &ctx );
        outfmt_array_end ( &ctx );

        outfmt_doc_end ( &ctx );
    }

    return EXIT_SUCCESS;
}


/* =========================================================================
 * Raw block operations
 * ========================================================================= */




/**
 * @brief Provede hexdump MRS bloku (nebo série bloků).
 *
 * Data jsou automaticky deinvertována (XOR 0xFF). S volbou noinv
 * se zobrazí surová (invertovaná) data.
 *
 * @param config       Ukazatel na konfiguraci.
 * @param block        Číslo MRS bloku.
 * @param size         Počet bajtů (0 = jeden blok = FSMRS_SECTOR_SIZE).
 * @param noinv        1 = zobrazit surová data bez deinverze.
 * @param dump_charset Režim konverze znakové sady v ASCII sloupci.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_dump_block ( st_FSMRS_CONFIG *config, uint16_t block,
                                      size_t size, int noinv,
                                      en_MZDSK_HEXDUMP_CHARSET dump_charset ) {
    if ( size == 0 ) size = FSMRS_SECTOR_SIZE;

    uint16_t trsec = fsmrs_block2trsec ( block );

    printf ( "\nDump %u B from MRS block %u (track %u, sector %u)%s:\n\n",
             (unsigned) size, block, ( trsec >> 8 ) & 0xff, trsec & 0xff,
             noinv ? " (raw, no inversion)" : "" );

    uint8_t dma[FSMRS_SECTOR_SIZE];

    while ( size ) {
        size_t length = ( size >= FSMRS_SECTOR_SIZE ) ? FSMRS_SECTOR_SIZE : size;

        en_MZDSK_RES err = fsmrs_read_block ( config->disc, block, dma );
        if ( err != MZDSK_RES_OK ) return err;

        if ( !noinv ) {
            mzdsk_invert_data ( dma, FSMRS_SECTOR_SIZE );
        }

        if ( size > FSMRS_SECTOR_SIZE ) {
            trsec = fsmrs_block2trsec ( block );
            printf ( "Block %u (track %u, sector %u):\n",
                     block, ( trsec >> 8 ) & 0xff, trsec & 0xff );
        }

        st_MZDSK_HEXDUMP_CFG hcfg;
        mzdsk_hexdump_init ( &hcfg );
        hcfg.charset = dump_charset;
        mzdsk_hexdump ( &hcfg, dma, (uint16_t) length );

        block++;
        size -= length;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Extrahuje data z MRS bloku do lokálního souboru.
 *
 * Data jsou automaticky deinvertována. S volbou noinv se uloží
 * surová (invertovaná) data. Režim noinv je indikován ve výpisu
 * textem "(raw, no inversion)".
 *
 * @param config   Ukazatel na konfiguraci.
 * @param block    Číslo MRS bloku.
 * @param out_file Výstupní soubor.
 * @param size     Počet bajtů (0 = jeden blok).
 * @param noinv    1 = bez deinverze.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_get_block ( st_FSMRS_CONFIG *config, uint16_t block,
                                     const char *out_file, size_t size, int noinv ) {

    /* Audit M-16: kontrola existence výstupního souboru před čtením z disku. */
    if ( check_output_overwrite ( out_file ) != EXIT_SUCCESS ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    if ( size == 0 ) size = FSMRS_SECTOR_SIZE;

    printf ( "\nExtract %u B from MRS block %u into file: %s%s\n\n",
             (unsigned) size, block, out_file,
             noinv ? " (raw, no inversion)" : "" );

    FILE *fh = fopen ( out_file, "wb" );
    if ( fh == NULL ) {
        fprintf ( stderr, "Error: Could not open file '%s': %s\n", out_file, strerror ( errno ) );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    uint8_t dma[FSMRS_SECTOR_SIZE];

    while ( size ) {
        size_t length = ( size >= FSMRS_SECTOR_SIZE ) ? FSMRS_SECTOR_SIZE : size;

        en_MZDSK_RES err = fsmrs_read_block ( config->disc, block, dma );
        if ( err != MZDSK_RES_OK ) { fclose ( fh ); return err; }

        if ( !noinv ) {
            mzdsk_invert_data ( dma, FSMRS_SECTOR_SIZE );
        }

        uint16_t trsec = fsmrs_block2trsec ( block );
        printf ( "  Block %u (track %u, sector %u): %u B\n",
                 block, ( trsec >> 8 ) & 0xff, trsec & 0xff, (unsigned) length );

        if ( fwrite ( dma, 1, length, fh ) != length ) {
            fprintf ( stderr, "Error: Could not write to file: %s\n", strerror ( errno ) );
            fclose ( fh );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        block++;
        size -= length;
    }

    printf ( "\nDone.\n\n" );
    fclose ( fh );
    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše data z lokálního souboru do MRS bloku.
 *
 * Data jsou automaticky invertována (XOR 0xFF) před zápisem.
 * S volbou noinv se zapíšou přímo bez inverze. Režim noinv
 * je indikován ve výpisu textem "(raw, no inversion)".
 *
 * @param config      Ukazatel na konfiguraci.
 * @param block       Číslo MRS bloku.
 * @param in_file     Vstupní soubor.
 * @param size        Počet bajtů (0 = celý soubor od offsetu).
 * @param file_offset Offset ve vstupním souboru.
 * @param noinv       1 = bez inverze.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_put_block ( st_FSMRS_CONFIG *config, uint16_t block,
                                     const char *in_file, size_t size,
                                     size_t file_offset, int noinv ) {

    FILE *fh = fopen ( in_file, "rb" );
    if ( fh == NULL ) {
        fprintf ( stderr, "Error: Could not open file '%s': %s\n", in_file, strerror ( errno ) );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    fseek ( fh, 0, SEEK_END );
    /* Audit M-26: ftell může vrátit -1, viz komentář v mzdsk-fsmz. */
    long ftell_res = ftell ( fh );
    if ( ftell_res < 0 ) {
        fprintf ( stderr, "Error: ftell failed on input file: %s\n", strerror ( errno ) );
        fclose ( fh );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    size_t filesize = (size_t) ftell_res;

    if ( file_offset > filesize ) {
        fprintf ( stderr, "Error: offset (%u) exceeds file size (%u)\n",
                  (unsigned) file_offset, (unsigned) filesize );
        fclose ( fh );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    fseek ( fh, (long) file_offset, SEEK_SET );
    if ( size == 0 ) size = filesize - file_offset;

    printf ( "\nWrite %u B from file %s into MRS block %u%s\n\n",
             (unsigned) size, in_file, block,
             noinv ? " (raw, no inversion)" : "" );

    uint8_t dma[FSMRS_SECTOR_SIZE];

    while ( size ) {
        size_t length = ( size >= FSMRS_SECTOR_SIZE ) ? FSMRS_SECTOR_SIZE : size;

        memset ( dma, 0x00, sizeof ( dma ) );
        if ( fread ( dma, 1, length, fh ) != length && ferror ( fh ) ) {
            fprintf ( stderr, "Error: Could not read from file: %s\n", strerror ( errno ) );
            fclose ( fh );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        if ( !noinv ) {
            mzdsk_invert_data ( dma, FSMRS_SECTOR_SIZE );
        }

        uint16_t trsec = fsmrs_block2trsec ( block );
        printf ( "  Block %u (track %u, sector %u): %u B\n",
                 block, ( trsec >> 8 ) & 0xff, trsec & 0xff, (unsigned) length );

        en_MZDSK_RES err = fsmrs_write_block ( config->disc, block, dma );
        if ( err != MZDSK_RES_OK ) { fclose ( fh ); return err; }

        block++;
        size -= length;
    }

    printf ( "\nDone.\n\n" );
    fclose ( fh );
    return MZDSK_RES_OK;
}


/* =========================================================================
 * Příkaz defrag
 * ========================================================================= */

/**
 * @brief Callback pro výpis průběhu defragmentace na stdout.
 *
 * Jednoduchý callback předávaný do fsmrs_defrag(), který vypisuje
 * informační zprávy knihovní funkce přímo na stdout.
 *
 * @param[in] message    Textová zpráva o průběhu.
 * @param[in] user_data  Nepoužito (vždy NULL).
 */
static void defrag_progress ( const char *message, void *user_data ) {
    (void) user_data;
    printf ( "%s", message );
}


/**
 * @brief Provede defragmentaci MRS disku (CLI wrapper).
 *
 * Deleguje na knihovní fsmrs_defrag() a vypisuje uživatelský
 * výstup na stdout.
 *
 * @param[in] disc       Ukazatel na otevřený disk. Nesmí být NULL.
 * @param[in] fat_block  Číslo bloku, kde začíná FAT.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int cmd_defrag ( st_MZDSK_DISC *disc, uint16_t fat_block ) {

    printf ( "\nRun defragmentation MRS\n\n" );

    en_MZDSK_RES err = fsmrs_defrag ( disc, fat_block, defrag_progress, NULL );
    if ( err != MZDSK_RES_OK ) {
        fprintf ( stderr, "\nError: Defragmentation failed: %s\n", mzdsk_get_error ( err ) );
        return EXIT_FAILURE;
    }

    printf ( "\nDone.\n\n" );
    return EXIT_SUCCESS;
}


/* =========================================================================
 * Příkaz init
 * ========================================================================= */

/**
 * @brief Vytvoří prázdnou MRS FAT a adresář na disketě.
 *
 * Zavolá fsmrs_format_fs() a poté přečte vytvořený FS zpět přes
 * fsmrs_init(), aby dynamicky zjistil skutečné rozložení FAT a adresáře.
 * Výsledné hodnoty vypíše konzistentně s příkazem "info".
 *
 * Nezasahuje do systémové oblasti (tracky 0-3) ani do datové oblasti
 * za adresářem.
 *
 * @param[in] disc      Ukazatel na otevřenou diskovou strukturu v RW režimu.
 * @param[in] fat_block Číslo bloku, kde má začínat FAT.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int cmd_init ( st_MZDSK_DISC *disc, uint16_t fat_block ) {
    printf ( "\nInitializing MRS filesystem at fat_block=%u...\n", fat_block );

    en_MZDSK_RES res = fsmrs_format_fs ( disc, fat_block );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: fsmrs_format_fs failed: %s\n", mzdsk_get_error ( res ) );
        return EXIT_FAILURE;
    }

    /* Přečteme vytvořený FS zpět, abychom zjistili skutečné rozložení */
    st_FSMRS_CONFIG verify;
    res = fsmrs_init ( disc, fat_block, &verify );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: verification read-back failed: %s\n", mzdsk_get_error ( res ) );
        return EXIT_FAILURE;
    }

    printf ( "  FAT written:       %u sectors at blocks %u-%u\n",
             verify.fat_sectors, verify.fat_block,
             verify.fat_block + verify.fat_sectors - 1 );
    printf ( "  Directory written: %u sectors at blocks %u-%u\n",
             verify.dir_sectors, verify.dir_block,
             verify.dir_block + verify.dir_sectors - 1 );
    printf ( "  Data block:        %u\n", verify.data_block );
    printf ( "  %u empty directory slots, %u max files\n",
             verify.max_files - verify.used_files, verify.max_files );
    printf ( "\nMRS filesystem initialized successfully.\n\n" );
    return EXIT_SUCCESS;
}


/* =========================================================================
 * Main
 * ========================================================================= */

/**
 * @brief Log callback pro DSK knihovnu - tiskne pouze chybové zprávy na stderr.
 *
 * Informační a warning hlášky potlačuje, aby výstup nástroje byl čitelný.
 */
static void dsk_log_stderr ( int level, const char *msg, void *user_data ) {
    (void) user_data;
    if ( level != DSK_LOG_ERROR ) return;
    fprintf ( stderr, "dsk error: %s\n", msg );
}


/**
 * @brief Extrahuje globální volbu --output/-o z sub_argv kdekoliv v poli,
 *        aplikuje ji a zkompaktuje pole argumentů.
 *
 * Řeší BUG 17: uživatel může --output/-o uvést kdekoliv (i za subpříkazem).
 * Akceptuje: -o VAL, --output VAL, --output=VAL.
 *
 * @param[in,out] sub_argc      Ukazatel na počet argumentů v sub_argv.
 * @param[in,out] sub_argv      Pole argumentů. Po volání může být kratší.
 * @param[out]    output_format Nastaví se pokud byla nalezena --output.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int extract_trailing_global_opts ( int *sub_argc, char **sub_argv,
                                            en_OUTFMT *output_format ) {
    int i = 1;
    while ( i < *sub_argc ) {
        const char *val = NULL;
        int consume = 0;

        if ( strncmp ( sub_argv[i], "--output=", 9 ) == 0 ) {
            val = sub_argv[i] + 9; consume = 1;
        } else if ( strcmp ( sub_argv[i], "--output" ) == 0 ||
                    strcmp ( sub_argv[i], "-o" ) == 0 ) {
            if ( i + 1 >= *sub_argc ) {
                fprintf ( stderr, "Error: %s requires an argument\n", sub_argv[i] );
                return EXIT_FAILURE;
            }
            val = sub_argv[i+1]; consume = 2;
        }

        if ( consume == 0 ) { i++; continue; }

        if ( outfmt_parse ( val, output_format ) != 0 ) {
            fprintf ( stderr, "Error: Unknown output format '%s' (use text, json or csv)\n", val );
            return EXIT_FAILURE;
        }

        for ( int j = i; j + consume < *sub_argc; j++ ) {
            sub_argv[j] = sub_argv[j + consume];
        }
        *sub_argc -= consume;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Vstupní bod programu mzdsk-mrs.
 *
 * Parsuje argumenty příkazové řádky, otevře DSK obraz, inicializuje
 * MRS konfiguraci a provede požadovanou operaci.
 *
 * @param argc Počet argumentů.
 * @param argv Pole řetězců s argumenty.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
int main ( int argc, char *argv[] ) {

    memory_driver_init();
    dsk_tools_set_log_cb ( dsk_log_stderr, NULL );

    if ( argc < 2 ) {
        print_usage ( stderr );
        return EXIT_FAILURE;
    }

    /* Definice dlouhých voleb pro getopt_long */
    static struct option global_options[] = {
        { "ro",           no_argument,       NULL, 'R' },
        { "overwrite",    no_argument,       NULL, 'O' },
        { "yes",          no_argument,       NULL, 'y' },
        { "output",       required_argument, NULL, 'o' },
        { "fat-block",    required_argument, NULL, 'f' },
        { "version",      no_argument,       NULL, 'V' },
        { "lib-versions", no_argument,       NULL, 'L' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL,           0,                 NULL,  0  }
    };

    /* Parsování globálních voleb přes getopt_long */
    int force_ro = 0;
    uint16_t fat_block = FSMRS_DEFAULT_FAT_BLOCK;
    en_OUTFMT output_format = OUTFMT_TEXT;

    optind = 1;
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "+o:yh", global_options, NULL ) ) != -1 ) {
        switch ( opt ) {

            case 'R': /* --ro */
                force_ro = 1;
                break;

            case 'O': /* --overwrite (audit M-16) */
                g_allow_overwrite = 1;
                break;

            case 'y': /* --yes (audit M-17) */
                g_assume_yes = 1;
                break;

            case 'o': /* --output FMT, -o FMT */
                if ( outfmt_parse ( optarg, &output_format ) != 0 ) {
                    fprintf ( stderr, "Error: Unknown output format '%s' (use text, json or csv)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;

            case 'f': { /* --fat-block N */
                char *end;
                long val = strtol ( optarg, &end, 0 );
                if ( *end != '\0' || val < 0 || val >= FSMRS_COUNT_BLOCKS ) {
                    fprintf ( stderr, "Error: Invalid --fat-block value '%s'\n", optarg );
                    return EXIT_FAILURE;
                }
                fat_block = (uint16_t) val;
                break;
            }

            case 'V': /* --version */
                printf ( "mzdsk-mrs %s (%s %s)\n",
                         MZDSK_MRS_TOOL_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
                return EXIT_SUCCESS;

            case 'L': /* --lib-versions */
                print_lib_versions();
                return EXIT_SUCCESS;

            case 'h': /* --help */
                print_usage ( stdout );
                return EXIT_SUCCESS;

            default: /* neznámá volba - getopt_long již vypsal chybu */
                print_usage ( stderr );
                return EXIT_FAILURE;
        }
    }

    /* Po zpracování voleb musí zbýt DSK soubor + subpříkaz */
    if ( optind >= argc ) {
        fprintf ( stderr, "Error: DSK file and subcommand required\n\n" );
        print_usage ( stderr );
        return EXIT_FAILURE;
    }

    char *dsk_filename = argv[optind++];

    /* Druhé kolo: globální volby mezi DSK souborem a subpříkazem */
    while ( ( opt = getopt_long ( argc, argv, "+o:yh", global_options, NULL ) ) != -1 ) {
        switch ( opt ) {
            case 'R': force_ro = 1; break;
            case 'O': g_allow_overwrite = 1; break;  /* audit M-16 */
            case 'y': g_assume_yes = 1; break;       /* audit M-17 */
            case 'o':
                if ( outfmt_parse ( optarg, &output_format ) != 0 ) {
                    fprintf ( stderr, "Error: Unknown output format '%s' (use text, json or csv)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;
            case 'f': {
                char *end;
                long val = strtol ( optarg, &end, 0 );
                if ( *end != '\0' || val < 0 || val >= FSMRS_COUNT_BLOCKS ) {
                    fprintf ( stderr, "Error: Invalid --fat-block value '%s'\n", optarg );
                    return EXIT_FAILURE;
                }
                fat_block = (uint16_t) val;
                break;
            }
            case 'V': printf ( "mzdsk-mrs %s (%s %s)\n", MZDSK_MRS_TOOL_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION ); return EXIT_SUCCESS;
            case 'L': print_lib_versions(); return EXIT_SUCCESS;
            case 'h': print_usage ( stdout ); return EXIT_SUCCESS;
            default: print_usage ( stderr ); return EXIT_FAILURE;
        }
    }

    if ( optind >= argc ) {
        fprintf ( stderr, "Error: subcommand required\n\n" );
        print_usage ( stderr );
        return EXIT_FAILURE;
    }

    char *subcmd = argv[optind];
    /* sub_argv[0] = subcmd (funguje jako "argv[0]" pro podřízenou getopt_long) */
    int sub_argc = argc - optind;
    char **sub_argv = &argv[optind];

    /* BUG 17: extrahovat globální volbu --output/-o, pokud ji uživatel
       umístil za subpříkaz. */
    if ( extract_trailing_global_opts ( &sub_argc, sub_argv, &output_format ) != EXIT_SUCCESS ) {
        return EXIT_FAILURE;
    }

    /* Určení zda jde o zápisovou operaci */
    int is_write_op = 0;
    if ( strcmp ( subcmd, "init" ) == 0 ||
         strcmp ( subcmd, "put" ) == 0 ||
         strcmp ( subcmd, "era" ) == 0 ||
         strcmp ( subcmd, "ren" ) == 0 ||
         strcmp ( subcmd, "set-addr" ) == 0 ||
         strcmp ( subcmd, "put-block" ) == 0 ||
         strcmp ( subcmd, "defrag" ) == 0 ) {
        is_write_op = 1;
    }
    if ( force_ro ) {
        if ( is_write_op ) {
            fprintf ( stderr, "Error: command '%s' requires write access, cannot use --ro\n", subcmd );
            return EXIT_FAILURE;
        }
    }

    /* Otevření DSK souboru */
    st_MZDSK_DISC disc;
    en_MZDSK_RES err;

    if ( is_write_op ) {
        err = mzdsk_disc_open_memory ( &disc, dsk_filename, FILE_DRIVER_OPMODE_RW );
    } else {
        err = mzdsk_disc_open ( &disc, dsk_filename, FILE_DRIVER_OPMODE_RO );
    }
    if ( err != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not open DSK file '%s': %s\n",
                  dsk_filename, mzdsk_get_error ( err ) );
        return EXIT_FAILURE;
    }

    /* Přepneme sector_info_cb na MRS variantu - MRS má na stopě 1
       256B sektory a na ostatních stopách 512B sektory, ale žádný
       sector není z pohledu driveru invertovaný (inverzi řeší MRS sám). */
    disc.sector_info_cb = fsmrs_sector_info_cb;
    disc.sector_info_cb_data = NULL;

    /* Zpracování podpříkazu */
    int ret = EXIT_SUCCESS;

    /* Příkaz "init" nemůžeme volat přes fsmrs_init() protože ten
       vyžaduje existující FAT. Init se volá přímo nad diskem. */
    if ( strcmp ( subcmd, "init" ) == 0 ) {

        /* Kontrola --force u subpříkazu init */
        static struct option init_opts[] = {
            { "force", no_argument, NULL, 'F' },
            { NULL,    0,           NULL,  0  }
        };
        int force = 0;
        optind = 0;
        int ic;
        while ( ( ic = getopt_long ( sub_argc, sub_argv, "", init_opts, NULL ) ) != -1 ) {
            if ( ic == 'F' ) force = 1;
            else { ret = EXIT_FAILURE; goto cleanup; }
        }

        /* Detekce existujícího MRS filesystému */
        st_FSMRS_CONFIG probe;
        if ( !force && fsmrs_init ( &disc, fat_block, &probe ) == MZDSK_RES_OK ) {
            fprintf ( stderr, "Error: MRS filesystem already exists (%u files). "
                      "Use --force to overwrite.\n", probe.used_files );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

        /* Audit M-17: po --force přejít přes interaktivní prompt */
        if ( !confirm_destructive_op ( "initialize MRS filesystem (erase all files)" ) ) {
            fprintf ( stderr, "Aborted.\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

        ret = cmd_init ( &disc, fat_block );
        if ( ret == EXIT_SUCCESS ) {
            en_MZDSK_RES save_err = mzdsk_disc_save ( &disc );
            if ( save_err != MZDSK_RES_OK ) {
                fprintf ( stderr, "Error: Could not save DSK file: %s\n",
                          mzdsk_get_error ( save_err ) );
                ret = EXIT_FAILURE;
            }
        }
        mzdsk_disc_close ( &disc );
        return ret;
    }

    /* Pro všechny ostatní příkazy nejprve načteme konfiguraci z FAT */
    st_FSMRS_CONFIG config;
    err = fsmrs_init ( &disc, fat_block, &config );
    if ( err != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not initialize MRS filesystem: %s\n",
                  mzdsk_get_error ( err ) );
        mzdsk_disc_close ( &disc );
        return EXIT_FAILURE;
    }

    if ( strcmp ( subcmd, "info" ) == 0 ) {
        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'info' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            ret = EXIT_FAILURE;
            goto cleanup;
        }
        cmd_info ( &config, output_format );

    } else if ( strcmp ( subcmd, "dir" ) == 0 ) {
        /* dir [--raw] */
        static struct option dir_opts[] = {
            { "raw", no_argument, NULL, 'r' },
            { NULL,  0,           NULL,  0  }
        };
        int raw = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", dir_opts, NULL ) ) != -1 ) {
            if ( c == 'r' ) raw = 1;
            else { ret = EXIT_FAILURE; goto cleanup; }
        }
        cmd_dir ( &config, raw, output_format );

    } else if ( strcmp ( subcmd, "fat" ) == 0 ) {
        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'fat' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            ret = EXIT_FAILURE;
            goto cleanup;
        }
        cmd_fat ( &config, output_format );

    } else if ( strcmp ( subcmd, "get" ) == 0 ) {
        /* get [--mzf] [--id N] [--all] <name[.ext]> <file> | --all <dir> */
        static struct option get_opts[] = {
            { "mzf",          no_argument,       NULL, 'm' },
            { "id",           required_argument, NULL, 'i' },
            { "all",          no_argument,       NULL, 'a' },
            { "on-duplicate", required_argument, NULL, 'D' },
            { "ftype",        required_argument, NULL, 'F' },
            { "exec-addr",    required_argument, NULL, 'A' },
            { "strt-addr",    required_argument, NULL, 'S' },
            { NULL,           0,                 NULL,  0  }
        };
        int use_mzf = 0;
        int get_all = 0;
        long file_id = -1;
        en_DUPLICATE_MODE dup_mode = DUP_RENAME;
        int ftype_override = -1;
        long exec_override = -1;
        long strt_override = -1;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", get_opts, NULL ) ) != -1 ) {
            switch ( c ) {
                case 'm': use_mzf = 1; break;
                case 'a': get_all = 1; break;
                case 'i': {
                    unsigned long id;
                    if ( parse_number ( optarg, &id ) != EXIT_SUCCESS || id == 0 || id > 255 ) {
                        fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                        ret = EXIT_FAILURE; goto cleanup;
                    }
                    file_id = (long) id;
                    break;
                }
                case 'D':
                    if ( strcmp ( optarg, "rename" ) == 0 ) {
                        dup_mode = DUP_RENAME;
                    } else if ( strcmp ( optarg, "overwrite" ) == 0 ) {
                        dup_mode = DUP_OVERWRITE;
                    } else if ( strcmp ( optarg, "skip" ) == 0 ) {
                        dup_mode = DUP_SKIP;
                    } else {
                        fprintf ( stderr, "Error: Unknown --on-duplicate mode '%s'"
                                  " (use rename, overwrite or skip)\n", optarg );
                        ret = EXIT_FAILURE; goto cleanup;
                    }
                    break;
                case 'F': {
                    unsigned long val;
                    if ( parse_number ( optarg, &val ) != EXIT_SUCCESS || val > 0xFF ) {
                        fprintf ( stderr, "Error: Invalid --ftype value '%s' (0x00-0xFF)\n", optarg );
                        ret = EXIT_FAILURE; goto cleanup;
                    }
                    ftype_override = (int) val;
                    break;
                }
                case 'A': {
                    unsigned long val;
                    if ( parse_number ( optarg, &val ) != EXIT_SUCCESS || val > 0xFFFF ) {
                        fprintf ( stderr, "Error: Invalid --exec-addr value '%s' (0x0000-0xFFFF)\n", optarg );
                        ret = EXIT_FAILURE; goto cleanup;
                    }
                    exec_override = (long) val;
                    break;
                }
                case 'S': {
                    unsigned long val;
                    if ( parse_number ( optarg, &val ) != EXIT_SUCCESS || val > 0xFFFF ) {
                        fprintf ( stderr, "Error: Invalid --strt-addr value '%s' (0x0000-0xFFFF)\n", optarg );
                        ret = EXIT_FAILURE; goto cleanup;
                    }
                    strt_override = (long) val;
                    break;
                }
                default: ret = EXIT_FAILURE; goto cleanup;
            }
        }
        /* Poziční argumenty za volbami */
        int pos_remaining = sub_argc - optind;

        if ( get_all ) {
            /* get --all [--mzf] [volby] <dir> */
            if ( !use_mzf && ( ftype_override >= 0 || exec_override >= 0 || strt_override >= 0 ) ) {
                fprintf ( stderr, "Error: --ftype/--exec-addr/--strt-addr require --mzf\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            if ( file_id >= 0 ) {
                fprintf ( stderr, "Error: --id cannot be used with --all\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            if ( pos_remaining < 1 ) {
                fprintf ( stderr, "Error: get --all requires <dir> argument\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            if ( pos_remaining > 1 ) {
                fprintf ( stderr, "Error: get --all has too many arguments (unexpected '%s')\n",
                          sub_argv[optind + 1] );
                ret = EXIT_FAILURE; goto cleanup;
            }
            ret = cmd_get_all ( &config, sub_argv[optind], dup_mode, use_mzf,
                                ftype_override, exec_override, strt_override );
        } else {
            /* Původní single-file get */
            if ( ftype_override >= 0 || exec_override >= 0 || strt_override >= 0 ) {
                fprintf ( stderr, "Error: --ftype/--exec-addr/--strt-addr can only be used with --all --mzf\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            if ( dup_mode != DUP_RENAME ) {
                fprintf ( stderr, "Error: --on-duplicate can only be used with --all\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }

            st_FSMRS_DIR_ITEM *item = NULL;
            const char *output_file = NULL;

            if ( file_id >= 0 ) {
                /* get [--mzf] --id N <file> */
                if ( pos_remaining < 1 ) {
                    fprintf ( stderr, "Error: get --id requires <output_file>\n" );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                item = fsmrs_search_file_by_id ( &config, (uint8_t) file_id );
                if ( item == NULL ) {
                    fprintf ( stderr, "Error: File with ID %ld not found\n", file_id );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                output_file = sub_argv[optind];
            } else if ( pos_remaining >= 2 ) {
                /* get [--mzf] <name[.ext]> <file> */
                item = find_file_by_name ( &config, sub_argv[optind] );
                if ( item == NULL ) {
                    fprintf ( stderr, "Error: File '%s' not found\n", sub_argv[optind] );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                output_file = sub_argv[optind + 1];
            } else {
                fprintf ( stderr, "Error: get requires <name[.ext]> <output_file> or --id <N> <output_file>\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            ret = use_mzf ? cmd_get_mzf ( &config, item, output_file )
                          : cmd_get ( &config, item, output_file );
        }

    } else if ( strcmp ( subcmd, "put" ) == 0 ) {
        /* put [--mzf] [--fstrt ADDR] [--fexec ADDR] [--name NAME.EXT] <file> [<name.ext>] */
        static struct option put_opts[] = {
            { "mzf",     no_argument,       NULL, 'm' },
            { "fstrt",   required_argument, NULL, 's' },
            { "fexec",   required_argument, NULL, 'x' },
            { "name",    required_argument, NULL, 'n' },
            { "charset", required_argument, NULL, 'C' },
            { NULL,      0,                 NULL,  0  }
        };
        int use_mzf = 0;
        uint16_t fstrt = 0x0000;
        uint16_t fexec = 0x0000;
        const char *strict_name = NULL;
        /* Výchozí varianta Sharp MZ znakové sady pro konverzi fname odvozeného
           z MZF hlavičky (put --mzf bez explicitního jména). Default EU. */
        en_MZF_NAME_ENCODING put_charset = MZF_NAME_ASCII_EU;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", put_opts, NULL ) ) != -1 ) {
            switch ( c ) {
                case 'm': use_mzf = 1; break;
                case 'n': strict_name = optarg; break;
                case 'C':
                    if ( strcmp ( optarg, "eu" ) == 0 ) {
                        put_charset = MZF_NAME_ASCII_EU;
                    } else if ( strcmp ( optarg, "jp" ) == 0 ) {
                        put_charset = MZF_NAME_ASCII_JP;
                    } else {
                        fprintf ( stderr, "Error: Unknown --charset '%s' (use 'eu' or 'jp')\n", optarg );
                        ret = EXIT_FAILURE; goto cleanup;
                    }
                    break;
                case 's': {
                    unsigned long val;
                    if ( parse_number ( optarg, &val ) != EXIT_SUCCESS || val > 0xFFFF ) {
                        fprintf ( stderr, "Error: Invalid --fstrt value '%s'\n", optarg );
                        ret = EXIT_FAILURE; goto cleanup;
                    }
                    fstrt = (uint16_t) val;
                    break;
                }
                case 'x': {
                    unsigned long val;
                    if ( parse_number ( optarg, &val ) != EXIT_SUCCESS || val > 0xFFFF ) {
                        fprintf ( stderr, "Error: Invalid --fexec value '%s'\n", optarg );
                        ret = EXIT_FAILURE; goto cleanup;
                    }
                    fexec = (uint16_t) val;
                    break;
                }
                default: ret = EXIT_FAILURE; goto cleanup;
            }
        }
        int pos_remaining = sub_argc - optind;
        if ( use_mzf ) {
            if ( pos_remaining < 1 ) {
                fprintf ( stderr, "Error: put --mzf requires <input_file>\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            /* Striktní --name vyžaduje, že nebyl zadán poziční override. */
            if ( strict_name != NULL && pos_remaining >= 2 ) {
                fprintf ( stderr, "Error: --name conflicts with positional <name.ext>\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            const char *input_file = sub_argv[optind];
            const char *mrs_name_override;
            int is_strict;
            if ( strict_name != NULL ) {
                mrs_name_override = strict_name;
                is_strict = 1;
            } else {
                mrs_name_override = ( pos_remaining >= 2 ) ? sub_argv[optind + 1] : NULL;
                is_strict = 0;
            }
            ret = cmd_put_mzf ( &config, input_file, mrs_name_override, is_strict, put_charset );
        } else {
            /* Striktní --name nahrazuje 2. poziční argument. */
            if ( strict_name != NULL && pos_remaining >= 2 ) {
                fprintf ( stderr, "Error: --name conflicts with positional <name.ext>\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            if ( strict_name == NULL && pos_remaining < 2 ) {
                fprintf ( stderr, "Error: put requires <input_file> <name.ext>, --name NAME.EXT, or --mzf <input_file>\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            if ( strict_name != NULL && pos_remaining < 1 ) {
                fprintf ( stderr, "Error: put --name requires <input_file>\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            const char *name_arg;
            int is_strict;
            if ( strict_name != NULL ) {
                name_arg = strict_name;
                is_strict = 1;
            } else {
                name_arg = sub_argv[optind + 1];
                is_strict = 0;
            }
            ret = cmd_put ( &config, sub_argv[optind], name_arg, is_strict, fstrt, fexec );
        }

    } else if ( strcmp ( subcmd, "era" ) == 0 ) {
        /* era [--id N] <name[.ext]> */
        static struct option era_opts[] = {
            { "id", required_argument, NULL, 'i' },
            { NULL, 0,                 NULL,  0  }
        };
        long file_id = -1;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", era_opts, NULL ) ) != -1 ) {
            if ( c == 'i' ) {
                unsigned long id;
                if ( parse_number ( optarg, &id ) != EXIT_SUCCESS || id == 0 || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                file_id = (long) id;
            } else { ret = EXIT_FAILURE; goto cleanup; }
        }
        st_FSMRS_DIR_ITEM *item = NULL;
        if ( file_id >= 0 ) {
            item = fsmrs_search_file_by_id ( &config, (uint8_t) file_id );
            if ( item == NULL ) {
                fprintf ( stderr, "Error: File with ID %ld not found\n", file_id );
                ret = EXIT_FAILURE; goto cleanup;
            }
        } else if ( optind < sub_argc ) {
            item = find_file_by_name ( &config, sub_argv[optind] );
            if ( item == NULL ) {
                fprintf ( stderr, "Error: File '%s' not found\n", sub_argv[optind] );
                ret = EXIT_FAILURE; goto cleanup;
            }
        } else {
            fprintf ( stderr, "Error: era requires <name[.ext]> or --id <N>\n" );
            ret = EXIT_FAILURE; goto cleanup;
        }
        ret = cmd_era ( &config, item );

    } else if ( strcmp ( subcmd, "ren" ) == 0 ) {
        /* ren [--id N] <name[.ext]> <new[.ext]> */
        static struct option ren_opts[] = {
            { "id", required_argument, NULL, 'i' },
            { NULL, 0,                 NULL,  0  }
        };
        long file_id = -1;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", ren_opts, NULL ) ) != -1 ) {
            if ( c == 'i' ) {
                unsigned long id;
                if ( parse_number ( optarg, &id ) != EXIT_SUCCESS || id == 0 || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                file_id = (long) id;
            } else { ret = EXIT_FAILURE; goto cleanup; }
        }
        st_FSMRS_DIR_ITEM *item = NULL;
        const char *new_name = NULL;
        int pos_remaining = sub_argc - optind;
        if ( file_id >= 0 ) {
            if ( pos_remaining < 1 ) {
                fprintf ( stderr, "Error: ren --id requires <new_name[.ext]>\n" );
                ret = EXIT_FAILURE; goto cleanup;
            }
            item = fsmrs_search_file_by_id ( &config, (uint8_t) file_id );
            if ( item == NULL ) {
                fprintf ( stderr, "Error: File with ID %ld not found\n", file_id );
                ret = EXIT_FAILURE; goto cleanup;
            }
            new_name = sub_argv[optind];
        } else if ( pos_remaining >= 2 ) {
            item = find_file_by_name ( &config, sub_argv[optind] );
            if ( item == NULL ) {
                fprintf ( stderr, "Error: File '%s' not found\n", sub_argv[optind] );
                ret = EXIT_FAILURE; goto cleanup;
            }
            new_name = sub_argv[optind + 1];
        } else {
            fprintf ( stderr, "Error: ren requires <name[.ext]> <new_name[.ext]> or --id <N> <new_name[.ext]>\n" );
            ret = EXIT_FAILURE; goto cleanup;
        }
        ret = cmd_ren ( &config, item, new_name );

    } else if ( strcmp ( subcmd, "set-addr" ) == 0 ) {
        /* set-addr [--id N] <name[.ext]> [--fstrt HEX] [--fexec HEX]
           Update pouze fstrt/fexec v adresářové položce. Alespoň jedna
           z voleb musí být zadaná. */
        static struct option sa_opts[] = {
            { "id",    required_argument, NULL, 'i' },
            { "fstrt", required_argument, NULL, 's' },
            { "fexec", required_argument, NULL, 'e' },
            { NULL,    0,                 NULL,  0  }
        };
        long file_id = -1;
        long opt_fstrt = -1, opt_fexec = -1;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", sa_opts, NULL ) ) != -1 ) {
            if ( c == 'i' ) {
                unsigned long id;
                if ( parse_number ( optarg, &id ) != EXIT_SUCCESS || id == 0 || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                file_id = (long) id;
            } else if ( c == 's' ) {
                unsigned long v;
                if ( parse_number ( optarg, &v ) != EXIT_SUCCESS || v > 0xFFFF ) {
                    fprintf ( stderr, "Error: Invalid --fstrt '%s' (expect 0x0000-0xFFFF)\n", optarg );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                opt_fstrt = (long) v;
            } else if ( c == 'e' ) {
                unsigned long v;
                if ( parse_number ( optarg, &v ) != EXIT_SUCCESS || v > 0xFFFF ) {
                    fprintf ( stderr, "Error: Invalid --fexec '%s' (expect 0x0000-0xFFFF)\n", optarg );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                opt_fexec = (long) v;
            } else { ret = EXIT_FAILURE; goto cleanup; }
        }
        if ( opt_fstrt < 0 && opt_fexec < 0 ) {
            fprintf ( stderr, "Error: set-addr requires --fstrt and/or --fexec\n" );
            ret = EXIT_FAILURE; goto cleanup;
        }
        st_FSMRS_DIR_ITEM *item = NULL;
        int pos_remaining = sub_argc - optind;
        if ( file_id >= 0 ) {
            item = fsmrs_search_file_by_id ( &config, (uint8_t) file_id );
            if ( item == NULL ) {
                fprintf ( stderr, "Error: File with ID %ld not found\n", file_id );
                ret = EXIT_FAILURE; goto cleanup;
            }
        } else if ( pos_remaining >= 1 ) {
            item = find_file_by_name ( &config, sub_argv[optind] );
            if ( item == NULL ) {
                fprintf ( stderr, "Error: File '%s' not found\n", sub_argv[optind] );
                ret = EXIT_FAILURE; goto cleanup;
            }
        } else {
            fprintf ( stderr, "Error: set-addr requires <name[.ext]> or --id <N>\n" );
            ret = EXIT_FAILURE; goto cleanup;
        }
        uint16_t new_strt = ( opt_fstrt >= 0 ) ? (uint16_t) opt_fstrt : item->fstrt;
        uint16_t new_exec = ( opt_fexec >= 0 ) ? (uint16_t) opt_fexec : item->fexec;
        ret = cmd_set_addr ( &config, item, new_strt, new_exec );

    } else if ( strcmp ( subcmd, "file" ) == 0 ) {
        /* file [--id N] <name[.ext]> */
        static struct option file_opts[] = {
            { "id", required_argument, NULL, 'i' },
            { NULL, 0,                 NULL,  0  }
        };
        long file_id = -1;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", file_opts, NULL ) ) != -1 ) {
            if ( c == 'i' ) {
                unsigned long id;
                if ( parse_number ( optarg, &id ) != EXIT_SUCCESS || id == 0 || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    ret = EXIT_FAILURE; goto cleanup;
                }
                file_id = (long) id;
            } else { ret = EXIT_FAILURE; goto cleanup; }
        }
        st_FSMRS_DIR_ITEM *item = NULL;
        if ( file_id >= 0 ) {
            item = fsmrs_search_file_by_id ( &config, (uint8_t) file_id );
            if ( item == NULL ) {
                fprintf ( stderr, "Error: File with ID %ld not found\n", file_id );
                ret = EXIT_FAILURE; goto cleanup;
            }
        } else if ( optind < sub_argc ) {
            item = find_file_by_name ( &config, sub_argv[optind] );
            if ( item == NULL ) {
                fprintf ( stderr, "Error: File '%s' not found\n", sub_argv[optind] );
                ret = EXIT_FAILURE; goto cleanup;
            }
        } else {
            fprintf ( stderr, "Error: file requires <name[.ext]> or --id <N>\n" );
            ret = EXIT_FAILURE; goto cleanup;
        }
        ret = cmd_file ( &config, item, output_format );

    } else if ( strcmp ( subcmd, "dump-block" ) == 0 ) {
        /* dump-block N [bytes] [--noinv] [--dump-charset MODE] */
        static struct option dblk_opts[] = {
            { "noinv",        no_argument,       NULL, 'n' },
            { "dump-charset", required_argument, NULL, 'd' },
            { NULL,           0,                 NULL,  0  }
        };
        int noinv = 0;
        en_MZDSK_HEXDUMP_CHARSET dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", dblk_opts, NULL ) ) != -1 ) {
            if ( c == 'n' ) {
                noinv = 1;
            } else if ( c == 'd' ) {
                if ( strcmp ( optarg, "raw" ) == 0 )          dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
                else if ( strcmp ( optarg, "eu" ) == 0 )      dump_charset = MZDSK_HEXDUMP_CHARSET_EU;
                else if ( strcmp ( optarg, "jp" ) == 0 )      dump_charset = MZDSK_HEXDUMP_CHARSET_JP;
                else if ( strcmp ( optarg, "utf8-eu" ) == 0 ) dump_charset = MZDSK_HEXDUMP_CHARSET_UTF8_EU;
                else if ( strcmp ( optarg, "utf8-jp" ) == 0 ) dump_charset = MZDSK_HEXDUMP_CHARSET_UTF8_JP;
                else {
                    fprintf ( stderr, "Error: Unknown dump-charset '%s' (use raw, eu, jp, utf8-eu or utf8-jp)\n", optarg );
                    ret = EXIT_FAILURE; goto cleanup;
                }
            } else { ret = EXIT_FAILURE; goto cleanup; }
        }
        if ( optind >= sub_argc ) {
            fprintf ( stderr, "Error: dump-block requires N [bytes]\n" );
            ret = EXIT_FAILURE; goto cleanup;
        }
        unsigned long block_num;
        if ( parse_number ( sub_argv[optind], &block_num ) != EXIT_SUCCESS || block_num >= FSMRS_COUNT_BLOCKS ) {
            fprintf ( stderr, "Error: Invalid block number '%s' (max %u)\n", sub_argv[optind], FSMRS_COUNT_BLOCKS - 1 );
            ret = EXIT_FAILURE; goto cleanup;
        }
        size_t blk_size = 0;
        if ( optind + 1 < sub_argc ) {
            unsigned long val;
            if ( parse_number ( sub_argv[optind + 1], &val ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Invalid size '%s'\n", sub_argv[optind + 1] );
                ret = EXIT_FAILURE; goto cleanup;
            }
            blk_size = (size_t) val;
        }
        en_MZDSK_RES err2 = cmd_dump_block ( &config, (uint16_t) block_num, blk_size, noinv, dump_charset );
        if ( err2 != MZDSK_RES_OK ) ret = EXIT_FAILURE;

    } else if ( strcmp ( subcmd, "get-block" ) == 0 ) {
        /* get-block N <file> [bytes] [--noinv] */
        static struct option gblk_opts[] = {
            { "noinv", no_argument, NULL, 'n' },
            { NULL,    0,           NULL,  0  }
        };
        int noinv = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", gblk_opts, NULL ) ) != -1 ) {
            if ( c == 'n' ) noinv = 1;
            else { ret = EXIT_FAILURE; goto cleanup; }
        }
        int pos_remaining = sub_argc - optind;
        if ( pos_remaining < 2 ) {
            fprintf ( stderr, "Error: get-block requires N <file> [bytes]\n" );
            ret = EXIT_FAILURE; goto cleanup;
        }
        unsigned long block_num;
        if ( parse_number ( sub_argv[optind], &block_num ) != EXIT_SUCCESS || block_num >= FSMRS_COUNT_BLOCKS ) {
            fprintf ( stderr, "Error: Invalid block number '%s'\n", sub_argv[optind] );
            ret = EXIT_FAILURE; goto cleanup;
        }
        const char *blk_file = sub_argv[optind + 1];
        size_t blk_size = 0;
        if ( pos_remaining >= 3 ) {
            unsigned long val;
            if ( parse_number ( sub_argv[optind + 2], &val ) == EXIT_SUCCESS ) {
                blk_size = (size_t) val;
            }
        }
        en_MZDSK_RES err2 = cmd_get_block ( &config, (uint16_t) block_num, blk_file, blk_size, noinv );
        if ( err2 != MZDSK_RES_OK ) ret = EXIT_FAILURE;

    } else if ( strcmp ( subcmd, "put-block" ) == 0 ) {
        /* put-block N <file> [bytes] [offset] [--noinv] */
        static struct option pblk_opts[] = {
            { "noinv", no_argument, NULL, 'n' },
            { NULL,    0,           NULL,  0  }
        };
        int noinv = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", pblk_opts, NULL ) ) != -1 ) {
            if ( c == 'n' ) noinv = 1;
            else { ret = EXIT_FAILURE; goto cleanup; }
        }
        int pos_remaining = sub_argc - optind;
        if ( pos_remaining < 2 ) {
            fprintf ( stderr, "Error: put-block requires N <file> [bytes] [offset]\n" );
            ret = EXIT_FAILURE; goto cleanup;
        }
        unsigned long block_num;
        if ( parse_number ( sub_argv[optind], &block_num ) != EXIT_SUCCESS || block_num >= FSMRS_COUNT_BLOCKS ) {
            fprintf ( stderr, "Error: Invalid block number '%s'\n", sub_argv[optind] );
            ret = EXIT_FAILURE; goto cleanup;
        }
        const char *blk_file = sub_argv[optind + 1];
        size_t blk_size = 0, blk_offset = 0;
        if ( pos_remaining >= 3 ) {
            unsigned long val;
            if ( parse_number ( sub_argv[optind + 2], &val ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Invalid size '%s'\n", sub_argv[optind + 2] );
                ret = EXIT_FAILURE; goto cleanup;
            }
            blk_size = (size_t) val;
        }
        if ( pos_remaining >= 4 ) {
            unsigned long val;
            if ( parse_number ( sub_argv[optind + 3], &val ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Invalid offset '%s'\n", sub_argv[optind + 3] );
                ret = EXIT_FAILURE; goto cleanup;
            }
            blk_offset = (size_t) val;
        }
        en_MZDSK_RES err2 = cmd_put_block ( &config, (uint16_t) block_num, blk_file,
                                              blk_size, blk_offset, noinv );
        if ( err2 != MZDSK_RES_OK ) ret = EXIT_FAILURE;

    } else if ( strcmp ( subcmd, "defrag" ) == 0 ) {

        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'defrag' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            ret = EXIT_FAILURE;
            goto cleanup;
        }
        /* Audit M-17 */
        if ( !confirm_destructive_op ( "defragment MRS (reorganize all file blocks)" ) ) {
            fprintf ( stderr, "Aborted.\n" );
            ret = EXIT_FAILURE;
            goto cleanup;
        }
        ret = cmd_defrag ( &disc, fat_block );

    } else {
        fprintf ( stderr, "Error: Unknown command '%s'\n\n", subcmd );
        print_usage ( stderr );
        ret = EXIT_FAILURE;
    }

cleanup:
    /* Uložení změn do souboru při úspěšné zápisové operaci */
    if ( is_write_op && ret == EXIT_SUCCESS ) {
        en_MZDSK_RES save_err = mzdsk_disc_save ( &disc );
        if ( save_err != MZDSK_RES_OK ) {
            fprintf ( stderr, "Error: Could not save DSK file: %s\n", mzdsk_get_error ( save_err ) );
            ret = EXIT_FAILURE;
        }
    }

    mzdsk_disc_close ( &disc );
    return ret;
}
