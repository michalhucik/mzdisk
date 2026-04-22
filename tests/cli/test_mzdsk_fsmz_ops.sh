#!/bin/bash
# CLI testy: mzdsk-fsmz - rozšířené operace (repair, bootstrap).

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="


## Počet souborů v FSMZ adresáři
fsmz_file_count() {
    "$MZDSK_FSMZ" -o json "$1" dir 2>/dev/null | grep -c '"id"'
}


## Vytvoří testovací MZF soubor
create_test_mzf() {
    local out="$1"
    local name="$2"
    local size="$3"

    dd if=/dev/zero bs=128 count=1 of="$out" 2>/dev/null
    printf '\x01' | dd of="$out" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf '\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d\x0d' \
        | dd of="$out" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf '%s\x0d' "$name" | dd of="$out" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf "\\x$(printf '%02x' $((size & 0xFF)))\\x$(printf '%02x' $(((size >> 8) & 0xFF)))" \
        | dd of="$out" bs=1 seek=18 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$out" bs=1 seek=20 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$out" bs=1 seek=22 conv=notrunc 2>/dev/null
    dd if=/dev/urandom bs="$size" count=1 2>/dev/null >> "$out"
}


# === Repair ===

## mzdsk-fsmz repair: opraví DINFO bitmapu
test_fsmz_repair() {
    local dsk="$TEST_TMPDIR/fsmz_repair.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    # přidat soubory
    create_test_mzf "$TEST_TMPDIR/rep1.mzf" "REPA" 256
    create_test_mzf "$TEST_TMPDIR/rep2.mzf" "REPB" 256
    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/rep1.mzf" 2>/dev/null || return 1
    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/rep2.mzf" 2>/dev/null || return 1

    # repair
    "$MZDSK_FSMZ" "$dsk" repair 2>/dev/null
    assert_eq "$?" "0" "repair should succeed" || return 1

    # soubory musí zůstat
    local count=$(fsmz_file_count "$dsk")
    assert_eq "$count" "2" "2 files after repair" || return 1
}


# === Bootstrap put/get ===

## mzdsk-fsmz bootstrap: put -> get roundtrip
test_fsmz_bootstrap_put_get() {
    local dsk="$TEST_TMPDIR/fsmz_boot.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    # vytvořit malý bootstrap MZF (typ 0x03 = BSD/bootstrap)
    local boot_mzf="$TEST_TMPDIR/boot.mzf"
    dd if=/dev/zero bs=128 count=1 of="$boot_mzf" 2>/dev/null
    printf '\x03' | dd of="$boot_mzf" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf 'IPLPRO\x0d' | dd of="$boot_mzf" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf '\x00\x01' | dd of="$boot_mzf" bs=1 seek=18 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$boot_mzf" bs=1 seek=20 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$boot_mzf" bs=1 seek=22 conv=notrunc 2>/dev/null
    dd if=/dev/urandom bs=256 count=1 2>/dev/null >> "$boot_mzf"

    "$MZDSK_FSMZ" "$dsk" boot put "$boot_mzf" 2>/dev/null
    assert_eq "$?" "0" "boot put should succeed" || return 1

    # get
    local boot_out="$TEST_TMPDIR/boot_out.mzf"
    "$MZDSK_FSMZ" "$dsk" boot get "$boot_out" 2>/dev/null
    assert_eq "$?" "0" "boot get should succeed" || return 1

    assert_file_exists "$boot_out" "bootstrap output file" || return 1
}


## mzdsk-fsmz era: uzamčený soubor nelze smazat bez --force
test_fsmz_era_respects_lock() {
    local dsk="$TEST_TMPDIR/fsmz_lock_era.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    create_test_mzf "$TEST_TMPDIR/locked.mzf" "LCKD" 256
    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/locked.mzf" 2>/dev/null || return 1

    local before=$(fsmz_file_count "$dsk")
    assert_eq "$before" "1" "1 file before lock" || return 1

    "$MZDSK_FSMZ" "$dsk" lock --id 0 1 2>/dev/null || return 1

    # era bez --force musí selhat s exit != 0
    "$MZDSK_FSMZ" "$dsk" era --id 0 >/dev/null 2>&1
    assert_neq "$?" "0" "era on locked file must fail" || return 1

    local after_locked=$(fsmz_file_count "$dsk")
    assert_eq "$after_locked" "1" "file kept after blocked era" || return 1

    # era --force musí projít a soubor odstranit
    "$MZDSK_FSMZ" "$dsk" era --id 0 --force >/dev/null 2>&1
    assert_eq "$?" "0" "era --force should succeed" || return 1

    local after_force=$(fsmz_file_count "$dsk")
    assert_eq "$after_force" "0" "file removed after era --force" || return 1
}


