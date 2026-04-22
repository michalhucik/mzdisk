/**
 * @file panel_create_imgui.cpp
 * @brief ImGui rendering pro okno "Create New Disk".
 *
 * Samostatné ImGui okno se dvousloupcovým layoutem:
 * - Levý sloupec: preset combo, sides, tracks, pružná mezera, Add rule
 * - Pravý sloupec: scrollovatelný seznam pravidel geometrie (adaptivní výška,
 *   horizontální scrollbar)
 * - Vertikální splitter mezi sloupci pro nastavení poměru šířek
 * - Spodní sekce: Disk Summary, Format checkbox, soubor, tlačítka
 *
 * Sector Map Popup pro editaci custom mapy sektorů.
 * Directory chooser přes IGFD.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "panels/panel_create.h"
#include "i18n.h"
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
}

#include "ui_helpers.h"


/* ====================================================================
 *  Statické popisky a stav
 * ==================================================================== */

/** @brief Popisky velikostí sektorů pro combo box. */
static const char *s_ssize_names[] = { "128 B", "256 B", "512 B", "1024 B" };

/** @brief Popisky řazení sektorů pro combo box. */
static const char *s_order_names[] = { "Custom", "Normal", "LEC", "LEC HD" };

/** @brief Klíč pro IGFD directory chooser dialog. */
static const char *CREATE_DIR_DIALOG_KEY = "CreateDirDlg";

/** @brief Příznak: sector map popup je otevřený. */
static bool s_show_sector_map = false;

/** @brief Index pravidla editovaného v sector map popup. */
static int s_editing_rule_idx = 0;

/** @brief Dočasná kopie sector mapy pro popup editor. */
static uint8_t s_tmp_sector_map[DSK_MAX_SECTORS];

/** @brief Velikost sektoru v bajtech podle indexu. */
static const int s_ssize_bytes[] = { 128, 256, 512, 1024 };


/* ====================================================================
 *  Sector Map Popup
 * ==================================================================== */

/**
 * @brief Vykreslí modální popup pro editaci custom sector mapy.
 *
 * Zobrazí dec + hex vstup pro každý sektor. OK potvrdí změny,
 * Cancel je zahodí. Změna mapy automaticky přepne preset na Custom.
 *
 * @param data Datový model Create okna.
 */
