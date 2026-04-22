/**
 * @file   mzdsk_global.c
 * @brief  Implementace globálních funkcí pro práci s DSK diskovými obrazy Sharp MZ.
 *
 * Otevírání/zavírání DSK obrazů, čtení/zápis sektorů s automatickou
 * detekcí inverze dat, pomocné řetězcové/paměťové funkce a výchozí
 * implementace sector_info callbacku.
 *
 * Port z fstool fs_global.c/fs_global_tools.c - pouze DSK podpora (bez FDC).
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mzdsk_global.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"


/** @brief Velikost cache bufferu pro sektorová data (maximální velikost sektoru). */
#define MZDSK_CACHE_SIZE    1024


/* ========================================================================
 * Chybové zprávy
 * ======================================================================== */


/**
 * @brief Vrátí textový popis chyby pro daný návratový kód.
 *
 * @param res Návratový kód operace (en_MZDSK_RES).
 * @return Ukazatel na statický řetězec s anglickým popisem chyby.
 *         Nikdy nevrací NULL.
 */
const char* mzdsk_get_error ( en_MZDSK_RES res ) {

    static const char *result_txt[] = {
        "Write protected",
        "Driver error",
        "OK",
        "File not found",
        "File exists",
        "Bad name",
        "Disc full (size)",
        "No space (fragmentation)",
        "Directory is full",
        "Invalid parameter",
        "Buffer too small",
        "Format error",
        "File is locked",
        "Unknown error"
    };

    if ( ( res < MZDSK_RES_WRITE_PROTECTED ) || ( res > MZDSK_RES_UNKNOWN_ERROR ) ) res = MZDSK_RES_UNKNOWN_ERROR;

    return result_txt[res + 2];
}


/* ========================================================================
 * Inverze dat
 * ======================================================================== */


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
void mzdsk_invert_data ( uint8_t *buffer, uint16_t size ) {
    uint16_t i;
    for ( i = 0; i < size; i++ ) {
        buffer[i] ^= 0xff;
    }
}


/* ========================================================================
 * Výchozí sector info callback
 * ======================================================================== */


/**
 * @brief Výchozí implementace callbacku pro zjištění informací o sektoru.
 *
 * Zjistí pravidlo pro danou stopu z tracks_rules a rozhodne o typu média:
 * - Pokud stopa má 16 sektorů a velikost sektoru 256 B -> FSMZ formát
 *   (MZDSK_MEDIUM_INVERTED | ssize)
 * - Jinak -> normální formát (MZDSK_MEDIUM_NORMAL | ssize)
 *
 * @param track Absolutní číslo stopy.
 * @param sector ID sektoru (nepoužívá se v této implementaci).
 * @param user_data Ukazatel na st_MZDSK_DISC (musí být platný).
 *
 * @return Kombinace en_MZDSK_MEDIUM | en_DSK_SECTOR_SIZE.
 * @return MZDSK_MEDIUM_NORMAL | DSK_SECTOR_SIZE_256 pokud pravidlo nenalezeno.
 */
uint8_t mzdsk_sector_info_cb ( uint16_t track, uint16_t sector, void *user_data ) {

    (void) sector;

    st_MZDSK_DISC *disc = (st_MZDSK_DISC *) user_data;

    st_DSK_TOOLS_TRACK_RULE_INFO *rule = dsk_tools_get_rule_for_track ( disc->tracks_rules, (uint8_t) track );

    if ( rule == NULL ) {
        return ( MZDSK_MEDIUM_NORMAL | DSK_SECTOR_SIZE_256 );
    }

    /* FSMZ formát: 16 sektorů po 256 bajtech -> invertovaná data */
    if ( ( rule->sectors == 16 ) && ( rule->ssize == DSK_SECTOR_SIZE_256 ) ) {
        return ( MZDSK_MEDIUM_INVERTED | rule->ssize );
    }

    return ( MZDSK_MEDIUM_NORMAL | rule->ssize );
}


/* ========================================================================
 * Otevírání a zavírání disku
 * ======================================================================== */


