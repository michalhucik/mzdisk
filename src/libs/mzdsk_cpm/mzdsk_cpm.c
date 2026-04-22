/**
 * @file   mzdsk_cpm.c
 * @brief  Implementace CP/M souborového systému pro disky Sharp MZ.
 *
 * Implementuje operace čtení/zápisu/mazání/přejmenování souborů,
 * správu atributů, čtení adresáře a alokační mapy na CP/M discích
 * ve formátu Sharp MZ. Podporuje formáty SD (9x512B), HD (18x512B)
 * a custom formáty s libovolným DPB.
 *
 * Geometrie DSK obrazu pro CP/M disky Sharp MZ:
 * - Absolutní stopa 0: datová stopa (9x512B pro SD, 18x512B pro HD)
 * - Absolutní stopa 1: MZ boot track (16x256B - formát FSMZ)
 * - Absolutní stopy 2+: datové stopy (9x512B pro SD, 18x512B pro HD)
 *
 * CP/M vidí disk takto (s OFF=2):
 * - CP/M stopa 0 = abs stopa 0 (rezervovaná - systémová)
 * - CP/M stopa 1 = abs stopa 1 (rezervovaná - boot track)
 * - CP/M stopa 2 = abs stopa 2 (první datová stopa, blok 0)
 * - atd.
 *
 * Pozor: abs stopa 1 je boot track s odlišnou geometrií (16x256B),
 * CP/M na ni nepřistupuje (je v rámci rezervovaných stop).
 *
 * I/O operace používají dsk_read_sector()/dsk_write_sector() přímo
 * (bez inverze), protože CP/M datové stopy mají normální data.
 *
 * Diskový kontext (st_MZDSK_DISC) je sdílený z mzdsk_global.h.
 * Návratové kódy (en_MZDSK_RES) jsou sdílené z mzdsk_global.h.
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mzdsk_cpm.h"


/* ================================================================
 * Interní konstanty
 * ================================================================ */

/** @brief Maximální počet alokačních bloků pro interní bitmapy. */
#define CPM_MAX_BLOCKS          4096

/** @brief Maximální počet adresářových položek pro interní buffery. */
#define CPM_MAX_DIR_ENTRIES     256

/** @brief Velikost bufferu pro čtení jednoho fyzického sektoru. */
#define CPM_SECTOR_BUFFER_SIZE  512

/** @brief Maximální podporovaná velikost alokačního bloku (interní buffery).
 *  CP/M 2.2 povoluje BSH 3-7, tj. block_size 1024-16384 B (128 << BSH). */
#define CPM_MAX_BLOCK_SIZE      16384


/* ================================================================
 * Interní pomocné funkce
 * ================================================================ */


/**
 * @brief Převede číslo CP/M alokačního bloku na fyzickou stopu, sektor a offset.
 *
 * CP/M bloky začínají od absolutní stopy dpb->off (tj. za rezervovanými
 * stopami). Každý blok obsahuje dpb->block_size bajtů. Fyzické sektory
 * mají velikost 512B a jsou číslovány od 1.
 *
 * Výpočet:
 * 1. Celkový bajtový offset bloku v datové oblasti = block * block_size
 * 2. Lineární logický sektor (128B) od začátku datové oblasti
 * 3. Převod na stopu a pozici v rámci stopy
 * 4. Přičtení offset rezervovaných stop (dpb->off)
 * 5. Výpočet fyzického sektoru (512B, číslovány od 1) a offsetu v sektoru
 *
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  block Číslo CP/M alokačního bloku.
 * @param[out] track Výstup: absolutní číslo stopy v DSK obrazu.
 * @param[out] sector Výstup: ID fyzického sektoru (1-based).
 * @param[out] offset Výstup: bajtový offset v rámci fyzického sektoru.
 *
 * @pre dpb != NULL && track != NULL && sector != NULL && offset != NULL
 * @pre dpb->spt > 0
 * @post *track je absolutní stopa v DSK obrazu
 * @post *sector je číslo fyzického sektoru (1 .. sectors_per_track)
 * @post *offset je offset v rámci sektoru (0 .. 511)
 */
void mzdsk_cpm_block_to_physical ( const st_MZDSK_CPM_DPB *dpb, uint16_t block,
                                    uint8_t *track, uint8_t *sector, uint16_t *offset ) {

    /* Celkový bajtový offset bloku od začátku datové oblasti CP/M */
    uint32_t byte_offset = (uint32_t) block * dpb->block_size;

    /* Logický sektor (128B) od začátku datové oblasti */
    uint32_t logical_sector = byte_offset / MZDSK_CPM_RECORD_SIZE;

    /* CP/M stopa (relativně k datové oblasti, tj. za OFF) */
    uint16_t cpm_track = (uint16_t) ( logical_sector / dpb->spt );

    /* Logický sektor v rámci stopy */
    uint16_t sector_in_track = (uint16_t) ( logical_sector % dpb->spt );

    /* Absolutní stopa v DSK obrazu: přičteme rezervované stopy */
    *track = (uint8_t) ( cpm_track + dpb->off );

    /* Fyzický sektor: 4 logické sektory (128B) = 1 fyzický sektor (512B) */
    /* Fyzické sektory jsou číslovány od 1 */
    *sector = (uint8_t) ( ( sector_in_track / 4 ) + 1 );

    /* Offset v rámci fyzického sektoru */
    *offset = (uint16_t) ( ( sector_in_track % 4 ) * MZDSK_CPM_RECORD_SIZE );
}


/**
 * @brief Přečte data z CP/M alokačního bloku.
 *
 * Postupně čte fyzické sektory pokrývající zadaný blok a kopíruje
 * data do výstupního bufferu. Pokud požadovaná velikost přesahuje
 * velikost bloku, čte se maximálně block_size bajtů.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  block Číslo CP/M alokačního bloku.
 * @param[out] buffer Výstupní buffer pro data bloku.
 * @param[in]  size Počet bajtů k přečtení (max dpb->block_size).
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DSK_ERROR při chybě čtení.
 *
 * @pre disc != NULL && dpb != NULL && buffer != NULL
 * @pre size <= dpb->block_size
 */
