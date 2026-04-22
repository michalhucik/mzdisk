/**
 * @file panel_raw_io.c
 * @brief Logika Raw I/O okna - export/import sektorů/bloků do/ze souboru.
 *
 * Implementuje Get (export z disku do souboru) a Put (import ze souboru
 * na disk) operace přes rozsah sektorů definovaný v Track/Sector
 * nebo Block režimu. Používá exportované funkce z panel_hexdump
 * (panel_hexdump_advance_sectors, panel_hexdump_get_track_params)
 * pro navigaci po sektorech.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "panels/panel_raw_io.h"
#include "libs/dsk/dsk.h"


void panel_raw_io_init ( st_PANEL_RAW_IO_DATA *data )
{
    if ( !data ) return;
    memset ( data, 0, sizeof ( *data ) );
    data->sector_count = 1;
    data->block_count = 1;
    data->start_sector = 1;
    data->block_config.origin_sector = 1;
    data->block_config.sectors_per_block = 1;
    /* Put: implicitní režim je "celý soubor" - typický use case je
       nahrát kompletní dump bez nutnosti počítat sektory. Pro dílčí
       zápis uživatel checkbox zruší. */
    data->put_whole_file = true;
}


void panel_raw_io_open_from_hexdump ( st_PANEL_RAW_IO_DATA *data,
                                       const st_PANEL_HEXDUMP_DATA *hd,
                                       en_RAW_IO_ACTION action )
{
    if ( !data || !hd ) return;

    data->action = action;
    data->addr_mode = hd->addr_mode;
    data->invert = hd->invert;
    data->max_track = hd->max_track;

    /* převzít blokovou konfiguraci */
    data->block_config = hd->block_config;
    data->sector_order = hd->block_config.sector_order;

    if ( hd->addr_mode == HEXDUMP_ADDR_BLOCK ) {
        data->start_block = hd->block;
        data->block_count = 1;
    } else {
        data->start_track = hd->track;
        data->start_sector = hd->sector;
        data->sector_count = 1;
    }

    data->byte_offset = 0;
    data->byte_count = 0;
    data->file_offset = 0;

    /* vyčistit UI stav */
    data->show_error = false;
    data->error_msg[0] = '\0';
    data->show_success = false;
    data->success_msg[0] = '\0';
    data->show_put_confirm = false;

    data->is_open = true;
}


/**
 * @brief Resolvuje startovní track/sector z parametrů operace.
 *
 * V T/S režimu vrátí přímo start_track/start_sector.
 * V Block režimu přepočítá start_block na track/sector pomocí
 * blokové konfigurace a panel_hexdump_advance_sectors().
 *
 * @param data Datový model s parametry.
 * @param disc Otevřený disk.
 * @param[out] out_track Výstupní stopa.
 * @param[out] out_sector Výstupní sektor ID (1-based).
 * @param[out] out_total_sectors Celkový počet sektorů k přenesení.
 * @return true při úspěchu, false pokud pozice je mimo rozsah.
 */
static bool resolve_start_position ( const st_PANEL_RAW_IO_DATA *data,
                                      st_MZDSK_DISC *disc,
                                      uint16_t *out_track,
                                      uint16_t *out_sector,
                                      int32_t *out_total_sectors )
{
    if ( data->addr_mode == HEXDUMP_ADDR_BLOCK ) {
        /* blokový režim: přepočítat start_block na T/S */
        const st_HEXDUMP_BLOCK_CONFIG *cfg = &data->block_config;
        int32_t sector_offset = ( data->start_block - cfg->first_block )
                                 * (int32_t) cfg->sectors_per_block;
        if ( sector_offset < 0 ) return false;

        uint16_t t = cfg->origin_track;
        uint16_t s = cfg->origin_sector;

        if ( sector_offset > 0 ) {
            if ( !panel_hexdump_advance_sectors ( disc, data->sector_order,
                                                   &t, &s, (int) sector_offset,
                                                   data->max_track ) ) {
                return false;
            }
        }

        *out_track = t;
        *out_sector = s;
        *out_total_sectors = data->block_count * (int32_t) cfg->sectors_per_block;
    } else {
        /* T/S režim: přímo */
        *out_track = data->start_track;
        *out_sector = data->start_sector;
        *out_total_sectors = data->sector_count;
    }

    return true;
}


