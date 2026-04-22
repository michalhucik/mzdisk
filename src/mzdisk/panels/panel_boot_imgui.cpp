/**
 * @file panel_boot_imgui.cpp
 * @brief ImGui rendering boot sector vieweru a bootstrap managementu.
 *
 * Vykresluje detailní informace z IPLPRO hlavičky (alokační blok 0),
 * DINFO bloku (alokační blok 15), systémových stop (CP/M)
 * a sekci pro správu bootstrapu (Put/Get/Clear).
 *
 * Bootstrap management:
 * - Get: export bootstrapu do MZF (pokud existuje platný IPLPRO)
 * - Clear: smazání bootstrapu (s potvrzovacím dialogem)
 * - Put Bottom: import MZF jako bottom bootstrap (bloky 1-14/15)
 * - Put Normal: import MZF jako FSMZ bootstrap do FAREA (jen FSMZ)
 * - Put Over: import MZF nad FAREA (jen FSMZ, experimentální)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "panels/panel_boot.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzglyphs/mzglyphs.h"
#include "libs/mz_vcode/mz_vcode.h"
#include "i18n.h"
}

#include "ui_helpers.h"


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
 * CG režimy: MZ ASCII -> video kód (mz_vcode) -> mzglyphs UTF-8 glyf.
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


/** @brief Klíč pro Get (export bootstrap) file dialog. */
static const char *BOOT_GET_DIALOG_KEY = "BootGetDlg";

/** @brief Klíč pro Put Bottom file dialog. */
static const char *BOOT_PUT_BOTTOM_DIALOG_KEY = "BootPutBottomDlg";

/** @brief Klíč pro Put Normal file dialog. */
static const char *BOOT_PUT_NORMAL_DIALOG_KEY = "BootPutNormalDlg";

/** @brief Klíč pro Put Over file dialog. */
static const char *BOOT_PUT_OVER_DIALOG_KEY = "BootPutOverDlg";


/**
 * @brief Pomocná funkce pro vykreslení řádku tabulky s popisem a hodnotou.
 *
 * @param label Popis pole (levý sloupec).
 * @param fmt   Printf formátovací řetězec.
 * @param ...   Argumenty pro formátování.
 */
#define TABLE_ROW( label, fmt, ... ) do { \
    ImGui::TableNextRow (); \
    ImGui::TableSetColumnIndex ( 0 ); \
    ImGui::TextUnformatted ( label ); \
    ImGui::TableSetColumnIndex ( 1 ); \
    ImGui::Text ( fmt, __VA_ARGS__ ); \
} while (0)


/**
 * @brief Vykreslí sekci IPLPRO hlavičky.
 *
 * @param data Naplněný datový model.
 */
static void render_iplpro_section ( const st_PANEL_BOOT_DATA *data, st_MZDISK_CONFIG *cfg )
{
    if ( !data->has_iplpro ) {
        ImGui::TextDisabled ( "%s", _ ( "IPLPRO header not readable." ) );
        return;
    }

    /* validační status */
    if ( data->iplpro_valid ) {
        ImGui::TextColored ( ImVec4 ( 0.4f, 0.9f, 0.4f, 1.0f ), "%s", _ ( "Valid IPLPRO header" ) );
    } else {
        ImGui::TextColored ( ImVec4 ( 0.9f, 0.4f, 0.4f, 1.0f ), "%s", _ ( "Invalid IPLPRO header" ) );
    }

    ImGui::Spacing ();

    if ( ImGui::BeginTable ( "##iplpro_table", 2, ImGuiTableFlags_None ) ) {
        ImGui::TableSetupColumn ( "field", ImGuiTableColumnFlags_WidthFixed, 200.0f );
        ImGui::TableSetupColumn ( "value", ImGuiTableColumnFlags_WidthStretch );

        /* jméno konvertované dle zvoleného kódování */
        {
            char display_name[64];
            convert_mzname_display ( data->mz_name, data->mz_name_len,
                                     display_name, (int) sizeof ( display_name ),
                                     cfg->boot_name_charset );
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex ( 0 );
            ImGui::TextUnformatted ( _ ( "Name" ) );
            ImGui::TableSetColumnIndex ( 1 );
            if ( cfg->boot_name_charset >= MZDSK_NAME_CHARSET_EU_CG1 ) {
                render_cg_text ( display_name );
            } else {
                ImGui::Text ( "%s", display_name );
            }
        }

        /* roletka pro volbu kódování bootstrap jména */
        ImGui::TableNextRow ();
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::TextUnformatted ( _ ( "Name encoding" ) );
        ImGui::TableSetColumnIndex ( 1 );
        ImGui::SetNextItemWidth ( 250 );
        if ( ImGui::Combo ( "##boot_name_charset", &cfg->boot_name_charset,
                            s_name_charset_labels, IM_ARRAYSIZE ( s_name_charset_labels ) ) ) {
            mzdisk_config_save ( cfg );
        }

        TABLE_ROW ( _ ( "File type" ), "0x%02X", data->ftype );
        TABLE_ROW ( _ ( "Size" ), "0x%04X (%d B)", data->fsize, data->fsize );
        TABLE_ROW ( _ ( "Start address" ), "0x%04X", data->fstrt );
        TABLE_ROW ( _ ( "Exec address" ), "0x%04X", data->fexec );
        TABLE_ROW ( _ ( "Start block" ), "%d", data->block );
        TABLE_ROW ( _ ( "Block count" ), "%d", data->block_count );

        if ( data->block_count > 0 ) {
            TABLE_ROW ( _ ( "Block range" ), "%d - %d", data->block, data->block_end );
        }

        TABLE_ROW ( _ ( "Bootstrap type" ), "%s", data->boot_type );

        ImGui::EndTable ();
    }

}