en_MZDSK_RES mzdsk_cpm_read_block ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                     uint16_t block, uint8_t *buffer, uint16_t size ) {

    uint8_t track;
    uint8_t sector;
    uint16_t offset;

    mzdsk_cpm_block_to_physical ( dpb, block, &track, &sector, &offset );

    uint16_t bytes_read = 0;
    uint8_t sector_buf[CPM_SECTOR_BUFFER_SIZE];

    while ( bytes_read < size ) {
        /* Přečteme fyzický sektor */
        if ( EXIT_SUCCESS != dsk_read_sector ( disc->handler, track, sector, sector_buf ) ) {
            return MZDSK_RES_DSK_ERROR;
        }

        /* Kolik bajtů zkopírovat z tohoto sektoru */
        uint16_t avail = CPM_SECTOR_BUFFER_SIZE - offset;
        uint16_t to_copy = size - bytes_read;
        if ( to_copy > avail ) to_copy = avail;

        memcpy ( buffer + bytes_read, sector_buf + offset, to_copy );
        bytes_read += to_copy;

        /* Přejdeme na další sektor */
        offset = 0;
        sector++;

        /* Počet fyzických sektorů na stopu */
        uint8_t phys_sectors_per_track = (uint8_t) ( dpb->spt / 4 );

        /* Pokud jsme přetekli přes stopu, přejdeme na další */
        if ( sector > phys_sectors_per_track ) {
            sector = 1;
            track++;
        }
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše data do CP/M alokačního bloku.
 *
 * Postupně čte fyzické sektory, modifikuje příslušné části a zapisuje
 * je zpět. Tím zajistí, že se nepřepíšou data v jiných částech sektoru.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  block Číslo CP/M alokačního bloku.
 * @param[in]  data Zdrojová data k zápisu.
 * @param[in]  size Počet bajtů k zápisu (max dpb->block_size).
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DSK_ERROR při chybě.
 *
 * @pre disc != NULL && dpb != NULL && data != NULL
 * @pre size <= dpb->block_size
 *
 * @par Vedlejší efekty:
 * - Modifikuje fyzické sektory na disku.
 */
en_MZDSK_RES mzdsk_cpm_write_block ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                      uint16_t block, const uint8_t *data, uint16_t size ) {

    uint8_t track;
    uint8_t sector;
    uint16_t offset;

    mzdsk_cpm_block_to_physical ( dpb, block, &track, &sector, &offset );

    uint16_t bytes_written = 0;
    uint8_t sector_buf[CPM_SECTOR_BUFFER_SIZE];

    while ( bytes_written < size ) {
        /* Přečteme původní obsah sektoru */
        if ( EXIT_SUCCESS != dsk_read_sector ( disc->handler, track, sector, sector_buf ) ) {
            return MZDSK_RES_DSK_ERROR;
        }

        /* Kolik bajtů zapíšeme do tohoto sektoru */
        uint16_t avail = CPM_SECTOR_BUFFER_SIZE - offset;
        uint16_t to_write = size - bytes_written;
        if ( to_write > avail ) to_write = avail;

        memcpy ( sector_buf + offset, data + bytes_written, to_write );

        /* Zapíšeme modifikovaný sektor zpět */
        if ( EXIT_SUCCESS != dsk_write_sector ( disc->handler, track, sector, sector_buf ) ) {
            return MZDSK_RES_DSK_ERROR;
        }

        bytes_written += to_write;

        /* Přejdeme na další sektor */
        offset = 0;
        sector++;

        uint8_t phys_sectors_per_track = (uint8_t) ( dpb->spt / 4 );

        if ( sector > phys_sectors_per_track ) {
            sector = 1;
            track++;
        }
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Interní funkce pro čtení surových adresářových položek.
 *
 * Načte adresářové bloky určené bitovou mapou AL0/AL1 v DPB a
 * sestaví pole adresářových položek.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[out] entries Výstupní pole adresářových položek.
 * @param[in]  max_entries Maximální počet položek ve výstupním poli.
 * @return Počet přečtených položek (>= 0), nebo -1 při chybě I/O.
 *
 * @pre disc != NULL && dpb != NULL && entries != NULL
 * @pre max_entries > 0
 */
static int cpm_read_raw_directory ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                    st_MZDSK_CPM_DIRENTRY *entries, int max_entries ) {

    int total_entries = 0;
    uint16_t dir_alloc = ( (uint16_t) dpb->al0 << 8 ) | dpb->al1;

    /* Heap místo stacku: 16 kB na stacku × hluboký call chain
     * (volá se z cpm_write_dir_entry, mzdsk_cpm_format_directory,
     * mzdsk_cpm_write_file) → na Windows 1 MB stacku risk přetečení.
     * Audit H-9. */
    uint8_t *block_data = malloc ( CPM_MAX_BLOCK_SIZE );
    if ( block_data == NULL ) return -1;

    /* Procházíme bity AL0/AL1 (16 bitů, MSB first) */
    int bit;
    for ( bit = 15; bit >= 0; bit-- ) {
        if ( !( dir_alloc & ( 1u << bit ) ) ) continue;

        uint16_t block_num = (uint16_t) ( 15 - bit );

        uint16_t read_size = dpb->block_size;

        if ( mzdsk_cpm_read_block ( disc, dpb, block_num, block_data, read_size ) != MZDSK_RES_OK ) {
            free ( block_data );
            return -1;
        }

        /* Zparsujeme 32B adresářové položky z bloku */
        uint16_t entries_in_block = read_size / MZDSK_CPM_DIRENTRY_SIZE;
        uint16_t i;
        for ( i = 0; i < entries_in_block && total_entries < max_entries; i++ ) {
            memcpy ( &entries[total_entries], &block_data[i * MZDSK_CPM_DIRENTRY_SIZE],
                     MZDSK_CPM_DIRENTRY_SIZE );
            total_entries++;
        }
    }

    free ( block_data );
    return total_entries;
}


/**
 * @brief Interní funkce pro zápis adresářové položky na disk.
 *
 * Vypočítá, ve kterém adresářovém bloku a na jakém offsetu v rámci
 * bloku se položka s daným indexem nachází, a zapíše ji.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  entry_index Index adresářové položky (0-based).
 * @param[in]  entry Adresářová položka k zápisu.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc != NULL && dpb != NULL && entry != NULL
 * @pre entry_index <= dpb->drm
 *
 * @par Vedlejší efekty:
 * - Modifikuje adresářový blok na disku.
 */
static en_MZDSK_RES cpm_write_dir_entry ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                          int entry_index, const st_MZDSK_CPM_DIRENTRY *entry ) {

    uint16_t entries_per_block = dpb->block_size / MZDSK_CPM_DIRENTRY_SIZE;

    /* Zjistíme, ve kterém adresářovém bloku je tato položka */
    int dir_block_index = entry_index / entries_per_block;
    int entry_in_block = entry_index % entries_per_block;

    /* Najdeme číslo bloku z AL0/AL1 */
    uint16_t dir_alloc = ( (uint16_t) dpb->al0 << 8 ) | dpb->al1;
    int block_count = 0;
    uint16_t block_num = 0;

    int bit;
    for ( bit = 15; bit >= 0; bit-- ) {
        if ( !( dir_alloc & ( 1u << bit ) ) ) continue;

        if ( block_count == dir_block_index ) {
            block_num = (uint16_t) ( 15 - bit );
            break;
        }
        block_count++;
    }

    /* Heap místo stacku - 16 kB na stacku. Audit H-9. */
    uint8_t *block_data = malloc ( CPM_MAX_BLOCK_SIZE );
    if ( block_data == NULL ) return MZDSK_RES_UNKNOWN_ERROR;
    uint16_t block_size = dpb->block_size;

    if ( mzdsk_cpm_read_block ( disc, dpb, block_num, block_data, block_size ) != MZDSK_RES_OK ) {
        free ( block_data );
        return MZDSK_RES_DSK_ERROR;
    }

    /* Aktualizujeme položku */
    memcpy ( &block_data[entry_in_block * MZDSK_CPM_DIRENTRY_SIZE], entry, MZDSK_CPM_DIRENTRY_SIZE );

    /* Zapíšeme blok zpět */
    en_MZDSK_RES write_res = mzdsk_cpm_write_block ( disc, dpb, block_num, block_data, block_size );
    free ( block_data );
    if ( write_res != MZDSK_RES_OK ) {
        return MZDSK_RES_DSK_ERROR;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Připraví jméno souboru pro porovnání s adresářovou položkou.
 *
 * Konvertuje vstupní jméno na velká písmena a doplní mezerami na field_len znaků.
 *
 * @param[in]  src Vstupní jméno souboru (null-terminated).
 * @param[out] dst Výstupní buffer (min field_len bajtů).
 * @param[in]  field_len Délka pole (8 pro jméno, 3 pro příponu).
 *
 * @pre src != NULL && dst != NULL
 * @pre field_len == 8 || field_len == 3
 * @post dst obsahuje jméno doplněné mezerami na field_len znaků
 */
static void cpm_prepare_name ( const char *src, uint8_t *dst, int field_len ) {

    int i = 0;

    while ( i < field_len && src[i] != '\0' ) {
        dst[i] = (uint8_t) toupper ( (unsigned char) src[i] );
        i++;
    }

    /* Doplnit mezerami */
    while ( i < field_len ) {
        dst[i] = 0x20;
        i++;
    }
}


/**
 * @brief Porovná adresářovou položku se zadaným jménem, příponou a uživatelem.
 *
 * Porovnání je case-insensitive (obě strany se porovnávají přes
 * připravené jméno v horním registru s výplní mezerami).
 * Bity 7 jména i přípony se při porovnání maskují (bit 7 může
 * obsahovat atributy nebo být nastaven z jiných důvodů).
 *
 * @param[in]  entry Adresářová položka.
 * @param[in]  cpm_fname Připravené jméno (8 znaků, horní registr, výplň mezerami).
 * @param[in]  cpm_ext Připravená přípona (3 znaky, horní registr, výplň mezerami).
 * @param[in]  user Číslo uživatele.
 * @return 1 pokud položka odpovídá, 0 pokud ne.
 *
 * @pre entry != NULL && cpm_fname != NULL && cpm_ext != NULL
 */
static int cpm_match_entry ( const st_MZDSK_CPM_DIRENTRY *entry,
                             const uint8_t *cpm_fname, const uint8_t *cpm_ext, uint8_t user ) {

    if ( entry->user != user ) return 0;

    /* Jméno - maskujeme bit 7 u každého znaku */
    int i;
    for ( i = 0; i < 8; i++ ) {
        if ( ( entry->fname[i] & 0x7F ) != cpm_fname[i] ) return 0;
    }

    /* Přípona - horní bit může obsahovat atributy, maskujeme ho */
    for ( i = 0; i < 3; i++ ) {
        if ( ( entry->ext[i] & 0x7F ) != cpm_ext[i] ) return 0;
    }

    return 1;
}


/**
 * @brief Sestaví bitmapu obsazených alokačních bloků z adresáře.
 *
 * Projde všechny platné (nesmazané) adresářové položky a pro každý
 * nenulový blok v alokační mapě nastaví odpovídající bit v bitmapě.
 *
 * @param[in]  entries Pole adresářových položek.
 * @param[in]  num_entries Počet položek v poli.
 * @param[out] alloc_map Bitmapa obsazených bloků (pole bajtů, min (dsm/8)+1).
 * @param[in]  dsm Maximální číslo bloku (DSM).
 *
 * @pre entries != NULL && alloc_map != NULL
 * @post alloc_map[block/8] & (1 << (block%8)) je nenulové pro obsazené bloky
 */
static void cpm_build_alloc_map ( const st_MZDSK_CPM_DIRENTRY *entries, int num_entries,
                                  uint8_t *alloc_map, uint16_t dsm ) {

    memset ( alloc_map, 0, ( dsm / 8 ) + 1 );

    int i;
    for ( i = 0; i < num_entries; i++ ) {
        const st_MZDSK_CPM_DIRENTRY *e = &entries[i];

        /* Přeskočit smazané a systémové položky */
        if ( e->user == MZDSK_CPM_DELETED_ENTRY ) continue;
        if ( e->user > 15 ) continue;

        /* Pro bloky menší než 256 - 8-bitová čísla bloků */
        if ( dsm <= 255 ) {
            int j;
            for ( j = 0; j < 16; j++ ) {
                uint8_t block = e->alloc[j];
                if ( block == 0 ) continue;
                if ( block <= dsm ) {
                    alloc_map[block / 8] |= (uint8_t) ( 1u << ( block % 8 ) );
                }
            }
        } else {
            /* 16-bitová čísla bloků (2 bajty na blok, little-endian) */
            int j;
            for ( j = 0; j < 16; j += 2 ) {
                uint16_t block = (uint16_t) e->alloc[j] | ( (uint16_t) e->alloc[j + 1] << 8 );
                if ( block == 0 ) continue;
                if ( block <= dsm ) {
                    alloc_map[block / 8] |= (uint8_t) ( 1u << ( block % 8 ) );
                }
            }
        }
    }
}


/**
 * @brief Najde první volný alokační blok.
 *
 * Prohledá bitmapu obsazených bloků a vrátí číslo prvního volného bloku.
 *
 * @param[in]  alloc_map Bitmapa obsazených bloků.
 * @param[in]  dsm Maximální číslo bloku (DSM).
 * @return Číslo volného bloku, nebo 0xFFFF pokud žádný není k dispozici.
 *
 * @pre alloc_map != NULL
 */
static uint16_t cpm_find_free_block ( const uint8_t *alloc_map, uint16_t dsm ) {

    uint16_t block;

    for ( block = 0; block <= dsm; block++ ) {
        if ( !( alloc_map[block / 8] & ( 1u << ( block % 8 ) ) ) ) {
            return block;
        }
    }

    return 0xFFFF;
}


/**
 * @brief Extrahuje jméno souboru z adresářové položky do null-terminated řetězce.
 *
 * Zkopíruje znaky jména, odstraní koncové mezery a přidá '\0'.
 * Bity 7 jména i přípony se maskují (bit 7 může obsahovat
 * atributy nebo být nastaven z jiných důvodů).
 *
 * @param[in]  entry Adresářová položka.
 * @param[out] filename Výstupní buffer pro jméno (min 9 bajtů).
 * @param[out] extension Výstupní buffer pro příponu (min 4 bajty).
 *
 * @pre entry != NULL && filename != NULL && extension != NULL
 * @post filename a extension jsou null-terminated řetězce bez koncových mezer
 */
static void cpm_extract_filename ( const st_MZDSK_CPM_DIRENTRY *entry,
                                   char *filename, char *extension ) {

    /* Jméno - maskujeme bit 7 u každého znaku */
    int i;
    for ( i = 0; i < 8; i++ ) {
        filename[i] = (char) ( entry->fname[i] & 0x7F );
    }
    filename[8] = '\0';

    /* Odříznout koncové mezery */
    i = 7;
    while ( i >= 0 && filename[i] == ' ' ) {
        filename[i] = '\0';
        i--;
    }

    /* Přípona - horní bit může obsahovat atributy */
    for ( i = 0; i < 3; i++ ) {
        extension[i] = (char) ( entry->ext[i] & 0x7F );
    }
    extension[3] = '\0';

    /* Odříznout koncové mezery */
    i = 2;
    while ( i >= 0 && extension[i] == ' ' ) {
        extension[i] = '\0';
        i--;
    }
}


/**
 * @brief Extrahuje atributy z adresářové položky.
 *
 * Čte bity 7 z příponových bajtů a mapuje je na en_MZDSK_CPM_ATTR.
 *
 * @param[in]  entry Adresářová položka.
 * @return Kombinace en_MZDSK_CPM_ATTR (bitový OR).
 *
 * @pre entry != NULL
 */
static uint8_t cpm_extract_attributes ( const st_MZDSK_CPM_DIRENTRY *entry ) {

    uint8_t attrs = 0;

    if ( entry->ext[0] & 0x80 ) attrs |= MZDSK_CPM_ATTR_READ_ONLY;
    if ( entry->ext[1] & 0x80 ) attrs |= MZDSK_CPM_ATTR_SYSTEM;
    if ( entry->ext[2] & 0x80 ) attrs |= MZDSK_CPM_ATTR_ARCHIVED;

    return attrs;
}


/**
 * @brief Nastaví atributy do adresářové položky.
 *
 * Nastaví nebo vynuluje bity 7 příponových bajtů podle zadaných atributů.
 * Dolní bity (vlastní přípona) se zachovají.
 *
 * @param[in,out] entry Adresářová položka.
 * @param[in]     attributes Kombinace en_MZDSK_CPM_ATTR (bitový OR).
 *
 * @pre entry != NULL
 * @post Bity 7 v entry->ext[0..2] odpovídají zadaným atributům.
 */
static void cpm_apply_attributes ( st_MZDSK_CPM_DIRENTRY *entry, uint8_t attributes ) {

    /* Vyčistíme bity 7 */
    entry->ext[0] &= 0x7F;
    entry->ext[1] &= 0x7F;
    entry->ext[2] &= 0x7F;

    /* Nastavíme požadované atributy */
    if ( attributes & MZDSK_CPM_ATTR_READ_ONLY ) entry->ext[0] |= 0x80;
    if ( attributes & MZDSK_CPM_ATTR_SYSTEM )    entry->ext[1] |= 0x80;
    if ( attributes & MZDSK_CPM_ATTR_ARCHIVED )  entry->ext[2] |= 0x80;
}


/**
 * @brief Označí adresářové bloky v alokační bitmapě jako obsazené.
 *
 * Projde bity AL0/AL1 v DPB a odpovídající bloky označí v bitmapě.
 *
 * @param[in]     dpb Disk Parameter Block.
 * @param[in,out] alloc_map Bitmapa obsazených bloků.
 *
 * @pre dpb != NULL && alloc_map != NULL
 * @post Adresářové bloky jsou označené jako obsazené v alloc_map.
 */
static void cpm_mark_dir_blocks ( const st_MZDSK_CPM_DPB *dpb, uint8_t *alloc_map ) {

    uint16_t dir_alloc = ( (uint16_t) dpb->al0 << 8 ) | dpb->al1;
    int bit;

    for ( bit = 15; bit >= 0; bit-- ) {
        if ( dir_alloc & ( 1u << bit ) ) {
            uint16_t block_num = (uint16_t) ( 15 - bit );
            alloc_map[block_num / 8] |= (uint8_t) ( 1u << ( block_num % 8 ) );
        }
    }
}


/* ================================================================
 * Implementace veřejného API - inicializace a konfigurace
 * ================================================================ */


/**
 * @brief Inicializuje DPB pro zadaný CP/M formát (preset).
 *
 * Vyplní strukturu DPB hodnotami odpovídajícími danému formátu.
 * Pro SD (Lamač): block_size=2048, 128 dir entries, 351 bloků, OFF=4.
 * Pro HD (Lucky-Soft): block_size=4096, 128 dir entries, 351 bloků, OFF=4.
 *
 * @param[out] dpb Ukazatel na strukturu DPB k inicializaci.
 * @param[in]  format Typ CP/M formátu (SD nebo HD).
 */
void mzdsk_cpm_init_dpb ( st_MZDSK_CPM_DPB *dpb, en_MZDSK_CPM_FORMAT format ) {

    if ( dpb == NULL ) return;

    memset ( dpb, 0, sizeof ( st_MZDSK_CPM_DPB ) );

    switch ( format ) {

        case MZDSK_CPM_FORMAT_SD:
            dpb->spt = 36;          /* 9 * 512 / 128 = 36 logických sektorů */
            dpb->bsh = 4;           /* log2(2048/128) = 4 */
            dpb->blm = 15;          /* (2048/128) - 1 = 15 */
            dpb->exm = 0;
            dpb->dsm = 350;         /* celkem 351 bloků (0..350) */
            dpb->drm = 127;         /* 128 adresářových položek (0..127) */
            dpb->al0 = 0xC0;        /* 2 bloky pro adresář (2 * 2048 = 4096B) */
            dpb->al1 = 0x00;
            dpb->cks = 32;          /* check vector = (drm+1) / 4 */
            dpb->off = 4;           /* 4 rezervované stopy (boot + systém) */
            dpb->block_size = 2048; /* velikost bloku */
            break;

        case MZDSK_CPM_FORMAT_HD:
            dpb->spt = 72;          /* 18 * 512 / 128 = 72 logických sektorů */
            dpb->bsh = 5;           /* log2(4096/128) = 5 (Lucky-Soft HD: 4 KB bloky) */
            dpb->blm = 31;          /* (4096/128) - 1 = 31 */
            dpb->exm = 1;           /* 8 ptr * 4096 = 32768 = 2 logické extenty */
            dpb->dsm = 350;         /* (160-4)*72/32 = 351 bloků (0..350) */
            dpb->drm = 127;         /* 128 adresářových položek (0..127) */
            dpb->al0 = 0xC0;        /* 2 bloky pro adresář (2 * 4096 = 8192 B) */
            dpb->al1 = 0x00;
            dpb->cks = 32;          /* check vector = (drm+1) / 4 */
            dpb->off = 4;           /* 4 rezervované stopy (boot + systém) */
            dpb->block_size = 4096; /* velikost bloku (Lucky-Soft HD) */
            break;
    }
}


/**
 * @brief Inicializuje DPB s libovolnými (custom) parametry.
 *
 * Odvozené hodnoty (blm, block_size, cks) se vypočtou automaticky.
 *
 * @param[out] dpb Ukazatel na strukturu DPB.
 * @param[in]  spt Počet logických sektorů na stopu.
 * @param[in]  bsh Block shift factor (3-7).
 * @param[in]  exm Extent mask.
 * @param[in]  dsm Celkový počet bloků - 1.
 * @param[in]  drm Počet adresářových položek - 1.
 * @param[in]  al0 Alokační bitmapa adresáře - byte 0.
 * @param[in]  al1 Alokační bitmapa adresáře - byte 1.
 * @param[in]  off Počet rezervovaných stop.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_init_dpb_custom ( st_MZDSK_CPM_DPB *dpb, uint16_t spt,
                                           uint8_t bsh, uint8_t exm, uint16_t dsm,
                                           uint16_t drm, uint8_t al0, uint8_t al1,
                                           uint16_t off ) {

    if ( dpb == NULL ) return MZDSK_RES_INVALID_PARAM;

    if ( bsh < 3 || bsh > 7 ) return MZDSK_RES_FORMAT_ERROR;

    memset ( dpb, 0, sizeof ( st_MZDSK_CPM_DPB ) );

    dpb->spt = spt;
    dpb->bsh = bsh;
    dpb->blm = (uint8_t) ( ( 1u << bsh ) - 1 );
    dpb->exm = exm;
    dpb->dsm = dsm;
    dpb->drm = drm;
    dpb->al0 = al0;
    dpb->al1 = al1;
    dpb->cks = ( drm + 1 ) / 4;
    dpb->off = off;
    dpb->block_size = (uint16_t) ( 128u << bsh );

    return MZDSK_RES_OK;
}


/**
 * @brief Ověří konzistenci parametrů DPB.
 *
 * Kontroluje interní konzistenci DPB: spt > 0, bsh v rozsahu,
 * block_size a blm odpovídají bsh, dsm > 0, block_size nepřekračuje
 * interní limit.
 *
 * @param[in]  dpb Ukazatel na strukturu DPB.
 * @return MZDSK_RES_OK pokud je DPB konzistentní, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_validate_dpb ( const st_MZDSK_CPM_DPB *dpb ) {

    if ( dpb == NULL ) return MZDSK_RES_INVALID_PARAM;

    /* spt musí být nenulové */
    if ( dpb->spt == 0 ) return MZDSK_RES_FORMAT_ERROR;

    /* bsh musí být v rozsahu 3-7 (block_size 1024-16384 B) */
    if ( dpb->bsh < 3 || dpb->bsh > 7 ) return MZDSK_RES_FORMAT_ERROR;

    /* block_size musí odpovídat bsh */
    if ( dpb->block_size != (uint16_t) ( 128u << dpb->bsh ) ) return MZDSK_RES_FORMAT_ERROR;

    /* blm musí odpovídat bsh */
    if ( dpb->blm != (uint8_t) ( ( 1u << dpb->bsh ) - 1 ) ) return MZDSK_RES_FORMAT_ERROR;

    /* dsm musí být alespoň 0 (minimálně 1 blok) - dsm je unsigned, vždy >= 0 */

    /* dsm musí vejít do interní alloc_map[CPM_MAX_BLOCKS/8 + 1].
     * Audit M-3: custom DPB s dsm >= CPM_MAX_BLOCKS by v
     * cpm_build_alloc_map zapsal bit mimo pole. */
    if ( dpb->dsm >= CPM_MAX_BLOCKS ) return MZDSK_RES_FORMAT_ERROR;

    /* block_size nesmí překročit interní limit bufferů */
    if ( dpb->block_size > CPM_MAX_BLOCK_SIZE ) return MZDSK_RES_FORMAT_ERROR;

    return MZDSK_RES_OK;
}


/**
 * @brief Vrátí textový popis chybového kódu.
 *
 * Obaluje mzdsk_get_error() z mzdsk_global. Vrací anglický popis
 * chybového kódu en_MZDSK_RES.
 *
 * @param[in]  res Návratový kód operace.
 * @return Ukazatel na statický řetězec s popisem. Nikdy nevrací NULL.
 */
const char* mzdsk_cpm_strerror ( en_MZDSK_RES res ) {
    return mzdsk_get_error ( res );
}


/* ================================================================
 * Implementace veřejného API - adresářové operace
 * ================================================================ */


/**
 * @brief Přečte surové 32B adresářové položky z disku.
 *
 * Veřejná obálka interní funkce cpm_read_raw_directory(). Vrátí
 * surové adresářové položky včetně smazaných (user == 0xE5).
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[out] entries Výstupní pole pro adresářové položky.
 * @param[in]  max_entries Maximální počet položek.
 * @return Počet přečtených položek (>= 0), nebo -1 při chybě.
 */
int mzdsk_cpm_read_raw_directory ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                    st_MZDSK_CPM_DIRENTRY *entries, int max_entries ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         entries == NULL || max_entries <= 0 ) {
        return -1;
    }

    return cpm_read_raw_directory ( disc, dpb, entries, max_entries );
}


