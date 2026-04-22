/**
 * @file   mzdsk_tools.h
 * @brief  Presetové nástroje pro vytváření DSK diskových obrazů Sharp MZ.
 *
 * Knihovna definuje předdefinované geometrie (presety) pro nejběžnější
 * diskové formáty Sharp MZ a poskytuje funkce pro vytvoření prázdného
 * DSK obrazu, případně i s inicializací souborového systému FSMZ
 * nebo CP/M.
 *
 * Presety odpovídají diskovým formátům používaným v emulátoru mz800new
 * a nástroji fstool.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef MZDSK_TOOLS_H
#define MZDSK_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"


    /* ============================================================ */
    /*  Presety                                                     */
    /* ============================================================ */

    /**
     * @brief Předdefinované diskové formáty (presety) pro Sharp MZ.
     *
     * Každý preset specifikuje geometrii disku (strany, stopy, sektory,
     * velikost sektoru, řazení sektorů a filler bajt).
     *
     * Hodnota MZDSK_PRESET_COUNT slouží jako zarážka a udává celkový
     * počet platných presetů.
     */
    typedef enum en_MZDSK_PRESET {
        MZDSK_PRESET_BASIC = 0,     /**< MZ-BASIC: 2 strany, 80 stop/stranu, 16x256 B, normální řazení */
        MZDSK_PRESET_CPM_SD,        /**< LEC CP/M SD: 2 strany, 80 stop/stranu, 9x512 B prokládané + boot 16x256 B */
        MZDSK_PRESET_CPM_HD,        /**< LEC CP/M HD: 2 strany, 80 stop/stranu, 18x512 B prokládané + boot 16x256 B */
        MZDSK_PRESET_MRS,           /**< MRS: 2 strany, 80 stop/stranu, 9x512 B prokládané + boot 16x256 B */
        MZDSK_PRESET_LEMMINGS,      /**< Lemmings: 2 strany, 80 stop/stranu, 9x512 B + stopa 16 custom 10x512 B */
        MZDSK_PRESET_COUNT          /**< Počet platných presetů (zarážka) */
    } en_MZDSK_PRESET;


    /* ============================================================ */
    /*  Konstanty FSMZ formátu                                      */
    /* ============================================================ */

    /**
     * @brief Kódovaná velikost sektoru pro FSMZ formát (256 B).
     *
     * Odpovídá DSK_SECTOR_SIZE_256. Používá se při vytváření
     * stop s MZ-BASIC geometrií.
     */
#define MZDSK_FSMZ_SECTOR_SSIZE         DSK_SECTOR_SIZE_256

    /**
     * @brief Velikost FSMZ sektoru v bajtech.
     */
#define MZDSK_FSMZ_SECTOR_SIZE          256

    /**
     * @brief Počet sektorů na stopě pro FSMZ formát.
     */
#define MZDSK_FSMZ_SECTORS_ON_TRACK     16

    /**
     * @brief Číslo alokačního bloku pro IPLPRO (bootstrap).
     */
#define MZDSK_FSMZ_ALOCBLOCK_IPLPRO     0

    /**
     * @brief Číslo alokačního bloku pro DINFO (informace o disku).
     */
#define MZDSK_FSMZ_ALOCBLOCK_DINFO      15

    /**
     * @brief Číslo prvního alokačního bloku adresáře.
     *
     * Adresář zabírá bloky 16 az 23 (8 bloků = 8 sektorů).
     */
#define MZDSK_FSMZ_ALOCBLOCK_DIR        16

    /**
     * @brief Počet položek adresáře v jednom bloku.
     *
     * Jedna položka má 32 bajtů, blok má 256 bajtů, tedy 8 položek.
     */
#define MZDSK_FSMZ_DIRITEMS_PER_BLOCK   8

    /**
     * @brief Výchozí číslo bloku pro začátek souborové oblasti (FAREA).
     *
     * Standardní hodnota 0x30 (desítkově 48).
     */
#define MZDSK_FSMZ_DEFAULT_FAREA_BLOCK  0x30

    /**
     * @brief Počet bloků adresáře standardního FSMZ (8 bloků).
     */
#define MZDSK_FSMZ_DIR_BLOCKS           8

    /**
     * @brief Typ souboru pro IPLPRO bootstrap záznam.
     *
     * IPLPRO má vždy ftype = 0x03.
     */
#define MZDSK_FSMZ_IPLPRO_FTYPE        0x03

    /**
     * @brief Délka jména IPLPRO souboru (kratší než standardní MZF).
     */
#define MZDSK_FSMZ_IPLFNAME_LENGTH     13

    /**
     * @brief Zakončovací znak jména souboru v FSMZ.
     */
