/**
 * @file panel_hexdump_imgui.cpp
 * @brief ImGui rendering hexdump panelu s editačním režimem.
 *
 * Vykresluje barevný hexdump s navigací po stopách/sektorech
 * nebo po blocích s konfigurovatelným počátkem a pořadím sektorů
 * (ID/Phys).
 *
 * Formát: offset | hex bajty (8+8) | textový sloupec.
 *
 * Barevné kódování hex bajtů:
 *   0x00       - šedá
 *   0x20-0x7E  - zelená (printable ASCII)
 *   0xFF       - tlumeně červená
 *   ostatní    - světlá
 *   změněné    - žlutá (v editačním režimu)
 *
 * Textový sloupec podporuje devět režimů kódování:
 *   Raw                  - printable ASCII (0x20-0x7E), ostatní '.'
 *   SharpMZ-EU -> ASCII  - jednobajtová konverze (evropská)
 *   SharpMZ-JP -> ASCII  - jednobajtová konverze (japonská)
 *   SharpMZ-EU -> UTF-8  - konverze na UTF-8 (evropská varianta)
 *   SharpMZ-JP -> UTF-8  - konverze na UTF-8 (japonská varianta)
 *   SharpMZ-EU -> MZ-CG1 - MZ ASCII -> video kód -> mzglyphs EU sada 1
 *   SharpMZ-EU -> MZ-CG2 - MZ ASCII -> video kód -> mzglyphs EU sada 2
 *   SharpMZ-JP -> MZ-CG1 - MZ ASCII -> video kód -> mzglyphs JP sada 1
 *   SharpMZ-JP -> MZ-CG2 - MZ ASCII -> video kód -> mzglyphs JP sada 2
 *
 * Editační režim:
 *   - Kurzor (obdélník) v hex nebo ASCII sloupci
 *   - Hex editace: číslice 0-9, A-F přepisují nibble
 *   - ASCII editace: printable znaky přepisují bajt
 *   - Tab přepíná fokus mezi hex a ASCII sloupcem
 *   - Šipky, Home, End pro navigaci
 *   - Změněné bajty zvýrazněny žlutě
 *   - Write zapíše na disk, Revert obnoví z undo bufferu
 *
 * Hexdump se vykresluje přes DrawList s fixními pozicemi sloupců
 * pro zaručené zarovnání nezávisle na fontu.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "panels/panel_hexdump.h"
#include "panels/panel_raw_io.h"
#include "i18n.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzglyphs/mzglyphs.h"
#include "libs/mz_vcode/mz_vcode.h"
}

#include "ui_helpers.h"


/** @brief Počet bajtů na jeden řádek hexdumpu. */
static const int COLS = 16;


/**
 * @brief Vrátí barvu pro daný bajt (pro hex sloupec).
 *
 * @param b Hodnota bajtu.
 * @return Barva jako ImU32 (pro DrawList).
 */
static ImU32 byte_color ( uint8_t b )
{
    if ( b == 0x00 )                return IM_COL32 ( 100, 100, 120, 255 );  /* šedá */
    if ( b >= 0x20 && b <= 0x7E )   return IM_COL32 ( 100, 230, 100, 255 ); /* zelená */
    if ( b == 0xFF )                return IM_COL32 ( 180, 100, 100, 255 );  /* tlumeně červená */
    return IM_COL32 ( 210, 210, 220, 255 ); /* světlá */
}


/** @brief Barva pro změněný bajt v editačním režimu. */
static const ImU32 COL_MODIFIED = IM_COL32 ( 255, 220, 60, 255 );

/** @brief Barva pozadí kurzoru. */
static const ImU32 COL_CURSOR_BG = IM_COL32 ( 60, 80, 160, 180 );

/** @brief Barva obrysu kurzoru v neaktivním sloupci. */
static const ImU32 COL_CURSOR_OUTLINE = IM_COL32 ( 80, 100, 180, 160 );


/**
 * @brief Popisky pro combo box výběru kódování.
 */
static const char *charset_labels[] = {
    "Raw",
    "SharpMZ-EU -> ASCII",
    "SharpMZ-JP -> ASCII",
    "SharpMZ-EU -> UTF-8",
    "SharpMZ-JP -> UTF-8",
    "SharpMZ-EU -> MZ-CG1",
    "SharpMZ-EU -> MZ-CG2",
    "SharpMZ-JP -> MZ-CG1",
    "SharpMZ-JP -> MZ-CG2",
};


/**
 * @brief Popisky pro combo box režimu adresování.
 */
static const char *addr_mode_labels[] = {
    "Track / Sector",
    "Block",
};


/**
 * @brief Popisky pro combo box pořadí sektorů.
 */
static const char *sector_order_labels[] = {
    "ID",
    "Phys",
};


/**
 * @brief Vykreslí help marker "(?) " s tooltipem při hoveru.
 *
 * Klasický ImGui vzor: šedý text "(?) " na stejném řádku,
 * po najetí myší zobrazí tooltip s popisným textem.
 *
 * @param text Text tooltipu (může být víceřádkový).
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
 * @param hd Datový model.
 * @param disc Disk pro čtení při navigaci.
 * @return true pokud se navigace změnila.
 */
