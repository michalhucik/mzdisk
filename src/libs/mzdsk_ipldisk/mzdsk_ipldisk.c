/**
 * @file   mzdsk_ipldisk.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Implementace souborového systému FSMZ (MZ-BASIC disk / IPLDISK).
 *
 * Implementuje nízkoúrovňové operace nad souborovým systémem FSMZ:
 * čtení/zápis alokačních bloků, správu IPLPRO a DINFO bloků,
 * operace s adresářem a souborovými daty.
 *
 * Popis MZ800_FS:
 *
 * Logický záznam na disku je invertovaný, z tohoto důvodu je
 * potřeba označení strany a samotná data enkódovat pomocí CPL.
 *
 * Celý disk má jednotný fyzický formát: 16 sektorů po 256 bajtech.
 *
 * Logický formát je rozdělen na alokační bloky. Jeden alokační blok odpovídá
 * velikosti jednoho fyzického sektoru.
 *
 * Alokační blok č. 0 má fyzické umístění - track: 1, sector: 1.
 * Alokační blok č. 1 má fyzické umístění - track: 1, sector: 2.
 * Alokační blok č. 15 má fyzické umístění - track: 1, sector: 16.
 * Alokační blok č. 16 má fyzické umístění - track: 0, sector: 1.
 *
 * Disk s dvoustranným formátem 80 stop: 80 * 2 * 16 = 2560 alokačních bloků.
 *
 * 1. fyzická stopa (MZFS BOOT):
 * Alokační blok 0 obsahuje bootstrap header (st_FSMZ_IPLPRO_BLOCK).
 * Alokační bloky 1-14 mohou být použity pro krátký bootstrap loader
 * (max. 3 584 bajtů). Alokační blok 15 obsahuje disk info a bitmapu
 * alokace (st_FSMZ_DINFO_BLOCK).
 *
 * 0. fyzická stopa:
 * Alokační bloky 16-23 (resp. 16-31 u IPLDISK) obsahují adresář.
 * Od farea (typicky blok 48) začíná souborová oblast.
 *
 * Port z fstool/fsmz.c. Z80 asm kód odstraněn.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2015-2026 Michal Hucik <hucik@ordoz.com>
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

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mzdsk_ipldisk.h"
#include "libs/dsk/dsk.h"
#include "libs/endianity/endianity.h"


/**
 * @brief Přepočet FSMZ alokačního bloku na logickou stopu a fyzický sektor.
 *
 * Formát používá obrácenou logiku strany, proto musíme udělat konverzi:
 *
 * stopa  logická stopa
 * ==========================
 * 0      1
 * 1      0
 * 2      3
 * 3      2
 * ...
 *
 * Stopu vracíme v horním bajtu a sektor ve spodním.
 *
 * @param block Číslo alokačního bloku.
 * @return Horní bajt = stopa, spodní bajt = sektor (1-based).
 */
uint16_t fsmz_block2trsec ( uint16_t block ) {
    uint16_t trsec;

    /* Audit L-3: block << 4 wrapuje v uint16_t pro block >= 4096.
       Maximální FSMZ disk má 80 stop * 16 sektorů = 1280 bloků
       (nebo 160 stop * 16 = 2560 u MZ-800 double sided), takže
       validní FSMZ data nikdy nedosáhnou hranice. Clamp chrání
       při propagaci korupt metadat. */
    if ( block >= 4096 ) {
        return 0; /* invalid marker */
    }

    trsec = ( block << 4 ) & 0xff00; /* stopa */

    /* obrátíme stranu */
    if ( trsec & 0x0100 ) {
        trsec -= 0x0100;
    } else {
        trsec += 0x0100;
    };

    trsec += ( (uint8_t) block & 0x0f ) + 1; /* sektor */

    return trsec;
}


/**
 * @brief Přepočet stopy a sektoru na číslo alokačního bloku.
 *
 * @param track Číslo stopy.
 * @param sector Číslo sektoru (1-based).
 * @return Číslo alokačního bloku.
 */
uint16_t fsmz_trsec2block ( uint8_t track, uint8_t sector ) {
    uint16_t block = ( track * FSMZ_SECTORS_ON_TRACK ) + sector - 1;
    return block;
}