## mzdsk-fsmz ren: uzamčený soubor nelze přejmenovat bez --force
test_fsmz_ren_respects_lock() {
    local dsk="$TEST_TMPDIR/fsmz_lock_ren.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    create_test_mzf "$TEST_TMPDIR/rn.mzf" "ORIG" 256
    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/rn.mzf" 2>/dev/null || return 1
    "$MZDSK_FSMZ" "$dsk" lock --id 0 1 2>/dev/null || return 1

    # ren bez --force musí selhat
    "$MZDSK_FSMZ" "$dsk" ren --id 0 NEW >/dev/null 2>&1
    assert_neq "$?" "0" "ren on locked file must fail" || return 1

    local still_orig=$("$MZDSK_FSMZ" -o json "$dsk" dir 2>/dev/null | grep -c '"ORIG"')
    assert_eq "$still_orig" "1" "original name kept after blocked ren" || return 1

    # ren --force musí projít
    "$MZDSK_FSMZ" "$dsk" ren --id 0 NEW --force >/dev/null 2>&1
    assert_eq "$?" "0" "ren --force should succeed" || return 1

    local now_new=$("$MZDSK_FSMZ" -o json "$dsk" dir 2>/dev/null | grep -c '"NEW"')
    assert_eq "$now_new" "1" "file renamed after ren --force" || return 1
}


## BUG C1 regression: put po era --force nezdědí lock flag z původní položky.
## Reprodukce byla: put Flappy -> lock --id X 1 -> era --id X --force ->
## put Bomber -> nový soubor v tomtéž slotu ukazoval locked=1, přestože
## nebyl uzamčen. Příčina: fsmz_write_file() nenastavovala diritem->locked
## po čtení volného slotu (kde zbyla 1 po dříve smazané zamčené položce).
test_fsmz_put_does_not_inherit_lock_flag() {
    local dsk="$TEST_TMPDIR/fsmz_c1_lockleak.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    create_test_mzf "$TEST_TMPDIR/lf1.mzf" "AAAA" 256
    create_test_mzf "$TEST_TMPDIR/lf2.mzf" "BBBB" 256

    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/lf1.mzf" 2>/dev/null || return 1

    # id prvního souboru je 0 (prázdný disk před put)
    "$MZDSK_FSMZ" "$dsk" lock --id 0 1 2>/dev/null || return 1
    "$MZDSK_FSMZ" "$dsk" era --id 0 --force 2>/dev/null || return 1

    # Nový put do stejného slotu
    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/lf2.mzf" 2>/dev/null || return 1

    # Nový soubor NESMÍ být locked - zdědil by flag po smazané AAAA
    local locked=$("$MZDSK_FSMZ" -o json "$dsk" dir 2>/dev/null \
        | grep -oE '"locked":\s*(true|false)' | head -1 | grep -oE '(true|false)')
    assert_eq "$locked" "false" "new put must not inherit lock from erased predecessor" \
        || return 1

    # Sanity: era bez --force teď MUSÍ projít (soubor není locked)
    "$MZDSK_FSMZ" "$dsk" era --id 0 >/dev/null 2>&1
    assert_eq "$?" "0" "era without --force should succeed on fresh (unlocked) file" || return 1
}


## Doplněk k C1: první put do prázdného slotu na čistém disku - žádný
## soubor nikdy nebyl uzamčen, přesto ověříme, že locked je 0 (ne
## neinicializovaný zbytek z přednastavené struktury).
test_fsmz_put_fresh_slot_not_locked() {
    local dsk="$TEST_TMPDIR/fsmz_c1_fresh.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    create_test_mzf "$TEST_TMPDIR/fresh.mzf" "FRSH" 256
    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/fresh.mzf" 2>/dev/null || return 1

    local locked=$("$MZDSK_FSMZ" -o json "$dsk" dir 2>/dev/null \
        | grep -oE '"locked":\s*(true|false)' | head -1 | grep -oE '(true|false)')
    assert_eq "$locked" "false" "fresh put on empty disk must have locked=false" \
        || return 1
}


## BUG C4 regression: při existujícím výstupním souboru u `get`/`get-block`
## se vypisovala dvojí chyba - konkrétní ("already exists...") + generická
## ("Error: Invalid parameter"). Druhá byla artefakt main loop fallbacku,
## protože handler vracel INVALID_PARAM i když už chybu vypsal sám.
test_fsmz_get_no_duplicate_error_on_existing_output() {
    local dsk="$TEST_TMPDIR/fsmz_c4_get.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    create_test_mzf "$TEST_TMPDIR/c4in.mzf" "CFR1" 256
    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/c4in.mzf" 2>/dev/null || return 1

    local out="$TEST_TMPDIR/c4out.mzf"
    printf 'existing' > "$out"

    local err
    err=$("$MZDSK_FSMZ" "$dsk" get --id 0 "$out" 2>&1 >/dev/null)
    local rc=$?

    assert_eq "$rc" "1" "get on existing output must fail with rc=1" || return 1
    assert_contains "$err" "already exists" "specific error must be printed" || return 1
    assert_not_contains "$err" "Invalid parameter" \
        "generic 'Invalid parameter' fallback must not duplicate" || return 1
}


## Doplněk C4 pro cmd_get_block - stejná cesta přes check_output_overwrite.
test_fsmz_get_block_no_duplicate_error_on_existing_output() {
    local dsk="$TEST_TMPDIR/fsmz_c4_getblock.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local out="$TEST_TMPDIR/c4_blk.bin"
    printf 'existing' > "$out"

    local err
    err=$("$MZDSK_FSMZ" "$dsk" get-block 0 "$out" 2>&1 >/dev/null)
    local rc=$?

    assert_eq "$rc" "1" "get-block on existing output must fail with rc=1" || return 1
    assert_contains "$err" "already exists" "specific error must be printed" || return 1
    assert_not_contains "$err" "Invalid parameter" \
        "generic 'Invalid parameter' fallback must not duplicate" || return 1
}


