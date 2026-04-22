/**
 * @file   mzdsk_dsk.c
 * @brief  Diagnostický a editační nástroj pro Extended CPC DSK kontejner.
 *
 * Pracuje přímo s DSK kontejnerem - bez vyšší vrstvy mzdsk_global
 * (ta provádí autofix, který by zde byl nežádoucí). Umožňuje inspekci
 * hlaviček, diagnostiku, opravu a editaci DSK obrazů.
 *
 * @par Subpříkazy:
 * - info       - raw inspekce DSK hlavičky (file_info, creator, tracks, sides, tsize)
 * - tracks     - detailní výpis track headerů (--abstrack T pro jednu stopu)
 * - check      - diagnostika bez opravy (exit code 0 = OK, 1 = chyby)
 * - repair     - diagnostika + oprava opravitelných chyb
 * - edit-header - editace DSK hlavičky (--creator TEXT)
 * - edit-track  - editace track headeru (T --gap HH --filler HH --track-num N --side N --fdc-status IDX STS1 STS2)
 *
 * @par Globální volby:
 * - --version       verze nástroje
 * - --lib-versions  verze všech použitých knihoven
 * - --help          nápověda
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
#include <string.h>
#include <getopt.h>
#include <errno.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "tools/common/mzdisk_cli_version.h"


/** @brief Verze nástroje mzdsk-dsk. */
#define MZDSK_DSK_VERSION "1.5.7"


/* =========================================================================
 * Vpřed deklarované funkce
 * ========================================================================= */

static void print_usage ( FILE *out, const char *progname );
static void print_version ( void );
static void print_lib_versions ( void );

static int cmd_info ( st_HANDLER *h );
static int cmd_tracks ( st_HANDLER *h, int argc, char **argv );
static int cmd_check ( st_HANDLER *h );
static int cmd_repair ( st_HANDLER *h );
static int cmd_edit_header ( st_HANDLER *h, int argc, char **argv );
static int cmd_edit_track ( st_HANDLER *h, int argc, char **argv );


/* =========================================================================
 * Logovací callback - přeposílá zprávy z dsk_tools na stderr
 * ========================================================================= */


/**
 * @brief Logovací callback pro dsk_tools knihovnu.
 *
 * Přeposílá logovací zprávy z knihovny na stderr s prefixem
 * podle úrovně.
 *
 * @param level Úroveň logování.
 * @param msg Formátovaná zpráva.
 * @param user_data Nepoužito (vždy NULL).
 */
static void log_callback ( int level, const char *msg, void *user_data ) {
    (void) user_data;
    const char *prefix = "";
    switch ( level ) {
        case DSK_LOG_INFO:    prefix = "INFO"; break;
        case DSK_LOG_WARNING: prefix = "WARNING"; break;
        case DSK_LOG_ERROR:   prefix = "ERROR"; break;
    }
    fprintf ( stderr, "[%s] %s\n", prefix, msg );
}


/* =========================================================================
 * Pomocné funkce pro přímé otevírání DSK
 * ========================================================================= */


/**
 * @brief Otevře DSK soubor přímo přes generic_driver (bez mzdsk_global).
 *
 * Tato funkce obchází mzdsk_disc_open(), která provádí autofix.
 * Pro diagnostický nástroj potřebujeme vidět DSK v surovém stavu.
 * Používá file driver - vhodné pro read-only subpříkazy (info, tracks, check).
 *
 * @param handler Pre-alokovaný handler.
 * @param driver Pre-alokovaný driver.
 * @param filename Cesta k DSK souboru.
 * @param mode Režim otevření.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int open_dsk_direct ( st_HANDLER *handler, st_DRIVER *driver, char *filename, en_FILE_DRIVER_OPEN_MODE mode ) {

    generic_driver_file_init ( driver );

    if ( generic_driver_open_file ( handler, driver, filename, mode ) == NULL ) {
        fprintf ( stderr, "Error: Cannot open '%s': %s\n",
            filename, generic_driver_error_message ( handler, driver ) );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Otevře DSK soubor do paměti pro atomický zápis.
 *
 * Načte celý obsah DSK souboru do paměti přes paměťový driver.
 * Veškeré změny se provádějí v RAM a zapíší se zpět až explicitním
 * voláním generic_driver_save_memory(). Pokud operace selže uprostřed,
 * původní soubor zůstane nezměněn.
 * Stejně jako open_dsk_direct() obchází autofix z mzdsk_disc_open_memory().
 *
 * @param handler Pre-alokovaný handler.
 * @param filename Cesta k DSK souboru.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int open_dsk_direct_memory ( st_HANDLER *handler, char *filename ) {

    if ( generic_driver_open_memory_from_file ( handler, &g_memory_driver_realloc, filename ) == NULL ) {
        fprintf ( stderr, "Error: Cannot load '%s' into memory\n", filename );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Zavře přímo otevřený DSK handler.
 *
 * @param handler Handler k uzavření.
 */
static void close_dsk_direct ( st_HANDLER *handler ) {
    generic_driver_close ( handler );
}


/* =========================================================================
 * Hlavní vstupní bod
 * ========================================================================= */


