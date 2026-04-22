/**
 * @file   mz_vcode.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Konverze Sharp MZ ASCII na Sharp MZ video kód (CG-ROM index pro VRAM).
 *
 * Knihovna převádí Sharp MZ ASCII kódy (0x00-0xFF) na odpovídající
 * video kódy (display kódy), které se zapisují do VRAM a indexují
 * znaky v CG-ROM generátoru.
 *
 * Převodní tabulka vychází z rutiny @?ADCN v dolním monitoru MZ-800
 * (adresa 0x0A92-0x0B91 v ROM). Podporovány jsou dvě varianty CG-ROM:
 * - EU (MZ-800) - evropská znaková sada
 * - JP (MZ-1500) - japonská znaková sada
 *
 * Varianty se liší pouze ve dvou pozicích:
 * - MZ-ASCII 0x6C: EU -> video 0xE5, JP -> video 0xE9
 * - MZ-ASCII 0x6D: EU -> video 0xE9, JP -> video 0xEA
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

#ifndef MZ_VCODE_H
#define MZ_VCODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>


/** @brief Verze knihovny mz_vcode. */
#define MZ_VCODE_VERSION "1.0.0"

/**
 * @brief Vrátí řetězec s verzí knihovny mz_vcode.
 * @return Statický řetězec s verzí (např. "1.0.0").
 */
extern const char* mz_vcode_version ( void );

/** @brief Video kód pro "žádný znak" (no key, nezobrazitelný). */
#define MZ_VCODE_NO_KEY 0xF0


/**
 * @brief Varianta CG-ROM znakové sady.
 *
 * Rozlišuje evropskou (MZ-800) a japonskou (MZ-1500) CG-ROM.
 * Převodní tabulky se liší pouze ve dvou pozicích (0x6C, 0x6D).
 */
typedef enum en_MZ_VCODE_CHARSET {
    MZ_VCODE_EU = 0,  /**< MZ-800 - evropská CG-ROM. */
    MZ_VCODE_JP,      /**< MZ-1500 - japonská CG-ROM. */
} en_MZ_VCODE_CHARSET;


/**
 * @brief Převede Sharp MZ ASCII kód na video kód (CG-ROM index).
 *
 * Přímý převod přes tabulku ADCN z ROM MZ-800/MZ-1500.
 * Řídící znaky (< 0x20) se převádí na 0xF0 (MZ_VCODE_NO_KEY)
 * nebo jiné hodnoty podle tabulky.
 *
 * @param ascii_code Sharp MZ ASCII kód (0x00-0xFF).
 * @param charset    Varianta CG-ROM (EU nebo JP).
 * @return Video kód (0x00-0xFF) pro zápis do VRAM.
 */
extern uint8_t mz_vcode_from_ascii ( uint8_t ascii_code, en_MZ_VCODE_CHARSET charset );


/**
 * @brief Převede Sharp MZ ASCII kód na video kód v režimu dump.
 *
 * Replikuje chování příkazu D (dump) monitoru MZ-800/MZ-1500:
 * znaky s kódem < 0x20 jsou nahrazeny tečkou (MZ-ASCII 0x2E)
 * před převodem přes tabulku.
 *
 * @param ascii_code Sharp MZ ASCII kód (0x00-0xFF).
 * @param charset    Varianta CG-ROM (EU nebo JP).
 * @return Video kód (0x00-0xFF) pro zápis do VRAM.
 *         Pro řídící znaky (< 0x20) vrací video kód tečky (0x2E).
 */
extern uint8_t mz_vcode_from_ascii_dump ( uint8_t ascii_code, en_MZ_VCODE_CHARSET charset );


#ifdef __cplusplus
}
#endif

#endif /* MZ_VCODE_H */