## Sanity: --overwrite umožní přepsat existující výstup.
test_fsmz_get_overwrite_flag_allows_overwrite() {
    local dsk="$TEST_TMPDIR/fsmz_c4_overw.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    create_test_mzf "$TEST_TMPDIR/c4ow.mzf" "COW1" 256
    "$MZDSK_FSMZ" "$dsk" put "$TEST_TMPDIR/c4ow.mzf" 2>/dev/null || return 1

    local out="$TEST_TMPDIR/c4ow_out.mzf"
    printf 'existing' > "$out"

    "$MZDSK_FSMZ" --overwrite "$dsk" get --id 0 "$out" >/dev/null 2>&1
    assert_eq "$?" "0" "--overwrite must allow replacing existing output" || return 1
}


## Helper: vytvoří IPLPRO-style bootstrap MZF (ftype=0x03, magic "IPLPRO").
## Použito pro boot put testy a následné set_header testy.
create_boot_mzf() {
    local out="$1"
    local size="${2:-256}"
    dd if=/dev/zero bs=128 count=1 of="$out" 2>/dev/null
    printf '\x03' | dd of="$out" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf 'IPLPRO\x0d' | dd of="$out" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf "\\x$(printf '%02x' $((size & 0xFF)))\\x$(printf '%02x' $(((size >> 8) & 0xFF)))" \
        | dd of="$out" bs=1 seek=18 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$out" bs=1 seek=20 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$out" bs=1 seek=22 conv=notrunc 2>/dev/null
    dd if=/dev/urandom bs="$size" count=1 2>/dev/null >> "$out"
}


## BUG E1 regression: boot --fstrt/--fexec/--ftype/--name na disku bez
## platného IPLPRO musí selhat (rc != 0) a NEMODIFIKOVAT disk. Dříve volání
## zapsalo partial header bez "IPLPRO" magic a vrátilo rc=0, takže disk
## skončil v nekonzistentním stavu.
test_fsmz_boot_set_rejects_without_iplpro() {
    local dsk="$TEST_TMPDIR/fsmz_e1_noipl.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    # --format-basic neinstaluje IPLPRO; sanity check přes boot info
    local info_out
    info_out=$("$MZDSK_FSMZ" "$dsk" boot 2>&1)
    assert_contains "$info_out" "Not found valid IPLPRO" \
        "sanity: disk po --format-basic nemá platný IPLPRO" || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # Každá z --fstrt/--fexec/--ftype/--name musí selhat bez modifikace disku.
    local err

    err=$("$MZDSK_FSMZ" "$dsk" boot --fstrt 0x1234 2>&1)
    local rc=$?
    assert_neq "$rc" "0" "boot --fstrt without IPLPRO must rc!=0" || return 1
    assert_contains "$err" "No valid IPLPRO" "specific error for --fstrt" || return 1
    assert_not_contains "$err" "Error: Unknown error" \
        "no duplicate 'Unknown error' fallback" || return 1

    err=$("$MZDSK_FSMZ" "$dsk" boot --fexec 0xABCD 2>&1)
    rc=$?
    assert_neq "$rc" "0" "boot --fexec without IPLPRO must rc!=0" || return 1
    assert_contains "$err" "No valid IPLPRO" "specific error for --fexec" || return 1

    err=$("$MZDSK_FSMZ" "$dsk" boot --ftype 01 2>&1)
    rc=$?
    assert_neq "$rc" "0" "boot --ftype without IPLPRO must rc!=0" || return 1
    assert_contains "$err" "No valid IPLPRO" "specific error for --ftype" || return 1

    err=$("$MZDSK_FSMZ" "$dsk" boot --name HACK 2>&1)
    rc=$?
    assert_neq "$rc" "0" "boot --name without IPLPRO must rc!=0" || return 1
    assert_contains "$err" "No valid IPLPRO" "specific error for --name" || return 1

    # Kombinace více voleb najednou
    err=$("$MZDSK_FSMZ" "$dsk" boot --fstrt 0x1111 --fexec 0x2222 --ftype 03 --name X 2>&1)
    rc=$?
    assert_neq "$rc" "0" "boot combined set without IPLPRO must rc!=0" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" \
        "E1 regression: disk must be UNCHANGED after all failed set attempts" || return 1
}


## Pozitivní test: boot --fstrt na disku s platným IPLPRO správně update-uje
## start address a disk zůstává validní.
test_fsmz_boot_set_fstrt_updates_field() {
    local dsk="$TEST_TMPDIR/fsmz_e1_fstrt.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local boot_mzf="$TEST_TMPDIR/e1_boot.mzf"
    create_boot_mzf "$boot_mzf" 256
    "$MZDSK_FSMZ" "$dsk" boot put "$boot_mzf" 2>/dev/null || return 1

    "$MZDSK_FSMZ" "$dsk" boot --fstrt 0x5000 >/dev/null 2>&1
    assert_eq "$?" "0" "boot --fstrt on valid IPLPRO must succeed" || return 1

    local start
    start=$("$MZDSK_FSMZ" -o json "$dsk" boot 2>/dev/null \
        | grep -oE '"start":\s*[0-9]+' | grep -oE '[0-9]+')
    assert_eq "$start" "20480" "fstrt set to 0x5000 (20480 decimal)" || return 1
}


