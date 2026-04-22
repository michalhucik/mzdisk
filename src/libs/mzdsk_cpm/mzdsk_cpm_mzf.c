/**
 * @file   mzdsk_cpm_mzf.c
 * @brief  Implementace konverze mezi CP/M soubory a MZF tape formátem.
 *
 * Viz mzdsk_cpm_mzf.h pro popis formátu a API.
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


#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "mzdsk_cpm_mzf.h"
#include "mzdsk_cpm.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"


/* =========================================================================
 * Interní pomocné funkce
 * ========================================================================= */


/**
 * @brief Sestaví 128B MZF hlavičku.
 *
 * Vyplní hlavičku standardního MZF rámce: typ, jméno v 8.3 formátu
 * s tečkou a CR terminátorem, volitelně bity 7 znaků přípony nesoucí
 * atributy (R/O, SYS, ARC - konvence používaná pro CP/M export typu 0x22),
 * velikost dat, load adresu a exec adresu.
 *
 * @param[out] hdr          Výstupní hlavička k vyplnění. Nesmí být NULL.
 * @param[in]  cpm_name     CP/M jméno souboru (max 8 znaků). Nesmí být NULL.
 * @param[in]  cpm_ext      CP/M přípona souboru (max 3 znaky). Nesmí být NULL.
 * @param[in]  cpm_attrs    Souborové atributy (en_MZDSK_CPM_ATTR).
 * @param[in]  ftype        MZF typ souboru.
 * @param[in]  fsize        Velikost datového bloku v bajtech.
 * @param[in]  strt_addr    Load adresa - zapíše se do pole `fstrt`.
 * @param[in]  exec_addr    Exec adresa - zapíše se do pole `fexec`.
 * @param[in]  encode_attrs Kódovat atributy do bitů 7 znaků přípony (0/1).
 *
 * @pre hdr != NULL && cpm_name != NULL && cpm_ext != NULL
 * @post hdr obsahuje kompletní MZF hlavičku v hostitelském byte-order.
 */
static void build_mzf_header ( st_MZF_HEADER *hdr,
                                const char *cpm_name, const char *cpm_ext,
                                uint8_t cpm_attrs, uint8_t ftype,
                                uint16_t fsize,
                                uint16_t strt_addr, uint16_t exec_addr,
                                int encode_attrs ) {

    memset ( hdr, 0, sizeof ( *hdr ) );

    /* Typ souboru */
    hdr->ftype = ftype;

    /* Jméno souboru - 8 znaků doplněných mezerami (0x20) */
    size_t name_len = strlen ( cpm_name );
    if ( name_len > 8 ) name_len = 8;

    for ( int i = 0; i < 8; i++ ) {
        hdr->fname.name[i] = ( i < (int) name_len ) ? (uint8_t) cpm_name[i] : 0x20;
    }

    /* Tečka jako oddělovač jména a přípony */
    hdr->fname.name[8] = 0x2E;

    /* Přípona - 3 znaky doplněné mezerami, volitelně bit 7 = atributy R/O, SYS, ARC */
    size_t ext_len = strlen ( cpm_ext );
    if ( ext_len > 3 ) ext_len = 3;

    for ( int i = 0; i < 3; i++ ) {
        uint8_t ch = ( i < (int) ext_len ) ? (uint8_t) cpm_ext[i] : 0x20;
        if ( encode_attrs ) {
            if ( i == 0 && ( cpm_attrs & MZDSK_CPM_ATTR_READ_ONLY ) ) ch |= 0x80;
            if ( i == 1 && ( cpm_attrs & MZDSK_CPM_ATTR_SYSTEM ) )    ch |= 0x80;
            if ( i == 2 && ( cpm_attrs & MZDSK_CPM_ATTR_ARCHIVED ) )  ch |= 0x80;
        }
        hdr->fname.name[9 + i] = ch;
    }

    /* CR terminátor za příponou */
    hdr->fname.name[12] = MZF_FNAME_TERMINATOR;

    /* Zbylé bajty fname (0x0E-0x10) jsou nulované z memset. */
    /* fname.terminator (0x11) - standardní MZF terminátor */
    hdr->fname.terminator = MZF_FNAME_TERMINATOR;

    /* Velikost dat, load adresa, exec adresa */
    hdr->fsize = fsize;
    hdr->fstrt = strt_addr;
    hdr->fexec = exec_addr;
}