/**
 * @brief Přečte nebo zapíše jeden FSMZ alokační blok.
 *
 * Interně převede číslo bloku na stopu a sektor pomocí fsmz_block2trsec
 * a deleguje na mzdsk_disc_read_sector resp. mzdsk_disc_write_sector.
 *
 * @param ioop Směr operace (FSMZ_IOOP_READ nebo FSMZ_IOOP_WRITE).
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param block Číslo alokačního bloku.
 * @param dma Datový buffer (min. FSMZ_SECTOR_SIZE bajtů).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_rw_block ( en_FSMZ_IOOP ioop, st_MZDSK_DISC *disc, uint16_t block, void* dma ) {
    uint8_t track;
    uint8_t sector;
    uint16_t trsec;

    trsec = fsmz_block2trsec ( block );
    track = (uint8_t) ( ( trsec >> 8 ) & 0xff );
    sector = (uint8_t) trsec & 0xff;

    if ( FSMZ_IOOP_READ == ioop ) {
        return mzdsk_disc_read_sector ( disc, track, sector, dma );
    };
    return mzdsk_disc_write_sector ( disc, track, sector, dma );
}


/**
 * @brief Přečte IPLPRO blok (alokační blok 0) a provede endianity korekci.
 *
 * Načte alokační blok 0 do struktury st_FSMZ_IPLPRO_BLOCK a konvertuje
 * vícebajtová pole (fsize, fstrt, fexec, block) z little-endian na
 * nativní byte order hostitele.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param iplpro Výstupní buffer pro IPLPRO blok.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_read_iplpro ( st_MZDSK_DISC *disc, st_FSMZ_IPLPRO_BLOCK *iplpro ) {
    en_MZDSK_RES err;
    err = fsmz_read_blocks ( disc, FSMZ_ALOCBLOCK_IPLPRO, sizeof ( st_FSMZ_IPLPRO_BLOCK ), iplpro );
    if ( err ) return err;
    iplpro->fsize = endianity_bswap16_LE ( iplpro->fsize );
    iplpro->fstrt = endianity_bswap16_LE ( iplpro->fstrt );
    iplpro->fexec = endianity_bswap16_LE ( iplpro->fexec );
    iplpro->block = endianity_bswap16_LE ( iplpro->block );
    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše IPLPRO blok do alokačního bloku 0 s endianity korekcí.
 *
 * Vytvoří lokální kopii vstupní struktury, konvertuje vícebajtová pole
 * do little-endian a zapíše na disk. Vstupní struktura se nemodifikuje.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param iplpro Ukazatel na IPLPRO blok k zápisu.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fs_mz_write_iplpro ( st_MZDSK_DISC *disc, st_FSMZ_IPLPRO_BLOCK *iplpro ) {
    st_FSMZ_IPLPRO_BLOCK iplpro_LE;
    memcpy ( &iplpro_LE, iplpro, sizeof ( st_FSMZ_IPLPRO_BLOCK ) );
    iplpro_LE.fsize = endianity_bswap16_LE ( iplpro_LE.fsize );
    iplpro_LE.fstrt = endianity_bswap16_LE ( iplpro_LE.fstrt );
    iplpro_LE.fexec = endianity_bswap16_LE ( iplpro_LE.fexec );
    iplpro_LE.block = endianity_bswap16_LE ( iplpro_LE.block );
    return fsmz_write_blocks ( disc, FSMZ_ALOCBLOCK_IPLPRO, sizeof ( st_FSMZ_IPLPRO_BLOCK ), &iplpro_LE );
}


/**
 * @brief Přečte DINFO blok (alokační blok 15) a provede endianity korekci.
 *
 * Načte alokační blok 15 do struktury st_FSMZ_DINFO_BLOCK a konvertuje
 * vícebajtová pole (used, blocks) z little-endian na nativní byte order.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param dinfo Výstupní buffer pro DINFO blok.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_read_dinfo ( st_MZDSK_DISC *disc, st_FSMZ_DINFO_BLOCK *dinfo ) {
    en_MZDSK_RES err;
    err = fsmz_read_blocks ( disc, FSMZ_ALOCBLOCK_DINFO, sizeof ( st_FSMZ_DINFO_BLOCK ), dinfo );
    if ( err ) return err;
    dinfo->used = endianity_bswap16_LE ( dinfo->used );
    dinfo->blocks = endianity_bswap16_LE ( dinfo->blocks );
    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše DINFO blok do alokačního bloku 15 s endianity korekcí.
 *
 * Vytvoří lokální kopii vstupní struktury, konvertuje vícebajtová pole
 * do little-endian a zapíše na disk. Vstupní struktura se nemodifikuje.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param dinfo Ukazatel na DINFO blok k zápisu.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_write_dinfo ( st_MZDSK_DISC *disc, st_FSMZ_DINFO_BLOCK *dinfo ) {
    st_FSMZ_DINFO_BLOCK dinfo_LE;
    memcpy ( &dinfo_LE, dinfo, sizeof ( st_FSMZ_DINFO_BLOCK ) );
    dinfo_LE.used = endianity_bswap16_LE ( dinfo_LE.used );
    dinfo_LE.blocks = endianity_bswap16_LE ( dinfo_LE.blocks );
    return fsmz_write_blocks ( disc, FSMZ_ALOCBLOCK_DINFO, sizeof ( st_FSMZ_DINFO_BLOCK ), &dinfo_LE );
}


/**
 * @brief Interní funkce pro načtení jednoho bloku adresáře.
 *
 * Načte alokační blok do disc->cache a nastaví dir->dir_bl na tuto cache.
 * Na big-endian systémech provede byte-swap vícebajtových polí položek.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param dir Struktura adresáře (bude aktualizována).
 * @param block Číslo alokačního bloku adresáře.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES fsmz_internal_read_dir_block ( st_MZDSK_DISC *disc, st_FSMZ_DIR *dir, uint16_t block ) {
    en_MZDSK_RES err;
    err = fsmz_read_block ( disc, block, disc->cache );
    if ( err ) return err;
    dir->dir_bl = (st_FSMZ_DIR_BLOCK*) disc->cache;
#ifdef __BYTE_ORDER
#if __BYTE_ORDER == __BIG_ENDIAN
    int i;
    for ( i = 0; i < FSMZ_DIRITEMS_PER_BLOCK; i++ ) {
        dir->dir_bl->item[i].fsize = endianity_bswap16_LE ( dir->dir_bl->item[i].fsize );
        dir->dir_bl->item[i].fstrt = endianity_bswap16_LE ( dir->dir_bl->item[i].fstrt );
        dir->dir_bl->item[i].fexec = endianity_bswap16_LE ( dir->dir_bl->item[i].fexec );
        dir->dir_bl->item[i].block = endianity_bswap16_LE ( dir->dir_bl->item[i].block );
    };
#endif
#else
    /* pokud neznáme __BYTE_ORDER, detekujeme za běhu */
    uint16_t ui16 = 0x0001;
    uint8_t *ui8 = ( uint8_t* ) & ui16;
    if ( *ui8 != 0x01 ) {
        int i;
        for ( i = 0; i < FSMZ_DIRITEMS_PER_BLOCK; i++ ) {
            dir->dir_bl->item[i].fsize = endianity_bswap16_LE ( dir->dir_bl->item[i].fsize );
            dir->dir_bl->item[i].fstrt = endianity_bswap16_LE ( dir->dir_bl->item[i].fstrt );
            dir->dir_bl->item[i].fexec = endianity_bswap16_LE ( dir->dir_bl->item[i].fexec );
            dir->dir_bl->item[i].block = endianity_bswap16_LE ( dir->dir_bl->item[i].block );
        };
    };
#endif
    return MZDSK_RES_OK;
}


