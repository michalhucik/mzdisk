/**
 * @file   mzdsk_cpm.h
 * @brief  Knihovna pro práci s CP/M souborovým systémem na discích Sharp MZ.
 *
 * Implementuje čtení/zápis/mazání/přejmenování souborů, správu atributů,
 * čtení adresáře a alokační mapy na CP/M discích ve formátu Sharp MZ
 * (SD 9x512B a HD 18x512B). Pracuje nad nízkoúrovňovou DSK vrstvou
 * (dsk.h / dsk_tools.h).
 *
 * CP/M používá logické 128B sektory. Převod na fyzické sektory DSK obrazu
 * zajišťují interní pomocné funkce.
 *
 * Boot stopa (absolutní stopa 1 v DSK) je ve formátu FSMZ (16x256B) a
 * CP/M ji nepoužívá. Rezervované stopy (dpb.off) jsou přeskakovány.
 *
 * Diskový kontext (st_MZDSK_DISC) se sdílí s ostatními knihovnami - je
 * definován v mzdsk_global.h. Typicky se inicializuje přes mzdsk_disc_open()
 * a poté se předá do CP/M i dalších knihoven.
 *
 * DPB (parametry formátu) se předává jako parametr, což umožňuje práci
 * s různými formáty na jednom disku.
 *
 * I/O operace: knihovna používá dsk_read_sector()/dsk_write_sector() přímo
 * (bez inverze), protože CP/M datové stopy (9x512B, 18x512B) mají
 * normální (neinvertovaná) data.
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


#ifndef MZDSK_CPM_H
#define MZDSK_CPM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "libs/mzdsk_global/mzdsk_global.h"


    /* ================================================================
     * CP/M formát
     * ================================================================ */

    /**
     * @brief Typ CP/M diskového formátu pro Sharp MZ.
     *
     * Rozlišuje podporované formáty lišící se počtem sektorů
     * na stopu a velikostí alokačního bloku. Všechny formáty
     * jsou 2-stranné (sides=2).
     */
    typedef enum en_MZDSK_CPM_FORMAT {
        MZDSK_CPM_FORMAT_SD = 0,      /**< SD formát (Lamač): 9x512B, 2KB bloky, OFF=4 */
        MZDSK_CPM_FORMAT_HD = 1,      /**< HD formát (Lucky-Soft): 18x512B, 4KB bloky, OFF=4 */
    } en_MZDSK_CPM_FORMAT;


    /* ================================================================
     * Souborové atributy
     * ================================================================ */

    /**
     * @brief Souborové atributy CP/M (bit 7 příponových bajtů).
     *
     * Atributy se ukládají v bitech 7 prvních tří bajtů přípony
     * v adresářové položce. Hodnoty lze kombinovat pomocí bitového OR.
     *
     * Mapování na adresářovou položku:
     * - R/O = bit 7 v ext[0]
     * - SYS = bit 7 v ext[1]
     * - ARC = bit 7 v ext[2]
     */
    typedef enum en_MZDSK_CPM_ATTR {
        MZDSK_CPM_ATTR_READ_ONLY = 0x01,  /**< Pouze pro čtení (bit 7 ext[0]) */
        MZDSK_CPM_ATTR_SYSTEM    = 0x02,  /**< Systémový soubor (bit 7 ext[1]) */
        MZDSK_CPM_ATTR_ARCHIVED  = 0x04,  /**< Archivovaný soubor (bit 7 ext[2]) */
    } en_MZDSK_CPM_ATTR;


    /* ================================================================
     * Disk Parameter Block (DPB)
     * ================================================================ */

    /**
     * @brief CP/M Disk Parameter Block - parametry geometrie CP/M disku.
     *
     * Obsahuje všechny parametry potřebné pro mapování logických
     * sektorů na fyzické a pro správu adresáře a alokačních bloků.
     *
     * Inicializace přes mzdsk_cpm_init_dpb() (preset SD/HD) nebo
     * mzdsk_cpm_init_dpb_custom() (libovolné parametry).
     *
     * @par Invarianty:
     * - block_size == 128 << bsh
     * - blm == (1 << bsh) - 1
     * - spt > 0
     * - off >= 2 (boot stopa + system stopa)
     */
    typedef struct st_MZDSK_CPM_DPB {
        uint16_t spt;           /**< Počet logických (128B) sektorů na stopu */
        uint8_t bsh;            /**< Block shift factor (log2(block_size/128)) */
        uint8_t blm;            /**< Block mask ((block_size/128) - 1) */
        uint8_t exm;            /**< Extent mask */
        uint16_t dsm;           /**< Celkový počet bloků - 1 */
        uint16_t drm;           /**< Počet adresářových položek - 1 */
        uint8_t al0;            /**< Alokační bitmapa adresáře - byte 0 */
        uint8_t al1;            /**< Alokační bitmapa adresáře - byte 1 */
        uint16_t cks;           /**< Velikost check vektoru */
        uint16_t off;           /**< Počet rezervovaných stop */
        uint16_t block_size;    /**< Velikost alokačního bloku v bajtech */
    } st_MZDSK_CPM_DPB;


    /* ================================================================
     * Adresářová položka
     * ================================================================ */

    /**
     * @brief Surová adresářová položka CP/M - 32 bajtů.
     *
     * Přesně odpovídá binární struktuře na disku. Jméno souboru
     * a přípona jsou doplněny mezerami (0x20).
     *
     * @par Invarianty:
     * - user == 0xE5 znamená smazanou položku
     * - user 0-15 jsou platné uživatelské čísla
     * - pro bloky < 256: alloc[] jsou 8-bitová čísla bloků (16 bloků)
     * - pro bloky >= 256: alloc[] jsou 16-bitová čísla (8 bloků, LE)
     */
    typedef struct st_MZDSK_CPM_DIRENTRY {
        uint8_t user;           /**< Číslo uživatele (0-15) nebo 0xE5 (smazáno) */
        uint8_t fname[8];       /**< Jméno souboru (doplněno mezerami) */
        uint8_t ext[3];         /**< Přípona souboru (doplněna mezerami, bit 7 = atributy) */
        uint8_t extent;         /**< Číslo extentu (nízký bajt) */
        uint8_t s1;             /**< Rezervováno (0) */
        uint8_t s2;             /**< Číslo extentu (vysoký bajt) */
        uint8_t rc;             /**< Počet záznamů (128B) v tomto extentu */
        uint8_t alloc[16];      /**< Alokační mapa bloků */
    } st_MZDSK_CPM_DIRENTRY;


    /** @brief Velikost jedné adresářové položky v bajtech. */
