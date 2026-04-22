/**
 * @file   mzdsk_mrs.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Knihovna pro práci se souborovým systémem MRS na disketách Sharp MZ.
 *
 * Poskytuje operace nad MRS (Memory Resident System) - inicializaci
 * konfigurace z FAT, převod blok -> stopa/sektor a čtení/zápis
 * jednotlivých bloků diskového obrazu.
 *
 * MRS souborový systém používá jednoduchou FAT (1 bajt na blok),
 * kde hodnota bajtu identifikuje vlastníka bloku (číslo souboru,
 * systémová oblast, FAT, adresář, nebo volný blok).
 *
 * Inicializační funkce fsmrs_init() přečte FAT z diskety a zjistí
 * z ní kompletní rozložení disku (FAT, adresář, datová oblast).
 * Všechny další operace pracují s konfigurační strukturou
 * st_FSMRS_CONFIG, která obsahuje kopii FAT v paměti.
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


#ifndef MZDSK_MRS_H
#define MZDSK_MRS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/dsk/dsk.h"
#include "libs/mzdsk_global/mzdsk_global.h"


    /* =====================================================================
     * Konstanty
     * ===================================================================== */

    /**
     * @brief Kódovaná velikost sektoru používaná MRS souborovým systémem.
     *
     * MRS pracuje se sektory o velikosti 512 bajtů (DSK_SECTOR_SIZE_512).
     */
#define FSMRS_DSK_SECTOR_SIZE DSK_SECTOR_SIZE_512

    /**
     * @brief Velikost jednoho MRS sektoru/bloku v bajtech.
     *
     * MRS sektor má fixně 512 bajtů (0x0200). Jeden blok = jeden sektor.
     */
#define FSMRS_SECTOR_SIZE 0x0200

    /**
     * @brief Počet sektorů na jedné stopě v MRS formátu.
     *
     * MRS formát používá 9 sektorů na stopu. Sektory jsou číslovány 1-9.
     */
#define FSMRS_SECTORS_ON_TRACK 9

    /**
     * @brief Celkový počet bloků na MRS disku (720K).
     *
     * 160 absolutních stop (80 fyzických x 2 strany) x 9 sektorů = 1440.
     * Jeden blok = jeden sektor = 512 B.
     */
#define FSMRS_COUNT_BLOCKS ( FSMRS_SECTORS_ON_TRACK * 160 )

    /**
     * @brief Počet adresářových položek na jeden sektor.
     *
     * Každá položka má 32 bajtů: 512 / 32 = 16.
     */
#define FSMRS_DIR_ITEMS_PER_SECTOR 16

    /**
     * @brief Maximální velikost bufferu pro adresář v bajtech.
     *
     * Vyhrazeno 8 sektorů = 4096 B = 128 položek, což je dostatečná
     * rezerva pro všechny známé varianty MRS formátu (typicky 6 nebo
     * 7 sektorů podle interpretace FAT markerů).
     */
#define FSMRS_MAX_DIR_BUFFER ( 8 * FSMRS_SECTOR_SIZE )


    /* =====================================================================
     * Speciální hodnoty ve FAT
     * ===================================================================== */

    /** @brief FAT hodnota: volný blok. */
#define FSMRS_FAT_FREE      0x00
    /** @brief FAT hodnota: blok patří FAT tabulce. */
#define FSMRS_FAT_FAT       0xFA
    /** @brief FAT hodnota: blok patří adresáři. */
#define FSMRS_FAT_DIR       0xFD
    /** @brief FAT hodnota: vadný sektor. */
#define FSMRS_FAT_BAD       0xFE
    /** @brief FAT hodnota: systémový/rezervovaný blok (boot oblast). */
