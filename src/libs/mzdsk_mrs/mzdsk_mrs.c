/**
 * @file   mzdsk_mrs.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.4.0
 * @brief  Implementace knihovny pro práci se souborovým systémem MRS.
 *
 * Obsahuje inicializaci konfigurace z FAT, převod čísla bloku na
 * stopu/sektor, callback pro informace o sektorech, čtení/zápis
 * jednotlivých MRS bloků a defragmentaci.
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

#include "mzdsk_mrs.h"
#include "libs/mzdsk_global/mzdsk_global.h"


/**
 * @brief Callback pro získání informací o sektoru na dané stopě.
 *
 * MRS formát používá na stopě 1 (boot stopa) sektory o velikosti 256 B,
 * na všech ostatních stopách sektory o velikosti 512 B. Data jsou vždy
 * v normálním (neinvertovaném) formátu - inverze (XOR 0xFF) se řeší
 * na úrovni MRS souborového systému, ne na úrovni DSK driveru.
 *
 * @param[in] track      Číslo stopy.
 * @param[in] sector     Číslo sektoru (nepoužívá se pro rozhodování).
 * @param[in] user_data  Uživatelská data (nepoužívá se, může být NULL).
 * @return Kombinace MZDSK_MEDIUM_NORMAL | DSK_SECTOR_SIZE_256 pro stopu 1,
 *         MZDSK_MEDIUM_NORMAL | DSK_SECTOR_SIZE_512 pro ostatní stopy.
 */
uint8_t fsmrs_sector_info_cb ( uint16_t track, uint16_t sector, void *user_data ) {
    (void) sector;
    (void) user_data;
    if ( track == 1 ) return ( MZDSK_MEDIUM_NORMAL | DSK_SECTOR_SIZE_256 );
    return ( MZDSK_MEDIUM_NORMAL | DSK_SECTOR_SIZE_512 );
}


/**
 * @brief Převede číslo bloku na kombinovanou hodnotu stopa/sektor.
 *
 * MRS disk má 9 sektorů na stopu, číslovaných 1-9.
 * Výpočet:
 *   stopa  = blok / 9
 *   sektor = (blok % 9) + 1
 *
 * Výsledek je zakódován do 16bitové hodnoty:
 *   - horní bajt (bity 15-8): číslo stopy
 *   - dolní bajt (bity 7-0): číslo sektoru (1-9)
 *
 * @param[in] block Číslo bloku (0 .. FSMRS_COUNT_BLOCKS-1).
 * @return Kombinovaná hodnota stopa/sektor.
 */
uint16_t fsmrs_block2trsec ( uint16_t block ) {
    uint8_t track = (uint8_t) ( block / FSMRS_SECTORS_ON_TRACK );
    uint8_t sector = (uint8_t) ( ( block % FSMRS_SECTORS_ON_TRACK ) + 1 );
    return (uint16_t) ( ( track << 8 ) | sector );
}


/**
 * @brief Přečte nebo zapíše jeden MRS blok.
 *
 * Převede číslo bloku na stopu a sektor pomocí fsmrs_block2trsec()
 * a podle typu operace zavolá mzdsk_disc_read_sector() nebo
 * mzdsk_disc_write_sector().
 *
 * @param[in]     ioop  Typ operace (FSMRS_IOOP_READ nebo FSMRS_IOOP_WRITE).
 * @param[in,out] disc  Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param[in]     block Číslo bloku (0 .. FSMRS_COUNT_BLOCKS-1).
 * @param[in,out] dma   Buffer pro data. Musí mít alespoň FSMRS_SECTOR_SIZE bajtů.
 *                      Nesmí být NULL.
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání I/O.
 *
 * @pre disc != NULL, dma != NULL
 * @pre block < FSMRS_COUNT_BLOCKS
 */
en_MZDSK_RES fsmrs_rw_block ( en_FSMRS_IOOP ioop, st_MZDSK_DISC *disc, uint16_t block, void* dma ) {
    uint16_t trsec = fsmrs_block2trsec ( block );
    uint8_t track = (uint8_t) ( ( trsec >> 8 ) & 0xff );
    uint8_t sector = (uint8_t) ( trsec & 0xff );

    if ( FSMRS_IOOP_READ == ioop ) {
        return mzdsk_disc_read_sector ( disc, track, sector, dma );
    };
    return mzdsk_disc_write_sector ( disc, track, sector, dma );
}


/**
 * @brief Invertuje data v bufferu (XOR 0xFF na každý bajt).
 *
 * MRS ukládá veškerá data na disketě bitově invertovaná.
 * Tato funkce slouží k deinverzi přečtených dat.
 *
 * @param[in,out] buffer  Buffer s daty k invertování. Nesmí být NULL.
 * @param[in]     size    Počet bajtů k invertování.
 *
 * @pre buffer != NULL
 * @post Každý bajt v buffer[0..size-1] je XORován hodnotou 0xFF.
 */
static void fsmrs_invert_buffer ( uint8_t *buffer, uint16_t size ) {
    for ( uint16_t i = 0; i < size; i++ ) {
        buffer[i] ^= 0xFF;
    }
}


/**
 * @brief Načte adresář do config->dir[] bufferu a spočítá obsazené soubory.
 *
 * Přečte všechny dir_sectors sektorů adresáře, deinvertuje je (XOR 0xFF)
 * a uloží do config->dir[]. Současně spočítá obsazené soubory
 * (položky s fname[0] > 0x20) a uloží do config->used_files.
 *
 * @param[in,out] config  Ukazatel na konfiguraci s vyplněným dir_block,
 *                        dir_sectors a disc. Výstupně se vyplní dir[]
 *                        a used_files.
 *
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání I/O.
 *
 * @pre config != NULL, config->disc != NULL
 * @pre config->dir_sectors * FSMRS_SECTOR_SIZE <= FSMRS_MAX_DIR_BUFFER
 */
