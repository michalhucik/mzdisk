/**
 * @file   mzdsk_ipldisk.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Veřejné API knihovny mzdsk_ipldisk - souborový systém FSMZ (MZ-BASIC disk).
 *
 * Knihovna implementuje nízkoúrovňové operace nad souborovým systémem FSMZ,
 * který používají počítače Sharp MZ-800 s diskovým BASICem a programem IPLDISK.
 *
 * Disk má jednotný fyzický formát: 16 sektorů po 256 bajtech na stopu.
 * Logický formát používá alokační bloky, kde jeden blok odpovídá jednomu
 * fyzickému sektoru. Číslování alokačních bloků začíná na stopě 1, sektoru 1.
 *
 * Struktura disku:
 * - Alokační blok 0: bootstrap (IPLPRO) hlavička
 * - Alokační bloky 1-14: volné nebo krátký bootstrap loader
 * - Alokační blok 15: informace o disku (DINFO) s bitmapou alokace
 * - Alokační bloky 16-23 (resp. 16-31 u IPLDISK): adresář
 * - Od bloku "farea" (typicky 48): souborová data
 *
 * IPLDISK rozšiřuje adresář na 16 sektorů (bloky 16-31),
 * čímž zvyšuje maximální počet položek ze 63 na 127.
 *
 * Port z fstool/fsmz. Interní FSMZ_ prefixy zachovány.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2015-2026 Michal Hucik <hucik@ordoz.com>
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


#ifndef MZDSK_IPLDISK_H
#define MZDSK_IPLDISK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/dsk/dsk.h"
#include "libs/mzdsk_global/mzdsk_global.h"


    /* =====================================================================
     * Konstanty formátu FSMZ
     * ===================================================================== */

    /** @brief Kódovaná velikost FSMZ sektoru pro DSK vrstvu (256 bajtů). */
#define FSMZ_DSK_SECTOR_SSIZE DSK_SECTOR_SIZE_256

    /** @brief Velikost jednoho FSMZ sektoru (alokačního bloku) v bajtech. */
#define FSMZ_SECTOR_SIZE 0x0100

    /** @brief Počet sektorů na jedné stopě FSMZ disku. */
#define FSMZ_SECTORS_ON_TRACK 16

    /** @brief Číslo alokačního bloku obsahujícího IPLPRO hlavičku. */
#define FSMZ_ALOCBLOCK_IPLPRO   0

    /** @brief Číslo alokačního bloku obsahujícího informace o disku (DINFO). */
#define FSMZ_ALOCBLOCK_DINFO    15

    /** @brief Číslo prvního alokačního bloku adresáře. */
#define FSMZ_ALOCBLOCK_DIR      16

    /** @brief Počet položek adresáře v jednom alokačním bloku (256 / 32 = 8). */
#define FSMZ_DIRITEMS_PER_BLOCK 8

    /** @brief Délka jména souboru v IPLPRO bloku (kratší než standardní MZF). */
#define FSMZ_IPLFNAME_LENGTH  13

    /** @brief Délka jména souboru v adresáři (standardní MZF délka). */
#define FSMZ_FNAME_LENGTH  17

    /** @brief Délka komentáře v IPLPRO bloku. */
#define FSMZ_IPLCMNT_LENGTH  224

    /** @brief Terminátorový bajt jména souboru (CR). */
#define FSMZ_FNAME_TERMINATOR 0x0d

    /** @brief Výchozí číslo alokačního bloku začátku souborové oblasti (farea). */
#define FSMZ_DEFAULT_FAREA_BLOCK  0x30

    /** @brief Velikost bitmapy souborové oblasti v DINFO bloku. */
#define FSMZ_FAREA_BITMAP_SIZE  250

    /**
     * @brief Maximální počet položek adresáře pro standardní MZ-800 Disc BASIC.
     *
     * Adresář zabírá 8 sektorů (bloky 16-23), první položka (32 bajtů) je
     * rezervovaná, takže zbývá 63 použitelných položek.
     */
