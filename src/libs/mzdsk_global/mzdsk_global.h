/**
 * @file   mzdsk_global.h
 * @brief  Globální typy, struktury a funkce pro práci s DSK diskovými obrazy Sharp MZ.
 *
 * Knihovna zapouzdřuje otevírání/zavírání DSK obrazů, čtení/zápis sektorů
 * s automatickou detekcí inverze dat (FSMZ formát) a pomocné řetězcové/
 * paměťové funkce pro filesystémové moduly (mzdsk_ipldisk, mzdsk_mrs,
 * mzdsk_cpm apod.).
 *
 * Port z fstool fs_global.h/fs_global_tools.h - pouze DSK podpora (bez FDC).
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


#ifndef MZDSK_GLOBAL_H
#define MZDSK_GLOBAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"


    /* ===================================================================
     * Typ média - normální vs. invertovaná data
     * =================================================================== */

    /**
     * @brief Typ média - určuje zda jsou data na sektoru normální nebo invertovaná.
     *
     * Dolní bajt (bity 0-7) nese kódovanou velikost sektoru (en_DSK_SECTOR_SIZE).
     * Horní bajt (bit 8) určuje, zda jsou data invertovaná (FSMZ formát).
     *
     * Invertovaná data se vyskytují u formátu MZ-BASIC (FSMZ) - stopy s 16
     * sektory po 256 bajtech. Při čtení/zápisu se automaticky provádí bitová
     * inverze (XOR 0xFF).
     */
    typedef enum en_MZDSK_MEDIUM {
        MZDSK_MEDIUM_NORMAL   = 0x00, /**< Normální (neinvertovaná) data */
        MZDSK_MEDIUM_INVERTED = 0x80  /**< Invertovaná data (FSMZ formát) */
    } en_MZDSK_MEDIUM;


    /* ===================================================================
     * Návratové kódy
     * =================================================================== */

    /**
     * @brief Návratové kódy operací knihovny mzdsk_global.
     *
     * Hodnoty jsou kompatibilní s původním fstool (en_FSGLOB_RES).
     */
    typedef enum en_MZDSK_RES {
        MZDSK_RES_WRITE_PROTECTED = -2, /**< Disk je chráněný proti zápisu */
        MZDSK_RES_DSK_ERROR    = -1,    /**< Chyba DSK vrstvy (čtení/zápis/formát) */
        MZDSK_RES_OK           = 0,     /**< Operace proběhla úspěšně */
        MZDSK_RES_FILE_NOT_FOUND,       /**< Soubor nebyl nalezen */
        MZDSK_RES_FILE_EXISTS,          /**< Soubor již existuje */
        MZDSK_RES_BAD_NAME,            /**< Neplatný název souboru */
        MZDSK_RES_DISK_FULL,           /**< Disk je plný (nedostatek bloků) */
        MZDSK_RES_NO_SPACE,            /**< Nedostatek souvislého místa (fragmentace) */
        MZDSK_RES_DIR_FULL,            /**< Adresář je plný */
        MZDSK_RES_INVALID_PARAM,       /**< Neplatný parametr (NULL ukazatel, mimo rozsah) */
        MZDSK_RES_BUFFER_SMALL,        /**< Výstupní buffer je příliš malý */
        MZDSK_RES_FORMAT_ERROR,        /**< Neplatný nebo nerozpoznaný formát disku */
        MZDSK_RES_FILE_LOCKED,         /**< Soubor je uzamčen (lock flag) */
        MZDSK_RES_UNKNOWN_ERROR,       /**< Neznámá chyba */
    } en_MZDSK_RES;

    /**
     * @brief Rozhoduje, zda je návratový kód fatální I/O chyba.
     *
     * Sémantika en_MZDSK_RES záměrně dělí chyby na dva druhy:
     * - Negativní hodnoty (DSK_ERROR, WRITE_PROTECTED) = fatální
     *   I/O chyba, stav disku nelze dál interpretovat.
     * - Pozitivní hodnoty (FILE_NOT_FOUND, END_OF_DIR, FILE_EXISTS...)
     *   = benigní logická situace; volající obvykle pokračuje jinak.
     *
     * Makro nahrazuje fragilní idiom `err < MZDSK_RES_OK`, který
     * spoléhal na konvenci "záporné = fatální" bez explicitního
     * pojmenování (audit L-10).
     */
