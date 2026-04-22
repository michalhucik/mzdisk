/**
 * @file panel_cpm.h
 * @brief CP/M filesystem panel - adresářový listing, souborové operace, alloc mapa a Maintenance tab.
 *
 * Zobrazuje dva sub-taby:
 * - Directory: obsah CP/M 2.2 adresáře (uživatel, název, přípona, velikost,
 *   atributy R/O/SYS/ARC), operace Get/Put, Delete, Rename a hromadné atributy
 * - Alloc Map: DPB parametry, statistiky obsazenosti, disk layout pruh,
 *   legenda per-file s golden angle HSV barvami, bloková mřížka s hover tooltipem
 *
 * Maintenance tab: Check (kontrola konzistence extentů),
 * Defrag (defragmentace souborové oblasti s progress callbackem),
 * Format File Area (vymaže adresář, zachová system tracks).
 *
 * Datový model (st_PANEL_CPM_DATA) obsahuje adresářový listing, kopii DPB,
 * alokační bitmapu se statistikami a per-blokové vlastnictví pro vizualizaci.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_CPM_H
#define MZDISK_PANEL_CPM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_cpm/mzdsk_cpm_mzf.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "config.h"


/** @brief Maximální počet souborů v CP/M adresáři. */
#define PANEL_CPM_MAX_FILES 256

/** @brief Maximální počet alokačních bloků pro block_owner pole. */
#define PANEL_CPM_MAX_BLOCKS 4096

/** @brief Hodnota block_owner pro volný blok. */
#define PANEL_CPM_BLOCK_FREE 0xFFFF

/** @brief Hodnota block_owner pro adresářový blok. */
#define PANEL_CPM_BLOCK_DIR 0xFFFE


/**
 * @brief Parametry MZF exportu z CP/M.
 *
 * Obsahuje uživatelsky nastavitelné hodnoty pro MZF hlavičku. Výchozí
 * hodnoty odpovídají konvenci utility SOKODI CMT.COM pro CP/M export
 * (typ 0x22, load/exec 0x0100, zapnuté kódování atributů).
 *
 * @par Invarianty:
 * - ftype ∈ <0, 255>.
 * - exec_addr, strt_addr ∈ <0, 65535>.
 */
typedef struct st_PANEL_CPM_MZF_EXPORT {
    int ftype;                  /**< MZF ftype (0x00-0xFF, výchozí 0x22). */
    int exec_addr;              /**< Exec adresa (0-0xFFFF, výchozí 0x0100). */
    int strt_addr;              /**< Load adresa (0-0xFFFF, výchozí 0x0100). */
    bool encode_attrs;          /**< Kódovat CP/M atributy do fname (výchozí true, platí jen pro ftype 0x22). */
    bool show_opts;             /**< Zobrazit options popup. */
    bool open_file_dlg;         /**< Po potvrzení options otevřít file dialog. */
} st_PANEL_CPM_MZF_EXPORT;


/**
 * @brief Stav resumovatelného bulk Get exportu s podporou ASK módu.
 *
 * Bulk export (raw i MZF) je přerušitelný při kolizi jména - ASK
 * popup zobrazí dotaz, uživatel rozhodne a export pokračuje od
 * `next_idx`. `override_mode` umožňuje "Apply to all" volbu, kdy
 * se zbytek bulk loopu chová podle zvoleného módu bez dalšího
 * dotazování.
 */
typedef struct st_PANEL_CPM_BULK_GET {
    bool active;                /**< Bulk operace je rozpracovaná. */
    bool is_mzf;                /**< false = raw bulk, true = MZF bulk. */
    int next_idx;               /**< Index v data->selected[], od kterého pokračovat. */
    int override_mode;          /**< -1 = per-file ASK, jinak en_MZDSK_EXPORT_DUP_MODE pro zbytek. */
    bool ask_pending;           /**< Čeká se na user rozhodnutí v ASK popupu. */
    int conflict_idx;           /**< Index souboru, jehož cílová cesta koliduje. */
    char conflict_path[2048];   /**< Plná cesta konfliktního cíle. */
    char dirpath[2048];         /**< Cílový adresář (bez trailing slash). */
    int ok_count;               /**< Průběžný počet úspěšných exportů. */
    int failed_count;           /**< Průběžný počet neúspěšných. */
    int err_len;                /**< Délka naakumulovaných errors. */
    char errors[2048];          /**< Akumulované chybové hlášky per-soubor. */
    uint8_t *shared_buf;        /**< Raw: sdílený buffer (alokovaný při active=true). */
    uint32_t shared_buf_size;   /**< Raw: velikost shared_buf. */
    st_PANEL_CPM_MZF_EXPORT mzf_opts; /**< MZF: kopie export options (nutná copy, opts v data se může měnit). */
} st_PANEL_CPM_BULK_GET;