static void draw_sector_map_popup ( st_PANEL_CREATE_DATA *data )
{
    if ( !s_show_sector_map ) return;

    ImGui::OpenPopup ( _L ( "Custom Sector Map" ) );

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( ImGui::BeginPopupModal ( _L ( "Custom Sector Map" ), &s_show_sector_map,
                                   ImGuiWindowFlags_AlwaysAutoResize ) ) {

        int num_sectors = data->rules[s_editing_rule_idx].sectors;
        for ( int i = 0; i < num_sectors; i++ ) {
            ImGui::Text ( "Sector %2d:", i + 1 );
            ImGui::SameLine ();

            /* Dec vstup */
            int dec_val = s_tmp_sector_map[i];
            char id_dec[32];
            snprintf ( id_dec, sizeof ( id_dec ), "##smdec%d", i );
            ImGui::SetNextItemWidth ( 60 );
            if ( ImGui::InputInt ( id_dec, &dec_val, 0, 0 ) ) {
                if ( dec_val < 0 ) dec_val = 0;
                if ( dec_val > 255 ) dec_val = 255;
                s_tmp_sector_map[i] = (uint8_t) dec_val;
            }

            ImGui::SameLine ();

            /* Hex vstup */
            char hex_buf[4];
            snprintf ( hex_buf, sizeof ( hex_buf ), "%02X", s_tmp_sector_map[i] );
            char id_hex[32];
            snprintf ( id_hex, sizeof ( id_hex ), "##smhex%d", i );
            ImGui::Text ( "0x" );
            ImGui::SameLine ();
            ImGui::SetNextItemWidth ( 40 );
            if ( ImGui::InputText ( id_hex, hex_buf, sizeof ( hex_buf ),
                                    ImGuiInputTextFlags_CharsHexadecimal ) ) {
                s_tmp_sector_map[i] = (uint8_t) strtol ( hex_buf, NULL, 16 );
            }
        }

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        float total_btn_w = 80 * 2 + ImGui::GetStyle ().ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail ().x;
        float offset = ( avail - total_btn_w ) * 0.5f;
        if ( offset > 0.0f )
            ImGui::SetCursorPosX ( ImGui::GetCursorPosX () + offset );

        if ( ImGui::Button ( "OK", ImVec2 ( 80, 0 ) ) ) {
            memcpy ( data->rules[s_editing_rule_idx].sector_map,
                     s_tmp_sector_map,
                     data->rules[s_editing_rule_idx].sectors );
            s_show_sector_map = false;
            data->preset_idx = PANEL_CREATE_PRESET_CUSTOM;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ImGui::Button ( _L ( "Cancel" ), ImVec2 ( 80, 0 ) ) ) {
            s_show_sector_map = false;
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    }
}


/* ====================================================================
 *  Disk Summary
 * ==================================================================== */

/**
 * @brief Vykreslí Disk Summary box pod pravidly.
 *
 * Zobrazí typ disku, rozměry (stopy x strany), celkovou kapacitu
 * a geometrii sektorů. Pro disky s jedním pravidlem zobrazí kompaktní
 * řádek, pro více pravidel "Mixed geometry".
 *
 * @param data Datový model Create okna.
 */
static void render_disk_summary ( const st_PANEL_CREATE_DATA *data )
{
    const st_PANEL_CREATE_PREDEF *presets = panel_create_get_presets ();
    const char *preset_name = presets[data->preset_idx].name;

    /* Výpočet celkové kapacity */
    int tracks_per_side = data->tracks / data->sides;
    long total_bytes = 0;

    /* Spočítej kapacitu z pravidel */
    for ( int abs_t = 0; abs_t < data->tracks; abs_t++ ) {
        /* Najdi platné pravidlo pro tuto stopu */
        int rule_idx = 0;
        for ( int r = 0; r < data->count_rules; r++ ) {
            if ( data->rules[r].from_track <= abs_t ) {
                rule_idx = r;
            }
        }
        total_bytes += (long) data->rules[rule_idx].sectors * s_ssize_bytes[data->rules[rule_idx].sector_size_idx];
    }

    float total_kb = (float) total_bytes / 1024.0f;

    ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 0.65f, 0.75f, 0.95f, 1.0f ) );

    if ( data->preset_idx != PANEL_CREATE_PRESET_CUSTOM ) {
        ImGui::Text ( "%s: %s | %dx%d %s | %.0f KB",
                      _ ( "Disk Summary" ), preset_name,
                      tracks_per_side, data->sides,
                      _ ( "tracks" ), total_kb );
    } else {
        ImGui::Text ( "%s: %s | %dx%d %s | %.0f KB",
                      _ ( "Disk Summary" ), _ ( "Custom" ),
                      tracks_per_side, data->sides,
                      _ ( "tracks" ), total_kb );
    }

    /* Geometrie - pokud jedno pravidlo, zobraz detaily */
    if ( data->count_rules == 1 ) {
        int sec = data->rules[0].sectors;
        int ssz = s_ssize_bytes[data->rules[0].sector_size_idx];
        ImGui::Text ( "%s: %d %s x %d B = %d B/%s",
                      _ ( "Geometry" ), sec, _ ( "sectors" ),
                      ssz, sec * ssz, _ ( "track" ) );
    } else {
        ImGui::Text ( "%s: %s (%d %s)",
                      _ ( "Geometry" ), _ ( "Mixed" ),
                      data->count_rules, _ ( "rules" ) );
    }

    ImGui::PopStyleColor ();
}


/* ====================================================================
 *  Hlavní render funkce
 * ==================================================================== */


