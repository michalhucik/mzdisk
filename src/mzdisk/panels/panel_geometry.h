/**
 * @file panel_geometry.h
 * @brief Vizuální mapa raw geometrie disku.
 *
 * Zobrazuje grafickou mřížku stop a sektorů, barevně odlišených podle
 * velikosti sektoru. Každý řádek je jedna fyzická stopa, buňky jsou
 * sektory. Pro dvoustranné disky se zobrazují dvě sekce (Side 0, Side 1).
 *
 * Architektura panel-split:
 *   panel_geometry.h/.c          - datový model + logika naplnění (čisté C)
 *   panel_geometry_imgui.cpp     - ImGui rendering (C++)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_GEOMETRY_H
#define MZDISK_PANEL_GEOMETRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/dsk/dsk.h"
#include "libs/mzdsk_global/mzdsk_global.h"


/** @brief Maximální počet absolutních stop v geometrii. */
#define PANEL_GEOMETRY_MAX_TRACKS  DSK_MAX_TOTAL_TRACKS  /* 204 */

/** @brief Maximální počet sektorů na jedné stopě. */
#define PANEL_GEOMETRY_MAX_SECTORS DSK_MAX_SECTORS        /* 29 */


/**
 * @brief Informace o jedné absolutní stopě pro geometrický panel.
 *
 * Obsahuje počet sektorů, velikost sektoru, příznak inverze
 * a pole ID sektorů (z hlavičky stopy v DSK obrazu).
 *
 * @par Invarianty:
 * - sectors <= PANEL_GEOMETRY_MAX_SECTORS.
 * - sector_size je jedna z hodnot: 128, 256, 512, 1024.
 * - sector_ids[0..sectors-1] obsahují platná ID sektorů.
 */
typedef struct st_PANEL_GEOMETRY_TRACK {
    uint8_t sectors;                                    /**< Počet sektorů na stopě. */
    uint16_t sector_size;                               /**< Velikost sektoru v bajtech. */
    bool is_inverted;                                   /**< FSMZ invertovaná data (16x256B). */
    uint8_t sector_ids[PANEL_GEOMETRY_MAX_SECTORS];     /**< ID sektorů z track headeru. */
} st_PANEL_GEOMETRY_TRACK;


/**
 * @brief Datový model panelu geometrie.
 *
 * Obsahuje pole per-track informací naplněné z disc->tracks_rules.
 *
 * @par Invarianty:
 * - Pokud is_loaded == true, pole tracks[] je naplněné pro [0..total_tracks).
 * - max_sectors je maximum ze všech tracks[i].sectors.
 * - sides je 1 nebo 2.
 */
typedef struct st_PANEL_GEOMETRY_DATA {
    bool is_loaded;                                             /**< Data jsou naplněna. */
    uint16_t total_tracks;                                      /**< Celkový počet absolutních stop. */
    uint8_t sides;                                              /**< Počet stran (1 nebo 2). */
    st_PANEL_GEOMETRY_TRACK tracks[PANEL_GEOMETRY_MAX_TRACKS];  /**< Per-track info. */
    uint8_t max_sectors;                                        /**< Max sektorů na libovolné stopě (pro šířku gridu). */
} st_PANEL_GEOMETRY_DATA;


/**
 * @brief Naplní datový model geometrie z otevřeného disku.
 *
 * Iteruje disc->tracks_rules->rule[], pro každé pravidlo naplní tracks[].
 * Počítá max_sectors, detekuje inverzi (16 sektorů + 256B) a čte
 * ID sektorů z track headerů v DSK obrazu.
 *
 * @param data Výstupní datový model.
 * @param disc Otevřený diskový obraz.
 *
 * @pre data != NULL, disc je platně otevřený s naplněným tracks_rules.
 * @post data->is_loaded == true při úspěchu, false pokud tracks_rules nejsou k dispozici.
 */
extern void panel_geometry_load ( st_PANEL_GEOMETRY_DATA *data, st_MZDSK_DISC *disc );


/**
 * @brief Vykreslí vizuální mapu geometrie disku (ImGui rendering).
 *
 * @param data Naplněný datový model.
 */
