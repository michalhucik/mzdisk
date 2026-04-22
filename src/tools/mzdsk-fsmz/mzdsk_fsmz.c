/**
 * @file   mzdsk_fsmz.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  CLI nástroj pro kompletní správu souborového systému FSMZ na DSK obrazech.
 *
 * Portováno z fstool_fsmz.c. Podporuje všechny operace nad FSMZ filesystémem:
 * - Výpis adresáře (dir, dir --type)
 * - Informace o souboru (file, file --id)
 * - Extrakce souborů do MZF (get, get --id, get --all)
 * - Vložení MZF souboru (put)
 * - Smazání souboru (era, era --id)
 * - Přejmenování souboru (ren, ren --id)
 * - Zamčení/odemčení souboru (lock)
 * - Změna souborového typu (chtype --id)
 * - Správa bootstrap (boot, boot put, boot get, boot clear, boot mini/bottom)
 * - Surový přístup k blokům (dump-block, get-block, put-block)
 * - Údržba (format, repair, defrag)
 *
 * Globální volby:
 * - --ipldisk        - IPLDISK rozšířený adresář (127 položek místo 63)
 * - --ro             - vynucený read-only režim
 * - --output FMT     - výstupní formát: text (výchozí), json, csv
 * - --charset MODE   - konverze Sharp MZ znakové sady: eu (výchozí), jp, utf8-eu, utf8-jp
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2018-2026 Michal Hucik <hucik@ordoz.com>
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
#include <stdbool.h>
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
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/mzdsk_hexdump/mzdsk_hexdump.h"
#include "libs/output_format/output_format.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/**
 * @brief Globální nastavení konverze Sharp MZ znakové sady.
 *
 * Nastavuje se jednou při startu z volby --charset.
 * Výchozí hodnota je MZF_NAME_ASCII_EU (jednobajtová konverze z evropské znakové sady).
 */
static en_MZF_NAME_ENCODING g_name_encoding = MZF_NAME_ASCII_EU;
#include "libs/dsk/dsk_tools.h"
#include "tools/common/mzdisk_cli_version.h"


/** @brief Verze nástroje mzdsk-fsmz. */
#define MZDSK_FSMZ_VERSION "1.14.2"


/** @brief Globální flag `--overwrite` (audit M-16). */
static int g_allow_overwrite = 0;


/** @brief Globální flag `--yes` (audit M-17). */
static int g_assume_yes = 0;


/**
 * @brief Interaktivní potvrzení destruktivní operace (audit M-17).
 *
 * @param op_name  Popis operace.
 * @return 1 pokud uživatel potvrdil (nebo --yes/non-TTY), 0 pokud odmítl.
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
 * @brief Ověří, zda ASCII jméno nebude při konverzi na MZ fname oříznuto.
 *
 * `fsmz_tool_convert_ascii_to_mzfname` tiše zkopíruje jen prvních
 * `length-1` znaků (viz audit L-14). Tato funkce vypíše warning na
 * stderr, pokud je zdrojový řetězec delší - uživatel pak ví, že jméno
 * bude oříznuto.
 *
 * @param filename ASCII jméno, které se bude konvertovat.
 * @return 0 vždy (pouze informační, konverze proběhne v každém případě).
 */
static int warn_fname_truncation ( const char *filename ) {
    if ( filename == NULL ) return 0;
    size_t src_len = strlen ( filename );
    /* FSMZ_FNAME_LENGTH je kapacita včetně 0x0d terminátoru, tj. 16
     * znaků použitelných pro jméno. */
    if ( src_len > FSMZ_FNAME_LENGTH - 1 ) {
        fprintf ( stderr, "Warning: filename '%s' is longer than %d characters "
                          "and will be truncated.\n",
                          filename, FSMZ_FNAME_LENGTH - 1 );
    }
    return 0;
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


/* =========================================================================
 * Strategie řešení duplicitních jmen při extrakci
 * ========================================================================= */

/**
 * @brief Strategie řešení duplicit při `get --all`.
 *
 * Určuje chování, pokud cílový .mzf soubor už v hostitelském adresáři
 * existuje. Sjednoceno se vzorem z `mzdsk-cpm --on-duplicate`.
 */
typedef enum en_DUPLICATE_MODE {
    DUP_RENAME    = 0,    /**< Přidá suffix ~N před ".mzf" (FOO~2.mzf, ~3...). Default. */
    DUP_OVERWRITE = 1,    /**< Přepíše existující soubor bez varování. */
    DUP_SKIP      = 2,    /**< Přeskočí soubor s varováním na stderr. */
} en_DUPLICATE_MODE;


/* =========================================================================
 * Typ bootstrapu
 * ========================================================================= */

/**
 * @brief Typ bootstrapu pro FSMZ disk.
 *
 * Určuje, kam se bootstrap uloží:
 * - MINI: bloky 1-14 (FSMZ kompatibilní - DINFO/dir nedotčeny)
 * - BOTTOM: bloky 1-15 nebo více (s --no-fsmz-compat celý disk)
 * - NORMAL: do souborové oblasti FAREA, klasický FSMZ boot
 * - OVER: nad FAREA oblast (experimentální)
 */
typedef enum en_BOOTSTRAP_TYPE {
    BOOTSTRAP_MINI = 0, /**< Mini boot uložený do bloků 1-14 (FSMZ kompatibilní) */
    BOOTSTRAP_BOTTOM,   /**< Bottom boot - jako mini, ale s volitelným překročením bloku 14 */
    BOOTSTRAP_NORMAL,   /**< Klasický FSMZ boot umístěný ve FAREA */
    BOOTSTRAP_OVER,     /**< FSMZ boot umístěný nad FAREA (pro disky s >128 stopami) */
} en_BOOTSTRAP_TYPE;


/* =========================================================================
 * Režim výpisu adresáře
 * ========================================================================= */

/**
 * @brief Režim filtrace při výpisu adresáře.
 */
typedef enum en_PRINTDIR_MODE {
    PRINTDIR_VALID = 0, /**< Zobrazit všechny platné (nesmazané) položky */
    PRINTDIR_FTYPE      /**< Zobrazit pouze položky s konkrétním ftype */
} en_PRINTDIR_MODE;


/* =========================================================================
 * Pomocné výpisové funkce
 * ========================================================================= */


/**
 * @brief Vytiskne jméno souboru ve formátu Sharp MZ na stdout.
 *
 * Konvertuje jméno ze Sharp MZ ASCII podle globálního nastavení
 * g_name_encoding (--charset). Pro ASCII režimy tiskne po znacích,
 * pro UTF-8 konvertuje do bufferu a vytiskne najednou.
 * Tisk končí při znaku < 0x20 (typicky terminátor 0x0d).
 *
 * @param fname Ukazatel na jméno souboru v Sharp MZ ASCII. Nesmí být NULL.
 *
 * @pre fname ukazuje na platný buffer s alespoň jedním bajtem < 0x20.
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
 * @brief Vytiskne detailní informace o položce adresáře.
 *
 * Výpis zahrnuje jméno (v uvozovkách), typ, příznak uzamčení,
 * startovací/spouštěcí adresu, velikost v bajtech/kB a počet bloků.
 *
 * @param diritem Ukazatel na položku adresáře. Nesmí být NULL.
 */
static void print_diritem ( st_FSMZ_DIR_ITEM *diritem ) {
    printf ( "\"" );
    print_filename ( diritem->fname );
    printf ( "\"\n" );
    printf ( "\ttype = 0x%02x, locked: %d, start = 0x%04x, size = 0x%04x, exec = 0x%04x\n",
             diritem->ftype, diritem->locked, diritem->fstrt, diritem->fsize, diritem->fexec );

    printf ( "\tblock = %d, size = ", diritem->block );

    if ( diritem->fsize < 1024 ) {
        printf ( "%d B", diritem->fsize );
    } else {
        float realsize = (float) diritem->fsize / 1024;
        printf ( "%0.2f kB ", realsize );
    }

    printf ( " ( %d blocks )\n\n", fsmz_blocks_from_size ( diritem->fsize ) );
}


/**
 * @brief Konvertuje jméno souboru ze Sharp MZ ASCII do null-terminated stringu.
 *
 * Konvertuje podle globálního nastavení g_name_encoding (--charset).
 * Pro ASCII režimy konvertuje po znacích, pro UTF-8 volá sharpmz_str_to_utf8().
 * Konverze končí při znaku < 0x20 (typicky terminátor 0x0d).
 *
 * @param[in]  fname    Ukazatel na jméno v Sharp MZ ASCII. Nesmí být NULL.
 * @param[out] dst      Výstupní buffer. Nesmí být NULL.
 *                      Pro ASCII stačí FSMZ_FNAME_LENGTH + 1 (18 bajtů).
 *                      Pro UTF-8 doporučeno MZF_FNAME_UTF8_BUF_SIZE (69 bajtů).
 * @param[in]  dst_size Velikost výstupního bufferu.
 *
 * @post dst obsahuje null-terminated řetězec v ASCII nebo UTF-8.
 */
static void fsmz_fname_to_str ( const uint8_t *fname, char *dst, size_t dst_size ) {
    /* Ochrana proti OOB čtení: viz komentář v print_filename(). */
    if ( g_name_encoding == MZF_NAME_ASCII_EU || g_name_encoding == MZF_NAME_ASCII_JP ) {
        size_t i = 0;
        while ( i < FSMZ_FNAME_LENGTH && i < dst_size - 1 && fname[i] >= 0x20 ) {
            dst[i] = (char) ( ( g_name_encoding == MZF_NAME_ASCII_JP )
                                ? sharpmz_jp_cnv_from ( fname[i] )
                                : sharpmz_cnv_from ( fname[i] ) );
            i++;
        }
        dst[i] = '\0';
    } else {
        /* UTF-8 režim - zjistit délku, konvertovat */
        uint8_t len = 0;
        while ( len < FSMZ_FNAME_LENGTH && fname[len] >= 0x20 ) len++;

        sharpmz_charset_t charset = ( g_name_encoding == MZF_NAME_UTF8_JP )
                                      ? SHARPMZ_CHARSET_JP : SHARPMZ_CHARSET_EU;
        sharpmz_str_to_utf8 ( fname, len, dst, dst_size, charset );
    }
}


/**
 * @brief Vytiskne informace o IPLPRO hlavičce bootstrapu.
 *
 * Výpis zahrnuje jméno, typ, startovací/spouštěcí adresu, velikost
 * a číslo počátečního bloku.
 *
 * @param iplpro Ukazatel na IPLPRO blok. Nesmí být NULL.
 */
static void print_iplpro_header ( st_FSMZ_IPLPRO_BLOCK *iplpro ) {
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
 * @brief Vytiskne informace z MZF hlavičky.
 *
 * Výpis zahrnuje jméno, typ, adresy, velikost v bajtech/kB a počet bloků.
 *
 * @param mzfhdr Ukazatel na MZF hlavičku. Nesmí být NULL.
 */
static void print_mzfhdr ( st_MZF_HEADER *mzfhdr ) {
    printf ( "\"" );
    print_filename ( MZF_UINT8_FNAME ( mzfhdr->fname ) );
    printf ( "\"\n" );
    printf ( "\ttype = 0x%02x, start = 0x%04x, size = 0x%04x, exec = 0x%04x\n",
             mzfhdr->ftype, mzfhdr->fstrt, mzfhdr->fsize, mzfhdr->fexec );

    printf ( "\tsize = " );

    if ( mzfhdr->fsize < 1024 ) {
        printf ( "%d B", mzfhdr->fsize );
    } else {
        float realsize = (float) mzfhdr->fsize / 1024;
        printf ( "%0.2f kB ", realsize );
    }

    printf ( " ( %d blocks )\n\n", fsmz_blocks_from_size ( mzfhdr->fsize ) );
}


/* =========================================================================
 * Operace: informace o disku
 * ========================================================================= */


/**
 * @brief Zobrazí informace o FSMZ disku (celková/obsazená/volná kapacita).
 *
 * Přečte DINFO blok z disku a zobrazí statistiky v blocích a kB.
 * Funkce se volá automaticky pro každý textový subkomand jako hlavička
 * výstupu.
 *
 * Pokud disk není v plném FSMZ formátu (např. CP/M, MRS, nebo jen boot
 * track), vypíše informativní hlášku a vrátí MZDSK_RES_OK - dispatch
 * pokračuje, protože operace jako `boot*` a raw block mají smysl i na
 * ne-FSMZ discích. Subkomandy vyžadující full FSMZ si to řeší vlastní
 * kontrolou.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu nebo ne-FSMZ disku, jinak chybový kód I/O.
 */
static en_MZDSK_RES cmd_print_disc_info ( st_MZDSK_DISC *disc ) {
    st_FSMZ_DINFO_BLOCK dinfo;
    en_MZDSK_RES err;

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nFSMZ Disk info is not available on this DSK image.\n\n" );
        return MZDSK_RES_OK;
    }

    printf ( "\nFSMZ Disk info:\n" );

    err = fsmz_read_dinfo ( disc, &dinfo );
    if ( err ) return err;

    printf ( "\tTotal disk size: %d blocks ( %d kB )\n", dinfo.blocks + 1,
             (int) ( fsmz_size_from_blocks ( dinfo.blocks + 1 ) / 1024 ) );
    printf ( "\tUsed: %d blocks ( %d kB )\n", dinfo.used,
             (int) ( fsmz_size_from_blocks ( dinfo.used ) / 1024 ) );
    if ( dinfo.used > dinfo.blocks + 1 ) {
        printf ( "\tFree: 0 blocks ( 0 kB ) - disk is too small for FSMZ metadata (%d blocks needed)\n", dinfo.used );
    } else {
        printf ( "\tFree: %d blocks ( %d kB )\n", ( dinfo.blocks + 1 - dinfo.used ),
                 (int) ( fsmz_size_from_blocks ( dinfo.blocks + 1 - dinfo.used ) / 1024 ) );
    }
    printf ( "\n" );
    return MZDSK_RES_OK;
}


/* =========================================================================
 * Operace: výpis adresáře
 * ========================================================================= */


/**
 * @brief Zobrazí obsah adresáře FSMZ s volitelným filtrováním podle typu.
 *
 * Podle režimu pdir_mode buď zobrazí všechny platné (nesmazané) položky,
 * nebo jen položky s konkrétním ftype.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param pdir_mode Režim filtrace (PRINTDIR_VALID nebo PRINTDIR_FTYPE).
 * @param ftype Souborový typ pro filtraci (použije se jen při PRINTDIR_FTYPE).
 * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_print_dir ( st_MZDSK_DISC *disc, en_PRINTDIR_MODE pdir_mode,
                                     uint8_t ftype, uint8_t fsmz_dir_items,
                                     en_OUTFMT output_format ) {

    st_FSMZ_DIR dir;
    st_FSMZ_DIR_ITEM *diritem;
    en_MZDSK_RES err;
    uint8_t item_id = 0;
    uint8_t found_items = 0;

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        fprintf ( stderr, "Error: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    if ( output_format == OUTFMT_TEXT ) {
        if ( pdir_mode == PRINTDIR_VALID ) {
            printf ( "\nFSMZ Read directory items:\n\n" );
        } else {
            printf ( "\nFSMZ Read directory items (ftype = 0x%02x):\n\n", ftype );
        }
    }

    st_OUTFMT_CTX ctx;
    outfmt_init ( &ctx, output_format );

    if ( output_format != OUTFMT_TEXT ) {
        static const char *csv_hdr[] = {
            "id", "name", "type", "locked", "start", "size", "exec", "block", "block_count"
        };
        outfmt_csv_header ( &ctx, csv_hdr, 9 );

        outfmt_doc_begin ( &ctx );
        outfmt_kv_str ( &ctx, "filesystem", "fsmz" );
        outfmt_array_begin ( &ctx, "files" );
    }

    err = fsmz_open_dir ( disc, &dir );
    if ( err ) return err;

    while ( 1 ) {
        diritem = fsmz_read_dir ( disc, &dir, fsmz_dir_items, &err );
        if ( err ) break;

        int show = 0;
        if ( pdir_mode == PRINTDIR_VALID ) {
            show = ( diritem->ftype != 0 );
        } else {
            show = ( diritem->ftype == ftype );
        }

        if ( show ) {
            if ( output_format == OUTFMT_TEXT ) {
                printf ( "id = %d - ", item_id );
                print_diritem ( diritem );
            } else {
                char name_str[MZF_FNAME_UTF8_BUF_SIZE];
                fsmz_fname_to_str ( diritem->fname, name_str, sizeof ( name_str ) );

                outfmt_item_begin ( &ctx );
                outfmt_field_int ( &ctx, "id", item_id );
                outfmt_field_str ( &ctx, "name", name_str );
                outfmt_field_hex8 ( &ctx, "type", diritem->ftype );
                outfmt_field_bool ( &ctx, "locked", diritem->locked );
                outfmt_field_hex16 ( &ctx, "start", diritem->fstrt );
                outfmt_field_uint ( &ctx, "size", (unsigned long) diritem->fsize );
                outfmt_field_hex16 ( &ctx, "exec", diritem->fexec );
                outfmt_field_int ( &ctx, "block", diritem->block );
                outfmt_field_int ( &ctx, "block_count", fsmz_blocks_from_size ( diritem->fsize ) );
                outfmt_item_end ( &ctx );
            }
            found_items++;
        }
        item_id++;
    }
    if ( MZDSK_RES_IS_FATAL ( err ) ) return err;

    if ( output_format == OUTFMT_TEXT ) {
        if ( found_items ) {
            printf ( "\nCOUNT ITEMS: %d\n\n", found_items );
        } else {
            printf ( "NO ITEMS FOUND!\n\n" );
        }
    } else {
        outfmt_array_end ( &ctx );
        outfmt_kv_int ( &ctx, "total_files", found_items );
        outfmt_doc_end ( &ctx );
    }

    return MZDSK_RES_OK;
}


/* =========================================================================
 * Operace: informace o souboru
 * ========================================================================= */


/**
 * @brief Zobrazí informace o souboru podle jména nebo ID.
 *
 * Vyhledá soubor v adresáři podle jména (filename != NULL) nebo
 * podle ID (filename == NULL) a zobrazí jeho detaily. Při hledání
 * podle jména se skutečné ID souboru zjistí z pozice v adresáři.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param filename ASCII jméno souboru (NULL = hledání podle item_id).
 * @param item_id ID souboru (použije se jen pokud filename == NULL).
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param output_format Výstupní formát (text, json, csv).
 *
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_NOT_FOUND pokud soubor
 *         neexistuje, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_print_fileinfo ( st_MZDSK_DISC *disc, char *filename, uint8_t item_id,
                                          uint8_t fsmz_dir_items, en_OUTFMT output_format ) {
    st_FSMZ_DIR_ITEM *diritem;
    en_MZDSK_RES err;

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        fprintf ( stderr, "Error: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    if ( output_format == OUTFMT_TEXT ) {
        if ( filename != NULL ) {
            printf ( "\nFSMZ Search file name: %s - ", filename );
        } else {
            printf ( "\nFSMZ Search file ID: %d - ", item_id );
        }
    }

    if ( filename != NULL ) {
        st_FSMZ_DIR dir_cache;
        diritem = fsmz_tool_get_diritem_pointer_and_dir_by_name ( disc, filename, &dir_cache, fsmz_dir_items, &err );
        /* Pozice v adresáři je 1-based a fsmz_read_dir() ji post-inkrementuje, ID je 0-based */
        if ( diritem != NULL ) {
            item_id = (uint8_t) ( dir_cache.position - 2 );
        }
    } else {
        diritem = fsmz_tool_get_diritem_pointer_and_dir_by_id ( disc, item_id, NULL, fsmz_dir_items, &err );
    }

    if ( MZDSK_RES_IS_FATAL ( err ) ) return err;

    if ( diritem != NULL && diritem->ftype != 0x00 ) {
        if ( output_format == OUTFMT_TEXT ) {
            printf ( "OK\n\n" );
            print_diritem ( diritem );
        } else {
            /* Buffer musí pokrýt i UTF-8 režim (audit M-30):
             * FSMZ_FNAME_LENGTH + 1 = 18 B stačí jen pro ASCII.
             * V UTF-8 je potřeba MZF_FNAME_UTF8_BUF_SIZE (69 B)
             * pro vícebajtové sekvence. */
            char name_str[MZF_FNAME_UTF8_BUF_SIZE];
            fsmz_fname_to_str ( diritem->fname, name_str, sizeof ( name_str ) );

            st_OUTFMT_CTX ctx;
            outfmt_init ( &ctx, output_format );

            static const char *csv_hdr[] = {
                "id", "name", "type", "locked", "start", "size", "exec", "block", "block_count"
            };
            outfmt_csv_header ( &ctx, csv_hdr, 9 );

            outfmt_doc_begin ( &ctx );
            outfmt_kv_str ( &ctx, "filesystem", "fsmz" );

            outfmt_array_begin ( &ctx, "files" );
            outfmt_item_begin ( &ctx );
            outfmt_field_int ( &ctx, "id", item_id );
            outfmt_field_str ( &ctx, "name", name_str );
            outfmt_field_hex8 ( &ctx, "type", diritem->ftype );
            outfmt_field_bool ( &ctx, "locked", diritem->locked );
            outfmt_field_hex16 ( &ctx, "start", diritem->fstrt );
            outfmt_field_uint ( &ctx, "size", (unsigned long) diritem->fsize );
            outfmt_field_hex16 ( &ctx, "exec", diritem->fexec );
            outfmt_field_int ( &ctx, "block", diritem->block );
            outfmt_field_int ( &ctx, "block_count", fsmz_blocks_from_size ( diritem->fsize ) );
            outfmt_item_end ( &ctx );
            outfmt_array_end ( &ctx );

            outfmt_doc_end ( &ctx );
        }
    } else {
        if ( output_format == OUTFMT_TEXT ) {
            printf ( "NOT FOUND\n\n" );
        } else {
            fprintf ( stderr, "Error: File not found\n" );
        }
        return MZDSK_RES_FILE_NOT_FOUND;
    }

    return MZDSK_RES_OK;
}


