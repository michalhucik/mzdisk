/**
 * @file panel_settings.h
 * @brief Okno "Settings" - datový model a API.
 *
 * Samostatné ImGui okno pro nastavení aplikace mzdisk.
 * Umožňuje konfigurovat:
 *   - jazyk (roletka s vlajkami)
 *   - font (rodina, velikost, tlačítko Default)
 *   - téma (Dark Blue / Dark / Light)
 *
 * Všechny změny se projevují okamžitě (bez Apply/Cancel).
 *
 * Architektura panel-split:
 *   panel_settings.h/.c          - datový model a logika (čisté C)
 *   panel_settings_imgui.cpp     - ImGui rendering (C++)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_SETTINGS_H
#define MZDISK_PANEL_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "config.h"


/** @brief Počet dostupných jazyků (auto + explicitní locale). */
#define PANEL_SETTINGS_LANG_COUNT 12

/** @brief Počet dostupných fontových rodin. */
#define PANEL_SETTINGS_FONT_COUNT 4

/** @brief Počet dostupných témat. */
#define PANEL_SETTINGS_THEME_COUNT 3

/** @brief Minimální velikost fontu (px). */
#define PANEL_SETTINGS_FONT_SIZE_MIN 16

/** @brief Maximální velikost fontu (px). */
#define PANEL_SETTINGS_FONT_SIZE_MAX 48


/**
 * @brief Stav Settings okna.
 *
 * Změny se promítají přímo do configu. Příznaky fonts_changed,
 * theme_changed a lang_changed signalizují volajícímu v app_imgui.cpp,
 * že má provést odpovídající akci (rebuild fontů, změna tématu, locale).
 *
 * @invariant lang_idx je v rozsahu 0..PANEL_SETTINGS_LANG_COUNT-1.
 * @invariant font_family_idx je v rozsahu 0..PANEL_SETTINGS_FONT_COUNT-1.
 * @invariant font_size je v rozsahu PANEL_SETTINGS_FONT_SIZE_MIN..PANEL_SETTINGS_FONT_SIZE_MAX.
 * @invariant theme_idx je v rozsahu 0..PANEL_SETTINGS_THEME_COUNT-1.
 */
typedef struct st_PANEL_SETTINGS_DATA {
    bool is_open;               /**< Okno je otevřené (řídí ImGui::Begin). */

    /* Snapshot cfg pro Cancel/restore. Ukládá se při prvním renderu
     * po otevření dialogu. Widgets mění cfg přímo (preview), Apply/OK
     * snapshot přepíše na nový baseline, Cancel snapshot restore. */
    st_MZDISK_CONFIG snapshot;
    bool snapshot_valid;        /**< Snapshot obsahuje platnou zálohu. */

    /* příznaky nastavené renderem pro app_imgui.cpp (zpracovat a resetovat) */
    bool font_family_changed;   /**< Příznak: rodina fontu se změnila - přebudovat atlas. */
    bool font_size_changed;     /**< Příznak: velikost fontu se změnila - přepočítat scale. */
    bool theme_changed;         /**< Příznak: téma se změnilo - zavolat setup_theme(). */
    bool lang_changed;          /**< Příznak: jazyk se změnil - zavolat i18n_set_language(). */
    bool save_requested;        /**< Příznak: Apply/OK kliknut - volající má uložit config. */
} st_PANEL_SETTINGS_DATA;


/**
 * @brief Inicializuje Settings okno.
 *
 * @param data Ukazatel na datový model Settings okna. Nesmí být NULL.
 *
 * @pre data != NULL.
 * @post data->is_open == false, changed příznaky jsou false.
 */
extern void panel_settings_init ( st_PANEL_SETTINGS_DATA *data );


/**
 * @brief Uvolní textury vlajek alokované v load_flag_textures.
 *
 * Volat před ImGui::DestroyContext(). Audit M-33.
 */
extern void panel_settings_shutdown ( void );


/**
 * @brief Převede řetězec jazyka z INI na index.
 *
 * @param lang Řetězec jazyka ("auto", "en", "cs", "de", ...).
 * @return Index jazyka (0..PANEL_SETTINGS_LANG_COUNT-1), nebo 0 pro neznámou hodnotu.
 */
extern int panel_settings_lang_str_to_idx ( const char *lang );


/**
 * @brief Vrátí INI řetězec jazyka pro daný index.
 *
 * @param idx Index jazyka (0..PANEL_SETTINGS_LANG_COUNT-1).
 * @return Statický řetězec ("auto", "en", "cs", ...).
 */
extern const char * panel_settings_lang_idx_to_str ( int idx );


/**
 * @brief Vrátí zobrazované názvy jazyků.
 *
 * @return Statické pole PANEL_SETTINGS_LANG_COUNT řetězců.
 */
extern const char * const * panel_settings_get_lang_names ( void );


/**
 * @brief Vrátí cesty k PNG vlajkám.
 *
 * @return Statické pole PANEL_SETTINGS_LANG_COUNT cest.
 */
extern const char * const * panel_settings_get_flag_paths ( void );


/**
 * @brief Vrátí zobrazované názvy fontových rodin.
 *
 * @return Statické pole PANEL_SETTINGS_FONT_COUNT řetězců.
 */
extern const char * const * panel_settings_get_font_names ( void );


/**
 * @brief Vrátí zobrazované názvy témat.
 *
 * @return Statické pole PANEL_SETTINGS_THEME_COUNT řetězců.
 */
extern const char * const * panel_settings_get_theme_names ( void );


/**
 * @brief Vykreslí Settings okno (ImGui rendering).
 *
 * Implementováno v panel_settings_imgui.cpp. Změny se projevují
 * okamžitě - příznaky v data signalizují volajícímu, jaké akce provést.
 *
 * @param data Datový model Settings okna.
 * @param cfg Konfigurace aplikace (čte i zapisuje přímo).
 */
extern void panel_settings_render ( st_PANEL_SETTINGS_DATA *data, st_MZDISK_CONFIG *cfg );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_SETTINGS_H */