#define FSMRS_FAT_SYSTEM    0xFF


    /* =====================================================================
     * Typy a struktury
     * ===================================================================== */

    /**
     * @brief Typ I/O operace pro čtení/zápis MRS bloku.
     *
     * Určuje směr datového přenosu při volání fsmrs_rw_block().
     */
    typedef enum en_FSMRS_IOOP {
        FSMRS_IOOP_READ = (uint8_t) 0,     /**< Operace čtení bloku */
        FSMRS_IOOP_WRITE = (uint8_t) 1,    /**< Operace zápisu bloku */
    } en_FSMRS_IOOP;


    /**
     * @brief Položka adresáře MRS souborového systému (32 bajtů).
     *
     * Každý záznam identifikuje jeden soubor na disketě. Název je
     * case-sensitive, duplicita se kontroluje pouze u prvních 8 znaků
     * (přípona není podstatná).
     *
     * Aktivní soubor má fname[0] > 0x20.
     * Smazaný soubor má fname vyplněný mezerami (0x20).
     * Nepoužitelný slot má fname[0] < 0x20.
     *
     * @invariant Velikost struktury musí být přesně 32 bajtů.
     */
    typedef struct st_FSMRS_DIR_ITEM {
        uint8_t  fname[8];      /**< Název souboru, doplněný mezerami (0x20) */
        uint8_t  ext[3];        /**< Přípona: "MRS", "DAT", "RAM" nebo "SCR" */
        uint8_t  file_id;       /**< Pořadové číslo souboru (1-88), slouží jako klíč ve FAT */
        uint16_t fstrt;         /**< Start adresa (LE). U MRS souborů: dolní bajt = 0xFA,
                                     horní bajt = pozice v textovém bufferu editoru.
                                     U DAT souborů: skutečná load adresa v RAM. */
        uint16_t bsize;         /**< Počet alokovaných bloků (LE). Velikost souboru = bsize * 512 B. */
        uint8_t  reserved1[6];  /**< Rezervováno (nuly) */
        uint16_t fexec;         /**< Exec adresa (LE). 0x0000 u zdrojových textů. */
        uint8_t  reserved2[4];  /**< Rezervováno (nuly) */
        uint8_t  reserved3[4];  /**< Inicializační vzor (typicky 0xCD490221 nebo nuly) */
    } st_FSMRS_DIR_ITEM;


    /**
     * @brief Konfigurační struktura MRS souborového systému.
     *
     * Obsahuje kompletní popis rozložení disku zjištěný analýzou FAT.
     * Inicializuje se voláním fsmrs_init() a předává se všem dalším
     * funkcím knihovny.
     *
     * Životní cyklus:
     * 1. Alokace (statická nebo dynamická)
     * 2. Inicializace přes fsmrs_init()
     * 3. Používání dalšími funkcemi knihovny
     * 4. Žádné uvolnění není třeba (struktura nevlastní žádné zdroje)
     *
     * @invariant Po úspěšném fsmrs_init() je disc != NULL.
     * @invariant fat_block + fat_sectors == dir_block (adresář bezprostředně
     *            následuje za FAT).
     * @invariant dir_block + dir_sectors == data_block (data bezprostředně
     *            následují za adresářem).
     */
    typedef struct st_FSMRS_CONFIG {
        st_MZDSK_DISC *disc;            /**< Odkaz na otevřenou diskovou strukturu (nevlastněný) */
        uint16_t fat_block;             /**< Číslo prvního bloku FAT */
        uint16_t fat_sectors;           /**< Počet sektorů FAT (zjištěno z hodnot 0xFA ve FAT) */
        uint16_t dir_block;             /**< Číslo prvního bloku adresáře */
        uint16_t dir_sectors;           /**< Počet sektorů adresáře (zjištěno z hodnot 0xFD ve FAT) */
        uint16_t data_block;            /**< Číslo prvního datového bloku */
        uint16_t total_blocks;          /**< Celkový počet použitelných bloků (bez systémových) */
        uint16_t free_blocks;           /**< Počet volných bloků (hodnota 0x00 ve FAT) */
        uint16_t used_files;            /**< Počet obsazených souborů v adresáři */
        uint16_t max_files;             /**< Celkový počet slotů v adresáři (dir_sectors * 16) */
        uint16_t usable_files;          /**< Počet použitelných slotů (bez systémových a nedostupných) */
        uint8_t  fat[FSMRS_COUNT_BLOCKS]; /**< Kopie FAT tabulky v paměti (1440 bajtů) */
        uint8_t  dir[FSMRS_MAX_DIR_BUFFER]; /**< Kopie adresáře v paměti (deinvertovaná) */
    } st_FSMRS_CONFIG;


    /* =====================================================================
     * Typy pro alokační mapu MRS
     * ===================================================================== */

    /**
     * @brief Typ MRS alokačního bloku v mapě.
     *
     * Určuje roli každého bloku na MRS disku podle jeho hodnoty ve FAT.
     * Hodnoty jsou vzájemně exkluzivní - každý blok má právě jeden typ.
     */
    typedef enum en_FSMRS_BLOCK_TYPE {
        FSMRS_BLOCK_FREE = 0,     /**< Volný blok (FAT hodnota 0x00) */
        FSMRS_BLOCK_FAT,           /**< FAT tabulka (FAT hodnota 0xFA) */
        FSMRS_BLOCK_DIR,           /**< Adresářový blok (FAT hodnota 0xFD) */
        FSMRS_BLOCK_SYSTEM,        /**< Systémový/rezervovaný blok (FAT hodnota 0xFF) */
        FSMRS_BLOCK_BAD,           /**< Vadný sektor (FAT hodnota 0xFE) */
        FSMRS_BLOCK_FILE,          /**< Souborový blok (FAT hodnota 1..max_file_id) */
    } en_FSMRS_BLOCK_TYPE;


    /**
     * @brief Statistiky MRS alokační mapy.
     *
     * Souhrnné počty bloků podle typu, počítané z FAT tabulky.
     * Hodnoty jsou platné po úspěšném volání fsmrs_get_block_map().
     *
     * @invariant total_blocks == fat_blocks + dir_blocks + sys_blocks
     *            + bad_blocks + file_blocks + free_blocks
     */
    typedef struct st_FSMRS_MAP_STATS {
        int total_blocks;     /**< Celkový počet bloků (FSMRS_COUNT_BLOCKS = 1440) */
        int fat_blocks;       /**< Počet FAT bloků (hodnota 0xFA) */
        int dir_blocks;       /**< Počet adresářových bloků (hodnota 0xFD) */
        int sys_blocks;       /**< Počet systémových bloků (hodnota 0xFF) */
        int bad_blocks;       /**< Počet vadných bloků (hodnota 0xFE) */
        int file_blocks;      /**< Počet souborových bloků (hodnota 1..max) */
        int free_blocks;      /**< Počet volných bloků (hodnota 0x00) */
    } st_FSMRS_MAP_STATS;


    /* =====================================================================
     * Inicializace a práce s konfigurací
     * ===================================================================== */

    /**
     * @brief Inicializuje konfiguraci MRS souborového systému z obsahu FAT.
     *
     * Přečte FAT sektory z diskety počínaje blokem fat_block a z jejich
     * obsahu zjistí kompletní rozložení disku:
     * - počet FAT sektorů (bloky s hodnotou FSMRS_FAT_FAT)
     * - počet adresářových sektorů (bloky s hodnotou FSMRS_FAT_DIR)
     * - začátek datové oblasti
     * - počet volných bloků
     * - počet obsazených souborů (analýzou adresáře)
     *
     * Po úspěšné inicializaci obsahuje config->fat[] kompletní kopii
     * FAT tabulky, config->dir[] obsahuje deinvertovaný adresář
     * a všechna odvozená pole jsou vyplněna.
     *
     * @param[in]  disc           Ukazatel na otevřenou diskovou strukturu. Nesmí být NULL.
     * @param[in]  fat_block      Číslo bloku, kde začíná FAT (typicky 36 pro 720K MRS disk).
     * @param[out] config         Ukazatel na konfigurační strukturu k naplnění. Nesmí být NULL.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud disc nebo config je NULL.
     * @return MZDSK_RES_DSK_ERROR při chybě čtení z disku.
     * @return MZDSK_RES_FORMAT_ERROR pokud FAT na zadané pozici nemá
     *         očekávanou strukturu (chybí značky 0xFA/0xFD).
     *
     * @pre disc musí být úspěšně otevřený přes mzdsk_disc_open().
     * @pre fat_block musí ukazovat na platný FAT blok (jeho hodnota
     *      ve FAT musí být FSMRS_FAT_FAT).
     *
     * @post Při úspěchu je config kompletně inicializovaný.
     * @post Při neúspěchu je obsah config nedefinovaný.
     */
    extern en_MZDSK_RES fsmrs_init ( st_MZDSK_DISC *disc, uint16_t fat_block, st_FSMRS_CONFIG *config );


    /**
     * @brief Vrátí ukazatel na adresářovou položku podle indexu.
     *
     * Ukazatel odkazuje přímo do interního bufferu config->dir[],
     * takže jeho platnost je vázána na životnost config struktury.
     *
     * @param[in] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     * @param[in] index   Index položky (0 .. max_files-1).
     *
     * @return Ukazatel na položku adresáře, nebo NULL při neplatném indexu.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     */
    extern st_FSMRS_DIR_ITEM* fsmrs_get_dir_item ( st_FSMRS_CONFIG *config, uint16_t index );


    /**
     * @brief Určí, zda je daný adresářový slot aktivní (obsahuje soubor).
     *
     * Slot je aktivní, pokud první znak jména je větší než 0x20 (mezera).
     * Smazaný nebo nepoužitý slot má fname[0] <= 0x20.
     *
     * @param[in] item  Ukazatel na adresářovou položku. Nesmí být NULL.
     * @return 1 pokud je slot aktivní, 0 pokud je volný/smazaný.
     */
    extern int fsmrs_is_dir_item_active ( const st_FSMRS_DIR_ITEM *item );


    /**
     * @brief Vytvoří čistý MRS souborový systém na disketě.
     *
     * Inicializuje FAT a adresář MRS formátu na zadané pozici.
     * Nezasahuje do systémové oblasti (bloky 0 .. fat_block-1), která
     * by měla obsahovat boot kód a MRS systém. Předpokládá 720K disketu
     * s 1440 alokačními bloky (9 sektorů × 160 stop × 512 B).
     *
     * Vytvořená struktura:
     * - FAT: 3 fyzické sektory (bloky fat_block, fat_block+1, fat_block+2)
     *   - Bloky 0 .. fat_block-1: FSMRS_FAT_SYSTEM (0xFF)
     *   - Bloky fat_block, fat_block+1: FSMRS_FAT_FAT (0xFA)
     *   - Bloky fat_block+2 .. fat_block+8: FSMRS_FAT_DIR (0xFD)
     *   - Ostatní bloky: FSMRS_FAT_FREE (0x00)
     *
     *   Pozn.: Blok fat_block+2 je fyzicky 3. FAT sektor, ale je
     *   označen jako 0xFD. Tuto inkonzistenci zavádí původní MRS
     *   driver a knihovna ji zachovává pro kompatibilitu.
     *
     * - Adresář: 6 fyzických sektorů (bloky fat_block+3 .. fat_block+8)
     *   - 96 slotů po 32 bajtech
     *   - Prvních 89 slotů: prázdné položky s postupně rostoucím
     *     file_id (1-89), jménem z mezer a inicializačním vzorem
     *     0xCD490221 na offsetu 28-31
     *   - Posledních 7 slotů: vyplněno hodnotou 0x1A (nedostupné sloty)
     *
     * Veškerá data jsou před zápisem invertována (XOR 0xFF).
     *
     * @param[in] disc      Ukazatel na otevřenou diskovou strukturu v režimu RW. Nesmí být NULL.
     * @param[in] fat_block Číslo bloku, kde má začínat FAT (typicky 36 pro 720K MRS).
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud disc je NULL nebo fat_block je mimo rozsah.
     * @return MZDSK_RES_WRITE_PROTECTED pokud je disk pouze pro čtení.
     * @return MZDSK_RES_DSK_ERROR při chybě zápisu.
     *
     * @pre disc musí být otevřený v režimu FILE_DRIVER_OPMODE_RW.
     * @pre fat_block + 8 < FSMRS_COUNT_BLOCKS
     *
     * @post Při úspěchu je FAT a adresář zapsán na disk.
     * @post Systémová oblast (bloky 0 .. fat_block-1) a datová oblast
     *       (bloky fat_block+9 a dále) nejsou ovlivněny.
     */
    extern en_MZDSK_RES fsmrs_format_fs ( st_MZDSK_DISC *disc, uint16_t fat_block );


    /* =====================================================================
     * Nízkoúrovňové I/O
     * ===================================================================== */

    /**
     * @brief Callback pro získání informací o sektoru na dané stopě.
     *
     * MRS používá na stopě 1 sektory 256 B (boot stopa), na ostatních
     * stopách sektory 512 B. Data jsou vždy MZDSK_MEDIUM_NORMAL
     * (neinvertovaná z pohledu driveru - inverze se řeší na úrovni MRS).
     *
     * @param[in] track      Číslo stopy.
     * @param[in] sector     Číslo sektoru (nepoužívá se).
     * @param[in] user_data  Uživatelská data (nepoužívá se, může být NULL).
     * @return Kombinace MZDSK_MEDIUM_* | DSK_SECTOR_SIZE_* pro daný sektor.
     */
    extern uint8_t fsmrs_sector_info_cb ( uint16_t track, uint16_t sector, void *user_data );

    /**
     * @brief Převede číslo bloku na stopu a sektor.
     *
     * MRS disk má 9 sektorů na stopu, číslovaných 1-9.
     *
     *   stopa  = blok / 9
     *   sektor = (blok % 9) + 1
     *
     * Výsledek je zakódován do 16bitové hodnoty:
     *   horní bajt = číslo stopy, dolní bajt = číslo sektoru.
     *
     * @param[in] block Číslo bloku (0 .. FSMRS_COUNT_BLOCKS-1).
     * @return Kombinovaná hodnota stopa/sektor.
     *
     * @note Dekódování: track = (retval >> 8) & 0xff,
     *       sector = retval & 0xff.
     */
    extern uint16_t fsmrs_block2trsec ( uint16_t block );

    /**
     * @brief Přečte nebo zapíše jeden MRS blok.
     *
     * Převede číslo bloku na stopu a sektor pomocí fsmrs_block2trsec()
     * a provede odpovídající I/O operaci.
     *
     * @param[in]     ioop  Typ operace (FSMRS_IOOP_READ nebo FSMRS_IOOP_WRITE).
     * @param[in,out] disc  Ukazatel na diskovou strukturu. Nesmí být NULL.
     * @param[in]     block Číslo bloku (0 .. FSMRS_COUNT_BLOCKS-1).
     * @param[in,out] dma   Buffer pro data. Musí mít alespoň FSMRS_SECTOR_SIZE bajtů.
     *                      Nesmí být NULL.
     * @return MZDSK_RES_OK při úspěchu, chybový kód při selhání.
     *
     * @pre disc != NULL, dma != NULL
     * @pre block < FSMRS_COUNT_BLOCKS
     */
    extern en_MZDSK_RES fsmrs_rw_block ( en_FSMRS_IOOP ioop, st_MZDSK_DISC *disc, uint16_t block, void* dma );

    /**
     * @brief Makro pro čtení jednoho MRS bloku.
     * @param disc  Ukazatel na diskovou strukturu (st_MZDSK_DISC*).
     * @param block Číslo bloku.
     * @param dma   Buffer pro přečtená data.
     * @return Výsledek operace (en_MZDSK_RES).
     */
