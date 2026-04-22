/**
 * @file panel_boot.h
 * @brief Boot sector viewer a bootstrap management.
 *
 * Zobrazuje kompletní informace z boot sektoru (alokační blok 0):
 * IPLPRO hlavičku se všemi poli (jméno, typ, adresy, velikost, komentář)
 * a DINFO blok (volume, farea, obsazenost). Dostupné pro libovolný disk
 * s FSMZ boot trackem (FSMZ, CP/M, MRS i BOOT_ONLY).
 *
 * Bootstrap management umožňuje:
 * - Bottom bootstrap (bloky 1-14/15): Put/Get/Clear pro všechny FS
 *   s volitelným zachováním FSMZ kompatibility
 * - FSMZ bootstrap (FAREA): Put/Get/Over/Clear jen pro plný FSMZ
 *
 * Pro CP/M disky doplňuje sekci "System Tracks" - informace o rezervovaných
 * stopách obsahujících CCP+BDOS+BIOS, které miniboot načítá do paměti Z80.
 *
 * Architektura panel-split:
 *   panel_boot.h/.c          - datový model + logika (čisté C)
 *   panel_boot_imgui.cpp     - ImGui rendering (C++)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_BOOT_H
#define MZDISK_PANEL_BOOT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk.h"
#include "config.h"


/**
 * @brief Datový model boot sector panelu.
 *
 * Obsahuje zpracovaná data z IPLPRO hlavičky (alokační blok 0)
 * a DINFO bloku (alokační blok 15). Data se naplní voláním
 * panel_boot_load() a renderují v panel_boot_render().
 *
 * @par Invarianty:
 * - Pokud has_iplpro == false, pole iplpro_* nejsou platná.
 * - Pokud has_dinfo == false, pole dinfo_* nejsou platná.
 * - name[] je vždy null-terminated.
 */
typedef struct st_PANEL_BOOT_DATA {
    bool is_loaded;                 /**< Panel má nějaká data k zobrazení. */
    en_MZDSK_FS_TYPE fs_type;       /**< Typ filesystému (pro rozhodnutí, zda zobrazit DINFO). */

    /* IPLPRO hlavička (alokační blok 0) */
    bool has_iplpro;                /**< IPLPRO hlavička je platná. */
    bool iplpro_valid;              /**< Hlavička prošla validací (ftype==0x03, "IPLPRO"). */
    char name[18];                  /**< Jméno bootstrap programu (ASCII fallback pro názvy souborů). */
    uint8_t mz_name[18];            /**< Originální jméno v Sharp MZ ASCII (pro konverzi dle zvoleného kódování). */
    int mz_name_len;                /**< Délka platných bajtů v mz_name. */
    uint8_t ftype;                  /**< Typ souboru (očekávaná hodnota 0x03). */
    uint16_t fsize;                 /**< Velikost bootstrap programu v bajtech. */
    uint16_t fstrt;                 /**< Startovací adresa v paměti Z80. */
    uint16_t fexec;                 /**< Spouštěcí adresa v paměti Z80. */
    uint16_t block;                 /**< Počáteční alokační blok bootstrap programu. */
    uint16_t block_count;           /**< Počet alokačních bloků (zaokrouhleno nahoru). */
    uint16_t block_end;             /**< Koncový alokační blok (block + block_count - 1). */
    char boot_type[16];             /**< Typ bootstrapu: "Mini", "Normal", "Over FSMZ". */

    /* DINFO blok (alokační blok 15) */
    bool has_dinfo;                 /**< DINFO blok je platný. */
    uint8_t volume_number;          /**< Master (0) nebo slave (>0). */
    uint8_t farea;                  /**< Počáteční blok souborové oblasti. */
    uint16_t dinfo_used;            /**< Počet obsazených bloků. */
    uint16_t dinfo_blocks;          /**< Celkový počet bloků - 1. */

    /* System tracks (CP/M reserved stopy s CCP+BDOS+BIOS) */
    bool has_system_tracks;         /**< Disk má systémové stopy (CP/M). */
    uint16_t system_tracks_off;     /**< DPB.off - celkový počet rezervovaných stop (včetně boot). */
    uint16_t system_tracks_count;   /**< Počet systémových stop (off - 1, bez boot tracku). */
    uint32_t system_tracks_size;    /**< Celková velikost systémových stop v bajtech. */
    char system_tracks_range[64];   /**< Textový popis rozsahu stop (např. "0, 2-3"). */

    /* Bootstrap management - stav UI */
    bool preserve_fsmz;             /**< Checkbox: zachovat FSMZ kompatibilitu při bottom put. */
    uint16_t max_bottom_blocks;     /**< Maximální počet bloků pro bottom bootstrap (vypočteno). */
    bool is_full_fsmz;              /**< Disk je v plném FSMZ formátu. */
    bool show_error;                /**< Zobrazit error popup. */
    char error_msg[256];            /**< Text chybové zprávy. */
    bool show_clear_confirm;        /**< Zobrazit potvrzovací dialog pro clear. */

    /* Rename a Set popup stavy - stejná sémantika jako u FSMZ directory */
    bool show_rename;               /**< Otevřít Rename popup. */
    char rename_buf[20];            /**< Editovaný ASCII název v Rename popupu. */
    bool show_set;                  /**< Otevřít Set popup. */
    int set_fstrt;                  /**< Editovaná start address v Set popupu (0-0xFFFF). */
    int set_fexec;                  /**< Editovaná exec address v Set popupu (0-0xFFFF). */
    int set_ftype;                  /**< Editovaný ftype v Set popupu (0x01-0xFF). */
} st_PANEL_BOOT_DATA;


