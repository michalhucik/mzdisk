/**
 * @file app_imgui.cpp
 * @brief ImGui backend pro mzdisk GUI - SDL3 + OpenGL3.
 *
 * Layout hlavního okna:
 *   - Menu bar (nahoře)
 *   - Toolbar (ikony pod menu)
 *   - Content area (sub-taby panelů, vyplní zbytek)
 *   - Status bar (dole)
 *
 * Multi-window model: v jedné instanci lze mít několik DSK otevřených
 * v samostatných oknech. Primární session se renderuje v hlavním okně,
 * detached sessions mají vlastní ImGui okna s plným UI shell. Viz
 * `disk_session.h` pro sémantiku is_open/has_disk/is_primary.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_opengl3.h"
#include "libs/igfd/ImGuiFileDialog.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

/* IGFD ikony - compressed base85 font data (static, proto #include .cpp) */
#include "libs/igfd/CustomFont.cpp"
#include "libs/igfd/CustomFont.h"

/* C hlavičky mzdisk */
extern "C" {
#include "app.h"
#include "i18n.h"
#include "disk_session.h"
#include "dragdrop.h"
#include "panels/panel_info.h"
#include "panels/panel_map.h"
#include "panels/panel_hexdump.h"
#include "panels/panel_fsmz.h"
#include "panels/panel_cpm.h"
#include "panels/panel_mrs.h"
#include "panels/panel_geometry.h"
#include "panels/panel_boot.h"
#include "panels/panel_create.h"
#include "panels/panel_raw_io.h"
#include "panels/panel_settings.h"
#include "config.h"
#include "texture_utils.h"
#include "libs/generic_driver/memory_driver.h"
/* hlavičky knihoven pro verze v About dialogu */
#include "libs/mzf/mzf.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/endianity/endianity.h"
#include "libs/mzdsk_tools/mzdsk_tools.h"
#include "libs/mzdsk_hexdump/mzdsk_hexdump.h"
#include "libs/mzdsk_cpm/mzdsk_cpm_mzf.h"
#include "libs/output_format/output_format.h"
#include "libs/mzglyphs/mzglyphs.h"
#include "libs/mz_vcode/mz_vcode.h"

/* render funkce z C++ souborů panelů */
void panel_info_render ( const st_PANEL_INFO_DATA *data, st_MZDISK_CONFIG *cfg );
void panel_map_render ( const st_PANEL_MAP_DATA *data );
void panel_hexdump_render ( st_PANEL_HEXDUMP_DATA *hd, st_MZDSK_DISC *disc,
                            const st_MZDSK_DETECT_RESULT *detect,
                            st_PANEL_RAW_IO_DATA *raw_io_data,
                            bool *is_dirty );
void panel_raw_io_render ( st_PANEL_RAW_IO_DATA *data, st_MZDSK_DISC *disc,
                            st_PANEL_HEXDUMP_DATA *hd,
                            bool *is_dirty, st_MZDISK_CONFIG *cfg,
                            uint64_t owner_id );
void panel_fsmz_render ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc, bool *is_dirty, st_MZDISK_CONFIG *cfg, uint64_t owner_session_id );
void panel_fsmz_maintenance_render ( st_PANEL_FSMZ_DATA *data, st_MZDSK_DISC *disc, bool *is_dirty );
void panel_cpm_render ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc, bool *is_dirty, st_MZDISK_CONFIG *cfg, uint64_t owner_session_id );
void panel_cpm_maintenance_render ( st_PANEL_CPM_DATA *data, st_MZDSK_DISC *disc,
                                    st_MZDSK_DETECT_RESULT *detect, bool *is_dirty );
void panel_mrs_render ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                        st_MZDSK_DETECT_RESULT *detect, bool *is_dirty, st_MZDISK_CONFIG *cfg,
                        uint64_t owner_session_id );
void panel_mrs_maintenance_render ( st_PANEL_MRS_DATA *data, st_FSMRS_CONFIG *config,
                                    st_MZDSK_DETECT_RESULT *detect, bool *is_dirty );
void panel_geometry_render ( const st_PANEL_GEOMETRY_DATA *data );
void panel_geom_edit_render ( st_PANEL_GEOM_EDIT_DATA *edit_data,
                               st_PANEL_GEOMETRY_DATA *geom_data,
                               st_MZDSK_DISC *disc,
                               bool *is_dirty,
                               bool *needs_reload );
void panel_boot_render ( st_PANEL_BOOT_DATA *data, st_MZDSK_DISC *disc,
                         st_MZDSK_DETECT_RESULT *detect, bool *is_dirty,
                         st_MZDISK_CONFIG *cfg );
void panel_create_render ( st_PANEL_CREATE_DATA *data, st_MZDISK_CONFIG *cfg );
void panel_settings_render ( st_PANEL_SETTINGS_DATA *data, st_MZDISK_CONFIG *cfg );
}

#include "ui_helpers.h"


/** @brief Session manager - v tomto okně je max 1 disk otevřený. */
static st_MZDISK_SESSION_MANAGER g_session_mgr;

/** @brief Klíč pro identifikaci Open Disk dialogu v IGFD. */
static const char *OPEN_DIALOG_KEY = "OpenDiskDlg";

/** @brief Klíč pro identifikaci Save As dialogu v IGFD. */
static const char *SAVEAS_DIALOG_KEY = "SaveAsDiskDlg";

/**
 * @brief Session id, pro kterou je otevřený Open Disk dialog.
 *
 * IGFD je singleton - může být otevřený jen jeden dialog. Uložíme si id
 * session, ze které uživatel dialog otevřel; při zpracování výsledku
 * načteme DSK do té session (ne do aktuálně fokusované).
 *
 * 0 = dialog není otevřený nebo cíl byl zapomenut.
 */
static uint64_t g_open_dialog_target_id = 0;

/**
 * @brief Session id, pro kterou je otevřený Save As dialog.
 */
static uint64_t g_saveas_dialog_target_id = 0;

/**
 * @brief Session id, do které se otevře disk vytvořený v Create dialogu.
 *
 * 0 = použije se primární session (nebo se vytvoří nová).
 */
static uint64_t g_create_target_id = 0;

/** @brief Globální konfigurace aplikace (mzdisk.ini). */
static st_MZDISK_CONFIG g_config;

/** @brief Watch stav pro detekci externích změn mzdisk.ini z jiných instancí. */
static st_MZDISK_CONFIG_WATCH g_config_watch;

/** @brief Stav okna "Create New Disk". */
static st_PANEL_CREATE_DATA g_create_data;

/** @brief Stav okna "Settings". */
static st_PANEL_SETTINGS_DATA g_settings_data;

/** @brief Příznak: fonty je potřeba přebudovat (po změně v Settings). */
static bool g_fonts_need_rebuild = false;

/** @brief Příznak: zobrazit About dialog. */
static bool g_show_about = false;

/** @brief Textura loga aplikace pro About dialog. */
static mzdisk_texture_t g_logo_texture = MZDISK_TEXTURE_INVALID;

/** @brief Šířka loga v pixelech. */
static int g_logo_width = 0;

/** @brief Výška loga v pixelech. */
static int g_logo_height = 0;

/** @brief Příznak: logo bylo načteno (nebo se pokus nezdařil). */
static bool g_logo_loaded = false;

/** @brief Cesta k PNG logu aplikace. */
static const char *LOGO_PATH = "./ui_resources/imgui/images/logo/mzdisk-logo.png";

/** @brief Příznak: čekáme na potvrzení pro ukončení aplikace. */
static bool g_pending_exit = false;

/** @brief Výška toolbaru v pixelech. */
static const float TOOLBAR_HEIGHT = 40.0f;

/** @brief Výška status baru v pixelech (28pt font + padding). */
static const float STATUSBAR_HEIGHT = 42.0f;


/* ==========================================================================
 *  Fonty
 * ========================================================================== */

/**
 * @brief Cesty k fontovým souborům (odpovídají font_family_idx 0..3).
 *
 * Musí odpovídat pořadí v panel_settings.c / s_font_paths[].
 */
static const char *s_font_file_paths[] = {
    "./ui_resources/imgui/fonts/DroidSans.ttf",
    "./ui_resources/imgui/fonts/Roboto-Medium.ttf",
    "./ui_resources/imgui/fonts/Cousine-Regular.ttf",
    "./ui_resources/imgui/fonts/Karla-Regular.ttf",
};


/** @brief Referenční velikost fontu v pixelech pro načtení do atlasu. */
static const float FONT_REFERENCE_SIZE = 28.0f;


/**
 * @brief Načte fonty do ImGui font atlasu.
 *
 * Fonty se načítají v referenční velikosti (28px). Skutečná velikost
 * se řídí přes style.FontScaleMain = (font_size / 28.0).
 *
 * @param io Reference na ImGuiIO.
 * @param font_family_idx Index fontové rodiny (0..3).
 */
static void load_fonts ( ImGuiIO &io, int font_family_idx )
{
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 3;
    font_cfg.OversampleV = 3;
    font_cfg.PixelSnapH = true;

    static const ImWchar i18n_ranges[] = {
        0x0020, 0x024F,
        0x0400, 0x04FF,
        0
    };

    /* zajistit platný index */
    if ( font_family_idx < 0 || font_family_idx >= 4 ) font_family_idx = 0;

    const char *font_path = s_font_file_paths[font_family_idx];

    ImFont *ui_font = io.Fonts->AddFontFromFileTTF (
        font_path, FONT_REFERENCE_SIZE, &font_cfg, i18n_ranges
    );
    if ( !ui_font ) {
        fprintf ( stderr, "Warning: Cannot load '%s', using default font.\n", font_path );
        io.Fonts->AddFontDefault ();
    }

    {
        const char *cjk_font_path = "./ui_resources/imgui/fonts/NotoSansJP-Regular.ttf";
        FILE *f = fopen ( cjk_font_path, "rb" );
        if ( f ) {
            fclose ( f );
            ImFontConfig cjk_cfg;
            cjk_cfg.MergeMode = true;
            cjk_cfg.OversampleH = 2;
            cjk_cfg.OversampleV = 1;
            cjk_cfg.PixelSnapH = true;
            io.Fonts->AddFontFromFileTTF (
                cjk_font_path, FONT_REFERENCE_SIZE, &cjk_cfg,
                io.Fonts->GetGlyphRangesJapanese ()
            );
        }
    }

    {
        static const ImWchar icons_ranges[] = { ICON_MIN_IGFD, ICON_MAX_IGFD, 0 };
        ImFontConfig icons_cfg;
        icons_cfg.MergeMode = true;
        icons_cfg.PixelSnapH = true;
        io.Fonts->AddFontFromMemoryCompressedBase85TTF (
            FONT_ICON_BUFFER_NAME_IGFD, FONT_REFERENCE_SIZE, &icons_cfg, icons_ranges
        );
    }

    {
        static const ImWchar mz_icon_ranges[] = { 0xE000, 0xE000, 0xE100, 0xE4FF, 0 };
        ImFontConfig mz_cfg;
        mz_cfg.MergeMode = true;
        mz_cfg.PixelSnapH = true;
        mz_cfg.OversampleH = 1;  /* bez oversamplingu - pixel-art CG-ROM znaky */
        mz_cfg.OversampleV = 1;
        mz_cfg.GlyphOffset = ImVec2 ( 0, 6 );  /* vertikální korekce CG glyfů */
        io.Fonts->AddFontFromFileTTF (
            "./ui_resources/imgui/symbols/mzglyphs.ttf", 24.0f,
            &mz_cfg, mz_icon_ranges
        );
    }
}


/**
 * @brief Aplikuje velikost fontu jako scale faktor.
 *
 * Nastaví style.FontScaleMain na poměr požadované velikosti
 * k referenční velikosti (28px).
 *
 * @param font_size Požadovaná velikost fontu v pixelech.
 */
static void apply_font_scale ( int font_size )
{
    ImGui::GetStyle ().FontScaleMain = (float) font_size / FONT_REFERENCE_SIZE;
}


/* ==========================================================================
 *  Téma
 * ========================================================================== */

/**
 * @brief Nastaví téma podle indexu.
 *
 * @param theme_idx Index tématu: 0=Dark Blue (custom), 1=Dark, 2=Light.
 */