/**
 * @brief Přečte adresář CP/M disku a vrátí seznam platných souborů.
 *
 * Načte adresářové bloky, zparsuje 32B položky, sloučí extenty
 * patřící ke stejnému souboru (stejné jméno + přípona + user)
 * a spočítá celkovou velikost každého souboru.
 *
 * Velikost souboru se počítá jako součet záznamů (rc) ze všech
 * extentů souboru krát 128 bajtů.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[out] files Výstupní pole pro informace o souborech.
 * @param[in]  max_files Maximální počet souborů ve výstupním poli.
 * @return Počet nalezených souborů (>= 0), nebo -1 při chybě.
 */
int mzdsk_cpm_read_directory ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                st_MZDSK_CPM_FILE_INFO *files, int max_files ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL || files == NULL || max_files <= 0 ) {
        return -1;
    }

    /* Přečteme surové adresářové položky */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return -1;

    int file_count = 0;

    int i;
    for ( i = 0; i < num_entries; i++ ) {
        const st_MZDSK_CPM_DIRENTRY *e = &entries[i];

        /* Přeskočit smazané a systémové položky */
        if ( e->user == MZDSK_CPM_DELETED_ENTRY ) continue;
        if ( e->user > 15 ) continue;

        /* Extrahovat jméno a příponu */
        char fname[9];
        char ext[4];
        cpm_extract_filename ( e, fname, ext );

        /* Zjistit, jestli tento soubor už máme v seznamu */
        int found = -1;
        int j;
        for ( j = 0; j < file_count; j++ ) {
            if ( files[j].user == e->user &&
                 strcmp ( files[j].filename, fname ) == 0 &&
                 strcmp ( files[j].extension, ext ) == 0 ) {
                found = j;
                break;
            }
        }

        /* Spočítat velikost tohoto extentu */
        uint32_t extent_records = (uint32_t) e->rc;

        if ( found >= 0 ) {
            /* Přičíst záznamy k existujícímu souboru */
            uint16_t extent_num = (uint16_t) e->s2 * 32 + e->extent;
            uint32_t records_before = (uint32_t) extent_num * MZDSK_CPM_RECORDS_PER_EXTENT;
            uint32_t total_records = records_before + extent_records;
            uint32_t new_size = total_records * MZDSK_CPM_RECORD_SIZE;
            if ( new_size > files[found].size ) {
                files[found].size = new_size;
            }
        } else {
            /* Nový soubor */
            if ( file_count >= max_files ) continue;

            files[file_count].user = e->user;
            strncpy ( files[file_count].filename, fname, sizeof ( files[file_count].filename ) - 1 );
            files[file_count].filename[sizeof ( files[file_count].filename ) - 1] = '\0';
            strncpy ( files[file_count].extension, ext, sizeof ( files[file_count].extension ) - 1 );
            files[file_count].extension[sizeof ( files[file_count].extension ) - 1] = '\0';

            /* Velikost = (extenty před tímto * 128 + rc) * 128 */
            uint16_t extent_num = (uint16_t) e->s2 * 32 + e->extent;
            uint32_t records_before = (uint32_t) extent_num * MZDSK_CPM_RECORDS_PER_EXTENT;
            files[file_count].size = ( records_before + extent_records ) * MZDSK_CPM_RECORD_SIZE;

            file_count++;
        }
    }

    return file_count;
}


