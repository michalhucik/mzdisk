/**
 * @file panel_create.h
 * @brief Okno "Create New Disk" - datový model, presety a API.
 *
 * Samostatné ImGui okno pro vytvoření nového DSK diskového obrazu.
 * Podporuje předdefinované formáty (MZ-BASIC, CP/M SD/HD, MRS, Lemmings)
 * a custom geometrii s pravidlovým systémem. Volitelně provede
 * inicializaci souborového systému (format).
 *
 * Architektura panel-split:
 *   panel_create.h/.c          - datový model, presety, logika vytváření (čisté C)
 *   panel_create_imgui.cpp     - ImGui rendering (C++)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_PANEL_CREATE_H
#define MZDISK_PANEL_CREATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "config.h"


/** @brief Maximální počet pravidel geometrie v Create okně. */
#define PANEL_CREATE_MAX_RULES 32

/** @brief Počet předdefinovaných presetů. */
#define PANEL_CREATE_PRESET_COUNT 6

/** @brief Index presetu "Custom format" (vždy první). */
#define PANEL_CREATE_PRESET_CUSTOM 0


/**
 * @brief Pravidlo geometrie v editovatelné formě (UI stav).
 *
 * Jedno pravidlo popisuje sektory na stopách od from_track dále
 * (dokud nepřijde další pravidlo s vyšším from_track).
 *
 * @invariant sector_size_idx je v rozsahu 0..3 (128/256/512/1024 B).
 * @invariant order_idx je v rozsahu 0..3 (Custom/Normal/LEC/LEC HD).
 */
typedef struct st_PANEL_CREATE_RULE {
    int from_track;                         /**< Absolutní stopa, od které pravidlo platí. */
    int sectors;                            /**< Počet sektorů na stopě. */
    int sector_size_idx;                    /**< Index velikosti sektoru: 0=128, 1=256, 2=512, 3=1024. */
    int order_idx;                          /**< Index řazení sektorů: 0=Custom, 1=Normal, 2=LEC, 3=LEC HD. */
    uint8_t filler;                         /**< Filler byte pro nové sektory. */
    uint8_t sector_map[DSK_MAX_SECTORS];    /**< Mapa ID sektorů na stopě. */
} st_PANEL_CREATE_RULE;


/**
 * @brief Předdefinované pravidlo presetu (const data).
 *
 * Statická data popisující jednu vrstvu geometrie v presetu.
 * sector_map == NULL znamená automatické generování podle order_idx.
 */
typedef struct st_PANEL_CREATE_PREDEF_RULE {
    uint8_t from_track;                 /**< Absolutní stopa od které pravidlo platí. */
    uint8_t sectors;                    /**< Počet sektorů na stopě. */
    int ssize_idx;                      /**< Index velikosti sektoru (0..3). */
    int order_idx;                      /**< Index řazení sektorů (0..3). */
    const uint8_t *sector_map;          /**< Mapa sektorů, nebo NULL pro auto z order_idx. */
    uint8_t filler;                     /**< Filler byte. */
} st_PANEL_CREATE_PREDEF_RULE;


/**
 * @brief Předdefinovaný formát/preset.
 *
 * Obsahuje název, výchozí geometrii a pravidla.
 * Pole formattable určuje, zda preset umožňuje inicializaci FS.
 */
typedef struct st_PANEL_CREATE_PREDEF {
    const char *name;                           /**< Zobrazovaný název presetu. */
    int default_sides;                          /**< Výchozí počet stran (1 nebo 2). */
    int default_tracks;                         /**< Výchozí celkový počet absolutních stop. */
    int count_rules;                            /**< Počet pravidel geometrie. */
    const st_PANEL_CREATE_PREDEF_RULE *rules;   /**< Pole předdefinovaných pravidel. */
    bool formattable;                           /**< Preset umožňuje formátování FS. */
} st_PANEL_CREATE_PREDEF;


/**
 * @brief Hlavní stav Create okna.
 *
 * Obsahuje veškerý editovatelný stav: výběr presetu, geometrii,
 * pravidla, cestu k souboru a výstupní flagy po vytvoření disku.
 *
 * @invariant preset_idx je v rozsahu 0..PANEL_CREATE_PRESET_COUNT-1.
 * @invariant count_rules je v rozsahu 1..PANEL_CREATE_MAX_RULES.
 * @invariant sides je 1 nebo 2.
 */