static void setup_theme ( int theme_idx )
{
    ImGuiStyle &style = ImGui::GetStyle ();

    /* společné geometrické parametry */
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 3.0f;
    style.TabCloseButtonMinWidthSelected = -1.0f;    /* křížek vždy viditelný */
    style.TabCloseButtonMinWidthUnselected = -1.0f;
    /* Border - konzistentně napříč všemi okny, dialogy a popupy.
       Šířka 2px (TODO: přesunout do Settings). */
    style.WindowBorderSize = 2.0f;
    style.PopupBorderSize = 2.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupRounding = 0.0f;

    style.WindowPadding = ImVec2 ( 8.0f, 8.0f );
    style.FramePadding = ImVec2 ( 6.0f, 4.0f );
    style.ItemSpacing = ImVec2 ( 8.0f, 4.0f );

    switch ( theme_idx ) {
        case 1: /* Dark */
            ImGui::StyleColorsDark ();
            break;
        case 2: /* Light */
            ImGui::StyleColorsLight ();
            break;
        default: /* 0 = Dark Blue (custom) */
            ImGui::StyleColorsDark ();
            {
                ImVec4 *colors = style.Colors;
                colors[ImGuiCol_WindowBg]           = ImVec4 ( 0.10f, 0.10f, 0.18f, 1.00f );
                colors[ImGuiCol_ChildBg]            = ImVec4 ( 0.10f, 0.10f, 0.18f, 1.00f );
                colors[ImGuiCol_PopupBg]            = ImVec4 ( 0.10f, 0.10f, 0.16f, 0.96f );
                colors[ImGuiCol_Border]             = ImVec4 ( 0.35f, 0.40f, 0.65f, 0.90f );
                colors[ImGuiCol_FrameBg]            = ImVec4 ( 0.12f, 0.12f, 0.22f, 1.00f );
                colors[ImGuiCol_FrameBgHovered]     = ImVec4 ( 0.16f, 0.16f, 0.30f, 1.00f );
                colors[ImGuiCol_FrameBgActive]      = ImVec4 ( 0.20f, 0.20f, 0.38f, 1.00f );
                colors[ImGuiCol_TitleBg]            = ImVec4 ( 0.06f, 0.06f, 0.12f, 1.00f );
                colors[ImGuiCol_TitleBgActive]      = ImVec4 ( 0.08f, 0.12f, 0.24f, 1.00f );
                colors[ImGuiCol_MenuBarBg]          = ImVec4 ( 0.08f, 0.08f, 0.16f, 1.00f );
                colors[ImGuiCol_ScrollbarBg]        = ImVec4 ( 0.06f, 0.06f, 0.12f, 0.60f );
                colors[ImGuiCol_ScrollbarGrab]      = ImVec4 ( 0.24f, 0.24f, 0.40f, 1.00f );
                colors[ImGuiCol_CheckMark]          = ImVec4 ( 0.40f, 0.60f, 1.00f, 1.00f );
                colors[ImGuiCol_SliderGrab]         = ImVec4 ( 0.30f, 0.45f, 0.80f, 1.00f );
                colors[ImGuiCol_Button]             = ImVec4 ( 0.14f, 0.20f, 0.40f, 1.00f );
                colors[ImGuiCol_ButtonHovered]      = ImVec4 ( 0.18f, 0.28f, 0.52f, 1.00f );
                colors[ImGuiCol_ButtonActive]       = ImVec4 ( 0.10f, 0.16f, 0.36f, 1.00f );
                colors[ImGuiCol_Header]             = ImVec4 ( 0.14f, 0.18f, 0.32f, 1.00f );
                colors[ImGuiCol_HeaderHovered]      = ImVec4 ( 0.18f, 0.24f, 0.42f, 1.00f );
                colors[ImGuiCol_HeaderActive]       = ImVec4 ( 0.12f, 0.16f, 0.30f, 1.00f );
                colors[ImGuiCol_Tab]                = ImVec4 ( 0.10f, 0.12f, 0.24f, 1.00f );
                colors[ImGuiCol_TabHovered]         = ImVec4 ( 0.18f, 0.24f, 0.44f, 1.00f );
                colors[ImGuiCol_TabSelected]        = ImVec4 ( 0.14f, 0.20f, 0.38f, 1.00f );
                colors[ImGuiCol_TextSelectedBg]     = ImVec4 ( 0.20f, 0.40f, 0.80f, 0.40f );
                colors[ImGuiCol_SeparatorHovered]   = ImVec4 ( 0.20f, 0.40f, 0.80f, 0.80f );
            }
            break;
    }
}


/* ==========================================================================
 *  Sdílení konfigurace mezi instancemi (mzdisk.ini watch)
 * ========================================================================== */

/**
 * @brief Uloží g_config a zaktualizuje baseline watch stavu.
 *
 * Wrapper nahrazující přímá volání mzdisk_config_save(&g_config) ve
 * všech call sites. Po úspěšném uložení zachytí novou signaturu souboru
 * jako baseline pro watch, takže vlastní zápis nevyvolá v
 * mzdisk_config_watch_poll() false-positive detekci externí změny.
 */
static void save_and_mark ( void )
{
    if ( mzdisk_config_save ( &g_config ) ) {
        mzdisk_config_watch_mark_saved ( &g_config_watch );
    }
}


/**
 * @brief Aplikuje externí změnu konfigurace (z jiné běžící instance).
 *
 * Porovná pole mezi starou a novou konfigurací a pro změněné fieldy
 * vyvolá odpovídající handlery:
 * - jazyk: i18n_set_language
 * - font_family / font_size: g_fonts_need_rebuild + apply_font_scale
 * - téma: setup_theme + viewport alpha fix
 * - last_create_dir (mění Create okno): panel_create_init (jen pokud
 *   dialog NENÍ otevřený, aby se nezmazal rozpracovaný stav uživatele)
 *
 * Ostatní fieldy (tab_visible, recent_files, ostatní paths, splittery,
 * charsety, export_dup_mode) se čtou per-frame, takže jejich
 * aktualizace v g_config (provedená volajícím) se projeví automaticky.
 *
 * @param old_cfg Aktuální stav v paměti před načtením z disku.
 * @param new_cfg Nový stav načtený z externě změněného mzdisk.ini.
 *
 * @pre old_cfg != NULL, new_cfg != NULL.
 */
static void apply_external_config_change ( const st_MZDISK_CONFIG *old_cfg,
                                            const st_MZDISK_CONFIG *new_cfg )
{
    if ( strcmp ( old_cfg->language, new_cfg->language ) != 0 ) {
        i18n_set_language ( new_cfg->language );
    }
    if ( old_cfg->font_family_idx != new_cfg->font_family_idx ||
         old_cfg->font_size != new_cfg->font_size ) {
        g_fonts_need_rebuild = true;
        if ( old_cfg->font_size != new_cfg->font_size ) {
            apply_font_scale ( new_cfg->font_size );
        }
    }
    if ( old_cfg->theme_idx != new_cfg->theme_idx ) {
        setup_theme ( new_cfg->theme_idx );
        ImGuiIO &io = ImGui::GetIO ();
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
            ImGui::GetStyle ().Colors[ImGuiCol_WindowBg].w = 1.0f;
        }
    }
    /* Re-init g_create_data, jen pokud dialog NENÍ otevřený - jinak
     * bychom smazali rozpracovaný stav. last_create_dir si dialog
     * natáhne při dalším otevření. */
    if ( !g_create_data.is_open &&
         strcmp ( old_cfg->last_create_dir, new_cfg->last_create_dir ) != 0 ) {
        panel_create_init ( &g_create_data, new_cfg );
    }
}


/* ==========================================================================
 *  Per-session operace (otevření, uložení, zavření, reload)
 *
 *  Všechny akce cílí na konkrétní "owner" session - tj. okno, ve kterém
 *  uživatel kliknul do menu/toolbaru. Tím se docílí, že File > Open
 *  v detached okně naplní právě tuto session, ne primární.
 * ========================================================================== */

/**
 * @brief Načte DSK do zadané session, řeší dirty stav.
 *
 * Pokud owner má dirty disk, uloží si filepath do owner->pending_open_path.
 * Tím se v render_unsaved_dialog otevře unsaved dialog pro tuto session.
 * Po Save/Don't Save apply_pending_action nahraje DSK do session (replace
 * disk, okno zůstává).
 *
 * Nesmí se nastavit pending_close, protože user volil Open (= zaměnit
 * disk), ne Close (= zavřít okno). Jinak by se po Open otevřel druhý
 * unsaved dialog a Discard by okno zavřel.
 *
 * Pokud owner nemá disk (EMPTY) nebo má čistý disk, rovnou zavolá
 * mzdisk_session_load.
 *
 * @param owner Session, do které se DSK otevírá.
 * @param filepath Cesta k DSK souboru.
 */
static void open_disk_in_session ( st_MZDISK_SESSION *owner, const char *filepath )
{
    if ( !owner || !filepath || filepath[0] == '\0' ) return;

    if ( owner->has_disk && owner->is_dirty ) {
        /* Dirty - unsaved dialog. Po Save/Don't Save se v
         * apply_pending_action načte nový disk do TÉTO session
         * (replace disk, okno zůstává otevřené). */
        strncpy ( owner->pending_open_path, filepath,
                  sizeof ( owner->pending_open_path ) - 1 );
        owner->pending_open_path[sizeof ( owner->pending_open_path ) - 1] = '\0';
        g_session_mgr.active_id = owner->id;
        return;
    }

    /* Čistá session nebo EMPTY - načíst přímo. */
    en_MZDSK_RES res = mzdisk_session_load ( owner, filepath );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error opening disk '%s': %s\n", filepath, mzdsk_get_error ( res ) );
        mzdisk_session_set_last_op ( owner, LAST_OP_FAILED, "Open" );
        return;
    }
    mzdisk_config_add_recent ( &g_config, filepath );
    save_and_mark ();
    mzdisk_session_set_last_op ( owner, LAST_OP_OK, "Open" );
}


/**
 * @brief Vytvoří nové detached okno (prázdná session bez disku).
 *
 * Výsledné okno má kompletní UI shell (menu + toolbar + welcome + statusbar)
 * a uživatel v něm může File > Open Disk nebo File > New Disk.
 */
static void create_new_window ( void )
{
    /* allow_primary = false: New Window vždy vytvoří detached okno,
     * i když by primární slot zrovna nebyl obsazen. Primární slot je
     * rezervovaný pro hlavní okno, které se inicializuje při startu. */
    st_MZDISK_SESSION *s = mzdisk_session_create_empty ( &g_session_mgr, false );
    if ( !s ) {
        fprintf ( stderr, "Error: maximum number of windows (%d) reached\n",
                  MZDISK_MAX_SESSIONS );
        return;
    }
}


/* ==========================================================================
 *  Menu bar
 * ========================================================================== */

/**
 * @brief Vykreslí obsah menu baru.
 *
 * Volá se uvnitř ImGui::BeginMenuBar() / EndMenuBar().
 *
 * @param[out] running Příznak běhu smyčky.
 */
/**
 * @brief Spustí novou instanci mzdisk procesu.
 */
static void spawn_new_window ( void )
{
    const char *exe = SDL_GetBasePath ();
#ifdef _WIN32
    /* Windows: CreateProcessA vezme exe path včetně quote uvozovek. */
    char cmd[1024];
    snprintf ( cmd, sizeof ( cmd ), "\"%smzdisk.exe\"", exe ? exe : "" );
    STARTUPINFOA si;
    memset ( &si, 0, sizeof ( si ) );
    si.cb = sizeof ( si );
    PROCESS_INFORMATION pi;
    if ( CreateProcessA ( nullptr, cmd, nullptr, nullptr, FALSE,
                          0, nullptr, nullptr, &si, &pi ) ) {
        CloseHandle ( pi.hThread );
        CloseHandle ( pi.hProcess );
    }
#else
    /* POSIX: fork + execl s absolutní cestou (audit M-40). Dříve se
     * volalo `system(cmd)` s uzavřenou cestou v uvozovkách a `&` pro
     * asynchronní běh - command injection přes shell metaznaky v cestě
     * (nepravděpodobné v praxi, ale zbytečné riziko). Bez shellu je
     * escape triviální - exec nezpracovává žádný path expansion. */
    char exe_path[1024];
    snprintf ( exe_path, sizeof ( exe_path ), "%smzdisk", exe ? exe : "" );
    pid_t pid = fork ();
    if ( pid == 0 ) {
        /* child: double-fork pro odpojení od parent process tree,
         * aby dítě neodeslalo SIGCHLD parentu. */
        pid_t pid2 = fork ();
        if ( pid2 == 0 ) {
            execl ( exe_path, exe_path, (char *) NULL );
            _exit ( 127 );
        }
        _exit ( 0 );
    } else if ( pid > 0 ) {
        int status;
        waitpid ( pid, &status, 0 );
    }
#endif
}


/**
 * @brief Otevře file dialog pro výběr DSK do zadané session.
 *
 * Uloží si owner id do g_open_dialog_target_id - po potvrzení v dialogu
 * se DSK načte do této session. Uživatel tak může ze libovolného okna
 * otevřít Open dialog a výsledek přistane v tom konkrétním okně.
 *
 * @param owner Session, do které se po potvrzení DSK načte.
 */
static void open_file_dialog ( st_MZDISK_SESSION *owner )
{
    if ( !owner ) return;
    g_open_dialog_target_id = owner->id;

    IGFD::FileDialogConfig config;
    config.path = g_config.last_open_dir;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                 | ImGuiFileDialogFlags_DontShowHiddenFiles
                 | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;
    ImGuiFileDialog::Instance ()->OpenDialog (
        OPEN_DIALOG_KEY, _ ( "Open Disk Image" ), ".dsk", config
    );
}


/**
 * @brief Uloží disk dané session.
 *
 * @param owner Session, jejíž disk se má uložit.
 */
static void save_session_disk ( st_MZDISK_SESSION *owner )
{
    if ( !owner || !owner->has_disk || !owner->is_dirty ) return;
    en_MZDSK_RES res = mzdisk_session_save ( owner );
    if ( res != MZDSK_RES_OK ) {
        fprintf ( stderr, "Error saving disk: %s\n", mzdsk_get_error ( res ) );
        mzdisk_session_set_last_op ( owner, LAST_OP_FAILED, "Save" );
    } else {
        mzdisk_session_set_last_op ( owner, LAST_OP_OK, "Save" );
    }
}


/**
 * @brief Otevře Save As file dialog pro danou session.
 *
 * @param owner Session, jejíž disk se má uložit pod jiným jménem.
 */
static void save_as_dialog ( st_MZDISK_SESSION *owner )
{
    if ( !owner || !owner->has_disk ) return;
    g_saveas_dialog_target_id = owner->id;

    IGFD::FileDialogConfig config;
    config.path = g_config.last_open_dir;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                 | ImGuiFileDialogFlags_DontShowHiddenFiles
                 | ImGuiFileDialogFlags_ConfirmOverwrite
                 | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;

    /* navrhnout aktuální jméno souboru */
    const char *name = strrchr ( owner->filepath, '/' );
    if ( !name ) name = strrchr ( owner->filepath, '\\' );
    config.fileName = name ? name + 1 : owner->filepath;

    ImGuiFileDialog::Instance ()->OpenDialog (
        SAVEAS_DIALOG_KEY, _ ( "Save Disk As" ), ".dsk", config
    );
}