void panel_create_render ( st_PANEL_CREATE_DATA *data, st_MZDISK_CONFIG *cfg )
{
    if ( !data || !data->is_open ) return;

    const st_PANEL_CREATE_PREDEF *presets = panel_create_get_presets ();
    bool is_custom = ( data->preset_idx == PANEL_CREATE_PRESET_CUSTOM );

    SetNextWindowDefaultCentered ( 1200, 560 );

    /* Výrazný border kolem okna - stejný styl jako u File Browseru */
    ImGui::PushStyleVar ( ImGuiStyleVar_WindowBorderSize, 2.0f );
    ImGui::PushStyleColor ( ImGuiCol_Border, ImVec4 ( 0.40f, 0.50f, 0.80f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBg, ImVec4 ( 0.10f, 0.14f, 0.30f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBgActive, ImVec4 ( 0.14f, 0.20f, 0.42f, 1.0f ) );

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;

    if ( !ImGui::Begin ( _L ( "Create New Disk" ), &data->is_open, flags ) ) {
        ImGui::End ();
        ImGui::PopStyleColor ( 3 );
        ImGui::PopStyleVar ();
        return;
    }

    const ImGuiStyle &style = ImGui::GetStyle ();
    const float splitter_w = 6.0f;
    const float label_col = 80.0f;
    const float input_w = 130.0f;
    const float ssize_w = 120.0f;
    const float order_w = 130.0f;

    /*
     * Výpočet výšky horní sekce (levý panel + rules) tak, aby
     * spodní sekce (summary, format, file, buttons) měla fixní místo
     * a horní sekce se adaptivně roztahovala.
     */
    float bottom_section_h =
        style.ItemSpacing.y +                                   /* Spacing */
        2.0f +                                                  /* Separator */
        style.ItemSpacing.y +                                   /* Spacing */
        ImGui::GetTextLineHeight () * 2 + style.ItemSpacing.y + /* Summary (2 řádky) */
        style.ItemSpacing.y +                                   /* Spacing */
        ImGui::GetFrameHeight () +                              /* Format checkbox */
        style.ItemSpacing.y +                                   /* Spacing */
        2.0f +                                                  /* Separator */
        style.ItemSpacing.y +                                   /* Spacing */
        ImGui::GetFrameHeight () +                              /* Filename */
        style.ItemSpacing.y +                                   /* ItemSpacing */
        ImGui::GetFrameHeight () +                              /* Directory */
        style.ItemSpacing.y +                                   /* Spacing */
        2.0f +                                                  /* Separator */
        style.ItemSpacing.y +                                   /* Spacing */
        ImGui::GetFrameHeight () +                              /* Buttons */
        style.ItemSpacing.y * 2 +                               /* extra reserve */
        ImGui::GetTextLineHeight () +                           /* error line reserve */
        8.0f;                                                   /* padding */

    float top_section_h = ImGui::GetContentRegionAvail ().y - bottom_section_h;
    if ( top_section_h < 80.0f ) top_section_h = 80.0f;

    /* Omez šířku splitteru na rozumné minimum/maximum */
    float avail_w = ImGui::GetContentRegionAvail ().x;
    if ( cfg->create_splitter_w < 160.0f ) cfg->create_splitter_w = 160.0f;
    if ( cfg->create_splitter_w > avail_w - 200.0f ) cfg->create_splitter_w = avail_w - 200.0f;

    /* ======= Levý panel (Preset, Sides, Tracks, flex mezera, + Add rule) ======= */
    ImGui::BeginChild ( "##left_panel", ImVec2 ( cfg->create_splitter_w, top_section_h ),
                        ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar );

    /* Preset combo */
    ImGui::SetNextItemWidth ( -FLT_MIN );
    if ( ImGui::BeginCombo ( "##preset", _ ( presets[data->preset_idx].name ) ) ) {
        for ( int i = 0; i < PANEL_CREATE_PRESET_COUNT; i++ ) {
            bool selected = ( data->preset_idx == i );
            if ( ImGui::Selectable ( _L ( presets[i].name ), selected ) ) {
                data->preset_idx = i;
                panel_create_load_preset ( data, i );
            }
            if ( selected ) ImGui::SetItemDefaultFocus ();
        }
        ImGui::EndCombo ();
    }

    ImGui::Spacing ();

    /* Sides */
    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s", _ ( "Sides" ) );
    ImGui::SameLine ( label_col );
    ImGui::SetNextItemWidth ( -FLT_MIN );
    const char *sides_items[] = { "1", "2" };
    int sides_idx = data->sides - 1;
    if ( ImGui::BeginCombo ( "##sides", sides_items[sides_idx] ) ) {
        for ( int i = 0; i < 2; i++ ) {
            bool selected = ( sides_idx == i );
            if ( ImGui::Selectable ( sides_items[i], selected ) ) {
                int new_sides = i + 1;
                /* Pouze CP/M SD (2) a CP/M HD (3) podporují single-side.
                   U ostatních presetů změna sides přepne na Custom. */
                if ( new_sides != data->sides
                     && data->preset_idx != PANEL_CREATE_PRESET_CUSTOM
                     && data->preset_idx != 2  /* CP/M SD */
                     && data->preset_idx != 3  /* CP/M HD */ ) {
                    data->preset_idx = PANEL_CREATE_PRESET_CUSTOM;
                }
                data->sides = new_sides;
            }
            if ( selected ) ImGui::SetItemDefaultFocus ();
        }
        ImGui::EndCombo ();
    }

    /* Tracks */
    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s", _ ( "Tracks" ) );
    ImGui::SameLine ( label_col );
    ImGui::SetNextItemWidth ( -FLT_MIN );
    {
        int step = ( data->sides == 2 ) ? 2 : 1;
        int step_fast = 10;
        if ( ImGui::InputScalar ( "##tracks", ImGuiDataType_S32, &data->tracks,
                                   &step, &step_fast, "%d",
                                   ImGuiInputTextFlags_CharsDecimal ) ) {
            if ( data->tracks < 1 ) data->tracks = 1;
            if ( data->tracks > DSK_MAX_TOTAL_TRACKS ) data->tracks = DSK_MAX_TOTAL_TRACKS;
            if ( data->sides == 2 && ( data->tracks % 2 ) != 0 ) {
                data->tracks++;
                if ( data->tracks > DSK_MAX_TOTAL_TRACKS ) data->tracks -= 2;
            }
        }
    }

    /* Pružná mezera - tlačí + Add rule ke spodnímu okraji levého panelu */
    float remaining = ImGui::GetContentRegionAvail ().y - ImGui::GetFrameHeight ();
    if ( remaining > 0.0f ) {
        ImGui::Dummy ( ImVec2 ( 0, remaining ) );
    }

    /* + Add rule - vždy dostupné (při stisku přepne na Custom) */
    bool can_add = ( data->count_rules < PANEL_CREATE_MAX_RULES );
    if ( !can_add ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _L ( "+ Add rule" ), ImVec2 ( -FLT_MIN, 0 ) ) ) {
        /* Přepnout na Custom pokud je zrovna jiný preset */
        data->preset_idx = PANEL_CREATE_PRESET_CUSTOM;
        st_PANEL_CREATE_RULE *nr = &data->rules[data->count_rules];
        memset ( nr, 0, sizeof ( st_PANEL_CREATE_RULE ) );
        nr->from_track = ( data->count_rules > 0 )
                         ? data->rules[data->count_rules - 1].from_track + 1
                         : 0;
        nr->sectors = 16;
        nr->sector_size_idx = 1;
        nr->order_idx = 1;
        nr->filler = 0xFF;
        dsk_tools_make_sector_map ( (uint8_t) nr->sectors,
                                    DSK_SEC_ORDER_NORMAL, nr->sector_map );
        data->count_rules++;
    }
    if ( !can_add ) ImGui::EndDisabled ();

    ImGui::EndChild ();

    /* ======= Vertikální splitter mezi levým a pravým panelem ======= */
    ImGui::SameLine ();
    {
        ImVec2 cursor = ImGui::GetCursorScreenPos ();
        ImGui::InvisibleButton ( "##vsplitter",
                                 ImVec2 ( splitter_w, top_section_h ) );
        if ( ImGui::IsItemActive () ) {
            cfg->create_splitter_w += ImGui::GetIO ().MouseDelta.x;
        }
        /* Vizuální indikace - zvýrazni při hoveru nebo tažení */
        ImU32 col = ImGui::GetColorU32 (
            ImGui::IsItemActive ()  ? ImGuiCol_SeparatorActive :
            ImGui::IsItemHovered () ? ImGuiCol_SeparatorHovered :
                                      ImGuiCol_Separator );
        ImGui::GetWindowDrawList ()->AddRectFilled (
            cursor,
            ImVec2 ( cursor.x + splitter_w, cursor.y + top_section_h ),
            col );
        if ( ImGui::IsItemHovered () || ImGui::IsItemActive () ) {
            ImGui::SetMouseCursor ( ImGuiMouseCursor_ResizeEW );
        }
    }

    ImGui::SameLine ();

    /* ======= Pravý panel - scrollovatelná pravidla (adaptivní výška) ======= */
    ImGui::BeginChild ( "##rules_scroll", ImVec2 ( 0, top_section_h ),
                        ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar );

    for ( int r = 0; r < data->count_rules; r++ ) {
        ImGui::PushID ( r );

        /* From track - rule 0 je vždy 0 a disabled.
           Vstupní pole jsou disabled pro ne-Custom presety. */
        bool is_first = ( r == 0 );
        bool fields_disabled = !is_custom || is_first;
        if ( fields_disabled ) {
            if ( is_first ) data->rules[0].from_track = 0;
            ImGui::BeginDisabled ();
        }

        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "%s:", _ ( "From track" ) );
        ImGui::SameLine ();
        ImGui::SetNextItemWidth ( input_w );
        {
            int step = 1;
            if ( ImGui::InputScalar ( "##ft", ImGuiDataType_S32,
                                       &data->rules[r].from_track,
                                       &step, NULL, "%d",
                                       ImGuiInputTextFlags_CharsDecimal ) ) {
                int min_ft = ( r > 0 ) ? data->rules[r - 1].from_track + 1 : 0;
                int max_ft = ( data->tracks > 0 ) ? data->tracks - 1 : 0;
                if ( data->rules[r].from_track < min_ft ) data->rules[r].from_track = min_ft;
                if ( data->rules[r].from_track > max_ft ) data->rules[r].from_track = max_ft;
            }
        }

        if ( fields_disabled ) ImGui::EndDisabled ();

        /* Sectors - disabled pro ne-Custom presety */
        ImGui::SameLine ();
        if ( !is_custom ) ImGui::BeginDisabled ();
        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "%s:", _ ( "Sectors" ) );
        ImGui::SameLine ();
        ImGui::SetNextItemWidth ( input_w );
        {
            int step = 1;
            if ( ImGui::InputScalar ( "##sec", ImGuiDataType_S32,
                                       &data->rules[r].sectors,
                                       &step, NULL, "%d",
                                       ImGuiInputTextFlags_CharsDecimal ) ) {
                if ( data->rules[r].sectors < 1 ) data->rules[r].sectors = 1;
                if ( data->rules[r].sectors > DSK_MAX_SECTORS ) data->rules[r].sectors = DSK_MAX_SECTORS;
            }
        }

        /* Sector size combo */
        ImGui::SameLine ();
        ImGui::SetNextItemWidth ( ssize_w );
        if ( ImGui::BeginCombo ( "##ss", s_ssize_names[data->rules[r].sector_size_idx] ) ) {
            for ( int i = 0; i < 4; i++ ) {
                bool selected = ( data->rules[r].sector_size_idx == i );
                if ( ImGui::Selectable ( s_ssize_names[i], selected ) ) {
                    data->rules[r].sector_size_idx = i;
                }
                if ( selected ) ImGui::SetItemDefaultFocus ();
            }
            ImGui::EndCombo ();
        }

        /* Order combo */
        ImGui::SameLine ();
        ImGui::SetNextItemWidth ( order_w );
        if ( ImGui::BeginCombo ( "##ord", _ ( s_order_names[data->rules[r].order_idx] ) ) ) {
            for ( int i = 0; i < 4; i++ ) {
                bool selected = ( data->rules[r].order_idx == i );
                if ( ImGui::Selectable ( _L ( s_order_names[i] ), selected ) ) {
                    data->rules[r].order_idx = i;
                    if ( i != 0 ) {
                        dsk_tools_make_sector_map (
                            (uint8_t) data->rules[r].sectors,
                            (en_DSK_SECTOR_ORDER_TYPE) i,
                            data->rules[r].sector_map );
                    }
                }
                if ( selected ) ImGui::SetItemDefaultFocus ();
            }
            ImGui::EndCombo ();
        }

        if ( !is_custom ) ImGui::EndDisabled ();

        /* Map button - vždy dostupné, hned za Order combo */
        ImGui::SameLine ();
        if ( ImGui::Button ( _L ( "Map" ) ) ) {
            s_editing_rule_idx = r;
            memcpy ( s_tmp_sector_map, data->rules[r].sector_map,
                     data->rules[r].sectors );
            s_show_sector_map = true;
        }

        /* Fill: 0x */
        ImGui::SameLine ();
        if ( !is_custom ) ImGui::BeginDisabled ();
        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "Fill: 0x" );
        ImGui::SameLine ();
        char fill_buf[4];
        snprintf ( fill_buf, sizeof ( fill_buf ), "%02X", data->rules[r].filler );
        ImGui::SetNextItemWidth ( 36 );
        if ( ImGui::InputText ( "##fill", fill_buf, sizeof ( fill_buf ),
                                ImGuiInputTextFlags_CharsHexadecimal ) ) {
            data->rules[r].filler = (uint8_t) strtol ( fill_buf, NULL, 16 );
        }
        if ( !is_custom ) ImGui::EndDisabled ();

        /* Remove rule - vždy dostupné (přepne na Custom) */
        ImGui::SameLine ();
        bool can_remove = ( data->count_rules > 1 );
        if ( !can_remove ) ImGui::BeginDisabled ();
        if ( ImGui::Button ( _L ( "Remove rule" ) ) ) {
            data->preset_idx = PANEL_CREATE_PRESET_CUSTOM;
            for ( int j = r; j < data->count_rules - 1; j++ ) {
                data->rules[j] = data->rules[j + 1];
            }
            data->count_rules--;
        }
        if ( !can_remove ) ImGui::EndDisabled ();

        ImGui::PopID ();
    }

    ImGui::EndChild ();

    /* ======= Disk Summary ======= */
    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    render_disk_summary ( data );

    /* ======= Format filesystem checkbox ======= */
    ImGui::Spacing ();
    {
        bool formattable = presets[data->preset_idx].formattable;
        if ( !formattable ) {
            data->format_filesystem = false;
            ImGui::BeginDisabled ();
        }
        ImGui::Checkbox ( _L ( "Format filesystem (initialize directory structures)" ),
                          &data->format_filesystem );
        if ( !formattable ) ImGui::EndDisabled ();
    }

    /* ======= Soubor ======= */
    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* Šířka sloupce labelu - dostatečná pro delší z "Filename" / "Directory"
       (v některých překladech je "Název souboru" výrazně širší než "Adresář") */
    float file_label_col = ImGui::CalcTextSize ( _ ( "Directory" ) ).x;
    float fname_w = ImGui::CalcTextSize ( _ ( "Filename" ) ).x;
    if ( fname_w > file_label_col ) file_label_col = fname_w;
    file_label_col += style.ItemSpacing.x * 2;
    if ( file_label_col < 120.0f ) file_label_col = 120.0f;

    /* Filename */
    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s", _ ( "Filename" ) );
    ImGui::SameLine ( file_label_col );
    float dsk_label_w = ImGui::CalcTextSize ( ".dsk" ).x + style.ItemSpacing.x;
    ImGui::SetNextItemWidth ( ImGui::GetContentRegionAvail ().x - dsk_label_w );
    ImGui::InputText ( "##fname", data->filename, sizeof ( data->filename ) );
    ImGui::SameLine ();
    ImGui::TextDisabled ( ".dsk" );

    /* Directory */
    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s", _ ( "Directory" ) );
    ImGui::SameLine ( file_label_col );
    float browse_w = ImGui::CalcTextSize ( _ ( "Browse..." ) ).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    ImGui::SetNextItemWidth ( ImGui::GetContentRegionAvail ().x - browse_w );
    ImGui::InputText ( "##dir", data->directory, sizeof ( data->directory ) );
    ImGui::SameLine ();
    if ( ImGui::Button ( _L ( "Browse..." ) ) ) {
        IGFD::FileDialogConfig igfd_cfg;
        igfd_cfg.path = data->directory;
        igfd_cfg.countSelectionMax = 1;
        igfd_cfg.flags = ImGuiFileDialogFlags_Modal
                       | ImGuiFileDialogFlags_DontShowHiddenFiles;
        ImGuiFileDialog::Instance ()->OpenDialog (
            CREATE_DIR_DIALOG_KEY, _ ( "Select Directory" ), nullptr, igfd_cfg
        );
    }

    /* ======= Tlačítka Close + Create ======= */
    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    {
        float btn_w = 120;
        float total_btn_w = btn_w * 2 + style.ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail ().x;
        float off = avail - total_btn_w;
        if ( off > 0.0f )
            ImGui::SetCursorPosX ( ImGui::GetCursorPosX () + off );
    }

    if ( ButtonMinWidth ( _L ( "Close" ) ) ) {
        data->is_open = false;
    }
    ImGui::SameLine ();

    bool can_create = ( data->filename[0] != '\0' )
                    && ( data->tracks > 0 )
                    && ( data->count_rules > 0 );
    if ( !can_create ) ImGui::BeginDisabled ();
    if ( ButtonMinWidth ( _L ( "Create" ) ) ) {
        panel_create_execute ( data );
        if ( data->created ) {
            /* Uložit poslední adresář do configu */
            strncpy ( cfg->last_create_dir, data->directory,
                      sizeof ( cfg->last_create_dir ) - 1 );
            cfg->last_create_dir[sizeof ( cfg->last_create_dir ) - 1] = '\0';
        }
    }
    if ( !can_create ) ImGui::EndDisabled ();

    /* Chybová hláška */
    if ( data->error_msg[0] != '\0' ) {
        ImGui::Spacing ();
        ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 1.0f, 0.3f, 0.3f, 1.0f ) );
        ImGui::TextWrapped ( "%s", data->error_msg );
        ImGui::PopStyleColor ();
    }

    /* Overwrite confirmation popup */
    if ( data->confirm_overwrite ) {
        ImGui::OpenPopup ( "##overwrite_confirm" );
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( ImGui::BeginPopupModal ( "##overwrite_confirm", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "File already exists:" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", data->created_filepath );
        ImGui::Spacing ();
        ImGui::Text ( "%s", _ ( "Overwrite?" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _L ( "Overwrite" ) ) ) {
            /* Potvrdit přepis - zavolat execute znovu (confirm_overwrite je stále true) */
            panel_create_execute ( data );
            if ( data->created ) {
                strncpy ( cfg->last_create_dir, data->directory,
                          sizeof ( cfg->last_create_dir ) - 1 );
                cfg->last_create_dir[sizeof ( cfg->last_create_dir ) - 1] = '\0';
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _L ( "Cancel" ) ) ) {
            data->confirm_overwrite = false;
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    }

    /* Sector map popup */
    draw_sector_map_popup ( data );

    ImGui::End ();

    ImGui::PopStyleColor ( 3 );
    ImGui::PopStyleVar ();

    /* ======= IGFD Directory chooser ======= */
    SetNextWindowDefaultCentered ( 900, 600 );

    /* Styl pro file dialog */
    ImGui::PushStyleVar ( ImGuiStyleVar_WindowBorderSize, 2.0f );
    ImGui::PushStyleColor ( ImGuiCol_Border, ImVec4 ( 0.40f, 0.50f, 0.80f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBg, ImVec4 ( 0.10f, 0.14f, 0.30f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBgActive, ImVec4 ( 0.14f, 0.20f, 0.42f, 1.0f ) );

    if ( ImGuiFileDialog::Instance ()->Display ( CREATE_DIR_DIALOG_KEY,
                                                  ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            snprintf ( data->directory, sizeof ( data->directory ), "%s", dir.c_str () );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    ImGui::PopStyleColor ( 3 );
    ImGui::PopStyleVar ();
}
