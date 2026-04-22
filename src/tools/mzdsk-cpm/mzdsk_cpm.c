/**
 * @file   mzdsk_cpm.c
 * @brief  CLI nástroj pro kompletní správu CP/M souborového systému na DSK obrazech.
 *
 * Podporované příkazy:
 * - dir              - výpis adresáře
 * - dir --ex         - rozšířený výpis s atributy
 * - dir --raw        - surový výpis 32B adresářových položek
 * - file <name.ext>  - informace o souboru
 * - get <name.ext> <output> - extrakce souboru (raw binární)
 * - get --mzf <name.ext> <output.mzf> [--addr N] - extrakce jako MZF CPM-IC
 * - get --all <dir> [options] - extrakce všech souborů do adresáře
 * - put <input> <name.ext>  - vložení souboru (raw binární)
 * - put --mzf <input.mzf> [name.ext]  - vložení z MZF CPM-IC
 * - era <name.ext>   - smazání souboru
 * - ren <old.ext> <new.ext> - přejmenování souboru
 * - chuser <name.ext> <new-user> - změna user number existujícího souboru
 * - attr <name.ext> [+|-][R|S|A] - nastavení atributů
 * - map              - alokační mapa
 * - free             - volné místo
 * - format           - inicializace prázdného adresáře
 *
 * Globální volby:
 * - --format sd|hd   - formát disku (výchozí: autodetekce)
 * - --user N         - uživatelské číslo (0-15, výchozí: 0 pro file operace, všichni pro dir)
 * - --ro             - vynucený read-only režim
 * - --output FMT     - výstupní formát: text (výchozí), json, csv
 * - --version        - verze programu
 * - --lib-versions   - verze knihoven
 *
 * Custom DPB parametry (přepíšou hodnoty z presetu):
 * - --spt N          - logické (128B) sektory na stopu
 * - --bsh N          - block shift factor (3-7)
 * - --exm N          - extent mask
 * - --dsm N          - celkový počet bloků - 1
 * - --drm N          - počet adresářových položek - 1
 * - --al0 N          - alokační bitmapa adresáře byte 0 (hex: 0xC0)
 * - --al1 N          - alokační bitmapa adresáře byte 1
 * - --off N          - počet rezervovaných stop
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#else
#define _mkdir(path) mkdir ( (path), 0755 )
#endif

#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_cpm/mzdsk_cpm_mzf.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/mzf/mzf.h"
#include "libs/mzdsk_hexdump/mzdsk_hexdump.h"
#include "libs/output_format/output_format.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/dsk/dsk_tools.h"
#include "tools/common/mzdisk_cli_version.h"


/** @brief Verze nástroje mzdsk-cpm. */
#define MZDSK_CPM_TOOL_VERSION "1.19.1"


/**
 * @brief Globální flag `--overwrite` (audit M-16).
 *
 * Default: 0 = `get`/`get-block` odmítne přepsat existující výstupní soubor.
 * S `--overwrite` = 1: dovoluje přepis. Settuje se z getopt v main().
 */
static int g_allow_overwrite = 0;


/**
 * @brief Globální flag `--yes`/`-y` (audit M-17).
 *
 * Default: 0 = pokud je stdin TTY, destruktivní operace (`format`,
 * `defrag`) zobrazí interaktivní prompt. Non-TTY (skripty, pipes)
 * pokračuje bez promptu pro zpětnou kompatibilitu.
 * S `--yes` = 1: prompt se přeskočí i v TTY.
 */
static int g_assume_yes = 0;


/**
 * @brief Interaktivní potvrzení destruktivní operace (audit M-17).
 *
 * @param op_name  Popis operace (např. "format", "defrag").
 * @return 1 pokud uživatel potvrdil (nebo --yes/non-TTY), 0 pokud odmítl.
 */
static int confirm_destructive_op ( const char *op_name ) {
    if ( g_assume_yes ) return 1;
    if ( !isatty ( fileno ( stdin ) ) ) return 1; /* non-TTY = skript, pokračuj */
    fprintf ( stderr, "This will %s the disk. Are you sure? [y/N] ", op_name );
    fflush ( stderr );
    char buf[16];
    if ( fgets ( buf, sizeof ( buf ), stdin ) == NULL ) return 0;
    return ( buf[0] == 'y' || buf[0] == 'Y' );
}


/**
 * @brief Zkontroluje, zda lze bezpečně zapsat na výstupní cestu (audit M-16).
 *
 * Pokud soubor existuje a `g_allow_overwrite == 0`, vrátí chybu.
 * Prázdná cesta a `-` (stdout) jsou vždy povolené.
 *
 * @param path  Cesta k výstupnímu souboru.
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


/**
 * @brief Vypočítá maximální velikost dat na disku z DPB.
 *
 * Vrací (dsm + 1) * block_size, což je celková kapacita datové oblasti
 * disku. Používá se jako horní mez pro alokaci bufferu při čtení/zápisu
 * souborů.
 *
 * @param dpb Disk Parameter Block.
 * @return Maximální velikost dat v bajtech.
 */
#define CPM_DISK_CAPACITY(dpb) ( (uint32_t) ( (dpb)->dsm + 1 ) * (dpb)->block_size )

/** @brief Maximální počet souborů ve výpisu adresáře. */
#define MAX_DIR_FILES   256

/** @brief Maximální délka kompletní výstupní cesty.
 *  Musí pojmout dir + "/userNN/" + "FILENAME~999.EXT.mzf\0".
 *  Sjednoceno s mzdsk-fsmz a mzdsk-mrs (audit L-13). */
#define MAX_PATH_LEN    1024

/** @brief Maximální délka bázového adresáře (dir + volitelný "/userNN").
 *  Ponechává 64 bajtů pro jméno souboru a suffix v MAX_PATH_LEN. */
#define MAX_BASE_DIR_LEN  ( MAX_PATH_LEN - 64 )


/**
 * @brief Strategie řešení duplicitních jmen při hromadné extrakci.
 *
 * Určuje chování příkazu get --all, pokud cílový soubor
 * se stejným jménem již existuje v cílovém adresáři.
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
 * @brief Parsuje jméno souboru ve formátu "NAME.EXT" na jméno a příponu.
 *
 * Rozdělí vstupní řetězec na jméno (max 8 znaků) a příponu (max 3 znaky)
 * podle tečky. Pokud tečka chybí, přípona je prázdná.
 *
 * @param[in]  input     Vstupní řetězec ve formátu "NAME.EXT". Nesmí být NULL.
 * @param[out] name      Výstupní buffer pro jméno (min 9 bajtů). Nesmí být NULL.
 * @param[out] ext       Výstupní buffer pro příponu (min 4 bajty). Nesmí být NULL.
 * @param[out] truncated Pokud != NULL, nastaví se na 1 když bylo jméno nebo
 *                       přípona zkrácena (vstup delší než 8/3 znaků). Nenulluje
 *                       se - volající musí inicializovat na 0.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě (prázdné jméno).
 *
 * @pre input != NULL && name != NULL && ext != NULL
 * @post name a ext jsou null-terminated řetězce
 */
static int parse_filename ( const char *input, char *name, char *ext, int *truncated ) {

    name[0] = '\0';
    ext[0] = '\0';

    /* Najdeme tečku */
    const char *dot = strchr ( input, '.' );

    if ( dot != NULL ) {
        /* Jméno před tečkou */
        size_t name_len = (size_t) ( dot - input );
        if ( name_len > 8 ) { name_len = 8; if ( truncated ) *truncated = 1; }
        if ( name_len == 0 ) return EXIT_FAILURE;
        memcpy ( name, input, name_len );
        name[name_len] = '\0';

        /* Přípona za tečkou */
        const char *ext_src = dot + 1;
        size_t ext_len = strlen ( ext_src );
        if ( ext_len > 3 ) { ext_len = 3; if ( truncated ) *truncated = 1; }
        memcpy ( ext, ext_src, ext_len );
        ext[ext_len] = '\0';
    } else {
        /* Bez tečky - celé je jméno */
        size_t name_len = strlen ( input );
        if ( name_len > 8 ) { name_len = 8; if ( truncated ) *truncated = 1; }
        if ( name_len == 0 ) return EXIT_FAILURE;
        memcpy ( name, input, name_len );
        name[name_len] = '\0';
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Zapíše do stderr warning o zkrácení jména v CP/M.
 *
 * Vytvoří normalizovaný tvar "NAME.EXT" (bez prázdné přípony se zobrazí
 * jen "NAME") a vypíše varování ve formátu "Warning: Name 'ORIG' was
 * truncated to 'NAME.EXT'".
 *
 * @param[in] orig Původní jméno zadané uživatelem (null-terminated).
 * @param[in] name Zkrácené jméno (null-terminated, max 8 znaků).
 * @param[in] ext  Zkrácená přípona (null-terminated, max 3 znaky).
 */
static void warn_cpm_truncation ( const char *orig, const char *name, const char *ext ) {
    if ( ext[0] != '\0' ) {
        fprintf ( stderr, "Warning: Name '%s' was truncated to '%s.%s'\n", orig, name, ext );
    } else {
        fprintf ( stderr, "Warning: Name '%s' was truncated to '%s'\n", orig, name );
    }
}


/**
 * @brief Přeloží en_MZDSK_NAMEVAL na stderr chybovou hlášku.
 *
 * Vypíše jednotný formát "Error: Name 'X' ..." odpovídající konkrétní
 * chybě validátoru. Použito v striktním režimu `--name`, který nikdy
 * nezkracuje ani normalizuje - při porušení vrací chybu.
 *
 * @param[in] orig   Původní uživatelský vstup pro citaci v hlášce.
 * @param[in] code   Kód chyby vrácený z mzdsk_validate_83_name.
 * @param[in] bad    Při MZDSK_NAMEVAL_BAD_CHAR první zakázaný znak.
 *
 * @pre code != MZDSK_NAMEVAL_OK (funkce se volá jen při chybě).
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
 * @brief Vrátí textový popis atributů.
 *
 * Sestaví řetězec ve formátu "R/O SYS ARC" z bitové kombinace atributů.
 * Neaktivní atributy se zobrazí jako "---".
 *
 * @param[in]  attrs Kombinace en_MZDSK_CPM_ATTR.
 * @param[out] buf Výstupní buffer (min 16 bajtů).
 *
 * @pre buf != NULL
 * @post buf obsahuje null-terminated řetězec s popisem atributů.
 */
static void format_attributes ( uint8_t attrs, char *buf, size_t buf_size ) {

    /* Audit M-29: předchozí verze používala `strcat` bez znalosti
     * velikosti cílového bufferu. Všechna volací místa používají
     * `attr_str[16]` což je přesně 12+terminátor+rezerva, takže
     * aktuálně safe, ale API bylo fragilní vůči budoucím změnám.
     * Nyní snprintf s explicitním size. */
    int n = snprintf ( buf, buf_size, "%s%s%s",
                       ( attrs & MZDSK_CPM_ATTR_READ_ONLY ) ? "R/O " : "--- ",
                       ( attrs & MZDSK_CPM_ATTR_SYSTEM )    ? "SYS " : "--- ",
                       ( attrs & MZDSK_CPM_ATTR_ARCHIVED )  ? "ARC"  : "---" );
    (void) n;
}




/* =========================================================================
 * Příkazy
 * ========================================================================= */


/**
 * @brief Zobrazí informace o CP/M disku.
 *
 * Vypíše detekovaný formát, parametry DPB a volné místo.
 *
 * @param disc Ukazatel na otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param cpm_format Detekovaný CP/M formát.
 *
 * @return MZDSK_RES_OK při úspěchu.
 */
static en_MZDSK_RES cmd_disc_info ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                     en_MZDSK_CPM_FORMAT cpm_format ) {

    const char *fmt_name;
    switch ( cpm_format ) {
        case MZDSK_CPM_FORMAT_HD: fmt_name = "HD (18x512B, Lucky-Soft)"; break;
        default:                  fmt_name = "SD (9x512B, Lamac)"; break;
    }

    printf ( "\nCP/M Disk info:\n" );
    printf ( "\tFormat:     %s\n", fmt_name );
    printf ( "\tBlock size: %d B\n", dpb->block_size );
    printf ( "\tBlocks:     %d (DSM=%d)\n", dpb->dsm + 1, dpb->dsm );
    printf ( "\tDir slots:  %d (DRM=%d)\n", dpb->drm + 1, dpb->drm );

    /* Detailní rozpis obsazení slotů. Selhání čtení není fatální pro info
     * výpis - jen přeskočíme detaily. */
    st_MZDSK_CPM_DIR_STATS dstats;
    if ( mzdsk_cpm_get_dir_stats ( disc, dpb, &dstats ) == MZDSK_RES_OK ) {
        printf ( "\t            used=%u, free=%u, blocked=%u\n",
                 dstats.used, dstats.free, dstats.blocked );
    }

    printf ( "\tReserved:   %d tracks\n", dpb->off );

    uint32_t free_bytes = mzdsk_cpm_free_space ( disc, dpb );
    uint32_t total_bytes = (uint32_t) ( dpb->dsm + 1 ) * dpb->block_size;
    uint32_t used_bytes = total_bytes - free_bytes;

    printf ( "\tTotal:      %lu kB\n", (unsigned long) ( total_bytes / 1024 ) );
    printf ( "\tUsed:       %lu kB\n", (unsigned long) ( used_bytes / 1024 ) );
    printf ( "\tFree:       %lu kB\n", (unsigned long) ( free_bytes / 1024 ) );
    printf ( "\n" );

    return MZDSK_RES_OK;
}


/**
 * @brief Výpis adresáře - základní verze.
 *
 * Vypíše seznam souborů s uživatelským číslem, jménem, příponou a velikostí.
 * Pokud je filter_user >= 0, zobrazí pouze soubory daného uživatele.
 * Pokud je filter_user < 0, zobrazí soubory všech uživatelů.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param filter_user Filtr uživatele: 0-15 = pouze daný uživatel, < 0 = všichni.
 * @param output_format Výstupní formát (text, json, csv).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_dir ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                               int filter_user, en_OUTFMT output_format ) {

    st_MZDSK_CPM_FILE_INFO files[MAX_DIR_FILES];
    int count = mzdsk_cpm_read_directory ( disc, dpb, files, MAX_DIR_FILES );

    if ( count < 0 ) {
        fprintf ( stderr, "Error: Failed to read directory\n" );
        return MZDSK_RES_DSK_ERROR;
    }

    uint32_t free_bytes = mzdsk_cpm_free_space ( disc, dpb );

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nCP/M Directory:\n\n" );

        /* Spočítáme filtrované soubory */
        int shown = 0;
        int i;
        for ( i = 0; i < count; i++ ) {
            if ( filter_user >= 0 && files[i].user != (uint8_t) filter_user ) continue;
            shown++;
        }

        if ( shown == 0 ) {
            printf ( "  No files found.\n\n" );
            return MZDSK_RES_OK;
        }

        printf ( "  User  Name         Ext   Size\n" );
        printf ( "  ----  ----------   ---   --------\n" );

        for ( i = 0; i < count; i++ ) {
            if ( filter_user >= 0 && files[i].user != (uint8_t) filter_user ) continue;
            printf ( "  %3d   %-8s     %-3s   %lu B\n",
                     files[i].user,
                     files[i].filename,
                     files[i].extension,
                     (unsigned long) files[i].size );
        }

        printf ( "\n  %d file(s), %lu kB free\n\n", shown, (unsigned long) ( free_bytes / 1024 ) );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = { "user", "name", "ext", "size" };
        outfmt_csv_header ( &ctx, csv_hdr, 4 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "cpm" );
        outfmt_array_begin ( &ctx, "files" );
        int i;
        int shown = 0;
        for ( i = 0; i < count; i++ ) {
            if ( filter_user >= 0 && files[i].user != (uint8_t) filter_user ) continue;
            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "user", files[i].user );
            outfmt_field_str ( &ctx, "name", files[i].filename );
            outfmt_field_str ( &ctx, "ext", files[i].extension );
            outfmt_field_uint ( &ctx, "size", (unsigned long) files[i].size );
            outfmt_item_end ( &ctx );
            shown++;
        }
        outfmt_array_end ( &ctx );

        outfmt_kv_int ( &ctx, "total_files", shown );
        outfmt_kv_uint ( &ctx, "free_bytes", (unsigned long) free_bytes );
        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Rozšířený výpis adresáře s atributy.
 *
 * Vypíše soubory včetně atributů (R/O, SYS, ARC), počtu extentů
 * a indexu první adresářové položky.
 * Pokud je filter_user >= 0, zobrazí pouze soubory daného uživatele.
 * Pokud je filter_user < 0, zobrazí soubory všech uživatelů.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param filter_user Filtr uživatele: 0-15 = pouze daný uživatel, < 0 = všichni.
 * @param output_format Výstupní formát (text, json, csv).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_dir_ex ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                  int filter_user, en_OUTFMT output_format ) {

    st_MZDSK_CPM_FILE_INFO_EX files[MAX_DIR_FILES];
    int count = mzdsk_cpm_read_directory_ex ( disc, dpb, files, MAX_DIR_FILES );

    if ( count < 0 ) {
        fprintf ( stderr, "Error: Failed to read directory\n" );
        return MZDSK_RES_DSK_ERROR;
    }

    uint32_t free_bytes = mzdsk_cpm_free_space ( disc, dpb );

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nCP/M Directory (extended):\n\n" );

        /* Spočítáme filtrované soubory */
        int shown = 0;
        int i;
        for ( i = 0; i < count; i++ ) {
            if ( filter_user >= 0 && files[i].user != (uint8_t) filter_user ) continue;
            shown++;
        }

        if ( shown == 0 ) {
            printf ( "  No files found.\n\n" );
            return MZDSK_RES_OK;
        }

        printf ( "  User  Name         Ext   Size       Attr         Ext  DirIdx\n" );
        printf ( "  ----  ----------   ---   --------   ----------   ---  ------\n" );

        for ( i = 0; i < count; i++ ) {
            if ( filter_user >= 0 && files[i].user != (uint8_t) filter_user ) continue;
            char attr_str[16];
            format_attributes (
                (uint8_t) ( ( files[i].read_only ? MZDSK_CPM_ATTR_READ_ONLY : 0 ) |
                             ( files[i].system    ? MZDSK_CPM_ATTR_SYSTEM    : 0 ) |
                             ( files[i].archived  ? MZDSK_CPM_ATTR_ARCHIVED  : 0 ) ),
                attr_str, sizeof ( attr_str ) );

            printf ( "  %3d   %-8s     %-3s   %7lu B  %s  %3d  %5d\n",
                     files[i].user,
                     files[i].filename,
                     files[i].extension,
                     (unsigned long) files[i].size,
                     attr_str,
                     files[i].extent_count,
                     files[i].first_dir_index );
        }

        printf ( "\n  %d file(s), %lu kB free\n\n", shown, (unsigned long) ( free_bytes / 1024 ) );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = {
            "user", "name", "ext", "size", "readonly", "system", "archived",
            "extent_count", "dir_index"
        };
        outfmt_csv_header ( &ctx, csv_hdr, 9 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "cpm" );
        outfmt_array_begin ( &ctx, "files" );
        int i;
        int shown = 0;
        for ( i = 0; i < count; i++ ) {
            if ( filter_user >= 0 && files[i].user != (uint8_t) filter_user ) continue;
            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "user", files[i].user );
            outfmt_field_str ( &ctx, "name", files[i].filename );
            outfmt_field_str ( &ctx, "ext", files[i].extension );
            outfmt_field_uint ( &ctx, "size", (unsigned long) files[i].size );
            outfmt_field_bool ( &ctx, "readonly", files[i].read_only );
            outfmt_field_bool ( &ctx, "system", files[i].system );
            outfmt_field_bool ( &ctx, "archived", files[i].archived );
            outfmt_field_int ( &ctx, "extent_count", files[i].extent_count );
            outfmt_field_int ( &ctx, "dir_index", files[i].first_dir_index );
            outfmt_item_end ( &ctx );
            shown++;
        }
        outfmt_array_end ( &ctx );

        outfmt_kv_int ( &ctx, "total_files", shown );
        outfmt_kv_uint ( &ctx, "free_bytes", (unsigned long) free_bytes );
        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Surový výpis adresářových položek.
 *
 * Zobrazí všechny 32B adresářové položky včetně smazaných.
 * Každá položka obsahuje user, jméno, příponu, extent, s2, rc
 * a kompletní 16B alokační mapu.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param output_format Výstupní formát (text, json, csv).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_dir_raw ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                   en_OUTFMT output_format ) {

    st_MZDSK_CPM_DIRENTRY entries[MAX_DIR_FILES];
    int count = mzdsk_cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );

    if ( count < 0 ) {
        fprintf ( stderr, "Error: Failed to read directory\n" );
        return MZDSK_RES_DSK_ERROR;
    }

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nCP/M Raw Directory (%d entries):\n\n", count );
        printf ( "  Idx  User  Name      Ext  Ex S2 RC  Alloc\n" );
        printf ( "  ---  ----  --------  ---  -- -- --  ------\n" );

        int i;
        for ( i = 0; i < count; i++ ) {
            const st_MZDSK_CPM_DIRENTRY *e = &entries[i];

            /* Jméno - maskujeme bit 7 */
            char fname[9];
            int j;
            for ( j = 0; j < 8; j++ ) fname[j] = (char) ( e->fname[j] & 0x7F );
            fname[8] = '\0';

            /* Přípona (maskujeme bity 7) */
            char ext[4];
            for ( j = 0; j < 3; j++ ) ext[j] = (char) ( e->ext[j] & 0x7F );
            ext[3] = '\0';

            /* Alokační mapa (prvních 8 bajtů) */
            printf ( "  %3d  0x%02X  %-8s  %-3s  %2d %2d %3d ",
                     i, e->user, fname, ext, e->extent, e->s2, e->rc );

            for ( j = 0; j < 8; j++ ) {
                printf ( "%02X ", e->alloc[j] );
            }
            printf ( "...\n" );
        }

        printf ( "\n" );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = { "idx", "user", "name", "ext", "extent", "s2", "rc", "alloc" };
        outfmt_csv_header ( &ctx, csv_hdr, 8 );

        outfmt_doc_begin ( &ctx );
        outfmt_array_begin ( &ctx, "entries" );

        int i;
        for ( i = 0; i < count; i++ ) {
            const st_MZDSK_CPM_DIRENTRY *e = &entries[i];

            char fname[9];
            int j;
            for ( j = 0; j < 8; j++ ) fname[j] = (char) ( e->fname[j] & 0x7F );
            fname[8] = '\0';

            char ext[4];
            for ( j = 0; j < 3; j++ ) ext[j] = (char) ( e->ext[j] & 0x7F );
            ext[3] = '\0';

            /* Alokační mapa jako hex řetězec - snprintf pro defenzivní
             * coding (audit M-28): buffer má přesně 49 B, výstup 48 B
             * (16 × 3), aktuálně safe, ale snprintf je robustnější
             * vůči budoucím změnám. */
            char alloc_str[49];
            for ( j = 0; j < 16; j++ ) {
                snprintf ( &alloc_str[j * 3], sizeof ( alloc_str ) - j * 3,
                           "%02X ", e->alloc[j] );
            }
            alloc_str[47] = '\0';

            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "idx", i );
            outfmt_field_int ( &ctx, "user", e->user );
            outfmt_field_str ( &ctx, "name", fname );
            outfmt_field_str ( &ctx, "ext", ext );
            outfmt_field_int ( &ctx, "extent", e->extent );
            outfmt_field_int ( &ctx, "s2", e->s2 );
            outfmt_field_int ( &ctx, "rc", e->rc );
            outfmt_field_str ( &ctx, "alloc", alloc_str );
            outfmt_item_end ( &ctx );
        }

        outfmt_array_end ( &ctx );
        outfmt_kv_int ( &ctx, "total_entries", count );
        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zobrazí informace o konkrétním souboru.
 *
 * Vypíše velikost, atributy, počet extentů a detaily adresářových
 * položek. Po výpisu zkontroluje, zda v sekvenci extentů nechybí
 * některý (počínaje extentem 0). Pokud ano, vypíše varování na stderr.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param filename Jméno souboru ve formátu "NAME.EXT".
 * @param user Číslo uživatele.
 * @param output_format Výstupní formát (text, json, csv).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @par Vedlejší efekty:
 * - Na stderr vypíše varování pokud soubor má chybějící extenty.
 */