/**
 * @brief Interní funkce pro zápis jednoho bloku adresáře s endianity korekcí.
 *
 * Vytvoří lokální kopii bloku adresáře, provede endianity korekci
 * a zapíše na disk.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param block Číslo alokačního bloku adresáře.
 * @param dir_bl Ukazatel na blok adresáře k zápisu.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
static en_MZDSK_RES fsmz_internal_write_dir_block ( st_MZDSK_DISC *disc, uint16_t block, st_FSMZ_DIR_BLOCK *dir_bl ) {
    st_FSMZ_DIR_BLOCK dir_bl_LE;

    memcpy ( &dir_bl_LE, dir_bl, sizeof ( st_FSMZ_DIR_BLOCK ) );

#ifdef __BYTE_ORDER
#if __BYTE_ORDER == __BIG_ENDIAN
    int i;
    for ( i = 0; i < FSMZ_DIRITEMS_PER_BLOCK; i++ ) {
        dir_bl_LE.item[i].fsize = endianity_bswap16_LE ( dir_bl_LE.item[i].fsize );
        dir_bl_LE.item[i].fstrt = endianity_bswap16_LE ( dir_bl_LE.item[i].fstrt );
        dir_bl_LE.item[i].fexec = endianity_bswap16_LE ( dir_bl_LE.item[i].fexec );
        dir_bl_LE.item[i].block = endianity_bswap16_LE ( dir_bl_LE.item[i].block );
    };
#endif
#else
    /* pokud neznáme __BYTE_ORDER, detekujeme za běhu */
    uint16_t ui16 = 0x0001;
    uint8_t *ui8 = ( uint8_t* ) & ui16;
    if ( *ui8 != 0x01 ) {
        int i;
        for ( i = 0; i < FSMZ_DIRITEMS_PER_BLOCK; i++ ) {
            dir_bl_LE.item[i].fsize = endianity_bswap16_LE ( dir_bl_LE.item[i].fsize );
            dir_bl_LE.item[i].fstrt = endianity_bswap16_LE ( dir_bl_LE.item[i].fstrt );
            dir_bl_LE.item[i].fexec = endianity_bswap16_LE ( dir_bl_LE.item[i].fexec );
            dir_bl_LE.item[i].block = endianity_bswap16_LE ( dir_bl_LE.item[i].block );
        };
    };
#endif

    return fsmz_write_block ( disc, block, &dir_bl_LE );
}


/**
 * @brief Otevře adresář - načte první blok (alokační blok 16).
 *
 * Nastaví pozici na 1, čímž přeskočí rezervovanou první položku
 * (32 bajtů s obsahem 0x80, 0x01, 0x00...).
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param dir Výstupní struktura adresáře.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_open_dir ( st_MZDSK_DISC *disc, st_FSMZ_DIR *dir ) {
    dir->position = 1;
    return fsmz_internal_read_dir_block ( disc, dir, FSMZ_ALOCBLOCK_DIR );
}


/**
 * @brief Načte blok adresáře obsahující zadanou pozici položky.
 *
 * Vypočítá, ve kterém alokačním bloku se nachází požadovaná pozice,
 * načte tento blok a nastaví dir->position.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param dir Struktura adresáře (bude aktualizována).
 * @param dir_position Index položky adresáře.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_NOT_FOUND pokud
 *         pozice přesahuje limit, jinak chybový kód.
 */
