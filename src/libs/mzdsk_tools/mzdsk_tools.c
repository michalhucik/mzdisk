/**
 * @file   mzdsk_tools.c
 * @brief  Implementace presetových nástrojů pro vytváření DSK diskových obrazů Sharp MZ.
 *
 * Obsahuje definice předdefinovaných diskových geometrií (presetů)
 * a funkcí pro vytvoření prázdných i formátovaných DSK obrazů
 * pro MZ-BASIC a CP/M.
 *
 * Presety odpovídají diskovým formátům z emulátoru mz800new
 * (dsk_create_window.cpp) a nástroje fstool (fstool_dsk.c).
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

#include "mzdsk_tools.h"
#include "libs/mzdsk_global/mzdsk_global.h"


/* ================================================================ */
/*  Interní konstanty a pomocné funkce                              */
/* ================================================================ */


/** @brief Počet pravidel pro MZ-BASIC preset (jedno pravidlo pro celý disk). */
#define BASIC_RULES_COUNT       1

/** @brief Počet pravidel pro CP/M presety (stopa 0, boot stopa 1, stopy 2+). */
#define CPM_RULES_COUNT         3

/** @brief Počet pravidel pro MRS preset (shodné s CP/M). */
#define MRS_RULES_COUNT         3

/** @brief Počet pravidel pro Lemmings preset (5 pravidel: stopa 0, 1, 2-15, 16, 17+). */
#define LEMMINGS_RULES_COUNT    5

/** @brief Filler bajt pro FSMZ formát (sektory se vytvářejí s 0xFF). */
#define FSMZ_FILLER             0xFF

/** @brief Filler bajt pro CP/M datové sektory. */
#define CPM_FILLER              0xE5


/** @brief Custom sektor mapa pro stopu 16 presetu Lemmings (10 sektorů). */
static const uint8_t s_lemmings_sector_map[10] = { 1, 6, 2, 7, 3, 8, 4, 9, 5, 21 };


/**
 * @brief Názvy presetů pro zobrazení v UI.
 *
 * Indexovány hodnotami en_MZDSK_PRESET. Řetězce jsou statické
 * a platné po celou dobu běhu programu.
 */
static const char *s_preset_names[MZDSK_PRESET_COUNT] = {
    "Sharp MZ-BASIC",
    "Sharp LEC CP/M SD",
    "Sharp LEC CP/M HD",
    "Sharp MRS",
    "Sharp Lemmings"
};


/**
 * @brief Převede číslo FSMZ alokačního bloku na absolutní stopu a ID sektoru.
 *
 * FSMZ formát používá obracenou logiku stran v rámci dvoustranného DSK:
 * - sudé absolutní stopy (strana 0 v DSK) odpovídají lichým logickým stopám
 * - liché absolutní stopy (strana 1 v DSK) odpovídají sudým logickým stopám
 *
 * Blok se skládá z 16 sektorů (256 B). Horní bajt návratové hodnoty
 * obsahuje absolutní stopu, dolní bajt ID sektoru (1-based).
 *
 * @param block Číslo alokačního bloku (0-based).
 * @return Zakódovaná dvojice (abs_track << 8) | sector_id.
 *
 * @note Tato funkce je portem fsmz_block2trsec() z projektu fstool.
 */
static uint16_t mzdsk_fsmz_block2trsec ( uint16_t block ) {
    uint16_t trsec;

    /* Stopa: horních 8 bitů z (block * 16) */
    trsec = ( block << 4 ) & 0xff00;

    /* Obracení strany: sudá -> lichá, lichá -> sudá */
    if ( trsec & 0x0100 ) {
        trsec -= 0x0100;
    } else {
        trsec += 0x0100;
    }

    /* Sektor: spodní 4 bity bloku + 1 (sektory jsou 1-based) */
    trsec += ( (uint8_t) block & 0x0f ) + 1;

    return trsec;
}


