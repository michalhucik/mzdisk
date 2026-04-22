/**
 * @file config.h
 * @brief Konfigurace aplikace mzdisk - ukládání/načítání z mzdisk.ini.
 *
 * Spravuje persistentní nastavení aplikace v INI souboru.
 * Ukládá poslední použité adresáře, viditelnost tabů, seznam
 * posledních otevřených souborů a nastavení jazyka, fontu a tématu.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_CONFIG_H
#define MZDISK_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>


/** @brief Maximální délka cesty v konfiguraci. */
#define MZDISK_CONFIG_PATH_MAX 1024

/** @brief Název konfiguračního souboru. */
#define MZDISK_CONFIG_FILENAME "mzdisk.ini"


/**
 * @brief Režim konverze Sharp MZ ASCII jmen pro zobrazení v GUI.
 *
 * Určuje, jakou konverzní funkci použít pro zobrazení bootstrap jména
 * a FSMZ souborových jmen. UTF-8 režimy zobrazují speciální znaky
 * pomocí Unicode ekvivalentů, ASCII režimy je mapují na nejbližší
 * ASCII znak.
 */
typedef enum en_MZDSK_NAME_CHARSET {
    MZDSK_NAME_CHARSET_EU_UTF8 = 0,    /**< SharpMZ-EU -> UTF-8 (výchozí pro GUI). */
    MZDSK_NAME_CHARSET_JP_UTF8,        /**< SharpMZ-JP -> UTF-8. */
    MZDSK_NAME_CHARSET_EU_ASCII,       /**< SharpMZ-EU -> ASCII. */
    MZDSK_NAME_CHARSET_JP_ASCII,       /**< SharpMZ-JP -> ASCII. */
    MZDSK_NAME_CHARSET_EU_CG1,        /**< SharpMZ-EU -> MZ-CG1 (pixel-art glyfy, EU sada 1). */
    MZDSK_NAME_CHARSET_EU_CG2,        /**< SharpMZ-EU -> MZ-CG2 (pixel-art glyfy, EU sada 2). */
    MZDSK_NAME_CHARSET_JP_CG1,        /**< SharpMZ-JP -> MZ-CG1 (pixel-art glyfy, JP sada 1). */
    MZDSK_NAME_CHARSET_JP_CG2,        /**< SharpMZ-JP -> MZ-CG2 (pixel-art glyfy, JP sada 2). */
    MZDSK_NAME_CHARSET_COUNT           /**< Počet režimů (sentinel). */
} en_MZDSK_NAME_CHARSET;


/**
 * @brief Identifikátory tabů v hlavním tab baru.
 *
 * Používá se pro indexování pole tab_visible v konfiguraci.
 * FS Directory a FS Maintenance jsou sdílené pro všechny typy FS
 * (FSMZ/CP/M/MRS) - viditelnost FS tabů závisí na detekovaném FS
 * AND na tomto flagu.
 */
typedef enum en_MZDISK_TAB {
    MZDISK_TAB_INFO = 0,            /**< Informační panel. */
    MZDISK_TAB_GEOMETRY,             /**< Vizuální mapa geometrie. */
    MZDISK_TAB_GEOMETRY_EDIT,        /**< Editace geometrie. */
    MZDISK_TAB_BOOT_SECTOR,          /**< Boot sector viewer. */
    MZDISK_TAB_BLOCK_MAP,            /**< Bloková/sektorová mapa. */
    MZDISK_TAB_HEXDUMP,              /**< Hexdump viewer/editor. */
    MZDISK_TAB_FS_DIR,               /**< FS directory (FSMZ/CP/M/MRS). */
    MZDISK_TAB_FS_MAINT,             /**< FS maintenance (FSMZ/CP/M/MRS). */
    MZDISK_TAB_COUNT                 /**< Počet tabů (sentinel). */
} en_MZDISK_TAB;


/**
 * @brief Režim řešení duplicitních jmen při exportu souborů v GUI.
 *
 * Globální preference pro všechny exportní operace (Get, Export All)
 * ve všech FS panelech (FSMZ, CP/M, MRS).
 */
typedef enum en_MZDSK_EXPORT_DUP_MODE {
    MZDSK_EXPORT_DUP_RENAME    = 0,    /**< Automaticky přejmenovat s ~N suffixem (výchozí). */
    MZDSK_EXPORT_DUP_OVERWRITE = 1,    /**< Přepsat existující soubor bez dotazu. */
    MZDSK_EXPORT_DUP_SKIP      = 2,    /**< Přeskočit existující soubor. */
    MZDSK_EXPORT_DUP_ASK       = 3,    /**< Zobrazit dialog s dotazem pro každý konflikt. */
    MZDSK_EXPORT_DUP_COUNT              /**< Počet režimů (sentinel). */
} en_MZDSK_EXPORT_DUP_MODE;


/** @brief Maximální počet položek v seznamu posledních otevřených souborů. */
#define MZDISK_CONFIG_RECENT_MAX 10

/** @brief Maximální délka řetězce jazyka v konfiguraci. */
#define MZDISK_CONFIG_LANG_MAX 16

