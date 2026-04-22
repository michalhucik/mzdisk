#!/bin/bash
# CLI testy: mzdsk-raw - surový přístup k sektorům/blokům.

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="


# === Sector dump ===

## mzdsk-raw dump: hexdump sektoru
test_raw_dump_sector() {
    local dsk="$TEST_TMPDIR/raw_dump.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    "$MZDSK_RAW" "$dsk" dump --track 0 --sector 1 2>/dev/null
    assert_eq "$?" "0" "dump sector should succeed" || return 1
}


# === Sector get/put roundtrip ===

## mzdsk-raw get -> put roundtrip (sektorový režim)
test_raw_get_put_sector() {
    local dsk="$TEST_TMPDIR/raw_sector.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local sec_file="$TEST_TMPDIR/sector.bin"
    "$MZDSK_RAW" "$dsk" get "$sec_file" --track 2 --sector 1 2>/dev/null
    assert_eq "$?" "0" "get sector should succeed" || return 1
    assert_file_exists "$sec_file" "sector file" || return 1

    "$MZDSK_RAW" "$dsk" put "$sec_file" --track 2 --sector 1 2>/dev/null
    assert_eq "$?" "0" "put sector should succeed" || return 1
}


# === Block get/put roundtrip ===

## mzdsk-raw get -> put roundtrip (blokový režim)
test_raw_get_put_block() {
    local dsk="$TEST_TMPDIR/raw_block.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local blk_file="$TEST_TMPDIR/block.bin"
    "$MZDSK_RAW" "$dsk" get "$blk_file" --block 16 2>/dev/null
    assert_eq "$?" "0" "get block should succeed" || return 1
    assert_file_exists "$blk_file" "block file" || return 1

    "$MZDSK_RAW" "$dsk" put "$blk_file" --block 16 2>/dev/null
    assert_eq "$?" "0" "put block should succeed" || return 1
}


# === BUG A2: file-offset za EOF ===

## mzdsk-raw put --file-offset za EOF musí selhat a sektor nezměnit
test_raw_put_file_offset_beyond_eof() {
    local dsk="$TEST_TMPDIR/raw_fo_eof.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    # Vstupní soubor 256 B
    local src="$TEST_TMPDIR/raw_fo_src.bin"
    dd if=/dev/urandom of="$src" bs=256 count=1 2>/dev/null

    # Snapshot disku před pokusem o put
    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # file-offset 500 je za EOF (256 B) - musí selhat
    "$MZDSK_RAW" "$dsk" put "$src" --track 2 --sector 1 --sectors 1 --file-offset 500 >/dev/null 2>&1
    assert_neq "$?" "0" "put with --file-offset past EOF must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk must be unchanged" || return 1
}


## mzdsk-raw put --file-offset přesně na EOF také musí selhat (zero bytes available)
test_raw_put_file_offset_at_eof() {
    local dsk="$TEST_TMPDIR/raw_fo_at.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local src="$TEST_TMPDIR/raw_fo_src2.bin"
    dd if=/dev/urandom of="$src" bs=256 count=1 2>/dev/null

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    "$MZDSK_RAW" "$dsk" put "$src" --track 2 --sector 1 --sectors 1 --file-offset 256 >/dev/null 2>&1
    assert_neq "$?" "0" "put with --file-offset == file size must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk must be unchanged" || return 1
}


## mzdsk-raw put s platným --file-offset uvnitř souboru stále funguje
test_raw_put_file_offset_valid() {
    local dsk="$TEST_TMPDIR/raw_fo_ok.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local src="$TEST_TMPDIR/raw_fo_src3.bin"
    dd if=/dev/urandom of="$src" bs=1024 count=1 2>/dev/null

    "$MZDSK_RAW" "$dsk" put "$src" --track 2 --sector 1 --sectors 1 --file-offset 100 >/dev/null 2>&1
    assert_eq "$?" "0" "put with valid --file-offset must succeed" || return 1
}


# === BUG-RAW-001: out-of-range track musí vracet informativní hlášku ===

