/**
 * @file panel_create.c
 * @brief Logika vytváření nového DSK diskového obrazu.
 *
 * Obsahuje statická data předdefinovaných presetů, inicializaci stavu,
 * načítání presetů do editovatelných pravidel a kompletní workflow
 * pro vytvoření a volitelné formátování DSK obrazu.
 *
 * Podporované formátovací režimy:
 * - Neformátovaný disk (libovolný preset)
 * - Formátovaný MZ-BASIC (FSMZ)
 * - Formátovaný CP/M SD (Lamač)
 * - Formátovaný CP/M HD (Lucky-Soft)
 * - Formátovaný MRS (Veselý 1993)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "panels/panel_create.h"

#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"


/* ====================================================================
 *  Předdefinovaná data presetů
 * ==================================================================== */


/** @brief Mapa sektorů pro Lemmings stopu 16 (10 sektorů, custom řazení). */
static const uint8_t s_lemmings_sector_map[10] = { 1, 6, 2, 7, 3, 8, 4, 9, 5, 21 };


/* --- Pravidla pro každý preset --- */

/** @brief Pravidla pro Custom formát (výchozí: 16x256B normální). */
static const st_PANEL_CREATE_PREDEF_RULE s_rules_custom[] = {
    { 0, 16, 1, 1, NULL, 0xFF }
};

/** @brief Pravidla pro MZ-BASIC (16x256B normální na všech stopách). */
static const st_PANEL_CREATE_PREDEF_RULE s_rules_mzbasic[] = {
    { 0, 16, 1, 1, NULL, 0xFF }
};

/** @brief Pravidla pro CP/M SD (stopa 0: 9x512B LEC, stopa 1: boot 16x256B, stopy 2+: 9x512B LEC). */
static const st_PANEL_CREATE_PREDEF_RULE s_rules_cpm_sd[] = {
    { 0, 9,  2, 2, NULL, 0xE5 },
    { 1, 16, 1, 1, NULL, 0xFF },
    { 2, 9,  2, 2, NULL, 0xE5 }
};

/** @brief Pravidla pro CP/M HD (stopa 0: 18x512B LEC HD, stopa 1: boot 16x256B, stopy 2+: 18x512B LEC HD). */
static const st_PANEL_CREATE_PREDEF_RULE s_rules_cpm_hd[] = {
    { 0, 18, 2, 3, NULL, 0xE5 },
    { 1, 16, 1, 1, NULL, 0xFF },
    { 2, 18, 2, 3, NULL, 0xE5 }
};

/** @brief Pravidla pro MRS (stopa 0: 9x512B LEC, stopa 1: boot 16x256B, stopy 2+: 9x512B LEC). */
static const st_PANEL_CREATE_PREDEF_RULE s_rules_mrs[] = {
    { 0, 9,  2, 2, NULL, 0xE5 },
    { 1, 16, 1, 1, NULL, 0xFF },
    { 2, 9,  2, 2, NULL, 0xE5 }
};

/** @brief Pravidla pro Lemmings (stopa 16: 10x512B custom mapa). */
static const st_PANEL_CREATE_PREDEF_RULE s_rules_lemmings[] = {
    { 0,  9,  2, 2, NULL, 0xE5 },
    { 1,  16, 1, 1, NULL, 0xE5 },
    { 2,  9,  2, 2, NULL, 0xE5 },
    { 16, 10, 2, 0, s_lemmings_sector_map, 0xE5 },
    { 17, 9,  2, 2, NULL, 0xE5 }
};


/**
 * @brief Pole předdefinovaných presetů.
 *
 * Pořadí odpovídá indexům v combo boxu:
 * 0 = Custom, 1 = MZ-BASIC, 2 = CP/M SD, 3 = CP/M HD, 4 = MRS, 5 = Lemmings.
 */
static const st_PANEL_CREATE_PREDEF s_presets[PANEL_CREATE_PRESET_COUNT] = {
    { "Custom format",       2, 160, 1, s_rules_custom,   false },
    { "Sharp MZ-BASIC",      2, 160, 1, s_rules_mzbasic,  true  },
    { "Sharp LEC CP/M SD",   2, 160, 3, s_rules_cpm_sd,   true  },
    { "Sharp LEC CP/M HD",   2, 160, 3, s_rules_cpm_hd,   true  },
    { "Sharp MRS",           2, 160, 3, s_rules_mrs,       true  },
    { "Sharp Lemmings",      2, 160, 5, s_rules_lemmings,  false }
};


