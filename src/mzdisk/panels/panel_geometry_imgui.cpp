/**
 * @file panel_geometry_imgui.cpp
 * @brief ImGui rendering vizuální mapy geometrie disku.
 *
 * Vykresluje barevnou mřížku stop a sektorů. Řádky = fyzické stopy,
 * sloupce = sektory. Barva buňky odpovídá velikosti sektoru.
 * FSMZ stopy (invertované) mají zvláštní barvu. Hover tooltip
 * zobrazuje číslo stopy, sektoru a velikost.
 *
 * Pro dvoustranné disky se zobrazují dvě sekce (Side 0, Side 1).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include "libs/imgui/imgui.h"

#include <cstdio>
#include <cstdlib>

extern "C" {
#include "panels/panel_geometry.h"
#include "libs/dsk/dsk.h"
#include "i18n.h"
}

#include "ui_helpers.h"


/** @brief Velikost jedné buňky mřížky v pixelech. */
static const float CELL_SIZE = 20.0f;

/** @brief Mezera mezi buňkami v pixelech. */
static const float CELL_GAP = 1.0f;


/**
 * @brief Vrátí barvu pro danou velikost sektoru.
 *
 * @param sector_size Velikost sektoru v bajtech.
 * @param is_inverted True pokud stopa má invertovaná data (FSMZ).
 * @return Barva jako ImU32 (pro DrawList).
 */
static ImU32 sector_size_color ( uint16_t sector_size, bool is_inverted )
{
    if ( is_inverted ) {
        return IM_COL32 ( 255, 152, 0, 255 );     /* oranžová - FSMZ invertovaná */
    }

    switch ( sector_size ) {
        case 128:   return IM_COL32 ( 100, 180, 220, 255 );   /* světle modrá */
        case 256:   return IM_COL32 ( 76,  175, 80,  255 );   /* zelená */
        case 512:   return IM_COL32 ( 33,  150, 243, 255 );   /* modrá */
        case 1024:  return IM_COL32 ( 156, 39,  176, 255 );   /* fialová */
        default:    return IM_COL32 ( 80,  80,  80,  255 );   /* šedá */
    }
}


/**
 * @brief Vykreslí legendu barev pro velikosti sektorů.
 *
 * Zobrazí barevné čtverečky s popisem velikosti. Zobrazí pouze
 * velikosti, které se skutečně vyskytují v datech.
 *
 * @param data Datový model geometrie.
 */
static void render_legend ( const st_PANEL_GEOMETRY_DATA *data )
{
    /* zjistit přítomné velikosti a inverzi */
    bool has_128 = false, has_256 = false, has_512 = false, has_1024 = false;
    bool has_inverted = false;

    for ( int i = 0; i < (int) data->total_tracks; i++ ) {
        if ( data->tracks[i].is_inverted ) {
            has_inverted = true;
            continue;
        }
        switch ( data->tracks[i].sector_size ) {
            case 128:  has_128 = true; break;
            case 256:  has_256 = true; break;
            case 512:  has_512 = true; break;
            case 1024: has_1024 = true; break;
            default: break;
        }
    }

    struct { uint16_t size; bool present; bool inverted; const char *label; } items[] = {
        { 256,  has_inverted, true,  "FSMZ 256B (inv)" },
        { 128,  has_128,      false, "128B" },
        { 256,  has_256,      false, "256B" },
        { 512,  has_512,      false, "512B" },
        { 1024, has_1024,     false, "1024B" },
    };

    for ( int i = 0; i < (int) ( sizeof ( items ) / sizeof ( items[0] ) ); i++ ) {
        if ( !items[i].present ) continue;

        ImVec2 pos = ImGui::GetCursorScreenPos ();
        ImDrawList *dl = ImGui::GetWindowDrawList ();
        dl->AddRectFilled ( pos, ImVec2 ( pos.x + 24, pos.y + 24 ),
                            sector_size_color ( items[i].size, items[i].inverted ) );
        ImGui::Dummy ( ImVec2 ( 26, 26 ) );
        ImGui::SameLine ();
        ImGui::TextUnformatted ( items[i].label );
        ImGui::SameLine ( 0, 20 );
    }
    ImGui::NewLine ();
}