/**
 * @brief Rozšířený výpis adresáře s atributy a počtem extentů.
 *
 * Funguje jako mzdsk_cpm_read_directory(), ale vrací rozšířené
 * informace: atributy (R/O, SYS, ARC), počet extentů a index
 * první adresářové položky.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[out] files Výstupní pole pro rozšířené informace.
 * @param[in]  max_files Maximální počet souborů.
 * @return Počet nalezených souborů (>= 0), nebo -1 při chybě.
 */
int mzdsk_cpm_read_directory_ex ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                   st_MZDSK_CPM_FILE_INFO_EX *files, int max_files ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL || files == NULL || max_files <= 0 ) {
        return -1;
    }

    /* Přečteme surové adresářové položky */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return -1;

    int file_count = 0;

    int i;
    for ( i = 0; i < num_entries; i++ ) {
        const st_MZDSK_CPM_DIRENTRY *e = &entries[i];

        /* Přeskočit smazané a neplatné položky */
        if ( e->user == MZDSK_CPM_DELETED_ENTRY ) continue;
        if ( e->user > 15 ) continue;

        /* Extrahovat jméno a příponu */
        char fname[9];
        char ext[4];
        cpm_extract_filename ( e, fname, ext );

        /* Zjistit, jestli tento soubor už máme v seznamu */
        int found = -1;
        int j;
        for ( j = 0; j < file_count; j++ ) {
            if ( files[j].user == e->user &&
                 strcmp ( files[j].filename, fname ) == 0 &&
                 strcmp ( files[j].extension, ext ) == 0 ) {
                found = j;
                break;
            }
        }

        /* Spočítat velikost tohoto extentu */
        uint32_t extent_records = (uint32_t) e->rc;

        if ( found >= 0 ) {
            /* Přičíst záznamy k existujícímu souboru */
            uint16_t extent_num = (uint16_t) e->s2 * 32 + e->extent;
            uint32_t records_before = (uint32_t) extent_num * MZDSK_CPM_RECORDS_PER_EXTENT;
            uint32_t total_records = records_before + extent_records;
            uint32_t new_size = total_records * MZDSK_CPM_RECORD_SIZE;
            if ( new_size > files[found].size ) {
                files[found].size = new_size;
            }
            files[found].extent_count++;
        } else {
            /* Nový soubor */
            if ( file_count >= max_files ) continue;

            memset ( &files[file_count], 0, sizeof ( st_MZDSK_CPM_FILE_INFO_EX ) );

            files[file_count].user = e->user;
            strncpy ( files[file_count].filename, fname, sizeof ( files[file_count].filename ) - 1 );
            files[file_count].filename[sizeof ( files[file_count].filename ) - 1] = '\0';
            strncpy ( files[file_count].extension, ext, sizeof ( files[file_count].extension ) - 1 );
            files[file_count].extension[sizeof ( files[file_count].extension ) - 1] = '\0';

            /* Velikost */
            uint16_t extent_num = (uint16_t) e->s2 * 32 + e->extent;
            uint32_t records_before = (uint32_t) extent_num * MZDSK_CPM_RECORDS_PER_EXTENT;
            files[file_count].size = ( records_before + extent_records ) * MZDSK_CPM_RECORD_SIZE;

            /* Atributy z prvního extentu */
            uint8_t attrs = cpm_extract_attributes ( e );
            files[file_count].read_only = ( attrs & MZDSK_CPM_ATTR_READ_ONLY ) ? 1 : 0;
            files[file_count].system    = ( attrs & MZDSK_CPM_ATTR_SYSTEM )    ? 1 : 0;
            files[file_count].archived  = ( attrs & MZDSK_CPM_ATTR_ARCHIVED )  ? 1 : 0;

            files[file_count].extent_count = 1;
            files[file_count].first_dir_index = (uint16_t) i;

            file_count++;
        }
    }

    return file_count;
}


/**
 * @brief Spočítá statistiku obsazení adresářových slotů.
 *
 * Načte surový adresář a klasifikuje každý 32B slot podle user bajtu:
 * used (user 0-15), free (0xE5), blocked (ostatní - out-of-range user
 * areas, které BDOS nezobrazí ve výpisu, ale drží je jako alokované).
 *
 * @param[in]  disc  Kontext disku.
 * @param[in]  dpb   Disk Parameter Block.
 * @param[out] stats Výstupní statistika.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DSK_ERROR při I/O chybě.
 */
en_MZDSK_RES mzdsk_cpm_get_dir_stats ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                         st_MZDSK_CPM_DIR_STATS *stats ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL || stats == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    memset ( stats, 0, sizeof ( *stats ) );
    stats->total = (uint16_t) ( dpb->drm + 1 );

    /* Načteme surový adresář. CPM_MAX_DIR_ENTRIES == 512 >> praktické limity. */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    for ( int i = 0; i < num_entries; i++ ) {
        uint8_t u = entries[i].user;
        if ( u == MZDSK_CPM_DELETED_ENTRY ) {
            stats->free++;
        } else if ( u <= 15 ) {
            stats->used++;
        } else {
            stats->blocked++;
        }
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše jednu adresářovou položku na disk.
 *
 * Veřejná obálka interní funkce cpm_write_dir_entry().
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  entry_index Index adresářové položky (0-based).
 * @param[in]  entry Adresářová položka k zápisu.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_write_dir_entry ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                           int entry_index,
                                           const st_MZDSK_CPM_DIRENTRY *entry ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL || entry == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    if ( entry_index < 0 || entry_index > (int) dpb->drm ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    return cpm_write_dir_entry ( disc, dpb, entry_index, entry );
}


/**
 * @brief Inicializuje prázdný CP/M adresář (vyplní 0xE5).
 *
 * Zapíše všechny adresářové bloky (určené AL0/AL1) hodnotou 0xE5,
 * čímž vytvoří prázdný adresář.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_format_directory ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Blok vyplněný hodnotou 0xE5 (smazané položky). Heap místo stacku -
     * 16 kB. Audit H-9. */
    uint8_t *empty_block = malloc ( CPM_MAX_BLOCK_SIZE );
    if ( empty_block == NULL ) return MZDSK_RES_UNKNOWN_ERROR;
    memset ( empty_block, MZDSK_CPM_DELETED_ENTRY, dpb->block_size );

    /* Zapíšeme všechny adresářové bloky */
    uint16_t dir_alloc = ( (uint16_t) dpb->al0 << 8 ) | dpb->al1;
    int bit;

    for ( bit = 15; bit >= 0; bit-- ) {
        if ( !( dir_alloc & ( 1u << bit ) ) ) continue;

        uint16_t block_num = (uint16_t) ( 15 - bit );

        en_MZDSK_RES res = mzdsk_cpm_write_block ( disc, dpb, block_num, empty_block, dpb->block_size );
        if ( res != MZDSK_RES_OK ) {
            free ( empty_block );
            return res;
        }
    }

    free ( empty_block );
    return MZDSK_RES_OK;
}


