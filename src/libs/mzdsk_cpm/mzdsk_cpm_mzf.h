/**
 * @file   mzdsk_cpm_mzf.h
 * @brief  Konverze mezi CP/M soubory a MZF tape formátem.
 *
 * Implementuje kódování a dekódování CP/M souborů do/z MZF (SharpMZ tape
 * formát). Jeden MZF = 128B hlavička + tělo dat o velikosti `fsize`.
 *
 * MZF hlavička popisuje SharpMZ program tak, jak byl uložen na pásce CMT:
 * obsahuje typ souboru (`ftype`), velikost dat (`fsize`), load adresu (`fstrt`)
 * a exec adresu (`fexec`). MZF lze použít i pro přenos jiných typů souborů
 * než nativní SharpMZ programy, včetně souborů z CP/M.
 *
 * Při exportu z CP/M se MZF hlavička vyplňuje těmito výchozími hodnotami
 * (konvence utility SOKODI CMT.COM):
 * - `ftype` = 0x22 (konvenční hodnota pro CP/M export)
 * - `fstrt` = 0x0100 (standardní CP/M TPA, kam .COM soubory patří)
 * - `fexec` = 0x0100 (stejné jako load, COM soubory mají load == exec)
 * - atributy R/O, SYS, ARC → bit 7 znaků přípony ve `fname`
 *
 * Všechny výchozí hodnoty lze přepsat volitelnými parametry
 * `mzdsk_cpm_mzf_encode_ex()`. Atributy se kódují pouze pokud
 * `ftype == 0x22` (CP/M konvence); pro ostatní typy volající typicky
 * předává `encode_attrs = 0`.
 *
 * Při importu MZF zpět na CP/M disk (`mzdsk_cpm_mzf_decode()`) se z MZF
 * čerpá jen jméno, velikost, data a atributy (bit 7 znaků přípony,
 * jen pro `ftype == 0x22`). Pole `fstrt` a `fexec` se ignorují, protože
 * CP/M koncept load/exec adres v tomto kontextu nepoužívá.
 *
 * @par Layout MZF hlavičky:
 * | Offset | Pole             | Význam                                 |
 * |--------|------------------|----------------------------------------|
 * | 0x00   | ftype            | Typ souboru (8b)                       |
 * | 0x01-08| fname.name[0..7] | CP/M jméno (8 znaků, doplněno mezerami)|
 * | 0x09   | fname.name[8]    | '.' (0x2E) - oddělovač                 |
 * | 0x0A-0C| fname.name[9..11]| CP/M přípona, bit 7 = atributy (jen typ 0x22) |
 * | 0x0D   | fname.name[12]   | CR (0x0D) - terminátor                 |
 * | 0x12-13| fsize            | Velikost datového bloku (LE, max 65535)|
 * | 0x14-15| fstrt            | Load adresa (LE)                       |
 * | 0x16-17| fexec            | Exec adresa (LE)                       |
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


#ifndef MZDSK_CPM_MZF_H
#define MZDSK_CPM_MZF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "libs/mzdsk_global/mzdsk_global.h"
#include "libs/mzf/mzf_tools.h"


    /** @defgroup cpm_mzf_constants Konstanty MZF exportu z CP/M
     *  @{ */

    /** @brief Výchozí MZF typ souboru pro export z CP/M (konvence SOKODI CMT.COM). */
#define MZDSK_CPM_MZF_FTYPE        0x22

    /**
     * @brief Výchozí load/exec adresa pro CP/M program.
     *
     * Standardní adresa TPA (Transient Program Area), kam CP/M 2.2 nahrává
     * `.COM` programy a kde je také spouští (load == exec).
     */
#define MZDSK_CPM_MZF_DEFAULT_ADDR 0x0100

    /**
     * @brief Maximální velikost dat v jednom MZF rámci.
     *
     * Pole `fsize` v MZF hlavičce je 16bitové, takže tělo rámce nemůže
     * překročit 65535 B. Soubory větší se do MZF nevejdou.
     */
