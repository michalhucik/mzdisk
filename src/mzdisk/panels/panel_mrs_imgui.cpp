/**
 * @file panel_mrs_imgui.cpp
 * @brief ImGui rendering MRS directory listingu, souborových operací, FAT vizualizace a Maintenance tabu.
 *
 * Zobrazuje dva sub-taby:
 * - Directory: tabulka souborů s checkboxy, Get/Put/Delete/Rename
 * - FAT Map: vizualizace FAT tabulky s per-file barvami, disk layout,
 *   legendou (linkový styl, stisk myši zvýrazní bloky souboru v mřížce
 *   a ztlumí ostatní) a blokovou mřížkou s hover tooltipem
 *
 * Maintenance tab poskytuje operace údržby:
 * - Defrag: defragmentace souborové oblasti (fsmrs_defrag s progress callbackem)
 * - Format (File Area): reinicializuje FAT a adresář, zachová systémovou oblast
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
#include <cmath>

extern "C" {
#include "panels/panel_mrs.h"
#include "config.h"
#include "dragdrop.h"
#include "i18n.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
}

#include "ui_helpers.h"

static const char *MRS_GET_DLG = "MrsGetDlg";         /**< Get MZF file/dir dialog. */
static const char *MRS_GET_RAW_DLG = "MrsGetRawDlg";  /**< Get raw file/dir dialog. */
static const char *MRS_PUT_DLG = "MrsPutDlg";         /**< Put MZF (import) file dialog. */
static const char *MRS_PUT_RAW_DLG = "MrsPutRawDlg";  /**< Put raw binárního souboru. */
/* MRS_EXPORT_ALL_DLG odstraněn v v0.7.11 - duplikoval funkcionalitu
   Get (Export MZF) s "vybrat vše" a jen zbytečně matl UI. */
static const ImU32 SELECTED_ROW_COLOR = IM_COL32 ( 40, 70, 120, 255 );

static bool g_mrs_pending_delete = false;
static bool g_mrs_pending_rename = false;
static int g_mrs_rename_index = -1;
static char g_mrs_rename_buf[16] = "";

/* Bulk Get ASK stav je nyní v data->bulk_get (st_PANEL_MRS_BULK_GET),
   shodně s CP/M panelem. Viz bulk_get_* helpery níže. */

/* Put file as... dialog state */
static bool g_mrs_put_pending_open = false;
static bool g_mrs_put_is_mzf = false;
static char g_mrs_put_src_path[1024] = "";
static char g_mrs_put_target_buf[16] = "";
static bool g_mrs_put_auto_truncate = false;
/* STRT/EXEC pro raw put (MZF put tyto hodnoty bere z MZF hlavičky). */
static int g_mrs_put_fstrt = 0;
static int g_mrs_put_fexec = 0;
static en_MZDSK_NAMEVAL g_mrs_put_last_err = MZDSK_NAMEVAL_OK;
static char g_mrs_put_bad_char = 0;
static en_MZF_NAME_ENCODING g_mrs_put_charset = MZF_NAME_ASCII_EU; /**< Varianta Sharp MZ pro derivaci fname. */
static bool g_mrs_put_refocus_name = false;        /**< true = v příštím frame zaostřit InputText a označit celý text. */


/**
 * @brief Odvodí cílové MRS 8.3 jméno z MZF hlavičky s vybranou Sharp MZ variantou.
 *
 * Načte 128B hlavičku z `mzf_path`, zavolá `mzf_tools_get_fname_ex()`
 * a výsledek rozparsuje na 8.3 podle poslední tečky. Vnitřní mezery
 * a netisknutelné znaky jsou nahrazeny `_` - výsledek tak splňuje MRS
 * omezení na jméno (8 + 3 znaky, printable ASCII bez mezer).
 *
 * @param[in]  mzf_path  Cesta k MZF souboru. Nesmí být NULL.
 * @param[in]  encoding  Varianta Sharp MZ znakové sady (EU nebo JP ASCII).
 * @param[out] out_buf   Výstupní buffer pro řetězec NAME.EXT. Nesmí být NULL.
 * @param[in]  buf_size  Velikost výstupního bufferu v bajtech.
 *
 * @return true při úspěchu, false pokud se MZF nepodařilo načíst.
 *         Při neúspěchu není `out_buf` modifikován.
 */
static bool mrs_derive_name_from_mzf ( const char *mzf_path,
                                         en_MZF_NAME_ENCODING encoding,
                                         char *out_buf, size_t buf_size )
{
    FILE *fp = fopen ( mzf_path, "rb" );
    if ( !fp ) return false;

    st_MZF_HEADER hdr;
    size_t read_count = fread ( &hdr, 1, sizeof ( hdr ), fp );
    fclose ( fp );
    if ( read_count != sizeof ( hdr ) ) return false;

    mzf_header_items_correction ( &hdr );

    char ascii_name[MZF_FILE_NAME_LENGTH + 1] = { 0 };
    mzf_tools_get_fname_ex ( &hdr, ascii_name, sizeof ( ascii_name ), encoding );

    /* Oříznout úvodní a koncové mezery, pak split na name.ext podle
       poslední tečky. */
    char *p = ascii_name;
    while ( *p == ' ' ) p++;
    size_t len = strlen ( p );
    while ( len > 0 && p[len - 1] == ' ' ) p[--len] = '\0';

    char *dot = strrchr ( p, '.' );
    const char *name_src;
    const char *ext_src;
    size_t name_len, ext_len;
    if ( dot ) {
        name_src = p;
        name_len = (size_t) ( dot - p );
        ext_src = dot + 1;
        ext_len = strlen ( ext_src );
    } else {
        name_src = p;
        name_len = strlen ( p );
        ext_src = "";
        ext_len = 0;
    }

    /* Sanitizace - MRS 8.3: printable ASCII bez mezer, max 8/3 znaků. */
    char name_part[9] = { 0 };
    char ext_part[4] = { 0 };
    if ( name_len > 8 ) name_len = 8;
    if ( ext_len > 3 ) ext_len = 3;
    for ( size_t i = 0; i < name_len; i++ ) {
        unsigned char c = (unsigned char) name_src[i];
        name_part[i] = ( c > 0x20 && c <= 0x7E ) ? (char) c : '_';
    }
    for ( size_t i = 0; i < ext_len; i++ ) {
        unsigned char c = (unsigned char) ext_src[i];
        ext_part[i] = ( c > 0x20 && c <= 0x7E ) ? (char) c : '_';
    }

    if ( name_part[0] == '\0' ) {
        /* Po sanitizaci prázdné jméno - fallback. */
        snprintf ( out_buf, buf_size, "FILE" );
    } else if ( ext_part[0] != '\0' ) {
        snprintf ( out_buf, buf_size, "%s.%s", name_part, ext_part );
    } else {
        snprintf ( out_buf, buf_size, "%s", name_part );
    }
    return true;
}


/**
 * @brief Provede tolerantní zkrácení MRS jména na 8.3 (case zachovává).
 */
static void mrs_truncate_name ( const char *input, char *out_name, char *out_ext )
{
    memset ( out_name, 0, 9 );
    memset ( out_ext, 0, 4 );
    const char *dot = strchr ( input, '.' );
    int nlen = dot ? (int) ( dot - input ) : (int) strlen ( input );
    if ( nlen > 8 ) nlen = 8;
    memcpy ( out_name, input, nlen );
    if ( dot ) {
        int elen = (int) strlen ( dot + 1 );
        if ( elen > 3 ) elen = 3;
        memcpy ( out_ext, dot + 1, elen );
    }
}


/**
 * @brief Sestaví lokalizovanou chybovou hlášku pro tooltip Put dialogu.
 */
static void mrs_put_format_error ( en_MZDSK_NAMEVAL code, char bad, char *buf, size_t buf_size )
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

static void format_fullname ( const st_PANEL_MRS_FILE *f, char *buf, int buf_size )
{
    snprintf ( buf, buf_size, "%s.%s", f->name, f->ext );
}


static int count_selected ( const st_PANEL_MRS_DATA *data )
{
    int count = 0;
    for ( int i = 0; i < data->file_count; i++ ) {
        if ( data->selected[i] ) count++;
    }
    return count;
}


/* =========================================================================
 * Bulk Get MZF export s podporou ASK módu (shodně s CP/M panelem)
 * ========================================================================= */

/**
 * @brief Provede zápis jednoho souboru (raw nebo MZF) a inkrementuje čítače.
 */
static void bulk_get_do_export ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                                  int file_idx, const char *filepath )
{
    st_PANEL_MRS_BULK_GET *b = &data->bulk_get;
    en_MZDSK_RES gr = b->is_mzf
        ? panel_mrs_get_file_mzf ( config, &data->files[file_idx], filepath )
        : panel_mrs_get_file     ( config, &data->files[file_idx], filepath );
    if ( gr == MZDSK_RES_OK ) {
        b->ok_count++;
    } else {
        b->failed_count++;
        char fname[16];
        format_fullname ( &data->files[file_idx], fname, sizeof ( fname ) );
        const char *suffix = b->is_mzf ? ".mzf" : "";
        int n = snprintf ( b->errors + b->err_len, sizeof ( b->errors ) - b->err_len,
                           "%s%s: %s\n", fname, suffix, mzdsk_get_error ( gr ) );
        if ( n > 0 ) b->err_len += n;
    }
}


/**
 * @brief Pokračuje v bulk Get exportu dokud nenarazí na ASK kolizi nebo neskončí.
 */
