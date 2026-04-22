/**
 * @file   output_format.c
 * @brief  Implementace proudového generování JSON a CSV výstupu.
 *
 * Generuje JSON a CSV výstup přímo na FILE* proud bez dynamických alokací.
 * Pro formát TEXT jsou všechny funkce NOP.
 *
 * JSON escapování řeší znaky: " \ a kontrolní znaky (\n, \r, \t, \b, \f).
 * CSV escapování: pokud hodnota obsahuje čárku, uvozovku nebo newline,
 * obalí se uvozovkami (RFC 4180).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "libs/output_format/output_format.h"


/* =========================================================================
 * Interní pomocné funkce
 * ========================================================================= */


/**
 * @brief Vypíše JSON-escapovaný řetězec (bez okolních uvozovek).
 *
 * Audit M-18: dříve se kontrolní znaky < 0x20 tiše přeskakovaly a
 * byty >= 0x80 (např. Sharp MZ JP grafické znaky) se posílaly raw.
 * To produkovalo nevalidní JSON (kontrolní znaky nejsou povoleny
 * v string literal bez escapování) a nevalidní UTF-8 (raw 0x80-0xFF).
 *
 * Teď všechny bajty < 0x20 a >= 0x7F escapujeme jako `\u00XX`.
 * Escape sekvence známých control chars (\n, \r, ...) zůstávají
 * kompaktní. Bajty v rozsahu 0x20-0x7E se vypisují raw.
 *
 * Pozn.: pro `get-all` adresářový výpis kde jméno obsahuje Sharp MZ
 * extended bajty doporučujeme předem zkonvertovat přes sharpmz_ascii
 * do UTF-8 (přes `--charset utf8-eu`/`utf8-jp`). Tento escape je
 * bezpečnostní fallback.
 *
 * @param[in] out Výstupní proud. Nesmí být NULL.
 * @param[in] str Řetězec k vypsání. Nesmí být NULL.
 */
static void json_print_escaped ( FILE *out, const char *str ) {
    for ( const char *p = str; *p; p++ ) {
        unsigned char c = (unsigned char) *p;
        switch ( *p ) {
            case '"':  fputs ( "\\\"", out ); break;
            case '\\': fputs ( "\\\\", out ); break;
            case '\n': fputs ( "\\n", out );  break;
            case '\r': fputs ( "\\r", out );  break;
            case '\t': fputs ( "\\t", out );  break;
            case '\b': fputs ( "\\b", out );  break;
            case '\f': fputs ( "\\f", out );  break;
            default:
                if ( c >= 0x20 && c < 0x7F ) {
                    /* Tisknutelné ASCII - raw. */
                    fputc ( *p, out );
                } else {
                    /* Kontrolní znak < 0x20 nebo non-ASCII >= 0x7F:
                     * escapujeme jako \u00XX pro validní JSON. */
                    fprintf ( out, "\\u%04x", c );
                }
                break;
        }
    }
}


/**
 * @brief Vypíše CSV hodnotu s escapováním podle RFC 4180.
 *
 * Pokud hodnota obsahuje čárku, uvozovku nebo newline, obalí se
 * uvozovkami a uvozovky uvnitř se zdvojí.
 *
 * @param[in] out Výstupní proud. Nesmí být NULL.
 * @param[in] str Řetězec k vypsání. Nesmí být NULL.
 */
static void csv_print_escaped ( FILE *out, const char *str ) {
    /* Zjisti zda je potřeba escapovat */
    int needs_escape = 0;
    for ( const char *p = str; *p; p++ ) {
        if ( *p == ',' || *p == '"' || *p == '\n' || *p == '\r' ) {
            needs_escape = 1;
            break;
        }
    }

    if ( !needs_escape ) {
        fputs ( str, out );
        return;
    }

    fputc ( '"', out );
    for ( const char *p = str; *p; p++ ) {
        if ( *p == '"' ) {
            fputs ( "\"\"", out );
        } else {
            fputc ( *p, out );
        }
    }
    fputc ( '"', out );
}


/**
 * @brief Vypíše CSV hlavičku "key,value" pro klíč-hodnota výstup.
 *
 * Hlavička se vypíše pouze jednou - při prvním volání. Další volání
 * jsou NOP díky příznaku csv_header_printed v kontextu.
 *
 * Tato funkce se používá pro CSV výstup příkazů, které generují
 * ploché klíč-hodnota páry (info, boot) místo tabulkových dat.
 *
 * @param[in,out] ctx Kontext. Nesmí být NULL. Formát musí být OUTFMT_CSV.
 *
 * @post Pokud hlavička ještě nebyla vypsána, vypíše "key,value\n"
 *       a nastaví csv_header_printed na 1.
 */
