#!/bin/bash
# CLI testy: mzdsk-mrs - rozšířené operace (init, defrag, rename, file info).

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="


## Pomocná funkce: vytvoří MRS disk s jedním souborem
create_mrs_with_file() {
    local dsk="$1"
    local name="$2"
    local size="${3:-512}"

    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    # vytvořit testovací MZF
    local mzf="$TEST_TMPDIR/${name}.mzf"
    dd if=/dev/zero bs=128 count=1 of="$mzf" 2>/dev/null
    printf '\x01' | dd of="$mzf" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf '%s\x0d' "$name" | dd of="$mzf" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf "\\x$(printf '%02x' $((size & 0xFF)))\\x$(printf '%02x' $(((size >> 8) & 0xFF)))" \
        | dd of="$mzf" bs=1 seek=18 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$mzf" bs=1 seek=20 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$mzf" bs=1 seek=22 conv=notrunc 2>/dev/null
    dd if=/dev/urandom bs="$size" count=1 2>/dev/null >> "$mzf"

    "$MZDSK_MRS" "$dsk" put --mzf "$mzf" 2>/dev/null || return 1
}


## Počet souborů v MRS adresáři
mrs_file_count() {
    "$MZDSK_MRS" -o json "$1" dir 2>/dev/null | grep -c '"name"'
}


# === Init ===

## mzdsk-mrs init: vytvoří prázdný MRS filesystem
test_mrs_init() {
    local dsk="$TEST_TMPDIR/mrs_init.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1

    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null
    assert_eq "$?" "0" "init should succeed" || return 1

    local count=$(mrs_file_count "$dsk")
    assert_eq "$count" "0" "empty dir after init" || return 1
}


## mzdsk-mrs init --force: přeinicializuje existující MRS
test_mrs_init_force() {
    local dsk="$TEST_TMPDIR/mrs_init_force.dsk"
    create_mrs_with_file "$dsk" "BEFORE" 256 || return 1

    local count_before=$(mrs_file_count "$dsk")
    assert_eq "$count_before" "1" "1 file before reinit" || return 1

    "$MZDSK_MRS" -y "$dsk" init --force 2>/dev/null
    assert_eq "$?" "0" "init --force should succeed" || return 1

    local count_after=$(mrs_file_count "$dsk")
    assert_eq "$count_after" "0" "empty after reinit" || return 1
}


# === Defrag ===

## mzdsk-mrs defrag: zachová soubory
test_mrs_defrag() {
    local dsk="$TEST_TMPDIR/mrs_defrag.dsk"
    create_mrs_with_file "$dsk" "FIRST" 512 || return 1

    # přidat druhý a třetí soubor
    local mzf2="$TEST_TMPDIR/SECOND.mzf"
    dd if=/dev/zero bs=128 count=1 of="$mzf2" 2>/dev/null
    printf '\x01' | dd of="$mzf2" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf 'SECOND\x0d' | dd of="$mzf2" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf '\x00\x02' | dd of="$mzf2" bs=1 seek=18 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$mzf2" bs=1 seek=20 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$mzf2" bs=1 seek=22 conv=notrunc 2>/dev/null
    dd if=/dev/urandom bs=512 count=1 2>/dev/null >> "$mzf2"
    "$MZDSK_MRS" "$dsk" put --mzf "$mzf2" 2>/dev/null || return 1

    # smazat první
    "$MZDSK_MRS" "$dsk" era "FIRST" 2>/dev/null || return 1

    "$MZDSK_MRS" -y "$dsk" defrag 2>/dev/null
    assert_eq "$?" "0" "defrag should succeed" || return 1

    local count=$(mrs_file_count "$dsk")
    assert_eq "$count" "1" "1 file after defrag" || return 1
}


# === Rename ===

## mzdsk-mrs ren: přejmenování souboru
test_mrs_rename() {
    local dsk="$TEST_TMPDIR/mrs_ren.dsk"
    create_mrs_with_file "$dsk" "OLDNAME" 256 || return 1

    "$MZDSK_MRS" "$dsk" ren "OLDNAME" "NEWNAME.DAT" 2>/dev/null
    assert_eq "$?" "0" "rename should succeed" || return 1

    local output
    output=$("$MZDSK_MRS" -o json "$dsk" dir 2>/dev/null)
    assert_contains "$output" "NEWNAME" "dir contains new name" || return 1
}