/* ================================================================
 * Implementace veřejného API - souborové operace
 * ================================================================ */


/**
 * @brief Kontrola existence souboru na CP/M disku.
 *
 * Vyhledá soubor podle jména, přípony a uživatelského čísla.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  filename Jméno souboru.
 * @param[in]  ext Přípona souboru.
 * @param[in]  user Číslo uživatele.
 * @return 1 pokud soubor existuje, 0 pokud ne, -1 při chybě.
 */
int mzdsk_cpm_file_exists ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                             const char *filename, const char *ext, uint8_t user ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         filename == NULL || ext == NULL ) {
        return -1;
    }

    /* Připravíme jméno pro porovnání */
    uint8_t cpm_fname[8];
    uint8_t cpm_ext[3];
    cpm_prepare_name ( filename, cpm_fname, 8 );
    cpm_prepare_name ( ext, cpm_ext, 3 );

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return -1;

    /* Hledáme alespoň jeden extent souboru */
    int i;
    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], cpm_fname, cpm_ext, user ) ) {
            return 1;
        }
    }

    return 0;
}


/**
 * @brief Přečte obsah souboru z CP/M disku do paměti.
 *
 * Projde všechny extenty souboru ve správném pořadí (seřazené
 * podle čísla extentu), přečte alokační bloky a zkopíruje data
 * do výstupního bufferu.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  filename Jméno souboru.
 * @param[in]  ext Přípona souboru.
 * @param[in]  user Číslo uživatele.
 * @param[out] buffer Výstupní buffer.
 * @param[in]  buffer_size Velikost bufferu.
 * @param[out] bytes_read Počet přečtených bajtů (může být NULL).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_read_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                    const char *filename, const char *ext,
                                    uint8_t user, void *buffer, uint32_t buffer_size,
                                    uint32_t *bytes_read ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         filename == NULL || ext == NULL || buffer == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    if ( bytes_read != NULL ) *bytes_read = 0;

    /* Připravíme jméno pro porovnání */
    uint8_t cpm_fname[8];
    uint8_t cpm_ext[3];
    cpm_prepare_name ( filename, cpm_fname, 8 );
    cpm_prepare_name ( ext, cpm_ext, 3 );

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Najdeme všechny extenty souboru a seřadíme je */
    st_MZDSK_CPM_DIRENTRY *file_extents[CPM_MAX_DIR_ENTRIES];
    uint16_t extent_nums[CPM_MAX_DIR_ENTRIES];
    int extent_count = 0;

    int i;
    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], cpm_fname, cpm_ext, user ) ) {
            file_extents[extent_count] = &entries[i];
            extent_nums[extent_count] = (uint16_t) entries[i].s2 * 32 + entries[i].extent;
            extent_count++;
        }
    }

    if ( extent_count == 0 ) return MZDSK_RES_FILE_NOT_FOUND;

    /* Seřadíme extenty podle čísla (selection sort - malý počet) */
    int si;
    for ( si = 0; si < extent_count - 1; si++ ) {
        int min_idx = si;
        int sj;
        for ( sj = si + 1; sj < extent_count; sj++ ) {
            if ( extent_nums[sj] < extent_nums[min_idx] ) {
                min_idx = sj;
            }
        }
        if ( min_idx != si ) {
            /* Prohodit */
            st_MZDSK_CPM_DIRENTRY *tmp_e = file_extents[si];
            file_extents[si] = file_extents[min_idx];
            file_extents[min_idx] = tmp_e;

            uint16_t tmp_n = extent_nums[si];
            extent_nums[si] = extent_nums[min_idx];
            extent_nums[min_idx] = tmp_n;
        }
    }

    /* Přečteme data ze všech extentů */
    uint32_t total_read = 0;
    uint8_t *out = (uint8_t *) buffer;

    int ei;
    for ( ei = 0; ei < extent_count; ei++ ) {
        const st_MZDSK_CPM_DIRENTRY *e = file_extents[ei];

        /* Počet bajtů v tomto extentu.
           S EXM > 0 jeden directory entry pokrývá (EXM+1) logických extentů.
           Non-last entry: všechny bloky jsou plné.
           Last entry: (EX & EXM) udává sub-extent, RC počet záznamů v něm. */
        uint32_t extent_bytes;
        if ( ei < extent_count - 1 ) {
            /* Plný entry - všechny bloky */
            int bp = ( dpb->dsm <= 255 ) ? 16 : 8;
            extent_bytes = (uint32_t) bp * dpb->block_size;
        } else {
            /* Poslední entry - sub-extent + RC */
            uint16_t sub_extent = e->extent & dpb->exm;
            extent_bytes = (uint32_t) ( sub_extent * MZDSK_CPM_RECORDS_PER_EXTENT + e->rc ) * MZDSK_CPM_RECORD_SIZE;
        }

        if ( extent_bytes == 0 ) continue;

        /* Počet bloků v tomto extentu potřebných pro extent_bytes dat */
        int blocks_per_extent;
        if ( dpb->dsm <= 255 ) {
            blocks_per_extent = 16;
        } else {
            blocks_per_extent = 8;
        }

        /* Přečteme bloky */
        uint32_t bytes_left = extent_bytes;
        int bi;
        for ( bi = 0; bi < blocks_per_extent && bytes_left > 0; bi++ ) {
            uint16_t block_num;

            if ( dpb->dsm <= 255 ) {
                block_num = e->alloc[bi];
            } else {
                block_num = (uint16_t) e->alloc[bi * 2] | ( (uint16_t) e->alloc[bi * 2 + 1] << 8 );
            }

            if ( block_num == 0 ) continue;

            /* Kolik bajtů přečíst z tohoto bloku */
            uint16_t to_read = dpb->block_size;
            if ( to_read > bytes_left ) to_read = (uint16_t) bytes_left;

            /* Kontrola přetečení bufferu */
            if ( total_read + to_read > buffer_size ) {
                return MZDSK_RES_BUFFER_SMALL;
            }

            en_MZDSK_RES res = mzdsk_cpm_read_block ( disc, dpb, block_num, out + total_read, to_read );
            if ( res != MZDSK_RES_OK ) return res;

            total_read += to_read;
            bytes_left -= to_read;
        }
    }

    if ( bytes_read != NULL ) *bytes_read = total_read;

    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše soubor na CP/M disk.
 *
 * Vytvoří potřebný počet directory entries (fyzických extentů) a alokuje
 * bloky pro data. Každý directory entry pokrývá (EXM+1) logických extentů
 * po 128 záznamech (16 kB). Pole extent/s2 v entry obsahuje logické číslo
 * extentu včetně sub-extent bitů, RC udává počet záznamů v posledním
 * sub-extentu (0-128).
 *
 * Postup:
 * 1. Kontrola, že soubor se stejným jménem neexistuje
 * 2. Přečtení adresáře a sestavení bitmapy obsazených bloků
 * 3. Alokace bloků a zápis dat
 * 4. Zápis adresářových položek s korektním extent číslováním a RC
 *
 * @param[in]  disc Kontext disku. Nesmí být NULL.
 * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
 * @param[in]  filename Jméno souboru (max 8 znaků). Nesmí být NULL.
 * @param[in]  ext Přípona souboru (max 3 znaky). Nesmí být NULL.
 * @param[in]  user Číslo uživatele (0-15).
 * @param[in]  data Ukazatel na data k zápisu. Nesmí být NULL.
 * @param[in]  data_size Velikost dat v bajtech.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc != NULL && disc->handler != NULL
 * @pre dpb != NULL && filename != NULL && ext != NULL && data != NULL
 * @post Při úspěchu soubor existuje na disku s danými daty.
 *
 * @par Vedlejší efekty:
 * - Modifikuje adresářové bloky a datové bloky na disku.
 */
