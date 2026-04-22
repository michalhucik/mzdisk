/**
 * @file panel_fsmz.h
 * @brief FSMZ filesystem panel - adresářový listing, souborové operace a Maintenance tab.
 *
 * Zobrazuje obsah FSMZ adresáře (MZ-BASIC disk) - názvy souborů,
 * typy, velikosti, adresy a alokační bloky. Poskytuje operace
 * Get (export do MZF) a Put (import z MZF).
 *
 * Maintenance tab: Repair (přepočet DINFO), Defrag (defragmentace),
 * Format File Area (vyčistí adresář a bitmapu, zachová bootstrap).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_FSMZ_H
#define MZDISK_PANEL_FSMZ_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "config.h"


/** @brief Maximální počet souborů v FSMZ adresáři. */
#define PANEL_FSMZ_MAX_FILES 128


/**
 * @brief Jedna položka FSMZ adresáře (pro zobrazení v GUI).
 *
 * Obsahuje jak zobrazitelné (ASCII) jméno, tak originální Sharp MZ
 * jméno potřebné pro souborové operace (get/put/delete).
 */
typedef struct st_PANEL_FSMZ_FILE {
    uint8_t index;              /**< Pozice v adresáři (0-based). */
    uint8_t ftype;              /**< Souborový typ (01=OBJ, 02=BTX, 03=BSD, 05=BRD). */
    char name[20];              /**< Název souboru (ASCII, konvertovaný z Sharp MZ). */
    uint8_t mz_fname[FSMZ_FNAME_LENGTH]; /**< Originální jméno v Sharp MZ ASCII. */
    uint16_t size;              /**< Velikost v bajtech. */
    uint16_t start_addr;        /**< Startovací adresa (Z80). */
    uint16_t exec_addr;         /**< Spouštěcí adresa (Z80). */
    uint16_t block;             /**< Počáteční alokační blok. */
    bool locked;                /**< Příznak uzamčení. */
} st_PANEL_FSMZ_FILE;


/**
 * @brief Stav modálního dialogu Set (STRT/EXEC/ftype editace).
 *
 * Drží rozpracované hodnoty pro popup, který umožní uživateli upravit
 * fstrt, fexec a ftype existující položky v adresáři. Data souboru
 * zůstávají beze změny - upravuje se pouze directory entry.
 */
typedef struct st_PANEL_FSMZ_SET_ADDR {
    bool show;                  /**< Zobrazit popup. */
    int file_idx;               /**< Index cílového souboru v data->files[]. */
    int fstrt;                  /**< Editovaná start address (0-0xFFFF). */
    int fexec;                  /**< Editovaná exec address (0-0xFFFF). */
    int ftype;                  /**< Editovaný souborový typ (0x01-0xFF). */
} st_PANEL_FSMZ_SET_ADDR;


/**
 * @brief Datový model FSMZ directory panelu.
 */
typedef struct st_PANEL_FSMZ_DATA {
    bool is_loaded;                             /**< Data jsou naplněna. */
    int file_count;                             /**< Počet nalezených souborů. */
    st_PANEL_FSMZ_FILE files[PANEL_FSMZ_MAX_FILES]; /**< Pole souborů. */
    bool selected[PANEL_FSMZ_MAX_FILES];        /**< Multiselect: true = soubor je vybraný. */
    int detail_index;                           /**< Index pro detail panel (-1 = žádný). */
    bool has_error;                             /**< Příznak pro zobrazení chybového popupu. */
    char error_msg[512];                        /**< Text chybové hlášky. */
    st_PANEL_FSMZ_SET_ADDR set_addr;            /**< Stav dialogu Set (STRT/EXEC/ftype). */
} st_PANEL_FSMZ_DATA;


/**
 * @brief Naplní datový model FSMZ adresáře z otevřeného disku.
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený disk s FSMZ filesystémem.
 */
extern void panel_fsmz_load ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc );


/**
 * @brief Exportuje soubor z FSMZ disku jako MZF soubor.
 *
 * Přečte data z FSMZ bloků, vytvoří MZF hlavičku a uloží
 * kompletní MZF soubor (128B hlavička + tělo) na lokální disk.
 *
 * @param disc Otevřený disk s FSMZ filesystémem.
 * @param file Položka adresáře k exportu.
 * @param mzf_path Cesta k výstupnímu MZF souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc je platně otevřený, file odpovídá existující položce.
 */
extern en_MZDSK_RES panel_fsmz_get_file ( st_MZDSK_DISC *disc, const st_PANEL_FSMZ_FILE *file, const char *mzf_path );


