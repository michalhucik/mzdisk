/**
 * @file panel_fsmz_imgui.cpp
 * @brief ImGui rendering FSMZ directory listingu, souborových operací a Maintenance tabu.
 *
 * Zobrazuje tabulku souborů s checkboxy pro multiselect, typy, názvy,
 * velikostmi, adresami a bloky. Kliknutí kdekoliv na řádku toggleuje
 * výběr. Záhlaví sloupce checkboxů obsahuje Select All.
 *
 * Maintenance tab poskytuje operace údržby:
 * - Repair: přepočet DINFO bitmapy z adresáře a bootstrapu
 * - Defrag: defragmentace souborové oblasti (s progress callbackem)
 * - Format (File Area): vyčistí adresář a bitmapu, zachová bootstrap
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/igfd/ImGuiFileDialog.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

extern "C" {
#include "panels/panel_fsmz.h"
#include "libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mzglyphs/mzglyphs.h"
#include "libs/mz_vcode/mz_vcode.h"
#include "config.h"
#include "dragdrop.h"
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
 * Výsledek vypadá jako v hexdump panelu.
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
        /* zjistit délku jednoho UTF-8 znaku */
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

    /* posunout kurzor za vykreslený text */
    ImGui::Dummy ( ImVec2 ( x - pos.x, line_h ) );
}


/**
 * @brief Vykreslí jméno - jako text nebo CG glyfy dle režimu.
 *
 * Pro CG režimy vykreslí prefix přes ImGui::Text a pak CG glyfy
 * přes render_cg_text(). Pro ostatní režimy použije ImGui::Text.
 *
 * @param prefix Prefix řetězec (např. "Bootstrap: "), nebo NULL.
 * @param display_name Konvertované zobrazitelné jméno.
 * @param charset Režim konverze (en_MZDSK_NAME_CHARSET).
 */
static void render_name_text ( const char *prefix, const char *display_name, int charset )
{
    if ( charset >= MZDSK_NAME_CHARSET_EU_CG1 ) {
        if ( prefix ) {
            ImGui::Text ( "%s", prefix );
            ImGui::SameLine ( 0, 0 );
        }
        render_cg_text ( display_name );
    } else {
        if ( prefix ) {
            ImGui::Text ( "%s%s", prefix, display_name );
        } else {
            ImGui::TextUnformatted ( display_name );
        }
    }
}


/**
 * @brief Vrátí délku platných bajtů v Sharp MZ jméně.
 *
 * @param mz_fname Pole bajtů Sharp MZ ASCII (FSMZ_FNAME_LENGTH).
 * @return Počet bajtů před terminátorem (0x0d) nebo < 0x20.
 */
static int mz_fname_valid_len ( const uint8_t *mz_fname )
{
    int len = 0;
    while ( len < FSMZ_FNAME_LENGTH && mz_fname[len] != FSMZ_FNAME_TERMINATOR && mz_fname[len] >= 0x20 ) len++;
    return len;
}


/** @brief Klíč pro Get (export) file dialog. */
static const char *GET_DIALOG_KEY = "FsmzGetDlg";

/** @brief Klíč pro Put (import) file dialog. */
static const char *PUT_DIALOG_KEY = "FsmzPutDlg";

/** @brief Barva pozadí vybraného řádku. */
static const ImU32 SELECTED_ROW_COLOR = IM_COL32 ( 40, 70, 120, 255 );

/** @brief Příznak: čekáme na potvrzení smazání. */
static bool g_fsmz_pending_delete = false;

/** @brief Příznak: rename dialog je otevřený. */
static bool g_fsmz_pending_rename = false;

/** @brief Index souboru pro rename. */
static int g_fsmz_rename_index = -1;

/** @brief Buffer pro nové jméno v rename dialogu. */
static char g_fsmz_rename_buf[20] = "";


/**
 * @brief Reloaduje FSMZ adresář a obnoví výběr souborů.
 *
 * @param data Datový model.
 * @param disc Disk.
 * @param keep_selection true = zachovat selected[] a detail_index.
 */
static void reload_fsmz ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc, bool keep_selection )
{
    bool saved_sel[PANEL_FSMZ_MAX_FILES];
    int saved_detail = data->detail_index;
    if ( keep_selection ) {
        memcpy ( saved_sel, data->selected, sizeof ( saved_sel ) );
    }

    panel_fsmz_load ( data, disc );

    if ( keep_selection ) {
        memcpy ( data->selected, saved_sel, sizeof ( saved_sel ) );
        data->detail_index = saved_detail;
    }
}


/**
 * @brief Spočítá počet vybraných souborů.
 *
 * @param data Datový model.
 * @return Počet souborů s selected[i] == true.
 */
static int count_selected ( const st_PANEL_FSMZ_DATA *data )
{
    int count = 0;
    for ( int i = 0; i < data->file_count; i++ ) {
        if ( data->selected[i] ) count++;
    }
    return count;
}


/**
 * @brief Vykreslí toolbar s tlačítky Get, Put, Delete, Rename, Lock, Set.
 *
 * @param data Datový model.
 * @param disc Otevřený disk.
 * @param is_dirty Dirty flag.
 * @param cfg Konfigurace (pro cesty dialogů).
 */