static en_MZDSK_RES cmd_file_info ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                     const char *filename, uint8_t user,
                                     en_OUTFMT output_format ) {

    char name[9], ext[4];
    if ( parse_filename ( filename, name, ext, NULL ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Invalid filename '%s'\n", filename );
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Rozšířený výpis adresáře pro nalezení souboru */
    st_MZDSK_CPM_FILE_INFO_EX files[MAX_DIR_FILES];
    int count = mzdsk_cpm_read_directory_ex ( disc, dpb, files, MAX_DIR_FILES );
    if ( count < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Převedeme jméno na velká písmena pro porovnání */
    char uname[9], uext[4];
    int j;
    for ( j = 0; name[j] && j < 8; j++ ) uname[j] = (char) toupper ( (unsigned char) name[j] );
    uname[j] = '\0';
    for ( j = 0; ext[j] && j < 3; j++ ) uext[j] = (char) toupper ( (unsigned char) ext[j] );
    uext[j] = '\0';

    int i;
    for ( i = 0; i < count; i++ ) {
        if ( files[i].user == user &&
             strcmp ( files[i].filename, uname ) == 0 &&
             strcmp ( files[i].extension, uext ) == 0 ) {

            char attr_str[16];
            format_attributes (
                (uint8_t) ( ( files[i].read_only ? MZDSK_CPM_ATTR_READ_ONLY : 0 ) |
                             ( files[i].system    ? MZDSK_CPM_ATTR_SYSTEM    : 0 ) |
                             ( files[i].archived  ? MZDSK_CPM_ATTR_ARCHIVED  : 0 ) ),
                attr_str, sizeof ( attr_str ) );

            if ( output_format == OUTFMT_TEXT ) {
                printf ( "\nFile: %s.%s (user %d)\n", files[i].filename, files[i].extension, files[i].user );
                printf ( "\tSize:       %lu B\n", (unsigned long) files[i].size );
                printf ( "\tAttributes: %s\n", attr_str );
                printf ( "\tExtents:    %d\n", files[i].extent_count );
                printf ( "\tDir index:  %d\n", files[i].first_dir_index );

                /* Výpis všech raw dir položek (extentů) souboru */
                st_MZDSK_CPM_DIRENTRY raw_entries[MAX_DIR_FILES];
                int raw_count = mzdsk_cpm_read_raw_directory ( disc, dpb, raw_entries, dpb->drm + 1 );
                if ( raw_count > 0 ) {
                    int alloc_per_ext = ( dpb->dsm <= 255 ) ? 16 : 8;

                    printf ( "\n\tDirectory entries:\n\n" );
                    printf ( "\t  Idx  Ex S2  RC  Alloc blocks\n" );
                    printf ( "\t  ---  -- --  --  ------------\n" );

                    for ( int r = 0; r < raw_count; r++ ) {
                        const st_MZDSK_CPM_DIRENTRY *e = &raw_entries[r];

                        /* Filtruj pouze extenty stejného souboru */
                        if ( e->user == user ) {
                            char rf[9], re[4];
                            for ( int k = 0; k < 8; k++ ) rf[k] = (char) ( e->fname[k] & 0x7F );
                            rf[8] = '\0';
                            for ( int k = 0; k < 3; k++ ) re[k] = (char) ( e->ext[k] & 0x7F );
                            re[3] = '\0';

                            /* Ořežeme trailing mezery pro porovnání */
                            while ( rf[0] && rf[strlen ( rf ) - 1] == ' ' ) rf[strlen ( rf ) - 1] = '\0';
                            while ( re[0] && re[strlen ( re ) - 1] == ' ' ) re[strlen ( re ) - 1] = '\0';

                            if ( strcmp ( rf, uname ) != 0 || strcmp ( re, uext ) != 0 ) continue;

                            printf ( "\t  %3d  %2d %2d  %2d ", r, e->extent, e->s2, e->rc );

                            for ( int a = 0; a < alloc_per_ext; a++ ) {
                                uint16_t blk;
                                if ( dpb->dsm <= 255 ) {
                                    blk = e->alloc[a];
                                } else {
                                    blk = (uint16_t) ( e->alloc[a * 2] | ( e->alloc[a * 2 + 1] << 8 ) );
                                }
                                if ( blk != 0 ) {
                                    printf ( "%u ", blk );
                                }
                            }
                            printf ( "\n" );
                        }
                    }
                }

                printf ( "\n" );
            } else {
                st_OUTFMT_CTX ctx;
                outfmt_init ( &ctx, output_format );

                /* Základní informace - CSV hlavička */
                static const char *csv_hdr[] = {
                    "user", "name", "ext", "size", "readonly", "system",
                    "archived", "extent_count", "dir_index"
                };
                outfmt_csv_header ( &ctx, csv_hdr, 9 );

                outfmt_doc_begin ( &ctx );
                outfmt_kv_str ( &ctx, "filesystem", "cpm" );
                outfmt_array_begin ( &ctx, "files" );
                outfmt_item_begin ( &ctx );
                outfmt_field_int ( &ctx, "user", files[i].user );
                outfmt_field_str ( &ctx, "name", files[i].filename );
                outfmt_field_str ( &ctx, "ext", files[i].extension );
                outfmt_field_uint ( &ctx, "size", (unsigned long) files[i].size );
                outfmt_field_bool ( &ctx, "readonly", files[i].read_only );
                outfmt_field_bool ( &ctx, "system", files[i].system );
                outfmt_field_bool ( &ctx, "archived", files[i].archived );
                outfmt_field_int ( &ctx, "extent_count", files[i].extent_count );
                outfmt_field_int ( &ctx, "dir_index", files[i].first_dir_index );
                outfmt_item_end ( &ctx );
                outfmt_array_end ( &ctx );

                outfmt_doc_end ( &ctx );
            }

            /* Kontrola chybějících extentů v sekvenci */
            st_MZDSK_CPM_EXTENT_CHECK ext_check;
            if ( mzdsk_cpm_check_extents ( disc, dpb, uname, uext, user,
                                            &ext_check ) == 0
                 && ext_check.count > 0 ) {
                fprintf ( stderr, "Warning: File '%s.%s' has missing extent(s): ",
                          uname, uext );
                for ( int m = 0; m < ext_check.count; m++ ) {
                    fprintf ( stderr, "%u%s", ext_check.missing[m],
                              ( m < ext_check.count - 1 ) ? ", " : "" );
                }
                fprintf ( stderr, "\n" );
                fprintf ( stderr, "Warning: Reported size %lu B may be inaccurate"
                          " (file is damaged on disk)\n",
                          (unsigned long) files[i].size );
            }

            return MZDSK_RES_OK;
        }
    }

    fprintf ( stderr, "Error: File '%s' not found (user %d)\n", filename, user );
    return MZDSK_RES_FILE_NOT_FOUND;
}


/**
 * @brief Extrahuje soubor z CP/M disku do hostitelského souboru.
 *
 * Přečte data souboru ze všech fyzicky přítomných extentů a zapíše
 * je do výstupního souboru. Po extrakci zkontroluje, zda v sekvenci
 * extentů nechybí některý. Pokud ano, vypíše varování na stderr
 * s informací o chybějících extentech a skutečném počtu extrahovaných
 * bajtů.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param cpm_filename Jméno CP/M souboru ve formátu "NAME.EXT".
 * @param output_path Cesta k výstupnímu souboru na hostiteli.
 * @param user Číslo uživatele.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @par Vedlejší efekty:
 * - Vytvoří/přepíše výstupní soubor na hostiteli.
 * - Na stderr vypíše varování pokud soubor má chybějící extenty.
 */
static en_MZDSK_RES cmd_get ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                               const char *cpm_filename, const char *output_path, uint8_t user ) {

    /* Audit M-16: kontrola existence výstupního souboru před čtením z disku. */
    if ( check_output_overwrite ( output_path ) != EXIT_SUCCESS ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    char name[9], ext[4];
    if ( parse_filename ( cpm_filename, name, ext, NULL ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Invalid filename '%s'\n", cpm_filename );
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Alokujeme buffer pro data souboru */
    uint32_t buf_size = CPM_DISK_CAPACITY ( dpb );
    uint8_t *buffer = (uint8_t *) malloc ( buf_size );
    if ( buffer == NULL ) {
        fprintf ( stderr, "Error: Memory allocation failed\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    uint32_t bytes_read = 0;
    en_MZDSK_RES res = mzdsk_cpm_read_file ( disc, dpb, name, ext, user,
                                              buffer, buf_size, &bytes_read );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not read file '%s': %s\n",
                  cpm_filename, mzdsk_cpm_strerror ( res ) );
        free ( buffer );
        return res;
    }

    /* Zapíšeme do výstupního souboru */
    FILE *fp = fopen ( output_path, "wb" );
    if ( fp == NULL ) {
        fprintf ( stderr, "Error: Could not create output file '%s': %s\n",
                  output_path, strerror ( errno ) );
        free ( buffer );
        return MZDSK_RES_DSK_ERROR;
    }

    size_t written = fwrite ( buffer, 1, bytes_read, fp );
    fclose ( fp );
    free ( buffer );

    if ( written != bytes_read ) {
        fprintf ( stderr, "Error: Write error to '%s'\n", output_path );
        return MZDSK_RES_DSK_ERROR;
    }

    printf ( "Extracted '%s' -> '%s' (%lu bytes)\n",
             cpm_filename, output_path, (unsigned long) bytes_read );

    /* Kontrola chybějících extentů - varování při poškození souboru */
    char uname[9], uext[4];
    int j;
    for ( j = 0; name[j] && j < 8; j++ ) uname[j] = (char) toupper ( (unsigned char) name[j] );
    uname[j] = '\0';
    for ( j = 0; ext[j] && j < 3; j++ ) uext[j] = (char) toupper ( (unsigned char) ext[j] );
    uext[j] = '\0';

    st_MZDSK_CPM_EXTENT_CHECK ext_check;
    if ( mzdsk_cpm_check_extents ( disc, dpb, uname, uext, user,
                                    &ext_check ) == 0
         && ext_check.count > 0 ) {
        fprintf ( stderr, "Warning: Missing extent(s): " );
        for ( int m = 0; m < ext_check.count; m++ ) {
            fprintf ( stderr, "%u%s", ext_check.missing[m],
                      ( m < ext_check.count - 1 ) ? ", " : "" );
        }
        fprintf ( stderr, "\n" );
        fprintf ( stderr, "Warning: Extracted data may be incomplete"
                  " (%lu bytes extracted)\n",
                  (unsigned long) bytes_read );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Vloží soubor z hostitele na CP/M disk.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param input_path Cesta ke vstupnímu souboru na hostiteli.
 * @param cpm_filename Jméno CP/M souboru ve formátu "NAME.EXT".
 * @param user Číslo uživatele.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_put ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                               const char *input_path, const char *cpm_filename,
                               int strict_name, uint8_t user ) {

    char name[9], ext[4];
    if ( strict_name ) {
        /* Striktní --name: žádné zkracování, zakázané znaky zakázány. */
        char bad_char = 0;
        en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( cpm_filename,
            MZDSK_NAMEVAL_FLAVOR_CPM, name, ext, &bad_char );
        if ( r != MZDSK_NAMEVAL_OK ) {
            report_strict_name_error ( cpm_filename, r, bad_char );
            return MZDSK_RES_INVALID_PARAM;
        }
    } else {
        /* Tolerantní tradiční cesta: truncation + warning. */
        int truncated = 0;
        if ( parse_filename ( cpm_filename, name, ext, &truncated ) != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Invalid filename '%s'\n", cpm_filename );
            return MZDSK_RES_INVALID_PARAM;
        }
        if ( truncated ) {
            warn_cpm_truncation ( cpm_filename, name, ext );
        }
    }

    /* Přečteme vstupní soubor */
    FILE *fp = fopen ( input_path, "rb" );
    if ( fp == NULL ) {
        fprintf ( stderr, "Error: Could not open input file '%s': %s\n",
                  input_path, strerror ( errno ) );
        return MZDSK_RES_DSK_ERROR;
    }

    /* Zjistíme velikost */
    fseek ( fp, 0, SEEK_END );
    long file_size = ftell ( fp );
    fseek ( fp, 0, SEEK_SET );

    uint32_t disk_capacity = CPM_DISK_CAPACITY ( dpb );
    if ( file_size < 0 || (unsigned long) file_size > disk_capacity ) {
        fprintf ( stderr, "Error: File '%s' is too large (%ld bytes, disk capacity %u bytes)\n",
                  input_path, file_size, disk_capacity );
        fclose ( fp );
        return MZDSK_RES_DISK_FULL;
    }

    /* Alokujeme buffer a přečteme data */
    uint8_t *buffer = (uint8_t *) malloc ( (size_t) file_size );
    if ( buffer == NULL ) {
        fprintf ( stderr, "Error: Memory allocation failed\n" );
        fclose ( fp );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    size_t read_count = fread ( buffer, 1, (size_t) file_size, fp );
    fclose ( fp );

    if ( (long) read_count != file_size ) {
        fprintf ( stderr, "Error: Read error from '%s'\n", input_path );
        free ( buffer );
        return MZDSK_RES_DSK_ERROR;
    }

    /* Zapíšeme na CP/M disk */
    en_MZDSK_RES res = mzdsk_cpm_write_file ( disc, dpb, name, ext, user,
                                               buffer, (uint32_t) file_size );
    free ( buffer );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not write file '%s': %s\n",
                  cpm_filename, mzdsk_cpm_strerror ( res ) );
        return res;
    }

    if ( ext[0] != '\0' ) {
        printf ( "Written '%s' -> '%s.%s' (%ld bytes)\n",
                 input_path, name, ext, file_size );
    } else {
        printf ( "Written '%s' -> '%s' (%ld bytes)\n",
                 input_path, name, file_size );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Extrahuje soubor z CP/M disku jako MZF rámec.
 *
 * Přečte soubor z CP/M disku, zjistí jeho atributy a zakóduje do MZF
 * (128B hlavička + tělo dat) s volitelným typem, load a exec adresou.
 * Výsledek zapíše do hostitelského souboru.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param cpm_filename Jméno CP/M souboru ve formátu "NAME.EXT".
 * @param output_path Cesta k výstupnímu MZF souboru na hostiteli.
 * @param user Číslo uživatele.
 * @param ftype MZF typ souboru (8b, výchozí 0x22 = konvence SOKODI CMT.COM).
 * @param exec_addr Exec adresa - zapíše se do pole `fexec`.
 * @param strt_addr Load adresa - zapíše se do pole `fstrt`.
 * @param encode_attrs Kódovat CP/M atributy R/O/SYS/ARC do bitů 7 fname[9..11].
 *                   Uplatní se pouze pro ftype == 0x22; pro ostatní typy
 *                   volající předává 0 (atributy se tam nekódují).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_get_mzf ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                    const char *cpm_filename, const char *output_path,
                                    uint8_t user, uint8_t ftype,
                                    uint16_t exec_addr, uint16_t strt_addr,
                                    int encode_attrs ) {

    /* Audit M-16: kontrola existence výstupního souboru před čtením z disku. */
    if ( check_output_overwrite ( output_path ) != EXIT_SUCCESS ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    char name[9], ext[4];
    if ( parse_filename ( cpm_filename, name, ext, NULL ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Invalid filename '%s'\n", cpm_filename );
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Přečteme data souboru z CP/M disku */
    uint32_t buf_size = CPM_DISK_CAPACITY ( dpb );
    uint8_t *buffer = (uint8_t *) malloc ( buf_size );
    if ( buffer == NULL ) {
        fprintf ( stderr, "Error: Memory allocation failed\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    uint32_t bytes_read = 0;
    en_MZDSK_RES res = mzdsk_cpm_read_file ( disc, dpb, name, ext, user,
                                              buffer, buf_size, &bytes_read );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not read file '%s': %s\n",
                  cpm_filename, mzdsk_cpm_strerror ( res ) );
        free ( buffer );
        return res;
    }

    /* Zjistíme souborové atributy (R/O, SYS, ARC) */
    uint8_t attrs = 0;
    res = mzdsk_cpm_get_attributes ( disc, dpb, name, ext, user, &attrs );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Warning: Could not read attributes for '%s': %s\n",
                  cpm_filename, mzdsk_cpm_strerror ( res ) );
        attrs = 0;
    }

    /* Zakódujeme do MZF. Kódování atributů do bitů 7 přípony se aplikuje
       jen pro ftype == 0x22 (konvence SOKODI CMT.COM); pro ostatní typy
       se neuplatní bez ohledu na uživatelskou volbu --no-attrs. */
    int effective_encode_attrs = ( ftype == MZDSK_CPM_MZF_FTYPE && encode_attrs ) ? 1 : 0;
    uint8_t *mzf_data = NULL;
    uint32_t mzf_size = 0;
    res = mzdsk_cpm_mzf_encode_ex ( buffer, bytes_read, name, ext, attrs,
                                     ftype, exec_addr, strt_addr,
                                     effective_encode_attrs,
                                     &mzf_data, &mzf_size );
    free ( buffer );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: MZF encoding failed: %s\n",
                  mzdsk_cpm_strerror ( res ) );
        return res;
    }

    /* Zapíšeme do výstupního souboru */
    FILE *fp = fopen ( output_path, "wb" );
    if ( fp == NULL ) {
        fprintf ( stderr, "Error: Could not create output file '%s': %s\n",
                  output_path, strerror ( errno ) );
        free ( mzf_data );
        return MZDSK_RES_DSK_ERROR;
    }

    size_t written = fwrite ( mzf_data, 1, mzf_size, fp );
    fclose ( fp );
    free ( mzf_data );

    if ( written != mzf_size ) {
        fprintf ( stderr, "Error: Write error to '%s'\n", output_path );
        return MZDSK_RES_DSK_ERROR;
    }

    printf ( "Extracted '%s' -> '%s' (MZF ftype=0x%02X, %lu bytes, strt=0x%04X, exec=0x%04X)\n",
             cpm_filename, output_path, ftype, (unsigned long) bytes_read,
             strt_addr, exec_addr );

    return MZDSK_RES_OK;
}


/**
 * @brief Sanitizuje jméno souboru pro uložení na hostitelský souborový systém.
 *
 * Nahradí znaky neplatné na Windows (* ? " < > | : \ /) podtržítkem.
 * Pracuje in-place nad předaným bufferem.
 *
 * @param[in,out] name Buffer s jménem souboru. Nesmí být NULL.
 *
 * @pre name != NULL a ukazuje na null-terminated řetězec.
 * @post Všechny neplatné znaky jsou nahrazeny '_'.
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
 * @brief Zkontroluje existenci souboru na hostitelském souborovém systému.
 *
 * Používá stat() pro ověření, zda soubor na dané cestě existuje.
 *
 * @param[in] path Cesta k souboru. Nesmí být NULL.
 *
 * @return 1 pokud soubor existuje, 0 pokud neexistuje.
 *
 * @pre path != NULL
 */
static int file_exists_on_host ( const char *path ) {
    struct stat st;
    return ( stat ( path, &st ) == 0 );
}


/**
 * @brief Sestaví cestu pro výstupní soubor s řešením duplicit.
 *
 * Vytvoří cestu ve formátu "<dir>/<NAME>.<EXT>" nebo
 * "<dir>/userNN/<NAME>.<EXT>" (pokud by_user != 0).
 * Pokud soubor na cílové cestě již existuje, aplikuje strategii
 * řešení duplicit:
 * - DUP_RENAME: přidá suffix ~N před příponu (SOUBOR~2.COM, ~3, ...)
 * - DUP_OVERWRITE: vrátí původní cestu bez změny
 * - DUP_SKIP: vrátí 1 (soubor se má přeskočit)
 *
 * @param[out] path_buf Výstupní buffer pro cestu (min MAX_PATH_LEN bajtů).
 * @param[in]  dir Cílový adresář. Nesmí být NULL.
 * @param[in]  filename Jméno souboru (max 8 znaků). Nesmí být NULL.
 * @param[in]  ext Přípona souboru (max 3 znaky). Nesmí být NULL.
 * @param[in]  user Číslo uživatele (0-15).
 * @param[in]  by_user Nenulová hodnota = vytvořit podadresář podle usera.
 * @param[in]  dup_mode Strategie řešení duplicit.
 * @param[in]  is_mzf Nenulová hodnota = přidat příponu ".mzf" místo originální.
 *
 * @return 0 = cesta připravena v path_buf, soubor se má extrahovat.
 * @return 1 = soubor se má přeskočit (DUP_SKIP a soubor existuje).
 * @return -1 = chyba (nelze vytvořit adresář, buffer overflow).
 *
 * @pre path_buf != NULL && dir != NULL && filename != NULL && ext != NULL
 * @post Při úspěchu (návrat 0) path_buf obsahuje null-terminated cestu.
 */
static int build_output_path ( char *path_buf, const char *dir,
                                const char *filename, const char *ext,
                                uint8_t user, int by_user,
                                en_DUPLICATE_MODE dup_mode, int is_mzf ) {

    /* Sanitizované jméno a přípona */
    char safe_name[9], safe_ext[4];
    strncpy ( safe_name, filename, 8 );
    safe_name[8] = '\0';
    strncpy ( safe_ext, ext, 3 );
    safe_ext[3] = '\0';
    sanitize_filename ( safe_name );
    sanitize_filename ( safe_ext );

    /* Sestavíme bázový adresář (max MAX_BASE_DIR_LEN) */
    char base_dir[MAX_BASE_DIR_LEN];
    if ( by_user ) {
        snprintf ( base_dir, sizeof ( base_dir ), "%s/user%02d", dir, user );
    } else {
        snprintf ( base_dir, sizeof ( base_dir ), "%s", dir );
    }

    /* Vytvoříme adresář(e) pokud neexistují */
    if ( _mkdir ( dir ) != 0 && errno != EEXIST ) {
        fprintf ( stderr, "Error: Could not create directory '%s': %s\n",
                  dir, strerror ( errno ) );
        return -1;
    }
    if ( by_user ) {
        if ( _mkdir ( base_dir ) != 0 && errno != EEXIST ) {
            fprintf ( stderr, "Error: Could not create directory '%s': %s\n",
                      base_dir, strerror ( errno ) );
            return -1;
        }
    }

    /* Sestavíme jméno souboru. Audit M-27: kontrolujeme snprintf
     * truncation - pokud by cesta přesáhla MAX_PATH_LEN, jinak by
     * došlo k tichému oříznutí a potenciální kolizi s jiným souborem
     * (u rename by nenašlo volný suffix). */
    const char *out_ext = is_mzf ? "mzf" : safe_ext;
    int path_len;

    if ( safe_ext[0] != '\0' && !is_mzf ) {
        path_len = snprintf ( path_buf, MAX_PATH_LEN, "%s/%s.%s", base_dir, safe_name, out_ext );
    } else if ( is_mzf ) {
        if ( safe_ext[0] != '\0' ) {
            path_len = snprintf ( path_buf, MAX_PATH_LEN, "%s/%s.%s.mzf", base_dir, safe_name, safe_ext );
        } else {
            path_len = snprintf ( path_buf, MAX_PATH_LEN, "%s/%s.mzf", base_dir, safe_name );
        }
    } else {
        path_len = snprintf ( path_buf, MAX_PATH_LEN, "%s/%s", base_dir, safe_name );
    }
    if ( path_len >= MAX_PATH_LEN ) {
        fprintf ( stderr, "Error: Output path too long\n" );
        return -1;
    }

    /* Kontrola duplicity */
    if ( file_exists_on_host ( path_buf ) ) {
        switch ( dup_mode ) {
            case DUP_OVERWRITE:
                /* Ponecháme cestu beze změny - přepíše se */
                break;

            case DUP_SKIP:
                fprintf ( stderr, "Warning: Skipping '%s' (file exists)\n", path_buf );
                return 1;

            case DUP_RENAME: {
                /* Hledáme volný suffix ~2, ~3, ... */
                int rn_len;
                for ( int n = 2; n < 1000; n++ ) {
                    if ( is_mzf ) {
                        if ( safe_ext[0] != '\0' ) {
                            rn_len = snprintf ( path_buf, MAX_PATH_LEN, "%s/%s~%d.%s.mzf",
                                                 base_dir, safe_name, n, safe_ext );
                        } else {
                            rn_len = snprintf ( path_buf, MAX_PATH_LEN, "%s/%s~%d.mzf",
                                                 base_dir, safe_name, n );
                        }
                    } else if ( safe_ext[0] != '\0' ) {
                        rn_len = snprintf ( path_buf, MAX_PATH_LEN, "%s/%s~%d.%s",
                                             base_dir, safe_name, n, out_ext );
                    } else {
                        rn_len = snprintf ( path_buf, MAX_PATH_LEN, "%s/%s~%d",
                                             base_dir, safe_name, n );
                    }
                    if ( rn_len >= MAX_PATH_LEN ) {
                        fprintf ( stderr, "Error: Output path too long (rename)\n" );
                        return -1;
                    }
                    if ( !file_exists_on_host ( path_buf ) ) break;
                }
                break;
            }
        }
    }

    return 0;
}


/**
 * @brief Extrahuje všechny soubory z CP/M disku do cílového adresáře.
 *
 * Přečte rozšířený adresář pomocí mzdsk_cpm_read_directory_ex(),
 * iteruje přes všechny soubory a extrahuje je jako raw binárky
 * (nebo jako MZF s CPM-IC hlavičkou). Řeší duplicitní jména
 * souborů podle zvolené strategie (rename/overwrite/skip).
 *
 * Pro každý soubor kontroluje chybějící extenty a vypisuje varování
 * na stderr.
 *
 * Na konci vypíše souhrn: počet extrahovaných, přeskočených a přejmenovaných.
 *
 * @param[in] disc Kontext disku. Nesmí být NULL.
 * @param[in] dpb Disk Parameter Block. Nesmí být NULL.
 * @param[in] dir Cílový adresář na hostiteli. Nesmí být NULL.
 * @param[in] filter_user Filtr uživatele: 0-15 = pouze daný uživatel, < 0 = všichni.
 * @param[in] is_mzf Nenulová hodnota = extrahovat jako MZF s CPM-IC hlavičkou.
 * @param[in] by_user Nenulová hodnota = vytvořit podadresáře podle user čísla.
 * @param[in] dup_mode Strategie řešení duplicitních jmen souborů.
 * @param[in] exec_addr Exec adresa pro MZF hlavičku (pole `fexec`,
 *                       relevantní pouze pokud is_mzf != 0).
 * @param[in] ftype MZF typ souboru (relevantní pouze pokud is_mzf != 0;
 *                   výchozí 0x22 = konvence SOKODI CMT.COM).
 * @param[in] strt_addr Load adresa zapsaná do pole `fstrt` (relevantní pouze
 *                   pokud is_mzf != 0).
 * @param[in] encode_attrs Kódovat CP/M atributy do bitů 7 fname[9..11]
 *                   (uplatní se jen pro ftype == 0x22).
 *
 * @return MZDSK_RES_OK při úspěchu (i pokud některé soubory přeskočeny).
 * @return MZDSK_RES_DSK_ERROR při chybě čtení adresáře.
 * @return Jiný chybový kód při chybě extrakce souboru.
 *
 * @pre disc != NULL && dpb != NULL && dir != NULL
 * @post Cílový adresář (a případné podadresáře) existují na hostiteli.
 *
 * @par Vedlejší efekty:
 * - Vytváří soubory a adresáře na hostitelském souborovém systému.
 * - Na stderr vypisuje varování při chybějících extentech a duplicitách.
 */
static en_MZDSK_RES cmd_get_all ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                    const char *dir, int filter_user,
                                    int is_mzf, int by_user,
                                    en_DUPLICATE_MODE dup_mode,
                                    uint8_t ftype, uint16_t exec_addr,
                                    uint16_t strt_addr, int encode_attrs ) {

    /* Přečteme rozšířený adresář */
    st_MZDSK_CPM_FILE_INFO_EX files[MAX_DIR_FILES];
    int count = mzdsk_cpm_read_directory_ex ( disc, dpb, files, MAX_DIR_FILES );

    if ( count < 0 ) {
        fprintf ( stderr, "Error: Failed to read directory\n" );
        return MZDSK_RES_DSK_ERROR;
    }

    if ( count == 0 ) {
        printf ( "No files found.\n" );
        return MZDSK_RES_OK;
    }

    /* Počítadla pro souhrn */
    int extracted = 0;
    int skipped = 0;
    int renamed = 0;
    int errors = 0;

    /* Alokujeme buffer pro data souboru (sdílený pro všechny soubory) */
    uint32_t buf_size = CPM_DISK_CAPACITY ( dpb );
    uint8_t *buffer = (uint8_t *) malloc ( buf_size );
    if ( buffer == NULL ) {
        fprintf ( stderr, "Error: Memory allocation failed\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    for ( int i = 0; i < count; i++ ) {

        /* Filtr uživatele */
        if ( filter_user >= 0 && files[i].user != (uint8_t) filter_user ) continue;

        /* Sestavíme výstupní cestu */
        char path_buf[MAX_PATH_LEN];
        int path_res = build_output_path ( path_buf, dir,
                                            files[i].filename, files[i].extension,
                                            files[i].user, by_user,
                                            dup_mode, is_mzf );

        if ( path_res < 0 ) {
            /* Chyba vytváření adresáře */
            errors++;
            continue;
        }
        if ( path_res > 0 ) {
            /* Soubor přeskočen (DUP_SKIP) */
            skipped++;
            continue;
        }

        /* Detekce přejmenování - porovnáme s originální cestou */
        char orig_path[MAX_PATH_LEN];
        {
            /* Sestavíme originální cestu bez rename suffixu pro porovnání */
            char safe_name[9], safe_ext[4];
            strncpy ( safe_name, files[i].filename, 8 );
            safe_name[8] = '\0';
            strncpy ( safe_ext, files[i].extension, 3 );
            safe_ext[3] = '\0';
            sanitize_filename ( safe_name );
            sanitize_filename ( safe_ext );

            const char *base_dir = dir;
            char user_dir[MAX_BASE_DIR_LEN];
            if ( by_user ) {
                snprintf ( user_dir, sizeof ( user_dir ), "%s/user%02d", dir, files[i].user );
                base_dir = user_dir;
            }

            if ( is_mzf ) {
                if ( safe_ext[0] != '\0' ) {
                    snprintf ( orig_path, MAX_PATH_LEN, "%s/%s.%s.mzf", base_dir, safe_name, safe_ext );
                } else {
                    snprintf ( orig_path, MAX_PATH_LEN, "%s/%s.mzf", base_dir, safe_name );
                }
            } else if ( safe_ext[0] != '\0' ) {
                snprintf ( orig_path, MAX_PATH_LEN, "%s/%s.%s", base_dir, safe_name, safe_ext );
            } else {
                snprintf ( orig_path, MAX_PATH_LEN, "%s/%s", base_dir, safe_name );
            }
        }
        int was_renamed = ( strcmp ( path_buf, orig_path ) != 0 );

        /* Přečteme data souboru */
        uint32_t bytes_read = 0;
        en_MZDSK_RES res = mzdsk_cpm_read_file ( disc, dpb,
                                                   files[i].filename, files[i].extension,
                                                   files[i].user,
                                                   buffer, buf_size, &bytes_read );

        if ( res != MZDSK_RES_OK ) {
            fprintf ( stderr, "Error: Could not read file '%s.%s' (user %d): %s\n",
                      files[i].filename, files[i].extension, files[i].user,
                      mzdsk_cpm_strerror ( res ) );
            errors++;
            continue;
        }

        /* Kontrola chybějících extentů */
        st_MZDSK_CPM_EXTENT_CHECK ext_check;
        if ( mzdsk_cpm_check_extents ( disc, dpb, files[i].filename, files[i].extension,
                                        files[i].user, &ext_check ) == 0
             && ext_check.count > 0 ) {
            fprintf ( stderr, "Warning: File '%s.%s' (user %d) has missing extent(s): ",
                      files[i].filename, files[i].extension, files[i].user );
            for ( int m = 0; m < ext_check.count; m++ ) {
                fprintf ( stderr, "%u%s", ext_check.missing[m],
                          ( m < ext_check.count - 1 ) ? ", " : "" );
            }
            fprintf ( stderr, "\n" );
            fprintf ( stderr, "Warning: Extracted data may be incomplete"
                      " (%lu bytes extracted)\n",
                      (unsigned long) bytes_read );
        }

        if ( is_mzf ) {
            /* MZF extrakce s CPM-IC hlavičkou */
            uint8_t attrs = 0;
            res = mzdsk_cpm_get_attributes ( disc, dpb,
                                              files[i].filename, files[i].extension,
                                              files[i].user, &attrs );
            if ( res != MZDSK_RES_OK ) {
                attrs = 0;
            }

            /* Atributy se kódují jen pro ftype == 0x22 (konvence SOKODI CMT.COM). */
            int effective_encode_attrs = ( ftype == MZDSK_CPM_MZF_FTYPE && encode_attrs ) ? 1 : 0;
            uint8_t *mzf_data = NULL;
            uint32_t mzf_size = 0;
            res = mzdsk_cpm_mzf_encode_ex ( buffer, bytes_read,
                                             files[i].filename, files[i].extension,
                                             attrs, ftype, exec_addr, strt_addr,
                                             effective_encode_attrs,
                                             &mzf_data, &mzf_size );

            if ( res != MZDSK_RES_OK ) {
                fprintf ( stderr, "Error: MZF encoding failed for '%s.%s': %s\n",
                          files[i].filename, files[i].extension,
                          mzdsk_cpm_strerror ( res ) );
                errors++;
                continue;
            }

            FILE *fp = fopen ( path_buf, "wb" );
            if ( fp == NULL ) {
                fprintf ( stderr, "Error: Could not create output file '%s': %s\n",
                          path_buf, strerror ( errno ) );
                free ( mzf_data );
                errors++;
                continue;
            }

            size_t written = fwrite ( mzf_data, 1, mzf_size, fp );
            fclose ( fp );
            free ( mzf_data );

            if ( written != mzf_size ) {
                fprintf ( stderr, "Error: Write error to '%s'\n", path_buf );
                errors++;
                continue;
            }

            printf ( "Extracted '%s.%s' (user %d) -> '%s' (MZF, %lu bytes)\n",
                     files[i].filename, files[i].extension, files[i].user,
                     path_buf, (unsigned long) bytes_read );
        } else {
            /* Raw binární extrakce */
            FILE *fp = fopen ( path_buf, "wb" );
            if ( fp == NULL ) {
                fprintf ( stderr, "Error: Could not create output file '%s': %s\n",
                          path_buf, strerror ( errno ) );
                errors++;
                continue;
            }

            size_t written = fwrite ( buffer, 1, bytes_read, fp );
            fclose ( fp );

            if ( written != bytes_read ) {
                fprintf ( stderr, "Error: Write error to '%s'\n", path_buf );
                errors++;
                continue;
            }

            printf ( "Extracted '%s.%s' (user %d) -> '%s' (%lu bytes)\n",
                     files[i].filename, files[i].extension, files[i].user,
                     path_buf, (unsigned long) bytes_read );
        }

        extracted++;
        if ( was_renamed ) renamed++;
    }

    free ( buffer );

    /* Souhrn */
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
    return ( errors > 0 ) ? MZDSK_RES_DSK_ERROR : MZDSK_RES_OK;
}


/**
 * @brief Vloží MZF soubor s CPM-IC hlavičkou na CP/M disk.
 *
 * Přečte MZF soubor z hostitele, dekóduje CPM-IC hlavičku(y),
 * spojí data ze všech stránek a zapíše výsledný soubor na CP/M disk.
 * Zachovává souborové atributy (R/O, SYS, ARC) z MZF hlavičky.
 *
 * Pokud je zadán name_override, použije se místo jména z MZF hlavičky.
 *
 * @param disc          Kontext disku.
 * @param dpb           Disk Parameter Block.
 * @param input_path    Cesta k vstupnímu MZF souboru na hostiteli.
 * @param name_override Volitelné přepsání CP/M jména ("NAME.EXT"), nebo NULL.
 * @param strict_name   Pokud nenulové, `name_override` prochází striktní 8.3 validací
 *                      bez tichého zkracování; jinak se aplikuje tolerantní truncate.
 * @param user          Číslo CP/M user area.
 * @param charset       Varianta Sharp MZ znakové sady pro konverzi fname
 *                      odvozeného z MZF hlavičky (MZF_NAME_ASCII_EU nebo
 *                      MZF_NAME_ASCII_JP). Ignorováno pro ftype 0x22.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_put_mzf ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                    const char *input_path, const char *name_override,
                                    int strict_name, uint8_t user,
                                    en_MZF_NAME_ENCODING charset,
                                    unsigned decode_flags ) {

    /* Kontrola že vstup je regular file (audit M-19). Bez toho by
     * např. adresář prošel fopen, ale fread by vrátil 0 a uživatel
     * by dostal matoucí "empty file" hlášku. */
    {
        struct stat st;
        if ( stat ( input_path, &st ) == 0 && !S_ISREG ( st.st_mode ) ) {
            fprintf ( stderr, "Error: '%s' is not a regular file\n", input_path );
            return MZDSK_RES_DSK_ERROR;
        }
    }

    /* Přečteme MZF soubor z hostitele */
    FILE *fp = fopen ( input_path, "rb" );
    if ( fp == NULL ) {
        fprintf ( stderr, "Error: Could not open input file '%s': %s\n",
                  input_path, strerror ( errno ) );
        return MZDSK_RES_DSK_ERROR;
    }

    fseek ( fp, 0, SEEK_END );
    long file_size = ftell ( fp );
    fseek ( fp, 0, SEEK_SET );

    if ( file_size < (long) MZF_HEADER_SIZE ) {
        fprintf ( stderr, "Error: File '%s' is too small for MZF format (%ld bytes)\n",
                  input_path, file_size );
        fclose ( fp );
        return MZDSK_RES_FORMAT_ERROR;
    }

    uint8_t *mzf_data = (uint8_t *) malloc ( (size_t) file_size );
    if ( mzf_data == NULL ) {
        fprintf ( stderr, "Error: Memory allocation failed\n" );
        fclose ( fp );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    size_t read_count = fread ( mzf_data, 1, (size_t) file_size, fp );
    fclose ( fp );

    if ( (long) read_count != file_size ) {
        fprintf ( stderr, "Error: Read error from '%s'\n", input_path );
        free ( mzf_data );
        return MZDSK_RES_DSK_ERROR;
    }

    /* Dekódujeme MZF CPM-IC */
    char cpm_name[9], cpm_ext[4];
    uint8_t attrs = 0;
    uint16_t exec_addr = 0;
    uint8_t *file_data = NULL;
    uint32_t data_size = 0;

    en_MZDSK_RES res = mzdsk_cpm_mzf_decode_ex2 ( mzf_data, (uint32_t) file_size,
                                                    charset, decode_flags,
                                                    cpm_name, cpm_ext, &attrs, &exec_addr,
                                                    &file_data, &data_size );
    free ( mzf_data );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: MZF decoding failed: %s\n",
                  mzdsk_cpm_strerror ( res ) );
        return res;
    }

    /* Jméno souboru - buď z MZF hlavičky, nebo override z příkazové řádky */
    char final_name[9], final_ext[4];

    if ( name_override != NULL ) {
        if ( strict_name ) {
            char bad_char = 0;
            en_MZDSK_NAMEVAL r = mzdsk_validate_83_name ( name_override,
                MZDSK_NAMEVAL_FLAVOR_CPM, final_name, final_ext, &bad_char );
            if ( r != MZDSK_NAMEVAL_OK ) {
                report_strict_name_error ( name_override, r, bad_char );
                free ( file_data );
                return MZDSK_RES_INVALID_PARAM;
            }
        } else {
            int trunc_ov = 0;
            if ( parse_filename ( name_override, final_name, final_ext, &trunc_ov ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Invalid filename '%s'\n", name_override );
                free ( file_data );
                return MZDSK_RES_INVALID_PARAM;
            }
            if ( trunc_ov ) {
                warn_cpm_truncation ( name_override, final_name, final_ext );
            }
        }
    } else {
        strncpy ( final_name, cpm_name, sizeof ( final_name ) - 1 );
        final_name[8] = '\0';
        strncpy ( final_ext, cpm_ext, sizeof ( final_ext ) - 1 );
        final_ext[3] = '\0';
    }

    /* Zapíšeme na CP/M disk */
    if ( data_size > 0 && file_data != NULL ) {
        res = mzdsk_cpm_write_file ( disc, dpb, final_name, final_ext, user,
                                      file_data, data_size );
    } else {
        /* Prázdný soubor - vytvoříme s minimální velikostí */
        uint8_t empty = 0x1A;
        res = mzdsk_cpm_write_file ( disc, dpb, final_name, final_ext, user,
                                      &empty, 1 );
    }
    free ( file_data );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not write file '%s.%s': %s\n",
                  final_name, final_ext, mzdsk_cpm_strerror ( res ) );
        return res;
    }

    /* Nastavení atributů z MZF hlavičky */
    if ( attrs != 0 ) {
        res = mzdsk_cpm_set_attributes ( disc, dpb, final_name, final_ext, user, attrs );
        if ( res != MZDSK_RES_OK ) {
            fprintf ( stderr, "Warning: Could not set attributes for '%s.%s': %s\n",
                      final_name, final_ext, mzdsk_cpm_strerror ( res ) );
        }
    }

    printf ( "Written '%s' -> '%s.%s' (MZF CPM-IC, %lu bytes, exec=0x%04X)\n",
             input_path, final_name, final_ext, (unsigned long) data_size, exec_addr );

    return MZDSK_RES_OK;
}


/**
 * @brief Smaže soubor z CP/M disku.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param filename Jméno souboru ve formátu "NAME.EXT".
 * @param user Číslo uživatele.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_era ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                               const char *filename, uint8_t user ) {

    char name[9], ext[4];
    if ( parse_filename ( filename, name, ext, NULL ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Invalid filename '%s'\n", filename );
        return MZDSK_RES_INVALID_PARAM;
    }

    en_MZDSK_RES res = mzdsk_cpm_delete_file ( disc, dpb, name, ext, user );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not delete file '%s': %s\n",
                  filename, mzdsk_cpm_strerror ( res ) );
        return res;
    }

    printf ( "Deleted '%s' (user %d)\n", filename, user );

    return MZDSK_RES_OK;
}


/**
 * @brief Přejmenuje soubor na CP/M disku.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param old_filename Současné jméno ve formátu "NAME.EXT".
 * @param new_filename Nové jméno ve formátu "NAME.EXT".
 * @param user Číslo uživatele.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_ren ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                               const char *old_filename, const char *new_filename,
                               uint8_t user ) {

    char old_name[9], old_ext[4];
    char new_name[9], new_ext[4];

    if ( parse_filename ( old_filename, old_name, old_ext, NULL ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Invalid filename '%s'\n", old_filename );
        return MZDSK_RES_INVALID_PARAM;
    }

    int trunc_new = 0;
    if ( parse_filename ( new_filename, new_name, new_ext, &trunc_new ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Invalid filename '%s'\n", new_filename );
        return MZDSK_RES_INVALID_PARAM;
    }
    if ( trunc_new ) {
        warn_cpm_truncation ( new_filename, new_name, new_ext );
    }

    en_MZDSK_RES res = mzdsk_cpm_rename_file ( disc, dpb, old_name, old_ext, user,
                                                new_name, new_ext );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not rename '%s' to '%s': %s\n",
                  old_filename, new_filename, mzdsk_cpm_strerror ( res ) );
        return res;
    }

    printf ( "Renamed '%s' -> '%s' (user %d)\n", old_filename, new_filename, user );

    return MZDSK_RES_OK;
}


/**
 * @brief Změní user number existujícího souboru.
 *
 * Volá mzdsk_cpm_set_user() pro přepsání user bytu na všech extentech.
 * Validuje rozsah nového user čísla (0-15) a formátuje chybové hlášky.
 *
 * @param disc         Kontext disku.
 * @param dpb          Disk Parameter Block.
 * @param filename     Jméno souboru ve formátu "NAME.EXT".
 * @param old_user     Současný user number (globální --user N).
 * @param new_user_str Nový user number jako řetězec (z CLI argumentu).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_chuser ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                   const char *filename, uint8_t old_user,
                                   const char *new_user_str ) {

    char name[9], ext[4];
    if ( parse_filename ( filename, name, ext, NULL ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Invalid filename '%s'\n", filename );
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Parsování nového user čísla (0-15) */
    char *endptr;
    unsigned long val = strtoul ( new_user_str, &endptr, 10 );
    if ( endptr == new_user_str || *endptr != '\0' || val > 15 ) {
        fprintf ( stderr, "Error: Invalid new user number '%s' (0-15)\n", new_user_str );
        return MZDSK_RES_INVALID_PARAM;
    }
    uint8_t new_user = (uint8_t) val;

    en_MZDSK_RES res = mzdsk_cpm_set_user ( disc, dpb, name, ext, old_user, new_user );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not change user of '%s' (user %d -> %d): %s\n",
                  filename, old_user, new_user, mzdsk_cpm_strerror ( res ) );
        return res;
    }

    printf ( "Changed user of '%s': %d -> %d\n", filename, old_user, new_user );

    return MZDSK_RES_OK;
}