/* =========================================================================
 * Operace: extrakce souborů do MZF
 * ========================================================================= */


/**
 * @brief Zapíše tělo MZF souboru z FSMZ bloků do handleru.
 *
 * Přečte data z alokačních bloků disku a zapíše je jako tělo MZF souboru.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param block Číslo počátečního alokačního bloku.
 * @param fsize Velikost dat v bajtech.
 * @param handler Cílový handler pro zápis MZF těla.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES write_mzf_body ( st_MZDSK_DISC *disc, uint16_t block, uint16_t fsize, st_HANDLER *handler ) {
    /* Heap místo stacku: 64 kB na stacku na MSYS2/Windows 1 MB stacku
     * na hraně, zejména v řetězech volání. Audit M-24. */
    uint8_t *body = malloc ( MZF_MAX_BODY_SIZE );
    if ( !body ) return MZDSK_RES_UNKNOWN_ERROR;
    en_MZDSK_RES err = fsmz_read_blocks ( disc, block, fsize, body );
    if ( err ) { free ( body ); return err; }
    if ( EXIT_FAILURE == mzf_write_body ( handler, body, fsize ) ) {
        free ( body );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    free ( body );
    return MZDSK_RES_OK;
}


/**
 * @brief Vytvoří MZF soubor v paměti z FSMZ bloku.
 *
 * Alokuje paměťový handler, zapíše do něj MZF hlavičku a tělo
 * načtené z FSMZ bloků.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param mzfhdr Ukazatel na MZF hlavičku. Nesmí být NULL.
 * @param block Číslo počátečního alokačního bloku dat.
 * @param[out] err Výstupní chybový kód.
 *
 * @return Ukazatel na paměťový handler s MZF daty, nebo NULL při chybě.
 *
 * @note Volající je zodpovědný za uvolnění handleru (generic_driver_close + free).
 */
static st_HANDLER* create_mem_mzf_from_block ( st_MZDSK_DISC *disc, st_MZF_HEADER *mzfhdr, uint16_t block, en_MZDSK_RES *err ) {

    st_HANDLER *handler = generic_driver_open_memory ( NULL, &g_memory_driver_realloc, 1 );
    *err = MZDSK_RES_UNKNOWN_ERROR;

    if ( !handler ) {
        fprintf ( stderr, "Error: cannot allocate in-memory MZF buffer\n" );
        return NULL;
    }

    handler->spec.memspec.swelling_enabled = 1;

    if ( EXIT_SUCCESS != mzf_write_header ( handler, mzfhdr ) ) {
        fprintf ( stderr, "Error: cannot build MZF header in memory\n" );
        generic_driver_close ( handler );
        free ( handler );
        return NULL;
    }

    *err = write_mzf_body ( disc, block, mzfhdr->fsize, handler );
    if ( *err ) {
        fprintf ( stderr, "Error: cannot read file body from disk (block %u, size %u)\n",
                  (unsigned) block, (unsigned) mzfhdr->fsize );
        generic_driver_close ( handler );
        free ( handler );
        return NULL;
    }

    *err = MZDSK_RES_OK;
    return handler;
}


/**
 * @brief Uloží MZF soubor z FSMZ bloku na lokální disk.
 *
 * Vytvoří MZF soubor v paměti a uloží ho do souboru na disku.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param mzfhdr Ukazatel na MZF hlavičku. Nesmí být NULL.
 * @param block Číslo počátečního alokačního bloku dat.
 * @param mzf_filename Cesta k výstupnímu MZF souboru.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES save_mzf_from_block ( st_MZDSK_DISC *disc, st_MZF_HEADER *mzfhdr, uint16_t block, char *mzf_filename ) {

    printf ( "Write file '" );
    print_filename ( MZF_UINT8_FNAME ( mzfhdr->fname ) );
    printf ( "' into %s\n\n", mzf_filename );

    en_MZDSK_RES err;
    st_HANDLER *handler = create_mem_mzf_from_block ( disc, mzfhdr, block, &err );
    if ( !handler ) {
        return err;
    }

    if ( EXIT_SUCCESS != generic_driver_save_memory ( handler, mzf_filename ) ) {
        fprintf ( stderr, "Error: cannot write output file '%s': %s\n",
                  mzf_filename, strerror ( errno ) );
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_FORMAT_ERROR;
    }

    generic_driver_close ( handler );
    free ( handler );

    return MZDSK_RES_OK;
}


/**
 * @brief Extrahuje soubor z FSMZ do MZF podle jména nebo ID.
 *
 * Vyhledá soubor v adresáři, vytvoří MZF hlavičku a uloží
 * hlavičku + tělo jako MZF soubor na lokální disk.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param filename ASCII jméno souboru (NULL = hledání podle item_id).
 * @param item_id ID souboru (použije se jen pokud filename == NULL).
 * @param mzf_filename Cesta k výstupnímu MZF souboru.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_get_mzf ( st_MZDSK_DISC *disc, char *filename, uint8_t item_id, char *mzf_filename, uint8_t fsmz_dir_items ) {

    /* Audit M-16: kontrola existence výstupního souboru před čtením z disku.
     * Vracíme FORMAT_ERROR jako sentinel "chyba už byla reportovaná" -
     * main smyčka tento kód nedoplňuje generickou hláškou (BUG C4). */
    if ( check_output_overwrite ( mzf_filename ) != EXIT_SUCCESS ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    st_FSMZ_DIR_ITEM *diritem;

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    en_MZDSK_RES err;
    if ( filename != NULL ) {
        printf ( "\nFSMZ Get file name: %s - ", filename );
        diritem = fsmz_tool_get_diritem_pointer_and_dir_by_name ( disc, filename, NULL, fsmz_dir_items, &err );
    } else {
        printf ( "\nFSMZ Get file ID: %d - ", item_id );
        diritem = fsmz_tool_get_diritem_pointer_and_dir_by_id ( disc, item_id, NULL, fsmz_dir_items, &err );
    }

    if ( err ) {
        printf ( "NOT FOUND\n\n" );
        return err;
    }

    printf ( "FOUND\n\n" );

    st_MZF_HEADER *mzfhdr = mzf_tools_create_mzfhdr ( diritem->ftype, diritem->fsize, diritem->fstrt, diritem->fexec, ( uint8_t* ) &diritem->fname, FSMZ_FNAME_LENGTH - 1, NULL );
    if ( !mzfhdr ) return MZDSK_RES_UNKNOWN_ERROR;

    err = save_mzf_from_block ( disc, mzfhdr, diritem->block, mzf_filename );
    free ( mzfhdr );
    return err;
}


/**
 * @brief Zjistí, zda soubor na hostitelském disku existuje.
 *
 * @param path Cesta k souboru. Nesmí být NULL.
 * @return 1 pokud soubor existuje, 0 jinak.
 */
static int fsmz_host_file_exists ( const char *path ) {
    struct stat st;
    return ( stat ( path, &st ) == 0 ) ? 1 : 0;
}


/**
 * @brief Sestaví cílovou cestu pro jeden extrahovaný MZF a aplikuje
 *        strategii řešení duplicit.
 *
 * Výchozí cesta je `<dir>/<sanitized_name>.mzf`. Pokud soubor existuje,
 * podle `dup_mode`:
 * - DUP_OVERWRITE: cesta zůstává, soubor se přepíše.
 * - DUP_SKIP: vrací 1 (volající soubor přeskočí) a vypíše varování.
 * - DUP_RENAME: hledá první volné `~N` před `.mzf` suffixem (2..999).
 *
 * @param[out] path_buf Výstupní buffer (min MAX_MZF_PATH_LEN bajtů).
 * @param[in]  path_buf_size Velikost bufferu.
 * @param[in]  dirpath Cílový adresář.
 * @param[in]  ascii_fname Sanitizované jméno bez přípony.
 * @param[in]  dup_mode Strategie.
 * @return 0 = cesta připravena k zápisu, 1 = přeskočit, -1 = buffer overflow.
 */
static int build_mzf_output_path ( char *path_buf, size_t path_buf_size,
                                    const char *dirpath, const char *ascii_fname,
                                    en_DUPLICATE_MODE dup_mode ) {
    if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s.mzf",
                              dirpath, ascii_fname ) >= path_buf_size ) {
        return -1;
    }

    if ( !fsmz_host_file_exists ( path_buf ) ) return 0;

    switch ( dup_mode ) {
        case DUP_OVERWRITE:
            return 0;
        case DUP_SKIP:
            fprintf ( stderr, "Warning: Skipping '%s' (file exists)\n", path_buf );
            return 1;
        case DUP_RENAME: {
            for ( int n = 2; n < 1000; n++ ) {
                if ( (size_t) snprintf ( path_buf, path_buf_size, "%s/%s~%d.mzf",
                                          dirpath, ascii_fname, n ) >= path_buf_size ) {
                    return -1;
                }
                if ( !fsmz_host_file_exists ( path_buf ) ) return 0;
            }
            /* Velmi nepravděpodobné - více než 999 duplicit */
            fprintf ( stderr, "Warning: Skipping '%s' (too many duplicates)\n", path_buf );
            return 1;
        }
    }
    return 0;
}


/**
 * @brief Extrahuje všechny soubory z FSMZ do adresáře jako MZF soubory.
 *
 * Projde celý adresář a pro každou platnou (nesmazanou) položku
 * uloží odpovídající MZF soubor do zadaného adresáře. Znaky neplatné
 * na Windows (*, ?, ", <, >, |, :, \, /) se v názvech souborů
 * nahradí podtržítkem '_'. Duplicity v rámci adresáře řeší podle
 * `dup_mode` (rename/overwrite/skip, viz `--on-duplicate`).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param dirpath Cesta k cílovému adresáři.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param dup_mode Strategie řešení duplicit.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 *
 * @note Pokud cílový adresář neexistuje, funkce se ho pokusí vytvořit.
 */
