/**
 * @file panel_map_imgui.cpp
 * @brief ImGui rendering vizuální blokové mapy disku.
 *
 * Vykresluje barevnou mřížku bloků pomocí ImGui DrawList.
 * Každý blok je malý obdélník, barva odpovídá typu (soubor, volný, systém, ...).
 * Hover tooltip zobrazuje číslo bloku a jeho typ.
 *
 * Pro CP/M disky navíc vykresluje "Disk Layout" - horizontální proporcionální
 * pruh zobrazující oblasti disku (Boot Track, System Tracks, Directory,
 * File Data, Free Space).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"

extern "C" {
#include "panels/panel_map.h"
#include "i18n.h"
}


/** @brief Velikost jedné buňky mapy v pixelech. */
static const float CELL_SIZE = 20.0f;

/** @brief Mezera mezi buňkami v pixelech. */
static const float CELL_GAP = 1.0f;


/**
 * @brief Vrátí barvu pro daný typ bloku.
 *
 * @param type Typ bloku.
 * @return Barva jako ImU32 (pro DrawList).
 */
static ImU32 block_color ( en_MAP_BLOCK_TYPE type )
{
    switch ( type ) {
        case MAP_BLOCK_FREE:        return IM_COL32 ( 40,  60,  40,  255 );   /* tmavě zelená */
        case MAP_BLOCK_FILE:        return IM_COL32 ( 76,  175, 80,  255 );   /* zelená */
        case MAP_BLOCK_SYSTEM:      return IM_COL32 ( 156, 39,  176, 255 );   /* fialová */
        case MAP_BLOCK_META:        return IM_COL32 ( 255, 152, 0,   255 );   /* oranžová */
        case MAP_BLOCK_DIR:         return IM_COL32 ( 33,  150, 243, 255 );   /* modrá */
        case MAP_BLOCK_BOOTSTRAP:   return IM_COL32 ( 121, 85,  200, 255 );   /* světle fialová */
        case MAP_BLOCK_BAD:         return IM_COL32 ( 244, 67,  54,  255 );   /* červená */
        case MAP_BLOCK_RESERVED:    return IM_COL32 ( 96,  125, 139, 255 );   /* šedomodrá */
        case MAP_BLOCK_OVER:        return IM_COL32 ( 50,  50,  50,  255 );   /* tmavě šedá */
        default:                    return IM_COL32 ( 30,  30,  30,  255 );
    }
}


/**
 * @brief Vrátí textový popis typu bloku (pro tooltip).
 *
 * @param type Typ bloku.
 * @return Statický řetězec.
 */
static const char* block_type_str ( en_MAP_BLOCK_TYPE type )
{
    switch ( type ) {
        case MAP_BLOCK_FREE:        return "Free";
        case MAP_BLOCK_FILE:        return "File";
        case MAP_BLOCK_SYSTEM:      return "System";
        case MAP_BLOCK_META:        return "Metadata";
        case MAP_BLOCK_DIR:         return "Directory";
        case MAP_BLOCK_BOOTSTRAP:   return "Bootstrap";
        case MAP_BLOCK_BAD:         return "Bad";
        case MAP_BLOCK_RESERVED:    return "Reserved";
        case MAP_BLOCK_OVER:        return "Over area";
        default:                    return "Unknown";
    }
}


/**
 * @brief Vykreslí legendu barev.
 *
 * Zobrazí barevné čtverečky s popisem typu bloku.
 *
 * @param data Datový model (pro filtrování - zobrazíme jen přítomné typy).
 */
static void render_legend ( const st_PANEL_MAP_DATA *data )
{
    struct { en_MAP_BLOCK_TYPE type; int count; } items[] = {
        { MAP_BLOCK_FILE,       data->file_count },
        { MAP_BLOCK_FREE,       data->free_count },
        { MAP_BLOCK_DIR,        data->dir_count },
        { MAP_BLOCK_SYSTEM,     data->system_count },
        { MAP_BLOCK_META,       data->meta_count },
        { MAP_BLOCK_BAD,        data->bad_count },
    };

    for ( int i = 0; i < (int) ( sizeof ( items ) / sizeof ( items[0] ) ); i++ ) {
        if ( items[i].count == 0 ) continue;

        ImVec2 pos = ImGui::GetCursorScreenPos ();
        ImDrawList *dl = ImGui::GetWindowDrawList ();
        dl->AddRectFilled ( pos, ImVec2 ( pos.x + 24, pos.y + 24 ),
                            block_color ( items[i].type ) );
        ImGui::Dummy ( ImVec2 ( 26, 26 ) );
        ImGui::SameLine ();
        ImGui::Text ( "%s: %d", block_type_str ( items[i].type ), items[i].count );
        ImGui::SameLine ( 0, 20 );
    }
    ImGui::NewLine ();
}


