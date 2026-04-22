/**
 * @file panel_settings.c
 * @brief Logika Settings okna - inicializace, jazyky, fonty, témata.
 *
 * Obsahuje statická data (názvy jazyků, fontů, témat a cesty k vlajkám).
 * Všechny jazyky převzaty z mz800new projektu.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <string.h>
#include "panels/panel_settings.h"


/* ====================================================================
 *  Statická data - jazyky (odpovídají vlajkám z mz800new)
 * ==================================================================== */


/** @brief Zobrazované názvy jazyků (odpovídají lang_idx 0..11). */
static const char * const s_lang_names[PANEL_SETTINGS_LANG_COUNT] = {
    "Auto (system default)",
    "English",
    "Cestina",
    "Deutsch",
    "Espanol",
    "Francais",
    "Italiano",
    "Japanese",
    "Nederlands",
    "Polski",
    "Slovencina",
    "Ukrainska",
};

/** @brief Hodnoty jazyka pro INI soubor (odpovídají lang_idx 0..11). */
static const char * const s_lang_values[PANEL_SETTINGS_LANG_COUNT] = {
    "auto",
    "en",
    "cs",
    "de",
    "es",
    "fr",
    "it",
    "ja",
    "nl",
    "pl",
    "sk",
    "uk",
};

/** @brief Cesty k PNG vlajkám (odpovídají lang_idx 0..11). */
static const char * const s_flag_paths[PANEL_SETTINGS_LANG_COUNT] = {
    "./ui_resources/imgui/images/flags/globe.png",
    "./ui_resources/imgui/images/flags/gb.png",
    "./ui_resources/imgui/images/flags/cz.png",
    "./ui_resources/imgui/images/flags/de.png",
    "./ui_resources/imgui/images/flags/es.png",
    "./ui_resources/imgui/images/flags/fr.png",
    "./ui_resources/imgui/images/flags/it.png",
    "./ui_resources/imgui/images/flags/jp.png",
    "./ui_resources/imgui/images/flags/nl.png",
    "./ui_resources/imgui/images/flags/pl.png",
    "./ui_resources/imgui/images/flags/sk.png",
    "./ui_resources/imgui/images/flags/ua.png",
};


/* ====================================================================
 *  Statická data - fonty a témata
 * ==================================================================== */


/** @brief Zobrazované názvy fontových rodin (odpovídají font_family_idx 0..3). */
static const char * const s_font_names[PANEL_SETTINGS_FONT_COUNT] = {
    "DroidSans",
    "Roboto Medium",
    "Cousine (monospace)",
    "Karla",
};

/** @brief Zobrazované názvy témat (odpovídají theme_idx 0..2). */
static const char * const s_theme_names[PANEL_SETTINGS_THEME_COUNT] = {
    "Dark Blue",
    "Dark",
    "Light",
};


/* ====================================================================
 *  Veřejné funkce
 * ==================================================================== */


void panel_settings_init ( st_PANEL_SETTINGS_DATA *data )
{
    if ( data == NULL ) return;  /* audit L-25: defenzivní NULL kontrola */

    data->is_open = false;
    data->snapshot_valid = false;
    data->font_family_changed = false;
    data->font_size_changed = false;
    data->theme_changed = false;
    data->lang_changed = false;
    data->save_requested = false;
}


int panel_settings_lang_str_to_idx ( const char *lang )
{
    for ( int i = 0; i < PANEL_SETTINGS_LANG_COUNT; i++ ) {
        if ( strcmp ( lang, s_lang_values[i] ) == 0 ) return i;
    }
    return 0; /* auto */
}


const char * panel_settings_lang_idx_to_str ( int idx )
{
    if ( idx < 0 || idx >= PANEL_SETTINGS_LANG_COUNT ) return "auto";
    return s_lang_values[idx];
}


const char * const * panel_settings_get_lang_names ( void )
{
    return s_lang_names;
}


const char * const * panel_settings_get_flag_paths ( void )
{
    return s_flag_paths;
}


const char * const * panel_settings_get_font_names ( void )
{
    return s_font_names;
}


const char * const * panel_settings_get_theme_names ( void )
{
    return s_theme_names;
}
