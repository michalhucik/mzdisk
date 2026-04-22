/**
 * @file   mzdsk_info.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Inspekční nástroj pro DSK diskové obrazy Sharp MZ (pouze čtení).
 *
 * Zobrazuje geometrii disku, identifikovaný formát, pravidla stop,
 * informace o FSMZ formátu (obsazenost, mapa disku, bootstrap),
 * a umožňuje hexdump sektorů i FSMZ bloků.
 *
 * Port relevantních read-only funkcí z fstool (fstool_fsmz.c, fstool_dsk.c)
 * do nového CLI nástroje nad knihovnami mzdsk_*.
 *
 * @par Použití:
 * @code
 *   mzdsk-info <dsk_file> [options]
 *   mzdsk-info --version
 *   mzdsk-info --lib-versions
 * @endcode
 *
 * @par Příkazy:
 * - bez podpříkazu: základní info (geometrie, formát, FSMZ disc info)
 * - --map: vizuální mapa obsazenosti disku s auto-detekcí souborového
 *          systému (FSMZ, CP/M SD/HD/SD2S/HD2S, MRS)
 * - --boot: informace o bootstrap (IPLPRO) hlavičce
 * - --sector T S: hexdump sektoru na stopě T, sektoru S (S je 1-based)
 * - --block N: hexdump FSMZ bloku N
 *
 * @par Globální volby:
 * - --output FMT: výstupní formát: text (výchozí), json, csv
 *   (pro --sector a --block pouze text - při jiném formátu se vypíše varování)
 * - --charset MODE: konverze Sharp MZ znakové sady: eu (výchozí), jp, utf8-eu, utf8-jp
 * - --version: verze nástroje
 * - --lib-versions: verze použitých knihoven
 * - --cnv: vynutit konverzi Sharp ASCII v hexdumpu
 * - --nocnv: vynutit standardní ASCII v hexdumpu
 * - --inv: vynutit inverzi dat v hexdumpu
 * - --noinv: vynutit neinverzi dat v hexdumpu
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

#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/output_format/output_format.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/mzdsk_hexdump/mzdsk_hexdump.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzf/mzf_tools.h"
#include "tools/common/mzdisk_cli_version.h"


/**
 * @brief Globální nastavení konverze Sharp MZ znakové sady.
 *
 * Nastavuje se jednou při startu z volby --charset.
 * Výchozí hodnota je MZF_NAME_ASCII_EU (jednobajtová konverze z evropské znakové sady).
 */
static en_MZF_NAME_ENCODING g_name_encoding = MZF_NAME_ASCII_EU;
#include "libs/endianity/endianity.h"


/** @brief Verze nástroje mzdsk-info. */
#define MZDSK_INFO_VERSION "1.4.6"

/** @brief Výchozí číslo bloku s MRS FAT tabulkou. */
#define MRS_DEFAULT_FAT_BLOCK 36


/**
 * @brief Výčet podporovaných příkazů nástroje.
 *
 * Každý příkaz odpovídá jednomu režimu běhu programu.
 * CMD_INFO je výchozí příkaz, pokud nebyl zadán žádný podpříkaz.
 */
typedef enum en_MZDSK_INFO_CMD {
    CMD_INFO = 0,       /**< Základní info o disku (výchozí) */
    CMD_MAP,            /**< Vizuální mapa obsazenosti */
    CMD_BOOT,           /**< Informace o bootstrap (IPLPRO) */
    CMD_SECTOR,         /**< Hexdump sektoru */
    CMD_BLOCK,          /**< Hexdump FSMZ bloku */
    CMD_VERSION,        /**< Verze nástroje */
    CMD_LIB_VERSIONS,   /**< Verze knihoven */
} en_MZDSK_INFO_CMD;


/**
 * @brief Vypíše verze všech použitých knihoven na standardní výstup.
 *
 * Výpis slouží pro diagnostiku a identifikaci konkrétních verzí
 * závislostí v buildech.
 */
