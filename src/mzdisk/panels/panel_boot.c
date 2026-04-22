/**
 * @file panel_boot.c
 * @brief Logika boot sector panelu - čtení IPLPRO/DINFO a bootstrap management.
 *
 * Čte alokační blok 0 (IPLPRO hlavička) a blok 15 (DINFO) a převádí
 * raw data do zobrazitelné formy. Konvertuje jména a komentáře
 * ze Sharp MZ ASCII. Klasifikuje typ bootstrapu (Mini/Bottom/Normal/Over FSMZ).
 *
 * Bootstrap management implementuje operace Put/Get/Clear pro bottom
 * bootstrap (všechny FS) i FAREA bootstrap (jen plný FSMZ).
 *
 * Pro CP/M disky doplňuje informace o systémových stopách (reserved
 * tracks s CCP+BDOS+BIOS), které miniboot načítá do paměti.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "panel_boot.h"
#include "i18n.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/dsk/dsk_tools.h"


/**
 * @brief Konvertuje jméno z IPLPRO hlavičky ze Sharp MZ ASCII.
 *
 * Kopíruje bajty fname[] a konvertuje přes sharpmz_cnv_from().
 * Terminuje na 0x0d nebo po dosažení FSMZ_IPLFNAME_LENGTH.
 *
 * @param[out] dst Výstupní buffer (min 18 bajtů, null-terminated).
 * @param fname  Zdrojové jméno v Sharp MZ ASCII.
 */
static void convert_name ( char *dst, const uint8_t *fname )
{
    int i = 0;
    while ( i < FSMZ_IPLFNAME_LENGTH && fname[i] != FSMZ_FNAME_TERMINATOR && fname[i] >= 0x20 ) {
        dst[i] = (char) sharpmz_cnv_from ( fname[i] );
        i++;
    }
    dst[i] = '\0';
}



/**
 * @brief Klasifikuje typ bootstrapu podle pozice a rozsahu bloků.
 *
 * - Mini: bloky 1-14 (FSMZ kompatibilní - DINFO/dir nedotčeny)
 * - Bottom: blok >= 1, blok < 16, ale block_end > 14 (přesahuje do DINFO/dir)
 * - Normal: blok >= farea (v souborové oblasti)
 * - Over FSMZ: blok > dinfo.blocks (za hranicí souborové oblasti)
 *
 * @param data Datový model s naplněnými block, block_end a dinfo poli.
 *
 * @pre data->block_count > 0 (bootstrap má nenulovou velikost).
 */
static void classify_boot_type ( st_PANEL_BOOT_DATA *data )
{
    if ( data->block >= 1 && data->block_end <= 14 ) {
        strncpy ( data->boot_type, "Mini", sizeof ( data->boot_type ) );
    } else if ( data->block >= 1 && data->block < 16 && data->block_end > 14 ) {
        strncpy ( data->boot_type, "Bottom", sizeof ( data->boot_type ) );
    } else if ( data->has_dinfo && data->block > data->dinfo_blocks ) {
        strncpy ( data->boot_type, "Over FSMZ", sizeof ( data->boot_type ) );
    } else {
        strncpy ( data->boot_type, "Normal", sizeof ( data->boot_type ) );
    }
}


/**
 * @brief Naplní informace o systémových stopách pro CP/M disky.
 *
 * CP/M disk má DPB.off rezervovaných absolutních stop. Jedna z nich
 * (abs. stopa 1) je FSMZ boot track (16x256B) s minibootem. Ostatní
 * rezervované stopy obsahují CP/M systém (CCP+BDOS+BIOS), který
 * miniboot načítá do paměti Z80.
 *
 * Funkce spočítá počet systémových stop, jejich celkovou velikost
 * a vytvoří textový popis rozsahu absolutních stop.
 *
 * @param data   Datový model boot panelu (výstup).
 * @param disc   Otevřený diskový obraz.
 * @param detect Výsledek auto-detekce (obsahuje cpm_dpb).
 *
 * @pre data != NULL, detect->type == MZDSK_FS_CPM
 * @post Při úspěchu data->has_system_tracks == true.
 */
