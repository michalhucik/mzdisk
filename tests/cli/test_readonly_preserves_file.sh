#!/bin/bash
# CLI regresní testy: read-only operace MUSÍ ponechat DSK soubor nedotčený.
#
# Historie: BUG D1 (2026-04-21) - mzdsk_disc_open() při požadavku RO
# otevíral soubor v RW režimu a spouštěl dsk_tools_repair(), což tiše
# truncovalo DSK obrazy s trailing data nebo NO_TRACKINFO. Všechny tooly
# (kromě mzdsk-dsk, který má vlastní open path) trpěly.
#
# Tento test zajistí, že se bug nemůže vrátit: pro každý read-only CLI
# příkaz nad DSK obrazem s "repairable" odchylkou musí zůstat SHA1
# i velikost souboru nezměněné.

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="


# --- Pomocné fixture funkce ---

## Vytvoří čistý FSMZ disk a přidá za EOF 10 KB náhodných dat.
## Výsledek: DSK s flagem TRAILING_DATA - repairable error.
make_disk_with_trailing_data() {
    local dsk="$1"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 --sides 2 --overwrite >/dev/null 2>&1 || return 1
    dd if=/dev/urandom bs=1024 count=10 >> "$dsk" 2>/dev/null || return 1
    return 0
}

## Vytvoří CP/M disk a přepíše bajt #48 (tracks count) na 0xFF,
## což vyvolá BAD_TRACKCOUNT + NO_TRACKINFO na high tracích.
make_disk_with_bad_trackcount() {
    local dsk="$1"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 --sides 2 --overwrite >/dev/null 2>&1 || return 1
    printf '\xff' | dd of="$dsk" bs=1 seek=48 conv=notrunc >/dev/null 2>&1 || return 1
    return 0
}

## Fingerprint souboru = "size:sha1" - používáme pro jedno-krokovou kontrolu.
file_fingerprint() {
    local f="$1"
    printf '%s:%s' "$(stat -c%s "$f")" "$(sha1sum "$f" | cut -d' ' -f1)"
}


# --- Testy na disku s TRAILING_DATA ---

test_info_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_info.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_INFO" "$dsk" >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-info (default) must not touch file"
}

test_info_map_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_info_map.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_INFO" "$dsk" --map >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-info --map must not touch file"
}

test_info_boot_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_info_boot.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_INFO" "$dsk" --boot >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-info --boot must not touch file"
}

test_info_sector_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_info_sec.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_INFO" "$dsk" --sector 1 1 >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-info --sector must not touch file"
}

test_fsmz_dir_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_fsmz.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_FSMZ" "$dsk" dir >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-fsmz dir must not touch file"
}

test_fsmz_boot_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_fsmz_boot.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_FSMZ" "$dsk" boot >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-fsmz boot must not touch file"
}

test_fsmz_dump_block_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_fsmz_dump.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_FSMZ" "$dsk" dump-block 0 >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-fsmz dump-block must not touch file"
}

test_raw_dump_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_raw.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_RAW" "$dsk" dump --track 0 --sector 1 >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-raw dump must not touch file"
}

test_raw_get_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_raw_get.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_RAW" "$dsk" get "$TEST_TMPDIR/ro_out.bin" --track 0 --sector 1 >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-raw get must not touch DSK file"
}

test_dsk_info_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_dsk_info.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_DSK" "$dsk" info >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-dsk info must not touch file"
}

test_dsk_check_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_dsk_check.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_DSK" "$dsk" check >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-dsk check must not touch file"
}

test_dsk_tracks_trailing_data_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_trail_dsk_tracks.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_DSK" "$dsk" tracks >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-dsk tracks must not touch file"
}


# --- Testy na disku s BAD_TRACKCOUNT (CP/M fixture) ---

test_info_bad_trackcount_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_badtc_info.dsk"
    make_disk_with_bad_trackcount "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_INFO" "$dsk" >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-info on BAD_TRACKCOUNT disk must not touch file"
}

test_cpm_dir_bad_trackcount_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_badtc_cpm.dsk"
    make_disk_with_bad_trackcount "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_CPM" "$dsk" dir >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-cpm dir on BAD_TRACKCOUNT disk must not touch file"
}

test_cpm_free_bad_trackcount_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_badtc_cpm_free.dsk"
    make_disk_with_bad_trackcount "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_CPM" "$dsk" free >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-cpm free on BAD_TRACKCOUNT disk must not touch file"
}

test_cpm_dpb_bad_trackcount_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_badtc_cpm_dpb.dsk"
    make_disk_with_bad_trackcount "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_CPM" "$dsk" dpb >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-cpm dpb on BAD_TRACKCOUNT disk must not touch file"
}

test_mrs_info_bad_trackcount_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_badtc_mrs.dsk"
    make_disk_with_bad_trackcount "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_MRS" "$dsk" info >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-mrs info on BAD_TRACKCOUNT disk must not touch file"
}


# --- Sanity check: na zdravém disku ---

test_info_clean_disk_preserves_file() {
    local dsk="$TEST_TMPDIR/ro_clean_info.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 --sides 2 --overwrite >/dev/null 2>&1 || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_INFO" "$dsk" --map >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_eq "$after" "$before" "mzdsk-info on clean disk must not touch file"
}


# --- Explicit repair MÁ truncovat (kontrolní - opačný kontrakt) ---

## mzdsk-dsk repair je EXPLICITNÍ uživatelská akce a MÁ soubor modifikovat
## (např. truncovat trailing data). Tento test potvrzuje, že kontrakt repair
## je opačný k read-only cestě - bez opravy by byl fix D1 nekompletní.
test_dsk_repair_explicit_modifies_file() {
    local dsk="$TEST_TMPDIR/ro_repair_explicit.dsk"
    make_disk_with_trailing_data "$dsk" || return 1
    local before after
    before=$(file_fingerprint "$dsk")
    "$MZDSK_DSK" "$dsk" repair >/dev/null 2>&1
    after=$(file_fingerprint "$dsk")
    assert_neq "$after" "$before" "mzdsk-dsk repair MUST modify file when issues present"
}


# --- Spuštění ---

# Trailing data scenario
run_test test_info_trailing_data_preserves_file
run_test test_info_map_trailing_data_preserves_file
run_test test_info_boot_trailing_data_preserves_file
run_test test_info_sector_trailing_data_preserves_file
run_test test_fsmz_dir_trailing_data_preserves_file
run_test test_fsmz_boot_trailing_data_preserves_file
run_test test_fsmz_dump_block_trailing_data_preserves_file
run_test test_raw_dump_trailing_data_preserves_file
run_test test_raw_get_trailing_data_preserves_file
run_test test_dsk_info_trailing_data_preserves_file
run_test test_dsk_check_trailing_data_preserves_file
run_test test_dsk_tracks_trailing_data_preserves_file

# BAD_TRACKCOUNT scenario
run_test test_info_bad_trackcount_preserves_file
run_test test_cpm_dir_bad_trackcount_preserves_file
run_test test_cpm_free_bad_trackcount_preserves_file
run_test test_cpm_dpb_bad_trackcount_preserves_file
run_test test_mrs_info_bad_trackcount_preserves_file

# Clean disk sanity
run_test test_info_clean_disk_preserves_file

# Explicit repair contract (opposite)
run_test test_dsk_repair_explicit_modifies_file

test_summary