/**
 * @brief Vstupní bod programu mzdsk-dsk.
 *
 * Zpracuje příkazovou řádku, otevře DSK obraz přímo (bez autofix)
 * a deleguje provedení na příslušnou funkci subpříkazu.
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
        { "version",      no_argument, NULL, 'V' },
        { "lib-versions", no_argument, NULL, 'L' },
        { "help",         no_argument, NULL, 'h' },
        { NULL,           0,           NULL,  0  }
    };

    /* Parsování globálních voleb přes getopt_long */
    optind = 1;
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "+h", long_options, NULL ) ) != -1 ) {
        switch ( opt ) {
            case 'V':
                print_version();
                return EXIT_SUCCESS;
            case 'L':
                print_lib_versions();
                return EXIT_SUCCESS;
            case 'h':
                print_usage ( stdout, argv[0] );
                return EXIT_SUCCESS;
            default:
                print_usage ( stderr, argv[0] );
                return EXIT_FAILURE;
        }
    }

    /* DSK soubor je poziční argument po volbách */
    if ( optind >= argc ) {
        print_usage ( stderr, argv[0] );
        return EXIT_FAILURE;
    }

    char *dsk_filename = argv[optind++];

    /* Druhé kolo: globální volby mezi DSK souborem a subpříkazem */
    while ( ( opt = getopt_long ( argc, argv, "+h", long_options, NULL ) ) != -1 ) {
        switch ( opt ) {
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

    /* Zbylé poziční argumenty pro subpříkaz.
     * sub_argv[0] = subcommand name (jako argv[0] pro getopt v subpříkazech) */
    int sub_argc = argc - optind + 1;
    char **sub_argv = argv + optind - 1;

    /* Určení režimu otevření podle subpříkazu. Write subkomanda načítají
     * DSK do paměti a zapisují zpět atomicky až po úspěšném dokončení. */
    en_FILE_DRIVER_OPEN_MODE open_mode;
    int is_write_op;

    if ( strcmp ( subcmd, "info" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RO;
        is_write_op = 0;
    } else if ( strcmp ( subcmd, "tracks" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RO;
        is_write_op = 0;
    } else if ( strcmp ( subcmd, "check" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RO;
        is_write_op = 0;
    } else if ( strcmp ( subcmd, "repair" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RW;
        is_write_op = 1;
    } else if ( strcmp ( subcmd, "edit-header" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RW;
        is_write_op = 1;
    } else if ( strcmp ( subcmd, "edit-track" ) == 0 ) {
        open_mode = FILE_DRIVER_OPMODE_RW;
        is_write_op = 1;
    } else {
        fprintf ( stderr, "Error: Unknown subcommand '%s'\n\n", subcmd );
        print_usage ( stderr, argv[0] );
        return EXIT_FAILURE;
    }

    /* Otevření DSK přímo (bez mzdsk_global autofix).
     * Zápisové subpříkazy (repair, edit-header, edit-track) používají
     * paměťový driver - změny proběhnou v RAM a zapíší se zpět jedním
     * voláním generic_driver_save_memory() až po úspěšném dokončení.
     * Pokud operace selže uprostřed, původní soubor zůstává nezměněn.
     * Čtecí subpříkazy (info, tracks, check) používají file driver. */
    st_HANDLER handler;
    st_DRIVER driver;

    /* Nastavení logovacího callbacku */
    dsk_tools_set_log_cb ( log_callback, NULL );

    if ( is_write_op ) {
        if ( EXIT_SUCCESS != open_dsk_direct_memory ( &handler, dsk_filename ) ) {
            return EXIT_FAILURE;
        }
    } else {
        if ( EXIT_SUCCESS != open_dsk_direct ( &handler, &driver, dsk_filename, open_mode ) ) {
            return EXIT_FAILURE;
        }
    }

    /* Validace DSK magic (file_info = "EXTENDED CPC DSK File\r\nDisk-Info\r\n").
       Odmítneme operace nad souborem, který nevypadá jako DSK, aby uživatel
       omylem nemodifikoval libovolný soubor přes edit-header/edit-track
       nebo neviděl nesmyslný výstup z info/tracks (BUG 6).
       Výjimka: `check` má vlastní diagnostiku včetně BAD_FILEINFO flagu,
       která uživateli ukáže, že soubor není DSK. */
    if ( strcmp ( subcmd, "check" ) != 0 ) {
        if ( EXIT_SUCCESS != dsk_tools_check_dsk_fileinfo ( &handler ) ) {
            fprintf ( stderr,
                "Error: '%s' is not an Extended CPC DSK file (magic mismatch).\n"
                "       Use 'check' to see details.\n",
                dsk_filename );
            close_dsk_direct ( &handler );
            return EXIT_FAILURE;
        }
    }

    /* Dispatch subpříkazu */
    int result = EXIT_FAILURE;

    if ( strcmp ( subcmd, "info" ) == 0 ) {
        result = cmd_info ( &handler );
    } else if ( strcmp ( subcmd, "tracks" ) == 0 ) {
        result = cmd_tracks ( &handler, sub_argc, sub_argv );
    } else if ( strcmp ( subcmd, "check" ) == 0 ) {
        result = cmd_check ( &handler );
    } else if ( strcmp ( subcmd, "repair" ) == 0 ) {
        result = cmd_repair ( &handler );
    } else if ( strcmp ( subcmd, "edit-header" ) == 0 ) {
        result = cmd_edit_header ( &handler, sub_argc, sub_argv );
    } else if ( strcmp ( subcmd, "edit-track" ) == 0 ) {
        result = cmd_edit_track ( &handler, sub_argc, sub_argv );
    }

    /* Uložení změn do souboru při úspěšné zápisové operaci */
    if ( is_write_op && result == EXIT_SUCCESS ) {
        if ( EXIT_SUCCESS != generic_driver_save_memory ( &handler, dsk_filename ) ) {
            fprintf ( stderr, "Error: Could not save DSK file '%s'\n", dsk_filename );
            close_dsk_direct ( &handler );
            return EXIT_FAILURE;
        }
    }

    close_dsk_direct ( &handler );

    return result;
}


/* =========================================================================
 * Subpříkaz: info
 * ========================================================================= */


/**
 * @brief Subpříkaz info - raw inspekce DSK hlavičky.
 *
 * Vypíše file_info, creator, tracks, sides a tabulku tsize
 * pro všechny stopy.
 *
 * @param h Handler.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int cmd_info ( st_HANDLER *h ) {

    st_DSK_HEADER_INFO hinfo;
    if ( EXIT_SUCCESS != dsk_tools_read_header_info ( h, &hinfo ) ) {
        fprintf ( stderr, "Error: Cannot read DSK header\n" );
        return EXIT_FAILURE;
    }

    /* Tisknutelná část file_info */
    char fi_str[DSK_FILEINFO_FIELD_LENGTH + 1];
    int fi = 0;
    while ( fi < DSK_FILEINFO_FIELD_LENGTH && hinfo.file_info[fi] >= 0x20 ) {
        fi_str[fi] = hinfo.file_info[fi];
        fi++;
    }
    fi_str[fi] = '\0';

    /* Tisknutelná část creator */
    char cr_str[DSK_CREATOR_FIELD_LENGTH + 1];
    int ci = 0;
    while ( ci < DSK_CREATOR_FIELD_LENGTH && hinfo.creator[ci] >= 0x20 ) {
        cr_str[ci] = hinfo.creator[ci];
        ci++;
    }
    cr_str[ci] = '\0';

    printf ( "File info : %s\n", fi_str );
    printf ( "Creator   : %s\n", cr_str );
    printf ( "Tracks    : %d\n", hinfo.tracks );
    printf ( "Sides     : %d\n", hinfo.sides );

    uint8_t total_tracks = hinfo.tracks * hinfo.sides;

    /* Velikost souboru */
    uint32_t file_size = 0;
    if ( EXIT_SUCCESS == generic_driver_get_size ( h, &file_size ) ) {
        printf ( "File size : %u bytes\n", (unsigned) file_size );
    }

    /* Trailing data */
    uint32_t trailing_offset = 0, trailing_size = 0;
    if ( EXIT_SUCCESS == dsk_tools_detect_trailing_data ( h, &trailing_offset, &trailing_size ) ) {
        if ( trailing_size > 0 ) {
            printf ( "Trailing  : %u bytes at offset 0x%08x\n", (unsigned) trailing_size, (unsigned) trailing_offset );
        }
    }

    /* Tabulka tsize */
    printf ( "\nTrack size table:\n" );
    printf ( "  %-8s  %-10s  %s\n", "AbsTrack", "tsize(hex)", "tsize(bytes)" );
    printf ( "  %-8s  %-10s  %s\n", "--------", "----------", "------------" );

    uint8_t i;
    for ( i = 0; i < total_tracks; i++ ) {
        uint16_t bytes = dsk_decode_track_size ( hinfo.tsize[i] );
        printf ( "  %-8d  0x%02x        %d\n", i, hinfo.tsize[i], bytes );
    }

    return EXIT_SUCCESS;
}


/* =========================================================================
 * Subpříkaz: tracks
 * ========================================================================= */


/**
 * @brief Vypíše detailní informace o jedné stopě.
 *
 * @param h Handler.
 * @param abstrack Absolutní číslo stopy.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int print_track_detail ( st_HANDLER *h, uint8_t abstrack ) {

    st_DSK_TRACK_HEADER_INFO thinfo;
    if ( EXIT_SUCCESS != dsk_tools_read_track_header_info ( h, abstrack, &thinfo ) ) {
        fprintf ( stderr, "Error: Cannot read track header for abstrack %d\n", abstrack );
        return EXIT_FAILURE;
    }

    printf ( "Track %d:\n", abstrack );
    printf ( "  Track number : %d\n", thinfo.track );
    printf ( "  Side         : %d\n", thinfo.side );
    printf ( "  Sector size  : %d (0x%02x)\n", dsk_decode_sector_size ( thinfo.ssize ), thinfo.ssize );
    printf ( "  Sectors      : %d\n", thinfo.sectors );
    printf ( "  GAP#3        : 0x%02x\n", thinfo.gap );
    printf ( "  Filler       : 0x%02x\n", thinfo.filler );

    if ( thinfo.sectors > 0 ) {
        printf ( "  Sectors detail:\n" );
        printf ( "    %-4s  %-6s  %-5s  %-5s  %-8s  %-8s\n",
            "Idx", "SectID", "Track", "Side", "FDC_STS1", "FDC_STS2" );
        printf ( "    %-4s  %-6s  %-5s  %-5s  %-8s  %-8s\n",
            "----", "------", "-----", "-----", "--------", "--------" );

        uint8_t si;
        for ( si = 0; si < thinfo.sectors && si < DSK_MAX_SECTORS; si++ ) {
            printf ( "    %-4d  %-6d  %-5d  %-5d  0x%02x      0x%02x\n",
                si,
                thinfo.sinfo[si].sector,
                thinfo.sinfo[si].track,
                thinfo.sinfo[si].side,
                thinfo.sinfo[si].fdc_sts1,
                thinfo.sinfo[si].fdc_sts2 );
        }
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Subpříkaz tracks - detailní výpis track headerů.
 *
 * Volba --abstrack T omezí výpis na jednu stopu.
 *
 * @param h Handler.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int cmd_tracks ( st_HANDLER *h, int argc, char **argv ) {

    int selected_track = -1; /* -1 = všechny */

    /* Parsování voleb */
    static struct option track_options[] = {
        { "abstrack", required_argument, NULL, 't' },
        { NULL,       0,                 NULL,  0  }
    };

    optind = 1; /* reset getopt - argv[0] je název subpříkazu */
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "", track_options, NULL ) ) != -1 ) {
        switch ( opt ) {
            case 't': {
                char *endp;
                long val = strtol ( optarg, &endp, 10 );
                if ( *endp != '\0' || val < 0 || val > 255 ) {
                    fprintf ( stderr, "Error: Invalid abstrack value '%s'\n", optarg );
                    return EXIT_FAILURE;
                }
                selected_track = (int) val;
                break;
            }
            default:
                return EXIT_FAILURE;
        }
    }

    /* Zjistit počet stop */
    st_DSK_HEADER_INFO hinfo;
    if ( EXIT_SUCCESS != dsk_tools_read_header_info ( h, &hinfo ) ) {
        fprintf ( stderr, "Error: Cannot read DSK header\n" );
        return EXIT_FAILURE;
    }

    uint8_t total_tracks = hinfo.tracks * hinfo.sides;

    if ( selected_track >= 0 ) {
        if ( selected_track >= total_tracks ) {
            fprintf ( stderr, "Error: Track %d out of range (0..%d)\n", selected_track, total_tracks - 1 );
            return EXIT_FAILURE;
        }
        return print_track_detail ( h, (uint8_t) selected_track );
    }

    /* Výpis všech stop */
    uint8_t i;
    for ( i = 0; i < total_tracks; i++ ) {
        if ( i > 0 ) printf ( "\n" );
        if ( EXIT_SUCCESS != print_track_detail ( h, i ) ) return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/* =========================================================================
 * Subpříkaz: check
 * ========================================================================= */


/**
 * @brief Pomocná funkce pro tisk diagnostického výsledku.
 *
 * @param diag Diagnostický výsledek.
 */
static void print_diag_report ( const st_DSK_DIAG_RESULT *diag ) {

    /* Tisknutelná část creator */
    char cr_str[DSK_CREATOR_FIELD_LENGTH + 1];
    int ci = 0;
    while ( ci < DSK_CREATOR_FIELD_LENGTH && diag->creator[ci] >= 0x20 ) {
        cr_str[ci] = diag->creator[ci];
        ci++;
    }
    cr_str[ci] = '\0';

    printf ( "Creator        : %s\n", cr_str );
    printf ( "Header tracks  : %d (tracks*sides)\n", diag->header_tracks );
    printf ( "Actual tracks  : %d\n", diag->actual_tracks );
    printf ( "Sides          : %d\n", diag->sides );
    printf ( "tsize diffs    : %d\n", diag->tsize_differences );

    if ( diag->actual_file_size > 0 ) {
        printf ( "Expected size  : %u bytes\n", (unsigned) diag->expected_image_size );
        printf ( "Actual size    : %u bytes\n", (unsigned) diag->actual_file_size );
    }

    /* Image-level flagy */
    printf ( "\nImage flags    : 0x%04x", diag->image_flags );
    if ( diag->image_flags == DSK_DIAG_IMAGE_OK ) {
        printf ( " (OK)" );
    } else {
        if ( diag->image_flags & DSK_DIAG_IMAGE_BAD_FILEINFO )   printf ( " BAD_FILEINFO" );
        if ( diag->image_flags & DSK_DIAG_IMAGE_BAD_TRACKCOUNT ) printf ( " BAD_TRACKCOUNT" );
        if ( diag->image_flags & DSK_DIAG_IMAGE_ODD_DOUBLE )     printf ( " ODD_DOUBLE" );
        if ( diag->image_flags & DSK_DIAG_IMAGE_BAD_TSIZE )      printf ( " BAD_TSIZE" );
        if ( diag->image_flags & DSK_DIAG_IMAGE_TRAILING_DATA )  printf ( " TRAILING_DATA" );
        if ( diag->image_flags & DSK_DIAG_IMAGE_TRACK_ERRORS )   printf ( " TRACK_ERRORS" );
        if ( diag->image_flags & DSK_DIAG_IMAGE_TRACKCOUNT_EXCEEDED ) printf ( " TRACKCOUNT_EXCEEDED" );
    }
    printf ( "\n" );

    /* Audit H-11: detailní report pro TRACKCOUNT_EXCEEDED */
    if ( diag->image_flags & DSK_DIAG_IMAGE_TRACKCOUNT_EXCEEDED ) {
        printf ( "\nHeader declares tracks=%d, sides=%d (total=%d), but driver supports max %d tracks.\n",
                 diag->raw_header_tracks, diag->sides,
                 (int) diag->raw_header_tracks * diag->sides,
                 DSK_MAX_TOTAL_TRACKS );
        printf ( "Run 'mzdsk-dsk %s repair' to clamp the header to %d track(s).\n",
                 "<file>", DSK_MAX_TOTAL_TRACKS / diag->sides );
    }

    /* Per-track chyby (pouze ty s chybami) */
    int has_track_issues = 0;
    uint8_t i;
    for ( i = 0; i < diag->count_tracks; i++ ) {
        if ( diag->tracks[i].flags != DSK_DIAG_TRACK_OK ) {
            has_track_issues = 1;
            break;
        }
    }

    if ( has_track_issues ) {
        printf ( "\nTrack issues:\n" );
        for ( i = 0; i < diag->count_tracks; i++ ) {
            const st_DSK_DIAG_TRACK *dt = &diag->tracks[i];
            if ( dt->flags == DSK_DIAG_TRACK_OK ) continue;

            printf ( "  Track %3d (offset 0x%08x): flags=0x%04x",
                dt->abstrack, (unsigned) dt->offset, dt->flags );

            if ( dt->flags & DSK_DIAG_TRACK_NO_TRACKINFO )    printf ( " NO_TRACKINFO" );
            if ( dt->flags & DSK_DIAG_TRACK_READ_ERROR )      printf ( " READ_ERROR" );
            if ( dt->flags & DSK_DIAG_TRACK_BAD_TRACK_NUM )   printf ( " BAD_TRACK_NUM(hdr=%d,exp=%d)", dt->hdr_track, dt->expected_track );
            if ( dt->flags & DSK_DIAG_TRACK_BAD_SIDE_NUM )    printf ( " BAD_SIDE_NUM(hdr=%d,exp=%d)", dt->hdr_side, dt->expected_side );
            if ( dt->flags & DSK_DIAG_TRACK_BAD_SECTORS )     printf ( " BAD_SECTORS(%d)", dt->sectors );
            if ( dt->flags & DSK_DIAG_TRACK_BAD_SSIZE )       printf ( " BAD_SSIZE(0x%02x)", dt->ssize );
            if ( dt->flags & DSK_DIAG_TRACK_BAD_TSIZE )       printf ( " BAD_TSIZE(hdr=0x%02x,calc=0x%02x)", dt->header_tsize, dt->computed_tsize );
            if ( dt->flags & DSK_DIAG_TRACK_DATA_UNREADABLE ) printf ( " DATA_UNREADABLE" );

            printf ( "\n" );
        }
    }

    /* Souhrn */
    int repairable = dsk_tools_diag_has_repairable_errors ( diag );
    int fatal = dsk_tools_diag_has_fatal_errors ( diag );

    printf ( "\nResult: " );
    if ( !repairable && !fatal ) {
        printf ( "DSK is OK!\n" );
    } else {
        if ( repairable ) printf ( "repairable errors found" );
        if ( repairable && fatal ) printf ( ", " );
        if ( fatal ) printf ( "FATAL errors found" );
        printf ( "\n" );
    }
}


/**
 * @brief Subpříkaz check - diagnostika bez opravy.
 *
 * Exit code 0 = OK, 1 = nalezeny chyby.
 *
 * @param h Handler.
 * @return EXIT_SUCCESS pokud je obraz v pořádku, EXIT_FAILURE pokud má chyby.
 */
static int cmd_check ( st_HANDLER *h ) {

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( h );
    if ( diag == NULL ) {
        fprintf ( stderr, "Error: Diagnostics failed\n" );
        return EXIT_FAILURE;
    }

    print_diag_report ( diag );

    int has_errors = dsk_tools_diag_has_repairable_errors ( diag )
                   || dsk_tools_diag_has_fatal_errors ( diag );

    dsk_tools_destroy_diag_result ( diag );

    return has_errors ? EXIT_FAILURE : EXIT_SUCCESS;
}


/* =========================================================================
 * Subpříkaz: repair
 * ========================================================================= */


/**
 * @brief Subpříkaz repair - diagnostika + oprava.
 *
 * Iterativně provádí dvojici diagnose/repair dokud buď nezmizí všechny
 * opravitelné chyby, nebo dokud se stav nepřestane měnit, max
 * REPAIR_MAX_PASSES iterací. Kaskádování je nutné, protože některé
 * opravy odhalí další chyby, které předchozí diagnóza neviděla - typicky
 * oprava BAD_TRACKCOUNT přepočítá expected_image_size a teprve pak
 * proběhne truncate TRAILING_DATA (BUG B6).
 *
 * @param h Handler.
 * @return EXIT_SUCCESS pokud je obraz opraven/OK, EXIT_FAILURE při neopravitelných chybách.
 */
static int cmd_repair ( st_HANDLER *h ) {

    /** Horní mez iterací - víc než dost na kaskádu BAD_TRACKCOUNT ->
        BAD_TSIZE -> TRAILING_DATA. Slouží jako ochrana proti patologické
        smyčce, pokud by se obraz nikdy nedostal do čistého stavu. */
    const int REPAIR_MAX_PASSES = 4;

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( h );
    if ( diag == NULL ) {
        fprintf ( stderr, "Error: Diagnostics failed\n" );
        return EXIT_FAILURE;
    }

    print_diag_report ( diag );

    if ( !dsk_tools_diag_has_repairable_errors ( diag ) ) {
        int fatal = dsk_tools_diag_has_fatal_errors ( diag );
        dsk_tools_destroy_diag_result ( diag );
        if ( fatal ) {
            fprintf ( stderr, "Warning: Fatal errors cannot be repaired automatically.\n" );
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    printf ( "\nRepairing...\n" );

    int result = EXIT_SUCCESS;
    int passes = 0;

    while ( 1 ) {
        passes++;

        if ( EXIT_SUCCESS != dsk_tools_repair ( h, diag ) ) {
            fprintf ( stderr, "Error: Repair failed (pass %d)\n", passes );
            result = EXIT_FAILURE;
            break;
        }

        /* Po opravě přediagnostikovat - některé chyby jsou odhalitelné
           až po úpravě hlavičky (např. TRAILING_DATA po BAD_TRACKCOUNT). */
        dsk_tools_destroy_diag_result ( diag );
        diag = dsk_tools_diagnose ( h );
        if ( diag == NULL ) {
            fprintf ( stderr, "Error: Re-diagnostics failed after pass %d\n", passes );
            result = EXIT_FAILURE;
            break;
        }

        if ( !dsk_tools_diag_has_repairable_errors ( diag ) ) {
            printf ( "Repair completed successfully (%d pass%s).\n",
                     passes, passes == 1 ? "" : "es" );
            break;
        }

        if ( passes >= REPAIR_MAX_PASSES ) {
            fprintf ( stderr, "Error: Repair did not converge after %d passes; remaining errors:\n",
                      REPAIR_MAX_PASSES );
            print_diag_report ( diag );
            result = EXIT_FAILURE;
            break;
        }

        printf ( "Additional errors detected after pass %d, continuing...\n", passes );
    }

    if ( diag != NULL ) {
        if ( dsk_tools_diag_has_fatal_errors ( diag ) ) {
            fprintf ( stderr, "Warning: Fatal errors cannot be repaired automatically.\n" );
            result = EXIT_FAILURE;
        }
        dsk_tools_destroy_diag_result ( diag );
    }

    return result;
}


/* =========================================================================
 * Subpříkaz: edit-header
 * ========================================================================= */


/**
 * @brief Subpříkaz edit-header - editace DSK hlavičky.
 *
 * @param h Handler.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int cmd_edit_header ( st_HANDLER *h, int argc, char **argv ) {

    char *new_creator = NULL;

    static struct option header_options[] = {
        { "creator", required_argument, NULL, 'c' },
        { NULL,      0,                 NULL,  0  }
    };

    optind = 1;
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "", header_options, NULL ) ) != -1 ) {
        switch ( opt ) {
            case 'c':
                new_creator = optarg;
                break;
            default:
                fprintf ( stderr, "Usage: edit-header --creator TEXT\n" );
                return EXIT_FAILURE;
        }
    }

    if ( new_creator == NULL ) {
        fprintf ( stderr, "Error: --creator is required\n" );
        fprintf ( stderr, "Usage: edit-header --creator TEXT\n" );
        return EXIT_FAILURE;
    }

    if ( strlen ( new_creator ) > DSK_CREATOR_FIELD_LENGTH ) {
        fprintf ( stderr, "Error: Creator string too long (max %d characters)\n", DSK_CREATOR_FIELD_LENGTH );
        return EXIT_FAILURE;
    }

    if ( EXIT_SUCCESS != dsk_tools_set_creator ( h, new_creator ) ) {
        fprintf ( stderr, "Error: Cannot set creator\n" );
        return EXIT_FAILURE;
    }

    printf ( "Creator set to: %s\n", new_creator );
    return EXIT_SUCCESS;
}


/* =========================================================================
 * Subpříkaz: edit-track
 * ========================================================================= */


/**
 * @brief Subpříkaz edit-track - editace track headeru.
 *
 * Prvním pozičním argumentem je abstrack. Volby: --gap, --filler,
 * --track-num, --side, --fdc-status.
 *
 * @param h Handler.
 * @param argc Počet argumentů subpříkazu.
 * @param argv Argumenty subpříkazu.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int cmd_edit_track ( st_HANDLER *h, int argc, char **argv ) {

    if ( argc < 2 ) {
        fprintf ( stderr, "Usage: edit-track ABSTRACK [--gap HH] [--filler HH] [--track-num N] [--side N] [--fdc-status IDX STS1 STS2] [--sector-ids MAP] [--sector-id IDX:ID]\n" );
        return EXIT_FAILURE;
    }

    /* argv[0] = "edit-track" (název subpříkazu), argv[1] = abstrack */
    char *endp;
    long abstrack_val = strtol ( argv[1], &endp, 10 );
    if ( *endp != '\0' || abstrack_val < 0 || abstrack_val > 255 ) {
        fprintf ( stderr, "Error: Invalid abstrack value '%s'\n", argv[1] );
        return EXIT_FAILURE;
    }
    uint8_t abstrack = (uint8_t) abstrack_val;

    /* Parsování voleb. Sentinel -1 = "neměnit", 0-255 = nová hodnota bajtu.
       Dříve byl sentinel 0xFF, což bránilo nastavit legitimní hodnotu 0xFF
       (běžný filler/gap u 5.25" disket - BUG 4). */
    int16_t new_track_num = -1;
    int16_t new_side = -1;
    int16_t new_gap = -1;
    int16_t new_filler = -1;

    int fdc_sector_idx = -1;
    uint8_t fdc_sts1 = 0;
    uint8_t fdc_sts2 = 0;

    /* --sector-ids: pole všech ID najednou */
    uint8_t sector_ids_map[DSK_MAX_SECTORS];
    int sector_ids_count = 0;

    /* --sector-id: jednotlivé editace (idx, new_id) */
    int single_edits[DSK_MAX_SECTORS][2];
    int single_edit_count = 0;

    static struct option edit_track_options[] = {
        { "gap",        required_argument, NULL, 'g' },
        { "filler",     required_argument, NULL, 'f' },
        { "track-num",  required_argument, NULL, 'T' },
        { "side",       required_argument, NULL, 's' },
        { "fdc-status", required_argument, NULL, 'F' },
        { "sector-ids", required_argument, NULL, 'I' },
        { "sector-id",  required_argument, NULL, 'i' },
        { NULL,         0,                 NULL,  0  }
    };

    optind = 2; /* Přeskočit argv[0] (subcommand) a argv[1] (abstrack) */
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "", edit_track_options, NULL ) ) != -1 ) {
        switch ( opt ) {
            case 'g': {
                unsigned long val = strtoul ( optarg, &endp, 16 );
                if ( *endp != '\0' || val > 0xFF ) {
                    fprintf ( stderr, "Error: Invalid gap value '%s'\n", optarg );
                    return EXIT_FAILURE;
                }
                new_gap = (int16_t) val;
                break;
            }
            case 'f': {
                unsigned long val = strtoul ( optarg, &endp, 16 );
                if ( *endp != '\0' || val > 0xFF ) {
                    fprintf ( stderr, "Error: Invalid filler value '%s'\n", optarg );
                    return EXIT_FAILURE;
                }
                new_filler = (int16_t) val;
                break;
            }
            case 'T': {
                long val = strtol ( optarg, &endp, 10 );
                if ( *endp != '\0' || val < 0 || val > 255 ) {
                    fprintf ( stderr, "Error: Invalid track-num value '%s'\n", optarg );
                    return EXIT_FAILURE;
                }
                new_track_num = (int16_t) val;
                break;
            }
            case 's': {
                long val = strtol ( optarg, &endp, 10 );
                if ( *endp != '\0' || val < 0 || val > 1 ) {
                    fprintf ( stderr, "Error: Invalid side value '%s' (must be 0 or 1)\n", optarg );
                    return EXIT_FAILURE;
                }
                new_side = (int16_t) val;
                break;
            }
            case 'F': {
                /* --fdc-status vyžaduje 3 argumenty: IDX STS1 STS2.
                 * getopt_long podporuje jen 0 nebo 1 argument na volbu,
                 * proto první (IDX) bereme přes optarg a další dva
                 * ručně z argv[optind]. Viz audit L-9 - tento pattern
                 * je nutný kvůli 3 argumentům, alternativa by byla
                 * `IDX:STS1:STS2` v jednom řetězci (breaking change). */
                errno = 0;
                long idx_val = strtol ( optarg, &endp, 10 );
                if ( *endp != '\0' || idx_val < 0 || idx_val > DSK_MAX_SECTORS - 1 || errno == ERANGE ) {
                    fprintf ( stderr, "Error: Invalid sector index '%s'\n", optarg );
                    return EXIT_FAILURE;
                }
                fdc_sector_idx = (int) idx_val;

                /* Další dva argumenty */
                if ( optind >= argc || optind + 1 >= argc ) {
                    fprintf ( stderr, "Error: --fdc-status requires 3 arguments: IDX STS1 STS2\n" );
                    return EXIT_FAILURE;
                }

                errno = 0;
                unsigned long sts1_val = strtoul ( argv[optind], &endp, 16 );
                if ( *endp != '\0' || sts1_val > 0xFF || errno == ERANGE ) {
                    fprintf ( stderr, "Error: Invalid STS1 value '%s'\n", argv[optind] );
                    return EXIT_FAILURE;
                }
                fdc_sts1 = (uint8_t) sts1_val;
                optind++;

                errno = 0;
                unsigned long sts2_val = strtoul ( argv[optind], &endp, 16 );
                if ( *endp != '\0' || sts2_val > 0xFF || errno == ERANGE ) {
                    fprintf ( stderr, "Error: Invalid STS2 value '%s'\n", argv[optind] );
                    return EXIT_FAILURE;
                }
                fdc_sts2 = (uint8_t) sts2_val;
                optind++;
                break;
            }
            case 'I': {
                /* --sector-ids MAP: čárkově oddělená sektorová ID.
                 * Pro konzistenci s --filler / --gap akceptujeme
                 * hexadecimální prefix "0x"/"0X" (audit M-22).
                 * Dec bez prefixu zůstává výchozí - nepřepínáme base=0,
                 * to by aktivovalo i oktály ("010" by dalo 8). */
                sector_ids_count = 0;
                const char *p = optarg;
                while ( *p != '\0' && sector_ids_count < DSK_MAX_SECTORS ) {
                    while ( *p == ' ' ) p++;
                    if ( *p == '\0' ) break;
                    char *ep;
                    int base = 10;
                    if ( p[0] == '0' && ( p[1] == 'x' || p[1] == 'X' ) ) {
                        base = 16;
                    }
                    long val = strtol ( p, &ep, base );
                    if ( ep == p || val < 0 || val > 255 ) {
                        fprintf ( stderr, "Error: Invalid sector ID in map '%s'\n", optarg );
                        return EXIT_FAILURE;
                    }
                    sector_ids_map[sector_ids_count++] = (uint8_t) val;
                    p = ep;
                    while ( *p == ' ' ) p++;
                    if ( *p == ',' ) p++;
                }
                if ( sector_ids_count == 0 ) {
                    fprintf ( stderr, "Error: Empty sector ID map\n" );
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'i': {
                /* --sector-id IDX:ID (např. "2:5") */
                if ( single_edit_count >= DSK_MAX_SECTORS ) {
                    fprintf ( stderr, "Error: Too many --sector-id options (max %d)\n", DSK_MAX_SECTORS );
                    return EXIT_FAILURE;
                }
                char *colon = strchr ( optarg, ':' );
                if ( !colon ) {
                    fprintf ( stderr, "Error: Invalid --sector-id format '%s' (expected IDX:ID)\n", optarg );
                    return EXIT_FAILURE;
                }
                *colon = '\0';
                long idx = strtol ( optarg, &endp, 10 );
                if ( *endp != '\0' || idx < 0 || idx > DSK_MAX_SECTORS - 1 ) {
                    fprintf ( stderr, "Error: Invalid sector index '%s'\n", optarg );
                    return EXIT_FAILURE;
                }
                long id = strtol ( colon + 1, &endp, 10 );
                if ( *endp != '\0' || id < 0 || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid sector ID '%s'\n", colon + 1 );
                    return EXIT_FAILURE;
                }
                single_edits[single_edit_count][0] = (int) idx;
                single_edits[single_edit_count][1] = (int) id;
                single_edit_count++;
                break;
            }
            default:
                fprintf ( stderr, "Usage: edit-track ABSTRACK [--gap HH] [--filler HH] [--track-num N] [--side N] [--fdc-status IDX STS1 STS2] [--sector-ids MAP] [--sector-id IDX:ID]\n" );
                return EXIT_FAILURE;
        }
    }

    /* Editace track headeru (gap, filler, track-num, side).
       Sentinel -1 znamená "neměnit"; všechny hodnoty 0-255 jsou validní
       včetně 0xFF. */
    if ( new_track_num >= 0 || new_side >= 0 || new_gap >= 0 || new_filler >= 0 ) {
        if ( EXIT_SUCCESS != dsk_tools_set_track_header ( h, abstrack, new_track_num, new_side, new_gap, new_filler ) ) {
            fprintf ( stderr, "Error: Cannot modify track header for abstrack %d\n", abstrack );
            return EXIT_FAILURE;
        }
        printf ( "Track %d header modified.\n", abstrack );
    }

    /* Editace FDC statusu */
    if ( fdc_sector_idx >= 0 ) {
        if ( EXIT_SUCCESS != dsk_tools_set_sector_fdc_status ( h, abstrack, (uint8_t) fdc_sector_idx, fdc_sts1, fdc_sts2 ) ) {
            fprintf ( stderr, "Error: Cannot set FDC status for abstrack %d sector %d\n", abstrack, fdc_sector_idx );
            return EXIT_FAILURE;
        }
        printf ( "Track %d sector %d FDC status set to STS1=0x%02x STS2=0x%02x\n",
            abstrack, fdc_sector_idx, fdc_sts1, fdc_sts2 );
    }

    /* Editace sector IDs - hromadná (--sector-ids) */
    if ( sector_ids_count > 0 ) {
        if ( EXIT_SUCCESS != dsk_tools_set_sector_ids ( h, abstrack, sector_ids_map, (uint8_t) sector_ids_count ) ) {
            /* FINDING-DSK-001: původní hláška "expected N sectors" kde N byl
               počet DODANÝCH ID. Informativnější varianta: přečíst skutečný
               počet sektorů stopy a jasně ho odlišit od poskytnutého počtu. */
            st_DSK_TRACK_HEADER_INFO thi;
            if ( dsk_tools_read_track_header_info ( h, abstrack, &thi ) == EXIT_SUCCESS ) {
                fprintf ( stderr,
                    "Error: Cannot set sector IDs for track %d: track has %d sectors, but %d ID%s provided\n",
                    abstrack, thi.sectors, sector_ids_count,
                    sector_ids_count == 1 ? " was" : "s were" );
            } else {
                fprintf ( stderr,
                    "Error: Cannot set sector IDs for track %d (%d ID%s provided; track header unreadable)\n",
                    abstrack, sector_ids_count,
                    sector_ids_count == 1 ? "" : "s" );
            }
            return EXIT_FAILURE;
        }
        printf ( "Track %d: %d sector IDs set.\n", abstrack, sector_ids_count );
    }

    /* Editace sector IDs - jednotlivé (--sector-id) */
    for ( int i = 0; i < single_edit_count; i++ ) {
        uint8_t s_idx = (uint8_t) single_edits[i][0];
        uint8_t s_id = (uint8_t) single_edits[i][1];
        if ( EXIT_SUCCESS != dsk_tools_set_sector_id ( h, abstrack, s_idx, s_id ) ) {
            fprintf ( stderr, "Error: Cannot set sector ID at index %d for abstrack %d\n",
                      s_idx, abstrack );
            return EXIT_FAILURE;
        }
        printf ( "Track %d sector index %d: ID set to %d.\n", abstrack, s_idx, s_id );
    }

    if ( new_track_num < 0 && new_side < 0 && new_gap < 0 && new_filler < 0
         && fdc_sector_idx < 0 && sector_ids_count == 0 && single_edit_count == 0 ) {
        fprintf ( stderr, "Warning: No modifications specified\n" );
    }

    return EXIT_SUCCESS;
}


/* =========================================================================
 * Nápověda a verze
 * ========================================================================= */


/**
 * @brief Vypíše nápovědu.
 *
 * @param[in] out      Výstupní stream (stdout pro --help, stderr pro chybové cesty).
 * @param[in] progname Jméno programu (argv[0]).
 */
static void print_usage ( FILE *out, const char *progname ) {
    fprintf ( out,
        "Usage: %s [global-options] <dsk_file> [global-options] <subcommand> [args...]\n"
        "\n"
        "Subcommands:\n"
        "  info                                  Show DSK header info\n"
        "  tracks [--abstrack T]                 Show track headers\n"
        "  check                                 Diagnose DSK (read-only)\n"
        "  repair                                Diagnose and repair DSK\n"
        "  edit-header --creator TEXT             Edit DSK header\n"
        "  edit-track T [options]                Edit track header\n"
        "    --gap HH                            Set GAP#3 (hex)\n"
        "    --filler HH                         Set filler byte (hex)\n"
        "    --track-num N                       Set track number\n"
        "    --side N                            Set side (0 or 1)\n"
        "    --fdc-status IDX STS1 STS2          Set FDC status for sector\n"
        "    --sector-ids ID,ID,...              Set all sector IDs at once\n"
        "    --sector-id IDX:ID                  Set single sector ID (repeatable)\n"
        "\n"
        "Global options:\n"
        "  --version                             Show version\n"
        "  --lib-versions                        Show library versions\n"
        "  --help                                Show this help\n",
        progname
    );
}


/**
 * @brief Vypíše verzi nástroje.
 */
static void print_version ( void ) {
    printf ( "mzdsk-dsk %s (%s %s)\n",
             MZDSK_DSK_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
}


/**
 * @brief Vypíše verze všech použitých knihoven.
 */
static void print_lib_versions ( void ) {
    printf ( "mzdsk-dsk %s (%s %s)\n\n",
             MZDSK_DSK_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
    printf ( "Library versions:\n" );
    printf ( "  dsk              %s\n", dsk_version() );
    printf ( "  dsk_tools        %s\n", dsk_tools_version() );
    printf ( "  generic_driver   %s\n", generic_driver_version() );
}
