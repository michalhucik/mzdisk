/**
 * @file disk_session.h
 * @brief Správa otevřených diskových obrazů v mzdisk GUI.
 *
 * Zapouzdřuje st_MZDSK_DISC + výsledek auto-detekce + GUI metadata
 * (dirty flag, cesta k souboru, režim práce). Umožňuje mít otevřených
 * více disků současně a přepínat mezi nimi.
 *
 * Životní cyklus session:
 *   mzdisk_session_open() -> práce s diskem -> mzdisk_session_save()
 *   -> mzdisk_session_close_by_id()
 *
 * Všechny disky se implicitně otevírají do paměti (memory driver).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_DISK_SESSION_H
#define MZDISK_DISK_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "panels/panel_info.h"
#include "panels/panel_map.h"
#include "panels/panel_hexdump.h"
#include "panels/panel_fsmz.h"
#include "panels/panel_cpm.h"
#include "panels/panel_mrs.h"
#include "panels/panel_geometry.h"
#include "panels/panel_boot.h"
#include "panels/panel_raw_io.h"


/** @brief Maximální počet současně otevřených disků. */
#define MZDISK_MAX_SESSIONS 16

/** @brief Maximální délka cesty k souboru. */
#define MZDISK_MAX_PATH 1024


/**
 * @brief Stav poslední operace nad session (audit L-19).
 *
 * Typ pro `st_MZDISK_SESSION::last_op_status`. Nahrazuje dřívější
 * `int` s volnými hodnotami 0/1/2 za silně typovaný enum - zamezí
 * záměně a čtení kódu je samo-popisné.
 */
typedef enum en_LAST_OP_STATUS {
    LAST_OP_NONE   = 0,   /**< Žádná operace dosud neproběhla. */
    LAST_OP_OK     = 1,   /**< Operace proběhla úspěšně. */
    LAST_OP_FAILED = 2,   /**< Operace selhala. */
} en_LAST_OP_STATUS;


/**
 * @brief Jedna otevřená disková session (jedno "okno" mzdisk GUI).
 *
 * Zapouzdřuje disk (st_MZDSK_DISC), výsledek auto-detekce filesystému
 * a GUI metadata potřebná pro zobrazení a správu.
 *
 * Session může být ve dvou stavech:
 *   - EMPTY - is_open == true, has_disk == false: okno existuje, disk
 *     není načtený, v content area je welcome screen.
 *   - LOADED - is_open == true, has_disk == true: okno existuje, disk
 *     je načtený, panely obsahují data.
 * is_open == false znamená, že slot je volný.
 *
 * @par Invarianty:
 * - Pokud has_disk == true, pak disc je platně otevřený a detect_result je naplněný.
 * - Pokud is_open == false, obsah ostatních členů je nedefinovaný.
 * - has_disk == true implikuje is_open == true.
 * - is_dirty == true znamená, že v paměti jsou neuložené změny (jen pokud has_disk).
 * - id != 0 pokud is_open == true (monotónní id přidělené při vzniku session).
 */