/**
 * @brief Vykreslí mřížku stop a sektorů pro jeden rozsah absolutních stop.
 *
 * Řádky = fyzické stopy, sloupce = sektory. Buňky jsou barevně odlišené
 * podle velikosti sektoru a inverze. Hover tooltip zobrazuje detail.
 *
 * @param data          Datový model geometrie.
 * @param abs_start     Počáteční absolutní stopa (včetně).
 * @param abs_end       Koncová absolutní stopa (bez).
 * @param side          Číslo strany (0 nebo 1) pro zobrazení ve tooltipu.
 * @param child_id      ImGui ID pro BeginChild oblast.
 */
static void render_grid ( const st_PANEL_GEOMETRY_DATA *data,
                          int abs_start, int abs_end, int side,
                          const char *child_id )
{
    int physical_tracks = abs_end - abs_start;
    if ( physical_tracks <= 0 ) return;

    float step = CELL_SIZE + CELL_GAP;

    /* popisek stopy zabírá prostor nalevo */
    float label_width = ImGui::CalcTextSize ( "T999" ).x + 8.0f;

    /* scrollovací oblast */
    ImVec2 avail = ImGui::GetContentRegionAvail ();
    float grid_height = (float) physical_tracks * step + 8.0f;
    float child_height = ( grid_height < avail.y ) ? grid_height : avail.y;

    if ( !ImGui::BeginChild ( child_id, ImVec2 ( 0, child_height ), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar ) ) {
        ImGui::EndChild ();
        return;
    }

    ImVec2 origin = ImGui::GetCursorScreenPos ();
    ImDrawList *dl = ImGui::GetWindowDrawList ();
    ImVec2 mouse = ImGui::GetMousePos ();

    int hovered_abs = -1;
    int hovered_col = -1;

    for ( int row = 0; row < physical_tracks; row++ ) {
        int abs_track = abs_start + row;
        int phys_track = abs_track / data->sides;
        const st_PANEL_GEOMETRY_TRACK *t = &data->tracks[abs_track];

        /* popisek stopy */
        ImVec2 label_pos = ImVec2 ( origin.x, origin.y + row * step + 2.0f );
        char label[16];
        snprintf ( label, sizeof ( label ), "%d", phys_track );
        dl->AddText ( label_pos, IM_COL32 ( 140, 140, 160, 255 ), label );

        /* buňky sektorů */
        for ( int col = 0; col < (int) t->sectors; col++ ) {
            ImVec2 p0 = ImVec2 ( origin.x + label_width + col * step,
                                 origin.y + row * step );
            ImVec2 p1 = ImVec2 ( p0.x + CELL_SIZE, p0.y + CELL_SIZE );

            dl->AddRectFilled ( p0, p1, sector_size_color ( t->sector_size, t->is_inverted ) );

            /* detekce hover */
            if ( mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y ) {
                hovered_abs = abs_track;
                hovered_col = col;
                dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
            }
        }
    }

    /* dummy pro správný scroll */
    float total_width = label_width + (float) data->max_sectors * step;
    ImGui::Dummy ( ImVec2 ( total_width, (float) physical_tracks * step ) );

    /* tooltip */
    if ( hovered_abs >= 0 ) {
        const st_PANEL_GEOMETRY_TRACK *ht = &data->tracks[hovered_abs];
        int phys_track = hovered_abs / data->sides;
        int sector_id = ht->sector_ids[hovered_col];

        ImGui::BeginTooltip ();
        if ( data->sides > 1 ) {
            ImGui::Text ( "%s %d, %s %d, %s %d (ID %d)",
                          _ ( "Track" ), phys_track,
                          _ ( "Side" ), side,
                          _ ( "Sector" ), hovered_col + 1, sector_id );
        } else {
            ImGui::Text ( "%s %d, %s %d (ID %d)",
                          _ ( "Track" ), phys_track,
                          _ ( "Sector" ), hovered_col + 1, sector_id );
        }
        ImGui::Text ( "%s: %d B", _ ( "Size" ), (int) ht->sector_size );
        if ( ht->is_inverted ) {
            ImGui::Text ( "%s", _ ( "Inverted data (FSMZ)" ) );
        }
        ImGui::EndTooltip ();
    }

    ImGui::EndChild ();
}