## BUG 12: put se zkrácením jména vypíše varování
test_mrs_put_truncates_with_warning() {
    local dsk="$TEST_TMPDIR/mrs_trunc.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=512 count=1 2>/dev/null

    local out
    out=$("$MZDSK_MRS" "$dsk" put "$data" LONGNAMETEST.DAT 2>&1)
    assert_eq "$?" "0" "put should succeed" || return 1
    assert_contains "$out" "Warning" "warning about truncation printed" || return 1
    assert_contains "$out" "truncated to" "warning mentions truncation target" || return 1
    assert_contains "$out" "LONGNAME.DAT" "warning shows final name" || return 1

    # Ověření, že je v dir pod zkráceným jménem
    local dir
    dir=$("$MZDSK_MRS" -o json "$dsk" dir 2>/dev/null)
    assert_contains "$dir" "LONGNAME" "dir contains truncated name" || return 1
}


## BUG B4: warning o truncation musí ukazovat skutečně uložené jméno
## (compose_ascii_name - trim jen trailing mezer, ponech interní mezery)
test_mrs_put_truncation_warning_matches_stored_name() {
    local dsk="$TEST_TMPDIR/mrs_b4.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=512 count=1 2>/dev/null

    # "FLAPPY ver 1.0A" má interní mezeru před 'v' - truncation musí
    # produkovat "FLAPPY v.0A" (8 znaků name s mezerou + ext "0A"),
    # ne "FLAPPY.0A" (dřívější bug: cyklus končil na první mezeře).
    local out
    out=$("$MZDSK_MRS" "$dsk" put "$data" "FLAPPY ver 1.0A" 2>&1)
    assert_eq "$?" "0" "put with truncation should succeed" || return 1
    assert_contains "$out" "truncated to 'FLAPPY v.0A'" \
        "warning must show actual stored name with internal space" || return 1

    # Ověření, že disk obsahuje přesně "FLAPPY v" jako fname (8. znak mezera)
    local dir
    dir=$("$MZDSK_MRS" -o json "$dsk" dir 2>/dev/null)
    assert_contains "$dir" '"name": "FLAPPY v"' \
        "dir JSON shows fname with internal space" || return 1
    assert_contains "$dir" '"ext": "0A"' \
        "dir JSON shows correct ext" || return 1
}


## BUG 18: info rozepisuje Used blocks na FAT/DIR/Bad/Files a hlásí
## nekonzistenci mezi FAT file_blocks a součtem dir bsize.
## Disk s FAT inkonzistencí pochází z fixture_mrs_inconsistent.
test_mrs_info_block_breakdown() {
    fixture_mrs_inconsistent || return 1
    local dsk="$FIXTURE_MRS_DSK"

    # Textový výstup obsahuje breakdown všech kategorií
    local out
    out=$("$MZDSK_MRS" "$dsk" info 2>/dev/null)
    assert_contains "$out" "FAT:" "info contains FAT breakdown" || return 1
    assert_contains "$out" "Directory:" "info contains Directory breakdown" || return 1
    assert_contains "$out" "Files (FAT):" "info contains Files (FAT) breakdown" || return 1
    assert_contains "$out" "Files (dir):" "info contains Files (dir) breakdown" || return 1

    # Fixture disk má nekonzistenci (pre-defrag) - varování musí být přítomné
    assert_contains "$out" "run \`defrag\`" "inconsistency warning printed (BUG 18)" || return 1

    # JSON obsahuje nové klíče
    local json
    json=$("$MZDSK_MRS" -o json "$dsk" info 2>/dev/null)
    assert_contains "$json" '"fat_blocks"' "JSON has fat_blocks key" || return 1
    assert_contains "$json" '"dir_blocks"' "JSON has dir_blocks key" || return 1
    assert_contains "$json" '"file_blocks"' "JSON has file_blocks key" || return 1
    assert_contains "$json" '"dir_bsize_sum"' "JSON has dir_bsize_sum key" || return 1

    # Po defragu musí byt dir_bsize_sum == file_blocks (konzistentní)
    local work="$TEST_TMPDIR/mrs_bd.dsk"
    cp "$dsk" "$work"
    "$MZDSK_MRS" -y "$work" defrag >/dev/null 2>&1 || return 1
    local post
    post=$("$MZDSK_MRS" -o json "$work" info 2>/dev/null)
    local fat_fb dir_sum
    fat_fb=$(echo "$post" | grep -oP '"file_blocks":\s*\K\d+')
    dir_sum=$(echo "$post" | grep -oP '"dir_bsize_sum":\s*\K\d+')
    assert_eq "$fat_fb" "$dir_sum" "after defrag: FAT file_blocks == dir_bsize_sum" || return 1

    # Warning nesmí být v post-defrag výstupu
    local post_text
    post_text=$("$MZDSK_MRS" "$work" info 2>/dev/null)
    if echo "$post_text" | grep -q "run \`defrag\`"; then
        echo -e "    ${_RED}FAIL${_NC}: inconsistency warning leaked after defrag"
        return 1
    fi
}