static void print_lib_versions ( void ) {
    printf ( "mzdsk-info %s (%s %s)\n\n",
             MZDSK_INFO_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
    printf ( "Library versions:\n" );
    printf ( "  dsk            %s\n", dsk_version () );
    printf ( "  mzdsk_global   %s\n", mzdsk_global_version () );
    printf ( "  mzdsk_ipldisk  %s\n", mzdsk_ipldisk_version () );
    printf ( "  mzdsk_cpm      %s\n", mzdsk_cpm_version () );
    printf ( "  mzdsk_mrs      %s\n", mzdsk_mrs_version () );
    printf ( "  mzdsk_detect   %s\n", mzdsk_detect_version () );
    printf ( "  mzdsk_hexdump  %s\n", mzdsk_hexdump_version () );
    printf ( "  output_format  %s\n", output_format_version () );
    printf ( "  generic_driver %s\n", generic_driver_version () );
    printf ( "  endianity      %s\n", endianity_version () );
    printf ( "  sharpmz_ascii  %s\n", sharpmz_ascii_version () );
}


/**
 * @brief Vypíše nápovědu k použití nástroje.
 *
 * @param[in] out       Výstupní stream (stdout pro --help, stderr pro chybové cesty).
 * @param[in] prog_name Jméno spustitelného souboru (argv[0]).
 */
static void print_usage ( FILE *out, const char *prog_name ) {
    fprintf ( out, "Usage:\n" );
    fprintf ( out, "  %s <dsk_file> [options]\n", prog_name );
    fprintf ( out, "  %s --version\n", prog_name );
    fprintf ( out, "  %s --lib-versions\n\n", prog_name );
    fprintf ( out, "Commands:\n" );
    fprintf ( out, "  (none)          Show disk geometry, format and FSMZ info\n" );
    fprintf ( out, "  --map           Show disk usage map with filesystem auto-detection\n" );
    fprintf ( out, "                  (FSMZ, CP/M SD/HD/SD2S/HD2S, MRS)\n" );
    fprintf ( out, "  --boot          Show bootstrap (IPLPRO) info\n" );
    fprintf ( out, "  --sector T S    Hexdump of sector at track T, sector S\n" );
    fprintf ( out, "  --block N       Hexdump of FSMZ block N\n\n" );
    fprintf ( out, "Options:\n" );
    fprintf ( out, "  --output FMT    Output format: text (default), json, csv\n" );
    fprintf ( out, "  -o FMT          Same as --output\n" );
    fprintf ( out, "  --charset MODE  Sharp MZ charset: eu (default), jp, utf8-eu, utf8-jp\n" );
    fprintf ( out, "  --cnv           Force enable Sharp ASCII conversion in hexdump\n" );
    fprintf ( out, "  --nocnv         Force disable Sharp ASCII conversion\n" );
    fprintf ( out, "  --dump-charset MODE  ASCII column charset: raw (default), eu, jp, utf8-eu, utf8-jp\n" );
    fprintf ( out, "  --inv           Force enable data inversion\n" );
    fprintf ( out, "  --noinv         Force disable data inversion\n" );
}


/**
 * @brief Vypíše jméno souboru zakončené 0x0d v Sharp MZ ASCII na stdout.
 *
 * Konvertuje jméno ze Sharp MZ ASCII podle globálního nastavení
 * g_name_encoding (--charset). Pro ASCII režimy tiskne po znacích,
 * pro UTF-8 konvertuje do bufferu a vytiskne najednou.
 * Tisk končí při znaku < 0x20 (typicky terminátor 0x0d).
 *
 * @param fname Ukazatel na pole jména souboru v Sharp MZ ASCII.
 *              Nesmí být NULL. Musí obsahovat terminátor < 0x20.
 */
static void print_filename ( const uint8_t *fname ) {
    /* Ochrana proti OOB čtení: fname má velikost FSMZ_FNAME_LENGTH.
     * Pokud je disk poškozený a buffer neobsahuje žádný bajt < 0x20,
     * smyčka bez horního omezení by čela až 256 B z 17-bajtového pole. */
    if ( g_name_encoding == MZF_NAME_ASCII_EU || g_name_encoding == MZF_NAME_ASCII_JP ) {
        uint8_t i = 0;
        while ( i < FSMZ_FNAME_LENGTH && fname[i] >= 0x20 ) {
            uint8_t c = ( g_name_encoding == MZF_NAME_ASCII_JP )
                          ? sharpmz_jp_cnv_from ( fname[i] )
                          : sharpmz_cnv_from ( fname[i] );
            putchar ( c );
            i++;
        }
    } else {
        /* UTF-8 režim - zjistit délku, konvertovat do bufferu */
        uint8_t len = 0;
        while ( len < FSMZ_FNAME_LENGTH && fname[len] >= 0x20 ) len++;

        char utf8_buf[MZF_FNAME_UTF8_BUF_SIZE];
        sharpmz_charset_t charset = ( g_name_encoding == MZF_NAME_UTF8_JP )
                                      ? SHARPMZ_CHARSET_JP : SHARPMZ_CHARSET_EU;
        sharpmz_str_to_utf8 ( fname, len, utf8_buf, sizeof ( utf8_buf ), charset );
        printf ( "%s", utf8_buf );
    }
}


/**
 * @brief Vypíše hlavičku IPLPRO bloku na stdout.
 *
 * Zobrazí jméno bootstrap programu (konvertované ze Sharp MZ ASCII),
 * typ souboru, start/size/exec adresy, číslo alokačního bloku
 * a velikost v blocích.
 *
 * @param iplpro Ukazatel na IPLPRO blok (po endianity korekci). Nesmí být NULL.
 */
static void print_iplpro_header ( const st_FSMZ_IPLPRO_BLOCK *iplpro ) {
    printf ( "\"" );
    print_filename ( iplpro->fname );
    printf ( "\"\n" );
    printf ( "\ttype = 0x%02x, start = 0x%04x, size = 0x%04x, exec = 0x%04x\n",
             iplpro->ftype, iplpro->fstrt, iplpro->fsize, iplpro->fexec );

    printf ( "\tblock = %d, size = ", iplpro->block );

    if ( iplpro->fsize < 1024 ) {
        printf ( "%d B", iplpro->fsize );
    } else {
        float realsize = (float) iplpro->fsize / 1024;
        printf ( "%0.2f kB", realsize );
    }

    printf ( " ( %d blocks )\n\n", fsmz_blocks_from_size ( iplpro->fsize ) );
}


/**
 * @brief Zobrazí základní informace o disku.
 *
 * Vypíše celkový počet stop, počet stran, pravidla geometrie
 * (odkud, kolik stop, sektory, velikost sektoru) a identifikovaný
 * formát. U disků s CP/M-like geometrií (9x512 nebo 18x512) se
 * provádí MRS FAT probe na bloku 36, aby se odlišil MRS od CP/M.
 * Pokud se jedná o plný FSMZ formát, zobrazí také informace
 * o obsazenosti (celkem/obsazeno/volno v blocích a kB).
 *
 * Port z fstool mztool_show_info() a fstool_fsmz_print_disc_info().
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @return MZDSK_RES_OK při úspěchu, jiný kód při chybě čtení DINFO.
 */
static en_MZDSK_RES cmd_show_info ( st_MZDSK_DISC *disc, en_OUTFMT output_format ) {

    /* Auto-detekce pro rozlišení MRS od CP/M (sdílejí geometrii 9x512) */
    st_MZDSK_DETECT_RESULT det;
    mzdsk_detect_filesystem ( disc, &det );
    int is_mrs = ( det.type == MZDSK_FS_MRS );

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nDisk total tracks: %d\n", disc->tracks_rules->total_tracks );
        printf ( "Disk sides: %d\n", disc->tracks_rules->sides );

        printf ( "\nTrack rules:\n" );
        int i;
        for ( i = 0; i < disc->tracks_rules->count_rules; i++ ) {
            st_DSK_TOOLS_TRACK_RULE_INFO *rule = &disc->tracks_rules->rule[i];
            printf ( "From %d, count %d, sectors %d, size %d B\n",
                     rule->from_track,
                     rule->count_tracks,
                     rule->sectors,
                     dsk_decode_sector_size ( rule->ssize ) );
        }

        printf ( "\nFormat test result: " );

        switch ( disc->format ) {
            case DSK_TOOLS_IDENTFORMAT_MZBOOT:
                printf ( "This disk has only FSMZ bootstrap track\n\n" );
                break;
            case DSK_TOOLS_IDENTFORMAT_MZBASIC:
                printf ( "This disk is probably in FSMZ format\n" );
                break;
            case DSK_TOOLS_IDENTFORMAT_MZCPM:
                if ( is_mrs ) {
                    printf ( "This disk is probably in MRS format\n\n" );
                } else {
                    printf ( "This disk is probably in MZ-CPM format\n\n" );
                }
                break;
            case DSK_TOOLS_IDENTFORMAT_MZCPMHD:
                printf ( "This disk is probably in MZ-CPM(HD) format\n\n" );
                break;
            case DSK_TOOLS_IDENTFORMAT_UNKNOWN:
            default:
                printf ( "Can't recognize disk format\n\n" );
                return MZDSK_RES_OK;
        }

        /* Pokud je formát FSMZ, zobrazíme disc info */
        if ( DSK_TOOLS_IDENTFORMAT_MZBASIC == disc->format ) {
            st_FSMZ_DINFO_BLOCK dinfo;
            en_MZDSK_RES err = fsmz_read_dinfo ( disc, &dinfo );
            if ( err ) return err;

            printf ( "\nFSMZ Disk info:\n" );
            printf ( "\tTotal disk size: %d blocks ( %d kB )\n",
                     dinfo.blocks + 1,
                     (int) ( fsmz_size_from_blocks ( dinfo.blocks + 1 ) / 1024 ) );
            printf ( "\tUsed: %d blocks ( %d kB )\n",
                     dinfo.used,
                     (int) ( fsmz_size_from_blocks ( dinfo.used ) / 1024 ) );
            if ( dinfo.used > dinfo.blocks + 1 ) {
                printf ( "\tFree: 0 blocks ( 0 kB ) - disk is too small for FSMZ metadata (%d blocks needed)\n", dinfo.used );
            } else {
                printf ( "\tFree: %d blocks ( %d kB )\n",
                         ( dinfo.blocks + 1 - dinfo.used ),
                         (int) ( fsmz_size_from_blocks ( dinfo.blocks + 1 - dinfo.used ) / 1024 ) );
            }
            printf ( "\n" );
        }
    } else {
        const char *format_str;
        switch ( disc->format ) {
            case DSK_TOOLS_IDENTFORMAT_MZBOOT:  format_str = "fsmz_boot"; break;
            case DSK_TOOLS_IDENTFORMAT_MZBASIC: format_str = "fsmz"; break;
            case DSK_TOOLS_IDENTFORMAT_MZCPM:   format_str = is_mrs ? "mrs" : "cpm"; break;
            case DSK_TOOLS_IDENTFORMAT_MZCPMHD: format_str = "cpm_hd"; break;
            default:                            format_str = "unknown"; break;
        }

        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_int ( &ctx, "total_tracks", disc->tracks_rules->total_tracks );
        outfmt_kv_int ( &ctx, "sides", disc->tracks_rules->sides );
        outfmt_kv_str ( &ctx, "format", format_str );

        if ( DSK_TOOLS_IDENTFORMAT_MZBASIC == disc->format ) {
            st_FSMZ_DINFO_BLOCK dinfo;
            en_MZDSK_RES err = fsmz_read_dinfo ( disc, &dinfo );
            if ( err == MZDSK_RES_OK ) {
                outfmt_kv_int ( &ctx, "total_blocks", dinfo.blocks + 1 );
                outfmt_kv_int ( &ctx, "used_blocks", dinfo.used );
                int free_blocks = ( dinfo.used > dinfo.blocks + 1 ) ? 0 : (int) ( dinfo.blocks + 1 - dinfo.used );
                outfmt_kv_int ( &ctx, "free_blocks", free_blocks );
            }
        }

        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zobrazí vizuální mapu obsazenosti FSMZ disku.
 *
 * Vypíše mapu všech alokačních bloků disku. Významy znaků:
 * - 'I': IPLPRO hlavička (blok 0)
 * - 'M': rezervováno pro DINFO (blok 15)
 * - 'D': adresář (bloky 16-23 resp. 16-31)
 * - 'B': bootstrap loader těleso
 * - 'x': obsazený blok
 * - '-': volný blok
 *
 * Za mapou následuje sumarizace a legenda.
 *
 * Port z fstool_fsmz_print_discmap(). Volána pouze pro disk
 * v plném FSMZ formátu (DSK_TOOLS_IDENTFORMAT_MZBASIC).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @return MZDSK_RES_OK při úspěchu, jiný kód při chybě.
 */
static en_MZDSK_RES cmd_show_map_fsmz ( st_MZDSK_DISC *disc, en_OUTFMT output_format ) {

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nRead bootstrap header: " );
    }

    uint8_t tracks = disc->tracks_rules->total_tracks / disc->tracks_rules->sides;
    uint8_t sides = disc->tracks_rules->sides;
    int total_blocks = tracks * sides * FSMZ_SECTORS_ON_TRACK;

    /* Sestavení alokační mapy přes knihovní API */
    en_FSMZ_BLOCK_TYPE block_map[4096]; /* 256 stop * 16 sektorů = max 4096 */
    st_FSMZ_MAP_STATS map_stats;
    int map_count = ( total_blocks < 4096 ) ? total_blocks : 4096;

    en_MZDSK_RES err = fsmz_tool_get_block_map ( disc, block_map, map_count, &map_stats );
    if ( err ) return err;

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "OK\nRead disc info: OK\n\n" );
        if ( map_stats.bitmap_inconsistencies > 0 ) {
            fprintf ( stderr, "Warning: %d bitmap inconsistencies detected "
                              "(bootstrap/bitmap mismatch).\n",
                              map_stats.bitmap_inconsistencies );
        }
    }

    /* Proudový JSON/CSV kontext */
    st_OUTFMT_CTX ctx;
    if ( output_format != OUTFMT_TEXT ) {
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = { "block", "type" };
        outfmt_csv_header ( &ctx, csv_hdr, 2 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "fsmz" );
        outfmt_kv_int ( &ctx, "total_blocks", map_stats.total_blocks );
        outfmt_kv_int ( &ctx, "farea_start", map_stats.farea_start );
        outfmt_array_begin ( &ctx, "blocks" );
    }

    /* Sledování, zda už byl vypsán nadpis pro farea / over_farea */
    int farea_header_printed = 0;
    int over_farea_header_printed = 0;

    for ( int i = 0; i < map_stats.total_blocks; i++ ) {

        /* Konverze typu bloku na zobrazovací znak a textový popisek */
        char ch;
        const char *type_str;

        switch ( block_map[i] ) {
            case FSMZ_BLOCK_IPLPRO:     ch = 'I'; type_str = "iplpro"; break;
            case FSMZ_BLOCK_META:        ch = 'M'; type_str = "meta"; break;
            case FSMZ_BLOCK_DIR:         ch = 'D'; type_str = "directory"; break;
            case FSMZ_BLOCK_USED:        ch = 'x'; type_str = "used"; break;
            case FSMZ_BLOCK_BOOTSTRAP:   ch = 'B'; type_str = "bootstrap"; break;
            case FSMZ_BLOCK_OVER_FAREA:  ch = '-'; type_str = "over_farea"; break;
            case FSMZ_BLOCK_FREE:
            default:                     ch = '-'; type_str = "free"; break;
        }

        if ( output_format == OUTFMT_TEXT ) {
            /* Nadpisy sekcí - vypíšeme při prvním bloku dané oblasti */
            if ( i == map_stats.farea_start && !farea_header_printed ) {
                printf ( "\nFSMZ file area:\n" );
                farea_header_printed = 1;
            }
            if ( block_map[i] == FSMZ_BLOCK_OVER_FAREA && !over_farea_header_printed ) {
                printf ( "\nOver FSMZ file area:\n" );
                over_farea_header_printed = 1;
            }
            printf ( "%c", ch );
        } else {
            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "block", i );
            outfmt_field_str ( &ctx, "type", type_str );
            outfmt_item_end ( &ctx );
        }
    }

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\n\n" );

        printf ( "Total blocks: %d\n", map_stats.total_blocks );
        printf ( "FAREA start block: %d\n", map_stats.farea_start );
        /* FAREA size: počítáno z dinfo.blocks+1 - dinfo.farea,
           ekvivalent = total_blocks - farea_start - over_farea
           (pro přesný výpis čteme hodnotu z mapy) */
        printf ( "FAREA size: %d\n", map_stats.total_blocks - map_stats.farea_start - map_stats.over_farea );
        printf ( "FAREA used: %d\n", map_stats.farea_used );
        printf ( "Over FAREA blocks: %d\n", map_stats.over_farea );

        printf ( "\nMap Legend\n" );
        printf ( "\tI - IPLPRO header\n" );
        printf ( "\tM - RESERVED for disc info and bitmap\n" );
        printf ( "\tD - RESERVED for directory items\n" );
        printf ( "\tB - Block is used by bootstrap loader\n" );
        printf ( "\tx - Block is used\n" );
        printf ( "\t  - Block is free for use\n\n" );
    } else {
        outfmt_array_end ( &ctx );
        outfmt_kv_int ( &ctx, "farea_used", map_stats.farea_used );
        outfmt_kv_int ( &ctx, "over_farea", map_stats.over_farea );
        outfmt_kv_int ( &ctx, "bitmap_inconsistencies", map_stats.bitmap_inconsistencies );
        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Vykreslí mapu FSMZ bootstrap stopy (16 bloků) bez format hlavičky.
 *
 * Přečte IPLPRO hlavičku, vypíše vizualizaci prvních 16 bloků
 * 0. stopy (IPLPRO + tělo bootstrap loaderu + volné bloky) a legendu.
 * Volána vždy, když má disk FSMZ boot track a chceme zobrazit jeho
 * layout, nezávisle na tom, jaký je filesystem v datové oblasti.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @return MZDSK_RES_OK při úspěchu, jiný kód při chybě čtení IPLPRO.
 *
 * @pre Disk musí mít FSMZ bootovatelnou stopu (mzboot_track == 1).
 */
static en_MZDSK_RES render_fsmz_boot_track ( st_MZDSK_DISC *disc, en_OUTFMT output_format,
                                               st_OUTFMT_CTX *parent_ctx ) {

    en_MZDSK_RES err;
    st_FSMZ_IPLPRO_BLOCK iplpro;

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nRead bootstrap header: " );
    }
    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;
    if ( output_format == OUTFMT_TEXT ) {
        printf ( "OK\n" );
    }

    /* Je na 0. bloku zavaděč? */
    char block0 = '-';
    uint16_t system_start = 0;
    uint16_t system_end = 0;

    if ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        block0 = 'I';
        system_start = iplpro.block;
        system_end = iplpro.block + ( iplpro.fsize / FSMZ_SECTOR_SIZE ) - 1;
        if ( iplpro.fsize % FSMZ_SECTOR_SIZE ) system_end++;
    }

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nFSMZ boot track:\n" );
        int i;
        for ( i = 0; i < FSMZ_SECTORS_ON_TRACK; i++ ) {
            char used = '-';
            if ( i == 0 ) {
                used = block0;
            } else if ( ( i >= system_start ) && ( i <= system_end ) ) {
                used = 'B';
            }
            printf ( "%c", used );
        }
        printf ( "\n\n" );

        printf ( "\nMap Legend\n" );
        printf ( "\tI - IPLPRO header\n" );
        printf ( "\tB - Block is used by bootstrap loader\n" );
        printf ( "\t  - Block is free for use\n\n" );
    } else if ( parent_ctx != NULL ) {
        /* Zapíšeme boot bloky do parent kontextu (pole "boot_track") */
        outfmt_array_begin ( parent_ctx, "boot_track" );
        for ( int i = 0; i < FSMZ_SECTORS_ON_TRACK; i++ ) {
            const char *type_str = "free";
            if ( i == 0 && block0 == 'I' ) type_str = "iplpro";
            else if ( ( i >= system_start ) && ( i <= system_end ) ) type_str = "bootstrap";

            outfmt_item_begin ( parent_ctx );
            outfmt_field_int ( parent_ctx, "block", i );
            outfmt_field_str ( parent_ctx, "type", type_str );
            outfmt_item_end ( parent_ctx );
        }
        outfmt_array_end ( parent_ctx );
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zobrazí mapu pouze FSMZ bootstrap stopy (16 bloků).
 *
 * Určeno pro disky, které nemají plný FSMZ formát a jejichž filesystem
 * v datové oblasti nebyl identifikován (MZBOOT, neznámý CP/M, ...).
 * Vypíše hlavičku o neznámém formátu a pak deleguje na
 * render_fsmz_boot_track().
 *
 * Port z větve fstool_print_discmap() pro ne-FSMZ disky.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @return MZDSK_RES_OK při úspěchu, jiný kód při chybě čtení IPLPRO.
 *
 * @pre Disk musí mít FSMZ bootovatelnou stopu (mzboot_track == 1).
 */
static en_MZDSK_RES cmd_show_map_boot_only ( st_MZDSK_DISC *disc, en_OUTFMT output_format ) {
    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nFORMAT: Disk in unknown format with FSMZ boot track.\n" );
        return render_fsmz_boot_track ( disc, output_format, NULL );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = { "block", "type" };
        outfmt_csv_header ( &ctx, csv_hdr, 2 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "unknown" );

        en_MZDSK_RES err = render_fsmz_boot_track ( disc, output_format, &ctx );
        outfmt_doc_end ( &ctx );
        return err;
    }
}