#define MZDSK_CPM_DIRENTRY_SIZE     32

    /** @brief Bajt označující smazanou adresářovou položku. */
#define MZDSK_CPM_DELETED_ENTRY     0xE5

    /** @brief Velikost jednoho logického CP/M záznamu v bajtech. */
#define MZDSK_CPM_RECORD_SIZE       128

    /** @brief Maximální počet záznamů v jednom extentu (pro EXM=0). */
#define MZDSK_CPM_RECORDS_PER_EXTENT 128

    /** @brief Maximální počet datových bajtů v jednom extentu (128 * 128). */
#define MZDSK_CPM_EXTENT_DATA_SIZE  16384


    /* ================================================================
     * Informace o souboru
     * ================================================================ */

    /**
     * @brief Rozparsovaná informace o CP/M souboru.
     *
     * Obsahuje jméno a příponu jako null-terminated řetězce (bez výplňových
     * mezer) a celkovou velikost souboru spočtenou ze všech extentů.
     *
     * @par Ownership:
     * - struktura je hodnotového typu, nevlastní žádnou dynamickou paměť
     */
    typedef struct st_MZDSK_CPM_FILE_INFO {
        uint8_t user;           /**< Číslo uživatele (0-15) */
        char filename[9];       /**< Jméno souboru, null-terminated, max 8 znaků */
        char extension[4];      /**< Přípona souboru, null-terminated, max 3 znaky */
        uint32_t size;          /**< Celková velikost souboru v bajtech */
    } st_MZDSK_CPM_FILE_INFO;


    /* ================================================================
     * Rozšířené informace o souboru
     * ================================================================ */

    /**
     * @brief Rozšířená informace o CP/M souboru včetně atributů.
     *
     * Obsahuje vše co st_MZDSK_CPM_FILE_INFO plus souborové atributy
     * (R/O, SYS, ARC), počet extentů a index první adresářové položky.
     *
     * @par Ownership:
     * - struktura je hodnotového typu, nevlastní žádnou dynamickou paměť
     */
    typedef struct st_MZDSK_CPM_FILE_INFO_EX {
        uint8_t user;              /**< Číslo uživatele (0-15) */
        char filename[9];          /**< Jméno souboru, null-terminated, max 8 znaků */
        char extension[4];         /**< Přípona souboru, null-terminated, max 3 znaky */
        uint32_t size;             /**< Celková velikost souboru v bajtech */
        uint8_t read_only;         /**< Atribut R/O (nenulový = nastavený) */
        uint8_t system;            /**< Atribut SYS (nenulový = nastavený) */
        uint8_t archived;          /**< Atribut ARC (nenulový = nastavený) */
        uint8_t extent_count;      /**< Počet extentů souboru */
        uint16_t first_dir_index;  /**< Index první adresářové položky */
    } st_MZDSK_CPM_FILE_INFO_EX;


    /* ================================================================
     * Statistika adresářových slotů
     * ================================================================ */

    /**
     * @brief Statistika obsazení adresářových slotů CP/M disku.
     *
     * Rozděluje adresářové sloty podle hodnoty user bajtu:
     * - used    - alokovaný slot v zobrazitelné user area (user ∈ 0-15).
     * - free    - volný slot (user == 0xE5).
     * - blocked - alokovaný slot mimo zobrazitelné user areas
     *             (user != 0xE5 a zároveň user > 15). BDOS tyto sloty
     *             považuje za obsazené, ale SEARCH je v aktivní user
     *             area nenajde, takže se neobjevují ve výpisu dir.
     *
     * @par Invarianty:
     * - total == used + free + blocked
     * - total == drm + 1
     */
    typedef struct st_MZDSK_CPM_DIR_STATS {
        uint16_t total;      /**< Celkový počet slotů (DRM+1). */
        uint16_t used;       /**< Sloty s user ∈ 0-15 (zobrazitelné). */
        uint16_t free;       /**< Sloty s user == 0xE5 (volné). */
        uint16_t blocked;    /**< Sloty s user != 0xE5 a user > 15 (blokované). */
    } st_MZDSK_CPM_DIR_STATS;


    /* ================================================================
     * Alokační bitmapa
     * ================================================================ */

    /**
     * @brief Alokační bitmapa CP/M disku.
     *
     * Obsahuje bitovou mapu obsazení bloků a souhrnné statistiky.
     * Maximální kapacita je 4096 bloků (512 bajtů * 8 bitů).
     *
     * Bit map[block/8] & (1 << (block%8)) je nenulový pro obsazený blok.
     *
     * @par Invarianty:
     * - total_blocks == used_blocks + free_blocks
     * - total_blocks == dsm + 1
     */
    typedef struct st_MZDSK_CPM_ALLOC_MAP {
        uint8_t map[512];       /**< Bitová mapa obsazení (max 4096 bloků) */
        uint16_t total_blocks;  /**< Celkový počet bloků (DSM+1) */
        uint16_t used_blocks;   /**< Počet obsazených bloků */
        uint16_t free_blocks;   /**< Počet volných bloků */
    } st_MZDSK_CPM_ALLOC_MAP;


    /* ================================================================
     * Veřejné API - inicializace a konfigurace
     * ================================================================ */


    /**
     * @brief Inicializuje DPB pro zadaný CP/M formát (preset).
     *
     * Vyplní strukturu st_MZDSK_CPM_DPB hodnotami odpovídajícími
     * zadanému formátu (SD nebo HD).
     *
     * @param[out] dpb Ukazatel na strukturu DPB k inicializaci. Nesmí být NULL.
     * @param[in]  format Typ CP/M formátu (SD nebo HD).
     *
     * @pre dpb != NULL
     * @post Všechna pole dpb jsou nastavena podle formátu.
     */
    extern void mzdsk_cpm_init_dpb ( st_MZDSK_CPM_DPB *dpb, en_MZDSK_CPM_FORMAT format );


    /**
     * @brief Inicializuje DPB s libovolnými (custom) parametry.
     *
     * Umožňuje konfiguraci pro nestandardní CP/M formáty (např. Lamačův
     * formát). Odvozené hodnoty (blm, block_size, cks) se vypočtou
     * automaticky z primárních parametrů.
     *
     * Odvozené hodnoty:
     * - block_size = 128 << bsh
     * - blm = (1 << bsh) - 1
     * - cks = (drm + 1) / 4
     *
     * @param[out] dpb Ukazatel na strukturu DPB k inicializaci. Nesmí být NULL.
     * @param[in]  spt Počet logických (128B) sektorů na stopu.
     * @param[in]  bsh Block shift factor (log2(block_size/128)), rozsah 3-7.
     * @param[in]  exm Extent mask.
     * @param[in]  dsm Celkový počet bloků - 1.
     * @param[in]  drm Počet adresářových položek - 1.
     * @param[in]  al0 Alokační bitmapa adresáře - byte 0.
     * @param[in]  al1 Alokační bitmapa adresáře - byte 1.
     * @param[in]  off Počet rezervovaných stop.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud dpb je NULL.
     * @return MZDSK_RES_FORMAT_ERROR pokud bsh je mimo rozsah 3-7.
     *
     * @pre dpb != NULL
     * @pre bsh v rozsahu 3-7 (block_size 1024-16384 B)
     * @post Všechna pole dpb jsou nastavena včetně odvozených hodnot.
     */
    extern en_MZDSK_RES mzdsk_cpm_init_dpb_custom ( st_MZDSK_CPM_DPB *dpb, uint16_t spt,
                                                      uint8_t bsh, uint8_t exm, uint16_t dsm,
                                                      uint16_t drm, uint8_t al0, uint8_t al1,
                                                      uint16_t off );


    /**
     * @brief Ověří konzistenci parametrů DPB.
     *
     * Kontroluje, zda:
     * - spt > 0
     * - bsh je v rozsahu 3-7
     * - block_size == 128 << bsh
     * - blm == (1 << bsh) - 1
     * - dsm > 0
     * - block_size <= 2048 (omezení interních bufferů)
     *
     * @param[in]  dpb Ukazatel na strukturu DPB. Nesmí být NULL.
     *
     * @return MZDSK_RES_OK pokud je DPB konzistentní.
     * @return MZDSK_RES_INVALID_PARAM pokud dpb je NULL.
     * @return MZDSK_RES_FORMAT_ERROR pokud DPB obsahuje nekonzistentní hodnoty.
     */
    extern en_MZDSK_RES mzdsk_cpm_validate_dpb ( const st_MZDSK_CPM_DPB *dpb );


    /**
     * @brief Vrátí textový popis chybového kódu.
     *
     * Obaluje mzdsk_get_error() z mzdsk_global.h. Vrací anglický
     * popis chybového kódu en_MZDSK_RES.
     *
     * @param[in]  res Návratový kód operace.
     * @return Ukazatel na statický řetězec s popisem chyby. Nikdy nevrací NULL.
     */
    extern const char* mzdsk_cpm_strerror ( en_MZDSK_RES res );


    /* ================================================================
     * Veřejné API - adresářové operace
     * ================================================================ */


    /**
     * @brief Přečte surové 32B adresářové položky z disku.
     *
     * Načte všechny adresářové bloky určené bitovou mapou AL0/AL1 v DPB
     * a vrátí pole surových 32B adresářových položek (včetně smazaných).
     *
     * @param[in]  disc Kontext disku (sdílený z mzdsk_global). Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[out] entries Výstupní pole pro adresářové položky. Nesmí být NULL.
     * @param[in]  max_entries Maximální počet položek ve výstupním poli.
     * @return Počet přečtených položek (>= 0), nebo -1 při chybě I/O.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && entries != NULL && max_entries > 0
     * @post entries[0..retval-1] obsahují surové adresářové položky
     */
    extern int mzdsk_cpm_read_raw_directory ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                               st_MZDSK_CPM_DIRENTRY *entries, int max_entries );


    /**
     * @brief Přečte adresář CP/M disku a vrátí seznam platných souborů.
     *
     * Načte všechny adresářové bloky, zparsuje 32B položky a sloučí
     * extenty patřící ke stejnému souboru. Smazané a neplatné položky
     * jsou ignorovány.
     *
     * Ve výstupním poli je každý soubor uveden pouze jednou
     * s celkovou velikostí.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[out] files Výstupní pole pro informace o souborech. Nesmí být NULL.
     * @param[in]  max_files Maximální počet souborů ve výstupním poli.
     * @return Počet nalezených platných souborů (>= 0), nebo -1 při chybě I/O.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && files != NULL && max_files > 0
     * @post files[0..retval-1] obsahují informace o nalezených souborech
     */
    extern int mzdsk_cpm_read_directory ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                           st_MZDSK_CPM_FILE_INFO *files, int max_files );


    /**
     * @brief Rozšířený výpis adresáře s atributy a počtem extentů.
     *
     * Funguje jako mzdsk_cpm_read_directory(), ale vrací rozšířené
     * informace včetně souborových atributů (R/O, SYS, ARC), počtu
     * extentů a indexu první adresářové položky.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[out] files Výstupní pole pro rozšířené informace o souborech. Nesmí být NULL.
     * @param[in]  max_files Maximální počet souborů ve výstupním poli.
     * @return Počet nalezených platných souborů (>= 0), nebo -1 při chybě I/O.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && files != NULL && max_files > 0
     * @post files[0..retval-1] obsahují rozšířené informace o nalezených souborech
     */
    extern int mzdsk_cpm_read_directory_ex ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                              st_MZDSK_CPM_FILE_INFO_EX *files, int max_files );


    /**
     * @brief Spočítá statistiku obsazení adresářových slotů.
     *
     * Přečte surový adresář a rozdělí sloty do tří kategorií:
     * used (user 0-15), free (0xE5), blocked (ostatní hodnoty user).
     *
     * @param[in]  disc  Kontext disku. Nesmí být NULL.
     * @param[in]  dpb   Disk Parameter Block. Nesmí být NULL.
     * @param[out] stats Výstupní struktura. Nesmí být NULL.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód I/O.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && stats != NULL
     * @post stats->total == stats->used + stats->free + stats->blocked
     * @post stats->total == dpb->drm + 1
     */
    extern en_MZDSK_RES mzdsk_cpm_get_dir_stats ( st_MZDSK_DISC *disc,
                                                    const st_MZDSK_CPM_DPB *dpb,
                                                    st_MZDSK_CPM_DIR_STATS *stats );


    /**
     * @brief Zapíše jednu adresářovou položku na disk na správnou pozici.
     *
     * Vypočítá, ve kterém adresářovém bloku a na jakém offsetu se
     * položka s daným indexem nachází, a zapíše ji.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[in]  entry_index Index adresářové položky (0-based).
     * @param[in]  entry Adresářová položka k zápisu. Nesmí být NULL.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc != NULL && dpb != NULL && entry != NULL
     * @pre entry_index <= dpb->drm
     *
     * @par Vedlejší efekty:
     * - Modifikuje adresářový blok na disku.
     */
    extern en_MZDSK_RES mzdsk_cpm_write_dir_entry ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                      int entry_index,
                                                      const st_MZDSK_CPM_DIRENTRY *entry );


    /**
     * @brief Inicializuje prázdný CP/M adresář (vyplní 0xE5).
     *
     * Zapíše všechny adresářové bloky (určené AL0/AL1) hodnotou 0xE5,
     * čímž vytvoří prázdný adresář. Používá se při formátování disku.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL
     *
     * @par Vedlejší efekty:
     * - Přepíše všechny adresářové bloky na disku.
     * - Všechny soubory v adresáři budou ztraceny.
     */
    extern en_MZDSK_RES mzdsk_cpm_format_directory ( st_MZDSK_DISC *disc,
                                                      const st_MZDSK_CPM_DPB *dpb );


    /* ================================================================
     * Veřejné API - souborové operace
     * ================================================================ */


    /**
     * @brief Kontrola existence souboru na CP/M disku.
     *
     * Vyhledá soubor podle jména, přípony a uživatelského čísla
     * v adresáři disku.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[in]  filename Jméno souboru (max 8 znaků). Nesmí být NULL.
     * @param[in]  ext Přípona souboru (max 3 znaky). Nesmí být NULL.
     * @param[in]  user Číslo uživatele (0-15).
     * @return 1 pokud soubor existuje, 0 pokud neexistuje, -1 při chybě.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && filename != NULL && ext != NULL
     */
    extern int mzdsk_cpm_file_exists ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                        const char *filename, const char *ext, uint8_t user );


    /**
     * @brief Přečte obsah souboru z CP/M disku do paměti.
     *
     * Vyhledá soubor podle jména, přípony a uživatelského čísla.
     * Načte všechny extenty souboru ve správném pořadí a zkopíruje
     * data do výstupního bufferu.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[in]  filename Jméno souboru (max 8 znaků). Nesmí být NULL.
     * @param[in]  ext Přípona souboru (max 3 znaky). Nesmí být NULL.
     * @param[in]  user Číslo uživatele (0-15).
     * @param[out] buffer Výstupní buffer pro data souboru. Nesmí být NULL.
     * @param[in]  buffer_size Velikost výstupního bufferu v bajtech.
     * @param[out] bytes_read Počet skutečně přečtených bajtů. Může být NULL.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && filename != NULL && ext != NULL && buffer != NULL
     * @post Při úspěchu buffer obsahuje data souboru a *bytes_read je velikost dat.
     */
    extern en_MZDSK_RES mzdsk_cpm_read_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                const char *filename, const char *ext,
                                                uint8_t user, void *buffer, uint32_t buffer_size,
                                                uint32_t *bytes_read );


    /**
     * @brief Zapíše soubor na CP/M disk.
     *
     * Vytvoří potřebný počet adresářových položek (extentů) a alokuje
     * datové bloky pro uložení dat. Pokud soubor se stejným jménem,
     * příponou a uživatelem již existuje, vrátí chybu MZDSK_RES_FILE_EXISTS.
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
    extern en_MZDSK_RES mzdsk_cpm_write_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                 const char *filename, const char *ext,
                                                 uint8_t user, const void *data, uint32_t data_size );


    /**
     * @brief Smaže soubor z CP/M disku.
     *
     * Najde všechny adresářové položky (extenty) odpovídající zadanému
     * jménu, příponě a uživateli a označí je jako smazané (user = 0xE5).
     * Datové bloky se neuvolňují explicitně - jsou uvolněny implicitně
     * tím, že na ně neodkazuje žádná adresářová položka.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[in]  filename Jméno souboru (max 8 znaků). Nesmí být NULL.
     * @param[in]  ext Přípona souboru (max 3 znaky). Nesmí být NULL.
     * @param[in]  user Číslo uživatele (0-15).
     * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_NOT_FOUND pokud neexistuje.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && filename != NULL && ext != NULL
     * @post Všechny extenty souboru mají user == 0xE5.
     *
     * @par Vedlejší efekty:
     * - Modifikuje adresářové bloky na disku.
     */
    extern en_MZDSK_RES mzdsk_cpm_delete_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                  const char *filename, const char *ext,
                                                  uint8_t user );


    /**
     * @brief Přejmenuje soubor na CP/M disku.
     *
     * Najde všechny extenty starého souboru a změní jim jméno a příponu
     * na nové hodnoty. Zachovává souborové atributy, uživatelské číslo
     * a alokační data.
     *
     * Pokud cílové jméno již existuje, vrátí MZDSK_RES_FILE_EXISTS.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[in]  old_name Současné jméno souboru (max 8 znaků). Nesmí být NULL.
     * @param[in]  old_ext Současná přípona souboru (max 3 znaky). Nesmí být NULL.
     * @param[in]  user Číslo uživatele (0-15).
     * @param[in]  new_name Nové jméno souboru (max 8 znaků). Nesmí být NULL.
     * @param[in]  new_ext Nová přípona souboru (max 3 znaky). Nesmí být NULL.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL
     * @pre old_name != NULL && old_ext != NULL
     * @pre new_name != NULL && new_ext != NULL
     * @post Všechny extenty souboru mají nové jméno a příponu.
     *
     * @par Vedlejší efekty:
     * - Modifikuje adresářové bloky na disku.
     */
    extern en_MZDSK_RES mzdsk_cpm_rename_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                  const char *old_name, const char *old_ext,
                                                  uint8_t user,
                                                  const char *new_name, const char *new_ext );


    /**
     * @brief Změní user number existujícího souboru na CP/M disku.
     *
     * Najde všechny extenty souboru v aktuální user oblasti (old_user)
     * a přepíše jim user byte na new_user. Ostatní pole (jméno, přípona,
     * atributy, alokační data) se zachovávají.
     *
     * Před modifikací se ověří, že v cílové user oblasti neexistuje soubor
     * stejného jména a přípony - v takovém případě vrací MZDSK_RES_FILE_EXISTS.
     *
     * V CP/M 2.2 (včetně Sharp P-CP/M) je každá user oblast izolovaný
     * namespace, proto se kontroluje duplicita pouze v rámci new_user.
     * User 0 není globálně speciální - to je feature až CP/M 3.0.
     *
     * Pokud old_user == new_user, funkce je no-op a vrací MZDSK_RES_OK.
     *
     * @param[in]  disc     Kontext disku (RW). Nesmí být NULL.
     * @param[in]  dpb      Disk Parameter Block. Nesmí být NULL.
     * @param[in]  filename Jméno souboru (max 8 znaků). Nesmí být NULL.
     * @param[in]  ext      Přípona souboru (max 3 znaky). Nesmí být NULL.
     * @param[in]  old_user Současné číslo uživatele (0-15).
     * @param[in]  new_user Nové číslo uživatele (0-15).
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud je new_user mimo 0-15 nebo NULL parametr.
     * @return MZDSK_RES_FILE_NOT_FOUND pokud soubor v old_user neexistuje.
     * @return MZDSK_RES_FILE_EXISTS pokud v new_user už stejné jméno existuje.
     * @return MZDSK_RES_DSK_ERROR při chybě I/O.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && filename != NULL && ext != NULL
     * @post Všechny extenty souboru mají user == new_user.
     *
     * @par Vedlejší efekty:
     * - Modifikuje adresářové bloky na disku.
     */
    extern en_MZDSK_RES mzdsk_cpm_set_user ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                               const char *filename, const char *ext,
                                               uint8_t old_user, uint8_t new_user );


    /* ================================================================
     * Veřejné API - atributy
     * ================================================================ */


    /**
     * @brief Nastaví souborové atributy (R/O, SYS, ARC).
     *
     * Nastaví zadané atributy na všech extentech souboru.
     * Předchozí atributy se přepíšou - nepoužívá se OR, ale přímé
     * nastavení. Pro přidání atributu je nutné nejdříve přečíst
     * stávající přes mzdsk_cpm_get_attributes() a provést OR.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[in]  filename Jméno souboru (max 8 znaků). Nesmí být NULL.
     * @param[in]  ext Přípona souboru (max 3 znaky). Nesmí být NULL.
     * @param[in]  user Číslo uživatele (0-15).
     * @param[in]  attributes Kombinace en_MZDSK_CPM_ATTR (bitový OR).
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && filename != NULL && ext != NULL
     * @post Všechny extenty souboru mají nastavené zadané atributy.
     *
     * @par Vedlejší efekty:
     * - Modifikuje adresářové bloky na disku.
     */
    extern en_MZDSK_RES mzdsk_cpm_set_attributes ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                     const char *filename, const char *ext,
                                                     uint8_t user, uint8_t attributes );


    /**
     * @brief Zjistí souborové atributy (R/O, SYS, ARC).
     *
     * Přečte atributy z první adresářové položky (extentu) souboru.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[in]  filename Jméno souboru (max 8 znaků). Nesmí být NULL.
     * @param[in]  ext Přípona souboru (max 3 znaky). Nesmí být NULL.
     * @param[in]  user Číslo uživatele (0-15).
     * @param[out] attributes Výstup: kombinace en_MZDSK_CPM_ATTR. Nesmí být NULL.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && filename != NULL && ext != NULL && attributes != NULL
     * @post Při úspěchu *attributes obsahuje aktuální atributy souboru.
     */
    extern en_MZDSK_RES mzdsk_cpm_get_attributes ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                     const char *filename, const char *ext,
                                                     uint8_t user, uint8_t *attributes );


    /* ================================================================
     * Veřejné API - alokační mapa a volné místo
     * ================================================================ */


    /**
     * @brief Sestaví alokační bitmapu disku.
     *
     * Projde všechny platné adresářové položky, sestaví bitovou mapu
     * obsazených bloků (včetně adresářových) a spočítá statistiky.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @param[out] alloc_map Výstupní alokační mapa. Nesmí být NULL.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL && alloc_map != NULL
     * @post Při úspěchu alloc_map obsahuje kompletní alokační mapu.
     */
    extern en_MZDSK_RES mzdsk_cpm_get_alloc_map ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                    st_MZDSK_CPM_ALLOC_MAP *alloc_map );


    /**
     * @brief Spočítá volné místo na CP/M disku v bajtech.
     *
     * Projde všechny platné adresářové položky, sestaví bitmapu
     * obsazených alokačních bloků a spočítá volné bloky.
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @param[in]  dpb Disk Parameter Block. Nesmí být NULL.
     * @return Volné místo v bajtech, nebo 0 při chybě.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL
     */
    extern uint32_t mzdsk_cpm_free_space ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb );


    /* ================================================================
     * Veřejné API - detekce formátu
     * ================================================================ */


    /**
     * @brief Detekuje CP/M formát z identifikovaného formátu disku.
     *
     * Převede en_DSK_TOOLS_IDENTFORMAT na en_MZDSK_CPM_FORMAT.
     * Podporuje formáty MZCPM (SD) a MZCPMHD (HD).
     *
     * @param[in]  disc Kontext disku. Nesmí být NULL.
     * @return Detekovaný CP/M formát (SD nebo HD).
     *
     * @pre disc != NULL
     * @note Pokud formát nelze rozpoznat, vrátí MZDSK_CPM_FORMAT_SD jako výchozí.
     */
    extern en_MZDSK_CPM_FORMAT mzdsk_cpm_detect_format ( const st_MZDSK_DISC *disc );


    /* ================================================================
     * Nízkoúrovňové blokové I/O
     * ================================================================ */

    /**
     * @brief Převede číslo CP/M alokačního bloku na fyzickou adresu.
     *
     * Vypočítá absolutní stopu, fyzický sektor a offset v rámci sektoru
     * pro zadaný alokační blok na základě DPB parametrů.
     *
     * @param[in]  dpb     Disk Parameter Block. Nesmí být NULL.
     * @param[in]  block   Číslo alokačního bloku (0 .. dsm).
     * @param[out] track   Výstup: absolutní číslo stopy v DSK obrazu.
     * @param[out] sector  Výstup: fyzický sektor na stopě (číslován od 1).
     * @param[out] offset  Výstup: bajtový offset v rámci sektoru.
     *
     * @pre dpb != NULL, track != NULL, sector != NULL, offset != NULL
     */
    extern void mzdsk_cpm_block_to_physical ( const st_MZDSK_CPM_DPB *dpb, uint16_t block,
                                               uint8_t *track, uint8_t *sector, uint16_t *offset );

    /**
     * @brief Přečte data z CP/M alokačního bloku.
     *
     * Postupně čte fyzické sektory pokrývající zadaný blok a kopíruje
     * data do výstupního bufferu.
     *
     * @param[in]  disc   Kontext disku. Nesmí být NULL.
     * @param[in]  dpb    Disk Parameter Block. Nesmí být NULL.
     * @param[in]  block  Číslo CP/M alokačního bloku.
     * @param[out] buffer Výstupní buffer pro data bloku.
     * @param[in]  size   Počet bajtů k přečtení (max dpb->block_size).
     *
     * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DSK_ERROR při chybě čtení.
     *
     * @pre disc != NULL && dpb != NULL && buffer != NULL
     * @pre size <= dpb->block_size
     */
    extern en_MZDSK_RES mzdsk_cpm_read_block ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                 uint16_t block, uint8_t *buffer, uint16_t size );

    /**
     * @brief Zapíše data do CP/M alokačního bloku.
     *
     * Postupně čte fyzické sektory, modifikuje příslušné části
     * a zapisuje je zpět (read-modify-write).
     *
     * @param[in] disc   Kontext disku. Nesmí být NULL.
     * @param[in] dpb    Disk Parameter Block. Nesmí být NULL.
     * @param[in] block  Číslo CP/M alokačního bloku.
     * @param[in] data   Zdrojová data k zápisu.
     * @param[in] size   Počet bajtů k zápisu (max dpb->block_size).
     *
     * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DSK_ERROR při chybě.
     *
     * @pre disc != NULL && dpb != NULL && data != NULL
     * @pre size <= dpb->block_size
     */
    extern en_MZDSK_RES mzdsk_cpm_write_block ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                  uint16_t block, const uint8_t *data, uint16_t size );


    /* ================================================================
     * Diagnostika konzistence
     * ================================================================ */


    /** @brief Maximální počet chybějících extentů ve výsledku. */