static bool render_nav_track_sector ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc )
{
    (void) disc;
    bool changed = false;

    /* stopa */
    ImGui::Text ( "%s:", _ ( "Track" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( 180 );
    int track = (int) hd->track;
    if ( ImGui::InputInt ( "##track", &track, 1, 10, ImGuiInputTextFlags_CharsDecimal ) ) {
        if ( track < 0 ) track = 0;
        if ( track > (int) hd->max_track ) track = (int) hd->max_track;
        hd->track = (uint16_t) track;
        hd->sector = 1;
        changed = true;
    }

    ImGui::SameLine ( 0, 20 );

    /* sektor */
    ImGui::Text ( "%s:", _ ( "Sector" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( 180 );
    int sector = (int) hd->sector;
    if ( ImGui::InputInt ( "##sector", &sector, 1, 1, ImGuiInputTextFlags_CharsDecimal ) ) {
        if ( sector < 1 ) sector = 1;
        if ( sector > (int) hd->max_sector ) sector = (int) hd->max_sector;
        hd->sector = (uint16_t) sector;
        changed = true;
    }

    ImGui::SameLine ( 0, 20 );
    ImGui::TextDisabled ( "(%d/%d, %d B)", hd->sector, hd->max_sector, hd->sector_size );

    return changed;
}


/**
 * @brief Vykreslí navigaci v blokovém režimu.
 *
 * Hlavní řádek: číslo bloku.
 * Sklápěcí sekce "Origin": konfigurace počátku + presety.
 * Info řádek: mapování bloku na track/sector.
 *
 * @param hd Datový model.
 * @param disc Disk pro čtení při navigaci.
 * @param detect Výsledek auto-detekce FS (pro presety, může být NULL).
 * @return true pokud se navigace změnila.
 */
static bool render_nav_block ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc,
                               const st_MZDSK_DETECT_RESULT *detect )
{
    bool changed = false;

    /* hlavní řádek: číslo bloku */
    ImGui::Text ( "%s:", _ ( "Block" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( 200 );
    int block = (int) hd->block;
    if ( ImGui::InputInt ( "##block", &block, 1, 10, ImGuiInputTextFlags_CharsDecimal ) ) {
        hd->block = (int32_t) block;
        changed = true;
    }

    /* info: mapování na track/sector */
    ImGui::SameLine ( 0, 20 );
    uint16_t map_track, map_sector;
    if ( panel_hexdump_block_to_track_sector ( hd, disc, &map_track, &map_sector ) ) {
        uint16_t info_sectors, info_ssize;
        /* zjistit velikost sektoru na cílové stopě */
        st_DSK_TOOLS_TRACKS_RULES_INFO *tr = disc->tracks_rules;
        info_sectors = 0;
        info_ssize = 256;
        if ( tr ) {
            for ( int i = 0; i < (int) tr->count_rules; i++ ) {
                st_DSK_TOOLS_TRACK_RULE_INFO *r = &tr->rule[i];
                if ( map_track >= r->from_track && map_track < r->from_track + r->count_tracks ) {
                    info_sectors = r->sectors;
                    info_ssize = (uint16_t) ( 128 << (uint8_t) r->ssize );
                    break;
                }
            }
        }
        uint16_t total_bytes = info_ssize * hd->block_config.sectors_per_block;
        (void) info_sectors;
        ImGui::TextDisabled ( "-> Track %d, Sector %d (%d B)",
                              map_track, map_sector, total_bytes );
    } else {
        ImGui::TextColored ( ImVec4 ( 1, 0.3f, 0.3f, 1 ), "%s", _ ( "Out of range" ) );
    }

    /* sklápěcí sekce: konfigurace originu */
    ImGui::PushStyleColor ( ImGuiCol_Header, ImVec4 ( 0.12f, 0.12f, 0.20f, 1.0f ) );
    if ( ImGui::TreeNode ( _ ( "Origin" ) ) ) {
        bool cfg_changed = false;

        /* presety */
        if ( ImGui::Button ( _ ( "Whole Disk" ) ) ) {
            panel_hexdump_preset_whole_disk ( hd );
            cfg_changed = true;
        }

        if ( detect && detect->type == MZDSK_FS_CPM ) {
            ImGui::SameLine ();
            if ( ImGui::Button ( _ ( "CP/M Blocks" ) ) ) {
                panel_hexdump_preset_cpm ( hd, &detect->cpm_dpb );
                cfg_changed = true;
            }
        }

        ImGui::Spacing ();

        /* 4-sloupcová tabulka: label | input | label | input */
        float label_w = ImGui::CalcTextSize ( "Sectors/Block:" ).x + 8.0f;

        if ( ImGui::BeginTable ( "##origin_cfg", 4, ImGuiTableFlags_None ) ) {
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
            int ot = (int) hd->block_config.origin_track;
            if ( ImGui::InputInt ( "##orig_track", &ot, 1, 1, ImGuiInputTextFlags_CharsDecimal ) ) {
                if ( ot < 0 ) ot = 0;
                if ( ot > (int) hd->max_track ) ot = (int) hd->max_track;
                hd->block_config.origin_track = (uint16_t) ot;
                cfg_changed = true;
            }

            ImGui::TableNextColumn ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( _ ( "Origin Sector:" ) );

            ImGui::TableNextColumn ();
            ImGui::SetNextItemWidth ( -FLT_MIN );
            int os = (int) hd->block_config.origin_sector;
            if ( ImGui::InputInt ( "##orig_sector", &os, 1, 1, ImGuiInputTextFlags_CharsDecimal ) ) {
                if ( os < 1 ) os = 1;
                hd->block_config.origin_sector = (uint16_t) os;
                cfg_changed = true;
            }

            /* řádek 2: First Block # | Sectors/Block */
            ImGui::TableNextRow ();

            ImGui::TableNextColumn ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( _ ( "First Block #:" ) );

            ImGui::TableNextColumn ();
            ImGui::SetNextItemWidth ( -FLT_MIN );
            int fb = (int) hd->block_config.first_block;
            if ( ImGui::InputInt ( "##first_block", &fb, 1, 1, ImGuiInputTextFlags_CharsDecimal ) ) {
                hd->block_config.first_block = (int32_t) fb;
                cfg_changed = true;
            }

            ImGui::TableNextColumn ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( _ ( "Sectors/Block:" ) );

            ImGui::TableNextColumn ();
            ImGui::SetNextItemWidth ( -FLT_MIN );
            int spb = (int) hd->block_config.sectors_per_block;
            if ( ImGui::InputInt ( "##sec_per_block", &spb, 1, 1, ImGuiInputTextFlags_CharsDecimal ) ) {
                if ( spb < 1 ) spb = 1;
                if ( spb > PANEL_HEXDUMP_MAX_SECTORS_PER_BLOCK )
                    spb = PANEL_HEXDUMP_MAX_SECTORS_PER_BLOCK;
                hd->block_config.sectors_per_block = (uint16_t) spb;
                cfg_changed = true;
            }

            /* řádek 3: Sector Order | (prázdný) */
            ImGui::TableNextRow ();

            ImGui::TableNextColumn ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( _ ( "Sector Order:" ) );

            ImGui::TableNextColumn ();
            ImGui::SetNextItemWidth ( 150 );
            int sord = (int) hd->block_config.sector_order;
            if ( ImGui::Combo ( "##sector_order", &sord, sector_order_labels,
                                IM_ARRAYSIZE ( sector_order_labels ) ) ) {
                hd->block_config.sector_order = (en_HEXDUMP_SECTOR_ORDER) sord;
                cfg_changed = true;
            }
            help_marker ( _ (
                "ID: Advances by sector ID (1, 2, 3, ...). "
                "If the next ID is not found on the track, "
                "moves to the next track starting from ID 1. "
                "Sectors with non-sequential IDs are skipped.\n\n"
                "Phys: Advances by physical position on the track "
                "as stored in the DSK image. Includes all sectors "
                "regardless of their ID numbering."
            ) );

            ImGui::TableNextColumn ();
            ImGui::TableNextColumn ();

            ImGui::EndTable ();
        }

        if ( cfg_changed ) changed = true;

        ImGui::TreePop ();
    }
    ImGui::PopStyleColor ();

    return changed;
}


/**
 * @brief Vykreslí navigační řádek (režim adresování, stopa/sektor/blok, inverze, kódování).
 *
 * Pokud je aktivní editace s neuloženými změnami a uživatel změní pozici,
 * navigace se zablokuje a nastaví pending_nav_discard pro potvrzovací dialog.
 *
 * @param hd Datový model.
 * @param disc Disk pro čtení při navigaci.
 * @param detect Výsledek auto-detekce FS (pro presety, může být NULL).
 * @return true pokud se navigace změnila (je třeba znovu načíst data).
 */
static bool render_navigation ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc,
                                const st_MZDSK_DETECT_RESULT *detect )
{
    bool changed = false;

    /* v editačním režimu s neuloženými změnami zakázat navigaci */
    bool nav_disabled = hd->edit_mode && hd->edit_dirty;
    if ( nav_disabled ) ImGui::BeginDisabled ();

    /* combo pro režim adresování */
    ImGui::Text ( "%s:", _ ( "Mode" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( 220 );
    int mode = (int) hd->addr_mode;
    if ( ImGui::Combo ( "##addr_mode", &mode, addr_mode_labels,
                        IM_ARRAYSIZE ( addr_mode_labels ) ) ) {
        hd->addr_mode = (en_HEXDUMP_ADDR_MODE) mode;
        changed = true;
    }

    ImGui::SameLine ( 0, 30 );

    /* navigace dle zvoleného režimu */
    if ( hd->addr_mode == HEXDUMP_ADDR_TRACK_SECTOR ) {
        if ( render_nav_track_sector ( hd, disc ) ) changed = true;
    } else {
        if ( render_nav_block ( hd, disc, detect ) ) changed = true;
    }

    if ( nav_disabled ) ImGui::EndDisabled ();

    /* druhý řádek: inverze + kódování */
    ImGui::Checkbox ( _L ( "Invert (XOR 0xFF)" ), &hd->invert );

    ImGui::SameLine ( 0, 30 );

    ImGui::Text ( "%s:", _ ( "Encoding" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( 280 );
    ImGui::Combo ( "##charset", (int *) &hd->charset, charset_labels,
                   IM_ARRAYSIZE ( charset_labels ) );

    if ( changed ) {
        if ( hd->edit_mode && !hd->edit_dirty ) {
            /* editace aktivní, ale žádné změny - bezpečně přenačíst */
            hd->edit_mode = false;
        }
        panel_hexdump_read_sector ( hd, disc );
    }

    return changed;
}


/**
 * @brief Vykreslí toolbar editačního režimu (Edit/Write/Revert).
 *
 * @param hd Datový model.
 * @param disc Otevřený disk.
 * @param is_dirty Ukazatel na session dirty flag.
 */
static void render_edit_toolbar ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc,
                                  bool *is_dirty )
{
    if ( !hd->edit_mode ) {
        /* tlačítko pro vstup do editace */
        bool can_edit = hd->is_loaded && !hd->read_error;
        if ( !can_edit ) ImGui::BeginDisabled ();
        if ( ImGui::Button ( _L ( "Edit" ) ) ) {
            panel_hexdump_enter_edit ( hd );
        }
        if ( !can_edit ) ImGui::EndDisabled ();
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) ) {
            ImGui::SetTooltip ( "%s", _ ( "Enter edit mode" ) );
        }
    } else {
        /* editační toolbar: Write + Revert + indikátor */
        bool has_changes = hd->edit_dirty;

        if ( !has_changes ) ImGui::BeginDisabled ();
        if ( ImGui::Button ( _L ( "Write" ) ) ) {
            en_MZDSK_RES res = panel_hexdump_write_data ( hd, disc );
            if ( res == MZDSK_RES_OK && is_dirty ) {
                *is_dirty = true;
            }
        }
        if ( !has_changes ) ImGui::EndDisabled ();
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) ) {
            ImGui::SetTooltip ( "%s", _ ( "Write changes to disk" ) );
        }

        ImGui::SameLine ();

        if ( !has_changes ) ImGui::BeginDisabled ();
        if ( ImGui::Button ( _L ( "Revert" ) ) ) {
            panel_hexdump_revert_edit ( hd );
            panel_hexdump_enter_edit ( hd );
        }
        if ( !has_changes ) ImGui::EndDisabled ();
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) ) {
            ImGui::SetTooltip ( "%s", _ ( "Revert to original data" ) );
        }

        ImGui::SameLine ();

        if ( has_changes ) ImGui::BeginDisabled ();
        if ( ImGui::Button ( _L ( "Close Editor" ) ) ) {
            panel_hexdump_revert_edit ( hd );
        }
        if ( has_changes ) ImGui::EndDisabled ();

        /* indikátor změn */
        if ( has_changes ) {
            ImGui::SameLine ( 0, 20 );
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.85f, 0.2f, 1.0f ), "%s",
                                 _ ( "Modified" ) );
        }

        /* Chybová hláška při neúspěšné konverzi znaku v ASCII editaci.
         * Zobrazuje se pod tlačítky, dokud uživatel nezadá úspěšně jiný
         * znak nebo neopustí editaci. Audit H-22 - bez tohoto GUI tiše
         * zapsal 0x20 (mezeru) pro neznámé znaky = data corruption. */
        if ( hd->edit_convert_error && hd->edit_convert_error_msg[0] != '\0' ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ),
                                 "%s", hd->edit_convert_error_msg );
        }
    }
}