/**
 * @brief Datový model CP/M directory panelu.
 *
 * Obsahuje adresářový listing, alokační mapu a per-blokové
 * vlastnictví pro vizualizaci Alloc Map tabu.
 *
 * @par Invarianty:
 * - block_owner[i] je PANEL_CPM_BLOCK_FREE, PANEL_CPM_BLOCK_DIR,
 *   nebo index do files[] (0..file_count-1)
 * - alloc_loaded je true pouze pokud alloc_map a block_owner jsou platné
 */
typedef struct st_PANEL_CPM_DATA {
    bool is_loaded;                                         /**< Data jsou naplněna. */
    int file_count;                                         /**< Počet souborů. */
    st_MZDSK_CPM_FILE_INFO_EX files[PANEL_CPM_MAX_FILES];  /**< Pole souborů. */
    bool selected[PANEL_CPM_MAX_FILES];                     /**< Multiselect. */
    int detail_index;                                       /**< Index pro detail panel (-1 = žádný). */
    char preset_name[32];                                   /**< Název CP/M presetu. */
    st_MZDSK_CPM_DPB dpb;                                  /**< Kopie DPB pro souborové operace. */
    en_MZDSK_CPM_FORMAT cpm_format;                         /**< CP/M preset formát (pro reload). */
    bool has_error;                                         /**< Příznak chybového popupu. */
    char error_msg[2048];                                   /**< Text chybové hlášky (větší buffer pro bulk
                                                                 Get/Put/Era, kde se kolektují hlášky per-soubor). */
    st_MZDSK_CPM_ALLOC_MAP alloc_map;                      /**< Alokační bitmapa a statistiky. */
    uint16_t block_owner[PANEL_CPM_MAX_BLOCKS];             /**< Vlastník bloku: index do files[], nebo PANEL_CPM_BLOCK_FREE/DIR. */
    bool alloc_loaded;                                      /**< Alloc mapa a block_owner jsou naplněny. */
    st_PANEL_CPM_MZF_EXPORT mzf_export;                     /**< Parametry MZF exportu. */
    st_PANEL_CPM_BULK_GET bulk_get;                         /**< Stav resumovatelného bulk Get/Get MZF exportu (ASK mód). */
    int filter_user;                                        /**< Aktivní filtr user oblasti: -1 = all, 0-15 = jen daný user. */
    st_MZDSK_CPM_DIR_STATS dir_stats;                       /**< Statistika obsazení directory slotů (total/used/free/blocked). */
    bool dir_stats_loaded;                                  /**< dir_stats je naplněna validními daty. */
} st_PANEL_CPM_DATA;


/**
 * @brief Naplní datový model CP/M adresáře z otevřeného disku.
 *
 * Načte rozšířený directory listing (soubory s atributy a extenty),
 * sestaví alokační bitmapu se statistikami a per-blokové vlastnictví
 * (mapování blok -> soubor/dir/free) pro vizualizaci Alloc Map tabu.
 *
 * @param data Výstupní datový model. Nesmí být NULL.
 * @param disc Otevřený disk. Nesmí být NULL.
 * @param detect Výsledek auto-detekce (obsahuje DPB a formát). Nesmí být NULL.
 *
 * @pre disc != NULL && disc->handler != NULL
 * @post data->is_loaded == true při úspěchu
 * @post data->alloc_loaded == true pokud se podařilo sestavit alloc mapu
 *
 * @par Vedlejší efekty:
 * - Čte sektory z disku (directory bloky).
 * - Zachovává has_error/error_msg přes reload (pro error popup).
 */
extern void panel_cpm_load ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect );


/**
 * @brief Exportuje soubor z CP/M disku jako raw binárku.
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param file Položka adresáře k exportu.
 * @param output_path Cesta k výstupnímu souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_get_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                          const st_MZDSK_CPM_FILE_INFO_EX *file, const char *output_path );


/**
 * @brief Vrátí požadovanou velikost bufferu pro export souboru CP/M.
 *
 * @param dpb Disk Parameter Block. Nesmí být NULL.
 * @return Minimální velikost bufferu v bajtech (dsm+1) * block_size, nebo 0 při dpb == NULL.
 */