static en_MZDSK_RES fsmrs_load_directory ( st_FSMRS_CONFIG *config ) {
    config->used_files = 0;
    config->usable_files = 0;

    if ( (uint32_t) config->dir_sectors * FSMRS_SECTOR_SIZE > FSMRS_MAX_DIR_BUFFER ) {
        return MZDSK_RES_BUFFER_SMALL;
    }

    for ( uint16_t s = 0; s < config->dir_sectors; s++ ) {
        uint8_t *dst = config->dir + s * FSMRS_SECTOR_SIZE;
        en_MZDSK_RES res = fsmrs_read_block ( config->disc, config->dir_block + s, dst );
        if ( res != MZDSK_RES_OK ) return res;

        /* Invertuj data - MRS ukládá vše invertovaně */
        fsmrs_invert_buffer ( dst, FSMRS_SECTOR_SIZE );

        /* Projdi 16 položek v sektoru a spočítej aktivní a použitelné */
        for ( uint8_t i = 0; i < FSMRS_DIR_ITEMS_PER_SECTOR; i++ ) {
            st_FSMRS_DIR_ITEM *item = (st_FSMRS_DIR_ITEM *) ( dst + i * sizeof ( st_FSMRS_DIR_ITEM ) );
            if ( item->fname[0] != 0x1A ) {
                config->usable_files++;
            }
            if ( item->fname[0] > 0x20 && item->file_id >= 1 ) {
                config->used_files++;
            }
        }
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Vrátí ukazatel na adresářovou položku podle indexu.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @param[in] index   Index položky (0 .. max_files-1).
 *
 * @return Ukazatel na položku, nebo NULL při neplatném indexu.
 */
st_FSMRS_DIR_ITEM* fsmrs_get_dir_item ( st_FSMRS_CONFIG *config, uint16_t index ) {
    if ( config == NULL ) return NULL;
    if ( index >= config->max_files ) return NULL;
    return (st_FSMRS_DIR_ITEM *) ( config->dir + index * sizeof ( st_FSMRS_DIR_ITEM ) );
}


/**
 * @brief Určí, zda je adresářový slot aktivní.
 *
 * @param[in] item  Ukazatel na položku. Nesmí být NULL.
 * @return 1 pokud je slot aktivní (fname[0] > 0x20), jinak 0.
 */
int fsmrs_is_dir_item_active ( const st_FSMRS_DIR_ITEM *item ) {
    if ( item == NULL ) return 0;
    /* Aktivní soubor má fname[0] > 0x20 a file_id >= 1.
       file_id == 0 je speciální/systémová položka (např. "///////." na pozici 0). */
    return ( item->fname[0] > 0x20 && item->file_id >= 1 ) ? 1 : 0;
}


/**
 * @brief Inicializuje konfiguraci MRS souborového systému z obsahu FAT.
 *
 * Přečte FAT sektory z diskety a z jejich obsahu zjistí kompletní
 * rozložení disku. Postup:
 *
 * 1. Přečte první FAT sektor (blok fat_block).
 * 2. Ověří, že FAT[fat_block] == FSMRS_FAT_FAT.
 * 3. Spočítá počet FAT sektorů (souvislé bloky s hodnotou 0xFA).
 * 4. Přečte případné další FAT sektory.
 * 5. Spočítá počet adresářových sektorů (souvislé bloky s hodnotou 0xFD
 *    bezprostředně za FAT).
 * 6. Určí začátek datové oblasti.
 * 7. Spočítá volné a použitelné bloky.
 * 8. Přečte adresář a spočítá obsazené soubory.
 *
 * @param[in]  disc       Ukazatel na otevřenou diskovou strukturu. Nesmí být NULL.
 * @param[in]  fat_block  Číslo bloku, kde začíná FAT (typicky 36).
 * @param[out] config     Ukazatel na konfigurační strukturu k naplnění. Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu.
 * @return MZDSK_RES_INVALID_PARAM pokud disc nebo config je NULL,
 *         nebo fat_block je mimo rozsah.
 * @return MZDSK_RES_DSK_ERROR při chybě čtení z disku.
 * @return MZDSK_RES_FORMAT_ERROR pokud FAT na zadané pozici nemá
 *         očekávanou strukturu.
 *
 * @pre disc musí být úspěšně otevřený přes mzdsk_disc_open().
 * @post Při úspěchu je config kompletně inicializovaný a config->fat[]
 *       obsahuje deinvertovanou kopii FAT.
 */
en_MZDSK_RES fsmrs_init ( st_MZDSK_DISC *disc, uint16_t fat_block, st_FSMRS_CONFIG *config ) {

    if ( disc == NULL || config == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    if ( fat_block >= FSMRS_COUNT_BLOCKS ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Vynulujeme konfiguraci */
    memset ( config, 0, sizeof ( st_FSMRS_CONFIG ) );
    config->disc = disc;
    config->fat_block = fat_block;

    /*
     * Krok 1: Přečteme první FAT sektor a ověříme, že je platný.
     *
     * První FAT sektor pokrývá bloky 0 až 511. Obsahuje informace
     * o celé systémové oblasti i o začátku datové oblasti, takže
     * z něj zjistíme kolik FAT a DIR sektorů existuje.
     */
    uint8_t *fat_ptr = config->fat;

    en_MZDSK_RES res = fsmrs_read_block ( disc, fat_block, fat_ptr );
    if ( res != MZDSK_RES_OK ) return res;

    fsmrs_invert_buffer ( fat_ptr, FSMRS_SECTOR_SIZE );

    /* Ověříme, že blok na pozici fat_block je označený jako FAT */
    if ( fat_ptr[fat_block] != FSMRS_FAT_FAT ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    /*
     * Krok 2: Z prvního FAT sektoru zjistíme počet FAT a DIR sektorů.
     *
     * FAT sektory (0xFA) jsou bezprostředně za sebou od fat_block.
     * DIR sektory (0xFD) bezprostředně následují za FAT sektory.
     */
    uint16_t pos = fat_block;

    /* Spočítej FAT sektory */
    while ( pos < FSMRS_SECTOR_SIZE && fat_ptr[pos] == FSMRS_FAT_FAT ) {
        pos++;
    }
    config->fat_sectors = pos - fat_block;

    /* Spočítej DIR sektory */
    config->dir_block = pos;
    while ( pos < FSMRS_SECTOR_SIZE && fat_ptr[pos] == FSMRS_FAT_DIR ) {
        pos++;
    }
    config->dir_sectors = pos - config->dir_block;
    config->data_block = pos;

    /* Ověříme, že jsme našli alespoň 1 FAT a 1 DIR sektor */
    if ( config->fat_sectors == 0 || config->dir_sectors == 0 ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* Sanita: fat_sectors je odvozeno z diskových dat a může být až 476
     * (celá stopa FAT markerů). Bez této kontroly by cyklus níže zapsal
     * mimo config->fat[FSMRS_COUNT_BLOCKS] (1440 B) pro fat_sectors >= 3.
     * Audit H-7. */
    if ( (size_t) config->fat_sectors * FSMRS_SECTOR_SIZE > sizeof ( config->fat ) ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    /*
     * Krok 3: Přečteme zbývající FAT sektory (pokud existují).
     *
     * Každý FAT sektor pokrývá 512 bloků. Čteme další sektory
     * a jejich data ukládáme do config->fat[] za první sektor.
     */
    for ( uint16_t s = 1; s < config->fat_sectors; s++ ) {
        uint8_t *dst = fat_ptr + s * FSMRS_SECTOR_SIZE;
        res = fsmrs_read_block ( disc, fat_block + s, dst );
        if ( res != MZDSK_RES_OK ) return res;
        fsmrs_invert_buffer ( dst, FSMRS_SECTOR_SIZE );
    }

    /*
     * Krok 4: Spočítáme statistiky z FAT.
     *
     * Procházíme všech FSMRS_COUNT_BLOCKS položek (1440).
     * U bloků za koncem FAT dat (fat_sectors * 512) předpokládáme,
     * že jsou volné (0x00).
     */
    uint16_t fat_coverage = config->fat_sectors * FSMRS_SECTOR_SIZE;
    config->free_blocks = 0;
    config->total_blocks = 0;

    for ( uint16_t i = 0; i < FSMRS_COUNT_BLOCKS; i++ ) {
        uint8_t val;
        if ( i < fat_coverage ) {
            val = config->fat[i];
        } else {
            /* Blok je mimo pokrytí FAT - považujeme za volný */
            config->fat[i] = FSMRS_FAT_FREE;
            val = FSMRS_FAT_FREE;
        }

        /* Systémové bloky (0xFF) nepočítáme do celkového počtu */
        if ( val != FSMRS_FAT_SYSTEM ) {
            config->total_blocks++;
            if ( val == FSMRS_FAT_FREE ) {
                config->free_blocks++;
            }
        }
    }

    /*
     * Krok 5: Maximální počet souborů je dán velikostí adresáře.
     */
    /* Audit M-4: typ uint16_t (dříve uint8_t) poskytuje head room pro disky
       s větším počtem adresářových sektorů; FSMRS_DIR_ITEMS_PER_SECTOR = 16. */
    config->max_files = (uint16_t) ( config->dir_sectors * FSMRS_DIR_ITEMS_PER_SECTOR );

    /*
     * Krok 6: Načteme adresář do paměti a spočítáme obsazené soubory.
     */
    res = fsmrs_load_directory ( config );
    if ( res != MZDSK_RES_OK ) return res;

    return MZDSK_RES_OK;
}


/**
 * @brief Vytvoří čistý MRS souborový systém na disketě.
 *
 * Vytvoří FAT a prázdný adresář podle konvence původního MRS driveru
 * (Vlastimil Veselý, 1993). Nezasahuje do systémové oblasti ani do
 * datové oblasti disku.
 *
 * Viz dokumentace v hlavičce pro podrobný popis layoutu.
 *
 * @param[in] disc      Ukazatel na otevřenou diskovou strukturu v RW režimu.
 * @param[in] fat_block Číslo bloku, kde má začínat FAT.
 *
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání.
 *
 * @pre disc != NULL
 * @pre fat_block + 8 < FSMRS_COUNT_BLOCKS
 */
en_MZDSK_RES fsmrs_format_fs ( st_MZDSK_DISC *disc, uint16_t fat_block ) {

    if ( disc == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* 3 FAT sektory + 6 DIR sektorů = 9 bloků, musí se vejít */
    if ( fat_block + 9 > FSMRS_COUNT_BLOCKS ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    uint8_t sector[FSMRS_SECTOR_SIZE];
    /*
     * Buffer je zaokrouhlený nahoru na násobek sektoru (3 sektory =
     * 1536 B), aby memcpy ve Kroku 2 nečetlo mimo pole. Posledních
     * 96 bajtů (1440..1535) je padding - nereprezentují žádný blok
     * (FSMRS_COUNT_BLOCKS = 1440), ale musí být inicializovány.
     */
    uint8_t fat[3 * FSMRS_SECTOR_SIZE];
    en_MZDSK_RES res;

    /*
     * Krok 1: Sestavení obsahu FAT v paměti.
     *
     * Bloky 0 .. fat_block-1      : 0xFF (systémová oblast)
     * Bloky fat_block, +1         : 0xFA (FAT sektory)
     * Bloky fat_block+2 .. +8     : 0xFD (adresářové sektory,
     *                                     včetně 3. FAT sektoru)
     * Ostatní bloky               : 0x00 (volné, včetně paddingu
     *                                     1440..1535)
     */
    memset ( fat, FSMRS_FAT_FREE, sizeof ( fat ) );

    for ( uint16_t i = 0; i < fat_block; i++ ) {
        fat[i] = FSMRS_FAT_SYSTEM;
    }

    fat[fat_block]     = FSMRS_FAT_FAT;
    fat[fat_block + 1] = FSMRS_FAT_FAT;

    for ( uint16_t i = 2; i <= 8; i++ ) {
        fat[fat_block + i] = FSMRS_FAT_DIR;
    }

    /*
     * Krok 2: Zápis 3 FAT sektorů.
     *
     * První sektor obsahuje bajty 0-511 (s markery), druhý 512-1023
     * (samé nuly), třetí 1024-1439 (samé nuly, posledních 96 bajtů
     * je nepoužitý padding - FSMRS_COUNT_BLOCKS = 1440).
     */
    for ( uint16_t s = 0; s < 3; s++ ) {
        memcpy ( sector, fat + s * FSMRS_SECTOR_SIZE, FSMRS_SECTOR_SIZE );
        fsmrs_invert_buffer ( sector, FSMRS_SECTOR_SIZE );
        res = fsmrs_write_block ( disc, fat_block + s, sector );
        if ( res != MZDSK_RES_OK ) return res;
    }

    /*
     * Krok 3: Sestavení prázdného adresáře v paměti.
     *
     * 6 sektorů = 96 položek po 32 B. Prvních 89 slotů jsou prázdné
     * s postupně rostoucím file_id (1-89), jménem z mezer a inicia-
     * lizačním vzorem 0xCD490221 na offsetu 28-31. Posledních 7 slotů
     * je vyplněno hodnotou 0x1A (nedostupné).
     */
    uint8_t dir[6 * FSMRS_SECTOR_SIZE];
    memset ( dir, 0, sizeof ( dir ) );

    for ( uint16_t slot = 0; slot < 89; slot++ ) {
        st_FSMRS_DIR_ITEM *item = (st_FSMRS_DIR_ITEM *) ( dir + slot * sizeof ( st_FSMRS_DIR_ITEM ) );
        memset ( item->fname, 0x20, sizeof ( item->fname ) );
        memset ( item->ext, 0x20, sizeof ( item->ext ) );
        item->file_id = (uint8_t) ( slot + 1 );
        /* fstrt, bsize, reserved1, fexec, reserved2 zůstávají nulové (z memset) */
        item->reserved3[0] = 0xCD;
        item->reserved3[1] = 0x49;
        item->reserved3[2] = 0x02;
        item->reserved3[3] = 0x21;
    }

    /* Posledních 7 slotů: vše 0x1A (nedostupné) */
    memset ( dir + 89 * sizeof ( st_FSMRS_DIR_ITEM ),
             0x1A,
             7 * sizeof ( st_FSMRS_DIR_ITEM ) );

    /*
     * Krok 4: Zápis 6 adresářových sektorů.
     *
     * DIR začíná bezprostředně za 3 FAT sektory, tedy na bloku
     * fat_block + 3. Označení ve FAT (0xFD) je ale na blocích
     * fat_block + 2 .. fat_block + 8, takže náš tool s auto-detekcí
     * přečte 1. "DIR" sektor jako obsah 3. FAT sektoru (samé nuly =
     * 16 prázdných slotů). Tato kvirka je zachována kvůli kompatibilitě
     * s originálním MRS driverem.
     */
    for ( uint16_t s = 0; s < 6; s++ ) {
        memcpy ( sector, dir + s * FSMRS_SECTOR_SIZE, FSMRS_SECTOR_SIZE );
        fsmrs_invert_buffer ( sector, FSMRS_SECTOR_SIZE );
        res = fsmrs_write_block ( disc, fat_block + 3 + s, sector );
        if ( res != MZDSK_RES_OK ) return res;
    }

    return MZDSK_RES_OK;
}


/* =========================================================================
 * Alokační mapa
 * ========================================================================= */

/**
 * @brief Sestaví alokační mapu MRS disku z FAT tabulky.
 *
 * Iteruje přes všechny bloky a klasifikuje FAT hodnotu na odpovídající
 * typ bloku. Volitelně spočítá souhrnné statistiky (počty bloků
 * jednotlivých typů).
 *
 * @param[in]  config    Inicializovaná MRS konfigurace. Nesmí být NULL.
 * @param[out] map       Pole o velikosti alespoň FSMRS_COUNT_BLOCKS. Nesmí být NULL.
 * @param[in]  map_size  Velikost pole map (počet prvků).
 * @param[out] stats     Statistiky (může být NULL).
 *
 * @pre config musí být inicializovaný přes fsmrs_init().
 */
void fsmrs_get_block_map ( const st_FSMRS_CONFIG *config,
                            en_FSMRS_BLOCK_TYPE *map,
                            int map_size,
                            st_FSMRS_MAP_STATS *stats ) {

    int count = FSMRS_COUNT_BLOCKS;
    if ( map_size < count ) count = map_size;

    int fat_blocks = 0, dir_blocks = 0, sys_blocks = 0;
    int bad_blocks = 0, file_blocks = 0, free_blocks = 0;

    for ( int i = 0; i < count; i++ ) {
        uint8_t val = config->fat[i];
        en_FSMRS_BLOCK_TYPE type;

        switch ( val ) {
            case FSMRS_FAT_FREE:   type = FSMRS_BLOCK_FREE;   free_blocks++; break;
            case FSMRS_FAT_FAT:    type = FSMRS_BLOCK_FAT;    fat_blocks++; break;
            case FSMRS_FAT_DIR:    type = FSMRS_BLOCK_DIR;    dir_blocks++; break;
            case FSMRS_FAT_BAD:    type = FSMRS_BLOCK_BAD;    bad_blocks++; break;
            case FSMRS_FAT_SYSTEM: type = FSMRS_BLOCK_SYSTEM; sys_blocks++; break;
            default:               type = FSMRS_BLOCK_FILE;   file_blocks++; break;
        }

        map[i] = type;
    }

    if ( stats != NULL ) {
        stats->total_blocks = count;
        stats->fat_blocks = fat_blocks;
        stats->dir_blocks = dir_blocks;
        stats->sys_blocks = sys_blocks;
        stats->bad_blocks = bad_blocks;
        stats->file_blocks = file_blocks;
        stats->free_blocks = free_blocks;
    }
}


/* =========================================================================
 * Souborové operace
 * ========================================================================= */

/**
 * @brief Vyhledá soubor v adresáři podle jména (a volitelně přípony).
 *
 * Prohledá všechny položky v config->dir[]. Porovnání je case-sensitive
 * na celých 8 bajtech jména. Pokud ext není NULL, kontroluje se i přípona
 * (3 bajty).
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @param[in] fname   Jméno souboru (8 bajtů, doplněné mezerami). Nesmí být NULL.
 * @param[in] ext     Přípona (3 bajty), nebo NULL pro ignorování přípony.
 *
 * @return Ukazatel na nalezenou položku, nebo NULL.
 */
st_FSMRS_DIR_ITEM* fsmrs_search_file ( st_FSMRS_CONFIG *config, const uint8_t *fname, const uint8_t *ext ) {
    if ( config == NULL || fname == NULL ) return NULL;

    for ( uint16_t i = 0; i < config->max_files; i++ ) {
        st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, i );
        if ( item == NULL ) break;
        if ( !fsmrs_is_dir_item_active ( item ) ) continue;

        if ( memcmp ( item->fname, fname, 8 ) != 0 ) continue;
        if ( ext != NULL && memcmp ( item->ext, ext, 3 ) != 0 ) continue;

        return item;
    }
    return NULL;
}


/**
 * @brief Vyhledá aktivní soubor v adresáři podle čísla souboru (file_id).
 *
 * @param[in] config   Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @param[in] file_id  Číslo souboru (1-89).
 *
 * @return Ukazatel na nalezenou aktivní položku, nebo NULL.
 */
st_FSMRS_DIR_ITEM* fsmrs_search_file_by_id ( st_FSMRS_CONFIG *config, uint8_t file_id ) {
    if ( config == NULL || file_id == 0 ) return NULL;

    for ( uint16_t i = 0; i < config->max_files; i++ ) {
        st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( config, i );
        if ( item == NULL ) break;
        if ( !fsmrs_is_dir_item_active ( item ) ) continue;

        if ( item->file_id == file_id ) return item;
    }
    return NULL;
}


/**
 * @brief Přečte data souboru do paměťového bufferu.
 *
 * Projde FAT tabulku ve vzestupném pořadí bloků, vybere bloky
 * patřící souboru (FAT[blok] == file_id), přečte je a deinvertuje.
 *
 * @param[in]  config    Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @param[in]  item      Ukazatel na adresářovou položku souboru. Nesmí být NULL.
 * @param[out] dst       Výstupní buffer. Nesmí být NULL.
 * @param[in]  dst_size  Velikost bufferu v bajtech.
 *
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání.
 */
en_MZDSK_RES fsmrs_read_file ( st_FSMRS_CONFIG *config, const st_FSMRS_DIR_ITEM *item, void *dst, uint32_t dst_size ) {
    if ( config == NULL || item == NULL || dst == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    uint8_t fid = item->file_id;
    uint16_t expected_blocks = item->bsize;
    uint32_t expected_size = (uint32_t) expected_blocks * FSMRS_SECTOR_SIZE;

    if ( expected_blocks == 0 ) return MZDSK_RES_OK;
    if ( dst_size < expected_size ) return MZDSK_RES_BUFFER_SMALL;

    uint8_t *out = (uint8_t *) dst;
    uint16_t blocks_read = 0;
    uint16_t fat_coverage = (uint16_t) ( config->fat_sectors * FSMRS_SECTOR_SIZE );
    if ( fat_coverage > FSMRS_COUNT_BLOCKS ) fat_coverage = FSMRS_COUNT_BLOCKS;

    for ( uint16_t b = 0; b < fat_coverage && blocks_read < expected_blocks; b++ ) {
        if ( config->fat[b] != fid ) continue;

        uint8_t sector[FSMRS_SECTOR_SIZE];
        en_MZDSK_RES res = fsmrs_read_block ( config->disc, b, sector );
        if ( res != MZDSK_RES_OK ) return res;

        fsmrs_invert_buffer ( sector, FSMRS_SECTOR_SIZE );
        memcpy ( out + (uint32_t) blocks_read * FSMRS_SECTOR_SIZE, sector, FSMRS_SECTOR_SIZE );
        blocks_read++;
    }

    if ( blocks_read != expected_blocks ) return MZDSK_RES_FORMAT_ERROR;

    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše nový soubor na disk.
 *
 * Najde volný adresářový slot, alokuje bloky v datové oblasti,
 * zapíše invertovaná data a aktualizuje FAT i adresář na disku.
 *
 * @param[in,out] config    Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @param[in]     fname     Jméno souboru (8 bajtů). Nesmí být NULL.
 * @param[in]     ext       Přípona (3 bajty). Nesmí být NULL.
 * @param[in]     fstrt     Start adresa.
 * @param[in]     fexec     Exec adresa.
 * @param[in]     src       Zdrojová data. Může být NULL pokud src_size == 0.
 * @param[in]     src_size  Velikost dat v bajtech.
 *
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání.
 */
en_MZDSK_RES fsmrs_write_file ( st_FSMRS_CONFIG *config, const uint8_t *fname, const uint8_t *ext,
                                 uint16_t fstrt, uint16_t fexec, const void *src, uint32_t src_size ) {
    if ( config == NULL || fname == NULL || ext == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }
    if ( src == NULL && src_size > 0 ) return MZDSK_RES_INVALID_PARAM;

    /* Kontrola platnosti jména */
    if ( fname[0] <= 0x20 ) return MZDSK_RES_BAD_NAME;

    /* Kontrola unikátnosti - MRS kontroluje jen fname (8 znaků) */
    if ( fsmrs_search_file ( config, fname, NULL ) != NULL ) {
        return MZDSK_RES_FILE_EXISTS;
    }

    /* Výpočet potřebných bloků */
    uint16_t bsize;
    if ( src_size == 0 ) {
        bsize = 1;
    } else {
        bsize = (uint16_t) ( ( src_size + FSMRS_SECTOR_SIZE - 1 ) / FSMRS_SECTOR_SIZE );
    }

    /* Kontrola volného místa */
    if ( config->free_blocks < bsize ) return MZDSK_RES_DISK_FULL;

    /* Nalezení volného adresářového slotu */
    st_FSMRS_DIR_ITEM *free_slot = NULL;
    for ( uint16_t i = 0; i < config->max_files; i++ ) {
        st_FSMRS_DIR_ITEM *slot = fsmrs_get_dir_item ( config, i );
        if ( slot == NULL ) break;
        /* Volný slot: fname[0] == 0x20 (smazaný nebo prázdný) */
        if ( slot->fname[0] == 0x20 ) {
            free_slot = slot;
            break;
        }
    }
    if ( free_slot == NULL ) return MZDSK_RES_DIR_FULL;

    uint8_t fid = free_slot->file_id;

    /* Limit alokace na FAT pokrytí */
    uint16_t fat_coverage = (uint16_t) ( config->fat_sectors * FSMRS_SECTOR_SIZE );
    if ( fat_coverage > FSMRS_COUNT_BLOCKS ) fat_coverage = FSMRS_COUNT_BLOCKS;

    /* Transakční alokace: snapshot FAT před změnami, abychom při selhání
     * write_block uprostřed mohli rollbackovat. Bez toho by config->fat
     * obsahoval orphaned záznamy (bloky označené jako fid, ale dir o nich
     * neví), což vede k leak volného místa v in-memory config. Audit H-8. */
    uint8_t fat_snapshot[FSMRS_COUNT_BLOCKS];
    memcpy ( fat_snapshot, config->fat, sizeof ( config->fat ) );

    /* Alokace bloků a zápis dat */
    const uint8_t *data = (const uint8_t *) src;
    uint16_t blocks_written = 0;

    for ( uint16_t b = config->data_block; b < fat_coverage && blocks_written < bsize; b++ ) {
        if ( config->fat[b] != FSMRS_FAT_FREE ) continue;

        /* Příprava sektoru - kopie dat nebo nuly pro poslední blok */
        uint8_t sector[FSMRS_SECTOR_SIZE];
        uint32_t offset = (uint32_t) blocks_written * FSMRS_SECTOR_SIZE;
        uint32_t remaining = ( src_size > offset ) ? ( src_size - offset ) : 0;

        if ( remaining >= FSMRS_SECTOR_SIZE ) {
            memcpy ( sector, data + offset, FSMRS_SECTOR_SIZE );
        } else {
            memset ( sector, 0, FSMRS_SECTOR_SIZE );
            if ( remaining > 0 ) {
                memcpy ( sector, data + offset, remaining );
            }
        }

        /* Inverze a zápis na disk */
        fsmrs_invert_buffer ( sector, FSMRS_SECTOR_SIZE );
        en_MZDSK_RES res = fsmrs_write_block ( config->disc, b, sector );
        if ( res != MZDSK_RES_OK ) {
            /* Rollback FAT snapshot: zapsané bloky na disku zůstanou jako
             * "ghost data", ale config->fat je konzistentní s původním stavem.
             * Následná operace tyto bloky bude správně považovat za volné. */
            memcpy ( config->fat, fat_snapshot, sizeof ( config->fat ) );
            return res;
        }

        config->fat[b] = fid;
        blocks_written++;
    }

    if ( blocks_written != bsize ) {
        memcpy ( config->fat, fat_snapshot, sizeof ( config->fat ) );
        return MZDSK_RES_DISK_FULL;
    }

    /* Aktualizace adresářové položky */
    memcpy ( free_slot->fname, fname, 8 );
    memcpy ( free_slot->ext, ext, 3 );
    free_slot->fstrt = fstrt;
    free_slot->fexec = fexec;
    free_slot->bsize = bsize;
    memset ( free_slot->reserved1, 0, sizeof ( free_slot->reserved1 ) );
    memset ( free_slot->reserved2, 0, sizeof ( free_slot->reserved2 ) );

    /* Aktualizace statistik */
    config->free_blocks -= bsize;
    config->used_files++;

    /* Zápis FAT a adresáře na disk */
    en_MZDSK_RES res = fsmrs_flush_fat ( config );
    if ( res != MZDSK_RES_OK ) return res;

    return fsmrs_flush_dir ( config );
}


/**
 * @brief Smaže soubor z disku.
 *
 * Uvolní bloky ve FAT, vymaže adresářovou položku a zapíše
 * změny na disk.
 *
 * @param[in,out] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @param[in,out] item    Ukazatel na adresářovou položku. Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání.
 */
en_MZDSK_RES fsmrs_delete_file ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item ) {
    if ( config == NULL || item == NULL ) return MZDSK_RES_INVALID_PARAM;

    uint8_t fid = item->file_id;
    uint16_t freed_blocks = 0;

    /* Uvolnění bloků ve FAT */
    uint16_t fat_coverage = (uint16_t) ( config->fat_sectors * FSMRS_SECTOR_SIZE );
    if ( fat_coverage > FSMRS_COUNT_BLOCKS ) fat_coverage = FSMRS_COUNT_BLOCKS;

    for ( uint16_t b = 0; b < fat_coverage; b++ ) {
        if ( config->fat[b] == fid ) {
            config->fat[b] = FSMRS_FAT_FREE;
            freed_blocks++;
        }
    }

    /* Vymazání adresářové položky - fname mezerami, vynulování metadat */
    memset ( item->fname, 0x20, sizeof ( item->fname ) );
    memset ( item->ext, 0x20, sizeof ( item->ext ) );
    item->fstrt = 0;
    item->fexec = 0;
    item->bsize = 0;
    memset ( item->reserved1, 0, sizeof ( item->reserved1 ) );
    memset ( item->reserved2, 0, sizeof ( item->reserved2 ) );
    /* file_id a reserved3 (init vzor) zůstávají beze změny */

    /* Aktualizace statistik */
    config->free_blocks += freed_blocks;
    if ( config->used_files > 0 ) config->used_files--;

    /* Zápis FAT a adresáře na disk */
    en_MZDSK_RES res = fsmrs_flush_fat ( config );
    if ( res != MZDSK_RES_OK ) return res;

    return fsmrs_flush_dir ( config );
}


/**
 * @brief Přejmenuje soubor (změní jméno a/nebo příponu).
 *
 * Ověří unikátnost nového jména, aktualizuje adresářovou položku
 * a zapíše adresář na disk.
 *
 * @param[in,out] config     Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @param[in,out] item       Ukazatel na adresářovou položku. Nesmí být NULL.
 * @param[in]     new_fname  Nové jméno (8 bajtů, doplněné mezerami). Nesmí být NULL.
 * @param[in]     new_ext    Nová přípona (3 bajty), nebo NULL pro zachování stávající.
 *
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání.
 */
en_MZDSK_RES fsmrs_rename_file ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item,
                                  const uint8_t *new_fname, const uint8_t *new_ext ) {
    if ( config == NULL || item == NULL || new_fname == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }
    if ( new_fname[0] <= 0x20 ) return MZDSK_RES_BAD_NAME;

    /* Kontrola unikátnosti nového jména */
    st_FSMRS_DIR_ITEM *existing = fsmrs_search_file ( config, new_fname, NULL );
    if ( existing != NULL && existing != item ) {
        return MZDSK_RES_FILE_EXISTS;
    }

    /* Aktualizace jména a přípony */
    memcpy ( item->fname, new_fname, 8 );
    if ( new_ext != NULL ) {
        memcpy ( item->ext, new_ext, 3 );
    }

    return fsmrs_flush_dir ( config );
}


en_MZDSK_RES fsmrs_set_addr ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item,
                                uint16_t fstrt, uint16_t fexec ) {
    if ( config == NULL || item == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }
    item->fstrt = fstrt;
    item->fexec = fexec;
    return fsmrs_flush_dir ( config );
}


/* =========================================================================
 * Defragmentace
 * ========================================================================= */

/**
 * @brief Interní struktura pro uchování metadat a dat jednoho souboru
 *        během defragmentace.
 *
 * Používá se jako dočasný úložný prostor pro soubory načtené z disku
 * před formátem a zápisem zpět v sekvenčním pořadí.
 *
 * @invariant Pokud data != NULL, pak data_size == bsize * FSMRS_SECTOR_SIZE.
 */
typedef struct st_FSMRS_DEFRAG_FILE {
    uint8_t  fname[8];     /**< Jméno souboru (8 bajtů, space-padded) */
    uint8_t  ext[3];       /**< Přípona souboru (3 bajty) */
    uint16_t fstrt;        /**< Start adresa */
    uint16_t fexec;        /**< Exec adresa */
    uint16_t bsize;        /**< Počet bloků */
    uint8_t  *data;        /**< Ukazatel na data souboru (vlastněný, musí být uvolněn) */
    uint32_t data_size;    /**< Velikost dat v bajtech (bsize * FSMRS_SECTOR_SIZE) */
} st_FSMRS_DEFRAG_FILE;


/**
 * @brief Uvolní paměť alokovanou pro pole defrag souborů.
 *
 * @param[in] files  Pole defrag souborů (může být NULL).
 * @param[in] count  Počet prvků v poli.
 */
static void defrag_free_files ( st_FSMRS_DEFRAG_FILE *files, int count ) {
    if ( files == NULL ) return;
    for ( int i = 0; i < count; i++ ) {
        free ( files[i].data );
    }
    free ( files );
}


en_MZDSK_RES fsmrs_defrag ( st_MZDSK_DISC *disc,
                              uint16_t fat_block,
                              fsmrs_defrag_cb_t progress_cb,
                              void *cb_data ) {

    if ( disc == NULL ) return MZDSK_RES_INVALID_PARAM;

    en_MZDSK_RES res;
    st_FSMRS_CONFIG config;
    char msg[128];

    /* Krok 1: Inicializace konfigurace z aktuálního stavu disku */
    res = fsmrs_init ( disc, fat_block, &config );
    if ( res != MZDSK_RES_OK ) return res;

    /*
     * Rozšíření FAT pokrytí o "skrytý" 3. FAT sektor.
     *
     * Originální MRS driver zapisuje 3 FAT sektory, ale 3. sektor
     * označuje ve FAT jako DIR (0xFD). Naše knihovna auto-detekuje
     * pouze 2 FAT sektory (bloky označené 0xFA), takže bloky
     * 1024-1439 jsou považovány za volné, i když mohou obsahovat
     * data souborů.
     *
     * Pro defragmentaci potřebujeme kompletní FAT pokrytí, aby
     * fsmrs_read_file mohla najít všechny bloky všech souborů.
     */
    if ( config.fat_sectors == 2 && config.dir_block == fat_block + 2 ) {
        uint8_t extra_fat[FSMRS_SECTOR_SIZE];
        res = fsmrs_read_block ( disc, fat_block + 2, extra_fat );
        if ( res == MZDSK_RES_OK ) {
            fsmrs_invert_buffer ( extra_fat, FSMRS_SECTOR_SIZE );
            /* Zkopíruj jen relevantní část: bloky 1024 .. FSMRS_COUNT_BLOCKS-1 */
            uint16_t extra_count = FSMRS_COUNT_BLOCKS - 1024;
            memcpy ( &config.fat[1024], extra_fat, extra_count );
            /* Dočasně zvýšíme fat_sectors, aby fsmrs_read_file viděla celý rozsah */
            config.fat_sectors = 3;
        }
    }

    if ( config.used_files == 0 ) {
        if ( progress_cb ) progress_cb ( "Defrag: no files on disk, nothing to do\n", cb_data );
        return MZDSK_RES_OK;
    }

    /* Krok 2: Alokace pole pro uložení souborů */
    st_FSMRS_DEFRAG_FILE *files = calloc ( config.used_files, sizeof ( st_FSMRS_DEFRAG_FILE ) );
    if ( files == NULL ) return MZDSK_RES_UNKNOWN_ERROR;

    int files_count = 0;

    /* Krok 3: Načtení všech aktivních souborů do paměti */
    if ( progress_cb ) progress_cb ( "Defrag: reading files\n", cb_data );

    for ( uint16_t i = 0; i < config.max_files; i++ ) {
        st_FSMRS_DIR_ITEM *item = fsmrs_get_dir_item ( &config, i );
        if ( item == NULL ) break;
        if ( !fsmrs_is_dir_item_active ( item ) ) continue;

        st_FSMRS_DEFRAG_FILE *f = &files[files_count];

        /* Uložení metadat */
        memcpy ( f->fname, item->fname, 8 );
        memcpy ( f->ext, item->ext, 3 );
        f->fstrt = item->fstrt;
        f->fexec = item->fexec;
        f->bsize = item->bsize;
        f->data_size = (uint32_t) item->bsize * FSMRS_SECTOR_SIZE;

        /* Alokace a čtení dat */
        f->data = malloc ( f->data_size );
        if ( f->data == NULL ) {
            defrag_free_files ( files, files_count );
            return MZDSK_RES_UNKNOWN_ERROR;
        }

        res = fsmrs_read_file ( &config, item, f->data, f->data_size );
        if ( res != MZDSK_RES_OK ) {
            free ( f->data );
            f->data = NULL;
            defrag_free_files ( files, files_count );
            return res;
        }

        if ( progress_cb ) {
            /* Sestavení jména pro výpis */
            int nlen = 8;
            while ( nlen > 0 && f->fname[nlen - 1] == 0x20 ) nlen--;
            int elen = 3;
            while ( elen > 0 && f->ext[elen - 1] == 0x20 ) elen--;

            char name[13];
            int pos = 0;
            for ( int j = 0; j < nlen; j++ ) name[pos++] = (char) f->fname[j];
            if ( elen > 0 ) {
                name[pos++] = '.';
                for ( int j = 0; j < elen; j++ ) name[pos++] = (char) f->ext[j];
            }
            name[pos] = '\0';

            snprintf ( msg, sizeof ( msg ), "  Read: %s (%u blocks)\n", name, f->bsize );
            progress_cb ( msg, cb_data );
        }

        files_count++;
    }

    /* Krok 4: Formát FAT a adresáře */
    if ( progress_cb ) progress_cb ( "Defrag: formatting disk\n", cb_data );

    res = fsmrs_format_fs ( disc, fat_block );
    if ( res != MZDSK_RES_OK ) {
        defrag_free_files ( files, files_count );
        return res;
    }

    /* Krok 5: Re-inicializace konfigurace z čerstvě naformátovaného disku */
    res = fsmrs_init ( disc, fat_block, &config );
    if ( res != MZDSK_RES_OK ) {
        defrag_free_files ( files, files_count );
        return res;
    }

    /* Krok 6: Sekvenční zápis všech souborů */
    if ( progress_cb ) progress_cb ( "Defrag: writing files\n", cb_data );

    for ( int i = 0; i < files_count; i++ ) {
        st_FSMRS_DEFRAG_FILE *f = &files[i];

        res = fsmrs_write_file ( &config, f->fname, f->ext, f->fstrt, f->fexec,
                                  f->data, f->data_size );
        if ( res != MZDSK_RES_OK ) {
            defrag_free_files ( files, files_count );
            return res;
        }

        if ( progress_cb ) {
            int nlen = 8;
            while ( nlen > 0 && f->fname[nlen - 1] == 0x20 ) nlen--;
            int elen = 3;
            while ( elen > 0 && f->ext[elen - 1] == 0x20 ) elen--;

            char name[13];
            int pos = 0;
            for ( int j = 0; j < nlen; j++ ) name[pos++] = (char) f->fname[j];
            if ( elen > 0 ) {
                name[pos++] = '.';
                for ( int j = 0; j < elen; j++ ) name[pos++] = (char) f->ext[j];
            }
            name[pos] = '\0';

            snprintf ( msg, sizeof ( msg ), "  Written: %s (%u blocks)\n", name, f->bsize );
            progress_cb ( msg, cb_data );
        }
    }

    defrag_free_files ( files, files_count );

    if ( progress_cb ) {
        snprintf ( msg, sizeof ( msg ), "Defrag: done (%d files defragmented)\n", files_count );
        progress_cb ( msg, cb_data );
    }

    return MZDSK_RES_OK;
}


/* =========================================================================
 * Zápis FAT a adresáře na disk
 * ========================================================================= */

/**
 * @brief Zapíše FAT tabulku z paměti na disk.
 *
 * Invertuje obsah config->fat[] po sektorech (512 B) a zapíše
 * na disk od bloku config->fat_block.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání.
 */
en_MZDSK_RES fsmrs_flush_fat ( st_FSMRS_CONFIG *config ) {
    if ( config == NULL ) return MZDSK_RES_INVALID_PARAM;

    uint8_t sector[FSMRS_SECTOR_SIZE];

    for ( uint16_t s = 0; s < config->fat_sectors; s++ ) {
        memcpy ( sector, config->fat + (uint32_t) s * FSMRS_SECTOR_SIZE, FSMRS_SECTOR_SIZE );
        fsmrs_invert_buffer ( sector, FSMRS_SECTOR_SIZE );
        en_MZDSK_RES res = fsmrs_write_block ( config->disc, config->fat_block + s, sector );
        if ( res != MZDSK_RES_OK ) return res;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše adresář z paměti na disk.
 *
 * Invertuje obsah config->dir[] po sektorech (512 B) a zapíše
 * na disk od bloku config->dir_block.
 *
 * @param[in] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
 * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání.
 */
en_MZDSK_RES fsmrs_flush_dir ( st_FSMRS_CONFIG *config ) {
    if ( config == NULL ) return MZDSK_RES_INVALID_PARAM;

    uint8_t sector[FSMRS_SECTOR_SIZE];

    for ( uint16_t s = 0; s < config->dir_sectors; s++ ) {
        memcpy ( sector, config->dir + (uint32_t) s * FSMRS_SECTOR_SIZE, FSMRS_SECTOR_SIZE );
        fsmrs_invert_buffer ( sector, FSMRS_SECTOR_SIZE );
        en_MZDSK_RES res = fsmrs_write_block ( config->disc, config->dir_block + s, sector );
        if ( res != MZDSK_RES_OK ) return res;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Vrátí řetězec s verzí knihovny mzdsk_mrs.
 * @return Statický řetězec s verzí.
 */
const char* mzdsk_mrs_version ( void ) {
    return MZDSK_MRS_VERSION;
}
