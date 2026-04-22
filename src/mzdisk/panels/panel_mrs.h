/**
 * @file panel_mrs.h
 * @brief MRS filesystem panel - adresářový listing, souborové operace, FAT vizualizace a Maintenance tab.
 *
 * Zobrazuje obsah MRS adresáře - název, přípona, ID, velikost, adresy.
 * Poskytuje operace Get/Put, Delete a Rename.
 * Obsahuje surová FAT data a layout parametry pro vizualizaci FAT mapy
 * (per-file barvy, disk layout pruh, bloková mřížka).
 *
 * Maintenance tab: Defrag (defragmentace souborové oblasti s progress callbackem),
 * Format File Area (reinicializuje FAT a adresář, zachová systémovou oblast).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_MRS_H
#define MZDISK_PANEL_MRS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "config.h"


/** @brief Maximální počet souborů v MRS adresáři. */
#define PANEL_MRS_MAX_FILES 128


/**
 * @brief Jedna položka MRS adresáře (pro zobrazení v GUI).
 */
typedef struct st_PANEL_MRS_FILE {
    uint8_t index;              /**< Pozice v adresáři (0-based). */
    char name[12];              /**< Název souboru (ASCII). */
    char ext[4];                /**< Přípona (MRS/DAT/RAM/SCR). */
    uint8_t file_id;            /**< ID souboru ve FAT. */
    uint16_t bsize;             /**< Počet bloků. */
    uint32_t size_bytes;        /**< Velikost v bajtech (bsize * 512). */
    uint16_t start_addr;        /**< Start adresa. */
    uint16_t exec_addr;         /**< Exec adresa. */
} st_PANEL_MRS_FILE;


/**
 * @brief Datový model MRS directory panelu.
 *
 * Obsahuje adresářový listing i surová FAT data pro vizualizaci
 * FAT mapy. Layout parametry (fat_block, dir_block, ...) popisují
 * rozložení oblastí na disku.
 */
/**
 * @brief Parametry MZF exportu pro MRS (single-file options popup).
 *
 * Při Get MZF (sel_count == 1) se před file dialogem zobrazí options
 * popup s editací jména a adres. Hodnoty se pak použijí v MZF
 * hlavičce namísto hodnot z MRS directory entry.
 */
typedef struct st_PANEL_MRS_MZF_EXPORT {
    char name[18];              /**< Návrh jména ("FNAME.EXT" + NUL). */
    int fstrt;                  /**< Start address (0x0000-0xFFFF). */
    int fexec;                  /**< Exec address (0x0000-0xFFFF). */
    bool show_opts;             /**< Zobrazit options popup. */
    bool open_file_dlg;         /**< Po potvrzení options otevřít file dialog. */
    int file_idx;               /**< Index souboru v data->files[] (pro single-file export). */
} st_PANEL_MRS_MZF_EXPORT;


/**
 * @brief Stav modálního dialogu Set Addr (STRT/EXEC editace).
 */
typedef struct st_PANEL_MRS_SET_ADDR {
    bool show;                  /**< Zobrazit popup. */
    int file_idx;               /**< Index cílového souboru v data->files[]. */
    int fstrt;                  /**< Editovaná start address. */
    int fexec;                  /**< Editovaná exec address. */
} st_PANEL_MRS_SET_ADDR;


/**
 * @brief Stav resumovatelného bulk Get exportu v MRS panelu s podporou ASK.
 *
 * Shodná sémantika jako st_PANEL_CPM_BULK_GET. Při ASK + kolizi uloží
 * context a vyskočí; render popup zobrazí per-file i Apply-to-all akce.
 */
typedef struct st_PANEL_MRS_BULK_GET {
    bool active;                /**< Bulk operace je rozpracovaná. */
    bool is_mzf;                /**< false = raw bulk, true = MZF bulk. */
    int next_idx;               /**< Index v data->selected[], od kterého pokračovat. */
    int override_mode;          /**< -1 = per-file ASK, jinak en_MZDSK_EXPORT_DUP_MODE. */
    bool ask_pending;           /**< Čeká se na user rozhodnutí v popupu. */
    int conflict_idx;           /**< Index souboru, jehož cílová cesta koliduje. */
    char conflict_path[2048];   /**< Plná cesta konfliktního cíle. */
    char dirpath[2048];         /**< Cílový adresář. */
    int ok_count;
    int failed_count;
    int err_len;
    char errors[2048];
} st_PANEL_MRS_BULK_GET;