/**
 * @brief Vykreslí potvrzovací dialog pro zahození neuložených změn.
 *
 * @param hd Datový model.
 */
static void render_discard_popup ( st_PANEL_HEXDUMP_DATA *hd )
{
    if ( hd->pending_nav_discard ) {
        ImGui::OpenPopup ( _ ( "Unsaved Changes" ) );
        hd->pending_nav_discard = false;
    }

    if ( ImGui::BeginPopupModal ( _ ( "Unsaved Changes" ), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( "%s", _ ( "Discard unsaved changes?" ) );
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Discard" ) ) ) {
            panel_hexdump_revert_edit ( hd );
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
}


/**
 * @brief Konvertuje bajt na zobrazitelný znak/řetězec podle zvoleného kódování.
 *
 * Pro režimy Raw a ASCII vrací jednoznakový řetězec.
 * Pro UTF-8 a MZ-CG režimy může vrátit vícebajtový UTF-8 řetězec.
 *
 * MZ-CG režimy: MZ-ASCII -> video kód (mz_vcode) -> mzglyphs UTF-8 glyf.
 *
 * @param b          Hodnota bajtu (po případné inverzi).
 * @param charset    Zvolené kódování.
 * @param[out] buf   Výstupní buffer (min 8 bajtů).
 * @return Ukazatel na buf.
 */
static const char* byte_to_display_char ( uint8_t b, en_HEXDUMP_CHARSET charset, char *buf )
{
    switch ( charset ) {
        case HEXDUMP_CHARSET_RAW:
            buf[0] = ( b >= 0x20 && b <= 0x7E ) ? (char) b : '.';
            buf[1] = '\0';
            break;

        case HEXDUMP_CHARSET_SHARPMZ_EU_ASCII: {
            int converted, printable;
            uint8_t c = sharpmz_convert_to_ASCII ( b, &converted, &printable );
            buf[0] = ( converted && printable ) ? (char) c : '.';
            buf[1] = '\0';
            break;
        }

        case HEXDUMP_CHARSET_SHARPMZ_JP_ASCII: {
            int converted, printable;
            uint8_t c = sharpmz_jp_convert_to_ASCII ( b, &converted, &printable );
            buf[0] = ( converted && printable ) ? (char) c : '.';
            buf[1] = '\0';
            break;
        }

        case HEXDUMP_CHARSET_SHARPMZ_EU_UTF8: {
            int converted, printable;
            const char *utf = sharpmz_eu_convert_to_UTF8 ( b, &converted, &printable );
            if ( converted && printable ) {
                strncpy ( buf, utf, 7 );
                buf[7] = '\0';
            } else {
                buf[0] = '.';
                buf[1] = '\0';
            }
            break;
        }

        case HEXDUMP_CHARSET_SHARPMZ_JP_UTF8: {
            int converted, printable;
            const char *utf = sharpmz_jp_convert_to_UTF8 ( b, &converted, &printable );
            if ( converted && printable ) {
                strncpy ( buf, utf, 7 );
                buf[7] = '\0';
            } else {
                buf[0] = '.';
                buf[1] = '\0';
            }
            break;
        }

        case HEXDUMP_CHARSET_SHARPMZ_EU_CG1: {
            uint8_t vcode = mz_vcode_from_ascii_dump ( b, MZ_VCODE_EU );
            mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_EU1, buf );
            break;
        }

        case HEXDUMP_CHARSET_SHARPMZ_EU_CG2: {
            uint8_t vcode = mz_vcode_from_ascii_dump ( b, MZ_VCODE_EU );
            mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_EU2, buf );
            break;
        }

        case HEXDUMP_CHARSET_SHARPMZ_JP_CG1: {
            uint8_t vcode = mz_vcode_from_ascii_dump ( b, MZ_VCODE_JP );
            mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_JP1, buf );
            break;
        }

        case HEXDUMP_CHARSET_SHARPMZ_JP_CG2: {
            uint8_t vcode = mz_vcode_from_ascii_dump ( b, MZ_VCODE_JP );
            mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_JP2, buf );
            break;
        }

        default:
            buf[0] = '.';
            buf[1] = '\0';
            break;
    }

    return buf;
}