extern void panel_geometry_render ( const st_PANEL_GEOMETRY_DATA *data );


/* =====================================================================
 *  Editace geometrie (Change Track / Append Tracks / Shrink)
 * ===================================================================== */


/** @brief Maximální délka textového vstupu pro custom sektorovou mapu. */
#define PANEL_GEOM_EDIT_CUSTOM_MAP_LEN 128

/** @brief Maximální délka výsledkové zprávy. */
#define PANEL_GEOM_EDIT_MSG_LEN 512


/**
 * @brief Datový model editace geometrie.
 *
 * Obsahuje parametry pro tři operace (Change Track, Append Tracks,
 * Shrink) a UI stav (pending potvrzení, výsledkové zprávy).
 *
 * @par Invarianty:
 * - ct_sectors, at_sectors ∈ <1, DSK_MAX_SECTORS>.
 * - ct_ssize_idx, at_ssize_idx ∈ <0, 3> (128/256/512/1024).
 * - ct_order_idx, at_order_idx ∈ <0, 3> (Normal/LEC/LEC HD/Custom).
 */
typedef struct st_PANEL_GEOM_EDIT_DATA {
    /* --- Change Track parametry --- */
    int ct_track;                                   /**< Číslo absolutní stopy. */
    int ct_sectors;                                 /**< Nový počet sektorů. */
    int ct_ssize_idx;                               /**< Index velikosti sektoru (0-3). */
    int ct_order_idx;                               /**< Index pořadí sektorů (0-3). */
    int ct_filler;                                  /**< Filler byte (0-255). */
    char ct_custom_map[PANEL_GEOM_EDIT_CUSTOM_MAP_LEN]; /**< Custom mapa sektorů ("1,2,3,..."). */

    /* --- Append Tracks parametry --- */
    int at_count;                                   /**< Počet stop k přidání. */
    int at_sectors;                                 /**< Počet sektorů na stopě. */
    int at_ssize_idx;                               /**< Index velikosti sektoru (0-3). */
    int at_order_idx;                               /**< Index pořadí sektorů (0-3). */
    int at_filler;                                  /**< Filler byte (0-255). */
    char at_custom_map[PANEL_GEOM_EDIT_CUSTOM_MAP_LEN]; /**< Custom mapa sektorů ("1,2,3,..."). */

    /* --- Shrink parametry --- */
    int sh_new_total;                               /**< Nový celkový počet absolutních stop. */

    /* --- Edit Sector IDs parametry --- */
    int si_track;                                   /**< Číslo absolutní stopy. */
    uint8_t si_ids[PANEL_GEOMETRY_MAX_SECTORS];     /**< Editovatelná pole sector ID. */
    int si_count;                                   /**< Počet sektorů na vybrané stopě. */
    bool si_loaded;                                 /**< Sector IDs jsou načteny. */
    int si_preset_idx;                              /**< Index presetu (0=beze změny). */

    /* --- UI stav --- */
    bool pending_ct;                                /**< Čeká potvrzení Change Track. */
    bool pending_at;                                /**< Čeká potvrzení Append Tracks. */
    bool pending_sh;                                /**< Čeká potvrzení Shrink. */
    bool pending_si;                                /**< Čeká potvrzení Edit Sector IDs. */
    bool show_result;                               /**< Zobrazit výsledek operace. */
    bool is_error;                                  /**< Výsledek je chyba. */
    char result_msg[PANEL_GEOM_EDIT_MSG_LEN];       /**< Text výsledku. */
} st_PANEL_GEOM_EDIT_DATA;


/**
 * @brief Inicializuje datový model editace geometrie na výchozí hodnoty.
 *
 * @param data Ukazatel na datový model.
 * @param disc Otevřený disk (pro výchozí hodnoty z aktuální geometrie).
 *
 * @pre data != NULL, disc != NULL.
 * @post Parametry nastaveny na rozumné výchozí hodnoty.
 */
extern void panel_geom_edit_init ( st_PANEL_GEOM_EDIT_DATA *data,
                                    const st_MZDSK_DISC *disc );


