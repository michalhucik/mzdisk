/**
 * @file panel_settings_imgui.cpp
 * @brief ImGui rendering Settings okna.
 *
 * Zobrazuje sekce Language (roletka s vlajkami), Font (+ Default)
 * a Style. Všechny změny se projevují okamžitě - příznaky v data
 * signalizují app_imgui.cpp, jaké akce provést.
 *
 * Border okna odpovídá vzoru z Create a File Browser oken.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

extern "C" {
#include "panels/panel_settings.h"
#include "texture_utils.h"
#include "i18n.h"
}


/* ====================================================================
 *  Textury vlajek (lazy loading)
 * ==================================================================== */

/** @brief Textury vlajek - 0 = ještě nenačtené. */
static mzdisk_texture_t s_flag_textures[PANEL_SETTINGS_LANG_COUNT] = {};

/** @brief Příznak: textury byly načteny. */
static bool s_flags_loaded = false;


/**
 * @brief Načte textury vlajek (volat jednou při prvním otevření).
 */
static void load_flag_textures ( void )
{
    if ( s_flags_loaded ) return;
    s_flags_loaded = true;
    const char * const *paths = panel_settings_get_flag_paths ();
    for ( int i = 0; i < PANEL_SETTINGS_LANG_COUNT; i++ ) {
        s_flag_textures[i] = mzdisk_texture_load_png ( paths[i], nullptr, nullptr );
    }
}


/**
 * @brief Uvolní textury vlajek (audit M-33).
 *
 * Volat před ImGui shutdown při ukončování aplikace. OS by textury
 * uvolnil sám, ale explicitní cleanup uklidí log valgrindu a chrání
 * případné budoucí hot-reload.
 */
extern "C" void panel_settings_shutdown ( void )
{
    for ( int i = 0; i < PANEL_SETTINGS_LANG_COUNT; i++ ) {
        if ( s_flag_textures[i] != MZDISK_TEXTURE_INVALID ) {
            mzdisk_texture_free ( s_flag_textures[i] );
            s_flag_textures[i] = MZDISK_TEXTURE_INVALID;
        }
    }
    s_flags_loaded = false;
}


/* ====================================================================
 *  Pomocná funkce: vlajka + text na řádek
 * ==================================================================== */

/**
 * @brief Vykreslí vlajku a text na jednom řádku (pro combo items).
 *
 * @param idx Index jazyka.
 * @param name Zobrazovaný název jazyka.
 */
static void render_flag_and_name ( int idx, const char *name )
{
    if ( s_flag_textures[idx] != MZDISK_TEXTURE_INVALID ) {
        /* vertikálně centrovat vlajku s textem */
        float text_h = ImGui::GetTextLineHeight ();
        float flag_h = 16.0f;
        float offset_y = ( text_h - flag_h ) * 0.5f;
        ImVec2 cursor = ImGui::GetCursorPos ();
        ImGui::SetCursorPos ( ImVec2 ( cursor.x, cursor.y + offset_y ) );
        ImGui::Image ( (ImTextureID)(intptr_t) s_flag_textures[idx], ImVec2 ( 24, flag_h ) );
        ImGui::SameLine ();
        /* vrátit vertikální pozici pro text */
        ImGui::SetCursorPosY ( cursor.y );
    }
    ImGui::TextUnformatted ( name );
}


/* ====================================================================
 *  Sekce: Language
 * ==================================================================== */

/**
 * @brief Vykreslí sekci výběru jazyka jako roletku s vlajkami.
 *
 * Při změně nastaví data->lang_changed a aktualizuje cfg->language.
 *
 * @param data Datový model Settings okna.
 * @param cfg Konfigurace aplikace.
 */