en_MZDSK_RES mzdsk_cpm_write_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                     const char *filename, const char *ext,
                                     uint8_t user, const void *data, uint32_t data_size ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         filename == NULL || ext == NULL || data == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Připravíme jméno */
    uint8_t cpm_fname[8];
    uint8_t cpm_ext[3];
    cpm_prepare_name ( filename, cpm_fname, 8 );
    cpm_prepare_name ( ext, cpm_ext, 3 );

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Kontrola, že soubor neexistuje */
    int i;
    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], cpm_fname, cpm_ext, user ) ) {
            return MZDSK_RES_FILE_EXISTS;
        }
    }

    /* Sestavíme bitmapu obsazených bloků */
    uint8_t alloc_map[CPM_MAX_BLOCKS / 8 + 1];
    cpm_build_alloc_map ( entries, num_entries, alloc_map, dpb->dsm );

    /* Označíme adresářové bloky jako obsazené */
    cpm_mark_dir_blocks ( dpb, alloc_map );

    /* Spočítáme potřebný počet extentů */
    uint32_t total_records = ( data_size + MZDSK_CPM_RECORD_SIZE - 1 ) / MZDSK_CPM_RECORD_SIZE;
    if ( total_records == 0 ) total_records = 1; /* Alespoň jeden záznam */

    int blocks_per_extent;
    if ( dpb->dsm <= 255 ) {
        blocks_per_extent = 16;
    } else {
        blocks_per_extent = 8;
    }

    uint16_t bytes_per_extent = (uint16_t) blocks_per_extent * dpb->block_size;
    uint32_t total_extents = ( data_size + bytes_per_extent - 1 ) / bytes_per_extent;
    if ( total_extents == 0 ) total_extents = 1;

    /* Najdeme volné adresářové položky */
    int free_dir_indices[CPM_MAX_DIR_ENTRIES];
    int free_dir_count = 0;

    for ( i = 0; i < num_entries; i++ ) {
        if ( entries[i].user == MZDSK_CPM_DELETED_ENTRY ) {
            free_dir_indices[free_dir_count++] = i;
        }
    }

    /* Pokud nejsou dostatečně smazané, zkusíme nevyužité sloty */
    for ( i = num_entries; i <= (int) dpb->drm && free_dir_count < CPM_MAX_DIR_ENTRIES; i++ ) {
        free_dir_indices[free_dir_count++] = i;
    }

    if ( (uint32_t) free_dir_count < total_extents ) {
        return MZDSK_RES_DIR_FULL;
    }

    /* Zapíšeme extenty a data */
    const uint8_t *src = (const uint8_t *) data;
    uint32_t data_remaining = data_size;
    uint32_t extent_idx;

    for ( extent_idx = 0; extent_idx < total_extents; extent_idx++ ) {

        st_MZDSK_CPM_DIRENTRY new_entry;
        memset ( &new_entry, 0, sizeof ( new_entry ) );

        new_entry.user = user;
        memcpy ( new_entry.fname, cpm_fname, 8 );
        memcpy ( new_entry.ext, cpm_ext, 3 );
        new_entry.s1 = 0;

        /* Kolik bajtů v tomto directory entry */
        uint32_t extent_data = data_remaining;
        if ( extent_data > bytes_per_extent ) extent_data = bytes_per_extent;

        /* Počet 128B záznamů v tomto directory entry */
        uint32_t records_in_entry = ( extent_data + MZDSK_CPM_RECORD_SIZE - 1 ) / MZDSK_CPM_RECORD_SIZE;

        /* Výpočet CP/M extent čísla a RC.
         * Každý directory entry pokrývá (EXM+1) logických extentů po 128 záznamech.
         * Sub-extent (nízké bity extent čísla maskované EXM) udává, do kterého
         * logického extentu v rámci entry data sahají.
         * RC udává počet záznamů v posledním sub-extentu (0-128). */
        uint32_t sub_extent = 0;
        if ( records_in_entry > MZDSK_CPM_RECORDS_PER_EXTENT ) {
            sub_extent = ( records_in_entry - 1 ) / MZDSK_CPM_RECORDS_PER_EXTENT;
        }
        uint32_t logical_extent = extent_idx * ( (uint32_t) dpb->exm + 1 ) + sub_extent;

        new_entry.extent = (uint8_t) ( logical_extent & 0x1F );
        new_entry.s2 = (uint8_t) ( logical_extent >> 5 );
        new_entry.rc = (uint8_t) ( records_in_entry - sub_extent * MZDSK_CPM_RECORDS_PER_EXTENT );

        /* Počet bloků potřebných v tomto extentu */
        uint32_t blocks_needed = ( extent_data + dpb->block_size - 1 ) / dpb->block_size;

        /* Alokujeme bloky a zapisujeme data */
        uint32_t bi;
        for ( bi = 0; bi < blocks_needed; bi++ ) {
            uint16_t free_block = cpm_find_free_block ( alloc_map, dpb->dsm );
            if ( free_block == 0xFFFF ) {
                return MZDSK_RES_DISK_FULL;
            }

            /* Označíme blok jako obsazený */
            alloc_map[free_block / 8] |= (uint8_t) ( 1u << ( free_block % 8 ) );

            /* Zapíšeme číslo bloku do alokační mapy extentu.
             * `alloc[]` má 16 bajtů. Pro 16-bit block entries (dsm > 255)
             * je maximum `bi = 7` (→ alloc[14..15]), což platí jen pokud
             * `blocks_needed <= 8`. To plyne z `exm+1 = 8` (max extent
             * records) + `block_size = 16384`. Invariant je výše
             * kalkulovaný z DPB, ne zde ověřovaný. Audit M-7. */
            if ( dpb->dsm <= 255 ) {
                new_entry.alloc[bi] = (uint8_t) free_block;
            } else {
                new_entry.alloc[bi * 2] = (uint8_t) ( free_block & 0xFF );
                new_entry.alloc[bi * 2 + 1] = (uint8_t) ( free_block >> 8 );
            }

            /* Zapíšeme data do bloku */
            uint16_t write_size = dpb->block_size;
            if ( write_size > data_remaining ) write_size = (uint16_t) data_remaining;

            /* Pokud zbývá méně dat než celý blok, doplníme CP/M EOF (0x1A).
             * Heap místo stacku - 16 kB. Audit H-9. */
            if ( write_size < dpb->block_size ) {
                uint8_t *block_buf = malloc ( CPM_MAX_BLOCK_SIZE );
                if ( block_buf == NULL ) return MZDSK_RES_UNKNOWN_ERROR;
                memset ( block_buf, 0x1A, dpb->block_size );
                memcpy ( block_buf, src, write_size );
                en_MZDSK_RES res = mzdsk_cpm_write_block ( disc, dpb, free_block, block_buf, dpb->block_size );
                free ( block_buf );
                if ( res != MZDSK_RES_OK ) return MZDSK_RES_DISK_FULL;
            } else {
                en_MZDSK_RES res = mzdsk_cpm_write_block ( disc, dpb, free_block, src, write_size );
                if ( res != MZDSK_RES_OK ) return MZDSK_RES_DISK_FULL;
            }

            src += write_size;
            data_remaining -= write_size;
        }

        /* Zapíšeme adresářovou položku */
        int dir_idx = free_dir_indices[extent_idx];

        /* Pokud index je za stávajícím rozsahem, musíme vytvořit prázdnou položku */
        if ( dir_idx >= num_entries ) {
            st_MZDSK_CPM_DIRENTRY empty_entry;
            memset ( &empty_entry, MZDSK_CPM_DELETED_ENTRY, sizeof ( empty_entry ) );

            int fi;
            for ( fi = num_entries; fi < dir_idx; fi++ ) {
                en_MZDSK_RES res = cpm_write_dir_entry ( disc, dpb, fi, &empty_entry );
                if ( res != MZDSK_RES_OK ) return res;
            }

            if ( dir_idx >= num_entries ) {
                num_entries = dir_idx + 1;
            }
        }

        en_MZDSK_RES res = cpm_write_dir_entry ( disc, dpb, dir_idx, &new_entry );
        if ( res != MZDSK_RES_OK ) return res;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Smaže soubor z CP/M disku.
 *
 * Najde všechny extenty souboru a označí je jako smazané
 * (user byte = 0xE5).
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  filename Jméno souboru.
 * @param[in]  ext Přípona souboru.
 * @param[in]  user Číslo uživatele.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_NOT_FOUND pokud neexistuje.
 */
en_MZDSK_RES mzdsk_cpm_delete_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                      const char *filename, const char *ext,
                                      uint8_t user ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         filename == NULL || ext == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Připravíme jméno */
    uint8_t cpm_fname[8];
    uint8_t cpm_ext[3];
    cpm_prepare_name ( filename, cpm_fname, 8 );
    cpm_prepare_name ( ext, cpm_ext, 3 );

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Najdeme všechny extenty souboru a smažeme je */
    int deleted_count = 0;

    int i;
    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], cpm_fname, cpm_ext, user ) ) {
            entries[i].user = MZDSK_CPM_DELETED_ENTRY;

            en_MZDSK_RES res = cpm_write_dir_entry ( disc, dpb, i, &entries[i] );
            if ( res != MZDSK_RES_OK ) return res;

            deleted_count++;
        }
    }

    if ( deleted_count == 0 ) return MZDSK_RES_FILE_NOT_FOUND;

    return MZDSK_RES_OK;
}


/**
 * @brief Přejmenuje soubor na CP/M disku.
 *
 * Najde všechny extenty starého souboru, ověří že nové jméno
 * neexistuje, a změní jméno a příponu ve všech extentech.
 * Atributy (bity 7 přípony) se zachovají.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  old_name Současné jméno souboru.
 * @param[in]  old_ext Současná přípona.
 * @param[in]  user Číslo uživatele.
 * @param[in]  new_name Nové jméno souboru.
 * @param[in]  new_ext Nová přípona.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_rename_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                      const char *old_name, const char *old_ext,
                                      uint8_t user,
                                      const char *new_name, const char *new_ext ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         old_name == NULL || old_ext == NULL ||
         new_name == NULL || new_ext == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Připravíme jména */
    uint8_t old_fname[8];
    uint8_t old_e[3];
    uint8_t new_fname[8];
    uint8_t new_e[3];

    cpm_prepare_name ( old_name, old_fname, 8 );
    cpm_prepare_name ( old_ext, old_e, 3 );
    cpm_prepare_name ( new_name, new_fname, 8 );
    cpm_prepare_name ( new_ext, new_e, 3 );

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Ověříme, že nové jméno neexistuje (pokud se liší od starého) */
    if ( memcmp ( old_fname, new_fname, 8 ) != 0 || memcmp ( old_e, new_e, 3 ) != 0 ) {
        int i;
        for ( i = 0; i < num_entries; i++ ) {
            if ( cpm_match_entry ( &entries[i], new_fname, new_e, user ) ) {
                return MZDSK_RES_FILE_EXISTS;
            }
        }
    }

    /* Přejmenujeme všechny extenty starého souboru */
    int renamed_count = 0;
    int i;

    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], old_fname, old_e, user ) ) {

            /* Zachováme atributy (bity 7) z původní přípony */
            uint8_t attrs = cpm_extract_attributes ( &entries[i] );

            /* Nastavíme nové jméno a příponu */
            memcpy ( entries[i].fname, new_fname, 8 );
            memcpy ( entries[i].ext, new_e, 3 );

            /* Obnovíme atributy */
            cpm_apply_attributes ( &entries[i], attrs );

            en_MZDSK_RES res = cpm_write_dir_entry ( disc, dpb, i, &entries[i] );
            if ( res != MZDSK_RES_OK ) return res;

            renamed_count++;
        }
    }

    if ( renamed_count == 0 ) return MZDSK_RES_FILE_NOT_FOUND;

    return MZDSK_RES_OK;
}


/**
 * @brief Změní user number existujícího souboru.
 *
 * Najde všechny extenty souboru v old_user a přepíše jim user byte
 * na new_user. Před modifikací ověří, že v new_user neexistuje soubor
 * stejného jména (namespace kolize).
 *
 * Pokud old_user == new_user, je operace no-op a vrací MZDSK_RES_OK.
 *
 * @param[in] disc     Kontext disku (RW).
 * @param[in] dpb      Disk Parameter Block.
 * @param[in] filename Jméno souboru.
 * @param[in] ext      Přípona souboru.
 * @param[in] old_user Současný user number (0-15).
 * @param[in] new_user Nový user number (0-15).
 * @return MZDSK_RES_OK / FILE_NOT_FOUND / FILE_EXISTS / INVALID_PARAM / DSK_ERROR.
 */