## Pozitivní test: boot --fexec
test_fsmz_boot_set_fexec_updates_field() {
    local dsk="$TEST_TMPDIR/fsmz_e1_fexec.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local boot_mzf="$TEST_TMPDIR/e1_boot_fe.mzf"
    create_boot_mzf "$boot_mzf" 256
    "$MZDSK_FSMZ" "$dsk" boot put "$boot_mzf" 2>/dev/null || return 1

    "$MZDSK_FSMZ" "$dsk" boot --fexec 0x6000 >/dev/null 2>&1
    assert_eq "$?" "0" "boot --fexec on valid IPLPRO must succeed" || return 1

    local exec_addr
    exec_addr=$("$MZDSK_FSMZ" -o json "$dsk" boot 2>/dev/null \
        | grep -oE '"exec":\s*[0-9]+' | grep -oE '[0-9]+')
    assert_eq "$exec_addr" "24576" "fexec set to 0x6000 (24576 decimal)" || return 1
}


## Pozitivní test: boot --ftype změní typ bootstrapu.
## IPLPRO magic vyžaduje ftype=0x03, takže změna na jinou hodnotu invalidizuje
## hlavičku (boot info by vrátilo "Not found valid IPLPRO!"). Ověřujeme
## přímo bajt 0 FSMZ bloku 0 přes mzdsk-fsmz dump-block (bere FSMZ adresování,
## ne raw DSK sektorové).
test_fsmz_boot_set_ftype_updates_field() {
    local dsk="$TEST_TMPDIR/fsmz_e1_ftype.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local boot_mzf="$TEST_TMPDIR/e1_boot_ft.mzf"
    create_boot_mzf "$boot_mzf" 256
    "$MZDSK_FSMZ" "$dsk" boot put "$boot_mzf" 2>/dev/null || return 1

    "$MZDSK_FSMZ" "$dsk" boot --ftype 05 >/dev/null 2>&1
    assert_eq "$?" "0" "boot --ftype on valid IPLPRO must succeed" || return 1

    # FSMZ blok 0 = Track 1, Sector 1 (po boot track). Bajt 0 = ftype.
    local first_byte
    first_byte=$("$MZDSK_FSMZ" "$dsk" dump-block 0 16 2>/dev/null \
        | grep '^0x0000:' | awk '{print $2}')
    assert_eq "$first_byte" "05" "ftype byte at FSMZ block 0 offset 0 set to 0x05" || return 1
}


## Pozitivní test: boot --name změní jméno bootstrapu.
test_fsmz_boot_set_name_updates_field() {
    local dsk="$TEST_TMPDIR/fsmz_e1_name.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local boot_mzf="$TEST_TMPDIR/e1_boot_nm.mzf"
    create_boot_mzf "$boot_mzf" 256
    "$MZDSK_FSMZ" "$dsk" boot put "$boot_mzf" 2>/dev/null || return 1

    "$MZDSK_FSMZ" "$dsk" boot --name MYBOOT >/dev/null 2>&1
    assert_eq "$?" "0" "boot --name on valid IPLPRO must succeed" || return 1

    local info
    info=$("$MZDSK_FSMZ" "$dsk" boot 2>/dev/null)
    assert_contains "$info" "MYBOOT" "new name visible in boot info" || return 1
}


## Pozitivní test: kombinace fstrt+fexec+name v jedné call. Ftype ponecháváme
## 0x03, aby IPLPRO magic zůstal platný a bylo možné použít `boot` info k
## ověření. Samostatný --ftype je ověřen v test_fsmz_boot_set_ftype_updates_field.
test_fsmz_boot_set_combined_updates_all() {
    local dsk="$TEST_TMPDIR/fsmz_e1_comb.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local boot_mzf="$TEST_TMPDIR/e1_boot_c.mzf"
    create_boot_mzf "$boot_mzf" 256
    "$MZDSK_FSMZ" "$dsk" boot put "$boot_mzf" 2>/dev/null || return 1

    "$MZDSK_FSMZ" "$dsk" boot --fstrt 0x3000 --fexec 0x4000 --ftype 03 --name COMBO \
        >/dev/null 2>&1
    assert_eq "$?" "0" "boot combined set must succeed" || return 1

    local start exec_addr t info
    start=$("$MZDSK_FSMZ" -o json "$dsk" boot 2>/dev/null \
        | grep -oE '"start":\s*[0-9]+' | grep -oE '[0-9]+')
    exec_addr=$("$MZDSK_FSMZ" -o json "$dsk" boot 2>/dev/null \
        | grep -oE '"exec":\s*[0-9]+' | grep -oE '[0-9]+')
    t=$("$MZDSK_FSMZ" -o json "$dsk" boot 2>/dev/null \
        | grep -oE '"type":\s*[0-9]+' | grep -oE '[0-9]+')
    info=$("$MZDSK_FSMZ" "$dsk" boot 2>/dev/null)

    assert_eq "$start" "12288" "combined: fstrt 0x3000" || return 1
    assert_eq "$exec_addr" "16384" "combined: fexec 0x4000" || return 1
    assert_eq "$t" "3" "combined: ftype 0x03 preserved" || return 1
    assert_contains "$info" "COMBO" "combined: name present" || return 1
}