static void render_language_section ( st_PANEL_SETTINGS_DATA *data, st_MZDISK_CONFIG *cfg )
{
    const char * const *lang_names = panel_settings_get_lang_names ();
    int lang_idx = panel_settings_lang_str_to_idx ( cfg->language );

    ImGui::SeparatorText ( _ ( "Language" ) );

    /* vlajka vybraného jazyka před combo */
    if ( s_flag_textures[lang_idx] != MZDISK_TEXTURE_INVALID ) {
        float frame_h = ImGui::GetFrameHeight ();
        float flag_h = 16.0f;
        float offset_y = ( frame_h - flag_h ) * 0.5f;
        ImVec2 cursor = ImGui::GetCursorPos ();
        ImGui::SetCursorPos ( ImVec2 ( cursor.x, cursor.y + offset_y ) );
        ImGui::Image ( (ImTextureID)(intptr_t) s_flag_textures[lang_idx], ImVec2 ( 24, flag_h ) );
        ImGui::SameLine ();
        ImGui::SetCursorPosY ( cursor.y );
    }

    /* combo s jazyky */
    ImGui::SetNextItemWidth ( ImGui::GetContentRegionAvail ().x );
    if ( ImGui::BeginCombo ( "##lang_combo", lang_names[lang_idx] ) ) {
        for ( int i = 0; i < PANEL_SETTINGS_LANG_COUNT; i++ ) {
            bool is_selected = ( lang_idx == i );

            ImGui::PushID ( i );
            if ( ImGui::Selectable ( "##lang_sel", is_selected, 0, ImVec2 ( 0, 0 ) ) ) {
                lang_idx = i;
                strncpy ( cfg->language, panel_settings_lang_idx_to_str ( i ),
                          MZDISK_CONFIG_LANG_MAX - 1 );
                cfg->language[MZDISK_CONFIG_LANG_MAX - 1] = '\0';
                data->lang_changed = true;
            }
            if ( is_selected ) ImGui::SetItemDefaultFocus ();

            /* vlajka + text vedle Selectable */
            ImGui::SameLine ( 0, 0 );
            render_flag_and_name ( i, lang_names[i] );

            ImGui::PopID ();
        }
        ImGui::EndCombo ();
    }

}


/* ====================================================================
 *  Sekce: Font
 * ==================================================================== */

/**
 * @brief Vykreslí sekci nastavení fontu (rodina + velikost + Default).
 *
 * Při změně nastaví data->fonts_changed a aktualizuje cfg.
 *
 * @param data Datový model Settings okna.
 * @param cfg Konfigurace aplikace.
 */
static void render_font_section ( st_PANEL_SETTINGS_DATA *data, st_MZDISK_CONFIG *cfg )
{
    const char * const *font_names = panel_settings_get_font_names ();

    ImGui::SeparatorText ( _ ( "Font" ) );

    /* rodina fontu - combo */
    if ( ImGui::BeginCombo ( _ ( "Family" ), font_names[cfg->font_family_idx] ) ) {
        for ( int i = 0; i < PANEL_SETTINGS_FONT_COUNT; i++ ) {
            bool is_selected = ( cfg->font_family_idx == i );
            if ( ImGui::Selectable ( font_names[i], is_selected ) ) {
                cfg->font_family_idx = i;
                data->font_family_changed = true;
            }
            if ( is_selected ) ImGui::SetItemDefaultFocus ();
        }
        ImGui::EndCombo ();
    }

    /* velikost - číselný input s +/- tlačítky (okamžitá změna přes scale) */
    if ( ImGui::InputInt ( _ ( "Size" ), &cfg->font_size, 1, 4 ) ) {
        /* omezit rozsah */
        if ( cfg->font_size < PANEL_SETTINGS_FONT_SIZE_MIN )
            cfg->font_size = PANEL_SETTINGS_FONT_SIZE_MIN;
        if ( cfg->font_size > PANEL_SETTINGS_FONT_SIZE_MAX )
            cfg->font_size = PANEL_SETTINGS_FONT_SIZE_MAX;
        data->font_size_changed = true;
    }

    /* Default tlačítko */
    ImGui::SameLine ();
    if ( ImGui::Button ( _L ( "Default" ) ) ) {
        bool family_changed = ( cfg->font_family_idx != 0 );
        bool size_changed = ( cfg->font_size != MZDISK_CONFIG_DEFAULT_FONT_SIZE );
        cfg->font_family_idx = 0;
        cfg->font_size = MZDISK_CONFIG_DEFAULT_FONT_SIZE;
        if ( family_changed ) data->font_family_changed = true;
        if ( size_changed ) data->font_size_changed = true;
    }
}


/* ====================================================================
 *  Sekce: Style
 * ==================================================================== */

/**
 * @brief Vykreslí sekci výběru tématu.
 *
 * Při změně nastaví data->theme_changed a aktualizuje cfg.
 *
 * @param data Datový model Settings okna.
 * @param cfg Konfigurace aplikace.
 */
static void render_style_section ( st_PANEL_SETTINGS_DATA *data, st_MZDISK_CONFIG *cfg )
{
    const char * const *theme_names = panel_settings_get_theme_names ();

    ImGui::SeparatorText ( _ ( "Style" ) );

    /* téma - combo */
    if ( ImGui::BeginCombo ( _ ( "Theme" ), theme_names[cfg->theme_idx] ) ) {
        for ( int i = 0; i < PANEL_SETTINGS_THEME_COUNT; i++ ) {
            bool is_selected = ( cfg->theme_idx == i );
            if ( ImGui::Selectable ( theme_names[i], is_selected ) ) {
                cfg->theme_idx = i;
                data->theme_changed = true;
            }
            if ( is_selected ) ImGui::SetItemDefaultFocus ();
        }
        ImGui::EndCombo ();
    }
}