extern "C" void panel_geometry_render ( const st_PANEL_GEOMETRY_DATA *data )
{
    if ( !data || !data->is_loaded ) {
        ImGui::TextDisabled ( "%s", _ ( "No geometry data available." ) );
        return;
    }

    /* souhrnné info */
    ImGui::Text ( "%s: %d", _ ( "Tracks" ), (int) data->total_tracks / data->sides );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d", _ ( "Sides" ), (int) data->sides );
    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s: %d", _ ( "Max sectors/track" ), (int) data->max_sectors );

    ImGui::Spacing ();
    render_legend ( data );
    ImGui::Spacing ();

    if ( data->sides == 2 ) {
        /* dvoustranný disk: rozdělit absolutní stopy na Side 0 a Side 1 */
        ImGui::SeparatorText ( _ ( "Side 0" ) );
        /* Side 0: absolutní stopy 0, 2, 4, ... -> ale tracks_rules jsou sekvenční */
        /* V Extended DSK: abs_track = phys_track * sides + side */
        /* Takže Side 0 = stopy 0, 2, 4, ... a Side 1 = stopy 1, 3, 5, ... */

        /* Sestavíme seznamy stop pro každou stranu */
        int tracks_per_side = data->total_tracks / data->sides;

        /* Side 0: vykreslíme stopy s abs_track % 2 == 0 */
        {
            float step = CELL_SIZE + CELL_GAP;
            float label_width = ImGui::CalcTextSize ( "T999" ).x + 8.0f;

            ImVec2 avail = ImGui::GetContentRegionAvail ();
            /* odhadnout výšku pro Side 0 - polovina dostupného prostoru minus separátory */
            float side_height = ( (float) tracks_per_side * step + 8.0f );
            float max_height = ( avail.y - 80.0f ) / 2.0f;
            if ( side_height > max_height && max_height > 0 ) side_height = max_height;

            if ( ImGui::BeginChild ( "##geom_side0", ImVec2 ( 0, side_height ), ImGuiChildFlags_None,
                                     ImGuiWindowFlags_HorizontalScrollbar ) ) {
                ImVec2 origin = ImGui::GetCursorScreenPos ();
                ImDrawList *dl = ImGui::GetWindowDrawList ();
                ImVec2 mouse = ImGui::GetMousePos ();

                int hovered_abs = -1;
                int hovered_col = -1;

                for ( int row = 0; row < tracks_per_side; row++ ) {
                    int abs_track = row * 2; /* Side 0 */
                    if ( abs_track >= (int) data->total_tracks ) break;
                    const st_PANEL_GEOMETRY_TRACK *t = &data->tracks[abs_track];

                    /* popisek stopy */
                    char label[16];
                    snprintf ( label, sizeof ( label ), "%d", row );
                    dl->AddText ( ImVec2 ( origin.x, origin.y + row * step + 2.0f ),
                                  IM_COL32 ( 140, 140, 160, 255 ), label );

                    for ( int col = 0; col < (int) t->sectors; col++ ) {
                        ImVec2 p0 = ImVec2 ( origin.x + label_width + col * step,
                                             origin.y + row * step );
                        ImVec2 p1 = ImVec2 ( p0.x + CELL_SIZE, p0.y + CELL_SIZE );
                        dl->AddRectFilled ( p0, p1, sector_size_color ( t->sector_size, t->is_inverted ) );
                        if ( mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y ) {
                            hovered_abs = abs_track;
                            hovered_col = col;
                            dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
                        }
                    }
                }

                float total_width = label_width + (float) data->max_sectors * step;
                ImGui::Dummy ( ImVec2 ( total_width, (float) tracks_per_side * step ) );

                if ( hovered_abs >= 0 ) {
                    const st_PANEL_GEOMETRY_TRACK *ht = &data->tracks[hovered_abs];
                    ImGui::BeginTooltip ();
                    ImGui::Text ( "%s %d, %s 0, %s %d (ID %d)",
                                  _ ( "Track" ), hovered_abs / 2,
                                  _ ( "Side" ),
                                  _ ( "Sector" ), hovered_col + 1,
                                  (int) ht->sector_ids[hovered_col] );
                    ImGui::Text ( "%s: %d B", _ ( "Size" ), (int) ht->sector_size );
                    if ( ht->is_inverted ) ImGui::Text ( "%s", _ ( "Inverted data (FSMZ)" ) );
                    ImGui::EndTooltip ();
                }
            }
            ImGui::EndChild ();
        }

        ImGui::SeparatorText ( _ ( "Side 1" ) );

        /* Side 1: stopy 1, 3, 5, ... */
        {
            float step = CELL_SIZE + CELL_GAP;
            float label_width = ImGui::CalcTextSize ( "T999" ).x + 8.0f;

            ImVec2 avail = ImGui::GetContentRegionAvail ();
            float side_height = avail.y;

            if ( ImGui::BeginChild ( "##geom_side1", ImVec2 ( 0, side_height ), ImGuiChildFlags_None,
                                     ImGuiWindowFlags_HorizontalScrollbar ) ) {
                ImVec2 origin = ImGui::GetCursorScreenPos ();
                ImDrawList *dl = ImGui::GetWindowDrawList ();
                ImVec2 mouse = ImGui::GetMousePos ();

                int hovered_abs = -1;
                int hovered_col = -1;

                for ( int row = 0; row < tracks_per_side; row++ ) {
                    int abs_track = row * 2 + 1; /* Side 1 */
                    if ( abs_track >= (int) data->total_tracks ) break;
                    const st_PANEL_GEOMETRY_TRACK *t = &data->tracks[abs_track];

                    char label[16];
                    snprintf ( label, sizeof ( label ), "%d", row );
                    dl->AddText ( ImVec2 ( origin.x, origin.y + row * step + 2.0f ),
                                  IM_COL32 ( 140, 140, 160, 255 ), label );

                    for ( int col = 0; col < (int) t->sectors; col++ ) {
                        ImVec2 p0 = ImVec2 ( origin.x + label_width + col * step,
                                             origin.y + row * step );
                        ImVec2 p1 = ImVec2 ( p0.x + CELL_SIZE, p0.y + CELL_SIZE );
                        dl->AddRectFilled ( p0, p1, sector_size_color ( t->sector_size, t->is_inverted ) );
                        if ( mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y ) {
                            hovered_abs = abs_track;
                            hovered_col = col;
                            dl->AddRect ( p0, p1, IM_COL32 ( 255, 255, 255, 200 ) );
                        }
                    }
                }

                float total_width = label_width + (float) data->max_sectors * step;
                ImGui::Dummy ( ImVec2 ( total_width, (float) tracks_per_side * step ) );

                if ( hovered_abs >= 0 ) {
                    const st_PANEL_GEOMETRY_TRACK *ht = &data->tracks[hovered_abs];
                    ImGui::BeginTooltip ();
                    ImGui::Text ( "%s %d, %s 1, %s %d (ID %d)",
                                  _ ( "Track" ), hovered_abs / 2,
                                  _ ( "Side" ),
                                  _ ( "Sector" ), hovered_col + 1,
                                  (int) ht->sector_ids[hovered_col] );
                    ImGui::Text ( "%s: %d B", _ ( "Size" ), (int) ht->sector_size );
                    if ( ht->is_inverted ) ImGui::Text ( "%s", _ ( "Inverted data (FSMZ)" ) );
                    ImGui::EndTooltip ();
                }
            }
            ImGui::EndChild ();
        }

    } else {
        /* jednostranný disk: jedna sekce */
        render_grid ( data, 0, (int) data->total_tracks, 0, "##geom_grid" );
    }
}


