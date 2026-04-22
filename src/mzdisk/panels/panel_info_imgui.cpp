/**
 * @file panel_info_imgui.cpp
 * @brief ImGui rendering informačního panelu.
 *
 * Zobrazuje geometrii disku, identifikovaný formát, track rules
 * a FS-specifické statistiky ve formě tabulek a přehledů.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"

extern "C" {
#include "panels/panel_info.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzglyphs/mzglyphs.h"
#include "libs/mz_vcode/mz_vcode.h"
#include "disk_session.h"
#include "i18n.h"
}


/** @brief Popisky pro combo výběr kódování Sharp MZ ASCII jmen. */
static const char *s_name_charset_labels[] = {
    "SharpMZ-EU -> UTF-8",
    "SharpMZ-JP -> UTF-8",
    "SharpMZ-EU -> ASCII",
    "SharpMZ-JP -> ASCII",
    "SharpMZ-EU -> MZ-CG1",
    "SharpMZ-EU -> MZ-CG2",
    "SharpMZ-JP -> MZ-CG1",
    "SharpMZ-JP -> MZ-CG2",
};


/**
 * @brief Konvertuje raw Sharp MZ ASCII jméno do zobrazitelného řetězce.
 *
 * Podle zvoleného režimu konvertuje bajty z Sharp MZ ASCII
 * do ASCII, UTF-8 nebo CG-ROM glyfů (mzglyphs font).
 *
 * @param mz_src Zdrojové bajty v Sharp MZ ASCII.
 * @param mz_len Počet platných bajtů.
 * @param dst Výstupní buffer (null-terminated).
 * @param dst_size Velikost výstupního bufferu.
 * @param charset Režim konverze (en_MZDSK_NAME_CHARSET).
 */
static void convert_mzname_display ( const uint8_t *mz_src, int mz_len,
                                      char *dst, int dst_size, int charset )
{
    if ( mz_len == 0 || dst_size < 2 ) {
        dst[0] = '\0';
        return;
    }

    int pos = 0;

    switch ( charset ) {
        case MZDSK_NAME_CHARSET_EU_UTF8:
        case MZDSK_NAME_CHARSET_JP_UTF8: {
            sharpmz_charset_t cs = ( charset == MZDSK_NAME_CHARSET_EU_UTF8 )
                                   ? SHARPMZ_CHARSET_EU : SHARPMZ_CHARSET_JP;
            sharpmz_str_to_utf8 ( mz_src, (size_t) mz_len, dst, (size_t) dst_size, cs );
            return;
        }
        case MZDSK_NAME_CHARSET_EU_ASCII:
        case MZDSK_NAME_CHARSET_JP_ASCII:
            for ( int i = 0; i < mz_len && pos < dst_size - 1; i++ ) {
                dst[pos++] = (char) ( ( charset == MZDSK_NAME_CHARSET_JP_ASCII )
                                      ? sharpmz_jp_cnv_from ( mz_src[i] )
                                      : sharpmz_cnv_from ( mz_src[i] ) );
            }
            break;
        case MZDSK_NAME_CHARSET_EU_CG1:
        case MZDSK_NAME_CHARSET_EU_CG2:
        case MZDSK_NAME_CHARSET_JP_CG1:
        case MZDSK_NAME_CHARSET_JP_CG2: {
            en_MZ_VCODE_CHARSET vvar = ( charset <= MZDSK_NAME_CHARSET_EU_CG2 )
                                       ? MZ_VCODE_EU : MZ_VCODE_JP;
            en_MZGLYPHS_CHARSET gset;
            if ( charset == MZDSK_NAME_CHARSET_EU_CG1 )      gset = MZGLYPHS_EU1;
            else if ( charset == MZDSK_NAME_CHARSET_EU_CG2 ) gset = MZGLYPHS_EU2;
            else if ( charset == MZDSK_NAME_CHARSET_JP_CG1 ) gset = MZGLYPHS_JP1;
            else                                              gset = MZGLYPHS_JP2;

            for ( int i = 0; i < mz_len && pos < dst_size - 5; i++ ) {
                uint8_t vcode = mz_vcode_from_ascii_dump ( mz_src[i], vvar );
                char gbuf[8];
                mzglyphs_to_utf8_buf ( vcode, gset, gbuf );
                int glen = (int) strlen ( gbuf );
                if ( pos + glen < dst_size - 1 ) {
                    memcpy ( dst + pos, gbuf, glen );
                    pos += glen;
                }
            }
            break;
        }
        default:
            break;
    }
    dst[pos] = '\0';
}