/**
 * @brief Vykreslí sekci DINFO bloku.
 *
 * @param data Naplněný datový model.
 */
static void render_dinfo_section ( const st_PANEL_BOOT_DATA *data )
{
    if ( !data->has_dinfo ) {
        ImGui::TextDisabled ( "%s", _ ( "DINFO block not readable." ) );
        return;
    }

    if ( ImGui::BeginTable ( "##dinfo_table", 2, ImGuiTableFlags_None ) ) {
        ImGui::TableSetupColumn ( "field", ImGuiTableColumnFlags_WidthFixed, 200.0f );
        ImGui::TableSetupColumn ( "value", ImGuiTableColumnFlags_WidthStretch );

        TABLE_ROW ( _ ( "Volume" ), "%s (%d)",
                    data->volume_number == 0 ? _ ( "Master (bootable)" ) : _ ( "Slave" ),
                    data->volume_number );
        TABLE_ROW ( _ ( "File area start" ), "%s %d (0x%02X)", _ ( "block" ), data->farea, data->farea );
        TABLE_ROW ( _ ( "Used blocks" ), "%d", data->dinfo_used );
        TABLE_ROW ( _ ( "Total blocks" ), "%d", data->dinfo_blocks + 1 );

        ImGui::EndTable ();
    }
}


/**
 * @brief Vykreslí sekci systémových stop (CP/M).
 *
 * @param data Naplněný datový model.
 */
static void render_system_tracks_section ( const st_PANEL_BOOT_DATA *data )
{
    if ( ImGui::BeginTable ( "##systracks_table", 2, ImGuiTableFlags_None ) ) {
        ImGui::TableSetupColumn ( "field", ImGuiTableColumnFlags_WidthFixed, 200.0f );
        ImGui::TableSetupColumn ( "value", ImGuiTableColumnFlags_WidthStretch );

        TABLE_ROW ( _ ( "Reserved tracks" ), "%d (OFF)", data->system_tracks_off );
        TABLE_ROW ( _ ( "System tracks" ), "%d", data->system_tracks_count );
        TABLE_ROW ( _ ( "Abs. track range" ), "%s", data->system_tracks_range );
        TABLE_ROW ( _ ( "Total size" ), "%u B (%.1f KB)",
                    data->system_tracks_size,
                    (float) data->system_tracks_size / 1024.0f );
        TABLE_ROW ( _ ( "Content" ), "%s", "CCP + BDOS + BIOS" );

        ImGui::EndTable ();
    }
}


/**
 * @brief Vykreslí sekci bootstrap management s tlačítky a konfigurací.
 *
 * @param data Datový model.
 * @param disc Otevřený disk.
 * @param detect Výsledek auto-detekce.
 * @param is_dirty Dirty flag session.
 * @param cfg Konfigurace (cesty k adresářům).
 */