/**
 * @brief Zavře session (okno i případný disk), s dialogem pokud dirty.
 *
 * Pokud je to primární session, hlavní okno zůstane (welcome screen).
 * Pokud je to detached session, její OS okno se zavře.
 *
 * @param owner Session k zavření.
 */
static void close_session_window ( st_MZDISK_SESSION *owner )
{
    if ( !owner ) return;
    /* Bez disku - "Close Disk" ztrácí smysl. Pro primární okno
     * by nezavřelo nic, pro detached by zavřelo celé okno, což je
     * nečekané chování (user klikl na Close Disk, ne Close Window). */
    if ( !owner->has_disk ) return;
    if ( owner->is_dirty ) {
        owner->pending_close = true;
        g_session_mgr.active_id = owner->id;
    } else {
        mzdisk_session_close_by_id ( &g_session_mgr, owner->id );
    }
}


/**
 * @brief Požádá o ukončení aplikace (s konfirmačním dialogem pokud dirty).
 *
 * Prochází všechny otevřené sessions. Pokud aspoň jedna je dirty,
 * nastaví g_pending_exit = true a unsaved dialog se bude sekvenčně
 * spouštět pro každou dirty session (bulk dialog je plánován pro Fázi 3).
 */
static void request_exit ( bool *running )
{
    bool any_dirty = false;
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        if ( g_session_mgr.sessions[i].is_open &&
             g_session_mgr.sessions[i].is_dirty ) {
            any_dirty = true;
            break;
        }
    }
    if ( any_dirty ) {
        g_pending_exit = true;
    } else {
        *running = false;
    }
}


/**
 * @brief Zpracuje globální klávesové zkratky.
 *
 * Volá se každý frame. Zkratky fungují i když menu není otevřené.
 * Cílí na aktuálně fokusované okno (active_id) - pokud žádné není,
 * na primární session.
 *
 * @param[out] running Příznak běhu smyčky.
 */
static void process_shortcuts ( bool *running )
{
    ImGuiIO &io = ImGui::GetIO ();

    /* ignorovat zkratky pokud je otevřený dialog nebo textový input */
    if ( io.WantTextInput ) return;

    bool ctrl = io.KeyCtrl;

    /* Aktuální cíl zkratky - naposled fokusované okno. */
    st_MZDISK_SESSION *target = mzdisk_session_get_active ( &g_session_mgr );
    if ( !target ) target = mzdisk_session_get_primary ( &g_session_mgr );

    /* Ctrl+N: New Window (prázdné detached okno).
     * Ctrl+Shift+N: Duplicate Process (samostatná instance mzdisk). */
    if ( ctrl && io.KeyShift && ImGui::IsKeyPressed ( ImGuiKey_N ) ) spawn_new_window ();
    else if ( ctrl && ImGui::IsKeyPressed ( ImGuiKey_N ) )           create_new_window ();

    if ( ctrl && ImGui::IsKeyPressed ( ImGuiKey_O ) )   open_file_dialog ( target );
    if ( ctrl && io.KeyShift && ImGui::IsKeyPressed ( ImGuiKey_S ) ) save_as_dialog ( target );
    else if ( ctrl && ImGui::IsKeyPressed ( ImGuiKey_S ) )        save_session_disk ( target );
    if ( ctrl && ImGui::IsKeyPressed ( ImGuiKey_W ) )             close_session_window ( target );
    if ( ctrl && ImGui::IsKeyPressed ( ImGuiKey_Q ) )   request_exit ( running );
}


/**
 * @brief Vykreslí menu bar pro konkrétní okno.
 *
 * Menu je per-okno: akce (Open, Save, Close, Reload) cílí na owner session.
 * Globální akce (Settings, About, Duplicate Process, Exit) jsou stejné
 * ve všech oknech.
 *
 * @param running Pointer na main loop flag (pro File > Exit).
 * @param owner Session, které toto menu patří (NULL = hlavní okno bez primární).
 */
static void render_main_menu ( bool *running, st_MZDISK_SESSION *owner )
{
    bool has_disk = ( owner && owner->has_disk );

    if ( ImGui::BeginMenu ( _ ( "File" ) ) ) {

            /* New Window deaktivovat při dosažení MZDISK_MAX_SESSIONS. */
            bool can_open_new = ( g_session_mgr.count < MZDISK_MAX_SESSIONS );
            if ( ImGui::MenuItem ( _ ( "New Window" ), "Ctrl+N", false,
                                    can_open_new ) )
                create_new_window ();
            if ( !can_open_new && ImGui::IsItemHovered () ) {
                ImGui::SetTooltip ( "%s",
                    _ ( "Maximum number of windows reached." ) );
            }

            if ( ImGui::MenuItem ( _ ( "New Disk..." ) ) ) {
                panel_create_init ( &g_create_data, &g_config );
                g_create_data.is_open = true;
                /* New Disk cílí na okno, ve kterém byla akce vyvolána -
                 * po vytvoření se disk auto-otevře do owner session. */
                g_create_target_id = owner ? owner->id : 0;
            }

            if ( ImGui::MenuItem ( _ ( "Open Disk..." ), "Ctrl+O", false, owner != nullptr ) )
                open_file_dialog ( owner );

            if ( ImGui::BeginMenu ( _ ( "Open Recent" ), g_config.recent_count > 0 ) ) {
                bool shift = ImGui::GetIO ().KeyShift;
                /* Detekce duplicitních basename - pokud má více záznamů
                 * stejný název souboru, přidáme k duplicitním parent
                 * adresář pro rozlišení. ImGui::MenuItem používá label
                 * jako ID, takže duplicitní labely musí být vždy
                 * rozlišeny ##index suffixem kvůli kolizi ID. */
                bool is_dup[MZDISK_CONFIG_RECENT_MAX] = { false };
                const char *names[MZDISK_CONFIG_RECENT_MAX] = { nullptr };
                for ( int i = 0; i < g_config.recent_count; i++ ) {
                    const char *p = g_config.recent_files[i];
                    const char *n = strrchr ( p, '/' );
                    if ( !n ) n = strrchr ( p, '\\' );
                    names[i] = n ? ( n + 1 ) : p;
                }
                for ( int i = 0; i < g_config.recent_count; i++ ) {
                    for ( int j = i + 1; j < g_config.recent_count; j++ ) {
                        if ( strcmp ( names[i], names[j] ) == 0 ) {
                            is_dup[i] = true;
                            is_dup[j] = true;
                        }
                    }
                }
                for ( int i = 0; i < g_config.recent_count; i++ ) {
                    const char *path = g_config.recent_files[i];
                    const char *name = names[i];
                    /* Pokud je název duplicitní, přidáme parent adresář
                     * v závorce, aby uživatel rozlišil položky. ##i
                     * zajišťuje unikátní ImGui ID i pro duplicity. */
                    char label[MZDISK_CONFIG_PATH_MAX + 64];
                    if ( is_dup[i] ) {
                        /* najít parent dir name */
                        size_t name_off = (size_t) ( name - path );
                        char parent[128] = "";
                        if ( name_off >= 2 ) {
                            size_t sep_pos = name_off - 1;
                            size_t end = sep_pos;
                            size_t start = 0;
                            for ( size_t k = sep_pos; k > 0; k-- ) {
                                if ( path[k - 1] == '/' || path[k - 1] == '\\' ) {
                                    start = k;
                                    break;
                                }
                            }
                            size_t plen = end - start;
                            if ( plen >= sizeof ( parent ) ) plen = sizeof ( parent ) - 1;
                            memcpy ( parent, path + start, plen );
                            parent[plen] = '\0';
                        }
                        if ( parent[0] ) {
                            snprintf ( label, sizeof ( label ), "%s  (%s)##%d",
                                       name, parent, i );
                        } else {
                            snprintf ( label, sizeof ( label ), "%s##%d", name, i );
                        }
                    } else {
                        snprintf ( label, sizeof ( label ), "%s##%d", name, i );
                    }
                    if ( ImGui::MenuItem ( label ) ) {
                        /* Shift+klik = otevřít v novém detached okně.
                         * Běžný klik = nahradit disk v tomto okně
                         *   (pokud je Settings open_in_new_window zapnuto,
                         *    i běžný klik otevírá do nového okna - ale jen
                         *    pokud má owner už nějaký disk, jinak by to
                         *    bylo matoucí). */
                        bool use_new =
                            shift || !owner ||
                            ( g_config.open_in_new_window && owner->has_disk );
                        if ( use_new ) {
                            st_MZDISK_SESSION *new_s =
                                mzdisk_session_create_empty ( &g_session_mgr, false );
                            if ( new_s ) {
                                open_disk_in_session ( new_s, path );
                            } else {
                                fprintf ( stderr, "Error: maximum number of windows (%d) reached\n",
                                          MZDISK_MAX_SESSIONS );
                            }
                        } else {
                            open_disk_in_session ( owner, path );
                        }
                    }
                    if ( ImGui::IsItemHovered () ) {
                        ImGui::SetTooltip ( "%s\n\n%s", path,
                                            _ ( "Shift+click to open in a new window" ) );
                    }
                }
                ImGui::Separator ();
                if ( ImGui::MenuItem ( _ ( "Clear Recent" ) ) ) {
                    mzdisk_config_clear_recent ( &g_config );
                    save_and_mark ();
                }
                ImGui::EndMenu ();
            }

            if ( ImGui::MenuItem ( _ ( "Save" ), "Ctrl+S", false, has_disk && owner->is_dirty ) )
                save_session_disk ( owner );

            if ( ImGui::MenuItem ( _ ( "Save As..." ), "Ctrl+Shift+S", false, has_disk ) )
                save_as_dialog ( owner );

            /* Close Disk enabled jen pokud je skutečně disk načtený.
             * Bez has_disk by Close na EMPTY session zavřela okno
             * (riziko ztráty kontextu bez varování). */
            if ( ImGui::MenuItem ( _ ( "Close Disk" ), "Ctrl+W", false, has_disk ) )
                close_session_window ( owner );

            ImGui::Separator ();

            if ( ImGui::MenuItem ( _ ( "Settings..." ) ) ) {
                g_settings_data.is_open = true;
            }

            ImGui::Separator ();

            /* Duplicate Process - spustí druhou nezávislou instanci mzdisk. */
            if ( ImGui::MenuItem ( _ ( "Duplicate Process" ), "Ctrl+Shift+N" ) )
                spawn_new_window ();

            if ( ImGui::MenuItem ( _ ( "Exit" ), "Ctrl+Q" ) )
                request_exit ( running );

            ImGui::EndMenu ();
        }

    if ( ImGui::BeginMenu ( _ ( "View" ) ) ) {
        if ( !has_disk ) ImGui::BeginDisabled ();
        ImGui::MenuItem ( _ ( "Info" ),             nullptr, &g_config.tab_visible[MZDISK_TAB_INFO] );
        ImGui::MenuItem ( _ ( "Geometry" ),         nullptr, &g_config.tab_visible[MZDISK_TAB_GEOMETRY] );
        ImGui::MenuItem ( _ ( "Geometry Edit" ),    nullptr, &g_config.tab_visible[MZDISK_TAB_GEOMETRY_EDIT] );
        ImGui::MenuItem ( _ ( "Boot Sector" ),      nullptr, &g_config.tab_visible[MZDISK_TAB_BOOT_SECTOR] );
        ImGui::MenuItem ( _ ( "Block Map" ),        nullptr, &g_config.tab_visible[MZDISK_TAB_BLOCK_MAP] );
        ImGui::MenuItem ( _ ( "Hexdump" ),          nullptr, &g_config.tab_visible[MZDISK_TAB_HEXDUMP] );
        ImGui::Separator ();
        ImGui::MenuItem ( _ ( "FS Directory" ),     nullptr, &g_config.tab_visible[MZDISK_TAB_FS_DIR] );
        ImGui::MenuItem ( _ ( "FS Maintenance" ),   nullptr, &g_config.tab_visible[MZDISK_TAB_FS_MAINT] );
        ImGui::Separator ();
        if ( ImGui::MenuItem ( _ ( "Show All" ) ) ) {
            for ( int i = 0; i < MZDISK_TAB_COUNT; i++ )
                g_config.tab_visible[i] = true;
        }
        if ( !has_disk ) ImGui::EndDisabled ();
        ImGui::EndMenu ();
    }

    if ( ImGui::BeginMenu ( _ ( "Help" ) ) ) {
        if ( ImGui::MenuItem ( _ ( "About mzdisk..." ) ) ) {
            g_show_about = true;
        }
        ImGui::EndMenu ();
    }
}


/* ==========================================================================
 *  About dialog
 * ========================================================================== */

/**
 * @brief Vykreslí modální About dialog s informacemi o aplikaci a knihovnách.
 *
 * Zobrazuje název, verzi, popis projektu, licenci, verze všech mzdsk knihoven
 * a verze externích závislostí (ImGui, SDL3). Dialog se otevře po kliknutí
 * na Help > About mzdisk... a zavře tlačítkem Close nebo klávesou Escape.
 *
 * @pre g_show_about musí být nastaveno na true pro otevření dialogu.
 * @post Po zavření dialogu je g_show_about nastaveno na false.
 */