## Boot clear odstraní IPLPRO - následný set musí selhat stejně jako na
## čerstvém --format-basic disku (E1 regrese).
test_fsmz_boot_set_rejects_after_clear() {
    local dsk="$TEST_TMPDIR/fsmz_e1_clear.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local boot_mzf="$TEST_TMPDIR/e1_boot_cl.mzf"
    create_boot_mzf "$boot_mzf" 256
    "$MZDSK_FSMZ" "$dsk" boot put "$boot_mzf" 2>/dev/null || return 1
    "$MZDSK_FSMZ" "$dsk" boot clear >/dev/null 2>&1 || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_FSMZ" "$dsk" boot --fstrt 0x9999 >/dev/null 2>&1
    assert_neq "$?" "0" "boot set after clear must rc!=0" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" \
        "disk must be unchanged after set attempt post-clear" || return 1
}


# === BUG E2: duplicitní "Error: Unknown error" po specifické chybě ===

## Helper: spočítá řádky začínající "Error:" ve spojeném stdout+stderr
## konkrétního fsmz volání. Pro E2 testy: musí být právě 1 (ne 2).
fsmz_error_line_count() {
    "$@" 2>&1 | grep -c "^Error:"
}


## BUG E2-A: put s neexistujícím MZF souborem vypisoval jak specifickou
## hlášku ("cannot open MZF file ..."), tak generický fallback z main loop
## ("Error: Unknown error"). Po fixu handler vrací MZDSK_RES_FORMAT_ERROR
## sentinel, takže main loop generický fallback přeskočí.
test_fsmz_put_missing_file_single_error() {
    local dsk="$TEST_TMPDIR/fsmz_e2_putmiss.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local err_output rc count
    err_output=$("$MZDSK_FSMZ" "$dsk" put /nonexistent/e2a.mzf 2>&1)
    "$MZDSK_FSMZ" "$dsk" put /nonexistent/e2a.mzf >/dev/null 2>&1
    rc=$?

    assert_eq "$rc" "1" "put missing file must rc=1" || return 1
    assert_contains "$err_output" "cannot open MZF file" \
        "specific error message present" || return 1
    assert_not_contains "$err_output" "Error: Unknown error" \
        "no generic 'Unknown error' fallback" || return 1

    count=$(echo "$err_output" | grep -c "^Error:")
    assert_eq "$count" "1" "exactly one 'Error:' line" || return 1
}


## BUG E2-B: boot get na disku bez IPLPRO vypisovalo "Not found valid IPLPRO
## header!" + generické "Error: Unknown error". Po fixu jen specifickou hlášku.
test_fsmz_boot_get_no_iplpro_single_error() {
    local dsk="$TEST_TMPDIR/fsmz_e2_bgetni.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local err_output rc count
    err_output=$("$MZDSK_FSMZ" "$dsk" boot get "$TEST_TMPDIR/e2b_out.mzf" 2>&1)
    "$MZDSK_FSMZ" "$dsk" boot get "$TEST_TMPDIR/e2b_out.mzf" >/dev/null 2>&1
    rc=$?

    assert_eq "$rc" "1" "boot get without IPLPRO must rc=1" || return 1
    assert_contains "$err_output" "Not found valid IPLPRO" \
        "specific IPLPRO error present" || return 1
    assert_not_contains "$err_output" "Error: Unknown error" \
        "no generic 'Unknown error' fallback" || return 1

    count=$(echo "$err_output" | grep -c "^Error:")
    assert_eq "$count" "1" "exactly one 'Error:' line" || return 1
}


## BUG E2-C: dir/put/era atd. na non-FSMZ disku (CP/M, MRS) vypisovalo
## "This disk is not in full FSMZ format" + generické fallback. Po fixu
## jen specifickou hlášku. Testujeme několik command paths.
test_fsmz_cross_fs_dir_single_error() {
    local cpm="$TEST_TMPDIR/fsmz_e2_cfsdir.dsk"
    "$MZDSK_CREATE" "$cpm" --format-cpm 80 2>/dev/null || return 1

    local err_output count
    err_output=$("$MZDSK_FSMZ" "$cpm" dir 2>&1)
    assert_contains "$err_output" "not in full FSMZ format" \
        "specific FS mismatch error present" || return 1
    assert_not_contains "$err_output" "Error: Unknown error" \
        "no generic 'Unknown error' fallback (dir)" || return 1

    count=$(echo "$err_output" | grep -c "^Error:")
    assert_eq "$count" "1" "exactly one 'Error:' line (dir)" || return 1
}