/** @brief Výchozí velikost fontu v pixelech. */
#define MZDISK_CONFIG_DEFAULT_FONT_SIZE 28


/**
 * @brief Konfigurace aplikace.
 *
 * Obsahuje všechna persistentní nastavení. Naplní se voláním
 * mzdisk_config_load(), uloží voláním mzdisk_config_save().
 */
typedef struct st_MZDISK_CONFIG {
    char last_open_dir[MZDISK_CONFIG_PATH_MAX];     /**< Poslední adresář pro Open Disk. */
    char last_get_dir[MZDISK_CONFIG_PATH_MAX];      /**< Poslední adresář pro Get (export MZF). */
    char last_put_dir[MZDISK_CONFIG_PATH_MAX];      /**< Poslední adresář pro Put (import MZF). */
    char last_create_dir[MZDISK_CONFIG_PATH_MAX];   /**< Poslední adresář pro Create New Disk. */
    bool tab_visible[MZDISK_TAB_COUNT];              /**< Viditelnost tabů v hlavním tab baru. */

    /* poslední otevřené soubory (MRU) */
    char recent_files[MZDISK_CONFIG_RECENT_MAX][MZDISK_CONFIG_PATH_MAX]; /**< MRU seznam cest. */
    int recent_count;                                /**< Počet platných položek v recent_files. */

    /* nastavení (Settings okno) */
    char language[MZDISK_CONFIG_LANG_MAX];           /**< Jazyk: "auto", "en", "cs". */
    int font_family_idx;                             /**< Index fontové rodiny (0..3). */
    int font_size;                                   /**< Velikost fontu v pixelech. */
    int theme_idx;                                   /**< Index tématu: 0=Dark Blue, 1=Dark, 2=Light. */

    /* kódování Sharp MZ ASCII jmen */
    int boot_name_charset;                           /**< Režim konverze bootstrap jména (en_MZDSK_NAME_CHARSET). */
    int fsmz_name_charset;                           /**< Režim konverze FSMZ souborových jmen (en_MZDSK_NAME_CHARSET). */

    /* export */
    int export_dup_mode;                             /**< Režim řešení duplicit při exportu (en_MZDSK_EXPORT_DUP_MODE). */

    /* drag&drop */
    int dnd_dup_mode;                                /**< Režim řešení duplicit při DnD mezi sessions (en_MZDSK_EXPORT_DUP_MODE). */

    /* layout */
    float create_splitter_w;                         /**< Šířka levého panelu v Create okně (splitter pozice v px). */
    float filebrowser_splitter_w;                    /**< Šířka Places panelu ve FileBrowseru (splitter pozice v px). */

    /* multi-window */
    bool open_in_new_window;                         /**< Pokud true, File > Open Disk / Recent otevře do nového detached okna místo nahrazení disku. */
} st_MZDISK_CONFIG;


/**
 * @brief Inicializuje konfiguraci na výchozí hodnoty.
 *
 * Všechny cesty se nastaví na ".".
 *
 * @param cfg Ukazatel na konfiguraci.
 */
extern void mzdisk_config_init ( st_MZDISK_CONFIG *cfg );


/**
 * @brief Načte konfiguraci z mzdisk.ini.
 *
 * Pokud soubor neexistuje nebo je nečitelný, ponechá výchozí hodnoty.
 *
 * @param cfg Ukazatel na konfiguraci (musí být předem inicializovaná).
 * @return true při úspěšném načtení, false pokud soubor neexistuje.
 */
extern bool mzdisk_config_load ( st_MZDISK_CONFIG *cfg );


/**
 * @brief Uloží konfiguraci do mzdisk.ini.
 *
 * @param cfg Ukazatel na konfiguraci.
 * @return true při úspěšném uložení, false při chybě zápisu.
 */
extern bool mzdisk_config_save ( const st_MZDISK_CONFIG *cfg );


/**
 * @brief Přidá cestu na začátek seznamu posledních otevřených souborů.
 *
 * Pokud cesta v seznamu už existuje, přesune ji na první pozici.
 * Pokud je seznam plný, odstraní nejstarší položku.
 *
 * @param cfg Ukazatel na konfiguraci.
 * @param filepath Absolutní cesta k DSK souboru.
 *
 * @pre cfg != NULL, filepath != NULL a neprázdný.
 * @post filepath je na indexu 0, recent_count <= MZDISK_CONFIG_RECENT_MAX.
 */
extern void mzdisk_config_add_recent ( st_MZDISK_CONFIG *cfg, const char *filepath );


/**
 * @brief Vymaže seznam posledních otevřených souborů.
 *
 * @param cfg Ukazatel na konfiguraci.
 *
 * @pre cfg != NULL.
 * @post recent_count == 0.
 */
extern void mzdisk_config_clear_recent ( st_MZDISK_CONFIG *cfg );