/**
 * @brief Vykreslí řadu barevných bloků s tooltipem.
 *
 * Společná pomocná funkce pro vykreslení mřížky bloků.
 * Počítá sloupce automaticky podle dostupné šířky.
 *
 * @param blocks     Pole typů bloků.
 * @param count      Počet bloků.
 * @param label      Popisek pro tooltip (např. "Block" nebo "Sector").
 */
static void render_block_cells ( const en_MAP_BLOCK_TYPE *blocks, int count, const char *label )
{
    ImVec2 avail = ImGui::GetContentRegionAvail ();
    float step = CELL_SIZE + CELL_GAP;
    int cols = (int) ( avail.x / step );
    if ( cols < 1 ) cols = 1;

    ImVec2 origin = ImGui::GetCursorScreenPos ();
    ImDrawList *dl = ImGui::GetWindowDrawList ();
    ImVec2 mouse = ImGui::GetMousePos ();

    int hovered_block = -1;

    for ( int i = 0; i < count; i++ ) {
        int col = i % cols;
        int row = i / cols;

        ImVec2 p0 = ImVec2 ( origin.x + col * step, origin.y + row * step );
        ImVec2 p1 = ImVec2 ( p0.x + CELL_SIZE, p0.y + CELL_SIZE );

        dl->AddRectFilled ( p0, p1, block_color ( blocks[i] ) );

        /* detekce hover */
        if ( mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y ) {
            hovered_block = i;
            dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
        }
    }

    /* dummy pro správný scroll a layout */
    int total_rows = ( count + cols - 1 ) / cols;
    ImGui::Dummy ( ImVec2 ( avail.x, total_rows * step ) );

    /* tooltip */
    if ( hovered_block >= 0 ) {
        ImGui::BeginTooltip ();
        ImGui::Text ( "%s %d", label, hovered_block );
        ImGui::Text ( "%s: %s", _ ( "Type" ), block_type_str ( blocks[hovered_block] ) );
        ImGui::EndTooltip ();
    }
}


/** @brief Výška pruhu disk layout v pixelech. */
static const float LAYOUT_BAR_HEIGHT = 36.0f;

/** @brief Minimální šířka segmentu v pixelech (aby malé oblasti byly čitelné). */
static const float LAYOUT_MIN_SEGMENT_WIDTH = 60.0f;


/**
 * @brief Vykreslí horizontální proporcionální pruh oblastí disku.
 *
 * Zobrazuje oblasti disku (Boot Track, System Tracks, Directory,
 * File Data, Free Space) jako barevné segmenty s šířkou úměrnou
 * velikosti oblasti. Malé segmenty dostanou minimální šířku, aby
 * byly viditelné a čitelné. Hover tooltip zobrazuje detail segmentu.
 *
 * Pod pruhem vykreslí legendu s popisem segmentů.
 *
 * @param data Datový model mapy s naplněným disk layout.
 */