/**
 * @brief Otevře DSK diskový obraz ze souboru.
 *
 * Provede kompletní inicializaci struktury st_MZDSK_DISC:
 * 1. Alokuje a inicializuje handler a driver pro generic_driver
 * 2. Otevře soubor přes generic_driver_open_file() v požadovaném režimu
 * 3. Ověří DSK formát přes dsk_tools_check_dsk_fileinfo()
 * 4. Analyzuje geometrii a získá pravidla stop
 * 5. Identifikuje formát diskety
 * 6. Nastaví výchozí sector_info_cb
 * 7. Alokuje cache buffer (1024 bajtů)
 *
 * Funkce NIKDY nemodifikuje otevíraný soubor. Read-only otevření (RO)
 * respektuje čistě read-only vrstvu generic_driveru (`fopen("rb")`) -
 * žádný zápis přes tento handler nemůže proběhnout. Poškozené DSK
 * obrazy (chybná tsize, trailing data, nadlimitní tracks count atd.)
 * se automaticky NEOPRAVUJÍ; pokud je hlavička natolik rozbitá, že
 * nelze získat pravidla stop, open vrátí MZDSK_RES_DSK_ERROR a volající
 * má šanci uživateli doporučit `mzdsk-dsk check` / `mzdsk-dsk repair`.
 *
 * Při jakékoliv chybě se všechny dosud alokované zdroje korektně uvolní
 * a struktura disc se vynuluje.
 *
 * @param disc Ukazatel na strukturu disku. Musí být platný (ne NULL).
 * @param filename Cesta k DSK souboru. Musí být platný řetězec (ne NULL).
 *                 Handler drží ukazatel na filename - řetězec musí
 *                 přežít celou dobu životnosti disku.
 * @param open_mode Režim otevření souboru (čtení/zápis/vytvoření).
 *
 * @return MZDSK_RES_OK při úspěchu.
 * @return MZDSK_RES_DSK_ERROR pokud soubor nelze otevřít nebo není platný DSK.
 * @return MZDSK_RES_UNKNOWN_ERROR pokud selže alokace paměti.
 *
 * @post Při úspěchu je disc kompletně inicializovaný.
 * @post Při neúspěchu je disc vynulovaný.
 * @post Obsah DSK souboru zůstává nezměněný - `mzdsk_disc_open()` nikdy
 *       nezapisuje do souboru.
 */