test_fsmz_cross_fs_put_single_error() {
    local cpm="$TEST_TMPDIR/fsmz_e2_cfsput.dsk"
    "$MZDSK_CREATE" "$cpm" --format-cpm 80 2>/dev/null || return 1

    create_test_mzf "$TEST_TMPDIR/e2c_put.mzf" "E2CP" 256

    local err_output count
    err_output=$("$MZDSK_FSMZ" "$cpm" put "$TEST_TMPDIR/e2c_put.mzf" 2>&1)
    assert_contains "$err_output" "not in full FSMZ format" \
        "specific FS mismatch error present" || return 1
    assert_not_contains "$err_output" "Error: Unknown error" \
        "no generic 'Unknown error' fallback (put)" || return 1

    count=$(echo "$err_output" | grep -c "^Error:")
    assert_eq "$count" "1" "exactly one 'Error:' line (put)" || return 1
}


## BUG E2-D: format na non-FSMZ disku vypisoval jen generické
## "Error: Unknown error" z main loop, zatímco specifická hláška
## "Error: This disk is not in full FSMZ format" se ztrácela v
## stdout/stderr interleavingu. Po fixu main loop generický fallback
## zcela vynechá, takže zůstává jen specifická hláška.
test_fsmz_format_cross_fs_single_error() {
    local cpm="$TEST_TMPDIR/fsmz_e2_cfsfmt.dsk"
    "$MZDSK_CREATE" "$cpm" --format-cpm 80 2>/dev/null || return 1

    local err_output count
    err_output=$("$MZDSK_FSMZ" --yes "$cpm" format 2>&1)
    assert_contains "$err_output" "not in full FSMZ format" \
        "specific FS mismatch error present" || return 1
    assert_not_contains "$err_output" "Error: Unknown error" \
        "no generic 'Unknown error' fallback" || return 1

    count=$(echo "$err_output" | grep -c "^Error:")
    assert_eq "$count" "1" "exactly one 'Error:' line (format)" || return 1
}


## mzdsk-fsmz bootstrap clear
test_fsmz_bootstrap_clear() {
    local dsk="$TEST_TMPDIR/fsmz_boot_clr.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    # nainstalovat bootstrap
    local boot_mzf="$TEST_TMPDIR/boot_clr.mzf"
    dd if=/dev/zero bs=128 count=1 of="$boot_mzf" 2>/dev/null
    printf '\x03' | dd of="$boot_mzf" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf 'IPLPRO\x0d' | dd of="$boot_mzf" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf '\x00\x01' | dd of="$boot_mzf" bs=1 seek=18 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$boot_mzf" bs=1 seek=20 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$boot_mzf" bs=1 seek=22 conv=notrunc 2>/dev/null
    dd if=/dev/urandom bs=256 count=1 2>/dev/null >> "$boot_mzf"

    "$MZDSK_FSMZ" "$dsk" boot put "$boot_mzf" 2>/dev/null || return 1

    # clear
    "$MZDSK_FSMZ" "$dsk" boot clear 2>/dev/null
    assert_eq "$?" "0" "boot clear should succeed" || return 1
}


# --- Spuštění ---

## BUG 7: put s neplatným MZF musí selhat a disk nechat nezměněn
test_fsmz_put_rejects_invalid_mzf() {
    local dsk="$TEST_TMPDIR/fsmz_bad_mzf.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # 256 B náhodného obsahu - validace detekuje size mismatch (fsize vs file)
    local bad="$TEST_TMPDIR/bad.mzf"
    dd if=/dev/urandom of="$bad" bs=256 count=1 2>/dev/null

    "$MZDSK_FSMZ" "$dsk" put "$bad" >/dev/null 2>&1
    assert_neq "$?" "0" "put of random 256B data must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk must be unchanged" || return 1
}


## BUG 7: put s menším MZF než hlavička (< 128 B) musí selhat
test_fsmz_put_rejects_too_small() {
    local dsk="$TEST_TMPDIR/fsmz_tiny.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local tiny="$TEST_TMPDIR/tiny.mzf"
    dd if=/dev/urandom of="$tiny" bs=5 count=1 2>/dev/null

    "$MZDSK_FSMZ" "$dsk" put "$tiny" >/dev/null 2>&1
    assert_neq "$?" "0" "put of tiny 5B file must fail" || return 1
}


## BUG 9: get --all s duplicitními jmény - rename (default) extrahuje všechny.
## Disk s kolizemi jmen pochází z fixture_fsmz_with_dups (synth: 6 souborů / 2 páry,
## refdata: 52 souborů / 2 páry) - test se přizpůsobí přes FIXTURE_FSMZ_* proměnné.
test_fsmz_get_all_rename() {
    fixture_fsmz_with_dups || return 1

    local dsk="$TEST_TMPDIR/fsmz_dup_rename.dsk"
    cp "$FIXTURE_FSMZ_DSK" "$dsk"

    local outdir="$TEST_TMPDIR/extract_rename"
    "$MZDSK_FSMZ" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "get --all (rename) should succeed" || return 1

    local count=$(ls "$outdir" | wc -l)
    assert_eq "$count" "$FIXTURE_FSMZ_TOTAL" \
        "all $FIXTURE_FSMZ_TOTAL files extracted in rename mode" || return 1

    local renamed=$(ls "$outdir" | grep -c '~2' 2>/dev/null)
    [ -z "$renamed" ] && renamed=0
    assert_eq "$renamed" "$FIXTURE_FSMZ_DUP_PAIRS" \
        "$FIXTURE_FSMZ_DUP_PAIRS files renamed with ~2 suffix" || return 1
}