static void render_about_dialog ( void )
{
    if ( g_show_about ) {
        ImGui::OpenPopup ( _ ( "About mzdisk" ) );
        g_show_about = false;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    ImGui::PushStyleVar ( ImGuiStyleVar_WindowBorderSize, 2.0f );
    ImGui::PushStyleColor ( ImGuiCol_Border, ImVec4 ( 0.40f, 0.50f, 0.80f, 1.0f ) );

    ImGui::SetNextWindowSize ( ImVec2 ( 600, 820 ), ImGuiCond_Appearing );

    if ( ImGui::BeginPopupModal ( _ ( "About mzdisk" ), nullptr,
                                  ImGuiWindowFlags_None ) ) {

        /* lazy load loga */
        if ( !g_logo_loaded ) {
            g_logo_texture = mzdisk_texture_load_png ( LOGO_PATH, &g_logo_width, &g_logo_height );
            g_logo_loaded = true;
        }

        /* logo aplikace (40% původní velikosti) */
        if ( g_logo_texture != MZDISK_TEXTURE_INVALID && g_logo_width > 0 && g_logo_height > 0 ) {
            float logo_display_h = (float) g_logo_height * 0.4f;
            float logo_display_w = (float) g_logo_width * 0.4f;
            float avail_w = ImGui::GetContentRegionAvail ().x;
            if ( avail_w > logo_display_w ) {
                ImGui::SetCursorPosX ( ImGui::GetCursorPosX () + ( avail_w - logo_display_w ) * 0.5f );
            }
            ImGui::Image ( (ImTextureID)(intptr_t) g_logo_texture,
                           ImVec2 ( logo_display_w, logo_display_h ) );
        }

        /* záhlaví - centrovaný, bold, větší font.
         * Audit M-34: dříve přímá manipulace `ImGui::GetFont()->Scale`
         * měnila internal state fontu - side-effect do dalších CalcTextSize
         * v celém frame. Nyní použito oficiální ImGui API
         * `PushFont(font, size)` které scale přidá jen uvnitř Push/Pop
         * dvojice. */
        {
            char title_buf[64];
            snprintf ( title_buf, sizeof ( title_buf ), "mzdisk %s", MZDISK_VERSION );
            ImFont *base_font = ImGui::GetIO ().Fonts->Fonts[0];
            float header_size = ImGui::GetStyle ().FontSizeBase * 1.4f;
            ImGui::PushFont ( base_font, header_size );

            float text_w = ImGui::CalcTextSize ( title_buf ).x;
            float avail_w = ImGui::GetContentRegionAvail ().x;
            ImGui::SetCursorPosX ( ImGui::GetCursorPosX () + ( avail_w - text_w ) * 0.5f );
            ImGui::TextUnformatted ( title_buf );

            ImGui::PopFont ();
        }
        ImGui::Spacing ();

        /* podtitulek - centrovaný, dva řádky */
        {
            const char *line1 = _ ( "Sharp MZ disk image manager" );
            float text_w = ImGui::CalcTextSize ( line1 ).x;
            float avail_w = ImGui::GetContentRegionAvail ().x;
            ImGui::SetCursorPosX ( ImGui::GetCursorPosX () + ( avail_w - text_w ) * 0.5f );
            ImGui::TextUnformatted ( line1 );

            const char *line2 = "FSMZ, CP/M, MRS";
            text_w = ImGui::CalcTextSize ( line2 ).x;
            avail_w = ImGui::GetContentRegionAvail ().x;
            ImGui::SetCursorPosX ( ImGui::GetCursorPosX () + ( avail_w - text_w ) * 0.5f );
            ImGui::TextUnformatted ( line2 );
        }
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        /* autor, licence, zdrojový kód. Hyperlink na GitHub se kreslí ručně
         * přes Selectable + barva; klik volá SDL_OpenURL, které spolehlivě
         * funguje na Linuxu (xdg-open) i na Windows. */
        ImGui::Text ( "%s: Michal Hucik <hucik@ordoz.com>", _ ( "Author" ) );
        ImGui::Text ( "%s: 2018-2026 Michal Hucik", _ ( "Copyright" ) );
        ImGui::Text ( "%s: GNU General Public License v3 (GPLv3)", _ ( "License" ) );

        ImGui::Spacing ();

        {
            static const char *repo_url = "https://github.com/michalhucik/mzdisk";
            ImGui::Text ( "%s:", _ ( "Source code" ) );
            ImGui::SameLine ();
            ImVec4 link_color ( 0.40f, 0.75f, 1.00f, 1.0f );
            ImGui::PushStyleColor ( ImGuiCol_Text, link_color );
            ImVec2 p0 = ImGui::GetCursorScreenPos ();
            ImVec2 ts = ImGui::CalcTextSize ( repo_url );
            if ( ImGui::Selectable ( repo_url, false, ImGuiSelectableFlags_None, ts ) ) {
                SDL_OpenURL ( repo_url );
            }
            /* podtržení */
            ImGui::GetWindowDrawList ()->AddLine (
                ImVec2 ( p0.x, p0.y + ts.y ),
                ImVec2 ( p0.x + ts.x, p0.y + ts.y ),
                ImGui::GetColorU32 ( link_color ) );
            ImGui::PopStyleColor ();
            if ( ImGui::IsItemHovered () ) {
                ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );
                ImGui::SetTooltip ( "%s", _ ( "Open in browser" ) );
            }
        }

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        /* verze mzdsk knihoven (implicitně sbalené) */
        if ( ImGui::TreeNodeEx ( _ ( "Library versions" ), ImGuiTreeNodeFlags_None ) ) {
            if ( ImGui::BeginTable ( "##libs", 2, ImGuiTableFlags_SizingStretchProp ) ) {
                ImGui::TableSetupColumn ( _ ( "Library" ), ImGuiTableColumnFlags_None, 0.6f );
                ImGui::TableSetupColumn ( _ ( "Version" ), ImGuiTableColumnFlags_None, 0.4f );
                ImGui::TableHeadersRow ();

                /* pomocné makro pro jeden řádek */
                #define ABOUT_LIB_ROW(name, ver) \
                    ImGui::TableNextRow (); \
                    ImGui::TableNextColumn (); ImGui::TextUnformatted ( name ); \
                    ImGui::TableNextColumn (); ImGui::TextUnformatted ( ver );

                ABOUT_LIB_ROW ( "dsk",             DSK_VERSION )
                ABOUT_LIB_ROW ( "dsk_tools",       DSK_TOOLS_VERSION )
                ABOUT_LIB_ROW ( "generic_driver",  GENERIC_DRIVER_VERSION )
                ABOUT_LIB_ROW ( "mzdsk_global",    MZDSK_GLOBAL_VERSION )
                ABOUT_LIB_ROW ( "mzdsk_detect",    MZDSK_DETECT_VERSION )
                ABOUT_LIB_ROW ( "mzdsk_ipldisk",   MZDSK_IPLDISK_VERSION )
                ABOUT_LIB_ROW ( "mzdsk_cpm",       MZDSK_CPM_VERSION )
                ABOUT_LIB_ROW ( "mzdsk_cpm_mzf",   MZDSK_CPM_MZF_VERSION )
                ABOUT_LIB_ROW ( "mzdsk_mrs",       MZDSK_MRS_VERSION )
                ABOUT_LIB_ROW ( "mzdsk_tools",     MZDSK_TOOLS_VERSION )
                ABOUT_LIB_ROW ( "mzdsk_hexdump",   MZDSK_HEXDUMP_VERSION )
                ABOUT_LIB_ROW ( "mzf",             MZF_VERSION )
                ABOUT_LIB_ROW ( "sharpmz_ascii",   SHARPMZ_ASCII_VERSION )
                ABOUT_LIB_ROW ( "endianity",       ENDIANITY_VERSION )
                ABOUT_LIB_ROW ( "output_format",   OUTPUT_FORMAT_VERSION )
                ABOUT_LIB_ROW ( "mzglyphs",        MZGLYPHS_VERSION )
                ABOUT_LIB_ROW ( "mz_vcode",        MZ_VCODE_VERSION )

                #undef ABOUT_LIB_ROW

                ImGui::EndTable ();
            }
            ImGui::TreePop ();
        }

        /* verze externích závislostí */
        if ( ImGui::TreeNodeEx ( _ ( "External dependencies" ) ) ) {
            if ( ImGui::BeginTable ( "##ext", 2, ImGuiTableFlags_SizingStretchProp ) ) {
                ImGui::TableSetupColumn ( _ ( "Library" ), ImGuiTableColumnFlags_None, 0.6f );
                ImGui::TableSetupColumn ( _ ( "Version" ), ImGuiTableColumnFlags_None, 0.4f );
                ImGui::TableHeadersRow ();

                ImGui::TableNextRow ();
                ImGui::TableNextColumn (); ImGui::TextUnformatted ( "Dear ImGui" );
                ImGui::TableNextColumn (); ImGui::TextUnformatted ( IMGUI_VERSION );

                ImGui::TableNextRow ();
                ImGui::TableNextColumn (); ImGui::TextUnformatted ( "SDL" );
                ImGui::TableNextColumn ();
                int sdl_ver = SDL_GetVersion ();
                ImGui::Text ( "%d.%d.%d",
                              SDL_VERSIONNUM_MAJOR ( sdl_ver ),
                              SDL_VERSIONNUM_MINOR ( sdl_ver ),
                              SDL_VERSIONNUM_MICRO ( sdl_ver ) );

                ImGui::EndTable ();
            }
            ImGui::TreePop ();
        }

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        /* zavírací tlačítko */
        float button_width = 120.0f;
        ImGui::SetCursorPosX ( ( ImGui::GetWindowSize ().x - button_width ) * 0.5f );
        if ( ImGui::Button ( _ ( "Close" ), ImVec2 ( button_width, 0 ) ) ) {
            ImGui::CloseCurrentPopup ();
        }

        /* Escape zavře dialog */
        if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) {
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    }

    ImGui::PopStyleColor ( 1 );
    ImGui::PopStyleVar ();
}


/* ==========================================================================
 *  Konfirmační dialog
 * ========================================================================== */

/**
 * @brief Aplikuje odloženou akci (close okna nebo replace disku) pro session.
 *
 * Volá se po Save / Don't Save v unsaved dialogu:
 *   - pending_open_path neprázdný -> nahradit disk v TÉTO session.
 *   - pending_close -> zavřít celé okno (close_by_id).
 *   - jinak nic.
 *
 * @param s Session, jejíž pending akce se má provést.
 */
static void apply_pending_action ( st_MZDISK_SESSION *s )
{
    if ( !s ) return;

    if ( s->pending_open_path[0] != '\0' ) {
        char path[MZDISK_MAX_PATH];
        strncpy ( path, s->pending_open_path, sizeof ( path ) - 1 );
        path[sizeof ( path ) - 1] = '\0';
        s->pending_open_path[0] = '\0';
        en_MZDSK_RES res = mzdisk_session_load ( s, path );
        if ( res != MZDSK_RES_OK ) {
            fprintf ( stderr, "Error opening disk '%s': %s\n", path, mzdsk_get_error ( res ) );
            mzdisk_session_set_last_op ( s, LAST_OP_FAILED, "Open" );
        } else {
            mzdisk_config_add_recent ( &g_config, path );
            save_and_mark ();
            mzdisk_session_set_last_op ( s, LAST_OP_OK, "Open" );
        }
    } else if ( s->pending_close ) {
        s->pending_close = false;
        mzdisk_session_close_by_id ( &g_session_mgr, s->id );
    }
}


/**
 * @brief Vykreslí per-session unsaved dialog (Close Disk nebo Open replace).
 *
 * Cílí na active session (naposled fokusovaná). Triggery:
 *   - session->pending_close (Close Disk, Ctrl+W, křížek okna)
 *   - session->pending_open_path (Open Disk nad dirty session -> replace disk)
 *
 * Bulk dialog při Exit řeší samostatná render_exit_unsaved_dialog.
 */
static void render_unsaved_dialog ( void )
{
    st_MZDISK_SESSION *active = mzdisk_session_get_active ( &g_session_mgr );

    bool show_dialog = active && ( active->pending_close ||
                                   active->pending_open_path[0] != '\0' );

    if ( show_dialog ) {
        ImGui::OpenPopup ( "##unsaved" );
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( ImGui::BeginPopupModal ( "##unsaved", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {

        if ( active ) {
            ImGui::Text ( "%s \"%s\" %s", _ ( "Disk" ), active->display_name,
                          _ ( "has unsaved changes." ) );
        }
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Save" ) ) ) {
            /* Pokus o uložení. Pokud selže, zobrazíme chybu v dialogu
             * a NEZAVŘEME session - zabráníme tím tiché ztrátě dat
             * (read-only FS, plný disk, síťový disk). */
            en_MZDSK_RES save_res = MZDSK_RES_OK;
            if ( active ) {
                save_res = mzdisk_session_save ( active );
                if ( save_res != MZDSK_RES_OK ) {
                    mzdisk_session_set_last_op ( active, LAST_OP_FAILED, "Save" );
                    snprintf ( active->last_save_error, sizeof ( active->last_save_error ),
                               "%s: %s", _ ( "Save failed" ), mzdsk_get_error ( save_res ) );
                } else {
                    mzdisk_session_set_last_op ( active, LAST_OP_OK, "Save" );
                    active->last_save_error[0] = '\0';
                }
            }
            if ( save_res == MZDSK_RES_OK && active ) {
                apply_pending_action ( active );
                ImGui::CloseCurrentPopup ();
            }
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Don't Save" ) ) ) {
            if ( active ) {
                active->last_save_error[0] = '\0';
                apply_pending_action ( active );
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            if ( active ) {
                active->pending_close = false;
                active->pending_open_path[0] = '\0';
                active->last_save_error[0] = '\0';
            }
            ImGui::CloseCurrentPopup ();
        }

        /* Chybová hláška z předchozího neúspěšného Save - zabrání
         * uživateli omylem ztratit práci. Per-session, aby víc detached
         * oken mohlo paralelně držet vlastní Save-error. */
        if ( active && active->last_save_error[0] != '\0' ) {
            ImGui::Spacing ();
            ImGui::Separator ();
            ImGui::Spacing ();
            ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ) );
            ImGui::TextWrapped ( "%s", active->last_save_error );
            ImGui::PopStyleColor ();
        }

        ImGui::EndPopup ();
    }
}


/**
 * @brief Spočítá otevřené dirty sessions.
 */
static int count_dirty_sessions ( void )
{
    int n = 0;
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        if ( g_session_mgr.sessions[i].is_open &&
             g_session_mgr.sessions[i].has_disk &&
             g_session_mgr.sessions[i].is_dirty ) {
            n++;
        }
    }
    return n;
}