en_MZDSK_RES mzdsk_disc_open ( st_MZDSK_DISC *disc, char *filename, en_FILE_DRIVER_OPEN_MODE open_mode ) {

    /* Vynulujeme strukturu */
    memset ( disc, 0, sizeof ( st_MZDSK_DISC ) );

    /* Alokace handleru */
    st_HANDLER *handler = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    if ( handler == NULL ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Alokace a inicializace driveru */
    st_DRIVER *driver = (st_DRIVER *) calloc ( 1, sizeof ( st_DRIVER ) );
    if ( driver == NULL ) {
        free ( handler );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    generic_driver_file_init ( driver );

    /*
     * Otevřeme soubor PŘESNĚ v požadovaném režimu. Read-only znamená
     * read-only - knihovna nesmí za žádných okolností modifikovat soubor.
     *
     * Audit M-5: po přiřazení disc->handler = handler může volající
     * mzdsk_disc_close() korektně uklidit i při selhání pozdějších
     * kroků (tracks_rules NULL check, cache NULL check). Proto
     * disc->handler nastavíme IHNED jakmile máme alokovaný handler,
     * aby se error cesty mohly unifikovat přes mzdsk_disc_close.
     */
    if ( generic_driver_open_file ( handler, driver, filename, open_mode ) == NULL ) {
        free ( driver );
        free ( handler );
        return MZDSK_RES_DSK_ERROR;
    }

    disc->handler = handler;

    /* Ověření DSK formátu (pouze čtení). */
    if ( dsk_tools_check_dsk_fileinfo ( handler ) != EXIT_SUCCESS ) {
        mzdsk_disc_close ( disc );
        return MZDSK_RES_DSK_ERROR;
    }

    /*
     * Poškozené DSK obrazy (chybná tsize hodnota, trailing data,
     * nadlimitní tracks count atd.) NEOPRAVUJEME automaticky. Uživatel
     * si případné opravy vyžádá explicitně přes `mzdsk-dsk check`
     * a `mzdsk-dsk repair`.
     *
     * Tento princip chrání uživatelská data: žádné tiché zápisy do
     * souboru během čtecích operací. Pokud je hlavička natolik rozbitá,
     * že nelze určit geometrii, selže dsk_tools_get_tracks_rules()
     * níže a open skončí kontrolovanou chybou.
     */

    /* Získání pravidel geometrie stop */
    disc->tracks_rules = dsk_tools_get_tracks_rules ( handler );
    if ( disc->tracks_rules == NULL ) {
        mzdsk_disc_close ( disc );
        return MZDSK_RES_DSK_ERROR;
    }

    /* Identifikace formátu diskety */
    disc->format = dsk_tools_identformat_from_tracks_rules ( disc->tracks_rules );

    /* Nastavení výchozího sector info callbacku */
    disc->sector_info_cb = mzdsk_sector_info_cb;
    disc->sector_info_cb_data = disc;

    /* Alokace cache bufferu */
    disc->cache = (uint8_t *) malloc ( MZDSK_CACHE_SIZE );
    if ( disc->cache == NULL ) {
        mzdsk_disc_close ( disc );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zavře otevřený DSK diskový obraz a uvolní všechny zdroje.
 *
 * Provede:
 * 1. Uvolní pravidla stop
 * 2. Uvolní cache buffer
 * 3. Zavře handler a uvolní handler i driver
 * 4. Vynuluje celou strukturu disc
 *
 * @param disc Ukazatel na strukturu disku. Může být NULL (NOP).
 *
 * @post Struktura disc je kompletně vynulovaná.
 *
 * @note Bezpečné volat opakovaně - druhé a další volání jsou NOP.
 *
 * @note Audit M-5: bezpečné volat i na **nekompletně inicializovanou**
 *       strukturu po selhání v `mzdsk_disc_open()`. Funkce kontroluje
 *       NULL u každého dynamicky alokovaného pole (tracks_rules, cache,
 *       handler) a selektivně uvolňuje jen to, co bylo alokováno.
 *       Tento invariant je load-bearing pro error-handling cesty v
 *       `mzdsk_disc_open()` - kdyby se přidala nová cesta, která nastaví
 *       `disc->handler` PŘED kompletní inicializací, mzdsk_disc_close
 *       stále korektně uklidí.
 */
void mzdsk_disc_close ( st_MZDSK_DISC *disc ) {

    if ( disc == NULL ) return;

    /* Uvolnění pravidel stop */
    if ( disc->tracks_rules != NULL ) {
        dsk_tools_destroy_track_rules ( disc->tracks_rules );
    }

    /* Uvolnění cache bufferu */
    if ( disc->cache != NULL ) {
        free ( disc->cache );
    }

    /* Zavření a uvolnění handleru + driveru.
     * Audit M-5: tato větev musí zůstat robustní vůči handleru v
     * libovolné fázi otevírání - od "čerstvě alokovaný s calloc"
     * (vše NULL) po "plně otevřený". generic_driver_close je
     * idempotentní a NULL-safe; free(handler) funguje i pokud jen
     * calloc proběhl; driver check funguje i pro handler->driver
     * nastavený přes generic_driver_file_init() bez následného open. */
    if ( disc->handler != NULL ) {
        st_DRIVER *driver = (st_DRIVER *) disc->handler->driver;
        generic_driver_close ( disc->handler );
        free ( disc->handler );
        /* Uvolnit driver jen pokud byl dynamicky alokován.
           Globální g_memory_driver_realloc se neuvolňuje. */
        if ( driver != NULL
             && driver != &g_memory_driver_realloc
             && driver != &g_memory_driver_static ) {
            free ( driver );
        }
    }

    /* Vynulování celé struktury */
    memset ( disc, 0, sizeof ( st_MZDSK_DISC ) );
}


/**
 * @brief Otevře DSK diskový obraz do paměti (bezpečný režim pro zápis).
 *
 * Načte celý DSK soubor do RAM přes paměťový handler. Všechny následné
 * operace (čtení i zápis) probíhají nad paměťovou kopií. Změny se
 * na disk zapíší až explicitním voláním mzdsk_disc_save().
 *
 * Tento režim je bezpečný pro zápisové operace - při chybě uprostřed
 * operace zůstane originální soubor nedotčený.
 *
 * Open sám o sobě NIKDY neupravuje obsah originálního souboru. Při RO
 * otevření se navíc na handler nastaví read-only flag, takže i kdyby
 * volající omylem zavolal mzdsk_disc_save(), soubor se nepřepíše a
 * funkce vrátí MZDSK_RES_WRITE_PROTECTED.
 *
 * Poškozené DSK obrazy se automaticky NEOPRAVUJÍ. Pokud je hlavička
 * natolik rozbitá, že nelze získat pravidla stop, open vrátí chybu
 * a uživatel má spustit `mzdsk-dsk check` / `mzdsk-dsk repair`.
 *
 * @param disc Ukazatel na strukturu disku. Musí být platný (ne NULL).
 * @param filename Cesta k DSK souboru.
 * @param open_mode Režim otevření (RW nebo RO).
 *
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 *
 * @post Při úspěchu je disc kompletně inicializovaný, pracuje nad pamětí.
 * @post Při neúspěchu je disc vynulovaný.
 * @post Obsah DSK souboru zůstává nezměněný - `mzdsk_disc_open_memory()`
 *       nikdy nezapisuje do souboru. Skutečný zápis provede až explicitní
 *       volání `mzdsk_disc_save()`.
 *
 * @note Volající musí po úspěšných operacích zavolat mzdsk_disc_save()
 *       pro uložení změn na disk a poté mzdsk_disc_close() pro uvolnění.
 */
en_MZDSK_RES mzdsk_disc_open_memory ( st_MZDSK_DISC *disc, char *filename, en_FILE_DRIVER_OPEN_MODE open_mode ) {

    memset ( disc, 0, sizeof ( st_MZDSK_DISC ) );

    /* Alokace handleru */
    st_HANDLER *handler = (st_HANDLER *) calloc ( 1, sizeof ( st_HANDLER ) );
    if ( handler == NULL ) return MZDSK_RES_UNKNOWN_ERROR;

    /* Paměťový driver s realloc podporou (automatické rozšiřování bufferu) */
    st_DRIVER *driver = &g_memory_driver_realloc;

    /* Načtení celého souboru do paměti */
    if ( generic_driver_open_memory_from_file ( handler, driver, filename ) == NULL ) {
        free ( handler );
        return MZDSK_RES_DSK_ERROR;
    }

    disc->handler = handler;

    /*
     * Respektuj požadovaný režim: RO znamená RO, nastavíme ochranu
     * handleru hned po otevření, aby žádná případná chyba v dalších
     * krocích nemohla vést k zápisu přes mzdsk_disc_save().
     */
    if ( open_mode == FILE_DRIVER_OPMODE_RO ) {
        generic_driver_set_handler_readonly_status ( handler, 1 );
    }

    /* Ověření DSK formátu (pouze čtení). */
    if ( dsk_tools_check_dsk_fileinfo ( handler ) != EXIT_SUCCESS ) {
        mzdsk_disc_close ( disc );
        return MZDSK_RES_DSK_ERROR;
    }

    /*
     * Poškozené DSK obrazy NEOPRAVUJEME automaticky - viz
     * mzdsk_disc_open() výše. Uživatel si opravy vyžádá explicitně
     * přes `mzdsk-dsk check` / `mzdsk-dsk repair`.
     */

    /* Uložení jména souboru pro pozdější save */
    disc->filename = filename;

    /* Získání pravidel geometrie stop */
    disc->tracks_rules = dsk_tools_get_tracks_rules ( handler );
    if ( disc->tracks_rules == NULL ) {
        mzdsk_disc_close ( disc );
        return MZDSK_RES_DSK_ERROR;
    }

    /* Identifikace formátu diskety */
    disc->format = dsk_tools_identformat_from_tracks_rules ( disc->tracks_rules );

    /* Nastavení výchozího sector info callbacku */
    disc->sector_info_cb = mzdsk_sector_info_cb;
    disc->sector_info_cb_data = disc;

    /* Alokace cache bufferu */
    disc->cache = (uint8_t *) malloc ( MZDSK_CACHE_SIZE );
    if ( disc->cache == NULL ) {
        mzdsk_disc_close ( disc );
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Uloží paměťový obraz disku zpět do souboru.
 *
 * Zapíše obsah paměťového handleru do souboru, jehož jméno
 * bylo předáno při otevření přes mzdsk_disc_open_memory().
 *
 * @param disc Ukazatel na otevřený disk (paměťový handler). Nesmí být NULL.
 *
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_WRITE_PROTECTED pokud
 *         je disk v RO režimu, jinak chybový kód.
 *
 * @pre Disk musí být otevřen přes mzdsk_disc_open_memory().
 * @pre disc->filename musí ukazovat na platný řetězec.
 */
en_MZDSK_RES mzdsk_disc_save ( st_MZDSK_DISC *disc ) {

    if ( disc == NULL || disc->handler == NULL ) return MZDSK_RES_UNKNOWN_ERROR;
    if ( disc->filename == NULL ) return MZDSK_RES_UNKNOWN_ERROR;

    if ( disc->handler->status & HANDLER_STATUS_READ_ONLY ) {
        return MZDSK_RES_WRITE_PROTECTED;
    }

    if ( EXIT_SUCCESS != generic_driver_save_memory ( disc->handler, disc->filename ) ) {
        return MZDSK_RES_DSK_ERROR;
    }

    return MZDSK_RES_OK;
}


/* ========================================================================
 * Čtení a zápis sektorů
 * ======================================================================== */


/**
 * @brief Přečte sektor z DSK obrazu s automatickou detekcí inverze.
 *
 * Přečte data sektoru do interního cache bufferu disc->cache.
 * Pokud sector_info_cb indikuje invertovaná data (FSMZ formát),
 * automaticky provede bitovou inverzi nad přečtenými daty.
 *
 * @param disc Ukazatel na otevřený disk. Nesmí být NULL.
 * @param track Absolutní číslo stopy.
 * @param sector ID sektoru na stopě.
 *
 * @return MZDSK_RES_OK při úspěchu - data jsou v disc->cache.
 * @return MZDSK_RES_DSK_ERROR při chybě čtení.
 *
 * @pre disc musí být úspěšně otevřený přes mzdsk_disc_open().
 * @post Při úspěchu disc->cache obsahuje data sektoru (případně deinvertovaná).
 */
en_MZDSK_RES mzdsk_disc_read_sector ( st_MZDSK_DISC *disc, uint16_t track, uint16_t sector, void *dma ) {

    st_HANDLER *h = disc->handler;
    st_DRIVER *d = h->driver;
    /* Defenzivní kontrola NULL driveru (audit M-6). Nestává se při
     * standardním volání přes mzdsk_disc_open, ale pokud volající
     * sestaví handler ručně bez driveru, další dereferencování `d`
     * by byl segfault. */
    if ( d == NULL ) return MZDSK_RES_DSK_ERROR;

    int ret = dsk_read_sector ( h, (uint8_t) track, (uint8_t) sector, dma );

    if ( EXIT_SUCCESS == ret ) {
        uint8_t sector_info = disc->sector_info_cb ( track, sector, disc->sector_info_cb_data );

        if ( ( sector_info & 0x80 ) == MZDSK_MEDIUM_INVERTED ) {
            uint16_t sector_size = dsk_decode_sector_size ( sector_info & 0x03 );
            mzdsk_invert_data ( dma, sector_size );
        }
    }

    if ( ( d->err ) || ( h->err ) ) {
        return MZDSK_RES_DSK_ERROR;
    }

    if ( ret != EXIT_SUCCESS ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše sektor do DSK obrazu s automatickou detekcí inverze.
 *
 * Zapíše data z interního cache bufferu disc->cache do DSK obrazu.
 * Pokud sector_info_cb indikuje invertovaná data (FSMZ formát),
 * data se před zápisem invertují a po zápisu se vrátí do původního
 * stavu (druhá inverze).
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
en_MZDSK_RES mzdsk_disc_write_sector ( st_MZDSK_DISC *disc, uint16_t track, uint16_t sector, void *dma ) {

    st_HANDLER *h = disc->handler;
    st_DRIVER *d = h->driver;

    uint8_t sector_info = disc->sector_info_cb ( track, sector, disc->sector_info_cb_data );
    uint16_t sector_size = 0;

    if ( ( sector_info & 0x80 ) == MZDSK_MEDIUM_INVERTED ) {
        sector_size = dsk_decode_sector_size ( sector_info & 0x03 );
        mzdsk_invert_data ( dma, sector_size );
    }

    int ret = dsk_write_sector ( h, (uint8_t) track, (uint8_t) sector, dma );

    if ( ( sector_info & 0x80 ) == MZDSK_MEDIUM_INVERTED ) {
        mzdsk_invert_data ( dma, sector_size );
    }

    if ( d->err ) {
        return MZDSK_RES_DSK_ERROR;
    }

    if ( (en_DSK_ERROR) h->err == DSK_ERROR_WRITE_PROTECTED ) {
        return MZDSK_RES_WRITE_PROTECTED;
    }

    if ( h->err ) {
        return MZDSK_RES_DSK_ERROR;
    }

    if ( ret != EXIT_SUCCESS ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    return MZDSK_RES_OK;
}


/* ========================================================================
 * Pomocné řetězcové a paměťové funkce
 * ======================================================================== */


/**
 * @brief Kopíruje řetězec s omezením délky a vyplňuje zbytek 0x00.
 *
 * Kopíruje znaky ze src do dst, dokud nenarazí na znak `terminator`
 * nebo neuplyne n znaků. Zbývající bajty do n se vyplní `'\0'`
 * (audit M-9: dřívější doc tvrdila "mezerami (0x20)", implementace
 * ale fakticky vyplňuje NUL).
 *
 * @param d Cílový buffer o velikosti alespoň n bajtů. Nesmí být NULL.
 * @param s Zdrojový řetězec. Nesmí být NULL.
 * @param n Maximální počet zpracovaných znaků.
 * @param terminator Znak ukončující kopii ze src (např. 0x00, 0x0d).
 *
 * @post dst obsahuje bytes zkopírované ze src (před terminátorem)
 *       a zbytek do n vyplněný `'\0'`. Buffer NENÍ explicitně
 *       NUL-terminovaný za n - volající musí velikost n+1 zajistit
 *       sám, pokud potřebuje C-string.
 */
uint8_t* mzdsk_strncpy ( uint8_t *d, const uint8_t *s, size_t n, uint8_t terminator ) {

    uint8_t *d1 = d;

    while ( n && ( *s != terminator ) ) {
        n--;
        *d++ = *s++;
    }

    while ( n-- ) {
        *d++ = '\0';
    }

    return d1;
}


/**
 * @brief Porovná dva řetězce ve formátu Sharp MZ.
 *
 * Konec řetězce je definován jakýmkoliv znakem menším než 0x20
 * (typicky 0x00 nebo 0x0d). Implementace má navíc horní mez 256
 * znaků, aby na poškozeném disku bez terminátoru nedošlo k OOB
 * čtení mimo volající buffer.
 *
 * @param asrc První řetězec.
 * @param adst Druhý řetězec.
 * @return 0 pokud jsou řetězce shodné (včetně případu, kdy oba
 *         skončí terminátorem < 0x20), nenulová pokud se liší nebo
 *         se v limitu nenašel terminátor (indikace nekonzistence).
 */
int mzdsk_mzstrcmp ( const uint8_t *asrc, const uint8_t *adst ) {
    int ret = 0;
    unsigned i = 0;

    while ( i < 256 && !( ret = *(uint8_t *) asrc - *(uint8_t *) adst ) && ( *adst >= 0x20 ) ) {
        ++asrc, ++adst, ++i;
    }

    /* Pokud jsme narazili na dolní mez (< 0x20) v obou řetězcích současně, shoda. */
    if ( i < 256 && ( *(uint8_t *) asrc < 0x20 ) && *(uint8_t *) adst < 0x20 ) return 0;

    /* Pokud se `ret` neuložil v cyklu (např. jsme vyčerpali 256 iterací
     * bez nalezení terminátoru v `adst`), vrátíme nenulu jako signál,
     * že řetězce nejsou bezpečně porovnatelné. */
    return ret != 0 ? ret : 1;
}


/**
 * @brief Porovná dva bloky paměti.
 *
 * @param a První blok.
 * @param b Druhý blok.
 * @param size Počet bajtů k porovnání.
 * @return 0 pokud jsou shodné, nenulová pokud se liší.
 */
int8_t mzdsk_memcmp ( const uint8_t *a, const uint8_t *b, size_t size ) {

    if ( !size ) return ( 0 );

    while ( --size && ( *a == *b ) ) {
        a++;
        b++;
    }

    if ( *a == *b ) return ( 0 );

    return ( *a - *b );
}


/* ========================================================================
 * Striktní validace 8.3 jména souboru
 * ======================================================================== */


/**
 * @brief Zakázané interpunkční znaky pro 8.3 jméno.
 *
 * Sada odpovídá CP/M BDOS konvenci plus unixovým path separátorům.
 * Řídicí znaky (0x00-0x1F, 0x7F) a mezera (0x20) se kontrolují zvlášť,
 * aby se daly rozlišit v budoucí diagnostice.
 */
static const char NAME_FORBIDDEN_PUNCT[] = "<>,;:=?*[]|/";


/**
 * @brief Otestuje, zda je znak pro 8.3 jméno zakázaný.
 *
 * Vrací 1 pokud je znak:
 *   - řídicí (< 0x20 nebo == 0x7F),
 *   - mezera (0x20),
 *   - některý z NAME_FORBIDDEN_PUNCT.
 *
 * Tečka '.' je zpracovaná volajícím jako separátor - zde vrací 1 jen
 * pokud ji volající předá po rozdělení.
 *
 * @param[in] c Testovaný znak (interpretovaný jako uint8_t).
 * @return 1 pokud je znak zakázaný, 0 pokud je povolený.
 */
static int name_char_is_forbidden ( unsigned char c ) {
    if ( c < 0x20 || c == 0x7F ) return 1;
    if ( c == 0x20 ) return 1;
    if ( c == '.' ) return 1;
    for ( const char *p = NAME_FORBIDDEN_PUNCT; *p != '\0'; p++ ) {
        if ( (unsigned char) *p == c ) return 1;
    }
    return 0;
}


en_MZDSK_NAMEVAL mzdsk_validate_83_name ( const char *input,
                                           en_MZDSK_NAMEVAL_FLAVOR flavor,
                                           char *out_name,
                                           char *out_ext,
                                           char *bad_char ) {
    /* flavor je aktuálně informativní - obě varianty používají stejnou
       forbidden sadu. Parametr ponechán pro budoucí rozlišení. */
    (void) flavor;

    if ( input == NULL || input[0] == '\0' ) return MZDSK_NAMEVAL_EMPTY;

    /* Najdeme první tečku - ta je separátor name/ext. */
    const char *dot = strchr ( input, '.' );
    size_t name_len;
    const char *ext_src;
    size_t ext_len;

    if ( dot != NULL ) {
        name_len = (size_t) ( dot - input );
        ext_src  = dot + 1;
        ext_len  = strlen ( ext_src );
    } else {
        name_len = strlen ( input );
        ext_src  = NULL;
        ext_len  = 0;
    }

    /* Prázdné jméno ("." nebo ".EXT") je neplatné. */
    if ( name_len == 0 ) return MZDSK_NAMEVAL_EMPTY;

    /* Délkové limity - kontrolujeme dřív, než hledáme zakázané znaky,
       aby chybová hláška preferovala "too long" před "bad char" pro
       případy jako "TOOLONGN*ME.EXT". */
    if ( name_len > 8 ) return MZDSK_NAMEVAL_NAME_TOO_LONG;
    if ( ext_len > 3 ) return MZDSK_NAMEVAL_EXT_TOO_LONG;

    /* Kontrola zakázaných znaků v jméně. */
    for ( size_t i = 0; i < name_len; i++ ) {
        unsigned char c = (unsigned char) input[i];
        if ( name_char_is_forbidden ( c ) ) {
            if ( bad_char != NULL ) *bad_char = (char) c;
            return MZDSK_NAMEVAL_BAD_CHAR;
        }
    }

    /* Kontrola zakázaných znaků v příponě. */
    for ( size_t i = 0; i < ext_len; i++ ) {
        unsigned char c = (unsigned char) ext_src[i];
        if ( name_char_is_forbidden ( c ) ) {
            if ( bad_char != NULL ) *bad_char = (char) c;
            return MZDSK_NAMEVAL_BAD_CHAR;
        }
    }

    /* Vyplnění výstupních bufferů. */
    if ( out_name != NULL ) {
        memcpy ( out_name, input, name_len );
        out_name[name_len] = '\0';
    }
    if ( out_ext != NULL ) {
        if ( ext_src != NULL ) {
            memcpy ( out_ext, ext_src, ext_len );
        }
        out_ext[ext_len] = '\0';
    }

    return MZDSK_NAMEVAL_OK;
}


/**
 * @brief Vrátí řetězec s verzí knihovny mzdsk_global.
 * @return Statický řetězec s verzí (např. "1.0.0").
 */
const char* mzdsk_global_version ( void ) {
    return MZDSK_GLOBAL_VERSION;
}
