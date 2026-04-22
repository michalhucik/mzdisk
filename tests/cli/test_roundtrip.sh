#!/bin/bash
# CLI testy: souborové roundtripy (put -> get -> binární shoda).

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="

## Vytvoří testovací MZF soubor (128B header + data).
## create_test_mzf <output_file> <name> <data_size>
create_test_mzf() {
    local out="$1"
    local name="$2"
    local size="$3"

    # 128B header vyplněný 0x0D (FSMZ normalizuje padding jména na 0x0D)
    # Nejdřív nuly, pak přepíšeme
    dd if=/dev/zero bs=128 count=1 of="$out" 2>/dev/null

    # byte 0: typ 0x01 (OBJ)
    printf '\x01' | dd of="$out" bs=1 seek=0 conv=notrunc 2>/dev/null

    # bytes 1-17: jméno + 0x0D padding (celé pole 17 bajtů)
    # Vyplnit celý name field 0x0D, pak přepsat jménem
    printf '\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d' \
        | dd of="$out" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf '%s\x0d' "$name" | dd of="$out" bs=1 seek=1 conv=notrunc 2>/dev/null

    # bytes 18-19: velikost (LE)
    printf "\\x$(printf '%02x' $((size & 0xFF)))\\x$(printf '%02x' $(((size >> 8) & 0xFF)))" \
        | dd of="$out" bs=1 seek=18 conv=notrunc 2>/dev/null

    # bytes 20-21: start adresa 0x1200
    printf '\x00\x12' | dd of="$out" bs=1 seek=20 conv=notrunc 2>/dev/null
    # bytes 22-23: exec adresa 0x1200
    printf '\x00\x12' | dd of="$out" bs=1 seek=22 conv=notrunc 2>/dev/null

    # Připojit data (náhodná)
    dd if=/dev/urandom bs="$size" count=1 2>/dev/null >> "$out"
}


# === FSMZ roundtrip ===

## FSMZ: put MZF -> get MZF -> binární shoda
test_fsmz_roundtrip() {
    local dsk="$TEST_TMPDIR/fsmz_rt.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local mzf_in="$TEST_TMPDIR/test_in.mzf"
    create_test_mzf "$mzf_in" "TESTFILE" 256

    "$MZDSK_FSMZ" "$dsk" put "$mzf_in" 2>/dev/null || return 1

    local mzf_out="$TEST_TMPDIR/test_out.mzf"
    "$MZDSK_FSMZ" --overwrite "$dsk" get "TESTFILE" "$mzf_out" 2>/dev/null || return 1

    cmp -s "$mzf_in" "$mzf_out"
    assert_eq "$?" "0" "MZF roundtrip binary match" || return 1
}

## FSMZ: put více souborů -> get -> shoda
test_fsmz_roundtrip_multiple() {
    local dsk="$TEST_TMPDIR/fsmz_multi.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    for i in 1 2 3; do
        local sz=$((i * 256))
        create_test_mzf "$TEST_TMPDIR/file${i}.mzf" "FILE${i}" $sz
        "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/file${i}.mzf" 2>/dev/null || return 1
    done

    for i in 1 2 3; do
        "$MZDSK_FSMZ" --overwrite "$dsk" get "FILE${i}" "$TEST_TMPDIR/out${i}.mzf" 2>/dev/null || return 1
        cmp -s "$TEST_TMPDIR/file${i}.mzf" "$TEST_TMPDIR/out${i}.mzf"
        assert_eq "$?" "0" "FILE${i} roundtrip" || return 1
    done
}


# === CP/M roundtrip ===

## CP/M: put raw -> get raw -> shoda (zarovnáno na 128B)
test_cpm_roundtrip() {
    local dsk="$TEST_TMPDIR/cpm_rt.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    # CP/M zarovnává na 128B záznamy
    local raw_in="$TEST_TMPDIR/TEST.DAT"
    dd if=/dev/urandom bs=512 count=1 of="$raw_in" 2>/dev/null

    # put <input> <name.ext>
    "$MZDSK_CPM" "$dsk" put "$raw_in" "TEST.DAT" 2>/dev/null || return 1

    # get <name.ext> <output>
    local raw_out="$TEST_TMPDIR/test_out.dat"
    "$MZDSK_CPM" --overwrite "$dsk" get "TEST.DAT" "$raw_out" 2>/dev/null || return 1

    # Porovnat prvních 512B (CP/M může připsat padding)
    cmp -s -n 512 "$raw_in" "$raw_out"
    assert_eq "$?" "0" "CP/M roundtrip binary match" || return 1
}


# === MRS roundtrip ===

## MRS: init -> put MZF -> get MZF -> datová shoda
## MRS ukládá jména v 8+3 formátu, takže MZF header (name pole) se může
## lišit od originálu. Porovnáváme typ, velikost, adresy a data.
test_mrs_roundtrip() {
    local dsk="$TEST_TMPDIR/mrs_rt.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1

    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local mzf_in="$TEST_TMPDIR/mrs_in.mzf"
    create_test_mzf "$mzf_in" "TESTMRS" 512

    "$MZDSK_MRS" "$dsk" put --mzf "$mzf_in" 2>/dev/null || return 1

    local mzf_out="$TEST_TMPDIR/mrs_out.mzf"
    "$MZDSK_MRS" --overwrite "$dsk" get --mzf "TESTMRS" "$mzf_out" 2>/dev/null || return 1

    # Byte 0: typ souboru
    cmp -s -n 1 "$mzf_in" "$mzf_out"
    assert_eq "$?" "0" "MRS MZF type byte match" || return 1

    # Bajty 18-23: velikost, start adresa, exec adresa
    cmp -s -i 18 -n 6 "$mzf_in" "$mzf_out"
    assert_eq "$?" "0" "MRS MZF size/addr match" || return 1

    # Data od bajtu 128
    cmp -s -i 128 "$mzf_in" "$mzf_out"
    assert_eq "$?" "0" "MRS MZF data payload match" || return 1
}


# === Spuštění ===

run_test test_fsmz_roundtrip
run_test test_fsmz_roundtrip_multiple
run_test test_cpm_roundtrip
run_test test_mrs_roundtrip

test_summary
