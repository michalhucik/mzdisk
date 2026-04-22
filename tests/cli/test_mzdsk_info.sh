#!/bin/bash
# CLI testy: mzdsk-info - read-only inspekce DSK obrazů.

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="


# === Základní geometrie ===

## mzdsk-info: výpis geometrie FSMZ disku
## Od mzdsk-create 1.2.0: --format-basic 80 = 80 stop/strana, --sides 2 default,
## celkem 160 stop.
test_info_geometry_fsmz() {
    local dsk="$TEST_TMPDIR/info_fsmz.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local output
    output=$("$MZDSK_INFO" "$dsk" 2>/dev/null)
    assert_contains "$output" "total tracks: 160" "total tracks" || return 1
    assert_contains "$output" "sides: 2" "sides" || return 1
}


## mzdsk-info: výpis geometrie CP/M disku
## --format-cpm 80 --sides 2 (default) = 160 total tracks.
test_info_geometry_cpm() {
    local dsk="$TEST_TMPDIR/info_cpm.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local output
    output=$("$MZDSK_INFO" "$dsk" 2>/dev/null)
    assert_contains "$output" "total tracks: 160" "total tracks" || return 1
}


# === Map ===

## mzdsk-info --map na FSMZ disku: detekce + obsazenost
test_info_map_fsmz() {
    local dsk="$TEST_TMPDIR/info_map_fsmz.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local output
    output=$("$MZDSK_INFO" "$dsk" --map 2>/dev/null)
    assert_contains "$output" "FSMZ" "map detects FSMZ" || return 1
}


## mzdsk-info --map na CP/M disku
test_info_map_cpm() {
    local dsk="$TEST_TMPDIR/info_map_cpm.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local output
    output=$("$MZDSK_INFO" "$dsk" --map 2>/dev/null)
    assert_contains "$output" "CP/M" "map detects CP/M" || return 1
}


# === JSON výstup ===

## mzdsk-info -o json: validní JSON s klíčovými poli
test_info_json() {
    local dsk="$TEST_TMPDIR/info_json.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local output
    output=$("$MZDSK_INFO" -o json "$dsk" 2>/dev/null)
    assert_contains "$output" "\"total_tracks\"" "JSON contains total_tracks key" || return 1
    assert_contains "$output" "\"sides\"" "JSON contains sides key" || return 1
    assert_contains "$output" "{" "JSON starts with {" || return 1
}


## mzdsk-info --map -o json: JSON výstup mapy
test_info_map_json() {
    local dsk="$TEST_TMPDIR/info_map_json.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local output
    output=$("$MZDSK_INFO" -o json "$dsk" --map 2>/dev/null)
    assert_contains "$output" "\"filesystem\"" "JSON map contains filesystem key" || return 1
}


# === Sector/Block dump ===

## mzdsk-info --sector: hexdump sektoru (exit 0)
test_info_sector() {
    local dsk="$TEST_TMPDIR/info_sector.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    "$MZDSK_INFO" "$dsk" --sector 0 1 2>/dev/null
    assert_eq "$?" "0" "sector dump should succeed" || return 1
}


## BUG-INFO-001: --sector validace musí respektovat skutečnou sector mapu,
## ne počet sektorů. Lemmings preset má na track 16 mapu
## {1,6,2,7,3,8,4,9,5,21} - 10 sektorů, max ID=21.
## ID=21 musí projít (existuje), ID=10 musí selhat s informativní hláškou
## (neexistuje, byť je <= 10).
test_info_sector_custom_map_valid_id() {
    local dsk="$TEST_TMPDIR/info_sec_cmap_ok.dsk"
    "$MZDSK_CREATE" "$dsk" --preset lemmings 2>/dev/null || return 1

    "$MZDSK_INFO" "$dsk" --sector 16 21 >/dev/null 2>&1
    assert_eq "$?" "0" "sector ID 21 must succeed (in lemmings sector map)" || return 1
}


test_info_sector_custom_map_missing_id() {
    local dsk="$TEST_TMPDIR/info_sec_cmap_missing.dsk"
    "$MZDSK_CREATE" "$dsk" --preset lemmings 2>/dev/null || return 1

    local err
    err=$("$MZDSK_INFO" "$dsk" --sector 16 10 2>&1 >/dev/null)
    local exit_code=$?
    assert_eq "$exit_code" "1" "sector ID 10 must fail (not in lemmings map)" || return 1
    assert_contains "$err" "sector ID 10 not found on track 16" \
        "informative error naming missing ID" || return 1
    assert_contains "$err" "21" "error lists ID 21 as available" || return 1
}


test_info_sector_custom_map_out_of_range() {
    local dsk="$TEST_TMPDIR/info_sec_cmap_oor.dsk"
    "$MZDSK_CREATE" "$dsk" --preset lemmings 2>/dev/null || return 1

    "$MZDSK_INFO" "$dsk" --sector 16 99 >/dev/null 2>&1
    assert_eq "$?" "1" "sector ID 99 must fail (way out of range)" || return 1
}


# --- Spuštění ---

run_test test_info_geometry_fsmz
run_test test_info_geometry_cpm
run_test test_info_map_fsmz
run_test test_info_map_cpm
run_test test_info_json
run_test test_info_map_json
run_test test_info_sector
run_test test_info_sector_custom_map_valid_id
run_test test_info_sector_custom_map_missing_id
run_test test_info_sector_custom_map_out_of_range

test_summary
