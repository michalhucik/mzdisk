#!/bin/bash
# CLI testy: formátování, defrag a destruktivní operace.

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="

## Pomocná funkce: vytvoří MZF a vloží na FSMZ disk
put_test_mzf() {
    local dsk="$1"
    local name="$2"
    local size="$3"
    local mzf="$TEST_TMPDIR/${name}.mzf"

    # 128B nulový header
    dd if=/dev/zero bs=128 count=1 of="$mzf" 2>/dev/null
    printf '\x01' | dd of="$mzf" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf '%s\x0d' "$name" | dd of="$mzf" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf "\\x$(printf '%02x' $((size & 0xFF)))\\x$(printf '%02x' $(((size >> 8) & 0xFF)))" \
        | dd of="$mzf" bs=1 seek=18 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$mzf" bs=1 seek=20 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$mzf" bs=1 seek=22 conv=notrunc 2>/dev/null
    dd if=/dev/urandom bs="$size" count=1 2>/dev/null >> "$mzf"

    "$MZDSK_FSMZ" "$dsk" put "$mzf" 2>/dev/null
}

## Počet souborů v FSMZ adresáři (z JSON výstupu)
fsmz_file_count() {
    "$MZDSK_FSMZ" -o json "$1" dir 2>/dev/null | grep -c '"id"'
}

## Počet souborů v CP/M adresáři
cpm_file_count() {
    "$MZDSK_CPM" -o json "$1" dir 2>/dev/null | grep -c '"name"'
}


# === FSMZ format ===

## FSMZ: put soubory -> format file area -> adresář prázdný
test_fsmz_format_clears_dir() {
    local dsk="$TEST_TMPDIR/fsmz_fmt.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    put_test_mzf "$dsk" "AAA" 256 || return 1
    put_test_mzf "$dsk" "BBB" 128 || return 1

    local count_before=$(fsmz_file_count "$dsk")
    assert_eq "$count_before" "2" "2 files before format" || return 1

    "$MZDSK_FSMZ" -y "$dsk" format 2>/dev/null || return 1

    local count_after=$(fsmz_file_count "$dsk")
    assert_eq "$count_after" "0" "0 files after format" || return 1
}


# === FSMZ defrag ===

## FSMZ: put soubory, smaž prostřední, defrag -> soubory zachovány
test_fsmz_defrag_preserves_files() {
    local dsk="$TEST_TMPDIR/fsmz_defrag.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    put_test_mzf "$dsk" "FIRST" 256 || return 1
    put_test_mzf "$dsk" "MIDDLE" 256 || return 1
    put_test_mzf "$dsk" "LAST" 256 || return 1

    "$MZDSK_FSMZ" "$dsk" era "MIDDLE" 2>/dev/null || return 1

    local count_before=$(fsmz_file_count "$dsk")
    assert_eq "$count_before" "2" "2 files before defrag" || return 1

    "$MZDSK_FSMZ" -y "$dsk" defrag 2>/dev/null || return 1

    local count_after=$(fsmz_file_count "$dsk")
    assert_eq "$count_after" "2" "2 files after defrag" || return 1

    "$MZDSK_FSMZ" --overwrite "$dsk" get "FIRST" "$TEST_TMPDIR/defrag_first.mzf" 2>/dev/null || return 1
    "$MZDSK_FSMZ" --overwrite "$dsk" get "LAST" "$TEST_TMPDIR/defrag_last.mzf" 2>/dev/null || return 1
    assert_file_exists "$TEST_TMPDIR/defrag_first.mzf" "FIRST after defrag" || return 1
    assert_file_exists "$TEST_TMPDIR/defrag_last.mzf" "LAST after defrag" || return 1
}


# === CP/M format ===

## CP/M: put soubory -> format directory -> adresář prázdný
test_cpm_format_clears_dir() {
    local dsk="$TEST_TMPDIR/cpm_fmt.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    dd if=/dev/urandom bs=256 count=1 of="$TEST_TMPDIR/A.COM" 2>/dev/null
    dd if=/dev/urandom bs=256 count=1 of="$TEST_TMPDIR/B.COM" 2>/dev/null
    "$MZDSK_CPM" "$dsk" put "$TEST_TMPDIR/A.COM" "A.COM" 2>/dev/null || return 1
    "$MZDSK_CPM" "$dsk" put "$TEST_TMPDIR/B.COM" "B.COM" 2>/dev/null || return 1

    local count_before=$(cpm_file_count "$dsk")
    assert_eq "$count_before" "2" "2 files before format" || return 1

    "$MZDSK_CPM" -y "$dsk" format 2>/dev/null || return 1

    local count_after=$(cpm_file_count "$dsk")
    assert_eq "$count_after" "0" "0 files after format" || return 1
}