#define fsmrs_read_block( disc, block, dma) fsmrs_rw_block ( FSMRS_IOOP_READ, disc, block, dma )

    /**
     * @brief Makro pro zápis jednoho MRS bloku.
     * @param disc  Ukazatel na diskovou strukturu (st_MZDSK_DISC*).
     * @param block Číslo bloku.
     * @param dma   Buffer s daty k zápisu.
     * @return Výsledek operace (en_MZDSK_RES).
     */
#define fsmrs_write_block( disc, block, dma) fsmrs_rw_block ( FSMRS_IOOP_WRITE, disc, block, dma )


    /* =====================================================================
     * Souborové operace
     * ===================================================================== */

    /**
     * @brief Vyhledá soubor v adresáři podle jména (a volitelně přípony).
     *
     * Prohledá všechny aktivní položky v config->dir[] a vrátí první
     * odpovídající. Porovnání jména je case-sensitive na celých 8 bajtech.
     *
     * @param[in] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     * @param[in] fname   Jméno souboru (8 bajtů, doplněné mezerami 0x20). Nesmí být NULL.
     * @param[in] ext     Přípona (3 bajty, doplněná mezerami 0x20), nebo NULL
     *                    pro vyhledání pouze podle jména (přípona se nekontroluje).
     *
     * @return Ukazatel na nalezenou položku v config->dir[], nebo NULL pokud nenalezeno.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre fname musí ukazovat na pole alespoň 8 bajtů.
     * @pre ext (pokud není NULL) musí ukazovat na pole alespoň 3 bajty.
     *
     * @note Vrácený ukazatel je platný po celou dobu životnosti config.
     * @note MRS kontroluje duplicitu pouze podle fname (8 znaků), přípona
     *       není podstatná pro unikátnost.
     */
    extern st_FSMRS_DIR_ITEM* fsmrs_search_file ( st_FSMRS_CONFIG *config, const uint8_t *fname, const uint8_t *ext );

    /**
     * @brief Vyhledá aktivní soubor v adresáři podle čísla souboru (file_id).
     *
     * Prohledá všechny položky v config->dir[] a vrátí aktivní položku
     * s odpovídajícím file_id.
     *
     * @param[in] config   Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     * @param[in] file_id  Číslo souboru (1-89, odpovídá hodnotě v dir položce).
     *
     * @return Ukazatel na nalezenou položku v config->dir[], nebo NULL pokud
     *         nenalezeno nebo file_id neodpovídá žádnému aktivnímu souboru.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     */
    extern st_FSMRS_DIR_ITEM* fsmrs_search_file_by_id ( st_FSMRS_CONFIG *config, uint8_t file_id );

    /**
     * @brief Přečte data souboru do paměťového bufferu.
     *
     * Projde celou FAT tabulku, vybere bloky patřící souboru
     * (identifikované hodnotou file_id) ve vzestupném pořadí
     * a přečte je do výstupního bufferu. Data jsou automaticky
     * deinvertována (XOR 0xFF).
     *
     * @param[in]  config    Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     * @param[in]  item      Ukazatel na adresářovou položku souboru. Nesmí být NULL.
     *                       Musí být aktivní (fsmrs_is_dir_item_active() == 1).
     * @param[out] dst       Výstupní buffer pro data souboru. Nesmí být NULL.
     * @param[in]  dst_size  Velikost výstupního bufferu v bajtech.
     *                       Musí být >= item->bsize * FSMRS_SECTOR_SIZE.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud je některý parametr NULL.
     * @return MZDSK_RES_BUFFER_SMALL pokud dst_size < potřebná velikost.
     * @return MZDSK_RES_DSK_ERROR při chybě čtení z disku.
     * @return MZDSK_RES_FORMAT_ERROR pokud počet nalezených bloků ve FAT
     *         neodpovídá hodnotě bsize v adresářové položce.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre item musí ukazovat do config->dir[] (platný ukazatel).
     * @post Při úspěchu dst obsahuje deinvertovaná data souboru
     *       (item->bsize * FSMRS_SECTOR_SIZE bajtů).
     */
    extern en_MZDSK_RES fsmrs_read_file ( st_FSMRS_CONFIG *config, const st_FSMRS_DIR_ITEM *item, void *dst, uint32_t dst_size );

    /**
     * @brief Zapíše nový soubor na disk.
     *
     * Postup:
     * 1. Ověří platnost jména (fname[0] > 0x20) a unikátnost.
     * 2. Spočítá potřebné bloky: ceil(src_size / 512), min. 1.
     * 3. Najde volný adresářový slot (fname[0] == 0x20).
     * 4. Alokuje volné bloky v datové oblasti (v rámci FAT pokrytí).
     * 5. Zapíše invertovaná data do alokovaných bloků.
     * 6. Aktualizuje FAT a adresář v paměti i na disku.
     *
     * @param[in,out] config    Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     * @param[in]     fname     Jméno souboru (8 bajtů, doplněné mezerami). Nesmí být NULL.
     * @param[in]     ext       Přípona souboru (3 bajty, doplněná mezerami). Nesmí být NULL.
     * @param[in]     fstrt     Start adresa (load address).
     * @param[in]     fexec     Exec adresa (execution address).
     * @param[in]     src       Zdrojová data souboru. Může být NULL pokud src_size == 0.
     * @param[in]     src_size  Velikost zdrojových dat v bajtech.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud je config, fname nebo ext NULL.
     * @return MZDSK_RES_BAD_NAME pokud fname[0] <= 0x20.
     * @return MZDSK_RES_FILE_EXISTS pokud soubor se stejným jménem již existuje.
     * @return MZDSK_RES_DISK_FULL pokud není dostatek volných bloků.
     * @return MZDSK_RES_DIR_FULL pokud není volný adresářový slot.
     * @return MZDSK_RES_DSK_ERROR při chybě zápisu na disk.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre config->disc musí být otevřený v režimu FILE_DRIVER_OPMODE_RW.
     *
     * @post Při úspěchu je soubor zapsán na disk, FAT a adresář aktualizovány.
     * @post config->free_blocks a config->used_files jsou aktualizovány.
     *
     * @warning Při selhání zápisu dat může dojít k částečné modifikaci disku.
     */
    extern en_MZDSK_RES fsmrs_write_file ( st_FSMRS_CONFIG *config, const uint8_t *fname, const uint8_t *ext,
                                            uint16_t fstrt, uint16_t fexec, const void *src, uint32_t src_size );

    /**
     * @brief Smaže soubor z disku.
     *
     * Postup:
     * 1. Uvolní všechny bloky souboru ve FAT (nastaví na FSMRS_FAT_FREE).
     * 2. Vymaže adresářovou položku (fname vyplní mezerami, vynuluje metadata).
     * 3. Zapíše aktualizovanou FAT a adresář na disk.
     *
     * @param[in,out] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     * @param[in,out] item    Ukazatel na adresářovou položku souboru k smazání.
     *                        Nesmí být NULL. Musí ukazovat do config->dir[].
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud je config nebo item NULL.
     * @return MZDSK_RES_DSK_ERROR při chybě zápisu na disk.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre config->disc musí být otevřený v režimu FILE_DRIVER_OPMODE_RW.
     * @pre item musí být aktivní (fsmrs_is_dir_item_active() == 1).
     *
     * @post Při úspěchu je soubor smazán, FAT a adresář aktualizovány.
     * @post item->fname je vyplněn mezerami (0x20).
     * @post config->free_blocks a config->used_files jsou aktualizovány.
     */
    extern en_MZDSK_RES fsmrs_delete_file ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item );

    /**
     * @brief Přejmenuje soubor (změní jméno a/nebo příponu).
     *
     * Postup:
     * 1. Ověří platnost nového jména (new_fname[0] > 0x20).
     * 2. Zkontroluje, že soubor s novým jménem neexistuje.
     * 3. Aktualizuje jméno a příponu v adresářové položce.
     * 4. Zapíše aktualizovaný adresář na disk.
     *
     * @param[in,out] config     Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     * @param[in,out] item       Ukazatel na adresářovou položku souboru. Nesmí být NULL.
     *                           Musí ukazovat do config->dir[].
     * @param[in]     new_fname  Nové jméno souboru (8 bajtů, doplněné mezerami). Nesmí být NULL.
     * @param[in]     new_ext    Nová přípona (3 bajty, doplněná mezerami), nebo NULL
     *                           pro zachování původní přípony.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud je config, item nebo new_fname NULL.
     * @return MZDSK_RES_BAD_NAME pokud new_fname[0] <= 0x20.
     * @return MZDSK_RES_FILE_EXISTS pokud soubor s novým jménem již existuje.
     * @return MZDSK_RES_DSK_ERROR při chybě zápisu na disk.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre config->disc musí být otevřený v režimu FILE_DRIVER_OPMODE_RW.
     *
     * @post Při úspěchu je jméno (a případně přípona) aktualizováno na disku.
     */
    extern en_MZDSK_RES fsmrs_rename_file ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item,
                                             const uint8_t *new_fname, const uint8_t *new_ext );


    /**
     * @brief Nastaví (update) start a exec adresu existujícího souboru.
     *
     * Data souboru ani FAT se nedotkne, modifikuje pouze directory
     * položku (fields fstrt a fexec) a flushne adresář na disk.
     *
     * @param[in,out] config  Konfigurace inicializovaná přes fsmrs_init().
     * @param[in,out] item    Ukazatel do config->dir_items[] - položka
     *                        existujícího souboru.
     * @param[in]     fstrt   Nová start (load) adresa (0x0000-0xFFFF).
     * @param[in]     fexec   Nová exec (execution) adresa (0x0000-0xFFFF).
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud je config nebo item NULL.
     * @return MZDSK_RES_DSK_ERROR při chybě zápisu na disk.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre config->disc musí být otevřený v režimu FILE_DRIVER_OPMODE_RW.
     *
     * @post Při úspěchu jsou fstrt/fexec aktualizované na disku.
     */
    extern en_MZDSK_RES fsmrs_set_addr ( st_FSMRS_CONFIG *config, st_FSMRS_DIR_ITEM *item,
                                          uint16_t fstrt, uint16_t fexec );


    /* =====================================================================
     * Alokační mapa
     * ===================================================================== */

    /**
     * @brief Sestaví alokační mapu MRS disku z FAT tabulky.
     *
     * Pro každý blok v rozsahu 0 .. FSMRS_COUNT_BLOCKS-1 (resp. do map_size)
     * klasifikuje FAT hodnotu na odpovídající typ bloku a volitelně
     * spočítá souhrnné statistiky.
     *
     * Klasifikační pravidla podle FAT hodnoty:
     * - 0x00 (FSMRS_FAT_FREE): FSMRS_BLOCK_FREE
     * - 0xFA (FSMRS_FAT_FAT): FSMRS_BLOCK_FAT
     * - 0xFD (FSMRS_FAT_DIR): FSMRS_BLOCK_DIR
     * - 0xFE (FSMRS_FAT_BAD): FSMRS_BLOCK_BAD
     * - 0xFF (FSMRS_FAT_SYSTEM): FSMRS_BLOCK_SYSTEM
     * - ostatní (1..max): FSMRS_BLOCK_FILE
     *
     * @param[in]  config    Inicializovaná MRS konfigurace s platnou FAT tabulkou.
     *                       Nesmí být NULL.
     * @param[out] map       Pole o velikosti alespoň FSMRS_COUNT_BLOCKS.
     *                       Nesmí být NULL. Po návratu obsahuje typ pro každý blok.
     * @param[in]  map_size  Velikost pole map (počet prvků).
     * @param[out] stats     Statistiky alokační mapy (může být NULL).
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre map_size >= FSMRS_COUNT_BLOCKS pro kompletní mapu.
     *
     * @post map je naplněna klasifikací pro min(FSMRS_COUNT_BLOCKS, map_size) bloků.
     * @post stats (pokud != NULL) obsahuje souhrnné statistiky.
     *
     * @note Funkce neprovádí žádný výstup na stdout/stderr ani disk I/O.
     */
    extern void fsmrs_get_block_map ( const st_FSMRS_CONFIG *config,
                                       en_FSMRS_BLOCK_TYPE *map,
                                       int map_size,
                                       st_FSMRS_MAP_STATS *stats );


    /* =====================================================================
     * Defragmentace
     * ===================================================================== */

    /**
     * @brief Typ callbacku pro hlášení průběhu defragmentace.
     *
     * Knihovní funkce fsmrs_defrag() volá tento callback s informačními
     * zprávami o průběhu operace. Zprávy jsou v angličtině a zakončené '\n'.
     *
     * @param message  Textová zpráva o průběhu (nulou zakončený řetězec).
     * @param user_data Uživatelská data předaná při volání fsmrs_defrag().
     */
    typedef void (*fsmrs_defrag_cb_t) ( const char *message, void *user_data );


    /**
     * @brief Provede defragmentaci MRS disku.
     *
     * Načte všechny aktivní soubory z MRS disku do paměti, provede
     * formát (fsmrs_format_fs) a znovu zapíše vše sekvenčně od začátku
     * datové oblasti bez mezer.
     *
     * Algoritmus:
     * 1. Inicializuje konfiguraci z FAT (fsmrs_init).
     * 2. Projde adresář a pro každý aktivní soubor:
     *    - Alokuje buffer o velikosti bsize * FSMRS_SECTOR_SIZE.
     *    - Přečte data souboru (fsmrs_read_file).
     *    - Uloží metadata (fname, ext, fstrt, fexec, bsize).
     * 3. Provede formát FAT a adresáře (fsmrs_format_fs).
     * 4. Re-inicializuje konfiguraci z čerstvě naformátovaného disku.
     * 5. Sekvenčně zapíše všechny soubory (fsmrs_write_file) -
     *    výsledkem jsou soubory uložené bez fragmentace.
     * 6. Uvolní veškerou alokovanou paměť.
     *
     * Průběh operace je hlášen přes progress_cb (pokud není NULL).
     *
     * @param[in] disc         Otevřený disk v MRS formátu (RW). Nesmí být NULL.
     * @param[in] fat_block    Číslo bloku, kde začíná FAT (typicky 36).
     * @param[in] progress_cb  Callback pro hlášení průběhu (může být NULL).
     * @param[in] cb_data      Uživatelská data pro callback (může být NULL).
     *
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc nesmí být NULL.
     * @pre disc musí být otevřený v režimu FILE_DRIVER_OPMODE_RW.
     * @pre Na disku musí existovat platný MRS souborový systém.
     *
     * @post Při úspěchu jsou všechny soubory na disku uloženy sekvenčně
     *       bez mezer od začátku datové oblasti.
     *
     * @warning Defragmentace je destruktivní operace - při chybě uprostřed
     *          procesu mohou být data na disku nekonzistentní nebo ztracena.
     *
     * @note Funkce nepíše na stdout ani stderr. Veškeré informační zprávy
     *       jsou předávány přes progress_cb.
     */
    extern en_MZDSK_RES fsmrs_defrag ( st_MZDSK_DISC *disc,
                                        uint16_t fat_block,
                                        fsmrs_defrag_cb_t progress_cb,
                                        void *cb_data );


    /* =====================================================================
     * Zápis FAT a adresáře na disk
     * ===================================================================== */

    /**
     * @brief Zapíše FAT tabulku z paměti na disk.
     *
     * Vezme config->fat[], invertuje ho (XOR 0xFF) po sektorech
     * a zapíše na disk počínaje blokem config->fat_block.
     *
     * @param[in] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud je config NULL.
     * @return MZDSK_RES_DSK_ERROR při chybě zápisu.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre config->disc musí být otevřený v režimu FILE_DRIVER_OPMODE_RW.
     */
    extern en_MZDSK_RES fsmrs_flush_fat ( st_FSMRS_CONFIG *config );

    /**
     * @brief Zapíše adresář z paměti na disk.
     *
     * Vezme config->dir[], invertuje ho (XOR 0xFF) po sektorech
     * a zapíše na disk počínaje blokem config->dir_block.
     *
     * @param[in] config  Ukazatel na inicializovanou konfiguraci. Nesmí být NULL.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud je config NULL.
     * @return MZDSK_RES_DSK_ERROR při chybě zápisu.
     *
     * @pre config musí být inicializovaný přes fsmrs_init().
     * @pre config->disc musí být otevřený v režimu FILE_DRIVER_OPMODE_RW.
     */
    extern en_MZDSK_RES fsmrs_flush_dir ( st_FSMRS_CONFIG *config );


    /* =====================================================================
     * Verze knihovny
     * ===================================================================== */

    /** @brief Verze knihovny mzdsk_mrs. */
#define MZDSK_MRS_VERSION "2.5.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzdsk_mrs.
     * @return Statický řetězec s verzí.
     */
    extern const char* mzdsk_mrs_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_MRS_H */