#define MZDSK_FSMZ_FNAME_TERMINATOR    0x0d


    /* ============================================================ */
    /*  API funkce                                                  */
    /* ============================================================ */

    /**
     * @brief Vrátí textový název presetu pro zobrazení v uživatelském rozhraní.
     *
     * @param preset Identifikátor presetu.
     * @return Ukazatel na statický řetězec s názvem presetu.
     *         Pro neplatný preset vrací "Unknown".
     *
     * @pre preset < MZDSK_PRESET_COUNT (jinak vrátí "Unknown").
     * @post Vrácený ukazatel je platný po celou dobu běhu programu.
     */
    extern const char* mzdsk_tools_preset_name ( en_MZDSK_PRESET preset );


    /**
     * @brief Vytvoří prázdný DSK obraz podle zvoleného presetu.
     *
     * Podle presetu sestaví strukturu st_DSK_DESCRIPTION s odpovídajícími
     * pravidly geometrie a zavolá dsk_tools_create_image() pro zapsání
     * kompletního DSK obrazu do handleru.
     *
     * Handler musí být otevřený pro zápis (READY, nikoli READ_ONLY).
     *
     * @param h Handler pro zápis DSK obrazu. Nesmí být NULL.
     * @param preset Identifikátor presetu (en_MZDSK_PRESET).
     * @param sides Počet stran (1 nebo 2).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     *         (neplatný preset, chyba alokace, chyba zápisu).
     *
     * @pre h != NULL a handler je otevřený pro zápis.
     * @pre preset < MZDSK_PRESET_COUNT.
     * @pre sides == 1 || sides == 2.
     * @post Při úspěchu obsahuje handler kompletní prázdný DSK obraz.
     *
     * @note Obraz je prázdný - neobsahuje žádnou inicializaci souborového
     *       systému. Pro formátovaný disk použijte mzdsk_tools_format_basic(),
     *       mzdsk_tools_format_cpm_sd() nebo mzdsk_tools_format_cpm_hd().
     */
    extern int mzdsk_tools_create_from_preset ( st_HANDLER *h, en_MZDSK_PRESET preset, uint8_t sides );


    /**
     * @brief Vytvoří MZ-BASIC formátovaný disk.
     *
     * Provede kompletní vytvoření a inicializaci MZ-BASIC diskety:
     * 1. Vytvoří DSK obraz se všemi stopami v geometrii 16x256 B.
     * 2. Inicializuje DINFO blok (blok 15): farea=0x30, blocks=celkem-1, used=0x30.
     * 3. Inicializuje adresář (bloky 16-23): první 32 B = {0x80, 0x01, 0x00...}.
     *
     * Blok 0 (IPLPRO) se NEZAPISUJE - prázdný disk nesmí mít platnou
     * IPLPRO hlavičku, jinak by Sharp MZ ROM detekoval neexistující
     * zavaděč. Blok 0 zůstává vyplněný fillerem (po inverzi ftype=0x00).
     *
     * Data se zapisují s bitovou inverzí, jak vyžaduje FSMZ formát
     * na sektorech 16x256 B.
     *
     * @param h Handler pro zápis DSK obrazu. Nesmí být NULL.
     * @param tracks Počet stop per strana.
     * @param sides Počet stran (1 nebo 2).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     *
     * @pre h != NULL a handler je otevřený pro zápis.
     * @pre tracks >= 1, sides == 1 nebo sides == 2.
     * @post Při úspěchu obsahuje handler kompletní formátovanou MZ-BASIC disketu.
     *
     * @note Celkový počet absolutních stop = tracks * sides.
     * @note Blok-to-track mapování respektuje FSMZ konvenci obracení stran
     *       (sudé absolutní stopy = strana 1, liché = strana 0 z pohledu FSMZ).
     */
    extern int mzdsk_tools_format_basic ( st_HANDLER *h, uint8_t tracks, uint8_t sides );


    /**
     * @brief Vytvoří CP/M SD (Lamač) formátovaný disk.
     *
     * Provede vytvoření DSK obrazu s geometrií pro LEC CP/M SD:
     * - Stopa 0: 9x512 B, prokládané řazení (LEC), filler 0xE5
     * - Stopa 1: 16x256 B, normální řazení (boot track), filler 0xFF
     * - Stopy 2+: 9x512 B, prokládané řazení (LEC), filler 0xE5
     *
     * @param h Handler pro zápis DSK obrazu. Nesmí být NULL.
     * @param tracks Celkový počet stop.
     * @param sides Počet stran (1 nebo 2, výchozí 2).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     *
     * @pre h != NULL a handler je otevřený pro zápis.
     * @pre tracks >= 3 (minimálně 3 stopy pro pravidlový systém).
     * @post Při úspěchu obsahuje handler kompletní CP/M SD DSK obraz.
     */
    extern int mzdsk_tools_format_cpm_sd ( st_HANDLER *h, uint8_t tracks, uint8_t sides );


    /**
     * @brief Vytvoří CP/M HD (Lucky-Soft) formátovaný disk.
     *
     * Provede vytvoření DSK obrazu s geometrií pro Lucky-Soft CP/M HD:
     * - Stopa 0: 18x512 B, 2x prokládané řazení (LEC HD), filler 0xE5
     * - Stopa 1: 16x256 B, normální řazení (boot track), filler 0xFF
     * - Stopy 2+: 18x512 B, 2x prokládané řazení (LEC HD), filler 0xE5
     *
     * @param h Handler pro zápis DSK obrazu. Nesmí být NULL.
     * @param tracks Celkový počet stop.
     * @param sides Počet stran (1 nebo 2, výchozí 2).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     *
     * @pre h != NULL a handler je otevřený pro zápis.
     * @pre tracks >= 3 (minimálně 3 stopy pro pravidlový systém).
     * @post Při úspěchu obsahuje handler kompletní CP/M HD DSK obraz.
     */
    extern int mzdsk_tools_format_cpm_hd ( st_HANDLER *h, uint8_t tracks, uint8_t sides );


    /* ============================================================ */
    /*  Verze knihovny                                               */
    /* ============================================================ */

    /** @brief Verze knihovny mzdsk_tools. */
#define MZDSK_TOOLS_VERSION "1.2.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzdsk_tools.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzdsk_tools_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_TOOLS_H */