/**
 * @brief Naplní datový model boot panelu z otevřeného disku.
 *
 * Přečte IPLPRO blok (alokační blok 0), zvaliduje hlavičku,
 * konvertuje jméno a komentář ze Sharp MZ ASCII a přečte DINFO
 * blok pro informace o volume a souborové oblasti.
 *
 * Panel je dostupný pro libovolný disk s FSMZ boot trackem
 * (mzboot_track == 1).
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 * @param detect Výsledek auto-detekce filesystému.
 *
 * @pre data != NULL, disc je platně otevřený, detect je naplněný.
 * @post data->is_loaded == true pokud disk má boot track.
 */
extern void panel_boot_load ( st_PANEL_BOOT_DATA *data, st_MZDSK_DISC *disc, st_MZDSK_DETECT_RESULT *detect );


/**
 * @brief Exportuje bootstrap do MZF souboru.
 *
 * Přečte IPLPRO hlavičku, zjistí umístění bootstrap dat
 * a uloží je jako MZF soubor (hlavička + tělo).
 *
 * @param disc Otevřený disk.
 * @param mzf_path Cesta k výstupnímu MZF souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí mít platný IPLPRO header.
 */
extern en_MZDSK_RES panel_boot_get_bootstrap ( st_MZDSK_DISC *disc, const char *mzf_path );


/**
 * @brief Nainstaluje bottom bootstrap z MZF souboru.
 *
 * Načte MZF, zapíše data od bloku 1 a aktualizuje IPLPRO hlavičku.
 * Na plném FSMZ v režimu Mini (preserve=true) nepřesáhne blok 14.
 * V režimu free (preserve=false) může využít více FSMZ bloků.
 * Na non-FSMZ discích limit je 15 bloků (1-15).
 *
 * @param data Datový model (pro preserve_fsmz a max_bottom_blocks).
 * @param disc Otevřený disk.
 * @param mzf_path Cesta ke vstupnímu MZF souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí mít boot track. IPLPRO musí být prázdný (nejdřív clear).
 */