en_MZDSK_RES panel_raw_io_execute_get ( st_PANEL_RAW_IO_DATA *data,
                                         st_MZDSK_DISC *disc )
{
    if ( !data || !disc ) return MZDSK_RES_DSK_ERROR;

    data->show_error = false;
    data->show_success = false;

    /* resolvovat startovní pozici */
    uint16_t cur_track, cur_sector;
    int32_t total_sectors;

    if ( !resolve_start_position ( data, disc, &cur_track, &cur_sector, &total_sectors ) ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Start position out of range" );
        data->show_error = true;
        return MZDSK_RES_DSK_ERROR;
    }

    if ( total_sectors <= 0 ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Sector count must be positive" );
        data->show_error = true;
        return MZDSK_RES_DSK_ERROR;
    }

    if ( data->filepath[0] == '\0' ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "No file path specified" );
        data->show_error = true;
        return MZDSK_RES_DSK_ERROR;
    }

    /* otevřít výstupní soubor */
    FILE *f = fopen ( data->filepath, "r+b" );
    if ( !f ) {
        f = fopen ( data->filepath, "wb" );
    }
    if ( !f ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot open file for writing: %.400s", data->filepath );
        data->show_error = true;
        return MZDSK_RES_DSK_ERROR;
    }

    /* file_offset: seeknout, doplnit nuly pokud soubor kratší */
    if ( data->file_offset > 0 ) {
        fseek ( f, 0, SEEK_END );
        long file_size = ftell ( f );

        if ( (long) data->file_offset > file_size ) {
            /* doplnit nuly */
            long gap = (long) data->file_offset - file_size;
            uint8_t zero = 0;
            fseek ( f, 0, SEEK_END );
            for ( long i = 0; i < gap; i++ ) {
                fwrite ( &zero, 1, 1, f );
            }
        }

        fseek ( f, (long) data->file_offset, SEEK_SET );
    } else {
        fseek ( f, 0, SEEK_SET );
    }

    /* smyčka přes sektory */
    int32_t bytes_written = 0;
    int32_t byte_limit = data->byte_count; /* 0 = neomezeno */
    uint8_t sector_buf[PANEL_HEXDUMP_MAX_SECTOR_SIZE];

    for ( int32_t i = 0; i < total_sectors; i++ ) {
        /* zjistit velikost sektoru na aktuální stopě */
        uint16_t trk_sectors, trk_ssize;
        panel_hexdump_get_track_params ( disc, cur_track, &trk_sectors, &trk_ssize );

        if ( trk_sectors == 0 ) {
            snprintf ( data->error_msg, sizeof ( data->error_msg ),
                       "Track %d has no sectors", cur_track );
            data->show_error = true;
            fclose ( f );
            return MZDSK_RES_DSK_ERROR;
        }

        /* Audit L-27: runtime kontrola cur_track/cur_sector > 255 před
           oříznutím do uint8_t pro dsk_read_sector(). */
        if ( cur_track > 255 || cur_sector > 255 ) {
            snprintf ( data->error_msg, sizeof ( data->error_msg ),
                       "Track/sector %d/%d exceeds uint8_t range",
                       cur_track, cur_sector );
            data->show_error = true;
            fclose ( f );
            return MZDSK_RES_DSK_ERROR;
        }

        /* přečíst sektor */
        int res = dsk_read_sector ( disc->handler, (uint8_t) cur_track,
                                     (uint8_t) cur_sector, sector_buf );
        if ( res != EXIT_SUCCESS ) {
            snprintf ( data->error_msg, sizeof ( data->error_msg ),
                       "Read error at track %d, sector %d", cur_track, cur_sector );
            data->show_error = true;
            fclose ( f );
            return MZDSK_RES_DSK_ERROR;
        }

        /* aplikovat inverzi */
        if ( data->invert ) {
            for ( uint16_t b = 0; b < trk_ssize; b++ ) {
                sector_buf[b] = (uint8_t) ~sector_buf[b];
            }
        }

        /* určit rozsah bajtů k zapsání z tohoto sektoru */
        uint16_t start_byte = 0;
        uint16_t end_byte = trk_ssize;

        /* byte_offset: přeskočit u prvního sektoru */
        if ( i == 0 && data->byte_offset > 0 ) {
            start_byte = (uint16_t) data->byte_offset;
            if ( start_byte >= trk_ssize ) {
                start_byte = trk_ssize; /* přeskočí celý sektor */
            }
        }

        /* byte_count: omezit celkový počet */
        if ( byte_limit > 0 ) {
            int32_t remaining = byte_limit - bytes_written;
            if ( remaining <= 0 ) break;
            if ( (int32_t) ( end_byte - start_byte ) > remaining ) {
                end_byte = start_byte + (uint16_t) remaining;
            }
        }

        /* zapsat do souboru */
        if ( end_byte > start_byte ) {
            size_t to_write = end_byte - start_byte;
            size_t written = fwrite ( sector_buf + start_byte, 1, to_write, f );
            if ( written != to_write ) {
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "Write error to file" );
                data->show_error = true;
                fclose ( f );
                return MZDSK_RES_DSK_ERROR;
            }
            bytes_written += (int32_t) to_write;
        }

        /* posun na další sektor */
        if ( i + 1 < total_sectors ) {
            if ( !panel_hexdump_advance_sectors ( disc, data->sector_order,
                                                   &cur_track, &cur_sector,
                                                   1, data->max_track ) ) {
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "Reached end of disk at track %d, sector %d",
                           cur_track, cur_sector );
                data->show_error = true;
                fclose ( f );
                return MZDSK_RES_DSK_ERROR;
            }
        }
    }

    fclose ( f );

    snprintf ( data->success_msg, sizeof ( data->success_msg ),
               "Exported %d bytes to file", bytes_written );
    data->show_success = true;

    return MZDSK_RES_OK;
}


