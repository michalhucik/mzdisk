/**
 * @file   mzdsk_raw.c
 * @brief  Nízkoúrovňový nástroj pro surový přístup k sektorům/blokům
 *         a modifikaci geometrie DSK obrazu.
 *
 * Umožňuje export/import/hexdump sektorů a bloků s konfigurovatelnými
 * režimy adresování (Track/Sector a Block), řazením sektorů (ID/Phys),
 * inverzí dat, byte/file offsety. Dále umožňuje úpravu geometrie disku -
 * změnu formátu stopy, přidání stop nebo zmenšení obrazu.
 *
 * @par Použití:
 * @code
 * mzdsk-raw <dsk_file> <subcommand> [args...] [options]
 * @endcode
 *
 * @par Subpříkazy:
 * - get <file> [options]         - export sektorů/bloků do souboru
 * - put <file> [options]         - import ze souboru na disk
 * - dump [options]               - hexdump na stdout
 * - change-track T SECS SSIZE FILLER [ORDER|MAP]
 * - append-tracks N SECS SSIZE FILLER [ORDER|MAP]
 * - shrink N
 *
 * @par Globální volby:
 * - --inv / --noinv  vynucení inverze dat
 * - --version        verze nástroje
 * - --lib-versions   verze všech použitých knihoven
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
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>

/* Portable 64-bit varianta fseek/ftell (audit L-5).
 * Na MSYS2/MinGW64 je `long` na Windows 32-bit, proto standardní
 * `fseek(..., long, ...)` selže pro soubory > 2 GB. */
#ifdef _WIN32
#  define mzdsk_fseek64(f, off, whence) _fseeki64 ( (f), (__int64)(off), (whence) )
#  define mzdsk_ftell64(f)              _ftelli64 ( (f) )
typedef long long mzdsk_off_t;
#else
#  define _FILE_OFFSET_BITS 64
#  define mzdsk_fseek64(f, off, whence) fseeko ( (f), (off_t)(off), (whence) )
#  define mzdsk_ftell64(f)              ftello ( (f) )
typedef off_t mzdsk_off_t;
#endif

#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_hexdump/mzdsk_hexdump.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "tools/common/mzdisk_cli_version.h"


/** @brief Verze nástroje mzdsk-raw. */
#define MZDSK_RAW_VERSION "2.2.10"


/* =========================================================================
 * Typy pro adresování a volby get/put/dump
 * ========================================================================= */


/* Globální flag `--overwrite` (analogie fsmz/cpm/mrs). Výchozí 0 =
 * `get` odmítne přepsat existující výstupní soubor a skončí chybou.
 * --file-offset > 0 (embed mode) je výjimka - zápis do existujícího
 * souboru je žádoucí use case a flag se nekontroluje. */
static int g_allow_overwrite = 0;


/**
 * @brief Režim adresování sektorů.
 *
 * Určuje, jak uživatel specifikuje rozsah dat na disku.
 */
typedef enum en_RAW_ADDR_MODE {
    RAW_ADDR_TRACK_SECTOR = 0,  /**< Přímé adresování stopou a sektorem. */
    RAW_ADDR_BLOCK,             /**< Blokové adresování s origin/spb konfigurací. */
} en_RAW_ADDR_MODE;


/**
 * @brief Pořadí procházení sektorů při sekvenčním čtení.
 *
 * Určuje, jak se postupuje od jednoho sektoru k dalšímu.
 * Na standardních Sharp MZ discích (sekvenční ID 1..N) se obě
 * varianty chovají identicky. Rozdíl nastává u disků s nesekvenčními
 * ID sektorů (např. LEMMINGS: sektory 1-9 + 22 na jedné stopě).
 */
typedef enum en_RAW_SECTOR_ORDER {
    RAW_ORDER_ID = 0,   /**< Podle ID - hledá sektor s ID+1, pokud neexistuje, přejde na další stopu. */
    RAW_ORDER_PHYS,     /**< Podle fyzické pozice v sinfo[] poli DSK stopy. */
} en_RAW_SECTOR_ORDER;


/**
 * @brief Rozparsované volby pro subpříkazy get, put a dump.
 *
 * Sdružuje všechny parametry adresování, řazení sektorů,
 * inverze dat a byte/file offsetů.
 *
 * @par Invarianty:
 * - V režimu RAW_ADDR_TRACK_SECTOR: sector_count >= 1, start_sector >= 1.
 * - V režimu RAW_ADDR_BLOCK: block_count >= 1, sectors_per_block >= 1.
 * - byte_count == 0 znamená "vše" (žádné omezení).
 */
typedef struct st_DATA_OPTIONS {
    en_RAW_ADDR_MODE addr_mode;     /**< Režim adresování. */
    en_RAW_SECTOR_ORDER order;      /**< Pořadí procházení sektorů. */

    /* Track/Sector režim */
    uint16_t start_track;           /**< Počáteční stopa (výchozí: 0). */
    uint16_t start_sector;          /**< Počáteční sektor ID, 1-based (výchozí: 1). */
    int32_t sector_count;           /**< Počet sektorů (výchozí: 1). */

    /* Block režim */
    int32_t start_block;            /**< Počáteční blok. */
    int32_t block_count;            /**< Počet bloků (výchozí: 1). */
    uint16_t origin_track;          /**< Origin track (výchozí: 0). */
    uint16_t origin_sector;         /**< Origin sector, 1-based (výchozí: 1). */
    int32_t first_block;            /**< Číslo prvního bloku na origin (výchozí: 0). */
    uint16_t sectors_per_block;     /**< Sektory na blok (výchozí: 1). */

    /* Data */
    int32_t byte_offset;            /**< Offset v prvním sektoru/bloku (výchozí: 0). */
    int32_t byte_count;             /**< Celkový počet bajtů, 0 = vše (výchozí: 0). */
    int64_t file_offset;            /**< Offset v souboru (výchozí: 0). */

    /* Dump-specifické */
    en_MZDSK_HEXDUMP_CHARSET dump_charset; /**< Znaková sada v ASCII sloupci hexdumpu. */
} st_DATA_OPTIONS;


/* =========================================================================
 * Vpřed deklarované funkce
 * ========================================================================= */

static void print_usage ( FILE *out, const char *progname );
static void print_version ( void );
static void print_lib_versions ( void );

static en_DSK_SECTOR_SIZE parse_sector_size ( const char *str );
static int parse_sector_order ( const char *str, uint8_t *sector_map, uint8_t sectors, en_DSK_SECTOR_ORDER_TYPE *out_order );
static int validate_filler ( const char *str, uint8_t *out_filler );

static void init_data_options ( st_DATA_OPTIONS *opts );
static int parse_data_options ( int argc, char **argv, st_DATA_OPTIONS *opts, char **out_filepath );

static int find_sector_position ( const st_DSK_SHORT_TRACK_INFO *tinfo, uint8_t sector_id );
static int count_consecutive_ids ( const st_DSK_SHORT_TRACK_INFO *tinfo, uint8_t start_id );
static bool advance_sectors ( st_HANDLER *h, en_RAW_SECTOR_ORDER order,
                              uint16_t *track, uint16_t *sector_id,
                              int count, uint16_t max_track );
static bool get_track_params ( st_HANDLER *h, uint16_t track,
                               uint16_t *sectors, uint16_t *sector_size );

static en_MZDSK_RES cmd_get ( st_MZDSK_DISC *disc, int argc, char **argv, int force_inv );
static en_MZDSK_RES cmd_put ( st_MZDSK_DISC *disc, int argc, char **argv, int force_inv );
static en_MZDSK_RES cmd_dump ( st_MZDSK_DISC *disc, int argc, char **argv, int force_inv );
static en_MZDSK_RES cmd_change_track ( st_MZDSK_DISC *disc, int argc, char **argv );
static en_MZDSK_RES cmd_append_tracks ( st_MZDSK_DISC *disc, int argc, char **argv );
static en_MZDSK_RES cmd_shrink ( st_MZDSK_DISC *disc, int argc, char **argv );


/* =========================================================================
 * Hlavní vstupní bod
 * ========================================================================= */


