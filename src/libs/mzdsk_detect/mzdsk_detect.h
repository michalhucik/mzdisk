/**
 * @file   mzdsk_detect.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Auto-detekce souborového systému na discích Sharp MZ.
 *
 * Implementuje třístupňovou detekci souborového systému:
 * 1. FSMZ (plný MZBASIC formát) - identifikován z geometrie
 * 2. MRS - FAT probe na bloku 36 (0xFA signatura)
 * 3. CP/M - zkoušení presetů SD/HD s validací adresáře
 *
 * Výsledkem detekce je typ filesystému a inicializované parametry
 * (DPB pro CP/M, config pro MRS), které mohou být přímo použity
 * pro další operace s diskem.
 *
 * Typické použití:
 * @code
 *   st_MZDSK_DETECT_RESULT result;
 *   en_MZDSK_RES err = mzdsk_detect_filesystem ( disc, &result );
 *   if ( err == MZDSK_RES_OK ) {
 *       switch ( result.type ) {
 *           case MZDSK_FS_FSMZ: ... break;
 *           case MZDSK_FS_CPM:  ... result.cpm_dpb ... break;
 *           case MZDSK_FS_MRS:  ... result.mrs_config ... break;
 *       }
 *   }
 * @endcode
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MZDSK_DETECT_H
#define MZDSK_DETECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"


/** @brief Verze knihovny mzdsk_detect. */
#define MZDSK_DETECT_VERSION "1.0.0"

/** @brief Výchozí číslo bloku s MRS FAT tabulkou. */
#define MZDSK_DETECT_MRS_DEFAULT_FAT_BLOCK 36


/**
 * @brief Typ detekovaného souborového systému.
 *
 * Výsledek auto-detekce. MZDSK_FS_UNKNOWN znamená, že žádný
 * ze známých filesystémů nebyl identifikován (disk může mít
 * custom formát nebo poškozenou strukturu).
 */
typedef enum en_MZDSK_FS_TYPE {
    MZDSK_FS_UNKNOWN = 0,   /**< Neznámý/nerozpoznaný formát */
    MZDSK_FS_FSMZ,          /**< FSMZ (MZ-BASIC) - plný MZBASIC formát */
    MZDSK_FS_CPM,            /**< CP/M 2.2 (SD nebo HD) */
    MZDSK_FS_MRS,            /**< MRS (Memory Resident System) */
    MZDSK_FS_BOOT_ONLY,      /**< Pouze FSMZ boot track, datová oblast nerozpoznána */
} en_MZDSK_FS_TYPE;


/**
 * @brief Výsledek auto-detekce souborového systému.
 *
 * Obsahuje typ detekovaného FS a inicializované parametry
 * relevantní pro daný typ. Při MZDSK_FS_CPM je platné cpm_dpb
 * a cpm_format, při MZDSK_FS_MRS je platný mrs_config.
 *
 * @invariant Pokud type == MZDSK_FS_CPM, pak cpm_dpb a cpm_format
 *            obsahují platné hodnoty pro detekovaný preset.
 * @invariant Pokud type == MZDSK_FS_MRS, pak mrs_config obsahuje
 *            platnou inicializovanou konfiguraci s FAT kopií.
 */
typedef struct st_MZDSK_DETECT_RESULT {
    en_MZDSK_FS_TYPE type;              /**< Detekovaný typ FS */
    en_MZDSK_CPM_FORMAT cpm_format;     /**< CP/M preset (platný jen při type == MZDSK_FS_CPM) */
    st_MZDSK_CPM_DPB cpm_dpb;          /**< CP/M DPB (platný jen při type == MZDSK_FS_CPM) */
    st_FSMRS_CONFIG mrs_config;         /**< MRS konfigurace (platná jen při type == MZDSK_FS_MRS) */
} st_MZDSK_DETECT_RESULT;