static void render_toolbar ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc,
                             bool *is_dirty, st_MZDISK_CONFIG *cfg )
{
    int sel_count = count_selected ( data );

    /* Get (export) */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();

    if ( ImGui::Button ( _ ( "Get (Export MZF)" ) ) ) {
        IGFD::FileDialogConfig config;
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal
                     | ImGuiFileDialogFlags_DontShowHiddenFiles
                     | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;
        config.path = cfg->last_get_dir;

        if ( sel_count == 1 ) {
            config.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
            for ( int i = 0; i < data->file_count; i++ ) {
                if ( data->selected[i] ) {
                    char default_name[32];
                    snprintf ( default_name, sizeof ( default_name ), "%s.mzf", data->files[i].name );
                    config.fileName = default_name;
                    break;
                }
            }
            ImGuiFileDialog::Instance ()->OpenDialog (
                GET_DIALOG_KEY, _ ( "Export to MZF" ), ".mzf", config
            );
        } else {
            ImGuiFileDialog::Instance ()->OpenDialog (
                GET_DIALOG_KEY, _ ( "Export to directory" ), nullptr, config
            );
        }
    }

    if ( sel_count == 0 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* Put (import) */
    if ( ImGui::Button ( _ ( "Put (Import MZF)" ) ) ) {
        IGFD::FileDialogConfig config;
        config.countSelectionMax = 0;
        config.flags = ImGuiFileDialogFlags_Modal
                     | ImGuiFileDialogFlags_DontShowHiddenFiles
                     | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;
        config.path = cfg->last_put_dir;

        ImGuiFileDialog::Instance ()->OpenDialog (
            PUT_DIALOG_KEY, _ ( "Import MZF File" ), ".mzf", config
        );
    }

    ImGui::SameLine ( 0, 10 );

    /* Delete */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Delete" ) ) ) {
        g_fsmz_pending_delete = true;
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* Rename (jen pro jeden vybraný soubor) */
    if ( sel_count != 1 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Rename" ) ) ) {
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] ) {
                g_fsmz_rename_index = i;
                strncpy ( g_fsmz_rename_buf, data->files[i].name, sizeof ( g_fsmz_rename_buf ) - 1 );
                g_fsmz_rename_buf[sizeof ( g_fsmz_rename_buf ) - 1] = '\0';
                g_fsmz_pending_rename = true;
                break;
            }
        }
    }
    if ( sel_count != 1 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* --- Set (jen pro 1 soubor) - editace STRT/EXEC/ftype v directory entry --- */
    if ( sel_count != 1 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Set" ) ) ) {
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] ) {
                data->set_addr.file_idx = i;
                data->set_addr.fstrt = data->files[i].start_addr;
                data->set_addr.fexec = data->files[i].exec_addr;
                data->set_addr.ftype = data->files[i].ftype;
                data->set_addr.show = true;
                break;
            }
        }
    }
    if ( sel_count != 1 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* Lock / Unlock (hromadné pro všechny vybrané) */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    {
        /* zjistit, zda převažuje locked nebo unlocked mezi vybranými */
        int locked_count = 0;
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] && data->files[i].locked ) locked_count++;
        }
        bool new_state = ( locked_count < sel_count ); /* lock pokud převažují unlocked */
        const char *lock_label = new_state ? _ ( "Lock" ) : _ ( "Unlock" );

        if ( ImGui::Button ( lock_label ) ) {
            char errors[512] = "";
            int err_len = 0;
            bool any_ok = false;

            for ( int i = 0; i < data->file_count; i++ ) {
                if ( !data->selected[i] ) continue;
                en_MZDSK_RES res = panel_fsmz_lock_file ( disc, &data->files[i], new_state );
                if ( res == MZDSK_RES_OK ) {
                    any_ok = true;
                } else {
                    int n = snprintf ( errors + err_len, sizeof ( errors ) - err_len,
                                       "%s: %s\n", data->files[i].name, mzdsk_get_error ( res ) );
                    if ( n > 0 ) err_len += n;
                }
            }

            if ( any_ok ) {
                *is_dirty = true;
                reload_fsmz ( data, disc, true );
            }
            if ( err_len > 0 ) {
                data->has_error = true;
                strncpy ( data->error_msg, errors, sizeof ( data->error_msg ) - 1 );
                data->error_msg[sizeof ( data->error_msg ) - 1] = '\0';
            }
        }
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();

    /* Encoding label + dropdown zarovnaný vpravo.
       Pozice se počítá přes SetCursorPosX od GetWindowContentRegionMax().x;
       předchozí varianta spojená se SameLine(0, spacing) posunula prvek za
       pravý okraj okna. Teď se používá absolutní pozice - dropdown skončí
       na pravém okraji content oblasti. */
    const char *enc_label = _ ( "Encoding" );
    char enc_text[32];
    snprintf ( enc_text, sizeof ( enc_text ), "%s:", enc_label );

    ImGuiStyle &style = ImGui::GetStyle ();
    float enc_combo_w = 250.0f;
    float enc_label_w = ImGui::CalcTextSize ( enc_text ).x;
    float enc_total_w = enc_label_w + style.ItemInnerSpacing.x + enc_combo_w;

    ImGui::SameLine ();
    float cur_x = ImGui::GetCursorPosX ();
    float right_x = ImGui::GetWindowContentRegionMax ().x;
    float target_x = right_x - enc_total_w;
    if ( target_x > cur_x ) {
        ImGui::SetCursorPosX ( target_x );
    }
    ImGui::TextUnformatted ( enc_text );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( enc_combo_w );
    if ( ImGui::Combo ( "##fsmz_name_charset", &cfg->fsmz_name_charset,
                        s_name_charset_labels, IM_ARRAYSIZE ( s_name_charset_labels ) ) ) {
        mzdisk_config_save ( cfg );
    }
}


/**
 * @brief Zpracuje výsledky file dialogů (Get/Put).
 *
 * @param data Datový model.
 * @param disc Otevřený disk.
 * @param is_dirty Ukazatel na dirty flag.
 * @param cfg Konfigurace (pro ukládání cest).
 */