/* =====================================================================
 *  Rendering editace geometrie
 * ===================================================================== */


/** @brief Popisky pro combo box velikosti sektoru. */
static const char *s_ssize_labels[] = {
    "128 B",
    "256 B",
    "512 B",
    "1024 B",
};

/** @brief Popisky pro combo box pořadí sektorů. */
static const char *s_gedit_order_labels[] = {
    "Normal",
    "Interleaved (LEC)",
    "Interleaved (LEC HD)",
    "Custom",
};


/**
 * @brief Vykreslí sdílené parametry stopy (sektory, velikost, pořadí, filler).
 *
 * Společná část pro Change Track a Append Tracks.
 *
 * @param prefix Prefix pro ImGui ID (např. "ct" nebo "at").
 * @param sectors Ukazatel na počet sektorů.
 * @param ssize_idx Ukazatel na index velikosti.
 * @param order_idx Ukazatel na index pořadí.
 * @param filler Ukazatel na filler byte.
 * @param custom_map Buffer pro custom mapu.
 * @param custom_map_size Velikost bufferu.
 */
static void render_track_params ( const char *prefix,
                                    int *sectors, int *ssize_idx,
                                    int *order_idx, int *filler,
                                    char *custom_map, int custom_map_size )
{
    float input_w = 140.0f;
    float label_w = 220.0f;
    char id[64];

    /* sektory a velikost na jednom řádku */
    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Sectors" ) );
    ImGui::SameLine ( label_w );
    ImGui::SetNextItemWidth ( input_w );
    std::snprintf ( id, sizeof ( id ), "##%s_sectors", prefix );
    if ( ImGui::InputInt ( id, sectors, 1, 5 ) ) {
        if ( *sectors < 1 ) *sectors = 1;
        if ( *sectors > DSK_MAX_SECTORS ) *sectors = DSK_MAX_SECTORS;
    }

    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s:", _ ( "Sector size" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( input_w );
    std::snprintf ( id, sizeof ( id ), "##%s_ssize", prefix );
    ImGui::Combo ( id, ssize_idx, s_ssize_labels, IM_ARRAYSIZE ( s_ssize_labels ) );

    /* pořadí a filler */
    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Sector order" ) );
    ImGui::SameLine ( label_w );
    ImGui::SetNextItemWidth ( 200.0f );
    std::snprintf ( id, sizeof ( id ), "##%s_order", prefix );
    ImGui::Combo ( id, order_idx, s_gedit_order_labels, IM_ARRAYSIZE ( s_gedit_order_labels ) );

    ImGui::SameLine ( 0, 20 );
    ImGui::Text ( "%s:", _ ( "Filler" ) );
    ImGui::SameLine ();
    ImGui::SetNextItemWidth ( input_w );
    std::snprintf ( id, sizeof ( id ), "##%s_filler", prefix );
    if ( ImGui::InputInt ( id, filler, 1, 16 ) ) {
        if ( *filler < 0 ) *filler = 0;
        if ( *filler > 255 ) *filler = 255;
    }
    ImGui::SameLine ();
    char hex_label[16];
    std::snprintf ( hex_label, sizeof ( hex_label ), "(0x%02X)", (unsigned) *filler );
    ImGui::TextDisabled ( "%s", hex_label );

    /* custom mapa (zobrazit jen pokud je custom) */
    if ( *order_idx == 3 ) {
        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "%s:", _ ( "Sector map" ) );
        ImGui::SameLine ( label_w );
        ImGui::SetNextItemWidth ( -FLT_MIN );
        std::snprintf ( id, sizeof ( id ), "##%s_map", prefix );
        ImGui::InputText ( id, custom_map, (size_t) custom_map_size );
        ImGui::SameLine ();
        ImGui::TextDisabled ( "(?)" );
        if ( ImGui::BeginItemTooltip () ) {
            ImGui::PushTextWrapPos ( ImGui::GetFontSize () * 25.0f );
            ImGui::TextUnformatted ( _ ( "Comma-separated sector IDs, e.g. 1,3,5,2,4,6" ) );
            ImGui::PopTextWrapPos ();
            ImGui::EndTooltip ();
        }
    }
}


