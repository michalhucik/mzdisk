#!/bin/bash
# CLI testy: geometrické operace (append-tracks, shrink, change-track).

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="

## Append 2 stop na CP/M SD disk
test_append_cpm_sd() {
    local dsk="$TEST_TMPDIR/append_cpm.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 || return 1
    local before=$("$MZDSK_INFO" -o json "$dsk")
    local tracks_before=$(json_get "$before" "total_tracks")

    "$MZDSK_RAW" "$dsk" append-tracks 2 9 512 0xe5 normal || return 1
    local after=$("$MZDSK_INFO" -o json "$dsk")
    local tracks_after=$(json_get "$after" "total_tracks")

    local expected=$((tracks_before + 2))
    assert_eq "$tracks_after" "$expected" "tracks after append" || return 1
    assert_json_eq "$after" "format" "cpm" "still cpm" || return 1
}

## Append 10 stop na FSMZ disk
test_append_fsmz() {
    local dsk="$TEST_TMPDIR/append_fsmz.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 || return 1
    local before=$("$MZDSK_INFO" -o json "$dsk")
    local tracks_before=$(json_get "$before" "total_tracks")

    "$MZDSK_RAW" "$dsk" append-tracks 10 16 256 0x00 normal || return 1
    local after=$("$MZDSK_INFO" -o json "$dsk")
    local tracks_after=$(json_get "$after" "total_tracks")

    local expected=$((tracks_before + 10))
    assert_eq "$tracks_after" "$expected" "tracks after append" || return 1
    assert_json_eq "$after" "format" "fsmz" "still fsmz" || return 1
}

## Shrink CP/M disku
test_shrink_cpm() {
    local dsk="$TEST_TMPDIR/shrink_cpm.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 || return 1

    "$MZDSK_RAW" "$dsk" shrink 40 || return 1
    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "40" "tracks after shrink" || return 1
}

## Shrink FSMZ disku
test_shrink_fsmz() {
    local dsk="$TEST_TMPDIR/shrink_fsmz.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 || return 1
    local before=$("$MZDSK_INFO" -o json "$dsk")
    local tracks_before=$(json_get "$before" "total_tracks")

    local new_tracks=$((tracks_before / 2))
    "$MZDSK_RAW" "$dsk" shrink $new_tracks || return 1
    local after=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$after" "total_tracks" "$new_tracks" "tracks after shrink" || return 1
}

## Append + shrink roundtrip
test_append_shrink_roundtrip() {
    local dsk="$TEST_TMPDIR/roundtrip.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd || return 1
    local orig=$("$MZDSK_INFO" -o json "$dsk")
    local orig_tracks=$(json_get "$orig" "total_tracks")

    # Append 20 stop
    "$MZDSK_RAW" "$dsk" append-tracks 20 9 512 0xe5 normal || return 1
    local after_append=$("$MZDSK_INFO" -o json "$dsk")
    local expected=$((orig_tracks + 20))
    assert_json_eq "$after_append" "total_tracks" "$expected" "after append" || return 1

    # Shrink zpět
    "$MZDSK_RAW" "$dsk" shrink $orig_tracks || return 1
    local after_shrink=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$after_shrink" "total_tracks" "$orig_tracks" "after shrink back" || return 1
}

## Change-track
test_change_track() {
    local dsk="$TEST_TMPDIR/change.dsk"
    "$MZDSK_CREATE" "$dsk" --custom 10 9 512 0xe5 --sides 1 || return 1

    "$MZDSK_RAW" "$dsk" change-track 5 16 256 0x00 normal || return 1

    local info=$("$MZDSK_INFO" -o json "$dsk")
    assert_json_eq "$info" "total_tracks" "10" "tracks unchanged" || return 1
}

## Atomicita shrink - při selhání (target > DSK_MAX_TOTAL_TRACKS) DSK nezměněn
test_shrink_atomicity() {
    local dsk="$TEST_TMPDIR/shrink_atom.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 >/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # 999 > DSK_MAX_TOTAL_TRACKS (255) - musí selhat
    "$MZDSK_RAW" "$dsk" shrink 999 >/dev/null 2>&1
    assert_neq "$?" "0" "shrink past limit must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after shrink failure" || return 1
}

## Atomicita append-tracks - při selhání (target > DSK_MAX_TOTAL_TRACKS) DSK nezměněn
test_append_atomicity() {
    local dsk="$TEST_TMPDIR/append_atom.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 >/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # Cíl 160 + 200 = 360 > 255 - musí selhat
    "$MZDSK_RAW" "$dsk" append-tracks 200 9 512 0xe5 normal >/dev/null 2>&1
    assert_neq "$?" "0" "append past DSK_MAX_TOTAL_TRACKS must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after append failure" || return 1
}

## Atomicita change-track - při neplatných parametrech DSK nezměněn
test_change_track_atomicity() {
    local dsk="$TEST_TMPDIR/change_atom.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 >/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # Track 9999 neexistuje - musí selhat
    "$MZDSK_RAW" "$dsk" change-track 9999 16 256 0x00 normal >/dev/null 2>&1
    assert_neq "$?" "0" "change-track on nonexistent track must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after change-track failure" || return 1
}

# --- Spuštění ---

run_test test_append_cpm_sd
run_test test_append_fsmz
run_test test_shrink_cpm
run_test test_shrink_fsmz
run_test test_append_shrink_roundtrip
run_test test_change_track
run_test test_shrink_atomicity
run_test test_append_atomicity
run_test test_change_track_atomicity

test_summary