en_MZDSK_RES mzdsk_cpm_set_user ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                    const char *filename, const char *ext,
                                    uint8_t old_user, uint8_t new_user ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         filename == NULL || ext == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Rozsah 0-15; 0xE5 je smazaný, vyšší hodnoty nejsou v CP/M 2.2 platné */
    if ( old_user > 15 || new_user > 15 ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Stejný user - nic k dělání */
    if ( old_user == new_user ) {
        return MZDSK_RES_OK;
    }

    /* Připravíme jméno pro porovnání */
    uint8_t cpm_fname[8];
    uint8_t cpm_ext[3];
    cpm_prepare_name ( filename, cpm_fname, 8 );
    cpm_prepare_name ( ext, cpm_ext, 3 );

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Kontrola namespace kolize v cílové user oblasti. CP/M 2.2 má každou
       user area jako izolovaný namespace - stejné jméno v new_user by
       vytvořilo nejednoznačnost pro read/delete. */
    int i;
    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], cpm_fname, cpm_ext, new_user ) ) {
            return MZDSK_RES_FILE_EXISTS;
        }
    }

    /* Přepíšeme user byte všech extentů */
    int modified_count = 0;
    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], cpm_fname, cpm_ext, old_user ) ) {
            entries[i].user = new_user;
            en_MZDSK_RES res = cpm_write_dir_entry ( disc, dpb, i, &entries[i] );
            if ( res != MZDSK_RES_OK ) return res;
            modified_count++;
        }
    }

    if ( modified_count == 0 ) return MZDSK_RES_FILE_NOT_FOUND;

    return MZDSK_RES_OK;
}


/* ================================================================
 * Implementace veřejného API - atributy
 * ================================================================ */


/**
 * @brief Nastaví souborové atributy na všech extentech souboru.
 *
 * Přepíše atributy (bity 7 příponových bajtů) na zadanou
 * kombinaci. Dolní bity přípony (vlastní znaky) se zachovají.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  filename Jméno souboru.
 * @param[in]  ext Přípona souboru.
 * @param[in]  user Číslo uživatele.
 * @param[in]  attributes Kombinace en_MZDSK_CPM_ATTR.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_set_attributes ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                          const char *filename, const char *ext,
                                          uint8_t user, uint8_t attributes ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         filename == NULL || ext == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Připravíme jméno */
    uint8_t cpm_fname[8];
    uint8_t cpm_ext[3];
    cpm_prepare_name ( filename, cpm_fname, 8 );
    cpm_prepare_name ( ext, cpm_ext, 3 );

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Nastavíme atributy na všech extentech souboru */
    int modified_count = 0;
    int i;

    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], cpm_fname, cpm_ext, user ) ) {

            cpm_apply_attributes ( &entries[i], attributes );

            en_MZDSK_RES res = cpm_write_dir_entry ( disc, dpb, i, &entries[i] );
            if ( res != MZDSK_RES_OK ) return res;

            modified_count++;
        }
    }

    if ( modified_count == 0 ) return MZDSK_RES_FILE_NOT_FOUND;

    return MZDSK_RES_OK;
}


/**
 * @brief Zjistí souborové atributy z první adresářové položky souboru.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[in]  filename Jméno souboru.
 * @param[in]  ext Přípona souboru.
 * @param[in]  user Číslo uživatele.
 * @param[out] attributes Výstup: kombinace en_MZDSK_CPM_ATTR.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_get_attributes ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                          const char *filename, const char *ext,
                                          uint8_t user, uint8_t *attributes ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ||
         filename == NULL || ext == NULL || attributes == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Připravíme jméno */
    uint8_t cpm_fname[8];
    uint8_t cpm_ext[3];
    cpm_prepare_name ( filename, cpm_fname, 8 );
    cpm_prepare_name ( ext, cpm_ext, 3 );

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Najdeme první extent souboru */
    int i;
    for ( i = 0; i < num_entries; i++ ) {
        if ( cpm_match_entry ( &entries[i], cpm_fname, cpm_ext, user ) ) {
            *attributes = cpm_extract_attributes ( &entries[i] );
            return MZDSK_RES_OK;
        }
    }

    return MZDSK_RES_FILE_NOT_FOUND;
}


/* ================================================================
 * Implementace veřejného API - alokační mapa a volné místo
 * ================================================================ */


/**
 * @brief Sestaví kompletní alokační bitmapu disku.
 *
 * Přečte adresář, sestaví bitmapu obsazených bloků (včetně
 * adresářových) a spočítá statistiky (total/used/free blocks).
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @param[out] alloc_map Výstupní alokační mapa.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES mzdsk_cpm_get_alloc_map ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                         st_MZDSK_CPM_ALLOC_MAP *alloc_map ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL || alloc_map == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return MZDSK_RES_DSK_ERROR;

    /* Vynulujeme celou alokační mapu */
    memset ( alloc_map, 0, sizeof ( st_MZDSK_CPM_ALLOC_MAP ) );

    /* Sestavíme bitmapu obsazených bloků */
    cpm_build_alloc_map ( entries, num_entries, alloc_map->map, dpb->dsm );

    /* Označíme adresářové bloky */
    cpm_mark_dir_blocks ( dpb, alloc_map->map );

    /* Spočítáme statistiky */
    alloc_map->total_blocks = dpb->dsm + 1;
    alloc_map->used_blocks = 0;
    alloc_map->free_blocks = 0;

    uint16_t block;
    for ( block = 0; block <= dpb->dsm; block++ ) {
        if ( alloc_map->map[block / 8] & ( 1u << ( block % 8 ) ) ) {
            alloc_map->used_blocks++;
        } else {
            alloc_map->free_blocks++;
        }
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Spočítá volné místo na CP/M disku.
 *
 * Přečte adresář, sestaví bitmapu obsazených bloků (včetně
 * adresářových bloků označených v AL0/AL1) a spočítá volné bloky.
 *
 * @param[in]  disc Kontext disku.
 * @param[in]  dpb Disk Parameter Block.
 * @return Volné místo v bajtech, nebo 0 při chybě.
 */
uint32_t mzdsk_cpm_free_space ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ) return 0;

    /* Přečteme adresář */
    st_MZDSK_CPM_DIRENTRY entries[CPM_MAX_DIR_ENTRIES];
    int num_entries = cpm_read_raw_directory ( disc, dpb, entries, dpb->drm + 1 );
    if ( num_entries < 0 ) return 0;

    /* Sestavíme bitmapu obsazených bloků */
    uint8_t alloc_map[CPM_MAX_BLOCKS / 8 + 1];
    cpm_build_alloc_map ( entries, num_entries, alloc_map, dpb->dsm );

    /* Označíme adresářové bloky jako obsazené */
    cpm_mark_dir_blocks ( dpb, alloc_map );

    /* Spočítáme volné bloky */
    uint32_t free_blocks = 0;
    uint16_t block;
    for ( block = 0; block <= dpb->dsm; block++ ) {
        if ( !( alloc_map[block / 8] & ( 1u << ( block % 8 ) ) ) ) {
            free_blocks++;
        }
    }

    return free_blocks * dpb->block_size;
}


/* ================================================================
 * Implementace veřejného API - detekce formátu
 * ================================================================ */


/**
 * @brief Detekuje CP/M formát z identifikovaného formátu disku.
 *
 * Převede formát z DSK identifikace (en_DSK_TOOLS_IDENTFORMAT)
 * na CP/M formát (en_MZDSK_CPM_FORMAT). HD geometrie (18x512)
 * vrátí MZDSK_CPM_FORMAT_HD, SD geometrie (9x512) vrátí
 * MZDSK_CPM_FORMAT_SD.
 *
 * @param[in]  disc Kontext disku.
 * @return Detekovaný CP/M formát.
 *
 * @note Pokud formát nelze rozpoznat, vrátí MZDSK_CPM_FORMAT_SD jako výchozí.
 */
en_MZDSK_CPM_FORMAT mzdsk_cpm_detect_format ( const st_MZDSK_DISC *disc ) {

    if ( disc == NULL ) return MZDSK_CPM_FORMAT_SD;

    if ( disc->format == DSK_TOOLS_IDENTFORMAT_MZCPMHD ) {
        return MZDSK_CPM_FORMAT_HD;
    }

    return MZDSK_CPM_FORMAT_SD;
}


/* ================================================================
 * Diagnostika konzistence
 * ================================================================ */


/**
 * @brief Zkontroluje konzistenci extentů CP/M souboru.
 *
 * Přečte surový adresář, vyhledá všechny extent entries daného souboru,
 * převede je na fyzické extenty (maskováním EXM sub-extent bitů)
 * a ověří, zda existují všechny fyzické extenty 0..max_ext.
 * Chybějící extenty zapíše do výsledkové struktury.
 *
 * @param[in]  disc     Kontext disku. Nesmí být NULL.
 * @param[in]  dpb      Disk Parameter Block. Nesmí být NULL.
 * @param[in]  filename Jméno souboru (velká písmena, ořezané). Nesmí být NULL.
 * @param[in]  ext      Přípona souboru (velká písmena, ořezaná). Nesmí být NULL.
 * @param[in]  user     Číslo uživatele (0-15).
 * @param[out] result   Výsledek kontroly. Nesmí být NULL.
 *
 * @return 0 při úspěchu, -1 při chybě čtení adresáře.
 */