static void load_system_tracks ( st_PANEL_BOOT_DATA *data,
                                 st_MZDSK_DISC *disc,
                                 st_MZDSK_DETECT_RESULT *detect )
{
    uint16_t off = detect->cpm_dpb.off;
    if ( off < 2 ) return; /* minimálně boot track + 1 systémová stopa */

    data->has_system_tracks = true;
    data->system_tracks_off = off;
    data->system_tracks_count = off - 1; /* bez boot tracku */

    /* velikost jedné datové stopy z DPB: spt * 128B (logické sektory) */
    uint32_t data_track_bytes = (uint32_t) detect->cpm_dpb.spt * MZDSK_CPM_RECORD_SIZE;
    data->system_tracks_size = (uint32_t) data->system_tracks_count * data_track_bytes;

    /* textový popis rozsahu absolutních stop (boot track=1 vynechat) */
    /* abs stopa 0 je vždy systémová, abs stopa 1 je boot, abs stopy 2..off-1 systémové */
    if ( off == 2 ) {
        /* jen abs stopa 0 */
        snprintf ( data->system_tracks_range, sizeof ( data->system_tracks_range ), "0" );
    } else if ( off == 3 ) {
        snprintf ( data->system_tracks_range, sizeof ( data->system_tracks_range ), "0, 2" );
    } else {
        snprintf ( data->system_tracks_range, sizeof ( data->system_tracks_range ),
                   "0, 2-%d", off - 1 );
    }

    (void) disc;
}


/**
 * @brief Vypočítá maximální počet bloků pro bottom bootstrap.
 *
 * Na plném FSMZ v režimu Mini (preserve): 14 bloků (1-14, před DINFO).
 * Na plném FSMZ v režimu free (!preserve): celkový počet FSMZ bloků - 1.
 * Na ostatních discích: 15 bloků (1-15, celá boot stopa bez IPLPRO).
 *
 * @param data Datový model s informací o FS typu a preserve_fsmz.
 * @param disc Otevřený disk (pro zjištění celkového počtu FSMZ bloků).
 */
static void calc_max_bottom_blocks ( st_PANEL_BOOT_DATA *data, st_MZDSK_DISC *disc )
{
    if ( data->is_full_fsmz ) {
        if ( data->preserve_fsmz ) {
            /* Mini režim: bloky 1-14, DINFO na 15, dir od 16 */
            data->max_bottom_blocks = FSMZ_SECTORS_ON_TRACK - 2;
        } else {
            /* Volný režim: celý disk kromě IPLPRO bloku */
            uint16_t total = 0;
            if ( disc->tracks_rules ) {
                total = disc->tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK;
            }
            data->max_bottom_blocks = ( total > 1 ) ? ( total - 1 ) : 0;
        }
    } else {
        /* Non-FSMZ: boot track má 16 sektorů, blok 0 = IPLPRO */
        data->max_bottom_blocks = FSMZ_SECTORS_ON_TRACK - 1;
    }
}