/**
 * @brief Importuje MZF soubor na FSMZ disk.
 *
 * Načte MZF soubor z lokálního disku, přečte hlavičku a tělo
 * a zapíše soubor do FSMZ filesystému.
 *
 * @param disc Otevřený disk s FSMZ filesystémem.
 * @param mzf_path Cesta ke vstupnímu MZF souboru.
 * @return MZDSK_RES_OK při úspěchu,
 *         MZDSK_RES_FILE_EXIST pokud soubor již existuje,
 *         MZDSK_RES_DISC_FULL / MZDSK_RES_NO_SPACE při nedostatku místa,
 *         jinak chybový kód.
 *
 * @pre disc je platně otevřený FSMZ disk.
 */
extern en_MZDSK_RES panel_fsmz_put_file ( st_MZDSK_DISC *disc, const char *mzf_path );


/**
 * @brief Smaže soubor z FSMZ disku.
 *
 * @param disc Otevřený disk s FSMZ filesystémem.
 * @param file Položka adresáře ke smazání.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_fsmz_delete_file ( st_MZDSK_DISC *disc, const st_PANEL_FSMZ_FILE *file );


/**
 * @brief Přejmenuje soubor na FSMZ disku.
 *
 * @param disc Otevřený disk s FSMZ filesystémem.
 * @param file Položka adresáře k přejmenování.
 * @param new_ascii_name Nové jméno v ASCII.
 * @return MZDSK_RES_OK při úspěchu,
 *         MZDSK_RES_FILE_EXISTS pokud nové jméno již existuje,
 *         jinak chybový kód.
 */
extern en_MZDSK_RES panel_fsmz_rename_file ( st_MZDSK_DISC *disc, const st_PANEL_FSMZ_FILE *file, const char *new_ascii_name );


/**
 * @brief Nastaví příznak uzamčení souboru na FSMZ disku.
 *
 * @param disc Otevřený disk s FSMZ filesystémem.
 * @param file Položka adresáře.
 * @param locked true = zamknout, false = odemknout.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_fsmz_lock_file ( st_MZDSK_DISC *disc, const st_PANEL_FSMZ_FILE *file, bool locked );


/**
 * @brief Aktualizuje STRT, EXEC a ftype directory položky souboru.
 *
 * Wrapper nad fsmz_set_addr - provádí jen metadata update, data souboru
 * zůstávají beze změny. Ctí lock flag (force=0).
 *
 * @param disc Otevřený disk s FSMZ filesystémem.
 * @param file Položka určující cílový soubor (použije mz_fname).
 * @param fstrt Nová start address (0x0000-0xFFFF).
 * @param fexec Nová exec address (0x0000-0xFFFF).
 * @param ftype Nový typ souboru (0x01-0xFF; 0x00 není povolené).
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_LOCKED pokud je
 *         soubor uzamčen, jinak chybový kód.
 */
extern en_MZDSK_RES panel_fsmz_set_addr ( st_MZDSK_DISC *disc,
                                           const st_PANEL_FSMZ_FILE *file,
                                           uint16_t fstrt, uint16_t fexec,
                                           uint8_t ftype );


/**
 * @brief Vrátí textový popis souborového typu FSMZ.
 *
 * @param ftype Kód typu (0x01=OBJ, 0x02=BTX, 0x03=BSD, 0x05=BRD).
 * @return Statický řetězec.
 */
extern const char* panel_fsmz_type_str ( uint8_t ftype );


/**
 * @brief Vykreslí FSMZ directory panel (ImGui rendering).
 *
 * Zobrazuje tabulku souborů, detail vybraného souboru a tlačítka
 * Get (export MZF) / Put (import MZF). Po úspěšném Put reloaduje
 * adresář a nastaví *is_dirty = true.
 *
 * @param data Datový model.
 * @param disc Otevřený disk (pro get/put operace).
 * @param is_dirty Ukazatel na dirty flag session (nastaví se při Put).
 * @param cfg Konfigurace aplikace (last_get_dir, last_put_dir).
 */
extern void panel_fsmz_render ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc, bool *is_dirty, st_MZDISK_CONFIG *cfg, uint64_t owner_session_id );


/**
 * @brief Vykreslí FSMZ Maintenance tab (ImGui rendering).
 *
 * Poskytuje operace údržby FSMZ disku:
 * - Repair: přepočet DINFO bitmapy z adresáře a bootstrapu
 * - Defrag: defragmentace souborové oblasti (s progress callbackem)
 * - Format (File Area): vyčistí adresář a bitmapu, zachová bootstrap
 *
 * Každá operace má potvrzovací dialog s popisem důsledků.
 *
 * @param data Datový model (pro reload po operacích).
 * @param disc Otevřený disk. Nesmí být NULL.
 * @param is_dirty Dirty flag (nastavuje se při modifikaci disku).
 */
extern void panel_fsmz_maintenance_render ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc, bool *is_dirty );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_FSMZ_H */
