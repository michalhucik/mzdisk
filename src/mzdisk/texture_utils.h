/**
 * @file texture_utils.h
 * @brief API pro načítání PNG obrazků do OpenGL textur.
 *
 * Slouží primárně pro zobrazení malých ikon (vlajky apod.) v ImGui.
 * Textury jsou načítány lazy - při prvním požadavku.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef MZDISK_TEXTURE_UTILS_H
#define MZDISK_TEXTURE_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/** @brief Neprůhledný typ pro OpenGL texturu (interně GLuint). */
typedef unsigned int mzdisk_texture_t;

/** @brief Neplatná hodnota textury. */
#define MZDISK_TEXTURE_INVALID 0


/**
 * @brief Načte PNG soubor do OpenGL textury.
 *
 * Používá stb_image pro dekódování a glGenTextures/glTexImage2D
 * pro upload do GPU. Textura má LINEAR filtrování (plynulé škálování
 * při různých velikostech UI) a CLAMP_TO_EDGE wrapping.
 *
 * @param filepath Cesta k PNG souboru.
 * @param[out] out_width Šířka obrazku v pixelech (může být NULL).
 * @param[out] out_height Výška obrazku v pixelech (může být NULL).
 * @return ID OpenGL textury, nebo MZDISK_TEXTURE_INVALID při chybě.
 *
 * @pre filepath != NULL.
 * @post Vrácená textura je platný OpenGL objekt (volající ji musí uvolnit).
 */
extern mzdisk_texture_t mzdisk_texture_load_png ( const char *filepath, int *out_width, int *out_height );


/**
 * @brief Uvolní OpenGL texturu.
 *
 * @param tex ID textury k uvolnění. MZDISK_TEXTURE_INVALID je bezpečně ignorováno.
 */
extern void mzdisk_texture_free ( mzdisk_texture_t tex );


#ifdef __cplusplus
}
#endif

#endif /* MZDISK_TEXTURE_UTILS_H */