/**
 * @brief Vstupní bod programu mzdsk-raw.
 *
 * Zpracuje příkazovou řádku, otevře DSK obraz a deleguje provedení
 * na příslušnou funkci subpříkazu.
 *
 * @param argc Počet argumentů příkazové řádky.
 * @param argv Pole argumentů příkazové řádky.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
int main ( int argc, char **argv ) {

    memory_driver_init();

    if ( argc < 2 ) {
        print_usage ( stderr, argv[0] );
        return EXIT_FAILURE;
    }

    /* Definice dlouhých voleb pro getopt_long */
    static struct option long_options[] = {
        { "inv",          no_argument, NULL, 'i' },
        { "noinv",        no_argument, NULL, 'I' },
        { "overwrite",    no_argument, NULL, 'O' },
        { "version",      no_argument, NULL, 'V' },
        { "lib-versions", no_argument, NULL, 'L' },
        { "help",         no_argument, NULL, 'h' },
        { NULL,           0,           NULL,  0  }
    };

    /* Parsování globálních voleb přes getopt_long */
    int force_inv = 0; /* 0 = auto, 1 = vynucená inverze, -1 = vynucená neinverze */

    optind = 1;
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "+h", long_options, NULL ) ) != -1 ) {
        switch ( opt ) {
            case 'i': /* --inv */
                force_inv = 1;
                break;
            case 'I': /* --noinv */
                force_inv = -1;
                break;
            case 'O': /* --overwrite */
                g_allow_overwrite = 1;
                break;
            case 'V': /* --version */
                print_version();
                return EXIT_SUCCESS;
            case 'L': /* --lib-versions */
                print_lib_versions();
                return EXIT_SUCCESS;
            case 'h': /* --help */
                print_usage ( stdout, argv[0] );
                return EXIT_SUCCESS;
            default: /* neznámá volba - getopt_long již vypsal chybu */
                print_usage ( stderr, argv[0] );
                return EXIT_FAILURE;
        }
    }

    /* DSK soubor a subpříkaz jsou poziční argumenty po volbách */
    if ( optind >= argc ) {
        print_usage ( stderr, argv[0] );
        return EXIT_FAILURE;
    }

    char *dsk_filename = argv[optind++];

    /* Druhé kolo: globální volby mezi DSK souborem a subpříkazem */
    while ( ( opt = getopt_long ( argc, argv, "+h", long_options, NULL ) ) != -1 ) {
        switch ( opt ) {
            case 'i': force_inv = 1; break;
            case 'I': force_inv = -1; break;
            case 'O': g_allow_overwrite = 1; break;
            case 'V': print_version(); return EXIT_SUCCESS;
            case 'L': print_lib_versions(); return EXIT_SUCCESS;
            case 'h': print_usage ( stdout, argv[0] ); return EXIT_SUCCESS;
            default: print_usage ( stderr, argv[0] ); return EXIT_FAILURE;
        }
    }

    if ( optind >= argc ) {
        fprintf ( stderr, "Error: subcommand required\n" );
        print_usage ( stderr, argv[0] );
        return EXIT_FAILURE;
    }

    char *subcmd = argv[optind++];

    /* Zbylé argumenty pro subpříkaz - extrahujeme globální volby (--inv, --noinv),
     * protože getopt v POSIX módu ("+") se zastavuje na prvním pozičním argumentu
     * a tyto volby by se za pozičními argumenty subpříkazu neparsovaly. */
    int sub_argc = 0;
    char **sub_argv = argv + optind; /* přepíšeme na místě - je to bezpečné */
    for ( int i = optind; i < argc; i++ ) {
        if ( strcmp ( argv[i], "--inv" ) == 0 ) {
            force_inv = 1;
        } else if ( strcmp ( argv[i], "--noinv" ) == 0 ) {
            force_inv = -1;
        } else if ( strcmp ( argv[i], "--overwrite" ) == 0 ) {
            g_allow_overwrite = 1;
        } else {
            sub_argv[sub_argc++] = argv[i];
        }
    }

    /* Určení režimu otevření podle subpříkazu. Write subkomanda vyžadují
     * RW režim a zároveň atomický zápis přes paměťový driver. */
    en_FILE_DRIVER_OPEN_MODE open_mode;
    int is_write_op;

    if ( strcmp ( subcmd, "get" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RO;
        is_write_op = 0;
    } else if ( strcmp ( subcmd, "put" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RW;
        is_write_op = 1;
    } else if ( strcmp ( subcmd, "dump" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RO;
        is_write_op = 0;
    } else if ( strcmp ( subcmd, "change-track" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RW;
        is_write_op = 1;
    } else if ( strcmp ( subcmd, "append-tracks" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RW;
        is_write_op = 1;
    } else if ( strcmp ( subcmd, "shrink" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RW;
        is_write_op = 1;
    } else {
        fprintf ( stderr, "Error: Unknown subcommand '%s'\n\n", subcmd );
        print_usage ( stderr, argv[0] );
        return EXIT_FAILURE;
    }

    /* Otevření DSK obrazu.
     * Zápisové subpříkazy (put, change-track, append-tracks, shrink) používají
     * paměťový driver - veškeré změny se provádějí v RAM a zapisují zpět jedním
     * voláním mzdsk_disc_save() až po úspěšném dokončení. Pokud operace selže
     * uprostřed, původní soubor zůstává netknutý.
     * Čtecí subpříkazy (get, dump) používají file driver - nízké paměťové nároky. */
    st_MZDSK_DISC disc;
    en_MZDSK_RES res;

    if ( is_write_op ) {
        res = mzdsk_disc_open_memory ( &disc, dsk_filename, open_mode );
    } else {
        res = mzdsk_disc_open ( &disc, dsk_filename, open_mode );
    }

    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: Cannot open DSK image '%s': %s\n", dsk_filename, mzdsk_get_error ( res ) );
        return EXIT_FAILURE;
    }

    /* Validace DSK magic (file_info = "EXTENDED CPC DSK File\r\n...") - zabrání
     * destruktivním operacím nad libovolným souborem (např. `mzdsk-raw foto.jpg
     * shrink 5`). Stejná ochrana jako v mzdsk-dsk po BUG 6. Audit H-19. */
    if ( EXIT_SUCCESS != dsk_tools_check_dsk_fileinfo ( disc.handler ) ) {
        fprintf ( stderr,
            "Error: '%s' is not an Extended CPC DSK file (magic mismatch).\n"
            "       Use 'mzdsk-dsk check' to see details.\n",
            dsk_filename );
        mzdsk_disc_close ( &disc );
        return EXIT_FAILURE;
    }

    /* Dispatch subpříkazu */
    en_MZDSK_RES cmd_res = MZDSK_RES_UNKNOWN_ERROR;

    if ( strcmp ( subcmd, "get" ) == 0 ) {
        cmd_res = cmd_get ( &disc, sub_argc, sub_argv, force_inv );
    } else if ( strcmp ( subcmd, "put" ) == 0 ) {
        cmd_res = cmd_put ( &disc, sub_argc, sub_argv, force_inv );
    } else if ( strcmp ( subcmd, "dump" ) == 0 ) {
        cmd_res = cmd_dump ( &disc, sub_argc, sub_argv, force_inv );
    } else if ( strcmp ( subcmd, "change-track" ) == 0 ) {
        cmd_res = cmd_change_track ( &disc, sub_argc, sub_argv );
    } else if ( strcmp ( subcmd, "append-tracks" ) == 0 ) {
        cmd_res = cmd_append_tracks ( &disc, sub_argc, sub_argv );
    } else if ( strcmp ( subcmd, "shrink" ) == 0 ) {
        cmd_res = cmd_shrink ( &disc, sub_argc, sub_argv );
    }

    /* Uložení změn do souboru při úspěšné zápisové operaci */
    if ( is_write_op && cmd_res == MZDSK_RES_OK ) {
        en_MZDSK_RES save_err = mzdsk_disc_save ( &disc );
        if ( save_err != MZDSK_RES_OK ) {
            fprintf ( stderr, "Error: Could not save DSK file: %s\n", mzdsk_get_error ( save_err ) );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
    }

    mzdsk_disc_close ( &disc );

    /* Subpříkazové funkce (cmd_get/put/dump/...) i jejich helpery
       (parse_data_options, validate_filler, parse_sector_order, ...)
       vypisují konkrétní chybovou hlášku přímo na stderr před návratem
       s chybovým kódem. Generický "Command failed: Unknown error"
       výpis zde by byl redundantní a matoucí (BUG B5). */
    if ( cmd_res != MZDSK_RES_OK ) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/* =========================================================================
 * Informační funkce
 * ========================================================================= */


/**
 * @brief Vytiskne nápovědu k použití programu.
 *
 * @param[in] out      Výstupní stream (stdout pro --help, stderr pro chybové cesty).
 * @param[in] progname Název spustitelného souboru (argv[0]).
 */
static void print_usage ( FILE *out, const char *progname ) {

    fprintf ( out, "Usage: %s <dsk_file> <subcommand> [args...] [options]\n\n", progname );
    fprintf ( out, "Subcommands:\n" );
    fprintf ( out, "  get <file> [options]                    Export sectors/blocks to file\n" );
    fprintf ( out, "  put <file> [options]                    Import file data into sectors/blocks\n" );
    fprintf ( out, "  dump [options]                          Hexdump sectors/blocks to stdout\n" );
    fprintf ( out, "  change-track T SECS SSIZE FILLER [ORDER|MAP]\n" );
    fprintf ( out, "                                         Change track format\n" );
    fprintf ( out, "  append-tracks N SECS SSIZE FILLER [ORDER|MAP]\n" );
    fprintf ( out, "                                         Add tracks\n" );
    fprintf ( out, "  shrink N                               Shrink to N total tracks\n\n" );
    fprintf ( out, "Addressing options (get/put/dump):\n" );
    fprintf ( out, "  --track T          Start track (default: 0)\n" );
    fprintf ( out, "  --sector S         Start sector ID, 1-based (default: 1)\n" );
    fprintf ( out, "  --sectors N        Number of sectors (default: 1)\n" );
    fprintf ( out, "  --block B          Start block (activates block mode)\n" );
    fprintf ( out, "  --blocks N         Number of blocks (default: 1)\n" );
    fprintf ( out, "  --origin-track T   Block origin track (default: 0)\n" );
    fprintf ( out, "  --origin-sector S  Block origin sector (default: 1)\n" );
    fprintf ( out, "  --first-block N    First block number at origin (default: 0)\n" );
    fprintf ( out, "  --spb N            Sectors per block (default: 1)\n" );
    fprintf ( out, "  --order id|phys    Sector traversal order (default: id)\n" );
    fprintf ( out, "  --byte-offset N    Offset in first sector/block (default: 0)\n" );
    fprintf ( out, "  --byte-count N     Total byte count, 0 = all (default: 0)\n" );
    fprintf ( out, "  --file-offset N    Offset in file (default: 0)\n" );
    fprintf ( out, "  --dump-charset MODE  ASCII column charset: raw (default), eu, jp, utf8-eu, utf8-jp\n" );
    fprintf ( out, "  --cnv                Alias for --dump-charset eu\n\n" );
    fprintf ( out, "Global options:\n" );
    fprintf ( out, "  --inv              Force data inversion on (XOR 0xFF)\n" );
    fprintf ( out, "  --noinv            Force data inversion off\n" );
    fprintf ( out, "  --overwrite        Allow 'get' to overwrite an existing output file\n" );
    fprintf ( out, "                     (default: refuse to overwrite; ignored when\n" );
    fprintf ( out, "                     --file-offset > 0 is used for embed mode)\n" );
    fprintf ( out, "  --version          Show version\n" );
    fprintf ( out, "  --lib-versions     Show library versions\n\n" );
    fprintf ( out, "Sector size (change-track/append-tracks): 128, 256, 512, 1024\n" );
    fprintf ( out, "Sector order (change-track/append-tracks): normal, lec, lechd, or custom map\n" );
}


/**
 * @brief Vytiskne verzi nástroje.
 */
static void print_version ( void ) {
    printf ( "mzdsk-raw %s (%s %s)\n",
             MZDSK_RAW_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
}


/**
 * @brief Vytiskne verze všech použitých knihoven.
 */
static void print_lib_versions ( void ) {

    printf ( "mzdsk-raw %s (%s %s)\n\n",
             MZDSK_RAW_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
    printf ( "Library versions:\n" );
    printf ( "  dsk:            %s\n", dsk_version () );
    printf ( "  mzdsk_global:   %s\n", mzdsk_global_version () );
    printf ( "  mzdsk_ipldisk:  %s\n", mzdsk_ipldisk_version () );
    printf ( "  mzdsk_hexdump:  %s\n", mzdsk_hexdump_version () );
    printf ( "  mzdsk_tools:    %s\n", mzdsk_tools_version () );
    printf ( "  sharpmz_ascii:  %s\n", sharpmz_ascii_version () );
    printf ( "  generic_driver: %s\n", generic_driver_version () );
}


/* =========================================================================
 * Parsování argumentů
 * ========================================================================= */


/**
 * @brief Parsuje velikost sektoru ze řetězce.
 *
 * Rozpoznává hodnoty "128", "256", "512" a "1024".
 *
 * @param str Vstupní řetězec s číselnou hodnotou velikosti.
 * @return Kódovaná velikost sektoru (en_DSK_SECTOR_SIZE), nebo
 *         DSK_SECTOR_SIZE_INVALID pro nerozpoznaný řetězec.
 */
static en_DSK_SECTOR_SIZE parse_sector_size ( const char *str ) {

    if ( strcmp ( str, "128" ) == 0 ) return DSK_SECTOR_SIZE_128;
    if ( strcmp ( str, "256" ) == 0 ) return DSK_SECTOR_SIZE_256;
    if ( strcmp ( str, "512" ) == 0 ) return DSK_SECTOR_SIZE_512;
    if ( strcmp ( str, "1024" ) == 0 ) return DSK_SECTOR_SIZE_1024;

    return DSK_SECTOR_SIZE_INVALID;
}


/**
 * @brief Parsuje typ řazení sektorů nebo vlastní mapu ze řetězce.
 *
 * Rozpoznává klíčová slova "normal", "lec" a "lechd".
 * Pokud řetězec neodpovídá žádnému klíčovému slovu, pokusí se
 * interpretovat ho jako čárkami oddělený seznam ID sektorů (vlastní mapa).
 *
 * Validace (RAW-5, RAW-6):
 * - Neplatný řetězec (ani klíčové slovo, ani validní čárkami oddělená mapa)
 *   je odmítnut s chybovou hláškou.
 * - Počet položek musí přesně odpovídat počtu sektorů.
 * - Každé ID musí být v rozsahu 0-255 a platné číslo.
 * - ID sektorů musí být unikátní (žádné duplicity).
 *
 * @param str Vstupní řetězec s typem řazení nebo seznamem sektorů.
 * @param[out] sector_map Výstupní pole pro vlastní mapu sektorů
 *                        (min DSK_MAX_SECTORS prvků). Plní se pouze
 *                        při DSK_SEC_ORDER_CUSTOM.
 * @param sectors Počet sektorů na stopě (pro validaci vlastní mapy).
 * @param[out] out_order Výstup: typ řazení sektorů (en_DSK_SECTOR_ORDER_TYPE).
 * @return 0 při úspěchu, -1 při chybě (s vypsanou hláškou na stderr).
 *
 * @pre str nesmí být NULL.
 * @pre sector_map musí mít alespoň DSK_MAX_SECTORS prvků.
 * @pre out_order nesmí být NULL.
 * @post Při úspěchu je *out_order nastaven a sector_map vyplněn (pro CUSTOM).
 */
static int parse_sector_order ( const char *str, uint8_t *sector_map, uint8_t sectors, en_DSK_SECTOR_ORDER_TYPE *out_order ) {

    if ( strcmp ( str, "normal" ) == 0 ) {
        *out_order = DSK_SEC_ORDER_NORMAL;
        return 0;
    }
    if ( strcmp ( str, "lec" ) == 0 ) {
        *out_order = DSK_SEC_ORDER_INTERLACED_LEC;
        return 0;
    }
    if ( strcmp ( str, "lechd" ) == 0 ) {
        *out_order = DSK_SEC_ORDER_INTERLACED_LEC_HD;
        return 0;
    }

    /* Pokud řetězec neobsahuje čárku a není klíčové slovo, je neplatný */
    if ( strchr ( str, ',' ) == NULL ) {
        fprintf ( stderr, "Error: Invalid sector order '%s' (valid: normal, lec, lechd, or comma-separated IDs)\n", str );
        return -1;
    }

    /* Parsování vlastní mapy sektorů - čárkami oddělená čísla */
    char buf[256];
    strncpy ( buf, str, sizeof ( buf ) - 1 );
    buf[sizeof ( buf ) - 1] = '\0';

    uint8_t idx = 0;
    /* strtok_r místo strtok: nerentrantní varianta uchovává stav v lokální
     * saveptr a je bezpečná pro případné paralelní volání (audit L-11). */
    char *saveptr = NULL;
    char *token = strtok_r ( buf, ",", &saveptr );

    while ( token != NULL && idx < DSK_MAX_SECTORS ) {
        char *endptr;
        long val = strtol ( token, &endptr, 10 );

        if ( *endptr != '\0' || val < 0 || val > 255 ) {
            fprintf ( stderr, "Error: Invalid sector ID '%s' in sector map (valid range: 0-255)\n", token );
            return -1;
        }

        sector_map[idx++] = (uint8_t) val;
        token = strtok_r ( NULL, ",", &saveptr );
    }

    /* Ověření, že počet položek v mapě odpovídá počtu sektorů */
    if ( idx != sectors ) {
        fprintf ( stderr, "Error: Sector map has %d entries but track has %d sectors\n", idx, sectors );
        return -1;
    }

    /* Ověření unikátnosti ID sektorů */
    for ( uint8_t i = 0; i < idx; i++ ) {
        for ( uint8_t j = i + 1; j < idx; j++ ) {
            if ( sector_map[i] == sector_map[j] ) {
                fprintf ( stderr, "Error: Duplicate sector ID %d in sector map\n", sector_map[i] );
                return -1;
            }
        }
    }

    *out_order = DSK_SEC_ORDER_CUSTOM;
    return 0;
}


/**
 * @brief Validuje a parsuje filler bajt ze řetězce.
 *
 * Parsuje číselnou hodnotu (dek/hex/okt) a ověří, že je v rozsahu 0-255.
 * Oproti přímému přetypování na uint8_t zachytí záporné hodnoty
 * i hodnoty mimo rozsah jednoho bajtu (RAW-7).
 *
 * @param str Vstupní řetězec s hodnotou filler bajtu.
 * @param[out] out_filler Výstup: naparsovaná hodnota filler bajtu.
 * @return 0 při úspěchu, -1 při chybě (s vypsanou hláškou na stderr).
 *
 * @pre str nesmí být NULL.
 * @pre out_filler nesmí být NULL.
 */
static int validate_filler ( const char *str, uint8_t *out_filler ) {

    char *endptr;
    long val = strtol ( str, &endptr, 0 );

    if ( *endptr != '\0' || val < 0 || val > 255 ) {
        fprintf ( stderr, "Error: Invalid filler value '%s' (valid range: 0-255, decimal or 0xHH)\n", str );
        return -1;
    }

    *out_filler = (uint8_t) val;
    return 0;
}


/* =========================================================================
 * Pomocné funkce pro inverzi dat
 * ========================================================================= */


/**
 * @brief Zjistí text popisující stav inverze pro daný sektor.
 *
 * Na základě sector_info callbacku a přepínače force_inv rozhodne,
 * zda data invertovat a vrátí popisný text pro výpis.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param track Absolutní číslo stopy.
 * @param sector ID sektoru.
 * @param force_inv Přepínač inverze (0 = auto, 1 = vynucená, -1 = vynucená ne).
 * @param[out] do_inv Výstup: nenulová hodnota pokud se má provést inverze.
 * @return Ukazatel na statický řetězec s popisem stavu inverze.
 */
static const char* get_inv_info ( st_MZDSK_DISC *disc, uint16_t track, uint16_t sector, int force_inv, int *do_inv ) {

    uint8_t sector_info = disc->sector_info_cb ( track, sector, disc->sector_info_cb_data );
    int native_inv = ( ( sector_info & 0x80 ) == MZDSK_MEDIUM_INVERTED ) ? 1 : 0;

    if ( force_inv == -1 ) {
        if ( native_inv ) {
            *do_inv = 1;
            return "forced NOT inverted";
        } else {
            *do_inv = 0;
            return "normal";
        }
    } else if ( force_inv == 1 ) {
        if ( !native_inv ) {
            *do_inv = 1;
            return "forced INVERTED";
        } else {
            *do_inv = 0;
            return "inverted";
        }
    } else {
        *do_inv = 0;
        if ( native_inv ) {
            return "inverted";
        } else {
            return "normal";
        }
    }
}


/* =========================================================================
 * Inicializace a parsování voleb pro get/put/dump
 * ========================================================================= */


/**
 * @brief Inicializuje datové volby na výchozí hodnoty.
 *
 * @param opts Ukazatel na strukturu voleb.
 *
 * @pre opts != NULL.
 * @post opts je inicializován s výchozími hodnotami:
 *       T/S režim, start_track=0, start_sector=1, sector_count=1.
 */
static void init_data_options ( st_DATA_OPTIONS *opts ) {

    memset ( opts, 0, sizeof ( *opts ) );
    opts->start_sector = 1;
    opts->sector_count = 1;
    opts->block_count = 1;
    opts->origin_sector = 1;
    opts->sectors_per_block = 1;
    opts->dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
}


/**
 * @brief Parsuje argumenty příkazové řádky pro get/put/dump.
 *
 * Manuální parsování long options (--track, --sector, --block, atd.)
 * a pozičního argumentu (cesta k souboru).
 *
 * Vzájemně vylučující se volby:
 * - --block a --track/--sector/--sectors se nesmí kombinovat.
 *
 * @param argc Počet argumentů.
 * @param argv Pole argumentů (bez subpříkazu, může obsahovat long options).
 * @param[out] opts Výstupní rozparsované volby.
 * @param[out] out_filepath Výstup: cesta k souboru (poziční argument), nebo NULL.
 * @return 0 při úspěchu, -1 při chybě (chyba vypsána na stderr).
 *
 * @pre opts != NULL, out_filepath != NULL.
 * @post Při úspěchu: opts je naplněn, *out_filepath ukazuje na filepath (nebo NULL pro dump).
 */
static int parse_data_options ( int argc, char **argv, st_DATA_OPTIONS *opts, char **out_filepath ) {

    *out_filepath = NULL;
    bool block_seen = false;
    bool track_seen = false;
    bool sector_seen = false;
    bool sectors_seen = false;

    for ( int i = 0; i < argc; i++ ) {

        if ( strcmp ( argv[i], "--track" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --track requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 0 || val > 255 ) {
                fprintf ( stderr, "Error: Invalid track number '%s' (valid range: 0-255)\n", argv[i] );
                return -1;
            }
            opts->start_track = (uint16_t) val;
            track_seen = true;

        } else if ( strcmp ( argv[i], "--sector" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --sector requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 0 || val > 255 ) {
                fprintf ( stderr, "Error: Invalid sector number '%s' (valid range: 0-255)\n", argv[i] );
                return -1;
            }
            opts->start_sector = (uint16_t) val;
            sector_seen = true;

        } else if ( strcmp ( argv[i], "--sectors" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --sectors requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 1 ) {
                fprintf ( stderr, "Error: Invalid sector count '%s' (must be >= 1)\n", argv[i] );
                return -1;
            }
            opts->sector_count = (int32_t) val;
            sectors_seen = true;

        } else if ( strcmp ( argv[i], "--block" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --block requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' ) {
                fprintf ( stderr, "Error: Invalid block number '%s'\n", argv[i] );
                return -1;
            }
            opts->start_block = (int32_t) val;
            opts->addr_mode = RAW_ADDR_BLOCK;
            block_seen = true;

        } else if ( strcmp ( argv[i], "--blocks" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --blocks requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 1 ) {
                fprintf ( stderr, "Error: Invalid block count '%s' (must be >= 1)\n", argv[i] );
                return -1;
            }
            opts->block_count = (int32_t) val;

        } else if ( strcmp ( argv[i], "--origin-track" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --origin-track requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 0 || val > 255 ) {
                fprintf ( stderr, "Error: Invalid origin track '%s' (valid range: 0-255)\n", argv[i] );
                return -1;
            }
            opts->origin_track = (uint16_t) val;

        } else if ( strcmp ( argv[i], "--origin-sector" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --origin-sector requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 0 || val > 255 ) {
                fprintf ( stderr, "Error: Invalid origin sector '%s' (valid range: 0-255)\n", argv[i] );
                return -1;
            }
            opts->origin_sector = (uint16_t) val;

        } else if ( strcmp ( argv[i], "--first-block" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --first-block requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' ) {
                fprintf ( stderr, "Error: Invalid first-block number '%s'\n", argv[i] );
                return -1;
            }
            opts->first_block = (int32_t) val;

        } else if ( strcmp ( argv[i], "--spb" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --spb requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 1 || val > 255 ) {
                fprintf ( stderr, "Error: Invalid sectors-per-block '%s' (valid range: 1-255)\n", argv[i] );
                return -1;
            }
            opts->sectors_per_block = (uint16_t) val;

        } else if ( strcmp ( argv[i], "--order" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --order requires a value\n" ); return -1; }
            if ( strcmp ( argv[i], "id" ) == 0 ) {
                opts->order = RAW_ORDER_ID;
            } else if ( strcmp ( argv[i], "phys" ) == 0 ) {
                opts->order = RAW_ORDER_PHYS;
            } else {
                fprintf ( stderr, "Error: Invalid order '%s' (valid: id, phys)\n", argv[i] );
                return -1;
            }

        } else if ( strcmp ( argv[i], "--byte-offset" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --byte-offset requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 0 ) {
                fprintf ( stderr, "Error: Invalid byte-offset '%s' (must be >= 0)\n", argv[i] );
                return -1;
            }
            opts->byte_offset = (int32_t) val;

        } else if ( strcmp ( argv[i], "--byte-count" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --byte-count requires a value\n" ); return -1; }
            char *endptr;
            long val = strtol ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 0 ) {
                fprintf ( stderr, "Error: Invalid byte-count '%s' (must be >= 0)\n", argv[i] );
                return -1;
            }
            opts->byte_count = (int32_t) val;

        } else if ( strcmp ( argv[i], "--file-offset" ) == 0 ) {
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --file-offset requires a value\n" ); return -1; }
            char *endptr;
            long long val = strtoll ( argv[i], &endptr, 10 );
            if ( *endptr != '\0' || val < 0 ) {
                fprintf ( stderr, "Error: Invalid file-offset '%s' (must be >= 0)\n", argv[i] );
                return -1;
            }
            opts->file_offset = (int64_t) val;

        } else if ( strcmp ( argv[i], "--dump-charset" ) == 0 ) {
            /* --dump-charset MODE: volba znakové sady ASCII sloupce hexdumpu */
            if ( ++i >= argc ) { fprintf ( stderr, "Error: --dump-charset requires a value\n" ); return -1; }
            if ( strcmp ( argv[i], "raw" ) == 0 )          opts->dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
            else if ( strcmp ( argv[i], "eu" ) == 0 )      opts->dump_charset = MZDSK_HEXDUMP_CHARSET_EU;
            else if ( strcmp ( argv[i], "jp" ) == 0 )      opts->dump_charset = MZDSK_HEXDUMP_CHARSET_JP;
            else if ( strcmp ( argv[i], "utf8-eu" ) == 0 ) opts->dump_charset = MZDSK_HEXDUMP_CHARSET_UTF8_EU;
            else if ( strcmp ( argv[i], "utf8-jp" ) == 0 ) opts->dump_charset = MZDSK_HEXDUMP_CHARSET_UTF8_JP;
            else {
                fprintf ( stderr, "Error: Unknown dump-charset '%s' (use raw, eu, jp, utf8-eu or utf8-jp)\n", argv[i] );
                return -1;
            }

        } else if ( strcmp ( argv[i], "--cnv" ) == 0 ) {
            /* --cnv: alias pro --dump-charset eu (zpětná kompatibilita) */
            opts->dump_charset = MZDSK_HEXDUMP_CHARSET_EU;

        } else if ( argv[i][0] == '-' && argv[i][1] == '-' ) {
            fprintf ( stderr, "Error: Unknown option '%s'\n", argv[i] );
            return -1;

        } else {
            /* poziční argument = cesta k souboru */
            if ( *out_filepath ) {
                fprintf ( stderr, "Error: Unexpected argument '%s'\n", argv[i] );
                return -1;
            }
            *out_filepath = argv[i];
        }
    }

    /* Validace: --block a --track/--sector/--sectors se vzájemně vylučují */
    if ( block_seen && ( track_seen || sector_seen || sectors_seen ) ) {
        fprintf ( stderr, "Error: --block and --track/--sector/--sectors are mutually exclusive\n" );
        return -1;
    }

    return 0;
}


/* =========================================================================
 * Navigace po sektorech (portováno z panel_hexdump.c)
 * ========================================================================= */


/**
 * @brief Najde fyzickou pozici sektoru s daným ID na stopě.
 *
 * Prohledá pole sinfo[] a vrátí index (0-based) sektoru
 * s hledaným ID.
 *
 * @param tinfo Informace o stopě.
 * @param sector_id Hledané ID sektoru.
 * @return Fyzická pozice (0-based), nebo -1 pokud ID neexistuje.
 *
 * @pre tinfo != NULL.
 */
static int find_sector_position ( const st_DSK_SHORT_TRACK_INFO *tinfo, uint8_t sector_id ) {

    for ( int i = 0; i < tinfo->sectors; i++ ) {
        if ( tinfo->sinfo[i] == sector_id ) return i;
    }
    return -1;
}


/**
 * @brief Spočítá kolik po sobě jdoucích ID sektorů existuje na stopě.
 *
 * Od sektoru s ID start_id+1 hledá souvislou řadu existujících ID.
 * Např. pro stopu s ID [1,2,3,4,5,6,7,8,9,22] a start_id=1
 * vrátí 8 (ID 2..9 existují, ID 10 ne).
 *
 * @param tinfo Informace o stopě.
 * @param start_id ID sektoru, od kterého se počítá (nezapočítává se).
 * @return Počet po sobě jdoucích následujících ID.
 *
 * @pre tinfo != NULL.
 */
static int count_consecutive_ids ( const st_DSK_SHORT_TRACK_INFO *tinfo, uint8_t start_id ) {

    int count = 0;
    for ( uint8_t next = start_id + 1; next > start_id; next++ ) { /* overflow ochrana */
        if ( find_sector_position ( tinfo, next ) < 0 ) break;
        count++;
    }
    return count;
}


/**
 * @brief Posune pozici (track, sector_id) o zadaný počet sektorů vpřed.
 *
 * Postup závisí na zvoleném pořadí sektorů:
 *
 * - ID režim: hledá sektor s ID+1 na aktuální stopě. Pokud neexistuje,
 *   přejde na další stopu a začne od ID 1. Sektory s nesekvenčním ID
 *   (např. LEMMINGS sektor 22) jsou přeskočeny, pokud na ně nenavazuje
 *   počáteční pozice.
 *
 * - Phys režim: postupuje podle fyzické pozice v sinfo[] poli stopy.
 *   Na konci stopy přejde na další stopu a začne od fyzické pozice 0
 *   (tj. sinfo[0]). Zachycuje všechny sektory včetně nesekvenčních.
 *
 * @param h Handler pro čtení track info.
 * @param order Pořadí procházení sektorů.
 * @param[in,out] track Aktuální stopa, aktualizuje se.
 * @param[in,out] sector_id Aktuální sector ID, aktualizuje se.
 * @param count Počet sektorů k přeskočení.
 * @param max_track Maximální číslo stopy.
 * @return true pokud je výsledná pozice platná, false pokud přetekla konec disku.
 *
 * @pre h != NULL, track != NULL, sector_id != NULL, count >= 0.
 * @post Při úspěchu: *track a *sector_id ukazují na platný sektor.
 */
static bool advance_sectors ( st_HANDLER *h, en_RAW_SECTOR_ORDER order,
                              uint16_t *track, uint16_t *sector_id,
                              int count, uint16_t max_track ) {

    uint16_t t = *track;
    uint16_t s = *sector_id;

    while ( count > 0 ) {
        st_DSK_SHORT_TRACK_INFO tinfo;
        if ( dsk_read_short_track_info ( h, NULL, (uint8_t) t, &tinfo ) != EXIT_SUCCESS ) return false;
        if ( tinfo.sectors == 0 ) return false;

        if ( order == RAW_ORDER_PHYS ) {
            /* fyzické pořadí: postup podle pozice v sinfo[] */
            int pos = find_sector_position ( &tinfo, (uint8_t) s );
            if ( pos < 0 ) return false;

            int remaining = tinfo.sectors - pos - 1;

            if ( count <= remaining ) {
                s = tinfo.sinfo[pos + count];
                count = 0;
            } else {
                count -= ( remaining + 1 );
                t++;
                if ( t > max_track ) return false;

                /* první fyzický sektor na další stopě */
                st_DSK_SHORT_TRACK_INFO next;
                if ( dsk_read_short_track_info ( h, NULL, (uint8_t) t, &next ) != EXIT_SUCCESS ) return false;
                if ( next.sectors == 0 ) return false;
                s = next.sinfo[0];
            }

        } else {
            /* ID pořadí: postup podle sekvenčních ID */
            int remaining = count_consecutive_ids ( &tinfo, (uint8_t) s );

            if ( count <= remaining ) {
                s = (uint16_t) ( s + count );
                count = 0;
            } else {
                count -= ( remaining + 1 );
                t++;
                if ( t > max_track ) return false;
                s = 1;
            }
        }
    }

    *track = t;
    *sector_id = s;
    return true;
}


/**
 * @brief Zjistí počet sektorů a velikost sektoru na dané stopě.
 *
 * Čte informace přímo z DSK hlavičky stopy přes handler.
 *
 * @param h Handler pro čtení.
 * @param track Číslo stopy.
 * @param[out] sectors Počet sektorů.
 * @param[out] sector_size Velikost sektoru v bajtech.
 * @return true při úspěchu, false při chybě.
 *
 * @pre h != NULL, sectors != NULL, sector_size != NULL.
 * @post *sectors a *sector_size obsahují parametry stopy,
 *       nebo 0/256 pokud stopa neexistuje.
 */
static bool get_track_params ( st_HANDLER *h, uint16_t track,
                               uint16_t *sectors, uint16_t *sector_size ) {

    st_DSK_SHORT_TRACK_INFO tinfo;
    if ( dsk_read_short_track_info ( h, NULL, (uint8_t) track, &tinfo ) != EXIT_SUCCESS ) {
        *sectors = 0;
        *sector_size = 256;
        return false;
    }
    *sectors = tinfo.sectors;
    *sector_size = dsk_decode_sector_size ( tinfo.ssize );
    return true;
}


/**
 * @brief Resolvuje startovní track/sector z rozparsovaných voleb.
 *
 * V T/S režimu vrátí přímo start_track/start_sector.
 * V Block režimu přepočítá start_block na track/sector pomocí
 * blokové konfigurace a advance_sectors().
 *
 * @param opts Rozparsované volby.
 * @param h Handler pro čtení track info.
 * @param max_track Maximální číslo stopy.
 * @param[out] out_track Výstupní stopa.
 * @param[out] out_sector Výstupní sektor ID (1-based).
 * @param[out] out_total_sectors Celkový počet sektorů k přenesení.
 * @return true při úspěchu, false pokud pozice je mimo rozsah.
 *
 * @pre opts != NULL, h != NULL, výstupní ukazatele != NULL.
 */
static bool resolve_start_position ( const st_DATA_OPTIONS *opts, st_HANDLER *h,
                                      uint16_t max_track,
                                      uint16_t *out_track, uint16_t *out_sector,
                                      int32_t *out_total_sectors ) {

    if ( opts->addr_mode == RAW_ADDR_BLOCK ) {
        /* blokový režim: přepočítat start_block na T/S */
        int32_t sector_offset = ( opts->start_block - opts->first_block )
                                 * (int32_t) opts->sectors_per_block;
        if ( sector_offset < 0 ) return false;

        /* origin_track musí být v rozsahu - jinak advance_sectors
           a následný dsk_read_sector skončí s matoucí hláškou
           "Track N has no sectors" (BUG-RAW-001). */
        if ( opts->origin_track > max_track ) {
            return false;
        }

        uint16_t t = opts->origin_track;
        uint16_t s = opts->origin_sector;

        if ( sector_offset > 0 ) {
            if ( !advance_sectors ( h, opts->order, &t, &s,
                                     (int) sector_offset, max_track ) ) {
                return false;
            }
        }

        *out_track = t;
        *out_sector = s;
        *out_total_sectors = opts->block_count * (int32_t) opts->sectors_per_block;
    } else {
        /* T/S režim: ověř track proti max_track. Dříve se to nekontrolovalo
           a out-of-range track (např. 200 na basic.dsk s max_track=159)
           skončil na matoucí hlášce "Track N has no sectors" z vnitřní
           track_params cesty (BUG-RAW-001). */
        if ( opts->start_track > max_track ) {
            return false;
        }

        *out_track = opts->start_track;
        *out_sector = opts->start_sector;
        *out_total_sectors = opts->sector_count;
    }

    return true;
}


/* =========================================================================
 * Subpříkaz: get - export sektorů/bloků do souboru
 * ========================================================================= */


/**
 * @brief Exportuje sektory/bloky z disku do souboru.
 *
 * Čte data přes dsk_read_sector() (surové bajty bez auto-inverze).
 * Podporuje oba režimy adresování (T/S a Block), řazení sektorů
 * (ID/Phys), inverzi (--inv/--noinv), byte offset/count a file offset.
 *
 * @param disc Ukazatel na otevřený disk (RO nebo RW). Nesmí být NULL.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu: <file> [options].
 * @param force_inv Přepínač inverze (0 = auto, 1 = vynucená, -1 = vynucená ne).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí být úspěšně otevřený.
 * @post Při úspěchu existuje výstupní soubor s exportovanými daty.
 */
static en_MZDSK_RES cmd_get ( st_MZDSK_DISC *disc, int argc, char **argv, int force_inv ) {

    st_DATA_OPTIONS opts;
    init_data_options ( &opts );

    char *filepath = NULL;
    if ( parse_data_options ( argc, argv, &opts, &filepath ) != 0 ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    if ( !filepath ) {
        fprintf ( stderr, "Error: get requires a filename argument\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    uint16_t max_track = disc->tracks_rules ? ( disc->tracks_rules->total_tracks - 1 ) : 0;

    /* resolvovat startovní pozici */
    uint16_t cur_track, cur_sector;
    int32_t total_sectors;

    if ( !resolve_start_position ( &opts, disc->handler, max_track,
                                    &cur_track, &cur_sector, &total_sectors ) ) {
        if ( opts.addr_mode == RAW_ADDR_BLOCK ) {
            fprintf ( stderr, "Error: Start position out of range (origin track 0..%u)\n",
                      (unsigned) max_track );
        } else {
            fprintf ( stderr, "Error: Track %u out of range (0..%u)\n",
                      (unsigned) opts.start_track, (unsigned) max_track );
        }
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    if ( total_sectors <= 0 ) {
        fprintf ( stderr, "Error: Sector count must be positive\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Audit M-20: sledovat, zda výstupní soubor existoval před fopen.
       Při selhání v průběhu write smyčky pak můžeme uklidit za sebou
       (unlink). Pokud existoval, necháme ho v stavu, ve kterém je -
       `--file-offset` je žádoucí use case pro "embed do existujícího
       binárního souboru" a sémantika atomického přepisu by ho rozbila. */
    int file_preexisted = ( access ( filepath, F_OK ) == 0 );

    /* BUG E3: bez --overwrite neponechávat existující výstupní soubor.
     * Výjimka: --file-offset > 0 je embed do existujícího souboru (audit
     * M-20), tam je pre-existence záměrem a overwrite chrání jen plný
     * přepis od začátku. Stejný kontrakt jako fsmz/cpm/mrs get-block. */
    if ( file_preexisted && !g_allow_overwrite && opts.file_offset == 0 ) {
        fprintf ( stderr,
                  "Error: Output file '%s' already exists. Use --overwrite to replace it.\n",
                  filepath );
        return MZDSK_RES_FILE_EXISTS;
    }

    /* otevřít výstupní soubor */
    FILE *f = fopen ( filepath, "r+b" );
    if ( !f ) {
        f = fopen ( filepath, "wb" );
    }
    if ( !f ) {
        fprintf ( stderr, "Error: Cannot open file for writing: %s\n", filepath );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* file_offset: seeknout, doplnit nuly pokud soubor kratší.
     * Používáme 64-bit wrapper kvůli souborům > 2 GB (audit L-5). */
    if ( opts.file_offset > 0 ) {
        mzdsk_fseek64 ( f, 0, SEEK_END );
        mzdsk_off_t file_size = mzdsk_ftell64 ( f );

        if ( (mzdsk_off_t) opts.file_offset > file_size ) {
            mzdsk_off_t gap = (mzdsk_off_t) opts.file_offset - file_size;
            mzdsk_fseek64 ( f, 0, SEEK_END );
            /* Bloková výplň nulami místo per-byte fwrite. Kontrola
             * návratové hodnoty odhalí plný disk dřív, než budeme
             * zapisovat vlastní data (audit L-16). */
            static const uint8_t zero_chunk [ 4096 ] = { 0 };
            while ( gap > 0 ) {
                size_t chunk = ( gap > (mzdsk_off_t) sizeof ( zero_chunk ) )
                             ? sizeof ( zero_chunk )
                             : (size_t) gap;
                if ( fwrite ( zero_chunk, 1, chunk, f ) != chunk ) {
                    fprintf ( stderr, "Error: Cannot pad file to offset %llu (write failed)\n",
                              (unsigned long long) opts.file_offset );
                    fclose ( f );
                    if ( !file_preexisted ) unlink ( filepath );
                    return MZDSK_RES_UNKNOWN_ERROR;
                }
                gap -= (mzdsk_off_t) chunk;
            }
        }

        mzdsk_fseek64 ( f, (mzdsk_off_t) opts.file_offset, SEEK_SET );
    } else {
        mzdsk_fseek64 ( f, 0, SEEK_SET );
    }

    /* smyčka přes sektory */
    int32_t bytes_written = 0;
    int32_t byte_limit = opts.byte_count; /* 0 = neomezeno */
    uint8_t sector_buf[DSK_MAX_SECTOR_SIZE];

    for ( int32_t i = 0; i < total_sectors; i++ ) {

        uint16_t trk_sectors, trk_ssize;
        get_track_params ( disc->handler, cur_track, &trk_sectors, &trk_ssize );

        if ( trk_sectors == 0 ) {
            fprintf ( stderr, "Error: Track %d has no sectors\n", cur_track );
            fclose ( f );
            if ( !file_preexisted ) unlink ( filepath ); /* audit M-20 */
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        /* přečíst sektor (surová data bez auto-inverze) */
        int res = dsk_read_sector ( disc->handler, (uint8_t) cur_track,
                                     (uint8_t) cur_sector, sector_buf );
        if ( res != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Read error at track %d, sector %d\n", cur_track, cur_sector );
            fclose ( f );
            if ( !file_preexisted ) unlink ( filepath ); /* audit M-20 */
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        /* inverze přes get_inv_info */
        int do_inv = 0;
        const char *inv_txt = get_inv_info ( disc, cur_track, cur_sector, force_inv, &do_inv );

        if ( do_inv ) {
            for ( uint16_t b = 0; b < trk_ssize; b++ ) {
                sector_buf[b] = (uint8_t) ~sector_buf[b];
            }
        }

        /* určit rozsah bajtů k zapsání z tohoto sektoru */
        uint16_t start_byte = 0;
        uint16_t end_byte = trk_ssize;

        /* byte_offset: přeskočit u prvního sektoru */
        if ( i == 0 && opts.byte_offset > 0 ) {
            start_byte = (uint16_t) opts.byte_offset;
            if ( start_byte >= trk_ssize ) start_byte = trk_ssize;
        }

        /* byte_count: omezit celkový počet */
        if ( byte_limit > 0 ) {
            int32_t remaining = byte_limit - bytes_written;
            if ( remaining <= 0 ) break;
            if ( (int32_t) ( end_byte - start_byte ) > remaining ) {
                end_byte = start_byte + (uint16_t) remaining;
            }
        }

        /* zapsat do souboru */
        if ( end_byte > start_byte ) {
            size_t to_write = end_byte - start_byte;
            size_t written = fwrite ( sector_buf + start_byte, 1, to_write, f );
            if ( written != to_write ) {
                fprintf ( stderr, "Error: Write error to file\n" );
                fclose ( f );
                if ( !file_preexisted ) unlink ( filepath ); /* audit M-20 */
                return MZDSK_RES_UNKNOWN_ERROR;
            }
            bytes_written += (int32_t) to_write;
        }

        printf ( "Track %d, Sector %d (%d B) - %s\n", cur_track, cur_sector, trk_ssize, inv_txt );
        fflush ( stdout );

        /* posun na další sektor */
        if ( i + 1 < total_sectors ) {
            if ( !advance_sectors ( disc->handler, opts.order,
                                     &cur_track, &cur_sector, 1, max_track ) ) {
                fprintf ( stderr, "Error: Reached end of disk at track %d, sector %d\n",
                           cur_track, cur_sector );
                fclose ( f );
                return MZDSK_RES_UNKNOWN_ERROR;
            }
        }
    }

    fclose ( f );

    printf ( "\nExported %d bytes to %s\n", bytes_written, filepath );
    return MZDSK_RES_OK;
}


/* =========================================================================
 * Subpříkaz: put - import ze souboru na disk
 * ========================================================================= */


/**
 * @brief Importuje data ze souboru na disk.
 *
 * Čte data ze souboru a zapisuje je do sektorů přes dsk_write_sector().
 * Podporuje oba režimy adresování (T/S a Block), řazení sektorů
 * (ID/Phys), inverzi (--inv/--noinv), byte offset/count a file offset.
 *
 * Pro částečné zápisy (byte_offset > 0 nebo byte_count < sector_size)
 * nejprve přečte existující sektor, případně provede inverzi
 * do logického prostoru, přepíše relevantní bajty ze souboru,
 * zpět invertuje a zapíše. Tím se zachovají nezměněné bajty.
 *
 * @param disc Ukazatel na otevřený disk (RW). Nesmí být NULL.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu: <file> [options].
 * @param force_inv Přepínač inverze (0 = auto, 1 = vynucená, -1 = vynucená ne).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí být úspěšně otevřený pro zápis.
 * @post Při úspěchu jsou data ze souboru zapsána do příslušných sektorů.
 */
static en_MZDSK_RES cmd_put ( st_MZDSK_DISC *disc, int argc, char **argv, int force_inv ) {

    st_DATA_OPTIONS opts;
    init_data_options ( &opts );

    char *filepath = NULL;
    if ( parse_data_options ( argc, argv, &opts, &filepath ) != 0 ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    if ( !filepath ) {
        fprintf ( stderr, "Error: put requires a filename argument\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    uint16_t max_track = disc->tracks_rules ? ( disc->tracks_rules->total_tracks - 1 ) : 0;

    /* resolvovat startovní pozici */
    uint16_t cur_track, cur_sector;
    int32_t total_sectors;

    if ( !resolve_start_position ( &opts, disc->handler, max_track,
                                    &cur_track, &cur_sector, &total_sectors ) ) {
        if ( opts.addr_mode == RAW_ADDR_BLOCK ) {
            fprintf ( stderr, "Error: Start position out of range (origin track 0..%u)\n",
                      (unsigned) max_track );
        } else {
            fprintf ( stderr, "Error: Track %u out of range (0..%u)\n",
                      (unsigned) opts.start_track, (unsigned) max_track );
        }
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    if ( total_sectors <= 0 ) {
        fprintf ( stderr, "Error: Sector count must be positive\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* otevřít vstupní soubor */
    FILE *f = fopen ( filepath, "rb" );
    if ( !f ) {
        fprintf ( stderr, "Error: Cannot open file for reading: %s\n", filepath );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* zjistit velikost souboru pro pre-flight kontrolu a hlášky.
     * 64-bit wrapper kvůli souborům > 2 GB (audit L-5). */
    mzdsk_fseek64 ( f, 0, SEEK_END );
    mzdsk_off_t file_size = mzdsk_ftell64 ( f );

    /* file_offset za EOF (nebo přesně na něm) znamená, že ze souboru
       nelze načíst žádná data - odmítnout, aby se destruktivně nezapsal
       obsah uninitialized bufferu nebo nulový padding (BUG A2). */
    if ( (mzdsk_off_t) opts.file_offset >= file_size ) {
        fprintf ( stderr, "Error: --file-offset %llu is at or beyond end of file (size %lld bytes); no data to import\n",
                  (unsigned long long) opts.file_offset, (long long) file_size );
        fclose ( f );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    mzdsk_fseek64 ( f, (mzdsk_off_t) opts.file_offset, SEEK_SET );

    /* smyčka přes sektory */
    int32_t bytes_written = 0;            /**< Počet skutečně načtených/zapsaných bajtů. */
    int32_t bytes_truncated = 0;           /**< Počet bajtů, které se dopadly nulami kvůli EOF. */
    int32_t byte_limit = opts.byte_count; /* 0 = neomezeno */
    uint8_t sector_buf[DSK_MAX_SECTOR_SIZE];

    for ( int32_t i = 0; i < total_sectors; i++ ) {

        uint16_t trk_sectors, trk_ssize;
        get_track_params ( disc->handler, cur_track, &trk_sectors, &trk_ssize );

        if ( trk_sectors == 0 ) {
            fprintf ( stderr, "Error: Track %d has no sectors\n", cur_track );
            fclose ( f );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        /* přečíst existující sektor (pro partial write) */
        int res = dsk_read_sector ( disc->handler, (uint8_t) cur_track,
                                     (uint8_t) cur_sector, sector_buf );
        if ( res != EXIT_SUCCESS ) {
            /* sektor neexistuje - vynulovat */
            memset ( sector_buf, 0, trk_ssize );
        }

        /* inverze přes get_inv_info */
        int do_inv = 0;
        const char *inv_txt = get_inv_info ( disc, cur_track, cur_sector, force_inv, &do_inv );

        /* pokud invertujeme, převedeme existující data do logického prostoru */
        if ( do_inv ) {
            for ( uint16_t b = 0; b < trk_ssize; b++ ) {
                sector_buf[b] = (uint8_t) ~sector_buf[b];
            }
        }

        /* určit rozsah bajtů k zápisu do tohoto sektoru */
        uint16_t start_byte = 0;
        uint16_t end_byte = trk_ssize;

        /* byte_offset: u prvního sektoru zapsat od offsetu */
        if ( i == 0 && opts.byte_offset > 0 ) {
            start_byte = (uint16_t) opts.byte_offset;
            if ( start_byte >= trk_ssize ) start_byte = trk_ssize;
        }

        /* byte_count: omezit celkový počet */
        if ( byte_limit > 0 ) {
            int32_t remaining = byte_limit - bytes_written;
            if ( remaining <= 0 ) break;
            if ( (int32_t) ( end_byte - start_byte ) > remaining ) {
                end_byte = start_byte + (uint16_t) remaining;
            }
        }

        /* přečíst data ze souboru do sektoru */
        if ( end_byte > start_byte ) {
            size_t to_read = end_byte - start_byte;
            size_t read_count = fread ( sector_buf + start_byte, 1, to_read, f );

            /* pokud soubor kratší, zbytek je nulový - padding (uživatel bude
               varován po dokončení smyčky přes bytes_truncated) */
            if ( read_count < to_read ) {
                memset ( sector_buf + start_byte + read_count, 0,
                         to_read - read_count );
                bytes_truncated += (int32_t) ( to_read - read_count );
            }

            bytes_written += (int32_t) read_count;
        }

        /* zpět invertovat do fyzického prostoru */
        if ( do_inv ) {
            for ( uint16_t b = 0; b < trk_ssize; b++ ) {
                sector_buf[b] = (uint8_t) ~sector_buf[b];
            }
        }

        /* zapsat sektor na disk */
        res = dsk_write_sector ( disc->handler, (uint8_t) cur_track,
                                  (uint8_t) cur_sector, sector_buf );
        if ( res != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Write error at track %d, sector %d\n", cur_track, cur_sector );
            fclose ( f );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        printf ( "Track %d, Sector %d (%d B) - %s\n", cur_track, cur_sector, trk_ssize, inv_txt );
        fflush ( stdout );

        /* posun na další sektor */
        if ( i + 1 < total_sectors ) {
            if ( !advance_sectors ( disc->handler, opts.order,
                                     &cur_track, &cur_sector, 1, max_track ) ) {
                fprintf ( stderr, "Error: Reached end of disk at track %d, sector %d\n",
                           cur_track, cur_sector );
                fclose ( f );
                return MZDSK_RES_UNKNOWN_ERROR;
            }
        }
    }

    fclose ( f );

    printf ( "\nImported %d bytes from %s\n", bytes_written, filepath );
    if ( bytes_truncated > 0 ) {
        fprintf ( stderr,
            "Warning: source file ended early; last %d byte(s) of the target range were padded with zeros\n",
            bytes_truncated );
    }
    return MZDSK_RES_OK;
}


/* =========================================================================
 * Subpříkaz: dump - hexdump sektorů/bloků na stdout
 * ========================================================================= */


/**
 * @brief Vypíše hexdump sektorů/bloků na stdout.
 *
 * Čte data přes dsk_read_sector() a vypisuje formátovaný hexdump
 * přes mzdsk_hexdump(). Před každým sektorem vypíše hlavičku
 * s číslem stopy a sektoru. Podporuje inverzi (--inv/--noinv),
 * volbu znakové sady ASCII sloupce (--dump-charset, --cnv),
 * byte offset/count a oba režimy adresování.
 *
 * @param disc Ukazatel na otevřený disk (RO). Nesmí být NULL.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu: [options] (žádný poziční argument).
 * @param force_inv Přepínač inverze (0 = auto, 1 = vynucená, -1 = vynucená ne).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí být úspěšně otevřený.
 */
static en_MZDSK_RES cmd_dump ( st_MZDSK_DISC *disc, int argc, char **argv, int force_inv ) {

    st_DATA_OPTIONS opts;
    init_data_options ( &opts );

    char *filepath = NULL;
    if ( parse_data_options ( argc, argv, &opts, &filepath ) != 0 ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    if ( filepath ) {
        fprintf ( stderr, "Error: dump does not accept a filename argument (use get instead)\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    uint16_t max_track = disc->tracks_rules ? ( disc->tracks_rules->total_tracks - 1 ) : 0;

    /* resolvovat startovní pozici */
    uint16_t cur_track, cur_sector;
    int32_t total_sectors;

    if ( !resolve_start_position ( &opts, disc->handler, max_track,
                                    &cur_track, &cur_sector, &total_sectors ) ) {
        if ( opts.addr_mode == RAW_ADDR_BLOCK ) {
            fprintf ( stderr, "Error: Start position out of range (origin track 0..%u)\n",
                      (unsigned) max_track );
        } else {
            fprintf ( stderr, "Error: Track %u out of range (0..%u)\n",
                      (unsigned) opts.start_track, (unsigned) max_track );
        }
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    if ( total_sectors <= 0 ) {
        fprintf ( stderr, "Error: Sector count must be positive\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* konfigurace hexdumpu */
    st_MZDSK_HEXDUMP_CFG hd_cfg;
    mzdsk_hexdump_init ( &hd_cfg );
    hd_cfg.charset = opts.dump_charset;

    /* smyčka přes sektory */
    int32_t bytes_dumped = 0;
    int32_t byte_limit = opts.byte_count;
    uint8_t sector_buf[DSK_MAX_SECTOR_SIZE];

    for ( int32_t i = 0; i < total_sectors; i++ ) {

        uint16_t trk_sectors, trk_ssize;
        get_track_params ( disc->handler, cur_track, &trk_sectors, &trk_ssize );

        if ( trk_sectors == 0 ) {
            fprintf ( stderr, "Error: Track %d has no sectors\n", cur_track );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        /* přečíst sektor (surová data) */
        int res = dsk_read_sector ( disc->handler, (uint8_t) cur_track,
                                     (uint8_t) cur_sector, sector_buf );
        if ( res != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Read error at track %d, sector %d\n", cur_track, cur_sector );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        /* inverze přes get_inv_info - nastavíme cfg.inv místo XOR bufferu */
        int do_inv = 0;
        const char *inv_txt = get_inv_info ( disc, cur_track, cur_sector, force_inv, &do_inv );
        hd_cfg.inv = do_inv;

        /* určit rozsah bajtů k zobrazení z tohoto sektoru */
        uint16_t start_byte = 0;
        uint16_t end_byte = trk_ssize;

        if ( i == 0 && opts.byte_offset > 0 ) {
            start_byte = (uint16_t) opts.byte_offset;
            if ( start_byte >= trk_ssize ) start_byte = trk_ssize;
        }

        if ( byte_limit > 0 ) {
            int32_t remaining = byte_limit - bytes_dumped;
            if ( remaining <= 0 ) break;
            if ( (int32_t) ( end_byte - start_byte ) > remaining ) {
                end_byte = start_byte + (uint16_t) remaining;
            }
        }

        /* hlavička sektoru */
        printf ( "--- Track %d, Sector %d (%d B, %s) ---\n",
                 cur_track, cur_sector, trk_ssize, inv_txt );

        /* hexdump */
        if ( end_byte > start_byte ) {
            uint16_t dump_len = end_byte - start_byte;
            mzdsk_hexdump ( &hd_cfg, sector_buf + start_byte, dump_len );
            bytes_dumped += dump_len;
        }

        /* posun na další sektor */
        if ( i + 1 < total_sectors ) {
            if ( !advance_sectors ( disc->handler, opts.order,
                                     &cur_track, &cur_sector, 1, max_track ) ) {
                fprintf ( stderr, "Error: Reached end of disk at track %d, sector %d\n",
                           cur_track, cur_sector );
                return MZDSK_RES_UNKNOWN_ERROR;
            }
        }
    }

    printf ( "Total: %d bytes\n", bytes_dumped );
    return MZDSK_RES_OK;
}


/* =========================================================================
 * Subpříkaz: change-track
 * ========================================================================= */

/**
 * @brief Změní formát jedné stopy v DSK obrazu.
 *
 * Nastaví nový počet sektorů, velikost sektoru, filler bajt
 * a řazení sektorů pro zadanou stopu. Pokud se změní velikost
 * stopy, přesune data následujících stop. Počet sektorů je
 * omezen na DSK_MAX_SECTORS (29) - limit Extended CPC DSK formátu
 * (256B Track-Info header pojme max (256-24)/8 = 29 sector info).
 *
 * Port z fstool_dsk_change().
 *
 * @param disc Ukazatel na otevřený disk (RW). Nesmí být NULL.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu: T SECS SSIZE FILLER [ORDER|MAP].
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí být úspěšně otevřený pro zápis.
 * @post Při úspěchu je geometrie stopy změněna.
 */
static en_MZDSK_RES cmd_change_track ( st_MZDSK_DISC *disc, int argc, char **argv ) {

    if ( argc < 4 ) {
        fprintf ( stderr, "Error: change-track requires at least 4 arguments: T SECS SSIZE FILLER\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Kontrola nadbytečných argumentů */
    if ( argc > 5 ) {
        fprintf ( stderr, "Error: 'change-track' does not accept extra arguments (got '%s')\n", argv[5] );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    char *endptr;
    long val_track = strtol ( argv[0], &endptr, 10 );
    if ( *endptr != '\0' || val_track < 0 || val_track > 255 ) {
        fprintf ( stderr, "Error: Invalid track number '%s' (valid range: 0-255)\n", argv[0] );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    long val_sectors = strtol ( argv[1], &endptr, 10 );
    if ( *endptr != '\0' || val_sectors < 1 || val_sectors > DSK_MAX_SECTORS ) {
        fprintf ( stderr, "Error: Invalid sector count '%s' (valid range: 1-%d)\n", argv[1], DSK_MAX_SECTORS );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    uint8_t track = (uint8_t) val_track;
    uint8_t sectors = (uint8_t) val_sectors;

    en_DSK_SECTOR_SIZE ssize = parse_sector_size ( argv[2] );

    if ( ssize == DSK_SECTOR_SIZE_INVALID ) {
        fprintf ( stderr, "Error: Invalid sector size '%s' (valid: 128, 256, 512, 1024)\n", argv[2] );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Validace filler hodnoty (RAW-7) */
    uint8_t filler;

    if ( validate_filler ( argv[3], &filler ) != 0 ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Parsování řazení sektorů (volitelné - výchozí je normal) */
    en_DSK_SECTOR_ORDER_TYPE secorder = DSK_SEC_ORDER_NORMAL;
    uint8_t sector_map[DSK_MAX_SECTORS];
    uint8_t *sector_map_ptr = NULL;

    if ( argc >= 5 ) {
        /* Validace sector order (RAW-5) a sector mapy (RAW-6) */
        if ( parse_sector_order ( argv[4], sector_map, sectors, &secorder ) != 0 ) {
            return MZDSK_RES_UNKNOWN_ERROR;
        }
        if ( secorder == DSK_SEC_ORDER_CUSTOM ) {
            sector_map_ptr = sector_map;
        }
    }

    st_HANDLER *h = disc->handler;

    printf ( "\nChanging DSK image track %d to new low-level format ...\n", track );
    fflush ( stdout );

    /* Pokud není vlastní mapa, vygeneruj ji z pravidla */
    uint8_t local_sector_map[DSK_MAX_SECTORS];

    if ( secorder != DSK_SEC_ORDER_CUSTOM ) {
        dsk_tools_make_sector_map ( sectors, secorder, local_sector_map );
        sector_map_ptr = local_sector_map;
    }

    if ( EXIT_SUCCESS != dsk_tools_change_track ( h, NULL, track, sectors, ssize, sector_map_ptr, filler ) ) {
        fprintf ( stderr, "Error: Can't change DSK image: %s\n", dsk_error_message ( h, h->driver ) );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    printf ( "Done.\n\n" );

    return MZDSK_RES_OK;
}


/* =========================================================================
 * Subpříkaz: append-tracks
 * ========================================================================= */


/**
 * @brief Přidá nové stopy na konec DSK obrazu.
 *
 * Přidá N nových stop se zadanou geometrií (počet sektorů,
 * velikost sektoru, filler bajt a řazení sektorů).
 * Při N=0 vypíše varování a vrátí MZDSK_RES_OK bez dalších akcí.
 * Vstupní čísla se validují proti rozsahu uint8_t (0-255).
 *
 * Pro dvoustranné disky musí být N sudé.
 *
 * Port z fstool_dsk_append().
 *
 * @param disc Ukazatel na otevřený disk (RW). Nesmí být NULL.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu: N SECS SSIZE FILLER [ORDER|MAP].
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí být úspěšně otevřený pro zápis.
 * @post Při úspěchu je obraz rozšířen o N nových stop.
 */
static en_MZDSK_RES cmd_append_tracks ( st_MZDSK_DISC *disc, int argc, char **argv ) {

    if ( argc < 4 ) {
        fprintf ( stderr, "Error: append-tracks requires at least 4 arguments: N SECS SSIZE FILLER\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Kontrola nadbytečných argumentů */
    if ( argc > 5 ) {
        fprintf ( stderr, "Error: 'append-tracks' does not accept extra arguments (got '%s')\n", argv[5] );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    char *endptr;
    long val_count = strtol ( argv[0], &endptr, 10 );
    if ( *endptr != '\0' || val_count < 0 || val_count > 255 ) {
        fprintf ( stderr, "Error: Invalid track count '%s' (valid range: 0-255)\n", argv[0] );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    long val_sectors = strtol ( argv[1], &endptr, 10 );
    if ( *endptr != '\0' || val_sectors < 1 || val_sectors > DSK_MAX_SECTORS ) {
        fprintf ( stderr, "Error: Invalid sector count '%s' (valid range: 1-%d)\n", argv[1], DSK_MAX_SECTORS );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    uint8_t count_tracks = (uint8_t) val_count;
    uint8_t sectors = (uint8_t) val_sectors;

    /* Kontrola nulového počtu stop - nic se nedělá */
    if ( count_tracks == 0 ) {
        fprintf ( stderr, "Warning: 0 tracks to append, nothing to do\n" );
        return MZDSK_RES_OK;
    }

    en_DSK_SECTOR_SIZE ssize = parse_sector_size ( argv[2] );

    if ( ssize == DSK_SECTOR_SIZE_INVALID ) {
        fprintf ( stderr, "Error: Invalid sector size '%s' (valid: 128, 256, 512, 1024)\n", argv[2] );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Validace filler hodnoty (RAW-7) */
    uint8_t filler;

    if ( validate_filler ( argv[3], &filler ) != 0 ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Parsování řazení sektorů (volitelné - výchozí je normal) */
    en_DSK_SECTOR_ORDER_TYPE secorder = DSK_SEC_ORDER_NORMAL;
    uint8_t sector_map[DSK_MAX_SECTORS];
    uint8_t *sector_map_ptr = NULL;

    if ( argc >= 5 ) {
        /* Validace sector order (RAW-5) a sector mapy (RAW-6) */
        if ( parse_sector_order ( argv[4], sector_map, sectors, &secorder ) != 0 ) {
            return MZDSK_RES_UNKNOWN_ERROR;
        }
        if ( secorder == DSK_SEC_ORDER_CUSTOM ) {
            sector_map_ptr = sector_map;
        }
    }

    st_HANDLER *h = disc->handler;

    /* Alokace popisu geometrie */
    st_DSK_DESCRIPTION *dskdesc = malloc ( dsk_tools_compute_description_size ( 1 ) );

    if ( !dskdesc ) {
        fprintf ( stderr, "Error: Can't create DSK description\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Kontrola sudosti pro dvoustranné disky */
    if ( ( disc->tracks_rules->sides == 2 ) && ( count_tracks & 1 ) ) {
        fprintf ( stderr, "Error: count_tracks must be divisible by 2 for double-sided disk\n" );
        free ( dskdesc );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Audit L-26: defenzivní NULL kontrola tracks_rules */
    if ( disc->tracks_rules == NULL ) {
        fprintf ( stderr, "Error: Disk tracks_rules is not initialized\n" );
        free ( dskdesc );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    uint8_t first_new_track = disc->tracks_rules->total_tracks;
    uint16_t new_total_tracks = (uint16_t) first_new_track + count_tracks;

    if ( ( count_tracks >= DSK_MAX_TOTAL_TRACKS ) || ( new_total_tracks > DSK_MAX_TOTAL_TRACKS ) ) {
        fprintf ( stderr, "Error: Too many tracks (max %d total)\n", DSK_MAX_TOTAL_TRACKS );
        free ( dskdesc );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    printf ( "\nAdd %d new tracks into DSK image (total number of tracks after enlarge: %d) ...\n", count_tracks, new_total_tracks );
    fflush ( stdout );

    dskdesc->count_rules = 1;
    dskdesc->sides = disc->tracks_rules->sides;
    dskdesc->tracks = (uint8_t) ( new_total_tracks / dskdesc->sides );

    dsk_tools_assign_description ( dskdesc, 0, first_new_track, sectors, ssize, secorder, sector_map_ptr, filler );

    int ret = dsk_tools_add_tracks ( h, dskdesc );
    free ( dskdesc );

    if ( EXIT_SUCCESS != ret ) {
        fprintf ( stderr, "Error: Can't append DSK image: %s\n", dsk_error_message ( h, h->driver ) );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    printf ( "Done.\n\n" );

    return MZDSK_RES_OK;
}


/* =========================================================================
 * Subpříkaz: shrink
 * ========================================================================= */


/**
 * @brief Zmenší DSK obraz na zadaný celkový počet absolutních stop.
 *
 * Odstraní stopy od konce obrazu, dokud celkový počet stop neodpovídá
 * zadané hodnotě.
 *
 * Port z fstool_dsk_shrink().
 *
 * @param disc Ukazatel na otevřený disk (RW). Nesmí být NULL.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu: N (cílový celkový počet stop).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí být úspěšně otevřený pro zápis.
 * @post Při úspěchu má obraz právě N absolutních stop.
 */
static en_MZDSK_RES cmd_shrink ( st_MZDSK_DISC *disc, int argc, char **argv ) {

    if ( argc < 1 ) {
        fprintf ( stderr, "Error: shrink requires 1 argument: N (total tracks)\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Kontrola nadbytečných argumentů */
    if ( argc > 1 ) {
        fprintf ( stderr, "Error: 'shrink' does not accept extra arguments (got '%s')\n", argv[1] );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    char *endptr;
    long val_total = strtol ( argv[0], &endptr, 10 );
    /* Minimum je 1 - shrink 0 by zlikvidoval celý obsah disku bez potvrzení.
     * Uživatel zpravidla chtěl jen zmenšit počet stop. Pokud opravdu chce
     * vytvořit prázdný soubor, přepíše ho přímo. Audit H-18. */
    if ( *endptr != '\0' || val_total < 1 || val_total > 255 ) {
        fprintf ( stderr, "Error: Invalid track count '%s' (valid range: 1-255)\n", argv[0] );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    uint8_t total_tracks = (uint8_t) val_total;

    st_HANDLER *h = disc->handler;

    /* Audit L-26: defenzivní NULL kontrola tracks_rules */
    if ( disc->tracks_rules == NULL ) {
        fprintf ( stderr, "Error: Disk tracks_rules is not initialized\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Zjistíme aktuální počet stop */
    uint8_t current_tracks = disc->tracks_rules->total_tracks;

    if ( total_tracks == current_tracks ) {
        printf ( "\nDisk already has %d track(s), nothing to do.\n\n", total_tracks );
        return MZDSK_RES_OK;
    }

    if ( total_tracks > current_tracks ) {
        fprintf ( stderr, "Error: Cannot shrink from %d to %d tracks (use append-tracks to grow)\n",
                  current_tracks, total_tracks );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    printf ( "\nShrink DSK image from %d to %d track(s) ...\n", current_tracks, total_tracks );
    fflush ( stdout );

    if ( EXIT_SUCCESS != dsk_tools_shrink_image ( h, NULL, total_tracks ) ) {
        fprintf ( stderr, "Error: Can't shrink DSK image: %s\n", dsk_error_message ( h, h->driver ) );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    printf ( "Done.\n\n" );

    return MZDSK_RES_OK;
}