static en_MZDSK_RES cmd_get_all ( st_MZDSK_DISC *disc, char *dirpath,
                                    uint8_t fsmz_dir_items, en_DUPLICATE_MODE dup_mode ) {

    printf ( "\nSave all from FSMZ into dir '%s'\n\n", dirpath );

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        fprintf ( stderr, "Error: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* pokus o vytvoření cílového adresáře (pokud již existuje, errno == EEXIST) */
    if ( _mkdir ( dirpath ) != 0 && errno != EEXIST ) {
        fprintf ( stderr, "Error: Could not create directory '%s': %s\n", dirpath, strerror ( errno ) );
        return MZDSK_RES_FORMAT_ERROR;
    }

    en_MZDSK_RES err;
    int extracted = 0;
    int skipped = 0;
    int renamed = 0;

    int i;
    for ( i = 0; i < fsmz_dir_items; i++ ) {
        st_FSMZ_DIR_ITEM *diritem = fsmz_tool_get_diritem_pointer_and_dir_by_id ( disc, i, NULL, fsmz_dir_items, &err );
        if ( err != MZDSK_RES_FILE_NOT_FOUND ) {
            if ( err ) return err;

            /* pokud se nejedná o smazanou položku, tak to uložíme */
            if ( diritem->ftype != 0x00 ) {

                st_MZF_HEADER *mzfhdr = mzf_tools_create_mzfhdr ( diritem->ftype, diritem->fsize, diritem->fstrt, diritem->fexec, ( uint8_t* ) &diritem->fname, FSMZ_FNAME_LENGTH - 1, NULL );
                if ( !mzfhdr ) return MZDSK_RES_UNKNOWN_ERROR;

                char ascii_fname[MZF_FNAME_UTF8_BUF_SIZE];
                mzf_tools_get_fname_ex ( mzfhdr, ascii_fname,
                                          sizeof ( ascii_fname ), g_name_encoding );

                /* Sanitizace znaků neplatných na Windows (*, ?, ", <, >, |, :, \, /) */
                for ( char *p = ascii_fname; *p; p++ ) {
                    if ( *p == '*' || *p == '?' || *p == '"' || *p == '<' ||
                         *p == '>' || *p == '|' || *p == ':' || *p == '\\' || *p == '/' ) {
                        *p = '_';
                    }
                }
                /* Ochrana proti path traversal (audit M-25): jméno tvořené
                 * jen tečkami a mezerami (např. ".", "..") by vedlo k cestě
                 * `dir/..` a zápisu do parent directory. */
                {
                    int only_dots_spaces = ( ascii_fname[0] != '\0' );
                    for ( char *p = ascii_fname; *p; p++ ) {
                        if ( *p != '.' && *p != ' ' ) { only_dots_spaces = 0; break; }
                    }
                    if ( only_dots_spaces && ascii_fname[0] != '\0' ) ascii_fname[0] = '_';
                }

                /* Cílová cesta s řešením duplicit (rename/overwrite/skip) */
                char mzf_filename[1024];
                int bres = build_mzf_output_path ( mzf_filename, sizeof ( mzf_filename ),
                                                    dirpath, ascii_fname, dup_mode );
                if ( bres < 0 ) {
                    free ( mzfhdr );
                    fprintf ( stderr, "Error: Output path too long for '%s'\n", ascii_fname );
                    return MZDSK_RES_FORMAT_ERROR;
                }
                if ( bres == 1 ) {
                    /* Přeskočeno (DUP_SKIP nebo příliš mnoho duplicit) */
                    free ( mzfhdr );
                    skipped++;
                    continue;
                }

                /* Detekce přejmenování - pokud finální cesta neodpovídá
                   původnímu <dir>/<ascii_fname>.mzf, soubor byl renamován. */
                char orig_path[1024];
                snprintf ( orig_path, sizeof ( orig_path ), "%s/%s.mzf",
                           dirpath, ascii_fname );
                int was_renamed = ( strcmp ( mzf_filename, orig_path ) != 0 );

                err = save_mzf_from_block ( disc, mzfhdr, diritem->block, mzf_filename );
                free ( mzfhdr );
                if ( err ) return err;

                extracted++;
                if ( was_renamed ) renamed++;
            }
        }
    }

    printf ( "\nExtracted: %d files (renamed: %d, skipped: %d)\n",
             extracted, renamed, skipped );
    return MZDSK_RES_OK;
}


/* =========================================================================
 * Operace: vložení MZF souboru
 * ========================================================================= */


/**
 * @brief Vloží MZF soubor na FSMZ disk.
 *
 * Načte MZF soubor z lokálního disku, přečte jeho hlavičku a tělo,
 * a zapíše ho do FSMZ souborového systému.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param mzf_filename Cesta k vstupnímu MZF souboru.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_put_mzf ( st_MZDSK_DISC *disc, char *mzf_filename, uint8_t fsmz_dir_items ) {

    en_MZDSK_RES err;

    printf ( "\nPut new FSMZ file %s\n\n", mzf_filename );

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        fprintf ( stderr, "Error: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* Pre-flight: soubor musí existovat, být regular file a >= MZF_HEADER_SIZE
     * (128 B). Bez S_ISREG kontroly (audit M-19) by adresář prošel velikostním
     * checkem a pak `fopen`/`fread` selhal s matoucí chybou. */
    {
        struct stat st;
        if ( stat ( mzf_filename, &st ) != 0 ) {
            fprintf ( stderr, "Error: cannot open MZF file '%s': %s\n",
                      mzf_filename, strerror ( errno ) );
            return MZDSK_RES_FORMAT_ERROR;
        }
        if ( !S_ISREG ( st.st_mode ) ) {
            fprintf ( stderr, "Error: '%s' is not a regular file\n", mzf_filename );
            return MZDSK_RES_FORMAT_ERROR;
        }
        if ( st.st_size < (off_t) MZF_HEADER_SIZE ) {
            fprintf ( stderr, "Error: '%s' is too small to be an MZF file "
                      "(%lld bytes, minimum %d bytes for header)\n",
                      mzf_filename, (long long) st.st_size, MZF_HEADER_SIZE );
            return MZDSK_RES_FORMAT_ERROR;
        }
    }

    st_HANDLER *handler = generic_driver_open_memory_from_file ( NULL, &g_memory_driver_realloc, mzf_filename );

    if ( !handler ) {
        fprintf ( stderr, "Error: cannot open MZF file '%s': %s\n",
                  mzf_filename, strerror ( errno ) );
        return MZDSK_RES_FORMAT_ERROR;
    }

    generic_driver_set_handler_readonly_status ( handler, 1 );

    /* Validace MZF formátu před čtením - odmítne příliš malé soubory,
       neplatnou hlavičku (chybí fname terminátor) a velikostní nesoulad
       (fsize v hlavičce > zbytek souboru). BUG 7: dříve se debug hlášky
       vypisovaly až po `print_mzfhdr`, což ukazovalo uživateli nesmyslné
       hodnoty parsované z random dat. */
    en_MZF_ERROR vres = mzf_file_validate ( handler );
    if ( vres != MZF_OK ) {
        fprintf ( stderr, "Error: '%s' is not a valid MZF file: %s\n",
                  mzf_filename, mzf_error_string ( vres ) );
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_FORMAT_ERROR;
    }

    st_MZF_HEADER mzfhdr;
    if ( EXIT_SUCCESS != mzf_read_header ( handler, &mzfhdr ) ) {
        fprintf ( stderr, "Error: cannot read MZF header from '%s'\n", mzf_filename );
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_FORMAT_ERROR;
    }

    print_mzfhdr ( &mzfhdr );

    /* Heap místo stacku - 64 kB (audit M-24). */
    uint8_t *data = malloc ( MZF_MAX_BODY_SIZE );
    if ( !data ) {
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* načíst a uložit tělo souboru */
    if ( EXIT_SUCCESS != mzf_read_body ( handler, data, mzfhdr.fsize ) ) {
        fprintf ( stderr, "Error: cannot read MZF body from '%s'\n", mzf_filename );
        generic_driver_close ( handler );
        free ( handler );
        free ( data );
        return MZDSK_RES_FORMAT_ERROR;
    }

    generic_driver_close ( handler );
    free ( handler );

    err = fsmz_write_file ( disc, mzfhdr.ftype, MZF_UINT8_FNAME ( mzfhdr.fname ), mzfhdr.fsize, mzfhdr.fstrt, mzfhdr.fexec, data, fsmz_dir_items );
    free ( data );
    if ( err == MZDSK_RES_OK ) {
        printf ( "OK\n" );
    }
    return err;
}


/* =========================================================================
 * Operace: smazání souboru
 * ========================================================================= */


/**
 * @brief Smaže soubor z FSMZ disku podle jména.
 *
 * Konvertuje ASCII jméno na Sharp MZ formát a zavolá fsmz_unlink_file().
 * Parametr force určuje, zda ignorovat lock flag (1) nebo jej ctít (0).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param filename ASCII jméno souboru.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_erase_by_name ( st_MZDSK_DISC *disc, char *filename, uint8_t fsmz_dir_items, uint8_t force ) {

    uint8_t mz_fname [ FSMZ_FNAME_LENGTH ];

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "Unlink file %s.\n\n", filename );
    warn_fname_truncation ( filename );
    fsmz_tool_convert_ascii_to_mzfname ( mz_fname, filename, 0 );

    return fsmz_unlink_file ( disc, mz_fname, fsmz_dir_items, force );
}


/**
 * @brief Smaže soubor z FSMZ disku podle ID.
 *
 * Najde soubor podle ID, získá jeho Sharp MZ jméno a zavolá fsmz_unlink_file().
 * Parametr force určuje, zda ignorovat lock flag (1) nebo jej ctít (0).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param item_id ID souboru v adresáři.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_erase_by_id ( st_MZDSK_DISC *disc, uint8_t item_id, uint8_t fsmz_dir_items, uint8_t force ) {
    st_FSMZ_DIR_ITEM *diritem;
    uint8_t mz_fname [ FSMZ_FNAME_LENGTH ];

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "Unlink file ID %d.\n\n", item_id );
    en_MZDSK_RES err;
    diritem = fsmz_tool_get_diritem_pointer_and_dir_by_id ( disc, item_id, NULL, fsmz_dir_items, &err );
    if ( err ) return err;
    memcpy ( mz_fname, diritem->fname, sizeof ( mz_fname ) );

    return fsmz_unlink_file ( disc, mz_fname, fsmz_dir_items, force );
}


/* =========================================================================
 * Operace: přejmenování souboru
 * ========================================================================= */


/**
 * @brief Přejmenuje soubor na FSMZ disku podle jména.
 *
 * Konvertuje oba názvy na Sharp MZ formát a zavolá fsmz_rename_file().
 * Parametr force určuje, zda ignorovat lock flag (1) nebo jej ctít (0).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param filename Původní ASCII jméno souboru.
 * @param new_filename Nové ASCII jméno souboru.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_rename_by_name ( st_MZDSK_DISC *disc, char *filename, char *new_filename, uint8_t fsmz_dir_items, uint8_t force ) {

    uint8_t mz_fname [ FSMZ_FNAME_LENGTH ];
    uint8_t new_mz_fname [ FSMZ_FNAME_LENGTH ];

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "Rename %s to %s.\n\n", filename, new_filename );
    warn_fname_truncation ( filename );
    warn_fname_truncation ( new_filename );

    fsmz_tool_convert_ascii_to_mzfname ( mz_fname, filename, 0 );
    fsmz_tool_convert_ascii_to_mzfname ( new_mz_fname, new_filename, 0 );

    return fsmz_rename_file ( disc, mz_fname, new_mz_fname, fsmz_dir_items, force );
}


/**
 * @brief Přejmenuje soubor na FSMZ disku podle ID.
 *
 * Najde soubor podle ID, získá jeho Sharp MZ jméno a přejmenuje ho.
 * Parametr force určuje, zda ignorovat lock flag (1) nebo jej ctít (0).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param item_id ID souboru v adresáři.
 * @param new_filename Nové ASCII jméno souboru.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_rename_by_id ( st_MZDSK_DISC *disc, uint8_t item_id, char *new_filename, uint8_t fsmz_dir_items, uint8_t force ) {

    st_FSMZ_DIR_ITEM *diritem;
    uint8_t mz_fname [ FSMZ_FNAME_LENGTH ];
    uint8_t new_mz_fname [ FSMZ_FNAME_LENGTH ];

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "Rename file ID %d to %s.\n\n", item_id, new_filename );

    en_MZDSK_RES err;
    diritem = fsmz_tool_get_diritem_pointer_and_dir_by_id ( disc, item_id, NULL, fsmz_dir_items, &err );
    if ( err ) return err;

    memcpy ( mz_fname, diritem->fname, sizeof ( mz_fname ) );

    warn_fname_truncation ( new_filename );
    fsmz_tool_convert_ascii_to_mzfname ( new_mz_fname, new_filename, 0 );

    return fsmz_rename_file ( disc, mz_fname, new_mz_fname, fsmz_dir_items, force );
}


/* =========================================================================
 * Operace: zamčení/odemčení souboru
 * ========================================================================= */


/**
 * @brief Zamkne nebo odemkne soubor na FSMZ disku podle jména.
 *
 * Vyhledá soubor podle jména, nastaví příznak locked a zapíše
 * aktualizovaný blok adresáře zpět na disk. Po vyhledání je nutné
 * korigovat dir.position o -1, protože fsmz_read_dir() ji
 * post-inkrementuje (stejný vzor jako fsmz_unlink_file
 * a fsmz_rename_file).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param filename ASCII jméno souboru.
 * @param lck_state Nový stav uzamčení (0 = odemčený, 1 = zamčený).
 * @param fsmz_dir_items Maximální počet položek adresáře.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_lock_by_name ( st_MZDSK_DISC *disc, char *filename, uint8_t lck_state, uint8_t fsmz_dir_items ) {

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "\n%s file %s.\n", ( lck_state == 1 ) ? "Lock" : "Unlock", filename );

    st_FSMZ_DIR dir;
    en_MZDSK_RES err;

    st_FSMZ_DIR_ITEM *diritem = fsmz_tool_get_diritem_pointer_and_dir_by_name ( disc, filename, &dir, fsmz_dir_items, &err );
    if ( err ) return err;

    diritem->locked = lck_state;
    /* fsmz_search_file() interně používá fsmz_read_dir(), která po vrácení
       položky provede post-inkrement dir.position. Před zápisem je nutné
       pozici korigovat zpět, aby fsmz_write_dirblock() zapisovala správný blok. */
    dir.position--;
    err = fsmz_write_dirblock ( disc, &dir, fsmz_dir_items );

    if ( !err ) printf ( "Done.\n\n" );
    return err;
}


/**
 * @brief Zamkne nebo odemkne soubor na FSMZ disku podle ID.
 *
 * Najde položku adresáře podle ID, nastaví příznak locked
 * a zapíše aktualizovaný blok adresáře zpět na disk.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param item_id ID souboru v adresáři.
 * @param lck_state Nový stav uzamčení (0 = odemčený, 1 = zamčený).
 * @param fsmz_dir_items Maximální počet položek adresáře.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_lock_by_id ( st_MZDSK_DISC *disc, uint8_t item_id, uint8_t lck_state, uint8_t fsmz_dir_items ) {

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "\n%s file ID %d.\n", ( lck_state == 1 ) ? "Lock" : "Unlock", item_id );

    st_FSMZ_DIR dir;
    en_MZDSK_RES err;

    st_FSMZ_DIR_ITEM *diritem = fsmz_tool_get_diritem_pointer_and_dir_by_id ( disc, item_id, &dir, fsmz_dir_items, &err );
    if ( err ) return err;

    diritem->locked = lck_state;
    err = fsmz_write_dirblock ( disc, &dir, fsmz_dir_items );

    if ( !err ) printf ( "Done.\n\n" );
    return err;
}


/* =========================================================================
 * Operace: změna typu souboru
 * ========================================================================= */


/**
 * @brief Změní souborový typ (ftype) položky adresáře podle ID.
 *
 * Načte příslušný blok adresáře, změní ftype položky a zapíše
 * blok zpět na disk.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param item_id ID souboru v adresáři.
 * @param ftype Nový souborový typ.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 * @pre item_id musí být platné (< fsmz_dir_items).
 */
static en_MZDSK_RES cmd_chtype_by_id ( st_MZDSK_DISC *disc, uint8_t item_id, uint8_t ftype, uint8_t fsmz_dir_items ) {

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    if ( item_id >= fsmz_dir_items ) {
        printf ( "\nError: Bad diritem ID %d (max is %d)\n", item_id, fsmz_dir_items - 1 );
        return MZDSK_RES_FILE_NOT_FOUND;
    }

    printf ( "\nChange ftype to 0x%02x for file ID %d.\n", ftype, item_id );

    st_FSMZ_DIR dir;
    uint8_t real_item_id = item_id + 1;
    en_MZDSK_RES err = fsmz_read_dirblock_with_diritem_position ( disc, &dir, real_item_id, fsmz_dir_items );
    if ( err ) return err;

    st_FSMZ_DIR_ITEM *diritem = &dir.dir_bl->item[ ( real_item_id ) & 0x07 ];
    diritem->ftype = ftype;
    err = fsmz_write_dirblock ( disc, &dir, fsmz_dir_items );
    if ( !err ) {
        printf ( "Done.\n\n" );
    }
    return err;
}


/* =========================================================================
 * Operace: změna STRT/EXEC/ftype existujícího souboru (set)
 * ========================================================================= */