/**
 * @brief Nastaví atributy souboru.
 *
 * Parsuje specifikaci atributů ve formátu "+R", "-S", "+RSA", "+S+A" atd.
 * Znak '+' nastaví atribut, '-' ho zruší. Přepínače '+'/'-' lze kombinovat
 * i uvnitř jednoho argumentu (např. "+R-S" nastaví R/O a zruší System).
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 * @param filename Jméno souboru ve formátu "NAME.EXT".
 * @param user Číslo uživatele.
 * @param attr_specs Pole specifikací atributů ("+R", "-S", "+RSA", "+S+A" atd.).
 * @param attr_count Počet specifikací.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_attr ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                const char *filename, uint8_t user,
                                char **attr_specs, int attr_count ) {

    char name[9], ext[4];
    if ( parse_filename ( filename, name, ext, NULL ) != EXIT_SUCCESS ) {
        fprintf ( stderr, "Error: Invalid filename '%s'\n", filename );
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Přečteme aktuální atributy */
    uint8_t attrs = 0;
    en_MZDSK_RES res = mzdsk_cpm_get_attributes ( disc, dpb, name, ext, user, &attrs );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: File '%s' not found: %s\n",
                  filename, mzdsk_cpm_strerror ( res ) );
        return res;
    }

    if ( attr_count == 0 ) {
        /* Bez specifikace - jen zobrazíme aktuální atributy */
        char attr_str[16];
        format_attributes ( attrs, attr_str, sizeof ( attr_str ) );
        printf ( "%s: %s\n", filename, attr_str );
        return MZDSK_RES_OK;
    }

    /* Aplikujeme změny */
    int i;
    for ( i = 0; i < attr_count; i++ ) {
        const char *spec = attr_specs[i];
        if ( strlen ( spec ) < 2 || ( spec[0] != '+' && spec[0] != '-' ) ) {
            fprintf ( stderr, "Error: Invalid attribute spec '%s' (use +R, -R, +S, -S, +A, -A)\n", spec );
            return MZDSK_RES_INVALID_PARAM;
        }

        int set = ( spec[0] == '+' );

        /* Iterujeme přes všechny znaky atributů za +/- (např. "+RSA" nebo "+R+S+A") */
        int j;
        for ( j = 1; spec[j] != '\0'; j++ ) {
            /* Přepínač režimu uvnitř řetězce (např. "+S+A" nebo "+R-S") */
            if ( spec[j] == '+' || spec[j] == '-' ) {
                set = ( spec[j] == '+' );
                continue;
            }
            char attr_char = (char) toupper ( (unsigned char) spec[j] );
            uint8_t mask = 0;

            switch ( attr_char ) {
                case 'R': mask = MZDSK_CPM_ATTR_READ_ONLY; break;
                case 'S': mask = MZDSK_CPM_ATTR_SYSTEM;    break;
                case 'A': mask = MZDSK_CPM_ATTR_ARCHIVED;  break;
                default:
                    fprintf ( stderr, "Error: Unknown attribute '%c' (use R, S, A)\n", spec[j] );
                    return MZDSK_RES_INVALID_PARAM;
            }

            if ( set ) {
                attrs |= mask;
            } else {
                attrs &= (uint8_t) ~mask;
            }
        }
    }

    /* Zapíšeme nové atributy */
    res = mzdsk_cpm_set_attributes ( disc, dpb, name, ext, user, attrs );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not set attributes: %s\n", mzdsk_cpm_strerror ( res ) );
        return res;
    }

    char attr_str[16];
    format_attributes ( attrs, attr_str, sizeof ( attr_str ) );
    printf ( "%s: %s\n", filename, attr_str );

    return MZDSK_RES_OK;
}