const st_PANEL_CREATE_PREDEF* panel_create_get_presets ( void )
{
    return s_presets;
}


/* ====================================================================
 *  Inicializace a načítání presetů
 * ==================================================================== */


void panel_create_load_preset ( st_PANEL_CREATE_DATA *data, int preset_idx )
{
    if ( !data || preset_idx < 0 || preset_idx >= PANEL_CREATE_PRESET_COUNT ) return;

    const st_PANEL_CREATE_PREDEF *p = &s_presets[preset_idx];

    data->preset_idx = preset_idx;
    data->sides = p->default_sides;
    data->tracks = p->default_tracks;
    data->count_rules = p->count_rules;
    data->format_filesystem = p->formattable;

    for ( int i = 0; i < p->count_rules && i < PANEL_CREATE_MAX_RULES; i++ ) {
        st_PANEL_CREATE_RULE *r = &data->rules[i];
        const st_PANEL_CREATE_PREDEF_RULE *pr = &p->rules[i];

        r->from_track = pr->from_track;
        r->sectors = pr->sectors;
        r->sector_size_idx = pr->ssize_idx;
        r->order_idx = pr->order_idx;
        r->filler = pr->filler;

        if ( pr->sector_map ) {
            memcpy ( r->sector_map, pr->sector_map, pr->sectors );
        } else {
            dsk_tools_make_sector_map ( pr->sectors,
                (en_DSK_SECTOR_ORDER_TYPE) pr->order_idx,
                r->sector_map );
        }
    }
}


void panel_create_init ( st_PANEL_CREATE_DATA *data, const st_MZDISK_CONFIG *cfg )
{
    if ( !data || !cfg ) return;

    memset ( data, 0, sizeof ( *data ) );
    data->is_open = false;
    data->created = false;
    data->error_msg[0] = '\0';

    strncpy ( data->filename, "new_disk", sizeof ( data->filename ) - 1 );
    strncpy ( data->directory, cfg->last_create_dir, sizeof ( data->directory ) - 1 );

    /* výchozí preset: MZ-BASIC (index 1) */
    panel_create_load_preset ( data, 1 );
}


/* ====================================================================
 *  Mapovací tabulka sector size index -> en_DSK_SECTOR_SIZE
 * ==================================================================== */


/** @brief Převodní tabulka z indexu velikosti sektoru na enum en_DSK_SECTOR_SIZE. */
static const en_DSK_SECTOR_SIZE s_ssize_values[] = {
    DSK_SECTOR_SIZE_128,    /* index 0 */
    DSK_SECTOR_SIZE_256,    /* index 1 */
    DSK_SECTOR_SIZE_512,    /* index 2 */
    DSK_SECTOR_SIZE_1024    /* index 3 */
};


/* ====================================================================
 *  Neformátované vytvoření disku (jen geometrie)
 * ==================================================================== */


/**
 * @brief Vytvoří neformátovaný DSK obraz podle pravidel z UI stavu.
 *
 * Sestaví st_DSK_DESCRIPTION z pravidel, otevře soubor a zavolá
 * dsk_tools_create_image().
 *
 * @param filepath Plná cesta k výstupnímu DSK souboru (ne const - driver bere char*).
 * @param data Datový model s pravidly geometrie.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @post Při chybě je popis uložen do data->error_msg.
 */