en_MZDSK_RES fsmz_read_dirblock_with_diritem_position ( st_MZDSK_DISC *disc, st_FSMZ_DIR *dir, uint8_t dir_position, uint8_t fsmz_dir_items ) {
    /* Pozn. k auditu M-2: `fsmz_dir_items` je v této knihovně 1-based
     * maximální pozice (63 pro FSMZ_MAX_DIR_ITEMS, 127 pro IPLDISK),
     * ne count. `fsmz_read_dir` iteruje postupně s inkrementem
     * `position++` a potřebuje, aby volání s `position == dir_items`
     * prošlo (to je poslední validní slot). Striktní `>=` zablokuje
     * normální iteraci na koncové pozici - viz test_fsmz_defrag.
     * Ochrana proti OOB bloku se řeší na straně volajícího. */
    if ( dir_position > fsmz_dir_items ) return MZDSK_RES_FILE_NOT_FOUND;
    en_MZDSK_RES err = fsmz_internal_read_dir_block ( disc, dir, FSMZ_ALOCBLOCK_DIR + ( dir_position >> 3 ) );
    if ( err ) return err;
    dir->position = dir_position;
    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše aktuální blok adresáře zpět na disk.
 *
 * Blok je určen aktuální pozicí v dir. Provede endianity korekci
 * před zápisem.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param dir Struktura adresáře s aktuální pozicí.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_NOT_FOUND pokud
 *         pozice přesahuje limit, jinak chybový kód.
 */
en_MZDSK_RES fsmz_write_dirblock ( st_MZDSK_DISC *disc, st_FSMZ_DIR *dir, uint8_t fsmz_dir_items ) {
    if ( dir->position > fsmz_dir_items ) return MZDSK_RES_FILE_NOT_FOUND;
    return fsmz_internal_write_dir_block ( disc, FSMZ_ALOCBLOCK_DIR + ( dir->position >> 3 ), dir->dir_bl );
}


/**
 * @brief Uloží jednu položku adresáře na zadanou pozici.
 *
 * Načte příslušný blok adresáře, zkopíruje položku na pozici
 * diritem_position a blok zapíše zpět na disk.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param diritem Položka adresáře k uložení.
 * @param diritem_position Pozice v adresáři (1-63 resp. 1-127).
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_save_diritem_to_position ( st_MZDSK_DISC *disc, st_FSMZ_DIR_ITEM *diritem, uint8_t diritem_position, uint8_t fsmz_dir_items ) {
    st_FSMZ_DIR dir;
    en_MZDSK_RES err = fsmz_read_dirblock_with_diritem_position ( disc, &dir, diritem_position, fsmz_dir_items );
    if ( err ) return err;
    st_FSMZ_DIR_ITEM *diritem_dst = &dir.dir_bl->item[ ( diritem_position ) & 0x07 ];
    memcpy ( diritem_dst, diritem, sizeof ( st_FSMZ_DIR_ITEM ) );
    return fsmz_write_dirblock ( disc, &dir, fsmz_dir_items );
}


/**
 * @brief Sekvenční čtení další položky adresáře.
 *
 * Standardní hodnota fsmz_dir_items je 63 pro MZ-800 Disc BASIC.
 * IPLDISK pracuje s adresářem o velikosti 16 sektorů a fsmz_dir_items je 127.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param dir Struktura adresáře (pozice se inkrementuje).
 * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
 * @param[out] res Výstupní chybový kód: MZDSK_RES_OK = OK,
 *                 MZDSK_RES_FILE_NOT_FOUND = konec adresáře, záporné = chyba.
 * @return Ukazatel na položku adresáře, nebo NULL při chybě/konci.
 *
 * @warning Vrácený ukazatel směřuje do disc->cache a je platný pouze
 *          do dalšího čtení sektoru.
 */
st_FSMZ_DIR_ITEM* fsmz_read_dir ( st_MZDSK_DISC *disc, st_FSMZ_DIR *dir, uint8_t fsmz_dir_items, en_MZDSK_RES *res ) {
    st_FSMZ_DIR_ITEM *diritem;

    if ( dir->position > fsmz_dir_items ) {
        *res = MZDSK_RES_FILE_NOT_FOUND;
        return NULL;
    };

    if ( !( dir->position & 0x07 ) ) {
        *res = fsmz_read_dirblock_with_diritem_position ( disc, dir, dir->position, fsmz_dir_items );
        if ( *res ) return NULL;
    };
    diritem = &dir->dir_bl->item [ dir->position & 0x07 ];
    dir->position++;
    *res = MZDSK_RES_OK;
    return diritem;
}


/**
 * @brief Kontinuální čtení řady po sobě jdoucích alokačních bloků.
 *
 * Čte 'size' bajtů počínaje alokačním blokem 'block'. Pokud 'size'
 * není násobkem FSMZ_SECTOR_SIZE, poslední neúplný blok se čte
 * přes disc->cache.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param block Číslo počátečního alokačního bloku.
 * @param size Počet bajtů ke čtení.
 * @param dst Cílový buffer (musí pojmout alespoň 'size' bajtů).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_read_blocks ( st_MZDSK_DISC *disc, uint16_t block, uint16_t size, void *dst ) {
    en_MZDSK_RES err;

    while ( size ) {
        if ( size < FSMZ_SECTOR_SIZE ) {
            err = fsmz_read_block ( disc, block, disc->cache );
            if ( err ) return err;
            memcpy ( dst, disc->cache, size );
            break;
        } else {
            err = fsmz_read_block ( disc, block++, dst );
            if ( err ) return err;
            size -= FSMZ_SECTOR_SIZE;
            dst = (void *) ( (size_t) dst + FSMZ_SECTOR_SIZE );
        };
    };
    return MZDSK_RES_OK;
}


/**
 * @brief Kontinuální zápis řady po sobě jdoucích alokačních bloků.
 *
 * Zapisuje 'size' bajtů počínaje alokačním blokem 'block'. Pokud 'size'
 * není násobkem FSMZ_SECTOR_SIZE, poslední neúplný blok se doplní
 * nulami přes disc->cache.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param block Číslo počátečního alokačního bloku.
 * @param size Počet bajtů k zápisu.
 * @param src Zdrojový buffer (musí obsahovat alespoň 'size' bajtů).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_write_blocks ( st_MZDSK_DISC *disc, uint16_t block, uint16_t size, void *src ) {
    en_MZDSK_RES err;

    while ( size ) {
        if ( size < FSMZ_SECTOR_SIZE ) {
            memcpy ( disc->cache, src, size );
            memset ( disc->cache + size, 0x00, FSMZ_SECTOR_SIZE - size );
            err = fsmz_write_block ( disc, block, disc->cache );
            if ( err ) return err;
            break;
        } else {
            err = fsmz_write_block ( disc, block++, src );
            if ( err ) return err;
            size -= FSMZ_SECTOR_SIZE;
            src = (void *) ( (size_t) src + FSMZ_SECTOR_SIZE );
        };
    };
    return MZDSK_RES_OK;
}


/**
 * @brief Vyhledá soubor v adresáři podle jména v Sharp MZ ASCII.
 *
 * Standardní hodnota fsmz_dir_items je 63 pro MZ-800 Disc BASIC.
 * IPLDISK pracuje s adresářem o velikosti 16 sektorů a fsmz_dir_items je 127.
 *
 * Pokud je soubor nalezen, vrátí ukazatel na DIR_ITEM a res = MZDSK_RES_OK.
 * Při nenalezeném záznamu res = MZDSK_RES_FILE_NOT_FOUND.
 * Při chybě res < 0.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param mz_fname Jméno souboru v Sharp MZ ASCII (zakončeno 0x0d).
 * @param dir_cache Struktura adresáře pro cachování (může být NULL).
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param[out] res Výstupní chybový kód.
 * @return Ukazatel na nalezenou položku, nebo NULL.
 */
st_FSMZ_DIR_ITEM* fsmz_search_file ( st_MZDSK_DISC *disc, uint8_t *mz_fname, st_FSMZ_DIR *dir_cache, uint8_t fsmz_dir_items, en_MZDSK_RES *res ) {
    st_FSMZ_DIR dir_local_cache;
    st_FSMZ_DIR *dir_p = &dir_local_cache;
    st_FSMZ_DIR_ITEM *diritem;
    en_MZDSK_RES err;

    if ( dir_cache != NULL ) dir_p = dir_cache;

    *res = fsmz_open_dir ( disc, dir_p );
    if ( *res ) return ( NULL );

    while ( 1 ) {
        diritem = fsmz_read_dir ( disc, dir_p, fsmz_dir_items, &err );
        if ( err ) break;

        if ( diritem->ftype ) {
            if ( !mzdsk_mzstrcmp ( mz_fname, diritem->fname ) ) {
                return ( diritem );
            };
        };
    };

    *res = err;
    return ( NULL );
}


/**
 * @brief Přepočet velikosti souboru na počet potřebných alokačních bloků.
 *
 * Interní funkce. Velikost se dělí 256 a zaokrouhluje nahoru.
 *
 * @param size Velikost souboru v bajtech (max. 65535).
 * @return Počet potřebných alokačních bloků.
 */
static uint16_t fsmz_size2blocks ( uint16_t size ) {
    uint16_t blocks = size >> 8;
    if ( size & 0xff ) {
        blocks++;
    };
    return ( blocks );
}


/**
 * @brief Vypočítá velikost v bajtech z počtu alokačních bloků.
 *
 * @param blocks Počet alokačních bloků.
 * @return Velikost v bajtech (blocks * FSMZ_SECTOR_SIZE).
 */
uint32_t fsmz_size_from_blocks ( uint16_t blocks ) {
    return ( (uint32_t) blocks * FSMZ_SECTOR_SIZE );
}


/**
 * @brief Vypočítá počet potřebných alokačních bloků pro zadanou velikost.
 *
 * Zaokrouhluje nahoru - pokud size není násobkem FSMZ_SECTOR_SIZE,
 * přidá jeden blok navíc.
 *
 * @param size Velikost v bajtech.
 * @return Počet potřebných alokačních bloků.
 */
uint16_t fsmz_blocks_from_size ( uint32_t size ) {
    uint16_t blocks = ( size % FSMZ_SECTOR_SIZE ) ? ( ( size / FSMZ_SECTOR_SIZE ) + 1 ) : ( size / FSMZ_SECTOR_SIZE );
    return blocks;
}


/**
 * @brief Aktualizuje volume_number v DINFO bloku na disku.
 *
 * Přečte DINFO z disku, změní volume_number a zapíše zpět.
 * Používá disc->cache jako pracovní buffer.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param volume_number Nová hodnota volume_number (0 = master, >0 = slave).
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_update_dinfo_volume_number ( st_MZDSK_DISC *disc, uint8_t volume_number ) {

    en_MZDSK_RES err;
    st_FSMZ_DINFO_BLOCK *dinfo_bl = (st_FSMZ_DINFO_BLOCK*) disc->cache;

    err = fsmz_read_dinfo ( disc, dinfo_bl );
    if ( err ) return err;

    dinfo_bl->volume_number = volume_number;

    return fsmz_write_dinfo ( disc, dinfo_bl );
}


/**
 * @brief Aktualizuje bitmapu souborové oblasti v paměti.
 *
 * Nastaví nebo resetuje bity v bitmapě pro zadaný rozsah bloků.
 * Pracuje přímo nad polem v paměti - nezapisuje na disk.
 *
 * @param map Ukazatel na bitmapu (FSMZ_FAREA_BITMAP_SIZE bajtů).
 * @param setres FSMZ_DINFO_BITMAP_SET (obsadit) nebo FSMZ_DINFO_BITMAP_RESET (uvolnit).
 * @param farea_block Počáteční blok relativně k farea.
 * @param count_blocks Počet bloků k označení.
 */
void fsmz_update_farea_bitmap ( uint8_t *map, uint8_t setres, uint16_t farea_block, uint16_t count_blocks ) {

    uint16_t map_byte = ( farea_block / 8 );
    uint8_t map_bit = ( farea_block & 0x07 );
    uint8_t i = 1 << map_bit;

    while ( count_blocks-- ) {

        if ( setres == FSMZ_DINFO_BITMAP_RESET ) {
            map [ map_byte ] &= ~i;
        } else {
            map [ map_byte ] |= i;
        };

        map_bit++;

        if ( map_bit == 0x08 ) {
            map_bit = 0;
            map_byte++;
            i = 1;
        } else {
            i = i << 1;
        };
    };
}


/**
 * @brief Aktualizuje DINFO bitmapu na disku pro zadaný rozsah bloků.
 *
 * Přečte DINFO z disku, aktualizuje počítadlo obsazených bloků (used)
 * a bitmapu souborové oblasti, pak zapíše DINFO zpět na disk.
 * Číslo bloku se automaticky přepočítá relativně k farea.
 *
 * Pokud blok leží mimo souborovou oblast (před farea nebo za koncem
 * bitmapy), funkce se tiše vrátí s MZDSK_RES_OK bez modifikace disku.
 * Tím se bezpečně ošetří volání s bloky v boot tracku (např. mini bootstrap
 * s block=1), které do FAREA bitmapy nepatří.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param setres FSMZ_DINFO_BITMAP_SET (obsadit) nebo FSMZ_DINFO_BITMAP_RESET (uvolnit).
 * @param block Absolutní číslo alokačního bloku.
 * @param count_blocks Počet bloků k označení.
 * @return MZDSK_RES_OK při úspěchu, jinak chybový kód.
 */
en_MZDSK_RES fsmz_update_dinfo_farea_bitmap ( st_MZDSK_DISC *disc, uint8_t setres, uint16_t block, uint16_t count_blocks ) {

    en_MZDSK_RES err;
    st_FSMZ_DINFO_BLOCK *dinfo_bl = (st_FSMZ_DINFO_BLOCK*) disc->cache;

    err = fsmz_read_dinfo ( disc, dinfo_bl );
    if ( err ) return err;

    /* Blok musí ležet ve FAREA - bloky v boot tracku do bitmapy nepatří */
    if ( block < dinfo_bl->farea ) {
        return MZDSK_RES_OK;
    }

    uint16_t farea_block = block - dinfo_bl->farea;

    /* Kontrola rozsahu bitmapy */
    if ( ( farea_block + count_blocks ) > ( FSMZ_FAREA_BITMAP_SIZE * 8 ) ) {
        return MZDSK_RES_OK;
    }

    if ( setres == FSMZ_DINFO_BITMAP_RESET ) {
        dinfo_bl->used -= count_blocks;
    } else {
        dinfo_bl->used += count_blocks;
    }

    fsmz_update_farea_bitmap ( ( uint8_t* ) & dinfo_bl->map, setres, farea_block, count_blocks );

    return fsmz_write_dinfo ( disc, dinfo_bl );
}


/**
 * @brief Smaže soubor z disku podle jména v Sharp MZ ASCII.
 *
 * Standardní hodnota fsmz_dir_items je 63 pro MZ-800 Disc BASIC.
 * IPLDISK pracuje s adresářem o velikosti 16 sektorů a fsmz_dir_items je 127.
 *
 * Nastaví ftype na 0 v adresáři a uvolní alokované bloky
 * v DINFO bitmapě.
 *
 * Pokud má soubor nastavený příznak locked a force == 0, funkce
 * vrátí MZDSK_RES_FILE_LOCKED a neprovede žádnou změnu.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param mz_fname Jméno souboru v Sharp MZ ASCII (zakončeno 0x0d).
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_NOT_FOUND pokud
 *         soubor nenalezen, MZDSK_RES_FILE_LOCKED pokud je soubor
 *         uzamčen a force == 0, jinak chybový kód.
 */
en_MZDSK_RES fsmz_unlink_file ( st_MZDSK_DISC *disc, uint8_t *mz_fname, uint8_t fsmz_dir_items, uint8_t force ) {
    st_FSMZ_DIR dir;
    st_FSMZ_DIR_ITEM *diritem;

    en_MZDSK_RES err;
    uint16_t num_blocks;
    uint16_t start_block;


    err = fsmz_open_dir ( disc, &dir );
    if ( err ) return err;

    while ( 1 ) {
        diritem = fsmz_read_dir ( disc, &dir, fsmz_dir_items, &err );
        if ( err ) break;

        if ( diritem->ftype ) {
            if ( !mzdsk_mzstrcmp ( mz_fname, diritem->fname ) ) break;
        };
    };
    if ( err ) return err;

    /* ochrana proti smazání uzamčeného souboru - vynucuje se, pokud
       volající explicitně nežádá force=1 */
    if ( diritem->locked && !force ) {
        return MZDSK_RES_FILE_LOCKED;
    }

    num_blocks = fsmz_size2blocks ( diritem->fsize );
    start_block = diritem->block;

    /* smazat položku adresáře */
    diritem->ftype = 0x00;

    err = fsmz_internal_write_dir_block ( disc, FSMZ_ALOCBLOCK_DIR + ( ( dir.position - 1 ) >> 3 ), dir.dir_bl );
    if ( err ) return err;

    /* upravit disc info */
    return fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_RESET, start_block, num_blocks );
}