/**
 * @brief Zapíše FSMZ blok do DSK obrazu s automatickou bitovou inverzí.
 *
 * Převede číslo bloku na absolutní stopu a sektor pomocí FSMZ mapování
 * (s obracenými stranami), invertuje data a zapíše je přes dsk_write_sector().
 *
 * @param h Handler DSK obrazu otevřený pro zápis.
 * @param block Číslo FSMZ alokačního bloku.
 * @param data Buffer s logickými daty (256 B). Obsah bude modifikován (invertován).
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě zápisu.
 *
 * @pre h != NULL, data != NULL.
 * @pre DSK obraz musí mít odpovídající stopu a sektor vytvořené.
 * @post Data v bufferu jsou po volání invertovaná (vedlejší efekt).
 *
 * @warning Funkce modifikuje obsah vstupního bufferu!
 */
static int mzdsk_write_fsmz_block ( st_HANDLER *h, uint16_t block, uint8_t *data ) {
    uint16_t trsec = mzdsk_fsmz_block2trsec ( block );
    uint8_t abs_track = ( trsec >> 8 ) & 0xff;
    uint8_t sector_id = trsec & 0xff;

    /* Invertovat data - FSMZ na disku ukládá invertovaně */
    mzdsk_invert_data ( data, MZDSK_FSMZ_SECTOR_SIZE );

    return dsk_write_sector ( h, abs_track, sector_id, data );
}


/**
 * @brief Vytvoří interně alokovanou strukturu st_DSK_DESCRIPTION a zavolá dsk_tools_create_image().
 *
 * Společný pomocný kód pro všechny presety a formátovací funkce.
 * Alokuje popis geometrie, nastaví globální parametry a vrátí ho
 * volajícímu k vyplnění pravidel.
 *
 * @param count_rules Počet pravidel geometrie.
 * @param tracks Počet stop per strana.
 * @param sides Počet stran (1 nebo 2).
 * @return Alokovaná a vyplněná struktura, nebo NULL při chybě alokace.
 *
 * @note Volající musí uvolnit vrácenou strukturu přes free().
 */
static st_DSK_DESCRIPTION* mzdsk_alloc_description ( uint8_t count_rules, uint8_t tracks, uint8_t sides ) {
    size_t desc_size = dsk_tools_compute_description_size ( count_rules );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( !desc ) {
        return NULL;
    }
    memset ( desc, 0, desc_size );
    desc->count_rules = count_rules;
    desc->tracks = tracks;
    desc->sides = sides;
    return desc;
}


/* ================================================================ */
/*  Implementace veřejného API                                      */
/* ================================================================ */


const char* mzdsk_tools_preset_name ( en_MZDSK_PRESET preset ) {
    if ( preset >= MZDSK_PRESET_COUNT ) {
        return "Unknown";
    }
    return s_preset_names[preset];
}