int mzdsk_cpm_check_extents ( st_MZDSK_DISC *disc,
                               const st_MZDSK_CPM_DPB *dpb,
                               const char *filename,
                               const char *ext,
                               uint8_t user,
                               st_MZDSK_CPM_EXTENT_CHECK *result ) {

    result->count = 0;

    /* Přečteme surový adresář */
    st_MZDSK_CPM_DIRENTRY raw_entries[MZDSK_CPM_MAX_MISSING_EXTENTS];
    int raw_count = mzdsk_cpm_read_raw_directory ( disc, dpb, raw_entries, dpb->drm + 1 );
    if ( raw_count < 0 ) return -1;

    /* Sesbíráme čísla extentů souboru */
    uint16_t extent_nums[MZDSK_CPM_MAX_MISSING_EXTENTS];
    int extent_count = 0;

    for ( int r = 0; r < raw_count; r++ ) {
        const st_MZDSK_CPM_DIRENTRY *e = &raw_entries[r];
        if ( e->user != user ) continue;
        if ( e->user == MZDSK_CPM_DELETED_ENTRY ) continue;

        /* Extrahujeme a porovnáme jméno */
        char rf[9], re[4];
        for ( int k = 0; k < 8; k++ ) rf[k] = (char) ( e->fname[k] & 0x7F );
        rf[8] = '\0';
        for ( int k = 0; k < 3; k++ ) re[k] = (char) ( e->ext[k] & 0x7F );
        re[3] = '\0';

        /* Ořežeme trailing mezery */
        while ( rf[0] && rf[strlen ( rf ) - 1] == ' ' ) rf[strlen ( rf ) - 1] = '\0';
        while ( re[0] && re[strlen ( re ) - 1] == ' ' ) re[strlen ( re ) - 1] = '\0';

        if ( strcmp ( rf, filename ) != 0 || strcmp ( re, ext ) != 0 ) continue;

        /* Převedeme na fyzický extent (maskujeme EXM sub-extent bity) */
        uint16_t ext_num = (uint16_t) ( e->s2 * 32 + e->extent );
        uint16_t phys_ext = (uint16_t) ( ext_num / ( dpb->exm + 1 ) );
        if ( extent_count < MZDSK_CPM_MAX_MISSING_EXTENTS ) {
            extent_nums[extent_count++] = phys_ext;
        }
    }

    if ( extent_count == 0 ) return 0;

    /* Najdeme maximální číslo fyzického extentu */
    uint16_t max_ext = 0;
    for ( int i = 0; i < extent_count; i++ ) {
        if ( extent_nums[i] > max_ext ) max_ext = extent_nums[i];
    }

    /* Zkontrolujeme, zda existují všechny fyzické extenty 0..max_ext */
    for ( uint16_t ex = 0; ex <= max_ext; ex++ ) {
        int found = 0;
        for ( int i = 0; i < extent_count; i++ ) {
            if ( extent_nums[i] == ex ) {
                found = 1;
                break;
            }
        }
        if ( !found && result->count < MZDSK_CPM_MAX_MISSING_EXTENTS ) {
            result->missing[result->count++] = ex;
        }
    }

    return 0;
}


/* ================================================================
 * Defragmentace
 * ================================================================ */


/** @brief Maximální počet souborů pro defragmentaci. */
#define CPM_DEFRAG_MAX_FILES  256


/**
 * @brief Informace o souboru pro interní použití při defragmentaci.
 *
 * Obsahuje metadata a ukazatel na data souboru v paměti.
 * Používá se jako mezipaměť mezi fází čtení a fází zápisu
 * při defragmentaci.
 *
 * @par Ownership:
 * - data ukazuje na dynamicky alokovanou paměť, volající musí uvolnit
 */
typedef struct st_CPM_DEFRAG_FILE {
    char filename[9];       /**< Jméno souboru, null-terminated */
    char extension[4];      /**< Přípona souboru, null-terminated */
    uint8_t user;           /**< Číslo uživatele (0-15) */
    uint8_t attributes;     /**< Kombinace en_MZDSK_CPM_ATTR */
    uint32_t size;          /**< Velikost souboru v bajtech */
    uint8_t *data;          /**< Ukazatel na data souboru v paměti */
} st_CPM_DEFRAG_FILE;


/**
 * @brief Provede defragmentaci CP/M disku.
 *
 * Načte všechny soubory do paměti, zformátuje adresář a zapíše
 * soubory zpět sekvenčně od prvního datového bloku. Zachovává
 * uživatelská čísla a souborové atributy.
 *
 * @param[in] disc        Kontext disku (RW). Nesmí být NULL.
 * @param[in] dpb         Disk Parameter Block. Nesmí být NULL.
 * @param[in] progress_cb Callback pro hlášení průběhu (může být NULL).
 * @param[in] cb_data     Uživatelská data pro callback (může být NULL).
 *
 * @return MZDSK_RES_OK při úspěchu.
 * @return MZDSK_RES_INVALID_PARAM pokud disc/handler/dpb je NULL nebo
 *         pokud handler NENÍ typu HANDLER_TYPE_MEMORY (audit H-5).
 * @return Jiný chybový kód při selhání IO nebo zápisu.
 *
 * @pre disc != NULL && disc->handler != NULL
 * @pre disc->handler->type == HANDLER_TYPE_MEMORY (audit H-5)
 * @pre dpb != NULL
 * @post Při úspěchu jsou všechny soubory sekvenčně uloženy bez mezer.
 *
 * @warning Defrag NENÍ interně transakční - selhání fáze 2 (format
 *          directory) nebo fáze 3 (sekvenční zápis) nechává disk
 *          v nekonzistentním stavu. Proto je vyžadován memory handler:
 *          volající otevře disk přes mzdsk_disc_open_memory(), po
 *          úspěšném defragu flushne přes mzdsk_disc_save(). Při chybě
 *          volající save nevolá a originální soubor zůstává nedotčený.
 */
en_MZDSK_RES mzdsk_cpm_defrag ( st_MZDSK_DISC *disc,
                                  const st_MZDSK_CPM_DPB *dpb,
                                  mzdsk_cpm_defrag_cb_t progress_cb,
                                  void *cb_data ) {

    if ( disc == NULL || disc->handler == NULL || dpb == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Audit H-5: defrag NENÍ transakční v rámci knihovny - fáze 2
     * (format directory) + fáze 3 (sekvenční zápis) probíhají přímo
     * nad handlerem. Selhání v půlce by u file handleru znamenalo
     * nenávratnou ztrátu dat. Vynucujeme memory handler - volající
     * musí disc otevřít přes mzdsk_disc_open_memory() a po úspěchu
     * flushnout mzdsk_disc_save(). Volající (mzdsk-cpm CLI + GUI
     * session) tento pattern splňují. */
    if ( disc->handler->type != HANDLER_TYPE_MEMORY ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    en_MZDSK_RES err = MZDSK_RES_OK;
    st_CPM_DEFRAG_FILE files[CPM_DEFRAG_MAX_FILES];
    int files_count = 0;
    char msg[128];

    /* Fáze 1: přečtení adresáře a načtení všech souborů do paměti */
    if ( progress_cb ) progress_cb ( "Defrag: reading directory\n", cb_data );

    st_MZDSK_CPM_FILE_INFO_EX file_list[CPM_DEFRAG_MAX_FILES];
    int count = mzdsk_cpm_read_directory_ex ( disc, dpb, file_list, CPM_DEFRAG_MAX_FILES );

    if ( count < 0 ) {
        return MZDSK_RES_DSK_ERROR;
    }

    if ( count == 0 ) {
        if ( progress_cb ) progress_cb ( "Defrag: no files found, nothing to do\n", cb_data );
        return MZDSK_RES_OK;
    }

    /* Maximální velikost dat na disku */
    uint32_t disk_capacity = (uint32_t) ( dpb->dsm + 1 ) * dpb->block_size;

    int i;
    for ( i = 0; i < count && files_count < CPM_DEFRAG_MAX_FILES; i++ ) {

        st_CPM_DEFRAG_FILE *f = &files[files_count];

        memcpy ( f->filename, file_list[i].filename, sizeof ( f->filename ) );
        memcpy ( f->extension, file_list[i].extension, sizeof ( f->extension ) );
        f->user = file_list[i].user;
        f->size = file_list[i].size;

        /* Sestavení atributů z rozšířených informací */
        f->attributes = 0;
        if ( file_list[i].read_only ) f->attributes |= MZDSK_CPM_ATTR_READ_ONLY;
        if ( file_list[i].system )    f->attributes |= MZDSK_CPM_ATTR_SYSTEM;
        if ( file_list[i].archived )  f->attributes |= MZDSK_CPM_ATTR_ARCHIVED;

        /* Alokace bufferu pro data souboru */
        uint32_t buf_size = f->size;
        if ( buf_size == 0 ) buf_size = 1;
        if ( buf_size > disk_capacity ) buf_size = disk_capacity;

        f->data = malloc ( buf_size );
        if ( !f->data ) {
            err = MZDSK_RES_UNKNOWN_ERROR;
            break;
        }

        /* Načtení dat souboru */
        uint32_t bytes_read = 0;
        err = mzdsk_cpm_read_file ( disc, dpb, f->filename, f->extension,
                                     f->user, f->data, buf_size, &bytes_read );
        if ( err != MZDSK_RES_OK ) {
            free ( f->data );
            f->data = NULL;
            break;
        }

        f->size = bytes_read;
        files_count++;

        if ( progress_cb ) {
            snprintf ( msg, sizeof ( msg ), "Defrag: read file %d/%d: %s.%s (%lu B)\n",
                       files_count, count, f->filename, f->extension,
                       (unsigned long) f->size );
            progress_cb ( msg, cb_data );
        }
    }

    /* Fáze 2: formátování adresáře */
    if ( err == MZDSK_RES_OK ) {

        if ( progress_cb ) progress_cb ( "Defrag: formatting directory\n", cb_data );

        err = mzdsk_cpm_format_directory ( disc, dpb );
    }

    /* Fáze 3: sekvenční zápis všech souborů */
    if ( err == MZDSK_RES_OK ) {

        if ( progress_cb && files_count > 0 ) progress_cb ( "Defrag: writing files\n", cb_data );

        for ( i = 0; i < files_count; i++ ) {

            st_CPM_DEFRAG_FILE *f = &files[i];

            err = mzdsk_cpm_write_file ( disc, dpb, f->filename, f->extension,
                                          f->user, f->data, f->size );
            if ( err != MZDSK_RES_OK ) break;

            /* Obnovení atributů (write_file je nenastavuje) */
            if ( f->attributes != 0 ) {
                err = mzdsk_cpm_set_attributes ( disc, dpb, f->filename, f->extension,
                                                  f->user, f->attributes );
                if ( err != MZDSK_RES_OK ) break;
            }

            if ( progress_cb ) {
                snprintf ( msg, sizeof ( msg ), "Defrag: wrote file %d/%d: %s.%s\n",
                           i + 1, files_count, f->filename, f->extension );
                progress_cb ( msg, cb_data );
            }
        }
    }

    /* Úklid: uvolnění všech dat */
    for ( i = 0; i < files_count; i++ ) {
        if ( files[i].data != NULL ) {
            free ( files[i].data );
            files[i].data = NULL;
        }
    }

    return err;
}


/* ================================================================
 * Verze knihovny
 * ================================================================ */


/**
 * @brief Vrátí řetězec s verzí knihovny mzdsk_cpm.
 * @return Statický řetězec s verzí (např. "2.0.0").
 */
const char* mzdsk_cpm_version ( void ) {
    return MZDSK_CPM_VERSION;
}