typedef struct st_PANEL_MRS_DATA {
    bool is_loaded;                                     /**< Data jsou naplněna. */
    int file_count;                                     /**< Počet souborů. */
    st_PANEL_MRS_FILE files[PANEL_MRS_MAX_FILES];       /**< Pole souborů. */
    bool selected[PANEL_MRS_MAX_FILES];                 /**< Multiselect. */
    int detail_index;                                   /**< Index pro detail (-1 = žádný). */
    uint16_t free_blocks;                               /**< Volných bloků. */
    uint8_t max_files;                                  /**< Celkových slotů. */
    bool has_error;                                     /**< Příznak chybového popupu. */
    char error_msg[2048];                               /**< Text chybové hlášky (větší buffer pro bulk
                                                             souhrny per-soubor). */
    st_PANEL_MRS_BULK_GET bulk_get;                     /**< Stav resumovatelného Get/Get MZF exportu (ASK mód). */
    st_PANEL_MRS_MZF_EXPORT mzf_export;                 /**< Parametry MZF single-file export dialogu (jméno, STRT, EXEC). */
    st_PANEL_MRS_SET_ADDR set_addr;                     /**< Stav dialogu Set Addr (update STRT/EXEC existujícího souboru). */

    /* FAT vizualizace */
    uint8_t fat_raw[FSMRS_COUNT_BLOCKS];                /**< Surová FAT data (kopie config->fat[]). */
    st_FSMRS_MAP_STATS fat_stats;                       /**< Statistiky alokační mapy. */
    uint16_t total_blocks;                              /**< Celkový počet bloků na disku. */
    uint16_t fat_block;                                 /**< Číslo prvního bloku FAT. */
    uint16_t fat_sectors;                               /**< Počet sektorů FAT. */
    uint16_t dir_block;                                 /**< Číslo prvního bloku adresáře. */
    uint16_t dir_sectors;                               /**< Počet sektorů adresáře. */
    uint16_t data_block;                                /**< Číslo prvního datového bloku. */
} st_PANEL_MRS_DATA;


/**
 * @brief Naplní datový model MRS adresáře.
 *
 * @param data Výstupní datový model.
 * @param detect Výsledek auto-detekce (obsahuje mrs_config).
 */
extern void panel_mrs_load ( st_PANEL_MRS_DATA *data, st_MZDSK_DETECT_RESULT *detect );


/**
 * @brief Exportuje soubor z MRS disku jako raw binárku.
 *
 * @param config MRS konfigurace.
 * @param file Položka adresáře k exportu.
 * @param output_path Cesta k výstupnímu souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_mrs_get_file ( st_FSMRS_CONFIG *config,
                                          const st_PANEL_MRS_FILE *file, const char *output_path );


/**
 * @brief Exportuje soubor z MRS disku jako MZF (typ OBJ, standardní hlavička).
 *
 * @param config MRS konfigurace.
 * @param file Položka adresáře k exportu.
 * @param output_path Cesta k výstupnímu MZF souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_mrs_get_file_mzf ( st_FSMRS_CONFIG *config,
                                               const st_PANEL_MRS_FILE *file, const char *output_path );


/**
 * @brief Exportuje soubor z MRS disku jako MZF s override metadat.
 *
 * Varianta panel_mrs_get_file_mzf, kde uživatel může přepsat jméno,
 * start a exec adresu MZF hlavičky. Data souboru se nemění - jen MZF
 * hlavička použije tyto hodnoty místo hodnot z MRS directory entry.
 *
 * @param config       MRS konfigurace.
 * @param file         Položka adresáře.
 * @param output_path  Cesta k výstupnímu MZF souboru.
 * @param override_name  Jméno pro MZF hlavičku (null-terminated, max 16
 *                       znaků). NULL = ponechat z MRS entry.
 * @param fstrt        Start address pro MZF hlavičku.
 * @param fexec        Exec address pro MZF hlavičku.
 * @return MZDSK_RES_OK nebo chybový kód.
 */
extern en_MZDSK_RES panel_mrs_get_file_mzf_ex ( st_FSMRS_CONFIG *config,
                                                 const st_PANEL_MRS_FILE *file,
                                                 const char *output_path,
                                                 const char *override_name,
                                                 uint16_t fstrt, uint16_t fexec );


