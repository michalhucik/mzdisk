/**
 * @file panel_hexdump.h
 * @brief Hexdump panel - prohlížeč sektorových/blokových dat.
 *
 * Zobrazuje hexdump vybraného sektoru nebo bloku disku.
 * Podporuje dva režimy adresování:
 * - Track/Sector: přímá navigace stopou a sektorem (1 sektor)
 * - Block: univerzální blokové adresování s konfigurovatelným
 *   počátkem (origin track/sector), prvním číslem bloku a počtem
 *   sektorů na blok. Umožňuje pokrýt libovolný FS (FSMZ, CP/M, MRS).
 *
 * V blokovém režimu je konfigurovatelné pořadí procházení sektorů:
 * - ID: sekvenční postup podle sector ID (1, 2, 3, ...), přeskakuje
 *   nesekvenční ID - emuluje chování řadiče disketové jednotky.
 * - Phys: postup podle fyzické pozice v DSK obrazu (sinfo[] pole),
 *   zachycuje všechny sektory včetně nesekvenčních.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_HEXDUMP_H
#define MZDISK_PANEL_HEXDUMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"

/* Forward deklarace pro Raw I/O datový model */
struct st_PANEL_RAW_IO_DATA;


/** @brief Maximální velikost jednoho sektoru v bajtech. */
#define PANEL_HEXDUMP_MAX_SECTOR_SIZE 1024

/** @brief Maximální počet sektorů na blok. */
#define PANEL_HEXDUMP_MAX_SECTORS_PER_BLOCK 8

/**
 * @brief Maximální velikost datového bufferu hexdumpu.
 *
 * Musí pojmout celý blok (max_sectors_per_block * max_sector_size).
 */
#define PANEL_HEXDUMP_MAX_DATA ( PANEL_HEXDUMP_MAX_SECTOR_SIZE * PANEL_HEXDUMP_MAX_SECTORS_PER_BLOCK )


/**
 * @brief Režim zobrazení znaků v textovém sloupci hexdumpu.
 *
 * Určuje, jakým způsobem se převádějí bajty na zobrazitelné znaky
 * v ASCII sloupci hexdumpu.
 */
typedef enum en_HEXDUMP_CHARSET {
    HEXDUMP_CHARSET_RAW = 0,            /**< Raw bajty - printable ASCII (0x20-0x7E), ostatní '.'. */
    HEXDUMP_CHARSET_SHARPMZ_EU_ASCII,   /**< Sharp MZ EU -> ASCII (sharpmz_convert_to_ASCII). */
    HEXDUMP_CHARSET_SHARPMZ_JP_ASCII,   /**< Sharp MZ JP -> ASCII (sharpmz_jp_convert_to_ASCII). */
    HEXDUMP_CHARSET_SHARPMZ_EU_UTF8,    /**< Sharp MZ EU -> UTF-8. */
    HEXDUMP_CHARSET_SHARPMZ_JP_UTF8,    /**< Sharp MZ JP -> UTF-8. */
    HEXDUMP_CHARSET_SHARPMZ_EU_CG1,     /**< Sharp MZ EU ASCII -> video kód -> mzglyphs EU sada 1. */
    HEXDUMP_CHARSET_SHARPMZ_EU_CG2,     /**< Sharp MZ EU ASCII -> video kód -> mzglyphs EU sada 2. */
    HEXDUMP_CHARSET_SHARPMZ_JP_CG1,     /**< Sharp MZ JP ASCII -> video kód -> mzglyphs JP sada 1. */
    HEXDUMP_CHARSET_SHARPMZ_JP_CG2,     /**< Sharp MZ JP ASCII -> video kód -> mzglyphs JP sada 2. */
} en_HEXDUMP_CHARSET;


/**
 * @brief Režim adresování hexdumpu.
 *
 * Určuje, jakým způsobem uživatel naviguje na data na disku.
 */
typedef enum en_HEXDUMP_ADDR_MODE {
    HEXDUMP_ADDR_TRACK_SECTOR = 0,  /**< Přímé adresování stopou a sektorem. */
    HEXDUMP_ADDR_BLOCK,             /**< Blokové adresování s konfigurovatelným počátkem. */
} en_HEXDUMP_ADDR_MODE;


/**
 * @brief Pořadí procházení sektorů při sekvenčním čtení.
 *
 * Určuje, jak se postupuje od jednoho sektoru k dalšímu
 * při blokovém adresování (a budoucím raw importu/exportu).
 *
 * Na standardních Sharp MZ discích (sekvenční ID 1..N) se obě
 * varianty chovají identicky. Rozdíl nastává u disků s nesekvenčními
 * ID sektorů (např. LEMMINGS: sektory 1-9 + 22 na jedné stopě).
 */
