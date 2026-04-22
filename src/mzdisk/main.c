/**
 * @file main.c
 * @brief Vstupní bod mzdisk GUI aplikace.
 *
 * Inicializuje lokalizaci a spouští hlavní aplikační smyčku.
 * Veškerá SDL3/OpenGL/ImGui logika je zapouzdřena v app_imgui.cpp,
 * tento soubor zůstává čistým C.
 *
 * Na Windows je aplikace linkovaná jako GUI (-mwindows), takže se
 * při spuštění nezobrazí konzolové okno. Parametr --console alokuje
 * konzoli pro diagnostický výstup (stdout/stderr).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "i18n.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>


/**
 * @brief Alokuje konzoli a přesměruje stdout/stderr.
 *
 * Na Windows (-mwindows) není konzole k dispozici. Tato funkce
 * vytvoří nové konzolové okno a přesměruje standardní streamy,
 * aby printf/fprintf(stderr, ...) fungovaly jako obvykle.
 *
 * @post stdout a stderr zapisují do nově alokované konzole.
 * @note Konzole se automaticky zavře při ukončení procesu.
 */
static void attach_console ( void )
{
    if ( !AllocConsole () )
        return;

    /* Přesměrování stdout a stderr do nové konzole. Pokud freopen
     * selže, stream zůstane původní (neplatný) - setvbuf nad ním by
     * byl UB. Proto explicitně kontrolujeme návratovou hodnotu (audit L-23). */
    FILE *new_stdout = freopen ( "CONOUT$", "w", stdout );
    FILE *new_stderr = freopen ( "CONOUT$", "w", stderr );

    /* vypnout bufferování pro okamžitý výstup - jen nad úspěšně
     * přesměrovanými streamy */
    if ( new_stdout != NULL ) {
        setvbuf ( stdout, NULL, _IONBF, 0 );
    }
    if ( new_stderr != NULL ) {
        setvbuf ( stderr, NULL, _IONBF, 0 );
    }
}
#endif /* _WIN32 */


/**
 * @brief Vstupní bod programu.
 *
 * Zpracuje parametr --console (na Windows alokuje konzolové okno),
 * inicializuje lokalizaci a spustí hlavní aplikační smyčku.
 *
 * @param argc Počet argumentů příkazové řádky.
 * @param argv Pole argumentů příkazové řádky.
 * @return EXIT_SUCCESS při normálním ukončení, EXIT_FAILURE při chybě.
 */
int main ( int argc, char *argv[] )
{
#ifdef _WIN32
    for ( int i = 1; i < argc; i++ ) {
        if ( strcmp ( argv[i], "--console" ) == 0 ) {
            attach_console ();
            break;
        }
    }
#endif

    i18n_init ();
    return mzdisk_app_run ( argc, argv );
}
