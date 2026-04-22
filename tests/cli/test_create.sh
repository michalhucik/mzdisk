#!/bin/bash
# CLI testy: tvorba disků (mzdsk-create) a ověření přes mzdsk-info.

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="

test_binaries_exist() {
    assert_file_exists "$MZDSK_CREATE" "mzdsk-create" || return 1
    assert_file_exists "$MZDSK_INFO" "mzdsk-info" || return 1
}

## Preset basic: 160T, 2 sides, detekce fsmz
test_preset_basic() {
    local dsk="$TEST_TMPDIR/basic.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "160" "tracks" || return 1
    assert_json_eq "$info" "sides" "2" "sides" || return 1
    assert_json_eq "$info" "format" "fsmz" "format" || return 1
}

## Preset cpm-sd: 160T, 2 sides
test_preset_cpm_sd() {
    local dsk="$TEST_TMPDIR/cpmsd.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "160" "tracks" || return 1
    assert_json_eq "$info" "sides" "2" "sides" || return 1
}

## Preset cpm-hd: 160T, 2 sides
test_preset_cpm_hd() {
    local dsk="$TEST_TMPDIR/cpmhd.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-hd || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "160" "tracks" || return 1
    assert_json_eq "$info" "sides" "2" "sides" || return 1
}

## Preset mrs: 160T, 2 sides
test_preset_mrs() {
    local dsk="$TEST_TMPDIR/mrs.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "160" "tracks" || return 1
    assert_json_eq "$info" "sides" "2" "sides" || return 1
}

## --format-basic: FSMZ, 80 tracks per side = 80T celkem, 2 sides
test_format_basic() {
    local dsk="$TEST_TMPDIR/fmt_basic.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "format" "fsmz" "format" || return 1
    assert_json_eq "$info" "sides" "2" "sides" || return 1
}

## --format-cpm: CP/M SD, 80 tracks per side
test_format_cpm() {
    local dsk="$TEST_TMPDIR/fmt_cpmsd.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "format" "cpm" "format" || return 1
}

## --format-cpmhd: CP/M HD, 80 tracks per side
test_format_cpmhd() {
    local dsk="$TEST_TMPDIR/fmt_cpmhd.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpmhd 80 || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "format" "cpm_hd" "format" || return 1
}

## --format-mrs: MRS, 80 tracks, 2 sides
test_format_mrs() {
    local dsk="$TEST_TMPDIR/fmt_mrs.dsk"
    "$MZDSK_CREATE" "$dsk" --format-mrs 80 || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "format" "mrs" "format" || return 1
    assert_json_eq "$info" "sides" "2" "sides" || return 1
}

## --format-mrs: MRS 1-sided
test_format_mrs_1sided() {
    local dsk="$TEST_TMPDIR/fmt_mrs1.dsk"
    "$MZDSK_CREATE" "$dsk" --format-mrs 80 --sides 1 || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "format" "mrs" "format" || return 1
    assert_json_eq "$info" "sides" "1" "sides" || return 1
}

## BUG 13/14: --format-basic <T> interpretuje T jako tracks-per-side,
## stejně jako --preset basic. Tj. --format-basic 80 --sides 2 musí dát
## 160 total, --format-basic 80 --sides 1 musí dát 80 total (odlišné velikosti).
test_format_sides_semantics() {
    local a="$TEST_TMPDIR/sem_a.dsk"
    local b="$TEST_TMPDIR/sem_b.dsk"
    local c="$TEST_TMPDIR/sem_c.dsk"
    local d="$TEST_TMPDIR/sem_d.dsk"

    "$MZDSK_CREATE" "$a" --format-basic 80 --sides 1 >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$b" --format-basic 80 --sides 2 >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$c" --preset basic --sides 1 >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$d" --preset basic --sides 2 >/dev/null 2>&1 || return 1

    local sa sb sc sd
    sa=$(stat -c %s "$a")
    sb=$(stat -c %s "$b")
    sc=$(stat -c %s "$c")
    sd=$(stat -c %s "$d")

    # --format-basic 80 --sides 1 == --preset basic --sides 1 (80 total)
    assert_eq "$sa" "$sc" "--format-basic matches --preset for 1 side" || return 1
    # --format-basic 80 --sides 2 == --preset basic --sides 2 (160 total)
    assert_eq "$sb" "$sd" "--format-basic matches --preset for 2 sides" || return 1
    # 2-sided verze musí být větší než 1-sided
    assert_neq "$sa" "$sb" "1-sided and 2-sided must differ (BUG 13)" || return 1

    # Info musí hlásit správné total tracks
    local info_a info_b
    info_a=$("$MZDSK_INFO" "$a" 2>/dev/null)
    info_b=$("$MZDSK_INFO" "$b" 2>/dev/null)
    assert_contains "$info_a" "total tracks: 80" "1-sided: 80 total" || return 1
    assert_contains "$info_b" "total tracks: 160" "2-sided: 160 total" || return 1
}