int mzdsk_tools_create_from_preset ( st_HANDLER *h, en_MZDSK_PRESET preset, uint8_t sides ) {

    if ( !h || ( sides != 1 && sides != 2 ) ) {
        return EXIT_FAILURE;
    }

    st_DSK_DESCRIPTION *desc = NULL;
    int ret;

    /* Presety definují 80 stop/stranu. Při sides=1 se vytvoří 80 abs. stop,
     * při sides=2 se vytvoří 160 abs. stop. */
    uint8_t tracks_per_side = 80;

    switch ( preset ) {

        case MZDSK_PRESET_BASIC:
            /*
             * MZ-BASIC: všechny stopy 16x256 B, normální řazení, filler 0xFF.
             */
            desc = mzdsk_alloc_description ( BASIC_RULES_COUNT, tracks_per_side, sides );
            if ( !desc ) return EXIT_FAILURE;
            dsk_tools_assign_description ( desc, 0, 0,
                MZDSK_FSMZ_SECTORS_ON_TRACK, MZDSK_FSMZ_SECTOR_SSIZE,
                DSK_SEC_ORDER_NORMAL, NULL, FSMZ_FILLER );
            break;

        case MZDSK_PRESET_CPM_SD:
            /*
             * LEC CP/M SD:
             * Stopa 0: 9x512 B, prokládané LEC, filler 0xE5.
             * Stopa 1: 16x256 B, normální (boot track), filler 0xFF.
             * Stopy 2+: 9x512 B, prokládané LEC, filler 0xE5.
             */
            desc = mzdsk_alloc_description ( CPM_RULES_COUNT, tracks_per_side, sides );
            if ( !desc ) return EXIT_FAILURE;
            dsk_tools_assign_description ( desc, 0, 0,
                9, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC, NULL, CPM_FILLER );
            dsk_tools_assign_description ( desc, 1, 1,
                MZDSK_FSMZ_SECTORS_ON_TRACK, MZDSK_FSMZ_SECTOR_SSIZE,
                DSK_SEC_ORDER_NORMAL, NULL, FSMZ_FILLER );
            dsk_tools_assign_description ( desc, 2, 2,
                9, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC, NULL, CPM_FILLER );
            break;

        case MZDSK_PRESET_CPM_HD:
            /*
             * LEC CP/M HD:
             * Stopa 0: 18x512 B, 2x prokládané LEC HD, filler 0xE5.
             * Stopa 1: 16x256 B, normální (boot track), filler 0xFF.
             * Stopy 2+: 18x512 B, 2x prokládané LEC HD, filler 0xE5.
             */
            desc = mzdsk_alloc_description ( CPM_RULES_COUNT, tracks_per_side, sides );
            if ( !desc ) return EXIT_FAILURE;
            dsk_tools_assign_description ( desc, 0, 0,
                18, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC_HD, NULL, CPM_FILLER );
            dsk_tools_assign_description ( desc, 1, 1,
                MZDSK_FSMZ_SECTORS_ON_TRACK, MZDSK_FSMZ_SECTOR_SSIZE,
                DSK_SEC_ORDER_NORMAL, NULL, FSMZ_FILLER );
            dsk_tools_assign_description ( desc, 2, 2,
                18, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC_HD, NULL, CPM_FILLER );
            break;

        case MZDSK_PRESET_MRS:
            /*
             * MRS:
             * Stopa 0: 9x512 B, prokládané LEC, filler 0xE5.
             * Stopa 1: 16x256 B, normální (boot track na straně 1), filler 0xFF.
             * Stopy 2+: 9x512 B, prokládané LEC, filler 0xE5.
             */
            desc = mzdsk_alloc_description ( MRS_RULES_COUNT, tracks_per_side, sides );
            if ( !desc ) return EXIT_FAILURE;
            dsk_tools_assign_description ( desc, 0, 0,
                9, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC, NULL, CPM_FILLER );
            dsk_tools_assign_description ( desc, 1, 1,
                MZDSK_FSMZ_SECTORS_ON_TRACK, MZDSK_FSMZ_SECTOR_SSIZE,
                DSK_SEC_ORDER_NORMAL, NULL, FSMZ_FILLER );
            dsk_tools_assign_description ( desc, 2, 2,
                9, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC, NULL, CPM_FILLER );
            break;

        case MZDSK_PRESET_LEMMINGS:
            /*
             * Lemmings:
             * Stopa 0: 9x512 B, prokládané LEC, filler 0xE5.
             * Stopa 1: 16x256 B, normální (boot track), filler 0xE5.
             * Stopy 2-15: 9x512 B, prokládané LEC, filler 0xE5.
             * Stopa 16: 10x512 B, custom mapa [1,6,2,7,3,8,4,9,5,21], filler 0xE5.
             * Stopy 17+: 9x512 B, prokládané LEC, filler 0xE5.
             */
            desc = mzdsk_alloc_description ( LEMMINGS_RULES_COUNT, tracks_per_side, sides );
            if ( !desc ) return EXIT_FAILURE;
            dsk_tools_assign_description ( desc, 0, 0,
                9, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC, NULL, CPM_FILLER );
            dsk_tools_assign_description ( desc, 1, 1,
                MZDSK_FSMZ_SECTORS_ON_TRACK, MZDSK_FSMZ_SECTOR_SSIZE,
                DSK_SEC_ORDER_NORMAL, NULL, CPM_FILLER );
            dsk_tools_assign_description ( desc, 2, 2,
                9, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC, NULL, CPM_FILLER );
            dsk_tools_assign_description ( desc, 3, 16,
                10, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_CUSTOM, (uint8_t *) s_lemmings_sector_map, CPM_FILLER );
            dsk_tools_assign_description ( desc, 4, 17,
                9, DSK_SECTOR_SIZE_512,
                DSK_SEC_ORDER_INTERLACED_LEC, NULL, CPM_FILLER );
            break;

        default:
            return EXIT_FAILURE;
    }

    ret = dsk_tools_create_image ( h, desc );
    free ( desc );
    return ret;
}