/**
 * @brief Vykreslí CG-ROM glyfy na aktuální pozici kurzoru.
 *
 * Renderuje UTF-8 řetězec s PUA glyfy z mzglyphs.ttf po jednom
 * znaku přes draw list, s centrováním v buňce fixní šířky.
 *
 * @param text Konvertovaný UTF-8 řetězec s PUA glyfy.
 */
static void render_cg_text ( const char *text )
{
    ImDrawList *dl = ImGui::GetWindowDrawList ();
    ImVec2 pos = ImGui::GetCursorScreenPos ();
    ImU32 color = ImGui::GetColorU32 ( ImGuiCol_Text );
    float char_w = ImGui::CalcTextSize ( "W" ).x;
    float line_h = ImGui::GetTextLineHeight ();
    float x = pos.x;

    const char *p = text;
    while ( *p ) {
        int clen = 1;
        if ( ( *p & 0xF0 ) == 0xF0 ) clen = 4;
        else if ( ( *p & 0xE0 ) == 0xE0 ) clen = 3;
        else if ( ( *p & 0xC0 ) == 0xC0 ) clen = 2;

        char buf[8];
        memcpy ( buf, p, clen );
        buf[clen] = '\0';

        ImVec2 tsz = ImGui::CalcTextSize ( buf );
        float cx = x + ( char_w - tsz.x ) * 0.5f;
        dl->AddText ( ImVec2 ( cx, pos.y ), color, buf );
        x += char_w;

        p += clen;
    }

    ImGui::Dummy ( ImVec2 ( x - pos.x, line_h ) );
}


/**
 * @brief Vykreslí sekci geometrie disku.
 *
 * Zobrazí celkový počet stop, strany, celkovou velikost a tabulku track rules.
 *
 * @param data Naplněný datový model informačního panelu.
 */
static void render_geometry ( const st_PANEL_INFO_DATA *data )
{
    if ( !ImGui::CollapsingHeader ( _ ( "Disk Geometry" ), ImGuiTreeNodeFlags_DefaultOpen ) )
        return;

    ImGui::Indent ();

    ImGui::Text ( "%s: %d", _ ( "Total tracks" ), data->total_tracks );
    ImGui::Text ( "%s: %d", _ ( "Sides" ), data->sides );
    ImGui::Text ( "%s: %u %s", _ ( "Total size" ),
                  data->total_size_bytes / 1024, _ ( "KB" ) );

    ImGui::Spacing ();

    /* tabulka track rules */
    if ( data->rule_count > 0 &&
         ImGui::BeginTable ( "track_rules", 5,
                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                             | ImGuiTableFlags_SizingStretchProp ) ) {

        ImGui::TableSetupColumn ( _ ( "Tracks" ) );
        ImGui::TableSetupColumn ( _ ( "Count" ) );
        ImGui::TableSetupColumn ( _ ( "Sectors" ) );
        ImGui::TableSetupColumn ( _ ( "Sector size" ) );
        ImGui::TableSetupColumn ( _ ( "Data" ) );
        ImGui::TableHeadersRow ();

        for ( int i = 0; i < data->rule_count; i++ ) {
            const st_PANEL_INFO_TRACK_RULE *r = &data->rules[i];
            ImGui::TableNextRow ();

            ImGui::TableNextColumn ();
            if ( r->count_tracks == 1 ) {
                ImGui::Text ( "%d", r->from_track );
            } else {
                ImGui::Text ( "%d-%d", r->from_track,
                              r->from_track + r->count_tracks - 1 );
            }

            ImGui::TableNextColumn ();
            ImGui::Text ( "%d", r->count_tracks );

            ImGui::TableNextColumn ();
            ImGui::Text ( "%d", r->sectors );

            ImGui::TableNextColumn ();
            ImGui::Text ( "%d B", r->sector_size );

            ImGui::TableNextColumn ();
            if ( r->is_inverted ) {
                ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s",
                                     _ ( "inverted" ) );
            } else {
                ImGui::TextDisabled ( "%s", _ ( "normal" ) );
            }
        }

        ImGui::EndTable ();
    }

    ImGui::Unindent ();
}