typedef enum en_HEXDUMP_SECTOR_ORDER {
    HEXDUMP_SECTOR_ORDER_ID = 0,    /**< Podle ID - jako řadič: hledá sektor s ID+1 na stopě,
                                         pokud neexistuje, přejde na další stopu od ID 1. */
    HEXDUMP_SECTOR_ORDER_PHYS,      /**< Podle fyzické pozice: postupuje v pořadí uložení
                                         sektorů v DSK obrazu (sinfo[] pole). */
} en_HEXDUMP_SECTOR_ORDER;


/**
 * @brief Konfigurace blokového adresování.
 *
 * Definuje počátek blokového prostoru (origin track/sector),
 * číslo prvního bloku na tomto počátku a kolik fyzických sektorů
 * tvoří jeden blok.
 *
 * @par Příklady:
 * - Celý disk (FSMZ):  origin_track=0, origin_sector=1, first_block=0, sectors_per_block=1
 * - CP/M SD bloky:     origin_track=4, origin_sector=1, first_block=0, sectors_per_block=2
 * - CP/M HD bloky:     origin_track=4, origin_sector=1, first_block=0, sectors_per_block=4
 * - MRS celý disk:     origin_track=0, origin_sector=1, first_block=0, sectors_per_block=1
 */
typedef struct st_HEXDUMP_BLOCK_CONFIG {
    uint16_t origin_track;          /**< Stopa, kde začíná blok first_block. */
    uint16_t origin_sector;         /**< Sektor ID (1-based), kde začíná blok first_block. */
    int32_t first_block;            /**< Číslo přiřazené prvnímu bloku na origin pozici. */
    uint16_t sectors_per_block;     /**< Počet fyzických sektorů na jeden blok (1, 2, 4, ...). */
    en_HEXDUMP_SECTOR_ORDER sector_order; /**< Pořadí procházení sektorů (ID nebo Phys). */
} st_HEXDUMP_BLOCK_CONFIG;


/**
 * @brief Datový model hexdump panelu.
 *
 * Obsahuje aktuálně zobrazený sektor/blok, navigační stav,
 * konfiguraci blokového adresování, volby zobrazení
 * (inverze, kódování znaků) a editační stav.
 *
 * @par Invarianty:
 * - V režimu HEXDUMP_ADDR_TRACK_SECTOR: data_size <= PANEL_HEXDUMP_MAX_SECTOR_SIZE
 * - V režimu HEXDUMP_ADDR_BLOCK: data_size <= sectors_per_block * sector_size
 * - block_config.sectors_per_block >= 1
 * - Pokud edit_mode == true, undo_data obsahuje kopii dat před editací.
 * - edit_dirty == true znamená, že data[] != undo_data[] (neuložené změny).
 */
typedef struct st_PANEL_HEXDUMP_DATA {
    bool is_loaded;                 /**< Data jsou načtená. */
    uint16_t track;                 /**< Aktuální stopa (pro track/sector režim i jako výsledek konverze). */
    uint16_t sector;                /**< Aktuální sektor, 1-based (pro track/sector režim i jako výsledek konverze). */
    uint16_t max_track;             /**< Maximální číslo stopy. */
    uint16_t max_sector;            /**< Maximální počet sektorů na aktuální stopě. */
    uint16_t sector_size;           /**< Velikost sektoru na aktuální stopě v bajtech. */
    uint16_t data_size;             /**< Skutečná velikost načtených dat v bajtech. */
    uint8_t data[PANEL_HEXDUMP_MAX_DATA]; /**< Raw data (bez auto-inverze). */
    bool read_error;                /**< Chyba při čtení. */
    bool invert;                    /**< Invertovat data při zobrazení (XOR 0xFF). */
    en_HEXDUMP_CHARSET charset;     /**< Kódování znaků v textovém sloupci. */

    /* blokové adresování */
    en_HEXDUMP_ADDR_MODE addr_mode;     /**< Aktuální režim adresování. */
    int32_t block;                      /**< Aktuální číslo bloku (v blokovém režimu). */
    st_HEXDUMP_BLOCK_CONFIG block_config; /**< Konfigurace blokového adresování. */

    /* editační stav */
    bool edit_mode;                      /**< Editace je aktivní. */
    bool edit_dirty;                     /**< V data[] jsou neuložené změny oproti undo_data[]. */
    uint8_t undo_data[PANEL_HEXDUMP_MAX_DATA]; /**< Kopie dat před editací (undo buffer). */
    int cursor_pos;                      /**< Pozice kurzoru (byte index 0-based v rámci data[]). */
    bool cursor_in_ascii;                /**< Kurzor je v ASCII sloupci (jinak v hex sloupci). */
    bool cursor_high_nibble;             /**< V hex sloupci: editujeme horní nibble (true) nebo dolní (false). */
    bool pending_nav_discard;            /**< Čekáme na potvrzení zahození změn při navigaci. */

    /* indikace neúspěšné konverze znaku při ASCII editaci (round-trip) */
    bool edit_convert_error;             /**< Poslední zápis znaku selhal kvůli nekonvertibilitě v aktuální znakové sadě. */
    char edit_convert_error_msg[128];    /**< Popisná hláška pro zobrazení pod hexdumpem. */
} st_PANEL_HEXDUMP_DATA;


