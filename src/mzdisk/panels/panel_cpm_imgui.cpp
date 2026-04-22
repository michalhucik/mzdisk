/**
 * @file panel_cpm_imgui.cpp
 * @brief ImGui rendering CP/M directory listingu, souborových operací, alloc mapy a Maintenance tabu.
 *
 * Zobrazuje dva sub-taby:
 * - Directory: tabulka souborů s checkboxy, Get/Put/Delete/Rename, atributy
 * - Alloc Map: DPB parametry, statistiky obsazenosti, disk layout pruh,
 *   legenda per-file (linkový styl, stisk myši zvýrazní bloky souboru
 *   v mřížce a ztlumí ostatní) + strukturální typy, bloková mřížka
 *   s hover tooltipem
 *
 * Maintenance tab poskytuje operace údržby:
 * - Check: read-only kontrola konzistence extentů všech souborů
 * - Defrag: defragmentace souborové oblasti (mzdsk_cpm_defrag s progress callbackem)
 * - Format (File Area): vymaže adresář (0xE5), zachová system tracks
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/igfd/ImGuiFileDialog.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>

extern "C" {
#include "panels/panel_cpm.h"
#include "config.h"
#include "dragdrop.h"
#include "i18n.h"
}

#include "ui_helpers.h"

static const char *CPM_GET_DLG = "CpmGetDlg";
static const char *CPM_PUT_DLG = "CpmPutDlg";
static const char *CPM_GET_MZF_DLG = "CpmGetMzfDlg";
static const char *CPM_PUT_MZF_DLG = "CpmPutMzfDlg";
static const ImU32 SELECTED_ROW_COLOR = IM_COL32 ( 40, 70, 120, 255 );

static bool g_cpm_pending_delete = false;
static bool g_cpm_pending_rename = false;
static int g_cpm_rename_index = -1;
static char g_cpm_rename_buf[13] = "";  /* NAME.EXT */

/* Put file as... dialog state */
static bool g_cpm_put_pending_open = false;        /**< Otevřít popup v příštím frame. */
static bool g_cpm_put_is_mzf = false;              /**< true = put --mzf, false = raw put. */
static char g_cpm_put_src_path[1024] = "";         /**< Cesta ke zdrojovému souboru. */
static char g_cpm_put_target_buf[16] = "";         /**< Editované cílové jméno NAME.EXT. */
static bool g_cpm_put_auto_truncate = false;       /**< Povolit zkrácení při > 8.3. */
static en_MZDSK_NAMEVAL g_cpm_put_last_err = MZDSK_NAMEVAL_OK; /**< Výsledek validace. */
static char g_cpm_put_bad_char = 0;                /**< První zakázaný znak pro BAD_CHAR. */
static en_MZF_NAME_ENCODING g_cpm_put_charset = MZF_NAME_ASCII_EU; /**< Varianta Sharp MZ pro derivaci fname. */
static bool g_cpm_put_refocus_name = false;        /**< true = v příštím frame zaostřit InputText a označit celý text. */
static int g_cpm_put_target_user = 0;              /**< Cílový CP/M user (0-15) v Put dialogu. Nastavuje se při otevření. */


/**
 * @brief Odvodí cílové 8.3 jméno z MZF hlavičky s vybranou Sharp MZ variantou.
 *
 * Načte 128B hlavičku z `mzf_path`, zavolá `mzdsk_cpm_mzf_decode_ex()`
 * s daným `encoding` a do `out_buf` zapíše řetězec ve formátu "NAME.EXT"
 * (nebo jen "NAME" pokud je přípona prázdná). Používá se pro předvyplnění
 * target inputu v "Put file as..." dialogu a pro přegenerování při
 * změně charset roletky.
 *
 * @param[in]  mzf_path  Cesta k MZF souboru na hostiteli. Nesmí být NULL.
 * @param[in]  encoding  Varianta Sharp MZ znakové sady (EU nebo JP ASCII).
 * @param[out] out_buf   Výstupní buffer pro řetězec NAME.EXT. Nesmí být NULL.
 * @param[in]  buf_size  Velikost výstupního bufferu v bajtech.
 *
 * @return true při úspěchu, false pokud se MZF nepodařilo načíst nebo dekódovat.
 *         Při neúspěchu není `out_buf` modifikován.
 */
static bool cpm_derive_name_from_mzf ( const char *mzf_path,
                                         en_MZF_NAME_ENCODING encoding,
                                         char *out_buf, size_t buf_size )
{
    FILE *fp = fopen ( mzf_path, "rb" );
    if ( !fp ) return false;

    fseek ( fp, 0, SEEK_END );
    long file_size = ftell ( fp );
    fseek ( fp, 0, SEEK_SET );

    if ( file_size < 128 ) { fclose ( fp ); return false; }

    uint8_t *mzf_data = (uint8_t *) malloc ( (size_t) file_size );
    if ( !mzf_data ) { fclose ( fp ); return false; }

    size_t read_count = fread ( mzf_data, 1, (size_t) file_size, fp );
    fclose ( fp );
    if ( (long) read_count != file_size ) { free ( mzf_data ); return false; }

    char cpm_name[9] = { 0 };
    char cpm_ext[4] = { 0 };
    uint8_t *body = NULL;
    uint32_t body_size = 0;

    en_MZDSK_RES res = mzdsk_cpm_mzf_decode_ex ( mzf_data, (uint32_t) file_size,
        encoding, cpm_name, cpm_ext, NULL, NULL, &body, &body_size );

    free ( mzf_data );
    free ( body );

    if ( res != MZDSK_RES_OK ) return false;

    if ( cpm_ext[0] != '\0' ) {
        snprintf ( out_buf, buf_size, "%s.%s", cpm_name, cpm_ext );
    } else {
        snprintf ( out_buf, buf_size, "%s", cpm_name );
    }
    return true;
}


/**
 * @brief Provede tolerantní zkrácení CP/M jména na 8.3 s toupper normalizací.
 *
 * @param[in]  input    Vstupní "name.ext" nebo "name".
 * @param[out] out_name Výstupní jméno (9 B, NUL-term, UPPERCASE).
 * @param[out] out_ext  Výstupní přípona (4 B, NUL-term, UPPERCASE).
 *
 * @pre input != NULL && out_name != NULL && out_ext != NULL.
 */
static void cpm_truncate_name ( const char *input, char *out_name, char *out_ext )
{
    memset ( out_name, 0, 9 );
    memset ( out_ext, 0, 4 );
    const char *dot = strchr ( input, '.' );
    int nlen = dot ? (int) ( dot - input ) : (int) strlen ( input );
    if ( nlen > 8 ) nlen = 8;
    for ( int i = 0; i < nlen; i++ ) {
        out_name[i] = (char) toupper ( (unsigned char) input[i] );
    }
    if ( dot ) {
        int elen = (int) strlen ( dot + 1 );
        if ( elen > 3 ) elen = 3;
        for ( int i = 0; i < elen; i++ ) {
            out_ext[i] = (char) toupper ( (unsigned char) dot[1 + i] );
        }
    }
}


/**
 * @brief Sestaví chybovou hlášku pro tooltip Put dialogu.
 *
 * @param[in]  code     Kód validace.
 * @param[in]  bad      Zakázaný znak (pro BAD_CHAR).
 * @param[out] buf      Výstupní buffer pro lokalizovanou hlášku.
 * @param[in]  buf_size Velikost bufferu.
 */
static void cpm_put_format_error ( en_MZDSK_NAMEVAL code, char bad, char *buf, size_t buf_size )
{
    switch ( code ) {
        case MZDSK_NAMEVAL_EMPTY:
            snprintf ( buf, buf_size, "%s", _ ( "Empty filename" ) );
            break;
        case MZDSK_NAMEVAL_NAME_TOO_LONG:
            snprintf ( buf, buf_size, "%s", _ ( "Name exceeds 8 characters" ) );
            break;
        case MZDSK_NAMEVAL_EXT_TOO_LONG:
            snprintf ( buf, buf_size, "%s", _ ( "Extension exceeds 3 characters" ) );
            break;
        case MZDSK_NAMEVAL_BAD_CHAR:
            snprintf ( buf, buf_size, "%s '%c'", _ ( "Forbidden character" ), bad );
            break;
        default:
            buf[0] = '\0';
    }
}


/* =========================================================================
 * Pomocné funkce pro directory tab
 * ========================================================================= */

/**
 * @brief Sestaví řetězec atributů CP/M souboru.
 */
static void format_attrs ( const st_MZDSK_CPM_FILE_INFO_EX *f, char *buf )
{
    buf[0] = f->read_only ? 'R' : '-';
    buf[1] = f->system    ? 'S' : '-';
    buf[2] = f->archived  ? 'A' : '-';
    buf[3] = '\0';
}


/**
 * @brief Sestaví CP/M jméno souboru ve formátu NAME.EXT.
 */
static void format_fullname ( const st_MZDSK_CPM_FILE_INFO_EX *f, char *buf, int buf_size )
{
    if ( f->extension[0] ) {
        snprintf ( buf, buf_size, "%s.%s", f->filename, f->extension );
    } else {
        snprintf ( buf, buf_size, "%s", f->filename );
    }
}


/**
 * @brief Rozparsuje NAME.EXT na jméno a příponu.
 *
 * @return true při úspěchu.
 */
static bool parse_cpm_name ( const char *input, char *name, char *ext )
{
    memset ( name, 0, 9 );
    memset ( ext, 0, 4 );

    const char *dot = strchr ( input, '.' );
    int name_len, ext_len = 0;

    if ( dot ) {
        name_len = (int) ( dot - input );
        ext_len = (int) strlen ( dot + 1 );
    } else {
        name_len = (int) strlen ( input );
    }

    if ( name_len < 1 || name_len > 8 || ext_len > 3 ) return false;

    for ( int i = 0; i < name_len; i++ ) {
        name[i] = (char) toupper ( (unsigned char) input[i] );
    }
    if ( dot ) {
        for ( int i = 0; i < ext_len; i++ ) {
            ext[i] = (char) toupper ( (unsigned char) dot[1 + i] );
        }
    }
    return true;
}


static int count_selected ( const st_PANEL_CPM_DATA *data )
{
    int count = 0;
    for ( int i = 0; i < data->file_count; i++ ) {
        if ( data->selected[i] ) count++;
    }
    return count;
}


/* =========================================================================
 * Alloc Map - barvy a konstanty
 * ========================================================================= */

/** @brief Velikost jedné buňky alloc mřížky v pixelech. */
static const float ALLOC_CELL_SIZE = 16.0f;

/** @brief Mezera mezi buňkami v pixelech. */
static const float ALLOC_CELL_GAP = 1.0f;

/** @brief Výška pruhu disk layout v pixelech. */
static const float ALLOC_LAYOUT_BAR_HEIGHT = 36.0f;

/** @brief Minimální šířka segmentu v pixelech. */
static const float ALLOC_LAYOUT_MIN_SEGMENT_WIDTH = 60.0f;

/** @brief Barva adresářových bloků (modrá). */
static const ImU32 COL_DIR = IM_COL32 ( 33, 150, 243, 255 );

/** @brief Barva volných bloků (tmavě zelená). */
static const ImU32 COL_FREE = IM_COL32 ( 40, 60, 40, 255 );

/** @brief Barva systémových stop (fialová). */
static const ImU32 COL_SYSTEM = IM_COL32 ( 156, 39, 176, 255 );

/** @brief Index souboru zvýrazněného kliknutím v legendě (-1 = žádný). */
static int g_cpm_legend_sel = -1;


/**
 * @brief Převede HSV na ImU32 barvu.
 *
 * @param h Odstín (0-360).
 * @param s Saturace (0-1).
 * @param v Hodnota (0-1).
 * @return ImU32 barva.
 */
static ImU32 hsv_to_col32 ( float h, float s, float v )
{
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB ( h / 360.0f, s, v, r, g, b );
    return IM_COL32 ( (int) ( r * 255 ), (int) ( g * 255 ), (int) ( b * 255 ), 255 );
}


/**
 * @brief Vrátí barvu pro soubor podle jeho indexu pomocí golden angle HSV rotace.
 *
 * Zajistí maximální vizuální rozlišení sousedních souborů.
 *
 * @param file_idx Index souboru v files[] (0-based).
 * @return ImU32 barva.
 */
static ImU32 file_index_color ( int file_idx )
{
    float h = fmodf ( (float) ( file_idx + 1 ) * 137.508f, 360.0f );
    return hsv_to_col32 ( h, 0.65f, 0.85f );
}


/**
 * @brief Vrátí barvu pro blok podle jeho vlastníka.
 *
 * @param owner Hodnota z block_owner[].
 * @return ImU32 barva.
 */
static ImU32 block_color ( uint16_t owner )
{
    if ( owner == PANEL_CPM_BLOCK_FREE ) return COL_FREE;
    if ( owner == PANEL_CPM_BLOCK_DIR )  return COL_DIR;
    return file_index_color ( (int) owner );
}


/**
 * @brief Spočítá počet adresářových bloků z AL0/AL1.
 *
 * @param dpb Disk Parameter Block.
 * @return Počet bloků alokovaných pro adresář.
 */
static int count_dir_blocks ( const st_MZDSK_CPM_DPB *dpb )
{
    uint16_t al = ( (uint16_t) dpb->al0 << 8 ) | dpb->al1;
    int count = 0;
    for ( int i = 0; i < 16; i++ ) {
        if ( al & ( 0x8000u >> i ) ) count++;
    }
    return count;
}


/* =========================================================================
 * Directory tab - rendering
 * ========================================================================= */

