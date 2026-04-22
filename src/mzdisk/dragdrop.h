/**
 * @file dragdrop.h
 * @brief Drag&drop souborů mezi directory panely různých sessions.
 *
 * Umožňuje uživateli přetáhnout soubor z directory panelu jedné session
 * (FSMZ / CP/M / MRS) do directory panelu jiné session. Podporuje jak
 * same-FS copy (FSMZ -> FSMZ atd.), tak cross-FS copy (FSMZ -> CP/M apod.)
 * přes MZF jako interchange formát.
 *
 * Architektura (viz devdocs/plan-drag-drop-gui.md):
 *   1. ImGui BeginDragDropSource v řádku directory tabulky -> payload
 *      st_DND_FILE_PAYLOAD s {source_session_id, fs_type, file_index}.
 *   2. AcceptDragDropPayload v cílovém panelu -> drop handler.
 *   3. Drop handler volá dnd_transfer_file() - via temp MZF soubor
 *      provede Get na zdroji a Put na cíli, volitelně s Erase zdroje.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_DRAGDROP_H
#define MZDISK_DRAGDROP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "disk_session.h"


/** @brief ImGui payload ID string pro DnD souborů mezi directory panely. */
#define DND_PAYLOAD_FILE "MZDISK_DND_FILE"

/** @brief Maximální počet souborů v jednom DnD drag operaci. */
#define DND_MAX_FILES 64


/**
 * @brief DnD payload - reference na soubor(y) ve zdrojové session.
 *
 * Payload přenáší jen referenci, ne obsah. Drop handler si obsah sám
 * vyčte přes panel_*_get_file_mzf voláním na zdrojové session.
 *
 * Multi-select: pokud uživatel vybral víc souborů (panel->selected[])
 * a drag startuje z vybraného řádku, payload obsahuje všechny vybrané
 * indexy. Jinak jen jeden index (count = 1).
 *
 * @par Invarianty:
 * - source_session_id != 0 (platné ID otevřené session v okamžiku drag).
 * - fs_type je jeden z MZDSK_FS_FSMZ / MZDSK_FS_CPM / MZDSK_FS_MRS.
 *   Ostatní typy (UNKNOWN, BOOT_ONLY) nepodporují soubory a DnD se
 *   nezahájí.
 * - count ∈ <1, DND_MAX_FILES>, file_indices[0..count-1] obsahují
 *   platné indexy v panel->files[] zdrojové session v okamžiku drag.
 * - V okamžiku drop může být session uzavřena nebo indexy přerovnány -
 *   drop handler ošetří.
 */
typedef struct st_DND_FILE_PAYLOAD {
    uint64_t source_session_id;           /**< ID zdrojové session. */
    en_MZDSK_FS_TYPE fs_type;             /**< FS typ zdroje (MZDSK_FS_FSMZ/CPM/MRS). */
    int count;                             /**< Počet platných indexů v file_indices[]. */
    int file_indices[DND_MAX_FILES];      /**< Indexy souborů v panel->files[] zdroje. */
} st_DND_FILE_PAYLOAD;


/**
 * @brief Naplní payload podle aktuálního multi-select stavu panelu.
 *
 * Pokud clicked_index je v selected[], payload obsahuje VŠECHNY
 * vybrané (do DND_MAX_FILES). Jinak obsahuje jen clicked_index.
 *
 * @param[out] payload Payload k naplnění.
 * @param session_id ID zdrojové session.
 * @param fs_type FS typ zdroje.
 * @param clicked_index Index řádku, ze kterého uživatel zahájil drag.
 * @param selected Pole selected[] z panelu.
 * @param file_count Počet platných položek v panel->files[].
 */
extern void dnd_fill_payload (
    st_DND_FILE_PAYLOAD *payload,
    uint64_t session_id,
    en_MZDSK_FS_TYPE fs_type,
    int clicked_index,
    const bool *selected,
    int file_count
);


/**
 * @brief Vrátí true, pokud DnD čeká na uživatelskou odpověď v ASK dialogu.
 *
 * Při dup_mode=ASK a prvním konfliktu jmen v cíli se iterace přes
 * soubory zastaví a tato funkce vrátí true. Volající (GUI hlavní
 * smyčka) pak zobrazí modal, kde uživatel vybere Overwrite/Rename/Skip
 * a volá `dnd_user_chose_ask` s odpovídajícím mode.
 */
extern bool dnd_has_pending_ask ( void );


/**
 * @brief Vrátí ASCII jméno souboru, který způsobil ASK konflikt.
 *
 * Jméno je extrahované z MZF hlavičky zdrojového souboru. Pointer je
 * platný dokud je g_dnd_ask pending (tj. dokud nebyl volán
 * dnd_user_chose_ask nebo další drop).
 *
 * @return Platný null-terminated řetězec, nebo prázdný řetězec pokud
 *         není nic pending.
 */
extern const char* dnd_get_ask_conflict_name ( void );


/**
 * @brief Vrátí počet zbývajících souborů k přenosu v aktuální ASK.
 *
 * Použitelné v dialogu pro hlášku "X of Y files".
 */
extern int dnd_get_ask_remaining_count ( void );


/**
 * @brief Aplikuje uživatelovu volbu na pending ASK item a pokračuje iterace.
 *
 * @param applied_mode:
 *   - MZDSK_EXPORT_DUP_OVERWRITE: přepsat existující a retry put.
 *   - MZDSK_EXPORT_DUP_RENAME: auto-suffix ~N a retry put.
 *   - MZDSK_EXPORT_DUP_SKIP: přeskočit soubor.
 * @param apply_all Pokud true, zvolený mode se propisuje do všech
 *                  zbývajících souborů bez dalšího dotazu (stávající
 *                  "Overwrite all" semantika). Pokud false, iteraci
 *                  pokračuje v ASK módu a při další kolizi se znovu
 *                  zeptá.
 *
 * Pro zrušení celé drop operace použijte `dnd_cancel_ask()`.
 */