/* ====================================================================
 *  Sekce: Export
 * ==================================================================== */

/**
 * @brief Vykreslí sekci nastavení exportu (režim duplicit).
 *
 * @param cfg Konfigurace aplikace.
 */
static void render_export_section ( st_MZDISK_CONFIG *cfg )
{
    /* Názvy se přepočítávají každý frame - _() vrací aktuální překlad */
    const char *dup_names[MZDSK_EXPORT_DUP_COUNT];
    dup_names[MZDSK_EXPORT_DUP_RENAME]    = _ ( "Auto rename (~N)" );
    dup_names[MZDSK_EXPORT_DUP_OVERWRITE]  = _ ( "Overwrite" );
    dup_names[MZDSK_EXPORT_DUP_SKIP]       = _ ( "Skip" );
    dup_names[MZDSK_EXPORT_DUP_ASK]        = _ ( "Ask" );

    ImGui::SeparatorText ( _ ( "Export" ) );

    int idx = cfg->export_dup_mode;
    if ( idx < 0 || idx >= MZDSK_EXPORT_DUP_COUNT ) idx = 0;

    /* PushID: stejný visible label ("If file exists") se používá také
     * v render_dnd_section, potřebujeme unikátní ImGui ID. */
    ImGui::PushID ( "export_dup" );
    if ( ImGui::BeginCombo ( _ ( "If file exists" ), dup_names[idx] ) ) {
        for ( int i = 0; i < MZDSK_EXPORT_DUP_COUNT; i++ ) {
            bool is_selected = ( idx == i );
            if ( ImGui::Selectable ( dup_names[i], is_selected ) ) {
                cfg->export_dup_mode = i;
            }
            if ( is_selected ) ImGui::SetItemDefaultFocus ();
        }
        ImGui::EndCombo ();
    }
    ImGui::PopID ();
}


/* ====================================================================
 *  Sekce: Drag&Drop (režim duplicit při přetažení souborů mezi sessions)
 * ==================================================================== */

/**
 * @brief Vykreslí sekci nastavení chování DnD mezi directory panely.
 *
 * @param cfg Konfigurace aplikace.
 */
static void render_dnd_section ( st_MZDISK_CONFIG *cfg )
{
    const char *dup_names[MZDSK_EXPORT_DUP_COUNT];
    dup_names[MZDSK_EXPORT_DUP_RENAME]    = _ ( "Auto rename (~N)" );
    dup_names[MZDSK_EXPORT_DUP_OVERWRITE]  = _ ( "Overwrite" );
    dup_names[MZDSK_EXPORT_DUP_SKIP]       = _ ( "Skip" );
    dup_names[MZDSK_EXPORT_DUP_ASK]        = _ ( "Ask" );

    ImGui::SeparatorText ( _ ( "Drag&Drop and CP/M User" ) );

    int idx = cfg->dnd_dup_mode;
    if ( idx < 0 || idx >= MZDSK_EXPORT_DUP_COUNT ) idx = MZDSK_EXPORT_DUP_ASK;

    /* PushID: stejný visible label jako v Export sekci, odlišujeme ID. */
    ImGui::PushID ( "dnd_dup" );
    if ( ImGui::BeginCombo ( _ ( "If file exists" ), dup_names[idx] ) ) {
        for ( int i = 0; i < MZDSK_EXPORT_DUP_COUNT; i++ ) {
            bool is_selected = ( idx == i );
            if ( ImGui::Selectable ( dup_names[i], is_selected ) ) {
                cfg->dnd_dup_mode = i;
            }
            if ( is_selected ) ImGui::SetItemDefaultFocus ();
        }
        ImGui::EndCombo ();
    }
    if ( ImGui::IsItemHovered () ) {
        ImGui::SetTooltip ( "%s",
            _ ( "Behavior when a dropped or CP/M chuser operation collides\n"
                "with an existing name in the target user area.\n"
                "\n"
                "Also applies to CP/M Skip (silent) vs error reporting choice." ) );
    }
    ImGui::PopID ();
}


/* ====================================================================
 *  Sekce: Window (multi-window chování)
 * ==================================================================== */