/**
 * @brief Kontext pro pozicování buněk hexdumpu.
 *
 * Sdílený výpočet pozic sloupců pro rendering i klikání myší.
 */
typedef struct {
    float char_w;           /**< Šířka jednoho znaku. */
    float line_h;           /**< Výška řádku. */
    float space_w;          /**< Šířka mezery. */
    float x_hex;            /**< X pozice začátku hex sloupce. */
    float hex_pair_w;       /**< Šířka jednoho hex páru ("HH "). */
    float hex_group_gap;    /**< Extra mezera mezi skupinami 8 bajtů. */
    float x_text;           /**< X pozice začátku textového sloupce. */
    ImVec2 origin;          /**< Počátek vykreslování (screen coords). */
} st_HEX_LAYOUT;


/**
 * @brief Inicializuje layout kontext pro hexdump rendering.
 *
 * @param[out] lay Výstupní layout.
 */
static void hex_layout_init ( st_HEX_LAYOUT *lay )
{
    lay->char_w = ImGui::CalcTextSize ( "0" ).x;
    lay->line_h = ImGui::GetTextLineHeightWithSpacing ();
    lay->space_w = ImGui::CalcTextSize ( " " ).x;
    lay->x_hex = lay->char_w * 5 + lay->space_w * 2;  /* "OOOO:  " */
    lay->hex_pair_w = lay->char_w * 2 + lay->space_w;  /* "HH " */
    lay->hex_group_gap = lay->space_w;
    lay->x_text = lay->x_hex + lay->hex_pair_w * COLS + lay->hex_group_gap + lay->space_w * 2;
    lay->origin = ImGui::GetCursorScreenPos ();
}


/**
 * @brief Vypočte screen pozici hex buňky pro daný byte index.
 *
 * @param lay Layout kontext.
 * @param idx Byte index (0-based).
 * @return Screen pozice levého horního rohu hex buňky.
 */
static ImVec2 hex_cell_pos ( const st_HEX_LAYOUT *lay, int idx )
{
    int line = idx / COLS;
    int col = idx % COLS;
    float extra = ( col >= 8 ) ? lay->hex_group_gap : 0;
    float x = lay->origin.x + lay->x_hex + col * lay->hex_pair_w + extra;
    float y = lay->origin.y + line * lay->line_h;
    return ImVec2 ( x, y );
}


/**
 * @brief Vypočte screen pozici ASCII buňky pro daný byte index.
 *
 * @param lay Layout kontext.
 * @param idx Byte index (0-based).
 * @return Screen pozice levého horního rohu ASCII buňky.
 */