extern uint32_t panel_cpm_get_file_buffer_size ( const st_MZDSK_CPM_DPB *dpb );


/**
 * @brief Exportuje soubor z CP/M disku jako raw binárku s preallokovaným bufferem.
 *
 * Varianta `panel_cpm_get_file` pro bulk export - volající alokuje
 * buffer jednou (`panel_cpm_get_file_buffer_size`) a reusuje ho
 * napříč iteracemi. Předejde opakované malloc/free ~1.1 MB bufferu
 * per soubor (audit M-36).
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param file Položka adresáře k exportu.
 * @param output_path Cesta k výstupnímu souboru.
 * @param buffer Preallokovaný buffer o minimální velikosti panel_cpm_get_file_buffer_size(dpb).
 * @param buf_size Skutečná velikost bufferu.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_UNKNOWN_ERROR při NULL argumentech
 *         nebo buf_size < požadovaná velikost, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_get_file_with_buffer ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                      const st_MZDSK_CPM_FILE_INFO_EX *file,
                                                      const char *output_path,
                                                      uint8_t *buffer, uint32_t buf_size );


/**
 * @brief Exportuje soubor z CP/M disku jako MZF s výchozími hodnotami.
 *
 * Přečte soubor, zakóduje do MZF rámce s výchozími parametry
 * (ftype=0x22, load/exec=0x0100, kódování atributů) a uloží
 * jako .mzf soubor.
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param file Položka adresáře k exportu.
 * @param output_path Cesta k výstupnímu MZF souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_get_file_mzf ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                               const st_MZDSK_CPM_FILE_INFO_EX *file, const char *output_path );


/**
 * @brief Exportuje soubor z CP/M disku jako MZF s rozšířenými volbami.
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param file Položka adresáře k exportu.
 * @param output_path Cesta k výstupnímu MZF souboru.
 * @param opts Parametry exportu (ftype, exec_addr, strt_addr, encode_attrs).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_get_file_mzf_ex ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                  const st_MZDSK_CPM_FILE_INFO_EX *file,
                                                  const char *output_path,
                                                  const st_PANEL_CPM_MZF_EXPORT *opts );


/**
 * @brief Inicializuje MZF export parametry na výchozí hodnoty.
 *
 * @param opts Struktura k inicializaci.
 * @post ftype=0x22, exec_addr=0x0100, strt_addr=0x0100, encode_attrs=true.
 */
extern void panel_cpm_mzf_export_init ( st_PANEL_CPM_MZF_EXPORT *opts );


/**
 * @brief Importuje soubor na CP/M disk (raw binárka).
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param input_path Cesta ke vstupnímu souboru.
 * @param cpm_name Jméno CP/M souboru (max 8 znaků).
 * @param cpm_ext Přípona CP/M souboru (max 3 znaky).
 * @param user Číslo uživatele (0-15).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_put_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                          const char *input_path,
                                          const char *cpm_name, const char *cpm_ext, uint8_t user );


/**
 * @brief Importuje MZF soubor na CP/M disk do zadané user oblasti.
 *
 * Dekóduje MZF rámec, extrahuje jméno, příponu, data a (pro ftype 0x22)
 * také atributy. Zapíše soubor a nastaví atributy. Pole fstrt a fexec
 * se pro CP/M import nepoužívají.
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param mzf_path Cesta k MZF souboru.
 * @param user Cílové číslo uživatele (0-15).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_put_file_mzf ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                               const char *mzf_path, uint8_t user );


/**
 * @brief Importuje MZF soubor na CP/M disk s přepsáním cílového jména a user.
 *
 * Varianta `panel_cpm_put_file_mzf` s volitelným override jména a přípony,
 * které jsou už předvalidované volajícím (typicky GUI dialog, který
 * validuje vstup v reálném čase pomocí `mzdsk_validate_83_name`).
 *
 * Pokud je `override_name` == NULL, použije se jméno z MZF hlavičky
 * (identické chování jako `panel_cpm_put_file_mzf`).
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param mzf_path Cesta k MZF souboru.
 * @param override_name Cílové jméno bez paddingu (max 8 znaků + NUL),
 *                      nebo NULL pro jméno z MZF hlavičky.
 * @param override_ext  Cílová přípona bez paddingu (max 3 znaky + NUL),
 *                      nebo NULL. Pokud je override_name zadán a override_ext
 *                      NULL, přípona je prázdná.
 * @param user Cílové číslo uživatele (0-15).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @note Backend neprovádí validaci znaků ani zkracování - jméno musí
 *       být předvalidované. Pokud je neplatné, zápis může selhat
 *       v knihovně `mzdsk_cpm_write_file`.
 */