## BUG 19: Konzistentní tvar chybových hlášek pro neplatný počet stop.
## Všechny --format-* i --custom používají stejnou strukturu
## "track count invalid (...): must be in X-Y total." a uvádí skutečné hodnoty.
test_error_track_range_consistent() {
    local dst="$TEST_TMPDIR/range_err.dsk"

    # --format-basic 0 -> prefix "track count invalid" + "must be in 2-204 total"
    local err
    err=$("$MZDSK_CREATE" "$dst" --format-basic 0 --overwrite 2>&1)
    assert_contains "$err" "track count invalid" "format-basic: common prefix" || return 1
    assert_contains "$err" "tracks-per-side=0" "format-basic: actual per-side shown" || return 1
    assert_contains "$err" "must be in 2-204 total" "format-basic: range" || return 1

    # --format-cpm 0 -> min=3
    err=$("$MZDSK_CREATE" "$dst" --format-cpm 0 --overwrite 2>&1)
    assert_contains "$err" "track count invalid" "format-cpm: common prefix" || return 1
    assert_contains "$err" "must be in 3-204 total" "format-cpm: range" || return 1

    # --format-mrs 0 -> min=6
    err=$("$MZDSK_CREATE" "$dst" --format-mrs 0 --overwrite 2>&1)
    assert_contains "$err" "track count invalid" "format-mrs: common prefix" || return 1
    assert_contains "$err" "must be in 6-204 total" "format-mrs: range" || return 1

    # --format-basic 200 --sides 2 -> overflow total=400
    err=$("$MZDSK_CREATE" "$dst" --format-basic 200 --sides 2 --overwrite 2>&1)
    assert_contains "$err" "total=400" "format-basic: total je skutečně uveden" || return 1

    # --custom 0 -> stejný prefix, min=1
    err=$("$MZDSK_CREATE" "$dst" --custom 0 16 256 0xFF --overwrite 2>&1)
    assert_contains "$err" "track count invalid" "custom: stejný prefix jako format-*" || return 1
    assert_contains "$err" "total=0" "custom: total shown" || return 1
    assert_contains "$err" "must be in 1-204 total" "custom: range" || return 1

    # --custom 205 -> nad max
    err=$("$MZDSK_CREATE" "$dst" --custom 205 16 256 0xFF --overwrite 2>&1)
    assert_contains "$err" "total=205" "custom 205: total shown" || return 1
}


## --custom: s explicitní --sides 1
test_custom_1sided() {
    local dsk="$TEST_TMPDIR/custom.dsk"
    "$MZDSK_CREATE" "$dsk" --custom 40 5 1024 0xFF --sides 1 || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "40" "tracks" || return 1
    assert_json_eq "$info" "sides" "1" "sides" || return 1
}

## --custom: default 2-sided
test_custom_2sided() {
    local dsk="$TEST_TMPDIR/custom2.dsk"
    "$MZDSK_CREATE" "$dsk" --custom 40 9 512 0xE5 || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "40" "tracks" || return 1
    assert_json_eq "$info" "sides" "2" "sides" || return 1
}

## --overwrite: přepsání existujícího souboru
test_overwrite() {
    local dsk="$TEST_TMPDIR/overwrite.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic || return 1
    "$MZDSK_CREATE" "$dsk" --preset mrs --overwrite || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "160" "tracks after overwrite" || return 1
}


## P2.4: --overwrite atomicity při neplatné geometrii
## Pokud existuje cílový soubor a mzdsk-create selže na validaci
## argumentů, původní soubor musí zůstat beze změny.
test_overwrite_atomicity_invalid_geometry() {
    local dsk="$TEST_TMPDIR/overwrite_atom.dsk"

    # Připravíme "cizí" soubor na cílové cestě - SHA hash držíme jako kotvu
    echo "This is an important existing file - do not delete!" > "$dsk"
    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # --format-basic 0 selže na validaci (track count invalid)
    "$MZDSK_CREATE" "$dsk" --format-basic 0 --overwrite >/dev/null 2>&1
    assert_neq "$?" "0" "create with invalid geometry must fail" || return 1

    # Soubor musí zůstat beze změny
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "existing file must be unchanged after failed create" || return 1

    # Pro jistotu i --custom s nevalidními rozměry
    local sha_mid=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_CREATE" "$dsk" --custom 205 16 256 0xFF --overwrite >/dev/null 2>&1
    assert_neq "$?" "0" "create with over-max tracks must fail" || return 1
    local sha_end=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_end" "$sha_mid" "existing file must be unchanged after over-max fail" || return 1
}


## P2.4: --overwrite atomicity bez --overwrite a s existujícím souborem
## Pokud soubor existuje a --overwrite není zadán, musí selhat a soubor
## zůstane nedotčen. Tohle je výchozí "safe" chování.
test_overwrite_requires_flag_preserves_file() {
    local dsk="$TEST_TMPDIR/overwrite_req.dsk"

    echo "DO NOT OVERWRITE ME" > "$dsk"
    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # Bez --overwrite selže - soubor existuje
    "$MZDSK_CREATE" "$dsk" --preset basic >/dev/null 2>&1
    assert_neq "$?" "0" "create without --overwrite on existing file must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "existing file unchanged without --overwrite" || return 1
}

# --- Spuštění ---

run_test test_binaries_exist
run_test test_preset_basic
run_test test_preset_cpm_sd
run_test test_preset_cpm_hd
run_test test_preset_mrs
run_test test_format_basic
run_test test_format_cpm
run_test test_format_cpmhd
run_test test_format_mrs
run_test test_format_mrs_1sided
run_test test_format_sides_semantics
run_test test_custom_1sided
run_test test_custom_2sided
run_test test_overwrite
run_test test_overwrite_atomicity_invalid_geometry
run_test test_overwrite_requires_flag_preserves_file
run_test test_error_track_range_consistent

test_summary