void panel_boot_load ( st_PANEL_BOOT_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect )
{
    /* zachovat UI stav přes reload */
    bool prev_preserve = data->preserve_fsmz;

    memset ( data, 0, sizeof ( *data ) );

    /* obnovit UI stav */
    data->preserve_fsmz = prev_preserve;

    /* kontrola, zda disk má boot track */
    if ( !disc->tracks_rules || disc->tracks_rules->mzboot_track != 1 ) {
        return;
    }

    data->is_loaded = true;
    data->fs_type = detect->type;
    data->is_full_fsmz = ( disc->format == DSK_TOOLS_IDENTFORMAT_MZBASIC );

    /* výchozí preserve_fsmz na true při prvním loadu */
    if ( data->is_full_fsmz && !prev_preserve ) {
        data->preserve_fsmz = true;
    }

    /* DINFO blok (alokační blok 15) */
    st_FSMZ_DINFO_BLOCK dinfo;
    if ( fsmz_read_dinfo ( disc, &dinfo ) == MZDSK_RES_OK ) {
        data->has_dinfo = true;
        data->volume_number = dinfo.volume_number;
        data->farea = dinfo.farea;
        data->dinfo_used = dinfo.used;
        data->dinfo_blocks = dinfo.blocks;
    }

    /* IPLPRO hlavička (alokační blok 0) */
    st_FSMZ_IPLPRO_BLOCK iplpro;
    if ( fsmz_read_iplpro ( disc, &iplpro ) != MZDSK_RES_OK ) {
        calc_max_bottom_blocks ( data, disc );
        return;
    }

    data->has_iplpro = true;
    data->iplpro_valid = ( fsmz_tool_test_iplpro_header ( &iplpro ) == EXIT_SUCCESS );

    /* Raw ftype se ukládá vždy (pro zobrazení v UI i u nevalidní hlavičky) */
    data->ftype = iplpro.ftype;

    /* Zbylé hodnoty mají smysl jen u validní IPLPRO hlavičky.
       Při nevalidní hlavičce (prázdný disk, poškozená data) by čtení
       polí jako block/fsize/fstrt dávalo nesmyslné výsledky. */
    if ( data->iplpro_valid ) {
        data->fsize = iplpro.fsize;
        data->fstrt = iplpro.fstrt;
        data->fexec = iplpro.fexec;
        data->block = iplpro.block;

        /* odvozené hodnoty */
        data->block_count = fsmz_blocks_from_size ( iplpro.fsize );
        if ( data->block_count > 0 ) {
            data->block_end = data->block + data->block_count - 1;
        }

        convert_name ( data->name, iplpro.fname );

        /* uložit raw Sharp MZ bajty pro konverzi dle zvoleného kódování */
        int k = 0;
        while ( k < FSMZ_IPLFNAME_LENGTH && iplpro.fname[k] != FSMZ_FNAME_TERMINATOR && iplpro.fname[k] >= 0x20 ) {
            data->mz_name[k] = iplpro.fname[k];
            k++;
        }
        data->mz_name[k] = 0;
        data->mz_name_len = k;

        classify_boot_type ( data );
    }

    /* maximální počet bloků pro bottom bootstrap */
    calc_max_bottom_blocks ( data, disc );

    /* systémové stopy (CP/M) */
    if ( detect->type == MZDSK_FS_CPM ) {
        load_system_tracks ( data, disc, detect );
    }
}


/* =========================================================================
 * Bootstrap management - operace
 * ========================================================================= */


/**
 * @brief Načte MZF soubor do paměti a přečte hlavičku a tělo.
 *
 * @param mzf_path Cesta k MZF souboru.
 * @param[out] hdr Výstupní MZF hlavička.
 * @param[out] body Výstupní buffer pro tělo (min MZF_MAX_BODY_SIZE bajtů).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES load_mzf_file ( const char *mzf_path, st_MZF_HEADER *hdr, uint8_t *body )
{
    st_HANDLER *handler = generic_driver_open_memory_from_file ( NULL, &g_memory_driver_realloc, mzf_path );
    if ( !handler ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    generic_driver_set_handler_readonly_status ( handler, 1 );

    if ( EXIT_SUCCESS != mzf_read_header ( handler, hdr ) ) {
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    if ( EXIT_SUCCESS != mzf_read_body ( handler, body, hdr->fsize ) ) {
        generic_driver_close ( handler );
        free ( handler );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    generic_driver_close ( handler );
    free ( handler );
    return MZDSK_RES_OK;
}


/**
 * @brief Vytvoří IPLPRO hlavičku z MZF hlavičky.
 *
 * Naplní standardní IPLPRO pole: ftype=0x03, klíčové slovo "IPLPRO",
 * jméno, velikost, adresy a komentář z MZF hlavičky.
 *
 * @param[out] iplpro Výstupní IPLPRO blok.
 * @param hdr Vstupní MZF hlavička.
 * @param start_block Počáteční alokační blok těla bootstrapu.
 */