static void render_disk_layout ( const st_PANEL_MAP_DATA *data )
{
    ImGui::SeparatorText ( _ ( "Disk Layout" ) );

    ImVec2 avail = ImGui::GetContentRegionAvail ();
    float bar_width = avail.x;
    if ( bar_width < 100.0f ) bar_width = 100.0f;

    /* dvouprůchodový výpočet šířek: malé segmenty dostanou minimum,
       zbytek se proporcionálně rozdělí mezi velké segmenty */
    float widths[PANEL_MAP_MAX_LAYOUT_SEGMENTS];
    float reserved_width = 0.0f;
    float remaining_bytes = 0.0f;

    for ( int i = 0; i < data->layout_segment_count; i++ ) {
        float ratio = (float) data->layout_segments[i].size_bytes / (float) data->layout_total_bytes;
        float w = ratio * bar_width;
        if ( w < LAYOUT_MIN_SEGMENT_WIDTH ) {
            widths[i] = LAYOUT_MIN_SEGMENT_WIDTH;
            reserved_width += LAYOUT_MIN_SEGMENT_WIDTH;
        } else {
            widths[i] = 0.0f; /* dopočítáme ve druhém průchodu */
            remaining_bytes += (float) data->layout_segments[i].size_bytes;
        }
    }

    /* druhý průchod: rozdělení zbývajícího prostoru */
    float remaining_width = bar_width - reserved_width;
    if ( remaining_width < 0.0f ) remaining_width = 0.0f;

    for ( int i = 0; i < data->layout_segment_count; i++ ) {
        if ( widths[i] == 0.0f && remaining_bytes > 0.0f ) {
            widths[i] = ( (float) data->layout_segments[i].size_bytes / remaining_bytes ) * remaining_width;
        }
    }

    ImVec2 origin = ImGui::GetCursorScreenPos ();
    ImDrawList *dl = ImGui::GetWindowDrawList ();
    ImVec2 mouse = ImGui::GetMousePos ();

    int hovered_segment = -1;
    float x = origin.x;

    for ( int i = 0; i < data->layout_segment_count; i++ ) {
        const st_PANEL_MAP_LAYOUT_SEGMENT *seg = &data->layout_segments[i];
        float w = widths[i];

        ImVec2 p0 = ImVec2 ( x, origin.y );
        ImVec2 p1 = ImVec2 ( x + w, origin.y + LAYOUT_BAR_HEIGHT );

        dl->AddRectFilled ( p0, p1, block_color ( seg->type ) );

        /* tenký oddělovač mezi segmenty */
        if ( i > 0 ) {
            dl->AddLine ( ImVec2 ( x, origin.y ), ImVec2 ( x, origin.y + LAYOUT_BAR_HEIGHT ),
                          IM_COL32 ( 20, 20, 20, 255 ), 1.0f );
        }

        /* popisek uvnitř segmentu (pokud se vejde) */
        ImVec2 text_size = ImGui::CalcTextSize ( seg->label );
        if ( text_size.x + 4.0f < w ) {
            float tx = x + ( w - text_size.x ) * 0.5f;
            float ty = origin.y + ( LAYOUT_BAR_HEIGHT - text_size.y ) * 0.5f;
            dl->AddText ( ImVec2 ( tx, ty ), IM_COL32 ( 255, 255, 255, 220 ), seg->label );
        }

        /* detekce hover */
        if ( mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y ) {
            hovered_segment = i;
            dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
        }

        x += w;
    }

    /* obrys celého pruhu */
    dl->AddRect ( origin, ImVec2 ( origin.x + bar_width, origin.y + LAYOUT_BAR_HEIGHT ),
                  IM_COL32 ( 80, 80, 80, 255 ) );

    /* dummy pro layout */
    ImGui::Dummy ( ImVec2 ( bar_width, LAYOUT_BAR_HEIGHT ) );

    /* tooltip */
    if ( hovered_segment >= 0 ) {
        const st_PANEL_MAP_LAYOUT_SEGMENT *seg = &data->layout_segments[hovered_segment];
        ImGui::BeginTooltip ();
        ImGui::Text ( "%s", seg->label );
        ImGui::Text ( "%s", seg->detail );
        ImGui::Text ( "%s: %u B (%.1f KB)", _ ( "Size" ),
                      seg->size_bytes, (float) seg->size_bytes / 1024.0f );
        ImGui::EndTooltip ();
    }

    /* legenda pod pruhem */
    ImGui::Spacing ();
    for ( int i = 0; i < data->layout_segment_count; i++ ) {
        const st_PANEL_MAP_LAYOUT_SEGMENT *seg = &data->layout_segments[i];
        ImVec2 pos = ImGui::GetCursorScreenPos ();
        dl = ImGui::GetWindowDrawList ();
        dl->AddRectFilled ( pos, ImVec2 ( pos.x + 16, pos.y + 16 ), block_color ( seg->type ) );
        ImGui::Dummy ( ImVec2 ( 18, 18 ) );
        ImGui::SameLine ();
        ImGui::Text ( "%s (%.1f%%)", seg->label,
                      100.0f * (float) seg->size_bytes / (float) data->layout_total_bytes );
        ImGui::SameLine ( 0, 20 );
    }
    ImGui::NewLine ();
}


/**
 * @brief Vykreslí legendu pro boot track mapu.
 *
 * Spočítá typy bloků přítomné v boot_blocks[] a zobrazí
 * barevné čtverečky s popisem a počtem.
 *
 * @param data Datový model mapy s naplněnou boot_blocks[].
 */