extern en_MZDSK_RES panel_boot_put_bottom ( st_PANEL_BOOT_DATA *data,
                                             st_MZDSK_DISC *disc,
                                             const char *mzf_path );


/**
 * @brief Nainstaluje normální FSMZ bootstrap z MZF souboru.
 *
 * Najde volné místo ve FAREA, zapíše data, aktualizuje IPLPRO
 * hlavičku, DINFO bitmapu a nastaví volume jako master.
 *
 * @param disc Otevřený disk.
 * @param mzf_path Cesta ke vstupnímu MZF souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí být v plném FSMZ formátu. IPLPRO musí být prázdný.
 */
extern en_MZDSK_RES panel_boot_put_normal ( st_MZDSK_DISC *disc, const char *mzf_path );


/**
 * @brief Nainstaluje over-FAREA bootstrap z MZF souboru (experimentální).
 *
 * Umístí bootstrap za oblast pokrytou DINFO bitmapou. Zapíše data,
 * aktualizuje IPLPRO hlavičku.
 *
 * @param disc Otevřený disk.
 * @param mzf_path Cesta ke vstupnímu MZF souboru.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí být v plném FSMZ formátu. IPLPRO musí být prázdný.
 */
extern en_MZDSK_RES panel_boot_put_over ( st_MZDSK_DISC *disc, const char *mzf_path );


/**
 * @brief Odstraní bootstrap z disku.
 *
 * Na plném FSMZ: uvolní bloky v DINFO bitmapě, vyčistí IPLPRO,
 * nastaví disk jako slave.
 * Na ostatních: pouze vyčistí IPLPRO blok.
 *
 * @param disc Otevřený disk.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre disc musí mít boot track.
 */
extern en_MZDSK_RES panel_boot_clear ( st_MZDSK_DISC *disc );


/**
 * @brief Přejmenuje bootstrap (upraví fname v IPLPRO bloku).
 *
 * Konvertuje ASCII jméno na Sharp MZ ASCII (délka FSMZ_IPLFNAME_LENGTH)
 * a zavolá fsmz_set_iplpro_header s ostatními poli NULL.
 *
 * @param disc Otevřený disk.
 * @param new_ascii_name Nové jméno v ASCII.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk má platnou IPLPRO hlavičku.
 */
extern en_MZDSK_RES panel_boot_rename ( st_MZDSK_DISC *disc, const char *new_ascii_name );


/**
 * @brief Aktualizuje STRT, EXEC a ftype v IPLPRO hlavičce.
 *
 * Wrapper nad fsmz_set_iplpro_header - upraví jen tři vybraná pole
 * bez změny dat bootstrapu. Pokud user nechce měnit některý field,
 * předá stejnou hodnotu jako je aktuální.
 *
 * @param disc Otevřený disk.
 * @param fstrt Nová start address (0x0000-0xFFFF).
 * @param fexec Nová exec address (0x0000-0xFFFF).
 * @param ftype Nový typ bootstrapu (0x01-0xFF; 0x00 není povolené).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @pre Disk má platnou IPLPRO hlavičku.
 */
extern en_MZDSK_RES panel_boot_set_header ( st_MZDSK_DISC *disc,
                                             uint16_t fstrt, uint16_t fexec,
                                             uint8_t ftype );


/**
 * @brief Vykreslí boot sector viewer s bootstrap managementem (ImGui rendering).
 *
 * @param data Datový model.
 * @param disc Otevřený disk (pro bootstrap operace).
 * @param detect Výsledek auto-detekce FS.
 * @param is_dirty Ukazatel na dirty flag session.
 * @param cfg Konfigurace aplikace (cesty k adresářům).
 */
extern void panel_boot_render ( st_PANEL_BOOT_DATA *data,
                                 st_MZDSK_DISC *disc,
                                 st_MZDSK_DETECT_RESULT *detect,
                                 bool *is_dirty,
                                 st_MZDISK_CONFIG *cfg );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_BOOT_H */
