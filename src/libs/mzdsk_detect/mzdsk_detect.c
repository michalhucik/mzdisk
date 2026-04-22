/**
 * @file   mzdsk_detect.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Implementace auto-detekce souborového systému na discích Sharp MZ.
 *
 * Třístupňový algoritmus:
 * 1. FSMZ identifikace z geometrie (DSK_TOOLS_IDENTFORMAT_MZBASIC)
 * 2. MRS FAT probe na bloku 36 (fsmrs_init)
 * 3. CP/M preset validace adresáře (SD/HD)
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */


#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libs/mzdsk_detect/mzdsk_detect.h"
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzdsk_cpm/mzdsk_cpm.h"
#include "libs/mzdsk_mrs/mzdsk_mrs.h"
#include "libs/dsk/dsk_tools.h"


/** @brief Maximální počet CP/M directory entries testovaných při detekci. */
#define DETECT_MAX_DIR_ENTRIES  256


/**
 * @brief Ověří, zda surová CP/M adresářová položka vypadá jako platná.
 *
 * Validace replikuje sémantiku CP/M 2.2 BDOS (ověřeno proti nipos BDOS,
 * `nipbdos1.s`): jediný bajt, který BDOS interpretuje jako "volný slot",
 * je user == 0xE5. Vše ostatní považuje za alokovanou directory entry.
 * BDOS obsah fname/ext nijak strukturálně nevaliduje - při SEARCH porovnává
 * jen po masce 0x7F.
 *
 * Kritéria:
 * - user == 0xE5: vždy validní (empty slot).
 * - user ∈ <0,15>: standardní CP/M 2.2 user area - vyžadujeme strukturálně
 *   platný extent header (rc ≤ 0x80, extent ≤ 0x1F).
 * - user > 15 (mimo 0xE5): BDOS takovou entry nezahrne do výpisu (SEARCH
 *   žádá match na aktivní USRCODE, který je 0-15), ale v alokačním skenu
 *   ji počítá jako obsazenou. Pro detekci CP/M ji akceptujeme jako
 *   "blokovaný slot" - nezakazuje, ani nepotvrzuje přítomnost CP/M.
 *
 * @param[in] entry Ukazatel na surovou položku. Nesmí být NULL.
 * @return 1 pokud entry odpovídá CP/M sémantice, 0 jinak.
 */