/**
 * @brief Vykreslí toolbar.
 */
static void render_toolbar ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                             bool *is_dirty, st_MZDISK_CONFIG *cfg )
{
    int sel_count = count_selected ( data );

    /* Get (export raw). Počet vybraných souborů je vidět u "Files:"
       labelu v render_directory_tab, v tlačítku ho už nedublujeme. */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Get (Export)" ) ) ) {
        IGFD::FileDialogConfig config;
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles;
        config.path = cfg->last_get_dir;

        if ( sel_count == 1 ) {
            config.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
            for ( int i = 0; i < data->file_count; i++ ) {
                if ( data->selected[i] ) {
                    char fname[16];
                    format_fullname ( &data->files[i], fname, sizeof ( fname ) );
                    config.fileName = fname;
                    break;
                }
            }
            ImGuiFileDialog::Instance ()->OpenDialog ( CPM_GET_DLG, _ ( "Export file" ), "((.*))", config );
        } else {
            ImGuiFileDialog::Instance ()->OpenDialog ( CPM_GET_DLG, _ ( "Export to directory" ), nullptr, config );
        }
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* Put (import raw) */
    if ( ImGui::Button ( _ ( "Put (Import)" ) ) ) {
        IGFD::FileDialogConfig config;
        config.countSelectionMax = 0;
        config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles;
        config.path = cfg->last_put_dir;
        ImGuiFileDialog::Instance ()->OpenDialog ( CPM_PUT_DLG, _ ( "Import file" ), "((.*))", config );
    }

    ImGui::SameLine ( 0, 10 );

    /* Get MZF (export jako CPM-IC MZF) - otevře options popup */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Get MZF" ) ) ) {
        panel_cpm_mzf_export_init ( &data->mzf_export );
        data->mzf_export.show_opts = true;
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* Put MZF (import z CPM-IC MZF) */
    if ( ImGui::Button ( _ ( "Put MZF" ) ) ) {
        IGFD::FileDialogConfig config;
        config.countSelectionMax = 0;
        config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles;
        config.path = cfg->last_put_dir;
        ImGuiFileDialog::Instance ()->OpenDialog ( CPM_PUT_MZF_DLG, _ ( "Import MZF (CPM-IC)" ), ".mzf", config );
    }

    ImGui::SameLine ( 0, 10 );

    /* Delete */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Delete" ) ) ) {
        g_cpm_pending_delete = true;
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* Rename */
    if ( sel_count != 1 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Rename" ) ) ) {
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] ) {
                g_cpm_rename_index = i;
                format_fullname ( &data->files[i], g_cpm_rename_buf, sizeof ( g_cpm_rename_buf ) );
                g_cpm_pending_rename = true;
                break;
            }
        }
    }
    if ( sel_count != 1 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* Atributy + chuser - hromadné pro všechny vybrané soubory.
       Layout: "#: [roletka]   Attr: R/O SYS ARC" */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    {
        /* Stav prvního vybraného (pro atributy) + detekce mixed user. */
        bool ro = false, sys = false, arc = false;
        int disp_user = -1;  /* -1 = mixed/none, 0-15 = jednotný user */
        bool first = true;
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( !data->selected[i] ) continue;
            if ( first ) {
                ro = (bool) data->files[i].read_only;
                sys = (bool) data->files[i].system;
                arc = (bool) data->files[i].archived;
                disp_user = data->files[i].user;
                first = false;
            } else if ( disp_user != data->files[i].user ) {
                disp_user = -1;
            }
        }

        /* chuser roletka "#:" před atributy. */
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ( "#:" );
        ImGui::SameLine ();
        ImGui::SetNextItemWidth ( 60.0f );

        const char *chu_items[17] = {
            "--",
            "0", "1", "2", "3", "4", "5", "6", "7",
            "8", "9", "10", "11", "12", "13", "14", "15"
        };
        int chu_idx = ( disp_user < 0 ) ? 0 : ( disp_user + 1 );
        if ( chu_idx < 0 ) chu_idx = 0;
        if ( chu_idx > 16 ) chu_idx = 16;
        int prev_idx = chu_idx;
        if ( ImGui::Combo ( "##cpm_chuser", &chu_idx,
                             chu_items, IM_ARRAYSIZE ( chu_items ) ) ) {
            /* "--" → no-op. Jinak pokud změna z původní, chuser
               na všech vybraných. */
            if ( chu_idx != 0 && chu_idx != prev_idx ) {
                uint8_t new_user = (uint8_t) ( chu_idx - 1 );
                for ( int i = 0; i < data->file_count; i++ ) {
                    if ( !data->selected[i] ) continue;
                    st_MZDSK_CPM_FILE_INFO_EX *f = &data->files[i];
                    if ( f->user == new_user ) continue;  /* už tam je */
                    en_MZDSK_RES res = panel_cpm_set_user ( disc, &data->dpb,
                                                              f, new_user );
                    if ( res == MZDSK_RES_OK ) {
                        *is_dirty = true;
                        f->user = new_user;
                    } else if ( res == MZDSK_RES_FILE_EXISTS ) {
                        /* Namespace kolize. Respektujeme Settings volbu
                           (Skip = tichý skip, ostatní = chyba). */
                        if ( cfg->dnd_dup_mode == MZDSK_EXPORT_DUP_SKIP ) {
                            /* tichý skip - no op */
                        } else {
                            data->has_error = true;
                            char fn[16];
                            format_fullname ( f, fn, sizeof ( fn ) );
                            snprintf ( data->error_msg,
                                       sizeof ( data->error_msg ),
                                       "%s: user %u already has this name",
                                       fn, (unsigned) new_user );
                        }
                    } else {
                        data->has_error = true;
                        char fn[16];
                        format_fullname ( f, fn, sizeof ( fn ) );
                        snprintf ( data->error_msg,
                                   sizeof ( data->error_msg ),
                                   "%s: %s", fn, mzdsk_get_error ( res ) );
                    }
                }
            }
        }
        if ( ImGui::IsItemHovered () ) {
            ImGui::SetTooltip ( "%s",
                _ ( "Change CP/M user area of selected files.\n"
                    "\n"
                    "'--' = no change (shown also when selection mixes different users).\n"
                    "Conflicts with existing names are reported as errors;\n"
                    "the file keeps its original user." ) );
        }

        /* Atributy: Attr: R/O SYS ARC */
        ImGui::SameLine ( 0, 20 );
        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "%s:", _ ( "Attr" ) );
        ImGui::SameLine ();

        bool changed = false;
        if ( ImGui::Checkbox ( "R/O", &ro ) ) changed = true;
        ImGui::SameLine ();
        if ( ImGui::Checkbox ( "SYS", &sys ) ) changed = true;
        ImGui::SameLine ();
        if ( ImGui::Checkbox ( "ARC", &arc ) ) changed = true;

        if ( changed ) {
            uint8_t attrs = 0;
            if ( ro ) attrs |= MZDSK_CPM_ATTR_READ_ONLY;
            if ( sys ) attrs |= MZDSK_CPM_ATTR_SYSTEM;
            if ( arc ) attrs |= MZDSK_CPM_ATTR_ARCHIVED;

            for ( int i = 0; i < data->file_count; i++ ) {
                if ( !data->selected[i] ) continue;
                st_MZDSK_CPM_FILE_INFO_EX *f = &data->files[i];
                en_MZDSK_RES res = panel_cpm_set_attrs ( disc, &data->dpb, f, attrs );
                if ( res == MZDSK_RES_OK ) {
                    *is_dirty = true;
                    f->read_only = ro ? 1 : 0;
                    f->system = sys ? 1 : 0;
                    f->archived = arc ? 1 : 0;
                } else {
                    data->has_error = true;
                    char fn[16];
                    format_fullname ( f, fn, sizeof ( fn ) );
                    snprintf ( data->error_msg, sizeof ( data->error_msg ),
                               "%s: %s", fn, mzdsk_get_error ( res ) );
                }
            }
        }
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();
}


/**
 * @brief Vykreslí obsah directory tabu.
 */
static void render_directory_tab ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                                    bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                    uint64_t owner_session_id )
{
    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s: %s", _ ( "Preset" ), data->preset_name );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d", _ ( "Files" ), data->file_count );
    /* Počet vybraných souborů vedle "Files:", konzistentně s MRS panelem.
       Zobrazuje i 0 (nezávisle na sel_count > 0) - uživatel tak hned vidí,
       že žádný soubor není vybraný. */
    {
        int dir_sel_count = count_selected ( data );
        ImGui::SameLine ( 0, 8 );
        ImGui::TextDisabled ( "(%d %s)", dir_sel_count, _ ( "selected" ) );
    }

    /* User filter roletka zarovnaná vpravo na Preset řádku.
       -1 = all (směr: filter+default target user), 0-15 = filter + target.
       Pro zarovnání vpravo použijeme SetCursorPosX vypočtený z
       GetWindowContentRegionMax (absolute), ne GetContentRegionAvail
       (rezidual po Text/SameLine). */
    {
        const float combo_w = 80.0f;
        const char *lbl = _ ( "User:" );
        float lbl_w = ImGui::CalcTextSize ( lbl ).x;
        float gap = ImGui::GetStyle ().ItemInnerSpacing.x;
        float total = lbl_w + gap + combo_w;

        /* Right-align: x = content right - total. Pokud by to přepsalo
           předchozí obsah (shift záporný), necháme prvky hned vedle textu. */
        ImGui::SameLine ();
        float cur_x = ImGui::GetCursorPosX ();
        float right_x = ImGui::GetWindowContentRegionMax ().x;
        float target_x = right_x - total;
        if ( target_x > cur_x ) ImGui::SetCursorPosX ( target_x );

        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ( lbl );
        ImGui::SameLine ( 0, gap );
        ImGui::SetNextItemWidth ( combo_w );
        const char *user_items[17] = {
            "all",
            "0", "1", "2", "3", "4", "5", "6", "7",
            "8", "9", "10", "11", "12", "13", "14", "15"
        };
        int idx = ( data->filter_user < 0 ) ? 0 : ( data->filter_user + 1 );
        if ( idx < 0 ) idx = 0;
        if ( idx > 16 ) idx = 16;
        if ( ImGui::Combo ( "##cpm_filter_user", &idx,
                             user_items, IM_ARRAYSIZE ( user_items ) ) ) {
            data->filter_user = ( idx == 0 ) ? -1 : ( idx - 1 );
        }
        if ( ImGui::IsItemHovered () ) {
            ImGui::SetTooltip ( "%s",
                _ ( "Filter directory by CP/M user area.\n"
                    "Also drives target user for Put/Import and drag-and-drop.\n"
                    "\n"
                    "'all' shows every user;\n"
                    "imports default to user 0;\n"
                    "drops from CP/M sources preserve their source user." ) );
        }
    }

    ImGui::Spacing ();
    render_toolbar ( data, disc, is_dirty, cfg );
    ImGui::Spacing ();

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
            /* target_cpm_user: filter nastavený uživatelem (-1=all = auto).
               Auto logika v dnd_handle_drop: src=CPM → zachovat source user,
               jinak user 0. Explicitní filter (0-15) přepíše vše. */
            en_MZDSK_RES r = dnd_handle_drop ( payload, owner_session_id,
                                                is_move, cfg->dnd_dup_mode,
                                                data->filter_user,
                                                err, sizeof ( err ) );
            if ( r != MZDSK_RES_OK ) fprintf ( stderr, "DnD failed: %s\n", err );
        }
        ImGui::EndDragDropTarget ();
    };

    if ( data->file_count == 0 ) {
        ImVec2 empty_pos = ImGui::GetCursorScreenPos ();
        ImGui::TextDisabled ( "%s", _ ( "Directory is empty." ) );
        ImVec2 avail = ImGui::GetContentRegionAvail ();
        if ( avail.y > 20.0f ) {
            ImVec2 avail_max ( empty_pos.x + ImGui::GetContentRegionMax ().x,
                               empty_pos.y + avail.y );
            ImRect r ( empty_pos, avail_max );
            dnd_try_accept_rect ( r, "##cpm_dnd_empty" );
        }
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

    if ( ImGui::BeginTable ( "cpm_dir", 7, flags ) ) {

        ImGui::TableSetupScrollFreeze ( 0, 1 );
        ImGui::TableSetupColumn ( "##sel",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 30.0f );
        ImGui::TableSetupColumn ( _ ( "User" ),  ImGuiTableColumnFlags_WidthFixed, 50.0f );
        ImGui::TableSetupColumn ( _ ( "Name" ),  ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn ( _ ( "Ext" ),   ImGuiTableColumnFlags_WidthFixed, 50.0f );
        ImGui::TableSetupColumn ( _ ( "Size" ),  ImGuiTableColumnFlags_WidthFixed, 90.0f );
        ImGui::TableSetupColumn ( _ ( "Attr" ),  ImGuiTableColumnFlags_WidthFixed, 60.0f );
        ImGui::TableSetupColumn ( _ ( "Ext#" ),  ImGuiTableColumnFlags_WidthFixed, 50.0f );

        /* záhlaví s Select All */
        ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::PushID ( "cpm_sel_all" );
        int sel_count = count_selected ( data );
        bool all_selected = ( sel_count == data->file_count && data->file_count > 0 );
        if ( ImGui::Checkbox ( "##selall", &all_selected ) ) {
            for ( int i = 0; i < data->file_count; i++ ) data->selected[i] = all_selected;
        }
        ImGui::PopID ();
        for ( int col = 1; col < 7; col++ ) {
            ImGui::TableSetColumnIndex ( col );
            ImGui::TableHeader ( ImGui::TableGetColumnName ( col ) );
        }

        for ( int i = 0; i < data->file_count; i++ ) {
            st_MZDSK_CPM_FILE_INFO_EX *f = &data->files[i];

            /* Filter user oblasti: -1 = all, 0-15 = jen daný user. */
            if ( data->filter_user >= 0
                 && f->user != (uint8_t) data->filter_user ) {
                continue;
            }

            ImGui::TableNextRow ();

            if ( data->selected[i] ) {
                ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg0, SELECTED_ROW_COLOR );
                ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg1, SELECTED_ROW_COLOR );
            }

            /* checkbox */
            ImGui::TableNextColumn ();
            char cb_id[16];
            snprintf ( cb_id, sizeof ( cb_id ), "##ccb%d", i );
            ImGui::Checkbox ( cb_id, &data->selected[i] );

            /* user */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%d", f->user );

            /* název */
            ImGui::TableNextColumn ();
            char label[32];
            snprintf ( label, sizeof ( label ), "%-8s##c%d", f->filename, i );
            if ( ImGui::Selectable ( label, data->selected[i],
                                     ImGuiSelectableFlags_SpanAllColumns
                                     | ImGuiSelectableFlags_AllowOverlap ) ) {
                data->selected[i] = !data->selected[i];
                data->detail_index = data->selected[i] ? i : -1;
            }
            if ( owner_session_id != 0 && ImGui::BeginDragDropSource ( ImGuiDragDropFlags_None ) ) {
                st_DND_FILE_PAYLOAD payload;
                dnd_fill_payload ( &payload, owner_session_id, MZDSK_FS_CPM,
                                    i, data->selected, data->file_count );
                ImGui::SetDragDropPayload ( DND_PAYLOAD_FILE, &payload, sizeof ( payload ) );
                const char *verb = ImGui::GetIO ().KeyCtrl ? _ ( "Move:" ) : _ ( "Copy:" );
                if ( payload.count > 1 ) {
                    ImGui::Text ( "%s %d %s", verb, payload.count, _ ( "files" ) );
                } else {
                    ImGui::Text ( "%s %s.%s", verb, f->filename, f->extension );
                }
                ImGui::EndDragDropSource ();
            }

            /* přípona */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%s", f->extension );

            /* velikost */
            ImGui::TableNextColumn ();
            if ( f->size >= 1024 ) {
                ImGui::Text ( "%u KB", (unsigned) ( f->size / 1024 ) );
            } else {
                ImGui::Text ( "%u B", (unsigned) f->size );
            }

            /* atributy */
            ImGui::TableNextColumn ();
            char attrs[8];
            format_attrs ( f, attrs );
            if ( f->read_only || f->system ) {
                ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s", attrs );
            } else {
                ImGui::TextDisabled ( "%s", attrs );
            }

            /* extenty */
            ImGui::TableNextColumn ();
            ImGui::Text ( "%d", f->extent_count );
        }

        ImGui::EndTable ();
    }

    /* Drop target přes celou tabulku (BeginDragDropTargetCustom). */
    ImVec2 table_max = ImGui::GetCursorScreenPos ();
    float win_right = ImGui::GetWindowPos ().x + ImGui::GetWindowContentRegionMax ().x;
    if ( table_max.y > table_min.y ) {
        ImRect r ( table_min, ImVec2 ( win_right, table_max.y ) );
        dnd_try_accept_rect ( r, "##cpm_dnd_table" );
    }

    /* detail */
    if ( data->detail_index >= 0 && data->detail_index < data->file_count ) {
        st_MZDSK_CPM_FILE_INFO_EX *f = &data->files[data->detail_index];
        char attrs[8];
        format_attrs ( f, attrs );

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        ImGui::Text ( "%s: %s.%s", _ ( "File" ), f->filename, f->extension );
        ImGui::Text ( "%s: %d", _ ( "User" ), f->user );
        ImGui::Text ( "%s: %u B (%u KB)", _ ( "Size" ), (unsigned) f->size, (unsigned) ( f->size / 1024 ) );
        ImGui::Text ( "%s: %s", _ ( "Attributes" ), attrs );
        ImGui::Text ( "%s: %d", _ ( "Extents" ), f->extent_count );
    }
}


