/**
 * @file   output_format.h
 * @brief  Knihovna pro strojově čitelný výstup (JSON, CSV) z CLI nástrojů.
 *
 * Poskytuje proudové (streaming) generování JSON a CSV výstupu
 * bez dynamických alokací. Výstup se zapisuje přímo na FILE* proud.
 *
 * Typické použití:
 * 1. Inicializace kontextu: outfmt_init()
 * 2. Začátek dokumentu: outfmt_doc_begin()
 * 3. Metadata: outfmt_kv_str(), outfmt_kv_int(), ...
 * 4. Pole položek: outfmt_array_begin() / outfmt_item_begin()+pole / outfmt_item_end() / outfmt_array_end()
 * 5. Konec dokumentu: outfmt_doc_end()
 *
 * Pro formát TEXT jsou všechny funkce NOP - textový výstup generuje
 * stávající kód v jednotlivých nástrojích.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 */

#ifndef OUTPUT_FORMAT_H
#define OUTPUT_FORMAT_H

#include <stdio.h>
#include <stdint.h>


/** @brief Verze knihovny output_format. */
#define OUTPUT_FORMAT_VERSION "1.1.1"


/**
 * @brief Výstupní formát.
 */
typedef enum en_OUTFMT {
    OUTFMT_TEXT = 0,    /**< Lidsky čitelný text (výchozí) */
    OUTFMT_JSON,        /**< JSON formát */
    OUTFMT_CSV          /**< CSV formát */
} en_OUTFMT;


/**
 * @brief Kontext výstupního formátování.
 *
 * Udržuje stav proudového generování (počet položek pro čárky v JSON,
 * stav CSV hlavičky apod.). Inicializuje se voláním outfmt_init().
 *
 * @invariant Po outfmt_init() je format nastaven a out ukazuje na platný proud.
 */
typedef struct st_OUTFMT_CTX {
    en_OUTFMT format;       /**< Aktivní výstupní formát */
    FILE *out;              /**< Výstupní proud */
    int item_count;         /**< Počet dosud vypsaných položek v poli (pro čárky v JSON) */
    int field_count;        /**< Počet dosud vypsaných polí v aktuální položce */
    int kv_count;           /**< Počet dosud vypsaných kv párů na kořenové úrovni */
    int in_array;           /**< 1 pokud jsme uvnitř pole */
    int in_object;          /**< 1 pokud jsme uvnitř objektu */
    int csv_header_printed; /**< 1 pokud CSV hlavička již vypsána */
    int csv_table_mode;     /**< 1 pokud byl kontext přepnut do tabulkového režimu
                                 voláním outfmt_csv_header() - kv páry se pak v CSV ignorují */
} st_OUTFMT_CTX;


/**
 * @brief Inicializuje kontext výstupního formátování.
 *
 * @param[out] ctx    Kontext k inicializaci. Nesmí být NULL.
 * @param[in]  format Požadovaný výstupní formát.
 *
 * @post ctx je připraven k použití, out = stdout.
 */
extern void outfmt_init ( st_OUTFMT_CTX *ctx, en_OUTFMT format );


/**
 * @brief Parsuje název výstupního formátu z řetězce.
 *
 * Rozpoznává: "text", "json", "csv" (case-insensitive).
 *
 * @param[in]  str    Vstupní řetězec. Nesmí být NULL.
 * @param[out] format Výstupní formát.
 *
 * @return 0 při úspěchu, -1 pokud řetězec není rozpoznán.
 */
extern int outfmt_parse ( const char *str, en_OUTFMT *format );


/**
 * @brief Začátek JSON dokumentu (otevře kořenový objekt).
 *
 * Pro JSON vypíše "{". Pro CSV a TEXT je NOP.
 *
 * @param[in,out] ctx Kontext. Nesmí být NULL.
 */
extern void outfmt_doc_begin ( st_OUTFMT_CTX *ctx );


/**
 * @brief Konec JSON dokumentu (zavře kořenový objekt).
 *
 * Pro JSON vypíše "}" a newline. Pro CSV a TEXT je NOP.
 *
 * @param[in,out] ctx Kontext. Nesmí být NULL.
 */
extern void outfmt_doc_end ( st_OUTFMT_CTX *ctx );


/**
 * @brief Vypíše klíč-hodnota pár (řetězec) na kořenové úrovni.
 *
 * Pro JSON: "key": "value" (s JSON escapováním).
 * Pro CSV: řádek "key,value" (s RFC 4180 escapováním); při prvním volání
 * se automaticky vypíše hlavička "key,value".
 * Pro TEXT je NOP.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota. Nesmí být NULL.
 */
extern void outfmt_kv_str ( st_OUTFMT_CTX *ctx, const char *key, const char *value );


/**
 * @brief Vypíše klíč-hodnota pár (celé číslo) na kořenové úrovni.
 *
 * Pro JSON: "key": value.
 * Pro CSV: řádek "key,value"; při prvním volání se automaticky vypíše
 * hlavička "key,value".
 * Pro TEXT je NOP.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota.
 */