static void csv_kv_header ( st_OUTFMT_CTX *ctx ) {
    if ( ctx->csv_header_printed ) return;
    fputs ( "key,value\n", ctx->out );
    ctx->csv_header_printed = 1;
}


/**
 * @brief Vypíše oddělovač před klíč-hodnota párem v JSON kořenovém objektu.
 *
 * Při prvním páru nevypíše nic, při dalších vypíše čárku a newline.
 *
 * @param[in,out] ctx Kontext. Nesmí být NULL.
 */
static void json_kv_separator ( st_OUTFMT_CTX *ctx ) {
    if ( ctx->kv_count > 0 ) {
        fputs ( ",\n", ctx->out );
    }
    ctx->kv_count++;
}


/* =========================================================================
 * Veřejné funkce
 * ========================================================================= */


void outfmt_init ( st_OUTFMT_CTX *ctx, en_OUTFMT format ) {
    memset ( ctx, 0, sizeof ( *ctx ) );
    ctx->format = format;
    ctx->out = stdout;
}


int outfmt_parse ( const char *str, en_OUTFMT *format ) {
    if ( strcasecmp ( str, "text" ) == 0 ) {
        *format = OUTFMT_TEXT;
        return 0;
    }
    if ( strcasecmp ( str, "json" ) == 0 ) {
        *format = OUTFMT_JSON;
        return 0;
    }
    if ( strcasecmp ( str, "csv" ) == 0 ) {
        *format = OUTFMT_CSV;
        return 0;
    }
    return -1;
}


void outfmt_doc_begin ( st_OUTFMT_CTX *ctx ) {
    if ( ctx->format == OUTFMT_JSON ) {
        fputs ( "{\n", ctx->out );
    }
    /* CSV hlavička se tiskne lazy - až při prvním kv páru */
}


void outfmt_doc_end ( st_OUTFMT_CTX *ctx ) {
    if ( ctx->format == OUTFMT_JSON ) {
        fputs ( "\n}\n", ctx->out );
    }
}


void outfmt_kv_str ( st_OUTFMT_CTX *ctx, const char *key, const char *value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        json_kv_separator ( ctx );
        fprintf ( ctx->out, "  \"%s\": \"", key );
        json_print_escaped ( ctx->out, value );
        fputc ( '"', ctx->out );
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->csv_table_mode ) return;
        csv_kv_header ( ctx );
        csv_print_escaped ( ctx->out, key );
        fputc ( ',', ctx->out );
        csv_print_escaped ( ctx->out, value );
        fputc ( '\n', ctx->out );
        ctx->kv_count++;
    }
}


void outfmt_kv_int ( st_OUTFMT_CTX *ctx, const char *key, long value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        json_kv_separator ( ctx );
        fprintf ( ctx->out, "  \"%s\": %ld", key, value );
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->csv_table_mode ) return;
        csv_kv_header ( ctx );
        csv_print_escaped ( ctx->out, key );
        fprintf ( ctx->out, ",%ld\n", value );
        ctx->kv_count++;
    }
}


void outfmt_kv_uint ( st_OUTFMT_CTX *ctx, const char *key, unsigned long value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        json_kv_separator ( ctx );
        fprintf ( ctx->out, "  \"%s\": %lu", key, value );
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->csv_table_mode ) return;
        csv_kv_header ( ctx );
        csv_print_escaped ( ctx->out, key );
        fprintf ( ctx->out, ",%lu\n", value );
        ctx->kv_count++;
    }
}


void outfmt_array_begin ( st_OUTFMT_CTX *ctx, const char *key ) {
    if ( ctx->format == OUTFMT_JSON ) {
        json_kv_separator ( ctx );
        fprintf ( ctx->out, "  \"%s\": [\n", key );
        ctx->in_array = 1;
        ctx->item_count = 0;
    }
}


void outfmt_array_end ( st_OUTFMT_CTX *ctx ) {
    if ( ctx->format == OUTFMT_JSON ) {
        fputs ( "\n  ]", ctx->out );
        ctx->in_array = 0;
    }
}