## BUG 9: get --all --on-duplicate skip - duplicity se přeskočí s varováním.
test_fsmz_get_all_skip() {
    fixture_fsmz_with_dups || return 1

    local dsk="$TEST_TMPDIR/fsmz_dup_skip.dsk"
    cp "$FIXTURE_FSMZ_DSK" "$dsk"

    local outdir="$TEST_TMPDIR/extract_skip"
    local err_output
    err_output=$("$MZDSK_FSMZ" --overwrite "$dsk" get --all "$outdir" --on-duplicate skip 2>&1)
    local rc=$?
    assert_eq "$rc" "0" "get --all --on-duplicate skip should succeed" || return 1

    local expected_kept=$((FIXTURE_FSMZ_TOTAL - FIXTURE_FSMZ_DUP_PAIRS))
    local count=$(ls "$outdir" | wc -l)
    assert_eq "$count" "$expected_kept" \
        "$expected_kept files kept under skip mode ($FIXTURE_FSMZ_TOTAL - $FIXTURE_FSMZ_DUP_PAIRS)" || return 1
    assert_contains "$err_output" "Skipping" "warning about skip printed" || return 1
}


## BUG 17: --output/-o funguje i za subpříkazem (včetně --charset u fsmz)
test_fsmz_output_after_subcmd() {
    fixture_fsmz_with_dups || return 1
    local dsk="$FIXTURE_FSMZ_DSK"

    # -o za subcmd
    local out1
    out1=$("$MZDSK_FSMZ" "$dsk" dir -o json 2>/dev/null)
    assert_contains "$out1" '"filesystem": "fsmz"' "-o za subcmd: JSON výstup" || return 1

    # --output= za subcmd
    local out2
    out2=$("$MZDSK_FSMZ" "$dsk" dir --output=json 2>/dev/null)
    assert_contains "$out2" '"filesystem": "fsmz"' "--output=VAL za subcmd: JSON" || return 1

    # --output VAL za subcmd
    local out3
    out3=$("$MZDSK_FSMZ" "$dsk" dir --output json 2>/dev/null)
    assert_contains "$out3" '"filesystem": "fsmz"' "--output VAL za subcmd: JSON" || return 1

    # Kombinace subcmd-specific volby + globální za ní (nesmí se rozbít)
    local out4
    out4=$("$MZDSK_FSMZ" "$dsk" dir --type 0x01 -o json 2>/dev/null)
    assert_contains "$out4" '"filesystem": "fsmz"' "--type + -o za subcmd" || return 1

    # Chybná hodnota u přesunuté volby selže stderr + exit 1
    local err rc
    err=$("$MZDSK_FSMZ" "$dsk" dir -o xml 2>&1 >/dev/null)
    "$MZDSK_FSMZ" "$dsk" dir -o xml >/dev/null 2>&1
    rc=$?
    assert_neq "$rc" "0" "neplatný -o za subcmd selže" || return 1
    assert_contains "$err" "Unknown output format" "chybová hláška" || return 1
}


## BUG 11: repair musí obnovit "Total disk size" v DINFO i po poškození bloku
test_fsmz_repair_dinfo_total_size() {
    local dsk="$TEST_TMPDIR/fsmz_rep11.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    # Referenční hodnota "Total disk size"
    local good_line
    good_line=$("$MZDSK_FSMZ" "$dsk" dir 2>/dev/null | grep "Total disk size")
    [ -z "$good_line" ] && return 1

    # Poškodit DINFO - FSMZ blok 15 leží na Track 1, Sector 16
    head -c 256 /dev/urandom > "$TEST_TMPDIR/poison.bin"
    "$MZDSK_RAW" "$dsk" put "$TEST_TMPDIR/poison.bin" \
        --track 1 --sector 16 --sectors 1 >/dev/null 2>&1 || return 1

    # Repair
    "$MZDSK_FSMZ" "$dsk" repair >/dev/null 2>&1
    assert_eq "$?" "0" "repair should succeed" || return 1

    # Po repair musí být Total disk size shodné s původní hodnotou
    local after_line
    after_line=$("$MZDSK_FSMZ" "$dsk" dir 2>/dev/null | grep "Total disk size")
    assert_eq "$after_line" "$good_line" "Total disk size restored" || return 1
}


## BUG 10: boot na ne-FSMZ disku musí fungovat a vypsat informativní hlášku
test_fsmz_boot_on_non_fsmz() {
    local cpm="$TEST_TMPDIR/fsmz_boot_cpm.dsk"
    "$MZDSK_CREATE" "$cpm" --format-cpm 80 2>/dev/null || return 1

    local out
    out=$("$MZDSK_FSMZ" "$cpm" boot 2>&1)
    local rc=$?
    assert_eq "$rc" "0" "boot on CP/M must succeed (boot track exists)" || return 1

    # Výstup obsahuje info hlášku, ne "Error"
    assert_contains "$out" "not available on this DSK" "info message present" || return 1

    # Nesmí tam být "Error:" v kontextu disc info
    if echo "$out" | grep -q "Error: This disk is not in full FSMZ format"; then
        echo -e "    ${_RED}FAIL${_NC}: old 'Error' header still leaks"
        return 1
    fi
}