static ImVec2 ascii_cell_pos ( const st_HEX_LAYOUT *lay, int idx )
{
    int line = idx / COLS;
    int col = idx % COLS;
    float x = lay->origin.x + lay->x_text + col * lay->char_w;
    float y = lay->origin.y + line * lay->line_h;
    return ImVec2 ( x, y );
}


/**
 * @brief Zjistí byte index pod pozicí myši v hexdump oblasti.
 *
 * @param lay Layout kontext.
 * @param mouse Pozice myši (screen coords).
 * @param total_size Celková velikost dat.
 * @param[out] in_ascii true pokud klik byl v ASCII sloupci, false pro hex.
 * @return Byte index (0-based), nebo -1 pokud mimo oblast.
 */
static int hit_test ( const st_HEX_LAYOUT *lay, ImVec2 mouse, int total_size,
                      bool *in_ascii )
{
    float rel_y = mouse.y - lay->origin.y;
    if ( rel_y < 0 ) return -1;

    int line = (int) ( rel_y / lay->line_h );
    int lines = ( total_size + COLS - 1 ) / COLS;
    if ( line >= lines ) return -1;

    float rel_x = mouse.x - lay->origin.x;

    /* zkusit hex sloupec */
    float hex_start = lay->x_hex;
    float hex_end = lay->x_hex + lay->hex_pair_w * COLS + lay->hex_group_gap;

    if ( rel_x >= hex_start && rel_x < hex_end ) {
        /* odečíst gap pro druhou skupinu */
        float x_in_hex = rel_x - hex_start;
        int col;

        /* spočítat gap: prvních 8 sloupců bez gapu, zbytek s gapem */
        float first_group_w = lay->hex_pair_w * 8;
        if ( x_in_hex < first_group_w ) {
            col = (int) ( x_in_hex / lay->hex_pair_w );
        } else {
            float x_after_gap = x_in_hex - first_group_w - lay->hex_group_gap;
            if ( x_after_gap < 0 ) return -1; /* v gapu */
            col = 8 + (int) ( x_after_gap / lay->hex_pair_w );
        }

        if ( col >= 0 && col < COLS ) {
            int idx = line * COLS + col;
            if ( idx < total_size ) {
                *in_ascii = false;
                return idx;
            }
        }
        return -1;
    }

    /* zkusit ASCII sloupec */
    float text_start = lay->x_text;
    float text_end = lay->x_text + lay->char_w * COLS;

    if ( rel_x >= text_start && rel_x < text_end ) {
        int col = (int) ( ( rel_x - text_start ) / lay->char_w );
        if ( col >= 0 && col < COLS ) {
            int idx = line * COLS + col;
            if ( idx < total_size ) {
                *in_ascii = true;
                return idx;
            }
        }
    }

    return -1;
}


/**
 * @brief Zpracuje klávesový vstup v editačním režimu.
 *
 * @param hd Datový model.
 */
