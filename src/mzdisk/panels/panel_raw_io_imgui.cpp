/**
 * @file panel_raw_io_imgui.cpp
 * @brief ImGui rendering pro Raw I/O okno.
 *
 * Nemodální okno s konfigurovatelným rozsahem sektorů/bloků,
 * datovými volbami (invert, byte offset/count) a cestou k souboru.
 * Podporuje Get (export) a Put (import) operace.
 *
 * Layout okna:
 *   - Hlavička s typem operace (Get/Put)
 *   - Režim adresování (T/S nebo Block) + Sector Order
 *   - Startovní pozice a počet
 *   - Datové volby (invert, byte offset, byte count)
 *   - Cesta k souboru + file offset
 *   - Tlačítka Close + Get/Put
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "panels/panel_raw_io.h"
#include "i18n.h"
}

#include "ui_helpers.h"


/** @brief Klíč pro IGFD file dialog Raw I/O. */
static const char *RAW_IO_FILE_DIALOG_KEY = "RawIOFileDlg";

/** @brief Popisky pro combo box režimu adresování. */
static const char *s_addr_mode_labels[] = {
    "Track / Sector",
    "Block",
};

/** @brief Popisky pro combo box pořadí sektorů. */
static const char *s_sector_order_labels[] = {
    "ID",
    "Phys",
};


/**
 * @brief Vykreslí help marker "(?) " s tooltipem při hoveru.
 *
 * @param text Text tooltipu.
 */
static void help_marker ( const char *text )
{
    ImGui::SameLine ();
    ImGui::TextDisabled ( "(?)" );
    if ( ImGui::BeginItemTooltip () ) {
        ImGui::PushTextWrapPos ( ImGui::GetFontSize () * 25.0f );
        ImGui::TextUnformatted ( text );
        ImGui::PopTextWrapPos ();
        ImGui::EndTooltip ();
    }
}


/**
 * @brief Vykreslí navigaci v režimu Track/Sector.
 *
 * @param data Datový model Raw I/O.
 */
static void render_ts_mode ( st_PANEL_RAW_IO_DATA *data, bool count_disabled )
{
    /* Start Track je u větších disků 3-ciferný (160+), +/- tlačítka
       ukrajují přes polovinu šířky. */
    float track_w = 140.0f;
    /* Start Sector je 1-2 cifry. */
    float sector_w = 120.0f;
    /* Count bývá řádově víc (desítky tisíc sektorů u větších disků). */
    float count_w = 220.0f;

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Start Track" ) );
    ImGui::SameLine ( 180 );
    ImGui::SetNextItemWidth ( track_w );
    int track = (int) data->start_track;
    if ( ImGui::InputInt ( "##start_track", &track, 1, 10 ) ) {
        if ( track < 0 ) track = 0;
        if ( track > (int) data->max_track ) track = (int) data->max_track;
        data->start_track = (uint16_t) track;
    }

    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s:", _ ( "Start Sector" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( sector_w );
    int sector = (int) data->start_sector;
    if ( ImGui::InputInt ( "##start_sector", &sector, 1, 1 ) ) {
        if ( sector < 1 ) sector = 1;
        data->start_sector = (uint16_t) sector;
    }

    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s:", _ ( "Count" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( count_w );
    if ( count_disabled ) ImGui::BeginDisabled ();
    int count = (int) data->sector_count;
    if ( ImGui::InputInt ( "##sector_count", &count, 1, 10 ) ) {
        if ( count < 1 ) count = 1;
        data->sector_count = (int32_t) count;
    }
    if ( count_disabled ) ImGui::EndDisabled ();
}


/**
 * @brief Vykreslí navigaci v blokovém režimu.
 *
 * @param data Datový model Raw I/O.
 */