## BUG 17: --output/-o funguje i za subpříkazem
test_mrs_output_after_subcmd() {
    fixture_mrs_inconsistent || return 1
    local dsk="$FIXTURE_MRS_DSK"

    local out1
    out1=$("$MZDSK_MRS" "$dsk" dir -o json 2>/dev/null)
    assert_contains "$out1" '"filesystem": "mrs"' "-o za subcmd" || return 1

    local out2
    out2=$("$MZDSK_MRS" "$dsk" dir --output=csv 2>/dev/null)
    assert_contains "$out2" "id,name,ext" "--output=VAL za subcmd (CSV header)" || return 1

    local out3
    out3=$("$MZDSK_MRS" "$dsk" dir --output csv 2>/dev/null)
    assert_contains "$out3" "id,name,ext" "--output VAL za subcmd" || return 1
}


## BUG 12: put s jménem, které po zkrácení způsobí duplicitu, selže
test_mrs_put_truncation_duplicate_fails() {
    local dsk="$TEST_TMPDIR/mrs_dupt.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=512 count=1 2>/dev/null

    "$MZDSK_MRS" "$dsk" put "$data" LONGNAMEA.DAT >/dev/null 2>&1 || return 1

    local out
    out=$("$MZDSK_MRS" "$dsk" put "$data" LONGNAMEB.DAT 2>&1)
    assert_neq "$?" "0" "duplicate after truncation must fail" || return 1
    assert_contains "$out" "Warning" "warning still printed before failure" || return 1
    assert_contains "$out" "File exists" "error message mentions duplicate" || return 1
}


# === Get --all ===

## Pomocná funkce: vytvoří MRS disk s N soubory
create_mrs_with_files() {
    local dsk="$1"
    shift
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    for name in "$@"; do
        local mzf="$TEST_TMPDIR/${name}.mzf"
        dd if=/dev/zero bs=128 count=1 of="$mzf" 2>/dev/null
        printf '\x01' | dd of="$mzf" bs=1 seek=0 conv=notrunc 2>/dev/null
        printf '%s\x0d' "$name" | dd of="$mzf" bs=1 seek=1 conv=notrunc 2>/dev/null
        printf '\x00\x02' | dd of="$mzf" bs=1 seek=18 conv=notrunc 2>/dev/null
        printf '\x00\x20' | dd of="$mzf" bs=1 seek=20 conv=notrunc 2>/dev/null
        printf '\x00\x30' | dd of="$mzf" bs=1 seek=22 conv=notrunc 2>/dev/null
        dd if=/dev/urandom bs=512 count=1 2>/dev/null >> "$mzf"
        "$MZDSK_MRS" "$dsk" put --mzf "$mzf" 2>/dev/null || return 1
    done
}


## get --all: raw export všech souborů
test_mrs_get_all_raw() {
    local dsk="$TEST_TMPDIR/mrs_ga_raw.dsk"
    create_mrs_with_files "$dsk" "AA.DAT" "BB.MRS" "CC.DAT" || return 1

    local outdir="$TEST_TMPDIR/ga_raw"
    "$MZDSK_MRS" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "get --all raw should succeed" || return 1

    # Ověřit, že existují 3 soubory
    assert_file_exists "$outdir/AA.DAT" "AA.DAT exported" || return 1
    assert_file_exists "$outdir/BB.MRS" "BB.MRS exported" || return 1
    assert_file_exists "$outdir/CC.DAT" "CC.DAT exported" || return 1

    # Každý soubor má 512 B (1 blok)
    local size=$(wc -c < "$outdir/AA.DAT" | tr -d ' ')
    assert_eq "$size" "512" "raw file size = 512" || return 1
}