void outfmt_item_begin ( st_OUTFMT_CTX *ctx ) {
    if ( ctx->format == OUTFMT_JSON ) {
        if ( ctx->item_count > 0 ) {
            fputs ( ",\n", ctx->out );
        }
        fputs ( "    {", ctx->out );
        ctx->in_object = 1;
        ctx->field_count = 0;
        ctx->item_count++;
    } else if ( ctx->format == OUTFMT_CSV ) {
        ctx->field_count = 0;
        ctx->item_count++;
    }
}


void outfmt_item_end ( st_OUTFMT_CTX *ctx ) {
    if ( ctx->format == OUTFMT_JSON ) {
        fputc ( '}', ctx->out );
        ctx->in_object = 0;
    } else if ( ctx->format == OUTFMT_CSV ) {
        fputc ( '\n', ctx->out );
    }
}


void outfmt_field_str ( st_OUTFMT_CTX *ctx, const char *key, const char *value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        if ( ctx->field_count > 0 ) fputs ( ", ", ctx->out );
        fprintf ( ctx->out, "\"%s\": \"", key );
        json_print_escaped ( ctx->out, value );
        fputc ( '"', ctx->out );
        ctx->field_count++;
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->field_count > 0 ) fputc ( ',', ctx->out );
        csv_print_escaped ( ctx->out, value );
        ctx->field_count++;
    }
}


void outfmt_field_int ( st_OUTFMT_CTX *ctx, const char *key, long value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        if ( ctx->field_count > 0 ) fputs ( ", ", ctx->out );
        fprintf ( ctx->out, "\"%s\": %ld", key, value );
        ctx->field_count++;
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->field_count > 0 ) fputc ( ',', ctx->out );
        fprintf ( ctx->out, "%ld", value );
        ctx->field_count++;
    }
}


void outfmt_field_uint ( st_OUTFMT_CTX *ctx, const char *key, unsigned long value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        if ( ctx->field_count > 0 ) fputs ( ", ", ctx->out );
        fprintf ( ctx->out, "\"%s\": %lu", key, value );
        ctx->field_count++;
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->field_count > 0 ) fputc ( ',', ctx->out );
        fprintf ( ctx->out, "%lu", value );
        ctx->field_count++;
    }
}


void outfmt_field_hex16 ( st_OUTFMT_CTX *ctx, const char *key, uint16_t value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        if ( ctx->field_count > 0 ) fputs ( ", ", ctx->out );
        fprintf ( ctx->out, "\"%s\": \"0x%04x\"", key, value );
        ctx->field_count++;
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->field_count > 0 ) fputc ( ',', ctx->out );
        fprintf ( ctx->out, "0x%04x", value );
        ctx->field_count++;
    }
}


void outfmt_field_hex8 ( st_OUTFMT_CTX *ctx, const char *key, uint8_t value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        if ( ctx->field_count > 0 ) fputs ( ", ", ctx->out );
        fprintf ( ctx->out, "\"%s\": \"0x%02x\"", key, value );
        ctx->field_count++;
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->field_count > 0 ) fputc ( ',', ctx->out );
        fprintf ( ctx->out, "0x%02x", value );
        ctx->field_count++;
    }
}


void outfmt_field_bool ( st_OUTFMT_CTX *ctx, const char *key, int value ) {
    if ( ctx->format == OUTFMT_JSON ) {
        if ( ctx->field_count > 0 ) fputs ( ", ", ctx->out );
        fprintf ( ctx->out, "\"%s\": %s", key, value ? "true" : "false" );
        ctx->field_count++;
    } else if ( ctx->format == OUTFMT_CSV ) {
        if ( ctx->field_count > 0 ) fputc ( ',', ctx->out );
        fprintf ( ctx->out, "%d", value ? 1 : 0 );
        ctx->field_count++;
    }
}


void outfmt_csv_header ( st_OUTFMT_CTX *ctx, const char *headers[], int count ) {
    if ( ctx->format != OUTFMT_CSV ) return;
    if ( ctx->csv_header_printed ) return;

    for ( int i = 0; i < count; i++ ) {
        if ( i > 0 ) fputc ( ',', ctx->out );
        fputs ( headers[i], ctx->out );
    }
    fputc ( '\n', ctx->out );
    ctx->csv_header_printed = 1;
    ctx->csv_table_mode = 1;
}


const char* output_format_version ( void ) {
    return OUTPUT_FORMAT_VERSION;
}