typedef struct st_MZDISK_SESSION {
    uint64_t id;                                /**< Stabilní identifikátor session (0 = neplatné). */
    int window_number;                          /**< Uživatelsky viditelné číslo okna: 0 = primární, 1..MZDISK_MAX_SESSIONS-1 = detached (první volné). */
    bool is_open;                               /**< Slot obsazený - okno session existuje. */
    bool has_disk;                              /**< DSK je načtený (LOADED stav). */
    bool is_dirty;                              /**< V paměti jsou neuložené změny (platí jen pro LOADED). */
    bool is_primary;                            /**< Session se renderuje v hlavním okně (ne detached). */
    bool window_open;                           /**< Detached ImGui okno: uživatel neklikl křížek. */
    bool pending_close;                         /**< Session čeká na potvrzení zavření (dirty). */
    bool pending_reload;                        /**< Session čeká na potvrzení reloadu (dirty). */
    char pending_open_path[MZDISK_MAX_PATH];    /**< Cesta k souboru čekajícímu na otevření (po unsaved dialogu). */
    char last_save_error[512];                  /**< Chyba posledního pokusu o Save v unsaved dialogu (prázdné = žádná). */
    st_MZDSK_DISC disc;                         /**< Diskový obraz (z mzdsk_global). */
    st_MZDSK_DETECT_RESULT detect_result;       /**< Výsledek auto-detekce FS. */
    char filepath[MZDISK_MAX_PATH];             /**< Absolutní cesta k DSK souboru. */
    char display_name[256];                     /**< Krátký název pro zobrazení v tabu. */
    st_PANEL_INFO_DATA info_data;               /**< Data pro informační panel. */
    st_PANEL_MAP_DATA map_data;                 /**< Data pro blokovou mapu. */
    st_PANEL_HEXDUMP_DATA hexdump_data;         /**< Data pro hexdump viewer. */
    st_PANEL_FSMZ_DATA fsmz_data;               /**< Data pro FSMZ directory. */
    st_PANEL_CPM_DATA cpm_data;                 /**< Data pro CP/M directory. */
    st_PANEL_MRS_DATA mrs_data;                 /**< Data pro MRS directory. */
    st_PANEL_GEOMETRY_DATA geometry_data;        /**< Data pro geometrickou mapu. */
    st_PANEL_BOOT_DATA boot_data;               /**< Data pro boot sector viewer. */
    st_PANEL_RAW_IO_DATA raw_io_data;           /**< Data pro Raw I/O okno. */
    st_PANEL_GEOM_EDIT_DATA geom_edit_data;     /**< Data pro editaci geometrie. */
    en_LAST_OP_STATUS last_op_status;           /**< Stav poslední operace - viz en_LAST_OP_STATUS. */
    char last_op_msg[128];                      /**< Popis poslední operace (např. "Save", "Put"). */
} st_MZDISK_SESSION;


/**
 * @brief Správce všech otevřených sessions.
 *
 * Drží pole sessions a id aktivní session. Aktivní session je ta, na kterou
 * cílí globální akce (menu, toolbar, zkratky). Session se identifikuje přes
 * stabilní id - slot v poli sessions[] se recykluje po close+open a není
 * proto stabilním klíčem.
 *
 * @par Invarianty:
 * - active_id == 0 pokud žádná session není otevřená.
 * - active_id != 0 implikuje existenci otevřené session s tím id.
 * - next_id > 0 (monotónní counter, startuje od 1).
 */
typedef struct st_MZDISK_SESSION_MANAGER {
    st_MZDISK_SESSION sessions[MZDISK_MAX_SESSIONS];   /**< Pole slotů sessions. */
    uint64_t active_id;                                  /**< Id aktivní session (0 = žádná). */
    uint64_t next_id;                                    /**< Generátor id pro další session_open. */
    int count;                                           /**< Počet otevřených sessions. */
} st_MZDISK_SESSION_MANAGER;


/**
 * @brief Inicializuje session manager.
 *
 * Vynuluje všechny sessions a nastaví active_id na 0, next_id na 1.
 *
 * @param mgr Ukazatel na session manager.
 *
 * @pre mgr != NULL.
 * @post Všechny sessions jsou zavřené, active_id == 0, next_id == 1, count == 0.
 */
extern void mzdisk_session_manager_init ( st_MZDISK_SESSION_MANAGER *mgr );


/**
 * @brief Vytvoří prázdnou session (okno bez disku).
 *
 * Alokuje volný slot, přidělí id (mgr->next_id) a nastaví session do EMPTY
 * stavu: is_open = true, has_disk = false.
 *
 * Parametr allow_primary řídí, zda se nová session může stát primární:
 *   - true + žádná primární neexistuje → nová je primární (renderuje se
 *     v hlavním okně).
 *   - true + primární existuje → nová je detached (vlastní ImGui okno).
 *   - false → nová je vždy detached, i kdyby primární zatím nebyla.
 *
 * Typické použití:
 *   - Inicializace hlavního okna při startu aplikace: allow_primary = true.
 *   - File > New Window (nové detached okno): allow_primary = false.
 *
 * Nová session se stane aktivní (active_id = s->id).
 *
 * @param mgr Session manager.
 * @param allow_primary Povolit, aby se nová session stala primární.
 * @return Ukazatel na vytvořenou session, nebo NULL při MZDISK_MAX_SESSIONS.
 *
 * @pre mgr != NULL.
 * @post Při úspěchu: is_open = true, has_disk = false, window_open = true,
 *       id != 0, active_id = s->id.
 */
extern st_MZDISK_SESSION* mzdisk_session_create_empty ( st_MZDISK_SESSION_MANAGER *mgr,
                                                         bool allow_primary );