/* =========================================================================
 * Alloc Map tab - rendering
 * ========================================================================= */

/**
 * @brief Vykreslí DPB parametry jako tabulku.
 *
 * Zobrazuje primární a odvozené parametry Disk Parameter Blocku.
 *
 * @param dpb Disk Parameter Block.
 */
static void render_dpb_info ( const st_MZDSK_CPM_DPB *dpb, const char *preset_name )
{
    ImGui::SeparatorText ( _ ( "Disk Parameter Block (DPB)" ) );

    ImGui::Text ( "%s: %s", _ ( "Preset" ), preset_name );
    ImGui::Spacing ();

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                          | ImGuiTableFlags_SizingFixedFit;

    if ( ImGui::BeginTable ( "##dpb_table", 4, flags ) ) {
        ImGui::TableSetupColumn ( _ ( "Parameter" ), ImGuiTableColumnFlags_WidthFixed, 100.0f );
        ImGui::TableSetupColumn ( _ ( "Value" ),     ImGuiTableColumnFlags_WidthFixed, 80.0f );
        ImGui::TableSetupColumn ( _ ( "Parameter" ), ImGuiTableColumnFlags_WidthFixed, 100.0f );
        ImGui::TableSetupColumn ( _ ( "Value" ),     ImGuiTableColumnFlags_WidthFixed, 80.0f );
        ImGui::TableHeadersRow ();

        /* řádek 1: SPT, Block size */
        ImGui::TableNextRow ();
        ImGui::TableNextColumn (); ImGui::Text ( "SPT" );
        ImGui::TableNextColumn (); ImGui::Text ( "%u", dpb->spt );
        ImGui::TableNextColumn (); ImGui::Text ( "%s", _ ( "Block size" ) );
        ImGui::TableNextColumn (); ImGui::Text ( "%u B", dpb->block_size );

        /* řádek 2: BSH, DSM */
        ImGui::TableNextRow ();
        ImGui::TableNextColumn (); ImGui::Text ( "BSH" );
        ImGui::TableNextColumn (); ImGui::Text ( "%u", dpb->bsh );
        ImGui::TableNextColumn (); ImGui::Text ( "DSM" );
        ImGui::TableNextColumn (); ImGui::Text ( "%u", dpb->dsm );

        /* řádek 3: BLM, DRM */
        ImGui::TableNextRow ();
        ImGui::TableNextColumn (); ImGui::Text ( "BLM" );
        ImGui::TableNextColumn (); ImGui::Text ( "%u", dpb->blm );
        ImGui::TableNextColumn (); ImGui::Text ( "DRM" );
        ImGui::TableNextColumn (); ImGui::Text ( "%u", dpb->drm );

        /* řádek 4: EXM, AL0/AL1 */
        ImGui::TableNextRow ();
        ImGui::TableNextColumn (); ImGui::Text ( "EXM" );
        ImGui::TableNextColumn (); ImGui::Text ( "%u", dpb->exm );
        ImGui::TableNextColumn (); ImGui::Text ( "AL0 / AL1" );
        ImGui::TableNextColumn (); ImGui::Text ( "0x%02X / 0x%02X", dpb->al0, dpb->al1 );

        /* řádek 5: OFF, CKS */
        ImGui::TableNextRow ();
        ImGui::TableNextColumn (); ImGui::Text ( "OFF" );
        ImGui::TableNextColumn (); ImGui::Text ( "%u", dpb->off );
        ImGui::TableNextColumn (); ImGui::Text ( "CKS" );
        ImGui::TableNextColumn (); ImGui::Text ( "%u", dpb->cks );

        /* řádek 6: Kapacita, Dir bloky */
        ImGui::TableNextRow ();
        ImGui::TableNextColumn (); ImGui::Text ( "%s", _ ( "Capacity" ) );
        ImGui::TableNextColumn (); ImGui::Text ( "%u KB", ( (unsigned) dpb->dsm + 1 ) * dpb->block_size / 1024 );
        ImGui::TableNextColumn (); ImGui::Text ( "%s", _ ( "Dir blocks" ) );
        ImGui::TableNextColumn (); ImGui::Text ( "%d", count_dir_blocks ( dpb ) );

        ImGui::EndTable ();
    }
}


/**
 * @brief Vykreslí řádek se statistikou obsazení directory slotů.
 *
 * Zobrazí total (konfigurační počet z DRM+1), used (user 0-15),
 * free (user == 0xE5) a blocked (ostatní user bajty - BDOS je drží
 * jako obsazené, ale výpis je neukazuje).
 *
 * @param data Datový model s naplněnou dir_stats.
 */
static void render_dir_stats ( const st_PANEL_CPM_DATA *data )
{
    if ( !data->dir_stats_loaded ) return;

    const st_MZDSK_CPM_DIR_STATS *ds = &data->dir_stats;

    ImGui::SeparatorText ( _ ( "Directory slots" ) );
    ImGui::Text ( "%s: %u", _ ( "Total" ), ds->total );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %u", _ ( "Used" ), ds->used );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %u", _ ( "Free" ), ds->free );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %u", _ ( "Blocked" ), ds->blocked );
    if ( ImGui::IsItemHovered () ) {
        ImGui::SetTooltip ( "%s", _ (
            "Allocated directory slots with user > 15 (outside the\n"
            "displayable CP/M 2.2 user areas 0-15). BDOS treats them\n"
            "as occupied but the SEARCH loop matches only the active\n"
            "USRCODE (0-15), so they never appear in directory\n"
            "listings." ) );
    }
}


/**
 * @brief Vykreslí řádek se statistikami alloc mapy.
 *
 * @param data Datový model s alloc_map.
 */
static void render_alloc_stats ( const st_PANEL_CPM_DATA *data )
{
    const st_MZDSK_CPM_ALLOC_MAP *am = &data->alloc_map;
    int dir_blocks = count_dir_blocks ( &data->dpb );

    ImGui::SeparatorText ( _ ( "Blocks" ) );
    ImGui::Text ( "%s: %u", _ ( "Total" ), am->total_blocks );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %u (%u KB)", _ ( "Used" ), am->used_blocks,
                  (unsigned) am->used_blocks * data->dpb.block_size / 1024 );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %u (%u KB)", _ ( "Free" ), am->free_blocks,
                  (unsigned) am->free_blocks * data->dpb.block_size / 1024 );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d", _ ( "Dir" ), dir_blocks );
}


/**
 * @brief Vykreslí disk layout pruh pro CP/M.
 *
 * Zobrazuje proporcionální segmenty: Boot Track, System Tracks, Dir, File Data, Free.
 *
 * @param data Datový model.
 */