extern "C" void panel_geom_edit_render ( st_PANEL_GEOM_EDIT_DATA *edit_data,
                                           st_PANEL_GEOMETRY_DATA *geom_data,
                                           st_MZDSK_DISC *disc,
                                           bool *is_dirty,
                                           bool *needs_reload )
{
    (void) geom_data; /* použije se v budoucnu pro inline preview */

    if ( !edit_data || !disc || !disc->tracks_rules ) {
        ImGui::TextDisabled ( "%s", _ ( "No disk open." ) );
        return;
    }

    *needs_reload = false;
    ImVec2 center = ImGui::GetMainViewport ()->GetCenter ();

    int total_tracks = (int) disc->tracks_rules->total_tracks;
    int sides = (int) disc->tracks_rules->sides;

    /* souhrnné info o disku */
    ImGui::Text ( "%s: %d  |  %s: %d  |  %s: %d",
                  _ ( "Total tracks" ), total_tracks,
                  _ ( "Tracks/side" ), total_tracks / sides,
                  _ ( "Sides" ), sides );
    ImGui::Spacing ();

    ImGui::TextWrapped ( "%s", _ ( "Geometry modification operations. These change the disk structure - use with caution." ) );
    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();

    /* scrollovací oblast pro obsah */
    ImGui::BeginChild ( "##geom_edit_scroll", ImVec2 ( 0, 0 ), ImGuiChildFlags_None );


    /* === Change Track === */
    ImGui::Text ( "%s", _ ( "Change Track" ) );
    ImGui::TextWrapped ( "%s", _ ( "Changes the geometry of a single track (sector count, sector size, sector order). Existing data on the track is lost." ) );
    ImGui::Spacing ();

    float input_w = 140.0f;
    float label_w = 220.0f;

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Track" ) );
    ImGui::SameLine ( label_w );
    ImGui::SetNextItemWidth ( input_w );
    if ( ImGui::InputInt ( "##ct_track", &edit_data->ct_track, 1, 10 ) ) {
        if ( edit_data->ct_track < 0 ) edit_data->ct_track = 0;
        if ( edit_data->ct_track >= total_tracks )
            edit_data->ct_track = total_tracks - 1;
    }

    render_track_params ( "ct",
                          &edit_data->ct_sectors, &edit_data->ct_ssize_idx,
                          &edit_data->ct_order_idx, &edit_data->ct_filler,
                          edit_data->ct_custom_map,
                          PANEL_GEOM_EDIT_CUSTOM_MAP_LEN );
    ImGui::Spacing ();
    if ( ImGui::Button ( _ ( "Change Track" ) ) ) {
        edit_data->pending_ct = true;
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();


    /* === Append Tracks === */
    ImGui::Text ( "%s", _ ( "Append Tracks" ) );
    ImGui::TextWrapped ( "%s", _ ( "Adds new tracks at the end of the disk image. For 2-sided disks, the count must be even." ) );
    ImGui::Spacing ();

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Count" ) );
    ImGui::SameLine ( label_w );
    ImGui::SetNextItemWidth ( input_w );
    if ( ImGui::InputInt ( "##at_count", &edit_data->at_count, 1, 10 ) ) {
        if ( edit_data->at_count < 1 ) edit_data->at_count = 1;
        int max_add = DSK_MAX_TOTAL_TRACKS - total_tracks;
        if ( edit_data->at_count > max_add ) edit_data->at_count = max_add;
    }

    render_track_params ( "at",
                          &edit_data->at_sectors, &edit_data->at_ssize_idx,
                          &edit_data->at_order_idx, &edit_data->at_filler,
                          edit_data->at_custom_map,
                          PANEL_GEOM_EDIT_CUSTOM_MAP_LEN );
    ImGui::Spacing ();

    int remaining = DSK_MAX_TOTAL_TRACKS - total_tracks;
    if ( remaining <= 0 ) {
        ImGui::BeginDisabled ();
        ImGui::Button ( _ ( "Append Tracks" ) );
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        ImGui::TextDisabled ( "%s", _ ( "Maximum tracks reached" ) );
    } else {
        if ( ImGui::Button ( _ ( "Append Tracks" ) ) ) {
            edit_data->pending_at = true;
        }
        ImGui::SameLine ();
        ImGui::TextDisabled ( "(%s: %d)", _ ( "max" ), remaining );
    }

    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();


    /* === Shrink === */
    ImGui::Text ( "%s", _ ( "Shrink" ) );
    ImGui::TextWrapped ( "%s", _ ( "Removes tracks from the end of the disk image. Data on removed tracks is permanently lost." ) );
    ImGui::Spacing ();

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "New total tracks" ) );
    ImGui::SameLine ( label_w );
    ImGui::SetNextItemWidth ( input_w );
    if ( ImGui::InputInt ( "##sh_total", &edit_data->sh_new_total, 1, 10 ) ) {
        if ( edit_data->sh_new_total < 1 ) edit_data->sh_new_total = 1;
        if ( edit_data->sh_new_total >= total_tracks )
            edit_data->sh_new_total = total_tracks - 1;
    }
    ImGui::SameLine ();
    {
        int remove_count = total_tracks - edit_data->sh_new_total;
        char info[64];
        std::snprintf ( info, sizeof ( info ), "(%s %d %s)",
                         _ ( "removes" ), remove_count, _ ( "tracks" ) );
        ImGui::TextDisabled ( "%s", info );
    }
    ImGui::Spacing ();

    if ( total_tracks <= 1 ) {
        ImGui::BeginDisabled ();
        ImGui::Button ( _ ( "Shrink" ) );
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        ImGui::TextDisabled ( "%s", _ ( "Disk has only 1 track" ) );
    } else {
        if ( ImGui::Button ( _ ( "Shrink" ) ) ) {
            edit_data->pending_sh = true;
        }
    }


    ImGui::Spacing ();
    ImGui::Separator ();
    ImGui::Spacing ();


    /* === Edit Sector IDs === */
    ImGui::Text ( "%s", _ ( "Edit Sector IDs" ) );
    ImGui::TextWrapped ( "%s", _ ( "Modifies sector IDs in the track header without changing data." ) );
    ImGui::Spacing ();

    ImGui::AlignTextToFramePadding ();
    ImGui::Text ( "%s:", _ ( "Track" ) );
    ImGui::SameLine ( label_w );
    ImGui::SetNextItemWidth ( input_w );
    {
        int prev_track = edit_data->si_track;
        if ( ImGui::InputInt ( "##si_track", &edit_data->si_track, 1, 10 ) ) {
            if ( edit_data->si_track < 0 ) edit_data->si_track = 0;
            if ( edit_data->si_track >= total_tracks )
                edit_data->si_track = total_tracks - 1;
        }
        /* Automaticky načíst sector IDs při změně stopy */
        if ( edit_data->si_track != prev_track || !edit_data->si_loaded ) {
            panel_geom_edit_load_sector_ids ( edit_data, disc );
        }
    }

    if ( edit_data->si_loaded && edit_data->si_count > 0 ) {
        ImGui::Spacing ();
        {
            char label[128];
            std::snprintf ( label, sizeof ( label ), "%s %d (%d %s):",
                             _ ( "Sector IDs for track" ), edit_data->si_track,
                             edit_data->si_count, _ ( "sectors" ) );
            ImGui::Text ( "%s", label );
        }
        ImGui::Spacing ();

        /* Mřížka InputInt polí pro sector IDs (4 sloupce) */
        int cols = 4;
        for ( int i = 0; i < edit_data->si_count; i++ ) {
            if ( i % cols != 0 ) ImGui::SameLine ( 0, 20 );

            char label[32];
            std::snprintf ( label, sizeof ( label ), "#%d:", i );
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled ( "%s", label );
            ImGui::SameLine ();

            int val = (int) edit_data->si_ids[i];
            ImGui::SetNextItemWidth ( 70.0f );
            char id[32];
            std::snprintf ( id, sizeof ( id ), "##si_id_%d", i );
            if ( ImGui::InputInt ( id, &val, 0, 0 ) ) {
                if ( val < 0 ) val = 0;
                if ( val > 255 ) val = 255;
                edit_data->si_ids[i] = (uint8_t) val;
            }
        }

        ImGui::Spacing ();

        /* Preset combo + Apply Preset */
        static const char *s_si_preset_labels[] = {
            "---",
            "Normal",
            "Interleaved (LEC)",
            "Interleaved (LEC HD)",
        };
        ImGui::AlignTextToFramePadding ();
        ImGui::Text ( "%s:", _ ( "Preset" ) );
        ImGui::SameLine ( label_w );
        ImGui::SetNextItemWidth ( 200.0f );
        ImGui::Combo ( "##si_preset", &edit_data->si_preset_idx,
                        s_si_preset_labels, IM_ARRAYSIZE ( s_si_preset_labels ) );
        ImGui::SameLine ( 0, 10 );
        if ( edit_data->si_preset_idx > 0 ) {
            if ( ImGui::Button ( _ ( "Apply Preset" ) ) ) {
                static const en_DSK_SECTOR_ORDER_TYPE si_order_map[] = {
                    DSK_SEC_ORDER_NORMAL,        /* nepoužito (idx 0 = ---) */
                    DSK_SEC_ORDER_NORMAL,
                    DSK_SEC_ORDER_INTERLACED_LEC,
                    DSK_SEC_ORDER_INTERLACED_LEC_HD,
                };
                uint8_t tmp_map[DSK_MAX_SECTORS];
                dsk_tools_make_sector_map ( (uint8_t) edit_data->si_count,
                                             si_order_map[edit_data->si_preset_idx],
                                             tmp_map );
                for ( int i = 0; i < edit_data->si_count; i++ ) {
                    edit_data->si_ids[i] = tmp_map[i];
                }
            }
        } else {
            ImGui::BeginDisabled ();
            ImGui::Button ( _ ( "Apply Preset" ) );
            ImGui::EndDisabled ();
        }

        ImGui::Spacing ();
        if ( ImGui::Button ( _ ( "Apply Sector IDs" ) ) ) {
            edit_data->pending_si = true;
        }
    } else if ( !edit_data->si_loaded ) {
        ImGui::Spacing ();
        ImGui::TextDisabled ( "%s", _ ( "Cannot read sector IDs for this track." ) );
    }


    /* === Výsledek === */
    if ( edit_data->show_result ) {
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( edit_data->is_error ) {
            ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 1.0f, 0.3f, 0.3f, 1.0f ) );
        } else {
            ImGui::PushStyleColor ( ImGuiCol_Text, ImVec4 ( 0.3f, 1.0f, 0.3f, 1.0f ) );
        }
        ImGui::TextWrapped ( "%s", edit_data->result_msg );
        ImGui::PopStyleColor ();
    }


    ImGui::EndChild (); /* ##geom_edit_scroll */


    /* === Potvrzovací dialogy (mimo scrollovací oblast) === */

    /* Change Track potvrzení */
    if ( edit_data->pending_ct ) {
        ImGui::OpenPopup ( "##gedit_ct_confirm" );
        edit_data->pending_ct = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##gedit_ct_confirm", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Change Track" ) );
        ImGui::Spacing ();

        int ssize_bytes = 128 << edit_data->ct_ssize_idx;
        ImGui::Text ( "%s: %d", _ ( "Track" ), edit_data->ct_track );
        ImGui::Text ( "%s: %d x %d B", _ ( "New geometry" ),
                      edit_data->ct_sectors, ssize_bytes );
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ), "%s",
                             _ ( "WARNING: Existing data on this track will be lost!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Change" ) ) ) {
            int res = panel_geom_edit_change_track ( edit_data, disc );
            if ( res == EXIT_SUCCESS ) {
                *is_dirty = true;
                *needs_reload = true;
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Append Tracks potvrzení */
    if ( edit_data->pending_at ) {
        ImGui::OpenPopup ( "##gedit_at_confirm" );
        edit_data->pending_at = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##gedit_at_confirm", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Append Tracks" ) );
        ImGui::Spacing ();

        int ssize_bytes = 128 << edit_data->at_ssize_idx;
        ImGui::Text ( "%s: %d", _ ( "Tracks to add" ), edit_data->at_count );
        ImGui::Text ( "%s: %d x %d B", _ ( "Geometry" ),
                      edit_data->at_sectors, ssize_bytes );
        ImGui::Text ( "%s: %d -> %d", _ ( "Total tracks" ),
                      total_tracks, total_tracks + edit_data->at_count );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Append" ) ) ) {
            int res = panel_geom_edit_append_tracks ( edit_data, disc );
            if ( res == EXIT_SUCCESS ) {
                *is_dirty = true;
                *needs_reload = true;
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Shrink potvrzení */
    if ( edit_data->pending_sh ) {
        ImGui::OpenPopup ( "##gedit_sh_confirm" );
        edit_data->pending_sh = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##gedit_sh_confirm", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Shrink" ) );
        ImGui::Spacing ();

        int remove_count = total_tracks - edit_data->sh_new_total;
        ImGui::Text ( "%s: %d -> %d", _ ( "Total tracks" ),
                      total_tracks, edit_data->sh_new_total );
        ImGui::Text ( "%s: %d", _ ( "Tracks to remove" ), remove_count );
        ImGui::Spacing ();
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                             _ ( "WARNING: Data on removed tracks will be permanently lost!" ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Shrink" ) ) ) {
            int res = panel_geom_edit_shrink ( edit_data, disc );
            if ( res == EXIT_SUCCESS ) {
                *is_dirty = true;
                *needs_reload = true;
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Edit Sector IDs potvrzení */
    if ( edit_data->pending_si ) {
        ImGui::OpenPopup ( "##gedit_si_confirm" );
        edit_data->pending_si = false;
    }
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );
    if ( ImGui::BeginPopupModal ( "##gedit_si_confirm", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::Text ( "%s", _ ( "Edit Sector IDs" ) );
        ImGui::Spacing ();

        ImGui::Text ( "%s: %d", _ ( "Track" ), edit_data->si_track );
        ImGui::Text ( "%s: %d", _ ( "Sectors" ), edit_data->si_count );
        ImGui::Spacing ();

        /* Zobrazit nová ID */
        {
            char ids_str[256];
            int pos = 0;
            for ( int i = 0; i < edit_data->si_count && pos < (int) sizeof ( ids_str ) - 8; i++ ) {
                if ( i > 0 ) pos += std::snprintf ( ids_str + pos, sizeof ( ids_str ) - (size_t) pos, ", " );
                pos += std::snprintf ( ids_str + pos, sizeof ( ids_str ) - (size_t) pos, "%d", edit_data->si_ids[i] );
            }
            ImGui::Text ( "%s: %s", _ ( "New" ), ids_str );
        }

        ImGui::Spacing ();
        ImGui::TextDisabled ( "%s",
            _ ( "Only sector IDs in the track header will be modified. Sector data remains unchanged." ) );
        ImGui::Spacing ();
        ImGui::Separator ();
        ImGui::Spacing ();

        if ( ButtonMinWidth ( _ ( "Apply" ) ) ) {
            int res = panel_geom_edit_apply_sector_ids ( edit_data, disc );
            if ( res == EXIT_SUCCESS ) {
                *is_dirty = true;
                *needs_reload = true;
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if ( ButtonMinWidth ( _ ( "Cancel" ) ) ) {
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
}