#define MZDSK_CPM_MZF_MAX_DATA     0xFFFF

    /** @} */


    /* ================================================================
     * Veřejné API
     * ================================================================ */


    /**
     * @brief Zakóduje CP/M soubor do MZF s výchozími hodnotami exportu.
     *
     * Sestaví jeden MZF rámec (128B hlavička + tělo dat) s:
     * - `ftype` = 0x22 (konvence SOKODI CMT.COM pro CP/M export)
     * - `fstrt` = 0x0100 (standardní CP/M TPA)
     * - `fexec` hodnotou z parametru (default volající obvykle předává 0x0100)
     * - atributy R/O, SYS, ARC zakódované do bitů 7 znaků přípony ve `fname`
     *
     * Výstupní buffer je alokován na heapu a volající zodpovídá
     * za jeho uvolnění přes free().
     *
     * @param[in]  data         Binární data souboru. Může být NULL pro data_size == 0.
     * @param[in]  data_size    Velikost dat v bajtech. Maximum MZDSK_CPM_MZF_MAX_DATA.
     * @param[in]  cpm_name     CP/M jméno souboru (max 8 znaků, null-terminated, uppercase).
     * @param[in]  cpm_ext      CP/M přípona souboru (max 3 znaky, null-terminated, uppercase).
     * @param[in]  cpm_attrs    Souborové atributy (kombinace en_MZDSK_CPM_ATTR).
     * @param[in]  exec_addr    Exec adresa zapsaná do MZF pole `fexec`.
     * @param[out] out_mzf      Výstupní ukazatel na alokovaný MZF buffer. Nesmí být NULL.
     * @param[out] out_mzf_size Výstupní velikost MZF bufferu v bajtech. Nesmí být NULL.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud povinný parametr je NULL nebo data
     *         přesahují MZDSK_CPM_MZF_MAX_DATA.
     * @return MZDSK_RES_UNKNOWN_ERROR při selhání alokace paměti.
     *
     * @pre cpm_name != NULL && cpm_ext != NULL
     * @pre out_mzf != NULL && out_mzf_size != NULL
     * @pre data_size > 0 implikuje data != NULL
     * @post Při úspěchu *out_mzf ukazuje na malloc'd buffer, *out_mzf_size = 128 + data_size.
     */
    extern en_MZDSK_RES mzdsk_cpm_mzf_encode ( const uint8_t *data, uint32_t data_size,
                                                 const char *cpm_name, const char *cpm_ext,
                                                 uint8_t cpm_attrs, uint16_t exec_addr,
                                                 uint8_t **out_mzf, uint32_t *out_mzf_size );


    /**
     * @brief Zakóduje CP/M soubor do MZF s volitelnými parametry hlavičky.
     *
     * Rozšířená verze `mzdsk_cpm_mzf_encode()` umožňující přepsat MZF
     * typ souboru, load adresu (`fstrt`), exec adresu (`fexec`) a zapnout
     * nebo vypnout kódování CP/M atributů do bitů 7 znaků přípony.
     *
     * Výstup je jeden MZF rámec (128B hlavička + tělo dat o velikosti
     * `data_size`), bez ohledu na hodnotu `ftype`. Pole `fsize` obsahuje
     * velikost dat, `fstrt` obsahuje `strt_addr`, `fexec` obsahuje `exec_addr`.
     *
     * @param[in]  data         Binární data souboru. Může být NULL pro data_size == 0.
     * @param[in]  data_size    Velikost dat v bajtech. Maximum MZDSK_CPM_MZF_MAX_DATA.
     * @param[in]  cpm_name     CP/M jméno souboru (max 8 znaků).
     * @param[in]  cpm_ext      CP/M přípona souboru (max 3 znaky).
     * @param[in]  cpm_attrs    Souborové atributy (kombinace en_MZDSK_CPM_ATTR).
     * @param[in]  ftype        MZF typ souboru (0x00-0xFF).
     * @param[in]  exec_addr    Exec adresa - zapíše se do pole `fexec`.
     * @param[in]  strt_addr    Load adresa - zapíše se do pole `fstrt`.
     * @param[in]  encode_attrs Kódovat CP/M atributy do bitů 7 fname[9..11] (true/false).
     *                           Volající by měl nastavit false pro `ftype != 0x22`
     *                           (konvence kódování atributů platí jen pro typ 0x22).
     * @param[out] out_mzf      Výstupní ukazatel na alokovaný MZF buffer.
     * @param[out] out_mzf_size Výstupní velikost MZF bufferu v bajtech.
     *
     * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
     */
    extern en_MZDSK_RES mzdsk_cpm_mzf_encode_ex ( const uint8_t *data, uint32_t data_size,
                                                     const char *cpm_name, const char *cpm_ext,
                                                     uint8_t cpm_attrs, uint8_t ftype,
                                                     uint16_t exec_addr, uint16_t strt_addr,
                                                     int encode_attrs,
                                                     uint8_t **out_mzf, uint32_t *out_mzf_size );


    /**
     * @brief Dekóduje MZF soubor na raw CP/M data a metadata.
     *
     * Rozparsuje 128B MZF hlavičku, z pole `fsize` zjistí velikost dat
     * a zkopíruje tělo rámce do výstupního bufferu. Extrahuje jméno,
     * příponu a (pouze pro `ftype == 0x22`) souborové atributy z bitu 7
     * znaků přípony. Pole `fstrt` a `fexec` se ignorují - CP/M koncept
     * load/exec adres nepoužívá.
     *
     * Výstupní buffer pro data je alokován na heapu (pokud data_size > 0)
     * a volající zodpovídá za jeho uvolnění přes free().
     *
     * Výstupní parametry cpm_name, cpm_ext, cpm_attrs a exec_addr
     * mohou být NULL, pokud daná metadata nejsou potřeba.
     *
     * @param[in]  mzf_data      MZF data (128B hlavička + tělo).
     * @param[in]  mzf_size      Velikost MZF dat v bajtech (min. 128 + fsize).
     * @param[out] cpm_name      CP/M jméno bez mezer (min 9 B buffer). Může být NULL.
     * @param[out] cpm_ext       CP/M přípona bez mezer (min 4 B buffer). Může být NULL.
     * @param[out] cpm_attrs     Souborové atributy (en_MZDSK_CPM_ATTR). Může být NULL.
     *                            Pro `ftype != 0x22` je vždy 0 (atributy se u jiných
     *                            typů do bitu 7 nekódují).
     * @param[out] exec_addr     Exec adresa z MZF pole `fexec`. Může být NULL.
     *                            Pozn.: CP/M tuto hodnotu nepoužívá, parametr zachován
     *                            pro případné informativní zobrazení.
     * @param[out] out_data      Výstupní ukazatel na alokovaný datový buffer. Nesmí být NULL.
     *                           Při fsize == 0 bude *out_data == NULL.
     * @param[out] out_data_size Výstupní velikost dat v bajtech. Nesmí být NULL.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud povinný parametr je NULL nebo mzf_size < 128.
     * @return MZDSK_RES_FORMAT_ERROR pokud `mzf_size` nestačí na deklarovaný `fsize`.
     * @return MZDSK_RES_UNKNOWN_ERROR při selhání alokace paměti.
     *
     * @pre mzf_data != NULL && mzf_size >= 128
     * @pre out_data != NULL && out_data_size != NULL
     */
    extern en_MZDSK_RES mzdsk_cpm_mzf_decode ( const uint8_t *mzf_data, uint32_t mzf_size,
                                                 char *cpm_name, char *cpm_ext,
                                                 uint8_t *cpm_attrs, uint16_t *exec_addr,
                                                 uint8_t **out_data, uint32_t *out_data_size );


    /**
     * @brief Dekóduje MZF soubor s volitelnou variantou Sharp MZ znakové sady.
     *
     * Rozšířená verze `mzdsk_cpm_mzf_decode()` - přidává parametr `encoding`,
     * který určuje, jakou variantou Sharp MZ znakové sady se konvertuje `fname`
     * na standardní ASCII u obecných MZF souborů (`ftype != 0x22`).
     *
     * Pro `ftype == 0x22` (CPM-IC export našich nástrojů) se parametr `encoding`
     * ignoruje - `fname` je tam uloženo přímo v ASCII a používá se pouze
     * maskování bitu 7 pro získání názvu a dekódování atributů.
     *
     * Pro `ftype != 0x22` se `fname` konvertuje přes `mzf_tools_get_fname_ex()`:
     * - `MZF_NAME_ASCII_EU`: Sharp MZ evropská varianta
     * - `MZF_NAME_ASCII_JP`: Sharp MZ japonská varianta
     *
     * Výsledný ASCII řetězec se trimuje, rozdělí na 8.3 podle poslední tečky
     * a sanitizuje na printable ASCII - zajišťuje použitelnost jména v CP/M
     * nástrojích (CCP, PIP).
     *
     * UTF-8 encoding není podporováno a vrací `MZDSK_RES_INVALID_PARAM`.
     *
     * @param[in]  mzf_data      MZF data (128B hlavička + tělo).
     * @param[in]  mzf_size      Velikost MZF dat v bajtech (min. 128 + fsize).
     * @param[in]  encoding      Varianta Sharp MZ znakové sady
     *                            (MZF_NAME_ASCII_EU nebo MZF_NAME_ASCII_JP).
     * @param[out] cpm_name      CP/M jméno (min 9 B buffer). Může být NULL.
     * @param[out] cpm_ext       CP/M přípona (min 4 B buffer). Může být NULL.
     * @param[out] cpm_attrs     Souborové atributy. Může být NULL.
     *                           Pro `ftype != 0x22` vždy 0.
     * @param[out] exec_addr     Exec adresa z MZF pole `fexec`. Může být NULL.
     * @param[out] out_data      Výstupní ukazatel na alokovaný datový buffer.
     * @param[out] out_data_size Výstupní velikost dat v bajtech.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud povinný parametr je NULL,
     *         mzf_size < 128, nebo encoding není ASCII varianta.
     * @return MZDSK_RES_FORMAT_ERROR pokud `mzf_size` nestačí na deklarovaný `fsize`.
     * @return MZDSK_RES_UNKNOWN_ERROR při selhání alokace paměti.
     */
    extern en_MZDSK_RES mzdsk_cpm_mzf_decode_ex ( const uint8_t *mzf_data, uint32_t mzf_size,
                                                    en_MZF_NAME_ENCODING encoding,
                                                    char *cpm_name, char *cpm_ext,
                                                    uint8_t *cpm_attrs, uint16_t *exec_addr,
                                                    uint8_t **out_data, uint32_t *out_data_size );


    /**
     * @brief Modifikátory chování `mzdsk_cpm_mzf_decode_ex2()`.
     *
     * Bitové pole ovlivňuje zacházení s MZF hlavičkami `ftype == 0x22`,
     * které naše nástroje exportují v CPM-IC konvenci (fname je ASCII
     * v 8.3 layoutu, bit 7 přípony kóduje atributy R/O, SYS, ARC).
     * Výchozí chování (flags == 0) odpovídá `mzdsk_cpm_mzf_decode_ex()`.
     */
    typedef enum en_MZDSK_CPM_MZF_DECODE_FLAGS {
        MZDSK_CPM_MZF_DECODE_DEFAULT       = 0,         /**< Výchozí chování: CPM-IC konvence pro ftype==0x22. */
        MZDSK_CPM_MZF_DECODE_FORCE_CHARSET = 1u << 0,   /**< I pro ftype==0x22 konvertuj fname přes encoding (Sharp MZ ASCII) místo CPM-IC ASCII masky. */
        MZDSK_CPM_MZF_DECODE_NO_ATTRS      = 1u << 1    /**< Nedekoduj atributy R/O, SYS, ARC z bitu 7 přípony (cpm_attrs bude 0). */
    } en_MZDSK_CPM_MZF_DECODE_FLAGS;


    /**
     * @brief Dekóduje MZF soubor s bitovými flagy ovlivňujícími ftype==0x22.
     *
     * Rozšíření `mzdsk_cpm_mzf_decode_ex()` o parametr `flags`, který
     * umožňuje potlačit specifické chování pro CPM-IC MZF (`ftype == 0x22`):
     *
     * - `MZDSK_CPM_MZF_DECODE_FORCE_CHARSET`: i pro ftype==0x22 se
     *   `fname` konvertuje přes `encoding` (Sharp MZ ASCII, EU/JP) -
     *   vhodné pro cizí nástroje, které použily ftype 0x22 jako
     *   marker, ale nezakódovaly fname jako ASCII.
     * - `MZDSK_CPM_MZF_DECODE_NO_ATTRS`: atributy z bitu 7 přípony
     *   se nedekódují (cpm_attrs bude 0). Vhodné pro cizí MZF, kde
     *   bit 7 není CP/M atribut.
     *
     * Oba flagy lze kombinovat (`| OR`). `flags == 0` je identické s
     * voláním `mzdsk_cpm_mzf_decode_ex()`. Sémantika ostatních
     * parametrů je stejná jako u `mzdsk_cpm_mzf_decode_ex()`.
     *
     * @param[in]  mzf_data      MZF data (128B hlavička + tělo).
     * @param[in]  mzf_size      Velikost MZF dat v bajtech (min. 128 + fsize).
     * @param[in]  encoding      Varianta Sharp MZ znakové sady
     *                            (MZF_NAME_ASCII_EU nebo MZF_NAME_ASCII_JP).
     * @param[in]  flags         Bitové pole z `en_MZDSK_CPM_MZF_DECODE_FLAGS`.
     * @param[out] cpm_name      CP/M jméno (min 9 B buffer). Může být NULL.
     * @param[out] cpm_ext       CP/M přípona (min 4 B buffer). Může být NULL.
     * @param[out] cpm_attrs     Souborové atributy. Může být NULL.
     * @param[out] exec_addr     Exec adresa z MZF pole `fexec`. Může být NULL.
     * @param[out] out_data      Výstupní ukazatel na alokovaný datový buffer.
     * @param[out] out_data_size Výstupní velikost dat v bajtech.
     *
     * @return MZDSK_RES_OK při úspěchu.
     * @return MZDSK_RES_INVALID_PARAM pokud povinný parametr je NULL,
     *         mzf_size < 128, nebo encoding není ASCII varianta.
     * @return MZDSK_RES_FORMAT_ERROR pokud `mzf_size` nestačí na deklarovaný `fsize`.
     * @return MZDSK_RES_UNKNOWN_ERROR při selhání alokace paměti.
     */
    extern en_MZDSK_RES mzdsk_cpm_mzf_decode_ex2 ( const uint8_t *mzf_data, uint32_t mzf_size,
                                                     en_MZF_NAME_ENCODING encoding,
                                                     unsigned flags,
                                                     char *cpm_name, char *cpm_ext,
                                                     uint8_t *cpm_attrs, uint16_t *exec_addr,
                                                     uint8_t **out_data, uint32_t *out_data_size );


    /* ================================================================
     * Verze
     * ================================================================ */

    /** @brief Verze modulu mzdsk_cpm_mzf. */
#define MZDSK_CPM_MZF_VERSION      "2.2.0"

    /**
     * @brief Vrátí řetězec s verzí modulu mzdsk_cpm_mzf.
     * @return Statický řetězec s verzí.
     */
    extern const char* mzdsk_cpm_mzf_version ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZDSK_CPM_MZF_H */