/**
 * @brief Zobrazí alokační mapu disku.
 *
 * Vypíše bitovou mapu bloků - obsazené (X) a volné (.) bloky.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_map ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                               en_OUTFMT output_format ) {

    st_MZDSK_CPM_ALLOC_MAP alloc_map;
    en_MZDSK_RES res = mzdsk_cpm_get_alloc_map ( disc, dpb, &alloc_map );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Could not read allocation map: %s\n", mzdsk_cpm_strerror ( res ) );
        return res;
    }

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nCP/M Allocation map (%d blocks):\n\n", alloc_map.total_blocks );

        /* Hlavička s desítkami */
        printf ( "       " );
        int j;
        for ( j = 0; j < 32; j++ ) {
            if ( j % 10 == 0 ) {
                printf ( "%d", j / 10 );
            } else {
                printf ( " " );
            }
        }
        printf ( "\n" );

        /* Hlavička s jednotkami */
        printf ( "       " );
        for ( j = 0; j < 32; j++ ) {
            printf ( "%d", j % 10 );
        }
        printf ( "\n" );

        /* Bloky po řádcích po 32 */
        uint16_t block;
        for ( block = 0; block < alloc_map.total_blocks; block++ ) {
            if ( block % 32 == 0 ) {
                printf ( "  %3d: ", block );
            }

            int used = alloc_map.map[block / 8] & ( 1u << ( block % 8 ) );
            printf ( "%c", used ? 'X' : '.' );

            if ( block % 32 == 31 || block == alloc_map.total_blocks - 1 ) {
                printf ( "\n" );
            }
        }

        printf ( "\n  Total: %d, Used: %d, Free: %d\n\n",
                 alloc_map.total_blocks, alloc_map.used_blocks, alloc_map.free_blocks );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = { "block", "used" };
        outfmt_csv_header ( &ctx, csv_hdr, 2 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "cpm" );
        outfmt_kv_int ( &ctx, "total_blocks", alloc_map.total_blocks );
        outfmt_kv_int ( &ctx, "used_blocks", alloc_map.used_blocks );
        outfmt_kv_int ( &ctx, "free_blocks", alloc_map.free_blocks );
        outfmt_array_begin ( &ctx, "blocks" );
        uint16_t block;
        for ( block = 0; block < alloc_map.total_blocks; block++ ) {
            int used = alloc_map.map[block / 8] & ( 1u << ( block % 8 ) );
            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "block", block );
            outfmt_field_bool ( &ctx, "used", used );
            outfmt_item_end ( &ctx );
        }
        outfmt_array_end ( &ctx );

        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zobrazí volné místo na disku.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 *
 * @return MZDSK_RES_OK při úspěchu.
 */