## get --all --mzf: MZF export všech souborů
test_mrs_get_all_mzf() {
    local dsk="$TEST_TMPDIR/mrs_ga_mzf.dsk"
    create_mrs_with_files "$dsk" "DD.DAT" "EE.MRS" || return 1

    local outdir="$TEST_TMPDIR/ga_mzf"
    "$MZDSK_MRS" --overwrite "$dsk" get --all --mzf "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "get --all --mzf should succeed" || return 1

    assert_file_exists "$outdir/DD.DAT.mzf" "DD.DAT.mzf exported" || return 1
    assert_file_exists "$outdir/EE.MRS.mzf" "EE.MRS.mzf exported" || return 1

    # MZF soubor má 128 + 512 = 640 B
    local size=$(wc -c < "$outdir/DD.DAT.mzf" | tr -d ' ')
    assert_eq "$size" "640" "MZF file size = 640" || return 1
}


## get --all --mzf --ftype: override MZF ftype
test_mrs_get_all_mzf_ftype() {
    local dsk="$TEST_TMPDIR/mrs_ga_ft.dsk"
    create_mrs_with_files "$dsk" "FF.DAT" || return 1

    local outdir="$TEST_TMPDIR/ga_ftype"
    "$MZDSK_MRS" --overwrite "$dsk" get --all --mzf --ftype 0x22 "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "get --all --mzf --ftype should succeed" || return 1

    # Kontrola ftype bajtu v MZF hlavičce (offset 0)
    local ftype
    ftype=$(od -An -tx1 -N1 "$outdir/FF.DAT.mzf" | tr -d ' ')
    assert_eq "$ftype" "22" "MZF ftype = 0x22" || return 1
}


## get --all --on-duplicate rename: přejmenuje kolidující soubory
test_mrs_get_all_dup_rename() {
    local dsk="$TEST_TMPDIR/mrs_ga_dup.dsk"
    create_mrs_with_files "$dsk" "GG.DAT" "HH.MRS" || return 1

    local outdir="$TEST_TMPDIR/ga_dup_rn"
    # První export
    "$MZDSK_MRS" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1 || return 1
    # Druhý export - soubory by se měly přejmenovat na ~2
    "$MZDSK_MRS" --overwrite "$dsk" get --all --on-duplicate rename "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "second export with rename should succeed" || return 1

    assert_file_exists "$outdir/GG~2.DAT" "GG~2.DAT created" || return 1
    assert_file_exists "$outdir/HH~2.MRS" "HH~2.MRS created" || return 1
}