/**
 * @brief Bulk unsaved dialog při Exit aplikace.
 *
 * Pokud g_pending_exit = true:
 *   - Žádná dirty session -> rovnou exit (running = false).
 *   - Jinak otevře modal se seznamem dirty sessions a 3 tlačítky:
 *     Save All: pokusí se uložit všechny, úspěšně uložené zavře.
 *               Nevydařené zanechá s last_save_error a dialog zůstává -
 *               uživatel může zkusit znovu nebo Discard All / Cancel.
 *     Discard All: zavře všechny sessions bez save, exit.
 *     Cancel: zruší exit (g_pending_exit = false).
 */
static void render_exit_unsaved_dialog ( bool *running )
{
    if ( !g_pending_exit ) return;

    int dirty = count_dirty_sessions ();
    if ( dirty == 0 ) {
        /* Nic k uložení - exit může proběhnout. */
        g_pending_exit = false;
        *running = false;
        return;
    }

    ImGui::OpenPopup ( "##exit_unsaved" );

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    ImGui::SetNextWindowSize ( ImVec2 ( 520, 0 ), ImGuiCond_Appearing );

    if ( ImGui::BeginPopupModal ( "##exit_unsaved", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {

        ImGui::Text ( "%s", _ ( "Unsaved changes:" ) );
        ImGui::Spacing ();

        /* Seznam dirty sessions. Pokud má session last_save_error, zobrazí
         * se červeně pod jménem. */
        ImGui::Indent ( 20.0f );
        for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
            st_MZDISK_SESSION *s = &g_session_mgr.sessions[i];
            if ( !s->is_open || !s->has_disk || !s->is_dirty ) continue;
            ImGui::BulletText ( "%s", s->display_name );
            if ( s->last_save_error[0] != '\0' ) {
                ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ) );
                ImGui::TextWrapped ( "    %s", s->last_save_error );
                ImGui::PopStyleColor ();
            }
        }
        ImGui::Unindent ( 20.0f );

        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Save All" ) ) ) {
            /* Pokus o Save u každé dirty session. Úspěšně uložené zavřeme,
             * neúspěšné ponecháme s chybou - dialog zůstává otevřený. */
            bool all_ok = true;
            for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
                st_MZDISK_SESSION *s = &g_session_mgr.sessions[i];
                if ( !s->is_open || !s->has_disk || !s->is_dirty ) continue;
                en_MZDSK_RES r = mzdisk_session_save ( s );
                if ( r != MZDSK_RES_OK ) {
                    mzdisk_session_set_last_op ( s, LAST_OP_FAILED, "Save" );
                    snprintf ( s->last_save_error, sizeof ( s->last_save_error ),
                               "%s: %s", _ ( "Save failed" ), mzdsk_get_error ( r ) );
                    all_ok = false;
                } else {
                    mzdisk_session_set_last_op ( s, LAST_OP_OK, "Save" );
                    s->last_save_error[0] = '\0';
                }
            }
            if ( all_ok ) {
                /* Všechny uloženy - exit. */
                g_pending_exit = false;
                *running = false;
                ImGui::CloseCurrentPopup ();
            }
            /* Jinak dialog zůstává - uživatel vidí, které session selhaly. */
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Discard All" ) ) ) {
            g_pending_exit = false;
            *running = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            /* Vyčistit last_save_error u všech sessions (uživatel zrušil
             * exit, chyby už nejsou relevantní). */
            for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
                g_session_mgr.sessions[i].last_save_error[0] = '\0';
            }
            g_pending_exit = false;
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    }
}


/**
 * @brief Provede reload disku v dané session ze souboru.
 *
 * Znovu načte DSK do existující session. Session samotná (okno) zůstává,
 * mění se jen disk data. Všechny neuložené změny jsou zahozeny.
 *
 * @param owner Session s otevřeným diskem.
 */
static void reload_session_disk ( st_MZDISK_SESSION *owner )
{
    if ( !owner || !owner->has_disk ) return;

    char path[MZDISK_MAX_PATH];
    strncpy ( path, owner->filepath, MZDISK_MAX_PATH - 1 );
    path[MZDISK_MAX_PATH - 1] = '\0';

    en_MZDSK_RES res = mzdisk_session_load ( owner, path );
    mzdisk_session_set_last_op ( owner,
        ( res == MZDSK_RES_OK ) ? LAST_OP_OK : LAST_OP_FAILED, "Reload" );
}


/**
 * @brief Vykreslí potvrzovací dialog pro reload dirty disku.
 *
 * Zobrazuje varování, že neuložené změny budou zahozeny.
 * Tlačítka: Reload (provede reload) a Cancel (zruší).
 *
 * Stav se čte z active session (session->pending_reload). Ve Fázi 2
 * se dialog queue umožní per-session dialogy nad detached okny.
 *
 * @pre active->pending_reload je nastaveno na true pro otevření dialogu.
 * @post Po akci je active->pending_reload nastaveno na false.
 */
static void render_reload_dialog ( void )
{
    st_MZDISK_SESSION *active = mzdisk_session_get_active ( &g_session_mgr );

    if ( active && active->pending_reload ) {
        ImGui::OpenPopup ( "##reload_confirm" );
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( ImGui::BeginPopupModal ( "##reload_confirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {

        if ( active ) {
            ImGui::Text ( "%s \"%s\"", _ ( "Reload" ), active->display_name );
        }
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s",
                            _ ( "All unsaved changes will be discarded!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Reload" ) ) ) {
            reload_session_disk ( active );
            if ( active ) active->pending_reload = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            if ( active ) active->pending_reload = false;
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    }
}


/**
 * @brief Modal pro DnD ASK konflikty (dup_mode = Ask).
 *
 * Zobrazí se při dnd_has_pending_ask(). Uživatel vybere akci pro aktuální
 * konfliktní soubor - volba se aplikuje také na všechny zbývající konflikty
 * v téže drop operaci (apply-to-all semantika).
 */
static void render_dnd_ask_dialog ( void )
{
    /* Stejný pattern jako CP/M / MRS bulk Get ASK popup:
       - OpenPopup pod podmínkou ask_pending (první snímek),
       - BeginPopupModal mimo ni (každý snímek, dokud je otevřen).
       Per-file akce (Overwrite/Rename/Skip) + Apply to all remaining
       (Overwrite all / Rename all / Skip all) + Cancel. Sjednocuje
       se sadou tlačítek v CP/M a MRS popupech. */
    static bool s_dnd_ask_opened = false;
    bool pending = dnd_has_pending_ask ();
    if ( pending && !s_dnd_ask_opened ) {
        ImGui::OpenPopup ( "##dnd_ask" );
        s_dnd_ask_opened = true;
    }

    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    ImGui::SetNextWindowSize ( ImVec2 ( 520, 0 ), ImGuiCond_Appearing );

    if ( ImGui::BeginPopupModal ( "##dnd_ask", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {

        ImGui::Text ( "%s", _ ( "File already exists:" ) );
        ImGui::Spacing ();
        ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 1.0f, 0.8f, 0.3f, 1.0f ) );
        ImGui::Indent ( 20.0f );
        ImGui::Text ( "%s", dnd_get_ask_conflict_name () );
        ImGui::Unindent ( 20.0f );
        ImGui::PopStyleColor ();
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        int action = -1;         /* 0=Overwrite, 1=Rename, 2=Skip */
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

        if ( action >= 0 ) {
            int mode = ( action == 0 ) ? MZDSK_EXPORT_DUP_OVERWRITE
                     : ( action == 1 ) ? MZDSK_EXPORT_DUP_RENAME
                                       : MZDSK_EXPORT_DUP_SKIP;
            dnd_user_chose_ask ( mode, apply_all );
            s_dnd_ask_opened = false;
            ImGui::CloseCurrentPopup ();
        } else if ( cancel ) {
            dnd_cancel_ask ();
            s_dnd_ask_opened = false;
            ImGui::CloseCurrentPopup ();
        }

        ImGui::EndPopup ();
    } else if ( !pending ) {
        /* Popup zavřen z jiného důvodu (např. click mimo v non-modal).
           V modal variantě toto nenastane, ale reset pro jistotu. */
        s_dnd_ask_opened = false;
    }
}


/* ==========================================================================
 *  File dialog
 * ========================================================================== */

static void process_file_dialog ( void )
{
    SetNextWindowDefaultCentered ( 1200, 768 );

    /* výrazný rámeček a titulek pro file dialog */
    ImGui::PushStyleVar ( ImGuiStyleVar_WindowBorderSize, 2.0f );
    ImGui::PushStyleColor ( ImGuiCol_Border, ImVec4 ( 0.40f, 0.50f, 0.80f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBg, ImVec4 ( 0.10f, 0.14f, 0.30f, 1.0f ) );
    ImGui::PushStyleColor ( ImGuiCol_TitleBgActive, ImVec4 ( 0.14f, 0.20f, 0.42f, 1.0f ) );

    ImGuiWindowFlags dlg_flags = ImGuiWindowFlags_NoCollapse;

    if ( ImGuiFileDialog::Instance ()->Display ( OPEN_DIALOG_KEY, dlg_flags ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            std::string filepath = ImGuiFileDialog::Instance ()->GetFilePathName ();
            std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();
            /* Cíl dialogu byl zaznamenán při open_file_dialog(owner). */
            st_MZDISK_SESSION *target = mzdisk_session_get_by_id ( &g_session_mgr,
                                                                   g_open_dialog_target_id );
            if ( target ) {
                /* Settings: pokud je open_in_new_window zapnuto, otevřeme
                 * disk do nové detached session místo nahrazení v current.
                 * Výjimka: cílová session ještě nemá disk (EMPTY) - pak
                 * otevřeme rovnou do ní, aby uživatel nedostal druhé okno
                 * tam, kde první ještě nic nedělá. */
                if ( g_config.open_in_new_window && target->has_disk ) {
                    st_MZDISK_SESSION *new_s =
                        mzdisk_session_create_empty ( &g_session_mgr, false );
                    if ( new_s ) {
                        open_disk_in_session ( new_s, filepath.c_str () );
                    } else {
                        fprintf ( stderr, "Error: maximum number of windows (%d) reached\n",
                                  MZDISK_MAX_SESSIONS );
                    }
                } else {
                    open_disk_in_session ( target, filepath.c_str () );
                }
            }
            g_open_dialog_target_id = 0;
            /* zapamatovat poslední adresář a uložit config */
            strncpy ( g_config.last_open_dir, dirpath.c_str (), sizeof ( g_config.last_open_dir ) - 1 );
            g_config.last_open_dir[sizeof ( g_config.last_open_dir ) - 1] = '\0';
            save_and_mark ();
        } else {
            g_open_dialog_target_id = 0;
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    /* Save As dialog */
    SetNextWindowDefaultCentered ( 1200, 768 );
    if ( ImGuiFileDialog::Instance ()->Display ( SAVEAS_DIALOG_KEY, dlg_flags ) ) {
        if ( ImGuiFileDialog::Instance ()->IsOk () ) {
            std::string filepath = ImGuiFileDialog::Instance ()->GetFilePathName ();
            std::string dirpath = ImGuiFileDialog::Instance ()->GetCurrentPath ();

            st_MZDISK_SESSION *target = mzdisk_session_get_by_id ( &g_session_mgr,
                                                                   g_saveas_dialog_target_id );
            if ( target && target->has_disk ) {
                /* aktualizovat cestu v session a disc */
                strncpy ( target->filepath, filepath.c_str (), MZDISK_MAX_PATH - 1 );
                target->filepath[MZDISK_MAX_PATH - 1] = '\0';
                target->disc.filename = target->filepath;

                /* uložit */
                en_MZDSK_RES res = mzdisk_session_save ( target );
                if ( res != MZDSK_RES_OK ) {
                    fprintf ( stderr, "Error saving disk: %s\n", mzdsk_get_error ( res ) );
                    mzdisk_session_set_last_op ( target, LAST_OP_FAILED, "Save As" );
                } else {
                    mzdisk_session_set_last_op ( target, LAST_OP_OK, "Save As" );
                }

                /* aktualizovat display name */
                const char *name = strrchr ( target->filepath, '/' );
                if ( !name ) name = strrchr ( target->filepath, '\\' );
                if ( name ) name++; else name = target->filepath;
                snprintf ( target->display_name, sizeof ( target->display_name ),
                           "%.200s (%s)", name, mzdisk_session_fs_type_str_detail ( &target->detect_result ) );

                /* aktualizovat recent files s novou cestou */
                mzdisk_config_add_recent ( &g_config, target->filepath );
            }
            g_saveas_dialog_target_id = 0;

            strncpy ( g_config.last_open_dir, dirpath.c_str (), sizeof ( g_config.last_open_dir ) - 1 );
            g_config.last_open_dir[sizeof ( g_config.last_open_dir ) - 1] = '\0';
            save_and_mark ();
        } else {
            g_saveas_dialog_target_id = 0;
        }
        ImGuiFileDialog::Instance ()->Close ();
    }

    ImGui::PopStyleColor ( 3 );
    ImGui::PopStyleVar ();
}


/* ==========================================================================
 *  Toolbar
 * ========================================================================== */

/**
 * @brief Vykreslí ikonový toolbar pod menu barem.
 *
 * Skupiny: Disk (Open/Save/Reload) | Nový disk (New Disk).
 */
/**
 * @brief Vykreslí statusbar pro okno, v obdélníku danému souřadnicemi.
 *
 * Kreslí do předaného ImDrawList. Pro hlavní okno se volá s draw listem
 * hlavního viewportu; pro detached okna s window draw listem.
 *
 * Pokud owner je primární session a aktivní id (naposled fokusovaná)
 * patří jiné detached session, zobrazí se vpravo tag "→ [focus: name]".
 * Dává uživateli vědět, že zkratky Ctrl+S/W cílí na jiné okno než
 * to, na které právě kouká. Pro detached okna je owner == active obvykle,
 * takže tag se neobjeví.
 *
 * @param dl ImDrawList, do kterého se kreslí (nikdy NULL).
 * @param sb_x Levý okraj obdélníku.
 * @param sb_y Horní okraj obdélníku.
 * @param sb_w Šířka obdélníku.
 * @param owner Session, jejíž stav se zobrazuje (NULL = "No disk loaded").
 */
static void render_statusbar ( ImDrawList *dl, float sb_x, float sb_y, float sb_w,
                                st_MZDISK_SESSION *owner )
{
    /* pozadí statusbaru */
    dl->AddRectFilled (
        ImVec2 ( sb_x, sb_y ),
        ImVec2 ( sb_x + sb_w, sb_y + STATUSBAR_HEIGHT ),
        IM_COL32 ( 18, 18, 32, 255 )
    );

    /* separator čára */
    dl->AddLine (
        ImVec2 ( sb_x, sb_y ),
        ImVec2 ( sb_x + sb_w, sb_y ),
        IM_COL32 ( 60, 70, 120, 255 ), 2.0f
    );

    float tx = sb_x + 10.0f;
    float ty = sb_y + 8.0f;
    ImU32 text_col = IM_COL32 ( 160, 160, 180, 255 );
    ImU32 sep_col = IM_COL32 ( 60, 60, 80, 255 );

    if ( !owner || !owner->has_disk ) {
        dl->AddText ( ImVec2 ( tx, ty ), IM_COL32 ( 100, 100, 120, 255 ),
                      _ ( "No disk loaded" ) );
        return;
    }

    /* 1. pozice: FS typ (s CP/M SD/HD rozlišením) */
    const char *fs = mzdisk_session_fs_type_str_detail ( &owner->detect_result );
    dl->AddText ( ImVec2 ( tx, ty ), text_col, fs );
    tx += ImGui::CalcTextSize ( fs ).x + 20.0f;

    dl->AddText ( ImVec2 ( tx, ty ), sep_col, "|" );
    tx += 20.0f;

    /* 2. pozice: celkový počet stop */
    char tracks_buf[16];
    if ( owner->disc.tracks_rules ) {
        snprintf ( tracks_buf, sizeof ( tracks_buf ), "%dT",
                   (int) owner->disc.tracks_rules->total_tracks );
    } else {
        snprintf ( tracks_buf, sizeof ( tracks_buf ), "?T" );
    }
    dl->AddText ( ImVec2 ( tx, ty ), text_col, tracks_buf );
    tx += ImGui::CalcTextSize ( tracks_buf ).x + 20.0f;

    dl->AddText ( ImVec2 ( tx, ty ), sep_col, "|" );
    tx += 20.0f;

    /* 3. pozice: název souboru (limitovaná délka) + Modified */
    const char *fname = strrchr ( owner->filepath, '/' );
    if ( !fname ) fname = strrchr ( owner->filepath, '\\' );
    if ( fname ) fname++; else fname = owner->filepath;

    char truncated[48];
    int flen = (int) strlen ( fname );
    if ( flen > 40 ) {
        snprintf ( truncated, sizeof ( truncated ), "%.18s...%.18s",
                   fname, fname + flen - 18 );
    } else {
        strncpy ( truncated, fname, sizeof ( truncated ) - 1 );
        truncated[sizeof ( truncated ) - 1] = '\0';
    }
    dl->AddText ( ImVec2 ( tx, ty ), text_col, truncated );
    tx += ImGui::CalcTextSize ( truncated ).x;

    /* dirty příznak */
    if ( owner->is_dirty ) {
        dl->AddText ( ImVec2 ( tx, ty ), IM_COL32 ( 255, 180, 80, 255 ), " - " );
        tx += ImGui::CalcTextSize ( " - " ).x;
        dl->AddText ( ImVec2 ( tx, ty ), IM_COL32 ( 255, 180, 80, 255 ),
                      _ ( "Modified" ) );
    }

    /* 4. pozice: poslední operace (zarovnáno vpravo) */
    float right_x = sb_x + sb_w - 10.0f;  /* pravý kraj pro right-align */
    if ( owner->last_op_status != LAST_OP_NONE ) {
        const char *status_str = ( owner->last_op_status == LAST_OP_OK )
            ? _ ( "OK" ) : _ ( "FAILED" );
        char op_text[192];
        snprintf ( op_text, sizeof ( op_text ), "%s - %s",
                   owner->last_op_msg, status_str );
        ImVec2 op_size = ImGui::CalcTextSize ( op_text );
        float op_x = right_x - op_size.x;
        ImU32 op_col = ( owner->last_op_status == LAST_OP_OK )
            ? IM_COL32 ( 80, 200, 80, 255 )
            : IM_COL32 ( 255, 80, 80, 255 );
        dl->AddText ( ImVec2 ( op_x, ty ), op_col, op_text );
        right_x = op_x - 20.0f;  /* posun pro další item vlevo */
    }

    /* Fokusový indikátor (který odkud odkazoval "→ focus: name") byl
       odstraněn 2026-04-21 - info bude dostupná přes stabilní titulky
       oken, viz TODO.md sekce "Okna: konzistentní border, titulky...". */
    (void) right_x;
}


static void render_toolbar ( st_MZDISK_SESSION *owner )
{
    bool has_disk = ( owner && owner->has_disk );

    ImGui::PushStyleVar ( ImGuiStyleVar_FramePadding, ImVec2 ( 6, 6 ) );
    ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 4, 4 ) );

    /* Open */
    bool can_open = ( owner != nullptr );
    if ( !can_open ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( ICON_IGFD_FOLDER ) ) {
        open_file_dialog ( owner );
    }
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) )
        ImGui::SetTooltip ( "%s", _ ( "Open Disk... (Ctrl+O)" ) );
    if ( !can_open ) ImGui::EndDisabled ();
    ImGui::SameLine ();

    /* Save */
    bool can_save = has_disk && owner->is_dirty;
    if ( !can_save ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( ICON_IGFD_SAVE ) ) {
        save_session_disk ( owner );
    }
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) )
        ImGui::SetTooltip ( "%s", _ ( "Save (Ctrl+S)" ) );
    if ( !can_save ) ImGui::EndDisabled ();
    ImGui::SameLine ();

    /* Reload */
    bool can_reload = has_disk;
    if ( !can_reload ) ImGui::BeginDisabled ();
    if ( ImGui::Button ( ICON_IGFD_REFRESH ) ) {
        if ( owner->is_dirty ) {
            owner->pending_reload = true;
            g_session_mgr.active_id = owner->id;
        } else {
            reload_session_disk ( owner );
        }
    }
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) )
        ImGui::SetTooltip ( "%s", _ ( "Reload DSK image" ) );
    if ( !can_reload ) ImGui::EndDisabled ();

    /* separátor */
    ImGui::SameLine ();
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ();

    /* New Disk */
    if ( ImGui::Button ( ICON_IGFD_ADD ) ) {
        panel_create_init ( &g_create_data, &g_config );
        g_create_data.is_open = true;
        g_create_target_id = owner ? owner->id : 0;
    }
    if ( ImGui::IsItemHovered () ) ImGui::SetTooltip ( "%s", _ ( "New Disk..." ) );

    ImGui::PopStyleVar ( 2 );
}