static void render_alloc_layout ( const st_PANEL_CPM_DATA *data )
{
    ImGui::SeparatorText ( _ ( "Disk Layout" ) );

    /* definice segmentů */
    struct {
        const char *label;
        uint32_t blocks;
        ImU32 color;
    } segs[5];
    int seg_count = 0;

    int dir_blocks = count_dir_blocks ( &data->dpb );
    uint16_t total = data->alloc_map.total_blocks;
    uint16_t file_blocks = data->alloc_map.used_blocks - (uint16_t) dir_blocks;

    /* Boot + System tracks (OFF stop = nepoužité CP/M) */
    if ( data->dpb.off > 0 ) {
        segs[seg_count].label = _ ( "System Tracks" );
        segs[seg_count].blocks = data->dpb.off;
        segs[seg_count].color = COL_SYSTEM;
        seg_count++;
    }

    /* Directory */
    segs[seg_count].label = "Dir";
    segs[seg_count].blocks = (uint32_t) dir_blocks;
    segs[seg_count].color = COL_DIR;
    seg_count++;

    /* File Data */
    if ( file_blocks > 0 ) {
        segs[seg_count].label = _ ( "File Data" );
        segs[seg_count].blocks = file_blocks;
        segs[seg_count].color = IM_COL32 ( 76, 175, 80, 255 ); /* zelená */
        seg_count++;
    }

    /* Free */
    if ( data->alloc_map.free_blocks > 0 ) {
        segs[seg_count].label = _ ( "Free" );
        segs[seg_count].blocks = data->alloc_map.free_blocks;
        segs[seg_count].color = COL_FREE;
        seg_count++;
    }

    /* celkový počet pro proporce (bloky + system tracks jako "logické bloky") */
    uint32_t bar_total = total + data->dpb.off;

    /* dvouprůchodový výpočet šířek */
    ImVec2 avail = ImGui::GetContentRegionAvail ();
    float bar_width = avail.x;
    if ( bar_width < 100.0f ) bar_width = 100.0f;

    float widths[5];
    float reserved_width = 0.0f;
    float remaining_blocks = 0.0f;

    for ( int i = 0; i < seg_count; i++ ) {
        float w = ( (float) segs[i].blocks / (float) bar_total ) * bar_width;
        if ( w < ALLOC_LAYOUT_MIN_SEGMENT_WIDTH ) {
            widths[i] = ALLOC_LAYOUT_MIN_SEGMENT_WIDTH;
            reserved_width += ALLOC_LAYOUT_MIN_SEGMENT_WIDTH;
        } else {
            widths[i] = 0.0f;
            remaining_blocks += (float) segs[i].blocks;
        }
    }

    float remaining_width = bar_width - reserved_width;
    if ( remaining_width < 0.0f ) remaining_width = 0.0f;

    for ( int i = 0; i < seg_count; i++ ) {
        if ( widths[i] == 0.0f && remaining_blocks > 0.0f ) {
            widths[i] = ( (float) segs[i].blocks / remaining_blocks ) * remaining_width;
        }
    }

    /* vykreslení pruhu */
    ImVec2 origin = ImGui::GetCursorScreenPos ();
    ImDrawList *dl = ImGui::GetWindowDrawList ();
    ImVec2 mouse = ImGui::GetMousePos ();
    int hovered_seg = -1;
    float x = origin.x;

    for ( int i = 0; i < seg_count; i++ ) {
        float w = widths[i];
        ImVec2 p0 = ImVec2 ( x, origin.y );
        ImVec2 p1 = ImVec2 ( x + w, origin.y + ALLOC_LAYOUT_BAR_HEIGHT );

        dl->AddRectFilled ( p0, p1, segs[i].color );

        if ( i > 0 ) {
            dl->AddLine ( ImVec2 ( x, origin.y ), ImVec2 ( x, origin.y + ALLOC_LAYOUT_BAR_HEIGHT ),
                          IM_COL32 ( 20, 20, 20, 255 ), 1.0f );
        }

        ImVec2 text_size = ImGui::CalcTextSize ( segs[i].label );
        if ( text_size.x + 4.0f < w ) {
            float tx = x + ( w - text_size.x ) * 0.5f;
            float ty = origin.y + ( ALLOC_LAYOUT_BAR_HEIGHT - text_size.y ) * 0.5f;
            dl->AddText ( ImVec2 ( tx, ty ), IM_COL32 ( 255, 255, 255, 220 ), segs[i].label );
        }

        if ( mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y ) {
            hovered_seg = i;
            dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
        }

        x += w;
    }

    dl->AddRect ( origin, ImVec2 ( origin.x + bar_width, origin.y + ALLOC_LAYOUT_BAR_HEIGHT ),
                  IM_COL32 ( 80, 80, 80, 255 ) );

    ImGui::Dummy ( ImVec2 ( bar_width, ALLOC_LAYOUT_BAR_HEIGHT ) );

    if ( hovered_seg >= 0 ) {
        ImGui::BeginTooltip ();
        ImGui::Text ( "%s", segs[hovered_seg].label );
        ImGui::Text ( "%u %s (%u KB)", segs[hovered_seg].blocks,
                      _ ( "blocks" ), (unsigned) segs[hovered_seg].blocks * data->dpb.block_size / 1024 );
        ImGui::EndTooltip ();
    }

    /* legenda pod pruhem */
    ImGui::Spacing ();
    for ( int i = 0; i < seg_count; i++ ) {
        ImVec2 pos = ImGui::GetCursorScreenPos ();
        dl = ImGui::GetWindowDrawList ();
        dl->AddRectFilled ( pos, ImVec2 ( pos.x + 16, pos.y + 16 ), segs[i].color );
        ImGui::Dummy ( ImVec2 ( 18, 18 ) );
        ImGui::SameLine ();
        ImGui::Text ( "%s: %u (%.1f%%)", segs[i].label, segs[i].blocks,
                      100.0f * (float) segs[i].blocks / (float) bar_total );
        ImGui::SameLine ( 0, 20 );
    }
    ImGui::NewLine ();
}


/**
 * @brief Vykreslí legendu souborů a strukturálních typů.
 *
 * Pro každý soubor zobrazí barevný čtvereček s "NAME.EXT (N blk)".
 * Pod soubory zobrazí strukturální typy (Dir, Free).
 *
 * @param data Datový model.
 */
static void render_alloc_legend ( const st_PANEL_CPM_DATA *data )
{
    ImGui::SeparatorText ( _ ( "Legend" ) );

    ImDrawList *dl = ImGui::GetWindowDrawList ();
    g_cpm_legend_sel = -1;

    /* soubory jako klikací linky */
    for ( int i = 0; i < data->file_count; i++ ) {
        const st_MZDSK_CPM_FILE_INFO_EX *f = &data->files[i];

        /* spočítat bloky tohoto souboru */
        int blk_count = 0;
        uint16_t total = data->alloc_map.total_blocks;
        for ( uint16_t b = 0; b < total; b++ ) {
            if ( data->block_owner[b] == (uint16_t) i ) blk_count++;
        }

        ImVec2 pos = ImGui::GetCursorScreenPos ();
        dl = ImGui::GetWindowDrawList ();
        ImU32 col = file_index_color ( i );

        dl->AddRectFilled ( pos, ImVec2 ( pos.x + 16, pos.y + 16 ), col );
        ImGui::Dummy ( ImVec2 ( 18, 18 ) );
        ImGui::SameLine ();

        /* jméno souboru jako link */
        char fn[16];
        format_fullname ( f, fn, sizeof ( fn ) );
        char label[32];
        snprintf ( label, sizeof ( label ), "%s (%d blk)", fn, blk_count );

        ImVec4 link_col = ImVec4 ( 0.4f, 0.7f, 1.0f, 1.0f );
        ImGui::TextColored ( link_col, "%s", label );

        bool hovered = ImGui::IsItemHovered ();
        bool held = hovered && ImGui::IsMouseDown ( 0 );

        /* podtržení při hoveru */
        if ( hovered ) {
            ImVec2 tmin = ImGui::GetItemRectMin ();
            ImVec2 tmax = ImGui::GetItemRectMax ();
            dl->AddLine ( ImVec2 ( tmin.x, tmax.y ), tmax,
                          ImGui::GetColorU32 ( link_col ) );
        }

        /* stisknuté tlačítko - zvýraznit bloky */
        if ( held ) {
            g_cpm_legend_sel = i;
            dl->AddRect ( pos, ImVec2 ( pos.x + 16, pos.y + 16 ),
                          IM_COL32 ( 255, 255, 255, 255 ), 0.0f, 0, 2.0f );
        }

        ImGui::SameLine ( 0, 16 );
    }

    if ( data->file_count > 0 ) ImGui::NewLine ();

    /* strukturální typy */
    int dir_blocks = count_dir_blocks ( &data->dpb );

    struct { const char *label; ImU32 color; int count; } types[] = {
        { "Dir",          COL_DIR,  dir_blocks },
        { _ ( "Free" ),   COL_FREE, (int) data->alloc_map.free_blocks },
    };

    for ( int i = 0; i < 2; i++ ) {
        if ( types[i].count == 0 ) continue;
        ImVec2 pos = ImGui::GetCursorScreenPos ();
        dl = ImGui::GetWindowDrawList ();
        dl->AddRectFilled ( pos, ImVec2 ( pos.x + 16, pos.y + 16 ), types[i].color );
        ImGui::Dummy ( ImVec2 ( 18, 18 ) );
        ImGui::SameLine ();
        ImGui::Text ( "%s: %d", types[i].label, types[i].count );
        ImGui::SameLine ( 0, 16 );
    }
    ImGui::NewLine ();
}


/**
 * @brief Vykreslí blokovou mřížku alloc mapy s per-file barvami a hover tooltipem.
 *
 * Každý blok je malý obdélník obarvený podle vlastníka.
 * Hover tooltip zobrazuje: blok#, typ (Dir/Free/soubor), jméno souboru.
 *
 * @param data Datový model s block_owner[] a alloc_map.
 */
static void render_alloc_grid ( const st_PANEL_CPM_DATA *data )
{
    ImGui::SeparatorText ( _ ( "CP/M Block Map" ) );

    uint16_t total = data->alloc_map.total_blocks;

    ImVec2 avail = ImGui::GetContentRegionAvail ();
    if ( !ImGui::BeginChild ( "##cpm_alloc_grid", avail, ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar ) ) {
        ImGui::EndChild ();
        return;
    }

    avail = ImGui::GetContentRegionAvail ();
    float step = ALLOC_CELL_SIZE + ALLOC_CELL_GAP;
    int cols = (int) ( avail.x / step );
    if ( cols < 1 ) cols = 1;

    ImVec2 origin = ImGui::GetCursorScreenPos ();
    ImDrawList *dl = ImGui::GetWindowDrawList ();
    ImVec2 mouse = ImGui::GetMousePos ();
    int hovered_block = -1;

    for ( int i = 0; i < total; i++ ) {
        int col = i % cols;
        int row = i / cols;

        ImVec2 p0 = ImVec2 ( origin.x + col * step, origin.y + row * step );
        ImVec2 p1 = ImVec2 ( p0.x + ALLOC_CELL_SIZE, p0.y + ALLOC_CELL_SIZE );

        ImU32 color = block_color ( data->block_owner[i] );

        /* ztlumit bloky nepatřící zvýrazněnému souboru */
        if ( g_cpm_legend_sel >= 0 ) {
            if ( data->block_owner[i] == (uint16_t) g_cpm_legend_sel ) {
                dl->AddRectFilled ( p0, p1, color );
                dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
            } else {
                dl->AddRectFilled ( p0, p1, ( color & 0x00FFFFFF ) | 0x40000000 );
            }
        } else {
            dl->AddRectFilled ( p0, p1, color );
        }

        if ( mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y ) {
            hovered_block = i;
            if ( g_cpm_legend_sel < 0 ) {
                dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
            }
        }
    }

    int total_rows = ( total + cols - 1 ) / cols;
    ImGui::Dummy ( ImVec2 ( avail.x, total_rows * step ) );

    /* tooltip */
    if ( hovered_block >= 0 ) {
        uint16_t owner = data->block_owner[hovered_block];

        ImGui::BeginTooltip ();
        ImGui::Text ( "Block %d", hovered_block );

        if ( owner == PANEL_CPM_BLOCK_DIR ) {
            ImGui::Text ( "Directory" );
        } else if ( owner == PANEL_CPM_BLOCK_FREE ) {
            ImGui::Text ( "%s", _ ( "Free" ) );
        } else if ( owner < data->file_count ) {
            char fn[16];
            format_fullname ( &data->files[owner], fn, sizeof ( fn ) );
            ImGui::Text ( "%s: %s (user %d)", _ ( "File" ), fn, data->files[owner].user );
        }

        ImGui::EndTooltip ();
    }

    ImGui::EndChild ();
}


/**
 * @brief Vykreslí obsah Alloc Map tabu.
 *
 * Zobrazuje DPB parametry, statistiky, disk layout pruh,
 * legendu per-file + strukturální a blokovou mřížku.
 *
 * @param data Datový model.
 */
static void render_alloc_map_tab ( const st_PANEL_CPM_DATA *data )
{
    if ( !data->alloc_loaded ) {
        ImGui::TextDisabled ( "%s", _ ( "Allocation map not available." ) );
        return;
    }

    render_dpb_info ( &data->dpb, data->preset_name );
    ImGui::Spacing ();
    render_alloc_stats ( data );
    ImGui::Spacing ();
    render_dir_stats ( data );
    ImGui::Spacing ();
    render_alloc_layout ( data );
    ImGui::Spacing ();
    render_alloc_legend ( data );
    ImGui::Spacing ();
    render_alloc_grid ( data );
}


/* =========================================================================
 * Bulk Get / Get MZF export s podporou ASK módu
 * ========================================================================= */

/**
 * @brief Provede skutečný zápis jednoho souboru v bulk exportu.
 *
 * Volá se až po vyřešení kolize (dirpath + fname + dedup → filepath).
 * Po chybě připojí hlášku do bulk->errors. Úspěchy/neúspěchy inkrementuje
 * v bulk->ok_count / bulk->failed_count.
 */
static void bulk_get_do_export ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                                  int file_idx, const char *filepath )
{
    st_PANEL_CPM_BULK_GET *b = &data->bulk_get;
    const st_MZDSK_CPM_FILE_INFO_EX *file = &data->files[ file_idx ];

    en_MZDSK_RES gr;
    if ( b->is_mzf ) {
        gr = panel_cpm_get_file_mzf_ex ( disc, &data->dpb, file, filepath, &b->mzf_opts );
    } else if ( b->shared_buf ) {
        gr = panel_cpm_get_file_with_buffer ( disc, &data->dpb, file,
                                                filepath, b->shared_buf, b->shared_buf_size );
    } else {
        gr = panel_cpm_get_file ( disc, &data->dpb, file, filepath );
    }

    if ( gr == MZDSK_RES_OK ) {
        b->ok_count++;
    } else {
        b->failed_count++;
        char fname[16];
        format_fullname ( file, fname, sizeof ( fname ) );
        const char *suffix = b->is_mzf ? ".mzf" : "";
        int n = snprintf ( b->errors + b->err_len, sizeof ( b->errors ) - b->err_len,
                           "%s%s: %s\n", fname, suffix, mzdsk_get_error ( gr ) );
        if ( n > 0 ) b->err_len += n;
    }
}


/**
 * @brief Pokračuje v bulk Get exportu dokud nenarazí na ASK konflikt nebo nedokončí.
 *
 * Iteruje přes `data->selected[]` od `bulk_get.next_idx`. Pro každý vybraný
 * soubor sestaví cílovou cestu a vyřeší kolizi dle `override_mode`
 * (priorita) nebo `cfg->export_dup_mode`. Při ASK + kolizi nastaví
 * `ask_pending` a vrátí se - render pak zobrazí popup a po rozhodnutí
 * zavolá tuto funkci znovu.
 *
 * @return true pokud bulk je dokončený; false pokud čeká na ASK.
 */