/**
 * @brief Načte DSK soubor do existující session.
 *
 * Pokud session již má disk (has_disk = true), nejprve ho zavře
 * (bez uložení - volající musí zajistit případný save). Pak otevře
 * nový DSK, provede auto-detekci a naplní panel data.
 *
 * Session musí být existující (is_open = true). Typicky se volá:
 *   1. Po mzdisk_session_create_empty() pro prvotní načtení.
 *   2. Nad existující session pro záměnu disku (File > Open).
 *
 * @param session Existující session (is_open = true).
 * @param filepath Cesta k DSK souboru.
 * @return MZDSK_RES_OK při úspěchu,
 *         MZDSK_RES_DSK_ERROR při chybě otevření.
 *
 * @pre session != NULL, session->is_open == true, filepath != NULL.
 * @post Při úspěchu: has_disk = true, disc a detect_result naplněné,
 *       panel data inicializovaná, is_dirty = false.
 *       Při chybě: has_disk = false, session zůstává EMPTY.
 */
extern en_MZDSK_RES mzdisk_session_load ( st_MZDISK_SESSION *session, const char *filepath );


/**
 * @brief Otevře DSK soubor jako novou session (compat wrapper).
 *
 * Kombinuje mzdisk_session_create_empty + mzdisk_session_load do jednoho
 * volání. Zachovává původní sémantiku: při úspěchu je nová session otevřená,
 * aktivní a LOADED (has_disk = true).
 *
 * @param mgr Session manager.
 * @param filepath Cesta k DSK souboru.
 * @return MZDSK_RES_OK při úspěchu,
 *         MZDSK_RES_DSK_ERROR při chybě otevření,
 *         MZDSK_RES_NO_SPACE pokud je dosažen limit MZDISK_MAX_SESSIONS.
 *
 * @pre mgr != NULL, filepath != NULL a ukazuje na existující soubor.
 * @post Při úspěchu: nová session je otevřená, je aktivní (active_id == s->id),
 *       has_disk == true, detect_result je naplněný, id != 0.
 *       Při chybě: stav manageru se nemění.
 */
extern en_MZDSK_RES mzdisk_session_open ( st_MZDISK_SESSION_MANAGER *mgr, const char *filepath );


/**
 * @brief Uloží změny aktivní session na disk.
 *
 * Zapíše paměťový obraz zpět do původního souboru.
 * Po úspěšném uložení nastaví is_dirty na false.
 *
 * @param session Ukazatel na session k uložení.
 * @return MZDSK_RES_OK při úspěchu,
 *         MZDSK_RES_WRITE_PROTECTED pokud je disk chráněn proti zápisu,
 *         MZDSK_RES_DSK_ERROR při chybě zápisu.
 *
 * @pre session != NULL, session->is_open == true.
 * @post Při úspěchu: is_dirty == false, soubor na disku odpovídá paměti.
 */
extern en_MZDSK_RES mzdisk_session_save ( st_MZDISK_SESSION *session );


/**
 * @brief Zavře session podle id a uvolní její zdroje.
 *
 * Neuloží změny - volající musí případně zavolat mzdisk_session_save() předem.
 * Pokud je zavíraná session aktivní, vybere jinou otevřenou jako aktivní,
 * nebo nastaví active_id na 0.
 *
 * Pokud je zavíraná session primární (is_primary == true), primární status
 * se po zavření uvolní - nová primární bude nastavena až při dalším
 * mzdisk_session_open.
 *
 * @param mgr Session manager.
 * @param id Id session k zavření.
 *
 * @pre mgr != NULL.
 * @post Session s daným id je zavřená (pokud existovala), zdroje uvolněny,
 *       active_id a count aktualizovány. Pokud id neexistuje, noop.
 */
extern void mzdisk_session_close_by_id ( st_MZDISK_SESSION_MANAGER *mgr, uint64_t id );


/**
 * @brief Zavře všechny otevřené sessions.
 *
 * Neuloží změny - volající musí případně uložit předem.
 *
 * @param mgr Session manager.
 *
 * @pre mgr != NULL.
 * @post Všechny sessions jsou zavřené, count == 0, active_id == 0.
 */
extern void mzdisk_session_close_all ( st_MZDISK_SESSION_MANAGER *mgr );


/**
 * @brief Vrátí ukazatel na aktivní session (nebo NULL pokud žádná není).
 *
 * @param mgr Session manager.
 * @return Ukazatel na aktivní session, nebo NULL.
 */