#define MZDSK_RES_IS_FATAL(res) ( (res) < MZDSK_RES_OK )


    /* ===================================================================
     * Callback pro zjištění informací o sektoru
     * =================================================================== */

    /**
     * @brief Typ callbacku pro zjištění informací o sektoru.
     *
     * Callback vrací kombinaci en_MZDSK_MEDIUM (horní bajt) a kódované
     * velikosti sektoru en_DSK_SECTOR_SIZE (dolní bajt).
     *
     * Filesystémové moduly ho používají k rozhodnutí, zda data na daném
     * sektoru potřebují invertovat (FSMZ formát).
     *
     * @param track Absolutní číslo stopy.
     * @param sector ID sektoru na stopě.
     * @param user_data Uživatelská data nastavená při registraci callbacku.
     * @return Kombinace en_MZDSK_MEDIUM | en_DSK_SECTOR_SIZE.
     */
    typedef uint8_t (*mzdsk_sector_info_cb_t)( uint16_t track, uint16_t sector, void *user_data );


    /* ===================================================================
     * Struktura disku
     * =================================================================== */

    /**
     * @brief Reprezentace otevřeného DSK diskového obrazu.
     *
     * Zapouzdřuje vše potřebné pro práci s jedním DSK souborem:
     * handler pro I/O operace, pravidla geometrie stop, identifikovaný
     * formát, sektorový cache buffer a callback pro detekci inverze dat.
     *
     * Životní cyklus:
     * 1. Inicializace přes mzdsk_disc_open()
     * 2. Čtení/zápis sektorů přes mzdsk_disc_read_sector()/mzdsk_disc_write_sector()
     * 3. Uvolnění přes mzdsk_disc_close()
     *
     * @invariant Po úspěšném mzdsk_disc_open() jsou handler, tracks_rules
     *            a cache vždy nenulové.
     * @invariant Po mzdsk_disc_close() jsou všechny členy vynulované.
     */
    typedef struct st_MZDSK_DISC {
        st_HANDLER *handler;                        /**< Handler pro I/O operace nad DSK souborem (vlastněný strukturou) */
        st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules; /**< Pravidla geometrie stop (vlastněná strukturou) */
        en_DSK_TOOLS_IDENTFORMAT format;            /**< Identifikovaný formát diskety */
        uint8_t *cache;                             /**< Sektorový cache buffer (1024 B, vlastněný strukturou) */
        mzdsk_sector_info_cb_t sector_info_cb;      /**< Callback pro zjištění typu média sektoru */
        void *sector_info_cb_data;                  /**< Uživatelská data pro sector_info_cb */
        char *filename;                             /**< Jméno DSK souboru pro paměťový režim (nevlastněné) */
    } st_MZDSK_DISC;


    /* ===================================================================
     * Otevírání a zavírání disku
     * =================================================================== */

    /**
     * @brief Otevře DSK diskový obraz ze souboru.
     *
     * Provede kompletní inicializaci struktury st_MZDSK_DISC:
     * 1. Otevře soubor přes generic_driver (souborový handler)
     * 2. Ověří DSK formát přes dsk_tools_check_dsk_fileinfo()
     * 3. Analyzuje geometrii a získá pravidla stop
     * 4. Identifikuje formát diskety
     * 5. Nastaví výchozí sector_info_cb (mzdsk_sector_info_cb)
     * 6. Alokuje cache buffer (1024 bajtů)
     *
     * @param disc Ukazatel na strukturu disku. Musí být platný (ne NULL).
     *             Struktura bude kompletně přepsána.
     * @param filename Cesta k DSK souboru. Musí být platný řetězec (ne NULL).
     *                 Handler drží ukazatel na filename - řetězec musí
     *                 přežít celou dobu životnosti disku.
     * @param open_mode Režim otevření souboru (čtení/zápis/vytvoření).
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_DSK_ERROR pokud soubor nelze otevřít nebo není platný DSK.
     * @return MZDSK_RES_UNKNOWN_ERROR pokud selže alokace paměti.
     *
     * @post Při úspěchu je disc kompletně inicializovaný a připravený k použití.
     * @post Při neúspěchu je disc vynulovaný (uvolněné zdroje).
     *
     * @note Volající je zodpovědný za zavolání mzdsk_disc_close() po ukončení
     *       práce s diskem.
     */
    extern en_MZDSK_RES mzdsk_disc_open ( st_MZDSK_DISC *disc, char *filename, en_FILE_DRIVER_OPEN_MODE open_mode );

    /**
     * @brief Otevře DSK diskový obraz do paměti (bezpečný režim pro zápis).
     *
     * Načte celý DSK soubor do RAM přes paměťový handler. Všechny operace
     * probíhají nad paměťovou kopií. Změny se na disk zapíší až voláním
     * mzdsk_disc_save(). Při chybě zůstane originální soubor nedotčený.
     *
     * @param disc Ukazatel na strukturu disku.
     * @param filename Cesta k DSK souboru.
     * @param open_mode Režim otevření.
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES mzdsk_disc_open_memory ( st_MZDSK_DISC *disc, char *filename, en_FILE_DRIVER_OPEN_MODE open_mode );

    /**
     * @brief Uloží paměťový obraz disku zpět do souboru.
     *
     * @param disc Ukazatel na otevřený disk (paměťový handler).
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     * @pre Disk musí být otevřen přes mzdsk_disc_open_memory().
     */
    extern en_MZDSK_RES mzdsk_disc_save ( st_MZDSK_DISC *disc );

    /**
     * @brief Zavře otevřený DSK diskový obraz a uvolní všechny zdroje.
     *
     * Provede:
     * 1. Uvolní pravidla stop (dsk_tools_destroy_track_rules)
     * 2. Uvolní cache buffer
     * 3. Zavře handler (generic_driver_close) a uvolní ho
     * 4. Vynuluje celou strukturu disc
     *
     * @param disc Ukazatel na strukturu disku. Může být NULL (NOP).
     *             Po návratu jsou všechny členy vynulované.
     *
     * @post Struktura disc je kompletně vynulovaná.
     *
     * @note Bezpečné volat opakovaně - druhé a další volání jsou NOP.
     */
    extern void mzdsk_disc_close ( st_MZDSK_DISC *disc );


    /* ===================================================================
     * Čtení a zápis sektorů
     * =================================================================== */

    /**
     * @brief Přečte sektor z DSK obrazu s automatickou detekcí inverze.
     *
     * Přečte data sektoru do interního cache bufferu disc->cache.
     * Pokud sector_info_cb indikuje invertovaná data (FSMZ formát),
     * automaticky provede bitovou inverzi (XOR 0xFF) nad přečtenými daty.
     *
     * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
     * @param track Absolutní číslo stopy.
     * @param sector ID sektoru na stopě.
     *
     * @return MZDSK_RES_OK při úspěchu - data jsou v disc->cache.
     * @return MZDSK_RES_DSK_ERROR při chybě čtení.
     *
     * @pre disc musí být úspěšně otevřený přes mzdsk_disc_open().
     * @post Při úspěchu disc->cache obsahuje data sektoru (případně invertovaná).
     *
     * @note Velikost platných dat v cache odpovídá velikosti sektoru na dané stopě.
     */
    extern en_MZDSK_RES mzdsk_disc_read_sector ( st_MZDSK_DISC *disc, uint16_t track, uint16_t sector, void *dma );

    /**
     * @brief Zapíše sektor do DSK obrazu s automatickou detekcí inverze.
     *
     * Zapíše data z interního cache bufferu disc->cache do DSK obrazu.
     * Pokud sector_info_cb indikuje invertovaná data (FSMZ formát),
     * automaticky provede bitovou inverzi (XOR 0xFF) nad daty před zápisem
     * a po zápisu data v cache vrátí do původního stavu (druhá inverze).
     *
     * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
     * @param track Absolutní číslo stopy.
     * @param sector ID sektoru na stopě.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_DSK_ERROR při chybě zápisu.
     *
     * @pre disc musí být úspěšně otevřený přes mzdsk_disc_open().
     * @pre disc->cache musí obsahovat data k zápisu.
     * @post Data jsou zapsána do DSK obrazu (případně invertovaná).
     * @post disc->cache obsahuje původní (neinvertovaná) data.
     */
    extern en_MZDSK_RES mzdsk_disc_write_sector ( st_MZDSK_DISC *disc, uint16_t track, uint16_t sector, void *dma );


    /* ===================================================================
     * Chybové zprávy
     * =================================================================== */

    /**
     * @brief Vrátí textový popis chyby pro daný návratový kód.
     *
     * @param res Návratový kód operace (en_MZDSK_RES).
     * @return Ukazatel na statický řetězec s anglickým popisem chyby.
     *         Nikdy nevrací NULL.
     */
    extern const char* mzdsk_get_error ( en_MZDSK_RES res );


    /* ===================================================================
     * Inverze dat
     * =================================================================== */

    /**
     * @brief Provede bitovou inverzi (XOR 0xFF) nad blokem dat.
     *
     * Invertuje každý bajt v bufferu. Používá se pro konverzi dat
     * mezi formátem FSMZ (invertovaná data na médiu) a normální
     * reprezentací v paměti.
     *
     * Funkce je idempotentní - dvojí zavolání vrátí data do původního stavu.
     *
     * @param buffer Ukazatel na data k invertování. Nesmí být NULL.
     * @param size Počet bajtů k invertování. Může být 0 (NOP).
     *
     * @post Každý bajt v buffer[0..size-1] je XOR-ován s 0xFF.
     */
    extern void mzdsk_invert_data ( uint8_t *buffer, uint16_t size );


    /* ===================================================================
     * Pomocné řetězcové a paměťové funkce
     * =================================================================== */

    /**
     * @brief Kopíruje řetězec s omezením délky a zarovnáním mezerami.
     *
     * Kopíruje nejvýše n znaků ze src do dst. Pokud je src kratší než n,
     * zbytek dst se vyplní mezerami (0x20). Na pozici dst[n] se zapíše
     * ukončovací nula.
     *
     * Funkce se používá pro práci s názvy souborů na Sharp MZ disketách,
     * kde jsou jména zarovnána mezerami na pevnou délku.
     *
     * @param dst Cílový buffer. Musí mít velikost alespoň n+1 bajtů. Nesmí být NULL.
     * @param src Zdrojový řetězec (ukončený nulou). Nesmí být NULL.
     * @param n Maximální počet kopírovaných znaků (bez ukončovací nuly).
     *
     * @post dst obsahuje nejvýše n znaků ze src, doplněné mezerami, ukončené nulou.
     */
    extern uint8_t* mzdsk_strncpy ( uint8_t *d, const uint8_t *s, size_t n, uint8_t terminator );

    /**
     * @brief Porovná dva řetězce ve formátu Sharp MZ.
     *
     * Konec řetězce je definován jakýmkoliv znakem menším než 0x20
     * (typicky 0x00 nebo 0x0d).
     *
     * @param asrc První řetězec. Nesmí být NULL.
     * @param adst Druhý řetězec. Nesmí být NULL.
     *
     * @return 0 pokud jsou řetězce shodné.
     * @return Nenulová hodnota pokud se liší.
     */
    extern int mzdsk_mzstrcmp ( const uint8_t *asrc, const uint8_t *adst );

    /**
     * @brief Porovná dva bloky paměti.
     *
     * @param a První blok paměti. Nesmí být NULL.
     * @param b Druhý blok paměti. Nesmí být NULL.
     * @param size Počet bajtů k porovnání.
     *
     * @return 0 pokud jsou bloky shodné.
     * @return Nenulová hodnota pokud se liší.
     */
    extern int8_t mzdsk_memcmp ( const uint8_t *a, const uint8_t *b, size_t size );


    /* ===================================================================
     * Výchozí sector info callback
     * =================================================================== */

    /**
     * @brief Výchozí implementace callbacku pro zjištění informací o sektoru.
     *
     * Zjistí pravidlo pro danou stopu z tracks_rules a rozhodne o typu média:
     * - Pokud stopa má 16 sektorů a velikost sektoru 256 B -> FSMZ formát
     *   (MZDSK_MEDIUM_INVERTED | ssize)
     * - Jinak -> normální formát (MZDSK_MEDIUM_NORMAL | ssize)
     *
     * Tato funkce se automaticky nastavuje jako sector_info_cb při
     * mzdsk_disc_open(). Lze ji nahradit vlastní implementací.
     *
     * @param track Absolutní číslo stopy.
     * @param sector ID sektoru (nepoužívá se v této implementaci).
     * @param user_data Ukazatel na st_MZDSK_DISC (musí být platný).
     *
     * @return Kombinace en_MZDSK_MEDIUM | en_DSK_SECTOR_SIZE.
     * @return MZDSK_MEDIUM_NORMAL | DSK_SECTOR_SIZE_256 pokud pravidlo nenalezeno.
     */
    extern uint8_t mzdsk_sector_info_cb ( uint16_t track, uint16_t sector, void *user_data );


    /* ===================================================================
     * Striktní validace 8.3 jména souboru
     * =================================================================== */

    /**
     * @brief Návratový kód pro mzdsk_validate_83_name().
     *
     * Rozlišuje mezi platným vstupem a konkrétní kategorií chyby.
     * Používá se v "striktním" módu, který narozdíl od tolerantních
     * parserů (parse_filename, parse_mrs_filename) vstup nezkracuje
     * a nic nenormalizuje - při jakémkoliv porušení vrátí chybu.
     */
    typedef enum en_MZDSK_NAMEVAL {
        MZDSK_NAMEVAL_OK            = 0, /**< Vstup je platný. */
        MZDSK_NAMEVAL_EMPTY,             /**< Prázdný vstup nebo nulová délka jména před tečkou. */
        MZDSK_NAMEVAL_NAME_TOO_LONG,     /**< Jméno (před první tečkou) má > 8 znaků. */
        MZDSK_NAMEVAL_EXT_TOO_LONG,      /**< Přípona (za první tečkou) má > 3 znaky. */
        MZDSK_NAMEVAL_BAD_CHAR           /**< Jméno nebo přípona obsahuje zakázaný znak. */
    } en_MZDSK_NAMEVAL;

    /**
     * @brief Filesystem flavor pro volbu sady zakázaných znaků.
     *
     * Aktuálně obě varianty sdílejí stejnou forbidden sadu (CP/M BDOS
     * pravidla), ale enum ponechává prostor pro budoucí rozlišení.
     */
    typedef enum en_MZDSK_NAMEVAL_FLAVOR {
        MZDSK_NAMEVAL_FLAVOR_CPM = 0, /**< Pravidla pro CP/M filesystem. */
        MZDSK_NAMEVAL_FLAVOR_MRS = 1  /**< Pravidla pro MRS filesystem. */
    } en_MZDSK_NAMEVAL_FLAVOR;

    /**
     * @brief Striktně validuje jméno souboru v 8.3 formátu pro CP/M nebo MRS.
     *
     * Rozdělí vstup na jméno a příponu podle první tečky. Ověří:
     *   1. Neprázdnost - jinak vrací MZDSK_NAMEVAL_EMPTY.
     *   2. Délka jména <= 8 - jinak MZDSK_NAMEVAL_NAME_TOO_LONG.
     *   3. Délka přípony <= 3 - jinak MZDSK_NAMEVAL_EXT_TOO_LONG.
     *   4. Žádné zakázané znaky - jinak MZDSK_NAMEVAL_BAD_CHAR.
     *
     * Zakázané znaky pro oba flavory:
     *   - řídicí znaky (0x00-0x1F, 0x7F)
     *   - mezera (0x20)
     *   - znaky CP/M BDOS konvence: `< > . , ; : = ? * [ ] | /`
     *
     * Funkce NEPROVÁDÍ žádné zkracování ani změnu case. Výstup ve @p out_name
     * a @p out_ext je přesnou kopií vstupu (bez paddingu mezerami).
     *
     * @param[in]  input     Vstupní řetězec "NAME" nebo "NAME.EXT". Nesmí být NULL.
     * @param[in]  flavor    Typ filesystemu (aktuálně informativní, obě mají
     *                       stejnou forbidden sadu).
     * @param[out] out_name  Buffer min 9 bajtů. Při návratu MZDSK_NAMEVAL_OK
     *                       obsahuje jméno (NUL-terminated, bez paddingu).
     *                       Při chybě je obsah nedefinovaný. Může být NULL.
     * @param[out] out_ext   Buffer min 4 bajty. Analogicky pro příponu. Pokud
     *                       vstup neobsahuje tečku, bude prázdný řetězec.
     *                       Může být NULL.
     * @param[out] bad_char  Pokud return == MZDSK_NAMEVAL_BAD_CHAR, obsahuje
     *                       první nalezený zakázaný znak. Jinak nedefinováno.
     *                       Může být NULL.
     *
     * @return MZDSK_NAMEVAL_OK při validním vstupu, jinak specifický kód.
     *
     * @pre @p input != NULL.
     * @note CP/M knihovna vstup interně převede na velká písmena (toupper).
     *       MRS knihovna zachovává originál - validátor povoluje obojí.
     */
    extern en_MZDSK_NAMEVAL mzdsk_validate_83_name ( const char *input,
                                                      en_MZDSK_NAMEVAL_FLAVOR flavor,
                                                      char *out_name,
                                                      char *out_ext,
                                                      char *bad_char );


    /* ===================================================================
     * Verze knihovny
     * =================================================================== */

    /** @brief Verze knihovny mzdsk_global. */
#define MZDSK_GLOBAL_VERSION "1.3.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny mzdsk_global.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* mzdsk_global_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_GLOBAL_H */