static bool bulk_get_resume ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                               st_MZDISK_CONFIG *cfg )
{
    st_PANEL_CPM_BULK_GET *b = &data->bulk_get;

    for ( int i = b->next_idx; i < data->file_count; i++ ) {
        if ( !data->selected[i] ) continue;

        char fname[16];
        format_fullname ( &data->files[i], fname, sizeof ( fname ) );
        /* filepath > dirpath (2048) + fname (16) + ".mzf" (4) + 2; větší
           buffer potlačuje -Wformat-truncation warning a zabrání ořezu. */
        char filepath[2048 + 32];
        snprintf ( filepath, sizeof ( filepath ), "%s/%s%s",
                   b->dirpath, fname, b->is_mzf ? ".mzf" : "" );

        int effective_mode = ( b->override_mode >= 0 )
                           ? b->override_mode : cfg->export_dup_mode;

        if ( effective_mode == MZDSK_EXPORT_DUP_ASK ) {
            /* ASK: pokud cíl existuje, uložit stav a vyskočit. Render
               zobrazí popup "File already exists: X" s tlačítky
               Overwrite/Rename/Skip + Apply to all. */
            FILE *f = fopen ( filepath, "rb" );
            if ( f ) {
                fclose ( f );
                b->conflict_idx = i;
                strncpy ( b->conflict_path, filepath, sizeof ( b->conflict_path ) - 1 );
                b->conflict_path[ sizeof ( b->conflict_path ) - 1 ] = '\0';
                b->ask_pending = true;
                b->next_idx = i; /* popup tento soubor ještě zpracuje */
                return false;
            }
            /* Cíl neexistuje - export přímo. */
            bulk_get_do_export ( data, disc, i, filepath );
        } else {
            /* Non-ASK: standardní resolve (rename / skip / overwrite). */
            int r = mzdisk_config_resolve_export_dup ( filepath, sizeof ( filepath ), effective_mode );
            if ( r == 0 ) {
                bulk_get_do_export ( data, disc, i, filepath );
            } else if ( r == 1 ) {
                /* skip - no op */
            } else {
                /* Chyba resolve (buffer overflow při rename) - zaznamenat. */
                b->failed_count++;
                int n = snprintf ( b->errors + b->err_len, sizeof ( b->errors ) - b->err_len,
                                   "%s: dup resolve failed\n", fname );
                if ( n > 0 ) b->err_len += n;
            }
        }
    }

    b->next_idx = data->file_count;
    return true;
}


/**
 * @brief Dokončí bulk export - sestaví summary do data->error_msg a uvolní shared_buf.
 */
static void bulk_get_finish ( st_PANEL_CPM_DATA *data )
{
    st_PANEL_CPM_BULK_GET *b = &data->bulk_get;
    if ( b->failed_count > 0 ) {
        /* Summary buffer je záměrně větší, aby se do něj bez warningu
           vešel prefix + plná errors[2048]. Finální strncpy do
           data->error_msg[2048] případný přesah ořízne. */
        char summary[ sizeof ( data->error_msg ) + 64 ];
        snprintf ( summary, sizeof ( summary ),
                   "%s: %d OK, %d failed:\n%s",
                   b->is_mzf ? _ ( "Export MZF" ) : _ ( "Export" ),
                   b->ok_count, b->failed_count, b->errors );
        strncpy ( data->error_msg, summary, sizeof ( data->error_msg ) - 1 );
        data->error_msg[ sizeof ( data->error_msg ) - 1 ] = '\0';
        data->has_error = true;
        fprintf ( stderr, "CP/M %s: %d file(s) OK, %d failed:\n%s",
                  b->is_mzf ? "Get MZF" : "Get",
                  b->ok_count, b->failed_count, b->errors );
    }
    free ( b->shared_buf );
    b->shared_buf = NULL;
    b->active = false;
}


/**
 * @brief Inicializuje bulk Get export a spustí první běh.
 *
 * @param is_mzf false = raw bulk, true = MZF bulk.
 * @param dirpath Cílový adresář.
 * @param mzf_opts NULL pro raw; jinak kopie MZF options.
 */
static void bulk_get_start ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                              st_MZDISK_CONFIG *cfg, bool is_mzf,
                              const char *dirpath,
                              const st_PANEL_CPM_MZF_EXPORT *mzf_opts )
{
    st_PANEL_CPM_BULK_GET *b = &data->bulk_get;
    memset ( b, 0, sizeof ( *b ) );
    b->active = true;
    b->is_mzf = is_mzf;
    b->override_mode = -1;
    strncpy ( b->dirpath, dirpath, sizeof ( b->dirpath ) - 1 );
    b->dirpath[ sizeof ( b->dirpath ) - 1 ] = '\0';

    if ( is_mzf && mzf_opts ) {
        b->mzf_opts = *mzf_opts;
    } else if ( !is_mzf ) {
        /* Raw: sdílený buffer (Audit M-36). Fallback na per-soubor malloc
           pokud alokace selže - bulk_get_do_export si vybere automaticky. */
        b->shared_buf_size = panel_cpm_get_file_buffer_size ( &data->dpb );
        b->shared_buf = b->shared_buf_size > 0
                      ? (uint8_t *) malloc ( b->shared_buf_size )
                      : NULL;
    }

    if ( bulk_get_resume ( data, disc, cfg ) ) {
        bulk_get_finish ( data );
    }
}


/**
 * @brief Vykreslí ASK popup pro bulk Get a po rozhodnutí resume/finish.
 *
 * POZOR: `BeginPopupModal` musí být volán každý snímek, dokud je popup
 * otevřen, jinak ImGui popup zavře (viz docs). Proto je struktura
 * shodná s ostatními modálními popupy (##cpm_error apod.): `OpenPopup`
 * je podmíněné (jen první frame po nastavení `ask_pending`), ale
 * `BeginPopupModal` je nepodmíněné.
 */