extern void outfmt_kv_int ( st_OUTFMT_CTX *ctx, const char *key, long value );


/**
 * @brief Vypíše klíč-hodnota pár (unsigned celé číslo) na kořenové úrovni.
 *
 * Pro JSON: "key": value.
 * Pro CSV: řádek "key,value"; při prvním volání se automaticky vypíše
 * hlavička "key,value".
 * Pro TEXT je NOP.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota.
 */
extern void outfmt_kv_uint ( st_OUTFMT_CTX *ctx, const char *key, unsigned long value );


/**
 * @brief Začátek pojmenovaného pole (pro seznamy souborů apod.).
 *
 * Pro JSON: "key": [. Pro CSV a TEXT je NOP.
 *
 * @param[in,out] ctx Kontext. Nesmí být NULL.
 * @param[in]     key Název pole. Nesmí být NULL.
 */
extern void outfmt_array_begin ( st_OUTFMT_CTX *ctx, const char *key );


/**
 * @brief Konec pole.
 *
 * Pro JSON: ]. Pro CSV a TEXT je NOP.
 *
 * @param[in,out] ctx Kontext. Nesmí být NULL.
 */
extern void outfmt_array_end ( st_OUTFMT_CTX *ctx );


/**
 * @brief Začátek jedné položky v poli.
 *
 * Pro JSON: otevře { (s čárkou pokud nejde o první položku).
 * Pro CSV: resetuje field_count pro nový řádek.
 * Pro TEXT: NOP.
 *
 * @param[in,out] ctx Kontext. Nesmí být NULL.
 */
extern void outfmt_item_begin ( st_OUTFMT_CTX *ctx );


/**
 * @brief Konec jedné položky v poli.
 *
 * Pro JSON: zavře }. Pro CSV: tiskne newline. Pro TEXT: NOP.
 *
 * @param[in,out] ctx Kontext. Nesmí být NULL.
 */
extern void outfmt_item_end ( st_OUTFMT_CTX *ctx );


/**
 * @brief Vypíše řetězcové pole uvnitř položky.
 *
 * Pro JSON: "key": "value" (s čárkou pokud nejde o první pole).
 * Pro CSV: value (s čárkou pokud nejde o první pole). Escapuje uvozovky.
 * Pro TEXT: NOP.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota. Nesmí být NULL.
 */
extern void outfmt_field_str ( st_OUTFMT_CTX *ctx, const char *key, const char *value );


/**
 * @brief Vypíše celočíselné pole (signed) uvnitř položky.
 *
 * Pro JSON: "key": value. Pro CSV: value. Pro TEXT: NOP.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota.
 */
extern void outfmt_field_int ( st_OUTFMT_CTX *ctx, const char *key, long value );


/**
 * @brief Vypíše celočíselné pole (unsigned) uvnitř položky.
 *
 * Pro JSON: "key": value. Pro CSV: value. Pro TEXT: NOP.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota.
 */
extern void outfmt_field_uint ( st_OUTFMT_CTX *ctx, const char *key, unsigned long value );


/**
 * @brief Vypíše hexadecimální pole (16-bit) uvnitř položky.
 *
 * Formát: "0xHHHH". Pro JSON jako řetězec, pro CSV jako hodnota.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota.
 */
extern void outfmt_field_hex16 ( st_OUTFMT_CTX *ctx, const char *key, uint16_t value );


/**
 * @brief Vypíše hexadecimální pole (8-bit) uvnitř položky.
 *
 * Formát: "0xHH". Pro JSON jako řetězec, pro CSV jako hodnota.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota.
 */
extern void outfmt_field_hex8 ( st_OUTFMT_CTX *ctx, const char *key, uint8_t value );


/**
 * @brief Vypíše booleovské pole uvnitř položky.
 *
 * Pro JSON: "key": true/false. Pro CSV: 0/1. Pro TEXT: NOP.
 *
 * @param[in,out] ctx   Kontext. Nesmí být NULL.
 * @param[in]     key   Klíč. Nesmí být NULL.
 * @param[in]     value Hodnota (0 = false, nenulová = true).
 */
extern void outfmt_field_bool ( st_OUTFMT_CTX *ctx, const char *key, int value );


/**
 * @brief Nastaví CSV hlavičku.
 *
 * Pro CSV vypíše řádek s názvy sloupců. Pro JSON a TEXT je NOP.
 * Musí se volat před prvním outfmt_item_begin().
 *
 * @param[in,out] ctx     Kontext. Nesmí být NULL.
 * @param[in]     headers Pole názvů sloupců. Nesmí být NULL.
 * @param[in]     count   Počet sloupců.
 */
extern void outfmt_csv_header ( st_OUTFMT_CTX *ctx, const char *headers[], int count );


/**
 * @brief Vrátí verzi knihovny.
 *
 * @return Ukazatel na statický řetězec s verzí.
 */
extern const char* output_format_version ( void );


#endif /* OUTPUT_FORMAT_H */