static void process_dialogs ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc,
                              bool *is_dirty, st_MZDISK_CONFIG *cfg )
{
    /* Get dialog */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( GET_DIALOG_KEY, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            int sel_count = count_selected ( data );

            if ( sel_count == 1 ) {
                std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
                for ( int i = 0; i < data->file_count; i++ ) {
                    if ( data->selected[i] ) {
                        panel_fsmz_get_file ( disc, &data->files[i], path.c_str () );
                        break;
                    }
                }
            } else {
                std::string dirpath = ImGuiFileDialog::Instance ()->GetFilePathName ();
                for ( int i = 0; i < data->file_count; i++ ) {
                    if ( !data->selected[i] ) continue;
                    char filepath[2048];
                    snprintf ( filepath, sizeof ( filepath ), "%s/%s.mzf",
                               dirpath.c_str (), data->files[i].name );
                    int r = mzdisk_config_resolve_export_dup ( filepath, sizeof ( filepath ), cfg->export_dup_mode );
                    if ( r == 0 ) {
                        panel_fsmz_get_file ( disc, &data->files[i], filepath );
                    }
                }
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_get_dir, dir.c_str (), sizeof ( cfg->last_get_dir ) - 1 );
            cfg->last_get_dir[sizeof ( cfg->last_get_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* Put dialog */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( PUT_DIALOG_KEY, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            auto selection = ImGuiFileDialog::Instance ()->GetSelection ();
            bool any_ok = false;
            char errors[512] = "";
            int err_len = 0;

            for ( auto &sel : selection ) {
                /* extrahovat jméno souboru z cesty */
                const char *fname = strrchr ( sel.second.c_str (), '/' );
                if ( !fname ) fname = strrchr ( sel.second.c_str (), '\\' );
                if ( fname ) fname++; else fname = sel.second.c_str ();

                en_MZDSK_RES res = panel_fsmz_put_file ( disc, sel.second.c_str () );
                if ( res == MZDSK_RES_OK ) {
                    any_ok = true;
                } else {
                    int n = snprintf ( errors + err_len, sizeof ( errors ) - err_len,
                                       "%s: %s\n", fname, mzdsk_get_error ( res ) );
                    if ( n > 0 ) err_len += n;
                }
            }

            if ( any_ok ) {
                *is_dirty = true;
                reload_fsmz ( data, disc, true );
            }

            if ( err_len > 0 ) {
                data->has_error = true;
                strncpy ( data->error_msg, errors, sizeof ( data->error_msg ) - 1 );
                data->error_msg[sizeof ( data->error_msg ) - 1] = '\0';
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_put_dir, dir.c_str (), sizeof ( cfg->last_put_dir ) - 1 );
            cfg->last_put_dir[sizeof ( cfg->last_put_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }
}


extern "C" void panel_fsmz_render ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc,
                                     bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                     uint64_t owner_session_id )
{
    if ( !data || !data->is_loaded ) {
        ImGui::TextDisabled ( "%s", _ ( "No FSMZ directory data." ) );
        return;
    }

    /* Status řádek: Files: X/Y (N selected). Maximální počet položek FSMZ
       adresáře je FSMZ_MAX_DIR_ITEMS (127). Styl shodný s MRS/CP/M. */
    int dir_sel_count = count_selected ( data );
    ImGui::Text ( "%s: %d / %d", _ ( "Files" ), data->file_count, FSMZ_MAX_DIR_ITEMS );
    ImGui::SameLine ( 0, 8 );
    ImGui::TextDisabled ( "(%d %s)", dir_sel_count, _ ( "selected" ) );

    ImGui::Spacing ();

    /* Toolbar s operačními tlačítky (vlevo) a Encoding roletkou (vpravo). */
    render_toolbar ( data, disc, is_dirty, cfg );

    ImGui::Spacing ();

    /* Drop zone helper - volá se jednou na konci s custom rect přes
     * celou oblast tabulky. BeginDragDropTargetCustom obchází problém,
     * kdy Selectable s AllowOverlap je překrytý Checkboxem - standardní
     * BeginDragDropTarget po Item by pak target nezachytil. */
    auto dnd_try_accept_rect = [&] ( const ImRect &rect, const char *id_str ) {
        if ( owner_session_id == 0 ) return;
        ImGuiID target_id = ImGui::GetID ( id_str );
        if ( !ImGui::BeginDragDropTargetCustom ( rect, target_id ) ) return;
        const ImGuiPayload *p =
            ImGui::AcceptDragDropPayload ( DND_PAYLOAD_FILE );
        if ( p && p->DataSize == (int) sizeof ( st_DND_FILE_PAYLOAD ) ) {
            const st_DND_FILE_PAYLOAD *payload =
                (const st_DND_FILE_PAYLOAD *) p->Data;
            char err[256] = {0};
            bool is_move = ImGui::GetIO ().KeyCtrl;
            /* target_cpm_user = -1: FSMZ cíl CP/M user parametr ignoruje,
               hodnota se použije jen u CP/M dst sessions. */
            en_MZDSK_RES r = dnd_handle_drop ( payload, owner_session_id,
                                                is_move, cfg->dnd_dup_mode,
                                                -1,
                                                err, sizeof ( err ) );
            if ( r != MZDSK_RES_OK ) {
                fprintf ( stderr, "DnD failed: %s\n", err );
            }
        }
        ImGui::EndDragDropTarget ();
    };

    if ( data->file_count == 0 ) {
        /* Prázdný adresář - "Directory is empty" + drop zone přes zbytek. */
        ImVec2 empty_pos = ImGui::GetCursorScreenPos ();
        ImGui::TextDisabled ( "%s", _ ( "Directory is empty." ) );
        ImVec2 avail = ImGui::GetContentRegionAvail ();
        if ( avail.y > 20.0f ) {
            ImVec2 avail_max ( empty_pos.x + ImGui::GetContentRegionMax ().x,
                               empty_pos.y + avail.y );
            ImRect r ( empty_pos, avail_max );
            dnd_try_accept_rect ( r, "##fsmz_dnd_empty" );
        }
        process_dialogs ( data, disc, is_dirty, cfg );
        return;
    }

    /* Zapamatuj si pozici před tabulkou pro drop-target rect. */
    ImVec2 table_min = ImGui::GetCursorScreenPos ();

    /* tabulka souborů */
    ImGuiTableFlags flags = ImGuiTableFlags_Borders
                          | ImGuiTableFlags_RowBg
                          | ImGuiTableFlags_Resizable
                          | ImGuiTableFlags_ScrollY
                          | ImGuiTableFlags_SizingStretchProp;

    if ( ImGui::BeginTable ( "fsmz_dir", 9, flags ) ) {

        ImGui::TableSetupScrollFreeze ( 0, 1 );
        ImGui::TableSetupColumn ( "##sel",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 30.0f );
        ImGui::TableSetupColumn ( "#",           ImGuiTableColumnFlags_WidthFixed, 30.0f );
        ImGui::TableSetupColumn ( _ ( "Type" ),  ImGuiTableColumnFlags_WidthFixed, 50.0f );
        ImGui::TableSetupColumn ( "##lock",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 20.0f );
        ImGui::TableSetupColumn ( _ ( "Name" ),  ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn ( _ ( "Size" ),  ImGuiTableColumnFlags_WidthFixed, 80.0f );
        ImGui::TableSetupColumn ( _ ( "Start" ), ImGuiTableColumnFlags_WidthFixed, 70.0f );
        ImGui::TableSetupColumn ( _ ( "Exec" ),  ImGuiTableColumnFlags_WidthFixed, 70.0f );
        ImGui::TableSetupColumn ( _ ( "Block" ), ImGuiTableColumnFlags_WidthFixed, 60.0f );

        /* vlastní záhlaví - Select All checkbox v prvním sloupci */
        ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );

        /* sloupec 0: Select All checkbox */
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::PushID ( "sel_all" );
        int sel_count = count_selected ( data );
        bool all_selected = ( sel_count == data->file_count && data->file_count > 0 );
        if ( ImGui::Checkbox ( "##selall", &all_selected ) ) {
            bool new_val = all_selected;
            for ( int i = 0; i < data->file_count; i++ ) data->selected[i] = new_val;
        }
        ImGui::PopID ();

        /* ostatní záhlaví normálně */
        for ( int col = 1; col < 9; col++ ) {
            ImGui::TableSetColumnIndex ( col );
            ImGui::TableHeader ( ImGui::TableGetColumnName ( col ) );
        }

        for ( int i = 0; i < data->file_count; i++ ) {
            st_PANEL_FSMZ_FILE *f = &data->files[i];

            ImGui::TableNextRow ();

            /* zvýraznění vybraných řádků */
            if ( data->selected[i] ) {
                ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg0, SELECTED_ROW_COLOR );
                ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg1, SELECTED_ROW_COLOR );
            }

            /* checkbox */
            ImGui::TableNextColumn ();
            char cb_id[16];
            snprintf ( cb_id, sizeof ( cb_id ), "##cb%d", i );
            ImGui::Checkbox ( cb_id, &data->selected[i] );

            /* # */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%d", f->index );

            /* typ */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%s", panel_fsmz_type_str ( f->ftype ) );

            /* lock ikona */
            ImGui::TableNextColumn ();
            if ( f->locked ) {
                ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "L" );
            }

            /* název - Selectable přes celý řádek, klik toggleuje výběr */
            ImGui::TableNextColumn ();
            char display_name[64];
            int mz_len = mz_fname_valid_len ( f->mz_fname );
            convert_mzname_display ( f->mz_fname, mz_len, display_name,
                                     (int) sizeof ( display_name ), cfg->fsmz_name_charset );

            if ( cfg->fsmz_name_charset >= MZDSK_NAME_CHARSET_EU_CG1 ) {
                /* CG režim: neviditelný Selectable + overlay CG glyfů */
                ImVec2 cell_pos = ImGui::GetCursorScreenPos ();
                char sel_id[16];
                snprintf ( sel_id, sizeof ( sel_id ), "##%d", i );
                if ( ImGui::Selectable ( sel_id, data->selected[i],
                                         ImGuiSelectableFlags_SpanAllColumns
                                         | ImGuiSelectableFlags_AllowOverlap ) ) {
                    data->selected[i] = !data->selected[i];
                    data->detail_index = data->selected[i] ? i : -1;
                }
                if ( owner_session_id != 0 && ImGui::BeginDragDropSource ( ImGuiDragDropFlags_None ) ) {
                    st_DND_FILE_PAYLOAD payload;
                    dnd_fill_payload ( &payload, owner_session_id, MZDSK_FS_FSMZ,
                                        i, data->selected, data->file_count );
                    ImGui::SetDragDropPayload ( DND_PAYLOAD_FILE, &payload, sizeof ( payload ) );
                    const char *verb = ImGui::GetIO ().KeyCtrl ? _ ( "Move:" ) : _ ( "Copy:" );
                    if ( payload.count > 1 ) {
                        ImGui::Text ( "%s %d %s", verb, payload.count, _ ( "files" ) );
                    } else {
                        ImGui::Text ( "%s %s", verb, display_name );
                    }
                    ImGui::EndDragDropSource ();
                }
                /* CG glyfy přes draw list na pozici buňky */
                ImDrawList *dl = ImGui::GetWindowDrawList ();
                ImU32 color = ImGui::GetColorU32 ( ImGuiCol_Text );
                float char_w = ImGui::CalcTextSize ( "W" ).x;
                float x = cell_pos.x;
                const char *p = display_name;
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
                    dl->AddText ( ImVec2 ( cx, cell_pos.y ), color, buf );
                    x += char_w;
                    p += clen;
                }
            } else {
                char label[96];
                snprintf ( label, sizeof ( label ), "%s##%d", display_name, i );
                if ( ImGui::Selectable ( label, data->selected[i],
                                         ImGuiSelectableFlags_SpanAllColumns
                                         | ImGuiSelectableFlags_AllowOverlap ) ) {
                    data->selected[i] = !data->selected[i];
                    data->detail_index = data->selected[i] ? i : -1;
                }
                if ( owner_session_id != 0 && ImGui::BeginDragDropSource ( ImGuiDragDropFlags_None ) ) {
                    st_DND_FILE_PAYLOAD payload;
                    dnd_fill_payload ( &payload, owner_session_id, MZDSK_FS_FSMZ,
                                        i, data->selected, data->file_count );
                    ImGui::SetDragDropPayload ( DND_PAYLOAD_FILE, &payload, sizeof ( payload ) );
                    const char *verb = ImGui::GetIO ().KeyCtrl ? _ ( "Move:" ) : _ ( "Copy:" );
                    if ( payload.count > 1 ) {
                        ImGui::Text ( "%s %d %s", verb, payload.count, _ ( "files" ) );
                    } else {
                        ImGui::Text ( "%s %s", verb, display_name );
                    }
                    ImGui::EndDragDropSource ();
                }
            }

            /* velikost */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%u", f->size );

            /* startovací adresa */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%04Xh", f->start_addr );

            /* spouštěcí adresa */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%04Xh", f->exec_addr );

            /* blok */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%u", f->block );
        }

        ImGui::EndTable ();
    }

    /* Drop target přes celou tabulku přes BeginDragDropTargetCustom.
     * Per-řádek target nefunguje kvůli Selectable(AllowOverlap) +
     * Checkbox nad ním - hover detekuje Checkbox a řádkový target se
     * neaktivuje. Tento rect pokryje celou plochu tabulky. */
    ImVec2 table_max = ImGui::GetCursorScreenPos ();
    float win_right = ImGui::GetWindowPos ().x + ImGui::GetWindowContentRegionMax ().x;
    if ( table_max.y > table_min.y ) {
        ImRect r ( table_min, ImVec2 ( win_right, table_max.y ) );
        dnd_try_accept_rect ( r, "##fsmz_dnd_table" );
    }

    /* detail vybraného souboru */
    if ( data->detail_index >= 0 && data->detail_index < data->file_count ) {
        st_PANEL_FSMZ_FILE *f = &data->files[data->detail_index];

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        {
            char detail_name[64];
            int dnl = mz_fname_valid_len ( f->mz_fname );
            convert_mzname_display ( f->mz_fname, dnl, detail_name,
                                     (int) sizeof ( detail_name ), cfg->fsmz_name_charset );
            char prefix[32];
            snprintf ( prefix, sizeof ( prefix ), "%s: ", _ ( "Name" ) );
            render_name_text ( prefix, detail_name, cfg->fsmz_name_charset );
        }
        ImGui::Text ( "%s: 0x%02X (%s)", _ ( "Type" ), f->ftype, panel_fsmz_type_str ( f->ftype ) );
        ImGui::Text ( "%s: %u B", _ ( "Size" ), f->size );
        ImGui::Text ( "%s: 0x%04X", _ ( "Start address" ), f->start_addr );
        ImGui::Text ( "%s: 0x%04X", _ ( "Exec address" ), f->exec_addr );
        ImGui::Text ( "%s: %u", _ ( "Start block" ), f->block );
        if ( f->locked ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s", _ ( "Locked" ) );
        }
    }

    /* Delete confirmation popup */
    if ( g_fsmz_pending_delete ) {
        ImGui::OpenPopup ( "##fsmz_delete" );
        g_fsmz_pending_delete = false;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##fsmz_delete", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        int del_count = count_selected ( data );
        ImGui::Text ( "%s %d %s?", _ ( "Delete" ), del_count, _ ( "file(s)" ) );
        ImGui::Spacing ();

        /* Seznam souborů ke smazání. Pro velký počet zabalit do scrollable
         * child regionu, aby dialog nepřerostl velikost obrazovky. */
        const int inline_limit = 8;
        bool use_scroll = ( del_count > inline_limit );
        if ( use_scroll ) {
            ImGui::BeginChild ( "##fsmz_del_list",
                                ImVec2 ( 420.0f, 180.0f ),
                                ImGuiChildFlags_Borders );
        }
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] ) {
                char del_name[64];
                int dnl = mz_fname_valid_len ( data->files[i].mz_fname );
                convert_mzname_display ( data->files[i].mz_fname, dnl, del_name,
                                         (int) sizeof ( del_name ), cfg->fsmz_name_charset );
                if ( cfg->fsmz_name_charset >= MZDSK_NAME_CHARSET_EU_CG1 ) {
                    ImGui::Bullet ();
                    ImGui::SameLine ( 0, 0 );
                    render_cg_text ( del_name );
                } else {
                    ImGui::BulletText ( "%s", del_name );
                }
            }
        }
        if ( use_scroll ) {
            ImGui::EndChild ();
        }
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Delete" ) ) ) {
            char errors[512] = "";
            int err_len = 0;
            bool any_ok = false;

            for ( int i = data->file_count - 1; i >= 0; i-- ) {
                if ( !data->selected[i] ) continue;
                en_MZDSK_RES res = panel_fsmz_delete_file ( disc, &data->files[i] );
                if ( res == MZDSK_RES_OK ) {
                    any_ok = true;
                } else {
                    int n = snprintf ( errors + err_len, sizeof ( errors ) - err_len,
                                       "%s: %s\n", data->files[i].name, mzdsk_get_error ( res ) );
                    if ( n > 0 ) err_len += n;
                }
            }

            if ( any_ok ) {
                *is_dirty = true;
                reload_fsmz ( data, disc, false );
            }
            if ( err_len > 0 ) {
                data->has_error = true;
                strncpy ( data->error_msg, errors, sizeof ( data->error_msg ) - 1 );
                data->error_msg[sizeof ( data->error_msg ) - 1] = '\0';
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Rename popup */
    if ( g_fsmz_pending_rename ) {
        ImGui::OpenPopup ( "##fsmz_rename" );
        g_fsmz_pending_rename = false;
    }

    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##fsmz_rename", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        if ( g_fsmz_rename_index >= 0 && g_fsmz_rename_index < data->file_count ) {
            {
                char cur_name[64];
                int cl = mz_fname_valid_len ( data->files[g_fsmz_rename_index].mz_fname );
                convert_mzname_display ( data->files[g_fsmz_rename_index].mz_fname, cl,
                                         cur_name, (int) sizeof ( cur_name ),
                                         cfg->fsmz_name_charset );
                char prefix[32];
                snprintf ( prefix, sizeof ( prefix ), "%s: ", _ ( "Current name" ) );
                render_name_text ( prefix, cur_name, cfg->fsmz_name_charset );
            }
            ImGui::Spacing ();
            ImGui::Text ( "%s:", _ ( "New name" ) );
            ImGui::SameLine ();
            ImGui::SetNextItemWidth ( 300 );

            /* auto-focus na input při prvním zobrazení */
            if ( ImGui::IsWindowAppearing () ) ImGui::SetKeyboardFocusHere ();

            bool enter = ImGui::InputText ( "##rename_input", g_fsmz_rename_buf,
                                            sizeof ( g_fsmz_rename_buf ),
                                            ImGuiInputTextFlags_EnterReturnsTrue );

            ImGui::Spacing ();
            ImGui::Separator ();
            ImGui::Spacing ();

            bool do_rename = enter || ButtonMinWidth ( "OK" );
            if ( do_rename && g_fsmz_rename_buf[0] != '\0' ) {
                en_MZDSK_RES res = panel_fsmz_rename_file ( disc,
                    &data->files[g_fsmz_rename_index], g_fsmz_rename_buf );
                if ( res == MZDSK_RES_OK ) {
                    *is_dirty = true;
                    reload_fsmz ( data, disc, true );
                } else {
                    data->has_error = true;
                    snprintf ( data->error_msg, sizeof ( data->error_msg ),
                               "%s: %s", g_fsmz_rename_buf, mzdsk_get_error ( res ) );
                }
                g_fsmz_rename_index = -1;
                ImGui::CloseCurrentPopup ();
            }
            if ( !do_rename ) {
                ImGui::SameLine ();
                if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
                    g_fsmz_rename_index = -1;
                    ImGui::CloseCurrentPopup ();
                }
            }
        }
        ImGui::EndPopup ();
    }

    /* --- Set popup (STRT/EXEC/ftype editor) --- */
    if ( data->set_addr.show ) {
        ImGui::OpenPopup ( "##fsmz_set_addr" );
        data->set_addr.show = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##fsmz_set_addr", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        int sai = data->set_addr.file_idx;
        if ( sai >= 0 && sai < data->file_count ) {
            const st_PANEL_FSMZ_FILE *sf = &data->files[sai];

            {
                char cur_name[64];
                int cl = mz_fname_valid_len ( sf->mz_fname );
                convert_mzname_display ( sf->mz_fname, cl, cur_name,
                                         (int) sizeof ( cur_name ),
                                         cfg->fsmz_name_charset );
                char prefix[32];
                snprintf ( prefix, sizeof ( prefix ), "%s: ", _ ( "File" ) );
                render_name_text ( prefix, cur_name, cfg->fsmz_name_charset );
            }
            ImGui::Spacing ();

            ImGui::Text ( "STRT:" );
            ImGui::SameLine ( 80 );
            ImGui::SetNextItemWidth ( 80 );
            {
                char hex[5];
                snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) data->set_addr.fstrt & 0xFFFF );
                if ( ImGui::InputText ( "##fsmz_sa_strt", hex, sizeof ( hex ),
                                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                    unsigned v = 0;
                    if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) data->set_addr.fstrt = (int) v;
                }
            }
            ImGui::SameLine ();
            ImGui::TextDisabled ( "(0x0000-0xFFFF)" );

            ImGui::Text ( "EXEC:" );
            ImGui::SameLine ( 80 );
            ImGui::SetNextItemWidth ( 80 );
            {
                char hex[5];
                snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) data->set_addr.fexec & 0xFFFF );
                if ( ImGui::InputText ( "##fsmz_sa_exec", hex, sizeof ( hex ),
                                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                    unsigned v = 0;
                    if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) data->set_addr.fexec = (int) v;
                }
            }
            ImGui::SameLine ();
            ImGui::TextDisabled ( "(0x0000-0xFFFF)" );

            /* Type: 2-znakový hex input (00-FF). Umožňuje i non-standard
               hodnoty pro exotické disky; 0x00 je však blokováno v libu. */
            ImGui::Text ( "Type:" );
            ImGui::SameLine ( 80 );
            ImGui::SetNextItemWidth ( 60 );
            {
                char hex[3];
                snprintf ( hex, sizeof ( hex ), "%02X", (unsigned) data->set_addr.ftype & 0xFF );
                if ( ImGui::InputText ( "##fsmz_sa_ftype", hex, sizeof ( hex ),
                                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                    unsigned v = 0;
                    if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFF ) data->set_addr.ftype = (int) v;
                }
            }
            ImGui::SameLine ();
            ImGui::TextDisabled ( "(0x01-0xFF; %s)", panel_fsmz_type_str ( (uint8_t) data->set_addr.ftype ) );

            ImGui::Spacing ();
            ImGui::Separator ();
            ImGui::Spacing ();

            bool type_ok = ( data->set_addr.ftype >= 0x01 && data->set_addr.ftype <= 0xFF );
            if ( !type_ok ) ImGui::BeginDisabled ();
            if ( ButtonMinWidth ( "OK" ) ) {
                en_MZDSK_RES r = panel_fsmz_set_addr ( disc, sf,
                                                       (uint16_t) data->set_addr.fstrt,
                                                       (uint16_t) data->set_addr.fexec,
                                                       (uint8_t) data->set_addr.ftype );
                if ( r == MZDSK_RES_OK ) {
                    *is_dirty = true;
                    reload_fsmz ( data, disc, true );
                } else {
                    snprintf ( data->error_msg, sizeof ( data->error_msg ),
                               "%s: %s", sf->name, mzdsk_get_error ( r ) );
                    data->has_error = true;
                }
                ImGui::CloseCurrentPopup ();
            }
            if ( !type_ok ) ImGui::EndDisabled ();
            ImGui::SameLine ();
            if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
                ImGui::CloseCurrentPopup ();
            }
        } else {
            ImGui::TextDisabled ( "%s", _ ( "No file selected." ) );
            if ( ButtonMinWidth ( "OK" ) ) ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* chybový popup */
    if ( data->has_error ) {
        ImGui::OpenPopup ( "##fsmz_error" );
        data->has_error = false;
    }

    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##fsmz_error", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", _ ( "Error" ) );
        ImGui::Spacing ();
        ImGui::TextUnformatted ( data->error_msg );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();
        if ( ButtonMinWidth ( "OK" ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* zpracování file dialogů */
    process_dialogs ( data, disc, is_dirty, cfg );
}


/* =========================================================================
 *  FSMZ Maintenance tab
 * ========================================================================= */


/** @brief Příznak: čekáme na potvrzení Repair. */
static bool g_fsmz_pending_repair = false;

/** @brief Příznak: čekáme na potvrzení Defrag. */
static bool g_fsmz_pending_defrag = false;

/** @brief Příznak: defragmentace právě probíhá. */
static bool g_fsmz_defrag_running = false;

/** @brief Log defragmentace (progress callback). */
static char g_fsmz_defrag_log[4096] = "";

/** @brief Délka textu v defrag logu. */
static int g_fsmz_defrag_log_len = 0;

/* Async infrastruktura pro defrag (audit C-5) - viz analogický komentář
 * v panel_cpm_imgui.cpp. */
static SDL_Thread *g_fsmz_defrag_thread = nullptr;
static SDL_Mutex *g_fsmz_defrag_mutex = nullptr;
static SDL_AtomicInt g_fsmz_defrag_done;
static en_MZDSK_RES g_fsmz_defrag_result = MZDSK_RES_OK;

static struct fsmz_defrag_ctx {
    st_MZDSK_DISC *disc;
} g_fsmz_defrag_ctx;

/** @brief Příznak: čekáme na potvrzení Format File Area. */
static bool g_fsmz_pending_format = false;

/** @brief Příznak: výsledek maintenance operace (zobrazí popup). */
static bool g_fsmz_maint_result = false;

/** @brief Zpráva výsledku maintenance operace. */
static char g_fsmz_maint_msg[512] = "";

/** @brief Příznak: výsledek je chyba (true) nebo úspěch (false). */
static bool g_fsmz_maint_is_error = false;


/**
 * @brief Callback pro průběh defragmentace - ukládá zprávy do logu.
 *
 * @param message Textová zpráva.
 * @param user_data Nepoužívá se.
 */
static void fsmz_defrag_progress_cb ( const char *message, void *user_data )
{
    (void) user_data;
    /* Volán z worker threadu - lock přes g_fsmz_defrag_mutex. Audit C-5. */
    if ( g_fsmz_defrag_mutex ) SDL_LockMutex ( g_fsmz_defrag_mutex );
    /* Ochrana proti heap overflow: viz analogický komentář v
     * panel_cpm_imgui.cpp -> cpm_defrag_progress_cb. */
    size_t capacity = sizeof ( g_fsmz_defrag_log );
    if ( (size_t) g_fsmz_defrag_log_len < capacity - 1 ) {
        size_t remaining = capacity - (size_t) g_fsmz_defrag_log_len;
        int n = snprintf ( g_fsmz_defrag_log + g_fsmz_defrag_log_len, remaining, "%s", message );
        if ( n > 0 ) {
            if ( (size_t) n >= remaining ) {
                g_fsmz_defrag_log_len = (int) ( capacity - 1 );
            } else {
                g_fsmz_defrag_log_len += n;
            }
        }
    }
    if ( g_fsmz_defrag_mutex ) SDL_UnlockMutex ( g_fsmz_defrag_mutex );
}


/**
 * @brief Worker thread entry point pro FSMZ defragmentaci (audit C-5).
 */
static int fsmz_defrag_thread_fn ( void *user_data )
{
    (void) user_data;
    g_fsmz_defrag_result = fsmz_tool_defrag ( g_fsmz_defrag_ctx.disc, PANEL_FSMZ_MAX_FILES,
                                                fsmz_defrag_progress_cb, nullptr );
    SDL_SetAtomicInt ( &g_fsmz_defrag_done, 1 );
    return 0;
}


extern "C" void panel_fsmz_maintenance_render ( st_PANEL_FSMZ_DATA *data,
                                                 st_MZDSK_DISC *disc,
                                                 bool *is_dirty )
{
    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();

    ImGui::TextWrapped ( "%s", _ ( "FSMZ disk maintenance operations. Use with caution - some operations modify disk data." ) );
    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* --- Repair --- */
    ImGui::Text ( "%s", _ ( "Repair DINFO" ) );
    ImGui::TextWrapped ( "%s", _ ( "Recalculates allocation bitmap from directory contents and bootstrap. Corrects any bitmap inconsistencies." ) );
    ImGui::Spacing ();
    if ( ImGui::Button ( _ ( "Repair" ) ) ) {
        g_fsmz_pending_repair = true;
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* --- Defrag --- */
    ImGui::Text ( "%s", _ ( "Defragmentation" ) );
    ImGui::TextWrapped ( "%s", _ ( "Reads all files into memory, formats disk, and writes files sequentially without gaps. Bootstrap is preserved if present." ) );
    ImGui::Spacing ();

    /* Polling worker threadu (audit C-5). */
    if ( g_fsmz_defrag_running && SDL_GetAtomicInt ( &g_fsmz_defrag_done ) ) {
        if ( g_fsmz_defrag_thread ) {
            SDL_WaitThread ( g_fsmz_defrag_thread, nullptr );
            g_fsmz_defrag_thread = nullptr;
        }
        g_fsmz_defrag_running = false;
        en_MZDSK_RES res = g_fsmz_defrag_result;
        if ( res == MZDSK_RES_OK ) {
            *is_dirty = true;
            panel_fsmz_load ( data, disc );
            g_fsmz_maint_is_error = false;
            snprintf ( g_fsmz_maint_msg, sizeof ( g_fsmz_maint_msg ),
                       "%s", _ ( "Defragmentation completed successfully." ) );
        } else {
            g_fsmz_maint_is_error = true;
            snprintf ( g_fsmz_maint_msg, sizeof ( g_fsmz_maint_msg ),
                       "%s: %s", _ ( "Defragmentation failed" ), mzdsk_get_error ( res ) );
        }
        g_fsmz_maint_result = true;
    }

    if ( g_fsmz_defrag_running ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Defragment" ) ) ) {
        g_fsmz_pending_defrag = true;
    }
    if ( g_fsmz_defrag_running ) ImGui::EndDisabled ();

    if ( g_fsmz_defrag_running ) {
        ImGui::SameLine ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.85f, 0.2f, 1.0f ),
                             "%s", _ ( "Defragmentation in progress..." ) );
    }

    /* defrag log - čteme pod mutexem. */
    bool have_fsmz_log;
    if ( g_fsmz_defrag_mutex ) SDL_LockMutex ( g_fsmz_defrag_mutex );
    have_fsmz_log = ( g_fsmz_defrag_log_len > 0 );
    if ( g_fsmz_defrag_mutex ) SDL_UnlockMutex ( g_fsmz_defrag_mutex );

    if ( have_fsmz_log ) {
        ImGui::Spacing ();
        ImGui::BeginChild ( "defrag_log", ImVec2 ( 0, 150 ), ImGuiChildFlags_Borders );
        if ( g_fsmz_defrag_mutex ) SDL_LockMutex ( g_fsmz_defrag_mutex );
        ImGui::TextUnformatted ( g_fsmz_defrag_log );
        if ( g_fsmz_defrag_mutex ) SDL_UnlockMutex ( g_fsmz_defrag_mutex );
        ImGui::SetScrollHereY ( 1.0f );
        ImGui::EndChild ();
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* --- Format File Area --- */
    ImGui::Text ( "%s", _ ( "Format (File Area)" ) );
    ImGui::TextWrapped ( "%s", _ ( "Clears directory and allocation bitmap. Bootstrap is preserved. All file data becomes inaccessible." ) );
    ImGui::Spacing ();
    if ( ImGui::Button ( _ ( "Format File Area" ) ) ) {
        g_fsmz_pending_format = true;
    }

    /* ======= Potvrzovací dialogy ======= */

    /* Repair potvrzení */
    if ( g_fsmz_pending_repair ) {
        ImGui::OpenPopup ( "##fsmz_repair_confirm" );
        g_fsmz_pending_repair = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##fsmz_repair_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Repair DINFO" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "This will recalculate the allocation bitmap from directory contents. Any inconsistencies in the bitmap will be corrected." ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Repair" ) ) ) {
            en_MZDSK_RES res = fsmz_tool_repair_dinfo ( disc, PANEL_FSMZ_MAX_FILES );
            if ( res == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_fsmz_load ( data, disc );
                g_fsmz_maint_is_error = false;
                snprintf ( g_fsmz_maint_msg, sizeof ( g_fsmz_maint_msg ),
                           "%s", _ ( "DINFO repair completed successfully." ) );
            } else {
                g_fsmz_maint_is_error = true;
                snprintf ( g_fsmz_maint_msg, sizeof ( g_fsmz_maint_msg ),
                           "%s: %s", _ ( "Repair failed" ), mzdsk_get_error ( res ) );
            }
            g_fsmz_maint_result = true;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Defrag potvrzení */
    if ( g_fsmz_pending_defrag ) {
        ImGui::OpenPopup ( "##fsmz_defrag_confirm" );
        g_fsmz_pending_defrag = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##fsmz_defrag_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Defragmentation" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "This will read all files into memory, format the disk, and write files sequentially without gaps. Bootstrap will be preserved if present." ) );
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s",
                            _ ( "WARNING: If the process is interrupted, data may be lost!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ImGui::Button ( _ ( "Defragment" ), ImVec2 ( 160, 0 ) ) ) {
            /* Asynchronní defrag (audit C-5). Finalizace se dělá v polling
             * bloku v render funkci. */
            g_fsmz_defrag_log[0] = '\0';
            g_fsmz_defrag_log_len = 0;
            g_fsmz_defrag_running = true;
            g_fsmz_defrag_result = MZDSK_RES_OK;
            SDL_SetAtomicInt ( &g_fsmz_defrag_done, 0 );

            if ( !g_fsmz_defrag_mutex ) g_fsmz_defrag_mutex = SDL_CreateMutex ();

            g_fsmz_defrag_ctx.disc = disc;

            g_fsmz_defrag_thread = SDL_CreateThread ( fsmz_defrag_thread_fn, "fsmz_defrag", nullptr );
            if ( !g_fsmz_defrag_thread ) {
                /* Fallback na synchronní cestu. */
                g_fsmz_defrag_running = false;
                g_fsmz_defrag_result = fsmz_tool_defrag ( disc, PANEL_FSMZ_MAX_FILES,
                                                            fsmz_defrag_progress_cb, nullptr );
                SDL_SetAtomicInt ( &g_fsmz_defrag_done, 1 );
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ImGui::Button ( _ ( "Cancel" ), ImVec2 ( 160, 0 ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Format File Area potvrzení */
    if ( g_fsmz_pending_format ) {
        ImGui::OpenPopup ( "##fsmz_format_confirm" );
        g_fsmz_pending_format = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##fsmz_format_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Format File Area" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "This will erase the directory and allocation bitmap. All file data will become inaccessible." ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "Bootstrap will be preserved (including Normal bootstrap data in file area)." ) );
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                            _ ( "WARNING: This operation cannot be undone!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Format" ) ) ) {
            en_MZDSK_RES res = fsmz_tool_format_file_area ( disc );
            if ( res == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_fsmz_load ( data, disc );
                g_fsmz_maint_is_error = false;
                snprintf ( g_fsmz_maint_msg, sizeof ( g_fsmz_maint_msg ),
                           "%s", _ ( "File area format completed successfully." ) );
            } else {
                g_fsmz_maint_is_error = true;
                snprintf ( g_fsmz_maint_msg, sizeof ( g_fsmz_maint_msg ),
                           "%s: %s", _ ( "Format failed" ), mzdsk_get_error ( res ) );
            }
            g_fsmz_maint_result = true;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Výsledek operace */
    if ( g_fsmz_maint_result ) {
        ImGui::OpenPopup ( "##fsmz_maint_result" );
        g_fsmz_maint_result = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##fsmz_maint_result", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        if ( g_fsmz_maint_is_error ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", _ ( "Error" ) );
        } else {
            ImGui::TextColored ( ImVec4 ( 0.4f, 1.0f, 0.4f, 1.0f ), "%s", _ ( "Success" ) );
        }
        ImGui::Spacing ();
        ImGui::TextUnformatted ( g_fsmz_maint_msg );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();
        if ( ButtonMinWidth ( "OK" ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
}