# === CP/M delete ===

## CP/M: put -> delete -> soubor zmizel
test_cpm_delete() {
    local dsk="$TEST_TMPDIR/cpm_del.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    dd if=/dev/urandom bs=256 count=1 of="$TEST_TMPDIR/DEL.COM" 2>/dev/null
    "$MZDSK_CPM" "$dsk" put "$TEST_TMPDIR/DEL.COM" "DEL.COM" 2>/dev/null || return 1

    local count_before=$(cpm_file_count "$dsk")
    assert_eq "$count_before" "1" "1 file before delete" || return 1

    "$MZDSK_CPM" "$dsk" era "DEL.COM" 2>/dev/null || return 1

    local count_after=$(cpm_file_count "$dsk")
    assert_eq "$count_after" "0" "0 files after delete" || return 1
}


# === FSMZ delete ===

## FSMZ: put -> delete -> soubor zmizel
test_fsmz_delete() {
    local dsk="$TEST_TMPDIR/fsmz_del.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    put_test_mzf "$dsk" "DELME" 128 || return 1

    local count_before=$(fsmz_file_count "$dsk")
    assert_eq "$count_before" "1" "1 file before delete" || return 1

    "$MZDSK_FSMZ" "$dsk" era "DELME" 2>/dev/null || return 1

    local count_after=$(fsmz_file_count "$dsk")
    assert_eq "$count_after" "0" "0 files after delete" || return 1
}


# === CP/M defrag ===

## CP/M: put soubory, smaž prostřední, defrag -> soubory zachovány
test_cpm_defrag_preserves_files() {
    local dsk="$TEST_TMPDIR/cpm_defrag.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    dd if=/dev/urandom bs=256 count=1 of="$TEST_TMPDIR/FIRST.COM" 2>/dev/null
    dd if=/dev/urandom bs=256 count=1 of="$TEST_TMPDIR/MIDDLE.COM" 2>/dev/null
    dd if=/dev/urandom bs=256 count=1 of="$TEST_TMPDIR/LAST.COM" 2>/dev/null
    "$MZDSK_CPM" "$dsk" put "$TEST_TMPDIR/FIRST.COM" "FIRST.COM" 2>/dev/null || return 1
    "$MZDSK_CPM" "$dsk" put "$TEST_TMPDIR/MIDDLE.COM" "MIDDLE.COM" 2>/dev/null || return 1
    "$MZDSK_CPM" "$dsk" put "$TEST_TMPDIR/LAST.COM" "LAST.COM" 2>/dev/null || return 1

    "$MZDSK_CPM" "$dsk" era "MIDDLE.COM" 2>/dev/null || return 1

    local count_before=$(cpm_file_count "$dsk")
    assert_eq "$count_before" "2" "2 files before defrag" || return 1

    "$MZDSK_CPM" -y "$dsk" defrag 2>/dev/null || return 1

    local count_after=$(cpm_file_count "$dsk")
    assert_eq "$count_after" "2" "2 files after defrag" || return 1

    "$MZDSK_CPM" --overwrite "$dsk" get "FIRST.COM" "$TEST_TMPDIR/defrag_first.com" 2>/dev/null || return 1
    "$MZDSK_CPM" --overwrite "$dsk" get "LAST.COM" "$TEST_TMPDIR/defrag_last.com" 2>/dev/null || return 1

    cmp -s -n 256 "$TEST_TMPDIR/FIRST.COM" "$TEST_TMPDIR/defrag_first.com"
    assert_eq "$?" "0" "FIRST.COM data match after defrag" || return 1
    cmp -s -n 256 "$TEST_TMPDIR/LAST.COM" "$TEST_TMPDIR/defrag_last.com"
    assert_eq "$?" "0" "LAST.COM data match after defrag" || return 1
}


# --- Spuštění ---

run_test test_fsmz_format_clears_dir
run_test test_fsmz_defrag_preserves_files
run_test test_fsmz_delete
run_test test_cpm_format_clears_dir
run_test test_cpm_delete
run_test test_cpm_defrag_preserves_files

test_summary