extern en_MZDSK_RES panel_cpm_put_file_mzf_ex ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                                  const char *mzf_path,
                                                  const char *override_name,
                                                  const char *override_ext,
                                                  uint8_t user );


/**
 * @brief Smaže soubor z CP/M disku.
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param file Položka adresáře ke smazání.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_delete_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                             const st_MZDSK_CPM_FILE_INFO_EX *file );


/**
 * @brief Přejmenuje soubor na CP/M disku.
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param file Položka adresáře k přejmenování.
 * @param new_name Nové jméno (max 8 znaků).
 * @param new_ext Nová přípona (max 3 znaky).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_rename_file ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                             const st_MZDSK_CPM_FILE_INFO_EX *file,
                                             const char *new_name, const char *new_ext );


/**
 * @brief Nastaví atributy souboru na CP/M disku.
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param file Položka adresáře.
 * @param attributes Kombinace en_MZDSK_CPM_ATTR (bitový OR).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
extern en_MZDSK_RES panel_cpm_set_attrs ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                           const st_MZDSK_CPM_FILE_INFO_EX *file, uint8_t attributes );


/**
 * @brief Změní user number existujícího souboru.
 *
 * Wrapper nad `mzdsk_cpm_set_user`. Přepíše user byte na všech extentech
 * souboru. Před modifikací ověří, že v cílové user oblasti neexistuje
 * soubor stejného jména.
 *
 * @param disc Otevřený disk.
 * @param dpb Disk Parameter Block.
 * @param file Položka adresáře (zdrojový user se bere z file->user).
 * @param new_user Nový user number (0-15).
 * @return MZDSK_RES_OK / FILE_EXISTS / FILE_NOT_FOUND / INVALID_PARAM.
 */
extern en_MZDSK_RES panel_cpm_set_user ( st_MZDSK_DISC *disc, const st_MZDSK_CPM_DPB *dpb,
                                          const st_MZDSK_CPM_FILE_INFO_EX *file, uint8_t new_user );


/**
 * @brief Vykreslí CP/M panel s taby Directory a Alloc Map (ImGui rendering).
 *
 * Directory tab: toolbar s Get/Put/Delete/Rename/atributy, tabulka souborů
 * s checkboxy pro multiselect, detailní informace o vybraném souboru.
 *
 * Alloc Map tab: DPB parametry tabulka, statistiky obsazenosti, disk layout
 * pruh, legenda per-file + strukturální typy, bloková mřížka s hover tooltipem.
 *
 * Popupy (Delete/Rename/Error) a file dialogy jsou vykresleny na úrovni
 * okna mimo taby.
 *
 * @param data Datový model. Nesmí být NULL.
 * @param disc Otevřený disk (pro souborové operace). Nesmí být NULL.
 * @param is_dirty Dirty flag (nastavuje se při modifikaci disku). Nesmí být NULL.
 * @param cfg Konfigurace (last_get_dir, last_put_dir). Nesmí být NULL.
 */
extern void panel_cpm_render ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                                bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                uint64_t owner_session_id );


/**
 * @brief Vykreslí CP/M Maintenance tab (ImGui rendering).
 *
 * Poskytuje operace údržby CP/M disku:
 * - Check: read-only kontrola konzistence extentů všech souborů
 * - Format (File Area): vymaže adresář (0xE5), zachová system tracks
 *
 * Každá operace má potvrzovací dialog s popisem důsledků.
 *
 * @param data Datový model (pro reload po operacích a seznam souborů pro Check).
 * @param disc Otevřený disk. Nesmí být NULL.
 * @param detect Výsledek auto-detekce (obsahuje DPB). Nesmí být NULL.
 * @param is_dirty Dirty flag (nastavuje se při modifikaci disku).
 */
extern void panel_cpm_maintenance_render ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                                            st_MZDSK_DETECT_RESULT *detect, bool *is_dirty );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_CPM_H */
