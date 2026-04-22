#!/bin/bash
# CLI testy: mzdsk-dsk - diagnostika a editace DSK kontejneru.

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="


# === Info ===

## mzdsk-dsk info: zobrazí hlavičku DSK
test_dsk_info() {
    local dsk="$TEST_TMPDIR/dsk_info.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    local output
    output=$("$MZDSK_DSK" "$dsk" info 2>/dev/null)
    assert_contains "$output" "Tracks" "info contains Tracks" || return 1
    assert_contains "$output" "Sides" "info contains Sides" || return 1
}


# === Tracks ===

## mzdsk-dsk tracks: výpis stop s geometrií
test_dsk_tracks() {
    local dsk="$TEST_TMPDIR/dsk_tracks.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    local output
    output=$("$MZDSK_DSK" "$dsk" tracks 2>/dev/null)
    assert_contains "$output" "Track" "tracks output contains Track" || return 1
}


## mzdsk-dsk tracks --abstrack: info o jedné stopě
test_dsk_tracks_single() {
    local dsk="$TEST_TMPDIR/dsk_tracks1.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    local output
    output=$("$MZDSK_DSK" "$dsk" tracks --abstrack 0 2>/dev/null)
    assert_contains "$output" "0" "single track output" || return 1
}


# === Check ===

## mzdsk-dsk check na validním DSK -> OK (exit 0)
test_dsk_check_ok() {
    local dsk="$TEST_TMPDIR/dsk_check.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    "$MZDSK_DSK" "$dsk" check 2>/dev/null
    assert_eq "$?" "0" "check on valid DSK should succeed" || return 1
}


# === Repair ===

## mzdsk-dsk repair na validním DSK (nic k opravě)
test_dsk_repair_valid() {
    local dsk="$TEST_TMPDIR/dsk_repair.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    "$MZDSK_DSK" "$dsk" repair 2>/dev/null
    assert_eq "$?" "0" "repair on valid DSK should succeed" || return 1

    # po repairu stále validní
    "$MZDSK_DSK" "$dsk" check 2>/dev/null
    assert_eq "$?" "0" "check after repair should still pass" || return 1
}


# === Edit header ===

## mzdsk-dsk edit-header: změna creator textu
test_dsk_edit_header() {
    local dsk="$TEST_TMPDIR/dsk_edit.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    "$MZDSK_DSK" "$dsk" edit-header --creator "TEST CREATOR" 2>/dev/null
    assert_eq "$?" "0" "edit-header should succeed" || return 1

    local output
    output=$("$MZDSK_DSK" "$dsk" info 2>/dev/null)
    assert_contains "$output" "TEST CREATOR" "creator text updated" || return 1
}


# === Edit track ===

## mzdsk-dsk edit-track: změna filler byte
test_dsk_edit_track() {
    local dsk="$TEST_TMPDIR/dsk_edit_track.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    "$MZDSK_DSK" "$dsk" edit-track 0 --filler E5 2>/dev/null
    assert_eq "$?" "0" "edit-track should succeed" || return 1

    # disk musí zůstat validní
    "$MZDSK_DSK" "$dsk" check 2>/dev/null
    assert_eq "$?" "0" "check after edit-track should pass" || return 1
}


## BUG 4: edit-track --filler FF se skutečně zapíše (dříve byl sentinel)
test_dsk_edit_track_filler_ff() {
    local dsk="$TEST_TMPDIR/dsk_filler_ff.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    "$MZDSK_DSK" "$dsk" edit-track 5 --filler FF >/dev/null 2>&1
    assert_eq "$?" "0" "edit-track --filler FF should succeed" || return 1

    local output
    output=$("$MZDSK_DSK" "$dsk" tracks --abstrack 5 2>/dev/null)
    assert_contains "$output" "0xff" "filler value 0xff must be stored" || return 1
}


