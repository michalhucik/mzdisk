/**
 * @file   mzdsk_create.c
 * @brief  CLI nástroj pro vytváření DSK diskových obrazů Sharp MZ.
 *
 * Nástroj umožňuje vytvořit nový DSK obraz buď z předdefinovaného presetu,
 * s formátováním pro MZ-BASIC / CP/M SD / CP/M HD / MRS, nebo s libovolnou
 * uživatelskou geometrií (custom).
 *
 * Podporované příkazy:
 * - --preset NAME       - vytvoří DSK z presetu (basic, cpm-sd, cpm-hd, mrs, lemmings)
 * - --format-basic T    - vytvoří a zformátuje MZ-BASIC disk s T stopami
 * - --format-cpm T      - vytvoří a zformátuje CP/M SD disk s T stopami
 * - --format-cpmhd T    - vytvoří a zformátuje CP/M HD disk s T stopami
 * - --format-mrs T      - vytvoří a zformátuje MRS disk s T stopami
 * - --custom T S SS F [O] - vytvoří disk s vlastní geometrií
 *
 * Globální volby:
 * - --overwrite         - přepsat existující soubor
 * - --sides N           - počet stran (1 nebo 2, výchozí 2)
 * - --version           - vypsat verzi programu
 * - --lib-versions      - vypsat verze knihoven
 *
 * Výstup programu je v angličtině (pro budoucí lokalizaci).
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
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>

#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "tools/common/mzdisk_cli_version.h"


/** @brief Verze nástroje mzdsk-create. */
#define MZDSK_CREATE_VERSION "1.2.5"


/**
 * @brief Název programu pro výpisy.
 */
static const char *g_program_name = "mzdsk-create";


/* ====================================================================
 *  Pomocné funkce
 * ==================================================================== */


/**
 * @brief Zkontroluje, zda řetězec je platné desítkové číslo.
 *
 * @param str Řetězec k ověření. Nesmí být NULL.
 * @return EXIT_SUCCESS pokud je řetězec platné desítkové číslo.
 * @return EXIT_FAILURE pokud není.
 */