/**
 * @brief Sanitizuje jeden znak pro CP/M directory entry.
 *
 * CP/M directory vyžaduje printable ASCII (po maskování bitu 7)
 * v rozsahu 0x20-0x7E - jinak detekce souborového systému odmítne
 * celý disk jako nevalidní CP/M. Vnitřní mezery jsou v CP/M 8.3
 * jménech nestandardní (ač technicky printable), proto se nahrazují
 * `_` - mezery na konci se beztak ořežou v callerovi.
 *
 * @param[in] c  Vstupní znak (libovolný bajt 0x00-0xFF).
 * @return       Sanitizovaný znak: printable ASCII bez mezery, nebo `_`.
 */
static char sanitize_cpm_char ( unsigned char c ) {
    if ( c == ' ' ) return '_';
    if ( c >= 0x21 && c <= 0x7E ) return (char) c;
    return '_';
}


/**
 * @brief Ořízne koncové mezery z řetězce.
 *
 * @param[in,out] s  Null-terminated řetězec. Nesmí být NULL.
 */
static void rtrim_spaces ( char *s ) {
    size_t len = strlen ( s );
    while ( len > 0 && s[len - 1] == ' ' ) {
        s[--len] = '\0';
    }
}


/**
 * @brief Odstraní úvodní mezery z řetězce.
 *
 * @param[in,out] s  Null-terminated řetězec. Nesmí být NULL.
 */
static void ltrim_spaces ( char *s ) {
    char *p = s;
    while ( *p == ' ' ) p++;
    if ( p != s ) memmove ( s, p, strlen ( p ) + 1 );
}


/**
 * @brief Parsuje metadata z MZF hlavičky s volitelnou Sharp MZ konverzí.
 *
 * Extrahuje CP/M 8.3 jméno, příponu, atributy a exec adresu z MZF hlavičky.
 * Postup závisí na MZF typu souboru:
 *
 * - `ftype == 0x22` (CPM-IC konvence, export našich nástrojů):
 *   `fname` je uloženo v ASCII s tečkou na pozici 8. Bit 7 znaků přípony
 *   kóduje atributy R/O, SYS, ARC. Po maskování bitu 7 se každý bajt
 *   sanitizuje na printable ASCII (non-printable → `_`).
 *
 * - `ftype != 0x22` (obecný MZF):
 *   `fname` je v Sharp MZ ASCII. Konverze na standardní ASCII se provede
 *   přes `mzf_tools_get_fname_ex()` s `encoding` (EU/JP). Výsledný řetězec
 *   se trimuje (leading/trailing mezery), rozdělí na `name.ext` podle
 *   poslední tečky a sanitizuje (non-printable a mezery uvnitř → `_`).
 *   Atributy jsou vždy 0. Když po sanitizaci vyjde prázdné jméno,
 *   použije se fallback `FILE`.
 *
 * Sanitizace na printable ASCII zajišťuje kompatibilitu s CP/M nástroji
 * (CCP, PIP), které vyžadují jméno, jež lze zadat na příkazové řádce.
 *
 * @param[in]  hdr        MZF hlavička v hostitelském byte-order. Nesmí být NULL.
 * @param[in]  encoding   Varianta Sharp MZ znakové sady pro konverzi.
 *                        Pro ftype != 0x22 se používá vždy. Pro ftype == 0x22
 *                        jen když je nastaven flag MZDSK_CPM_MZF_DECODE_FORCE_CHARSET.
 * @param[in]  flags      Bitové pole z `en_MZDSK_CPM_MZF_DECODE_FLAGS` -
 *                        FORCE_CHARSET přesměruje ftype==0x22 do Sharp MZ
 *                        konverzní větve, NO_ATTRS potlačí dekódování
 *                        atributů z bitu 7 přípony.
 * @param[out] cpm_name   CP/M jméno bez mezer (min 9 B). Může být NULL.
 * @param[out] cpm_ext    CP/M přípona bez mezer (min 4 B). Může být NULL.
 * @param[out] cpm_attrs  Souborové atributy. Může být NULL.
 * @param[out] exec_addr  Exec adresa (z pole `fexec`). Může být NULL.
 *
 * @pre hdr != NULL
 * @post cpm_name (pokud != NULL) obsahuje null-terminated ASCII řetězec
 *       délky 0-8, každý znak je printable ASCII ∈ <0x21,0x7E>.
 * @post cpm_ext (pokud != NULL) obsahuje null-terminated ASCII řetězec
 *       délky 0-3, každý znak je printable ASCII ∈ <0x21,0x7E>.
 */