## BUG 4: edit-track --gap FF se skutečně zapíše
test_dsk_edit_track_gap_ff() {
    local dsk="$TEST_TMPDIR/dsk_gap_ff.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    "$MZDSK_DSK" "$dsk" edit-track 5 --gap FF >/dev/null 2>&1
    assert_eq "$?" "0" "edit-track --gap FF should succeed" || return 1

    local output
    output=$("$MZDSK_DSK" "$dsk" tracks --abstrack 5 2>/dev/null)
    assert_contains "$output" "GAP#3        : 0xff" "gap value 0xff must be stored" || return 1
}


## BUG 5: check vrací exit != 0 při nalezených opravitelných chybách
test_dsk_check_exit_code_on_errors() {
    local dsk="$TEST_TMPDIR/dsk_check_err.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    # Připojíme 1024 B trailing data (vyvolá TRAILING_DATA)
    dd if=/dev/urandom bs=1024 count=1 2>/dev/null >> "$dsk"

    "$MZDSK_DSK" "$dsk" check >/dev/null 2>&1
    assert_neq "$?" "0" "check must return non-zero on trailing data" || return 1
}


## BUG 6: info/edit-* na nevalidním souboru musí selhat bez modifikace
test_dsk_rejects_non_dsk_file() {
    local bogus="$TEST_TMPDIR/dsk_bogus.bin"
    dd if=/dev/urandom of="$bogus" bs=1024 count=100 2>/dev/null
    local sha_before=$(sha1sum "$bogus" | awk '{print $1}')

    # info na random souboru musí selhat
    "$MZDSK_DSK" "$bogus" info >/dev/null 2>&1
    assert_neq "$?" "0" "info on non-DSK must fail" || return 1

    # tracks taky
    "$MZDSK_DSK" "$bogus" tracks >/dev/null 2>&1
    assert_neq "$?" "0" "tracks on non-DSK must fail" || return 1

    # edit-header taky a SOUBOR MUSÍ ZŮSTAT NEZMĚNĚN
    "$MZDSK_DSK" "$bogus" edit-header --creator HIJACK >/dev/null 2>&1
    assert_neq "$?" "0" "edit-header on non-DSK must fail" || return 1

    local sha_after=$(sha1sum "$bogus" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "non-DSK file must not be modified" || return 1

    # edit-track taky
    "$MZDSK_DSK" "$bogus" edit-track 0 --filler 00 >/dev/null 2>&1
    assert_neq "$?" "0" "edit-track on non-DSK must fail" || return 1
}


## BUG 6: check má na nevalidním souboru stále fungovat (diagnostika)
test_dsk_check_on_non_dsk_reports_error() {
    local bogus="$TEST_TMPDIR/dsk_bogus_chk.bin"
    dd if=/dev/urandom of="$bogus" bs=1024 count=100 2>/dev/null

    # check se nesmí zamítnout magic kontrolou - musí projít dispatchem,
    # vypsat diagnostický report a vrátit exit != 0 kvůli BAD_FILEINFO.
    "$MZDSK_DSK" "$bogus" check >/dev/null 2>&1
    assert_neq "$?" "0" "check on non-DSK must report error (non-zero)" || return 1
}


## BUG 5: repair odstraňuje TRAILING_DATA (truncate na expected_image_size)
test_dsk_repair_trailing_data() {
    local dsk="$TEST_TMPDIR/dsk_repair_trail.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    local expected_size=$(stat -c '%s' "$dsk")

    # Připojíme 2048 B trailing data
    dd if=/dev/urandom bs=2048 count=1 2>/dev/null >> "$dsk"
    local bloated_size=$(stat -c '%s' "$dsk")
    assert_eq "$bloated_size" "$((expected_size + 2048))" "trailing data added" || return 1

    # Repair musí soubor zkrátit zpět na expected_size
    "$MZDSK_DSK" "$dsk" repair >/dev/null 2>&1
    assert_eq "$?" "0" "repair should succeed" || return 1

    local final_size=$(stat -c '%s' "$dsk")
    assert_eq "$final_size" "$expected_size" "file truncated to expected size" || return 1

    # Po repairu check musí vrátit 0 (všechno OK)
    "$MZDSK_DSK" "$dsk" check >/dev/null 2>&1
    assert_eq "$?" "0" "check after repair must be clean" || return 1
}


## BUG B6: repair musí dokončit kaskádu BAD_TRACKCOUNT+TRAILING_DATA
## v jednom volání (iterativní repair uvnitř cmd_repair).
test_dsk_repair_cascaded_errors() {
    local dsk="$TEST_TMPDIR/dsk_repair_cascade.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    # Poškodíme bajt tracks v hlavičce (offset 48 = dhdr.tracks)
    # Hodnota 0xFF způsobí BAD_TRACKCOUNT (+ ODD_DOUBLE kvůli lichému counku)
    # + track 160 pak má NO_TRACKINFO/READ_ERROR. Po opravě track count
    # na 80 se odhalí TRAILING_DATA, který pre-repair diagnóza neviděla.
    printf '\xff' | dd of="$dsk" bs=1 seek=48 conv=notrunc 2>/dev/null

    # Jeden repair musí plně vyřešit kaskádu (BUG B6: dřív potřeboval 2x)
    local out
    out=$("$MZDSK_DSK" "$dsk" repair 2>&1)
    assert_eq "$?" "0" "repair should succeed in single invocation" || return 1
    assert_contains "$out" "Repair completed successfully" \
        "repair must report success" || return 1

    # Po jednom repairu musí check vrátit 0 (všechno OK)
    "$MZDSK_DSK" "$dsk" check >/dev/null 2>&1
    assert_eq "$?" "0" "check after single repair must be clean" || return 1
}


## Audit H-11: header tracks*sides > DSK_MAX_TOTAL_TRACKS (204).
## check musí detekovat TRACKCOUNT_EXCEEDED a repair ho opravit clampem.
test_dsk_repair_trackcount_exceeded() {
    local dsk="$TEST_TMPDIR/dsk_trackcount_exceeded.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-hd 2>/dev/null || return 1

    # cpm-hd: 80 tracks * 2 sides = 160.
    # Přepíšeme header tracks na 128 → 128*2=256 > 204 = TRACKCOUNT_EXCEEDED.
    printf '\x80' | dd of="$dsk" bs=1 seek=48 conv=notrunc 2>/dev/null

    # check musí hlásit TRACKCOUNT_EXCEEDED a vrátit exit code != 0
    local out
    out=$("$MZDSK_DSK" "$dsk" check 2>&1)
    local ec=$?
    assert_neq "$ec" "0" "check on TRACKCOUNT_EXCEEDED must fail" || return 1
    assert_contains "$out" "TRACKCOUNT_EXCEEDED" \
        "check output must mention TRACKCOUNT_EXCEEDED" || return 1
    assert_contains "$out" "repair" \
        "check output must advise 'repair'" || return 1

    # repair musí clampnout header a zanechat OK DSK
    "$MZDSK_DSK" "$dsk" repair >/dev/null 2>&1
    assert_eq "$?" "0" "repair should succeed" || return 1

    "$MZDSK_DSK" "$dsk" check >/dev/null 2>&1
    assert_eq "$?" "0" "check after repair must be clean" || return 1
}


# === Edit Sector IDs ===

## edit-track --sector-ids: přepsání všech sektorových ID najednou
test_dsk_edit_sector_ids() {
    local dsk="$TEST_TMPDIR/dsk_sector_ids.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    # basic preset: 16x256B, normal order (1,2,...,16)
    "$MZDSK_DSK" "$dsk" edit-track 0 --sector-ids 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1 2>/dev/null
    assert_eq "$?" "0" "edit-track --sector-ids should succeed" || return 1

    # ověřit, že ID se změnila (idx 0 má SectID 16)
    local output
    output=$("$MZDSK_DSK" "$dsk" tracks --abstrack 0 2>/dev/null)
    echo "$output" | grep -q "0     16" || { echo "FAIL: first sector should have SectID 16"; return 1; }

    # disk musí zůstat validní
    "$MZDSK_DSK" "$dsk" check 2>/dev/null
    assert_eq "$?" "0" "check after sector-ids should pass" || return 1
}


## edit-track --sector-id IDX:ID: změna jednoho sektoru
test_dsk_edit_sector_id_single() {
    local dsk="$TEST_TMPDIR/dsk_sector_id1.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    "$MZDSK_DSK" "$dsk" edit-track 0 --sector-id 0:42 2>/dev/null
    assert_eq "$?" "0" "edit-track --sector-id should succeed" || return 1

    local output
    output=$("$MZDSK_DSK" "$dsk" tracks --abstrack 0 2>/dev/null)
    echo "$output" | grep -q "0     42" || { echo "FAIL: sector index 0 should have SectID 42"; return 1; }
}


## edit-track --sector-ids s duplicitou (P2.2) - duplicita je povolená
## Chráněné/kopírované disky někdy mají duplicitní IDs na stopě;
## nástroj to smí tiše přijmout, ověříme dokumentací aktuálního chování.
test_dsk_edit_sector_ids_duplicate() {
    local dsk="$TEST_TMPDIR/dsk_sector_ids_dup.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    # 16 sektorů s prvními dvěma ID = 1 (duplicita)
    "$MZDSK_DSK" "$dsk" edit-track 0 --sector-ids 1,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 2>/dev/null
    assert_eq "$?" "0" "edit-track with duplicate IDs should succeed" || return 1

    # ověřit, že disk je stále formálně validní
    "$MZDSK_DSK" "$dsk" check 2>/dev/null
    assert_eq "$?" "0" "check on disk with dup IDs should still pass" || return 1

    # ověřit obsah track headeru - idx 0 a idx 1 mají oba SectID 1
    local output
    output=$("$MZDSK_DSK" "$dsk" tracks --abstrack 0 2>/dev/null)
    local line_count
    line_count=$(echo "$output" | grep -cE '^ *[01] +1 ')
    assert_eq "$line_count" "2" "two sectors with SectID 1 present" || return 1
}


## edit-track --sector-ids se špatným počtem selže
## FINDING-DSK-001: chybová hláška musí jasně odlišit počet sektorů na stopě
## od počtu dodaných ID (dříve bylo matoucí "expected N sectors" kde N
## bylo právě počet dodaných ID, ne počet sektorů na stopě).
test_dsk_edit_sector_ids_bad_count() {
    local dsk="$TEST_TMPDIR/dsk_sector_ids_bad.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    # basic = 16 sektorů/stopu, posíláme jen 3
    local err
    err=$("$MZDSK_DSK" "$dsk" edit-track 0 --sector-ids 1,2,3 2>&1 >/dev/null)
    assert_neq "$?" "0" "sector-ids with wrong count should fail" || return 1
    assert_contains "$err" "track has 16 sectors" "error reports actual track sector count" || return 1
    assert_contains "$err" "3 IDs were provided" "error reports provided ID count" || return 1
}


## Atomicita edit-track - při neplatných parametrech DSK nezměněn (memory driver)
test_dsk_edit_track_atomicity() {
    local dsk="$TEST_TMPDIR/dsk_atom_et.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    # Neplatný sector-ids count -> musí selhat
    "$MZDSK_DSK" "$dsk" edit-track 0 --sector-ids 1,2,3 >/dev/null 2>&1
    assert_neq "$?" "0" "edit-track with bad sector-ids count must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after edit-track failure" || return 1
}

## Atomicita edit-header - při neplatných parametrech DSK nezměněn
test_dsk_edit_header_atomicity() {
    local dsk="$TEST_TMPDIR/dsk_atom_eh.dsk"
    "$MZDSK_CREATE" "$dsk" --preset basic 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    # edit-header bez modifikačních voleb -> musí selhat nebo být no-op
    # (volba --unknown-option je garantovaně neznámá)
    "$MZDSK_DSK" "$dsk" edit-header --nonexistent-option xxx >/dev/null 2>&1
    assert_neq "$?" "0" "edit-header with unknown option must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after edit-header failure" || return 1
}

## Atomicita repair - na neexistující stopě (korupce DSK) selže, soubor nezměněn
test_dsk_repair_atomicity_bad_file() {
    # Vytvoříme "DSK" soubor, který ale není platný DSK (magic mismatch)
    # -> repair odmítne operaci přes magic check, soubor nezměněn
    local bad="$TEST_TMPDIR/dsk_atom_bad.dsk"
    printf 'not a DSK file' > "$bad"

    local sha_before=$(sha1sum "$bad" | awk '{print $1}')
    "$MZDSK_DSK" "$bad" repair >/dev/null 2>&1
    assert_neq "$?" "0" "repair on non-DSK must fail" || return 1
    local sha_after=$(sha1sum "$bad" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "non-DSK file unchanged after failed repair" || return 1
}


# --- Spuštění ---

run_test test_dsk_info
run_test test_dsk_tracks
run_test test_dsk_tracks_single
run_test test_dsk_check_ok
run_test test_dsk_repair_valid
run_test test_dsk_repair_cascaded_errors
run_test test_dsk_repair_trackcount_exceeded
run_test test_dsk_edit_header
run_test test_dsk_edit_track
run_test test_dsk_edit_track_filler_ff
run_test test_dsk_edit_track_gap_ff
## BUG 16: track header filler a gap musí odpovídat reálnému obsahu
test_dsk_track_filler_metadata() {
    # --custom s explicitním fillerem 0xE5
    local dsk1="$TEST_TMPDIR/bug16_custom.dsk"
    "$MZDSK_CREATE" "$dsk1" --custom 80 16 256 0xE5 normal --sides 2 >/dev/null 2>&1 || return 1
    local out1
    out1=$("$MZDSK_DSK" "$dsk1" tracks --abstrack 0 2>/dev/null)
    assert_contains "$out1" "Filler       : 0xe5" "--custom filler in header (BUG 16)" || return 1
    assert_contains "$out1" "GAP#3        : 0x4e" "--custom GAP#3 default in header" || return 1

    # --preset basic - filler je 0xFF (FSMZ)
    local dsk2="$TEST_TMPDIR/bug16_preset.dsk"
    "$MZDSK_CREATE" "$dsk2" --preset basic --overwrite >/dev/null 2>&1 || return 1
    local out2
    out2=$("$MZDSK_DSK" "$dsk2" tracks --abstrack 0 2>/dev/null)
    assert_contains "$out2" "Filler       : 0xff" "--preset basic filler 0xff" || return 1
    assert_contains "$out2" "GAP#3        : 0x4e" "--preset basic GAP#3 default" || return 1

    # --format-cpm 80 - stopa 0 má filler 0xE5 (CP/M datová stopa)
    local dsk3="$TEST_TMPDIR/bug16_fmtcpm.dsk"
    "$MZDSK_CREATE" "$dsk3" --format-cpm 80 --overwrite >/dev/null 2>&1 || return 1
    local out3
    out3=$("$MZDSK_DSK" "$dsk3" tracks --abstrack 0 2>/dev/null)
    assert_contains "$out3" "Filler       : 0xe5" "--format-cpm data track filler 0xe5" || return 1
    # Boot track (abstrack 1 podle CP/M layout = FSMZ 16 sectors) má filler 0xff
    local out3b
    out3b=$("$MZDSK_DSK" "$dsk3" tracks --abstrack 1 2>/dev/null)
    assert_contains "$out3b" "Filler       : 0xff" "--format-cpm boot track filler 0xff" || return 1
}


run_test test_dsk_check_exit_code_on_errors
run_test test_dsk_repair_trailing_data
run_test test_dsk_rejects_non_dsk_file
run_test test_dsk_check_on_non_dsk_reports_error
run_test test_dsk_track_filler_metadata
run_test test_dsk_edit_sector_ids
run_test test_dsk_edit_sector_id_single
run_test test_dsk_edit_sector_ids_duplicate
run_test test_dsk_edit_sector_ids_bad_count
run_test test_dsk_edit_track_atomicity
run_test test_dsk_edit_header_atomicity
run_test test_dsk_repair_atomicity_bad_file

test_summary