/**
 * @brief Inicializuje hexdump data - nastaví na stopu 0, sektor 1.
 *
 * Nastaví režim na Track/Sector a výchozí blokovou konfiguraci
 * (origin T0/S1, first_block=0, sectors_per_block=1).
 *
 * @param hd Datový model.
 * @param disc Otevřený disk (pro zjištění limitů).
 *
 * @pre hd != NULL, disc != NULL a otevřený.
 * @post hd je inicializován, první sektor je načtený.
 */
extern void panel_hexdump_init ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc );


/**
 * @brief Načte data z disku do datového modelu.
 *
 * V režimu Track/Sector čte jeden sektor přímo dle ID.
 * V režimu Block čte sectors_per_block po sobě jdoucích sektorů
 * počínaje sektorem odpovídajícím zvolenému bloku. Pořadí
 * procházení sektorů uvnitř bloku je dáno block_config.sector_order.
 *
 * @param hd Datový model (track/sector nebo block musí být nastavené).
 * @param disc Otevřený disk.
 *
 * @pre hd != NULL, disc != NULL a otevřený.
 * @post Při úspěchu: hd->is_loaded == true, hd->data a hd->data_size platné.
 *       Při chybě: hd->read_error == true.
 */
extern void panel_hexdump_read_sector ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc );


/**
 * @brief Přepočítá číslo bloku na fyzickou stopu a sektor.
 *
 * Na základě block_config a geometrie disku vypočte, na které
 * stopě a sektoru začíná zadaný blok. Postupuje od origin pozice
 * dle zvoleného pořadí sektorů (block_config.sector_order):
 * - ID: sekvenčně podle sector ID, přeskakuje nesekvenční.
 * - Phys: podle fyzické pozice v sinfo[] poli.
 *
 * @param hd Datový model s nastavenou block_config a block.
 * @param disc Otevřený disk (pro zjištění geometrie a sector ID mapy).
 * @param[out] out_track Výstupní stopa.
 * @param[out] out_sector Výstupní sektor ID (1-based).
 * @return true pokud je blok v platném rozsahu disku, false jinak.
 *
 * @pre hd != NULL, disc != NULL, out_track != NULL, out_sector != NULL.
 */
extern bool panel_hexdump_block_to_track_sector ( const st_PANEL_HEXDUMP_DATA *hd,
                                                    st_MZDSK_DISC *disc,
                                                    uint16_t *out_track,
                                                    uint16_t *out_sector );


/**
 * @brief Nastaví blokovou konfiguraci z detekovaného CP/M presetu.
 *
 * Nastaví origin na stopu dpb->off, sektor 1, first_block=0
 * a sectors_per_block dle dpb->block_size a velikosti fyzického sektoru.
 *
 * @param hd Datový model.
 * @param dpb CP/M DPB s parametry presetu.
 *
 * @pre hd != NULL, dpb != NULL.
 * @post block_config je nastavena pro CP/M blokové adresování.
 */
extern void panel_hexdump_preset_cpm ( st_PANEL_HEXDUMP_DATA *hd,
                                        const st_MZDSK_CPM_DPB *dpb );


/**
 * @brief Nastaví blokovou konfiguraci pro celý disk (1 blok = 1 sektor).
 *
 * Nastaví origin na T0/S1, first_block=0, sectors_per_block=1.
 *
 * @param hd Datový model.
 *
 * @pre hd != NULL.
 * @post block_config je nastavena pro lineární sektorové adresování.
 */
extern void panel_hexdump_preset_whole_disk ( st_PANEL_HEXDUMP_DATA *hd );


/**
 * @brief Zjistí počet sektorů a velikost sektoru na dané stopě.
 *
 * Exportovaná verze interní funkce pro použití z panel_raw_io.
 *
 * @param disc Otevřený disk.
 * @param track Číslo stopy.
 * @param[out] sectors Počet sektorů.
 * @param[out] sector_size Velikost sektoru v bajtech.
 *
 * @pre disc != NULL, sectors != NULL, sector_size != NULL.
 * @post *sectors a *sector_size obsahují parametry stopy,
 *       nebo 0/256 pokud stopa neexistuje.
 */