extern void dnd_user_chose_ask ( int applied_mode, bool apply_all );


/**
 * @brief Zruší pending DnD drop iteraci (Cancel v ASK popupu).
 *
 * Smaže temp soubor, pokud byly předchozí iterace úspěšné, reloadne
 * cílové panely s last_op hláškou "cancelled".
 */
extern void dnd_cancel_ask ( void );


/**
 * @brief Jednorázová inicializace DnD modulu - zaregistruje session manager.
 *
 * Panel render funkce nemají přímý přístup k manageru, ale drop handler
 * ho potřebuje pro najití zdrojové session podle id v payloadu. Volající
 * (GUI init) proto musí zaregistrovat mgr přes dnd_init před prvním
 * DnD eventem.
 *
 * @param mgr Session manager aplikace. Ulož NULL pro vypnutí DnD.
 */
extern void dnd_init ( st_MZDISK_SESSION_MANAGER *mgr );


/**
 * @brief Drop handler - zpracuje payload z AcceptDragDropPayload.
 *
 * Najde zdrojovou a cílovou session v registrovaném manageru (viz
 * dnd_init). Pokud některá neexistuje (např. zdroj zavřen mezi drag
 * a drop), vrátí chybu. Jinak volá dnd_transfer_file a při úspěchu
 * reloaduje panely obou sessions (cíle i zdroje při move).
 *
 * Konflikty jmen (FILE_EXISTS v cíli) se řeší podle dup_mode
 * (en_MZDSK_EXPORT_DUP_MODE z config):
 *   - RENAME (a ASK v MVP): přidej ~N suffix do jména MZF a retry put.
 *   - SKIP: soubor přeskočí, vrací MZDSK_RES_OK s err_msg "skipped".
 *   - OVERWRITE: zatím nepodporován, fallback na RENAME.
 *
 * Speciální případ: pokud payload->source_session_id == dst_id
 * (drop do sebe), proběhne klasický transfer - u same-FS to skončí
 * duplikátem (FSMZ povoluje) nebo auto-rename (CP/M/MRS).
 *
 * @param payload Payload z AcceptDragDropPayload (DND_PAYLOAD_FILE).
 * @param dst_session_id Id cílové session.
 * @param is_move true pro move (smazat zdroj), false pro copy.
 * @param dup_mode Strategie řešení duplicit (en_MZDSK_EXPORT_DUP_MODE).
 * @param target_cpm_user Cílová CP/M user oblast (0-15), nebo -1 pro auto:
 *                        při src=CP/M zachová zdrojový user, jinak 0.
 *                        Aplikuje se jen když dst session je CP/M.
 * @param err_msg Buffer pro chybovou hlášku.
 * @param err_msg_size Velikost bufferu.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chyba.
 *
 * @pre dnd_init byl zavolán s platným mgr.
 * @pre payload != NULL, dst_session_id != 0.
 */
extern en_MZDSK_RES dnd_handle_drop (
    const st_DND_FILE_PAYLOAD *payload,
    uint64_t dst_session_id,
    bool is_move,
    int dup_mode,
    int target_cpm_user,
    char *err_msg, size_t err_msg_size
);


/**
 * @brief Provede přenos souboru ze zdroje do cíle (copy nebo move).
 *
 * Vytvoří dočasný MZF soubor, volá panel_*_get_file_mzf na zdrojovou
 * session (podle src_fs), pak panel_*_put_file_mzf na cílovou session
 * (podle dst_fs). Pokud is_move a put uspěje, volá panel_*_delete_file
 * na zdroj. Nakonec smaže temp MZF.
 *
 * FS typy src_fs a dst_fs mohou být libovolné kombinace
 * (FSMZ/CPM/MRS x FSMZ/CPM/MRS). Cross-FS přenos ztrácí některá
 * metadata (CP/M user area, MRS load/exec, FSMZ ftype rozdíly).
 *
 * Při úspěšném přenosu:
 *   - dst->is_dirty = true, dst panel data jsou reloadovaná.
 *   - src->is_dirty = true (jen pokud is_move), src panel data
 *     reloadovaná.
 *
 * Při selhání get: žádná změna, err_msg obsahuje popis.
 * Při selhání put: zdroj beze změny, err_msg obsahuje popis.
 * Při selhání delete při move: cíl už má kopii (MZDSK_RES_OK ohledně
 * přenosu), ale zdroj zůstává - vráceno však MZDSK_RES z delete.
 *
 * @param src Zdrojová session (musí být is_open, has_disk).
 * @param src_index Index souboru v panel->files[] zdroje.
 * @param dst Cílová session (musí být is_open, has_disk).
 * @param is_move false = copy, true = move (zdroj po put smazat).
 * @param err_msg Buffer pro chybovou hlášku (anglicky, zakončeno '\0').
 * @param err_msg_size Velikost bufferu err_msg.
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód z get/put/delete.
 *
 * @pre src != NULL, src->is_open, src->has_disk, src_index >= 0.
 * @pre dst != NULL, dst->is_open, dst->has_disk.
 * @pre err_msg != NULL, err_msg_size > 0.
 */
extern en_MZDSK_RES dnd_transfer_file (
    st_MZDISK_SESSION *src, int src_index,
    st_MZDISK_SESSION *dst,
    bool is_move,
    int dup_mode,
    int target_cpm_user,
    char *err_msg, size_t err_msg_size
);


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_DRAGDROP_H */