extern st_MZDISK_SESSION* mzdisk_session_get_active ( st_MZDISK_SESSION_MANAGER *mgr );


/**
 * @brief Najde session podle id.
 *
 * @param mgr Session manager.
 * @param id Hledané id.
 * @return Ukazatel na otevřenou session s daným id, nebo NULL.
 */
extern st_MZDISK_SESSION* mzdisk_session_get_by_id ( st_MZDISK_SESSION_MANAGER *mgr, uint64_t id );


/**
 * @brief Vrátí ukazatel na primární session (renderuje se embedded v hlavním okně).
 *
 * @param mgr Session manager.
 * @return Ukazatel na primární session, nebo NULL pokud žádná není.
 */
extern st_MZDISK_SESSION* mzdisk_session_get_primary ( st_MZDISK_SESSION_MANAGER *mgr );


/**
 * @brief Nastaví aktivní session dle id.
 *
 * @param mgr Session manager.
 * @param id Id session (musí být otevřená).
 *
 * @post Pokud id odpovídá otevřené session, mgr->active_id = id.
 *       Jinak beze změny.
 */
extern void mzdisk_session_set_active_by_id ( st_MZDISK_SESSION_MANAGER *mgr, uint64_t id );


/**
 * @brief Znovu načte metadata disku a všechna panelová data.
 *
 * Použije se po operacích, které mění geometrii disku
 * (change-track, append-tracks, shrink). Refreshuje tracks_rules,
 * format, FS detekci a všechny panel data.
 *
 * @param session Aktivní session.
 *
 * @pre session != NULL, session->is_open == true, disc platný.
 * @post Všechna panelová data odpovídají aktuálnímu stavu disku.
 */
extern void mzdisk_session_reload_panels ( st_MZDISK_SESSION *session );


/**
 * @brief Vrátí textový popis detekovaného filesystému.
 *
 * @param type Typ filesystému z auto-detekce.
 * @return Statický řetězec (např. "FSMZ", "CP/M", "MRS", "Unknown").
 */
extern const char* mzdisk_session_fs_type_str ( en_MZDSK_FS_TYPE type );


/**
 * @brief Vrátí detailní textový popis filesystému včetně CP/M varianty.
 *
 * Pro CP/M rozlišuje SD a HD podle cpm_format v detect_result.
 * Pro ostatní FS typy vrací stejný řetězec jako mzdisk_session_fs_type_str().
 *
 * @param result Výsledek auto-detekce FS.
 * @return Statický řetězec (např. "FSMZ", "CP/M SD", "CP/M HD", "MRS", "Unknown").
 *
 * @pre result != NULL.
 */
extern const char* mzdisk_session_fs_type_str_detail ( const st_MZDSK_DETECT_RESULT *result );


/**
 * @brief Vygeneruje titulek OS/ImGui okna pro session.
 *
 * Formát:
 * - Primární session bez disku:  "mzdisk vX.Y.Z"
 * - Primární session s diskem:   "mzdisk vX.Y.Z - name.dsk"
 * - Detached session bez disku:  "mzdisk #NN"
 * - Detached session s diskem:   "mzdisk #NN - name.dsk"
 *
 * kde NN je `session->id` (monotónní counter, 1..).
 *
 * Používá se jak pro SDL_SetWindowTitle hlavního okna, tak pro prefix
 * ImGui Begin titulku detached okna (plus stabilní `###mzdisk-session-...`
 * suffix pro ImGui ID).
 *
 * @param[in]  session Session (smí být NULL → vrátí "mzdisk vX.Y.Z").
 * @param[out] buf     Výstupní buffer (min ~280 B).
 * @param      size    Velikost bufferu.
 */
extern void mzdisk_session_format_window_title (
    const st_MZDISK_SESSION *session, char *buf, size_t size );


/**
 * @brief Nastaví stav poslední operace pro zobrazení ve statusbaru.
 *
 * @param session Session, do které se stav zapisuje.
 * @param status Stav operace (en_LAST_OP_STATUS).
 * @param msg Popis operace (např. "Save", "Put 3 files"). Zkopíruje se.
 *
 * @pre session != NULL, msg != NULL.
 * @post session->last_op_status a session->last_op_msg jsou nastaveny.
 */
extern void mzdisk_session_set_last_op ( st_MZDISK_SESSION *session, en_LAST_OP_STATUS status, const char *msg );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_DISK_SESSION_H */