/**
 * @brief Vykreslí sekci nastavení chování oken.
 *
 * @param cfg Konfigurace aplikace.
 */
static void render_window_section ( st_MZDISK_CONFIG *cfg )
{
    ImGui::SeparatorText ( _ ( "Windows" ) );

    bool flag = cfg->open_in_new_window;
    if ( ImGui::Checkbox ( _ ( "Open disks in new window" ), &flag ) ) {
        cfg->open_in_new_window = flag;
    }
    if ( ImGui::IsItemHovered () ) {
        ImGui::SetTooltip ( "%s", _ ( "File > Open Disk and Recent open the DSK in a new detached window instead of replacing the disk in the current window." ) );
    }
}


/* ====================================================================
 *  Hlavní render funkce
 * ==================================================================== */

extern "C" void panel_settings_render ( st_PANEL_SETTINGS_DATA *data, st_MZDISK_CONFIG *cfg )
{
    if ( !data->is_open ) return;

    /* Snapshot cfg při prvním otevření - pro Cancel restore. */
    if ( !data->snapshot_valid ) {
        data->snapshot = *cfg;
        data->snapshot_valid = true;
    }

    /* lazy load textur vlajek */
    load_flag_textures ();

    /* Centrování jen při Appearing (po uzavření a znovu-otevření). */
    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 400, 0 ), ImVec2 ( FLT_MAX, FLT_MAX ) );
    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    /* Border styling - odstraněno PushStyleVar/PushStyleColor, řeší to
       globální theme (app_imgui.cpp init_theme). */

    bool request_apply = false;
    bool request_ok = false;
    bool request_cancel = false;

    /* Sledovat, zda user klikl křížek (is_open -> false přes Begin). */
    if ( ImGui::Begin ( _L ( "Settings" ), &data->is_open,
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize ) ) {

        render_language_section ( data, cfg );
        ImGui::Spacing ();
        render_font_section ( data, cfg );
        ImGui::Spacing ();
        render_style_section ( data, cfg );
        ImGui::Spacing ();
        render_export_section ( cfg );
        ImGui::Spacing ();
        render_dnd_section ( cfg );
        ImGui::Spacing ();
        render_window_section ( cfg );
        ImGui::Spacing ();

        ImGui::Separator ();
        ImGui::Spacing ();

        /* Footer tlačítka - Apply / OK / Cancel. Kreslíme zprava vlevo
         * (klasický dialog layout). */
        const float btn_w = 100.0f;
        const float btn_spacing = 8.0f;
        const float total_w = btn_w * 3 + btn_spacing * 2;
        float avail = ImGui::GetContentRegionAvail ().x;
        if ( avail > total_w ) {
            ImGui::SetCursorPosX ( ImGui::GetCursorPosX () + ( avail - total_w ) );
        }
        if ( ImGui::Button ( _ ( "Apply" ), ImVec2 ( btn_w, 0 ) ) ) request_apply = true;
        ImGui::SameLine ( 0, btn_spacing );
        if ( ImGui::Button ( "OK", ImVec2 ( btn_w, 0 ) ) ) request_ok = true;
        ImGui::SameLine ( 0, btn_spacing );
        if ( ImGui::Button ( _ ( "Cancel" ), ImVec2 ( btn_w, 0 ) ) ) request_cancel = true;
    }
    bool window_closed_via_x = !data->is_open && data->snapshot_valid;
    ImGui::End ();

    /* Zpracování tlačítek mimo Begin/End. Save do INI deleguje volající
     * (app_imgui.cpp) přes save_and_mark, aby se notifikoval file watch. */
    if ( request_apply ) {
        data->save_requested = true;
        /* Nový baseline - snapshot se přepíše aktuálním cfg. */
        data->snapshot = *cfg;
    }
    if ( request_ok ) {
        data->save_requested = true;
        data->is_open = false;
        data->snapshot_valid = false;
    }
    if ( request_cancel || window_closed_via_x ) {
        /* Restore cfg ze snapshot a vynutit change flags pro refresh
         * (preview změn se vrátí zpět na původní hodnoty). */
        if ( strcmp ( cfg->language, data->snapshot.language ) != 0 ) data->lang_changed = true;
        if ( cfg->font_family_idx != data->snapshot.font_family_idx ) data->font_family_changed = true;
        if ( cfg->font_size != data->snapshot.font_size ) data->font_size_changed = true;
        if ( cfg->theme_idx != data->snapshot.theme_idx ) data->theme_changed = true;
        *cfg = data->snapshot;
        data->is_open = false;
        data->snapshot_valid = false;
    }
}
