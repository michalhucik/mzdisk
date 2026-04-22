/**
 * @file app.h
 * @brief Hlavní aplikační stav a životní cyklus mzdisk GUI.
 *
 * Definuje strukturu aplikačního stavu a poskytuje C API pro inicializaci,
 * spuštění hlavní smyčky a ukončení aplikace. Implementace je v app_imgui.cpp
 * (SDL3 + OpenGL + ImGui backend).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_APP_H
#define MZDISK_APP_H

#ifdef __cplusplus
extern "C" {
#endif


/** @brief Verze mzdisk GUI. */
#define MZDISK_VERSION "0.8.0"

/** @brief Název aplikace. */
#define MZDISK_APP_NAME "mzdisk"

/** @brief Výchozí šířka okna v pixelech. */
#define MZDISK_DEFAULT_WINDOW_WIDTH 1280

/** @brief Výchozí výška okna v pixelech. */
#define MZDISK_DEFAULT_WINDOW_HEIGHT 800


/**
 * @brief Spustí mzdisk GUI aplikaci.
 *
 * Inicializuje SDL3, OpenGL, ImGui (docking + multi-viewport),
 * načte fonty, spustí hlavní smyčku a po jejím ukončení uvolní zdroje.
 *
 * @param argc Počet argumentů z příkazové řádky.
 * @param argv Pole argumentů z příkazové řádky.
 * @return EXIT_SUCCESS při normálním ukončení, EXIT_FAILURE při chybě.
 *
 * @pre SDL3 a OpenGL musí být dostupné v systému.
 * @post Všechny SDL/OpenGL/ImGui zdroje jsou uvolněny.
 */
extern int mzdisk_app_run ( int argc, char *argv[] );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_APP_H */