/**
 * @brief Vrátí lidsky čitelný název CP/M presetu.
 *
 * @param format CP/M preset.
 * @return Statický řetězec s názvem presetu.
 */
static const char* cpm_format_name ( en_MZDSK_CPM_FORMAT format ) {
    switch ( format ) {
        case MZDSK_CPM_FORMAT_SD: return "SD (9x512B, Lamac)";
        case MZDSK_CPM_FORMAT_HD: return "HD (18x512B, Lucky-Soft)";
        default:                  return "unknown";
    }
}


/**
 * @brief Zobrazí alokační mapu CP/M disku s detekovaným presetem.
 *
 * Formát mapy odpovídá výstupu `mzdsk-cpm map`: záhlaví s desítkami
 * a jednotkami, 32 znaků na řádek, 'X' = obsazený blok, '.' = volný.
 * V hlavičce je vypsán název detekovaného presetu (např. "SD 2-sided").
 *
 * @param[in] disc   Ukazatel na otevřený disk. Nesmí být NULL.
 * @param[in] dpb    DPB pro detekovaný preset. Nesmí být NULL.
 * @param[in] format Detekovaný CP/M preset (pro popisek).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_show_map_cpm ( st_MZDSK_DISC *disc,
                                        const st_MZDSK_CPM_DPB *dpb,
                                        en_MZDSK_CPM_FORMAT format,
                                        en_OUTFMT output_format ) {

    st_MZDSK_CPM_ALLOC_MAP alloc_map;
    en_MZDSK_RES res = mzdsk_cpm_get_alloc_map ( disc, dpb, &alloc_map );
    if ( res != MZDSK_RES_OK ) return res;

    /* Statistika obsazení adresářových slotů (pro text i strukturovaný výstup) */
    st_MZDSK_CPM_DIR_STATS dstats;
    int dstats_ok = ( mzdsk_cpm_get_dir_stats ( disc, dpb, &dstats ) == MZDSK_RES_OK );

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nFORMAT: CP/M %s\n", cpm_format_name ( format ) );
        printf ( "Preset: default (block %d B, %d blocks, %d dir slots, %d reserved tracks)\n",
                 dpb->block_size, dpb->dsm + 1, dpb->drm + 1, dpb->off );
        if ( dstats_ok ) {
            printf ( "Dir slots: total=%u, used=%u, free=%u, blocked=%u\n",
                     dstats.total, dstats.used, dstats.free, dstats.blocked );
        }

        printf ( "\nCP/M Allocation map (%d blocks):\n\n", alloc_map.total_blocks );

        printf ( "       " );
        for ( int j = 0; j < 32; j++ ) {
            if ( j % 10 == 0 ) printf ( "%d", j / 10 );
            else printf ( " " );
        }
        printf ( "\n" );

        printf ( "       " );
        for ( int j = 0; j < 32; j++ ) {
            printf ( "%d", j % 10 );
        }
        printf ( "\n" );

        for ( uint16_t block = 0; block < alloc_map.total_blocks; block++ ) {
            if ( block % 32 == 0 ) printf ( "  %3d: ", block );
            int used = alloc_map.map[block / 8] & ( 1u << ( block % 8 ) );
            printf ( "%c", used ? 'X' : '.' );
            if ( block % 32 == 31 || block == alloc_map.total_blocks - 1 ) {
                printf ( "\n" );
            }
        }

        printf ( "\n  Total: %d, Used: %d, Free: %d\n\n",
                 alloc_map.total_blocks, alloc_map.used_blocks, alloc_map.free_blocks );

        printf ( "Map Legend\n" );
        printf ( "\tX - Block is used\n" );
        printf ( "\t. - Block is free for use\n\n" );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = { "block", "used" };
        outfmt_csv_header ( &ctx, csv_hdr, 2 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "cpm" );
        outfmt_kv_str ( &ctx, "format", cpm_format_name ( format ) );
        outfmt_kv_int ( &ctx, "total_blocks", alloc_map.total_blocks );
        outfmt_kv_int ( &ctx, "used_blocks", alloc_map.used_blocks );
        outfmt_kv_int ( &ctx, "free_blocks", alloc_map.free_blocks );
        if ( dstats_ok ) {
            outfmt_kv_int ( &ctx, "dir_slots_total", dstats.total );
            outfmt_kv_int ( &ctx, "dir_slots_used", dstats.used );
            outfmt_kv_int ( &ctx, "dir_slots_free", dstats.free );
            outfmt_kv_int ( &ctx, "dir_slots_blocked", dstats.blocked );
        }
        outfmt_array_begin ( &ctx, "blocks" );
        for ( uint16_t block = 0; block < alloc_map.total_blocks; block++ ) {
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


/* ================================================================
 * MRS souborový systém
 * ================================================================ */


/** @brief Výchozí počet FAT sektorů u standardního 720K MRS disku. */
#define MRS_DEFAULT_FAT_SECTORS 2

/** @brief Výchozí počet DIR sektorů u standardního 720K MRS disku. */
#define MRS_DEFAULT_DIR_SECTORS 7

/** @brief Počet znaků v jednom řádku MRS mapy. */
#define MRS_MAP_COLS 64


/**
 * @brief Zobrazí alokační mapu MRS disku.
 *
 * Pro každý blok ve FAT vypíše jeden znak podle jeho role:
 * - '-' = volný (0x00)
 * - 'F' = FAT sektor (0xFA)
 * - 'D' = directory sektor (0xFD)
 * - 'E' = bad (0xFE)
 * - 'S' = systémový blok (0xFF, typicky boot oblast)
 * - 'x' = blok obsazený uživatelským souborem (1..max_file_id)
 *
 * Mapa je zalomená po MRS_MAP_COLS znacích na řádek. Před mapou
 * je vypsán layout (fat_block, fat_sectors, dir_block, dir_sectors,
 * data_block) s indikací, zda jde o default preset.
 *
 * @param[in] config Ukazatel na inicializovanou MRS konfiguraci. Nesmí být NULL.
 * @return MZDSK_RES_OK vždy (pouze výpis).
 */
static en_MZDSK_RES cmd_show_map_mrs ( const st_FSMRS_CONFIG *config, en_OUTFMT output_format ) {

    /* Sestavení alokační mapy přes knihovní API */
    en_FSMRS_BLOCK_TYPE block_map[FSMRS_COUNT_BLOCKS];
    st_FSMRS_MAP_STATS map_stats;
    fsmrs_get_block_map ( config, block_map, FSMRS_COUNT_BLOCKS, &map_stats );

    int is_default =
        ( config->fat_block == MRS_DEFAULT_FAT_BLOCK ) &&
        ( config->fat_sectors == MRS_DEFAULT_FAT_SECTORS ) &&
        ( config->dir_sectors == MRS_DEFAULT_DIR_SECTORS );

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nFORMAT: MRS filesystem\n" );
        if ( is_default ) {
            printf ( "Preset: default (FAT block %d, %d FAT sectors, %d DIR sectors)\n",
                     MRS_DEFAULT_FAT_BLOCK, MRS_DEFAULT_FAT_SECTORS, MRS_DEFAULT_DIR_SECTORS );
        } else {
            printf ( "Preset: custom layout\n" );
            printf ( "  FAT block:    %d\n", config->fat_block );
            printf ( "  FAT sectors:  %d\n", config->fat_sectors );
            printf ( "  DIR block:    %d\n", config->dir_block );
            printf ( "  DIR sectors:  %d\n", config->dir_sectors );
            printf ( "  Data block:   %d\n", config->data_block );
        }

        printf ( "\nMRS Allocation map (%d blocks):\n\n", FSMRS_COUNT_BLOCKS );

        for ( int block = 0; block < FSMRS_COUNT_BLOCKS; block++ ) {
            if ( block % MRS_MAP_COLS == 0 ) printf ( "  %4d: ", block );

            char ch;
            switch ( block_map[block] ) {
                case FSMRS_BLOCK_FREE:   ch = '-'; break;
                case FSMRS_BLOCK_FAT:    ch = 'F'; break;
                case FSMRS_BLOCK_DIR:    ch = 'D'; break;
                case FSMRS_BLOCK_BAD:    ch = 'E'; break;
                case FSMRS_BLOCK_SYSTEM: ch = 'S'; break;
                case FSMRS_BLOCK_FILE:   ch = 'x'; break;
                default:                 ch = '?'; break;
            }
            printf ( "%c", ch );

            if ( block % MRS_MAP_COLS == MRS_MAP_COLS - 1 || block == FSMRS_COUNT_BLOCKS - 1 ) {
                printf ( "\n" );
            }
        }

        printf ( "\n" );
        printf ( "  Total blocks:  %d\n", FSMRS_COUNT_BLOCKS );
        printf ( "  System (S):    %d\n", map_stats.sys_blocks );
        printf ( "  FAT (F):       %d\n", map_stats.fat_blocks );
        printf ( "  Directory (D): %d\n", map_stats.dir_blocks );
        printf ( "  File data (x): %d\n", map_stats.file_blocks );
        if ( map_stats.bad_blocks > 0 ) printf ( "  Bad (E):       %d\n", map_stats.bad_blocks );
        printf ( "  Free (-):      %d\n", map_stats.free_blocks );
        printf ( "  Used files:    %d / %d\n\n", config->used_files, config->max_files );

        printf ( "Map Legend\n" );
        printf ( "\tS - System/reserved block (0xFF)\n" );
        printf ( "\tF - FAT block (0xFA)\n" );
        printf ( "\tD - Directory block (0xFD)\n" );
        printf ( "\tE - Bad block (0xFE)\n" );
        printf ( "\tx - Block owned by a file\n" );
        printf ( "\t- - Block is free for use\n\n" );
    } else {
        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        static const char *csv_hdr[] = { "block", "type" };
        outfmt_csv_header ( &ctx, csv_hdr, 2 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "mrs" );
        outfmt_array_begin ( &ctx, "blocks" );
        for ( int block = 0; block < FSMRS_COUNT_BLOCKS; block++ ) {
            const char *type_str;
            switch ( block_map[block] ) {
                case FSMRS_BLOCK_FREE:   type_str = "free"; break;
                case FSMRS_BLOCK_FAT:    type_str = "fat"; break;
                case FSMRS_BLOCK_DIR:    type_str = "directory"; break;
                case FSMRS_BLOCK_BAD:    type_str = "bad"; break;
                case FSMRS_BLOCK_SYSTEM: type_str = "system"; break;
                case FSMRS_BLOCK_FILE:   type_str = "file"; break;
                default:                 type_str = "unknown"; break;
            }
            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "block", block );
            outfmt_field_str ( &ctx, "type", type_str );
            outfmt_item_end ( &ctx );
        }
        outfmt_array_end ( &ctx );

        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/* ================================================================
 * Dispečer mapy
 * ================================================================ */


/**
 * @brief Dispečer mapy obsazenosti s auto-detekcí souborového systému.
 *
 * Používá mzdsk_detect_filesystem() pro identifikaci FS a podle
 * výsledku deleguje na příslušnou zobrazovací funkci.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param output_format Výstupní formát (TEXT/JSON/CSV).
 * @return MZDSK_RES_OK při úspěchu, jiný kód při chybě.
 */
static en_MZDSK_RES cmd_show_map ( st_MZDSK_DISC *disc, en_OUTFMT output_format ) {

    st_MZDSK_DETECT_RESULT det;
    mzdsk_detect_filesystem ( disc, &det );

    switch ( det.type ) {

        case MZDSK_FS_FSMZ:
            if ( output_format == OUTFMT_TEXT ) {
                printf ( "\nFORMAT: Disk in FSMZ format.\n" );
            }
            return cmd_show_map_fsmz ( disc, output_format );

        case MZDSK_FS_MRS:
            if ( output_format == OUTFMT_TEXT ) {
                en_MZDSK_RES err = render_fsmz_boot_track ( disc, output_format, NULL );
                if ( err ) return err;
            }
            return cmd_show_map_mrs ( &det.mrs_config, output_format );

        case MZDSK_FS_CPM:
            if ( output_format == OUTFMT_TEXT ) {
                en_MZDSK_RES err = render_fsmz_boot_track ( disc, output_format, NULL );
                if ( err ) return err;
            }
            return cmd_show_map_cpm ( disc, &det.cpm_dpb, det.cpm_format, output_format );

        case MZDSK_FS_BOOT_ONLY:
            fprintf ( stderr, "\nWarning: could not identify filesystem (neither MRS FAT nor CP/M directory)\n" );
            fprintf ( stderr, "Hint: for custom DPB use 'mzdsk-cpm --spt/--bsh/... map'\n" );
            fprintf ( stderr, "      for custom MRS FAT block use 'mzdsk-mrs --fat-block N info'\n" );
            return cmd_show_map_boot_only ( disc, output_format );

        case MZDSK_FS_UNKNOWN:
        default:
            fprintf ( stderr, "\nError: unknown disk format (no FSMZ boot track)\n" );
            fprintf ( stderr, "Hint: use mzdsk-cpm map for CP/M or mzdsk-mrs info for MRS disks\n" );
            return MZDSK_RES_FORMAT_ERROR;
    }
}


/**
 * @brief Zobrazí informace o bootstrap (IPLPRO) hlavičce.
 *
 * Přečte IPLPRO blok (alokační blok 0), otestuje platnost hlavičky
 * a pokud je platná, vypíše detaily (jméno, typ, adresy, velikost).
 *
 * Port z fstool_fsmz_print_bootstrap_info().
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @return MZDSK_RES_OK při úspěchu, jiný kód při chybě.
 *
 * @pre Disk musí mít bootovatelnou stopu (mzboot_track == 1).
 */
static en_MZDSK_RES cmd_show_boot ( st_MZDSK_DISC *disc, en_OUTFMT output_format ) {

    if ( 1 != disc->tracks_rules->mzboot_track ) {
        fprintf ( stderr, "Error: This disk is not in FSMZ bootable format\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nReading bootstrap header ...\n\n" );
    }

    st_FSMZ_IPLPRO_BLOCK iplpro;
    en_MZDSK_RES err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    if ( EXIT_SUCCESS != fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        fprintf ( stderr, "Error: Not found valid IPLPRO header!\n" );
        return MZDSK_RES_FORMAT_ERROR;
    } else if ( output_format == OUTFMT_TEXT ) {
        print_iplpro_header ( &iplpro );
    } else {
        /* Konverze jména ze Sharp MZ ASCII podle --charset */
        char name_str[MZF_FNAME_UTF8_BUF_SIZE];
        if ( g_name_encoding == MZF_NAME_ASCII_EU || g_name_encoding == MZF_NAME_ASCII_JP ) {
            size_t ni = 0;
            while ( ni < FSMZ_FNAME_LENGTH && iplpro.fname[ni] >= 0x20 ) {
                name_str[ni] = (char) ( ( g_name_encoding == MZF_NAME_ASCII_JP )
                                          ? sharpmz_jp_cnv_from ( iplpro.fname[ni] )
                                          : sharpmz_cnv_from ( iplpro.fname[ni] ) );
                ni++;
            }
            name_str[ni] = '\0';
        } else {
            uint8_t len = 0;
            while ( len < FSMZ_FNAME_LENGTH && iplpro.fname[len] >= 0x20 ) len++;
            sharpmz_charset_t charset = ( g_name_encoding == MZF_NAME_UTF8_JP )
                                          ? SHARPMZ_CHARSET_JP : SHARPMZ_CHARSET_EU;
            sharpmz_str_to_utf8 ( iplpro.fname, len, name_str, sizeof ( name_str ), charset );
        }

        st_OUTFMT_CTX ctx;
        outfmt_init ( &ctx, output_format );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "name", name_str );
        outfmt_kv_int ( &ctx, "type", iplpro.ftype );
        outfmt_kv_uint ( &ctx, "start", (unsigned long) iplpro.fstrt );
        outfmt_kv_uint ( &ctx, "size", (unsigned long) iplpro.fsize );
        outfmt_kv_uint ( &ctx, "exec", (unsigned long) iplpro.fexec );
        outfmt_kv_int ( &ctx, "block", iplpro.block );
        outfmt_kv_int ( &ctx, "block_count", fsmz_blocks_from_size ( iplpro.fsize ) );
        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}




/**
 * @brief Provede hexdump jednoho sektoru z DSK obrazu.
 *
 * Zjistí velikost sektoru a typ média (normální/invertovaný),
 * rozhodne o inverzi dat podle parametru force_inv, nastaví režim
 * znakové sady podle dump_charset, přečte sektor a vypíše hexdump
 * s informační hlavičkou. Hlavička se vypisuje až po úspěšném čtení
 * dat, aby se na stdout nedostala při chybě čtení (driver error apod.).
 *
 * Hodnoty force_inv:
 * - 0: automatická detekce
 * - 1: vynutit zapnutí
 * - -1: vynutit vypnutí
 *
 * Port z fstool_dsk_hexdump_sector().
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param abs_track Absolutní číslo stopy (0-based).
 * @param sector ID sektoru (1-based).
 * @param print_info Nenulová = vypsat informační hlavičku před hexdumpem.
 * @param force_inv Vynucení inverze: 0=auto, 1=zapnout, -1=vypnout.
 * @param dump_charset Režim konverze znakové sady v ASCII sloupci hexdumpu.
 * @return MZDSK_RES_OK při úspěchu, jiný kód při chybě.
 *
 * @pre Volající musí zajistit, že abs_track a sector jsou v platném rozsahu.
 *      Validace se provádí v main() před voláním.
 */
static en_MZDSK_RES hexdump_sector ( st_MZDSK_DISC *disc, uint8_t abs_track, uint8_t sector, int print_info, int force_inv, en_MZDSK_HEXDUMP_CHARSET dump_charset ) {

    uint8_t sector_info = disc->sector_info_cb ( abs_track, sector, disc->sector_info_cb_data );
    uint16_t sector_size = dsk_decode_sector_size ( sector_info & 0x03 );

    /* Textový popisek režimu znakové sady pro informační hlavičku */
    const char *cnv_txt = NULL;
    switch ( dump_charset ) {
        case MZDSK_HEXDUMP_CHARSET_RAW:     cnv_txt = "ASCII"; break;
        case MZDSK_HEXDUMP_CHARSET_EU:      cnv_txt = "SharpASCII-EU"; break;
        case MZDSK_HEXDUMP_CHARSET_JP:      cnv_txt = "SharpASCII-JP"; break;
        case MZDSK_HEXDUMP_CHARSET_UTF8_EU: cnv_txt = "SharpUTF8-EU"; break;
        case MZDSK_HEXDUMP_CHARSET_UTF8_JP: cnv_txt = "SharpUTF8-JP"; break;
        default:                            cnv_txt = "ASCII"; break;
    }

    int native_inv = 0;
    int inv = 0;
    const char *inv_txt = NULL;

    if ( ( sector_info & 0x80 ) == MZDSK_MEDIUM_INVERTED ) {
        native_inv = 1;
    }

    if ( force_inv == -1 ) {
        if ( native_inv == 1 ) {
            inv_txt = "forced NOT inverted";
            inv = 1;
        } else {
            inv_txt = "normal";
            inv = 0;
        }
    } else if ( force_inv == 1 ) {
        if ( native_inv == 0 ) {
            inv_txt = "forced INVERTED";
            inv = 1;
        } else {
            inv_txt = "inverted";
            inv = 0;
        }
    } else {
        inv = 0;
        inv_txt = ( native_inv == 1 ) ? "inverted" : "normal";
    }

    uint8_t *dma = malloc ( sector_size );
    if ( !dma ) {
        fprintf ( stderr, "Error: memory allocation failed\n" );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    en_MZDSK_RES err = mzdsk_disc_read_sector ( disc, abs_track, sector, dma );
    if ( err == MZDSK_RES_OK ) {
        if ( print_info ) {
            printf ( "\nDump %d bytes from abs track: %d, sector: %d - %s, %s:\n\n", sector_size, abs_track, sector, cnv_txt, inv_txt );
        }
        st_MZDSK_HEXDUMP_CFG hcfg;
        mzdsk_hexdump_init ( &hcfg );
        hcfg.col_separator = 4;
        hcfg.ascii_separator = 4;
        hcfg.inv = inv;
        hcfg.charset = dump_charset;
        mzdsk_hexdump ( &hcfg, dma, sector_size );
    }

    free ( dma );
    return err;
}


/**
 * @brief Provede hexdump FSMZ alokačního bloku.
 *
 * Přepočítá číslo bloku na stopu/sektor a zavolá hexdump_sector().
 * Kontroluje, zda se jedná o FSMZ stopu (16x256 B).
 *
 * Port z fstool_fsmz_print_block().
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param block Číslo FSMZ alokačního bloku.
 * @param force_inv Vynucení inverze: 0=auto, 1=zapnout, -1=vypnout.
 * @param dump_charset Režim konverze znakové sady v ASCII sloupci hexdumpu.
 * @return MZDSK_RES_OK při úspěchu.
 * @return MZDSK_RES_FORMAT_ERROR pokud blok je mimo rozsah disku
 *         nebo stopa nemá FSMZ geometrii (chybová hláška je vypsána na stderr).
 */
static en_MZDSK_RES cmd_show_block ( st_MZDSK_DISC *disc, uint16_t block, int force_inv, en_MZDSK_HEXDUMP_CHARSET dump_charset ) {

    uint16_t max_block = (uint16_t) ( disc->tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK );
    if ( block >= max_block ) {
        fprintf ( stderr, "Error: Invalid block number %d (max %d)\n", block, max_block - 1 );
        return MZDSK_RES_FORMAT_ERROR;
    }

    uint16_t trsec = fsmz_block2trsec ( block );

    uint8_t track = ( trsec >> 8 ) & 0xff;
    uint8_t sector = trsec & 0xff;

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        uint8_t sector_info = disc->sector_info_cb ( track, sector, disc->sector_info_cb_data );
        if ( sector_info != ( MZDSK_MEDIUM_NORMAL | DSK_SECTOR_SIZE_256 ) ) {
            fprintf ( stderr, "Error: Block %d (track %d, sector %d) is not in FSMZ format "
                      "(expected 16x256B geometry)\n", block, track, sector );
            return MZDSK_RES_FORMAT_ERROR;
        }
    }

    const char *inv_txt = ( force_inv < 0 ) ? "forced NOT inverted" : "inverted";

    /* Textový popisek režimu znakové sady */
    const char *cnv_txt;
    switch ( dump_charset ) {
        case MZDSK_HEXDUMP_CHARSET_RAW:     cnv_txt = "ASCII"; break;
        case MZDSK_HEXDUMP_CHARSET_EU:      cnv_txt = "SharpASCII-EU"; break;
        case MZDSK_HEXDUMP_CHARSET_JP:      cnv_txt = "SharpASCII-JP"; break;
        case MZDSK_HEXDUMP_CHARSET_UTF8_EU: cnv_txt = "SharpUTF8-EU"; break;
        case MZDSK_HEXDUMP_CHARSET_UTF8_JP: cnv_txt = "SharpUTF8-JP"; break;
        default:                            cnv_txt = "ASCII"; break;
    }

    printf ( "\nDump %d bytes from FSMZ block %d (track: %d, sector: %d) - %s, %s:\n\n", FSMZ_SECTOR_SIZE, block, track, sector, cnv_txt, inv_txt );

    return hexdump_sector ( disc, track, sector, 0, force_inv, dump_charset );
}


/**
 * @brief Vstupní bod nástroje mzdsk-info.
 *
 * Zpracuje argumenty příkazové řádky, otevře DSK obraz v režimu
 * pouze pro čtení, provede požadovaný příkaz a disk zavře.
 *
 * Podporované příkazy:
 * - bez argumentů: základní info o disku
 * - --map: mapa obsazenosti disku
 * - --boot: informace o bootstrap
 * - --sector T S: hexdump sektoru
 * - --block N: hexdump FSMZ bloku
 * - --version: verze nástroje
 * - --lib-versions: verze knihoven
 *
 * Příkazy --map, --boot, --sector a --block se vzájemně vylučují.
 * Zadání více příkazů najednou vrátí chybu.
 *
 * U příkazů --sector a --block se provádí validace rozsahu:
 * - číslo sektoru musí být >= 1 (sektory jsou 1-based)
 * - číslo stopy musí být v rozsahu 0 .. total_tracks-1
 * - číslo sektoru nesmí přesáhnout počet sektorů na dané stopě
 *
 * Formát --output json/csv není podporován pro hexdump příkazy
 * (--sector, --block). Pokud je zadán, vypíše se varování na stderr
 * a pokračuje se s textovým výstupem.
 *
 * @param argc Počet argumentů.
 * @param argv Pole argumentů.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
int main ( int argc, char *argv[] ) {

    /* Bez argumentů - zobrazit nápovědu */
    if ( argc < 2 ) {
        print_usage ( stderr, argv[0] );
        return EXIT_FAILURE;
    }

    /* Definice dlouhých voleb pro getopt_long */
    static struct option long_options[] = {
        { "output",       required_argument, NULL, 'o' },
        { "charset",      required_argument, NULL, 'E' },
        { "map",          no_argument,       NULL, 'm' },
        { "boot",         no_argument,       NULL, 'b' },
        { "sector",       required_argument, NULL, 's' },
        { "block",        required_argument, NULL, 'B' },
        { "cnv",          no_argument,       NULL, 'c' },
        { "nocnv",        no_argument,       NULL, 'C' },
        { "dump-charset", required_argument, NULL, 'D' },
        { "inv",          no_argument,       NULL, 'i' },
        { "noinv",        no_argument,       NULL, 'I' },
        { "version",      no_argument,       NULL, 'V' },
        { "lib-versions", no_argument,       NULL, 'L' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL,           0,                 NULL,  0  }
    };

    /* Parsování voleb přes getopt_long */
    en_MZDSK_INFO_CMD cmd = CMD_INFO;
    en_OUTFMT output_format = OUTFMT_TEXT;
    en_MZDSK_HEXDUMP_CHARSET dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
    int force_inv = 0;   /* 0=auto, 1=zapnuto, -1=vypnuto */
    uint8_t sector_track = 0;
    uint8_t sector_id = 0;
    uint16_t block_num = 0;

    optind = 1;
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "o:h", long_options, NULL ) ) != -1 ) {

        switch ( opt ) {

            case 'V': /* --version */
                printf ( "mzdsk-info %s (%s %s)\n",
                         MZDSK_INFO_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
                return EXIT_SUCCESS;

            case 'L': /* --lib-versions */
                print_lib_versions();
                return EXIT_SUCCESS;

            case 'h': /* --help */
                print_usage ( stdout, argv[0] );
                return EXIT_SUCCESS;

            case 'o': /* --output FMT, -o FMT */
                if ( outfmt_parse ( optarg, &output_format ) != 0 ) {
                    fprintf ( stderr, "Error: Unknown output format '%s' (use text, json or csv)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;

            case 'E': { /* --charset MODE */
                en_MZF_NAME_ENCODING enc;
                if ( strcmp ( optarg, "eu" ) == 0 )      { enc = MZF_NAME_ASCII_EU; }
                else if ( strcmp ( optarg, "jp" ) == 0 )      { enc = MZF_NAME_ASCII_JP; }
                else if ( strcmp ( optarg, "utf8-eu" ) == 0 ) { enc = MZF_NAME_UTF8_EU; }
                else if ( strcmp ( optarg, "utf8-jp" ) == 0 ) { enc = MZF_NAME_UTF8_JP; }
                else {
                    fprintf ( stderr, "Error: Unknown charset '%s' (use eu, jp, utf8-eu or utf8-jp)\n", optarg );
                    return EXIT_FAILURE;
                }
                g_name_encoding = enc;
                break;
            }

            case 'm': /* --map */
                if ( cmd != CMD_INFO ) {
                    fprintf ( stderr, "Error: only one command allowed (--map, --boot, --sector, --block)\n" );
                    return EXIT_FAILURE;
                }
                cmd = CMD_MAP;
                break;

            case 'b': /* --boot */
                if ( cmd != CMD_INFO ) {
                    fprintf ( stderr, "Error: only one command allowed (--map, --boot, --sector, --block)\n" );
                    return EXIT_FAILURE;
                }
                cmd = CMD_BOOT;
                break;

            case 's': { /* --sector T S (track jako optarg, sektor jako další argument) */
                if ( cmd != CMD_INFO ) {
                    fprintf ( stderr, "Error: only one command allowed (--map, --boot, --sector, --block)\n" );
                    return EXIT_FAILURE;
                }
                cmd = CMD_SECTOR;
                char *endptr;
                long val_track = strtol ( optarg, &endptr, 10 );
                if ( *endptr != '\0' || val_track < 0 || val_track > 255 ) {
                    fprintf ( stderr, "Error: Invalid track number '%s' (valid range: 0-255)\n", optarg );
                    return EXIT_FAILURE;
                }
                sector_track = (uint8_t) val_track;

                /* Druhý argument --sector: číslo sektoru */
                if ( optind >= argc ) {
                    fprintf ( stderr, "Error: --sector requires two arguments: <track> <sector>\n" );
                    return EXIT_FAILURE;
                }
                long val_sector = strtol ( argv[optind], &endptr, 10 );
                if ( *endptr != '\0' || val_sector < 0 || val_sector > 255 ) {
                    fprintf ( stderr, "Error: Invalid sector number '%s' (valid range: 0-255)\n", argv[optind] );
                    return EXIT_FAILURE;
                }
                sector_id = (uint8_t) val_sector;
                optind++;
                break;
            }

            case 'B': { /* --block N */
                if ( cmd != CMD_INFO ) {
                    fprintf ( stderr, "Error: only one command allowed (--map, --boot, --sector, --block)\n" );
                    return EXIT_FAILURE;
                }
                cmd = CMD_BLOCK;
                char *endptr;
                errno = 0;
                long val_block = strtol ( optarg, &endptr, 10 );
                /* Horní hranice 65535 je maximum datového typu uint16_t, další
                 * validace proti skutečnému disc->max_block proběhne po
                 * otevření disku v cmd_show_block() (audit L-6). */
                if ( *endptr != '\0' || val_block < 0 || val_block > 65535 || errno == ERANGE ) {
                    fprintf ( stderr, "Error: Invalid block number '%s' (valid range: 0-65535)\n", optarg );
                    return EXIT_FAILURE;
                }
                block_num = (uint16_t) val_block;
                break;
            }

            case 'c': /* --cnv */
                dump_charset = MZDSK_HEXDUMP_CHARSET_EU;
                break;

            case 'C': /* --nocnv */
                dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
                break;

            case 'D': { /* --dump-charset MODE */
                if ( strcmp ( optarg, "raw" ) == 0 )          dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
                else if ( strcmp ( optarg, "eu" ) == 0 )      dump_charset = MZDSK_HEXDUMP_CHARSET_EU;
                else if ( strcmp ( optarg, "jp" ) == 0 )      dump_charset = MZDSK_HEXDUMP_CHARSET_JP;
                else if ( strcmp ( optarg, "utf8-eu" ) == 0 ) dump_charset = MZDSK_HEXDUMP_CHARSET_UTF8_EU;
                else if ( strcmp ( optarg, "utf8-jp" ) == 0 ) dump_charset = MZDSK_HEXDUMP_CHARSET_UTF8_JP;
                else {
                    fprintf ( stderr, "Error: Unknown dump-charset '%s' (use raw, eu, jp, utf8-eu or utf8-jp)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;
            }

            case 'i': /* --inv */
                force_inv = 1;
                break;

            case 'I': /* --noinv */
                force_inv = -1;
                break;

            default: /* neznámá volba - getopt_long již vypsal chybu */
                print_usage ( stderr, argv[0] );
                return EXIT_FAILURE;
        }
    }

    /* DSK soubor je první poziční argument po volbách */
    if ( optind >= argc ) {
        fprintf ( stderr, "Error: DSK file required\n\n" );
        print_usage ( stderr, argv[0] );
        return EXIT_FAILURE;
    }

    char *dsk_filename = argv[optind++];

    /* Kontrola přebytečných pozičních argumentů */
    if ( optind < argc ) {
        fprintf ( stderr, "Error: unexpected argument '%s'\n", argv[optind] );
        return EXIT_FAILURE;
    }

    /* Otevření DSK obrazu v režimu čtení */
    st_MZDSK_DISC disc;
    en_MZDSK_RES err = mzdsk_disc_open ( &disc, dsk_filename, FILE_DRIVER_OPMODE_RO );
    if ( err != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error: can't open DSK file '%s': %s\n", dsk_filename, mzdsk_get_error ( err ) );
        return EXIT_FAILURE;
    }

    /* Provedení požadovaného příkazu */
    en_MZDSK_RES result = MZDSK_RES_OK;

    switch ( cmd ) {

        case CMD_INFO:
            result = cmd_show_info ( &disc, output_format );
            break;

        case CMD_MAP:
            result = cmd_show_map ( &disc, output_format );
            break;

        case CMD_BOOT:
            result = cmd_show_boot ( &disc, output_format );
            break;

        case CMD_SECTOR:
            /* INFO-1: varování pokud uživatel požaduje nepodporovaný formát */
            if ( output_format != OUTFMT_TEXT ) {
                fprintf ( stderr, "Warning: --output format is not supported for hexdump, using text\n" );
            }
            /* INFO-2: validace rozsahu track před čtením */
            if ( sector_track >= disc.tracks_rules->total_tracks ) {
                fprintf ( stderr, "Error: track %d out of range (0-%d)\n",
                         sector_track, disc.tracks_rules->total_tracks - 1 );
                result = MZDSK_RES_FORMAT_ERROR;
                break;
            }
            if ( sector_id < 1 ) {
                fprintf ( stderr, "Error: sector number must be >= 1\n" );
                result = MZDSK_RES_FORMAT_ERROR;
                break;
            }
            /* BUG-INFO-001: ověřit, že zadané ID sektoru je v sinfo tabulce
               dané stopy. Dřívější validace porovnávala jen POČET sektorů
               (rule->sectors), což je špatně u stop s custom sector mapou
               (např. lemmings track 16: 10 sektorů, ale ID = {1,6,2,7,3,
               8,4,9,5,21}, takže ID=21 platně existuje a ID=10 neexistuje). */
            {
                st_DSK_TRACK_HEADER_INFO thi;
                if ( dsk_tools_read_track_header_info ( disc.handler, sector_track, &thi ) != EXIT_SUCCESS ) {
                    fprintf ( stderr, "Error: cannot read track %d header info\n", sector_track );
                    result = MZDSK_RES_DSK_ERROR;
                    break;
                }
                int found = 0;
                for ( int si = 0; si < thi.sectors; si++ ) {
                    if ( thi.sinfo[si].sector == sector_id ) {
                        found = 1;
                        break;
                    }
                }
                if ( !found ) {
                    fprintf ( stderr, "Error: sector ID %d not found on track %d (available IDs:",
                             sector_id, sector_track );
                    for ( int si = 0; si < thi.sectors; si++ ) {
                        fprintf ( stderr, " %d", thi.sinfo[si].sector );
                    }
                    fprintf ( stderr, ")\n" );
                    result = MZDSK_RES_FORMAT_ERROR;
                    break;
                }
            }
            result = hexdump_sector ( &disc, sector_track, sector_id, 1, force_inv, dump_charset );
            break;

        case CMD_BLOCK:
            /* INFO-1: varování pokud uživatel požaduje nepodporovaný formát */
            if ( output_format != OUTFMT_TEXT ) {
                fprintf ( stderr, "Warning: --output format is not supported for hexdump, using text\n" );
            }
            result = cmd_show_block ( &disc, block_num, force_inv, dump_charset );
            break;

        default:
            fprintf ( stderr, "Error: unknown command\n" );
            result = MZDSK_RES_UNKNOWN_ERROR;
            break;
    }

    /* Uzavření DSK obrazu */
    mzdsk_disc_close ( &disc );

    if ( result != MZDSK_RES_OK ) {
        /* FORMAT_ERROR již byl vypsán specificky přímo command handlerem */
        if ( result != MZDSK_RES_FORMAT_ERROR ) {
            fprintf ( stderr, "Error: %s\n", mzdsk_get_error ( result ) );
        }
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