## BUG 9: neplatný --on-duplicate mode
test_fsmz_get_all_invalid_mode() {
    local dsk="$TEST_TMPDIR/fsmz_dup_bad.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    "$MZDSK_FSMZ" --overwrite "$dsk" get --all "$TEST_TMPDIR/out" --on-duplicate garbage >/dev/null 2>&1
    assert_neq "$?" "0" "invalid --on-duplicate mode must fail" || return 1
}


## BUG 8: chybové hlášky nesmí obsahovat debug pattern "funkce():číslo - "
test_fsmz_no_debug_messages() {
    local dsk="$TEST_TMPDIR/fsmz_nodebug.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    # Několik cest, které dříve vypisovaly debug hlášky
    local tiny="$TEST_TMPDIR/tiny.mzf"
    dd if=/dev/urandom of="$tiny" bs=5 count=1 2>/dev/null
    local bad="$TEST_TMPDIR/bad.mzf"
    dd if=/dev/urandom of="$bad" bs=256 count=1 2>/dev/null

    # put na neplatné MZF
    local out1=$("$MZDSK_FSMZ" "$dsk" put "$tiny" 2>&1)
    local out2=$("$MZDSK_FSMZ" "$dsk" put "$bad" 2>&1)
    # boot put na neplatné MZF (jiná cesta)
    local out3=$("$MZDSK_FSMZ" "$dsk" boot put "$tiny" 2>&1)
    local out4=$("$MZDSK_FSMZ" "$dsk" boot put "$bad" 2>&1)
    # put na neexistující soubor
    local out5=$("$MZDSK_FSMZ" "$dsk" put /nonexistent/file.mzf 2>&1)

    local combined="$out1$out2$out3$out4$out5"
    # Pattern "slovo(): " nebo "():" vedle číslic značí debug hlášku
    if echo "$combined" | grep -Eq '[a-z_]+\(\):[0-9]+ -'; then
        echo -e "    ${_RED}FAIL${_NC}: debug-style message leaked"
        echo "      pattern 'func():line -' found in output"
        return 1
    fi
}


## BUG 7: put prázdného souboru (0 B) musí selhat s jasnou hláškou
test_fsmz_put_rejects_empty() {
    local dsk="$TEST_TMPDIR/fsmz_empty.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local empty="$TEST_TMPDIR/empty.mzf"
    : > "$empty"

    local err_output=$("$MZDSK_FSMZ" "$dsk" put "$empty" 2>&1)
    "$MZDSK_FSMZ" "$dsk" put "$empty" >/dev/null 2>&1
    assert_neq "$?" "0" "put of empty file must fail" || return 1
    assert_contains "$err_output" "too small" "error mentions size" || return 1
}


## Atomicita era - smazání neexistujícího souboru nesmí změnit DSK (memory driver)
test_fsmz_era_atomicity() {
    local dsk="$TEST_TMPDIR/fsmz_atom_era.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_FSMZ" "$dsk" era NONEXISTENT >/dev/null 2>&1
    assert_neq "$?" "0" "era nonexistent must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after era failure" || return 1
}

## Atomicita put - put neexistujícího zdrojového souboru nesmí změnit DSK
test_fsmz_put_atomicity() {
    local dsk="$TEST_TMPDIR/fsmz_atom_put.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_FSMZ" "$dsk" put /does/not/exist.mzf >/dev/null 2>&1
    assert_neq "$?" "0" "put nonexistent must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after put failure" || return 1
}


run_test test_fsmz_repair
run_test test_fsmz_bootstrap_put_get
run_test test_fsmz_bootstrap_clear
run_test test_fsmz_era_respects_lock
run_test test_fsmz_ren_respects_lock
run_test test_fsmz_put_rejects_invalid_mzf
run_test test_fsmz_put_rejects_too_small
run_test test_fsmz_put_rejects_empty
run_test test_fsmz_no_debug_messages
run_test test_fsmz_get_all_rename
run_test test_fsmz_get_all_skip
run_test test_fsmz_get_all_invalid_mode
run_test test_fsmz_boot_on_non_fsmz
run_test test_fsmz_repair_dinfo_total_size
run_test test_fsmz_output_after_subcmd
run_test test_fsmz_era_atomicity
run_test test_fsmz_put_atomicity
run_test test_fsmz_put_does_not_inherit_lock_flag
run_test test_fsmz_put_fresh_slot_not_locked
run_test test_fsmz_get_no_duplicate_error_on_existing_output
run_test test_fsmz_get_block_no_duplicate_error_on_existing_output
run_test test_fsmz_get_overwrite_flag_allows_overwrite
run_test test_fsmz_boot_set_rejects_without_iplpro
run_test test_fsmz_boot_set_fstrt_updates_field
run_test test_fsmz_boot_set_fexec_updates_field
run_test test_fsmz_boot_set_ftype_updates_field
run_test test_fsmz_boot_set_name_updates_field
run_test test_fsmz_boot_set_combined_updates_all
run_test test_fsmz_boot_set_rejects_after_clear
run_test test_fsmz_put_missing_file_single_error
run_test test_fsmz_boot_get_no_iplpro_single_error
run_test test_fsmz_cross_fs_dir_single_error
run_test test_fsmz_cross_fs_put_single_error
run_test test_fsmz_format_cross_fs_single_error

test_summary