/**
 * @brief Aktualizuje fstrt/fexec/ftype položky adresáře podle jména.
 *
 * Vyhledá soubor podle ASCII jména, převede na Sharp MZ ASCII a zavolá
 * fsmz_set_addr. Alespoň jeden z has_* musí být 1.
 *
 * @param disc Otevřený disk. Nesmí být NULL.
 * @param filename ASCII jméno souboru.
 * @param fstrt Nová start address (platí jen pokud has_fstrt == 1).
 * @param has_fstrt Pokud 1, fstrt se má upravit.
 * @param fexec Nová exec address (platí jen pokud has_fexec == 1).
 * @param has_fexec Pokud 1, fexec se má upravit.
 * @param ftype Nový typ (platí jen pokud has_ftype == 1; 0x01-0xFF).
 * @param has_ftype Pokud 1, ftype se má upravit.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_set_addr_by_name ( st_MZDSK_DISC *disc, char *filename,
                                             uint16_t fstrt, int has_fstrt,
                                             uint16_t fexec, int has_fexec,
                                             uint8_t ftype, int has_ftype,
                                             uint8_t fsmz_dir_items, uint8_t force ) {
    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "Set address fields for %s.\n", filename );
    if ( has_fstrt ) printf ( "  fstrt: 0x%04X\n", fstrt );
    if ( has_fexec ) printf ( "  fexec: 0x%04X\n", fexec );
    if ( has_ftype ) printf ( "  ftype: 0x%02X\n", ftype );
    printf ( "\n" );

    warn_fname_truncation ( filename );

    uint8_t mz_fname [ FSMZ_FNAME_LENGTH ];
    fsmz_tool_convert_ascii_to_mzfname ( mz_fname, filename, 0 );

    return fsmz_set_addr ( disc, mz_fname,
                            has_fstrt ? &fstrt : NULL,
                            has_fexec ? &fexec : NULL,
                            has_ftype ? &ftype : NULL,
                            fsmz_dir_items, force );
}


/**
 * @brief Aktualizuje fstrt/fexec/ftype položky adresáře podle ID.
 *
 * Najde Sharp MZ jméno položky podle ID a deleguje na cmd_set_addr_by_name
 * (přes fsmz_set_addr).
 *
 * @param disc Otevřený disk. Nesmí být NULL.
 * @param item_id Pořadové číslo položky (0-based).
 * @param fstrt Nová start address (platí jen pokud has_fstrt == 1).
 * @param has_fstrt Pokud 1, fstrt se má upravit.
 * @param fexec Nová exec address (platí jen pokud has_fexec == 1).
 * @param has_fexec Pokud 1, fexec se má upravit.
 * @param ftype Nový typ (platí jen pokud has_ftype == 1; 0x01-0xFF).
 * @param has_ftype Pokud 1, ftype se má upravit.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu.
 */
static en_MZDSK_RES cmd_set_addr_by_id ( st_MZDSK_DISC *disc, uint8_t item_id,
                                          uint16_t fstrt, int has_fstrt,
                                          uint16_t fexec, int has_fexec,
                                          uint8_t ftype, int has_ftype,
                                          uint8_t fsmz_dir_items, uint8_t force ) {
    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    st_FSMZ_DIR_ITEM *diritem;
    en_MZDSK_RES err;
    diritem = fsmz_tool_get_diritem_pointer_and_dir_by_id ( disc, item_id, NULL, fsmz_dir_items, &err );
    if ( err ) return err;

    printf ( "Set address fields for file ID %d.\n", item_id );
    if ( has_fstrt ) printf ( "  fstrt: 0x%04X\n", fstrt );
    if ( has_fexec ) printf ( "  fexec: 0x%04X\n", fexec );
    if ( has_ftype ) printf ( "  ftype: 0x%02X\n", ftype );
    printf ( "\n" );

    uint8_t mz_fname [ FSMZ_FNAME_LENGTH ];
    memcpy ( mz_fname, diritem->fname, sizeof ( mz_fname ) );

    return fsmz_set_addr ( disc, mz_fname,
                            has_fstrt ? &fstrt : NULL,
                            has_fexec ? &fexec : NULL,
                            has_ftype ? &ftype : NULL,
                            fsmz_dir_items, force );
}


/* =========================================================================
 * Operace: bootstrap
 * ========================================================================= */


/**
 * @brief Klasifikuje typ bootstrapu podle pozice a rozsahu bloků.
 *
 * Určí typ bootstrapu na základě počátečního bloku, koncového bloku
 * a informací z DINFO:
 * - "Mini": bloky 1-14 (FSMZ kompatibilní - DINFO/dir nedotčeny)
 * - "Bottom": blok >= 1, blok < 16, ale block_end > 14 (přesahuje do DINFO/dir)
 * - "Normal": blok v souborové oblasti (>= 16)
 * - "Over FSMZ": blok za hranicí souborové oblasti (> dinfo.blocks)
 *
 * @param disc Otevřený disk (pro čtení DINFO).
 * @param block Počáteční blok bootstrapu.
 * @param block_count Počet bloků bootstrapu.
 * @return Statický řetězec s názvem typu bootstrapu.
 */
static const char* classify_bootstrap_type ( st_MZDSK_DISC *disc,
                                              uint16_t block,
                                              uint16_t block_count )
{
    uint16_t block_end = ( block_count > 0 ) ? ( block + block_count - 1 ) : block;

    if ( block >= 1 && block_end <= 14 ) {
        return "Mini";
    }

    if ( block >= 1 && block < 16 && block_end > 14 ) {
        return "Bottom";
    }

    /* zkusit přečíst DINFO pro rozlišení Normal vs. Over FSMZ */
    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC == disc->format ) {
        st_FSMZ_DINFO_BLOCK dinfo;
        if ( fsmz_read_dinfo ( disc, &dinfo ) == MZDSK_RES_OK ) {
            if ( block > dinfo.blocks ) {
                return "Over FSMZ";
            }
        }
    }

    return "Normal";
}


/**
 * @brief Zobrazí informace o bootstrap hlavičce (IPLPRO).
 *
 * Přečte IPLPRO blok z disku a zobrazí jeho obsah včetně klasifikace
 * typu bootstrapu (Mini/Bottom/Normal/Over FSMZ).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param output_format Výstupní formát (text/json/csv).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí mít boot stopu (tracks_rules->mzboot_track == 1).
 */