## mzdsk-raw T/S režim s tracem mimo rozsah musí vrátit exit 1
## a hlášku "Track N out of range (0..MAX)" místo matoucí "Track N has no sectors".
test_raw_track_out_of_range() {
    local dsk="$TEST_TMPDIR/raw_oor.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local err
    err=$("$MZDSK_RAW" "$dsk" dump --track 200 --sector 1 --sectors 1 2>&1 >/dev/null)
    assert_eq "$?" "1" "dump with track 200 must fail" || return 1
    assert_contains "$err" "out of range" "error mentions out of range" || return 1
    assert_not_contains "$err" "has no sectors" \
        "old misleading 'has no sectors' message must not appear" || return 1
}

## Block mód s origin track mimo rozsah - stejná kategorie chyby
test_raw_block_origin_out_of_range() {
    local dsk="$TEST_TMPDIR/raw_blk_oor.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local err
    err=$("$MZDSK_RAW" "$dsk" dump --block 0 --origin-track 200 2>&1 >/dev/null)
    assert_eq "$?" "1" "dump with origin-track 200 must fail" || return 1
    assert_contains "$err" "out of range" "error mentions out of range" || return 1
}

## Validní abstrakt track (80 na 160-track disku) musí dál fungovat
## - track 80 = logic track 40 side 0, je to platný abs track < 160.
test_raw_track_valid_high_abs() {
    local dsk="$TEST_TMPDIR/raw_valid_high.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    "$MZDSK_RAW" "$dsk" dump --track 80 --sector 1 --sectors 1 >/dev/null 2>&1
    assert_eq "$?" "0" "dump of valid abs track 80 (of 160 total) must succeed" || return 1
}


# === BUG B5: generic "Command failed: Unknown error" duplikát ===

## Chybová hláška z subpříkazu nesmí být následovaná redundantní
## generickou hláškou "Error: Command failed: Unknown error" z main.
test_raw_no_duplicate_error_message() {
    local dsk="$TEST_TMPDIR/raw_b5.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local src="$TEST_TMPDIR/raw_b5_src.bin"
    dd if=/dev/urandom of="$src" bs=16 count=1 2>/dev/null

    # Spustit příkaz, který selže v cmd_put s konkrétní chybou
    local err
    err=$("$MZDSK_RAW" "$dsk" put "$src" --track 2 --sector 1 --sectors 1 \
        --file-offset 500 2>&1 >/dev/null)
    assert_contains "$err" "is at or beyond end of file" \
        "specific error present" || return 1
    assert_not_contains "$err" "Command failed" \
        "no duplicate generic 'Command failed' message" || return 1
    assert_not_contains "$err" "Unknown error" \
        "no 'Unknown error' in stderr" || return 1

    # Druhý případ: append-tracks s odd count na 2-sided disk
    err=$("$MZDSK_RAW" "$dsk" append-tracks 5 16 256 0xE5 normal 2>&1 >/dev/null)
    assert_contains "$err" "must be divisible by 2" \
        "append-tracks specific error" || return 1
    assert_not_contains "$err" "Command failed" \
        "append-tracks: no duplicate generic message" || return 1
}


# === BUG E3: get musí odmítnout přepis existujícího souboru bez --overwrite ===

## BUG E3: `get` bez --overwrite tise přepisoval existující výstupní soubor,
## zatímco ostatní nástroje (fsmz, cpm, mrs) vyžadují --overwrite. Po fixu
## je chování konzistentní: refuse rc=1 + specifická hláška.
test_raw_get_refuses_existing_output_without_overwrite() {
    local dsk="$TEST_TMPDIR/raw_e3_refuse.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local out="$TEST_TMPDIR/raw_e3_out.bin"
    echo "ORIGINAL CONTENT DO NOT OVERWRITE" > "$out"
    local orig_sha=$(sha1sum "$out" | awk '{print $1}')

    local err
    err=$("$MZDSK_RAW" "$dsk" get --block 0 "$out" 2>&1)
    local rc=$?

    assert_eq "$rc" "1" "get on existing output must rc=1" || return 1
    assert_contains "$err" "already exists" \
        "specific 'already exists' error present" || return 1
    assert_contains "$err" "Use --overwrite" \
        "hint about --overwrite flag present" || return 1

    local after_sha=$(sha1sum "$out" | awk '{print $1}')
    assert_eq "$after_sha" "$orig_sha" \
        "existing file content must be preserved" || return 1
}