static bool bulk_get_resume ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                               st_MZDISK_CONFIG *cfg )
{
    st_PANEL_MRS_BULK_GET *b = &data->bulk_get;

    for ( int i = b->next_idx; i < data->file_count; i++ ) {
        if ( !data->selected[i] ) continue;

        char fname[16];
        format_fullname ( &data->files[i], fname, sizeof ( fname ) );
        /* Viz komentář v panel_cpm_imgui: extra 32 B potlačuje
           -Wformat-truncation warning. */
        char filepath[2048 + 32];
        snprintf ( filepath, sizeof ( filepath ), "%s/%s%s",
                   b->dirpath, fname, b->is_mzf ? ".mzf" : "" );

        int effective_mode = ( b->override_mode >= 0 )
                           ? b->override_mode : cfg->export_dup_mode;

        if ( effective_mode == MZDSK_EXPORT_DUP_ASK ) {
            FILE *f = fopen ( filepath, "rb" );
            if ( f ) {
                fclose ( f );
                b->conflict_idx = i;
                strncpy ( b->conflict_path, filepath, sizeof ( b->conflict_path ) - 1 );
                b->conflict_path[ sizeof ( b->conflict_path ) - 1 ] = '\0';
                b->ask_pending = true;
                b->next_idx = i;
                return false;
            }
            bulk_get_do_export ( data, config, i, filepath );
        } else {
            int r = mzdisk_config_resolve_export_dup ( filepath, sizeof ( filepath ), effective_mode );
            if ( r == 0 ) {
                bulk_get_do_export ( data, config, i, filepath );
            } else if ( r == 1 ) {
                /* skip */
            } else {
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
 * @brief Dokončí bulk - summary do error_msg + popup.
 */
static void bulk_get_finish ( st_PANEL_MRS_DATA *data )
{
    st_PANEL_MRS_BULK_GET *b = &data->bulk_get;
    if ( b->failed_count > 0 ) {
        char summary[ sizeof ( data->error_msg ) + 64 ];
        snprintf ( summary, sizeof ( summary ),
                   "%s: %d OK, %d failed:\n%s",
                   b->is_mzf ? _ ( "Export MZF" ) : _ ( "Export" ),
                   b->ok_count, b->failed_count, b->errors );
        strncpy ( data->error_msg, summary, sizeof ( data->error_msg ) - 1 );
        data->error_msg[ sizeof ( data->error_msg ) - 1 ] = '\0';
        data->has_error = true;
        fprintf ( stderr, "MRS %s: %d file(s) OK, %d failed:\n%s",
                  b->is_mzf ? "Get MZF" : "Get",
                  b->ok_count, b->failed_count, b->errors );
    }
    b->active = false;
}


static void bulk_get_start ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                              st_MZDISK_CONFIG *cfg, bool is_mzf, const char *dirpath )
{
    st_PANEL_MRS_BULK_GET *b = &data->bulk_get;
    memset ( b, 0, sizeof ( *b ) );
    b->active = true;
    b->is_mzf = is_mzf;
    b->override_mode = -1;
    strncpy ( b->dirpath, dirpath, sizeof ( b->dirpath ) - 1 );
    b->dirpath[ sizeof ( b->dirpath ) - 1 ] = '\0';

    if ( bulk_get_resume ( data, config, cfg ) ) {
        bulk_get_finish ( data );
    }
}


/**
 * @brief ASK popup pro bulk Get - stejný pattern jako v CP/M panelu.
 *
 * BeginPopupModal musí být volán každý snímek (ImGui požadavek), proto
 * je `OpenPopup` pod podmínkou `ask_pending`, ale `BeginPopupModal`
 * mimo ni. Viz v0.7.10 fix v CP/M panelu.
 */
static void render_bulk_get_ask_popup ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                                         st_MZDISK_CONFIG *cfg )
{
    st_PANEL_MRS_BULK_GET *b = &data->bulk_get;

    if ( b->ask_pending ) {
        ImGui::OpenPopup ( "##mrs_bulk_get_ask" );
        b->ask_pending = false;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_bulk_get_ask", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "File already exists:" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", b->conflict_path );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        int action = -1;
        bool apply_all = false;
        bool cancel = false;

        if ( ButtonMinWidth ( _ ( "Overwrite" ) ) )     { action = 0; }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Rename" ) ) )        { action = 1; }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Skip" ) ) )          { action = 2; }

        ImGui::Spacing ();
        ImGui::TextDisabled ( "%s", _ ( "Apply to all remaining:" ) );
        if ( ButtonMinWidth ( _ ( "Overwrite all" ) ) ) { action = 0; apply_all = true; }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Rename all" ) ) )    { action = 1; apply_all = true; }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Skip all" ) ) )      { action = 2; apply_all = true; }

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) )        { cancel = true; }

        if ( action >= 0 || cancel ) {
            int idx = b->conflict_idx;
            if ( !cancel ) {
                char filepath[2048];
                strncpy ( filepath, b->conflict_path, sizeof ( filepath ) - 1 );
                filepath[ sizeof ( filepath ) - 1 ] = '\0';

                if ( action == 0 ) {
                    bulk_get_do_export ( data, config, idx, filepath );
                } else if ( action == 1 ) {
                    if ( mzdisk_config_resolve_export_dup ( filepath, sizeof ( filepath ),
                                                             MZDSK_EXPORT_DUP_RENAME ) == 0 ) {
                        bulk_get_do_export ( data, config, idx, filepath );
                    } else {
                        b->failed_count++;
                        char fname[16];
                        format_fullname ( &data->files[idx], fname, sizeof ( fname ) );
                        int n = snprintf ( b->errors + b->err_len, sizeof ( b->errors ) - b->err_len,
                                           "%s: rename failed\n", fname );
                        if ( n > 0 ) b->err_len += n;
                    }
                }
                /* action == 2 = Skip: nic. */

                if ( apply_all ) {
                    b->override_mode = ( action == 0 ) ? MZDSK_EXPORT_DUP_OVERWRITE
                                     : ( action == 1 ) ? MZDSK_EXPORT_DUP_RENAME
                                                       : MZDSK_EXPORT_DUP_SKIP;
                }

                b->next_idx = idx + 1;
                if ( bulk_get_resume ( data, config, cfg ) ) {
                    bulk_get_finish ( data );
                }
            } else {
                bulk_get_finish ( data );
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
}


/* =========================================================================
 * FAT Map - barvy a konstanty
 * ========================================================================= */

/** @brief Velikost jedné buňky FAT mřížky v pixelech. */
static const float FAT_CELL_SIZE = 16.0f;

/** @brief Mezera mezi buňkami v pixelech. */
static const float FAT_CELL_GAP = 1.0f;

/** @brief Výška pruhu disk layout v pixelech. */
static const float FAT_LAYOUT_BAR_HEIGHT = 36.0f;

/** @brief Minimální šířka segmentu v pixelech. */
static const float FAT_LAYOUT_MIN_SEGMENT_WIDTH = 60.0f;

/** @brief Barva systémových bloků (fialová). */
static const ImU32 COL_SYSTEM = IM_COL32 ( 156, 39, 176, 255 );

/** @brief Barva FAT bloků (oranžová). */
static const ImU32 COL_FAT = IM_COL32 ( 255, 152, 0, 255 );

/** @brief Barva adresářových bloků (modrá). */
static const ImU32 COL_DIR = IM_COL32 ( 33, 150, 243, 255 );

/** @brief Barva volných bloků (tmavě zelená). */
static const ImU32 COL_FREE = IM_COL32 ( 40, 60, 40, 255 );

/** @brief Barva vadných bloků (červená). */
static const ImU32 COL_BAD = IM_COL32 ( 244, 67, 54, 255 );

/** @brief File ID zvýrazněný kliknutím v legendě (0 = žádný). */
static uint8_t g_mrs_legend_sel = 0;


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
 * @brief Vrátí barvu pro daný file_id pomocí golden angle HSV rotace.
 *
 * Zajistí maximální vizuální rozlišení sousedních file_id hodnot.
 *
 * @param file_id Číslo souboru (1-89).
 * @return ImU32 barva.
 */
static ImU32 file_id_color ( uint8_t file_id )
{
    float h = fmodf ( (float) file_id * 137.508f, 360.0f );
    return hsv_to_col32 ( h, 0.65f, 0.85f );
}


/**
 * @brief Vrátí barvu pro danou FAT hodnotu.
 *
 * Strukturální typy mají fixní barvy shodné s panel_map.
 * Souborové bloky (file_id 1-89) mají unikátní barvu z HSV rotace.
 *
 * @param fat_val Hodnota z FAT tabulky.
 * @return ImU32 barva.
 */
static ImU32 fat_block_color ( uint8_t fat_val )
{
    switch ( fat_val ) {
        case FSMRS_FAT_FREE:   return COL_FREE;
        case FSMRS_FAT_FAT:    return COL_FAT;
        case FSMRS_FAT_DIR:    return COL_DIR;
        case FSMRS_FAT_BAD:    return COL_BAD;
        case FSMRS_FAT_SYSTEM: return COL_SYSTEM;
        default:               return file_id_color ( fat_val );
    }
}


/**
 * @brief Najde jméno souboru podle file_id v datovém modelu.
 *
 * @param data Datový model.
 * @param file_id Číslo souboru.
 * @param buf Výstupní buffer pro "NAME.EXT".
 * @param buf_size Velikost bufferu.
 * @return true pokud nalezeno.
 */
static bool find_file_name ( const st_PANEL_MRS_DATA *data, uint8_t file_id,
                             char *buf, int buf_size )
{
    for ( int i = 0; i < data->file_count; i++ ) {
        if ( data->files[i].file_id == file_id ) {
            snprintf ( buf, buf_size, "%s.%s", data->files[i].name, data->files[i].ext );
            return true;
        }
    }
    return false;
}


/* =========================================================================
 * FAT Map - rendering
 * ========================================================================= */

/**
 * @brief Vykreslí řádek se statistikami FAT mapy.
 *
 * @param data Datový model s fat_stats.
 */
static void render_fat_stats ( const st_PANEL_MRS_DATA *data )
{
    const st_FSMRS_MAP_STATS *s = &data->fat_stats;
    ImGui::Text ( "%s: %d", _ ( "Total blocks" ), s->total_blocks );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d", _ ( "Free" ), s->free_blocks );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d", _ ( "Files" ), s->file_blocks );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d", _ ( "System" ), s->sys_blocks );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "FAT: %d", s->fat_blocks );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d", _ ( "Dir" ), s->dir_blocks );
    if ( s->bad_blocks > 0 ) {
        ImGui::SameLine ( 0, 20 );
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.3f, 0.3f, 1.0f ), "%s: %d", _ ( "Bad" ), s->bad_blocks );
    }
}