/* ==========================================================================
 *  Content area - panely pro aktivní disk
 * ========================================================================== */


/**
 * @brief Zkontroluje, zda panel právě změnil dirty/error stav, a aktualizuje last_op.
 *
 * Volá se po render funkci panelu. Porovná aktuální dirty/error stav s hodnotami
 * před voláním panelu a pokud došlo ke změně, nastaví last_op_status.
 *
 * @param s Session.
 * @param was_dirty Hodnota is_dirty před voláním panelu.
 * @param had_error Hodnota has_error panelu před voláním (nebo false pokud panel nemá has_error).
 * @param now_has_error Aktuální has_error panelu (nebo false).
 * @param panel_name Krátký název panelu pro last_op_msg (např. "FSMZ", "CP/M").
 */
static void check_panel_last_op ( st_MZDISK_SESSION *s, bool was_dirty,
                                   bool had_error, bool now_has_error,
                                   const char *panel_name )
{
    if ( now_has_error && !had_error ) {
        mzdisk_session_set_last_op ( s, LAST_OP_FAILED, panel_name );
    } else if ( s->is_dirty && !was_dirty ) {
        mzdisk_session_set_last_op ( s, LAST_OP_OK, panel_name );
    }
}


/**
 * @brief Vykreslí obsah pro konkrétní session (tabbar se všemi panely).
 *
 * Pokud s == NULL nebo je session EMPTY (has_disk=false), zobrazí uvítací
 * obrazovku bez tabbaru. Funkce je použitelná jak pro primární session
 * v hlavním okně, tak pro detached session v samostatných oknech.
 *
 * @param s Session, pro kterou se má vykreslit obsah (NULL nebo EMPTY = welcome).
 */
static void render_content_for ( st_MZDISK_SESSION *s )
{
    if ( !s || !s->has_disk ) {
        /* žádný disk - uvítací obrazovka (bez tabbaru, aby uživatele
         * nemátl prázdnými taby) */
        ImGui::Spacing ();
        ImGui::Spacing ();
        ImGui::Indent ( 40.0f );
        ImGui::TextUnformatted ( "mzdisk" );
        ImGui::SameLine ();
        ImGui::TextDisabled ( "v%s", MZDISK_VERSION );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s", _ ( "Sharp MZ disk image manager. Open a DSK file to begin." ) );
        ImGui::Spacing ();
        ImGui::TextDisabled ( "%s", _ ( "File > Open Disk... or click the folder icon in the toolbar." ) );
        ImGui::Spacing ();
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s",
                             _ ( "Disclaimer" ) );
        ImGui::Spacing ();
        ImGui::TextWrapped ( "%s",
            _ ( "Always work on a copy of your DSK image, never on the original." ) );
        ImGui::TextWrapped ( "%s",
            _ ( "The author provides no warranty and accepts no liability for "
                "data loss or damage resulting from use of the mzdisk project tools." ) );
        ImGui::TextWrapped ( "%s",
            _ ( "Distributed under the GPLv3 license, without warranty." ) );
        ImGui::Unindent ( 40.0f );
        return;
    }

    /* sub-taby pro různé pohledy na disk */
    if ( ImGui::BeginTabBar ( "ViewTabs", ImGuiTabBarFlags_Reorderable ) ) {

        if ( g_config.tab_visible[MZDISK_TAB_INFO] ) {
            if ( TabItemWithTooltip ( _ ( "Info" ), &g_config.tab_visible[MZDISK_TAB_INFO] ) ) {
                panel_info_render ( &s->info_data, &g_config );
                ImGui::EndTabItem ();
            }
        }
        if ( g_config.tab_visible[MZDISK_TAB_GEOMETRY] ) {
            if ( TabItemWithTooltip ( _ ( "Geometry" ), &g_config.tab_visible[MZDISK_TAB_GEOMETRY] ) ) {
                panel_geometry_render ( &s->geometry_data );
                ImGui::EndTabItem ();
            }
        }
        if ( g_config.tab_visible[MZDISK_TAB_GEOMETRY_EDIT] ) {
            if ( TabItemWithTooltip ( _ ( "Geometry Edit" ), &g_config.tab_visible[MZDISK_TAB_GEOMETRY_EDIT] ) ) {
                bool wd = s->is_dirty;
                bool we = s->geom_edit_data.is_error;
                bool needs_reload = false;
                panel_geom_edit_render ( &s->geom_edit_data, &s->geometry_data,
                                          &s->disc, &s->is_dirty, &needs_reload );
                check_panel_last_op ( s, wd, we, s->geom_edit_data.is_error, "Geometry" );
                if ( needs_reload ) {
                    mzdisk_session_reload_panels ( s );
                }
                ImGui::EndTabItem ();
            }
        }
        if ( g_config.tab_visible[MZDISK_TAB_BOOT_SECTOR] ) {
            if ( TabItemWithTooltip ( _ ( "Boot Sector" ), &g_config.tab_visible[MZDISK_TAB_BOOT_SECTOR] ) ) {
                bool wd = s->is_dirty;
                panel_boot_render ( &s->boot_data, &s->disc, &s->detect_result,
                                    &s->is_dirty, &g_config );
                check_panel_last_op ( s, wd, false, false, "Boot" );
                ImGui::EndTabItem ();
            }
        }
        if ( g_config.tab_visible[MZDISK_TAB_BLOCK_MAP] ) {
            if ( TabItemWithTooltip ( _ ( "Block Map" ), &g_config.tab_visible[MZDISK_TAB_BLOCK_MAP] ) ) {
                panel_map_render ( &s->map_data );
                ImGui::EndTabItem ();
            }
        }
        if ( g_config.tab_visible[MZDISK_TAB_HEXDUMP] ) {
            if ( TabItemWithTooltip ( _ ( "Hexdump" ), &g_config.tab_visible[MZDISK_TAB_HEXDUMP] ) ) {
                bool wd = s->is_dirty;
                panel_hexdump_render ( &s->hexdump_data, &s->disc, &s->detect_result,
                                       &s->raw_io_data, &s->is_dirty );
                check_panel_last_op ( s, wd, false, false, "Hexdump" );
                ImGui::EndTabItem ();
            }
        }

        /* FS-specifické panely */
        if ( s->detect_result.type == MZDSK_FS_FSMZ && s->fsmz_data.is_loaded ) {
            if ( g_config.tab_visible[MZDISK_TAB_FS_DIR] ) {
                if ( TabItemWithTooltip ( _ ( "FSMZ Directory" ), &g_config.tab_visible[MZDISK_TAB_FS_DIR] ) ) {
                    bool wd = s->is_dirty;
                    bool we = s->fsmz_data.has_error;
                    panel_fsmz_render ( &s->fsmz_data, &s->disc, &s->is_dirty, &g_config, s->id );
                    check_panel_last_op ( s, wd, we, s->fsmz_data.has_error, "FSMZ" );
                    ImGui::EndTabItem ();
                }
            }
            if ( g_config.tab_visible[MZDISK_TAB_FS_MAINT] ) {
                if ( TabItemWithTooltip ( _ ( "FSMZ Maintenance" ), &g_config.tab_visible[MZDISK_TAB_FS_MAINT] ) ) {
                    bool wd = s->is_dirty;
                    bool we = s->fsmz_data.has_error;
                    panel_fsmz_maintenance_render ( &s->fsmz_data, &s->disc, &s->is_dirty );
                    check_panel_last_op ( s, wd, we, s->fsmz_data.has_error, "FSMZ" );
                    ImGui::EndTabItem ();
                }
            }
        }
        if ( s->detect_result.type == MZDSK_FS_CPM && s->cpm_data.is_loaded ) {
            if ( g_config.tab_visible[MZDISK_TAB_FS_DIR] ) {
                if ( TabItemWithTooltip ( _ ( "CP/M Directory" ), &g_config.tab_visible[MZDISK_TAB_FS_DIR] ) ) {
                    bool wd = s->is_dirty;
                    bool we = s->cpm_data.has_error;
                    panel_cpm_render ( &s->cpm_data, &s->disc, &s->is_dirty, &g_config, s->id );
                    check_panel_last_op ( s, wd, we, s->cpm_data.has_error, "CP/M" );
                    ImGui::EndTabItem ();
                }
            }
            if ( g_config.tab_visible[MZDISK_TAB_FS_MAINT] ) {
                if ( TabItemWithTooltip ( _ ( "CP/M Maintenance" ), &g_config.tab_visible[MZDISK_TAB_FS_MAINT] ) ) {
                    bool wd = s->is_dirty;
                    bool we = s->cpm_data.has_error;
                    panel_cpm_maintenance_render ( &s->cpm_data, &s->disc,
                                                  &s->detect_result, &s->is_dirty );
                    check_panel_last_op ( s, wd, we, s->cpm_data.has_error, "CP/M" );
                    ImGui::EndTabItem ();
                }
            }
        }
        if ( s->detect_result.type == MZDSK_FS_MRS && s->mrs_data.is_loaded ) {
            if ( g_config.tab_visible[MZDISK_TAB_FS_DIR] ) {
                if ( TabItemWithTooltip ( _ ( "MRS Directory" ), &g_config.tab_visible[MZDISK_TAB_FS_DIR] ) ) {
                    bool wd = s->is_dirty;
                    bool we = s->mrs_data.has_error;
                    panel_mrs_render ( &s->mrs_data, &s->detect_result.mrs_config,
                                      &s->detect_result, &s->is_dirty, &g_config, s->id );
                    check_panel_last_op ( s, wd, we, s->mrs_data.has_error, "MRS" );
                    ImGui::EndTabItem ();
                }
            }
            if ( g_config.tab_visible[MZDISK_TAB_FS_MAINT] ) {
                if ( TabItemWithTooltip ( _ ( "MRS Maintenance" ), &g_config.tab_visible[MZDISK_TAB_FS_MAINT] ) ) {
                    bool wd = s->is_dirty;
                    bool we = s->mrs_data.has_error;
                    panel_mrs_maintenance_render ( &s->mrs_data, &s->detect_result.mrs_config,
                                                  &s->detect_result, &s->is_dirty );
                    check_panel_last_op ( s, wd, we, s->mrs_data.has_error, "MRS" );
                    ImGui::EndTabItem ();
                }
            }
        }

        ImGui::EndTabBar ();
    }
}