static void render_boot_legend ( const st_PANEL_MAP_DATA *data )
{
    /* spočítat typy přítomné v boot blocích */
    int count_free = 0, count_system = 0, count_bootstrap = 0;
    for ( int i = 0; i < data->boot_block_count; i++ ) {
        switch ( data->boot_blocks[i] ) {
            case MAP_BLOCK_FREE:        count_free++; break;
            case MAP_BLOCK_SYSTEM:      count_system++; break;
            case MAP_BLOCK_BOOTSTRAP:   count_bootstrap++; break;
            default: break;
        }
    }

    struct { en_MAP_BLOCK_TYPE type; int count; } items[] = {
        { MAP_BLOCK_SYSTEM,     count_system },
        { MAP_BLOCK_BOOTSTRAP,  count_bootstrap },
        { MAP_BLOCK_FREE,       count_free },
    };

    for ( int i = 0; i < (int) ( sizeof ( items ) / sizeof ( items[0] ) ); i++ ) {
        if ( items[i].count == 0 ) continue;

        ImVec2 pos = ImGui::GetCursorScreenPos ();
        ImDrawList *dl = ImGui::GetWindowDrawList ();
        dl->AddRectFilled ( pos, ImVec2 ( pos.x + 24, pos.y + 24 ),
                            block_color ( items[i].type ) );
        ImGui::Dummy ( ImVec2 ( 26, 26 ) );
        ImGui::SameLine ();
        ImGui::Text ( "%s: %d", block_type_str ( items[i].type ), items[i].count );
        ImGui::SameLine ( 0, 20 );
    }
    ImGui::NewLine ();
}


/**
 * @brief Vykreslí boot track mapu (16 bloků stopy 0).
 *
 * Zobrazí nadpis "FSMZ Boot track", legendu barev a jednu řadu 16 bloků.
 * Používá se pro non-FSMZ systémy (CP/M, MRS), kde stopa 0
 * obsahuje IPLPRO a bootstrap loader.
 *
 * @param data Datový model mapy s naplněnou boot_blocks[].
 */
static void render_boot_track ( const st_PANEL_MAP_DATA *data )
{
    ImGui::SeparatorText ( _ ( "FSMZ Boot track" ) );
    render_boot_legend ( data );
    ImGui::Spacing ();
    render_block_cells ( data->boot_blocks, data->boot_block_count, _ ( "Sector" ) );
}


/**
 * @brief Vykreslí filesystem mapu ve scrollovací oblasti.
 *
 * Každý blok je barevný obdélník. Hover zobrazuje tooltip s číslem a typem.
 * Mřížka je umístěna v BeginChild oblasti s vlastním scrollbarem.
 *
 * @param data  Datový model mapy.
 * @param title Nadpis sekce (např. "CP/M Allocation map").
 */
static void render_grid ( const st_PANEL_MAP_DATA *data, const char *title )
{
    ImGui::SeparatorText ( title );

    /* scrollovací child oblast zabere veškerý zbývající prostor */
    ImVec2 avail = ImGui::GetContentRegionAvail ();
    if ( !ImGui::BeginChild ( "##block_grid", avail, ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar ) ) {
        ImGui::EndChild ();
        return;
    }

    render_block_cells ( data->blocks, data->block_count, _ ( "Block" ) );

    ImGui::EndChild ();
}


/**
 * @brief Vrátí nadpis filesystem mapy podle typu FS.
 *
 * @param fs_type Typ filesystému.
 * @return Statický řetězec s nadpisem.
 */
static const char* fs_map_title ( en_MZDSK_FS_TYPE fs_type )
{
    switch ( fs_type ) {
        case MZDSK_FS_CPM:       return _ ( "CP/M Allocation map" );
        case MZDSK_FS_MRS:       return _ ( "MRS Allocation map" );
        case MZDSK_FS_FSMZ:      return _ ( "FSMZ Block map" );
        case MZDSK_FS_BOOT_ONLY: return _ ( "FSMZ Block map" );
        default:                 return _ ( "Block map" );
    }
}


extern "C" void panel_map_render ( const st_PANEL_MAP_DATA *data )
{
    if ( !data || !data->is_loaded ) {
        ImGui::TextDisabled ( "%s", _ ( "No block map available for this disk." ) );
        return;
    }

    /* statistiky - zobrazíme jen pokud máme filesystem mapu */
    if ( data->block_count > 0 ) {
        ImGui::Text ( "%s: %d", _ ( "Total blocks" ), data->block_count );
        ImGui::SameLine ( 0, 20 );
        ImGui::Text ( "%s: %d", _ ( "Free" ), data->free_count );
        ImGui::SameLine ( 0, 20 );
        ImGui::Text ( "%s: %d", _ ( "Used" ), data->block_count - data->free_count );

        ImGui::Spacing ();
        render_legend ( data );
    }

    /* disk layout pruh (CP/M) */
    if ( data->has_disk_layout ) {
        render_disk_layout ( data );
        ImGui::Spacing ();
    }

    /* boot track mapa pro non-FSMZ systémy */
    if ( data->has_boot_map ) {
        render_boot_track ( data );
        ImGui::Spacing ();
    }

    /* filesystem mapa - jen pokud máme bloky */
    if ( data->block_count > 0 ) {
        render_grid ( data, fs_map_title ( data->fs_type ) );
    }
}