/**
 * @brief Vykreslí disk layout pruh pro MRS.
 *
 * Zobrazuje proporcionální segmenty: System, FAT, Dir, Data, Free.
 *
 * @param data Datový model s layout parametry.
 */
static void render_fat_layout ( const st_PANEL_MRS_DATA *data )
{
    ImGui::SeparatorText ( _ ( "Disk Layout" ) );

    /* definice segmentů */
    struct {
        const char *label;
        uint32_t blocks;
        ImU32 color;
    } segs[5];
    int seg_count = 0;

    /* System - bloky 0 .. fat_block-1 */
    if ( data->fat_block > 0 ) {
        segs[seg_count].label = "System";
        segs[seg_count].blocks = data->fat_block;
        segs[seg_count].color = COL_SYSTEM;
        seg_count++;
    }

    /* FAT */
    segs[seg_count].label = "FAT";
    segs[seg_count].blocks = data->fat_sectors;
    segs[seg_count].color = COL_FAT;
    seg_count++;

    /* Directory */
    segs[seg_count].label = "Dir";
    segs[seg_count].blocks = data->dir_sectors;
    segs[seg_count].color = COL_DIR;
    seg_count++;

    /* Data (file + free + bad) */
    uint32_t data_blocks = (uint32_t) FSMRS_COUNT_BLOCKS - data->data_block;
    uint32_t used_data = (uint32_t) data->fat_stats.file_blocks;
    if ( used_data > 0 ) {
        segs[seg_count].label = _ ( "File Data" );
        segs[seg_count].blocks = used_data;
        segs[seg_count].color = IM_COL32 ( 76, 175, 80, 255 ); /* zelená */
        seg_count++;
    }

    /* Free */
    uint32_t free_data = data_blocks - used_data - (uint32_t) data->fat_stats.bad_blocks;
    if ( free_data > 0 ) {
        segs[seg_count].label = _ ( "Free" );
        segs[seg_count].blocks = free_data;
        segs[seg_count].color = COL_FREE;
        seg_count++;
    }

    /* celkový počet bloků */
    uint32_t total = FSMRS_COUNT_BLOCKS;

    /* dvouprůchodový výpočet šířek */
    ImVec2 avail = ImGui::GetContentRegionAvail ();
    float bar_width = avail.x;
    if ( bar_width < 100.0f ) bar_width = 100.0f;

    float widths[5];
    float reserved_width = 0.0f;
    float remaining_blocks = 0.0f;

    for ( int i = 0; i < seg_count; i++ ) {
        float w = ( (float) segs[i].blocks / (float) total ) * bar_width;
        if ( w < FAT_LAYOUT_MIN_SEGMENT_WIDTH ) {
            widths[i] = FAT_LAYOUT_MIN_SEGMENT_WIDTH;
            reserved_width += FAT_LAYOUT_MIN_SEGMENT_WIDTH;
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
        ImVec2 p1 = ImVec2 ( x + w, origin.y + FAT_LAYOUT_BAR_HEIGHT );

        dl->AddRectFilled ( p0, p1, segs[i].color );

        if ( i > 0 ) {
            dl->AddLine ( ImVec2 ( x, origin.y ), ImVec2 ( x, origin.y + FAT_LAYOUT_BAR_HEIGHT ),
                          IM_COL32 ( 20, 20, 20, 255 ), 1.0f );
        }

        ImVec2 text_size = ImGui::CalcTextSize ( segs[i].label );
        if ( text_size.x + 4.0f < w ) {
            float tx = x + ( w - text_size.x ) * 0.5f;
            float ty = origin.y + ( FAT_LAYOUT_BAR_HEIGHT - text_size.y ) * 0.5f;
            dl->AddText ( ImVec2 ( tx, ty ), IM_COL32 ( 255, 255, 255, 220 ), segs[i].label );
        }

        if ( mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y ) {
            hovered_seg = i;
            dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
        }

        x += w;
    }

    dl->AddRect ( origin, ImVec2 ( origin.x + bar_width, origin.y + FAT_LAYOUT_BAR_HEIGHT ),
                  IM_COL32 ( 80, 80, 80, 255 ) );

    ImGui::Dummy ( ImVec2 ( bar_width, FAT_LAYOUT_BAR_HEIGHT ) );

    if ( hovered_seg >= 0 ) {
        ImGui::BeginTooltip ();
        ImGui::Text ( "%s", segs[hovered_seg].label );
        ImGui::Text ( "%u %s (%u B)", segs[hovered_seg].blocks,
                      _ ( "blocks" ), segs[hovered_seg].blocks * FSMRS_SECTOR_SIZE );
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
                      100.0f * (float) segs[i].blocks / (float) total );
        ImGui::SameLine ( 0, 20 );
    }
    ImGui::NewLine ();
}


/**
 * @brief Vykreslí legendu souborů a strukturálních typů.
 *
 * Pro každý soubor zobrazí barevný čtvereček s "NAME.EXT (N blocks)".
 * Pod soubory zobrazí strukturální typy.
 *
 * @param data Datový model.
 */