static void build_iplpro_from_mzf ( st_FSMZ_IPLPRO_BLOCK *iplpro,
                                     const st_MZF_HEADER *hdr,
                                     uint16_t start_block )
{
    memset ( iplpro, 0, sizeof ( *iplpro ) );
    iplpro->ftype = 0x03;
    memcpy ( &iplpro->iplpro, "IPLPRO", 6 );
    iplpro->fsize = hdr->fsize;
    iplpro->fstrt = hdr->fstrt;
    iplpro->fexec = hdr->fexec;
    iplpro->block = start_block;

    memset ( iplpro->fname, FSMZ_FNAME_TERMINATOR, FSMZ_IPLFNAME_LENGTH );
    memcpy ( iplpro->fname, MZF_UINT8_FNAME ( hdr->fname ), FSMZ_IPLFNAME_LENGTH - 1 );

    memset ( iplpro->cmnt, 0x00, FSMZ_IPLCMNT_LENGTH );
    memcpy ( iplpro->cmnt, hdr->cmnt, MZF_CMNT_LENGTH );
}


en_MZDSK_RES panel_boot_get_bootstrap ( st_MZDSK_DISC *disc, const char *mzf_path )
{
    st_FSMZ_IPLPRO_BLOCK iplpro;
    en_MZDSK_RES err;

    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    if ( EXIT_SUCCESS != fsmz_tool_test_iplpro_header ( &iplpro ) ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* vytvořit MZF hlavičku z IPLPRO */
    st_MZF_HEADER *mzfhdr = mzf_tools_create_mzfhdr (
        iplpro.ftype, iplpro.fsize, iplpro.fstrt, iplpro.fexec,
        (uint8_t *) &iplpro.fname, FSMZ_IPLFNAME_LENGTH - 1,
        (uint8_t *) &iplpro.cmnt
    );
    if ( !mzfhdr ) return MZDSK_RES_UNKNOWN_ERROR;

    /* vytvořit MZF v paměti. Startovací velikost 1 + swelling_enabled,
       aby realloc driver mohl buffer zvětšovat při zápisu hlavičky a těla
       - analogie s panel_fsmz.c a CLI (src/tools/mzdsk-fsmz). Bez toho
       selhává i samotné mzf_write_header, což ústí v chybu exportu
       i tam, kde je zdroj (IPLPRO) platný, např. CP/M boot sektor
       s validní IPLPRO-like hlavičkou. */
    st_HANDLER *handler = generic_driver_open_memory ( NULL, &g_memory_driver_realloc, 1 );
    if ( !handler ) {
        free ( mzfhdr );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    handler->spec.memspec.swelling_enabled = 1;

    if ( EXIT_SUCCESS != mzf_write_header ( handler, mzfhdr ) ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Načíst tělo z bloků. Alokace 64 KB na heap místo stacku
     * (audit H-24: na MSYS2/Windows 1 MB default stack). */
    uint8_t *body = malloc ( MZF_MAX_BODY_SIZE );
    if ( !body ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    err = fsmz_read_blocks ( disc, iplpro.block, iplpro.fsize, body );
    if ( err ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        free ( body );
        return err;
    }

    if ( EXIT_SUCCESS != mzf_write_body ( handler, body, iplpro.fsize ) ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        free ( body );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* uložit do souboru */
    if ( EXIT_SUCCESS != generic_driver_save_memory ( handler, (char *) mzf_path ) ) {
        generic_driver_close ( handler );
        free ( handler );
        free ( mzfhdr );
        free ( body );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    generic_driver_close ( handler );
    free ( handler );
    free ( mzfhdr );
    free ( body );
    return MZDSK_RES_OK;
}


en_MZDSK_RES panel_boot_put_bottom ( st_PANEL_BOOT_DATA *data,
                                      st_MZDSK_DISC *disc,
                                      const char *mzf_path )
{
    st_MZF_HEADER mzfhdr;
    /* Heap místo stacku: 64 KB na stacku bylo na MSYS2/Windows na hraně
     * limit 1 MB default stacku, zejména v GUI threadech. Audit H-24. */
    uint8_t *body = malloc ( MZF_MAX_BODY_SIZE );
    if ( !body ) return MZDSK_RES_UNKNOWN_ERROR;
    en_MZDSK_RES err;

    /* ověřit, že IPLPRO je prázdný */
    st_FSMZ_IPLPRO_BLOCK iplpro_check;
    err = fsmz_read_iplpro ( disc, &iplpro_check );
    if ( err ) { free ( body ); return err; }

    if ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro_check ) ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "%s", _ ( "Existing IPLPRO header found. Clear bootstrap first." ) );
        data->show_error = true;
        free ( body );
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* načíst MZF */
    err = load_mzf_file ( mzf_path, &mzfhdr, body );
    if ( err ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "%s", _ ( "Cannot read MZF file." ) );
        data->show_error = true;
        free ( body );
        return err;
    }

    /* zkontrolovat velikost */
    uint16_t count_blocks = fsmz_blocks_from_size ( mzfhdr.fsize );
    calc_max_bottom_blocks ( data, disc );

    if ( count_blocks > data->max_bottom_blocks ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   _ ( "Bootstrap too large: needs %d blocks, max %d available." ),
                   count_blocks, data->max_bottom_blocks );
        data->show_error = true;
        free ( body );
        return MZDSK_RES_NO_SPACE;
    }

    /* zapsat tělo od bloku 1 */
    err = fsmz_write_blocks ( disc, 1, mzfhdr.fsize, body );
    if ( err ) { free ( body ); return err; }

    /* zapsat IPLPRO hlavičku */
    st_FSMZ_IPLPRO_BLOCK iplpro;
    build_iplpro_from_mzf ( &iplpro, &mzfhdr, 1 );

    err = fs_mz_write_iplpro ( disc, &iplpro );
    free ( body );
    return err ? err : MZDSK_RES_OK;
}


en_MZDSK_RES panel_boot_put_normal ( st_MZDSK_DISC *disc, const char *mzf_path )
{
    st_MZF_HEADER mzfhdr;
    /* Heap místo stacku - viz komentář v panel_boot_put_bottom. Audit H-24. */
    uint8_t *body = malloc ( MZF_MAX_BODY_SIZE );
    if ( !body ) return MZDSK_RES_UNKNOWN_ERROR;
    en_MZDSK_RES err;

    if ( disc->format != DSK_TOOLS_IDENTFORMAT_MZBASIC ) {
        free ( body );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* ověřit, že IPLPRO je prázdný */
    st_FSMZ_IPLPRO_BLOCK iplpro_check;
    err = fsmz_read_iplpro ( disc, &iplpro_check );
    if ( err ) { free ( body ); return err; }

    if ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro_check ) ) {
        free ( body );
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* načíst MZF */
    err = load_mzf_file ( mzf_path, &mzfhdr, body );
    if ( err ) { free ( body ); return err; }

    /* najít volné místo v FAREA */
    uint16_t count_blocks = fsmz_blocks_from_size ( mzfhdr.fsize );
    uint16_t start_block;

    err = fsmz_check_free_blocks ( disc, count_blocks, &start_block );
    if ( err ) { free ( body ); return err; }

    /* zapsat tělo */
    err = fsmz_write_blocks ( disc, start_block, mzfhdr.fsize, body );
    free ( body );
    if ( err ) return err;

    /* zapsat IPLPRO hlavičku */
    st_FSMZ_IPLPRO_BLOCK iplpro;
    build_iplpro_from_mzf ( &iplpro, &mzfhdr, start_block );

    err = fs_mz_write_iplpro ( disc, &iplpro );
    if ( err ) return err;

    /* aktualizovat DINFO bitmapu a volume */
    err = fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_SET, start_block, count_blocks );
    if ( err ) return err;

    err = fsmz_update_dinfo_volume_number ( disc, FSMZ_DINFO_MASTER );
    if ( err ) return err;

    return MZDSK_RES_OK;
}


en_MZDSK_RES panel_boot_put_over ( st_MZDSK_DISC *disc, const char *mzf_path )
{
    st_MZF_HEADER mzfhdr;
    /* Heap místo stacku - viz komentář v panel_boot_put_bottom. Audit H-24. */
    uint8_t *body = malloc ( MZF_MAX_BODY_SIZE );
    if ( !body ) return MZDSK_RES_UNKNOWN_ERROR;
    en_MZDSK_RES err;

    if ( disc->format != DSK_TOOLS_IDENTFORMAT_MZBASIC ) {
        free ( body );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* ověřit, že IPLPRO je prázdný */
    st_FSMZ_IPLPRO_BLOCK iplpro_check;
    err = fsmz_read_iplpro ( disc, &iplpro_check );
    if ( err ) { free ( body ); return err; }

    if ( EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro_check ) ) {
        free ( body );
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* načíst MZF */
    err = load_mzf_file ( mzf_path, &mzfhdr, body );
    if ( err ) { free ( body ); return err; }

    /* pozice: za oblastí pokrytou DINFO bitmapou */
    st_FSMZ_DINFO_BLOCK dinfo;
    err = fsmz_read_dinfo ( disc, &dinfo );
    if ( err ) { free ( body ); return err; }

    uint16_t start_block = ( FSMZ_FAREA_BITMAP_SIZE * 8 ) + dinfo.farea;

    /* zapsat tělo */
    err = fsmz_write_blocks ( disc, start_block, mzfhdr.fsize, body );
    free ( body );
    if ( err ) return err;

    /* zapsat IPLPRO hlavičku */
    st_FSMZ_IPLPRO_BLOCK iplpro;
    build_iplpro_from_mzf ( &iplpro, &mzfhdr, start_block );

    err = fs_mz_write_iplpro ( disc, &iplpro );
    if ( err ) return err;

    return MZDSK_RES_OK;
}


en_MZDSK_RES panel_boot_clear ( st_MZDSK_DISC *disc )
{
    en_MZDSK_RES err;

    /* na plném FSMZ: uvolnit bloky v DINFO bitmapě */
    if ( disc->format == DSK_TOOLS_IDENTFORMAT_MZBASIC ) {
        st_FSMZ_IPLPRO_BLOCK iplpro;
        err = fsmz_read_iplpro ( disc, &iplpro );
        if ( err == MZDSK_RES_OK && EXIT_SUCCESS == fsmz_tool_test_iplpro_header ( &iplpro ) ) {
            uint16_t count_blocks = fsmz_blocks_from_size ( iplpro.fsize );

            /* uvolnit v bitmapě jen pokud bootstrap je v FAREA oblasti */
            if ( iplpro.block >= FSMZ_DEFAULT_FAREA_BLOCK ) {
                err = fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_RESET, iplpro.block, count_blocks );
                if ( err ) return err;
            }

            /* nastavit disk jako slave (nebootovatelný) */
            err = fsmz_update_dinfo_volume_number ( disc, FSMZ_DINFO_SLAVE );
            if ( err ) return err;
        }
    }

    /* vyčistit IPLPRO blok */
    st_FSMZ_IPLPRO_BLOCK empty;
    memset ( &empty, 0x00, sizeof ( empty ) );
    return fs_mz_write_iplpro ( disc, &empty );
}


en_MZDSK_RES panel_boot_rename ( st_MZDSK_DISC *disc, const char *new_ascii_name )
{
    if ( disc == NULL || new_ascii_name == NULL ) return MZDSK_RES_INVALID_PARAM;

    uint8_t mz_fname[FSMZ_IPLFNAME_LENGTH];
    /* is_iplpro_fname=1 -> kratší délka (13 B) s terminátorem 0x0d */
    fsmz_tool_convert_ascii_to_mzfname ( mz_fname, (char *) new_ascii_name, 1 );

    return fsmz_set_iplpro_header ( disc, mz_fname, NULL, NULL, NULL );
}


en_MZDSK_RES panel_boot_set_header ( st_MZDSK_DISC *disc,
                                      uint16_t fstrt, uint16_t fexec,
                                      uint8_t ftype )
{
    if ( disc == NULL ) return MZDSK_RES_INVALID_PARAM;
    if ( ftype == 0x00 ) return MZDSK_RES_INVALID_PARAM;

    return fsmz_set_iplpro_header ( disc, NULL, &fstrt, &fexec, &ftype );
}