/* ==========================================================================
 *  Detached session okna (Fáze 2 multi-window)
 * ========================================================================== */

/**
 * @brief Vykreslí jednu detached session v samostatném ImGui okně.
 *
 * Detached okno má kompletní UI shell:
 *   - menubar (File/View/Help) - akce cílí na TUTO session
 *   - toolbar (Open/Save/Reload/New Disk) - akce cílí na TUTO session
 *   - content area (welcome pokud has_disk=false, jinak tabbar panelů)
 *   - statusbar (stav TÉTO session)
 *
 * Počáteční pozice je umístěna mimo main viewport, aby multi-viewport
 * spawn-nul vlastní OS okno (platform viewport). Titulek obsahuje ###id
 * suffix - oddělí viditelnou část od stabilního ID pro ImGui window state.
 *
 * Focus tracking: pokud uživatel interaguje s tímto oknem, nastaví
 * mgr->active_id na jeho id. Zkratky pak cílí na toto okno.
 *
 * Křížek okna: window_open -> false spustí close flow (čistá session
 * zavřít okamžitě, dirty přes unsaved dialog).
 *
 * @param s Session, která se má vykreslit. Musí být is_open a !is_primary.
 * @param running Pointer na main loop flag (pro File > Exit v menu okna).
 */
static void render_session_window ( st_MZDISK_SESSION *s, bool *running )
{
    if ( !s || !s->is_open || s->is_primary ) return;

    /* Počáteční pozice: drobně odsazená od levého horního rohu viditelné
     * oblasti hlavního viewportu. Offset ladí každou další detached
     * session podle id, aby se nepřekrývaly. Multi-viewport ImGui si sám
     * odpojí okno do samostatného OS okna, jakmile ho uživatel přetáhne
     * mimo hranice main viewportu; dřívější přístup (spawn přímo mimo
     * viewport) končil na single-monitor setupech mimo viditelnou plochu. */
    ImGuiViewport *main_vp = ImGui::GetMainViewport ();
    float offset = 30.0f * (float) ( s->id % 6 );
    float init_x = main_vp->WorkPos.x + 60.0f + offset;
    float init_y = main_vp->WorkPos.y + 60.0f + offset;
    ImGui::SetNextWindowSize ( ImVec2 ( 1000, 700 ), ImGuiCond_FirstUseEver );
    ImGui::SetNextWindowPos ( ImVec2 ( init_x, init_y ), ImGuiCond_FirstUseEver );

    /* Titulek: viditelná část ("mzdisk #NN [- name.dsk]") + ### +
     * klíč pro ImGui state. Klíč je odvozen ze session->id, které
     * je monotónní a stabilní přes celou dobu života session - i když
     * se v ní mění EMPTY → LOADED stav. Dříve používaný hash-by-filepath
     * vedl k poskakování okna při načtení DSK (okno dostalo nové ImGui
     * ID a ztratilo pozici). Za cenu: pozice okna se neukládá persistently
     * mezi spuštěními aplikace, což je akceptovatelný trade-off.
     *
     * NoSavedSettings používáme pro všechny detached sessions - id je
     * monotónní counter, následující spuštění by ho stejně neznalo. */
    char title_visible[280];
    mzdisk_session_format_window_title ( s, title_visible, sizeof ( title_visible ) );

    char title[320];
    snprintf ( title, sizeof ( title ),
               "%s###mzdisk-session-%llu",
               title_visible, (unsigned long long) s->id );

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse
                            | ImGuiWindowFlags_MenuBar
                            | ImGuiWindowFlags_NoSavedSettings;

    /* Padding sladěný s hlavním oknem. */
    ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 12, 0 ) );
    ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 0, 0 ) );

    bool was_open = s->window_open;
    bool opened = ImGui::Begin ( title, &s->window_open, wflags );

    if ( opened ) {
        /* Focus tracking - cílení globálních akcí na naposled fokusované okno. */
        if ( ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows ) ) {
            g_session_mgr.active_id = s->id;
        }

        /* menubar */
        ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 8, 4 ) );
        if ( ImGui::BeginMenuBar () ) {
            render_main_menu ( running, s );
            ImGui::EndMenuBar ();
        }
        ImGui::PopStyleVar ();

        /* toolbar */
        ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 8, 4 ) );
        ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 4, 4 ) );
        ImGui::BeginChild ( "##DetachedToolbar", ImVec2 ( 0, TOOLBAR_HEIGHT ),
                            ImGuiChildFlags_None );
        render_toolbar ( s );
        ImGui::EndChild ();
        ImGui::PopStyleVar ( 2 );

        /* content area */
        float content_bottom_pad = 8.0f;
        ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 16, 8 ) );
        ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 8, 4 ) );
        ImGui::BeginChild ( "##DetachedContent",
                            ImVec2 ( 0, -STATUSBAR_HEIGHT - content_bottom_pad ),
                            ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar );
        render_content_for ( s );
        ImGui::EndChild ();
        ImGui::PopStyleVar ( 2 );

        /* statusbar - kreslíme do draw listu tohoto okna v rezervovaném
         * pruhu dole. Získáme absolutní souřadnice obsahu okna. */
        {
            ImVec2 win_pos = ImGui::GetWindowPos ();
            ImVec2 win_size = ImGui::GetWindowSize ();
            float sb_x = win_pos.x;
            float sb_y = win_pos.y + win_size.y - STATUSBAR_HEIGHT;
            float sb_w = win_size.x;
            ImDrawList *dl = ImGui::GetWindowDrawList ();
            render_statusbar ( dl, sb_x, sb_y, sb_w, s );
        }
    }
    ImGui::End ();
    ImGui::PopStyleVar ( 2 );

    /* Křížek okna stisknut - Begin přepnul window_open na false. */
    if ( was_open && !s->window_open ) {
        if ( s->has_disk && s->is_dirty ) {
            /* Dirty: okno zůstává otevřené, dialog unsaved se rozběhne. */
            s->window_open = true;
            s->pending_close = true;
            g_session_mgr.active_id = s->id;
        } else {
            /* Čisté okno (ať už s čistým diskem nebo EMPTY) - zavřít. */
            mzdisk_session_close_by_id ( &g_session_mgr, s->id );
        }
    }
}


/**
 * @brief Vykreslí všechna detached session okna (non-primární otevřené).
 *
 * @par Pozn. iterace přes pole sessions[] indexem: render_session_window
 * může způsobit close_by_id (u čisté session při křížku), což vynuluje
 * slot. Nemění to platnost dalších indexů, protože pole je statické.
 */
static void render_detached_session_windows ( bool *running )
{
    for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
        st_MZDISK_SESSION *s = &g_session_mgr.sessions[i];
        if ( s->is_open && !s->is_primary ) {
            render_session_window ( s, running );
        }
    }
}


/* ==========================================================================
 *  Hlavní vstupní bod
 * ========================================================================== */