en_MZDSK_RES panel_raw_io_execute_put ( st_PANEL_RAW_IO_DATA *data,
                                         st_MZDSK_DISC *disc )
{
    if ( !data || !disc ) return MZDSK_RES_DSK_ERROR;

    data->show_error = false;
    data->show_success = false;

    /* resolvovat startovní pozici */
    uint16_t cur_track, cur_sector;
    int32_t total_sectors;

    if ( !resolve_start_position ( data, disc, &cur_track, &cur_sector, &total_sectors ) ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Start position out of range" );
        data->show_error = true;
        return MZDSK_RES_DSK_ERROR;
    }

    if ( total_sectors <= 0 ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Sector count must be positive" );
        data->show_error = true;
        return MZDSK_RES_DSK_ERROR;
    }

    if ( data->filepath[0] == '\0' ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "No file path specified" );
        data->show_error = true;
        return MZDSK_RES_DSK_ERROR;
    }

    /* otevřít vstupní soubor */
    FILE *f = fopen ( data->filepath, "rb" );
    if ( !f ) {
        snprintf ( data->error_msg, sizeof ( data->error_msg ),
                   "Cannot open file for reading: %.400s", data->filepath );
        data->show_error = true;
        return MZDSK_RES_DSK_ERROR;
    }

    /* zjistit velikost souboru (pro put_whole_file režim a pro kontrolu
       offsetu), pak seeknout na file_offset */
    fseek ( f, 0, SEEK_END );
    long file_size = ftell ( f );
    if ( data->file_offset > 0 ) {
        if ( (long) data->file_offset > file_size ) {
            /* offset za koncem souboru - vše bude nulové */
            fseek ( f, 0, SEEK_END );
        } else {
            fseek ( f, (long) data->file_offset, SEEK_SET );
        }
    } else {
        fseek ( f, 0, SEEK_SET );
    }

    /* smyčka přes sektory */
    int32_t bytes_written = 0;
    int32_t byte_limit = data->byte_count; /* 0 = neomezeno */
    uint8_t sector_buf[PANEL_HEXDUMP_MAX_SECTOR_SIZE];

    /* "Whole file" režim: override byte_limit na přesnou velikost zbytku
       souboru od file_offset, total_sectors na velký horní odhad (smyčka
       se sama ukončí přes break při dosažení byte_limit). */
    if ( data->put_whole_file ) {
        long remaining_in_file = file_size - (long) data->file_offset;
        if ( remaining_in_file < 0 ) remaining_in_file = 0;
        byte_limit = (int32_t) remaining_in_file;
        /* total_sectors už nemá omezovat - dáme dost velký upper bound;
           při vyčerpání disku smyčka spadne na "Reached end of disk". */
        total_sectors = 1 << 24;

        /* Prázdný soubor (nebo file_offset za koncem) - neběží smyčka,
           jinak by byte_limit=0 zapisovalo plné sektory (podmínka "0 =
           neomezeno" se by na to chytla). */
        if ( byte_limit == 0 ) {
            fclose ( f );
            snprintf ( data->success_msg, sizeof ( data->success_msg ),
                       "Imported 0 bytes from file (empty)" );
            data->show_success = true;
            return MZDSK_RES_OK;
        }
    }

    for ( int32_t i = 0; i < total_sectors; i++ ) {
        /* zjistit velikost sektoru na aktuální stopě */
        uint16_t trk_sectors, trk_ssize;
        panel_hexdump_get_track_params ( disc, cur_track, &trk_sectors, &trk_ssize );

        if ( trk_sectors == 0 ) {
            snprintf ( data->error_msg, sizeof ( data->error_msg ),
                       "Track %d has no sectors", cur_track );
            data->show_error = true;
            fclose ( f );
            return MZDSK_RES_DSK_ERROR;
        }

        /* přečíst existující sektor (pro partial write) */
        int res = dsk_read_sector ( disc->handler, (uint8_t) cur_track,
                                     (uint8_t) cur_sector, sector_buf );
        if ( res != EXIT_SUCCESS ) {
            /* sektor ještě neexistuje - vynulovat */
            memset ( sector_buf, 0, trk_ssize );
        }

        /* určit rozsah bajtů k zápisu do tohoto sektoru */
        uint16_t start_byte = 0;
        uint16_t end_byte = trk_ssize;

        /* byte_offset: u prvního sektoru zapsat od offsetu */
        if ( i == 0 && data->byte_offset > 0 ) {
            start_byte = (uint16_t) data->byte_offset;
            if ( start_byte >= trk_ssize ) {
                start_byte = trk_ssize;
            }
        }

        /* byte_count: omezit celkový počet */
        if ( byte_limit > 0 ) {
            int32_t remaining = byte_limit - bytes_written;
            if ( remaining <= 0 ) break;
            if ( (int32_t) ( end_byte - start_byte ) > remaining ) {
                end_byte = start_byte + (uint16_t) remaining;
            }
        }

        /* přečíst data ze souboru do sektoru */
        if ( end_byte > start_byte ) {
            size_t to_read = end_byte - start_byte;
            size_t read_count = fread ( sector_buf + start_byte, 1, to_read, f );

            /* pokud soubor kratší, zbytek je nulový */
            if ( read_count < to_read ) {
                memset ( sector_buf + start_byte + read_count, 0,
                         to_read - read_count );
            }

            bytes_written += (int32_t) to_read;
        }

        /* aplikovat inverzi */
        if ( data->invert ) {
            for ( uint16_t b = 0; b < trk_ssize; b++ ) {
                sector_buf[b] = (uint8_t) ~sector_buf[b];
            }
        }

        /* zapsat sektor na disk */
        res = dsk_write_sector ( disc->handler, (uint8_t) cur_track,
                                  (uint8_t) cur_sector, sector_buf );
        if ( res != EXIT_SUCCESS ) {
            snprintf ( data->error_msg, sizeof ( data->error_msg ),
                       "Write error at track %d, sector %d", cur_track, cur_sector );
            data->show_error = true;
            fclose ( f );
            return MZDSK_RES_DSK_ERROR;
        }

        /* posun na další sektor */
        if ( i + 1 < total_sectors ) {
            if ( !panel_hexdump_advance_sectors ( disc, data->sector_order,
                                                   &cur_track, &cur_sector,
                                                   1, data->max_track ) ) {
                snprintf ( data->error_msg, sizeof ( data->error_msg ),
                           "Reached end of disk at track %d, sector %d",
                           cur_track, cur_sector );
                data->show_error = true;
                fclose ( f );
                return MZDSK_RES_DSK_ERROR;
            }
        }
    }

    fclose ( f );

    snprintf ( data->success_msg, sizeof ( data->success_msg ),
               "Imported %d bytes from file", bytes_written );
    data->show_success = true;

    return MZDSK_RES_OK;
}
