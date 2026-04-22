/**
 * @file i18n.h
 * @brief Centrální header pro lokalizaci mzdisk GUI (gettext).
 *
 * Převzato a upraveno z projektu mz800new.
 * Textová doména: "mzdisk".
 *
 * Makra:
 *   _()   - okamžitý překlad (pasivní texty: Text, Tooltip, format stringy)
 *   _L()  - překlad se stabilním ImGui ID (aktivní prvky: Button, MenuItem, ...)
 *   N_()  - značkovací makro pro xgettext (nepřekládá, jen registruje)
 *   C_()  - kontextový překlad
 */

#ifndef MZDISK_I18N_H
#define MZDISK_I18N_H

#include <libintl.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** Hlavní překladové makro - okamžitý překlad. */
#define _(STRING) gettext(STRING)

/** Značkovací makro - string se nepřekládá okamžitě, ale registruje se do .pot. */
#define N_(STRING) STRING

/** Kontextový překlad. */
#define C_(CTX, STR) pgettext(CTX, STR)

/**
 * @brief Emulace pgettext() - kontextový překlad.
 *
 * GNU gettext nemá pgettext() jako standardní makro,
 * proto ho implementujeme pomocí oddělovače EOT (\004).
 */
#ifndef pgettext
#define pgettext(context, msgid) \
    pgettext_impl(context "\004" msgid, msgid)

static inline const char *pgettext_impl ( const char *context_msgid, const char *msgid )
{
    const char *translated = gettext ( context_msgid );
    return ( translated == context_msgid ) ? msgid : translated;
}
#endif

/**
 * @brief Lokalizace se stabilním ImGui ID.
 *
 * Aktivní ImGui prvky (Button, MenuItem, Checkbox, ...) generují interní ID
 * z textového labelu. Lokalizace mění label -> mění ID -> kolize a nestabilita.
 *
 * _L("Cancel") vrátí "Zrušit##Cancel" - zobrazí přeložený text, ale ID zůstane
 * stabilní (odvozené z anglického originálu).
 *
 * Použití:
 *   _L() - aktivní prvky: Button, MenuItem, Checkbox, Begin, BeginMenu, ...
 *   _()  - pasivní texty: Text, Tooltip, format stringy, ...
 *
 * @param text Anglický zdrojový řetězec.
 * @return Řetězec ve formátu "překlad##originál" z kruhového bufferu.
 *
 * @warning Kruhový buffer má `L_BUFFERS_COUNT` slotů (32) - nepředávat
 *          více než 32 _L() volání v jednom výrazu. UI je jednovláknové,
 *          takže to v praxi bez problémů stačí. Pokud byste narazili
 *          na limit, výstup starších volání v témže výrazu bude přepsán.
 *          Hodnota zvýšena z původních 16 (audit L-24) jako obranná
 *          hranice proti kombinaci několika překladů v printf-like stringech.
 */
#define L_BUFFERS_COUNT 32
#define L_BUFFER_SIZE   512

#ifdef __cplusplus
#include <cstdio>
static inline const char* _L ( const char* text ) {
    static char buffers [ L_BUFFERS_COUNT ][ L_BUFFER_SIZE ];
    static int idx = 0;
    char* buf = buffers [ idx & ( L_BUFFERS_COUNT - 1 ) ];
    idx++;
    std::snprintf ( buf, L_BUFFER_SIZE, "%s##%s", _ ( text ), text );
    return buf;
}
#else
#include <stdio.h>
static inline const char* _L ( const char* text ) {
    static char buffers [ L_BUFFERS_COUNT ][ L_BUFFER_SIZE ];
    static int idx = 0;
    char* buf = buffers [ idx & ( L_BUFFERS_COUNT - 1 ) ];
    idx++;
    snprintf ( buf, L_BUFFER_SIZE, "%s##%s", _ ( text ), text );
    return buf;
}
#endif

/** Textová doména pro mzdisk GUI. */
#define MZDISK_I18N_TEXTDOMAIN "mzdisk"

/**
 * @brief Inicializace gettext - volat jednou v main().
 *
 * Nastaví locale na uživatelské výchozí, vyhledá .mo soubory
 * v locale/ (dist) nebo src/locale/ (vývoj), nastaví UTF-8 kódování
 * (ImGui vyžaduje UTF-8; na Windows by gettext jinak vracel cp1250).
 */
static inline void i18n_init ( void )
{
    setlocale ( LC_ALL, "" );
    /* dist/ má .mo v locale/, zdrojový strom v src/locale/ */
    const char *localedir = "locale";
    struct stat st;
    if ( stat ( localedir, &st ) != 0 )
        localedir = "src/locale";
    bindtextdomain ( MZDISK_I18N_TEXTDOMAIN, localedir );
    /* ImGui vyžaduje UTF-8; na Windows by gettext jinak vracel cp1250 */
    bind_textdomain_codeset ( MZDISK_I18N_TEXTDOMAIN, "UTF-8" );
    textdomain ( MZDISK_I18N_TEXTDOMAIN );
}


/**
 * @brief Nastaví locale podle volby jazyka z konfigurace.
 *
 * Volá se po načtení configu a při změně v Settings.
 * Hodnota "auto" ponechá systémový locale, ostatní nastaví
 * odpovídající locale.
 *
 * Na Windows setlocale() nepodporuje POSIX locale stringy (de_DE.UTF-8),
 * proto nastavujeme proměnnou LANGUAGE, kterou GNU gettext (libintl)
 * kontroluje přednostně před systémovým locale.
 *
 * @param lang Řetězec jazyka: "auto", "en", "cs", "de", "es",
 *             "fr", "it", "ja", "nl", "pl", "sk", "uk".
 */
static inline void i18n_set_language ( const char *lang )
{
    if ( !lang || strcmp ( lang, "auto" ) == 0 ) {
        setlocale ( LC_ALL, "" );
#ifdef _WIN32
        _putenv ( "LANGUAGE=" );
#else
        unsetenv ( "LANGUAGE" );
#endif
    } else {
        /* nastavit LANGUAGE pro gettext (funguje na všech platformách).
         * Audit M-42: na MSVC `_putenv` si drží pointer na předaný
         * řetězec bez kopie, takže lokální `env_buf` by po návratu
         * byl invalidován. Na MinGW/GCC (náš build) obvykle kopíruje,
         * ale `_putenv_s` je portable a spolehlivější. */
#ifdef _WIN32
        _putenv_s ( "LANGUAGE", lang );
#else
        setenv ( "LANGUAGE", lang, 1 );
#endif
        /* mapování kódu jazyka na locale string */
        static const struct { const char *code; const char *locale; } map[] = {
            { "en", "en_US.UTF-8" },
            { "cs", "cs_CZ.UTF-8" },
            { "de", "de_DE.UTF-8" },
            { "es", "es_ES.UTF-8" },
            { "fr", "fr_FR.UTF-8" },
            { "it", "it_IT.UTF-8" },
            { "ja", "ja_JP.UTF-8" },
            { "nl", "nl_NL.UTF-8" },
            { "pl", "pl_PL.UTF-8" },
            { "sk", "sk_SK.UTF-8" },
            { "uk", "uk_UA.UTF-8" },
        };
        for ( unsigned i = 0; i < sizeof ( map ) / sizeof ( map[0] ); i++ ) {
            if ( strcmp ( lang, map[i].code ) == 0 ) {
                setlocale ( LC_ALL, map[i].locale );
                return;
            }
        }
        setlocale ( LC_ALL, "" ); /* fallback na systémové */
    }
}

#endif /* MZDISK_I18N_H */