## get --all --on-duplicate skip: přeskočí existující soubory
test_mrs_get_all_dup_skip() {
    local dsk="$TEST_TMPDIR/mrs_ga_skip.dsk"
    create_mrs_with_files "$dsk" "II.DAT" || return 1

    local outdir="$TEST_TMPDIR/ga_dup_sk"
    "$MZDSK_MRS" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1 || return 1

    local sha_before=$(sha1sum "$outdir/II.DAT" | awk '{print $1}')

    local out
    out=$("$MZDSK_MRS" --overwrite "$dsk" get --all --on-duplicate skip "$outdir" 2>&1)
    assert_eq "$?" "0" "skip export should succeed" || return 1
    assert_contains "$out" "Skipping" "stderr contains Skipping warning" || return 1

    local sha_after=$(sha1sum "$outdir/II.DAT" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "file unchanged after skip" || return 1
}


## get --all --on-duplicate overwrite: přepíše existující soubory
test_mrs_get_all_dup_overwrite() {
    local dsk="$TEST_TMPDIR/mrs_ga_ovw.dsk"
    create_mrs_with_files "$dsk" "JJ.DAT" || return 1

    local outdir="$TEST_TMPDIR/ga_dup_ow"
    "$MZDSK_MRS" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1 || return 1

    # Druhý export s overwrite - nesmí selhat
    "$MZDSK_MRS" --overwrite "$dsk" get --all --on-duplicate overwrite "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "overwrite export should succeed" || return 1

    assert_file_exists "$outdir/JJ.DAT" "JJ.DAT still exists after overwrite" || return 1
}


## get --all z prázdného disku
test_mrs_get_all_empty() {
    local dsk="$TEST_TMPDIR/mrs_ga_empty.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local outdir="$TEST_TMPDIR/ga_empty"
    local out
    out=$("$MZDSK_MRS" --overwrite "$dsk" get --all "$outdir" 2>&1)
    assert_eq "$?" "0" "get --all from empty disk should succeed" || return 1
    assert_contains "$out" "No files found" "output says no files" || return 1
}


## BUG B7: get --all při chybách u některých souborů musí vrátit exit 1
## I když se jiné soubory povedly extrahovat, errors > 0 znamená
## částečné selhání a skript to musí mít šanci detekovat.
## Fixture disk obsahuje soubory s bsize=1, ale FAT má 0 bloků pro daný file_id
## - fsmrs_read_file na nich selže, ale FIXTURE_MRS_OK_FILES souborů se extrahuje OK.
test_mrs_get_all_exit_on_errors() {
    fixture_mrs_with_read_errors || return 1

    local outdir="$TEST_TMPDIR/ga_errors"
    "$MZDSK_MRS" "$FIXTURE_MRS_ERR_DSK" get --all "$outdir" >/dev/null 2>&1
    assert_neq "$?" "0" "get --all with read errors must return non-zero" || return 1

    # Přesto se musí extrahovat aspoň nějaké soubory
    local count=$(ls "$outdir" 2>/dev/null | wc -l)
    [ "$count" -gt "0" ] || { echo "    FAIL: no files extracted"; return 1; }
}


## get --all --ftype bez --mzf -> chyba
test_mrs_get_all_ftype_requires_mzf() {
    local dsk="$TEST_TMPDIR/mrs_ga_ftmzf.dsk"
    create_mrs_with_files "$dsk" "KK.DAT" || return 1

    "$MZDSK_MRS" --overwrite "$dsk" get --all --ftype 0x22 "$TEST_TMPDIR/ga_ft_err" >/dev/null 2>&1
    assert_neq "$?" "0" "get --all --ftype without --mzf must fail" || return 1
}


## get --all --exec-addr a --strt-addr override
test_mrs_get_all_addr_override() {
    local dsk="$TEST_TMPDIR/mrs_ga_addr.dsk"
    create_mrs_with_files "$dsk" "LL.DAT" || return 1

    local outdir="$TEST_TMPDIR/ga_addr"
    "$MZDSK_MRS" --overwrite "$dsk" get --all --mzf --exec-addr 0x2000 --strt-addr 0x3000 "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "get --all with addr override should succeed" || return 1

    # Kontrola fexec (offset 22-23, LE) a fstrt (offset 20-21, LE) v MZF
    local fexec fstrt
    fexec=$(od -An -tx1 -j22 -N2 "$outdir/LL.DAT.mzf" | tr -d ' ')
    fstrt=$(od -An -tx1 -j20 -N2 "$outdir/LL.DAT.mzf" | tr -d ' ')
    assert_eq "$fexec" "0020" "MZF fexec = 0x2000 (LE)" || return 1
    assert_eq "$fstrt" "0030" "MZF fstrt = 0x3000 (LE)" || return 1
}


## Atomicita era - smazání neexistujícího souboru nesmí změnit DSK (memory driver)
test_mrs_era_atomicity() {
    local dsk="$TEST_TMPDIR/mrs_atom_era.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_MRS" "$dsk" era NONE.DAT >/dev/null 2>&1
    assert_neq "$?" "0" "era nonexistent must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after era failure" || return 1
}

## Atomicita put - put neexistujícího zdrojového souboru nesmí změnit DSK
test_mrs_put_atomicity() {
    local dsk="$TEST_TMPDIR/mrs_atom_put.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_MRS" "$dsk" put /does/not/exist.dat >/dev/null 2>&1
    assert_neq "$?" "0" "put nonexistent must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after put failure" || return 1
}


## --name strict: validní jméno bez varování projde
test_mrs_put_strict_name_ok() {
    local dsk="$TEST_TMPDIR/mrs_strict_ok.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null

    local out
    out=$("$MZDSK_MRS" "$dsk" put "$data" --name HELLO.TXT 2>&1)
    assert_eq "$?" "0" "strict --name HELLO.TXT should succeed" || return 1
    if echo "$out" | grep -q "Warning"; then
        echo "  unexpected warning in strict mode: $out"
        return 1
    fi
    local dir
    dir=$("$MZDSK_MRS" -o json "$dsk" dir 2>/dev/null)
    assert_contains "$dir" "HELLO" "dir contains stored name" || return 1
}


## --name strict: dlouhé jméno musí selhat bez truncation
test_mrs_put_strict_name_too_long_fails() {
    local dsk="$TEST_TMPDIR/mrs_strict_long.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    local out
    out=$("$MZDSK_MRS" "$dsk" put "$data" --name LONGNAMETEST.DAT 2>&1)
    assert_neq "$?" "0" "strict --name too long must fail" || return 1
    assert_contains "$out" "exceeds 8.3 limit" "error mentions 8.3 limit" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after strict fail" || return 1
}


## --name strict: zakázaný znak musí selhat
test_mrs_put_strict_name_forbidden_char_fails() {
    local dsk="$TEST_TMPDIR/mrs_strict_char.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null

    local out
    out=$("$MZDSK_MRS" "$dsk" put "$data" --name 'A?B.DAT' 2>&1)
    assert_neq "$?" "0" "strict --name with '?' must fail" || return 1
    assert_contains "$out" "forbidden character" "error mentions forbidden character" || return 1
}


## --name strict: konflikt s pozičním argumentem
test_mrs_put_strict_name_conflict_with_positional() {
    local dsk="$TEST_TMPDIR/mrs_strict_conf.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null

    local out
    out=$("$MZDSK_MRS" "$dsk" put "$data" HELLO.DAT --name OTHER.DAT 2>&1)
    assert_neq "$?" "0" "positional + --name must fail" || return 1
    assert_contains "$out" "conflicts with positional" "error about conflict" || return 1
}


## --name strict u put --mzf: validní override projde
test_mrs_put_mzf_strict_name_ok() {
    local dsk="$TEST_TMPDIR/mrs_mzf_strict.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null

    "$MZDSK_MRS" "$dsk" put "$data" --name SRC.BIN >/dev/null 2>&1 || return 1
    local mzf="$TEST_TMPDIR/src.mzf"
    "$MZDSK_MRS" --overwrite "$dsk" get --mzf SRC.BIN "$mzf" >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$dsk" --preset mrs --overwrite 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    "$MZDSK_MRS" "$dsk" put --mzf "$mzf" --name OTHER.DAT >/dev/null 2>&1
    assert_eq "$?" "0" "strict put --mzf --name should succeed" || return 1
    local dir
    dir=$("$MZDSK_MRS" -o json "$dsk" dir 2>/dev/null)
    assert_contains "$dir" "OTHER" "dir contains overridden name" || return 1
    if echo "$dir" | grep -q '"name":\s*"SRC"'; then
        echo "  MZF header name leaked into dir: $dir"
        return 1
    fi
}


## --name strict u put --mzf: dlouhé jméno selže
test_mrs_put_mzf_strict_name_fails() {
    local dsk="$TEST_TMPDIR/mrs_mzf_strict_fail.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null
    "$MZDSK_MRS" "$dsk" put "$data" --name SRC.BIN >/dev/null 2>&1 || return 1
    local mzf="$TEST_TMPDIR/src.mzf"
    "$MZDSK_MRS" --overwrite "$dsk" get --mzf SRC.BIN "$mzf" >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$dsk" --preset mrs --overwrite 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local out
    out=$("$MZDSK_MRS" "$dsk" put --mzf "$mzf" --name TOOLONGNAME.XYZ 2>&1)
    assert_neq "$?" "0" "strict --mzf --name too long must fail" || return 1
    assert_contains "$out" "exceeds 8.3 limit" "error mentions 8.3 limit" || return 1
}


## put --mzf odvození jména z MZF selže informativně, ne generickým "Bad name".
## Když MZF fname začíná mezerou (např. " F1200"), odvozený MRS název nelze použít -
## CLI musí uživateli poradit explicitní override.
test_mrs_put_mzf_bad_derived_name_message() {
    local dsk="$TEST_TMPDIR/mrs_mzf_bad_derive.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init >/dev/null 2>&1 || return 1

    # MZF s leading space v fname
    local mzf="$TEST_TMPDIR/leadspace.mzf"
    printf '\x01 F1200\x0D' > "$mzf"
    dd if=/dev/zero of="$mzf" bs=1 count=10 seek=8 conv=notrunc 2>/dev/null
    printf '\x04\x00\x00\x00\x00\x00' >> "$mzf"
    dd if=/dev/zero of="$mzf" bs=1 count=104 seek=24 conv=notrunc 2>/dev/null
    printf '\xDE\xAD\xBE\xEF' >> "$mzf"

    local out
    out=$("$MZDSK_MRS" "$dsk" put --mzf "$mzf" 2>&1)
    assert_neq "$?" "0" "put --mzf with unusable derived name must fail" || return 1
    assert_contains "$out" "Could not derive usable MRS filename" "error explains problem" || return 1
    assert_contains "$out" "specify name explicitly" "error suggests explicit override" || return 1

    # Po ručním override musí put projít
    "$MZDSK_MRS" "$dsk" put --mzf "$mzf" F1200.DAT >/dev/null 2>&1
    assert_eq "$?" "0" "explicit override must succeed" || return 1
}


## --charset jp na put --mzf: neznámá hodnota selže s informativní chybou.
test_mrs_put_mzf_charset_invalid() {
    local dsk="$TEST_TMPDIR/mrs_charset_inv.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init >/dev/null 2>&1 || return 1

    local mzf="$TEST_TMPDIR/dummy.mzf"
    printf '\x01TEST\x0D' > "$mzf"
    dd if=/dev/zero of="$mzf" bs=1 count=123 seek=5 conv=notrunc 2>/dev/null
    printf '\x00\x00\x00\x00\x00\x00\x00' >> "$mzf"

    local out
    out=$("$MZDSK_MRS" "$dsk" put --mzf "$mzf" --charset klingon 2>&1)
    assert_neq "$?" "0" "unknown charset must fail" || return 1
    assert_contains "$out" "Unknown --charset" "error mentions unknown charset" || return 1
}


## put --mzf poziční override stále používá tolerantní truncation (backward compat)
test_mrs_put_mzf_positional_still_lax() {
    local dsk="$TEST_TMPDIR/mrs_mzf_lax.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null
    "$MZDSK_MRS" "$dsk" put "$data" --name SRC.BIN >/dev/null 2>&1 || return 1
    local mzf="$TEST_TMPDIR/src.mzf"
    "$MZDSK_MRS" --overwrite "$dsk" get --mzf SRC.BIN "$mzf" >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$dsk" --preset mrs --overwrite 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local out
    out=$("$MZDSK_MRS" "$dsk" put --mzf "$mzf" LONGNAMETEST.DAT 2>&1)
    assert_eq "$?" "0" "positional lax must succeed with truncation" || return 1
    assert_contains "$out" "Warning" "tolerant path still warns" || return 1
    assert_contains "$out" "truncated to" "warning mentions truncation" || return 1
}


# --- Spuštění ---

run_test test_mrs_init
run_test test_mrs_init_force
run_test test_mrs_defrag
run_test test_mrs_rename
run_test test_mrs_put_truncates_with_warning
run_test test_mrs_put_truncation_warning_matches_stored_name
run_test test_mrs_put_truncation_duplicate_fails
run_test test_mrs_output_after_subcmd
run_test test_mrs_info_block_breakdown
run_test test_mrs_era_atomicity
run_test test_mrs_put_atomicity
run_test test_mrs_get_all_raw
run_test test_mrs_get_all_mzf
run_test test_mrs_get_all_mzf_ftype
run_test test_mrs_get_all_dup_rename
run_test test_mrs_get_all_dup_skip
run_test test_mrs_get_all_dup_overwrite
run_test test_mrs_get_all_empty
run_test test_mrs_get_all_exit_on_errors
run_test test_mrs_get_all_ftype_requires_mzf
run_test test_mrs_get_all_addr_override
run_test test_mrs_put_strict_name_ok
run_test test_mrs_put_strict_name_too_long_fails
run_test test_mrs_put_strict_name_forbidden_char_fails
run_test test_mrs_put_strict_name_conflict_with_positional
run_test test_mrs_put_mzf_strict_name_ok
run_test test_mrs_put_mzf_strict_name_fails
run_test test_mrs_put_mzf_bad_derived_name_message
run_test test_mrs_put_mzf_charset_invalid
run_test test_mrs_put_mzf_positional_still_lax

test_summary