static void render_bootstrap_management ( st_PANEL_BOOT_DATA *data,
                                           st_MZDSK_DISC *disc,
                                           st_MZDSK_DETECT_RESULT *detect,
                                           bool *is_dirty,
                                           st_MZDISK_CONFIG *cfg )
{
    bool has_valid_iplpro = data->has_iplpro && data->iplpro_valid;

    /* --- toolbar: Get, Clear --- */
    if ( !has_valid_iplpro ) ImGui::BeginDisabled ();

    if ( ImGui::Button ( _ ( "Get (Export MZF)" ) ) ) {
        IGFD::FileDialogConfig fdcfg;
        fdcfg.path = cfg->last_get_dir;
        fdcfg.countSelectionMax = 1;
        fdcfg.flags = ImGuiFileDialogFlags_Modal
                    | ImGuiFileDialogFlags_ConfirmOverwrite
                    | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;

        /* navrhnout jméno z IPLPRO */
        static char suggested_name[64];
        snprintf ( suggested_name, sizeof ( suggested_name ), "%s.mzf", data->name );
        fdcfg.fileName = suggested_name;

        ImGuiFileDialog::Instance ()->OpenDialog (
            BOOT_GET_DIALOG_KEY, _ ( "Export Bootstrap to MZF" ), ".mzf", fdcfg
        );
    }

    ImGui::SameLine ();

    /* Rename - analogie FSMZ Directory Rename (jen fname v IPLPRO) */
    if ( ImGui::Button ( _ ( "Rename" ) ) ) {
        /* předvyplnit aktuální ASCII fallback jméno */
        strncpy ( data->rename_buf, data->name, sizeof ( data->rename_buf ) - 1 );
        data->rename_buf[sizeof ( data->rename_buf ) - 1] = '\0';
        data->show_rename = true;
    }

    ImGui::SameLine ();

    /* Set - editace STRT/EXEC/ftype v IPLPRO (analogie FSMZ Set) */
    if ( ImGui::Button ( _ ( "Set" ) ) ) {
        data->set_fstrt = (int) data->fstrt;
        data->set_fexec = (int) data->fexec;
        data->set_ftype = (int) data->ftype;
        data->show_set = true;
    }

    ImGui::SameLine ();

    if ( ImGui::Button ( _ ( "Clear" ) ) ) {
        data->show_clear_confirm = true;
    }

    if ( !has_valid_iplpro ) ImGui::EndDisabled ();

    /* --- toolbar: Put Bottom, Put Normal, Put Over --- */
    ImGui::SameLine ( 0, 30 );
    ImGui::Text ( "|" );
    ImGui::SameLine ( 0, 30 );

    /* Put Bottom - vždy dostupné pokud IPLPRO je prázdný */
    if ( has_valid_iplpro ) ImGui::BeginDisabled ();

    if ( ImGui::Button ( _ ( "Put Bottom" ) ) ) {
        IGFD::FileDialogConfig fdcfg;
        fdcfg.path = cfg->last_put_dir;
        fdcfg.countSelectionMax = 1;
        fdcfg.flags = ImGuiFileDialogFlags_Modal
                    | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;

        ImGuiFileDialog::Instance ()->OpenDialog (
            BOOT_PUT_BOTTOM_DIALOG_KEY, _ ( "Import MZF as Bottom Bootstrap" ), ".mzf", fdcfg
        );
    }

    /* Put Normal a Put Over - jen pro plný FSMZ */
    if ( data->is_full_fsmz ) {
        ImGui::SameLine ();
        if ( ImGui::Button ( _ ( "Put Normal" ) ) ) {
            IGFD::FileDialogConfig fdcfg;
            fdcfg.path = cfg->last_put_dir;
            fdcfg.countSelectionMax = 1;
            fdcfg.flags = ImGuiFileDialogFlags_Modal
                        | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;

            ImGuiFileDialog::Instance ()->OpenDialog (
                BOOT_PUT_NORMAL_DIALOG_KEY, _ ( "Import MZF as Normal Bootstrap" ), ".mzf", fdcfg
            );
        }

        ImGui::SameLine ();
        if ( ImGui::Button ( _ ( "Put Over" ) ) ) {
            IGFD::FileDialogConfig fdcfg;
            fdcfg.path = cfg->last_put_dir;
            fdcfg.countSelectionMax = 1;
            fdcfg.flags = ImGuiFileDialogFlags_Modal
                        | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;

            ImGuiFileDialog::Instance ()->OpenDialog (
                BOOT_PUT_OVER_DIALOG_KEY, _ ( "Import MZF as Over-FAREA Bootstrap" ), ".mzf", fdcfg
            );
        }
    }

    if ( has_valid_iplpro ) ImGui::EndDisabled ();

    /* --- konfigurace bottom bootstrapu --- */
    if ( data->is_full_fsmz ) {
        ImGui::Spacing ();
        bool prev = data->preserve_fsmz;
        ImGui::Checkbox ( _ ( "Preserve FSMZ compatibility (Mini mode)" ), &data->preserve_fsmz );
        if ( data->preserve_fsmz != prev ) {
            /* přepočítat max bloků */
            if ( data->preserve_fsmz ) {
                data->max_bottom_blocks = FSMZ_SECTORS_ON_TRACK - 2; /* 14 */
            } else {
                uint16_t total = 0;
                if ( disc->tracks_rules ) {
                    total = disc->tracks_rules->total_tracks * FSMZ_SECTORS_ON_TRACK;
                }
                data->max_bottom_blocks = ( total > 1 ) ? ( total - 1 ) : 0;
            }
        }

        if ( !data->preserve_fsmz ) {
            ImGui::SameLine ();
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ),
                                 "(%s)", _ ( "Disk will no longer be FSMZ compatible!" ) );
        }
    }

    ImGui::TextDisabled ( "%s: %d (%d B)",
                          _ ( "Max bottom blocks" ),
                          data->max_bottom_blocks,
                          data->max_bottom_blocks * FSMZ_SECTOR_SIZE );

    /* --- zpracování file dialogů --- */
    SetNextWindowDefaultCentered ( 1200, 768 );
    ImGui::PushStyleVar ( ImGuiStyleVar_WindowBorderSize, 2.0f );
    ImGui::PushStyleColor ( ImGuiCol_Border, ImVec4 ( 0.40f, 0.50f, 0.80f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBg, ImVec4 ( 0.10f, 0.14f, 0.30f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBgActive, ImVec4 ( 0.14f, 0.20f, 0.42f, 1.0f ) );

    ImGuiWindowFlags dlg_flags = ImGuiWindowFlags_NoCollapse;

    /* Get dialog */
    if ( ImGuiFileDialog::Instance ()->Display ( BOOT_GET_DIALOG_KEY, dlg_flags ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
            std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();

            en_MZDSK_RES err = panel_boot_get_bootstrap ( disc, path.c_str () );
            if ( err != MZDSK_RES_OK ) {
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "%s", _ ( "Failed to export bootstrap." ) );
                data->show_error = true;
            }

            strncpy ( cfg->last_get_dir, dirpath.c_str (), sizeof ( cfg->last_get_dir ) - 1 );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* Put Bottom dialog */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( BOOT_PUT_BOTTOM_DIALOG_KEY, dlg_flags ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
            std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();

            en_MZDSK_RES err = panel_boot_put_bottom ( data, disc, path.c_str () );
            if ( err == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_boot_load ( data, disc, detect );
            }

            strncpy ( cfg->last_put_dir, dirpath.c_str (), sizeof ( cfg->last_put_dir ) - 1 );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* Put Normal dialog */
    if ( data->is_full_fsmz ) {
        SetNextWindowDefaultCentered ( 1200, 768 );
        if ( ImGuiFileDialog::Instance ()->Display ( BOOT_PUT_NORMAL_DIALOG_KEY, dlg_flags ) ) {
            if ( ImGuiFileDialog::Instance ()->IsOk () ) {
                std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
                std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();

                en_MZDSK_RES err = panel_boot_put_normal ( disc, path.c_str () );
                if ( err == MZDSK_RES_OK ) {
                    *is_dirty = true;
                    panel_boot_load ( data, disc, detect );
                } else {
                    snprintf ( data->error_msg, sizeof ( data->error_msg ),
                               "%s", _ ( "Failed to install normal bootstrap." ) );
                    data->show_error = true;
                }

                strncpy ( cfg->last_put_dir, dirpath.c_str (), sizeof ( cfg->last_put_dir ) - 1 );
            }
            ImGuiFileDialog::Instance ()->Close ();
        }

        /* Put Over dialog */
        SetNextWindowDefaultCentered ( 1200, 768 );
        if ( ImGuiFileDialog::Instance ()->Display ( BOOT_PUT_OVER_DIALOG_KEY, dlg_flags ) ) {
            if ( ImGuiFileDialog::Instance ()->IsOk () ) {
                std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
                std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();

                en_MZDSK_RES err = panel_boot_put_over ( disc, path.c_str () );
                if ( err == MZDSK_RES_OK ) {
                    *is_dirty = true;
                    panel_boot_load ( data, disc, detect );
                } else {
                    snprintf ( data->error_msg, sizeof ( data->error_msg ),
                               "%s", _ ( "Failed to install over-FAREA bootstrap." ) );
                    data->show_error = true;
                }

                strncpy ( cfg->last_put_dir, dirpath.c_str (), sizeof ( cfg->last_put_dir ) - 1 );
            }
            ImGuiFileDialog::Instance ()->Close ();
        }
    }

    ImGui::PopStyleColor ( 3 );
    ImGui::PopStyleVar ();

    /* --- Clear potvrzovací dialog --- */
    if ( data->show_clear_confirm ) {
        ImGui::OpenPopup ( "##clear_boot_confirm" );
        data->show_clear_confirm = false;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( ImGui::BeginPopupModal ( "##clear_boot_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Clear bootstrap from disk?" ) );
        if ( data->is_full_fsmz ) {
            ImGui::TextDisabled ( "%s", _ ( "FAREA blocks will be deallocated, disk set to Slave." ) );
        }
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Clear" ) ) ) {
            en_MZDSK_RES err = panel_boot_clear ( disc );
            if ( err == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_boot_load ( data, disc, detect );
            } else {
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "%s", _ ( "Failed to clear bootstrap." ) );
                data->show_error = true;
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* --- Rename popup --- */
    if ( data->show_rename ) {
        ImGui::OpenPopup ( "##boot_rename" );
        data->show_rename = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##boot_rename", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        /* zobrazit aktuální jméno podle zvoleného encoding (stejně jako
           v detail panelu) */
        {
            char cur_name[64];
            convert_mzname_display ( data->mz_name, data->mz_name_len,
                                     cur_name, (int) sizeof ( cur_name ),
                                     cfg->boot_name_charset );
            ImGui::Text ( "%s:", _ ( "Current name" ) );
            ImGui::SameLine ();
            if ( cfg->boot_name_charset >= MZDSK_NAME_CHARSET_EU_CG1 ) {
                render_cg_text ( cur_name );
            } else {
                ImGui::TextUnformatted ( cur_name );
            }
        }
        ImGui::Spacing ();
        ImGui::Text ( "%s:", _ ( "New name" ) );
        ImGui::SameLine ();
        ImGui::SetNextItemWidth ( 300 );

        if ( ImGui::IsWindowAppearing () ) ImGui::SetKeyboardFocusHere ();

        bool enter = ImGui::InputText ( "##boot_rename_input", data->rename_buf,
                                         sizeof ( data->rename_buf ),
                                         ImGuiInputTextFlags_EnterReturnsTrue );

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        bool do_rename = enter || ButtonMinWidth ( "OK" );
        if ( do_rename && data->rename_buf[0] != '\0' ) {
            en_MZDSK_RES r = panel_boot_rename ( disc, data->rename_buf );
            if ( r == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_boot_load ( data, disc, detect );
            } else {
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "%s: %s", data->rename_buf, mzdsk_get_error ( r ) );
                data->show_error = true;
            }
            ImGui::CloseCurrentPopup ();
        }
        if ( !do_rename ) {
            ImGui::SameLine ();
            if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
                ImGui::CloseCurrentPopup ();
            }
        }
        ImGui::EndPopup ();
    }

    /* --- Set popup (STRT/EXEC/ftype editor) --- */
    if ( data->show_set ) {
        ImGui::OpenPopup ( "##boot_set" );
        data->show_set = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##boot_set", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        {
            char cur_name[64];
            convert_mzname_display ( data->mz_name, data->mz_name_len,
                                     cur_name, (int) sizeof ( cur_name ),
                                     cfg->boot_name_charset );
            ImGui::Text ( "%s:", _ ( "Bootstrap" ) );
            ImGui::SameLine ();
            if ( cfg->boot_name_charset >= MZDSK_NAME_CHARSET_EU_CG1 ) {
                render_cg_text ( cur_name );
            } else {
                ImGui::TextUnformatted ( cur_name );
            }
        }
        ImGui::Spacing ();

        ImGui::Text ( "STRT:" );
        ImGui::SameLine ( 80 );
        ImGui::SetNextItemWidth ( 80 );
        {
            char hex[5];
            snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) data->set_fstrt & 0xFFFF );
            if ( ImGui::InputText ( "##boot_set_strt", hex, sizeof ( hex ),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                unsigned v = 0;
                if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) data->set_fstrt = (int) v;
            }
        }
        ImGui::SameLine ();
        ImGui::TextDisabled ( "(0x0000-0xFFFF)" );

        ImGui::Text ( "EXEC:" );
        ImGui::SameLine ( 80 );
        ImGui::SetNextItemWidth ( 80 );
        {
            char hex[5];
            snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) data->set_fexec & 0xFFFF );
            if ( ImGui::InputText ( "##boot_set_exec", hex, sizeof ( hex ),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                unsigned v = 0;
                if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) data->set_fexec = (int) v;
            }
        }
        ImGui::SameLine ();
        ImGui::TextDisabled ( "(0x0000-0xFFFF)" );

        ImGui::Text ( "Type:" );
        ImGui::SameLine ( 80 );
        ImGui::SetNextItemWidth ( 60 );
        {
            char hex[3];
            snprintf ( hex, sizeof ( hex ), "%02X", (unsigned) data->set_ftype & 0xFF );
            if ( ImGui::InputText ( "##boot_set_ftype", hex, sizeof ( hex ),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                unsigned v = 0;
                if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFF ) data->set_ftype = (int) v;
            }
        }
        ImGui::SameLine ();
        ImGui::TextDisabled ( "(0x01-0xFF)" );

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        bool type_ok = ( data->set_ftype >= 0x01 && data->set_ftype <= 0xFF );
        if ( !type_ok ) ImGui::BeginDisabled ();
        if ( ButtonMinWidth ( "OK" ) ) {
            en_MZDSK_RES r = panel_boot_set_header ( disc,
                                                     (uint16_t) data->set_fstrt,
                                                     (uint16_t) data->set_fexec,
                                                     (uint8_t) data->set_ftype );
            if ( r == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_boot_load ( data, disc, detect );
            } else {
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "%s", mzdsk_get_error ( r ) );
                data->show_error = true;
            }
            ImGui::CloseCurrentPopup ();
        }
        if ( !type_ok ) ImGui::EndDisabled ();
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* --- Error popup --- */
    if ( data->show_error ) {
        ImGui::OpenPopup ( "##boot_error" );
        data->show_error = false;
    }

    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( ImGui::BeginPopupModal ( "##boot_error", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::TextColored ( ImVec4 ( 1, 0.3f, 0.3f, 1 ), "%s", data->error_msg );
        ImGui::Spacing ();
        if ( ButtonMinWidth ( "OK" ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
}


extern "C" void panel_boot_render ( st_PANEL_BOOT_DATA *data,
                                     st_MZDSK_DISC *disc,
                                     st_MZDSK_DETECT_RESULT *detect,
                                     bool *is_dirty,
                                     st_MZDISK_CONFIG *cfg )
{
    if ( !data || !data->is_loaded ) {
        ImGui::TextDisabled ( "%s", _ ( "No boot sector data available." ) );
        return;
    }

    /* Scroll wrapper - při přetečení obsahu se objeví svislý scrollbar, */
    /* který uživateli signalizuje skrytý obsah. */
    ImGui::BeginChild ( "##boot_scroll", ImVec2 ( 0, 0 ), ImGuiChildFlags_None );

    /* IPLPRO hlavička */
    if ( ImGui::CollapsingHeader ( _ ( "IPLPRO Header (block 0)" ), ImGuiTreeNodeFlags_DefaultOpen ) ) {
        ImGui::Indent ();
        render_iplpro_section ( data, cfg );
        ImGui::Unindent ();
    }

    /* DINFO blok - jen pro full FSMZ disky */
    if ( data->fs_type == MZDSK_FS_FSMZ ) {
        ImGui::Spacing ();

        if ( ImGui::CollapsingHeader ( _ ( "DINFO Block (block 15)" ), ImGuiTreeNodeFlags_DefaultOpen ) ) {
            ImGui::Indent ();
            render_dinfo_section ( data );
            ImGui::Unindent ();
        }
    }

    /* systémové stopy - jen pro CP/M disky */
    if ( data->has_system_tracks ) {
        ImGui::Spacing ();

        if ( ImGui::CollapsingHeader ( _ ( "System Tracks (CP/M)" ), ImGuiTreeNodeFlags_DefaultOpen ) ) {
            ImGui::Indent ();
            render_system_tracks_section ( data );
            ImGui::Unindent ();
        }
    }

    /* bootstrap management */
    ImGui::Spacing ();

    if ( ImGui::CollapsingHeader ( _ ( "Bootstrap Management" ), ImGuiTreeNodeFlags_DefaultOpen ) ) {
        ImGui::Indent ();
        render_bootstrap_management ( data, disc, detect, is_dirty, cfg );
        ImGui::Unindent ();
    }

    ImGui::EndChild ();
}