/**
 * @brief Přejmenuje soubor na disku.
 *
 * Standardní hodnota fsmz_dir_items je 63 pro MZ-800 Disc BASIC.
 * IPLDISK pracuje s adresářem o velikosti 16 sektorů a fsmz_dir_items je 127.
 *
 * Zkontroluje, zda nové jméno ještě neexistuje. Pokud ano, vrátí
 * MZDSK_RES_FILE_EXISTS. Pak najde soubor se starým jménem a přejmenuje ho.
 *
 * Pokud má soubor nastavený příznak locked a force == 0, funkce
 * vrátí MZDSK_RES_FILE_LOCKED a neprovede žádnou změnu.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param mz_fname Původní jméno souboru v Sharp MZ ASCII.
 * @param new_mz_fname Nové jméno souboru v Sharp MZ ASCII.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_EXISTS pokud
 *         nové jméno již existuje, MZDSK_RES_FILE_LOCKED pokud je
 *         soubor uzamčen a force == 0, jinak chybový kód.
 */
en_MZDSK_RES fsmz_rename_file ( st_MZDSK_DISC *disc, uint8_t *mz_fname, uint8_t *new_mz_fname, uint8_t fsmz_dir_items, uint8_t force ) {
    st_FSMZ_DIR dir;
    st_FSMZ_DIR_ITEM *diritem;
    en_MZDSK_RES err;
    int8_t i;

    if ( new_mz_fname == NULL || new_mz_fname [ 0 ] < 0x20 ) return MZDSK_RES_BAD_NAME;

    /* Lokální kopie nového jména: nesmíme zapisovat do bufferu volajícího.
     * Viz komentář v fsmz_write_file(). Audit H-6. */
    uint8_t local_new_fname [ FSMZ_FNAME_LENGTH ];
    memcpy ( local_new_fname, new_mz_fname, FSMZ_FNAME_LENGTH );
    local_new_fname [ FSMZ_FNAME_LENGTH - 1 ] = 0x0d;

    fsmz_search_file ( disc, local_new_fname, NULL, fsmz_dir_items, &err );
    if ( err == MZDSK_RES_OK ) return ( MZDSK_RES_FILE_EXISTS );
    if ( err != MZDSK_RES_FILE_NOT_FOUND ) return err;


    err = fsmz_open_dir ( disc, &dir );
    if ( err ) return err;


    while ( 1 ) {
        diritem = fsmz_read_dir ( disc, &dir, fsmz_dir_items, &err );
        if ( err ) break;

        if ( diritem->ftype ) {
            if ( !mzdsk_mzstrcmp ( mz_fname, diritem->fname ) ) break;
        };
    };
    if ( err ) return err;

    /* ochrana proti přejmenování uzamčeného souboru - vynucuje se, pokud
       volající explicitně nežádá force=1 */
    if ( diritem->locked && !force ) {
        return MZDSK_RES_FILE_LOCKED;
    }

    /* přejmenovat položku adresáře - iterujeme přes lokální kopii,
     * která má zaručenou velikost FSMZ_FNAME_LENGTH. */
    i = 0;
    while ( i < (int8_t) FSMZ_FNAME_LENGTH && !( local_new_fname[i] < 0x20 ) ) {
        diritem->fname[i] = local_new_fname[i];
        i++;
    };

    while ( (size_t) i < sizeof ( diritem->fname ) ) {
        diritem->fname[i++] = 0x0d;
    };

    return fsmz_internal_write_dir_block ( disc, FSMZ_ALOCBLOCK_DIR + ( ( dir.position - 1 ) >> 3 ), dir.dir_bl );
}