/**
 * @brief Provede operaci Change Track - změní geometrii jedné stopy.
 *
 * Parsuje custom sektorovou mapu (pokud je zvolen custom režim),
 * vygeneruje sektorovou mapu a zavolá dsk_tools_change_track().
 *
 * @param data Datový model s parametry operace.
 * @param disc Otevřený disk.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @pre data != NULL, disc != NULL a otevřený.
 * @post Při chybě: data->is_error == true, data->result_msg naplněný.
 */
extern int panel_geom_edit_change_track ( st_PANEL_GEOM_EDIT_DATA *data,
                                           st_MZDSK_DISC *disc );


/**
 * @brief Provede operaci Append Tracks - přidá stopy na konec obrazu.
 *
 * Sestaví st_DSK_DESCRIPTION s jedním pravidlem, kde tracks je nový
 * celkový počet stop na stranu a absolute_track je první nová absolutní
 * stopa (stávající total_tracks). Zavolá dsk_tools_add_tracks().
 *
 * @param data Datový model s parametry operace.
 * @param disc Otevřený disk.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @pre data != NULL, disc != NULL a otevřený.
 * @post Při chybě: data->is_error == true, data->result_msg naplněný.
 */
extern int panel_geom_edit_append_tracks ( st_PANEL_GEOM_EDIT_DATA *data,
                                            st_MZDSK_DISC *disc );


/**
 * @brief Provede operaci Shrink - odstraní stopy z konce obrazu.
 *
 * Zavolá dsk_tools_shrink_image() s novým celkovým počtem stop.
 *
 * @param data Datový model s parametry operace.
 * @param disc Otevřený disk.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @pre data != NULL, disc != NULL a otevřený.
 * @post Při chybě: data->is_error == true, data->result_msg naplněný.
 */
extern int panel_geom_edit_shrink ( st_PANEL_GEOM_EDIT_DATA *data,
                                     st_MZDSK_DISC *disc );


/**
 * @brief Načte aktuální sector IDs z track headeru do editačního modelu.
 *
 * Volá dsk_tools_read_track_header_info(), zkopíruje
 * sinfo[].sector do data->si_ids[] a nastaví si_count.
 * Po úspěchu nastaví si_loaded = true.
 *
 * @param data Editační datový model.
 * @param disc Otevřený disk.
 *
 * @pre data != NULL, disc != NULL a otevřený.
 * @pre data->si_track je v rozsahu 0..total_tracks-1.
 * @post data->si_ids[] naplněny, data->si_count nastaven, data->si_loaded = true.
 */
extern void panel_geom_edit_load_sector_ids ( st_PANEL_GEOM_EDIT_DATA *data,
                                               st_MZDSK_DISC *disc );


/**
 * @brief Zapíše editovaná sector IDs zpět do track headeru.
 *
 * Volá dsk_tools_set_sector_ids() s data->si_ids[].
 * Nemodifikuje datovou oblast - jen hlavičku stopy.
 * Výsledek uloží do data->result_msg.
 *
 * @param data Editační datový model.
 * @param disc Otevřený disk.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @pre data != NULL, disc != NULL, data->si_loaded == true.
 * @post Při chybě: data->is_error == true, data->result_msg naplněný.
 */
extern int panel_geom_edit_apply_sector_ids ( st_PANEL_GEOM_EDIT_DATA *data,
                                               st_MZDSK_DISC *disc );


/**
 * @brief Vykreslí tab "Geometry Edit" (ImGui rendering).
 *
 * Implementováno v panel_geometry_imgui.cpp.
 * Obsahuje tři sekce: Change Track, Append Tracks, Shrink.
 * Každá sekce má potvrzovací dialog.
 *
 * @param edit_data Datový model editace geometrie.
 * @param geom_data Datový model geometrie (read-only, pro info o disku).
 * @param disc Otevřený disk.
 * @param is_dirty Příznak neuložených změn (operace ho nastaví na true).
 * @param needs_reload Výstupní příznak - true pokud je třeba reload session.
 */
extern void panel_geom_edit_render ( st_PANEL_GEOM_EDIT_DATA *edit_data,
                                      st_PANEL_GEOMETRY_DATA *geom_data,
                                      st_MZDSK_DISC *disc,
                                      bool *is_dirty,
                                      bool *needs_reload );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_GEOMETRY_H */