/**
 * @brief Aktualizuje STRT a EXEC adresy existujícího MRS souboru.
 *
 * Wrapper nad fsmrs_set_addr - najde directory položku podle file_id
 * z st_PANEL_MRS_FILE a zapíše nové adresy.
 *
 * @param config MRS konfigurace.
 * @param file Položka (z data->files[]) určující cílový soubor.
 * @param fstrt Nová start address (0x0000-0xFFFF).
 * @param fexec Nová exec address (0x0000-0xFFFF).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_mrs_set_addr ( st_FSMRS_CONFIG *config,
                                          const st_PANEL_MRS_FILE *file,
                                          uint16_t fstrt, uint16_t fexec );


/**
 * @brief Importuje raw binární soubor na MRS disk.
 *
 * Volající (typicky GUI dialog) předává předvalidované jméno a příponu
 * (bez paddingu, max 8+3 znaky). Backend neprovádí validaci znaků
 * ani zkracování - neplatné jméno může způsobit chybu v knihovně
 * `fsmrs_write_file`.
 *
 * @param config MRS konfigurace.
 * @param input_path Cesta ke vstupnímu souboru.
 * @param name Jméno souboru bez paddingu (1-8 znaků + NUL). Nesmí být NULL.
 * @param ext Přípona bez paddingu (0-3 znaky + NUL), nebo NULL pro prázdnou.
 * @param fstrt Start adresa (load address) pro MZF metadata.
 * @param fexec Exec adresa pro MZF metadata.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_mrs_put_file ( st_FSMRS_CONFIG *config, const char *input_path,
                                          const char *name, const char *ext,
                                          uint16_t fstrt, uint16_t fexec );


/**
 * @brief Importuje MZF soubor na MRS disk.
 *
 * Přečte MZF hlavičku (jméno, adresy) a tělo, zapíše na MRS disk.
 *
 * @param config MRS konfigurace.
 * @param mzf_path Cesta k MZF souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_mrs_put_file_mzf ( st_FSMRS_CONFIG *config, const char *mzf_path );


/**
 * @brief Importuje MZF soubor na MRS disk s přepsáním cílového jména.
 *
 * Varianta `panel_mrs_put_file_mzf` s volitelným override jména a přípony.
 * Pokud je `override_name` == NULL, použije se jméno z MZF hlavičky.
 *
 * @param config MRS konfigurace.
 * @param mzf_path Cesta k MZF souboru.
 * @param override_name Cílové jméno bez paddingu (max 8 znaků + NUL),
 *                      nebo NULL pro jméno z MZF hlavičky.
 * @param override_ext  Cílová přípona bez paddingu (max 3 znaky + NUL),
 *                      nebo NULL. Pokud je override_name zadán a override_ext
 *                      NULL, přípona je prázdná.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @note Volající je zodpovědný za validaci - backend neprovádí kontrolu
 *       znaků ani zkracování.
 */
extern en_MZDSK_RES panel_mrs_put_file_mzf_ex ( st_FSMRS_CONFIG *config, const char *mzf_path,
                                                  const char *override_name,
                                                  const char *override_ext );


/**
 * @brief Smaže soubor z MRS disku.
 *
 * @param config MRS konfigurace.
 * @param file Položka adresáře ke smazání.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_mrs_delete_file ( st_FSMRS_CONFIG *config, const st_PANEL_MRS_FILE *file );


/**
 * @brief Přejmenuje soubor na MRS disku.
 *
 * @param config MRS konfigurace.
 * @param file Položka adresáře.
 * @param new_name Nové jméno (max 8 znaků).
 * @param new_ext Nová přípona (max 3 znaky), nebo NULL.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_mrs_rename_file ( st_FSMRS_CONFIG *config, const st_PANEL_MRS_FILE *file,
                                             const char *new_name, const char *new_ext );


/**
 * @brief Vykreslí MRS directory panel (ImGui rendering).
 *
 * @param data Datový model.
 * @param config MRS konfigurace (pro souborové operace).
 * @param detect Výsledek auto-detekce (pro reload).
 * @param is_dirty Dirty flag.
 * @param cfg Konfigurace aplikace.
 */
extern void panel_mrs_render ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                                st_MZDSK_DETECT_RESULT *detect, bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                uint64_t owner_session_id );


/**
 * @brief Vykreslí MRS Maintenance tab (ImGui rendering).
 *
 * Poskytuje operace údržby MRS disku:
 * - Format (File Area): reinicializuje FAT a adresář, zachová systémovou oblast
 *
 * Operace má potvrzovací dialog s popisem důsledků.
 *
 * @param data Datový model (pro reload po operacích).
 * @param config MRS konfigurace. Nesmí být NULL.
 * @param detect Výsledek auto-detekce (pro reload). Nesmí být NULL.
 * @param is_dirty Dirty flag (nastavuje se při modifikaci disku).
 */
extern void panel_mrs_maintenance_render ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                                            st_MZDSK_DETECT_RESULT *detect, bool *is_dirty );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_MRS_H */