static void render_block_mode ( st_PANEL_RAW_IO_DATA *data, bool count_disabled )
{
    /* Start Block bývá čtyř- i pětimístné (tisíce bloků na velkém disku). */
    float block_w = 220.0f;
    /* Count i pro bloky bývá řádově větší. */
    float count_w = 220.0f;

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Start Block" ) );
    ImGui::SameLine ( 180 );
    ImGui::SetNextItemWidth ( block_w );
    int block = (int) data->start_block;
    if ( ImGui::InputInt ( "##start_block", &block, 1, 10 ) ) {
        data->start_block = (int32_t) block;
    }

    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s:", _ ( "Count" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( count_w );
    if ( count_disabled ) ImGui::BeginDisabled ();
    int count = (int) data->block_count;
    if ( ImGui::InputInt ( "##block_count", &count, 1, 10 ) ) {
        if ( count < 1 ) count = 1;
        data->block_count = (int32_t) count;
    }
    if ( count_disabled ) ImGui::EndDisabled ();

    /* sklápěcí Origin konfigurace */
    ImGui::PushStyleColor ( ImGuiCol_Header, ImVec4 ( 0.12f, 0.12f, 0.20f, 1.0f ) );
    if ( ImGui::TreeNode ( _ ( "Origin" ) ) ) {
        float label_w = ImGui::CalcTextSize ( "Sectors/Block:" ).x + 8.0f;

        if ( ImGui::BeginTable ( "##raw_io_origin", 4, ImGuiTableFlags_None ) ) {
            ImGui::TableSetupColumn ( "l0", ImGuiTableColumnFlags_WidthFixed, label_w );
            ImGui::TableSetupColumn ( "v0", ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupColumn ( "l1", ImGuiTableColumnFlags_WidthFixed, label_w );
            ImGui::TableSetupColumn ( "v1", ImGuiTableColumnFlags_WidthStretch );

            /* řádek 1: Origin Track | Origin Sector */
            ImGui::TableNextRow ();
            ImGui::TableNextColumn ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( _ ( "Origin Track:" ) );
            ImGui::TableNextColumn ();
            ImGui::SetNextItemWidth ( -FLT_MIN );
            int ot = (int) data->block_config.origin_track;
            if ( ImGui::InputInt ( "##rio_orig_track", &ot, 1, 1 ) ) {
                if ( ot < 0 ) ot = 0;
                if ( ot > (int) data->max_track ) ot = (int) data->max_track;
                data->block_config.origin_track = (uint16_t) ot;
            }

            ImGui::TableNextColumn ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( _ ( "Origin Sector:" ) );
            ImGui::TableNextColumn ();
            ImGui::SetNextItemWidth ( -FLT_MIN );
            int os = (int) data->block_config.origin_sector;
            if ( ImGui::InputInt ( "##rio_orig_sector", &os, 1, 1 ) ) {
                if ( os < 1 ) os = 1;
                data->block_config.origin_sector = (uint16_t) os;
            }

            /* řádek 2: First Block # | Sectors/Block */
            ImGui::TableNextRow ();
            ImGui::TableNextColumn ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( _ ( "First Block #:" ) );
            ImGui::TableNextColumn ();
            ImGui::SetNextItemWidth ( -FLT_MIN );
            int fb = (int) data->block_config.first_block;
            if ( ImGui::InputInt ( "##rio_first_block", &fb, 1, 1 ) ) {
                data->block_config.first_block = (int32_t) fb;
            }

            ImGui::TableNextColumn ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( _ ( "Sectors/Block:" ) );
            ImGui::TableNextColumn ();
            ImGui::SetNextItemWidth ( -FLT_MIN );
            int spb = (int) data->block_config.sectors_per_block;
            if ( ImGui::InputInt ( "##rio_sec_per_block", &spb, 1, 1 ) ) {
                if ( spb < 1 ) spb = 1;
                if ( spb > PANEL_HEXDUMP_MAX_SECTORS_PER_BLOCK )
                    spb = PANEL_HEXDUMP_MAX_SECTORS_PER_BLOCK;
                data->block_config.sectors_per_block = (uint16_t) spb;
            }

            ImGui::EndTable ();
        }

        ImGui::TreePop ();
    }
    ImGui::PopStyleColor ();
}


extern "C" void panel_raw_io_render ( st_PANEL_RAW_IO_DATA *data, st_MZDSK_DISC *disc,
                                       st_PANEL_HEXDUMP_DATA *hd,
                                       bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                       uint64_t owner_id )
{
    if ( !data || !data->is_open ) return;

    bool is_get = ( data->action == RAW_IO_ACTION_GET );
    /* Put "celý soubor": jen u Put a jen když je checkbox zapnutý.
       Pro Get se žádná auto-délka nedává, uživatel Count vždy specifikuje. */
    bool whole_file_active = !is_get && data->put_whole_file;
    const char *title = is_get ? _ ( "Raw I/O - Get (Export)" )
                                : _ ( "Raw I/O - Put (Import)" );

    ImGui::SetNextWindowSize ( ImVec2 ( 0, 0 ), ImGuiCond_Always );

    /* výrazný border */
    ImGui::PushStyleVar ( ImGuiStyleVar_WindowBorderSize, 2.0f );
    ImGui::PushStyleColor ( ImGuiCol_Border, ImVec4 ( 0.40f, 0.50f, 0.80f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBg, ImVec4 ( 0.10f, 0.14f, 0.30f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBgActive, ImVec4 ( 0.14f, 0.20f, 0.42f, 1.0f ) );

    /* Unikátní window ID přes owner_id - umožní paralelní Raw I/O okna
     * pro víc sessions v multi-window režimu. 0 = kompat fallback. */
    char win_id[160];
    if ( owner_id != 0 ) {
        std::snprintf ( win_id, sizeof ( win_id ),
                        "%s###RawIOWin-%llu", title, (unsigned long long) owner_id );
    } else {
        std::snprintf ( win_id, sizeof ( win_id ), "%s###RawIOWin", title );
    }

    if ( !ImGui::Begin ( win_id, &data->is_open,
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::End ();
        ImGui::PopStyleColor ( 3 );
        ImGui::PopStyleVar ();
        return;
    }

    /* === Režim adresování + Sector Order === */
    ImGui::Text ( "%s:", _ ( "Mode" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( 200 );
    int mode = (int) data->addr_mode;
    if ( ImGui::Combo ( "##rio_addr_mode", &mode, s_addr_mode_labels,
                        IM_ARRAYSIZE ( s_addr_mode_labels ) ) ) {
        data->addr_mode = (en_HEXDUMP_ADDR_MODE) mode;
    }

    ImGui::SameLine ( 0, 30 );
    ImGui::Text ( "%s:", _ ( "Sector Order" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( 120 );
    int sord = (int) data->sector_order;
    if ( ImGui::Combo ( "##rio_sector_order", &sord, s_sector_order_labels,
                        IM_ARRAYSIZE ( s_sector_order_labels ) ) ) {
        data->sector_order = (en_HEXDUMP_SECTOR_ORDER) sord;
    }
    help_marker ( _ (
        "ID: Advances by sector ID (1, 2, 3, ...). "
        "Skips non-sequential IDs.\n\n"
        "Phys: Advances by physical position on the track."
    ) );

    ImGui::Spacing ();

    /* === Startovní pozice a počet === */
    if ( data->addr_mode == HEXDUMP_ADDR_BLOCK ) {
        render_block_mode ( data, whole_file_active );
    } else {
        render_ts_mode ( data, whole_file_active );
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* === Datové volby === */
    ImGui::Text ( "%s:", _ ( "Data" ) );
    ImGui::Spacing ();

    ImGui::Checkbox ( _L ( "Invert (XOR 0xFF)" ), &data->invert );

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Byte offset" ) );
    ImGui::SameLine ( 180 );
    /* Byte offset může jít do desítek tisíc bajtů, původních 120 px bylo
       těsných - 160 s +/- tlačítky pojme 5-6cif. */
    ImGui::SetNextItemWidth ( 160 );
    int boff = (int) data->byte_offset;
    if ( ImGui::InputInt ( "##byte_offset", &boff, 1, 16 ) ) {
        if ( boff < 0 ) boff = 0;
        data->byte_offset = (int32_t) boff;
    }

    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s:", _ ( "Byte count" ) );
    ImGui::SameLine ();
    /* Stejná šířka jako Byte offset. */
    ImGui::SetNextItemWidth ( 160 );
    if ( whole_file_active ) ImGui::BeginDisabled ();
    int bcnt = (int) data->byte_count;
    if ( ImGui::InputInt ( "##byte_count", &bcnt, 1, 16 ) ) {
        if ( bcnt < 0 ) bcnt = 0;
        data->byte_count = (int32_t) bcnt;
    }
    if ( whole_file_active ) ImGui::EndDisabled ();
    help_marker ( _ ( "0 = all (no limit)" ) );

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* === Soubor === */
    ImGui::Text ( "%s:", _ ( "File" ) );
    ImGui::Spacing ();

    const ImGuiStyle &style = ImGui::GetStyle ();
    float browse_w = ImGui::CalcTextSize ( _ ( "Browse..." ) ).x
                     + style.FramePadding.x * 2 + style.ItemSpacing.x;

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Path" ) );
    ImGui::SameLine ( 180 );
    ImGui::SetNextItemWidth ( ImGui::GetContentRegionAvail ().x - browse_w );
    ImGui::InputText ( "##rio_filepath", data->filepath, sizeof ( data->filepath ) );
    ImGui::SameLine ();
    if ( ImGui::Button ( _L ( "Browse..." ) ) ) {
        IGFD::FileDialogConfig igfd_cfg;
        igfd_cfg.path = is_get ? cfg->last_get_dir : cfg->last_put_dir;
        igfd_cfg.countSelectionMax = 1;
        igfd_cfg.flags = ImGuiFileDialogFlags_Modal
                       | ImGuiFileDialogFlags_DontShowHiddenFiles;
        if ( is_get ) {
            igfd_cfg.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
        }
        /* Regex ((.*)) místo .*: IGFD považuje ".*" za "soubor s tečkou
           a libovolnou příponou", takže skryje soubory bez přípony.
           (( ... )) je regex syntaxe, která matchuje úplně vše včetně
           souborů bez přípony. */
        ImGuiFileDialog::Instance ()->OpenDialog (
            RAW_IO_FILE_DIALOG_KEY, _ ( "Select File" ), "((.*))", igfd_cfg
        );
    }

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "File offset" ) );
    ImGui::SameLine ( 180 );
    ImGui::SetNextItemWidth ( 160 );
    int64_t foff = data->file_offset;
    int foff32 = (int) foff;
    if ( ImGui::InputInt ( "##file_offset", &foff32, 1, 256 ) ) {
        if ( foff32 < 0 ) foff32 = 0;
        data->file_offset = (int64_t) foff32;
    }

    /* Put-only: checkbox "celý soubor" - default zapnutý, potlačí ruční
       Count a Byte count, vše se spočítá podle velikosti souboru. */
    if ( !is_get ) {
        ImGui::Checkbox ( _L ( "Whole file (auto count)" ), &data->put_whole_file );
        help_marker ( _ (
            "When checked, the whole file from 'File offset' onward is "
            "written. Count and Byte count are ignored."
        ) );
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* === Tlačítka === */
    {
        float btn_w = 120;
        float total_btn_w = btn_w * 2 + style.ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail ().x;
        float off = avail - total_btn_w;
        if ( off > 0.0f )
            ImGui::SetCursorPosX ( ImGui::GetCursorPosX () + off );

        if ( ImGui::Button ( _L ( "Close" ), ImVec2 ( btn_w, 0 ) ) ) {
            data->is_open = false;
        }
        ImGui::SameLine ();

        bool can_exec = ( data->filepath[0] != '\0' );
        if ( !can_exec ) ImGui::BeginDisabled ();

        if ( is_get ) {
            if ( ImGui::Button ( _L ( "Get" ), ImVec2 ( btn_w, 0 ) ) ) {
                panel_raw_io_execute_get ( data, disc );
            }
        } else {
            if ( ImGui::Button ( _L ( "Put" ), ImVec2 ( btn_w, 0 ) ) ) {
                data->show_put_confirm = true;
            }
        }

        if ( !can_exec ) ImGui::EndDisabled ();
    }

    /* === Zprávy === */
    if ( data->show_error && data->error_msg[0] != '\0' ) {
        ImGui::Spacing ();
        ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 1.0f, 0.3f, 0.3f, 1.0f ) );
        ImGui::TextWrapped ( "%s", data->error_msg );
        ImGui::PopStyleColor ();
    }
    if ( data->show_success && data->success_msg[0] != '\0' ) {
        ImGui::Spacing ();
        ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 0.3f, 1.0f, 0.3f, 1.0f ) );
        ImGui::TextWrapped ( "%s", data->success_msg );
        ImGui::PopStyleColor ();
    }

    /* === Put potvrzovací dialog === */
    if ( data->show_put_confirm ) {
        ImGui::OpenPopup ( "##raw_io_put_confirm" );
        data->show_put_confirm = false;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( ImGui::BeginPopupModal ( "##raw_io_put_confirm", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Write data from file to disk?" ) );
        ImGui::Spacing ();
        ImGui::TextDisabled ( "%s", data->filepath );
        ImGui::Spacing ();
        ImGui::Text ( "%s", _ ( "This will overwrite disk sectors. Continue?" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ImGui::Button ( _L ( "Write" ), ImVec2 ( 120, 0 ) ) ) {
            en_MZDSK_RES res = panel_raw_io_execute_put ( data, disc );
            if ( res == MZDSK_RES_OK ) {
                if ( is_dirty ) *is_dirty = true;
                /* Zapsali jsme sektory, Hexview si drží cache posledního
                   přečteného sektoru - bez refreshe by zobrazoval starý
                   obsah. Refresh jen když uživatel předá hd. */
                if ( hd ) {
                    panel_hexdump_read_sector ( hd, disc );
                }
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ImGui::Button ( _L ( "Cancel" ), ImVec2 ( 120, 0 ) ) ) {
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    }

    ImGui::End ();

    ImGui::PopStyleColor ( 3 );
    ImGui::PopStyleVar ();

    /* === IGFD file dialog === */
    SetNextWindowDefaultCentered ( 1200, 768 );

    ImGui::PushStyleVar ( ImGuiStyleVar_WindowBorderSize, 2.0f );
    ImGui::PushStyleColor ( ImGuiCol_Border, ImVec4 ( 0.40f, 0.50f, 0.80f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBg, ImVec4 ( 0.10f, 0.14f, 0.30f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBgActive, ImVec4 ( 0.14f, 0.20f, 0.42f, 1.0f ) );

    if ( ImGuiFileDialog::Instance ()->Display ( RAW_IO_FILE_DIALOG_KEY,
                                                  ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            std::string fpath = ImGuiFileDialog::Instance ()->GetFilePathName ();
            std::string dpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            std::snprintf ( data->filepath, sizeof ( data->filepath ),
                            "%s", fpath.c_str () );

            /* uložit poslední adresář */
            if ( is_get ) {
                std::snprintf ( cfg->last_get_dir, sizeof ( cfg->last_get_dir ),
                                "%s", dpath.c_str () );
            } else {
                std::snprintf ( cfg->last_put_dir, sizeof ( cfg->last_put_dir ),
                                "%s", dpath.c_str () );
            }
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    ImGui::PopStyleColor ( 3 );
    ImGui::PopStyleVar ();
}