/**
 * @brief Aktualizuje fstrt, fexec a ftype existujícího souboru v adresáři.
 *
 * Vyhledá položku v adresáři podle jména, respektuje lock flag (force=0)
 * a přepíše vybrané pole directory entry. Data souboru ani jeho alokační
 * bloky se nemění - jde o metadata-only update.
 *
 * @param disc Otevřený disk. Nesmí být NULL.
 * @param mz_fname Jméno v Sharp MZ ASCII (zakončeno 0x0d).
 * @param fstrt Ukazatel na novou start address; NULL = nezměnit.
 * @param fexec Ukazatel na novou exec address; NULL = nezměnit.
 * @param ftype Ukazatel na nový typ souboru; NULL = nezměnit. Hodnota 0x00
 *              by zneplatnila položku adresáře a není povolena.
 * @param fsmz_dir_items Maximální počet položek adresáře (63 nebo 127).
 * @param force Pokud 1, ignorovat příznak locked; pokud 0, ctít lock.
 * @return MZDSK_RES_OK při úspěchu,
 *         MZDSK_RES_INVALID_PARAM pokud jsou všechny ukazatele NULL
 *         nebo pokud ftype ukazuje na 0x00,
 *         MZDSK_RES_FILE_NOT_FOUND pokud soubor nenalezen,
 *         MZDSK_RES_FILE_LOCKED pokud je soubor uzamčen a force == 0,
 *         jinak chybový kód čtení/zápisu adresáře.
 */