static void handle_edit_input ( st_PANEL_HEXDUMP_DATA *hd )
{
    if ( !hd->edit_mode || !hd->is_loaded || hd->data_size == 0 ) return;

    /* nezpracovávat vstup pokud je aktivní jiný widget (InputInt, Combo, ...) */
    if ( ImGui::IsAnyItemActive () ) return;

    /* nezpracovávat vstup pokud je otevřený popup (discard dialog, ...) */
    if ( ImGui::IsPopupOpen ( (ImGuiID) 0, ImGuiPopupFlags_AnyPopupId ) ) return;

    ImGuiIO &io = ImGui::GetIO ();

    int max_pos = (int) hd->data_size - 1;

    /* navigace */
    if ( ImGui::IsKeyPressed ( ImGuiKey_LeftArrow ) ) {
        if ( hd->cursor_pos > 0 ) {
            hd->cursor_pos--;
            hd->cursor_high_nibble = true;
        }
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_RightArrow ) ) {
        if ( hd->cursor_pos < max_pos ) {
            hd->cursor_pos++;
            hd->cursor_high_nibble = true;
        }
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_UpArrow ) ) {
        if ( hd->cursor_pos >= COLS ) {
            hd->cursor_pos -= COLS;
            hd->cursor_high_nibble = true;
        }
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_DownArrow ) ) {
        if ( hd->cursor_pos + COLS <= max_pos ) {
            hd->cursor_pos += COLS;
            hd->cursor_high_nibble = true;
        }
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_Home ) ) {
        hd->cursor_pos = ( hd->cursor_pos / COLS ) * COLS;
        hd->cursor_high_nibble = true;
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_End ) ) {
        int line_end = ( hd->cursor_pos / COLS ) * COLS + COLS - 1;
        if ( line_end > max_pos ) line_end = max_pos;
        hd->cursor_pos = line_end;
        hd->cursor_high_nibble = true;
    }

    /* Tab: přepnout hex <-> ASCII */
    if ( ImGui::IsKeyPressed ( ImGuiKey_Tab ) ) {
        hd->cursor_in_ascii = !hd->cursor_in_ascii;
        hd->cursor_high_nibble = true;
    }

    /* Escape: opustit editaci */
    if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) {
        if ( hd->edit_dirty ) {
            hd->pending_nav_discard = true;
        } else {
            panel_hexdump_revert_edit ( hd );
        }
        return;
    }

    /*
     * Datový vstup přes IsKeyPressed - nezávisí na SDL_StartTextInput,
     * funguje spolehlivě na všech platformách.
     */

    /* mapovací tabulka: ImGuiKey -> hex nibble hodnota.
     * Včetně číslic z numerické klávesnice (Keypad0-Keypad9). */
    static const struct { ImGuiKey key; int nibble; char ascii; } hex_keys[] = {
        { ImGuiKey_0, 0, '0' }, { ImGuiKey_1, 1, '1' }, { ImGuiKey_2, 2, '2' },
        { ImGuiKey_3, 3, '3' }, { ImGuiKey_4, 4, '4' }, { ImGuiKey_5, 5, '5' },
        { ImGuiKey_6, 6, '6' }, { ImGuiKey_7, 7, '7' }, { ImGuiKey_8, 8, '8' },
        { ImGuiKey_9, 9, '9' }, { ImGuiKey_A, 10, 'A' }, { ImGuiKey_B, 11, 'B' },
        { ImGuiKey_C, 12, 'C' }, { ImGuiKey_D, 13, 'D' }, { ImGuiKey_E, 14, 'E' },
        { ImGuiKey_F, 15, 'F' },
        { ImGuiKey_Keypad0, 0, '0' }, { ImGuiKey_Keypad1, 1, '1' },
        { ImGuiKey_Keypad2, 2, '2' }, { ImGuiKey_Keypad3, 3, '3' },
        { ImGuiKey_Keypad4, 4, '4' }, { ImGuiKey_Keypad5, 5, '5' },
        { ImGuiKey_Keypad6, 6, '6' }, { ImGuiKey_Keypad7, 7, '7' },
        { ImGuiKey_Keypad8, 8, '8' }, { ImGuiKey_Keypad9, 9, '9' },
    };

    if ( hd->cursor_in_ascii ) {
        /*
         * ASCII editace: zapsaný znak závisí na zvoleném kódování.
         * - Raw: uloží se přímo ASCII hodnota, blokuje znaky > 0x7E.
         * - SharpMZ-EU/JP -> ASCII: reverzní konverze přes sharpmz_cnv_to/jp_cnv_to.
         * - SharpMZ-EU/JP -> UTF-8, MZ-CG: reverzní UTF-8 konverze.
         */
        uint8_t typed = 0; /* napsaný ASCII znak (0 = žádný) */

        /* písmena A-Z */
        for ( int k = ImGuiKey_A; k <= ImGuiKey_Z; k++ ) {
            if ( ImGui::IsKeyPressed ( (ImGuiKey) k ) ) {
                typed = (uint8_t) ( io.KeyShift ? ( 'A' + ( k - ImGuiKey_A ) )
                                                : ( 'a' + ( k - ImGuiKey_A ) ) );
                break;
            }
        }

        /* číslice 0-9 (horní řada) */
        if ( !typed ) {
            for ( int k = ImGuiKey_0; k <= ImGuiKey_9; k++ ) {
                if ( ImGui::IsKeyPressed ( (ImGuiKey) k ) ) {
                    typed = (uint8_t) ( '0' + ( k - ImGuiKey_0 ) );
                    break;
                }
            }
        }

        /* číslice z numerické klávesnice (Keypad0-Keypad9) */
        if ( !typed ) {
            for ( int k = ImGuiKey_Keypad0; k <= ImGuiKey_Keypad9; k++ ) {
                if ( ImGui::IsKeyPressed ( (ImGuiKey) k ) ) {
                    typed = (uint8_t) ( '0' + ( k - ImGuiKey_Keypad0 ) );
                    break;
                }
            }
        }

        /* Speciální znaky a interpunkce. Pro znaky, které nemají vlastní
         * ImGuiKey (např. ';', ':', '+', '*', '!'), používáme
         * ImGui::GetIO().InputQueueCharacters - ten obsahuje UTF-8 kódy
         * všech zadaných znaků a respektuje layout klávesnice (audit L-21). */
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_Space ) )  typed = ' ';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_Period ) ) typed = '.';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_Minus ) )  typed = '-';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_Comma ) )  typed = ',';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_Slash ) )  typed = '/';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_Equal ) )  typed = '=';
        /* numpad speciální znaky */
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_KeypadDecimal ) )  typed = '.';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_KeypadDivide ) )   typed = '/';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_KeypadMultiply ) ) typed = '*';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_KeypadSubtract ) ) typed = '-';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_KeypadAdd ) )      typed = '+';
        if ( !typed && ImGui::IsKeyPressed ( ImGuiKey_KeypadEqual ) )    typed = '=';
        /* Fallback přes znakovou frontu - pokryje všechny ASCII znaky,
         * které nemají ImGuiKey enum (;, :, +, *, !, ?, ', ", @, #, ...). */
        if ( !typed ) {
            for ( int ci = 0; ci < io.InputQueueCharacters.Size; ci++ ) {
                ImWchar wc = io.InputQueueCharacters [ ci ];
                /* jen tisknutelné ASCII (0x20-0x7E) */
                if ( wc >= 0x20 && wc <= 0x7E ) {
                    /* vyloučit A-Z/a-z/0-9 - ty už máme přes IsKeyPressed */
                    if ( ( wc >= 'A' && wc <= 'Z' ) ||
                         ( wc >= 'a' && wc <= 'z' ) ||
                         ( wc >= '0' && wc <= '9' ) ) continue;
                    typed = (uint8_t) wc;
                    break;
                }
            }
        }

        if ( typed ) {
            /* konverze dle zvoleného kódování */
            uint8_t value = typed;
            bool valid = true;
            const char *charset_name = "";

            switch ( hd->charset ) {
                case HEXDUMP_CHARSET_RAW:
                    /* Raw: pouze standardní ASCII (0x20-0x7E) */
                    if ( typed > 0x7E ) valid = false;
                    charset_name = "Raw ASCII";
                    break;

                case HEXDUMP_CHARSET_SHARPMZ_EU_ASCII: {
                    /* EU ASCII: reverzní jednobajtová konverze.
                     * Round-trip kontrola (audit H-22): cnv_to() vrací ' '
                     * (0x20) pro neznámé znaky. Pokud cnv_from(cnv_to(typed))
                     * nedá zpět typed, znak se v této znakové sadě nedá
                     * zakódovat a tichý zápis 0x20 by byl data corruption. */
                    value = sharpmz_cnv_to ( typed );
                    if ( sharpmz_cnv_from ( value ) != typed ) valid = false;
                    charset_name = "Sharp MZ EU ASCII";
                    break;
                }

                case HEXDUMP_CHARSET_SHARPMZ_JP_ASCII: {
                    /* JP ASCII: reverzní jednobajtová konverze + round-trip. */
                    value = sharpmz_jp_cnv_to ( typed );
                    if ( sharpmz_jp_cnv_from ( value ) != typed ) valid = false;
                    charset_name = "Sharp MZ JP ASCII";
                    break;
                }

                case HEXDUMP_CHARSET_SHARPMZ_EU_UTF8:
                case HEXDUMP_CHARSET_SHARPMZ_EU_CG1:
                case HEXDUMP_CHARSET_SHARPMZ_EU_CG2: {
                    /* EU UTF-8/CG: reverzní UTF-8 konverze + round-trip.
                     * Porovnáváme první bajt UTF-8 reprezentace vs. typed
                     * (pro ASCII typed by měla být 1:1). */
                    char str[2] = { (char) typed, '\0' };
                    value = sharpmz_eu_convert_UTF8_to ( str );
                    int conv, print;
                    const char *back = sharpmz_eu_convert_to_UTF8 ( value, &conv, &print );
                    if ( !conv || !back || (uint8_t) back[0] != typed ) valid = false;
                    charset_name = "Sharp MZ EU (UTF-8)";
                    break;
                }

                case HEXDUMP_CHARSET_SHARPMZ_JP_UTF8:
                case HEXDUMP_CHARSET_SHARPMZ_JP_CG1:
                case HEXDUMP_CHARSET_SHARPMZ_JP_CG2: {
                    /* JP UTF-8/CG: reverzní UTF-8 konverze + round-trip. */
                    char str[2] = { (char) typed, '\0' };
                    value = sharpmz_jp_convert_UTF8_to ( str );
                    int conv, print;
                    const char *back = sharpmz_jp_convert_to_UTF8 ( value, &conv, &print );
                    if ( !conv || !back || (uint8_t) back[0] != typed ) valid = false;
                    charset_name = "Sharp MZ JP (UTF-8)";
                    break;
                }

                default:
                    break;
            }

            if ( valid ) {
                /* Pokud je zapnutá inverze, uložit invertovanou hodnotu
                 * (data[] obsahují raw bajty, inverze se aplikuje jen při zobrazení) */
                hd->data[hd->cursor_pos] = hd->invert ? (uint8_t) ~value : value;
                hd->edit_dirty = true;
                hd->edit_convert_error = false;
                hd->edit_convert_error_msg[0] = '\0';
                if ( hd->cursor_pos < max_pos ) hd->cursor_pos++;
            } else {
                /* Znak nelze v aktuální znakové sadě reprezentovat.
                 * Nezapisujeme (bez tiché data corruption) a uživateli
                 * zobrazíme hlášku pod hexdumpem. Audit H-22. */
                hd->edit_convert_error = true;
                if ( typed >= 0x20 && typed <= 0x7E ) {
                    snprintf ( hd->edit_convert_error_msg, sizeof ( hd->edit_convert_error_msg ),
                               "Character '%c' cannot be encoded in %s", (char) typed, charset_name );
                } else {
                    snprintf ( hd->edit_convert_error_msg, sizeof ( hd->edit_convert_error_msg ),
                               "Byte 0x%02X cannot be encoded in %s", typed, charset_name );
                }
            }
        }

    } else {
        /* hex režim: číslice 0-9, A-F
         * Uživatel edituje ZOBRAZENÝ bajt (po případné inverzi).
         * Výsledek se musí uložit zpět invertovaný, pokud je inverze zapnutá. */
        for ( int i = 0; i < (int) ( sizeof ( hex_keys ) / sizeof ( hex_keys[0] ) ); i++ ) {
            if ( ImGui::IsKeyPressed ( hex_keys[i].key ) ) {
                int nibble = hex_keys[i].nibble;
                int pos = hd->cursor_pos;
                /* Pracujeme se zobrazenou hodnotou (po inverzi) */
                uint8_t b = hd->invert ? (uint8_t) ~hd->data[pos] : hd->data[pos];

                if ( hd->cursor_high_nibble ) {
                    b = (uint8_t) ( ( nibble << 4 ) | ( b & 0x0F ) );
                    hd->data[pos] = hd->invert ? (uint8_t) ~b : b;
                    hd->cursor_high_nibble = false;
                } else {
                    b = (uint8_t) ( ( b & 0xF0 ) | nibble );
                    hd->data[pos] = hd->invert ? (uint8_t) ~b : b;
                    hd->cursor_high_nibble = true;
                    if ( hd->cursor_pos < max_pos ) hd->cursor_pos++;
                }
                hd->edit_dirty = true;
                break; /* zpracovat jen jednu klávesu za frame */
            }
        }
    }
}