/**
 * @brief Vykreslí sekci identifikace formátu.
 *
 * Zobrazí identifikovaný DSK formát a detekovaný filesystem.
 *
 * @param data Naplněný datový model.
 */
static void render_identification ( const st_PANEL_INFO_DATA *data )
{
    if ( !ImGui::CollapsingHeader ( _ ( "Format Identification" ), ImGuiTreeNodeFlags_DefaultOpen ) )
        return;

    ImGui::Indent ();

    ImGui::Text ( "%s: %s", _ ( "DSK format" ),
                  panel_info_format_str ( data->dsk_format ) );
    ImGui::Text ( "%s: %s", _ ( "Filesystem" ),
                  mzdisk_session_fs_type_str ( data->fs_type ) );

    ImGui::Unindent ();
}


/**
 * @brief Vykreslí FSMZ specifické informace.
 *
 * Blokové statistiky a bootstrap info.
 *
 * @param data Naplněný datový model.
 */
static void render_fsmz_info ( const st_PANEL_INFO_DATA *data, st_MZDISK_CONFIG *cfg )
{
    if ( !data->has_fsmz_info && !data->has_boot_info ) return;

    if ( !ImGui::CollapsingHeader ( _ ( "FSMZ Details" ), ImGuiTreeNodeFlags_DefaultOpen ) )
        return;

    ImGui::Indent ();

    if ( data->has_fsmz_info ) {
        ImGui::Text ( "%s: %d", _ ( "Total blocks" ), data->fsmz_total_blocks );

        /* progress bar pro obsazenost */
        float usage = ( data->fsmz_total_blocks > 0 )
                      ? (float) data->fsmz_used_blocks / (float) data->fsmz_total_blocks
                      : 0.0f;
        char overlay[64];
        snprintf ( overlay, sizeof ( overlay ), "%d / %d (%d%%)",
                   data->fsmz_used_blocks, data->fsmz_total_blocks,
                   (int) ( usage * 100.0f ) );
        ImGui::Text ( "%s:", _ ( "Usage" ) );
        ImGui::SameLine ();
        ImGui::ProgressBar ( usage, ImVec2 ( -1, 0 ), overlay );

        ImGui::Text ( "%s: %d", _ ( "Free blocks" ), data->fsmz_free_blocks );
    }

    if ( data->has_boot_info ) {
        ImGui::Spacing ();

        /* roletka pro kódování bootstrap jména - na samostatném řádku */
        ImGui::Text ( "%s:", _ ( "Encoding" ) );
        ImGui::SameLine ();
        ImGui::SetNextItemWidth ( 250 );
        if ( ImGui::Combo ( "##info_boot_charset", &cfg->boot_name_charset,
                            s_name_charset_labels, IM_ARRAYSIZE ( s_name_charset_labels ) ) ) {
            mzdisk_config_save ( cfg );
        }

        char display_name[64];
        convert_mzname_display ( data->mz_boot_name, data->mz_boot_name_len,
                                 display_name, (int) sizeof ( display_name ),
                                 cfg->boot_name_charset );
        if ( cfg->boot_name_charset >= MZDSK_NAME_CHARSET_EU_CG1 ) {
            char prefix[32];
            snprintf ( prefix, sizeof ( prefix ), "%s: ", _ ( "Bootstrap" ) );
            ImGui::Text ( "%s", prefix );
            ImGui::SameLine ( 0, 0 );
            render_cg_text ( display_name );
        } else {
            ImGui::Text ( "%s: %s", _ ( "Bootstrap" ), display_name );
        }
        ImGui::Text ( "  %s: %s", _ ( "Type" ), data->boot_type );
        ImGui::Text ( "  %s: %d (%d B)", _ ( "Size" ),
                      data->boot_blocks, data->boot_size_bytes );
        ImGui::Text ( "  %s: %d", _ ( "Start block" ), data->boot_start_block );
    }

    ImGui::Unindent ();
}


/**
 * @brief Vykreslí CP/M specifické informace.
 *
 * DPB parametry a preset.
 *
 * @param data Naplněný datový model.
 */