static void parse_mzf_header_ex ( const st_MZF_HEADER *hdr,
                                    en_MZF_NAME_ENCODING encoding,
                                    unsigned flags,
                                    char *cpm_name, char *cpm_ext,
                                    uint8_t *cpm_attrs,
                                    uint16_t *exec_addr ) {

    /* CPM-IC konvenci (ASCII+atributy) uplatníme jen pokud to volající
     * neveto'uje flagem FORCE_CHARSET. FORCE_CHARSET přesměruje ftype==0x22
     * do obecné větve, která konvertuje fname přes Sharp MZ encoding. */
    bool apply_cpm_ic = ( hdr->ftype == MZDSK_CPM_MZF_FTYPE ) &&
                        !( flags & MZDSK_CPM_MZF_DECODE_FORCE_CHARSET );

    if ( apply_cpm_ic ) {
        /* CPM-IC konvence: fname je ASCII v 8.3 layoutu, bit 7 přípony = atributy. */

        /* Jméno - 8 znaků, bit 7 maskován, sanitizace, ořez koncových mezer */
        if ( cpm_name != NULL ) {
            int last = -1;
            for ( int i = 0; i < 8; i++ ) {
                uint8_t b = hdr->fname.name[i] & 0x7F;
                /* Zachováme mezery (pro ořez níže), jinak sanitizace. */
                char ch = ( b == ' ' ) ? ' ' :
                          ( b >= 0x21 && b <= 0x7E ) ? (char) b : '_';
                cpm_name[i] = ch;
                if ( ch != ' ' ) last = i;
            }
            cpm_name[last + 1] = '\0';
        }

        /* Přípona - 3 znaky, bit 7 = atributy, sanitizace, ořez koncových mezer */
        if ( cpm_ext != NULL ) {
            int last = -1;
            for ( int i = 0; i < 3; i++ ) {
                uint8_t b = hdr->fname.name[9 + i] & 0x7F;
                char ch = ( b == ' ' ) ? ' ' :
                          ( b >= 0x21 && b <= 0x7E ) ? (char) b : '_';
                cpm_ext[i] = ch;
                if ( ch != ' ' ) last = i;
            }
            cpm_ext[last + 1] = '\0';
        }

        /* Atributy z bitu 7 příponových bajtů. Flag NO_ATTRS zruší
         * dekódování - vhodné pro cizí MZF, kde bit 7 není CP/M atribut. */
        if ( cpm_attrs != NULL ) {
            *cpm_attrs = 0;
            if ( !( flags & MZDSK_CPM_MZF_DECODE_NO_ATTRS ) ) {
                if ( hdr->fname.name[9]  & 0x80 ) *cpm_attrs |= MZDSK_CPM_ATTR_READ_ONLY;
                if ( hdr->fname.name[10] & 0x80 ) *cpm_attrs |= MZDSK_CPM_ATTR_SYSTEM;
                if ( hdr->fname.name[11] & 0x80 ) *cpm_attrs |= MZDSK_CPM_ATTR_ARCHIVED;
            }
        }
    } else {
        /* Obecný MZF - fname je v Sharp MZ ASCII, konverze přes sharpmz_ascii. */
        char ascii_name[MZF_FILE_NAME_LENGTH + 1];
        mzf_tools_get_fname_ex ( hdr, ascii_name, sizeof ( ascii_name ), encoding );

        /* Oříznutí úvodních a koncových mezer (Bomber má leading mezeru,
           obecně padding mezerami). */
        rtrim_spaces ( ascii_name );
        ltrim_spaces ( ascii_name );

        /* Rozdělení na name.ext podle poslední tečky */
        const char *name_src;
        const char *ext_src;
        size_t name_len, ext_len;
        char *dot = strrchr ( ascii_name, '.' );
        if ( dot != NULL ) {
            name_src = ascii_name;
            name_len = (size_t) ( dot - ascii_name );
            ext_src = dot + 1;
            ext_len = strlen ( ext_src );
        } else {
            name_src = ascii_name;
            name_len = strlen ( ascii_name );
            ext_src = "";
            ext_len = 0;
        }

        if ( cpm_name != NULL ) {
            size_t n = ( name_len > 8 ) ? 8 : name_len;
            for ( size_t i = 0; i < n; i++ ) {
                cpm_name[i] = sanitize_cpm_char ( (unsigned char) name_src[i] );
            }
            cpm_name[n] = '\0';
            /* Po sanitizaci prázdné jméno (vstup byl samé non-printable) -
               fallback, aby CP/M directory entry zůstala validní. */
            if ( cpm_name[0] == '\0' ) {
                strcpy ( cpm_name, "FILE" );
            }
        }

        if ( cpm_ext != NULL ) {
            size_t n = ( ext_len > 3 ) ? 3 : ext_len;
            for ( size_t i = 0; i < n; i++ ) {
                cpm_ext[i] = sanitize_cpm_char ( (unsigned char) ext_src[i] );
            }
            cpm_ext[n] = '\0';
        }

        if ( cpm_attrs != NULL ) {
            *cpm_attrs = 0;
        }
    }

    if ( exec_addr != NULL ) {
        *exec_addr = hdr->fexec;
    }
}