static int str_is_decimal ( const char *str ) {
    if ( !str || !*str ) return EXIT_FAILURE;
    for ( int i = 0; str[i]; i++ ) {
        if ( !isdigit ( (unsigned char) str[i] ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Zkontroluje, zda řetězec je platné hexadecimální číslo (s prefixem 0x/0X).
 *
 * @param str Řetězec k ověření. Nesmí být NULL.
 * @return EXIT_SUCCESS pokud je řetězec platné hexadecimální číslo s prefixem 0x.
 * @return EXIT_FAILURE pokud není.
 */
static int str_is_hex ( const char *str ) {
    if ( !str || strlen ( str ) < 3 ) return EXIT_FAILURE;
    if ( str[0] != '0' || ( str[1] != 'x' && str[1] != 'X' ) ) return EXIT_FAILURE;
    for ( int i = 2; str[i]; i++ ) {
        if ( !isxdigit ( (unsigned char) str[i] ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Parsuje číselný řetězec (desítkový nebo hexadecimální s 0x prefixem).
 *
 * Podporuje zápis v desítkové soustavě (např. "42") i v šestnáctkové
 * s prefixem 0x (např. "0xff").
 *
 * @param str Řetězec s číslem. Nesmí být NULL.
 * @param retcode Výstup: EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *                Nesmí být NULL.
 * @return Parsovaná hodnota čísla. Při chybě vrací 0 a nastaví *retcode = EXIT_FAILURE.
 *
 * @post Při neúspěchu se na stderr vypíše chybová hláška.
 */
static unsigned long parse_number ( const char *str, int *retcode ) {
    *retcode = EXIT_SUCCESS;
    int base = 0;
    if ( EXIT_SUCCESS == str_is_decimal ( str ) ) {
        base = 10;
    } else if ( EXIT_SUCCESS == str_is_hex ( str ) ) {
        base = 16;
    } else {
        fprintf ( stderr, "Error: '%s' is not a valid decimal or hex number.\n", str );
        *retcode = EXIT_FAILURE;
        return 0;
    }

    /* Audit M-15: errno=0 + ERANGE kontrola zachytí přetečení
       (999999999999999999 → ULONG_MAX tiše). */
    errno = 0;
    unsigned long val = strtoul ( str, NULL, base );
    if ( errno == ERANGE ) {
        fprintf ( stderr, "Error: Value '%s' is out of range.\n", str );
        *retcode = EXIT_FAILURE;
        return 0;
    }
    return val;
}


/**
 * @brief Vypíše sjednocenou chybovou hlášku o neplatném rozsahu celkového
 *        počtu stop u --format-* příkazů.
 *
 * --format-* přijímá tracks-per-side; celkový počet je tracks × sides.
 * Hláška uvádí obě hodnoty (per-side i celkem) a očekávaný rozsah, aby
 * uživatel viděl přesně, co selhalo.
 *
 * @param per_side Uživatelem zadaný počet stop na stranu.
 * @param sides    Počet stran (1 nebo 2).
 * @param min_total Minimální přípustný celkový počet stop.
 */
static void err_format_track_range ( unsigned long per_side, uint8_t sides,
                                      int min_total ) {
    fprintf ( stderr,
              "Error: track count invalid (tracks-per-side=%lu, sides=%u, "
              "total=%lu): must be in %d-%d total.\n",
              per_side, sides, per_side * sides,
              min_total, DSK_MAX_TOTAL_TRACKS );
}


/**
 * @brief Vypíše sjednocenou chybovou hlášku o neplatném rozsahu celkového
 *        počtu stop u --custom příkazu.
 *
 * --custom přijímá přímo celkový počet stop.
 *
 * @param total Uživatelem zadaný celkový počet stop.
 */
static void err_custom_track_range ( unsigned long total ) {
    fprintf ( stderr,
              "Error: track count invalid (total=%lu): must be in 1-%d total.\n",
              total, DSK_MAX_TOTAL_TRACKS );
}


/**
 * @brief Ověří unikátnost všech ID v mapě sektorů.
 *
 * @param sector_map Pole ID sektorů. Nesmí být NULL.
 * @param size Počet prvků v poli.
 * @return EXIT_SUCCESS pokud jsou všechna ID unikátní.
 * @return EXIT_FAILURE pokud existuje duplikát (chyba se vypíše na stderr).
 */
static int check_sector_map ( const uint8_t *sector_map, uint8_t size ) {
    if ( size <= 1 ) return EXIT_SUCCESS;
    for ( int i = 0; i < size - 1; i++ ) {
        for ( int j = i + 1; j < size; j++ ) {
            if ( sector_map[i] == sector_map[j] ) {
                fprintf ( stderr, "Error: invalid sector map - ID 0x%02X (%d) is not unique.\n",
                          sector_map[i], sector_map[i] );
                return EXIT_FAILURE;
            }
        }
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Parsuje textový zápis mapy sektorů (čísla oddělená čárkami).
 *
 * Formát vstupu: "1,2,3,4" nebo "1, 5, 3, 7" apod.
 * Podporuje desítková i hexadecimální čísla (0x prefix).
 * Platné ID sektorů jsou 1-255.
 *
 * @param text Textový zápis mapy sektorů. Nesmí být NULL.
 * @param sector_map Výstupní pole ID sektorů (min DSK_MAX_SECTORS prvků). Nesmí být NULL.
 * @param retcode Výstup: EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *                Nesmí být NULL.
 * @return Počet parsovaných ID sektorů. Při chybě vrací 0.
 *
 * @post Při neúspěchu se na stderr vypíše chybová hláška.
 */
static int parse_sector_map ( const char *text, uint8_t *sector_map, int *retcode ) {
    int count = 0;
    int length = (int) strlen ( text );

    for ( int i = 0; i < length; i++ ) {
        if ( isdigit ( (unsigned char) text[i] ) ) {
            /* Najdi konec čísla */
            int j;
            for ( j = i; j < length; j++ ) {
                if ( !( isxdigit ( (unsigned char) text[j] ) || text[j] == 'x' || text[j] == 'X' ) ) {
                    break;
                }
            }

            int num_len = j - i;
            char *buf = malloc ( (size_t) num_len + 1 );
            if ( !buf ) {
                fprintf ( stderr, "Error: memory allocation failed.\n" );
                *retcode = EXIT_FAILURE;
                return 0;
            }
            memcpy ( buf, &text[i], (size_t) num_len );
            buf[num_len] = '\0';
            i = j;

            unsigned long sector_id = parse_number ( buf, retcode );
            free ( buf );
            if ( EXIT_SUCCESS != *retcode ) return 0;

            if ( sector_id < 1 || sector_id > 255 ) {
                fprintf ( stderr, "Error: invalid sector ID %lu. Value must be 1-255.\n", sector_id );
                *retcode = EXIT_FAILURE;
                return 0;
            }

            if ( count >= DSK_MAX_SECTORS ) {
                fprintf ( stderr, "Error: sector map has more than %d IDs.\n", DSK_MAX_SECTORS );
                *retcode = EXIT_FAILURE;
                return 0;
            }

            sector_map[count++] = (uint8_t) sector_id;

        } else if ( text[i] != ' ' && text[i] != ',' ) {
            fprintf ( stderr, "Error: invalid character '%c' in sector map.\n", text[i] );
            *retcode = EXIT_FAILURE;
            return 0;
        }
    }

    /* Prázdný vstup ("", "  ", ",,,") - caller by dostal count=0 a
     * tichou prázdnou mapu. Odmítáme explicitně (audit L-7). */
    if ( count == 0 ) {
        fprintf ( stderr, "Error: sector map is empty (no IDs parsed).\n" );
        *retcode = EXIT_FAILURE;
        return 0;
    }

    *retcode = check_sector_map ( sector_map, (uint8_t) count );
    return count;
}


/**
 * @brief Zkontroluje, zda soubor existuje.
 *
 * @param filename Cesta k souboru. Nesmí být NULL.
 * @return 1 pokud soubor existuje, 0 pokud ne.
 */
static int file_exists ( const char *filename ) {
    FILE *f = fopen ( filename, "rb" );
    if ( f ) {
        fclose ( f );
        return 1;
    }
    return 0;
}


/* ====================================================================
 *  Výpis nápovědy
 * ==================================================================== */


/**
 * @brief Vypíše nápovědu programu na stdout.
 *
 * Zobrazí kompletní přehled všech podporovaných příkazů a voleb
 * včetně příkladů použití.
 *
 * @param[in] out Výstupní stream (stdout pro --help, stderr pro chybové cesty).
 *
 * @post Na zadaný stream se vypíše formátovaná nápověda.
 */
static void print_usage ( FILE *out ) {
    fprintf ( out, "\n"
             "Usage:\n"
             "  %s <dsk_file> --preset <name> [--sides N] [--overwrite]\n"
             "  %s <dsk_file> --format-basic <tracks> [--sides N] [--overwrite]\n"
             "  %s <dsk_file> --format-cpm <tracks> [--sides N] [--overwrite]\n"
             "  %s <dsk_file> --format-cpmhd <tracks> [--sides N] [--overwrite]\n"
             "  %s <dsk_file> --format-mrs <tracks> [--sides N] [--overwrite]\n"
             "  %s <dsk_file> --custom <tracks> <sectors> <ssize> <filler> [order] [--sides N] [--overwrite]\n"
             "  %s --version\n"
             "  %s --lib-versions\n"
             "\n"
             "Commands:\n"
             "  --preset <name>       Create empty DSK with preset geometry (no FS init).\n"
             "                        Presets: basic, cpm-sd, cpm-hd, mrs, lemmings\n"
             "                        Note: presets only set disk geometry and sector layout.\n"
             "                        Use --format-* commands to create a ready-to-use disk\n"
             "                        with initialized filesystem structures.\n"
             "\n"
             "  --format-basic <T>    Create and format MZ-BASIC disk with T tracks per side.\n"
             "                        Total tracks = T x sides.\n"
             "  --format-cpm <T>      Create and format CP/M SD disk with T tracks per side.\n"
             "                        Total tracks = T x sides.\n"
             "  --format-cpmhd <T>    Create and format CP/M HD disk with T tracks per side.\n"
             "                        Total tracks = T x sides.\n"
             "  --format-mrs <T>      Create and format MRS disk with T tracks per side.\n"
             "                        Total tracks = T x sides.\n"
             "\n"
             "  --custom <T> <S> <SS> <F> [O]\n"
             "                        Create DSK with custom geometry.\n"
             "                        T  = total tracks across all sides (1-%d)\n"
             "                        S  = sectors per track (1-%d)\n"
             "                        SS = sector size (128, 256, 512, 1024)\n"
             "                        F  = filler byte (0-255)\n"
             "                        O  = sector order (optional):\n"
             "                             normal - sequential (1,2,3,...)\n"
             "                             lec    - interlaced with interval 2\n"
             "                             lechd  - interlaced with interval 3\n"
             "                             or comma-separated sector map (e.g. 1,3,5,2,4,6)\n"
             "\n"
             "Options:\n"
             "  --overwrite           Overwrite existing file.\n"
             "  --sides N             Number of sides (1 or 2, default 2).\n"
             "  --version             Print version and exit.\n"
             "  --lib-versions        Print library versions and exit.\n"
             "\n",
             g_program_name, g_program_name, g_program_name, g_program_name,
             g_program_name, g_program_name, g_program_name, g_program_name,
             DSK_MAX_TOTAL_TRACKS, DSK_MAX_SECTORS );
}


/**
 * @brief Vypíše verzi programu na stdout.
 *
 * @post Na stdout se vypíše řetězec s verzí.
 */
static void print_version ( void ) {
    printf ( "%s %s (%s %s)\n",
             g_program_name, MZDSK_CREATE_VERSION,
             MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
}


/**
 * @brief Vypíše verze všech použitých knihoven na stdout.
 *
 * Zobrazí verzi programu samotného a verze knihoven:
 * dsk, mzdsk_global, mzdsk_ipldisk, mzdsk_tools, generic_driver.
 *
 * @post Na stdout se vypíší řetězce s verzemi knihoven.
 */
static void print_lib_versions ( void ) {
    printf ( "%s %s (%s %s)\n\n",
             g_program_name, MZDSK_CREATE_VERSION,
             MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
    printf ( "Library versions:\n" );
    printf ( "  dsk:             %s\n", dsk_version () );
    printf ( "  mzdsk_global:    %s\n", mzdsk_global_version () );
    printf ( "  mzdsk_ipldisk:   %s\n", mzdsk_ipldisk_version () );
    printf ( "  mzdsk_mrs:       %s\n", mzdsk_mrs_version () );
    printf ( "  mzdsk_tools:     %s\n", mzdsk_tools_version () );
    printf ( "  generic_driver:  %s\n", generic_driver_version () );
}


/* ====================================================================
 *  Otevření výstupního handleru
 * ==================================================================== */


/**
 * @brief Otevře výstupní soubor pro zápis přes generic_driver.
 *
 * Vytvoří a inicializuje handler a driver, poté otevře soubor
 * v režimu FILE_DRIVER_OPMODE_W (vytvoření/přepsání).
 *
 * @param handler Ukazatel na handler. Nesmí být NULL.
 * @param driver Ukazatel na driver. Nesmí být NULL.
 * @param filename Cesta k výstupnímu souboru. Nesmí být NULL.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @post Při úspěchu je handler otevřený a připravený pro zápis.
 * @post Při neúspěchu se na stderr vypíše chybová hláška.
 *
 * @note Volající je zodpovědný za zavolání generic_driver_close()
 *       po ukončení práce s handlerem.
 */
static int open_output_handler ( st_HANDLER *handler, st_DRIVER *driver, char *filename ) {

    generic_driver_file_init ( driver );

    if ( !generic_driver_open_file ( handler, driver, filename, FILE_DRIVER_OPMODE_W ) ) {
        fprintf ( stderr, "Error: cannot create file '%s': %s\n",
                  filename, generic_driver_error_message ( handler, driver ) );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Příkaz --preset
 * ==================================================================== */


/**
 * @brief Převede textový název presetu na enum en_MZDSK_PRESET.
 *
 * Porovnávání je case-insensitive. Podporované názvy:
 * "basic", "cpm-sd", "cpm-hd", "mrs", "lemmings".
 *
 * @param name Textový název presetu. Nesmí být NULL.
 * @param preset Výstup: odpovídající preset enum. Nesmí být NULL.
 * @return EXIT_SUCCESS pokud je název platný preset.
 * @return EXIT_FAILURE pokud název neodpovídá žádnému presetu.
 *
 * @post Při neúspěchu se na stderr vypíše chybová hláška se seznamem
 *       platných presetů.
 */
static int resolve_preset_name ( const char *name, en_MZDSK_PRESET *preset ) {
    if ( 0 == strcasecmp ( name, "basic" ) ) {
        *preset = MZDSK_PRESET_BASIC;
    } else if ( 0 == strcasecmp ( name, "cpm-sd" ) ) {
        *preset = MZDSK_PRESET_CPM_SD;
    } else if ( 0 == strcasecmp ( name, "cpm-hd" ) ) {
        *preset = MZDSK_PRESET_CPM_HD;
    } else if ( 0 == strcasecmp ( name, "mrs" ) ) {
        *preset = MZDSK_PRESET_MRS;
    } else if ( 0 == strcasecmp ( name, "lemmings" ) ) {
        *preset = MZDSK_PRESET_LEMMINGS;
    } else {
        fprintf ( stderr, "Error: unknown preset '%s'.\n"
                  "Valid presets: basic, cpm-sd, cpm-hd, mrs, lemmings\n", name );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Provede příkaz --preset: vytvoří DSK obraz z předdefinovaného presetu.
 *
 * Otevře výstupní soubor, zavolá mzdsk_tools_create_from_preset() a zavře handler.
 *
 * @param filename Cesta k výstupnímu DSK souboru. Nesmí být NULL.
 * @param preset_name Textový název presetu (case-insensitive). Nesmí být NULL.
 * @param sides Počet stran (1 nebo 2).
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @post Při úspěchu vznikne nový DSK soubor s obsahem podle presetu.
 * @post Na stdout se vypíší informace o vytvořeném obrazu.
 */
static int cmd_preset ( char *filename, const char *preset_name, uint8_t sides ) {

    en_MZDSK_PRESET preset;
    if ( EXIT_SUCCESS != resolve_preset_name ( preset_name, &preset ) ) {
        return EXIT_FAILURE;
    }

    st_HANDLER handler;
    st_DRIVER driver;

    if ( EXIT_SUCCESS != open_output_handler ( &handler, &driver, filename ) ) {
        return EXIT_FAILURE;
    }

    printf ( "Creating DSK from preset '%s' (%s, %d side%s) -> '%s'...\n",
             preset_name, mzdsk_tools_preset_name ( preset ),
             sides, ( sides > 1 ) ? "s" : "", filename );

    int ret = mzdsk_tools_create_from_preset ( &handler, preset, sides );

    generic_driver_close ( &handler );

    if ( EXIT_SUCCESS != ret ) {
        fprintf ( stderr, "Error: failed to create DSK from preset '%s'.\n", preset_name );
        return EXIT_FAILURE;
    }

    printf ( "Done.\n" );
    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Příkaz --format-basic
 * ==================================================================== */


/**
 * @brief Provede příkaz --format-basic: vytvoří a zformátuje MZ-BASIC disk.
 *
 * Otevře výstupní soubor, zavolá mzdsk_tools_format_basic() s danými parametry
 * a zavře handler. Výsledný obraz obsahuje inicializovaný souborový systém FSMZ.
 *
 * @param filename Cesta k výstupnímu DSK souboru. Nesmí být NULL.
 * @param tracks Počet stop na jednu stranu. Musí být >= 2.
 *               Celkový počet stop na disku = tracks * sides.
 * @param sides Počet stran (1 nebo 2).
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @post Při úspěchu vznikne nový DSK soubor se zformátovaným MZ-BASIC diskem.
 */
static int cmd_format_basic ( char *filename, unsigned int tracks, uint8_t sides ) {

    st_HANDLER handler;
    st_DRIVER driver;

    if ( EXIT_SUCCESS != open_output_handler ( &handler, &driver, filename ) ) {
        return EXIT_FAILURE;
    }

    printf ( "Creating formatted MZ-BASIC disk (%u tracks/side, %u side(s), %u total) -> '%s'...\n",
             tracks, sides, tracks * sides, filename );

    /* tracks = počet stop na stranu, sides = počet stran */
    int ret = mzdsk_tools_format_basic ( &handler, (uint8_t) tracks, sides );

    generic_driver_close ( &handler );

    if ( EXIT_SUCCESS != ret ) {
        fprintf ( stderr, "Error: failed to create formatted MZ-BASIC disk.\n" );
        return EXIT_FAILURE;
    }

    printf ( "Done.\n" );
    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Varování o nevyužitých stopách u CP/M formátů
 * ==================================================================== */


/**
 * @brief Vypočítá minimální počet stop potřebných pro CP/M formát a vypíše varování.
 *
 * Z DPB parametrů (dsm, block_size, spt, off) spočítá, kolik stop CP/M formát
 * skutečně využije. Pokud zadaný počet stop (tracks) překračuje potřebný počet,
 * vypíše na stderr varování s informací o nevyužitých stopách.
 *
 * Výpočet:
 * - Celková datová kapacita = (dsm + 1) * block_size
 * - Kapacita jedné datové stopy = (spt / (512 / 128)) * 512 = spt * 128
 * - Počet datových stop = ceil(datová_kapacita / kapacita_stopy)
 * - Potřebné celkové stopy = datové stopy + off
 *
 * @param format CP/M formát pro inicializaci DPB.
 * @param tracks Zadaný celkový počet stop.
 * @param spt_physical Počet fyzických sektorů na stopu (9 pro SD, 18 pro HD).
 * @param sector_size Velikost fyzického sektoru v bajtech (512).
 *
 * @post Pokud tracks > potřebné stopy, vypíše varování o nevyužitých stopách.
 *       Pokud tracks < potřebné stopy, vypíše varování, že DPB bude adresovat
 *       neexistující sektory.
 */
static void warn_cpm_unused_tracks ( en_MZDSK_CPM_FORMAT format, unsigned int tracks,
                                      unsigned int spt_physical, unsigned int sector_size ) {
    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, format );

    /* Celková datová kapacita CP/M formátu v bajtech */
    unsigned long data_capacity = (unsigned long) ( dpb.dsm + 1 ) * dpb.block_size;

    /* Kapacita jedné datové stopy v bajtech */
    unsigned long track_capacity = (unsigned long) spt_physical * sector_size;

    /* Počet datových stop (zaokrouhlení nahoru) */
    unsigned int data_tracks = (unsigned int) ( ( data_capacity + track_capacity - 1 ) / track_capacity );

    /* Celkový potřebný počet stop (datové + rezervované) */
    unsigned int needed_tracks = data_tracks + dpb.off;

    if ( tracks > needed_tracks ) {
        fprintf ( stderr, "Warning: CP/M format uses only %u tracks, %u track(s) will be unused.\n",
                  needed_tracks, tracks - needed_tracks );
    } else if ( tracks < needed_tracks ) {
        fprintf ( stderr, "Warning: CP/M DPB expects %u tracks but disk has only %u."
                  " DPB will address non-existent sectors.\n",
                  needed_tracks, tracks );
    }
}


/* ====================================================================
 *  Příkaz --format-cpm
 * ==================================================================== */


/**
 * @brief Provede příkaz --format-cpm: vytvoří a zformátuje CP/M SD disk.
 *
 * Otevře výstupní soubor, zavolá mzdsk_tools_format_cpm_sd() s daným počtem
 * stop a zavře handler. Pokud zadaný počet stop přesahuje kapacitu CP/M SD
 * formátu, vypíše varování na stderr.
 *
 * @param filename Cesta k výstupnímu DSK souboru. Nesmí být NULL.
 * @param tracks Celkový počet stop. Musí být >= 3.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @post Při úspěchu vznikne nový DSK soubor se zformátovaným CP/M SD diskem.
 */
static int cmd_format_cpm ( char *filename, unsigned int tracks, uint8_t sides ) {

    /* tracks = per-side; vnitřní API vyžaduje total_tracks */
    unsigned int total_tracks = tracks * sides;

    /* Varování pokud disk bude mít nevyužité stopy */
    warn_cpm_unused_tracks ( MZDSK_CPM_FORMAT_SD, total_tracks, 9, 512 );

    st_HANDLER handler;
    st_DRIVER driver;

    if ( EXIT_SUCCESS != open_output_handler ( &handler, &driver, filename ) ) {
        return EXIT_FAILURE;
    }

    printf ( "Creating formatted CP/M SD disk (%u tracks/side, %d side%s, %u total) -> '%s'...\n",
             tracks, sides, ( sides > 1 ) ? "s" : "", total_tracks, filename );

    int ret = mzdsk_tools_format_cpm_sd ( &handler, (uint8_t) total_tracks, sides );

    generic_driver_close ( &handler );

    if ( EXIT_SUCCESS != ret ) {
        fprintf ( stderr, "Error: failed to create formatted CP/M SD disk.\n" );
        return EXIT_FAILURE;
    }

    printf ( "Done.\n" );
    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Příkaz --format-cpmhd
 * ==================================================================== */


/**
 * @brief Provede příkaz --format-cpmhd: vytvoří a zformátuje CP/M HD disk.
 *
 * Otevře výstupní soubor, zavolá mzdsk_tools_format_cpm_hd() s daným počtem
 * stop a zavře handler.
 *
 * @param filename Cesta k výstupnímu DSK souboru. Nesmí být NULL.
 * @param tracks Celkový počet stop. Musí být >= 3.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @post Při úspěchu vznikne nový DSK soubor se zformátovaným CP/M HD diskem.
 */
static int cmd_format_cpmhd ( char *filename, unsigned int tracks, uint8_t sides ) {

    /* tracks = per-side; vnitřní API vyžaduje total_tracks */
    unsigned int total_tracks = tracks * sides;

    /* Varování pokud disk bude mít nevyužité stopy */
    warn_cpm_unused_tracks ( MZDSK_CPM_FORMAT_HD, total_tracks, 18, 512 );

    st_HANDLER handler;
    st_DRIVER driver;

    if ( EXIT_SUCCESS != open_output_handler ( &handler, &driver, filename ) ) {
        return EXIT_FAILURE;
    }

    printf ( "Creating formatted CP/M HD disk (%u tracks/side, %d side%s, %u total) -> '%s'...\n",
             tracks, sides, ( sides > 1 ) ? "s" : "", total_tracks, filename );

    int ret = mzdsk_tools_format_cpm_hd ( &handler, (uint8_t) total_tracks, sides );

    generic_driver_close ( &handler );

    if ( EXIT_SUCCESS != ret ) {
        fprintf ( stderr, "Error: failed to create formatted CP/M HD disk.\n" );
        return EXIT_FAILURE;
    }

    printf ( "Done.\n" );
    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Příkaz --format-mrs
 * ==================================================================== */


/** @brief Výchozí číslo bloku, kde začíná FAT na MRS disketě. */
#define FSMRS_DEFAULT_FAT_BLOCK 36

/**
 * @brief Minimální počet absolutních stop pro MRS formát.
 *
 * FAT začíná na bloku 36 a zabírá 9 bloků (3 FAT + 6 DIR).
 * Celkový počet bloků = total_tracks * 9 (sektorů na stopu).
 * Musí platit: fat_block + 9 <= total_tracks * 9, tedy
 * 36 + 9 = 45 bloků, 45 / 9 = 5 stop. Zaokrouhleno nahoru na
 * sudé číslo (MRS je vždy 2-stranný) = 6.
 */
#define MRS_MIN_TRACKS 6


/**
 * @brief Provede příkaz --format-mrs: vytvoří a zformátuje MRS disk.
 *
 * Postup:
 * 1. Vytvoří DSK obraz s MRS geometrií (3 pravidla: stopa 0 = 9x512B LEC,
 *    stopa 1 = 16x256B normální, stopy 2+ = 9x512B LEC; 2 strany).
 * 2. Otevře vytvořený DSK přes paměťový driver jako st_MZDSK_DISC.
 * 3. Přepne sector_info_cb na MRS variantu (fsmrs_sector_info_cb).
 * 4. Zavolá fsmrs_format_fs() pro inicializaci FAT a adresáře.
 * 5. Uloží a zavře disk.
 *
 * @param filename Cesta k výstupnímu DSK souboru. Nesmí být NULL.
 * @param tracks Celkový počet absolutních stop. Musí být sudé (2 strany)
 *               a >= MRS_MIN_TRACKS.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @pre filename ukazuje na platnou cestu, kam lze zapisovat.
 * @post Při úspěchu vznikne nový DSK soubor se zformátovaným MRS diskem
 *       (prázdná FAT a adresář na pozici fat_block=36).
 */
static int cmd_format_mrs ( char *filename, unsigned int tracks, uint8_t sides ) {

    st_HANDLER handler;
    st_DRIVER driver;

    if ( EXIT_SUCCESS != open_output_handler ( &handler, &driver, filename ) ) {
        return EXIT_FAILURE;
    }

    /* tracks = per-side; total = tracks * sides */
    unsigned int total_tracks = tracks * sides;
    printf ( "Creating formatted MRS disk (%u tracks/side, %d side%s, %u total) -> '%s'...\n",
             tracks, sides, ( sides > 1 ) ? "s" : "", total_tracks, filename );

    /*
     * Krok 1: Vytvoření prázdného DSK obrazu s MRS geometrií.
     *
     * MRS geometrie: 3 pravidla stop.
     * Stopa 0: 9x512 B, prokládané LEC, filler 0xE5.
     * Stopa 1: 16x256 B, normální řazení (boot track), filler 0xFF.
     * Stopy 2+: 9x512 B, prokládané LEC, filler 0xE5.
     */
    uint8_t tracks_per_side = (uint8_t) tracks;
    st_DSK_DESCRIPTION *desc = malloc ( dsk_tools_compute_description_size ( 3 ) );

    if ( !desc ) {
        fprintf ( stderr, "Error: memory allocation failed.\n" );
        generic_driver_close ( &handler );
        return EXIT_FAILURE;
    }

    desc->count_rules = 3;
    desc->sides = sides;
    desc->tracks = tracks_per_side;

    dsk_tools_assign_description ( desc, 0, 0,
        9, DSK_SECTOR_SIZE_512,
        DSK_SEC_ORDER_INTERLACED_LEC, NULL, 0xE5 );
    dsk_tools_assign_description ( desc, 1, 1,
        MZDSK_FSMZ_SECTORS_ON_TRACK, MZDSK_FSMZ_SECTOR_SSIZE,
        DSK_SEC_ORDER_NORMAL, NULL, 0xFF );
    dsk_tools_assign_description ( desc, 2, 2,
        9, DSK_SECTOR_SIZE_512,
        DSK_SEC_ORDER_INTERLACED_LEC, NULL, 0xE5 );

    int ret = dsk_tools_create_image ( &handler, desc );
    free ( desc );
    generic_driver_close ( &handler );

    if ( EXIT_SUCCESS != ret ) {
        fprintf ( stderr, "Error: failed to create MRS DSK image.\n" );
        return EXIT_FAILURE;
    }

    /*
     * Krok 2: Otevření vytvořeného DSK přes paměťový driver
     * a inicializace MRS souborového systému.
     */
    st_MZDSK_DISC disc;
    en_MZDSK_RES err = mzdsk_disc_open_memory ( &disc, filename, FILE_DRIVER_OPMODE_RW );
    if ( err != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: cannot reopen DSK for MRS formatting: %s\n",
                  mzdsk_get_error ( err ) );
        return EXIT_FAILURE;
    }

    /* Přepnutí sector_info_cb na MRS variantu - MRS má na stopě 1
       256B sektory a na ostatních stopách 512B sektory. */
    disc.sector_info_cb = fsmrs_sector_info_cb;
    disc.sector_info_cb_data = NULL;

    en_MZDSK_RES res = fsmrs_format_fs ( &disc, FSMRS_DEFAULT_FAT_BLOCK );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: MRS filesystem initialization failed: %s\n",
                  mzdsk_get_error ( res ) );
        mzdsk_disc_close ( &disc );
        return EXIT_FAILURE;
    }

    /* Uložení a zavření */
    res = mzdsk_disc_save ( &disc );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: cannot save DSK file: %s\n",
                  mzdsk_get_error ( res ) );
        mzdsk_disc_close ( &disc );
        return EXIT_FAILURE;
    }

    mzdsk_disc_close ( &disc );
    printf ( "Done.\n" );
    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Příkaz --custom
 * ==================================================================== */


/**
 * @brief Provede příkaz --custom: vytvoří DSK s uživatelskou geometrií.
 *
 * Parsuje parametry geometrie (stopy, sektory, velikost sektoru,
 * filler byte, řazení sektorů), sestaví st_DSK_DESCRIPTION a zavolá
 * dsk_tools_create_image() pro vytvoření obrazu.
 *
 * @param filename Cesta k výstupnímu DSK souboru. Nesmí být NULL.
 * @param total_tracks Celkový počet absolutních stop.
 * @param sectors Počet sektorů na stopě.
 * @param ssize Kódovaná velikost sektoru (en_DSK_SECTOR_SIZE).
 * @param filler Filler byte pro vyplnění sektorů (0-255).
 * @param secorder Typ řazení sektorů.
 * @param sector_map Mapa sektorů (pouze pro DSK_SEC_ORDER_CUSTOM, jinak NULL).
 * @param sides Počet stran (1 nebo 2).
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @pre Pokud secorder == DSK_SEC_ORDER_CUSTOM, sector_map musí obsahovat
 *      platnou mapu s sectors prvky.
 * @pre Pokud sides == 2, total_tracks musí být sudé.
 * @post Při úspěchu vznikne nový DSK soubor s požadovanou geometrií.
 */
static int cmd_custom ( char *filename, unsigned int total_tracks, uint8_t sectors,
                        en_DSK_SECTOR_SIZE ssize, uint8_t filler,
                        en_DSK_SECTOR_ORDER_TYPE secorder, uint8_t *sector_map,
                        uint8_t sides ) {

    st_HANDLER handler;
    st_DRIVER driver;

    if ( EXIT_SUCCESS != open_output_handler ( &handler, &driver, filename ) ) {
        return EXIT_FAILURE;
    }

    printf ( "Creating custom DSK (%u tracks, %u sectors, %u B/sector, filler 0x%02X, %u side(s)) -> '%s'...\n",
             total_tracks, sectors, dsk_decode_sector_size ( ssize ), filler, sides, filename );

    /* Sestavení popisu geometrie */
    st_DSK_DESCRIPTION *dskdesc = malloc ( dsk_tools_compute_description_size ( 1 ) );

    if ( !dskdesc ) {
        fprintf ( stderr, "Error: memory allocation failed.\n" );
        generic_driver_close ( &handler );
        return EXIT_FAILURE;
    }

    dskdesc->count_rules = 1;
    dskdesc->sides = sides;
    dskdesc->tracks = (uint8_t) ( total_tracks / sides );

    dsk_tools_assign_description ( dskdesc, 0, 0, sectors, ssize, secorder, sector_map, filler );

    int ret = dsk_tools_create_image ( &handler, dskdesc );
    free ( dskdesc );
    generic_driver_close ( &handler );

    if ( EXIT_SUCCESS != ret ) {
        fprintf ( stderr, "Error: failed to create custom DSK image.\n" );
        return EXIT_FAILURE;
    }

    printf ( "Done.\n" );
    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Hlavní funkce
 * ==================================================================== */


/**
 * @brief Vstupní bod programu mzdsk-create.
 *
 * Parsuje příkazovou řádku a deleguje na odpovídající příkaz.
 * Zpracovává globální volby (--help, --overwrite, --sides, --version, --lib-versions).
 * Volba --sides je podporována u všech příkazů včetně presetů.
 *
 * @param argc Počet argumentů příkazové řádky.
 * @param argv Pole řetězců argumentů příkazové řádky.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @post Návratový kód 0 = úspěch, 1 = chyba.
 */
int main ( int argc, char *argv[] ) {

    /* Inicializace globálních driverů pro paměťový handler.
       Vyžadováno pro --format-mrs (reopen DSK přes memory driver). */
    memory_driver_init ();

    /* Žádné argumenty - výpis nápovědy */
    if ( argc < 2 ) {
        print_usage ( stderr );
        return EXIT_FAILURE;
    }

    /* Definice dlouhých voleb pro getopt_long */
    static struct option long_options[] = {
        { "overwrite",    no_argument,       NULL, 'w' },
        { "sides",        required_argument, NULL, 'S' },
        { "preset",       required_argument, NULL, 'p' },
        { "format-basic", required_argument, NULL, 'b' },
        { "format-cpm",   required_argument, NULL, 'c' },
        { "format-cpmhd", required_argument, NULL, 'd' },
        { "format-mrs",   required_argument, NULL, 'r' },
        { "custom",       no_argument,       NULL, 'C' },
        { "version",      no_argument,       NULL, 'V' },
        { "lib-versions", no_argument,       NULL, 'L' },
        { "help",         no_argument,       NULL, 'H' },
        { NULL,           0,                 NULL,  0  }
    };

    /* Parsování voleb přes getopt_long */
    int overwrite = 0;
    uint8_t sides = 2;

    /* Typ zvoleného příkazu */
    enum {
        CREATE_NONE,
        CREATE_PRESET,
        CREATE_FORMAT_BASIC,
        CREATE_FORMAT_CPM,
        CREATE_FORMAT_CPMHD,
        CREATE_FORMAT_MRS,
        CREATE_CUSTOM
    } command = CREATE_NONE;

    const char *cmd_arg = NULL; /**< Argument příkazu (preset name nebo track count). */

    optind = 1;
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "", long_options, NULL ) ) != -1 ) {

        switch ( opt ) {

            case 'V': /* --version */
                print_version();
                return EXIT_SUCCESS;

            case 'L': /* --lib-versions */
                print_lib_versions();
                return EXIT_SUCCESS;

            case 'H': /* --help */
                print_usage ( stdout );
                return EXIT_SUCCESS;

            case 'w': /* --overwrite */
                overwrite = 1;
                break;

            case 'S': { /* --sides N */
                int rc;
                unsigned long val = parse_number ( optarg, &rc );
                if ( EXIT_SUCCESS != rc || ( val != 1 && val != 2 ) ) {
                    fprintf ( stderr, "Error: --sides must be 1 or 2.\n" );
                    return EXIT_FAILURE;
                }
                sides = (uint8_t) val;
                break;
            }

            case 'p': /* --preset <name> */
                if ( command != CREATE_NONE ) {
                    fprintf ( stderr, "Error: only one command can be specified.\n" );
                    return EXIT_FAILURE;
                }
                command = CREATE_PRESET;
                cmd_arg = optarg;
                break;

            case 'b': /* --format-basic <tracks> */
                if ( command != CREATE_NONE ) {
                    fprintf ( stderr, "Error: only one command can be specified.\n" );
                    return EXIT_FAILURE;
                }
                command = CREATE_FORMAT_BASIC;
                cmd_arg = optarg;
                break;

            case 'c': /* --format-cpm <tracks> */
                if ( command != CREATE_NONE ) {
                    fprintf ( stderr, "Error: only one command can be specified.\n" );
                    return EXIT_FAILURE;
                }
                command = CREATE_FORMAT_CPM;
                cmd_arg = optarg;
                break;

            case 'd': /* --format-cpmhd <tracks> */
                if ( command != CREATE_NONE ) {
                    fprintf ( stderr, "Error: only one command can be specified.\n" );
                    return EXIT_FAILURE;
                }
                command = CREATE_FORMAT_CPMHD;
                cmd_arg = optarg;
                break;

            case 'r': /* --format-mrs <tracks> */
                if ( command != CREATE_NONE ) {
                    fprintf ( stderr, "Error: only one command can be specified.\n" );
                    return EXIT_FAILURE;
                }
                command = CREATE_FORMAT_MRS;
                cmd_arg = optarg;
                break;

            case 'C': /* --custom */
                if ( command != CREATE_NONE ) {
                    fprintf ( stderr, "Error: only one command can be specified.\n" );
                    return EXIT_FAILURE;
                }
                command = CREATE_CUSTOM;
                break;

            default: /* neznámá volba - getopt_long již vypsal chybu */
                print_usage ( stderr );
                return EXIT_FAILURE;
        }
    }

    /* Musí být zadán příkaz */
    if ( command == CREATE_NONE ) {
        fprintf ( stderr, "Error: no command specified.\n" );
        print_usage ( stderr );
        return EXIT_FAILURE;
    }

    /* DSK soubor je první poziční argument po volbách */
    if ( optind >= argc ) {
        fprintf ( stderr, "Error: output DSK file required.\n" );
        print_usage ( stderr );
        return EXIT_FAILURE;
    }

    char *dsk_file = argv[optind++];

    /* Kontrola přepsání existujícího souboru */
    if ( !overwrite && file_exists ( dsk_file ) ) {
        fprintf ( stderr, "Error: file '%s' already exists. Use --overwrite to replace it.\n", dsk_file );
        return EXIT_FAILURE;
    }

    /* Zpracování příkazů */
    switch ( command ) {

        case CREATE_PRESET:
            return cmd_preset ( dsk_file, cmd_arg, sides );

        case CREATE_FORMAT_BASIC: {
            int rc;
            unsigned long tracks = parse_number ( cmd_arg, &rc );
            if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;
            /* tracks = per-side; validujeme per-side i total */
            unsigned long total = tracks * sides;
            if ( tracks < 1 || total < 2 || total > DSK_MAX_TOTAL_TRACKS ) {
                err_format_track_range ( tracks, sides, 2 );
                return EXIT_FAILURE;
            }
            return cmd_format_basic ( dsk_file, (unsigned int) tracks, sides );
        }

        case CREATE_FORMAT_CPM: {
            int rc;
            unsigned long tracks = parse_number ( cmd_arg, &rc );
            if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;
            unsigned long total = tracks * sides;
            if ( tracks < 1 || total < 3 || total > DSK_MAX_TOTAL_TRACKS ) {
                err_format_track_range ( tracks, sides, 3 );
                return EXIT_FAILURE;
            }
            return cmd_format_cpm ( dsk_file, (unsigned int) tracks, sides );
        }

        case CREATE_FORMAT_CPMHD: {
            int rc;
            unsigned long tracks = parse_number ( cmd_arg, &rc );
            if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;
            unsigned long total = tracks * sides;
            if ( tracks < 1 || total < 3 || total > DSK_MAX_TOTAL_TRACKS ) {
                err_format_track_range ( tracks, sides, 3 );
                return EXIT_FAILURE;
            }
            return cmd_format_cpmhd ( dsk_file, (unsigned int) tracks, sides );
        }

        case CREATE_FORMAT_MRS: {
            int rc;
            unsigned long tracks = parse_number ( cmd_arg, &rc );
            if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;
            unsigned long total = tracks * sides;
            if ( tracks < 1 || total < MRS_MIN_TRACKS || total > DSK_MAX_TOTAL_TRACKS ) {
                err_format_track_range ( tracks, sides, MRS_MIN_TRACKS );
                return EXIT_FAILURE;
            }
            return cmd_format_mrs ( dsk_file, (unsigned int) tracks, sides );
        }

        case CREATE_CUSTOM: {
            /* Poziční argumenty za volbami: T S SS F [order] */
            int remaining = argc - optind;
            if ( remaining < 4 ) {
                fprintf ( stderr, "Error: --custom requires at least 4 parameters: <tracks> <sectors> <ssize> <filler>\n" );
                return EXIT_FAILURE;
            }

            int rc;

            unsigned long total_tracks = parse_number ( argv[optind], &rc );
            if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;

            unsigned long sectors_val = parse_number ( argv[optind + 1], &rc );
            if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;

            unsigned long ssize_val = parse_number ( argv[optind + 2], &rc );
            if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;

            unsigned long filler_val = parse_number ( argv[optind + 3], &rc );
            if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;

            /* Validace total_tracks */
            if ( total_tracks < 1 || total_tracks > DSK_MAX_TOTAL_TRACKS ) {
                err_custom_track_range ( total_tracks );
                return EXIT_FAILURE;
            }
            if ( sides == 2 && ( total_tracks & 1 ) ) {
                fprintf ( stderr, "Error: track count must be even for 2-sided disk.\n" );
                return EXIT_FAILURE;
            }

            /* Validace sectors */
            if ( sectors_val < 1 || sectors_val > DSK_MAX_SECTORS ) {
                fprintf ( stderr, "Error: sectors per track must be 1-%d.\n", DSK_MAX_SECTORS );
                return EXIT_FAILURE;
            }

            /* Validace sector size */
            en_DSK_SECTOR_SIZE ssize = dsk_encode_sector_size ( (uint16_t) ssize_val );
            if ( ssize == DSK_SECTOR_SIZE_INVALID ) {
                fprintf ( stderr, "Error: invalid sector size %lu. Must be 128, 256, 512, or 1024.\n", ssize_val );
                return EXIT_FAILURE;
            }

            /* Validace filler */
            if ( filler_val > 0xff ) {
                fprintf ( stderr, "Error: filler byte must be 0-255.\n" );
                return EXIT_FAILURE;
            }

            /* Parsování volitelného sector order */
            en_DSK_SECTOR_ORDER_TYPE secorder = DSK_SEC_ORDER_NORMAL;
            uint8_t sector_map[DSK_MAX_SECTORS];
            uint8_t *sector_map_ptr = NULL;

            if ( remaining >= 5 ) {
                const char *order_str = argv[optind + 4];
                if ( 0 == strcasecmp ( order_str, "normal" ) ) {
                    secorder = DSK_SEC_ORDER_NORMAL;
                } else if ( 0 == strcasecmp ( order_str, "lec" ) ) {
                    secorder = DSK_SEC_ORDER_INTERLACED_LEC;
                } else if ( 0 == strcasecmp ( order_str, "lechd" ) ) {
                    secorder = DSK_SEC_ORDER_INTERLACED_LEC_HD;
                } else {
                    /* Pokus o parsování jako custom sector map */
                    secorder = DSK_SEC_ORDER_CUSTOM;
                    int map_size = parse_sector_map ( order_str, sector_map, &rc );
                    if ( EXIT_SUCCESS != rc ) return EXIT_FAILURE;
                    if ( map_size != (int) sectors_val ) {
                        fprintf ( stderr, "Error: sector map size (%d) does not match sectors per track (%lu).\n",
                                  map_size, sectors_val );
                        return EXIT_FAILURE;
                    }
                    sector_map_ptr = sector_map;
                }
            }

            return cmd_custom ( dsk_file, (unsigned int) total_tracks, (uint8_t) sectors_val,
                                ssize, (uint8_t) filler_val, secorder, sector_map_ptr, sides );
        }

        default:
            break;
    }

    fprintf ( stderr, "Error: no valid command specified.\n" );
    print_usage ( stderr );
    return EXIT_FAILURE;
}