static en_MZDSK_RES cmd_boot_info ( st_MZDSK_DISC *disc, en_OUTFMT output_format ) {
    en_MZDSK_RES err;
    st_FSMZ_IPLPRO_BLOCK iplpro;

    if ( 1 != disc->tracks_rules->mzboot_track ) {
        fprintf ( stderr, "Error: This disk is not in FSMZ bootable format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    if ( output_format == OUTFMT_TEXT ) {
        printf ( "\nReading bootstrap header ...\n\n" );
    }

    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    if ( EXIT_SUCCESS != fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        fprintf ( stderr, "Error: Not found valid IPLPRO header!\n" );
    } else {
        uint16_t block_count = fsmz_blocks_from_size ( iplpro.fsize );
        const char *boot_type = classify_bootstrap_type ( disc, iplpro.block, block_count );

        if ( output_format == OUTFMT_TEXT ) {
            print_iplpro_header ( &iplpro );
            printf ( "\tboot_type = %s\n\n", boot_type );
        } else {
            char name_str[MZF_FNAME_UTF8_BUF_SIZE];
            fsmz_fname_to_str ( iplpro.fname, name_str, sizeof ( name_str ) );

            st_OUTFMT_CTX ctx;
            outfmt_init ( &ctx, output_format );

            outfmt_doc_begin ( &ctx );
            outfmt_kv_str ( &ctx, "filesystem", "fsmz" );
            outfmt_kv_str ( &ctx, "name", name_str );
            outfmt_kv_int ( &ctx, "type", iplpro.ftype );
            outfmt_kv_uint ( &ctx, "start", (unsigned long) iplpro.fstrt );
            outfmt_kv_uint ( &ctx, "size", (unsigned long) iplpro.fsize );
            outfmt_kv_uint ( &ctx, "exec", (unsigned long) iplpro.fexec );
            outfmt_kv_int ( &ctx, "block", iplpro.block );
            outfmt_kv_int ( &ctx, "block_count", block_count );
            outfmt_kv_str ( &ctx, "boot_type", boot_type );
            outfmt_doc_end ( &ctx );
        }
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Aktualizuje fname, fstrt, fexec a ftype v IPLPRO bloku.
 *
 * Wrapper nad fsmz_set_iplpro_header. Alespoň jeden z has_* musí být 1.
 * Jméno se konvertuje přes fsmz_tool_convert_ascii_to_mzfname s příznakem
 * is_iplpro_fname=1 (délka FSMZ_IPLFNAME_LENGTH).
 *
 * Pokud disk nemá platný IPLPRO (set nelze aplikovat bez bootstrapu),
 * vypíše konkrétní hlášku a vrátí MZDSK_RES_FORMAT_ERROR sentinel,
 * aby main loop nedoplňoval generický fallback. Disk není modifikován.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param name Nové jméno v ASCII (použije se jen pokud has_name == 1).
 * @param has_name Pokud 1, fname se má upravit.
 * @param fstrt Nová start address (platí pokud has_fstrt == 1).
 * @param has_fstrt Pokud 1, fstrt se má upravit.
 * @param fexec Nová exec address (platí pokud has_fexec == 1).
 * @param has_fexec Pokud 1, fexec se má upravit.
 * @param ftype Nový typ (platí pokud has_ftype == 1; 0x01-0xFF).
 * @param has_ftype Pokud 1, ftype se má upravit.
 *
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FORMAT_ERROR pokud
 *         IPLPRO není platný (chyba byla konkrétně reportována),
 *         jinak chybový kód.
 *
 * @pre Disk musí mít boot stopu (tracks_rules->mzboot_track == 1).
 */
static en_MZDSK_RES cmd_boot_set_header ( st_MZDSK_DISC *disc,
                                            const char *name, int has_name,
                                            uint16_t fstrt, int has_fstrt,
                                            uint16_t fexec, int has_fexec,
                                            uint8_t ftype, int has_ftype ) {
    if ( 1 != disc->tracks_rules->mzboot_track ) {
        fprintf ( stderr, "Error: This disk is not in FSMZ bootable format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    uint8_t mz_fname [ FSMZ_IPLFNAME_LENGTH ];
    if ( has_name ) {
        warn_fname_truncation ( name );
        fsmz_tool_convert_ascii_to_mzfname ( mz_fname, (char *) name, 1 );
    }

    printf ( "Update bootstrap header:\n" );
    if ( has_name )  printf ( "  name:  %s\n", name );
    if ( has_fstrt ) printf ( "  fstrt: 0x%04X\n", fstrt );
    if ( has_fexec ) printf ( "  fexec: 0x%04X\n", fexec );
    if ( has_ftype ) printf ( "  ftype: 0x%02X\n", ftype );
    printf ( "\n" );

    en_MZDSK_RES err = fsmz_set_iplpro_header ( disc,
                                                  has_name  ? mz_fname : NULL,
                                                  has_fstrt ? &fstrt : NULL,
                                                  has_fexec ? &fexec : NULL,
                                                  has_ftype ? &ftype : NULL );

    /* Disk bez platného IPLPRO: knihovna nic nezapsala, ale uživatel
     * potřebuje konkrétní hlášku (ne generické "File not found"). */
    if ( err == MZDSK_RES_FILE_NOT_FOUND ) {
        fprintf ( stderr,
                  "Error: No valid IPLPRO header on disk - cannot edit bootstrap fields.\n"
                  "       Install a bootstrap first (e.g. 'boot put <mzf>').\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    return err;
}


/**
 * @brief Extrahuje bootstrap (IPLPRO) z disku do MZF souboru.
 *
 * Přečte IPLPRO hlavičku, ověří její platnost, vytvoří MZF hlavičku
 * a uloží bootstrap jako MZF soubor.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param mzf_filename Cesta k výstupnímu MZF souboru.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí mít boot stopu.
 */
static en_MZDSK_RES cmd_boot_get ( st_MZDSK_DISC *disc, char *mzf_filename ) {

    st_FSMZ_IPLPRO_BLOCK iplpro;
    en_MZDSK_RES err;

    if ( 1 != disc->tracks_rules->mzboot_track ) {
        printf ( "\nError: This disk is not in FSMZ bootable format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "\nRead bootstrap header:\n\n" );

    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    if ( EXIT_SUCCESS != fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        fprintf ( stderr, "Error: Not found valid IPLPRO header!\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    st_MZF_HEADER *mzfhdr = mzf_tools_create_mzfhdr ( iplpro.ftype, iplpro.fsize, iplpro.fstrt, iplpro.fexec, ( uint8_t* ) &iplpro.fname, FSMZ_IPLFNAME_LENGTH - 1, ( uint8_t* ) &iplpro.cmnt );
    if ( !mzfhdr ) return MZDSK_RES_FORMAT_ERROR;

    err = save_mzf_from_block ( disc, mzfhdr, iplpro.block, mzf_filename );
    free ( mzfhdr );
    return err;
}


/**
 * @brief Vyčistí IPLPRO blok (blok 0) - vynuluje ho.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí mít boot stopu.
 */
static en_MZDSK_RES clean_iplpro_block ( st_MZDSK_DISC *disc ) {

    st_FSMZ_IPLPRO_BLOCK iplpro;

    if ( 1 != disc->tracks_rules->mzboot_track ) {
        printf ( "\nError: This disk is not in FSMZ bootable format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    printf ( "\nClean IPLPRO block\n\n" );
    memset ( &iplpro, 0x00, sizeof ( iplpro ) );

    return fs_mz_write_iplpro ( disc, &iplpro );
}


/**
 * @brief Odstraní FSMZ bootstrap z disku.
 *
 * Uvolní alokované bloky bootstrapu v DINFO bitmapě,
 * vyčistí IPLPRO blok a nastaví disk jako slave (nebootovatelný).
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí být v plném FSMZ formátu s platným IPLPRO.
 */
static en_MZDSK_RES remove_fsmz_bootstrap ( st_MZDSK_DISC *disc ) {

    en_MZDSK_RES err;
    st_FSMZ_IPLPRO_BLOCK iplpro;

    printf ( "\nRemove mzfs bootstrap" );

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    if ( EXIT_SUCCESS != fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        fprintf ( stderr, "Error: Not found valid IPLPRO header!\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    uint16_t count_blocks = fsmz_blocks_from_size ( iplpro.fsize );

    err = fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_RESET, iplpro.block, count_blocks );
    if ( err ) return err;

    err = clean_iplpro_block ( disc );
    if ( err ) return err;

    return fsmz_update_dinfo_volume_number ( disc, FSMZ_DINFO_SLAVE );
}


/**
 * @brief Vyčistí bootstrap z disku (IPLPRO + DINFO bitmap).
 *
 * Kombinuje vyčištění IPLPRO bloku a odstranění bootstrapu z FAREA bitmapy.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí mít boot stopu.
 */
static en_MZDSK_RES cmd_boot_clear ( st_MZDSK_DISC *disc ) {

    en_MZDSK_RES err;

    /* Nejprve uvolníme bloky v DINFO bitmapě (potřebujeme platný IPLPRO header) */
    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC == disc->format ) {
        err = remove_fsmz_bootstrap ( disc );
        if ( err ) return err;
    }

    /* Až poté vyčistíme IPLPRO blok */
    return clean_iplpro_block ( disc );
}


/**
 * @brief Nainstaluje MZF soubor jako bootstrap na FSMZ disk.
 *
 * Načte MZF soubor, ověří volné místo, zapíše data a aktualizuje
 * IPLPRO hlavičku a DINFO bitmapu.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param mzf_filename Cesta k vstupnímu MZF souboru.
 * @param bootstrap_type Typ bootstrapu (BOOTSTRAP_MINI, BOOTSTRAP_BOTTOM, BOOTSTRAP_NORMAL, BOOTSTRAP_OVER).
 * @param no_fsmz_compat Při true na plném FSMZ povolí přesah přes blok 14
 *                        (celý disk minus IPLPRO). Relevantní jen pro MINI/BOTTOM.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk musí mít boot stopu. NORMAL a OVER vyžadují plný FSMZ formát.
 *      MINI/BOTTOM funguje na všech formátech s boot stopou.
 *      Bez no_fsmz_compat: max 14 bloků na FSMZ, 15 na ostatních.
 *      S no_fsmz_compat na FSMZ: total_tracks * 16 - 1 (celý disk).
 * @pre Na disku nesmí být existující platný IPLPRO (musí být nejprve vyčištěn).
 */
static en_MZDSK_RES cmd_boot_put ( st_MZDSK_DISC *disc, char *mzf_filename,
                                    en_BOOTSTRAP_TYPE bootstrap_type, bool no_fsmz_compat ) {

    st_FSMZ_IPLPRO_BLOCK iplpro;
    en_MZDSK_RES err;

    if ( 1 != disc->tracks_rules->mzboot_track ) {
        printf ( "\nError: This disk is not in FSMZ bootable format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    switch ( bootstrap_type ) {

        case BOOTSTRAP_MINI:
        case BOOTSTRAP_BOTTOM:
            printf ( "\nPut new bottom FSMZ bootstrap:\n" );
            break;

        case BOOTSTRAP_NORMAL:
            if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
                printf ( "\nError: This disk is not in full FSMZ format\n\n" );
                return MZDSK_RES_FORMAT_ERROR;
            }
            printf ( "\nPut new normal FSMZ bootstrap:\n" );
            break;

        case BOOTSTRAP_OVER:
            if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
                printf ( "\nError: This disk is not in full FSMZ format\n\n" );
                return MZDSK_RES_FORMAT_ERROR;
            }
            printf ( "\nPut new over-FAREA FSMZ bootstrap (experimental):\n" );
            break;

        default:
            printf ( "\nError: unknown bootstrap type!\n\n" );
            return MZDSK_RES_FORMAT_ERROR;
    }

    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    if ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        fprintf ( stderr, "Error: Can't put bootstrap - found existing IPLPRO header! Must be removed or cleaned.\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* Pre-flight: MZF soubor musí existovat, být regular file a >= 128 B
     * (viz BUG 7 + audit M-19 pro S_ISREG). */
    {
        struct stat st;
        if ( stat ( mzf_filename, &st ) != 0 ) {
            fprintf ( stderr, "Error: cannot open MZF file '%s': %s\n",
                      mzf_filename, strerror ( errno ) );
            return MZDSK_RES_FORMAT_ERROR;
        }
        if ( !S_ISREG ( st.st_mode ) ) {
            fprintf ( stderr, "Error: '%s' is not a regular file\n", mzf_filename );
            return MZDSK_RES_FORMAT_ERROR;
        }
        if ( st.st_size < (off_t) MZF_HEADER_SIZE ) {
            fprintf ( stderr, "Error: '%s' is too small to be an MZF file "
                      "(%lld bytes, minimum %d bytes for header)\n",
                      mzf_filename, (long long) st.st_size, MZF_HEADER_SIZE );
            return MZDSK_RES_FORMAT_ERROR;
        }
    }

    st_HANDLER *handler = generic_driver_open_memory_from_file ( NULL, &g_memory_driver_realloc, mzf_filename );

    if ( !handler ) {
        fprintf ( stderr, "Error: cannot open MZF file '%s': %s\n",
                  mzf_filename, strerror ( errno ) );
        return MZDSK_RES_FORMAT_ERROR;
    }

    generic_driver_set_handler_readonly_status ( handler, 1 );

    /* Validace MZF hlavičky - konzistence s cmd_put_mzf (BUG 7). */
    en_MZF_ERROR vres = mzf_file_validate ( handler );
    if ( vres != MZF_OK ) {
        fprintf ( stderr, "Error: '%s' is not a valid MZF file: %s\n",
                  mzf_filename, mzf_error_string ( vres ) );
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_FORMAT_ERROR;
    }

    st_MZF_HEADER mzfhdr;
    if ( EXIT_SUCCESS != mzf_read_header ( handler, &mzfhdr ) ) {
        fprintf ( stderr, "Error: cannot read MZF header from '%s'\n", mzf_filename );
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_FORMAT_ERROR;
    }

    print_mzfhdr ( &mzfhdr );

    iplpro.ftype = 0x03;
    memcpy ( &iplpro.iplpro, "IPLPRO", sizeof ( iplpro.iplpro ) );

    iplpro.fsize = mzfhdr.fsize;
    iplpro.fstrt = mzfhdr.fstrt;
    iplpro.fexec = mzfhdr.fexec;

    memset ( iplpro.fname, 0x0d, FSMZ_IPLFNAME_LENGTH );
    memcpy ( &iplpro.fname, &mzfhdr.fname, FSMZ_IPLFNAME_LENGTH - 1 );

    memset ( iplpro.cmnt, 0x00, FSMZ_IPLCMNT_LENGTH );
    memcpy ( &iplpro.cmnt, &mzfhdr.cmnt, MZF_CMNT_LENGTH );

    /* ověřit velikost */
    uint16_t count_blocks = fsmz_blocks_from_size ( iplpro.fsize );

    if ( BOOTSTRAP_MINI == bootstrap_type || BOOTSTRAP_BOTTOM == bootstrap_type ) {

        iplpro.block = 1;

        uint16_t miniboot_max_blocks;
        if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
            /* non-FSMZ: boot track má 16 sektorů, blok 0 = IPLPRO */
            miniboot_max_blocks = FSMZ_SECTORS_ON_TRACK - 1;
        } else if ( no_fsmz_compat ) {
            /* plný FSMZ s --no-fsmz-compat: celý disk kromě IPLPRO */
            uint16_t total = disc->tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK;
            miniboot_max_blocks = ( total > 1 ) ? ( total - 1 ) : 0;
        } else {
            /* plný FSMZ: bloky 1-14, DINFO na 15, dir od 16 */
            miniboot_max_blocks = FSMZ_SECTORS_ON_TRACK - 2;
        }

        if ( count_blocks > miniboot_max_blocks ) {
            fprintf ( stderr, "Error: filesize for bottom bootstrap exceeded ( fsize: 0x%04x > max: 0x%04x )\n\n", iplpro.fsize, FSMZ_SECTOR_SIZE * miniboot_max_blocks );
            generic_driver_close ( handler );
            free ( handler );
            return MZDSK_RES_NO_SPACE;
        }

    } else if ( BOOTSTRAP_NORMAL == bootstrap_type ) {

        err = fsmz_check_free_blocks ( disc, count_blocks, &iplpro.block );

        if ( err ) {
            generic_driver_close ( handler );
            free ( handler );
            return err;
        }

    } else if ( BOOTSTRAP_OVER == bootstrap_type ) {
        /* OVERBOOT: umístit bootstrap nad oblast adresovatelnou FAREA bitmapou */
        st_FSMZ_DINFO_BLOCK dinfo;
        err = fsmz_read_dinfo ( disc, &dinfo );
        if ( err ) {
            generic_driver_close ( handler );
            free ( handler );
            return err;
        }
        /* první blok za oblastí pokrytou bitmapou */
        iplpro.block = ( FSMZ_FAREA_BITMAP_SIZE * 8 ) + dinfo.farea;
    }

    /* Heap místo stacku - 64 kB (audit M-24). */
    uint8_t *data = malloc ( MZF_MAX_BODY_SIZE );
    if ( !data ) {
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* načíst a uložit tělo souboru */
    if ( EXIT_SUCCESS != mzf_read_body ( handler, data, iplpro.fsize ) ) {
        fprintf ( stderr, "Error: cannot read MZF body from '%s'\n", mzf_filename );
        generic_driver_close ( handler );
        free ( handler );
        free ( data );
        return MZDSK_RES_FORMAT_ERROR;
    }

    generic_driver_close ( handler );
    free ( handler );

    err = fsmz_write_blocks ( disc, iplpro.block, iplpro.fsize, data );
    free ( data );
    if ( err ) return err;

    err = fs_mz_write_iplpro ( disc, &iplpro );
    if ( err ) return err;

    if ( BOOTSTRAP_NORMAL == bootstrap_type ) {
        err = fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_SET, iplpro.block, count_blocks );
        if ( err ) return err;
        err = fsmz_update_dinfo_volume_number ( disc, FSMZ_DINFO_MASTER );
        if ( err ) return err;
    }

    return MZDSK_RES_OK;
}


/* =========================================================================
 * Operace: rychlý formát
 * ========================================================================= */


/**
 * @brief Provede rychlý formát FSMZ disku (CLI wrapper).
 *
 * Deleguje na knihovní fsmz_tool_fast_format() a vypisuje uživatelský
 * výstup na stdout/stderr.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_fast_format ( st_MZDSK_DISC *disc ) {

    printf ( "\nFast FSMZ format\n" );

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    en_MZDSK_RES err = fsmz_tool_fast_format ( disc );
    if ( !err ) printf ( "Done!\n\n" );
    return err;
}


/* =========================================================================
 * Operace: oprava DINFO
 * ========================================================================= */


/**
 * @brief Opraví DINFO blok (CLI wrapper).
 *
 * Deleguje na knihovní fsmz_tool_repair_dinfo() a vypisuje uživatelský
 * výstup na stdout/stderr.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_repair_dinfo ( st_MZDSK_DISC *disc, uint8_t fsmz_dir_items ) {

    printf ( "\nRepair FSMZ disc info\n\n" );

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    en_MZDSK_RES err = fsmz_tool_repair_dinfo ( disc, fsmz_dir_items );

    if ( !err ) {
        printf ( "Done.\n\n" );
    }

    return err;
}


/* =========================================================================
 * Operace: defragmentace
 * ========================================================================= */


/**
 * @brief Callback pro výpis průběhu defragmentace na stdout.
 *
 * Jednoduchý callback předávaný do fsmz_tool_defrag(), který vypisuje
 * informační zprávy knihovní funkce přímo na stdout.
 *
 * @param message Textová zpráva o průběhu.
 * @param user_data Nepoužito (vždy NULL).
 */
static void defrag_progress ( const char *message, void *user_data ) {
    (void) user_data;
    printf ( "%s", message );
}


/**
 * @brief Provede defragmentaci FSMZ disku (CLI wrapper).
 *
 * Deleguje na knihovní fsmz_tool_defrag() a vypisuje uživatelský
 * výstup na stdout.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_defrag ( st_MZDSK_DISC *disc, uint8_t fsmz_dir_items ) {

    printf ( "\nRun defragmentation FSMZ\n\n" );

    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC != disc->format ) {
        printf ( "\nError: This disk is not in full FSMZ format\n" );
        return MZDSK_RES_FORMAT_ERROR;
    }

    en_MZDSK_RES err = fsmz_tool_defrag ( disc, fsmz_dir_items, defrag_progress, NULL );
    if ( !err ) printf ( "Done.\n\n" );
    return err;
}


/* =========================================================================
 * Nápověda a verze
 * ========================================================================= */


/**
 * @brief Vypíše verze všech použitých knihoven.
 */
static void print_lib_versions ( void ) {
    printf ( "mzdsk-fsmz %s (%s %s)\n\n",
             MZDSK_FSMZ_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
    printf ( "Library versions:\n" );
    printf ( "  mzdsk_global      %s\n", mzdsk_global_version() );
    printf ( "  mzdsk_ipldisk     %s\n", mzdsk_ipldisk_version() );
    printf ( "  mzdsk_hexdump     %s\n", mzdsk_hexdump_version() );
    printf ( "  output_format     %s\n", output_format_version() );
    printf ( "  mzf               %s\n", mzf_version() );
    printf ( "  generic_driver    %s\n", generic_driver_version() );
    printf ( "  sharpmz_ascii     %s\n", sharpmz_ascii_version() );
}


/* =========================================================================
 * Operace: surový přístup k FSMZ blokům
 * ========================================================================= */


/**
 * @brief Extrahuje data z FSMZ alokačního bloku (nebo série bloků) do souboru.
 *
 * Převede číslo bloku na stopu a sektor pomocí fsmz_block2trsec()
 * a přečte data. Podporuje čtení přes více bloků pokud je zadána
 * velikost přesahující jeden blok (256 B).
 *
 * @param disc      Ukazatel na otevřený disk. Nesmí být NULL.
 * @param block     Číslo prvního FSMZ alokačního bloku.
 * @param out_file  Cesta k výstupnímu souboru.
 * @param size      Počet bajtů ke čtení (0 = jeden blok = FSMZ_SECTOR_SIZE).
 * @param noinv     1 = vynucená neinverze (data zůstanou invertovaná).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_get_block ( st_MZDSK_DISC *disc, uint16_t block,
                                     const char *out_file, size_t size, int noinv ) {

    /* Audit M-16: kontrola existence výstupního souboru před čtením z disku.
     * FORMAT_ERROR = sentinel "chyba už byla reportovaná" (BUG C4). */
    if ( check_output_overwrite ( out_file ) != EXIT_SUCCESS ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    if ( size == 0 ) size = FSMZ_SECTOR_SIZE;

    printf ( "\nExtract %u B from FSMZ block %u into file: %s\n\n", (unsigned) size, block, out_file );

    FILE *fh = fopen ( out_file, "wb" );
    if ( fh == NULL ) {
        fprintf ( stderr, "Error: Could not open file '%s': %s\n", out_file, strerror ( errno ) );
        return MZDSK_RES_FORMAT_ERROR;
    }

    uint8_t dma[FSMZ_SECTOR_SIZE];

    while ( size ) {
        uint16_t trsec = fsmz_block2trsec ( block );
        size_t length = ( size >= FSMZ_SECTOR_SIZE ) ? FSMZ_SECTOR_SIZE : size;

        printf ( "  Block %u (track %u, sector %u): %u B\n",
                 block, ( trsec >> 8 ) & 0xff, trsec & 0xff, (unsigned) length );

        en_MZDSK_RES err = fsmz_read_blocks ( disc, block++, sizeof ( dma ), dma );
        if ( err != MZDSK_RES_OK ) {
            fclose ( fh );
            return err;
        }

        /* Vynucená neinverze - vrátit data do invertovaného stavu */
        if ( noinv ) {
            mzdsk_invert_data ( dma, sizeof ( dma ) );
        }

        if ( fwrite ( dma, 1, length, fh ) != length ) {
            fprintf ( stderr, "Error: Could not write to file: %s\n", strerror ( errno ) );
            fclose ( fh );
            return MZDSK_RES_FORMAT_ERROR;
        }

        size -= length;
    }

    printf ( "\nDone.\n\n" );
    fclose ( fh );
    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše data z lokálního souboru do FSMZ alokačního bloku (nebo série bloků).
 *
 * Čte data ze vstupního souboru a zapisuje je do po sobě jdoucích
 * alokačních bloků počínaje blokem N. Poslední neúplný blok se
 * doplní nulami.
 *
 * @param disc      Ukazatel na otevřený disk (RW). Nesmí být NULL.
 * @param block     Číslo prvního FSMZ alokačního bloku.
 * @param in_file   Cesta k vstupnímu souboru.
 * @param size      Počet bajtů k zápisu (0 = celý soubor od offsetu).
 * @param offset    Offset ve vstupním souboru.
 * @param noinv     1 = vynucená neinverze (data se zapíšou invertovaná).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_put_block ( st_MZDSK_DISC *disc, uint16_t block,
                                     const char *in_file, size_t size,
                                     size_t offset, int noinv ) {

    FILE *fh = fopen ( in_file, "rb" );
    if ( fh == NULL ) {
        fprintf ( stderr, "Error: Could not open file '%s': %s\n", in_file, strerror ( errno ) );
        return MZDSK_RES_FORMAT_ERROR;
    }

    fseek ( fh, 0, SEEK_END );
    /* Audit M-26: ftell může vrátit -1. Cast na size_t by dal ~4 GB.
     * Mezistep přes long s kontrolou < 0. */
    long ftell_res = ftell ( fh );
    if ( ftell_res < 0 ) {
        fprintf ( stderr, "Error: ftell failed on input file: %s\n", strerror ( errno ) );
        fclose ( fh );
        return MZDSK_RES_FORMAT_ERROR;
    }
    size_t filesize = (size_t) ftell_res;

    if ( offset > filesize ) {
        fprintf ( stderr, "Error: offset (%u) exceeds file size (%u)\n", (unsigned) offset, (unsigned) filesize );
        fclose ( fh );
        return MZDSK_RES_FORMAT_ERROR;
    }

    fseek ( fh, (long) offset, SEEK_SET );

    if ( size == 0 ) {
        size = filesize - offset;
    }

    printf ( "\nWrite %u B from file %s into FSMZ block %u\n\n", (unsigned) size, in_file, block );

    uint8_t dma[FSMZ_SECTOR_SIZE];

    while ( size ) {
        size_t length;

        if ( size >= FSMZ_SECTOR_SIZE ) {
            length = FSMZ_SECTOR_SIZE;
        } else {
            length = size;
            memset ( dma, 0x00, sizeof ( dma ) );
        }

        if ( fread ( dma, 1, length, fh ) != length ) {
            if ( ferror ( fh ) ) {
                fprintf ( stderr, "Error: Could not read from file: %s\n", strerror ( errno ) );
                fclose ( fh );
                return MZDSK_RES_FORMAT_ERROR;
            }
        }

        /* Vynucená neinverze - data se zapíšou invertovaná */
        if ( noinv ) {
            mzdsk_invert_data ( dma, sizeof ( dma ) );
        }

        uint16_t trsec = fsmz_block2trsec ( block );

        printf ( "  Block %u (track %u, sector %u): %u B\n",
                 block, ( trsec >> 8 ) & 0xff, trsec & 0xff, (unsigned) length );

        en_MZDSK_RES err = fsmz_write_blocks ( disc, block++, sizeof ( dma ), dma );
        if ( err != MZDSK_RES_OK ) {
            fclose ( fh );
            return err;
        }

        size -= length;
    }

    printf ( "\nDone.\n\n" );
    fclose ( fh );
    return MZDSK_RES_OK;
}




/**
 * @brief Provede hexdump FSMZ alokačního bloku (nebo série bloků).
 *
 * Přečte FSMZ bloky (256 B) a vypíše jejich obsah jako hexdump
 * s ASCII sloupcem. Data jsou automaticky deinvertována (FSMZ).
 * Volba dump_charset řídí konverzi znakové sady v ASCII sloupci hexdumpu.
 *
 * @param disc         Ukazatel na otevřený disk. Nesmí být NULL.
 * @param block        Číslo prvního FSMZ alokačního bloku.
 * @param size         Počet bajtů k zobrazení (0 = jeden blok = FSMZ_SECTOR_SIZE).
 * @param dump_charset Režim konverze znakové sady v ASCII sloupci.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES cmd_dump_block ( st_MZDSK_DISC *disc, uint16_t block, size_t size,
                                      en_MZDSK_HEXDUMP_CHARSET dump_charset ) {
    if ( size == 0 ) size = FSMZ_SECTOR_SIZE;

    uint16_t trsec = fsmz_block2trsec ( block );
    const char *cnv_txt = ( dump_charset != MZDSK_HEXDUMP_CHARSET_RAW ) ? "SharpASCII" : "ASCII";

    printf ( "\nDump %u B from FSMZ block %u (track %u, sector %u) - %s:\n\n",
             (unsigned) size, block, ( trsec >> 8 ) & 0xff, trsec & 0xff, cnv_txt );

    uint8_t dma[FSMZ_SECTOR_SIZE];

    while ( size ) {
        size_t length = ( size >= FSMZ_SECTOR_SIZE ) ? FSMZ_SECTOR_SIZE : size;

        en_MZDSK_RES err = fsmz_read_blocks ( disc, block, sizeof ( dma ), dma );
        if ( err != MZDSK_RES_OK ) return err;

        if ( size > FSMZ_SECTOR_SIZE ) {
            trsec = fsmz_block2trsec ( block );
            printf ( "Block %u (track %u, sector %u):\n", block, ( trsec >> 8 ) & 0xff, trsec & 0xff );
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
 * @brief Vypíše nápovědu k použití nástroje.
 *
 * @param[in] out Výstupní stream (stdout pro --help, stderr pro chybové cesty).
 */
static void print_usage ( FILE *out ) {
    fprintf ( out, "mzdsk-fsmz " MZDSK_FSMZ_VERSION " - FSMZ filesystem management tool\n\n" );
    fprintf ( out, "Usage: mzdsk-fsmz [options] <dsk_file> <command> [args...]\n\n" );
    fprintf ( out, "Global options:\n" );
    fprintf ( out, "  --ipldisk        Use IPLDISK extended directory (127 items instead of 63)\n" );
    fprintf ( out, "  --ro             Forced read-only mode\n" );
    fprintf ( out, "  --overwrite      Allow 'get'/'get-block' to overwrite an existing output file\n" );
    fprintf ( out, "                   (default: refuse to overwrite)\n" );
    fprintf ( out, "  --yes, -y        Skip interactive confirmation prompt for 'format'/'defrag';\n" );
    fprintf ( out, "                   non-TTY (pipes, scripts) is always silent\n" );
    fprintf ( out, "  --output FMT     Output format: text (default), json, csv\n" );
    fprintf ( out, "  -o FMT           Same as --output\n" );
    fprintf ( out, "  --charset MODE   Sharp MZ charset: eu (default), jp, utf8-eu, utf8-jp\n" );
    fprintf ( out, "  --version        Show version\n" );
    fprintf ( out, "  --lib-versions   Show library versions\n\n" );
    fprintf ( out, "Directory operations:\n" );
    fprintf ( out, "  dir              List all files\n" );
    fprintf ( out, "  dir --type T     List files filtered by ftype (hex, e.g. 01)\n" );
    fprintf ( out, "  file <name>      Show file info by name\n" );
    fprintf ( out, "  file --id N      Show file info by ID\n\n" );
    fprintf ( out, "File operations:\n" );
    fprintf ( out, "  get <name> <mzf> Extract file to MZF by name\n" );
    fprintf ( out, "  get --id N <mzf> Extract file to MZF by ID\n" );
    fprintf ( out, "  get --all <dir> [--on-duplicate MODE]\n" );
    fprintf ( out, "                   Extract all files to directory\n" );
    fprintf ( out, "                   --on-duplicate MODE  Handle duplicate names:\n" );
    fprintf ( out, "                                        rename (default), overwrite, skip\n" );
    fprintf ( out, "  put <mzf>        Insert MZF file\n" );
    fprintf ( out, "  era <name>       Delete file by name\n" );
    fprintf ( out, "  era --id N       Delete file by ID\n" );
    fprintf ( out, "    --force        Ignore lock flag (required to delete a locked file)\n" );
    fprintf ( out, "  ren <name> <new> Rename file by name\n" );
    fprintf ( out, "  ren --id N <new> Rename file by ID\n" );
    fprintf ( out, "    --force        Ignore lock flag (required to rename a locked file)\n" );
    fprintf ( out, "  lock <name> 0|1      Lock (1) or unlock (0) file by name\n" );
    fprintf ( out, "  lock --id N 0|1      Lock (1) or unlock (0) file by ID\n" );
    fprintf ( out, "  chtype --id N T  Change ftype of file by ID (T is hex)\n" );
    fprintf ( out, "  set <name> [--fstrt HEX] [--fexec HEX] [--ftype HEX]\n" );
    fprintf ( out, "                   Update STRT/EXEC/ftype of existing file by name\n" );
    fprintf ( out, "  set --id N [--fstrt HEX] [--fexec HEX] [--ftype HEX]\n" );
    fprintf ( out, "                   Update STRT/EXEC/ftype of existing file by ID\n" );
    fprintf ( out, "    --fstrt HEX    New load address (0x0000-0xFFFF)\n" );
    fprintf ( out, "    --fexec HEX    New exec address (0x0000-0xFFFF)\n" );
    fprintf ( out, "    --ftype HEX    New ftype (0x01-0xFF)\n" );
    fprintf ( out, "    --force        Ignore lock flag\n\n" );
    fprintf ( out, "Bootstrap operations:\n" );
    fprintf ( out, "  boot                         Show bootstrap info (with type classification)\n" );
    fprintf ( out, "  boot [--fstrt HEX] [--fexec HEX] [--ftype HEX] [--name NAME]\n" );
    fprintf ( out, "                               Update bootstrap header fields and show info\n" );
    fprintf ( out, "  boot put <mzf>               Install normal bootstrap\n" );
    fprintf ( out, "  boot get <mzf>               Extract bootstrap to MZF\n" );
    fprintf ( out, "  boot clear                   Clear bootstrap\n" );
    fprintf ( out, "  boot bottom <mzf>            Install bottom bootstrap\n" );
    fprintf ( out, "  boot mini <mzf>              Alias for 'boot bottom'\n" );
    fprintf ( out, "  boot bottom --no-fsmz-compat <mzf>  Allow overwriting FSMZ structures\n" );
    fprintf ( out, "  boot over <mzf>              Install over-FAREA bootstrap (experimental)\n\n" );
    fprintf ( out, "Raw block operations:\n" );
    fprintf ( out, "  dump-block N [bytes]                   Hexdump of FSMZ block(s)\n" );
    fprintf ( out, "    --dump-charset MODE                   ASCII column charset: raw (default), eu, jp, utf8-eu, utf8-jp\n" );
    fprintf ( out, "    --cnv                                Alias for --dump-charset eu\n" );
    fprintf ( out, "  get-block N <file> [bytes]             Extract FSMZ block(s) to file\n" );
    fprintf ( out, "  put-block N <file> [bytes] [offset]    Write file into FSMZ block(s)\n" );
    fprintf ( out, "    --noinv                              Disable auto-inversion\n\n" );
    fprintf ( out, "Maintenance:\n" );
    fprintf ( out, "  format           Quick FSMZ format\n" );
    fprintf ( out, "  repair           Repair DINFO block\n" );
    fprintf ( out, "  defrag           Defragment filesystem\n" );
}


/* =========================================================================
 * Pomocné funkce pro parsování argumentů
 * ========================================================================= */


/**
 * @brief Parsuje hexadecimální bajt z řetězce.
 *
 * Podporuje formáty: "0xFF", "0xff", "FF", "ff".
 *
 * @param str Vstupní řetězec.
 * @param[out] value Výstupní hodnota.
 *
 * @return EXIT_SUCCESS pokud se podařilo parsovat, EXIT_FAILURE pokud ne.
 */
static int parse_hex_byte ( const char *str, uint8_t *value ) {
    char *endptr;
    unsigned long val = strtoul ( str, &endptr, 16 );
    if ( endptr == str || *endptr != '\0' || val > 0xff ) {
        return EXIT_FAILURE;
    }
    *value = (uint8_t) val;
    return EXIT_SUCCESS;
}


/**
 * @brief Parsuje desítkové číslo z řetězce.
 *
 * @param str Vstupní řetězec.
 * @param[out] value Výstupní hodnota.
 *
 * @return EXIT_SUCCESS pokud se podařilo parsovat, EXIT_FAILURE pokud ne.
 */
static int parse_decimal ( const char *str, unsigned long *value ) {
    /* Audit M-21: odmítnout záporné vstupy explicitně. strtoul
     * interpretuje "-1" jako `-1UL` (= ULONG_MAX), což by mohlo projít
     * přes následné range checky v podivných okrajových případech
     * (viz H-17 v block/offset kontextu). */
    if ( str == NULL || str[0] == '-' || *str == '\0' ) return EXIT_FAILURE;
    char *endptr;
    /* Audit M-15: errno=0 + ERANGE kontrola zachytí přetečení
       (999999999999999999 → ULONG_MAX tiše). */
    errno = 0;
    *value = strtoul ( str, &endptr, 10 );
    if ( errno == ERANGE ) return EXIT_FAILURE;
    if ( endptr == str || *endptr != '\0' ) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Parsuje hodnotu --charset argumentu na en_MZF_NAME_ENCODING.
 *
 * Podporované hodnoty: "eu", "jp", "utf8-eu", "utf8-jp".
 *
 * @param str      Vstupní řetězec.
 * @param[out] enc Výstupní hodnota kódování.
 *
 * @return EXIT_SUCCESS pokud se podařilo parsovat, EXIT_FAILURE pokud ne.
 */
static int parse_charset ( const char *str, en_MZF_NAME_ENCODING *enc ) {
    if ( strcmp ( str, "eu" ) == 0 )      { *enc = MZF_NAME_ASCII_EU; return EXIT_SUCCESS; }
    if ( strcmp ( str, "jp" ) == 0 )      { *enc = MZF_NAME_ASCII_JP; return EXIT_SUCCESS; }
    if ( strcmp ( str, "utf8-eu" ) == 0 ) { *enc = MZF_NAME_UTF8_EU;  return EXIT_SUCCESS; }
    if ( strcmp ( str, "utf8-jp" ) == 0 ) { *enc = MZF_NAME_UTF8_JP;  return EXIT_SUCCESS; }
    return EXIT_FAILURE;
}


/**
 * @brief Parsuje hodnotu --dump-charset argumentu na en_MZDSK_HEXDUMP_CHARSET.
 *
 * Podporované hodnoty: "raw", "eu", "jp", "utf8-eu", "utf8-jp".
 *
 * @param str      Vstupní řetězec.
 * @param[out] enc Výstupní hodnota režimu.
 *
 * @return EXIT_SUCCESS pokud se podařilo parsovat, EXIT_FAILURE pokud ne.
 */
static int parse_dump_charset ( const char *str, en_MZDSK_HEXDUMP_CHARSET *enc ) {
    if ( strcmp ( str, "raw" ) == 0 )     { *enc = MZDSK_HEXDUMP_CHARSET_RAW;     return EXIT_SUCCESS; }
    if ( strcmp ( str, "eu" ) == 0 )      { *enc = MZDSK_HEXDUMP_CHARSET_EU;      return EXIT_SUCCESS; }
    if ( strcmp ( str, "jp" ) == 0 )      { *enc = MZDSK_HEXDUMP_CHARSET_JP;      return EXIT_SUCCESS; }
    if ( strcmp ( str, "utf8-eu" ) == 0 ) { *enc = MZDSK_HEXDUMP_CHARSET_UTF8_EU; return EXIT_SUCCESS; }
    if ( strcmp ( str, "utf8-jp" ) == 0 ) { *enc = MZDSK_HEXDUMP_CHARSET_UTF8_JP; return EXIT_SUCCESS; }
    return EXIT_FAILURE;
}


/**
 * @brief Extrahuje globální volby --output/-o a --charset/-C z sub_argv,
 *        aplikuje je a zkompaktuje pole argumentů.
 *
 * Řeší BUG 17: uživatel může tyto volby uvést kdekoliv (před DSK souborem,
 * mezi DSK souborem a subpříkazem, i ZA subpříkazem). Dosud fungovaly jen
 * první dvě pozice. Tento helper skenuje sub_argv[1..] (první prvek je
 * název subpříkazu), při nálezu option+value je z pole vyhodí a hodnotu
 * aplikuje do *output_format nebo *name_encoding.
 *
 * Akceptuje: -o VAL, --output VAL, --output=VAL, -C VAL, --charset VAL,
 *            --charset=VAL.
 *
 * @param[in,out] sub_argc      Ukazatel na počet argumentů v sub_argv.
 * @param[in,out] sub_argv      Pole argumentů. Po volání může být kratší.
 * @param[out]    output_format Nastaví se pokud byla nalezena --output.
 * @param[out]    name_encoding Nastaví se pokud byla nalezena --charset.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybné hodnotě nebo
 *         chybějícím argumentu.
 */
static int extract_trailing_global_opts ( int *sub_argc, char **sub_argv,
                                            en_OUTFMT *output_format,
                                            en_MZF_NAME_ENCODING *name_encoding ) {
    int i = 1; /* přeskočíme sub_argv[0] = název subpříkazu */
    while ( i < *sub_argc ) {
        const char *val = NULL;
        int is_output = 0, is_charset = 0;
        int consume = 0;

        if ( strncmp ( sub_argv[i], "--output=", 9 ) == 0 ) {
            val = sub_argv[i] + 9;  is_output = 1;  consume = 1;
        } else if ( strncmp ( sub_argv[i], "--charset=", 10 ) == 0 ) {
            val = sub_argv[i] + 10; is_charset = 1; consume = 1;
        } else if ( strcmp ( sub_argv[i], "--output" ) == 0 ||
                    strcmp ( sub_argv[i], "-o" ) == 0 ) {
            if ( i + 1 >= *sub_argc ) {
                fprintf ( stderr, "Error: %s requires an argument\n", sub_argv[i] );
                return EXIT_FAILURE;
            }
            val = sub_argv[i+1]; is_output = 1; consume = 2;
        } else if ( strcmp ( sub_argv[i], "--charset" ) == 0 ||
                    strcmp ( sub_argv[i], "-C" ) == 0 ) {
            if ( i + 1 >= *sub_argc ) {
                fprintf ( stderr, "Error: %s requires an argument\n", sub_argv[i] );
                return EXIT_FAILURE;
            }
            val = sub_argv[i+1]; is_charset = 1; consume = 2;
        }

        if ( consume == 0 ) { i++; continue; }

        if ( is_output ) {
            if ( outfmt_parse ( val, output_format ) != 0 ) {
                fprintf ( stderr, "Error: Unknown output format '%s' (use text, json or csv)\n", val );
                return EXIT_FAILURE;
            }
        } else if ( is_charset ) {
            if ( parse_charset ( val, name_encoding ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Unknown charset '%s' (use eu, jp, utf8-eu or utf8-jp)\n", val );
                return EXIT_FAILURE;
            }
        }

        /* Zkompaktování pole - posun zbývajících prvků */
        for ( int j = i; j + consume < *sub_argc; j++ ) {
            sub_argv[j] = sub_argv[j + consume];
        }
        *sub_argc -= consume;
        /* Neinkrementujeme i - na této pozici je teď další argument */
    }
    return EXIT_SUCCESS;
}


/* =========================================================================
 * Hlavní funkce
 * ========================================================================= */


/**
 * @brief Vstupní bod programu mzdsk-fsmz.
 *
 * Parsuje argumenty příkazové řádky, otevře DSK obraz, provede
 * požadovanou operaci a zavře disk.
 *
 * Pořadí zpracování argumentů:
 * 1. Globální přepínače (--help, --version, --lib-versions, --ipldisk, --ro)
 * 2. Cesta k DSK souboru (první nepřepínačový argument)
 * 3. Podpříkaz (druhý nepřepínačový argument)
 * 4. Argumenty podpříkazu
 *
 * Pro čtecí operace se disk otevírá v režimu RO,
 * pro zápisové operace v režimu RW.
 *
 * @param argc Počet argumentů.
 * @param argv Pole řetězců s argumenty.
 *
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
int main ( int argc, char *argv[] ) {

    memory_driver_init();

    if ( argc < 2 ) {
        print_usage ( stderr );
        return EXIT_FAILURE;
    }

    /* Definice dlouhých voleb pro getopt_long */
    static struct option global_options[] = {
        { "ipldisk",      no_argument,       NULL, 'I' },
        { "ro",           no_argument,       NULL, 'R' },
        { "overwrite",    no_argument,       NULL, 'O' },
        { "yes",          no_argument,       NULL, 'y' },
        { "output",       required_argument, NULL, 'o' },
        { "charset",      required_argument, NULL, 'C' },
        { "version",      no_argument,       NULL, 'V' },
        { "lib-versions", no_argument,       NULL, 'L' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL,           0,                 NULL,  0  }
    };

    /* Parsování globálních voleb přes getopt_long */
    int ipldisk = 0;
    int force_ro = 0;
    en_OUTFMT output_format = OUTFMT_TEXT;

    optind = 1;
    int opt;
    while ( ( opt = getopt_long ( argc, argv, "+o:yh", global_options, NULL ) ) != -1 ) {
        switch ( opt ) {

            case 'I': /* --ipldisk */
                ipldisk = 1;
                break;

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

            case 'C': /* --charset MODE */
                if ( parse_charset ( optarg, &g_name_encoding ) != EXIT_SUCCESS ) {
                    fprintf ( stderr, "Error: Unknown charset '%s' (use eu, jp, utf8-eu or utf8-jp)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;

            case 'V': /* --version */
                printf ( "mzdsk-fsmz %s (%s %s)\n",
                         MZDSK_FSMZ_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION );
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
            case 'I': ipldisk = 1; break;
            case 'R': force_ro = 1; break;
            case 'O': g_allow_overwrite = 1; break;  /* audit M-16 */
            case 'y': g_assume_yes = 1; break;       /* audit M-17 */
            case 'o':
                if ( outfmt_parse ( optarg, &output_format ) != 0 ) {
                    fprintf ( stderr, "Error: Unknown output format '%s' (use text, json or csv)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;
            case 'C':
                if ( parse_charset ( optarg, &g_name_encoding ) != EXIT_SUCCESS ) {
                    fprintf ( stderr, "Error: Unknown charset '%s' (use eu, jp, utf8-eu or utf8-jp)\n", optarg );
                    return EXIT_FAILURE;
                }
                break;
            case 'V': printf ( "mzdsk-fsmz %s (%s %s)\n", MZDSK_FSMZ_VERSION, MZDISK_CLI_RELEASE_NAME, MZDISK_CLI_RELEASE_VERSION ); return EXIT_SUCCESS;
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

    /* BUG 17: extrahovat globální volby --output/-o a --charset/-C, pokud
       je uživatel umístil za subpříkaz. */
    if ( extract_trailing_global_opts ( &sub_argc, sub_argv, &output_format,
                                          &g_name_encoding ) != EXIT_SUCCESS ) {
        return EXIT_FAILURE;
    }

    uint8_t fsmz_dir_items = ipldisk ? FSMZ_IPLDISK_MAX_DIR_ITEMS : FSMZ_MAX_DIR_ITEMS;

    /* Určení zda jde o čtecí nebo zápisovou operaci */
    int is_write_op = 0;
    if ( strcmp ( subcmd, "put" ) == 0 ||
         strcmp ( subcmd, "era" ) == 0 ||
         strcmp ( subcmd, "ren" ) == 0 ||
         strcmp ( subcmd, "lock" ) == 0 ||
         strcmp ( subcmd, "chtype" ) == 0 ||
         strcmp ( subcmd, "set" ) == 0 ||
         strcmp ( subcmd, "put-block" ) == 0 ||
         strcmp ( subcmd, "format" ) == 0 ||
         strcmp ( subcmd, "repair" ) == 0 ||
         strcmp ( subcmd, "defrag" ) == 0 ) {
        is_write_op = 1;
    }

    /* Speciální případ: "boot" subpříkazy */
    if ( strcmp ( subcmd, "boot" ) == 0 ) {
        if ( sub_argc >= 2 ) {
            char *boot_sub = sub_argv[1];
            if ( strcmp ( boot_sub, "put" ) == 0 ||
                 strcmp ( boot_sub, "clear" ) == 0 ||
                 strcmp ( boot_sub, "mini" ) == 0 ||
                 strcmp ( boot_sub, "bottom" ) == 0 ||
                 strcmp ( boot_sub, "over" ) == 0 ) {
                is_write_op = 1;
            }
            /* boot s některou z --fstrt/--fexec/--ftype/--name je write-op
             * (update IPLPRO hlavičky). */
            for ( int bi = 1; bi < sub_argc; bi++ ) {
                if ( strncmp ( sub_argv[bi], "--fstrt", 7 ) == 0 ||
                     strncmp ( sub_argv[bi], "--fexec", 7 ) == 0 ||
                     strncmp ( sub_argv[bi], "--ftype", 7 ) == 0 ||
                     strncmp ( sub_argv[bi], "--name", 6 ) == 0 ) {
                    is_write_op = 1;
                    break;
                }
            }
        }
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

    /* Zobrazení informací o disku (jen pro textový výstup) */
    if ( output_format == OUTFMT_TEXT ) {
        err = cmd_print_disc_info ( &disc );
        if ( MZDSK_RES_IS_FATAL ( err ) ) {
            /* chyba I/O - ukončíme */
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
    }

    /* Zpracování podpříkazu */

    /* ---- dir ---- */
    if ( strcmp ( subcmd, "dir" ) == 0 ) {

        static struct option dir_opts[] = {
            { "type", required_argument, NULL, 't' },
            { NULL,   0,                 NULL,  0  }
        };
        uint8_t ftype = 0;
        int has_type = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", dir_opts, NULL ) ) != -1 ) {
            if ( c == 't' ) {
                if ( parse_hex_byte ( optarg, &ftype ) != EXIT_SUCCESS ) {
                    fprintf ( stderr, "Error: Invalid ftype value '%s'\n", optarg );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
                has_type = 1;
            } else {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
        }
        /* Kontrola nadbytečných pozičních argumentů */
        if ( optind < sub_argc ) {
            fprintf ( stderr, "Error: 'dir' does not accept extra arguments (got '%s')\n", sub_argv[optind] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        if ( has_type ) {
            err = cmd_print_dir ( &disc, PRINTDIR_FTYPE, ftype, fsmz_dir_items, output_format );
        } else {
            err = cmd_print_dir ( &disc, PRINTDIR_VALID, 0, fsmz_dir_items, output_format );
        }

    /* ---- file ---- */
    } else if ( strcmp ( subcmd, "file" ) == 0 ) {

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
                if ( parse_decimal ( optarg, &id ) != EXIT_SUCCESS || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
                file_id = (long) id;
            } else {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
        }
        if ( file_id >= 0 ) {
            /* --id N: žádný poziční argument není očekáván */
            if ( optind < sub_argc ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_print_fileinfo ( &disc, NULL, (uint8_t) file_id, fsmz_dir_items, output_format );
        } else if ( optind < sub_argc ) {
            /* Jeden poziční argument (jméno), nic dalšího */
            if ( optind + 1 < sub_argc ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_print_fileinfo ( &disc, sub_argv[optind], 0, fsmz_dir_items, output_format );
        } else {
            fprintf ( stderr, "Error: file command requires <name> or --id N\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        if ( err == MZDSK_RES_FILE_NOT_FOUND ) {
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

    /* ---- get ---- */
    } else if ( strcmp ( subcmd, "get" ) == 0 ) {

        static struct option get_opts[] = {
            { "all",          no_argument,       NULL, 'a' },
            { "id",           required_argument, NULL, 'i' },
            { "on-duplicate", required_argument, NULL, 'D' },
            { NULL,           0,                 NULL,  0  }
        };
        long file_id = -1;
        int get_all = 0;
        en_DUPLICATE_MODE ga_dup_mode = DUP_RENAME;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", get_opts, NULL ) ) != -1 ) {
            switch ( c ) {
                case 'a': get_all = 1; break;
                case 'i': {
                    unsigned long id;
                    if ( parse_decimal ( optarg, &id ) != EXIT_SUCCESS || id > 255 ) {
                        fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    file_id = (long) id;
                    break;
                }
                case 'D':
                    if ( strcmp ( optarg, "rename" ) == 0 ) {
                        ga_dup_mode = DUP_RENAME;
                    } else if ( strcmp ( optarg, "overwrite" ) == 0 ) {
                        ga_dup_mode = DUP_OVERWRITE;
                    } else if ( strcmp ( optarg, "skip" ) == 0 ) {
                        ga_dup_mode = DUP_SKIP;
                    } else {
                        fprintf ( stderr, "Error: Unknown --on-duplicate mode '%s'"
                                  " (use rename, overwrite or skip)\n", optarg );
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    break;
                default:
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
            }
        }
        if ( get_all ) {
            if ( optind >= sub_argc ) {
                fprintf ( stderr, "Error: get --all requires <dir> argument\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* --all <dir> [--on-duplicate MODE]: jeden poziční argument */
            if ( sub_argc - optind > 1 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_get_all ( &disc, sub_argv[optind], fsmz_dir_items, ga_dup_mode );
        } else if ( file_id >= 0 ) {
            if ( optind >= sub_argc ) {
                fprintf ( stderr, "Error: get --id N requires <mzf> argument\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* --id N <mzf>: jeden poziční argument */
            if ( sub_argc - optind > 1 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_get_mzf ( &disc, NULL, (uint8_t) file_id, sub_argv[optind], fsmz_dir_items );
        } else if ( sub_argc - optind >= 2 ) {
            /* <name> <mzf>: dva poziční argumenty */
            if ( sub_argc - optind > 2 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_get_mzf ( &disc, sub_argv[optind], 0, sub_argv[optind + 1], fsmz_dir_items );
        } else {
            fprintf ( stderr, "Error: get command requires <name> <mzf>, --id N <mzf>, or --all <dir>\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

    /* ---- put ---- */
    } else if ( strcmp ( subcmd, "put" ) == 0 ) {

        if ( sub_argc < 2 ) {
            fprintf ( stderr, "Error: put command requires <mzf> argument\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        if ( sub_argc > 2 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[2] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_put_mzf ( &disc, sub_argv[1], fsmz_dir_items );

    /* ---- era ---- */
    } else if ( strcmp ( subcmd, "era" ) == 0 ) {

        static struct option era_opts[] = {
            { "id",    required_argument, NULL, 'i' },
            { "force", no_argument,       NULL, 'f' },
            { NULL,    0,                 NULL,  0  }
        };
        long file_id = -1;
        uint8_t force = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", era_opts, NULL ) ) != -1 ) {
            if ( c == 'i' ) {
                unsigned long id;
                if ( parse_decimal ( optarg, &id ) != EXIT_SUCCESS || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
                file_id = (long) id;
            } else if ( c == 'f' ) {
                force = 1;
            } else {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
        }
        if ( file_id >= 0 ) {
            /* --id N: žádný poziční argument není očekáván */
            if ( optind < sub_argc ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_erase_by_id ( &disc, (uint8_t) file_id, fsmz_dir_items, force );
        } else if ( optind < sub_argc ) {
            /* Jeden poziční argument (jméno), nic dalšího */
            if ( optind + 1 < sub_argc ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_erase_by_name ( &disc, sub_argv[optind], fsmz_dir_items, force );
        } else {
            fprintf ( stderr, "Error: era command requires <name> or --id N\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

    /* ---- ren ---- */
    } else if ( strcmp ( subcmd, "ren" ) == 0 ) {

        static struct option ren_opts[] = {
            { "id",    required_argument, NULL, 'i' },
            { "force", no_argument,       NULL, 'f' },
            { NULL,    0,                 NULL,  0  }
        };
        long file_id = -1;
        uint8_t force = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", ren_opts, NULL ) ) != -1 ) {
            if ( c == 'i' ) {
                unsigned long id;
                if ( parse_decimal ( optarg, &id ) != EXIT_SUCCESS || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
                file_id = (long) id;
            } else if ( c == 'f' ) {
                force = 1;
            } else {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
        }
        if ( file_id >= 0 ) {
            if ( optind >= sub_argc ) {
                fprintf ( stderr, "Error: ren --id N requires <new> argument\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* --id N <new>: jeden poziční argument */
            if ( optind + 1 < sub_argc ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_rename_by_id ( &disc, (uint8_t) file_id, sub_argv[optind], fsmz_dir_items, force );
        } else if ( sub_argc - optind >= 2 ) {
            /* <name> <new>: dva poziční argumenty */
            if ( sub_argc - optind > 2 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_rename_by_name ( &disc, sub_argv[optind], sub_argv[optind + 1], fsmz_dir_items, force );
        } else {
            fprintf ( stderr, "Error: ren command requires <name> <new> or --id N <new>\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

    /* ---- lock ---- */
    } else if ( strcmp ( subcmd, "lock" ) == 0 ) {

        static struct option lock_opts[] = {
            { "id", required_argument, NULL, 'i' },
            { NULL, 0,                 NULL,  0  }
        };
        long file_id = -1;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", lock_opts, NULL ) ) != -1 ) {
            if ( c == 'i' ) {
                unsigned long id;
                if ( parse_decimal ( optarg, &id ) != EXIT_SUCCESS || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
                file_id = (long) id;
            } else {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
        }
        int pos_remaining = sub_argc - optind;
        if ( file_id >= 0 ) {
            if ( pos_remaining < 1 ) {
                fprintf ( stderr, "Error: lock --id N requires 0|1 argument\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            /* --id N <0|1>: jeden poziční argument */
            if ( pos_remaining > 1 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            unsigned long lck;
            if ( parse_decimal ( sub_argv[optind], &lck ) != EXIT_SUCCESS || lck > 1 ) {
                fprintf ( stderr, "Error: lock state must be 0 or 1\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_lock_by_id ( &disc, (uint8_t) file_id, (uint8_t) lck, fsmz_dir_items );
        } else if ( pos_remaining >= 2 ) {
            /* <name> <0|1>: dva poziční argumenty */
            if ( pos_remaining > 2 ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            unsigned long lck;
            if ( parse_decimal ( sub_argv[optind + 1], &lck ) != EXIT_SUCCESS || lck > 1 ) {
                fprintf ( stderr, "Error: lock state must be 0 or 1\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_lock_by_name ( &disc, sub_argv[optind], (uint8_t) lck, fsmz_dir_items );
        } else {
            fprintf ( stderr, "Error: lock command requires <name> 0|1 or --id N 0|1\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

    /* ---- chtype ---- */
    } else if ( strcmp ( subcmd, "chtype" ) == 0 ) {

        static struct option chtype_opts[] = {
            { "id", required_argument, NULL, 'i' },
            { NULL, 0,                 NULL,  0  }
        };
        long file_id = -1;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", chtype_opts, NULL ) ) != -1 ) {
            if ( c == 'i' ) {
                unsigned long id;
                if ( parse_decimal ( optarg, &id ) != EXIT_SUCCESS || id > 255 ) {
                    fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
                file_id = (long) id;
            } else {
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
        }
        if ( file_id < 0 || optind >= sub_argc ) {
            fprintf ( stderr, "Error: chtype command requires --id N T (T is hex ftype)\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* --id N <T>: jeden poziční argument */
        if ( sub_argc - optind > 1 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        uint8_t ftype;
        if ( parse_hex_byte ( sub_argv[optind], &ftype ) != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Invalid ftype value '%s'\n", sub_argv[optind] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_chtype_by_id ( &disc, (uint8_t) file_id, ftype, fsmz_dir_items );

    /* ---- set ---- */
    } else if ( strcmp ( subcmd, "set" ) == 0 ) {

        static struct option set_addr_opts[] = {
            { "id",    required_argument, NULL, 'i' },
            { "fstrt", required_argument, NULL, 's' },
            { "fexec", required_argument, NULL, 'e' },
            { "ftype", required_argument, NULL, 't' },
            { "force", no_argument,       NULL, 'F' },
            { NULL,    0,                 NULL,  0  }
        };
        long file_id = -1;
        uint16_t fstrt = 0, fexec = 0;
        uint8_t ftype = 0;
        int has_fstrt = 0, has_fexec = 0, has_ftype = 0;
        int force = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", set_addr_opts, NULL ) ) != -1 ) {
            switch ( c ) {
                case 'i': {
                    unsigned long id;
                    if ( parse_decimal ( optarg, &id ) != EXIT_SUCCESS || id > 255 ) {
                        fprintf ( stderr, "Error: Invalid file ID '%s'\n", optarg );
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    file_id = (long) id;
                    break;
                }
                case 's': {
                    char *endptr;
                    unsigned long v = strtoul ( optarg, &endptr, 16 );
                    if ( endptr == optarg || *endptr != '\0' || v > 0xFFFF ) {
                        fprintf ( stderr, "Error: Invalid --fstrt value '%s' (expected 0x0000-0xFFFF)\n", optarg );
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    fstrt = (uint16_t) v; has_fstrt = 1;
                    break;
                }
                case 'e': {
                    char *endptr;
                    unsigned long v = strtoul ( optarg, &endptr, 16 );
                    if ( endptr == optarg || *endptr != '\0' || v > 0xFFFF ) {
                        fprintf ( stderr, "Error: Invalid --fexec value '%s' (expected 0x0000-0xFFFF)\n", optarg );
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    fexec = (uint16_t) v; has_fexec = 1;
                    break;
                }
                case 't': {
                    if ( parse_hex_byte ( optarg, &ftype ) != EXIT_SUCCESS || ftype == 0x00 ) {
                        fprintf ( stderr, "Error: Invalid --ftype value '%s' (expected 0x01-0xFF)\n", optarg );
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                    }
                    has_ftype = 1;
                    break;
                }
                case 'F': force = 1; break;
                default:
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
            }
        }
        if ( !has_fstrt && !has_fexec && !has_ftype ) {
            fprintf ( stderr, "Error: set requires at least one of --fstrt, --fexec, --ftype\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        if ( file_id >= 0 ) {
            if ( optind < sub_argc ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_set_addr_by_id ( &disc, (uint8_t) file_id,
                                         fstrt, has_fstrt,
                                         fexec, has_fexec,
                                         ftype, has_ftype,
                                         fsmz_dir_items, (uint8_t) force );
        } else if ( optind < sub_argc ) {
            if ( optind + 1 < sub_argc ) {
                fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_set_addr_by_name ( &disc, sub_argv[optind],
                                           fstrt, has_fstrt,
                                           fexec, has_fexec,
                                           ftype, has_ftype,
                                           fsmz_dir_items, (uint8_t) force );
        } else {
            fprintf ( stderr, "Error: set command requires <name> or --id N\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

    /* ---- boot ---- */
    } else if ( strcmp ( subcmd, "boot" ) == 0 ) {

        /* Pokud první poziční argument chybí nebo začíná "--", jde o
           variantu "boot [--fstrt HEX] [--fexec HEX] [--ftype HEX]
           [--name NAME]" - volitelná úprava bootstrap hlavičky s
           následným výpisem. */
        if ( sub_argc < 2 || sub_argv[1][0] == '-' ) {
            static struct option boot_info_opts[] = {
                { "fstrt", required_argument, NULL, 's' },
                { "fexec", required_argument, NULL, 'e' },
                { "ftype", required_argument, NULL, 't' },
                { "name",  required_argument, NULL, 'n' },
                { NULL,    0,                 NULL,  0  }
            };
            uint16_t b_fstrt = 0, b_fexec = 0;
            uint8_t b_ftype = 0;
            const char *b_name = NULL;
            int has_fstrt = 0, has_fexec = 0, has_ftype = 0, has_name = 0;
            optind = 0;
            int c;
            while ( ( c = getopt_long ( sub_argc, sub_argv, "", boot_info_opts, NULL ) ) != -1 ) {
                switch ( c ) {
                    case 's': {
                        char *endptr;
                        unsigned long v = strtoul ( optarg, &endptr, 16 );
                        if ( endptr == optarg || *endptr != '\0' || v > 0xFFFF ) {
                            fprintf ( stderr, "Error: Invalid --fstrt value '%s' (expected 0x0000-0xFFFF)\n", optarg );
                            mzdsk_disc_close ( &disc );
                            return EXIT_FAILURE;
                        }
                        b_fstrt = (uint16_t) v; has_fstrt = 1;
                        break;
                    }
                    case 'e': {
                        char *endptr;
                        unsigned long v = strtoul ( optarg, &endptr, 16 );
                        if ( endptr == optarg || *endptr != '\0' || v > 0xFFFF ) {
                            fprintf ( stderr, "Error: Invalid --fexec value '%s' (expected 0x0000-0xFFFF)\n", optarg );
                            mzdsk_disc_close ( &disc );
                            return EXIT_FAILURE;
                        }
                        b_fexec = (uint16_t) v; has_fexec = 1;
                        break;
                    }
                    case 't': {
                        if ( parse_hex_byte ( optarg, &b_ftype ) != EXIT_SUCCESS || b_ftype == 0x00 ) {
                            fprintf ( stderr, "Error: Invalid --ftype value '%s' (expected 0x01-0xFF)\n", optarg );
                            mzdsk_disc_close ( &disc );
                            return EXIT_FAILURE;
                        }
                        has_ftype = 1;
                        break;
                    }
                    case 'n':
                        b_name = optarg;
                        has_name = 1;
                        break;
                    default:
                        mzdsk_disc_close ( &disc );
                        return EXIT_FAILURE;
                }
            }
            if ( optind < sub_argc ) {
                fprintf ( stderr, "Error: 'boot' has too many arguments (unexpected '%s')\n", sub_argv[optind] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }

            if ( has_fstrt || has_fexec || has_ftype || has_name ) {
                /* nejdřív aktualizace hlavičky, pak zobrazení info */
                err = cmd_boot_set_header ( &disc, b_name, has_name,
                                              b_fstrt, has_fstrt,
                                              b_fexec, has_fexec,
                                              b_ftype, has_ftype );
                if ( err == MZDSK_RES_OK ) {
                    err = cmd_boot_info ( &disc, output_format );
                }
            } else {
                err = cmd_boot_info ( &disc, output_format );
            }
        } else if ( strcmp ( sub_argv[1], "put" ) == 0 ) {
            if ( sub_argc < 3 ) {
                fprintf ( stderr, "Error: boot put requires <mzf> argument\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            if ( sub_argc > 3 ) {
                fprintf ( stderr, "Error: 'boot %s' has too many arguments (unexpected '%s')\n", sub_argv[1], sub_argv[3] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_boot_put ( &disc, sub_argv[2], BOOTSTRAP_NORMAL, false );
        } else if ( strcmp ( sub_argv[1], "get" ) == 0 ) {
            if ( sub_argc < 3 ) {
                fprintf ( stderr, "Error: boot get requires <mzf> argument\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            if ( sub_argc > 3 ) {
                fprintf ( stderr, "Error: 'boot %s' has too many arguments (unexpected '%s')\n", sub_argv[1], sub_argv[3] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_boot_get ( &disc, sub_argv[2] );
        } else if ( strcmp ( sub_argv[1], "clear" ) == 0 ) {
            if ( sub_argc > 2 ) {
                fprintf ( stderr, "Error: 'boot clear' does not accept extra arguments (got '%s')\n", sub_argv[2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_boot_clear ( &disc );
        } else if ( strcmp ( sub_argv[1], "mini" ) == 0 || strcmp ( sub_argv[1], "bottom" ) == 0 ) {
            /* parsuj volitelný --no-fsmz-compat flag */
            bool no_fsmz_compat = false;
            char *mzf_arg = NULL;
            for ( int i = 2; i < sub_argc; i++ ) {
                if ( strcmp ( sub_argv[i], "--no-fsmz-compat" ) == 0 ) {
                    no_fsmz_compat = true;
                } else if ( !mzf_arg ) {
                    mzf_arg = sub_argv[i];
                } else {
                    fprintf ( stderr, "Error: 'boot %s' has too many arguments (unexpected '%s')\n", sub_argv[1], sub_argv[i] );
                    mzdsk_disc_close ( &disc );
                    return EXIT_FAILURE;
                }
            }
            if ( !mzf_arg ) {
                fprintf ( stderr, "Error: boot %s requires <mzf> argument\n", sub_argv[1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_boot_put ( &disc, mzf_arg, BOOTSTRAP_BOTTOM, no_fsmz_compat );
        } else if ( strcmp ( sub_argv[1], "over" ) == 0 ) {
            if ( sub_argc < 3 ) {
                fprintf ( stderr, "Error: boot over requires <mzf> argument\n" );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            if ( sub_argc > 3 ) {
                fprintf ( stderr, "Error: 'boot %s' has too many arguments (unexpected '%s')\n", sub_argv[1], sub_argv[3] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            err = cmd_boot_put ( &disc, sub_argv[2], BOOTSTRAP_OVER, false );
        } else {
            fprintf ( stderr, "Error: Unknown boot subcommand '%s'\n", sub_argv[1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }

    /* ---- format ---- */
    } else if ( strcmp ( subcmd, "format" ) == 0 ) {

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
        err = cmd_fast_format ( &disc );

    /* ---- repair ---- */
    } else if ( strcmp ( subcmd, "repair" ) == 0 ) {

        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'repair' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_repair_dinfo ( &disc, fsmz_dir_items );

    /* ---- defrag ---- */
    } else if ( strcmp ( subcmd, "defrag" ) == 0 ) {

        if ( sub_argc > 1 ) {
            fprintf ( stderr, "Error: 'defrag' does not accept extra arguments (got '%s')\n", sub_argv[1] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* Audit M-17 */
        if ( !confirm_destructive_op ( "defragment (reorganize all file blocks)" ) ) {
            fprintf ( stderr, "Aborted.\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        err = cmd_defrag ( &disc, fsmz_dir_items );

    /* ---- dump-block ---- */
    } else if ( strcmp ( subcmd, "dump-block" ) == 0 ) {

        static struct option dblk_opts[] = {
            { "dump-charset", required_argument, NULL, 'd' },
            { "cnv",          no_argument,       NULL, 'c' },
            { NULL,           0,                 NULL,  0  }
        };
        en_MZDSK_HEXDUMP_CHARSET dump_charset = MZDSK_HEXDUMP_CHARSET_RAW;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", dblk_opts, NULL ) ) != -1 ) {
            if ( c == 'c' ) {
                dump_charset = MZDSK_HEXDUMP_CHARSET_EU;
            } else if ( c == 'd' ) {
                if ( parse_dump_charset ( optarg, &dump_charset ) != EXIT_SUCCESS ) {
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
        unsigned long block_num;
        if ( parse_decimal ( sub_argv[optind], &block_num ) != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Invalid block number '%s'\n", sub_argv[optind] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        uint16_t max_block = (uint16_t) ( disc.tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK );
        if ( block_num >= max_block ) {
            fprintf ( stderr, "Error: Invalid block number %lu (max %u)\n", block_num, max_block - 1 );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* N [bytes]: max 2 poziční argumenty */
        if ( sub_argc - optind > 2 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 2] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        size_t blk_size = 0;
        if ( optind + 1 < sub_argc ) {
            unsigned long val;
            if ( parse_decimal ( sub_argv[optind + 1], &val ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Invalid size '%s'\n", sub_argv[optind + 1] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            blk_size = (size_t) val;
        }
        err = cmd_dump_block ( &disc, (uint16_t) block_num, blk_size, dump_charset );

    /* ---- get-block ---- */
    } else if ( strcmp ( subcmd, "get-block" ) == 0 ) {

        static struct option gblk_opts[] = {
            { "noinv", no_argument, NULL, 'n' },
            { NULL,    0,           NULL,  0  }
        };
        int noinv = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", gblk_opts, NULL ) ) != -1 ) {
            if ( c == 'n' ) noinv = 1;
            else { mzdsk_disc_close ( &disc ); return EXIT_FAILURE; }
        }
        int pos_remaining = sub_argc - optind;
        if ( pos_remaining < 2 ) {
            fprintf ( stderr, "Error: get-block requires N <file> [bytes]\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* N <file> [bytes]: max 3 poziční argumenty */
        if ( pos_remaining > 3 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 3] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        unsigned long block_num;
        if ( parse_decimal ( sub_argv[optind], &block_num ) != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Invalid block number '%s'\n", sub_argv[optind] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        uint16_t max_block = (uint16_t) ( disc.tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK );
        if ( block_num >= max_block ) {
            fprintf ( stderr, "Error: Invalid block number %lu (max %u)\n", block_num, max_block - 1 );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        const char *blk_file = sub_argv[optind + 1];
        size_t blk_size = 0;
        if ( pos_remaining >= 3 ) {
            unsigned long val;
            if ( parse_decimal ( sub_argv[optind + 2], &val ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Invalid size '%s'\n", sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            blk_size = (size_t) val;
        }
        err = cmd_get_block ( &disc, (uint16_t) block_num, blk_file, blk_size, noinv );

    /* ---- put-block ---- */
    } else if ( strcmp ( subcmd, "put-block" ) == 0 ) {

        static struct option pblk_opts[] = {
            { "noinv", no_argument, NULL, 'n' },
            { NULL,    0,           NULL,  0  }
        };
        int noinv = 0;
        optind = 0;
        int c;
        while ( ( c = getopt_long ( sub_argc, sub_argv, "", pblk_opts, NULL ) ) != -1 ) {
            if ( c == 'n' ) noinv = 1;
            else { mzdsk_disc_close ( &disc ); return EXIT_FAILURE; }
        }
        int pos_remaining = sub_argc - optind;
        if ( pos_remaining < 2 ) {
            fprintf ( stderr, "Error: put-block requires N <file> [bytes] [offset]\n" );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        /* N <file> [bytes] [offset]: max 4 poziční argumenty */
        if ( pos_remaining > 4 ) {
            fprintf ( stderr, "Error: '%s' has too many arguments (unexpected '%s')\n", subcmd, sub_argv[optind + 4] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        unsigned long block_num;
        if ( parse_decimal ( sub_argv[optind], &block_num ) != EXIT_SUCCESS ) {
            fprintf ( stderr, "Error: Invalid block number '%s'\n", sub_argv[optind] );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        uint16_t max_block = (uint16_t) ( disc.tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK );
        if ( block_num >= max_block ) {
            fprintf ( stderr, "Error: Invalid block number %lu (max %u)\n", block_num, max_block - 1 );
            mzdsk_disc_close ( &disc );
            return EXIT_FAILURE;
        }
        const char *blk_file = sub_argv[optind + 1];
        size_t blk_size = 0, blk_offset = 0;
        if ( pos_remaining >= 3 ) {
            unsigned long val;
            if ( parse_decimal ( sub_argv[optind + 2], &val ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Invalid size '%s'\n", sub_argv[optind + 2] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            blk_size = (size_t) val;
        }
        if ( pos_remaining >= 4 ) {
            unsigned long val;
            if ( parse_decimal ( sub_argv[optind + 3], &val ) != EXIT_SUCCESS ) {
                fprintf ( stderr, "Error: Invalid offset '%s'\n", sub_argv[optind + 3] );
                mzdsk_disc_close ( &disc );
                return EXIT_FAILURE;
            }
            blk_offset = (size_t) val;
        }
        err = cmd_put_block ( &disc, (uint16_t) block_num, blk_file, blk_size, blk_offset, noinv );

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
        /* MZDSK_RES_FORMAT_ERROR slouží v tomto nástroji jako sentinel
         * "chyba už byla konkrétně reportovaná command handlerem" -
         * používá se jak pro skutečný format mismatch, tak pro jiné
         * situace, kde handler sám vypsal konkrétní hlášku (např.
         * check_output_overwrite). Pro ostatní kódy main ještě doplní
         * generickou hlášku, aby uživatel dostal alespoň obecnou
         * informaci o povaze chyby. */
        if ( err != MZDSK_RES_FORMAT_ERROR ) {
            fprintf ( stderr, "Error: %s\n", mzdsk_get_error ( err ) );
        }
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