en_MZDSK_RES fsmz_set_addr ( st_MZDSK_DISC *disc, uint8_t *mz_fname,
                              const uint16_t *fstrt, const uint16_t *fexec,
                              const uint8_t *ftype,
                              uint8_t fsmz_dir_items, uint8_t force ) {
    st_FSMZ_DIR dir;
    st_FSMZ_DIR_ITEM *diritem;
    en_MZDSK_RES err;

    /* vyžadujeme alespoň jednu změnu */
    if ( fstrt == NULL && fexec == NULL && ftype == NULL ) return MZDSK_RES_INVALID_PARAM;

    /* ftype == 0x00 by zneplatnila položku - zakázat */
    if ( ftype != NULL && *ftype == 0x00 ) return MZDSK_RES_INVALID_PARAM;

    err = fsmz_open_dir ( disc, &dir );
    if ( err ) return err;

    while ( 1 ) {
        diritem = fsmz_read_dir ( disc, &dir, fsmz_dir_items, &err );
        if ( err ) break;

        if ( diritem->ftype ) {
            if ( !mzdsk_mzstrcmp ( mz_fname, diritem->fname ) ) break;
        };
    };
    if ( err ) return err;

    /* ctít lock flag pokud není force */
    if ( diritem->locked && !force ) {
        return MZDSK_RES_FILE_LOCKED;
    }

    if ( fstrt != NULL ) diritem->fstrt = *fstrt;
    if ( fexec != NULL ) diritem->fexec = *fexec;
    if ( ftype != NULL ) diritem->ftype = *ftype;

    return fsmz_internal_write_dir_block ( disc, FSMZ_ALOCBLOCK_DIR + ( ( dir.position - 1 ) >> 3 ), dir.dir_bl );
}


/**
 * @brief Aktualizuje fname, fstrt, fexec a ftype v IPLPRO bloku.
 *
 * Read-modify-write: přečte IPLPRO, ověří že obsahuje platnou hlavičku
 * ("IPLPRO" magic + ftype==0x03), volitelně upraví vybraná pole a zapíše
 * zpět. Endianity a komentář/data zůstávají zachovány. Jméno se kopíruje
 * do pevné délky FSMZ_IPLFNAME_LENGTH a doplňuje se terminátorem 0x0d
 * (stejně jako fsmz_rename_file pro directory).
 *
 * Pokud IPLPRO neobsahuje platnou hlavičku (disk bez bootstrapu nebo po
 * `boot clear`), funkce vrátí MZDSK_RES_FILE_NOT_FOUND a NIC na disk
 * nezapíše. Pro instalaci nového bootstrapu slouží fsmz_write_file +
 * nastavení ftype=0x03 + IPLPRO magic (viz fsmz_write_bootstrap v
 * mzdsk_ipldisk_tools).
 *
 * @param disc Otevřený disk. Nesmí být NULL.
 * @param mz_fname Nové jméno v Sharp MZ ASCII (aspoň FSMZ_IPLFNAME_LENGTH
 *                 bajtů). NULL = nezměnit.
 * @param fstrt Ukazatel na novou start address. NULL = nezměnit.
 * @param fexec Ukazatel na novou exec address. NULL = nezměnit.
 * @param ftype Ukazatel na nový typ. NULL = nezměnit; 0x00 odmítnuto.
 * @return MZDSK_RES_OK při úspěchu,
 *         MZDSK_RES_INVALID_PARAM pokud všechny ukazatele NULL nebo
 *         ftype == 0x00,
 *         MZDSK_RES_FILE_NOT_FOUND pokud IPLPRO hlavička není platná
 *         (chybí "IPLPRO" magic nebo ftype != 0x03) - disk není
 *         modifikován,
 *         jinak chybový kód čtení/zápisu IPLPRO.
 */
en_MZDSK_RES fsmz_set_iplpro_header ( st_MZDSK_DISC *disc,
                                       const uint8_t *mz_fname,
                                       const uint16_t *fstrt,
                                       const uint16_t *fexec,
                                       const uint8_t *ftype ) {
    st_FSMZ_IPLPRO_BLOCK iplpro;
    en_MZDSK_RES err;

    if ( mz_fname == NULL && fstrt == NULL && fexec == NULL && ftype == NULL ) {
        return MZDSK_RES_INVALID_PARAM;
    }
    if ( ftype != NULL && *ftype == 0x00 ) return MZDSK_RES_INVALID_PARAM;

    err = fsmz_read_iplpro ( disc, &iplpro );
    if ( err ) return err;

    /* Ochrana proti tiché korupci: set má smysl pouze na disku, který už
     * obsahuje platný IPLPRO. Jinak bychom zapsali partial header bez
     * "IPLPRO" magic a vytvořili nekonzistentní stav (viz BUG E1 v
     * final-test-report-linux-2026-04-22-000.md). */
    if ( iplpro.ftype != 0x03
         || strncmp ( (const char*) iplpro.iplpro, "IPLPRO", 6 ) != 0 ) {
        return MZDSK_RES_FILE_NOT_FOUND;
    }

    if ( mz_fname != NULL ) {
        /* kopírovat do FSMZ_IPLFNAME_LENGTH a doplnit 0x0d terminátor.
         * Analogie fsmz_rename_file - nepředpokládáme, že vstupní buffer
         * je korektně zakončený, takže zajistíme invariant sami. */
        int i = 0;
        while ( i < (int) FSMZ_IPLFNAME_LENGTH && mz_fname[i] >= 0x20 ) {
            iplpro.fname[i] = mz_fname[i];
            i++;
        }
        while ( i < (int) FSMZ_IPLFNAME_LENGTH ) {
            iplpro.fname[i++] = 0x0d;
        }
    }
    if ( fstrt != NULL ) iplpro.fstrt = *fstrt;
    if ( fexec != NULL ) iplpro.fexec = *fexec;
    if ( ftype != NULL ) iplpro.ftype = *ftype;

    return fs_mz_write_iplpro ( disc, &iplpro );
}


/**
 * @brief Vyhledá volné souvislé místo na disku.
 *
 * Přečte DINFO bitmapu a hledá první souvislý úsek volných
 * alokačních bloků požadované velikosti.
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param num_blocks Požadovaný počet souvislých volných bloků.
 * @param[out] start_block Výstup: číslo počátečního bloku nalezeného úseku.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_DISK_FULL pokud
 *         není dostatek volných bloků celkem, MZDSK_RES_NO_SPACE pokud
 *         není dostatek souvislého místa, jinak chybový kód.
 */
