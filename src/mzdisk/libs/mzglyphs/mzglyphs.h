/**
 * @file   mzglyphs.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Konverze Sharp MZ video kódů na UTF-8 glyfy z fontu mzglyphs.ttf.
 *
 * Knihovna převádí Sharp MZ video kódy (0x00-0xFF) na UTF-8 řetězce
 * odkazující do Unicode Private Use Area, kde jsou namapovány
 * znakové sady fontu mzglyphs.ttf:
 *
 * - U+E100-E1FF: EU sada 1
 * - U+E200-E2FF: EU sada 2
 * - U+E300-E3FF: JP sada 1
 * - U+E400-E4FF: JP sada 2
 *
 * Výsledné UTF-8 kódy jsou vždy 3 bajty (rozsah U+E100-E4FF).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MZGLYPHS_H
#define MZGLYPHS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>


/** @brief Verze knihovny mzglyphs. */
#define MZGLYPHS_VERSION "1.0.0"

/**
 * @brief Vrátí řetězec s verzí knihovny mzglyphs.
 * @return Statický řetězec s verzí (např. "1.0.0").
 */
extern const char* mzglyphs_version ( void );


/**
 * @brief Znaková sada fontu mzglyphs.ttf.
 *
 * Každá sada pokrývá 256 video kódů (0x00-0xFF) namapovaných
 * do Unicode Private Use Area.
 */
typedef enum en_MZGLYPHS_CHARSET {
    MZGLYPHS_EU1 = 0,  /**< EU sada 1 (U+E100-E1FF). */
    MZGLYPHS_EU2,      /**< EU sada 2 (U+E200-E2FF). */
    MZGLYPHS_JP1,      /**< JP sada 1 (U+E300-E3FF). */
    MZGLYPHS_JP2,      /**< JP sada 2 (U+E400-E4FF). */
} en_MZGLYPHS_CHARSET;


/** @brief Minimální velikost bufferu pro mzglyphs_to_utf8_buf() (3 B UTF-8 + terminátor). */
#define MZGLYPHS_UTF8_BUF_SIZE 4


/**
 * @brief Převede Sharp MZ video kód na UTF-8 glyf z fontu mzglyphs.ttf.
 *
 * Vrací ukazatel na interní statický buffer s null-terminated UTF-8
 * řetězcem (3 bajty + terminátor). Buffer je platný do dalšího volání
 * této funkce.
 *
 * @param video_code Sharp MZ video kód (0x00-0xFF).
 * @param charset    Znaková sada fontu.
 * @return Ukazatel na statický null-terminated UTF-8 řetězec. Nikdy nevrací NULL.
 *
 * @warning Není thread-safe (sdílený statický buffer).
 */
extern const char* mzglyphs_to_utf8 ( uint8_t video_code, en_MZGLYPHS_CHARSET charset );


/**
 * @brief Převede Sharp MZ video kód na UTF-8 glyf do uživatelského bufferu.
 *
 * Zapíše null-terminated UTF-8 řetězec (3 bajty + terminátor) do
 * zadaného bufferu. Thread-safe alternativa k mzglyphs_to_utf8().
 *
 * @param video_code Sharp MZ video kód (0x00-0xFF).
 * @param charset    Znaková sada fontu.
 * @param[out] buf   Výstupní buffer. Nesmí být NULL.
 *                   Musí mít minimálně MZGLYPHS_UTF8_BUF_SIZE (4) bajtů.
 *
 * @post buf obsahuje null-terminated UTF-8 řetězec (3 bajty + '\0').
 */
extern void mzglyphs_to_utf8_buf ( uint8_t video_code, en_MZGLYPHS_CHARSET charset, char *buf );


#ifdef __cplusplus
}
#endif

#endif /* MZGLYPHS_H */