static void render_cpm_info ( const st_PANEL_INFO_DATA *data )
{
    if ( !data->has_cpm_info ) return;

    if ( !ImGui::CollapsingHeader ( _ ( "CP/M Details" ), ImGuiTreeNodeFlags_DefaultOpen ) )
        return;

    ImGui::Indent ();

    ImGui::Text ( "%s: %s", _ ( "Preset" ), data->cpm_preset_name );
    ImGui::Text ( "%s: %d B", _ ( "Block size" ), data->cpm_block_size );
    ImGui::Text ( "%s: %d", _ ( "Total blocks" ), data->cpm_total_blocks );
    ImGui::Text ( "%s: %d", _ ( "Directory entries" ), data->cpm_dir_entries );
    ImGui::Text ( "%s: %d", _ ( "Reserved tracks" ), data->cpm_reserved_tracks );

    ImGui::Unindent ();
}


/**
 * @brief Vykreslí informace o FSMZ boot oblasti na non-FSMZ discích.
 *
 * Zobrazuje počet FSMZ stop/bloků a bootstrap info pokud existuje.
 *
 * @param data Naplněný datový model.
 */
static void render_fsmz_boot_area ( const st_PANEL_INFO_DATA *data, st_MZDISK_CONFIG *cfg )
{
    if ( !data->has_fsmz_boot_area ) return;

    if ( !ImGui::CollapsingHeader ( _ ( "FSMZ Boot Area" ), ImGuiTreeNodeFlags_DefaultOpen ) )
        return;

    ImGui::Indent ();

    ImGui::Text ( "%s: %d", _ ( "FSMZ tracks" ), data->fsmz_boot_tracks );
    ImGui::Text ( "%s: %d (%d B)", _ ( "FSMZ blocks" ),
                  data->fsmz_boot_blocks, data->fsmz_boot_blocks * 256 );

    if ( data->has_boot_info ) {
        ImGui::Spacing ();

        /* roletka pro kódování bootstrap jména - na samostatném řádku */
        ImGui::Text ( "%s:", _ ( "Encoding" ) );
        ImGui::SameLine ();
        ImGui::SetNextItemWidth ( 250 );
        if ( ImGui::Combo ( "##bootarea_charset", &cfg->boot_name_charset,
                            s_name_charset_labels, IM_ARRAYSIZE ( s_name_charset_labels ) ) ) {
            mzdisk_config_save ( cfg );
        }

        char display_name[64];
        convert_mzname_display ( data->mz_boot_name, data->mz_boot_name_len,
                                 display_name, (int) sizeof ( display_name ),
                                 cfg->boot_name_charset );
        if ( cfg->boot_name_charset >= MZDSK_NAME_CHARSET_EU_CG1 ) {
            char prefix[32];
            snprintf ( prefix, sizeof ( prefix ), "%s: ", _ ( "Bootstrap" ) );
            ImGui::Text ( "%s", prefix );
            ImGui::SameLine ( 0, 0 );
            render_cg_text ( display_name );
        } else {
            ImGui::Text ( "%s: %s", _ ( "Bootstrap" ), display_name );
        }
        ImGui::Text ( "  %s: %s", _ ( "Type" ), data->boot_type );
        ImGui::Text ( "  %s: %d (%d B)", _ ( "Size" ),
                      data->boot_blocks, data->boot_size_bytes );
        ImGui::Text ( "  %s: %d", _ ( "Start block" ), data->boot_start_block );
    }

    ImGui::Unindent ();
}


/**
 * @brief Vykreslí kompletní informační panel pro danou session.
 *
 * Volá se z hlavní renderovací smyčky pro aktivní session.
 *
 * @param data Naplněný datový model (z panel_info_load).
 * @param cfg Konfigurace aplikace (boot_name_charset pro kódování bootstrap jména).
 */
extern "C" void panel_info_render ( const st_PANEL_INFO_DATA *data, st_MZDISK_CONFIG *cfg )
{
    if ( !data || !data->is_loaded ) {
        ImGui::TextDisabled ( "%s", _ ( "No disk information available." ) );
        return;
    }

    /* Scroll wrapper - při přetečení obsahu se objeví svislý scrollbar, */
    /* který uživateli signalizuje skrytý obsah. */
    ImGui::BeginChild ( "##info_scroll", ImVec2 ( 0, 0 ), ImGuiChildFlags_None );

    render_geometry ( data );
    ImGui::Spacing ();

    render_identification ( data );
    ImGui::Spacing ();

    render_fsmz_info ( data, cfg );
    render_fsmz_boot_area ( data, cfg );
    render_cpm_info ( data );

    ImGui::EndChild ();
}