static void render_fat_legend ( const st_PANEL_MRS_DATA *data )
{
    ImGui::SeparatorText ( _ ( "Legend" ) );

    ImDrawList *dl = ImGui::GetWindowDrawList ();
    g_mrs_legend_sel = 0;

    /* soubory jako klikací linky */
    for ( int i = 0; i < data->file_count; i++ ) {
        const st_PANEL_MRS_FILE *f = &data->files[i];
        ImVec2 pos = ImGui::GetCursorScreenPos ();
        dl = ImGui::GetWindowDrawList ();
        ImU32 col = file_id_color ( f->file_id );

        dl->AddRectFilled ( pos, ImVec2 ( pos.x + 16, pos.y + 16 ), col );
        ImGui::Dummy ( ImVec2 ( 18, 18 ) );
        ImGui::SameLine ();

        /* jméno souboru jako link */
        char label[32];
        snprintf ( label, sizeof ( label ), "%s.%s (%u blk)", f->name, f->ext, f->bsize );

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
            g_mrs_legend_sel = f->file_id;
            dl->AddRect ( pos, ImVec2 ( pos.x + 16, pos.y + 16 ),
                          IM_COL32 ( 255, 255, 255, 255 ), 0.0f, 0, 2.0f );
        }

        ImGui::SameLine ( 0, 16 );
    }

    if ( data->file_count > 0 ) ImGui::NewLine ();

    /* strukturální typy */
    struct { const char *label; ImU32 color; int count; } types[] = {
        { "System", COL_SYSTEM, data->fat_stats.sys_blocks },
        { "FAT",    COL_FAT,    data->fat_stats.fat_blocks },
        { "Dir",    COL_DIR,    data->fat_stats.dir_blocks },
        { _ ( "Free" ), COL_FREE, data->fat_stats.free_blocks },
        { _ ( "Bad" ),  COL_BAD,  data->fat_stats.bad_blocks },
    };

    for ( int i = 0; i < 5; i++ ) {
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
 * @brief Vykreslí blokovou mřížku FAT mapy s per-file barvami a hover tooltipem.
 *
 * Každý blok je malý obdélník obarvený podle vlastníka (file_id).
 * Hover tooltip zobrazuje: blok#, stopa/sektor, FAT hodnotu (hex), jméno souboru.
 *
 * @param data Datový model s fat_raw[].
 */
static void render_fat_grid ( const st_PANEL_MRS_DATA *data )
{
    ImGui::SeparatorText ( _ ( "MRS FAT Map" ) );

    ImVec2 avail = ImGui::GetContentRegionAvail ();
    if ( !ImGui::BeginChild ( "##mrs_fat_grid", avail, ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar ) ) {
        ImGui::EndChild ();
        return;
    }

    avail = ImGui::GetContentRegionAvail ();
    float step = FAT_CELL_SIZE + FAT_CELL_GAP;
    int cols = (int) ( avail.x / step );
    if ( cols < 1 ) cols = 1;

    ImVec2 origin = ImGui::GetCursorScreenPos ();
    ImDrawList *dl = ImGui::GetWindowDrawList ();
    ImVec2 mouse = ImGui::GetMousePos ();
    int hovered_block = -1;

    for ( int i = 0; i < FSMRS_COUNT_BLOCKS; i++ ) {
        int col = i % cols;
        int row = i / cols;

        ImVec2 p0 = ImVec2 ( origin.x + col * step, origin.y + row * step );
        ImVec2 p1 = ImVec2 ( p0.x + FAT_CELL_SIZE, p0.y + FAT_CELL_SIZE );

        ImU32 color = fat_block_color ( data->fat_raw[i] );

        /* ztlumit bloky nepatřící zvýrazněnému souboru */
        if ( g_mrs_legend_sel != 0 ) {
            if ( data->fat_raw[i] == g_mrs_legend_sel ) {
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
            if ( g_mrs_legend_sel == 0 ) {
                dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
            }
        }
    }

    int total_rows = ( FSMRS_COUNT_BLOCKS + cols - 1 ) / cols;
    ImGui::Dummy ( ImVec2 ( avail.x, total_rows * step ) );

    /* tooltip */
    if ( hovered_block >= 0 ) {
        int track = hovered_block / FSMRS_SECTORS_ON_TRACK;
        int sector = ( hovered_block % FSMRS_SECTORS_ON_TRACK ) + 1;
        uint8_t fat_val = data->fat_raw[hovered_block];

        ImGui::BeginTooltip ();
        ImGui::Text ( "Block %d (Track %d, Sector %d)", hovered_block, track, sector );
        ImGui::Text ( "FAT: 0x%02X", fat_val );

        /* identifikace obsahu */
        switch ( fat_val ) {
            case FSMRS_FAT_FREE:   ImGui::Text ( "%s", _ ( "Free" ) ); break;
            case FSMRS_FAT_FAT:    ImGui::Text ( "FAT" ); break;
            case FSMRS_FAT_DIR:    ImGui::Text ( "Directory" ); break;
            case FSMRS_FAT_BAD:    ImGui::Text ( "%s", _ ( "Bad sector" ) ); break;
            case FSMRS_FAT_SYSTEM: ImGui::Text ( "System" ); break;
            default: {
                char fname_buf[16];
                if ( find_file_name ( data, fat_val, fname_buf, sizeof ( fname_buf ) ) ) {
                    ImGui::Text ( "File: %s (ID %d)", fname_buf, fat_val );
                } else {
                    ImGui::Text ( "File ID %d", fat_val );
                }
            } break;
        }

        ImGui::EndTooltip ();
    }

    ImGui::EndChild ();
}


/* =========================================================================
 * Directory tab - rendering
 * ========================================================================= */

/**
 * @brief Vykreslí toolbar s tlačítky Get/Put/Delete/Rename.
 */
static void render_toolbar ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                             st_MZDSK_DETECT_RESULT *detect, bool *is_dirty, st_MZDISK_CONFIG *cfg )
{
    (void) config; (void) detect; (void) is_dirty;
    int sel_count = count_selected ( data );

    /* --- Get (Export) - raw tělo souboru bez MZF hlavičky --- */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Get (Export)" ) ) ) {
        IGFD::FileDialogConfig fconfig;
        fconfig.countSelectionMax = 1;
        fconfig.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles;
        fconfig.path = cfg->last_get_dir;

        if ( sel_count == 1 ) {
            fconfig.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
            for ( int i = 0; i < data->file_count; i++ ) {
                if ( data->selected[i] ) {
                    char fname[16];
                    format_fullname ( &data->files[i], fname, sizeof ( fname ) );
                    fconfig.fileName = fname;
                    break;
                }
            }
            ImGuiFileDialog::Instance ()->OpenDialog ( MRS_GET_RAW_DLG, _ ( "Export file" ), "((.*))", fconfig );
        } else {
            ImGuiFileDialog::Instance ()->OpenDialog ( MRS_GET_RAW_DLG, _ ( "Export to directory" ), nullptr, fconfig );
        }
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* --- Put (Import) - raw import s dialogem (NAME.EXT + STRT + EXEC) --- */
    if ( ImGui::Button ( _ ( "Put (Import)" ) ) ) {
        IGFD::FileDialogConfig fconfig;
        fconfig.countSelectionMax = 0;
        fconfig.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles;
        fconfig.path = cfg->last_put_dir;
        ImGuiFileDialog::Instance ()->OpenDialog ( MRS_PUT_RAW_DLG, _ ( "Import file" ), "((.*))", fconfig );
    }

    ImGui::SameLine ( 0, 10 );

    /* --- Get MZF: sel_count==1 otevře options popup s name/STRT/EXEC;
       sel_count>1 rovnou bulk directory picker s hodnotami z MRS entry. --- */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Get MZF" ) ) ) {
        if ( sel_count == 1 ) {
            /* Inicializovat options z MRS entry vybraného souboru. */
            for ( int i = 0; i < data->file_count; i++ ) {
                if ( data->selected[i] ) {
                    memset ( &data->mzf_export, 0, sizeof ( data->mzf_export ) );
                    data->mzf_export.file_idx = i;
                    data->mzf_export.fstrt = data->files[i].start_addr;
                    data->mzf_export.fexec = data->files[i].exec_addr;
                    char fname[16];
                    format_fullname ( &data->files[i], fname, sizeof ( fname ) );
                    snprintf ( data->mzf_export.name, sizeof ( data->mzf_export.name ),
                               "%s", fname );
                    data->mzf_export.show_opts = true;
                    break;
                }
            }
        } else {
            /* Multi-file: rovnou directory picker, hodnoty z MRS entries. */
            IGFD::FileDialogConfig fconfig;
            fconfig.countSelectionMax = 1;
            fconfig.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles;
            fconfig.path = cfg->last_get_dir;
            ImGuiFileDialog::Instance ()->OpenDialog ( MRS_GET_DLG, _ ( "Export MZFs to directory" ), nullptr, fconfig );
        }
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* --- Put MZF --- */
    if ( ImGui::Button ( _ ( "Put MZF" ) ) ) {
        IGFD::FileDialogConfig fconfig;
        fconfig.countSelectionMax = 0;
        fconfig.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles;
        fconfig.path = cfg->last_put_dir;
        ImGuiFileDialog::Instance ()->OpenDialog ( MRS_PUT_DLG, _ ( "Import MZF File" ), ".mzf", fconfig );
    }

    ImGui::SameLine ( 0, 10 );

    /* --- Delete --- */
    if ( sel_count == 0 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Delete" ) ) ) {
        g_mrs_pending_delete = true;
    }
    if ( sel_count == 0 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* --- Rename (jen pro 1 soubor) --- */
    if ( sel_count != 1 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Rename" ) ) ) {
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] ) {
                g_mrs_rename_index = i;
                format_fullname ( &data->files[i], g_mrs_rename_buf, sizeof ( g_mrs_rename_buf ) );
                g_mrs_pending_rename = true;
                break;
            }
        }
    }
    if ( sel_count != 1 ) ImGui::EndDisabled ();

    ImGui::SameLine ( 0, 10 );

    /* --- Set Addr (jen pro 1 soubor) - editace STRT/EXEC v directory entry --- */
    if ( sel_count != 1 ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Set Addr" ) ) ) {
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] ) {
                data->set_addr.file_idx = i;
                data->set_addr.fstrt = data->files[i].start_addr;
                data->set_addr.fexec = data->files[i].exec_addr;
                data->set_addr.show = true;
                break;
            }
        }
    }
    if ( sel_count != 1 ) ImGui::EndDisabled ();
    /* "(N selected)" indikátor je v status řádku nad toolbarem. */
}


/**
 * @brief Vykreslí obsah directory tabu (tabulka souborů + detail).
 */
static void render_directory_tab ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                                   st_MZDSK_DETECT_RESULT *detect, bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                   uint64_t owner_session_id )
{
    /* Status řádek: Free blocks nejdřív, pak Files: X/Y (N selected).
       "(N selected)" stejný styl jako v CP/M directory tabu, zobrazuje
       i 0. Dříve byl "(N selected)" až za tlačítky Rename v toolbaru. */
    int dir_sel_count = count_selected ( data );
    ImGui::Text ( "%s: %d", _ ( "Free blocks" ), data->free_blocks );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d / %d", _ ( "Files" ), data->file_count, data->max_files );
    ImGui::SameLine ( 0, 8 );
    ImGui::TextDisabled ( "(%d %s)", dir_sel_count, _ ( "selected" ) );
    ImGui::Spacing ();
    render_toolbar ( data, config, detect, is_dirty, cfg );
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
            /* target_cpm_user = -1: MRS cíl CP/M user parametr ignoruje. */
            en_MZDSK_RES r = dnd_handle_drop ( payload, owner_session_id,
                                                is_move, cfg->dnd_dup_mode,
                                                -1,
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
            dnd_try_accept_rect ( r, "##mrs_dnd_empty" );
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

    if ( ImGui::BeginTable ( "mrs_dir", 8, flags ) ) {

        ImGui::TableSetupScrollFreeze ( 0, 1 );
        ImGui::TableSetupColumn ( "##sel",         ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 30.0f );
        ImGui::TableSetupColumn ( _ ( "ID" ),      ImGuiTableColumnFlags_WidthFixed, 30.0f );
        ImGui::TableSetupColumn ( _ ( "Name" ),    ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn ( _ ( "Ext" ),     ImGuiTableColumnFlags_WidthFixed, 50.0f );
        ImGui::TableSetupColumn ( _ ( "Blocks" ),  ImGuiTableColumnFlags_WidthFixed, 60.0f );
        ImGui::TableSetupColumn ( _ ( "Size" ),    ImGuiTableColumnFlags_WidthFixed, 80.0f );
        ImGui::TableSetupColumn ( _ ( "Start" ),   ImGuiTableColumnFlags_WidthFixed, 70.0f );
        ImGui::TableSetupColumn ( _ ( "Exec" ),    ImGuiTableColumnFlags_WidthFixed, 70.0f );

        /* záhlaví s Select All */
        ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::PushID ( "mrs_sel_all" );
        int sel_count = count_selected ( data );
        bool all_selected = ( sel_count == data->file_count && data->file_count > 0 );
        if ( ImGui::Checkbox ( "##selall", &all_selected ) ) {
            for ( int i = 0; i < data->file_count; i++ ) data->selected[i] = all_selected;
        }
        ImGui::PopID ();
        for ( int col = 1; col < 8; col++ ) {
            ImGui::TableSetColumnIndex ( col );
            ImGui::TableHeader ( ImGui::TableGetColumnName ( col ) );
        }

        for ( int i = 0; i < data->file_count; i++ ) {
            st_PANEL_MRS_FILE *f = &data->files[i];

            ImGui::TableNextRow ();

            if ( data->selected[i] ) {
                ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg0, SELECTED_ROW_COLOR );
                ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg1, SELECTED_ROW_COLOR );
            }

            ImGui::TableNextColumn ();
            char cb_id[16];
            snprintf ( cb_id, sizeof ( cb_id ), "##mcb%d", i );
            ImGui::Checkbox ( cb_id, &data->selected[i] );

            ImGui::TableNextColumn ();
            ImGui::Text ( "%d", f->file_id );

            ImGui::TableNextColumn ();
            char label[32];
            snprintf ( label, sizeof ( label ), "%s##m%d", f->name, i );
            if ( ImGui::Selectable ( label, data->selected[i],
                                     ImGuiSelectableFlags_SpanAllColumns
                                     | ImGuiSelectableFlags_AllowOverlap ) ) {
                data->selected[i] = !data->selected[i];
                data->detail_index = data->selected[i] ? i : -1;
            }
            if ( owner_session_id != 0 && ImGui::BeginDragDropSource ( ImGuiDragDropFlags_None ) ) {
                st_DND_FILE_PAYLOAD payload;
                dnd_fill_payload ( &payload, owner_session_id, MZDSK_FS_MRS,
                                    i, data->selected, data->file_count );
                ImGui::SetDragDropPayload ( DND_PAYLOAD_FILE, &payload, sizeof ( payload ) );
                const char *verb = ImGui::GetIO ().KeyCtrl ? _ ( "Move:" ) : _ ( "Copy:" );
                if ( payload.count > 1 ) {
                    ImGui::Text ( "%s %d %s", verb, payload.count, _ ( "files" ) );
                } else {
                    ImGui::Text ( "%s %s.%s", verb, f->name, f->ext );
                }
                ImGui::EndDragDropSource ();
            }

            ImGui::TableNextColumn ();
            ImGui::Text ( "%s", f->ext );

            ImGui::TableNextColumn ();
            ImGui::Text ( "%u", f->bsize );

            ImGui::TableNextColumn ();
            ImGui::Text ( "%u B", (unsigned) f->size_bytes );

            ImGui::TableNextColumn ();
            ImGui::Text ( "%04Xh", f->start_addr );

            ImGui::TableNextColumn ();
            ImGui::Text ( "%04Xh", f->exec_addr );
        }

        ImGui::EndTable ();
    }

    /* Drop target přes celou tabulku (BeginDragDropTargetCustom). */
    ImVec2 table_max = ImGui::GetCursorScreenPos ();
    float win_right = ImGui::GetWindowPos ().x + ImGui::GetWindowContentRegionMax ().x;
    if ( table_max.y > table_min.y ) {
        ImRect r ( table_min, ImVec2 ( win_right, table_max.y ) );
        dnd_try_accept_rect ( r, "##mrs_dnd_table" );
    }

    /* detail */
    if ( data->detail_index >= 0 && data->detail_index < data->file_count ) {
        st_PANEL_MRS_FILE *f = &data->files[data->detail_index];

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        ImGui::Text ( "%s: %s.%s", _ ( "File" ), f->name, f->ext );
        ImGui::Text ( "%s: %d", _ ( "File ID" ), f->file_id );
        ImGui::Text ( "%s: %u (%u B)", _ ( "Blocks" ), f->bsize, (unsigned) f->size_bytes );
        ImGui::Text ( "%s: 0x%04X", _ ( "Start address" ), f->start_addr );
        ImGui::Text ( "%s: 0x%04X", _ ( "Exec address" ), f->exec_addr );
    }
}