/* =========================================================================
 * Veřejné API
 * ========================================================================= */


/** @copydoc mzdsk_cpm_mzf_encode_ex */
en_MZDSK_RES mzdsk_cpm_mzf_encode_ex ( const uint8_t *data, uint32_t data_size,
                                          const char *cpm_name, const char *cpm_ext,
                                          uint8_t cpm_attrs, uint8_t ftype,
                                          uint16_t exec_addr, uint16_t strt_addr,
                                          int encode_attrs,
                                          uint8_t **out_mzf, uint32_t *out_mzf_size ) {

    if ( cpm_name == NULL || cpm_ext == NULL ||
         out_mzf == NULL || out_mzf_size == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    if ( data_size > 0 && data == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* fsize je v MZF hlavičce 16bitové pole, proto nelze zapsat více
       než MZDSK_CPM_MZF_MAX_DATA (65535) bajtů do jednoho rámce. */
    if ( data_size > MZDSK_CPM_MZF_MAX_DATA ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Alokace: 128B hlavička + data_size */
    uint32_t total_size = MZF_HEADER_SIZE + data_size;
    uint8_t *mzf = (uint8_t *) malloc ( total_size );
    if ( mzf == NULL ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }

    /* Sestavení hlavičky v hostitelském byte-order */
    st_MZF_HEADER hdr;
    build_mzf_header ( &hdr, cpm_name, cpm_ext, cpm_attrs, ftype,
                        (uint16_t) data_size, strt_addr, exec_addr,
                        encode_attrs );

    /* Konverze uint16 polí na LE pro serializaci */
    mzf_header_items_correction ( &hdr );

    /* Zápis hlavičky a dat */
    memcpy ( mzf, &hdr, MZF_HEADER_SIZE );
    if ( data_size > 0 ) {
        memcpy ( mzf + MZF_HEADER_SIZE, data, data_size );
    }

    *out_mzf = mzf;
    *out_mzf_size = total_size;

    return MZDSK_RES_OK;
}


/** @copydoc mzdsk_cpm_mzf_encode */
en_MZDSK_RES mzdsk_cpm_mzf_encode ( const uint8_t *data, uint32_t data_size,
                                      const char *cpm_name, const char *cpm_ext,
                                      uint8_t cpm_attrs, uint16_t exec_addr,
                                      uint8_t **out_mzf, uint32_t *out_mzf_size ) {

    return mzdsk_cpm_mzf_encode_ex ( data, data_size, cpm_name, cpm_ext,
                                      cpm_attrs, MZDSK_CPM_MZF_FTYPE,
                                      exec_addr, MZDSK_CPM_MZF_DEFAULT_ADDR,
                                      1, out_mzf, out_mzf_size );
}


/** @copydoc mzdsk_cpm_mzf_decode_ex2 */
en_MZDSK_RES mzdsk_cpm_mzf_decode_ex2 ( const uint8_t *mzf_data, uint32_t mzf_size,
                                         en_MZF_NAME_ENCODING encoding,
                                         unsigned flags,
                                         char *cpm_name, char *cpm_ext,
                                         uint8_t *cpm_attrs, uint16_t *exec_addr,
                                         uint8_t **out_data, uint32_t *out_data_size ) {

    if ( mzf_data == NULL || mzf_size < MZF_HEADER_SIZE ||
         out_data == NULL || out_data_size == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Povolené jsou pouze ASCII varianty (EU/JP) - UTF-8 encoding by vygeneroval
       víc než 1 bajt na znak a CP/M 8.3 directory s ním neumí pracovat. */
    if ( encoding != MZF_NAME_ASCII_EU && encoding != MZF_NAME_ASCII_JP ) {
        return MZDSK_RES_INVALID_PARAM;
    }

    /* Načtení a konverze hlavičky */
    st_MZF_HEADER hdr;
    memcpy ( &hdr, mzf_data, MZF_HEADER_SIZE );
    mzf_header_items_correction ( &hdr );

    /* Velikost dat podle fsize v hlavičce */
    uint32_t data_size = hdr.fsize;

    /* Kontrola, že data za hlavičkou skutečně jsou */
    if ( MZF_HEADER_SIZE + data_size > mzf_size ) {
        return MZDSK_RES_FORMAT_ERROR;
    }

    /* Extrakce metadat */
    parse_mzf_header_ex ( &hdr, encoding, flags, cpm_name, cpm_ext, cpm_attrs, exec_addr );

    /* Prázdný soubor */
    if ( data_size == 0 ) {
        *out_data = NULL;
        *out_data_size = 0;
        return MZDSK_RES_OK;
    }

    /* Alokace a kopie datového bloku */
    uint8_t *result = (uint8_t *) malloc ( data_size );
    if ( result == NULL ) {
        return MZDSK_RES_UNKNOWN_ERROR;
    }
    memcpy ( result, mzf_data + MZF_HEADER_SIZE, data_size );

    *out_data = result;
    *out_data_size = data_size;

    return MZDSK_RES_OK;
}


/** @copydoc mzdsk_cpm_mzf_decode_ex */
en_MZDSK_RES mzdsk_cpm_mzf_decode_ex ( const uint8_t *mzf_data, uint32_t mzf_size,
                                        en_MZF_NAME_ENCODING encoding,
                                        char *cpm_name, char *cpm_ext,
                                        uint8_t *cpm_attrs, uint16_t *exec_addr,
                                        uint8_t **out_data, uint32_t *out_data_size ) {

    return mzdsk_cpm_mzf_decode_ex2 ( mzf_data, mzf_size, encoding,
                                        MZDSK_CPM_MZF_DECODE_DEFAULT,
                                        cpm_name, cpm_ext, cpm_attrs, exec_addr,
                                        out_data, out_data_size );
}


/** @copydoc mzdsk_cpm_mzf_decode */
en_MZDSK_RES mzdsk_cpm_mzf_decode ( const uint8_t *mzf_data, uint32_t mzf_size,
                                      char *cpm_name, char *cpm_ext,
                                      uint8_t *cpm_attrs, uint16_t *exec_addr,
                                      uint8_t **out_data, uint32_t *out_data_size ) {

    return mzdsk_cpm_mzf_decode_ex ( mzf_data, mzf_size, MZF_NAME_ASCII_EU,
                                       cpm_name, cpm_ext, cpm_attrs, exec_addr,
                                       out_data, out_data_size );
}


/** @copydoc mzdsk_cpm_mzf_version */
const char* mzdsk_cpm_mzf_version ( void ) {
    return MZDSK_CPM_MZF_VERSION;
}