extern "C" int mzdisk_app_run ( int argc, char *argv[] )
{
    (void) argc;
    (void) argv;

    mzdisk_session_manager_init ( &g_session_mgr );
    dnd_init ( &g_session_mgr );

    /* --- SDL3 inicializace --- */

    if ( !SDL_Init ( SDL_INIT_VIDEO ) ) {
        fprintf ( stderr, "Error: SDL_Init failed: %s\n", SDL_GetError () );
        return EXIT_FAILURE;
    }

    SDL_GL_SetAttribute ( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
    SDL_GL_SetAttribute ( SDL_GL_CONTEXT_MINOR_VERSION, 3 );
    SDL_GL_SetAttribute ( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
    SDL_GL_SetAttribute ( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute ( SDL_GL_DEPTH_SIZE, 24 );
    SDL_GL_SetAttribute ( SDL_GL_STENCIL_SIZE, 8 );

    SDL_Window *window = SDL_CreateWindow (
        MZDISK_APP_NAME " v" MZDISK_VERSION,
        MZDISK_DEFAULT_WINDOW_WIDTH,
        MZDISK_DEFAULT_WINDOW_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if ( !window ) {
        fprintf ( stderr, "Error: SDL_CreateWindow failed: %s\n", SDL_GetError () );
        SDL_Quit ();
        return EXIT_FAILURE;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext ( window );
    if ( !gl_context ) {
        fprintf ( stderr, "Error: SDL_GL_CreateContext failed: %s\n", SDL_GetError () );
        SDL_DestroyWindow ( window );
        SDL_Quit ();
        return EXIT_FAILURE;
    }

    SDL_GL_MakeCurrent ( window, gl_context );
    SDL_GL_SetSwapInterval ( 1 );

    /* Posunout okno o malý offset - každá instance bude jinde.
     * Seed kombinuje SDL_GetTicks (hrubá milisekundová granularita)
     * s PID (nízké 16 bitů), aby rychlý spawn dvou instancí nedával
     * stejný offset (audit L-20). */
    {
        static unsigned seed = 0;
        if ( !seed ) {
            unsigned ticks = (unsigned) SDL_GetTicks ();
#ifdef _WIN32
            unsigned pid = (unsigned) GetCurrentProcessId ();
#else
            unsigned pid = (unsigned) getpid ();
#endif
            seed = ticks ^ ( pid << 8 ) ^ pid;
            if ( !seed ) seed = 1u; /* never 0 */
        }
        int offset_x = 30 + (int) ( seed % 120 );
        int offset_y = 30 + (int) ( ( seed * 7 ) % 80 );
        int wx, wy;
        SDL_GetWindowPosition ( window, &wx, &wy );
        SDL_SetWindowPosition ( window, wx + offset_x, wy + offset_y );
    }

    /* --- Inicializace driverů a konfigurace --- */
    memory_driver_init ();
    mzdisk_config_init ( &g_config );
    mzdisk_config_load ( &g_config );
    /* Watch baseline zachytí aktuální signaturu mzdisk.ini, aby
     * následné vlastní ani externí změny byly správně detekovány. */
    mzdisk_config_watch_init ( &g_config_watch, MZDISK_CONFIG_FILENAME );
    mzdisk_config_watch_mark_saved ( &g_config_watch );
    panel_create_init ( &g_create_data, &g_config );
    panel_settings_init ( &g_settings_data );
    ImGuiFileDialog::Instance ()->SetPlacesPaneWidth ( g_config.filebrowser_splitter_w );

    /* nastavit locale podle uloženého jazyka */
    i18n_set_language ( g_config.language );

    /* --- ImGui inicializace --- */

    IMGUI_CHECKVERSION ();
    ImGui::CreateContext ();

    ImGuiIO &io = ImGui::GetIO ();
    io.IniFilename = "mzdisk-imgui.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  /* pro modální dialogy mimo hlavní okno */

    setup_theme ( g_config.theme_idx );

    /* viewports: zajistit neprůhledné pozadí oken */
    if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
        ImGuiStyle &vstyle = ImGui::GetStyle ();
        vstyle.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    load_fonts ( io, g_config.font_family_idx );
    apply_font_scale ( g_config.font_size );

    ImGui_ImplSDL3_InitForOpenGL ( window, gl_context );
    ImGui_ImplOpenGL3_Init ( "#version 330" );

    /* IGFD ikony pro typy souborů */
    {
        ImVec4 dsk_color = ImVec4 ( 0.0f, 1.0f, 1.0f, 0.9f );
        ImVec4 mzf_color = ImVec4 ( 0.0f, 1.0f, 1.0f, 0.9f );

        ImGuiFileDialog::Instance ()->SetFileStyle ( IGFD_FileStyleByExtention, ".dsk", dsk_color, ICON_MZGLYPH_FLOPPY_DISC );
        ImGuiFileDialog::Instance ()->SetFileStyle ( IGFD_FileStyleByExtention, ".DSK", dsk_color, ICON_MZGLYPH_FLOPPY_DISC );
        ImGuiFileDialog::Instance ()->SetFileStyle ( IGFD_FileStyleByExtention, ".mzf", mzf_color, ICON_MZGLYPH_CASETTE );
        ImGuiFileDialog::Instance ()->SetFileStyle ( IGFD_FileStyleByExtention, ".MZF", mzf_color, ICON_MZGLYPH_CASETTE );
        ImGuiFileDialog::Instance ()->SetFileStyle ( IGFD_FileStyleByExtention, ".m12", mzf_color, ICON_MZGLYPH_CASETTE );
        ImGuiFileDialog::Instance ()->SetFileStyle ( IGFD_FileStyleByExtention, ".M12", mzf_color, ICON_MZGLYPH_CASETTE );
    }

    /* Prázdná primární session pro hlavní okno - uživatel tak hned vidí
     * welcome screen a File > Open / New Window funguje v hlavním menu.
     * Bez toho by se první kliknutí na New Window "ztratilo" tím, že by
     * naplnilo primární slot místo otevření detached okna. */
    mzdisk_session_create_empty ( &g_session_mgr, true );

    /* --- Hlavní smyčka --- */

    bool running = true;

    while ( running ) {

        SDL_Event event;
        while ( SDL_PollEvent ( &event ) ) {
            ImGui_ImplSDL3_ProcessEvent ( &event );
            if ( event.type == SDL_EVENT_QUIT )
                request_exit ( &running );
            if ( event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                 && event.window.windowID == SDL_GetWindowID ( window ) )
                request_exit ( &running );
        }

        if ( SDL_GetWindowFlags ( window ) & SDL_WINDOW_MINIMIZED ) {
            SDL_Delay ( 100 );
            continue;
        }

        /* Poll externí změny mzdisk.ini (jiná instance GUI zapsala
         * config). Throttlováno interně na ~2 Hz. Při detekci se načte
         * nový config, diff proti stávajícímu g_config a apply - může
         * zvednout g_fonts_need_rebuild, proto musí proběhnout před
         * kontrolou tohoto flagu níže. */
        if ( mzdisk_config_watch_poll ( &g_config_watch, SDL_GetTicks () ) ) {
            st_MZDISK_CONFIG new_cfg;
            mzdisk_config_init ( &new_cfg );
            if ( mzdisk_config_load ( &new_cfg ) ) {
                apply_external_config_change ( &g_config, &new_cfg );
                g_config = new_cfg;
            }
        }

        /* přebudovat font atlas pokud se změnila rodina fontu */
        if ( g_fonts_need_rebuild ) {
            io.Fonts->Clear ();
            load_fonts ( io, g_config.font_family_idx );
            g_fonts_need_rebuild = false;
        }

        ImGui_ImplOpenGL3_NewFrame ();
        ImGui_ImplSDL3_NewFrame ();
        ImGui::NewFrame ();

        /* globální klávesové zkratky */
        process_shortcuts ( &running );

        /* --- Fullscreen okno přes celý viewport --- */

        ImGuiViewport *vp = ImGui::GetMainViewport ();
        ImGui::SetNextWindowPos ( vp->WorkPos );
        ImGui::SetNextWindowSize ( vp->WorkSize );

        ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar
                                | ImGuiWindowFlags_NoResize
                                | ImGuiWindowFlags_NoMove
                                | ImGuiWindowFlags_NoCollapse
                                | ImGuiWindowFlags_NoBringToFrontOnFocus
                                | ImGuiWindowFlags_NoScrollbar
                                | ImGuiWindowFlags_NoScrollWithMouse
                                | ImGuiWindowFlags_MenuBar;

        ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 12, 0 ) );
        ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 0, 0 ) );
        ImGui::Begin ( "##MainWindow", nullptr, wflags );

        /* Hlavní okno patří primární session (nebo NULL pokud žádná). */
        st_MZDISK_SESSION *main_owner = mzdisk_session_get_primary ( &g_session_mgr );

        /* Aktualizovat SDL titulek hlavního OS okna - reflektuje stav disku. */
        {
            char wt[280];
            mzdisk_session_format_window_title ( main_owner, wt, sizeof ( wt ) );
            /* SDL_SetWindowTitle je volné - nastavuje jen pokud se liší. */
            static char last_wt[280] = "";
            if ( strcmp ( wt, last_wt ) != 0 ) {
                SDL_SetWindowTitle ( window, wt );
                strncpy ( last_wt, wt, sizeof ( last_wt ) - 1 );
                last_wt[sizeof ( last_wt ) - 1] = '\0';
            }
        }

        /* menu bar (uvnitř okna) - potřebuje normální ItemSpacing */
        ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 8, 4 ) );
        if ( ImGui::BeginMenuBar () ) {
            render_main_menu ( &running, main_owner );
            ImGui::EndMenuBar ();
        }
        ImGui::PopStyleVar ();

        /* toolbar */
        ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 8, 4 ) );
        ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 4, 4 ) );
        ImGui::BeginChild ( "##Toolbar", ImVec2 ( 0, TOOLBAR_HEIGHT ), ImGuiChildFlags_None );
        render_toolbar ( main_owner );
        ImGui::EndChild ();
        ImGui::PopStyleVar ( 2 );

        /* content area - zabere vše kromě statusbaru + 8px spodní mezera */
        float content_bottom_pad = 8.0f;
        ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 16, 8 ) );
        ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 8, 4 ) );
        ImGui::BeginChild ( "##Content", ImVec2 ( 0, -STATUSBAR_HEIGHT - content_bottom_pad ),
                            ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar );
        /* Hlavní okno renderuje primární session (main_owner). */
        render_content_for ( main_owner );
        ImGui::EndChild ();
        ImGui::PopStyleVar ( 2 );

        ImGui::PopStyleVar ( 2 ); /* WindowPadding + ItemSpacing z Begin */

        /* statusbar hlavního okna - kreslíme přes foreground DrawList
         * hlavního viewportu (fixní pozice v dolním pruhu) */
        {
            ImGuiViewport *svp = ImGui::GetMainViewport ();
            ImDrawList *dl = ImGui::GetForegroundDrawList ( svp );
            float sb_y = svp->WorkPos.y + svp->WorkSize.y - STATUSBAR_HEIGHT;
            float sb_x = svp->WorkPos.x;
            float sb_w = svp->WorkSize.x;
            render_statusbar ( dl, sb_x, sb_y, sb_w, main_owner );
        }

        ImGui::End (); /* ##MainWindow */

        /* detached session okna - každá neprimární otevřená session má
         * vlastní ImGui okno s plným UI shell (menubar/toolbar/content/
         * statusbar). Multi-viewport je spawn-ne jako OS okna. */
        render_detached_session_windows ( &running );

        /* file dialog */
        process_file_dialog ();

        /* Create New Disk okno */
        if ( g_create_data.is_open ) {
            panel_create_render ( &g_create_data, &g_config );
        }

        /* Settings okno - změny se zobrazují jako preview, uloží se až na
         * Apply/OK. Cancel/křížek restore cfg ze snapshotu (viz panel_settings). */
        if ( g_settings_data.is_open ) {
            panel_settings_render ( &g_settings_data, &g_config );
        }
        /* Zpracovat change flags (preview refresh) - spouští se jak při
         * interaktivní úpravě uvnitř dialogu, tak při Cancel restore. */
        if ( g_settings_data.font_family_changed ) {
            g_fonts_need_rebuild = true;
            g_settings_data.font_family_changed = false;
        }
        if ( g_settings_data.font_size_changed ) {
            apply_font_scale ( g_config.font_size );
            g_settings_data.font_size_changed = false;
        }
        if ( g_settings_data.theme_changed ) {
            setup_theme ( g_config.theme_idx );
            if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
                ImGui::GetStyle ().Colors[ImGuiCol_WindowBg].w = 1.0f;
            }
            g_settings_data.theme_changed = false;
        }
        if ( g_settings_data.lang_changed ) {
            i18n_set_language ( g_config.language );
            g_settings_data.lang_changed = false;
        }
        /* Apply/OK uloží config do INI + oznámí přes file watch. */
        if ( g_settings_data.save_requested ) {
            save_and_mark ();
            g_settings_data.save_requested = false;
        }

        /* Raw I/O okna - jedno na každou otevřenou session, která má
         * raw_io_data.is_open = true. Panel sám drží vlastní okno
         * (ImGui::Begin uvnitř panel_raw_io_render). Každé má titulek
         * s session id, aby ImGui odlišil jejich window state. */
        for ( int i = 0; i < MZDISK_MAX_SESSIONS; i++ ) {
            st_MZDISK_SESSION *rio_s = &g_session_mgr.sessions[i];
            if ( rio_s->is_open && rio_s->raw_io_data.is_open ) {
                bool wd = rio_s->is_dirty;
                panel_raw_io_render ( &rio_s->raw_io_data, &rio_s->disc,
                                       &rio_s->hexdump_data,
                                       &rio_s->is_dirty, &g_config, rio_s->id );
                check_panel_last_op ( rio_s, wd, false, false, "Raw I/O" );
            }
        }

        /* Auto-open po vytvoření nového disku - cíl je session, ze které byla
         * akce vyvolána (g_create_target_id). Pokud target neexistuje nebo
         * byl mezitím zavřen, otevře se do primární (nebo nové) session. */
        if ( g_create_data.created ) {
            g_create_data.created = false;
            g_create_data.is_open = false;
            st_MZDISK_SESSION *target = mzdisk_session_get_by_id ( &g_session_mgr,
                                                                    g_create_target_id );
            g_create_target_id = 0;
            if ( !target ) {
                target = mzdisk_session_get_primary ( &g_session_mgr );
            }
            if ( !target ) {
                /* fallback - vytvoř novou primární session pokud žádná není */
                target = mzdisk_session_create_empty ( &g_session_mgr, true );
            }
            if ( target ) {
                open_disk_in_session ( target, g_create_data.created_filepath );
            }
            /* Uložit last_create_dir do configu */
            strncpy ( g_config.last_create_dir, g_create_data.directory,
                      sizeof ( g_config.last_create_dir ) - 1 );
            g_config.last_create_dir[sizeof ( g_config.last_create_dir ) - 1] = '\0';
            save_and_mark ();
        }

        /* konfirmační dialog */
        render_about_dialog ();
        render_unsaved_dialog ();
        render_exit_unsaved_dialog ( &running );
        render_reload_dialog ();
        render_dnd_ask_dialog ();

        /* --- OpenGL rendering --- */

        ImGui::Render ();
        glViewport ( 0, 0, (int) io.DisplaySize.x, (int) io.DisplaySize.y );
        glClearColor ( 0.06f, 0.06f, 0.10f, 1.00f );
        glClear ( GL_COLOR_BUFFER_BIT );
        ImGui_ImplOpenGL3_RenderDrawData ( ImGui::GetDrawData () );

        /* multi-viewport: renderování oken mimo hlavní okno (file dialog, atd.) */
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
            SDL_GLContext backup_context = SDL_GL_GetCurrentContext ();
            SDL_Window *backup_window = SDL_GL_GetCurrentWindow ();
            ImGui::UpdatePlatformWindows ();
            ImGui::RenderPlatformWindowsDefault ();
            SDL_GL_MakeCurrent ( backup_window, backup_context );
        }

        SDL_GL_SwapWindow ( window );
    }

    /* --- Úklid --- */

    g_config.filebrowser_splitter_w = ImGuiFileDialog::Instance ()->GetPlacesPaneWidth ();
    save_and_mark ();
    mzdisk_session_close_all ( &g_session_mgr );

    /* uvolnění textury loga */
    mzdisk_texture_free ( g_logo_texture );

    /* uvolnění textur vlajek v panel_settings (audit M-33) */
    panel_settings_shutdown ();

    ImGui_ImplOpenGL3_Shutdown ();
    ImGui_ImplSDL3_Shutdown ();
    ImGui::DestroyContext ();

    SDL_GL_DestroyContext ( gl_context );
    SDL_DestroyWindow ( window );
    SDL_Quit ();

    return EXIT_SUCCESS;
}
