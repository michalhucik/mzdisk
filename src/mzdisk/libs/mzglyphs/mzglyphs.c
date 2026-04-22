/**
 * @file   mzglyphs.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Implementace konverze Sharp MZ video kódů na UTF-8 glyfy.
 *
 * Mapování video kódu na Unicode codepoint:
 *   codepoint = 0xE100 + (charset * 0x100) + video_code
 *
 * Výsledný codepoint je vždy v rozsahu U+E100-E4FF, což je
 * 3-bajtová UTF-8 sekvence (0xEE xx xx).
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

#include "mzglyphs.h"


/**
 * @brief Zakóduje Unicode codepoint do 3-bajtového UTF-8 v bufferu.
 *
 * Codepoint musí být v rozsahu U+0800-U+FFFF (3-bajtová sekvence).
 *
 * @param cp  Unicode codepoint.
 * @param buf Výstupní buffer (minimálně 4 bajty).
 */
static void encode_utf8_3byte ( uint32_t cp, char *buf ) {
    buf[0] = (char) ( 0xE0 | ( cp >> 12 ) );
    buf[1] = (char) ( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
    buf[2] = (char) ( 0x80 | ( cp & 0x3F ) );
    buf[3] = '\0';
}


/** @copydoc mzglyphs_to_utf8_buf */
void mzglyphs_to_utf8_buf ( uint8_t video_code, en_MZGLYPHS_CHARSET charset, char *buf ) {
    uint32_t codepoint = 0xE100 + ( (uint32_t) charset * 0x100 ) + video_code;
    encode_utf8_3byte ( codepoint, buf );
}


/** @copydoc mzglyphs_to_utf8 */
const char* mzglyphs_to_utf8 ( uint8_t video_code, en_MZGLYPHS_CHARSET charset ) {
    static char buf[MZGLYPHS_UTF8_BUF_SIZE];
    mzglyphs_to_utf8_buf ( video_code, charset, buf );
    return buf;
}


/** @copydoc mzglyphs_version */
const char* mzglyphs_version ( void ) {
    return MZGLYPHS_VERSION;
}