#define FSMZ_MAX_DIR_ITEMS 63

    /**
     * @brief Maximální počet položek adresáře pro IPLDISK.
     *
     * IPLDISK rozšiřuje adresář na 16 sektorů (bloky 16-31), čímž
     * zvyšuje kapacitu na 127 položek. Tento rozšířený adresář je
     * z BASICu čitelný pouze z části.
     */
#define FSMZ_IPLDISK_MAX_DIR_ITEMS 127

    /** @brief Hodnota pro resetování bitu v DINFO bitmapě (blok volný). */
#define FSMZ_DINFO_BITMAP_RESET     0

    /** @brief Hodnota pro nastavení bitu v DINFO bitmapě (blok obsazený). */
#define FSMZ_DINFO_BITMAP_SET       1

    /** @brief Hodnota volume_number pro bootovatelný (master) disk. */
#define FSMZ_DINFO_MASTER   0

    /** @brief Hodnota volume_number pro nebootovatelný (slave) disk. */
#define FSMZ_DINFO_SLAVE   1


    /* =====================================================================
     * Datové struktury
     * ===================================================================== */

    /**
     * @brief Sektor popisující bootstrap (IPLPRO) - alokační blok 0.
     *
     * Obsahuje hlavičku bootstrap programu s typem souboru (vždy 0x03),
     * klíčovým slovem "IPLPRO", jménem, adresami a číslem počátečního
     * alokačního bloku tělesa bootstrap programu.
     *
     * @note Jméno souboru (fname) je kratší než standardní MZF jméno -
     *       má pouze FSMZ_IPLFNAME_LENGTH (13) bajtů, zakončeno 0x0d.
     *
     * @invariant ftype musí být vždy 0x03.
     * @invariant iplpro musí obsahovat "IPLPRO" (6 bajtů).
     */
    typedef struct st_FSMZ_IPLPRO_BLOCK {
        uint8_t ftype;                              /**< Typ souboru - vždy 0x03 */
        uint8_t iplpro[6];                          /**< Klíčové slovo "IPLPRO" */
        uint8_t fname [ FSMZ_IPLFNAME_LENGTH ];     /**< Jméno bootstrap programu, zakončeno 0x0d */
        uint16_t fsize;                             /**< Velikost bootstrap programu v bajtech */
        uint16_t fstrt;                             /**< Startovací adresa v paměti Z80 */
        uint16_t fexec;                             /**< Spouštěcí adresa v paměti Z80 */
        uint8_t unused[4];                          /**< Rezervované bajty (0x00) */
        uint16_t block;                             /**< Počáteční alokační blok bootstrap programu */
        uint8_t cmnt[FSMZ_IPLCMNT_LENGTH];          /**< Komentář - některé diskety ho obsahují, jinak nepoužito */
    } st_FSMZ_IPLPRO_BLOCK;


    /**
     * @brief Sektor s informacemi o disku (DINFO) - alokační blok 15.
     *
     * Obsahuje číslo svazku (volume), začátek souborové oblasti (farea),
     * počet obsazených bloků, celkový počet bloků a bitmapu alokace.
     *
     * Bitmapa nepopisuje bloky od začátku disku, ale začíná až od bloku
     * definovaného hodnotou farea. Maximální velikost souborové oblasti
     * je tedy 250 * 8 = 2000 alokačních bloků.
     *
     * @invariant used <= blocks
     * @invariant farea >= FSMZ_ALOCBLOCK_DIR + 8 (minimálně za adresářem)
     */
    typedef struct st_FSMZ_DINFO_BLOCK {
        uint8_t volume_number;                      /**< Master = 0 (bootovatelný), slave > 0 */
        uint8_t farea;                              /**< Alokační blok začátku souborové oblasti (typicky 0x30) */
        uint16_t used;                              /**< Počet obsazených bloků na celém disku */
        uint16_t blocks;                            /**< Celkový počet bloků na disku - 1 */
        uint8_t map[FSMZ_FAREA_BITMAP_SIZE];        /**< Bitmapa souborové oblasti - obsazený blok = 1 */
    } st_FSMZ_DINFO_BLOCK;


    /**
     * @brief Položka adresáře FSMZ (32 bajtů).
     *
     * Každá položka popisuje jeden soubor na disku. Pokud je ftype == 0x00,
     * položka se považuje za smazanou.
     *
     * Souborové typy:
     * - 0x00: smazaná položka
     * - 0x01: binární program v nativním tvaru
     * - 0x02: bootstrap
     * - 0x03: BASIC data
     * - 0x05: BASIC program
     *
     * @invariant Jméno souboru je zakončeno 0x0d.
     * @invariant sizeof(st_FSMZ_DIR_ITEM) == 32
     */
    typedef struct st_FSMZ_DIR_ITEM {
        uint8_t ftype;                              /**< Souborový typ (0x00 = smazaná položka) */
        uint8_t fname [ FSMZ_FNAME_LENGTH ];        /**< Jméno souboru, zakončeno 0x0d */
        uint8_t locked;                             /**< Příznak uzamčení souboru */
        uint8_t unused1;                            /**< Rezervovaný bajt */
        uint16_t fsize;                             /**< Velikost souboru v bajtech */
        uint16_t fstrt;                             /**< Startovací adresa v paměti Z80 */
        uint16_t fexec;                             /**< Spouštěcí adresa v paměti Z80 */
        uint8_t unused2[4];                         /**< Rezervované bajty */
        uint16_t block;                             /**< Počáteční alokační blok dat souboru */
    } st_FSMZ_DIR_ITEM;


    /**
     * @brief Jeden sektor (blok) adresáře (256 bajtů).
     *
     * Obsahuje FSMZ_DIRITEMS_PER_BLOCK (8) položek adresáře.
     *
     * Adresář zabírá alokační bloky 16-23 (standardní BASIC) nebo
     * 16-31 (IPLDISK rozšířený formát).
     */
    typedef struct st_FSMZ_DIR_BLOCK {
        st_FSMZ_DIR_ITEM item [ FSMZ_DIRITEMS_PER_BLOCK ]; /**< Pole položek adresáře */
    } st_FSMZ_DIR_BLOCK;


    /**
     * @brief Struktura pro sekvenční čtení adresáře.
     *
     * Udržuje aktuální pozici v adresáři a ukazatel na načtený blok.
     * Používá se při iteraci přes položky adresáře.
     *
     * @note Ukazatel dir_bl typicky ukazuje do cache bufferu disku
     *       (disc->cache), takže jeho platnost závisí na tom, zda
     *       nedošlo k přečtení jiného sektoru.
     */
    typedef struct st_FSMZ_DIR {
        int8_t position;                            /**< Aktuální pozice v adresáři (index položky) */
        st_FSMZ_DIR_BLOCK *dir_bl;                  /**< Ukazatel na aktuálně načtený blok adresáře */
    } st_FSMZ_DIR;


    /**
     * @brief Směr I/O operace nad alokačním blokem.
     */
    typedef enum en_FSMZ_IOOP {
        FSMZ_IOOP_READ = (uint8_t) 0,              /**< Operace čtení */
        FSMZ_IOOP_WRITE = (uint8_t) 1,              /**< Operace zápisu */
    } en_FSMZ_IOOP;


    /* =====================================================================
     * Veřejné funkce
     * ===================================================================== */

    /**
     * @brief Přepočet FSMZ alokačního bloku na logickou stopu a fyzický sektor.
     *
     * FSMZ formát používá obrácenou logiku stran - liché logické stopy
     * se mapují na sudé fyzické a naopak. Stopa je v horním bajtu
     * výsledku, sektor ve spodním.
     *
     * @param block Číslo alokačního bloku.
     * @return Horní bajt = stopa, spodní bajt = sektor (1-based).
     */
    extern uint16_t fsmz_block2trsec ( uint16_t block );

    /**
     * @brief Přepočet stopy a sektoru na číslo alokačního bloku.
     *
     * @param track Číslo stopy.
     * @param sector Číslo sektoru (1-based).
     * @return Číslo alokačního bloku.
     */
    extern uint16_t fsmz_trsec2block ( uint8_t track, uint8_t sector );

    /**
     * @brief Přečte nebo zapíše jeden FSMZ alokační blok.
     *
     * Interně převede číslo bloku na stopu/sektor a zavolá
     * mzdsk_disc_read_sector resp. mzdsk_disc_write_sector.
     *
     * @param ioop Směr operace (FSMZ_IOOP_READ nebo FSMZ_IOOP_WRITE).
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param block Číslo alokačního bloku.
     * @param dma Datový buffer (min. FSMZ_SECTOR_SIZE bajtů).
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_rw_block ( en_FSMZ_IOOP ioop, st_MZDSK_DISC *disc, uint16_t block, void* dma );

    /**
     * @brief Makro pro čtení jednoho alokačního bloku.
     * @param disc Ukazatel na diskovou strukturu.
     * @param block Číslo alokačního bloku.
     * @param dma Cílový buffer.
     */