static int create_unformatted ( char *filepath, st_PANEL_CREATE_DATA *data )
{
    size_t desc_size = dsk_tools_compute_description_size ( (uint8_t) data->count_rules );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Memory allocation failed" );
        return EXIT_FAILURE;
    }
    memset ( desc, 0, desc_size );

    desc->tracks = (uint8_t) ( data->tracks / data->sides );
    desc->sides = (uint8_t) data->sides;
    desc->count_rules = (uint16_t) data->count_rules;

    for ( int r = 0; r < data->count_rules; r++ ) {
        dsk_tools_assign_description ( desc, (uint8_t) r,
            (uint8_t) data->rules[r].from_track,
            (uint8_t) data->rules[r].sectors,
            s_ssize_values[data->rules[r].sector_size_idx],
            (en_DSK_SECTOR_ORDER_TYPE) data->rules[r].order_idx,
            data->rules[r].sector_map,
            data->rules[r].filler );
    }

    st_HANDLER handler;
    st_DRIVER driver;
    generic_driver_file_init ( &driver );

    if ( !generic_driver_open_file ( &handler, &driver, filepath, FILE_DRIVER_OPMODE_W ) ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot create file: %.400s", filepath );
        free ( desc );
        return EXIT_FAILURE;
    }

    int ret = dsk_tools_create_image ( &handler, desc );
    generic_driver_close ( &handler );
    free ( desc );

    if ( ret != EXIT_SUCCESS ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Error creating DSK image" );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Formátované vytvoření disku
 * ==================================================================== */


/**
 * @brief Vytvoří zformátovaný MZ-BASIC (FSMZ) disk.
 *
 * @param filepath Plná cesta k výstupnímu souboru.
 * @param data Datový model.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int create_format_basic ( char *filepath, st_PANEL_CREATE_DATA *data )
{
    st_HANDLER handler;
    st_DRIVER driver;
    generic_driver_file_init ( &driver );

    if ( !generic_driver_open_file ( &handler, &driver, filepath, FILE_DRIVER_OPMODE_W ) ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot create file: %.400s", filepath );
        return EXIT_FAILURE;
    }

    uint8_t tracks_per_side = (uint8_t) ( data->tracks / data->sides );
    int ret = mzdsk_tools_format_basic ( &handler, tracks_per_side, (uint8_t) data->sides );
    generic_driver_close ( &handler );

    if ( ret != EXIT_SUCCESS ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Error formatting MZ-BASIC disk" );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Vytvoří zformátovaný CP/M SD disk.
 *
 * @param filepath Plná cesta k výstupnímu souboru.
 * @param data Datový model.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int create_format_cpm_sd ( char *filepath, st_PANEL_CREATE_DATA *data )
{
    st_HANDLER handler;
    st_DRIVER driver;
    generic_driver_file_init ( &driver );

    if ( !generic_driver_open_file ( &handler, &driver, filepath, FILE_DRIVER_OPMODE_W ) ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot create file: %.400s", filepath );
        return EXIT_FAILURE;
    }

    int ret = mzdsk_tools_format_cpm_sd ( &handler, (uint8_t) data->tracks, (uint8_t) data->sides );
    generic_driver_close ( &handler );

    if ( ret != EXIT_SUCCESS ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Error formatting CP/M SD disk" );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Vytvoří zformátovaný CP/M HD disk.
 *
 * @param filepath Plná cesta k výstupnímu souboru.
 * @param data Datový model.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int create_format_cpm_hd ( char *filepath, st_PANEL_CREATE_DATA *data )
{
    st_HANDLER handler;
    st_DRIVER driver;
    generic_driver_file_init ( &driver );

    if ( !generic_driver_open_file ( &handler, &driver, filepath, FILE_DRIVER_OPMODE_W ) ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot create file: %.400s", filepath );
        return EXIT_FAILURE;
    }

    int ret = mzdsk_tools_format_cpm_hd ( &handler, (uint8_t) data->tracks, (uint8_t) data->sides );
    generic_driver_close ( &handler );

    if ( ret != EXIT_SUCCESS ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Error formatting CP/M HD disk" );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/** @brief Výchozí číslo bloku, kde začíná FAT na MRS disketě. */
#define FSMRS_DEFAULT_FAT_BLOCK 36

/**
 * @brief Vytvoří zformátovaný MRS disk (dvoustupňový proces).
 *
 * Postup:
 * 1. Vytvoří DSK obraz s MRS geometrií (3 pravidla).
 * 2. Znovu otevře přes mzdsk_disc_open_memory().
 * 3. Nastaví fsmrs_sector_info_cb a zavolá fsmrs_format_fs().
 * 4. Uloží a zavře.
 *
 * @param filepath Plná cesta k výstupnímu souboru.
 * @param data Datový model.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int create_format_mrs ( char *filepath, st_PANEL_CREATE_DATA *data )
{
    /* Krok 1: Vytvořit prázdný DSK s MRS geometrií */
    uint8_t tracks_per_side = (uint8_t) ( data->tracks / data->sides );

    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( dsk_tools_compute_description_size ( 3 ) );
    if ( !desc ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Memory allocation failed" );
        return EXIT_FAILURE;
    }

    desc->count_rules = 3;
    desc->sides = (uint8_t) data->sides;
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

    st_HANDLER handler;
    st_DRIVER driver;
    generic_driver_file_init ( &driver );

    if ( !generic_driver_open_file ( &handler, &driver, filepath, FILE_DRIVER_OPMODE_W ) ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot create file: %.400s", filepath );
        free ( desc );
        return EXIT_FAILURE;
    }

    int ret = dsk_tools_create_image ( &handler, desc );
    free ( desc );
    generic_driver_close ( &handler );

    if ( ret != EXIT_SUCCESS ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Error creating MRS DSK image" );
        return EXIT_FAILURE;
    }

    /* Krok 2: Otevřít přes paměťový driver a inicializovat MRS FS */
    st_MZDSK_DISC disc;
    /* filepath je const, ale mzdsk_disc_open_memory potřebuje char* -
       uložíme kopii do created_filepath, který je už naplněný */
    en_MZDSK_RES err = mzdsk_disc_open_memory ( &disc, data->created_filepath, FILE_DRIVER_OPMODE_RW );
    if ( err != MZDSK_RES_OK ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot reopen DSK for MRS formatting: %s", mzdsk_get_error ( err ) );
        return EXIT_FAILURE;
    }

    disc.sector_info_cb = fsmrs_sector_info_cb;
    disc.sector_info_cb_data = NULL;

    en_MZDSK_RES res = fsmrs_format_fs ( &disc, FSMRS_DEFAULT_FAT_BLOCK );
    if ( res != MZDSK_RES_OK ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "MRS filesystem initialization failed: %s", mzdsk_get_error ( res ) );
        mzdsk_disc_close ( &disc );
        return EXIT_FAILURE;
    }

    res = mzdsk_disc_save ( &disc );
    mzdsk_disc_close ( &disc );

    if ( res != MZDSK_RES_OK ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot save MRS DSK file: %s", mzdsk_get_error ( res ) );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/* ====================================================================
 *  Hlavní execute funkce
 * ==================================================================== */


int panel_create_execute ( st_PANEL_CREATE_DATA *data )
{
    if ( !data ) return EXIT_FAILURE;

    data->error_msg[0] = '\0';
    data->created = false;

    /* Sestavit plnou cestu k souboru */
    snprintf ( data->created_filepath, sizeof ( data->created_filepath ),
               "%s/%s.dsk", data->directory, data->filename );

    /* Kontrola existence - pokud soubor existuje a nemáme potvrzení,
       požádáme o potvrzení přepisu přes confirm_overwrite dialog */
    if ( !data->confirm_overwrite ) {
        FILE *test = fopen ( data->created_filepath, "rb" );
        if ( test ) {
            fclose ( test );
            data->confirm_overwrite = true;
            return EXIT_FAILURE;
        }
    }
    data->confirm_overwrite = false;

    int ret;

    if ( data->format_filesystem ) {
        /* Formátovaný disk podle presetu */
        switch ( data->preset_idx ) {
            case 1: /* MZ-BASIC */
                ret = create_format_basic ( data->created_filepath, data );
                break;
            case 2: /* CP/M SD */
                ret = create_format_cpm_sd ( data->created_filepath, data );
                break;
            case 3: /* CP/M HD */
                ret = create_format_cpm_hd ( data->created_filepath, data );
                break;
            case 4: /* MRS */
                ret = create_format_mrs ( data->created_filepath, data );
                break;
            default:
                /* Neformátovatelný preset nebo Custom - fallback na neformátovaný */
                ret = create_unformatted ( data->created_filepath, data );
                break;
        }
    } else {
        /* Neformátovaný disk */
        ret = create_unformatted ( data->created_filepath, data );
    }

    if ( ret == EXIT_SUCCESS ) {
        data->created = true;
    }

    return ret;
}