static en_MZDSK_RES cmd_free ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                en_OUTFMT output_format ) {

    uint32_t free_bytes = mzdsk_cpm_free_space ( disc, dpb );
    uint32_t total_bytes = (uint32_t) ( dpb->dsm + 1 ) * dpb->block_size;

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "%lu kB free of %lu kB total (%lu bytes)\n",
                 (unsigned long) ( free_bytes / 1024 ),
                 (unsigned long) ( total_bytes / 1024 ),
                 (unsigned long) free_bytes );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "cpm" );
        outfmt_kv_uint ( &ctx, "total_bytes", (unsigned long) total_bytes );
        outfmt_kv_uint ( &ctx, "free_bytes", (unsigned long) free_bytes );
        outfmt_kv_uint ( &ctx, "used_bytes", (unsigned long) ( total_bytes - free_bytes ) );
        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Formátuje adresář CP/M disku (vyplní 0xE5).
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_format ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb ) {

    en_MZDSK_RES res = mzdsk_cpm_format_directory ( disc, dpb );

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Format failed: %s\n", mzdsk_cpm_strerror ( res ) );
        return res;
    }

    printf ( "CP/M directory formatted (%d entries cleared)\n", dpb->drm + 1 );

    return MZDSK_RES_OK;
}


/**
 * @brief Callback pro hlášení průběhu defragmentace.
 *
 * Tiskne informační zprávy knihovní funkce přímo na stdout.
 *
 * @param message Textová zpráva o průběhu.
 * @param user_data Nepoužito (vždy NULL).
 */
static void defrag_progress ( const char *message, void *user_data ) {
    (void) user_data;
    printf ( "%s", message );
}


/**
 * @brief Provede defragmentaci CP/M disku.
 *
 * Načte všechny soubory do paměti, zformátuje adresář a zapíše
 * vše sekvenčně bez mezer. Zachovává uživatelská čísla a atributy.
 *
 * @param disc Kontext disku.
 * @param dpb Disk Parameter Block.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_defrag ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb ) {

    printf ( "\nRun defragmentation CP/M\n\n" );

    en_MZDSK_RES err = mzdsk_cpm_defrag ( disc, dpb, defrag_progress, NULL );
    if ( !err ) printf ( "Done.\n\n" );
    return err;
}


/**
 * @brief Zobrazí aktuální DPB parametry.
 *
 * Vypíše všechny parametry Disk Parameter Block včetně odvozených
 * hodnot. Slouží k ověření konfigurace při experimentování s
 * nestandardními formáty.
 *
 * @param dpb Disk Parameter Block.
 * @param source Textový popis zdroje konfigurace (preset/custom).
 *
 * @return MZDSK_RES_OK vždy.
 */
static en_MZDSK_RES cmd_dpb ( const st_MZDSK_CPM_DPB *dpb, const char *source,
                               en_OUTFMT output_format ) {

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nDisk Parameter Block (%s):\n\n", source );

        printf ( "  SPT  = %-5d   Logical (128B) sectors per track\n", dpb->spt );
        printf ( "  BSH  = %-5d   Block shift factor (block_size = 128 << BSH)\n", dpb->bsh );
        printf ( "  BLM  = %-5d   Block mask ((1 << BSH) - 1)\n", dpb->blm );
        printf ( "  EXM  = %-5d   Extent mask\n", dpb->exm );
        printf ( "  DSM  = %-5d   Total blocks - 1 (%d blocks)\n", dpb->dsm, dpb->dsm + 1 );
        printf ( "  DRM  = %-5d   Directory entries - 1 (%d entries)\n", dpb->drm, dpb->drm + 1 );
        printf ( "  AL0  = 0x%02X    Directory allocation byte 0\n", dpb->al0 );
        printf ( "  AL1  = 0x%02X    Directory allocation byte 1\n", dpb->al1 );
        printf ( "  CKS  = %-5d   Check vector size\n", dpb->cks );
        printf ( "  OFF  = %-5d   Reserved tracks\n", dpb->off );

        printf ( "\n  Derived:\n" );
        printf ( "  Block size  = %d B\n", dpb->block_size );
        printf ( "  Phys. sect. = %d per track (%d x 512B)\n", dpb->spt / 4, dpb->spt / 4 );
        printf ( "  Dir. size   = %d B (%d blocks)\n",
                 ( dpb->drm + 1 ) * MZDSK_CPM_DIRENTRY_SIZE,
                 ( ( dpb->drm + 1 ) * MZDSK_CPM_DIRENTRY_SIZE + dpb->block_size - 1 ) / dpb->block_size );
        printf ( "  Total cap.  = %lu kB\n", (unsigned long) ( (uint32_t) ( dpb->dsm + 1 ) * dpb->block_size / 1024 ) );
        printf ( "\n" );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "source", source );
        outfmt_kv_int ( &ctx, "spt", dpb->spt );
        outfmt_kv_int ( &ctx, "bsh", dpb->bsh );
        outfmt_kv_int ( &ctx, "blm", dpb->blm );
        outfmt_kv_int ( &ctx, "exm", dpb->exm );
        outfmt_kv_int ( &ctx, "dsm", dpb->dsm );
        outfmt_kv_int ( &ctx, "drm", dpb->drm );
        outfmt_kv_int ( &ctx, "al0", dpb->al0 );
        outfmt_kv_int ( &ctx, "al1", dpb->al1 );
        outfmt_kv_int ( &ctx, "cks", dpb->cks );
        outfmt_kv_int ( &ctx, "off", dpb->off );
        outfmt_kv_int ( &ctx, "block_size", dpb->block_size );
        outfmt_kv_uint ( &ctx, "total_bytes", (unsigned long) ( (uint32_t) ( dpb->dsm + 1 ) * dpb->block_size ) );
        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/* =========================================================================
 * Verze a nápověda
 * ========================================================================= */


/**
 * @brief Vypíše verze všech použitých knihoven.
 */