#define MZDSK_CPM_MAX_MISSING_EXTENTS 256


    /**
     * @brief Výsledek kontroly extentů CP/M souboru.
     *
     * Obsahuje čísla chybějících fyzických extentů (díry v posloupnosti
     * 0..max_extent). Pokud count == 0, soubor je konzistentní.
     */
    typedef struct st_MZDSK_CPM_EXTENT_CHECK {
        uint16_t missing[MZDSK_CPM_MAX_MISSING_EXTENTS]; /**< Čísla chybějících extentů */
        int count;                                         /**< Počet chybějících extentů */
    } st_MZDSK_CPM_EXTENT_CHECK;


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
     * @return 0 při úspěchu (i pokud jsou díry - ty jsou v result),
     *         -1 při chybě čtení adresáře.
     *
     * @pre disc != NULL && dpb != NULL && result != NULL
     * @post result->count obsahuje počet chybějících extentů.
     *       result->missing[0..count-1] obsahuje jejich čísla.
     */
    extern int mzdsk_cpm_check_extents ( st_MZDSK_DISC *disc,
                                          const st_MZDSK_CPM_DPB *dpb,
                                          const char *filename,
                                          const char *ext,
                                          uint8_t user,
                                          st_MZDSK_CPM_EXTENT_CHECK *result );


    /* ================================================================
     * Verze knihovny
     * ================================================================ */

    /* ================================================================
     * Defragmentace
     * ================================================================ */


    /**
     * @brief Typ callbacku pro hlášení průběhu defragmentace CP/M disku.
     *
     * Knihovní funkce mzdsk_cpm_defrag() volá tento callback s informačními
     * zprávami o průběhu operace. Zprávy jsou v angličtině a zakončené '\n'.
     *
     * @param message Textová zpráva o průběhu (nulou zakončený řetězec).
     * @param user_data Uživatelská data předaná při volání mzdsk_cpm_defrag().
     */
    typedef void (*mzdsk_cpm_defrag_cb_t) ( const char *message, void *user_data );


    /**
     * @brief Provede defragmentaci CP/M disku.
     *
     * Načte všechny soubory z CP/M disku do paměti (včetně atributů
     * a uživatelských čísel), zformátuje adresář a znovu zapíše
     * vše sekvenčně od prvního volného alokačního bloku bez mezer.
     *
     * Algoritmus:
     * 1. Přečte rozšířený adresář (read_directory_ex) a získá seznam souborů.
     * 2. Pro každý soubor načte data do paměťového bufferu (read_file).
     * 3. Zformátuje adresář (format_directory) - vyplní 0xE5.
     * 4. Sekvenčně zapíše všechny soubory zpět (write_file).
     * 5. Obnoví atributy souborů (set_attributes).
     *
     * Průběh operace je hlášen přes progress_cb (pokud není NULL).
     *
     * @param[in] disc        Kontext disku (RW). Nesmí být NULL.
     * @param[in] dpb         Disk Parameter Block. Nesmí být NULL.
     * @param[in] progress_cb Callback pro hlášení průběhu (může být NULL).
     * @param[in] cb_data     Uživatelská data pro callback (může být NULL).
     *
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     *
     * @pre disc != NULL && disc->handler != NULL
     * @pre dpb != NULL
     *
     * @post Při úspěchu jsou všechny soubory na disku uloženy sekvenčně
     *       bez mezer od prvního datového bloku. Atributy a uživatelská
     *       čísla jsou zachovány.
     *
     * @warning Defragmentace je destruktivní operace - při chybě uprostřed
     *          procesu mohou být data na disku nekonzistentní nebo ztracena.
     *
     * @note Funkce nepíše na stdout ani stderr. Veškeré informační zprávy
     *       jsou předávány přes progress_cb.
     */
    extern en_MZDSK_RES mzdsk_cpm_defrag ( st_MZDSK_DISC *disc,
                                             const st_MZDSK_CPM_DPB *dpb,
                                             mzdsk_cpm_defrag_cb_t progress_cb,
                                             void *cb_data );


    /** @brief Verze knihovny mzdsk_cpm. */
#define MZDSK_CPM_VERSION "2.6.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzdsk_cpm.
     * @return Statický řetězec s verzí (např. "2.0.0").
     */
    extern const char* mzdsk_cpm_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_CPM_H */