/**
 * @brief Vykreslí hexdump obsah dat přes DrawList s editačním kurzorem.
 *
 * Používá fixní pozice sloupců pro zaručené zarovnání.
 * 16 bajtů na řádek, formát: OOOO:  HH HH ... HH  |text sloupec|
 *
 * V editačním režimu:
 * - Zvýrazňuje změněné bajty (žlutě).
 * - Vykresluje kurzor (vyplněný obdélník v aktivním sloupci,
 *   obrys v neaktivním).
 * - Zpracovává klik myší pro pozicování kurzoru.
 *
 * @param hd Datový model s načtenými daty.
 */
static void render_hex_content ( st_PANEL_HEXDUMP_DATA *hd )
{
    if ( !hd->is_loaded ) {
        if ( hd->read_error ) {
            ImGui::TextColored ( ImVec4 ( 1, 0.3f, 0.3f, 1 ), "%s", _ ( "Read error!" ) );
        } else {
            ImGui::TextDisabled ( "%s", _ ( "No data." ) );
        }
        return;
    }

    st_HEX_LAYOUT lay;
    hex_layout_init ( &lay );

    uint16_t total_size = hd->data_size;
    int lines = ( total_size + COLS - 1 ) / COLS;

    ImDrawList *dl = ImGui::GetWindowDrawList ();

    ImU32 col_offset = IM_COL32 ( 100, 100, 140, 255 );      /* barva offsetu */
    ImU32 col_separator = IM_COL32 ( 60, 60, 80, 255 );       /* barva separátoru */
    ImU32 col_text = IM_COL32 ( 140, 180, 200, 255 );         /* barva textového sloupce */

    /* klik myší pro pozicování kurzoru */
    if ( hd->edit_mode && ImGui::IsMouseClicked ( ImGuiMouseButton_Left ) ) {
        ImVec2 mouse = ImGui::GetMousePos ();
        /* kontrola, že klik je uvnitř child okna */
        ImVec2 wmin = ImGui::GetWindowPos ();
        ImVec2 wmax = ImVec2 ( wmin.x + ImGui::GetWindowSize ().x,
                                wmin.y + ImGui::GetWindowSize ().y );
        if ( mouse.x >= wmin.x && mouse.x < wmax.x &&
             mouse.y >= wmin.y && mouse.y < wmax.y ) {
            bool in_ascii = false;
            int hit = hit_test ( &lay, mouse, (int) total_size, &in_ascii );
            if ( hit >= 0 ) {
                hd->cursor_pos = hit;
                hd->cursor_in_ascii = in_ascii;
                hd->cursor_high_nibble = true;
            }
        }
    }

    /* kurzor: vykreslení pozadí */
    if ( hd->edit_mode && hd->cursor_pos < (int) total_size ) {
        /* aktivní sloupec: vyplněný obdélník */
        if ( hd->cursor_in_ascii ) {
            ImVec2 p = ascii_cell_pos ( &lay, hd->cursor_pos );
            dl->AddRectFilled ( p, ImVec2 ( p.x + lay.char_w, p.y + lay.line_h ), COL_CURSOR_BG );
            /* obrys v hex sloupci */
            ImVec2 hp = hex_cell_pos ( &lay, hd->cursor_pos );
            dl->AddRect ( hp, ImVec2 ( hp.x + lay.char_w * 2, hp.y + lay.line_h ), COL_CURSOR_OUTLINE );
        } else {
            ImVec2 p = hex_cell_pos ( &lay, hd->cursor_pos );
            dl->AddRectFilled ( p, ImVec2 ( p.x + lay.char_w * 2, p.y + lay.line_h ), COL_CURSOR_BG );
            /* obrys v ASCII sloupci */
            ImVec2 ap = ascii_cell_pos ( &lay, hd->cursor_pos );
            dl->AddRect ( ap, ImVec2 ( ap.x + lay.char_w, ap.y + lay.line_h ), COL_CURSOR_OUTLINE );
        }
    }

    for ( int line = 0; line < lines; line++ ) {
        int off = line * COLS;
        float y = lay.origin.y + line * lay.line_h;

        /* offset (adresa) - GCC považuje off za neomezený int,
           proto 16 bajtů i když v praxi stačí méně */
        char addr[16];
        snprintf ( addr, sizeof ( addr ), "%04X:", off );
        dl->AddText ( ImVec2 ( lay.origin.x, y ), col_offset, addr );

        /* hex bajty */
        for ( int col = 0; col < COLS; col++ ) {
            int idx = off + col;
            float extra = ( col >= 8 ) ? lay.hex_group_gap : 0;
            float cx = lay.origin.x + lay.x_hex + col * lay.hex_pair_w + extra;

            if ( idx < total_size ) {
                uint8_t b = hd->data[idx];
                if ( hd->invert ) b = (uint8_t) ~b;

                /* barva: změněný bajt žlutě, jinak standardní */
                ImU32 color;
                if ( hd->edit_mode && hd->data[idx] != hd->undo_data[idx] ) {
                    color = COL_MODIFIED;
                } else {
                    color = byte_color ( b );
                }

                char hex[4];
                snprintf ( hex, sizeof ( hex ), "%02X", b );
                dl->AddText ( ImVec2 ( cx, y ), color, hex );
            }
        }

        /* separátor */
        float sep_x = lay.origin.x + lay.x_text - lay.space_w * 2;
        dl->AddText ( ImVec2 ( sep_x, y ), col_separator, "|" );

        /* textový sloupec */
        float tx = lay.origin.x + lay.x_text;
        for ( int col = 0; col < COLS; col++ ) {
            int idx = off + col;
            if ( idx >= total_size ) break;

            uint8_t b = hd->data[idx];
            if ( hd->invert ) b = (uint8_t) ~b;

            /* barva: změněný bajt žlutě */
            ImU32 color;
            if ( hd->edit_mode && hd->data[idx] != hd->undo_data[idx] ) {
                color = COL_MODIFIED;
            } else {
                color = col_text;
            }

            char cbuf[8];
            byte_to_display_char ( b, hd->charset, cbuf );

            /* CG režimy: centrovat glyf v buňce (ImGui ořezává padding z fontu) */
            if ( hd->charset >= HEXDUMP_CHARSET_SHARPMZ_EU_CG1 ) {
                ImVec2 tsz = ImGui::CalcTextSize ( cbuf );
                float cx = tx + ( lay.char_w - tsz.x ) * 0.5f;
                dl->AddText ( ImVec2 ( cx, y ), color, cbuf );
            } else {
                dl->AddText ( ImVec2 ( tx, y ), color, cbuf );
            }
            tx += lay.char_w;
        }

        /* ukončovací separátor */
        dl->AddText ( ImVec2 ( tx, y ), col_separator, "|" );
    }

    /* dummy pro správný scroll a layout */
    float total_w = lay.x_text + lay.char_w * COLS + lay.space_w * 4;
    ImGui::Dummy ( ImVec2 ( total_w, lines * lay.line_h ) );
}