extern void panel_hexdump_get_track_params ( st_MZDSK_DISC *disc, uint16_t track,
                                              uint16_t *sectors, uint16_t *sector_size );


/**
 * @brief Posune pozici (track, sector_id) o zadaný počet sektorů vpřed.
 *
 * Exportovaná verze interní funkce pro použití z panel_raw_io.
 * Postup závisí na zvoleném pořadí sektorů (ID nebo Phys).
 *
 * @param disc Otevřený disk.
 * @param order Pořadí procházení sektorů.
 * @param[in,out] track Aktuální stopa, aktualizuje se.
 * @param[in,out] sector_id Aktuální sector ID, aktualizuje se.
 * @param count Počet sektorů k přeskočení.
 * @param max_track Maximální číslo stopy.
 * @return true pokud je výsledná pozice platná, false pokud přetekla konec disku.
 *
 * @pre disc != NULL, track != NULL, sector_id != NULL, count >= 0.
 * @post Při úspěchu: *track a *sector_id ukazují na platný sektor.
 */
extern bool panel_hexdump_advance_sectors ( st_MZDSK_DISC *disc, en_HEXDUMP_SECTOR_ORDER order,
                                             uint16_t *track, uint16_t *sector_id,
                                             int count, uint16_t max_track );


/**
 * @brief Aktivuje editační režim.
 *
 * Uloží kopii aktuálních dat do undo_data, nastaví edit_mode na true
 * a kurzor na pozici 0 v hex sloupci.
 *
 * @param hd Datový model.
 *
 * @pre hd != NULL, hd->is_loaded == true.
 * @post hd->edit_mode == true, hd->undo_data obsahuje kopii hd->data,
 *       hd->cursor_pos == 0, hd->edit_dirty == false.
 */
extern void panel_hexdump_enter_edit ( st_PANEL_HEXDUMP_DATA *hd );


/**
 * @brief Zruší editaci a obnoví data z undo bufferu.
 *
 * Zkopíruje undo_data zpět do data a vypne editační režim.
 *
 * @param hd Datový model.
 *
 * @pre hd != NULL, hd->edit_mode == true.
 * @post hd->edit_mode == false, hd->edit_dirty == false,
 *       hd->data == kopie původních dat.
 */
extern void panel_hexdump_revert_edit ( st_PANEL_HEXDUMP_DATA *hd );


/**
 * @brief Zapíše editovaná data zpět na disk.
 *
 * V režimu Track/Sector zapíše jeden sektor přes dsk_write_sector().
 * V režimu Block iteruje přes všechny sektory bloku (sectors_per_block)
 * a zapíše odpovídající části data[] bufferu. Respektuje sector_order
 * stejně jako panel_hexdump_read_sector().
 *
 * Po úspěšném zápisu aktualizuje undo_data a nastaví edit_dirty na false.
 *
 * @param hd Datový model s editovanými daty.
 * @param disc Otevřený disk.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DSK_ERROR při chybě.
 *
 * @pre hd != NULL, disc != NULL, hd->edit_mode == true, hd->is_loaded == true.
 * @post Při úspěchu: sektory na disku obsahují nová data,
 *       hd->undo_data == hd->data, hd->edit_dirty == false.
 */
extern en_MZDSK_RES panel_hexdump_write_data ( st_PANEL_HEXDUMP_DATA *hd,
                                                 st_MZDSK_DISC *disc );


/**
 * @brief Vykreslí hexdump panel (ImGui rendering).
 *
 * V editačním režimu zobrazuje kurzor, zvýrazňuje změněné bajty
 * a zpracovává klávesový vstup pro editaci hex/ASCII hodnot.
 *
 * @param hd Datový model.
 * @param disc Otevřený disk (pro čtení při navigaci a zápis při editaci).
 * @param detect Výsledek auto-detekce FS (pro presety blokového adresování, může být NULL).
 * @param raw_io_data Datový model Raw I/O okna (pro otevření Get/Put, může být NULL).
 * @param is_dirty Ukazatel na dirty flag session (Write operace ho nastaví na true, může být NULL).
 */
extern void panel_hexdump_render ( st_PANEL_HEXDUMP_DATA *hd,
                                    st_MZDSK_DISC *disc,
                                    const st_MZDSK_DETECT_RESULT *detect,
                                    struct st_PANEL_RAW_IO_DATA *raw_io_data,
                                    bool *is_dirty );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_HEXDUMP_H */