/**
 * @brief Detekuje souborový systém na otevřeném disku.
 *
 * Provede třístupňovou detekci:
 * 1. Pokud je disk identifikován jako MZBASIC (z geometrie),
 *    vrátí MZDSK_FS_FSMZ.
 * 2. Pokud má disk CP/M-like geometrii (9x512 nebo 18x512),
 *    zkusí MRS FAT probe (blok 36). Pokud projde, vrátí
 *    MZDSK_FS_MRS s inicializovanou mrs_config.
 * 3. Zkusí CP/M presety (SD/HD) a validuje adresář. Pokud
 *    projde, vrátí MZDSK_FS_CPM s DPB a formátem.
 * 4. Pokud má disk alespoň FSMZ boot track, vrátí
 *    MZDSK_FS_BOOT_ONLY.
 * 5. Jinak vrátí MZDSK_FS_UNKNOWN.
 *
 * @param[in]  disc   Ukazatel na otevřený disk. Nesmí být NULL.
 *                    Disk musí mít platné tracks_rules (z mzdsk_disc_open).
 * @param[out] result Ukazatel na strukturu, kam se uloží výsledek.
 *                    Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu (i pokud FS není rozpoznán -
 *         v tom případě result->type == MZDSK_FS_UNKNOWN).
 *
 * @pre disc musí být otevřený a mít platné tracks_rules.
 * @post result->type obsahuje detekovaný typ a příslušné členy
 *       struktury jsou inicializovány.
 */
extern en_MZDSK_RES mzdsk_detect_filesystem ( st_MZDSK_DISC *disc,
                                                st_MZDSK_DETECT_RESULT *result );


/**
 * @brief Ověří, zda surová CP/M adresářová položka vypadá jako platná.
 *
 * Replikuje sémantiku CP/M 2.2 BDOS:
 * - user == 0xE5 (empty) - vždy validní.
 * - user ∈ <0,15> - standardní user area, vyžadujeme rc ≤ 0x80 a extent ≤ 0x1F.
 * - user > 15 (mimo 0xE5) - BDOS takovou entry ignoruje ve výpisech, ale
 *   považuje ji za alokovaný slot; akceptujeme jako "blokovaný".
 *
 * Obsah fname/ext nevalidujeme - BDOS při SEARCH porovnává jen po masce 0x7F
 * a nevyžaduje printable ASCII.
 *
 * @param[in] entry Ukazatel na surovou položku. Nesmí být NULL.
 * @return 1 pokud entry odpovídá CP/M sémantice, 0 jinak.
 */
extern int mzdsk_detect_is_plausible_cpm_entry ( const st_MZDSK_CPM_DIRENTRY *entry );


/**
 * @brief Zkusí validovat CP/M adresář s konkrétním DPB presetem.
 *
 * Inicializuje DPB podle presetu, načte surový adresář a ověří,
 * že všechny položky projdou validací
 * mzdsk_detect_is_plausible_cpm_entry().
 *
 * @param[in]  disc    Ukazatel na otevřený disk. Nesmí být NULL.
 * @param[in]  format  CP/M preset (SD/HD).
 * @param[out] dpb_out Ukazatel, kam se při úspěchu uloží DPB.
 *                     Nesmí být NULL.
 *
 * @return 1 pokud adresář validuje, 0 jinak.
 */
extern int mzdsk_detect_try_cpm_preset ( st_MZDSK_DISC *disc,
                                          en_MZDSK_CPM_FORMAT format,
                                          st_MZDSK_CPM_DPB *dpb_out );


/**
 * @brief Zkusí auto-detekovat CP/M formát podle geometrie disku.
 *
 * Vybere kandidátní presety podle počtu sektorů na stopu
 * (9 -> SD, 18 -> HD) a postupně je zkouší validovat.
 *
 * @param[in]  disc       Ukazatel na otevřený disk. Nesmí být NULL.
 * @param[out] format_out Detekovaný preset (platný jen při návratové hodnotě 1).
 * @param[out] dpb_out    DPB presetu (platný jen při návratové hodnotě 1).
 *
 * @return 1 pokud byl preset identifikován, 0 jinak.
 */
extern int mzdsk_detect_cpm_format ( st_MZDSK_DISC *disc,
                                      en_MZDSK_CPM_FORMAT *format_out,
                                      st_MZDSK_CPM_DPB *dpb_out );


/**
 * @brief Vrátí řetězec s verzí knihovny.
 *
 * @return Statický řetězec s verzí.
 */
extern const char* mzdsk_detect_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_DETECT_H */