## BUG E3: --overwrite povolí přepis existujícího souboru.
test_raw_get_overwrite_flag_allows_replace() {
    local dsk="$TEST_TMPDIR/raw_e3_allow.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local out="$TEST_TMPDIR/raw_e3_allow.bin"
    echo "OLD" > "$out"

    "$MZDSK_RAW" --overwrite "$dsk" get --block 0 "$out" >/dev/null 2>&1
    assert_eq "$?" "0" "get --overwrite on existing must rc=0" || return 1

    # Soubor musí mít velikost 1 bloku (256 B na FSMZ)
    local sz=$(stat -c%s "$out")
    assert_eq "$sz" "256" "overwritten file has block size" || return 1
}


## BUG E3: --overwrite za subpříkazem (po `get`) se parsuje stejně jako globálně.
test_raw_get_overwrite_after_subcmd() {
    local dsk="$TEST_TMPDIR/raw_e3_after.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local out="$TEST_TMPDIR/raw_e3_after.bin"
    echo "OLD" > "$out"

    "$MZDSK_RAW" "$dsk" get --block 0 "$out" --overwrite >/dev/null 2>&1
    assert_eq "$?" "0" "--overwrite after subcmd must work" || return 1
}


## BUG E3: --file-offset > 0 je embed mode - existence souboru je záměr,
## overwrite check se přeskočí (M-20 audit).
test_raw_get_file_offset_embed_mode() {
    local dsk="$TEST_TMPDIR/raw_e3_embed.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local out="$TEST_TMPDIR/raw_e3_embed.bin"
    head -c 500 /dev/urandom > "$out"

    "$MZDSK_RAW" "$dsk" get --block 0 --file-offset 500 "$out" >/dev/null 2>&1
    assert_eq "$?" "0" "get --file-offset 500 (embed) must rc=0" || return 1

    local sz=$(stat -c%s "$out")
    assert_eq "$sz" "756" "embed appended block (500 + 256) to existing file" || return 1
}


## BUG E3: get na neexistující výstup funguje (default use case).
test_raw_get_new_file_succeeds() {
    local dsk="$TEST_TMPDIR/raw_e3_new.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local out="$TEST_TMPDIR/raw_e3_new_out.bin"
    rm -f "$out"

    "$MZDSK_RAW" "$dsk" get --block 0 "$out" >/dev/null 2>&1
    assert_eq "$?" "0" "get to new file must rc=0" || return 1
    assert_file_exists "$out" "new output file created" || return 1
}


## BUG E3: `dump` na stdout nepostižen overwrite logikou (žádný file output).
test_raw_dump_unaffected_by_existing_file() {
    local dsk="$TEST_TMPDIR/raw_e3_dump.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    "$MZDSK_RAW" "$dsk" dump --block 0 >/dev/null 2>&1
    assert_eq "$?" "0" "dump must always succeed (stdout output)" || return 1
}


# --- Spuštění ---

run_test test_raw_dump_sector
run_test test_raw_get_put_sector
run_test test_raw_get_put_block
run_test test_raw_put_file_offset_beyond_eof
run_test test_raw_put_file_offset_at_eof
run_test test_raw_put_file_offset_valid
run_test test_raw_track_out_of_range
run_test test_raw_block_origin_out_of_range
run_test test_raw_track_valid_high_abs
run_test test_raw_no_duplicate_error_message
run_test test_raw_get_refuses_existing_output_without_overwrite
run_test test_raw_get_overwrite_flag_allows_replace
run_test test_raw_get_overwrite_after_subcmd
run_test test_raw_get_file_offset_embed_mode
run_test test_raw_get_new_file_succeeds
run_test test_raw_dump_unaffected_by_existing_file

test_summary