static void render_bulk_get_ask_popup ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                                         st_MZDISK_CONFIG *cfg )
{
    st_PANEL_CPM_BULK_GET *b = &data->bulk_get;

    if ( b->ask_pending ) {
        ImGui::OpenPopup ( "##cpm_bulk_get_ask" );
        b->ask_pending = false;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##cpm_bulk_get_ask", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "File already exists:" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", b->conflict_path );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        /* Per-file akce. */
        int action = -1;          /* -1 = none, 0 = overwrite, 1 = rename, 2 = skip */
        bool apply_all = false;
        bool cancel = false;

        if ( ButtonMinWidth ( _ ( "Overwrite" ) ) )           { action = 0; }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Rename" ) ) )              { action = 1; }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Skip" ) ) )                { action = 2; }

        ImGui::Spacing ();
        ImGui::TextDisabled ( "%s", _ ( "Apply to all remaining:" ) );
        if ( ButtonMinWidth ( _ ( "Overwrite all" ) ) )       { action = 0; apply_all = true; }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Rename all" ) ) )          { action = 1; apply_all = true; }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Skip all" ) ) )            { action = 2; apply_all = true; }

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) )              { cancel = true; }

        if ( action >= 0 || cancel ) {
            int idx = b->conflict_idx;
            if ( !cancel ) {
                char filepath[2048];
                strncpy ( filepath, b->conflict_path, sizeof ( filepath ) - 1 );
                filepath[ sizeof ( filepath ) - 1 ] = '\0';

                if ( action == 0 ) {
                    /* Overwrite: použít conflict_path jak je. */
                    bulk_get_do_export ( data, disc, idx, filepath );
                } else if ( action == 1 ) {
                    /* Rename ~N pomocí resolve_export_dup. */
                    if ( mzdisk_config_resolve_export_dup ( filepath, sizeof ( filepath ),
                                                             MZDSK_EXPORT_DUP_RENAME ) == 0 ) {
                        bulk_get_do_export ( data, disc, idx, filepath );
                    } else {
                        b->failed_count++;
                        char fname[16];
                        format_fullname ( &data->files[idx], fname, sizeof ( fname ) );
                        int n = snprintf ( b->errors + b->err_len, sizeof ( b->errors ) - b->err_len,
                                           "%s: rename failed\n", fname );
                        if ( n > 0 ) b->err_len += n;
                    }
                } else {
                    /* Skip - nic. */
                }

                if ( apply_all ) {
                    b->override_mode = ( action == 0 ) ? MZDSK_EXPORT_DUP_OVERWRITE
                                     : ( action == 1 ) ? MZDSK_EXPORT_DUP_RENAME
                                                       : MZDSK_EXPORT_DUP_SKIP;
                }

                b->next_idx = idx + 1;
                if ( bulk_get_resume ( data, disc, cfg ) ) {
                    bulk_get_finish ( data );
                }
            } else {
                /* Cancel: ukončit bulk bez dalšího zpracování. */
                bulk_get_finish ( data );
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
}


/* =========================================================================
 * Popupy a dialogy (sdílené)
 * ========================================================================= */

/**
 * @brief Zpracuje file dialogy.
 */
static void process_dialogs ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                              bool *is_dirty, st_MZDISK_CONFIG *cfg,
                              st_MZDSK_DETECT_RESULT *detect )
{
    /* Get dialog */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( CPM_GET_DLG, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            int sel_count = count_selected ( data );

            /* Kolekce chyb: dřívější multi-select tiše ignoroval
             * návratovky `panel_cpm_get_file` → tichá ztráta dat při
             * exportu. Audit M-38. */
            char get_errors[1024] = "";
            int get_err_len = 0;
            int get_ok = 0, get_failed = 0;

            if ( sel_count == 1 ) {
                std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
                for ( int i = 0; i < data->file_count; i++ ) {
                    if ( data->selected[i] ) {
                        en_MZDSK_RES gr = panel_cpm_get_file ( disc, &data->dpb, &data->files[i], path.c_str () );
                        if ( gr == MZDSK_RES_OK ) get_ok++;
                        else {
                            get_failed++;
                            char fname[16];
                            format_fullname ( &data->files[i], fname, sizeof ( fname ) );
                            int n = snprintf ( get_errors + get_err_len, sizeof ( get_errors ) - get_err_len,
                                               "%s: %s\n", fname, mzdsk_get_error ( gr ) );
                            if ( n > 0 ) get_err_len += n;
                        }
                        break;
                    }
                }
            } else {
                /* Bulk raw Get - resumovatelný přes ASK popup, pokud je
                   cfg->export_dup_mode == ASK. Viz bulk_get_* helpery. */
                std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();
                bulk_get_start ( data, disc, cfg, /*is_mzf=*/false,
                                 dirpath.c_str (), /*mzf_opts=*/nullptr );
                /* bulk_get_start už sám aplikuje get_errors logiku do
                   data->error_msg + has_error (přes bulk_get_finish). */
                (void) get_errors; (void) get_err_len; (void) get_ok; (void) get_failed;
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_get_dir, dir.c_str (), sizeof ( cfg->last_get_dir ) - 1 );
            cfg->last_get_dir[sizeof ( cfg->last_get_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* Put dialog - po vybrání jednoho souboru otevře "Put file as..." popup
       pro striktní validaci jména. Multi-select použije tolerantní cestu
       (stejný vzor jako dřív - parse_cpm_name + panel_cpm_put_file). */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( CPM_PUT_DLG, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            auto selection = ImGuiFileDialog::Instance ()->GetSelection ();

            if ( selection.size () == 1 ) {
                /* Jediný soubor -> otevřít dialog s validací. */
                auto it = selection.begin ();
                strncpy ( g_cpm_put_src_path, it->second.c_str (),
                          sizeof ( g_cpm_put_src_path ) - 1 );
                g_cpm_put_src_path[sizeof ( g_cpm_put_src_path ) - 1] = '\0';

                /* Předvyplnit target name z basename souboru. */
                const char *bname = strrchr ( it->first.c_str (), '/' );
                if ( !bname ) bname = strrchr ( it->first.c_str (), '\\' );
                if ( bname ) bname++; else bname = it->first.c_str ();
                strncpy ( g_cpm_put_target_buf, bname,
                          sizeof ( g_cpm_put_target_buf ) - 1 );
                g_cpm_put_target_buf[sizeof ( g_cpm_put_target_buf ) - 1] = '\0';

                g_cpm_put_is_mzf = false;
                g_cpm_put_auto_truncate = false;
                /* Výchozí target user: pokud filter je konkrétní, použij ho;
                   jinak (all) předvyplň 0. */
                g_cpm_put_target_user = ( data->filter_user >= 0 )
                                        ? data->filter_user : 0;
                g_cpm_put_pending_open = true;
            } else {
                /* Multi-select -> tolerantní bulk put. */
                bool any_ok = false;
                char errors[512] = "";
                int err_len = 0;

                for ( auto &sel : selection ) {
                    const char *bname = strrchr ( sel.first.c_str (), '/' );
                    if ( !bname ) bname = strrchr ( sel.first.c_str (), '\\' );
                    if ( bname ) bname++; else bname = sel.first.c_str ();

                    char name[9], ext[4];
                    if ( !parse_cpm_name ( bname, name, ext ) ) {
                        int n = snprintf ( errors + err_len, sizeof ( errors ) - err_len,
                                           "%s: Invalid CP/M name\n", bname );
                        if ( n > 0 ) err_len += n;
                        continue;
                    }

                    /* Multi-file put (Import batch) jde vždy do user oblasti
                       určené filtrem - při filter=all (-1) do user 0. */
                    uint8_t batch_user = ( data->filter_user >= 0 )
                                         ? (uint8_t) data->filter_user : 0;
                    en_MZDSK_RES res = panel_cpm_put_file ( disc, &data->dpb,
                        sel.second.c_str (), name, ext, batch_user );
                    if ( res == MZDSK_RES_OK ) {
                        any_ok = true;
                    } else {
                        int n = snprintf ( errors + err_len, sizeof ( errors ) - err_len,
                                           "%s: %s\n", bname, mzdsk_get_error ( res ) );
                        if ( n > 0 ) err_len += n;
                    }
                }

                if ( any_ok ) {
                    *is_dirty = true;
                    panel_cpm_load ( data, disc, detect );
                }
                if ( err_len > 0 ) {
                    data->has_error = true;
                    strncpy ( data->error_msg, errors, sizeof ( data->error_msg ) - 1 );
                    data->error_msg[sizeof ( data->error_msg ) - 1] = '\0';
                }
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_put_dir, dir.c_str (), sizeof ( cfg->last_put_dir ) - 1 );
            cfg->last_put_dir[sizeof ( cfg->last_put_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* MZF Export Options popup */
    if ( data->mzf_export.show_opts ) {
        ImGui::OpenPopup ( _ ( "MZF Export Options" ) );
        data->mzf_export.show_opts = false;
    }

    {
        ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
        ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
        ImGui::SetNextWindowSize ( ImVec2 ( 440, 0 ), ImGuiCond_Appearing );
    }
    ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 16, 12 ) );
    if ( ImGui::BeginPopupModal ( _ ( "MZF Export Options" ), nullptr,
                                   ImGuiWindowFlags_NoResize ) ) {

        ImGui::Spacing ();

        float lbl_w = 180.0f;
        float inp_w = 80.0f;

        /* File type - hex vstup (00-FF) */
        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "%s:", _ ( "File type" ) );
        ImGui::SameLine ( lbl_w );
        ImGui::SetNextItemWidth ( inp_w );
        {
            char hex_buf[3]; /* 2 hex + NUL */
            std::snprintf ( hex_buf, sizeof ( hex_buf ), "%02X", (unsigned) data->mzf_export.ftype & 0xFF );
            if ( ImGui::InputText ( "##mzf_ftype_hex", hex_buf, sizeof ( hex_buf ),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                unsigned val = 0;
                if ( sscanf ( hex_buf, "%x", &val ) == 1 && val <= 0xFF ) {
                    data->mzf_export.ftype = (int) val;
                }
            }
        }
        ImGui::SameLine ( 0, 10 );
        ImGui::TextDisabled ( "(0x22 default)" );

        /* Exec address - hex vstup (max 4 hex číslic) */
        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "%s:", _ ( "Exec address" ) );
        ImGui::SameLine ( lbl_w );
        ImGui::SetNextItemWidth ( inp_w );
        {
            char hex_buf[5]; /* 4 hex + NUL */
            std::snprintf ( hex_buf, sizeof ( hex_buf ), "%04X", (unsigned) data->mzf_export.exec_addr & 0xFFFF );
            if ( ImGui::InputText ( "##mzf_exec", hex_buf, sizeof ( hex_buf ),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                unsigned val = 0;
                if ( sscanf ( hex_buf, "%x", &val ) == 1 && val <= 0xFFFF ) {
                    data->mzf_export.exec_addr = (int) val;
                }
            }
        }

        /* Load address - hex vstup, platí pro všechny ftype */
        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "%s:", _ ( "Load address" ) );
        ImGui::SameLine ( lbl_w );
        ImGui::SetNextItemWidth ( inp_w );
        {
            char hex_buf[5]; /* 4 hex + NUL */
            std::snprintf ( hex_buf, sizeof ( hex_buf ), "%04X", (unsigned) data->mzf_export.strt_addr & 0xFFFF );
            if ( ImGui::InputText ( "##mzf_strt", hex_buf, sizeof ( hex_buf ),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                unsigned val = 0;
                if ( sscanf ( hex_buf, "%x", &val ) == 1 && val <= 0xFFFF ) {
                    data->mzf_export.strt_addr = (int) val;
                }
            }
        }

        /* Encode attrs - jen pro ftype 0x22 (konvence SOKODI CMT.COM) */
        bool is_cpm_ic = ( data->mzf_export.ftype == 0x22 );
        if ( !is_cpm_ic ) ImGui::BeginDisabled ();
        ImGui::Checkbox ( _ ( "Encode CP/M attrs" ), &data->mzf_export.encode_attrs );
        if ( !is_cpm_ic ) ImGui::EndDisabled ();
        ImGui::SameLine ( 0, 10 );
        ImGui::TextDisabled ( "(0x22 only)" );

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Export.." ) ) ) {
            data->mzf_export.open_file_dlg = true;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
    ImGui::PopStyleVar (); /* WindowPadding */

    /* Po potvrzení options otevřít file dialog.
       Single-select: file picker s předvyplněným jménem a ConfirmOverwrite.
       Multi-select: directory picker - do zvoleného adresáře se bulk uloží
       `<fname>.mzf` pro každý vybraný soubor. */
    if ( data->mzf_export.open_file_dlg ) {
        data->mzf_export.open_file_dlg = false;
        int mzf_sel_count = count_selected ( data );
        IGFD::FileDialogConfig config;
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles;
        config.path = cfg->last_get_dir;
        if ( mzf_sel_count == 1 ) {
            config.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
            for ( int i = 0; i < data->file_count; i++ ) {
                if ( data->selected[i] ) {
                    char fname[16];
                    format_fullname ( &data->files[i], fname, sizeof ( fname ) );
                    char mzfname[20];
                    std::snprintf ( mzfname, sizeof ( mzfname ), "%s.mzf", fname );
                    config.fileName = mzfname;
                    break;
                }
            }
            ImGuiFileDialog::Instance ()->OpenDialog ( CPM_GET_MZF_DLG, _ ( "Export to MZF" ), ".mzf", config );
        } else {
            ImGuiFileDialog::Instance ()->OpenDialog ( CPM_GET_MZF_DLG, _ ( "Export MZFs to directory" ), nullptr, config );
        }
    }

    /* Get MZF file dialog.
       Single: file picker → přímý export do zvoleného souboru.
       Multi:  directory picker → bulk_get_start (s ASK podporou). */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( CPM_GET_MZF_DLG, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            int mzf_sel_count = count_selected ( data );

            if ( mzf_sel_count == 1 ) {
                std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
                for ( int i = 0; i < data->file_count; i++ ) {
                    if ( data->selected[i] ) {
                        en_MZDSK_RES gr = panel_cpm_get_file_mzf_ex ( disc, &data->dpb, &data->files[i],
                                                                       path.c_str (), &data->mzf_export );
                        if ( gr != MZDSK_RES_OK ) {
                            char fname[16];
                            format_fullname ( &data->files[i], fname, sizeof ( fname ) );
                            snprintf ( data->error_msg, sizeof ( data->error_msg ),
                                       "%s.mzf: %s", fname, mzdsk_get_error ( gr ) );
                            data->has_error = true;
                        }
                        break;
                    }
                }
            } else {
                std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();
                bulk_get_start ( data, disc, cfg, /*is_mzf=*/true,
                                 dirpath.c_str (), &data->mzf_export );
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_get_dir, dir.c_str (), sizeof ( cfg->last_get_dir ) - 1 );
            cfg->last_get_dir[sizeof ( cfg->last_get_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* Put MZF dialog (CPM-IC import) - analogicky k Put: single-select
       otevře "Put as..." popup, multi-select tolerantní bulk put. */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( CPM_PUT_MZF_DLG, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            auto selection = ImGuiFileDialog::Instance ()->GetSelection ();

            if ( selection.size () == 1 ) {
                auto it = selection.begin ();
                strncpy ( g_cpm_put_src_path, it->second.c_str (),
                          sizeof ( g_cpm_put_src_path ) - 1 );
                g_cpm_put_src_path[sizeof ( g_cpm_put_src_path ) - 1] = '\0';

                /* Resetovat charset na EU default a pokusit se odvodit jméno
                   z MZF hlavičky. Při neúspěchu fallback na basename bez .mzf. */
                g_cpm_put_charset = MZF_NAME_ASCII_EU;
                bool derived = cpm_derive_name_from_mzf ( g_cpm_put_src_path,
                    g_cpm_put_charset, g_cpm_put_target_buf,
                    sizeof ( g_cpm_put_target_buf ) );

                if ( !derived ) {
                    const char *bname = strrchr ( it->first.c_str (), '/' );
                    if ( !bname ) bname = strrchr ( it->first.c_str (), '\\' );
                    if ( bname ) bname++; else bname = it->first.c_str ();
                    strncpy ( g_cpm_put_target_buf, bname,
                              sizeof ( g_cpm_put_target_buf ) - 1 );
                    g_cpm_put_target_buf[sizeof ( g_cpm_put_target_buf ) - 1] = '\0';
                    size_t tlen = strlen ( g_cpm_put_target_buf );
                    if ( tlen > 4 && strcasecmp ( g_cpm_put_target_buf + tlen - 4, ".mzf" ) == 0 ) {
                        g_cpm_put_target_buf[tlen - 4] = '\0';
                    }
                }

                g_cpm_put_is_mzf = true;
                g_cpm_put_auto_truncate = false;
                g_cpm_put_target_user = ( data->filter_user >= 0 )
                                        ? data->filter_user : 0;
                g_cpm_put_pending_open = true;
            } else {
                bool any_ok = false;
                char errors[512] = "";
                int err_len = 0;

                for ( auto &sel : selection ) {
                    const char *bname = strrchr ( sel.first.c_str (), '/' );
                    if ( !bname ) bname = strrchr ( sel.first.c_str (), '\\' );
                    if ( bname ) bname++; else bname = sel.first.c_str ();

                    uint8_t batch_mzf_user = ( data->filter_user >= 0 )
                                              ? (uint8_t) data->filter_user : 0;
                    en_MZDSK_RES res = panel_cpm_put_file_mzf ( disc, &data->dpb,
                                          sel.second.c_str (), batch_mzf_user );
                    if ( res == MZDSK_RES_OK ) {
                        any_ok = true;
                    } else {
                        int n = snprintf ( errors + err_len, sizeof ( errors ) - err_len,
                                           "%s: %s\n", bname, mzdsk_get_error ( res ) );
                        if ( n > 0 ) err_len += n;
                    }
                }

                if ( any_ok ) {
                    *is_dirty = true;
                    panel_cpm_load ( data, disc, detect );
                }
                if ( err_len > 0 ) {
                    data->has_error = true;
                    strncpy ( data->error_msg, errors, sizeof ( data->error_msg ) - 1 );
                    data->error_msg[sizeof ( data->error_msg ) - 1] = '\0';
                }
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_put_dir, dir.c_str (), sizeof ( cfg->last_put_dir ) - 1 );
            cfg->last_put_dir[sizeof ( cfg->last_put_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* === "Put file as..." dialog === */
    if ( g_cpm_put_pending_open ) {
        ImGui::OpenPopup ( _ ( "Put file as..." ) );
        g_cpm_put_pending_open = false;
        /* Reset validace pro čistý start. */
        g_cpm_put_last_err = MZDSK_NAMEVAL_OK;
        g_cpm_put_bad_char = 0;
    }

    {
        ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
        ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
        ImGui::SetNextWindowSize ( ImVec2 ( 520, 0 ), ImGuiCond_Appearing );
    }
    ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 16, 12 ) );
    if ( ImGui::BeginPopupModal ( _ ( "Put file as..." ), nullptr,
                                   ImGuiWindowFlags_NoResize ) ) {

        ImGui::Spacing ();
        ImGui::TextUnformatted ( _ ( "Source file:" ) );
        ImGui::SameLine ();
        ImGui::TextDisabled ( "%s", g_cpm_put_src_path );

        ImGui::Spacing ();
        ImGui::TextUnformatted ( _ ( "Target name:" ) );
        ImGui::SameLine ( 120 );
        ImGui::SetNextItemWidth ( 180 );

        /* Přepočítat validaci pro každý frame (po event změny textu). */
        char tmp_name[9], tmp_ext[4];
        g_cpm_put_last_err = mzdsk_validate_83_name ( g_cpm_put_target_buf,
            MZDSK_NAMEVAL_FLAVOR_CPM, tmp_name, tmp_ext, &g_cpm_put_bad_char );

        bool is_valid = ( g_cpm_put_last_err == MZDSK_NAMEVAL_OK );
        bool length_only = ( g_cpm_put_last_err == MZDSK_NAMEVAL_NAME_TOO_LONG
                          || g_cpm_put_last_err == MZDSK_NAMEVAL_EXT_TOO_LONG );

        /* Barva rámečku: zelená = valid, červená = invalid. */
        ImVec4 border_color = is_valid
            ? ImVec4 ( 0.2f, 0.8f, 0.2f, 1.0f )
            : ImVec4 ( 0.9f, 0.3f, 0.3f, 1.0f );
        ImGui::PushStyleColor ( ImGuiCol_Border, border_color );
        ImGui::PushStyleVar ( ImGuiStyleVar_FrameBorderSize, 2.0f );

        /* Fokus a select-all buď při prvním zobrazení dialogu, nebo po změně
           charset roletky (uživatel typicky chce celé jméno přepsat). */
        ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue;
        if ( ImGui::IsWindowAppearing () || g_cpm_put_refocus_name ) {
            ImGui::SetKeyboardFocusHere ();
            input_flags |= ImGuiInputTextFlags_AutoSelectAll;
            g_cpm_put_refocus_name = false;
        }
        bool enter_pressed = ImGui::InputText ( "##cpm_put_target",
            g_cpm_put_target_buf, sizeof ( g_cpm_put_target_buf ),
            input_flags );

        ImGui::PopStyleVar ();
        ImGui::PopStyleColor ();

        /* User roletka (0-15) zarovnaná vpravo na stejném řádku. */
        {
            const float user_combo_w = 70.0f;
            const char *lbl = _ ( "User:" );
            float lbl_w = ImGui::CalcTextSize ( lbl ).x;
            float gap = ImGui::GetStyle ().ItemInnerSpacing.x;
            float total = lbl_w + gap + user_combo_w;

            ImGui::SameLine ();
            float cur_x = ImGui::GetCursorPosX ();
            float right_x = ImGui::GetWindowContentRegionMax ().x;
            float target_x = right_x - total;
            if ( target_x > cur_x ) ImGui::SetCursorPosX ( target_x );

            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ( lbl );
            ImGui::SameLine ( 0, gap );
            ImGui::SetNextItemWidth ( user_combo_w );
            const char *user_items[16] = {
                "0", "1", "2", "3", "4", "5", "6", "7",
                "8", "9", "10", "11", "12", "13", "14", "15"
            };
            int u_idx = g_cpm_put_target_user;
            if ( u_idx < 0 ) u_idx = 0;
            if ( u_idx > 15 ) u_idx = 15;
            if ( ImGui::Combo ( "##cpm_put_user", &u_idx,
                                 user_items, IM_ARRAYSIZE ( user_items ) ) ) {
                g_cpm_put_target_user = u_idx;
            }
        }

        /* Tooltip s chybou při invalidním stavu. */
        if ( !is_valid && ImGui::IsItemHovered () ) {
            char err_msg[128];
            cpm_put_format_error ( g_cpm_put_last_err, g_cpm_put_bad_char,
                                    err_msg, sizeof ( err_msg ) );
            ImGui::SetTooltip ( "%s", err_msg );
        }

        /* Varianta Sharp MZ znakové sady pro odvození jména z MZF hlavičky.
           Zobrazuje se jen pro Put MZF (raw put žádnou konverzi nepotřebuje).
           Po změně se jméno přegeneruje a v dalším frame se zaostří + vybere,
           aby ho uživatel mohl rovnou přepsat. */
        if ( g_cpm_put_is_mzf ) {
            ImGui::Spacing ();
            ImGui::TextUnformatted ( _ ( "MZF fname charset:" ) );
            ImGui::SameLine ( 120 );
            ImGui::SetNextItemWidth ( 180 );
            const char *cs_items[] = { "EU (MZ-700/800)", "JP (MZ-1500)" };
            int cs_idx = ( g_cpm_put_charset == MZF_NAME_ASCII_JP ) ? 1 : 0;
            if ( ImGui::Combo ( "##cpm_put_charset", &cs_idx, cs_items,
                                IM_ARRAYSIZE ( cs_items ) ) ) {
                g_cpm_put_charset = ( cs_idx == 1 )
                    ? MZF_NAME_ASCII_JP : MZF_NAME_ASCII_EU;
                /* Přegenerovat jméno z MZF hlavičky s novou variantou.
                   Při neúspěchu nech stávající buffer beze změny. */
                cpm_derive_name_from_mzf ( g_cpm_put_src_path,
                    g_cpm_put_charset, g_cpm_put_target_buf,
                    sizeof ( g_cpm_put_target_buf ) );
                g_cpm_put_refocus_name = true;
            }
        }

        ImGui::Spacing ();
        ImGui::Checkbox ( _ ( "Auto-truncate on overflow" ), &g_cpm_put_auto_truncate );

        /* Inline hint nebo chybová hláška. */
        ImGui::Spacing ();
        if ( is_valid ) {
            ImGui::TextDisabled ( "%s", _ ( "Hint: 8 chars max for name, 3 for ext." ) );
        } else {
            char err_msg[128];
            cpm_put_format_error ( g_cpm_put_last_err, g_cpm_put_bad_char,
                                    err_msg, sizeof ( err_msg ) );
            ImGui::TextColored ( ImVec4 ( 0.9f, 0.3f, 0.3f, 1.0f ), "%s", err_msg );
        }

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        /* OK povolen pokud: validní jméno, NEBO (auto_truncate && jen délka). */
        bool ok_enabled = is_valid || ( g_cpm_put_auto_truncate && length_only );

        if ( !ok_enabled ) ImGui::BeginDisabled ();
        bool do_ok = ButtonMinWidth ( "OK" ) || ( ok_enabled && enter_pressed );
        if ( !ok_enabled ) ImGui::EndDisabled ();

        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            g_cpm_put_src_path[0] = '\0';
            ImGui::CloseCurrentPopup ();
        }

        if ( do_ok ) {
            char final_name[9], final_ext[4];
            if ( is_valid ) {
                strncpy ( final_name, tmp_name, sizeof ( final_name ) );
                strncpy ( final_ext, tmp_ext, sizeof ( final_ext ) );
            } else {
                /* length_only + auto_truncate: zkrátíme ručně. */
                cpm_truncate_name ( g_cpm_put_target_buf, final_name, final_ext );
            }

            uint8_t put_user = (uint8_t) ( g_cpm_put_target_user & 0x0F );
            en_MZDSK_RES res;
            if ( g_cpm_put_is_mzf ) {
                res = panel_cpm_put_file_mzf_ex ( disc, &data->dpb, g_cpm_put_src_path,
                    final_name, final_ext, put_user );
            } else {
                res = panel_cpm_put_file ( disc, &data->dpb, g_cpm_put_src_path,
                    final_name, final_ext, put_user );
            }

            if ( res == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_cpm_load ( data, disc, detect );
            } else {
                const char *bname = strrchr ( g_cpm_put_src_path, '/' );
                if ( !bname ) bname = strrchr ( g_cpm_put_src_path, '\\' );
                if ( bname ) bname++; else bname = g_cpm_put_src_path;
                data->has_error = true;
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "%.200s: %s", bname, mzdsk_get_error ( res ) );
            }

            g_cpm_put_src_path[0] = '\0';
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    }
    ImGui::PopStyleVar (); /* WindowPadding */
}


/**
 * @brief Zpracuje modální popupy (Delete, Rename, Error).
 */
static void process_popups ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc, bool *is_dirty,
                              st_MZDISK_CONFIG *cfg )
{
    /* Delete confirmation */
    if ( g_cpm_pending_delete ) {
        ImGui::OpenPopup ( "##cpm_delete" );
        g_cpm_pending_delete = false;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##cpm_delete", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        int del_count = count_selected ( data );
        ImGui::Text ( "%s %d %s?", _ ( "Delete" ), del_count, _ ( "file(s)" ) );
        ImGui::Spacing ();

        /* Velký počet zabalit do scrollable child regionu. */
        const int inline_limit = 8;
        bool use_scroll = ( del_count > inline_limit );
        if ( use_scroll ) {
            ImGui::BeginChild ( "##cpm_del_list",
                                ImVec2 ( 420.0f, 180.0f ),
                                ImGuiChildFlags_Borders );
        }
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] ) {
                char fn[16];
                format_fullname ( &data->files[i], fn, sizeof ( fn ) );
                ImGui::BulletText ( "%s (user %d)", fn, data->files[i].user );
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
                en_MZDSK_RES res = panel_cpm_delete_file ( disc, &data->dpb, &data->files[i] );
                if ( res == MZDSK_RES_OK ) {
                    any_ok = true;
                } else {
                    char fn[16];
                    format_fullname ( &data->files[i], fn, sizeof ( fn ) );
                    int n = snprintf ( errors + err_len, sizeof ( errors ) - err_len,
                                       "%s: %s\n", fn, mzdsk_get_error ( res ) );
                    if ( n > 0 ) err_len += n;
                }
            }

            if ( any_ok ) {
                *is_dirty = true;
                st_MZDSK_DETECT_RESULT det;
                memcpy ( &det.cpm_dpb, &data->dpb, sizeof ( st_MZDSK_CPM_DPB ) );
                det.cpm_format = data->cpm_format;
                panel_cpm_load ( data, disc, &det );
            }
            if ( err_len > 0 ) {
                data->has_error = true;
                strncpy ( data->error_msg, errors, sizeof ( data->error_msg ) - 1 );
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
    if ( g_cpm_pending_rename ) {
        ImGui::OpenPopup ( "##cpm_rename" );
        g_cpm_pending_rename = false;
    }

    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##cpm_rename", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        if ( g_cpm_rename_index >= 0 && g_cpm_rename_index < data->file_count ) {
            st_MZDSK_CPM_FILE_INFO_EX *f = &data->files[g_cpm_rename_index];
            ImGui::Text ( "%s: %s.%s", _ ( "Current name" ), f->filename, f->extension );
            ImGui::Spacing ();
            ImGui::Text ( "%s:", _ ( "New name (NAME.EXT)" ) );
            ImGui::SameLine ();
            ImGui::SetNextItemWidth ( 200 );
            if ( ImGui::IsWindowAppearing () ) ImGui::SetKeyboardFocusHere ();
            bool enter = ImGui::InputText ( "##cpm_ren_input", g_cpm_rename_buf,
                                            sizeof ( g_cpm_rename_buf ),
                                            ImGuiInputTextFlags_EnterReturnsTrue );
            ImGui::Spacing ();
            ImGui::Separator ();
            ImGui::Spacing ();

            bool do_rename = enter || ButtonMinWidth ( "OK" );
            if ( do_rename && g_cpm_rename_buf[0] != '\0' ) {
                char new_name[9], new_ext[4];
                if ( parse_cpm_name ( g_cpm_rename_buf, new_name, new_ext ) ) {
                    en_MZDSK_RES res = panel_cpm_rename_file ( disc, &data->dpb, f, new_name, new_ext );
                    if ( res == MZDSK_RES_OK ) {
                        *is_dirty = true;
                        st_MZDSK_DETECT_RESULT det;
                        memcpy ( &det.cpm_dpb, &data->dpb, sizeof ( st_MZDSK_CPM_DPB ) );
                        det.cpm_format = data->cpm_format;
                        panel_cpm_load ( data, disc, &det );
                    } else {
                        data->has_error = true;
                        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                                   "%s: %s", g_cpm_rename_buf, mzdsk_get_error ( res ) );
                    }
                } else {
                    data->has_error = true;
                    snprintf ( data->error_msg, sizeof ( data->error_msg ),
                               "Invalid CP/M name: %s", g_cpm_rename_buf );
                }
                g_cpm_rename_index = -1;
                ImGui::CloseCurrentPopup ();
            }
            if ( !do_rename ) {
                ImGui::SameLine ();
                if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
                    g_cpm_rename_index = -1;
                    ImGui::CloseCurrentPopup ();
                }
            }
        }
        ImGui::EndPopup ();
    }

    /* Bulk Get ASK popup - musí být PŘED "Error popup", aby se nepřekrývaly. */
    render_bulk_get_ask_popup ( data, disc, cfg );

    /* Error popup */
    if ( data->has_error ) {
        ImGui::OpenPopup ( "##cpm_error" );
        data->has_error = false;
    }

    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##cpm_error", nullptr,
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
}


/* =========================================================================
 * Hlavní render funkce
 * ========================================================================= */

extern "C" void panel_cpm_render ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                                    bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                    uint64_t owner_session_id )
{
    if ( !data || !data->is_loaded ) {
        ImGui::TextDisabled ( "%s", _ ( "No CP/M directory data." ) );
        return;
    }

    if ( ImGui::BeginTabBar ( "##cpm_tabs" ) ) {

        if ( TabItemWithTooltip ( _ ( "Directory" ) ) ) {
            render_directory_tab ( data, disc, is_dirty, cfg, owner_session_id );
            ImGui::EndTabItem ();
        }

        if ( TabItemWithTooltip ( _ ( "Alloc Map" ) ) ) {
            render_alloc_map_tab ( data );
            ImGui::EndTabItem ();
        }

        ImGui::EndTabBar ();
    }

    /* popupy a dialogy jsou na úrovni okna, ne uvnitř tabu */
    process_popups ( data, disc, is_dirty, cfg );

    st_MZDSK_DETECT_RESULT det;
    memcpy ( &det.cpm_dpb, &data->dpb, sizeof ( st_MZDSK_CPM_DPB ) );
    det.cpm_format = data->cpm_format;
    process_dialogs ( data, disc, is_dirty, cfg, &det );
}


/* =========================================================================
 *  CP/M Maintenance tab
 * ========================================================================= */


/** @brief Příznak: výsledky Check jsou k dispozici. */
static bool g_cpm_check_done = false;

/** @brief Log výsledků Check. */
static char g_cpm_check_log[4096] = "";

/** @brief Délka textu v check logu. */
static int g_cpm_check_log_len = 0;

/** @brief Počet souborů s chybami. */
static int g_cpm_check_errors = 0;

/** @brief Příznak: čekáme na potvrzení Defrag. */
static bool g_cpm_pending_defrag = false;

/** @brief Příznak: defragmentace právě probíhá. */
static bool g_cpm_defrag_running = false;

/** @brief Log defragmentace (progress callback). */
static char g_cpm_defrag_log[4096] = "";

/** @brief Délka textu v defrag logu. */
static int g_cpm_defrag_log_len = 0;

/* Async infrastruktura pro defrag (audit C-5): defrag běží ve worker
 * threadu, aby UI nezamrzlo. Hlavní smyčka každý frame kontroluje
 * `g_cpm_defrag_done` a když je nastaven na 1, `WaitThread` posbírá
 * výsledek a finalizuje UI state. */
static SDL_Thread *g_cpm_defrag_thread = nullptr;
static SDL_Mutex *g_cpm_defrag_mutex = nullptr;
static SDL_AtomicInt g_cpm_defrag_done;
static en_MZDSK_RES g_cpm_defrag_result = MZDSK_RES_OK;

/** @brief Kontext předávaný do worker threadu. Kopie dpb, ne pointer
 *  do `data`, aby panel mohl žít svým životem během defragu. */
static struct cpm_defrag_ctx {
    st_MZDSK_DISC *disc;
    st_MZDSK_CPM_DPB dpb;
} g_cpm_defrag_ctx;

/** @brief Příznak: čekáme na potvrzení Format File Area. */
static bool g_cpm_pending_format = false;

/** @brief Příznak: výsledek maintenance operace. */
static bool g_cpm_maint_result = false;

/** @brief Zpráva výsledku. */
static char g_cpm_maint_msg[512] = "";

/** @brief Příznak: výsledek je chyba. */
static bool g_cpm_maint_is_error = false;


/**
 * @brief Callback pro průběh defragmentace - ukládá zprávy do logu.
 *
 * Volán z worker threadu (audit C-5), proto zápis do `g_cpm_defrag_log`
 * chráníme `g_cpm_defrag_mutex`. UI render čte buffer pod stejným
 * lockem (viz render funkce).
 *
 * @param message Textová zpráva.
 * @param user_data Nepoužívá se.
 */
static void cpm_defrag_progress_cb ( const char *message, void *user_data )
{
    (void) user_data;
    if ( g_cpm_defrag_mutex ) SDL_LockMutex ( g_cpm_defrag_mutex );
    /* Ochrana proti heap overflow: pokud je buffer plný, další zprávy
     * zahodit. Bez této kontroly mohl `snprintf` vrátit n > dostupné
     * místo a následný `log_len += n` způsobil, že další volání počítalo
     * `sizeof(log) - log_len` jako záporné/podteklé číslo a zapsalo
     * mimo buffer. */
    size_t capacity = sizeof ( g_cpm_defrag_log );
    if ( (size_t) g_cpm_defrag_log_len < capacity - 1 ) {
        size_t remaining = capacity - (size_t) g_cpm_defrag_log_len;
        int n = snprintf ( g_cpm_defrag_log + g_cpm_defrag_log_len, remaining, "%s", message );
        if ( n > 0 ) {
            if ( (size_t) n >= remaining ) {
                g_cpm_defrag_log_len = (int) ( capacity - 1 );
            } else {
                g_cpm_defrag_log_len += n;
            }
        }
    }
    if ( g_cpm_defrag_mutex ) SDL_UnlockMutex ( g_cpm_defrag_mutex );
}


/**
 * @brief Worker thread entry point pro defragmentaci.
 *
 * Běží mimo UI thread - volá synchronní `mzdsk_cpm_defrag()` s
 * progress callbackem a po dokončení nastaví atomic done flag, který
 * UI render loop poté zachytí.
 */
static int cpm_defrag_thread_fn ( void *user_data )
{
    (void) user_data;
    g_cpm_defrag_result = mzdsk_cpm_defrag ( g_cpm_defrag_ctx.disc,
                                               &g_cpm_defrag_ctx.dpb,
                                               cpm_defrag_progress_cb, nullptr );
    SDL_SetAtomicInt ( &g_cpm_defrag_done, 1 );
    return 0;
}


extern "C" void panel_cpm_maintenance_render ( st_PANEL_CPM_DATA *data,
                                                st_MZDSK_DISC *disc,
                                                st_MZDSK_DETECT_RESULT *detect,
                                                bool *is_dirty )
{
    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();

    ImGui::TextWrapped ( "%s", _ ( "CP/M disk maintenance operations." ) );
    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* --- Check --- */
    ImGui::Text ( "%s", _ ( "Check Extents" ) );
    ImGui::TextWrapped ( "%s", _ ( "Verifies extent consistency for all files. Read-only operation - no data is modified." ) );
    ImGui::Spacing ();
    if ( ImGui::Button ( _ ( "Check" ) ) ) {
        g_cpm_check_log[0] = '\0';
        g_cpm_check_log_len = 0;
        g_cpm_check_errors = 0;
        g_cpm_check_done = false;

        int total_ok = 0;
        for ( int i = 0; i < data->file_count; i++ ) {
            st_MZDSK_CPM_FILE_INFO_EX *f = &data->files[i];
            st_MZDSK_CPM_EXTENT_CHECK result;

            char fname[9], ext_buf[4];
            /* extrahovat jméno a příponu jako C řetězce */
            snprintf ( fname, sizeof ( fname ), "%-8.8s", f->filename );
            snprintf ( ext_buf, sizeof ( ext_buf ), "%-3.3s", f->extension );

            int rc = mzdsk_cpm_check_extents ( disc, &data->dpb,
                                                fname, ext_buf, f->user, &result );
            if ( rc < 0 ) {
                int n = snprintf ( g_cpm_check_log + g_cpm_check_log_len,
                                   sizeof ( g_cpm_check_log ) - g_cpm_check_log_len,
                                   "%s.%s (U%d): read error\n", f->filename, f->extension, f->user );
                if ( n > 0 ) g_cpm_check_log_len += n;
                g_cpm_check_errors++;
            } else if ( result.count > 0 ) {
                int n = snprintf ( g_cpm_check_log + g_cpm_check_log_len,
                                   sizeof ( g_cpm_check_log ) - g_cpm_check_log_len,
                                   "%s.%s (U%d): %d missing extent(s)\n",
                                   f->filename, f->extension, f->user, result.count );
                if ( n > 0 ) g_cpm_check_log_len += n;
                g_cpm_check_errors++;
            } else {
                total_ok++;
            }
        }

        /* shrnutí */
        int n = snprintf ( g_cpm_check_log + g_cpm_check_log_len,
                           sizeof ( g_cpm_check_log ) - g_cpm_check_log_len,
                           "\n--- %d file(s) checked, %d OK, %d error(s) ---\n",
                           data->file_count, total_ok, g_cpm_check_errors );
        if ( n > 0 ) g_cpm_check_log_len += n;
        g_cpm_check_done = true;
    }

    /* check log */
    if ( g_cpm_check_done ) {
        ImGui::Spacing ();
        if ( g_cpm_check_errors == 0 ) {
            ImGui::TextColored ( ImVec4 ( 0.4f, 1.0f, 0.4f, 1.0f ), "%s",
                                _ ( "All files OK." ) );
        } else {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%d %s",
                                g_cpm_check_errors, _ ( "file(s) with errors" ) );
        }
        ImGui::BeginChild ( "check_log", ImVec2 ( 0, 200 ), ImGuiChildFlags_Borders );
        ImGui::TextUnformatted ( g_cpm_check_log );
        ImGui::EndChild ();
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* --- Defrag --- */
    ImGui::Text ( "%s", _ ( "Defragmentation" ) );
    ImGui::TextWrapped ( "%s", _ ( "Reads all files into memory, formats directory, and writes files sequentially without gaps. System tracks and attributes are preserved." ) );
    ImGui::Spacing ();

    /* Polling worker threadu: každý frame kontrolujeme, jestli dokončil.
     * Pokud ano, posbíráme výsledek přes SDL_WaitThread a finalizujeme
     * UI state (reload panelu, maint_msg). Audit C-5. */
    if ( g_cpm_defrag_running && SDL_GetAtomicInt ( &g_cpm_defrag_done ) ) {
        if ( g_cpm_defrag_thread ) {
            SDL_WaitThread ( g_cpm_defrag_thread, nullptr );
            g_cpm_defrag_thread = nullptr;
        }
        g_cpm_defrag_running = false;
        en_MZDSK_RES res = g_cpm_defrag_result;
        if ( res == MZDSK_RES_OK ) {
            *is_dirty = true;
            panel_cpm_load ( data, disc, detect );
            g_cpm_maint_is_error = false;
            snprintf ( g_cpm_maint_msg, sizeof ( g_cpm_maint_msg ),
                       "%s", _ ( "Defragmentation completed successfully." ) );
        } else {
            g_cpm_maint_is_error = true;
            snprintf ( g_cpm_maint_msg, sizeof ( g_cpm_maint_msg ),
                       "%s: %s", _ ( "Defragmentation failed" ), mzdsk_get_error ( res ) );
        }
        g_cpm_maint_result = true;
    }

    if ( g_cpm_defrag_running ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Defragment" ) ) ) {
        g_cpm_pending_defrag = true;
    }
    if ( g_cpm_defrag_running ) ImGui::EndDisabled ();

    /* Indikátor běhu defragu - doplňuje disabled button. */
    if ( g_cpm_defrag_running ) {
        ImGui::SameLine ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.85f, 0.2f, 1.0f ),
                             "%s", _ ( "Defragmentation in progress..." ) );
    }

    /* defrag log - čteme pod mutexem, worker thread do něj zapisuje
     * z progress callbacku. */
    bool have_log;
    if ( g_cpm_defrag_mutex ) SDL_LockMutex ( g_cpm_defrag_mutex );
    have_log = ( g_cpm_defrag_log_len > 0 );
    if ( g_cpm_defrag_mutex ) SDL_UnlockMutex ( g_cpm_defrag_mutex );

    if ( have_log ) {
        ImGui::Spacing ();
        ImGui::BeginChild ( "cpm_defrag_log", ImVec2 ( 0, 150 ), ImGuiChildFlags_Borders );
        if ( g_cpm_defrag_mutex ) SDL_LockMutex ( g_cpm_defrag_mutex );
        ImGui::TextUnformatted ( g_cpm_defrag_log );
        if ( g_cpm_defrag_mutex ) SDL_UnlockMutex ( g_cpm_defrag_mutex );
        ImGui::SetScrollHereY ( 1.0f );
        ImGui::EndChild ();
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* --- Format File Area --- */
    ImGui::Text ( "%s", _ ( "Format (File Area)" ) );
    ImGui::TextWrapped ( "%s", _ ( "Erases directory (fills with 0xE5). System tracks (CCP+BDOS+BIOS) are preserved. All files become inaccessible." ) );
    ImGui::Spacing ();
    if ( ImGui::Button ( _ ( "Format File Area" ) ) ) {
        g_cpm_pending_format = true;
    }

    /* ======= Potvrzovací dialogy ======= */

    /* Defrag potvrzení */
    if ( g_cpm_pending_defrag ) {
        ImGui::OpenPopup ( "##cpm_defrag_confirm" );
        g_cpm_pending_defrag = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##cpm_defrag_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Defragmentation" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "This will read all files into memory, format the directory, and write files sequentially without gaps. System tracks, attributes and user numbers are preserved." ) );
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s",
                            _ ( "WARNING: If the process is interrupted, data may be lost!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ImGui::Button ( _ ( "Defragment" ), ImVec2 ( 160, 0 ) ) ) {
            /* Asynchronní defrag: vyčistíme log, spustíme worker thread,
             * zavřeme popup. UI render loop potom každý frame pollí
             * `g_cpm_defrag_done` a finalizuje po dokončení. Audit C-5. */
            g_cpm_defrag_log[0] = '\0';
            g_cpm_defrag_log_len = 0;
            g_cpm_defrag_running = true;
            g_cpm_defrag_result = MZDSK_RES_OK;
            SDL_SetAtomicInt ( &g_cpm_defrag_done, 0 );

            if ( !g_cpm_defrag_mutex ) g_cpm_defrag_mutex = SDL_CreateMutex ();

            g_cpm_defrag_ctx.disc = disc;
            memcpy ( &g_cpm_defrag_ctx.dpb, &data->dpb, sizeof ( st_MZDSK_CPM_DPB ) );

            g_cpm_defrag_thread = SDL_CreateThread ( cpm_defrag_thread_fn, "cpm_defrag", nullptr );
            if ( !g_cpm_defrag_thread ) {
                /* Fallback na synchronní cestu pokud thread nejde spustit. */
                g_cpm_defrag_running = false;
                g_cpm_defrag_result = mzdsk_cpm_defrag ( disc, &data->dpb,
                                                          cpm_defrag_progress_cb, nullptr );
                SDL_SetAtomicInt ( &g_cpm_defrag_done, 1 );
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
    if ( g_cpm_pending_format ) {
        ImGui::OpenPopup ( "##cpm_format_confirm" );
        g_cpm_pending_format = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##cpm_format_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Format File Area" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "This will erase the entire CP/M directory. All files will become inaccessible. System tracks will be preserved." ) );
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                            _ ( "WARNING: This operation cannot be undone!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Format" ) ) ) {
            en_MZDSK_RES res = mzdsk_cpm_format_directory ( disc, &data->dpb );
            if ( res == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_cpm_load ( data, disc, detect );
                g_cpm_maint_is_error = false;
                snprintf ( g_cpm_maint_msg, sizeof ( g_cpm_maint_msg ),
                           "%s", _ ( "File area format completed successfully." ) );
            } else {
                g_cpm_maint_is_error = true;
                snprintf ( g_cpm_maint_msg, sizeof ( g_cpm_maint_msg ),
                           "%s: %s", _ ( "Format failed" ), mzdsk_get_error ( res ) );
            }
            g_cpm_maint_result = true;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Výsledek operace */
    if ( g_cpm_maint_result ) {
        ImGui::OpenPopup ( "##cpm_maint_result" );
        g_cpm_maint_result = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##cpm_maint_result", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        if ( g_cpm_maint_is_error ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", _ ( "Error" ) );
        } else {
            ImGui::TextColored ( ImVec4 ( 0.4f, 1.0f, 0.4f, 1.0f ), "%s", _ ( "Success" ) );
        }
        ImGui::Spacing ();
        ImGui::TextUnformatted ( g_cpm_maint_msg );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();
        if ( ButtonMinWidth ( "OK" ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
}