typedef struct st_PANEL_CREATE_DATA {
    bool is_open;                           /**< Okno je otevřené (řídí ImGui::Begin). */

    /* nastavení geometrie */
    int preset_idx;                         /**< Index aktuálního presetu. */
    int sides;                              /**< Počet stran (1 nebo 2). */
    int tracks;                             /**< Celkový počet absolutních stop. */
    int count_rules;                        /**< Počet aktivních pravidel. */
    st_PANEL_CREATE_RULE rules[PANEL_CREATE_MAX_RULES]; /**< Editovatelná pravidla geometrie. */
    bool format_filesystem;                 /**< Inicializovat souborový systém po vytvoření. */

    /* soubor */
    char filename[256];                     /**< Jméno výstupního souboru (bez .dsk). */
    char directory[MZDISK_CONFIG_PATH_MAX]; /**< Cílový adresář. */

    /* výstup po Create */
    bool created;                           /**< Příznak: disk byl úspěšně vytvořen (ke zpracování app_imgui). */
    char created_filepath[MZDISK_CONFIG_PATH_MAX + 260]; /**< Plná cesta k vytvořenému DSK souboru. */

    /* potvrzení přepisu */
    bool confirm_overwrite;                 /**< Příznak: čekáme na potvrzení přepisu existujícího souboru. */

    /* chybová hláška */
    char error_msg[512];                    /**< Poslední chybová hláška (prázdný řetězec = žádná chyba). */
} st_PANEL_CREATE_DATA;


/**
 * @brief Vrátí ukazatel na pole předdefinovaných presetů.
 *
 * @return Statické pole o PANEL_CREATE_PRESET_COUNT prvcích.
 */
extern const st_PANEL_CREATE_PREDEF* panel_create_get_presets ( void );


/**
 * @brief Inicializuje stav Create okna na výchozí hodnoty.
 *
 * Nastaví výchozí preset (MZ-BASIC, index 1), naplní pravidla
 * z presetu a nastaví výchozí jméno souboru.
 *
 * @param data Ukazatel na datový model Create okna. Nesmí být NULL.
 * @param cfg Ukazatel na konfiguraci (pro last_create_dir). Nesmí být NULL.
 *
 * @pre data != NULL, cfg != NULL.
 * @post data je kompletně inicializovaný a připravený k zobrazení.
 */
extern void panel_create_init ( st_PANEL_CREATE_DATA *data, const st_MZDISK_CONFIG *cfg );


/**
 * @brief Naplní pravidla z vybraného presetu.
 *
 * Přepíše sides, tracks, count_rules a rules[] hodnotami z presetu.
 * Automaticky nastaví format_filesystem podle presetu (formattable).
 *
 * @param data Ukazatel na datový model. Nesmí být NULL.
 * @param preset_idx Index presetu (0..PANEL_CREATE_PRESET_COUNT-1).
 *
 * @pre data != NULL, preset_idx je platný.
 * @post Pravidla, sides, tracks a format_filesystem jsou přepsány z presetu.
 */
extern void panel_create_load_preset ( st_PANEL_CREATE_DATA *data, int preset_idx );


/**
 * @brief Provede vytvoření DSK obrazu podle aktuálního stavu.
 *
 * Podle hodnoty format_filesystem a preset_idx rozhodne, zda vytvoří
 * pouze prázdný obraz (dsk_tools_create_image) nebo zformátovaný disk
 * (mzdsk_tools_format_basic/cpm_sd/cpm_hd, fsmrs_format_fs).
 *
 * Sestaví plnou cestu (directory/filename.dsk) a výsledek uloží
 * do data->created_filepath. Při úspěchu nastaví data->created = true.
 * Při chybě naplní data->error_msg.
 *
 * @param data Ukazatel na datový model. Nesmí být NULL.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 *
 * @pre data->filename a data->directory jsou platné řetězce.
 * @post Při úspěchu data->created == true a data->created_filepath je naplněn.
 * @post Při chybě data->error_msg obsahuje popis chyby.
 */
extern int panel_create_execute ( st_PANEL_CREATE_DATA *data );


/**
 * @brief Vykreslí Create New Disk okno (ImGui rendering).
 *
 * Implementováno v panel_create_imgui.cpp.
 *
 * @param data Datový model Create okna.
 * @param cfg Konfigurace aplikace (pro IGFD cesty).
 */
extern void panel_create_render ( st_PANEL_CREATE_DATA *data, st_MZDISK_CONFIG *cfg );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_PANEL_CREATE_H */