#define fsmz_read_block( disc, block, dma) fsmz_rw_block ( FSMZ_IOOP_READ, disc, block, dma )

    /**
     * @brief Makro pro zápis jednoho alokačního bloku.
     * @param disc Ukazatel na diskovou strukturu.
     * @param block Číslo alokačního bloku.
     * @param dma Zdrojový buffer.
     */
#define fsmz_write_block( disc, block, dma) fsmz_rw_block ( FSMZ_IOOP_WRITE, disc, block, dma )

    /**
     * @brief Přečte IPLPRO blok (alokační blok 0) a provede endianity korekci.
     *
     * Vícebajtová pole (fsize, fstrt, fexec, block) se automaticky
     * konvertují z little-endian na nativní byte order hostitele.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param iplpro Výstupní buffer pro IPLPRO blok.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_read_iplpro ( st_MZDSK_DISC *disc, st_FSMZ_IPLPRO_BLOCK *iplpro );

    /**
     * @brief Zapíše IPLPRO blok do alokačního bloku 0 s endianity korekcí.
     *
     * Vstupní data se před zápisem konvertují do little-endian,
     * vstupní struktura se nemodifikuje (kopíruje se interně).
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param iplpro Ukazatel na IPLPRO blok k zápisu.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fs_mz_write_iplpro ( st_MZDSK_DISC *disc, st_FSMZ_IPLPRO_BLOCK *iplpro );

    /**
     * @brief Přečte DINFO blok (alokační blok 15) a provede endianity korekci.
     *
     * Vícebajtová pole (used, blocks) se automaticky konvertují
     * z little-endian na nativní byte order hostitele.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param dinfo Výstupní buffer pro DINFO blok.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_read_dinfo ( st_MZDSK_DISC *disc, st_FSMZ_DINFO_BLOCK *dinfo );

    /**
     * @brief Zapíše DINFO blok do alokačního bloku 15 s endianity korekcí.
     *
     * Vstupní data se před zápisem konvertují do little-endian,
     * vstupní struktura se nemodifikuje (kopíruje se interně).
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param dinfo Ukazatel na DINFO blok k zápisu.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_write_dinfo ( st_MZDSK_DISC *disc, st_FSMZ_DINFO_BLOCK *dinfo );

    /**
     * @brief Otevře adresář - načte první blok (alokační blok 16).
     *
     * Nastaví pozici na 1 (přeskočí rezervovanou první položku)
     * a načte první blok adresáře do disc->cache.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param dir Výstupní struktura adresáře.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_open_dir ( st_MZDSK_DISC *disc, st_FSMZ_DIR * dir );

    /**
     * @brief Načte blok adresáře obsahující zadanou pozici položky.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param dir Struktura adresáře (bude aktualizována).
     * @param dir_position Index položky adresáře.
     * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
     * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_NFND pokud pozice
     *         přesahuje limit, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_read_dirblock_with_diritem_position ( st_MZDSK_DISC *disc, st_FSMZ_DIR *dir, uint8_t dir_position, uint8_t fsmz_dir_items );

    /**
     * @brief Sekvenční čtení další položky adresáře.
     *
     * Při dosažení hranice alokačního bloku automaticky načte další blok.
     * Pozice v dir se po každém volání inkrementuje.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param dir Struktura adresáře (pozice se inkrementuje).
     * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
     * @param[out] res Výstupní chybový kód: MZDSK_RES_OK = OK,
     *                 MZDSK_RES_FILE_NFND = konec adresáře, záporné = chyba.
     * @return Ukazatel na položku adresáře, nebo NULL při chybě/konci.
     *
     * @warning Vrácený ukazatel směřuje do disc->cache a platí pouze
     *          do dalšího čtení sektoru.
     */
    extern st_FSMZ_DIR_ITEM* fsmz_read_dir ( st_MZDSK_DISC *disc, st_FSMZ_DIR *dir, uint8_t fsmz_dir_items, en_MZDSK_RES *res );

    /**
     * @brief Zapíše aktuální blok adresáře zpět na disk.
     *
     * Blok je určen pozicí v dir. Provede endianity korekci.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param dir Struktura adresáře s aktuální pozicí.
     * @param fsmz_dir_items Maximální počet položek adresáře.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_write_dirblock ( st_MZDSK_DISC *disc, st_FSMZ_DIR *dir, uint8_t fsmz_dir_items );

    /**
     * @brief Uloží jednu položku adresáře na zadanou pozici.
     *
     * Načte příslušný blok adresáře, nahradí položku na pozici
     * a blok zapíše zpět.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param diritem Položka adresáře k uložení.
     * @param diritem_position Pozice v adresáři (1-63 resp. 1-127).
     * @param fsmz_dir_items Maximální počet položek adresáře.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_save_diritem_to_position ( st_MZDSK_DISC *disc, st_FSMZ_DIR_ITEM *diritem, uint8_t diritem_position, uint8_t fsmz_dir_items );

    /**
     * @brief Kontinuální čtení řady po sobě jdoucích alokačních bloků.
     *
     * Čte 'size' bajtů počínaje alokačním blokem 'block'. Pokud 'size'
     * není násobkem FSMZ_SECTOR_SIZE, poslední blok se čte přes cache.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param block Číslo počátečního alokačního bloku.
     * @param size Počet bajtů ke čtení.
     * @param dst Cílový buffer.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_read_blocks ( st_MZDSK_DISC *disc, uint16_t block, uint16_t size, void *dst );

    /**
     * @brief Kontinuální zápis řady po sobě jdoucích alokačních bloků.
     *
     * Zapisuje 'size' bajtů počínaje alokačním blokem 'block'. Pokud 'size'
     * není násobkem FSMZ_SECTOR_SIZE, poslední blok se doplní nulami.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param block Číslo počátečního alokačního bloku.
     * @param size Počet bajtů k zápisu.
     * @param src Zdrojový buffer.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_write_blocks ( st_MZDSK_DISC *disc, uint16_t block, uint16_t size, void *src );

    /**
     * @brief Vyhledá soubor v adresáři podle jména v Sharp MZ ASCII.
     *
     * Prohledá adresář sekvenčně od začátku. Porovnává jména pomocí
     * mzdsk_mzstrcmp. Ignoruje smazané položky (ftype == 0).
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param mz_fname Jméno souboru v Sharp MZ ASCII (zakončeno 0x0d).
     * @param dir_cache Struktura adresáře pro cachování (může být NULL - použije se lokální).
     * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
     * @param[out] res Výstupní chybový kód: MZDSK_RES_OK = nalezeno,
     *                 MZDSK_RES_FILE_NFND = nenalezeno, záporné = chyba.
     * @return Ukazatel na nalezenou položku adresáře, nebo NULL.
     *
     * @warning Vrácený ukazatel směřuje do disc->cache.
     */
    extern st_FSMZ_DIR_ITEM* fsmz_search_file ( st_MZDSK_DISC *disc, uint8_t *mz_fname, st_FSMZ_DIR *dir_cache, uint8_t fsmz_dir_items, en_MZDSK_RES *res );

    /**
     * @brief Vypočítá velikost v bajtech z počtu alokačních bloků.
     *
     * @param blocks Počet alokačních bloků.
     * @return Velikost v bajtech (blocks * FSMZ_SECTOR_SIZE).
     */
    extern uint32_t fsmz_size_from_blocks ( uint16_t blocks );

    /**
     * @brief Vypočítá počet potřebných alokačních bloků pro zadanou velikost.
     *
     * Zaokrouhluje nahoru - pokud size není násobkem FSMZ_SECTOR_SIZE,
     * přidá jeden blok navíc.
     *
     * @param size Velikost v bajtech.
     * @return Počet potřebných alokačních bloků.
     */
    extern uint16_t fsmz_blocks_from_size ( uint32_t size );

    /**
     * @brief Smaže soubor z disku podle jména v Sharp MZ ASCII.
     *
     * Nastaví ftype na 0 v adresáři a uvolní alokované bloky
     * v DINFO bitmapě.
     *
     * Pokud je soubor označen příznakem locked, funkce standardně
     * vrátí MZDSK_RES_FILE_LOCKED a nic neprovede. Pokud je force == 1,
     * lock flag je ignorován a soubor je smazán.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param mz_fname Jméno souboru v Sharp MZ ASCII (zakončeno 0x0d).
     * @param fsmz_dir_items Maximální počet položek adresáře.
     * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
     * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_NOT_FOUND pokud
     *         soubor nenalezen, MZDSK_RES_FILE_LOCKED pokud je soubor
     *         uzamčen a force == 0, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_unlink_file ( st_MZDSK_DISC *disc, uint8_t *mz_fname, uint8_t fsmz_dir_items, uint8_t force );

    /**
     * @brief Přejmenuje soubor na disku.
     *
     * Zkontroluje, zda nové jméno ještě neexistuje. Jméno se ukládá
     * v Sharp MZ ASCII s terminátorem 0x0d.
     *
     * Pokud je soubor označen příznakem locked, funkce standardně
     * vrátí MZDSK_RES_FILE_LOCKED a nic neprovede. Pokud je force == 1,
     * lock flag je ignorován a soubor je přejmenován.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param mz_fname Původní jméno souboru v Sharp MZ ASCII.
     * @param new_mz_fname Nové jméno souboru v Sharp MZ ASCII.
     * @param fsmz_dir_items Maximální počet položek adresáře.
     * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
     * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_EXISTS pokud
     *         nové jméno již existuje, MZDSK_RES_FILE_LOCKED pokud je
     *         soubor uzamčen a force == 0, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_rename_file ( st_MZDSK_DISC *disc, uint8_t *mz_fname, uint8_t *new_mz_fname, uint8_t fsmz_dir_items, uint8_t force );

    /**
     * @brief Aktualizuje pole fstrt, fexec a ftype existujícího souboru v adresáři.
     *
     * Vyhledá soubor v adresáři podle jména (Sharp MZ ASCII) a volitelně
     * přepíše hodnoty fstrt (startovací adresa), fexec (spouštěcí adresa)
     * a ftype (souborový typ) v directory entry. Data souboru se nemění.
     *
     * Pro každý parametr: pokud je odpovídající ukazatel NULL, hodnota
     * se neupravuje. Tím lze vyměnit jen jednu z hodnot bez nutnosti
     * znát ostatní.
     *
     * Pokud je soubor označen příznakem locked, funkce standardně vrátí
     * MZDSK_RES_FILE_LOCKED. Při force == 1 je lock ignorován.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param mz_fname Jméno souboru v Sharp MZ ASCII (zakončeno 0x0d).
     * @param fstrt Ukazatel na novou startovací adresu (NULL = nezměnit).
     * @param fexec Ukazatel na novou spouštěcí adresu (NULL = nezměnit).
     * @param ftype Ukazatel na nový typ souboru (NULL = nezměnit;
     *              platné hodnoty 0x01-0xFF, 0x00 je rezervováno pro
     *              smazanou položku a není povoleno - vrátí MZDSK_RES_INVALID_PARAM).
     * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
     * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
     * @return MZDSK_RES_OK při úspěchu,
     *         MZDSK_RES_FILE_NOT_FOUND pokud soubor nenalezen,
     *         MZDSK_RES_FILE_LOCKED pokud je soubor uzamčen a force == 0,
     *         MZDSK_RES_INVALID_PARAM pokud jsou všechny tři ukazatele NULL
     *         nebo pokud ftype ukazuje na hodnotu 0x00,
     *         jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_set_addr ( st_MZDSK_DISC *disc, uint8_t *mz_fname,
                                         const uint16_t *fstrt, const uint16_t *fexec,
                                         const uint8_t *ftype,
                                         uint8_t fsmz_dir_items, uint8_t force );

    /**
     * @brief Aktualizuje pole fname, fstrt, fexec a ftype v IPLPRO bloku.
     *
     * Přečte IPLPRO blok, ověří že obsahuje platnou hlavičku (ftype==0x03
     * a "IPLPRO" magic), volitelně přepíše vybraná pole a zapíše zpět.
     * Data bootstrapu samotná se nemění - jde o metadata-only update
     * hlavičky v alokačním bloku 0.
     *
     * Pro každý parametr: pokud je ukazatel NULL, hodnota se neupravuje.
     * Alespoň jeden ukazatel musí být nenulový.
     *
     * Pokud IPLPRO hlavička není platná (disk bez bootstrapu nebo po
     * `boot clear`), funkce vrátí MZDSK_RES_FILE_NOT_FOUND a na disk
     * nic nezapíše. Nový bootstrap je nutno instalovat přes boot put
     * cestu (knihovní fsmz_write_bootstrap).
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param mz_fname Nové jméno bootstrap programu v Sharp MZ ASCII.
     *                 Bufferu musí mít aspoň FSMZ_IPLFNAME_LENGTH (13)
     *                 bajtů. NULL = nezměnit. Pokud je kratší, doplní
     *                 se terminátorem 0x0d.
     * @param fstrt Ukazatel na novou startovací adresu. NULL = nezměnit.
     * @param fexec Ukazatel na novou spouštěcí adresu. NULL = nezměnit.
     * @param ftype Ukazatel na nový typ bootstrapu. NULL = nezměnit.
     *              Hodnota 0x00 není povolena (zneplatnila by hlavičku).
     * @return MZDSK_RES_OK při úspěchu,
     *         MZDSK_RES_INVALID_PARAM pokud jsou všechny ukazatele NULL
     *         nebo pokud ftype ukazuje na 0x00,
     *         MZDSK_RES_FILE_NOT_FOUND pokud IPLPRO hlavička není
     *         platná - disk není modifikován,
     *         jinak chybový kód čtení/zápisu IPLPRO bloku.
     */
    extern en_MZDSK_RES fsmz_set_iplpro_header ( st_MZDSK_DISC *disc,
                                                  const uint8_t *mz_fname,
                                                  const uint16_t *fstrt,
                                                  const uint16_t *fexec,
                                                  const uint8_t *ftype );

    /**
     * @brief Zapíše soubor na disk.
     *
     * Najde volné souvislé místo na disku, zapíše data,
     * vytvoří položku v adresáři a aktualizuje DINFO bitmapu.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param ftype Souborový typ (0x01-0x05).
     * @param mz_fname Jméno souboru v Sharp MZ ASCII (zakončeno 0x0d).
     * @param fsize Velikost souboru v bajtech.
     * @param fstrt Startovací adresa v paměti Z80.
     * @param fexec Spouštěcí adresa v paměti Z80.
     * @param src Zdrojový buffer s daty souboru.
     * @param fsmz_dir_items Maximální počet položek adresáře.
     * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_EXIST pokud
     *         soubor již existuje, MZDSK_RES_DISC_FULL / MZDSK_RES_NO_SPACE
     *         při nedostatku místa, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_write_file ( st_MZDSK_DISC *disc, uint8_t ftype, uint8_t *mz_fname, uint16_t fsize, uint16_t fstrt, uint16_t fexec, void *src, uint8_t fsmz_dir_items );

    /**
     * @brief Aktualizuje bitmapu souborové oblasti v paměti.
     *
     * Nastaví nebo resetuje bity v bitmapě pro zadaný rozsah bloků.
     * Pracuje přímo nad polem v paměti - nezapisuje na disk.
     *
     * @param map Ukazatel na bitmapu (FSMZ_FAREA_BITMAP_SIZE bajtů).
     * @param setres FSMZ_DINFO_BITMAP_SET (obsadit) nebo FSMZ_DINFO_BITMAP_RESET (uvolnit).
     * @param farea_block Počáteční blok relativně k farea.
     * @param count_blocks Počet bloků k označení.
     */
    extern void fsmz_update_farea_bitmap ( uint8_t *map, uint8_t setres, uint16_t farea_block, uint16_t count_blocks );

    /**
     * @brief Aktualizuje volume_number v DINFO bloku na disku.
     *
     * Přečte DINFO, změní volume_number a zapíše zpět.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param volume_number Nová hodnota volume_number (0 = master, >0 = slave).
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_update_dinfo_volume_number ( st_MZDSK_DISC *disc, uint8_t volume_number );

    /**
     * @brief Aktualizuje DINFO bitmapu na disku pro zadaný rozsah bloků.
     *
     * Přečte DINFO z disku, aktualizuje počítadlo obsazených bloků
     * a bitmapu, pak zapíše DINFO zpět. Číslo bloku se automaticky
     * přepočítá relativně k farea.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param setres FSMZ_DINFO_BITMAP_SET nebo FSMZ_DINFO_BITMAP_RESET.
     * @param block Absolutní číslo alokačního bloku.
     * @param count_blocks Počet bloků k označení.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_update_dinfo_farea_bitmap ( st_MZDSK_DISC *disc, uint8_t setres, uint16_t block, uint16_t count_blocks );

    /**
     * @brief Vyhledá volné souvislé místo na disku.
     *
     * Prohledá DINFO bitmapu a najde první souvislý úsek
     * volných alokačních bloků požadované velikosti.
     *
     * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param num_blocks Požadovaný počet souvislých volných bloků.
     * @param[out] start_block Výstup: číslo počátečního bloku nalezeného úseku.
     * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DISC_FULL pokud
     *         není dostatek volných bloků, MZDSK_RES_NO_SPACE pokud
     *         není dostatek souvislého místa, jinak chybový kód.
     */
    extern en_MZDSK_RES fsmz_check_free_blocks ( st_MZDSK_DISC *disc, uint16_t num_blocks, uint16_t *start_block );


    /* =====================================================================
     * Verze knihovny
     * ===================================================================== */

    /** @brief Verze knihovny mzdsk_ipldisk. */
#define MZDSK_IPLDISK_VERSION "1.5.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzdsk_ipldisk.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzdsk_ipldisk_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_IPLDISK_H */