/**
 * @brief Stav sledování externí změny konfiguračního souboru.
 *
 * Drží podpis (mtime + size) posledního známého stavu `mzdisk.ini` a
 * timestamp posledního pollu pro throttling. Používá se pro detekci
 * změn configu z jiné běžící instance GUI mzdisk: watch si po každém
 * vlastním uložení zapamatuje novou signaturu, a v hlavní render
 * smyčce se periodicky ověřuje, zda se signatura na disku neliší.
 *
 * Invariant: pokud je `last_size == -1`, ostatní `last_*` fieldy
 * jsou nedefinované a poll je vyhodnotí jako "soubor neexistuje".
 */
typedef struct st_MZDISK_CONFIG_WATCH {
    long long last_mtime_sec;     /**< st_mtime v sekundách (0 = neznámý). */
    long long last_mtime_nsec;    /**< st_mtim.tv_nsec na FS s podporou (0 jinak). */
    long long last_size;          /**< Velikost souboru v bajtech (-1 = neznámý/neexistuje). */
    unsigned  last_check_ms;      /**< SDL_GetTicks() hodnota posledního pollu. */
    unsigned  poll_interval_ms;   /**< Throttling interval mezi reálnými stat() voláními (výchozí 500). */
    char      path[MZDISK_CONFIG_PATH_MAX]; /**< Cesta k INI souboru. */
} st_MZDISK_CONFIG_WATCH;


/**
 * @brief Inicializuje watch nad zadaným INI souborem.
 *
 * Zkopíruje cestu, nastaví `poll_interval_ms = 500` a ostatní pole
 * označí jako neznámé (`last_size = -1`). Po inicializaci je obvyklé
 * ihned zavolat `mzdisk_config_watch_mark_saved()`, který zachytí
 * aktuální signaturu souboru jako baseline.
 *
 * @param w        Ukazatel na watch strukturu. Nesmí být NULL.
 * @param ini_path Cesta k INI souboru (typicky MZDISK_CONFIG_FILENAME).
 *                 Nesmí být NULL. Při přetečení bufferu se oříže.
 *
 * @pre w != NULL, ini_path != NULL.
 * @post w->path obsahuje null-terminated cestu, w->last_size == -1.
 */
extern void mzdisk_config_watch_init ( st_MZDISK_CONFIG_WATCH *w, const char *ini_path );


/**
 * @brief Zachytí aktuální signaturu INI souboru jako baseline.
 *
 * Volat po každém úspěšném `mzdisk_config_save()`, aby vlastní zápis
 * nevyvolal v `mzdisk_config_watch_poll()` false-positive detekci
 * externí změny. Pokud soubor neexistuje, baseline se označí jako
 * "neznámý" (`last_size = -1`) a následující poll vrátí false.
 *
 * @param w Ukazatel na watch strukturu. NULL se tiše ignoruje.
 */
extern void mzdisk_config_watch_mark_saved ( st_MZDISK_CONFIG_WATCH *w );


/**
 * @brief Provede kontrolu externí změny INI souboru (throttlováno).
 *
 * Pokud od minulého skutečného `stat()` voláním uplynulo méně než
 * `w->poll_interval_ms`, vrátí ihned `false` bez syscallu. Jinak
 * provede `stat()` a porovná mtime+size s baseline. Při detekci
 * změny aktualizuje baseline na novou signaturu a vrátí `true` -
 * stejná změna se v dalším pollu už znovu nehlásí.
 *
 * Neexistující soubor vrátí `false` (baseline zůstane -1).
 *
 * @param w      Ukazatel na watch strukturu. NULL vrátí false.
 * @param now_ms Aktuální čas v ms (typicky z `SDL_GetTicks()`).
 *
 * @return true  Soubor byl externě změněn od posledního `mark_saved`
 *               (volající musí provést reload configu + apply).
 * @return false Buď throttle aktivní, nebo soubor beze změny,
 *               nebo neexistuje, nebo w == NULL.
 */
extern bool mzdisk_config_watch_poll ( st_MZDISK_CONFIG_WATCH *w, unsigned now_ms );


/**
 * @brief Vyřeší duplicitní jméno výstupního souboru při exportu.
 *
 * Pokud soubor na zadané cestě existuje, podle režimu dup_mode buď
 * přepíše cestu na unikátní variantu s ~N suffixem (RENAME),
 * ponechá cestu beze změny (OVERWRITE), nebo vrátí příznak přeskočení (SKIP).
 * Režim ASK je zpracováván volajícím před voláním této funkce.
 *
 * @param[in,out] path       Cesta k výstupnímu souboru. Při RENAME se přepíše
 *                           na unikátní cestu. Buffer musí mít alespoň path_size bajtů.
 * @param[in]     path_size  Velikost bufferu path.
 * @param[in]     dup_mode   Režim řešení duplicit (en_MZDSK_EXPORT_DUP_MODE).
 *                           Režim ASK nesmí být předán - volající ho musí zpracovat sám.
 *
 * @return 0 = soubor se má zapsat (cesta v path je připravena).
 * @return 1 = soubor se má přeskočit (SKIP a soubor existuje).
 * @return -1 = chyba (buffer overflow při RENAME).
 *
 * @pre path != NULL, path_size > 0, dup_mode != MZDSK_EXPORT_DUP_ASK.
 */
extern int mzdisk_config_resolve_export_dup ( char *path, int path_size, int dup_mode );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_CONFIG_H */