int mzdsk_tools_format_basic ( st_HANDLER *h, uint8_t tracks, uint8_t sides ) {

    if ( !h || tracks == 0 || ( sides != 1 && sides != 2 ) ) {
        return EXIT_FAILURE;
    }

    /*
     * Krok 1: Vytvoření prázdného DSK obrazu.
     *
     * FSMZ blokové mapování (mzdsk_fsmz_block2trsec) alternuje mezi
     * sousedními absolutními stopami (sudá/lichá), takže funguje korektně
     * jak pro 1-stranný, tak pro 2-stranný DSK.
     *
     * Parametr tracks = počet stop per strana, sides = počet stran.
     * Celkový počet absolutních stop = tracks * sides.
     */
    uint8_t total_tracks = tracks * sides;

    st_DSK_DESCRIPTION *desc = mzdsk_alloc_description ( 1, tracks, sides );
    if ( !desc ) {
        return EXIT_FAILURE;
    }

    dsk_tools_assign_description ( desc, 0, 0,
        MZDSK_FSMZ_SECTORS_ON_TRACK, MZDSK_FSMZ_SECTOR_SSIZE,
        DSK_SEC_ORDER_NORMAL, NULL, FSMZ_FILLER );

    if ( EXIT_SUCCESS != dsk_tools_create_image ( h, desc ) ) {
        free ( desc );
        return EXIT_FAILURE;
    }
    free ( desc );

    /*
     * Krok 2: Inicializace DINFO bloku (alokační blok 15).
     *
     * Blok 0 (IPLPRO) se NEZAPISUJE - na prázdném disku nesmí být
     * platná IPLPRO hlavička (ftype=0x03 + "IPLPRO"), protože by
     * Sharp MZ ROM detekoval přítomnost zavaděče, který neexistuje.
     * Blok 0 zůstává vyplněný fillerem (0xFF), což po FSMZ inverzi
     * dává ftype=0x00 - nevalidní hlavička.
     *
     * Struktura DINFO:
     * - volume_number = 0 (master)
     * - farea = 0x30 (blok kde začíná souborová oblast)
     * - used = 0x0030 (počet obsazených bloků na prázdném disku)
     * - blocks = min(total_blocks, max_addressable) - 1 (oříznuté na kapacitu bitmapy)
     * - map[250] = 0x00 (bitmapa FAREA - vše volné)
     */
    uint8_t sector_buf[MZDSK_FSMZ_SECTOR_SIZE];
    memset ( sector_buf, 0x00, MZDSK_FSMZ_SECTOR_SIZE );

    /* volume_number = 0 (master disk) - offset 0 */
    sector_buf[0] = 0x00;
    /* farea = 0x30 - offset 1 */
    sector_buf[1] = MZDSK_FSMZ_DEFAULT_FAREA_BLOCK;
    /* used = 0x0030 (little-endian) - offset 2-3 */
    sector_buf[2] = MZDSK_FSMZ_DEFAULT_FAREA_BLOCK;        /* low byte */
    sector_buf[3] = 0x00;                                   /* high byte */
    /* blocks = min(total_blocks, max_addressable) - 1 (little-endian) - offset 4-5.
     * Bitmapa má FSMZ_FAREA_BITMAP_SIZE (250) bajtů = 2000 bitů,
     * takže maximální adresovatelný rozsah je farea + 2000 = 2048 bloků. */
    uint16_t total_blocks = (uint16_t) total_tracks * MZDSK_FSMZ_SECTORS_ON_TRACK;
    uint16_t max_addressable = (uint16_t) ( MZDSK_FSMZ_DEFAULT_FAREA_BLOCK + 250 * 8 );
    uint16_t blocks_field = ( total_blocks >= max_addressable )
                            ? ( max_addressable - 1 ) : ( total_blocks - 1 );
    sector_buf[4] = (uint8_t) ( blocks_field & 0xff );      /* low byte */
    sector_buf[5] = (uint8_t) ( ( blocks_field >> 8 ) & 0xff ); /* high byte */
    /* map[250] zůstává 0x00 (vše volné) */

    if ( EXIT_SUCCESS != mzdsk_write_fsmz_block ( h, MZDSK_FSMZ_ALOCBLOCK_DINFO, sector_buf ) ) {
        return EXIT_FAILURE;
    }

    /*
     * Krok 3: Inicializace adresáře (alokační bloky 16-23).
     *
     * První blok adresáře (blok 16) má první 32 bajtů speciální:
     * - bajt 0 = 0x80 (příznak začátku adresáře)
     * - bajt 1 = 0x01
     * - bajty 2-31 = 0x00
     *
     * Zbývající bloky adresáře (17-23) jsou vyplněné nulami.
     */
    for ( uint16_t dir_block = 0; dir_block < MZDSK_FSMZ_DIR_BLOCKS; dir_block++ ) {
        memset ( sector_buf, 0x00, MZDSK_FSMZ_SECTOR_SIZE );

        if ( dir_block == 0 ) {
            /* První položka adresáře - speciální příznak */
            sector_buf[0] = 0x80;
            sector_buf[1] = 0x01;
        }

        if ( EXIT_SUCCESS != mzdsk_write_fsmz_block ( h, MZDSK_FSMZ_ALOCBLOCK_DIR + dir_block, sector_buf ) ) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Společná implementace pro vytvoření CP/M DSK obrazu (SD i HD).
 *
 * Vytvoří DSK obraz se třemi pravidly geometrie:
 * - Stopa 0: datové sektory (9x512 nebo 18x512), filler 0xE5
 * - Stopa 1: boot track (16x256 B), filler 0xFF
 * - Stopy 2+: datové sektory, filler 0xE5
 *
 * @param h Handler pro zápis DSK obrazu. Nesmí být NULL.
 * @param tracks Počet stop.
 * @param sectors Počet sektorů na datových stopách (9 pro SD, 18 pro HD).
 * @param sec_order Typ řazení sektorů (LEC nebo LEC HD).
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @pre h != NULL.
 * @pre tracks >= 3.
 * @post Při úspěchu obsahuje handler kompletní CP/M DSK obraz.
 *
 * @note Tato funkce je interní a není součástí veřejného API.
 *       Odpovídá fstool_dsk_create_cpm() z projektu fstool.
 */
static int mzdsk_create_cpm_internal ( st_HANDLER *h, uint8_t total_tracks, uint8_t sides, uint8_t sectors, en_DSK_SECTOR_ORDER_TYPE sec_order ) {

    if ( !h || total_tracks < 3 || ( sides != 1 && sides != 2 ) ) {
        return EXIT_FAILURE;
    }

    uint8_t tracks_per_side = (uint8_t) ( total_tracks / sides );
    st_DSK_DESCRIPTION *desc = mzdsk_alloc_description ( CPM_RULES_COUNT, tracks_per_side, sides );
    if ( !desc ) {
        return EXIT_FAILURE;
    }

    /* Stopa 0: datové sektory s prokládaným řazením */
    dsk_tools_assign_description ( desc, 0, 0,
        sectors, DSK_SECTOR_SIZE_512,
        sec_order, NULL, CPM_FILLER );

    /* Stopa 1: boot track - FSMZ geometrie */
    dsk_tools_assign_description ( desc, 1, 1,
        MZDSK_FSMZ_SECTORS_ON_TRACK, MZDSK_FSMZ_SECTOR_SSIZE,
        DSK_SEC_ORDER_NORMAL, NULL, FSMZ_FILLER );

    /* Stopy 2+: datové sektory s prokládaným řazením */
    dsk_tools_assign_description ( desc, 2, 2,
        sectors, DSK_SECTOR_SIZE_512,
        sec_order, NULL, CPM_FILLER );

    int ret = dsk_tools_create_image ( h, desc );
    free ( desc );
    return ret;
}


int mzdsk_tools_format_cpm_sd ( st_HANDLER *h, uint8_t tracks, uint8_t sides ) {
    return mzdsk_create_cpm_internal ( h, tracks, sides, 9, DSK_SEC_ORDER_INTERLACED_LEC );
}


int mzdsk_tools_format_cpm_hd ( st_HANDLER *h, uint8_t tracks, uint8_t sides ) {
    return mzdsk_create_cpm_internal ( h, tracks, sides, 18, DSK_SEC_ORDER_INTERLACED_LEC_HD );
}


/**
 * @brief Vrátí řetězec s verzí knihovny mzdsk_tools.
 * @return Statický řetězec s verzí (např. "1.0.0").
 */
const char* mzdsk_tools_version ( void ) {
    return MZDSK_TOOLS_VERSION;
}