/**
 * @brief Vykreslí obsah FAT Map tabu.
 */
static void render_fat_map_tab ( const st_PANEL_MRS_DATA *data )
{
    render_fat_stats ( data );
    ImGui::Spacing ();
    render_fat_layout ( data );
    ImGui::Spacing ();
    render_fat_legend ( data );
    ImGui::Spacing ();
    render_fat_grid ( data );
}


/* =========================================================================
 * Popupy a dialogy (sdílené)
 * ========================================================================= */

static void process_dialogs ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                              st_MZDSK_DETECT_RESULT *detect,
                              bool *is_dirty, st_MZDISK_CONFIG *cfg )
{
    /* --- Get raw dialog (tělo souboru bez MZF hlavičky) --- */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( MRS_GET_RAW_DLG, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            int sel_count = count_selected ( data );
            if ( sel_count == 1 ) {
                std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
                for ( int i = 0; i < data->file_count; i++ ) {
                    if ( data->selected[i] ) {
                        en_MZDSK_RES gr = panel_mrs_get_file ( config, &data->files[i], path.c_str () );
                        if ( gr != MZDSK_RES_OK ) {
                            char fname[16];
                            format_fullname ( &data->files[i], fname, sizeof ( fname ) );
                            snprintf ( data->error_msg, sizeof ( data->error_msg ),
                                       "%s: %s", fname, mzdsk_get_error ( gr ) );
                            data->has_error = true;
                        }
                        break;
                    }
                }
            } else {
                std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();
                bulk_get_start ( data, config, cfg, /*is_mzf=*/false, dirpath.c_str () );
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_get_dir, dir.c_str (), sizeof ( cfg->last_get_dir ) - 1 );
            cfg->last_get_dir[sizeof ( cfg->last_get_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* --- Get MZF dialog ---
       Single file: user prošel přes options popup (data->mzf_export
       má name/fstrt/fexec), file picker je s filter .mzf a předvyplněným
       jménem. Zavoláme panel_mrs_get_file_mzf_ex s override hodnotami.
       Multi-file: directory picker, bulk_get_start(is_mzf=true). */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( MRS_GET_DLG, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            int sel_count = count_selected ( data );

            if ( sel_count == 1 ) {
                std::string path = ImGuiFileDialog::Instance ()->GetFilePathName ();
                int idx = data->mzf_export.file_idx;
                if ( idx >= 0 && idx < data->file_count && data->selected[idx] ) {
                    en_MZDSK_RES gr = panel_mrs_get_file_mzf_ex ( config, &data->files[idx],
                                                                    path.c_str (),
                                                                    data->mzf_export.name,
                                                                    (uint16_t) data->mzf_export.fstrt,
                                                                    (uint16_t) data->mzf_export.fexec );
                    if ( gr != MZDSK_RES_OK ) {
                        char fname[16];
                        format_fullname ( &data->files[idx], fname, sizeof ( fname ) );
                        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                                   "%s.mzf: %s", fname, mzdsk_get_error ( gr ) );
                        data->has_error = true;
                    }
                }
            } else {
                std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();
                bulk_get_start ( data, config, cfg, /*is_mzf=*/true, dirpath.c_str () );
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_get_dir, dir.c_str (), sizeof ( cfg->last_get_dir ) - 1 );
            cfg->last_get_dir[sizeof ( cfg->last_get_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* Put raw dialog - single-select otevře "Put as..." popup. */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( MRS_PUT_RAW_DLG, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            auto selection = ImGuiFileDialog::Instance ()->GetSelection ();
            if ( !selection.empty () ) {
                auto it = selection.begin ();
                strncpy ( g_mrs_put_src_path, it->second.c_str (),
                          sizeof ( g_mrs_put_src_path ) - 1 );
                g_mrs_put_src_path[sizeof ( g_mrs_put_src_path ) - 1] = '\0';

                const char *bname = strrchr ( it->first.c_str (), '/' );
                if ( !bname ) bname = strrchr ( it->first.c_str (), '\\' );
                if ( bname ) bname++; else bname = it->first.c_str ();
                strncpy ( g_mrs_put_target_buf, bname,
                          sizeof ( g_mrs_put_target_buf ) - 1 );
                g_mrs_put_target_buf[sizeof ( g_mrs_put_target_buf ) - 1] = '\0';

                g_mrs_put_is_mzf = false;
                g_mrs_put_auto_truncate = false;
                g_mrs_put_fstrt = 0;
                g_mrs_put_fexec = 0;
                g_mrs_put_pending_open = true;
            }

            std::string dir = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            strncpy ( cfg->last_put_dir, dir.c_str (), sizeof ( cfg->last_put_dir ) - 1 );
            cfg->last_put_dir[sizeof ( cfg->last_put_dir ) - 1] = '\0';
            mzdisk_config_save ( cfg );
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* Put MZF dialog - single-select otevře "Put as...", multi-select
       použije tolerantní cestu (jméno z MZF hlavičky). */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( MRS_PUT_DLG, ImGuiWindowFlags_NoCollapse ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            auto selection = ImGuiFileDialog::Instance ()->GetSelection ();

            if ( selection.size () == 1 ) {
                auto it = selection.begin ();
                strncpy ( g_mrs_put_src_path, it->second.c_str (),
                          sizeof ( g_mrs_put_src_path ) - 1 );
                g_mrs_put_src_path[sizeof ( g_mrs_put_src_path ) - 1] = '\0';

                /* Resetovat charset na EU default a pokusit se odvodit jméno
                   z MZF hlavičky. Při neúspěchu fallback na basename bez .mzf. */
                g_mrs_put_charset = MZF_NAME_ASCII_EU;
                bool derived = mrs_derive_name_from_mzf ( g_mrs_put_src_path,
                    g_mrs_put_charset, g_mrs_put_target_buf,
                    sizeof ( g_mrs_put_target_buf ) );

                if ( !derived ) {
                    const char *bname = strrchr ( it->first.c_str (), '/' );
                    if ( !bname ) bname = strrchr ( it->first.c_str (), '\\' );
                    if ( bname ) bname++; else bname = it->first.c_str ();
                    strncpy ( g_mrs_put_target_buf, bname,
                              sizeof ( g_mrs_put_target_buf ) - 1 );
                    g_mrs_put_target_buf[sizeof ( g_mrs_put_target_buf ) - 1] = '\0';
                    size_t tlen = strlen ( g_mrs_put_target_buf );
                    if ( tlen > 4 && strcasecmp ( g_mrs_put_target_buf + tlen - 4, ".mzf" ) == 0 ) {
                        g_mrs_put_target_buf[tlen - 4] = '\0';
                    }
                }

                g_mrs_put_is_mzf = true;
                g_mrs_put_auto_truncate = false;
                g_mrs_put_pending_open = true;
            } else {
                bool any_ok = false;
                char errors[512] = "";
                int err_len = 0;

                for ( auto &sel : selection ) {
                    const char *bname = strrchr ( sel.first.c_str (), '/' );
                    if ( !bname ) bname = strrchr ( sel.first.c_str (), '\\' );
                    if ( bname ) bname++; else bname = sel.first.c_str ();

                    en_MZDSK_RES res = panel_mrs_put_file_mzf ( config, sel.second.c_str () );
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
                    panel_mrs_load ( data, detect );
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

    /* === "Put file as..." popup === */
    if ( g_mrs_put_pending_open ) {
        ImGui::OpenPopup ( _ ( "Put file as..." ) );
        g_mrs_put_pending_open = false;
        g_mrs_put_last_err = MZDSK_NAMEVAL_OK;
        g_mrs_put_bad_char = 0;
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
        ImGui::TextDisabled ( "%s", g_mrs_put_src_path );

        ImGui::Spacing ();
        ImGui::TextUnformatted ( _ ( "Target name:" ) );
        ImGui::SameLine ( 120 );
        ImGui::SetNextItemWidth ( 180 );

        char tmp_name[9], tmp_ext[4];
        g_mrs_put_last_err = mzdsk_validate_83_name ( g_mrs_put_target_buf,
            MZDSK_NAMEVAL_FLAVOR_MRS, tmp_name, tmp_ext, &g_mrs_put_bad_char );

        bool is_valid = ( g_mrs_put_last_err == MZDSK_NAMEVAL_OK );
        bool length_only = ( g_mrs_put_last_err == MZDSK_NAMEVAL_NAME_TOO_LONG
                          || g_mrs_put_last_err == MZDSK_NAMEVAL_EXT_TOO_LONG );

        ImVec4 border_color = is_valid
            ? ImVec4 ( 0.2f, 0.8f, 0.2f, 1.0f )
            : ImVec4 ( 0.9f, 0.3f, 0.3f, 1.0f );
        ImGui::PushStyleColor ( ImGuiCol_Border, border_color );
        ImGui::PushStyleVar ( ImGuiStyleVar_FrameBorderSize, 2.0f );

        /* Fokus a select-all buď při prvním zobrazení dialogu, nebo po změně
           charset roletky. */
        ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue;
        if ( ImGui::IsWindowAppearing () || g_mrs_put_refocus_name ) {
            ImGui::SetKeyboardFocusHere ();
            input_flags |= ImGuiInputTextFlags_AutoSelectAll;
            g_mrs_put_refocus_name = false;
        }
        bool enter_pressed = ImGui::InputText ( "##mrs_put_target",
            g_mrs_put_target_buf, sizeof ( g_mrs_put_target_buf ),
            input_flags );

        ImGui::PopStyleVar ();
        ImGui::PopStyleColor ();

        if ( !is_valid && ImGui::IsItemHovered () ) {
            char err_msg[128];
            mrs_put_format_error ( g_mrs_put_last_err, g_mrs_put_bad_char,
                                    err_msg, sizeof ( err_msg ) );
            ImGui::SetTooltip ( "%s", err_msg );
        }

        /* Varianta Sharp MZ znakové sady pro odvození jména z MZF hlavičky.
           Jen pro Put MZF (raw put nepoužívá konverzi). Po změně se jméno
           přegeneruje a v dalším frame se InputText zaostří + vybere. */
        if ( g_mrs_put_is_mzf ) {
            ImGui::Spacing ();
            ImGui::TextUnformatted ( _ ( "MZF fname charset:" ) );
            ImGui::SameLine ( 120 );
            ImGui::SetNextItemWidth ( 180 );
            const char *cs_items[] = { "EU (MZ-700/800)", "JP (MZ-1500)" };
            int cs_idx = ( g_mrs_put_charset == MZF_NAME_ASCII_JP ) ? 1 : 0;
            if ( ImGui::Combo ( "##mrs_put_charset", &cs_idx, cs_items,
                                IM_ARRAYSIZE ( cs_items ) ) ) {
                g_mrs_put_charset = ( cs_idx == 1 )
                    ? MZF_NAME_ASCII_JP : MZF_NAME_ASCII_EU;
                mrs_derive_name_from_mzf ( g_mrs_put_src_path,
                    g_mrs_put_charset, g_mrs_put_target_buf,
                    sizeof ( g_mrs_put_target_buf ) );
                g_mrs_put_refocus_name = true;
            }
        }

        /* STRT a EXEC adresy - jen pro raw put (MZF put je bere z MZF
           hlavičky); uživatel je může editovat v hex formátu. */
        if ( !g_mrs_put_is_mzf ) {
            ImGui::Spacing ();
            ImGui::Text ( "STRT:" );
            ImGui::SameLine ( 120 );
            ImGui::SetNextItemWidth ( 80 );
            {
                char hex[5];
                snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) g_mrs_put_fstrt & 0xFFFF );
                if ( ImGui::InputText ( "##mrs_put_strt", hex, sizeof ( hex ),
                                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                    unsigned v = 0;
                    if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) g_mrs_put_fstrt = (int) v;
                }
            }
            ImGui::SameLine ();
            ImGui::TextDisabled ( "(0x0000-0xFFFF)" );

            ImGui::Text ( "EXEC:" );
            ImGui::SameLine ( 120 );
            ImGui::SetNextItemWidth ( 80 );
            {
                char hex[5];
                snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) g_mrs_put_fexec & 0xFFFF );
                if ( ImGui::InputText ( "##mrs_put_exec", hex, sizeof ( hex ),
                                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                    unsigned v = 0;
                    if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) g_mrs_put_fexec = (int) v;
                }
            }
            ImGui::SameLine ();
            ImGui::TextDisabled ( "(0x0000-0xFFFF)" );
        }

        ImGui::Spacing ();
        ImGui::Checkbox ( _ ( "Auto-truncate on overflow" ), &g_mrs_put_auto_truncate );

        ImGui::Spacing ();
        if ( is_valid ) {
            ImGui::TextDisabled ( "%s", _ ( "Hint: 8 chars max for name, 3 for ext." ) );
        } else {
            char err_msg[128];
            mrs_put_format_error ( g_mrs_put_last_err, g_mrs_put_bad_char,
                                    err_msg, sizeof ( err_msg ) );
            ImGui::TextColored ( ImVec4 ( 0.9f, 0.3f, 0.3f, 1.0f ), "%s", err_msg );
        }

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        bool ok_enabled = is_valid || ( g_mrs_put_auto_truncate && length_only );

        if ( !ok_enabled ) ImGui::BeginDisabled ();
        bool do_ok = ButtonMinWidth ( "OK" ) || ( ok_enabled && enter_pressed );
        if ( !ok_enabled ) ImGui::EndDisabled ();

        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            g_mrs_put_src_path[0] = '\0';
            ImGui::CloseCurrentPopup ();
        }

        if ( do_ok ) {
            char final_name[9], final_ext[4];
            if ( is_valid ) {
                strncpy ( final_name, tmp_name, sizeof ( final_name ) );
                strncpy ( final_ext, tmp_ext, sizeof ( final_ext ) );
            } else {
                mrs_truncate_name ( g_mrs_put_target_buf, final_name, final_ext );
            }

            en_MZDSK_RES res;
            if ( g_mrs_put_is_mzf ) {
                res = panel_mrs_put_file_mzf_ex ( config, g_mrs_put_src_path,
                    final_name, final_ext );
            } else {
                res = panel_mrs_put_file ( config, g_mrs_put_src_path,
                    final_name, final_ext,
                    (uint16_t) g_mrs_put_fstrt,
                    (uint16_t) g_mrs_put_fexec );
            }

            if ( res == MZDSK_RES_OK ) {
                *is_dirty = true;
                panel_mrs_load ( data, detect );
            } else {
                const char *bname = strrchr ( g_mrs_put_src_path, '/' );
                if ( !bname ) bname = strrchr ( g_mrs_put_src_path, '\\' );
                if ( bname ) bname++; else bname = g_mrs_put_src_path;
                data->has_error = true;
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "%.200s: %s", bname, mzdsk_get_error ( res ) );
            }

            g_mrs_put_src_path[0] = '\0';
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    }
    ImGui::PopStyleVar (); /* WindowPadding */
}


static void process_popups ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                             st_MZDSK_DETECT_RESULT *detect, bool *is_dirty,
                             st_MZDISK_CONFIG *cfg )
{
    /* Delete confirmation */
    if ( g_mrs_pending_delete ) {
        ImGui::OpenPopup ( "##mrs_delete" );
        g_mrs_pending_delete = false;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_delete", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        int del_count = count_selected ( data );
        ImGui::Text ( "%s %d %s?", _ ( "Delete" ), del_count, _ ( "file(s)" ) );
        ImGui::Spacing ();

        /* Velký počet zabalit do scrollable child regionu. */
        const int inline_limit = 8;
        bool use_scroll = ( del_count > inline_limit );
        if ( use_scroll ) {
            ImGui::BeginChild ( "##mrs_del_list",
                                ImVec2 ( 420.0f, 180.0f ),
                                ImGuiChildFlags_Borders );
        }
        for ( int i = 0; i < data->file_count; i++ ) {
            if ( data->selected[i] ) {
                ImGui::BulletText ( "%s.%s", data->files[i].name, data->files[i].ext );
            }
        }
        if ( use_scroll ) {
            ImGui::EndChild ();
        }
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Delete" ) ) ) {
            bool any_ok = false;
            char errors[512] = "";
            int err_len = 0;

            for ( int i = data->file_count - 1; i >= 0; i-- ) {
                if ( !data->selected[i] ) continue;
                en_MZDSK_RES res = panel_mrs_delete_file ( config, &data->files[i] );
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
                panel_mrs_load ( data, detect );
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
    if ( g_mrs_pending_rename ) {
        ImGui::OpenPopup ( "##mrs_rename" );
        g_mrs_pending_rename = false;
    }

    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_rename", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        if ( g_mrs_rename_index >= 0 && g_mrs_rename_index < data->file_count ) {
            st_PANEL_MRS_FILE *f = &data->files[g_mrs_rename_index];
            ImGui::Text ( "%s: %s.%s", _ ( "Current name" ), f->name, f->ext );
            ImGui::Spacing ();
            ImGui::Text ( "%s:", _ ( "New name (NAME.EXT)" ) );
            ImGui::SameLine ();
            ImGui::SetNextItemWidth ( 200 );
            if ( ImGui::IsWindowAppearing () ) ImGui::SetKeyboardFocusHere ();
            bool enter = ImGui::InputText ( "##mrs_ren_input", g_mrs_rename_buf,
                                            sizeof ( g_mrs_rename_buf ),
                                            ImGuiInputTextFlags_EnterReturnsTrue );
            ImGui::Spacing ();
            ImGui::Separator ();
            ImGui::Spacing ();

            bool do_rename = enter || ButtonMinWidth ( "OK" );
            if ( do_rename && g_mrs_rename_buf[0] != '\0' ) {
                char new_name[9] = "", new_ext[4] = "";
                const char *dot = strchr ( g_mrs_rename_buf, '.' );
                if ( dot ) {
                    int nlen = (int) ( dot - g_mrs_rename_buf );
                    if ( nlen > 8 ) nlen = 8;
                    strncpy ( new_name, g_mrs_rename_buf, nlen );
                    strncpy ( new_ext, dot + 1, 3 );
                } else {
                    strncpy ( new_name, g_mrs_rename_buf, 8 );
                }

                en_MZDSK_RES res = panel_mrs_rename_file ( config, f,
                    new_name, dot ? new_ext : NULL );
                if ( res == MZDSK_RES_OK ) {
                    *is_dirty = true;
                    panel_mrs_load ( data, detect );
                } else {
                    data->has_error = true;
                    snprintf ( data->error_msg, sizeof ( data->error_msg ),
                               "%s: %s", g_mrs_rename_buf, mzdsk_get_error ( res ) );
                }
                g_mrs_rename_index = -1;
                ImGui::CloseCurrentPopup ();
            }
            if ( !do_rename ) {
                ImGui::SameLine ();
                if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
                    g_mrs_rename_index = -1;
                    ImGui::CloseCurrentPopup ();
                }
            }
        }
        ImGui::EndPopup ();
    }

    /* Error popup */
    if ( data->has_error ) {
        ImGui::OpenPopup ( "##mrs_error" );
        data->has_error = false;
    }

    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_error", nullptr,
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

    /* --- MZF Export Options (single-file Get MZF) --- */
    if ( data->mzf_export.show_opts ) {
        ImGui::OpenPopup ( "##mrs_mzf_opts" );
        data->mzf_export.show_opts = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_mzf_opts", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::TextColored ( ImVec4 ( 0.8f, 0.8f, 1.0f, 1.0f ), "%s", _ ( "MZF Export Options" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        ImGui::Text ( "%s:", _ ( "Name" ) );
        ImGui::SameLine ( 120 );
        ImGui::SetNextItemWidth ( 200 );
        ImGui::InputText ( "##mrs_mzf_name", data->mzf_export.name,
                            sizeof ( data->mzf_export.name ) );

        ImGui::Text ( "STRT:" );
        ImGui::SameLine ( 120 );
        ImGui::SetNextItemWidth ( 80 );
        {
            char hex[5];
            snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) data->mzf_export.fstrt & 0xFFFF );
            if ( ImGui::InputText ( "##mrs_mzf_strt", hex, sizeof ( hex ),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                unsigned v = 0;
                if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) data->mzf_export.fstrt = (int) v;
            }
        }
        ImGui::SameLine ();
        ImGui::TextDisabled ( "(0x0000-0xFFFF)" );

        ImGui::Text ( "EXEC:" );
        ImGui::SameLine ( 120 );
        ImGui::SetNextItemWidth ( 80 );
        {
            char hex[5];
            snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) data->mzf_export.fexec & 0xFFFF );
            if ( ImGui::InputText ( "##mrs_mzf_exec", hex, sizeof ( hex ),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                unsigned v = 0;
                if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) data->mzf_export.fexec = (int) v;
            }
        }
        ImGui::SameLine ();
        ImGui::TextDisabled ( "(0x0000-0xFFFF)" );

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Export.." ) ) ) {
            data->mzf_export.open_file_dlg = true;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            data->mzf_export.file_idx = -1;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Po confirm options otevřít file dialog. */
    if ( data->mzf_export.open_file_dlg ) {
        data->mzf_export.open_file_dlg = false;
        IGFD::FileDialogConfig fconfig;
        fconfig.countSelectionMax = 1;
        fconfig.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles
                      | ImGuiFileDialogFlags_ConfirmOverwrite;
        fconfig.path = cfg->last_get_dir;
        char mzfname[24];
        snprintf ( mzfname, sizeof ( mzfname ), "%s.mzf", data->mzf_export.name );
        fconfig.fileName = mzfname;
        ImGuiFileDialog::Instance ()->OpenDialog ( MRS_GET_DLG, _ ( "Export to MZF" ), ".mzf", fconfig );
    }

    /* --- Set Addr popup (STRT/EXEC editor) --- */
    if ( data->set_addr.show ) {
        ImGui::OpenPopup ( "##mrs_set_addr" );
        data->set_addr.show = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_set_addr", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        int sai = data->set_addr.file_idx;
        if ( sai >= 0 && sai < data->file_count ) {
            const st_PANEL_MRS_FILE *sf = &data->files[sai];
            ImGui::Text ( "%s: %s.%s", _ ( "File" ), sf->name, sf->ext );
            ImGui::Spacing ();

            ImGui::Text ( "STRT:" );
            ImGui::SameLine ( 80 );
            ImGui::SetNextItemWidth ( 80 );
            {
                char hex[5];
                snprintf ( hex, sizeof ( hex ), "%04X", (unsigned) data->set_addr.fstrt & 0xFFFF );
                if ( ImGui::InputText ( "##mrs_sa_strt", hex, sizeof ( hex ),
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
                if ( ImGui::InputText ( "##mrs_sa_exec", hex, sizeof ( hex ),
                                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase ) ) {
                    unsigned v = 0;
                    if ( sscanf ( hex, "%x", &v ) == 1 && v <= 0xFFFF ) data->set_addr.fexec = (int) v;
                }
            }
            ImGui::SameLine ();
            ImGui::TextDisabled ( "(0x0000-0xFFFF)" );

            ImGui::Spacing ();
            ImGui::Separator ();
            ImGui::Spacing ();

            if ( ButtonMinWidth ( "OK" ) ) {
                en_MZDSK_RES r = panel_mrs_set_addr ( config, sf,
                                                       (uint16_t) data->set_addr.fstrt,
                                                       (uint16_t) data->set_addr.fexec );
                if ( r == MZDSK_RES_OK ) {
                    *is_dirty = true;
                    panel_mrs_load ( data, detect );
                } else {
                    snprintf ( data->error_msg, sizeof ( data->error_msg ),
                               "%s.%s: %s", sf->name, sf->ext, mzdsk_get_error ( r ) );
                    data->has_error = true;
                }
                ImGui::CloseCurrentPopup ();
            }
            ImGui::SameLine ();
            if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
                ImGui::CloseCurrentPopup ();
            }
        } else {
            ImGui::TextDisabled ( "No file selected." );
            if ( ButtonMinWidth ( "OK" ) ) ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Bulk Get ASK popup (CP/M-style per-file i Apply-to-all akce). */
    render_bulk_get_ask_popup ( data, config, cfg );
}


/* =========================================================================
 * Hlavní render funkce
 * ========================================================================= */

extern "C" void panel_mrs_render ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                                    st_MZDSK_DETECT_RESULT *detect, bool *is_dirty, st_MZDISK_CONFIG *cfg,
                                    uint64_t owner_session_id )
{
    if ( !data || !data->is_loaded ) {
        ImGui::TextDisabled ( "%s", _ ( "No MRS directory data." ) );
        return;
    }

    if ( ImGui::BeginTabBar ( "##mrs_tabs" ) ) {

        if ( TabItemWithTooltip ( _ ( "Directory" ) ) ) {
            render_directory_tab ( data, config, detect, is_dirty, cfg, owner_session_id );
            ImGui::EndTabItem ();
        }

        if ( TabItemWithTooltip ( _ ( "FAT Map" ) ) ) {
            render_fat_map_tab ( data );
            ImGui::EndTabItem ();
        }

        ImGui::EndTabBar ();
    }

    /* popupy a dialogy jsou na úrovni okna, ne uvnitř tabu */
    process_popups ( data, config, detect, is_dirty, cfg );
    process_dialogs ( data, config, detect, is_dirty, cfg );
}


/* =========================================================================
 *  MRS Maintenance tab
 * ========================================================================= */


/** @brief Příznak: čekáme na potvrzení Defrag. */
static bool g_mrs_pending_defrag = false;

/** @brief Příznak: defragmentace právě probíhá. */
static bool g_mrs_defrag_running = false;

/** @brief Log defragmentace (progress callback). */
static char g_mrs_defrag_log[4096] = "";

/** @brief Délka textu v defrag logu. */
static int g_mrs_defrag_log_len = 0;

/* Async infrastruktura pro defrag (audit C-5) - viz analogický komentář
 * v panel_cpm_imgui.cpp. */
static SDL_Thread *g_mrs_defrag_thread = nullptr;
static SDL_Mutex *g_mrs_defrag_mutex = nullptr;
static SDL_AtomicInt g_mrs_defrag_done;
static en_MZDSK_RES g_mrs_defrag_result = MZDSK_RES_OK;

static struct mrs_defrag_ctx {
    st_MZDSK_DISC *disc;
    uint16_t fat_block;
} g_mrs_defrag_ctx;

/** @brief Příznak: čekáme na potvrzení Format File Area. */
static bool g_mrs_pending_format = false;

/** @brief Příznak: výsledek maintenance operace. */
static bool g_mrs_maint_result = false;

/** @brief Zpráva výsledku. */
static char g_mrs_maint_msg[512] = "";

/** @brief Příznak: výsledek je chyba. */
static bool g_mrs_maint_is_error = false;


/**
 * @brief Callback pro průběh defragmentace - ukládá zprávy do logu.
 *
 * @param message Textová zpráva.
 * @param user_data Nepoužívá se.
 */
static void mrs_defrag_progress_cb ( const char *message, void *user_data )
{
    (void) user_data;
    /* Volán z worker threadu - zápis do logu pod mutexem (audit C-5). */
    if ( g_mrs_defrag_mutex ) SDL_LockMutex ( g_mrs_defrag_mutex );
    /* Ochrana proti heap overflow: viz analogický komentář v
     * panel_cpm_imgui.cpp -> cpm_defrag_progress_cb. */
    size_t capacity = sizeof ( g_mrs_defrag_log );
    if ( (size_t) g_mrs_defrag_log_len >= capacity - 1 ) {
        if ( g_mrs_defrag_mutex ) SDL_UnlockMutex ( g_mrs_defrag_mutex );
        return;
    }
    size_t remaining = capacity - (size_t) g_mrs_defrag_log_len;
    int n = snprintf ( g_mrs_defrag_log + g_mrs_defrag_log_len, remaining, "%s", message );
    if ( n > 0 ) {
        if ( (size_t) n >= remaining ) {
            g_mrs_defrag_log_len = (int) ( capacity - 1 );
        } else {
            g_mrs_defrag_log_len += n;
        }
    }
    if ( g_mrs_defrag_mutex ) SDL_UnlockMutex ( g_mrs_defrag_mutex );
}


/**
 * @brief Worker thread entry point pro MRS defragmentaci (audit C-5).
 */
static int mrs_defrag_thread_fn ( void *user_data )
{
    (void) user_data;
    g_mrs_defrag_result = fsmrs_defrag ( g_mrs_defrag_ctx.disc,
                                           g_mrs_defrag_ctx.fat_block,
                                           mrs_defrag_progress_cb, nullptr );
    SDL_SetAtomicInt ( &g_mrs_defrag_done, 1 );
    return 0;
}


extern "C" void panel_mrs_maintenance_render ( st_PANEL_MRS_DATA *data,
                                                st_FSMRS_CONFIG *config,
                                                st_MZDSK_DETECT_RESULT *detect,
                                                bool *is_dirty )
{
    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();

    ImGui::TextWrapped ( "%s", _ ( "MRS disk maintenance operations." ) );
    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* --- Defrag --- */
    ImGui::Text ( "%s", _ ( "Defragmentation" ) );
    ImGui::TextWrapped ( "%s", _ ( "Reads all files into memory, formats file area, and writes files sequentially without gaps. System area is preserved." ) );
    ImGui::Spacing ();

    /* Polling worker threadu (audit C-5). */
    if ( g_mrs_defrag_running && SDL_GetAtomicInt ( &g_mrs_defrag_done ) ) {
        if ( g_mrs_defrag_thread ) {
            SDL_WaitThread ( g_mrs_defrag_thread, nullptr );
            g_mrs_defrag_thread = nullptr;
        }
        g_mrs_defrag_running = false;
        en_MZDSK_RES res = g_mrs_defrag_result;
        if ( res == MZDSK_RES_OK ) {
            fsmrs_init ( config->disc, config->fat_block, config );
            *is_dirty = true;
            panel_mrs_load ( data, detect );
            g_mrs_maint_is_error = false;
            snprintf ( g_mrs_maint_msg, sizeof ( g_mrs_maint_msg ),
                       "%s", _ ( "Defragmentation completed successfully." ) );
        } else {
            g_mrs_maint_is_error = true;
            snprintf ( g_mrs_maint_msg, sizeof ( g_mrs_maint_msg ),
                       "%s: %s", _ ( "Defragmentation failed" ), mzdsk_get_error ( res ) );
        }
        g_mrs_maint_result = true;
    }

    if ( g_mrs_defrag_running ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( _ ( "Defragment" ) ) ) {
        g_mrs_pending_defrag = true;
    }
    if ( g_mrs_defrag_running ) ImGui::EndDisabled ();

    if ( g_mrs_defrag_running ) {
        ImGui::SameLine ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.85f, 0.2f, 1.0f ),
                             "%s", _ ( "Defragmentation in progress..." ) );
    }

    /* defrag log - čteme pod mutexem. */
    bool have_mrs_log;
    if ( g_mrs_defrag_mutex ) SDL_LockMutex ( g_mrs_defrag_mutex );
    have_mrs_log = ( g_mrs_defrag_log_len > 0 );
    if ( g_mrs_defrag_mutex ) SDL_UnlockMutex ( g_mrs_defrag_mutex );

    if ( have_mrs_log ) {
        ImGui::Spacing ();
        ImGui::BeginChild ( "mrs_defrag_log", ImVec2 ( 0, 150 ), ImGuiChildFlags_Borders );
        if ( g_mrs_defrag_mutex ) SDL_LockMutex ( g_mrs_defrag_mutex );
        ImGui::TextUnformatted ( g_mrs_defrag_log );
        if ( g_mrs_defrag_mutex ) SDL_UnlockMutex ( g_mrs_defrag_mutex );
        ImGui::SetScrollHereY ( 1.0f );
        ImGui::EndChild ();
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* --- Format File Area --- */
    ImGui::Text ( "%s", _ ( "Format (File Area)" ) );
    ImGui::TextWrapped ( "%s", _ ( "Reinitializes FAT and directory. System area (boot + MRS driver) is preserved. All files become inaccessible." ) );
    ImGui::Spacing ();
    if ( ImGui::Button ( _ ( "Format File Area" ) ) ) {
        g_mrs_pending_format = true;
    }

    /* ======= Potvrzovací dialogy ======= */

    /* Defrag potvrzení */
    if ( g_mrs_pending_defrag ) {
        ImGui::OpenPopup ( "##mrs_defrag_confirm" );
        g_mrs_pending_defrag = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_defrag_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Defragmentation" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "This will read all files into memory, format the file area, and write files sequentially without gaps. System area (boot code, MRS driver) will be preserved." ) );
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s",
                            _ ( "WARNING: If the process is interrupted, data may be lost!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ImGui::Button ( _ ( "Defragment" ), ImVec2 ( 160, 0 ) ) ) {
            /* Asynchronní defrag (audit C-5). Finalizace v polling bloku
             * nahoře, zde jen nastavíme state a spustíme worker. */
            g_mrs_defrag_log[0] = '\0';
            g_mrs_defrag_log_len = 0;
            g_mrs_defrag_running = true;
            g_mrs_defrag_result = MZDSK_RES_OK;
            SDL_SetAtomicInt ( &g_mrs_defrag_done, 0 );

            if ( !g_mrs_defrag_mutex ) g_mrs_defrag_mutex = SDL_CreateMutex ();

            g_mrs_defrag_ctx.disc = config->disc;
            g_mrs_defrag_ctx.fat_block = config->fat_block;

            g_mrs_defrag_thread = SDL_CreateThread ( mrs_defrag_thread_fn, "mrs_defrag", nullptr );
            if ( !g_mrs_defrag_thread ) {
                /* Fallback na synchronní cestu. */
                g_mrs_defrag_running = false;
                g_mrs_defrag_result = fsmrs_defrag ( config->disc, config->fat_block,
                                                      mrs_defrag_progress_cb, nullptr );
                SDL_SetAtomicInt ( &g_mrs_defrag_done, 1 );
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
    if ( g_mrs_pending_format ) {
        ImGui::OpenPopup ( "##mrs_format_confirm" );
        g_mrs_pending_format = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_format_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Format File Area" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "This will reinitialize the FAT table and directory. All files will become inaccessible. System area (boot code, MRS driver) will be preserved." ) );
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                            _ ( "WARNING: This operation cannot be undone!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Format" ) ) ) {
            en_MZDSK_RES res = fsmrs_format_fs ( config->disc, config->fat_block );
            if ( res == MZDSK_RES_OK ) {
                /* reinicializovat config z nové FAT */
                en_MZDSK_RES init_res = fsmrs_init ( config->disc, config->fat_block, config );
                (void) init_res;
                *is_dirty = true;
                panel_mrs_load ( data, detect );
                g_mrs_maint_is_error = false;
                snprintf ( g_mrs_maint_msg, sizeof ( g_mrs_maint_msg ),
                           "%s", _ ( "File area format completed successfully." ) );
            } else {
                g_mrs_maint_is_error = true;
                snprintf ( g_mrs_maint_msg, sizeof ( g_mrs_maint_msg ),
                           "%s: %s", _ ( "Format failed" ), mzdsk_get_error ( res ) );
            }
            g_mrs_maint_result = true;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Výsledek operace */
    if ( g_mrs_maint_result ) {
        ImGui::OpenPopup ( "##mrs_maint_result" );
        g_mrs_maint_result = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##mrs_maint_result", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        if ( g_mrs_maint_is_error ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", _ ( "Error" ) );
        } else {
            ImGui::TextColored ( ImVec4 ( 0.4f, 1.0f, 0.4f, 1.0f ), "%s", _ ( "Success" ) );
        }
        ImGui::Spacing ();
        ImGui::TextUnformatted ( g_mrs_maint_msg );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();
        if ( ButtonMinWidth ( "OK" ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
}