en_MZDSK_RES fsmz_check_free_blocks ( st_MZDSK_DISC *disc, uint16_t num_blocks, uint16_t *start_block ) {

    en_MZDSK_RES err;
    st_FSMZ_DINFO_BLOCK *dinfo_bl;
    int8_t tst_phase;
    uint16_t disc_blocks;
    uint16_t map_byte;
    uint8_t map_bit;
    uint8_t i;
    uint16_t tst_blocks;

    /* je místo na disku? */
    dinfo_bl = (st_FSMZ_DINFO_BLOCK*) disc->cache;
    err = fsmz_read_dinfo ( disc, dinfo_bl );
    if ( err ) return err;
    if ( num_blocks > ( dinfo_bl->blocks - dinfo_bl->used ) ) return MZDSK_RES_DISK_FULL;

    /* je souvislé místo na disku? */
    disc_blocks = dinfo_bl->blocks - dinfo_bl->farea;
    map_byte = 0;
    map_bit = 0;
    i = 1;
    tst_phase = 0;

    tst_blocks = num_blocks;

    while ( disc_blocks ) {

        if ( map_byte >= FSMZ_FAREA_BITMAP_SIZE ) break;

        if ( dinfo_bl->map [ map_byte ] & i ) {
            tst_phase = 0;
        } else {
            if ( !tst_phase ) {
                tst_phase = 1;
                *start_block = dinfo_bl->farea + ( map_byte * 8 ) + map_bit;
                tst_blocks = num_blocks - 1;
            } else {
                tst_blocks--;
            };

            if ( !tst_blocks ) {
                tst_phase = 2;
                break;
            };
        };

        map_bit++;
        if ( map_bit == 0x08 ) {
            map_bit = 0;
            map_byte++;
            i = 1;
        } else {
            i = i << 1;
        };
        disc_blocks--;
    };

    if ( tst_phase != 2 ) return MZDSK_RES_NO_SPACE;

    return MZDSK_RES_OK;
}


/**
 * @brief Zapíše soubor na disk.
 *
 * Standardní hodnota fsmz_dir_items je 63 pro MZ-800 Disc BASIC.
 * IPLDISK pracuje s adresářem o velikosti 16 sektorů a fsmz_dir_items je 127.
 *
 * Postup:
 * 1. Ověří, že soubor se stejným jménem neexistuje
 * 2. Najde volné souvislé místo na disku
 * 3. Zapíše tělo souboru
 * 4. Najde volnou položku v adresáři
 * 5. Zapíše položku adresáře
 * 6. Aktualizuje DINFO bitmapu
 *
 * @param disc Ukazatel na diskovou strukturu. Nesmí být NULL.
 * @param ftype Souborový typ (0x01-0x05).
 * @param mz_fname Jméno souboru v Sharp MZ ASCII (zakončeno 0x0d).
 * @param fsize Velikost souboru v bajtech.
 * @param fstrt Startovací adresa v paměti Z80.
 * @param fexec Spouštěcí adresa v paměti Z80.
 * @param src Zdrojový buffer s daty souboru.
 * @param fsmz_dir_items Maximální počet položek adresáře.
 * @return MZDSK_RES_OK při úspěchu, MZDSK_RES_FILE_EXISTS pokud
 *         soubor již existuje, MZDSK_RES_DISK_FULL / MZDSK_RES_NO_SPACE /
 *         MZDSK_RES_DIR_FULL při nedostatku místa, jinak chybový kód.
 */
en_MZDSK_RES fsmz_write_file ( st_MZDSK_DISC *disc, uint8_t ftype, uint8_t *mz_fname, uint16_t fsize, uint16_t fstrt, uint16_t fexec, void *src, uint8_t fsmz_dir_items ) {

    st_FSMZ_DIR dir;
    st_FSMZ_DIR_ITEM *diritem;

    en_MZDSK_RES err;
    uint16_t num_blocks;
    uint16_t start_block = 0;

    if ( mz_fname == NULL || mz_fname [ 0 ] < 0x20 ) return MZDSK_RES_BAD_NAME;

    /* Lokální kopie jména: nesmíme zapisovat do bufferu volajícího.
     * Dříve `mz_fname[FSMZ_FNAME_LENGTH - 1] = 0x0d;` psalo do
     * vstupního parametru bez znalosti jeho velikosti → OOB zápis
     * pokud volající předal kratší buffer. Audit H-6. */
    uint8_t local_fname [ FSMZ_FNAME_LENGTH ];
    memcpy ( local_fname, mz_fname, FSMZ_FNAME_LENGTH );
    local_fname [ FSMZ_FNAME_LENGTH - 1 ] = 0x0d;

    fsmz_search_file ( disc, local_fname, NULL, fsmz_dir_items, &err );
    if ( err == MZDSK_RES_OK ) return MZDSK_RES_FILE_EXISTS;
    if ( err != MZDSK_RES_FILE_NOT_FOUND ) return err;

    num_blocks = fsmz_size2blocks ( fsize );

    /* kontrola velikosti */
    err = fsmz_check_free_blocks ( disc, num_blocks, &start_block );
    if ( err ) return err;

    /* uložit tělo souboru */
    err = fsmz_write_blocks ( disc, start_block, fsize, src );
    if ( err ) return err;

    /* pokud je místo v adresáři, uložit položku adresáře */
    err = fsmz_open_dir ( disc, &dir );
    if ( err ) return err;


    while ( 1 ) {
        diritem = fsmz_read_dir ( disc, &dir, fsmz_dir_items, &err );
        if ( err ) break;

        if ( !diritem->ftype ) break;
    };
    if ( err == MZDSK_RES_FILE_NOT_FOUND ) return MZDSK_RES_DIR_FULL;
    if ( err ) return err;

    /* Poctivě vyčistit celý slot před naplněním novými údaji. Jinak
     * by ve slotu mohly zbýt hodnoty po dřívější (smazané) položce
     * - především příznak `locked` (fsmz_unlink_file nuluje pouze
     * ftype) a rezervovaná pole `unused1`/`unused2`. Konzistentní
     * s defrag_write_file() v mzdsk_ipldisk_tools.c. */
    memset ( diritem, 0, sizeof ( *diritem ) );

    diritem->ftype = ftype;

    memcpy ( diritem->fname, local_fname, sizeof ( diritem->fname ) );

    diritem->fsize = fsize;
    diritem->fstrt = fstrt;
    diritem->fexec = fexec;
    diritem->block = start_block;

    err = fsmz_internal_write_dir_block ( disc, FSMZ_ALOCBLOCK_DIR + ( ( dir.position - 1 ) >> 3 ), dir.dir_bl );
    if ( err ) return err;

    /* upravit disc info */
    return fsmz_update_dinfo_farea_bitmap ( disc, FSMZ_DINFO_BITMAP_SET, start_block, num_blocks );
}


/**
 * @brief Vrátí řetězec s verzí knihovny mzdsk_ipldisk.
 * @return Statický řetězec s verzí (např. "1.0.0").
 */
const char* mzdsk_ipldisk_version ( void ) {
    return MZDSK_IPLDISK_VERSION;
}