extern "C" void panel_hexdump_render ( st_PANEL_HEXDUMP_DATA *hd,
                                        st_MZDSK_DISC *disc,
                                        const st_MZDSK_DETECT_RESULT *detect,
                                        st_PANEL_RAW_IO_DATA *raw_io_data,
                                        bool *is_dirty )
{
    if ( !hd || !disc ) {
        ImGui::TextDisabled ( "%s", _ ( "No disk data available." ) );
        return;
    }

    render_navigation ( hd, disc, detect );

    /* editor toolbar */
    render_edit_toolbar ( hd, disc, is_dirty );

    /* tlačítka Get/Put pro Raw I/O */
    if ( raw_io_data ) {
        ImGui::SameLine ( 0, 20 );
        if ( ImGui::Button ( _L ( "Get" ) ) ) {
            panel_raw_io_open_from_hexdump ( raw_io_data, hd, RAW_IO_ACTION_GET );
        }
        if ( ImGui::IsItemHovered () ) {
            ImGui::SetTooltip ( "%s", _ ( "Export sectors to file" ) );
        }
        ImGui::SameLine ();
        if ( ImGui::Button ( _L ( "Put" ) ) ) {
            panel_raw_io_open_from_hexdump ( raw_io_data, hd, RAW_IO_ACTION_PUT );
        }
        if ( ImGui::IsItemHovered () ) {
            ImGui::SetTooltip ( "%s", _ ( "Import sectors from file" ) );
        }
    }

    ImGui::Separator ();
    ImGui::Spacing ();

    /* potvrzovací dialog pro zahození změn */
    render_discard_popup ( hd );

    /* v editačním režimu zachytit klávesový vstup (před BeginChild, na úrovni hlavního okna) */
    if ( hd->edit_mode ) {
        handle_edit_input ( hd );
    }

    /* scrollovatelná oblast pro hexdump */
    ImGui::BeginChild ( "HexContent", ImVec2 ( 0, 0 ), ImGuiChildFlags_None,
                        ImGuiWindowFlags_HorizontalScrollbar );
    render_hex_content ( hd );
    ImGui::EndChild ();
}