static void print_lib_versions ( void ) {
    printf ( "mzdsk-cpm %s (%s %s)\n\n",
             MZDSK_CPM_TOOL_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
    printf ( "Library versions:\n" );
    printf ( "  mzdsk_cpm         %s\n", mzdsk_cpm_version() );
    printf ( "  mzdsk_cpm_mzf     %s\n", mzdsk_cpm_mzf_version() );
    printf ( "  mzdsk_global      %s\n", mzdsk_global_version() );
    printf ( "  mzdsk_hexdump     %s\n", mzdsk_hexdump_version() );
    printf ( "  output_format     %s\n", output_format_version() );
    printf ( "  generic_driver    %s\n", generic_driver_version() );
}


/* =========================================================================
 * Raw block operations
 * ========================================================================= */




/**
 * @brief Provede hexdump CP/M alokačního bloku.
 *
 * @param disc         Kontext disku.
 * @param dpb          Disk Parameter Block.
 * @param block        Číslo CP/M bloku.
 * @param size         Počet bajtů (0 = celý blok).
 * @param dump_charset Režim konverze znakové sady v ASCII sloupci.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_dump_block ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                      uint16_t block, size_t size,
                                      en_MZDSK_HEXDUMP_CHARSET dump_charset ) {

    if ( size == 0 ) size = dpb->block_size;

    uint8_t track, sector;
    uint16_t offset;
    mzdsk_cpm_block_to_physical ( dpb, block, &track, &sector, &offset );

    printf ( "\nDump %u B from CP/M block %u (track %u, sector %u, offset %u):\n\n",
             (unsigned) size, block, track, sector, offset );

    while ( size > 0 ) {
        uint16_t chunk = ( size > dpb->block_size ) ? dpb->block_size : (uint16_t) size;
        uint8_t *buf = (uint8_t *) malloc ( dpb->block_size );
        if ( buf == NULL ) return MZDSK_RES_UNKNOWN_ERROR;

        en_MZDSK_RES res = mzdsk_cpm_read_block ( disc, dpb, block, buf, chunk );
        if ( res != MZDSK_RES_OK ) {
            free ( buf );
            return res;
        }

        if ( size > dpb->block_size ) {
            mzdsk_cpm_block_to_physical ( dpb, block, &track, &sector, &offset );
            printf ( "Block %u (track %u, sector %u):\n", block, track, sector );
        }

        st_MZDSK_HEXDUMP_CFG hcfg;
        mzdsk_hexdump_init ( &hcfg );
        hcfg.charset = dump_charset;
        mzdsk_hexdump ( &hcfg, buf, chunk );
        free ( buf );

        block++;
        size -= chunk;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Extrahuje data z CP/M alokačního bloku do souboru.
 *
 * @param disc  Kontext disku.
 * @param dpb   Disk Parameter Block.
 * @param block Číslo CP/M bloku.
 * @param out_file Výstupní soubor.
 * @param size  Počet bajtů (0 = celý blok).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_get_block ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                     uint16_t block, const char *out_file, size_t size ) {

    /* Audit M-16: kontrola existence výstupního souboru před čtením z disku. */
    if ( check_output_overwrite ( out_file ) != EXIT_SUCCESS ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    if ( size == 0 ) size = dpb->block_size;

    printf ( "\nExtract %u B from CP/M block %u into file: %s\n\n", (unsigned) size, block, out_file );

    FILE *fh = fopen ( out_file, "wb" );
    if ( fh == NULL ) {
        fprintf ( stderr, "Error: Could not open file '%s': %s\n", out_file, strerror ( errno ) );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    while ( size > 0 ) {
        uint16_t chunk = ( size > dpb->block_size ) ? dpb->block_size : (uint16_t) size;
        uint8_t *buf = (uint8_t *) malloc ( dpb->block_size );
        if ( buf == NULL ) { fclose ( fh ); return MZDSK_RES_UNKNOWN_ERROR; }

        en_MZDSK_RES res = mzdsk_cpm_read_block ( disc, dpb, block, buf, chunk );
        if ( res != MZDSK_RES_OK ) { free ( buf ); fclose ( fh ); return res; }

        if ( fwrite ( buf, 1, chunk, fh ) != chunk ) {
            fprintf ( stderr, "Error: Could not write to file: %s\n", strerror ( errno ) );
            free ( buf ); fclose ( fh );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        uint8_t tr, sec;
        uint16_t off;
        mzdsk_cpm_block_to_physical ( dpb, block, &tr, &sec, &off );
        printf ( "  Block %u (track %u, sector %u): %u B\n", block, tr, sec, chunk );

        free ( buf );
        block++;
        size -= chunk;
    }

    printf ( "\nDone.\n\n" );
    fclose ( fh );
    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše data z lokálního souboru do CP/M alokačního bloku.
 *
 * @param disc    Kontext disku.
 * @param dpb     Disk Parameter Block.
 * @param block   Číslo CP/M bloku.
 * @param in_file Vstupní soubor.
 * @param size    Počet bajtů (0 = celý soubor od offsetu).
 * @param offset  Offset ve vstupním souboru.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_put_block ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                     uint16_t block, const char *in_file,
                                     size_t size, size_t file_offset ) {

    FILE *fh = fopen ( in_file, "rb" );
    if ( fh == NULL ) {
        fprintf ( stderr, "Error: Could not open file '%s': %s\n", in_file, strerror ( errno ) );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    fseek ( fh, 0, SEEK_END );
    size_t filesize = (size_t) ftell ( fh );

    if ( file_offset > filesize ) {
        fprintf ( stderr, "Error: offset (%u) exceeds file size (%u)\n", (unsigned) file_offset, (unsigned) filesize );
        fclose ( fh );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    fseek ( fh, (long) file_offset, SEEK_SET );
    if ( size == 0 ) size = filesize - file_offset;

    printf ( "\nWrite %u B from file %s into CP/M block %u\n\n", (unsigned) size, in_file, block );

    while ( size > 0 ) {
        uint16_t chunk = ( size > dpb->block_size ) ? dpb->block_size : (uint16_t) size;
        uint8_t *buf = (uint8_t *) malloc ( dpb->block_size );
        if ( buf == NULL ) { fclose ( fh ); return MZDSK_RES_UNKNOWN_ERROR; }

        memset ( buf, 0, dpb->block_size );
        if ( fread ( buf, 1, chunk, fh ) != chunk && ferror ( fh ) ) {
            fprintf ( stderr, "Error: Could not read from file: %s\n", strerror ( errno ) );
            free ( buf ); fclose ( fh );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        en_MZDSK_RES res = mzdsk_cpm_write_block ( disc, dpb, block, buf, dpb->block_size );
        if ( res != MZDSK_RES_OK ) { free ( buf ); fclose ( fh ); return res; }

        uint8_t tr, sec;
        uint16_t off;
        mzdsk_cpm_block_to_physical ( dpb, block, &tr, &sec, &off );
        printf ( "  Block %u (track %u, sector %u): %u B\n", block, tr, sec, chunk );

        free ( buf );
        block++;
        size -= chunk;
    }

    printf ( "\nDone.\n\n" );
    fclose ( fh );
    return MZDSK_RES_OK;
}


/**
 * @brief Vypíše nápovědu k použití nástroje.
 *
 * @param[in] out Výstupní stream (stdout pro --help, stderr pro chybové cesty).
 */
static void print_usage ( FILE *out ) {
    fprintf ( out, "mzdsk-cpm " MZDSK_CPM_TOOL_VERSION " - CP/M filesystem management tool\n\n" );
    fprintf ( out, "Usage: mzdsk-cpm [options] <dsk_file> <command> [args...]\n\n" );
    fprintf ( out, "Global options:\n" );
    fprintf ( out, "  --format sd|hd       Set CP/M format (default: autodetect)\n" );
    fprintf ( out, "  --user N         Set user number 0-15 (default: 0; dir: all if omitted)\n" );
    fprintf ( out, "  --ro             Forced read-only mode\n" );
    fprintf ( out, "  --force          Operate on a disk that is not detected as CP/M\n" );
    fprintf ( out, "                   (required for 'format' on blank/non-CP/M disks)\n" );
    fprintf ( out, "  --overwrite      Allow 'get'/'get-block' to overwrite an existing output file\n" );
    fprintf ( out, "                   (default: refuse to overwrite)\n" );
    fprintf ( out, "  --yes, -y        Skip interactive confirmation prompt for destructive\n" );
    fprintf ( out, "                   operations ('format', 'defrag'); non-TTY is always silent\n" );
    fprintf ( out, "  --output FMT     Output format: text (default), json, csv\n" );
    fprintf ( out, "  -o FMT           Same as --output\n" );
    fprintf ( out, "  --version        Show version\n" );
    fprintf ( out, "  --lib-versions   Show library versions\n\n" );
    fprintf ( out, "Custom DPB parameters (override format preset):\n" );
    fprintf ( out, "  --spt N          Logical (128B) sectors per track\n" );
    fprintf ( out, "  --bsh N          Block shift factor (3-7), block_size = 128 << BSH\n" );
    fprintf ( out, "  --exm N          Extent mask\n" );
    fprintf ( out, "  --dsm N          Total blocks - 1\n" );
    fprintf ( out, "  --drm N          Directory entries - 1\n" );
    fprintf ( out, "  --al0 N          Directory allocation byte 0 (hex: 0xC0)\n" );
    fprintf ( out, "  --al1 N          Directory allocation byte 1 (hex: 0x00)\n" );
    fprintf ( out, "  --off N          Reserved tracks count\n\n" );
    fprintf ( out, "Directory operations:\n" );
    fprintf ( out, "  dir              List all files\n" );
    fprintf ( out, "  dir --ex         Extended listing with attributes\n" );
    fprintf ( out, "  dir --raw        Raw 32-byte directory entries\n" );
    fprintf ( out, "  file <name.ext>  Show file info\n\n" );
    fprintf ( out, "File operations:\n" );
    fprintf ( out, "  get <name.ext> <output>      Extract file from disk (raw binary)\n" );
    fprintf ( out, "  get --mzf <name.ext> <output.mzf> [options]\n" );
    fprintf ( out, "                               Extract as MZF (SharpMZ tape format)\n" );
    fprintf ( out, "    --ftype HH                 MZF file type (hex, default: 22;\n" );
    fprintf ( out, "                               known values: 01=OBJ, 02=BTX, 03=BSD,\n" );
    fprintf ( out, "                               04=BRD, 05=RB, 22=CP/M export convention)\n" );
    fprintf ( out, "    --exec-addr N              Exec address written to MZF 'fexec' field\n" );
    fprintf ( out, "                               (default: 0x0100; --addr N kept as alias)\n" );
    fprintf ( out, "    --strt-addr N              Load address written to MZF 'fstrt' field\n" );
    fprintf ( out, "                               (default: 0x0100)\n" );
    fprintf ( out, "    --no-attrs                 Don't encode R/O, SYS, ARC attributes into\n" );
    fprintf ( out, "                               bit 7 of fname[9..11] (only applies to\n" );
    fprintf ( out, "                               --ftype 22; other types never encode attrs)\n" );
    fprintf ( out, "  get --all <dir> [options]    Extract all files to directory\n" );
    fprintf ( out, "    --mzf                      Extract as MZF (options from 'get --mzf' apply)\n" );
    fprintf ( out, "    --by-user                  Create subdirectories per user (user00-user15)\n" );
    fprintf ( out, "    --on-duplicate MODE        Handle duplicate names: rename (default), overwrite, skip\n" );
    fprintf ( out, "    --ftype HH                 MZF file type (hex, default: 22)\n" );
    fprintf ( out, "    --exec-addr N              MZF exec address (default: 0x0100)\n" );
    fprintf ( out, "    --strt-addr N              MZF load address (default: 0x0100)\n" );
    fprintf ( out, "    --no-attrs                 Don't encode CP/M attrs into fname (ftype 22 only)\n" );
    fprintf ( out, "  put <input> <name.ext>       Write file to disk (raw binary)\n" );
    fprintf ( out, "  put <input> --name NAME.EXT  Write file with strict name validation\n" );
    fprintf ( out, "                               (no truncation; errors on 8.3 overflow or forbidden chars)\n" );
    fprintf ( out, "  put --mzf <input.mzf> [name.ext]\n" );
    fprintf ( out, "                               Write MZF with CPM-IC header to disk\n" );
    fprintf ( out, "                               name.ext overrides filename from MZF header\n" );
    fprintf ( out, "  put --mzf <input.mzf> --name NAME.EXT\n" );
    fprintf ( out, "                               Strict override (no truncation)\n" );
    fprintf ( out, "    --charset MODE             Sharp MZ variant for fname conversion (eu default, jp)\n" );
    fprintf ( out, "                               Applies to general MZF (ftype != 0x22); ignored for 0x22.\n" );
    fprintf ( out, "    --force-charset            Apply --charset also for ftype==0x22 (override CPM-IC ASCII)\n" );
    fprintf ( out, "                               Use for foreign MZF with 0x22 marker but Sharp-encoded fname.\n" );
    fprintf ( out, "    --no-attrs                 Don't decode R/O, SYS, ARC attributes from fname bit 7\n" );
    fprintf ( out, "                               (ftype 22 only; use for foreign MZF where bit 7 is not attrs)\n" );
    fprintf ( out, "  era <name.ext>               Delete file\n" );
    fprintf ( out, "  ren <old.ext> <new.ext>      Rename file\n" );
    fprintf ( out, "  chuser <name.ext> <user>     Change user number (0-15) of existing file\n" );
    fprintf ( out, "                               Source user taken from global --user N (default 0)\n\n" );
    fprintf ( out, "Attributes:\n" );
    fprintf ( out, "  attr <name.ext>              Show attributes\n" );
    fprintf ( out, "  attr <name.ext> [+|-][RSA]   Set/clear attributes\n" );
    fprintf ( out, "       +R = Read-Only, +S = System, +A = Archived\n" );
    fprintf ( out, "       Combine: +RSA, +S+A, +R-S (set R/O, clear System)\n\n" );
    fprintf ( out, "Raw block operations:\n" );
    fprintf ( out, "  dump-block N [bytes]             Hexdump of CP/M block(s)\n" );
    fprintf ( out, "    --dump-charset MODE            ASCII column charset: raw (default), eu, jp, utf8-eu, utf8-jp\n" );
    fprintf ( out, "  get-block N <file> [bytes]       Extract CP/M block(s) to file\n" );
    fprintf ( out, "  put-block N <file> [bytes] [off] Write file into CP/M block(s)\n\n" );
    fprintf ( out, "Disk operations:\n" );
    fprintf ( out, "  dpb              Show active DPB parameters\n" );
    fprintf ( out, "  map              Show allocation map\n" );
    fprintf ( out, "  free             Show free space\n" );
    fprintf ( out, "  format           Initialize empty directory (0xE5)\n" );
    fprintf ( out, "  defrag           Defragment disk (rewrite files sequentially)\n" );
}


/* =========================================================================
 * Hlavní funkce
 * ========================================================================= */


/**
 * @brief Parsuje celočíselnou hodnotu z řetězce (decimálně nebo hexadecimálně).
 *
 * Podporuje prefix "0x" nebo "0X" pro hexadecimální hodnoty.
 * Hodnota musí být v rozsahu 0..max_val.
 *
 * @param[in]  str Vstupní řetězec. Nesmí být NULL.
 * @param[in]  name Jméno parametru (pro chybové hlášení).
 * @param[in]  max_val Maximální povolená hodnota.
 * @param[out] result Výstupní hodnota.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě (s výpisem na stderr).
 */
static int parse_uint_arg ( const char *str, const char *name,
                            unsigned long max_val, unsigned long *result ) {

    if ( str == NULL || *str == '\0' ) {
        fprintf ( stderr, "Error: Empty value for %s\n", name );
        return EXIT_FAILURE;
    }

    /* Audit M-15: strtoul tiše akceptuje znaménko '-' (vrátí wrap-around
       např. '-1' → ULONG_MAX). Odmítnout explicitně. */
    if ( str[0] == '-' ) {
        fprintf ( stderr, "Error: Negative value '%s' not allowed for %s\n", str, name );
        return EXIT_FAILURE;
    }

    char *endptr;
    int base = 10;

    if ( str[0] == '0' && ( str[1] == 'x' || str[1] == 'X' ) ) {
        base = 16;
    }

    /* Audit M-15: errno=0 + ERANGE kontrola zachytí přetečení
       (např. --user 999999999999999999 → ULONG_MAX bez chyby). */
    errno = 0;
    *result = strtoul ( str, &endptr, base );

    if ( errno == ERANGE ) {
        fprintf ( stderr, "Error: Value '%s' for %s is out of range\n", str, name );
        return EXIT_FAILURE;
    }

    if ( endptr == str || *endptr != '\0' ) {
        fprintf ( stderr, "Error: Invalid value '%s' for %s\n", str, name );
        return EXIT_FAILURE;
    }

    if ( *result > max_val ) {
        fprintf ( stderr, "Error: Value %lu for %s exceeds maximum %lu\n",
                  *result, name, max_val );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Vstupní bod programu mzdsk-cpm.
 *
 * Parsuje argumenty příkazové řádky, otevře DSK obraz, detekuje
 * nebo nastaví CP/M formát, provede požadovanou operaci a zavře disk.
 *
 * Pořadí zpracování argumentů:
 * 1. Globální přepínače (--help, --version, --lib-versions, --format, --user, --ro)
 * 2. Custom DPB přepínače (--spt, --bsh, --exm, --dsm, --drm, --al0, --al1, --off)
 * 3. Cesta k DSK souboru (první nepřepínačový argument)
 * 4. Podpříkaz (druhý nepřepínačový argument)
 * 5. Argumenty podpříkazu
 *
 * Pokud jsou zadány custom DPB přepínače, přepíšou příslušné hodnoty
 * v presetu zvoleného formátu (SD/HD/autodetekce). Odvozené hodnoty
 * (BLM, block_size, CKS) se přepočtou automaticky.
 *
 * Pro čtecí operace se disk otevírá v režimu RO,
 * pro zápisové operace v režimu RW.
 *
 * @param argc Počet argumentů.
 * @param argv Pole řetězců s argumenty.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
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
    int i = 1; /* přeskočíme sub_argv[0] = název subpříkazu */
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


int main ( int argc, char *argv[] ) {

    memory_driver_init();

    if ( argc < 2 ) {
        print_usage ( stderr );
        return EXIT_FAILURE;
    }

    /* Definice dlouhých voleb pro getopt_long.
     * Interní kódy 1-8 pro custom DPB volby (nemají krátkou variantu). */
    enum {
        OPT_SPT = 1, OPT_BSH, OPT_EXM, OPT_DSM, OPT_DRM, OPT_AL0, OPT_AL1, OPT_OFF
    };

    static struct option global_options[] = {
        { "ro",           no_argument,       NULL, 'R' },
        { "force",        no_argument,       NULL, 'F' },
        { "overwrite",    no_argument,       NULL, 'O' },
        { "yes",          no_argument,       NULL, 'y' },
        { "format",       required_argument, NULL, 'f' },
        { "output",       required_argument, NULL, 'o' },
        { "user",         required_argument, NULL, 'u' },
        { "spt",          required_argument, NULL, OPT_SPT },
        { "bsh",          required_argument, NULL, OPT_BSH },
        { "exm",          required_argument, NULL, OPT_EXM },
        { "dsm",          required_argument, NULL, OPT_DSM },
        { "drm",          required_argument, NULL, OPT_DRM },
        { "al0",          required_argument, NULL, OPT_AL0 },
        { "al1",          required_argument, NULL, OPT_AL1 },
        { "off",          required_argument, NULL, OPT_OFF },
        { "version",      no_argument,       NULL, 'V' },
        { "lib-versions", no_argument,       NULL, 'L' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL,           0,                 NULL,  0  }
    };

    /* Parsování globálních voleb přes getopt_long */
    int force_ro = 0;
    int force_fs = 0;     /**< --force: obejít kontrolu, že disk je CP/M */
    int format_forced = 0;
    en_MZDSK_CPM_FORMAT forced_format = MZDSK_CPM_FORMAT_SD;
    uint8_t user = 0;
    int user_specified = 0;
    en_OUTFMT output_format = OUTFMT_TEXT;

    /* Custom DPB přepínače (-1 = nezadáno) */
    long custom_spt = -1;
    long custom_bsh = -1;
    long custom_exm = -1;
    long custom_dsm = -1;
    long custom_drm = -1;
    long custom_al0 = -1;
    long custom_al1 = -1;
    long custom_off = -1;
    int has_custom_dpb = 0;

    optind = 1;
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "+o:yh", global_options, NULL ) ) != -1 ) {
        switch ( opt ) {

            case 'V': /* --version */
                printf ( "mzdsk-cpm %s (%s %s)\n",
                         MZDSK_CPM_TOOL_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
                return EXIT_SUCCESS;

            case 'L': /* --lib-versions */
                print_lib_versions();
                return EXIT_SUCCESS;

            case 'h': /* --help */
                print_usage ( stdout );
                return EXIT_SUCCESS;

            case 'R': /* --ro */
                force_ro = 1;
                break;

            case 'F': /* --force */
                force_fs = 1;
                break;

            case 'O': /* --overwrite (audit M-16) */
                g_allow_overwrite = 1;
                break;

            case 'y': /* --yes (audit M-17) */
                g_assume_yes = 1;
                break;

            case 'f': /* --format sd|hd */
                if ( strcmp ( optarg, "sd" ) == 0 || strcmp ( optarg, "SD" ) == 0 ) {
                    forced_format = MZDSK_CPM_FORMAT_SD;
                    format_forced = 1;
                } else if ( strcmp ( optarg, "hd" ) == 0 || strcmp ( optarg, "HD" ) == 0 ) {
                    forced_format = MZDSK_CPM_FORMAT_HD;
                    format_forced = 1;
                } else {
                    fprintf ( stderr, "Error: Unknown format '%s' (use sd or hd)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;

            case 'o': /* --output FMT, -o FMT */
                if ( outfmt_parse ( optarg, &output_format ) != 0 ) {
                    fprintf ( stderr, "Error: Unknown output format '%s' (use text, json or csv)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;

            case 'u': { /* --user N */
                char *endptr;
                unsigned long val = strtoul ( optarg, &endptr, 10 );
                if ( endptr == optarg || *endptr != '\0' || val > 15 ) {
                    fprintf ( stderr, "Error: Invalid user number '%s' (0-15)\n", optarg );
                    return EXIT_FAILURE;
                }
                user = (uint8_t) val;
                user_specified = 1;
                break;
            }

            /* Custom DPB volby */
            case OPT_SPT: {
                unsigned long val;
                if ( parse_uint_arg ( optarg, "--spt", 65535, &val ) != EXIT_SUCCESS ) return EXIT_FAILURE;
                custom_spt = (long) val; has_custom_dpb = 1;
                break;
            }
            case OPT_BSH: {
                unsigned long val;
                if ( parse_uint_arg ( optarg, "--bsh", 7, &val ) != EXIT_SUCCESS ) return EXIT_FAILURE;
                if ( val < 3 ) { fprintf ( stderr, "Error: --bsh must be 3-7\n" ); return EXIT_FAILURE; }
                custom_bsh = (long) val; has_custom_dpb = 1;
                break;
            }
            case OPT_EXM: {
                unsigned long val;
                if ( parse_uint_arg ( optarg, "--exm", 255, &val ) != EXIT_SUCCESS ) return EXIT_FAILURE;
                custom_exm = (long) val; has_custom_dpb = 1;
                break;
            }
            case OPT_DSM: {
                unsigned long val;
                if ( parse_uint_arg ( optarg, "--dsm", 65535, &val ) != EXIT_SUCCESS ) return EXIT_FAILURE;
                custom_dsm = (long) val; has_custom_dpb = 1;
                break;
            }
            case OPT_DRM: {
                unsigned long val;
                if ( parse_uint_arg ( optarg, "--drm", 65535, &val ) != EXIT_SUCCESS ) return EXIT_FAILURE;
                custom_drm = (long) val; has_custom_dpb = 1;
                break;
            }
            case OPT_AL0: {
                unsigned long val;
                if ( parse_uint_arg ( optarg, "--al0", 255, &val ) != EXIT_SUCCESS ) return EXIT_FAILURE;
                custom_al0 = (long) val; has_custom_dpb = 1;
                break;
            }
            case OPT_AL1: {
                unsigned long val;
                if ( parse_uint_arg ( optarg, "--al1", 255, &val ) != EXIT_SUCCESS ) return EXIT_FAILURE;
                custom_al1 = (long) val; has_custom_dpb = 1;
                break;
            }
            case OPT_OFF: {
                unsigned long val;
                if ( parse_uint_arg ( optarg, "--off", 65535, &val ) != EXIT_SUCCESS ) return EXIT_FAILURE;
                custom_off = (long) val; has_custom_dpb = 1;
                break;
            }

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
            case 'V': printf ( "mzdsk-cpm %s (%s %s)\n", MZDSK_CPM_TOOL_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION ); return EXIT_SUCCESS;
            case 'L': print_lib_versions(); return EXIT_SUCCESS;
            case 'h': print_usage ( stdout ); return EXIT_SUCCESS;
            case 'R': force_ro = 1; break;
            case 'F': force_fs = 1; break;
            case 'O': g_allow_overwrite = 1; break;  /* audit M-16 */
            case 'y': g_assume_yes = 1; break;       /* audit M-17 */
            case 'f':
                if ( strcmp ( optarg, "sd" ) == 0 || strcmp ( optarg, "SD" ) == 0 ) {
                    forced_format = MZDSK_CPM_FORMAT_SD; format_forced = 1;
                } else if ( strcmp ( optarg, "hd" ) == 0 || strcmp ( optarg, "HD" ) == 0 ) {
                    forced_format = MZDSK_CPM_FORMAT_HD; format_forced = 1;
                } else {
                    fprintf ( stderr, "Error: Unknown format '%s' (use sd or hd)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;
            case 'o':
                if ( outfmt_parse ( optarg, &output_format ) != 0 ) {
                    fprintf ( stderr, "Error: Unknown output format '%s' (use text, json or csv)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;
            case 'u': {
                char *endptr;
                unsigned long val = strtoul ( optarg, &endptr, 10 );
                if ( endptr == optarg || *endptr != '\0' || val > 15 ) {
                    fprintf ( stderr, "Error: Invalid user number '%s' (0-15)\n", optarg );
                    return EXIT_FAILURE;
                }
                user = (uint8_t) val; user_specified = 1;
                break;
            }
            case '?':
                /* neznámá volba - getopt_long už vypsal chybu */
                return EXIT_FAILURE;
            default:
                /* Custom DPB volby (--spt, --bsh, ...) uvedené za DSK souborem.
                 * Audit M-23: dřív se tiše zahazovaly, což vedlo k operaci
                 * s default DPB místo custom nastavení - uživatel si myslel,
                 * že jeho --spt je aktivní. Nyní explicitní chyba s radou. */
                fprintf ( stderr, "Error: Custom DPB options (--spt, --bsh, --exm, --dsm, --drm, --al0, --al1, --cks, --off) must be specified BEFORE the DSK file.\n" );
                fprintf ( stderr, "       Example: mzdsk-cpm --spt 32 --bsh 4 ... disk.dsk format\n" );
                return EXIT_FAILURE;
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

    /* Určení zda jde o zápisovou operaci.
     * attr je zápisová jen s modifikátory (+/-), bez nich jen zobrazuje. */
    int is_write_op = 0;
    if ( strcmp ( subcmd, "put" ) == 0 ||
         strcmp ( subcmd, "era" ) == 0 ||
         strcmp ( subcmd, "ren" ) == 0 ||
         strcmp ( subcmd, "chuser" ) == 0 ||
         ( strcmp ( subcmd, "attr" ) == 0 && sub_argc > 2 ) ||
         strcmp ( subcmd, "put-block" ) == 0 ||
         strcmp ( subcmd, "format" ) == 0 ||
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
        fprintf ( stderr, "Error: Could not open DSK file '%s': %s\n", dsk_filename, mzdsk_get_error ( err ) );
        return EXIT_FAILURE;
    }

    /* Kontrola, zda je disk skutečně CP/M. Bez --force odmítneme
       non-CP/M disky, aby se zabránilo tichému poškození dat
       (FSMZ FAREA by se při 'format' přepsala 0xE5 atd.). */
    {
        st_MZDSK_DETECT_RESULT det;
        memset ( &det, 0, sizeof ( det ) );
        mzdsk_detect_filesystem ( &disc, &det );
        if ( det.type != MZDSK_FS_CPM ) {
            const char *fs_name;
            switch ( det.type ) {
                case MZDSK_FS_FSMZ:      fs_name = "FSMZ (MZ-BASIC)"; break;
                case MZDSK_FS_MRS:       fs_name = "MRS"; break;
                case MZDSK_FS_BOOT_ONLY: fs_name = "FSMZ boot track only"; break;
                default:                 fs_name = "unknown"; break;
            }
            if ( !force_fs ) {
                fprintf ( stderr,
                    "Error: disk '%s' is not a CP/M filesystem (detected: %s)\n"
                    "       Use --force to operate on a non-CP/M disk (e.g. to format it as CP/M).\n",
                    dsk_filename, fs_name );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            fprintf ( stderr,
                "Warning: disk '%s' is not a CP/M filesystem (detected: %s); continuing due to --force\n",
                dsk_filename, fs_name );
        }
    }

    /* Detekce nebo nastavení CP/M formátu */
    en_MZDSK_CPM_FORMAT cpm_format;
    if ( format_forced ) {
        cpm_format = forced_format;
        /* Varování pokud vynucený formát neodpovídá detekovanému */
        en_MZDSK_CPM_FORMAT detected = mzdsk_cpm_detect_format ( &disc );
        if ( detected != cpm_format ) {
            fprintf ( stderr, "Warning: forced format does not match detected disk geometry\n" );
        }
    } else {
        cpm_format = mzdsk_cpm_detect_format ( &disc );
    }

    /* Inicializace DPB - preset jako základ */
    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, cpm_format );

    /* Aplikace custom DPB přepínačů (přepíšou odpovídající pole v presetu) */
    if ( has_custom_dpb ) {
        if ( custom_spt >= 0 ) dpb.spt = (uint16_t) custom_spt;
        if ( custom_bsh >= 0 ) dpb.bsh = (uint8_t) custom_bsh;
        if ( custom_exm >= 0 ) dpb.exm = (uint8_t) custom_exm;
        if ( custom_dsm >= 0 ) dpb.dsm = (uint16_t) custom_dsm;
        if ( custom_drm >= 0 ) dpb.drm = (uint16_t) custom_drm;
        if ( custom_al0 >= 0 ) dpb.al0 = (uint8_t) custom_al0;
        if ( custom_al1 >= 0 ) dpb.al1 = (uint8_t) custom_al1;
        if ( custom_off >= 0 ) dpb.off = (uint16_t) custom_off;

        /* Přepočteme odvozené hodnoty z BSH */
        dpb.blm = (uint8_t) ( ( 1u << dpb.bsh ) - 1 );
        dpb.block_size = (uint16_t) ( 128u << dpb.bsh );
        dpb.cks = ( dpb.drm + 1 ) / 4;
    }

    /* Textový popis zdroje konfigurace */
    const char *dpb_source;
    if ( has_custom_dpb ) {
        dpb_source = "custom";
    } else if ( format_forced ) {
        switch ( cpm_format ) {
            case MZDSK_CPM_FORMAT_HD: dpb_source = "HD preset"; break;
            default:                  dpb_source = "SD preset"; break;
        }
    } else {
        dpb_source = "autodetected";
    }

    /* Zobrazení informací o disku (jen pro textový výstup) */
    if ( output_format == OUTFMT_TEXT ) {
        err = cmd_disc_info ( &disc, &dpb, cpm_format );
    }

    /* Zpracování podpříkazu */

    /* ---- dpb ---- */
    if ( strcmp ( subcmd, "dpb" ) == 0 ) {

        /* Kontrola neočekávaných argumentů */
        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'dpb' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_dpb ( &dpb, dpb_source, output_format );

    /* ---- dir ---- */
    } else if ( strcmp ( subcmd, "dir" ) == 0 ) {

        /* Filtr uživatele: -1 = zobraz všechny, 0-15 = pouze daný uživatel */
        int filter_user = user_specified ? (int) user : -1;

        static struct option dir_opts[] = {
            { "ex",  no_argument, NULL, 'e' },
            { "raw", no_argument, NULL, 'r' },
            { NULL,  0,           NULL,  0  }
        };
        int dir_mode = 0; /* 0=normal, 1=ex, 2=raw */
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", dir_opts, NULL ) ) != -1 ) {
            if ( c == 'e' ) dir_mode = 1;
            else if ( c == 'r' ) dir_mode = 2;
            else { mzdsk_disc_close ( &disc ); return EXIT_FAILURE; }
        }

        /* Kontrola neočekávaných pozičních argumentů */
        if ( optind < sub_argc ) {
            fprintf ( stderr, "Error: 'dir' does not accept extra arguments (got '%s')\n", sub_argv[optind] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

        if ( dir_mode == 1 ) {
            err = cmd_dir_ex ( &disc, &dpb, filter_user, output_format );
        } else if ( dir_mode == 2 ) {
            err = cmd_dir_raw ( &disc, &dpb, output_format );
        } else {
            err = cmd_dir ( &disc, &dpb, filter_user, output_format );
        }

    /* ---- file ---- */
    } else if ( strcmp ( subcmd, "file" ) == 0 ) {

        if ( sub_argc < 2 ) {
            fprintf ( stderr, "Error: file command requires <name.ext>\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Kontrola nadbytečných argumentů */
        if ( sub_argc > 2 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[2] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_file_info ( &disc, &dpb, sub_argv[1], user, output_format );

    /* ---- get ---- */
    } else if ( strcmp ( subcmd, "get" ) == 0 ) {

        static struct option get_opts[] = {
            { "all",          no_argument,       NULL, 'a' },
            { "mzf",          no_argument,       NULL, 'm' },
            { "exec-addr",    required_argument, NULL, 'A' },
            { "addr",         required_argument, NULL, 'A' }, /* alias pro --exec-addr (BUG 3 legacy) */
            { "ftype",        required_argument, NULL, 'F' },
            { "strt-addr",    required_argument, NULL, 'S' },
            { "no-attrs",     no_argument,       NULL, 'N' },
            { "by-user",      no_argument,       NULL, 'U' },
            { "on-duplicate", required_argument, NULL, 'D' },
            { NULL,           0,                 NULL,  0  }
        };
        int get_all = 0;
        int ga_mzf = 0;
        int ga_by_user = 0;
        en_DUPLICATE_MODE ga_dup_mode = DUP_RENAME;
        uint16_t ga_exec_addr = MZDSK_CPM_MZF_DEFAULT_ADDR;
        uint8_t  ga_ftype = MZDSK_CPM_MZF_FTYPE;
        uint16_t ga_strt_addr = MZDSK_CPM_MZF_DEFAULT_ADDR;
        int ga_encode_attrs = 1;
        /* Sentinel příznaky pro striktní validaci kompatibility voleb.
         * Sledujeme, zda byla volba skutečně zadána - to nelze odvodit
         * z hodnoty samotné, protože defaulty (např. MZDSK_CPM_MZF_FTYPE)
         * jsou validními uživatelskými hodnotami. */
        int ga_ftype_set = 0;
        int ga_exec_addr_set = 0;
        int ga_strt_addr_set = 0;
        int ga_no_attrs_set = 0;
        int ga_dup_mode_set = 0;

        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", get_opts, NULL ) ) != -1 ) {
            switch ( c ) {
                case 'a': get_all = 1; break;
                case 'm': ga_mzf = 1; break;
                case 'U': ga_by_user = 1; break;
                case 'A': {
                    unsigned long val;
                    if ( parse_uint_arg ( optarg, "--exec-addr", 0xFFFF, &val ) != EXIT_SUCCESS ) {
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    ga_exec_addr = (uint16_t) val;
                    ga_exec_addr_set = 1;
                    break;
                }
                case 'F': {
                    unsigned long val;
                    if ( parse_uint_arg ( optarg, "--ftype", 0xFF, &val ) != EXIT_SUCCESS ) {
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    ga_ftype = (uint8_t) val;
                    ga_ftype_set = 1;
                    break;
                }
                case 'S': {
                    unsigned long val;
                    if ( parse_uint_arg ( optarg, "--strt-addr", 0xFFFF, &val ) != EXIT_SUCCESS ) {
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    ga_strt_addr = (uint16_t) val;
                    ga_strt_addr_set = 1;
                    break;
                }
                case 'N':
                    ga_encode_attrs = 0;
                    ga_no_attrs_set = 1;
                    break;
                case 'D':
                    if ( strcmp ( optarg, "overwrite" ) == 0 ) {
                        ga_dup_mode = DUP_OVERWRITE;
                    } else if ( strcmp ( optarg, "skip" ) == 0 ) {
                        ga_dup_mode = DUP_SKIP;
                    } else if ( strcmp ( optarg, "rename" ) == 0 ) {
                        ga_dup_mode = DUP_RENAME;
                    } else {
                        fprintf ( stderr, "Error: Unknown --on-duplicate mode '%s'"
                                  " (use rename, overwrite or skip)\n", optarg );
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    ga_dup_mode_set = 1;
                    break;
                default:
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
            }
        }

        /* Striktní validace kompatibility voleb (konzistentní s mzdsk-mrs).
         * Tiché ignorování neaplikovatelných voleb by skrylo chybu v pipeline. */
        int mzf_only_flag_set = ga_ftype_set || ga_exec_addr_set ||
                                ga_strt_addr_set || ga_no_attrs_set;
        if ( mzf_only_flag_set && !ga_mzf ) {
            fprintf ( stderr, "Error: --ftype/--exec-addr/--strt-addr/--no-attrs require --mzf\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        if ( ga_by_user && !get_all ) {
            fprintf ( stderr, "Error: --by-user can only be used with --all\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        if ( ga_dup_mode_set && !get_all ) {
            fprintf ( stderr, "Error: --on-duplicate can only be used with --all\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

        int pos_remaining = sub_argc - optind;

        if ( get_all ) {
            /* get --all <dir> [--mzf] [--by-user] [--on-duplicate MODE] [--exec-addr N] */
            if ( pos_remaining < 1 ) {
                fprintf ( stderr, "Error: get --all requires <dir> argument\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* Kontrola nadbytečných pozičních argumentů */
            if ( pos_remaining > 1 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            int filter_user = user_specified ? (int) user : -1;
            err = cmd_get_all ( &disc, &dpb, sub_argv[optind], filter_user,
                                ga_mzf, ga_by_user, ga_dup_mode,
                                ga_ftype, ga_exec_addr, ga_strt_addr, ga_encode_attrs );
        } else if ( ga_mzf ) {
            /* get --mzf <name.ext> <output.mzf> [--exec-addr N] [--ftype HH] [--strt-addr N] [--no-attrs] */
            if ( pos_remaining < 2 ) {
                fprintf ( stderr, "Error: get --mzf requires <name.ext> <output.mzf>\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* Kontrola nadbytečných pozičních argumentů */
            if ( pos_remaining > 2 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_get_mzf ( &disc, &dpb, sub_argv[optind], sub_argv[optind + 1],
                                user, ga_ftype, ga_exec_addr, ga_strt_addr, ga_encode_attrs );
        } else {
            if ( pos_remaining < 2 ) {
                fprintf ( stderr, "Error: get command requires <name.ext> <output>\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* Kontrola nadbytečných pozičních argumentů */
            if ( pos_remaining > 2 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_get ( &disc, &dpb, sub_argv[optind], sub_argv[optind + 1], user );
        }

    /* ---- put ---- */
    } else if ( strcmp ( subcmd, "put" ) == 0 ) {

        static struct option put_opts[] = {
            { "mzf",           no_argument,       NULL, 'm' },
            { "name",          required_argument, NULL, 'n' },
            { "charset",       required_argument, NULL, 'c' },
            { "force-charset", no_argument,       NULL, 'F' },
            { "no-attrs",      no_argument,       NULL, 'N' },
            { NULL,            0,                 NULL,  0  }
        };
        int use_mzf = 0;
        const char *strict_name = NULL;
        /* Výchozí varianta Sharp MZ znakové sady pro konverzi fname u obecných
           MZF (ftype != 0x22). Default EU - naprostá většina MZF souborů
           pro MZ-700/800 používá evropskou variantu. */
        en_MZF_NAME_ENCODING put_charset = MZF_NAME_ASCII_EU;
        /* Flagy pro mzdsk_cpm_mzf_decode_ex2 - modifikátory zacházení
           s ftype==0x22 (CPM-IC konvence). */
        unsigned decode_flags = MZDSK_CPM_MZF_DECODE_DEFAULT;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", put_opts, NULL ) ) != -1 ) {
            if ( c == 'm' ) use_mzf = 1;
            else if ( c == 'n' ) strict_name = optarg;
            else if ( c == 'c' ) {
                if ( strcmp ( optarg, "eu" ) == 0 ) {
                    put_charset = MZF_NAME_ASCII_EU;
                } else if ( strcmp ( optarg, "jp" ) == 0 ) {
                    put_charset = MZF_NAME_ASCII_JP;
                } else {
                    fprintf ( stderr, "Error: Unknown --charset '%s' (use 'eu' or 'jp')\n", optarg );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
            }
            else if ( c == 'F' ) decode_flags |= MZDSK_CPM_MZF_DECODE_FORCE_CHARSET;
            else if ( c == 'N' ) decode_flags |= MZDSK_CPM_MZF_DECODE_NO_ATTRS;
            else { mzdsk_disc_close ( &disc ); return EXIT_FAILURE; }
        }
        int pos_remaining = sub_argc - optind;
        if ( use_mzf ) {
            if ( pos_remaining < 1 ) {
                fprintf ( stderr, "Error: put --mzf requires <input.mzf>\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* Kontrola nadbytečných pozičních argumentů (1 povinný + 1 volitelný) */
            if ( pos_remaining > 2 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* Striktní --name vyžaduje, že nebyl zadán poziční override. */
            if ( strict_name != NULL && pos_remaining >= 2 ) {
                fprintf ( stderr, "Error: --name conflicts with positional <name.ext>\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            const char *name_override;
            int is_strict;
            if ( strict_name != NULL ) {
                name_override = strict_name;
                is_strict = 1;
            } else {
                name_override = ( pos_remaining >= 2 ) ? sub_argv[optind + 1] : NULL;
                is_strict = 0;
            }
            err = cmd_put_mzf ( &disc, &dpb, sub_argv[optind], name_override,
                                 is_strict, user, put_charset, decode_flags );
        } else {
            /* --force-charset a --no-attrs mají smysl jen s --mzf */
            if ( decode_flags != MZDSK_CPM_MZF_DECODE_DEFAULT ) {
                fprintf ( stderr, "Error: --force-charset/--no-attrs require --mzf\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* Striktní --name nahrazuje 2. poziční argument. */
            if ( strict_name != NULL && pos_remaining >= 2 ) {
                fprintf ( stderr, "Error: --name conflicts with positional <name.ext>\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            if ( strict_name == NULL && pos_remaining < 2 ) {
                fprintf ( stderr, "Error: put command requires <input> <name.ext> or <input> --name NAME.EXT\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            if ( strict_name != NULL && pos_remaining < 1 ) {
                fprintf ( stderr, "Error: put --name requires <input>\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* Kontrola nadbytečných pozičních argumentů */
            if ( pos_remaining > 2 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
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
            err = cmd_put ( &disc, &dpb, sub_argv[optind], name_arg, is_strict, user );
        }

    /* ---- era ---- */
    } else if ( strcmp ( subcmd, "era" ) == 0 ) {

        if ( sub_argc < 2 ) {
            fprintf ( stderr, "Error: era command requires <name.ext>\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Kontrola nadbytečných argumentů */
        if ( sub_argc > 2 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[2] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_era ( &disc, &dpb, sub_argv[1], user );

    /* ---- ren ---- */
    } else if ( strcmp ( subcmd, "ren" ) == 0 ) {

        if ( sub_argc < 3 ) {
            fprintf ( stderr, "Error: ren command requires <old.ext> <new.ext>\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Kontrola nadbytečných argumentů */
        if ( sub_argc > 3 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[3] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_ren ( &disc, &dpb, sub_argv[1], sub_argv[2], user );

    /* ---- chuser ---- */
    } else if ( strcmp ( subcmd, "chuser" ) == 0 ) {

        if ( sub_argc < 3 ) {
            fprintf ( stderr, "Error: chuser command requires <name.ext> <new-user>\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Kontrola nadbytečných argumentů */
        if ( sub_argc > 3 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[3] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_chuser ( &disc, &dpb, sub_argv[1], user, sub_argv[2] );

    /* ---- attr ---- */
    } else if ( strcmp ( subcmd, "attr" ) == 0 ) {

        if ( sub_argc < 2 ) {
            fprintf ( stderr, "Error: attr command requires <name.ext> [+|-][RSA]...\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* attr_specs jsou argumenty za jménem souboru */
        err = cmd_attr ( &disc, &dpb, sub_argv[1], user, &sub_argv[2], sub_argc - 2 );

    /* ---- map ---- */
    } else if ( strcmp ( subcmd, "map" ) == 0 ) {

        /* Kontrola neočekávaných argumentů */
        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'map' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_map ( &disc, &dpb, output_format );

    /* ---- free ---- */
    } else if ( strcmp ( subcmd, "free" ) == 0 ) {

        /* Kontrola neočekávaných argumentů */
        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'free' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_free ( &disc, &dpb, output_format );

    /* ---- format ---- */
    } else if ( strcmp ( subcmd, "format" ) == 0 ) {

        /* Kontrola neočekávaných argumentů */
        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'format' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Audit M-17: interaktivní potvrzení destruktivní operace */
        if ( !confirm_destructive_op ( "format (erase all files)" ) ) {
            fprintf ( stderr, "Aborted.\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_format ( &disc, &dpb );

    /* ---- defrag ---- */
    } else if ( strcmp ( subcmd, "defrag" ) == 0 ) {

        /* Kontrola neočekávaných argumentů */
        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'defrag' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Audit M-17: interaktivní potvrzení (defrag reorganizuje data) */
        if ( !confirm_destructive_op ( "defragment (reorganize all file blocks)" ) ) {
            fprintf ( stderr, "Aborted.\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_defrag ( &disc, &dpb );

    /* ---- dump-block ---- */
    } else if ( strcmp ( subcmd, "dump-block" ) == 0 ) {

        static struct option dblk_opts[] = {
            { "dump-charset", required_argument, NULL, 'd' },
            { NULL,           0,                 NULL,  0  }
        };
        en_MZDSK_HEXDUMP_CHARSET dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", dblk_opts, NULL ) ) != -1 ) {
            if ( c == 'd' ) {
                if ( strcmp ( optarg, "raw" ) == 0 )          dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
                else if ( strcmp ( optarg, "eu" ) == 0 )      dump_charset = MZDSK_HEXDUMP_CHARSET_EU;
                else if ( strcmp ( optarg, "jp" ) == 0 )      dump_charset = MZDSK_HEXDUMP_CHARSET_JP;
                else if ( strcmp ( optarg, "utf8-eu" ) == 0 ) dump_charset = MZDSK_HEXDUMP_CHARSET_UTF8_EU;
                else if ( strcmp ( optarg, "utf8-jp" ) == 0 ) dump_charset = MZDSK_HEXDUMP_CHARSET_UTF8_JP;
                else {
                    fprintf ( stderr, "Error: Unknown dump-charset '%s' (use raw, eu, jp, utf8-eu or utf8-jp)\n", optarg );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
            } else { mzdsk_disc_close ( &disc ); return EXIT_FAILURE; }
        }
        if ( optind >= sub_argc ) {
            fprintf ( stderr, "Error: dump-block requires N [bytes]\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Kontrola nadbytečných argumentů */
        if ( sub_argc - optind > 2 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        unsigned long block_num;
        if ( parse_uint_arg ( sub_argv[optind], "block", dpb.dsm, &block_num ) != EXIT_SUCCESS ) {
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        size_t blk_size = 0;
        if ( optind + 1 < sub_argc ) {
            unsigned long val;
            if ( parse_uint_arg ( sub_argv[optind + 1], "size", 0xFFFFFFFF, &val ) != EXIT_SUCCESS ) {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            blk_size = (size_t) val;
        }
        err = cmd_dump_block ( &disc, &dpb, (uint16_t) block_num, blk_size, dump_charset );

    /* ---- get-block ---- */
    } else if ( strcmp ( subcmd, "get-block" ) == 0 ) {

        if ( sub_argc < 3 ) {
            fprintf ( stderr, "Error: get-block requires N <file> [bytes]\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Kontrola nadbytečných argumentů */
        if ( sub_argc > 4 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[4] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        unsigned long block_num;
        if ( parse_uint_arg ( sub_argv[1], "block", dpb.dsm, &block_num ) != EXIT_SUCCESS ) {
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        size_t blk_size = 0;
        if ( sub_argc >= 4 ) {
            unsigned long val;
            if ( parse_uint_arg ( sub_argv[3], "size", 0xFFFFFFFF, &val ) != EXIT_SUCCESS ) {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            blk_size = (size_t) val;
        }
        err = cmd_get_block ( &disc, &dpb, (uint16_t) block_num, sub_argv[2], blk_size );

    /* ---- put-block ---- */
    } else if ( strcmp ( subcmd, "put-block" ) == 0 ) {

        if ( sub_argc < 3 ) {
            fprintf ( stderr, "Error: put-block requires N <file> [bytes] [offset]\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Kontrola nadbytečných argumentů */
        if ( sub_argc > 5 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[5] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        unsigned long block_num;
        if ( parse_uint_arg ( sub_argv[1], "block", dpb.dsm, &block_num ) != EXIT_SUCCESS ) {
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        size_t blk_size = 0, blk_offset = 0;
        if ( sub_argc >= 4 ) {
            unsigned long val;
            if ( parse_uint_arg ( sub_argv[3], "size", 0xFFFFFFFF, &val ) != EXIT_SUCCESS ) {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            blk_size = (size_t) val;
        }
        if ( sub_argc >= 5 ) {
            unsigned long val;
            if ( parse_uint_arg ( sub_argv[4], "offset", 0xFFFFFFFF, &val ) != EXIT_SUCCESS ) {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            blk_offset = (size_t) val;
        }
        err = cmd_put_block ( &disc, &dpb, (uint16_t) block_num, sub_argv[2], blk_size, blk_offset );

    } else {
        fprintf ( stderr, "Error: Unknown command '%s'\n\n", subcmd );
        print_usage ( stderr );
        mzdsk_disc_close ( &disc );
        return EXIT_FAILURE;
    }

    /* Uložení změn do souboru při úspěšné zápisové operaci */
    if ( is_write_op && err == MZDSK_RES_OK ) {
        en_MZDSK_RES save_err = mzdsk_disc_save ( &disc );
        if ( save_err != MZDSK_RES_OK ) {
            fprintf ( stderr, "Error: Could not save DSK file: %s\n", mzdsk_get_error ( save_err ) );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
    }

    mzdsk_disc_close ( &disc );

    if ( err != MZDSK_RES_OK ) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