int mzdsk_detect_is_plausible_cpm_entry ( const st_MZDSK_CPM_DIRENTRY *entry ) {

    /* Prázdný slot - vždy validní. */
    if ( entry->user == MZDSK_CPM_DELETED_ENTRY ) return 1;

    /* Standardní user area 0-15: vyžadujeme konzistentní extent header. */
    if ( entry->user <= 15 ) {
        if ( entry->rc > 0x80 ) return 0;
        if ( entry->extent > 0x1F ) return 0;
        return 1;
    }

    /* user > 15: alokovaný slot mimo zobrazitelné user areas. Akceptujeme. */
    return 1;
}


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
int mzdsk_detect_try_cpm_preset ( st_MZDSK_DISC *disc,
                                   en_MZDSK_CPM_FORMAT format,
                                   st_MZDSK_CPM_DPB *dpb_out ) {

    st_MZDSK_CPM_DPB dpb;
    mzdsk_cpm_init_dpb ( &dpb, format );

    /* Heap místo stacku: 256 * sizeof(st_MZDSK_CPM_DIRENTRY) = 8 kB. Volající
     * `mzdsk_detect_filesystem` má vedle toho ještě vlastní st_FSMRS_CONFIG
     * (~5.6 kB) v caller frame, takže na Windows (1 MB stack) + GUI threadech
     * byl risk přetečení. Audit H-10. */
    st_MZDSK_CPM_DIRENTRY *entries = malloc ( DETECT_MAX_DIR_ENTRIES * sizeof ( *entries ) );
    if ( entries == NULL ) return 0;

    int count = mzdsk_cpm_read_raw_directory ( disc, &dpb, entries, DETECT_MAX_DIR_ENTRIES );
    if ( count <= 0 ) {
        free ( entries );
        return 0;
    }

    for ( int i = 0; i < count; i++ ) {
        if ( !mzdsk_detect_is_plausible_cpm_entry ( &entries[i] ) ) {
            free ( entries );
            return 0;
        }
    }

    free ( entries );
    *dpb_out = dpb;
    return 1;
}


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
int mzdsk_detect_cpm_format ( st_MZDSK_DISC *disc,
                               en_MZDSK_CPM_FORMAT *format_out,
                               st_MZDSK_CPM_DPB *dpb_out ) {

    /* Najdeme první datové track rule (9x512 nebo 18x512) */
    st_DSK_TOOLS_TRACK_RULE_INFO *data_rule = NULL;
    for ( int i = 0; i < disc->tracks_rules->count_rules; i++ ) {
        st_DSK_TOOLS_TRACK_RULE_INFO *rule = &disc->tracks_rules->rule[i];
        if ( rule->sectors == 9 || rule->sectors == 18 ) {
            data_rule = rule;
            break;
        }
    }
    if ( data_rule == NULL ) return 0;

    en_MZDSK_CPM_FORMAT candidates[1];
    int n_candidates = 0;

    if ( data_rule->sectors == 9 ) {
        candidates[n_candidates++] = MZDSK_CPM_FORMAT_SD;
    } else if ( data_rule->sectors == 18 ) {
        candidates[n_candidates++] = MZDSK_CPM_FORMAT_HD;
    } else {
        return 0;
    }

    for ( int i = 0; i < n_candidates; i++ ) {
        if ( mzdsk_detect_try_cpm_preset ( disc, candidates[i], dpb_out ) ) {
            *format_out = candidates[i];
            return 1;
        }
    }
    return 0;
}


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
 * @param[out] result Ukazatel na strukturu, kam se uloží výsledek.
 *                    Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu (i pokud FS není rozpoznán).
 *
 * @pre disc musí být otevřený a mít platné tracks_rules.
 * @post result->type obsahuje detekovaný typ a příslušné členy
 *       struktury jsou inicializovány.
 */
en_MZDSK_RES mzdsk_detect_filesystem ( st_MZDSK_DISC *disc,
                                         st_MZDSK_DETECT_RESULT *result ) {

    memset ( result, 0, sizeof ( *result ) );
    result->type = MZDSK_FS_UNKNOWN;

    /* Bez boot tracku nemáme co detekovat */
    if ( 0 == disc->tracks_rules->mzboot_track ) {
        return MZDSK_RES_OK;
    }

    /* Krok 1: FSMZ (plný MZBASIC formát) */
    if ( DSK_TOOLS_IDENTFORMAT_MZBASIC == disc->format ) {
        result->type = MZDSK_FS_FSMZ;
        return MZDSK_RES_OK;
    }

    /* Krok 2+3: CP/M-like geometrie - zkusíme MRS a pak CP/M */
    if ( DSK_TOOLS_IDENTFORMAT_MZCPM == disc->format ||
         DSK_TOOLS_IDENTFORMAT_MZCPMHD == disc->format ) {

        /* Krok 2: MRS FAT probe */
        if ( MZDSK_RES_OK == fsmrs_init ( disc, MZDSK_DETECT_MRS_DEFAULT_FAT_BLOCK,
                                            &result->mrs_config ) ) {
            result->type = MZDSK_FS_MRS;
            return MZDSK_RES_OK;
        }

        /* Krok 3: CP/M presety */
        if ( mzdsk_detect_cpm_format ( disc, &result->cpm_format, &result->cpm_dpb ) ) {
            result->type = MZDSK_FS_CPM;
            return MZDSK_RES_OK;
        }

        /* Disk má boot track ale datová oblast nerozpoznána */
        result->type = MZDSK_FS_BOOT_ONLY;
        return MZDSK_RES_OK;
    }

    /* Jen FSMZ boot track (MZBOOT identformat) */
    if ( disc->tracks_rules->mzboot_track ) {
        result->type = MZDSK_FS_BOOT_ONLY;
        return MZDSK_RES_OK;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Vrátí řetězec s verzí knihovny.
 *
 * @return Statický řetězec s verzí.
 */
const char* mzdsk_detect_version ( void ) {
    return MZDSK_DETECT_VERSION;
}
