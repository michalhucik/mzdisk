#!/bin/bash
# CLI testy: mzdsk-cpm - odmítnutí non-CP/M disků, --force override.

source "$(dirname "$0")/helpers.sh"

echo "=== $0 ==="


## mzdsk-cpm na FSMZ disku bez --force musí selhat (BUG A1)
test_cpm_rejects_fsmz_disk() {
    local dsk="$TEST_TMPDIR/cpm_rej_fsmz.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    # Snapshot před pokusem o operace
    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    # dir: read-only operace - taky musí selhat (výstup by byl nesmyslný)
    "$MZDSK_CPM" "$dsk" dir >/dev/null 2>&1
    assert_neq "$?" "0" "cpm dir on FSMZ disk must fail" || return 1

    # format: destruktivní operace - nesmí disk poškodit
    "$MZDSK_CPM" -y "$dsk" format >/dev/null 2>&1
    assert_neq "$?" "0" "cpm format on FSMZ disk must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "FSMZ disk must be unchanged" || return 1
}


## mzdsk-cpm na MRS disku bez --force musí selhat
test_cpm_rejects_mrs_disk() {
    local dsk="$TEST_TMPDIR/cpm_rej_mrs.dsk"
    "$MZDSK_CREATE" "$dsk" --preset mrs --overwrite 2>/dev/null || return 1
    "$MZDSK_MRS" -y "$dsk" init 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')

    "$MZDSK_CPM" "$dsk" dir >/dev/null 2>&1
    assert_neq "$?" "0" "cpm dir on MRS disk must fail" || return 1

    "$MZDSK_CPM" -y "$dsk" format >/dev/null 2>&1
    assert_neq "$?" "0" "cpm format on MRS disk must fail" || return 1

    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "MRS disk must be unchanged" || return 1
}


## mzdsk-cpm --force obchází kontrolu FS
test_cpm_force_bypasses_detection() {
    local dsk="$TEST_TMPDIR/cpm_force.dsk"
    "$MZDSK_CREATE" "$dsk" --format-basic 80 2>/dev/null || return 1

    # bez force selže
    "$MZDSK_CPM" "$dsk" dir >/dev/null 2>&1
    assert_neq "$?" "0" "without --force must fail" || return 1

    # s force prochází
    "$MZDSK_CPM" --force "$dsk" dir >/dev/null 2>&1
    assert_eq "$?" "0" "with --force must succeed" || return 1
}


## mzdsk-cpm na skutečném CP/M disku funguje bez --force
test_cpm_accepts_cpm_disk() {
    local dsk="$TEST_TMPDIR/cpm_ok.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    "$MZDSK_CPM" "$dsk" dir >/dev/null 2>&1
    assert_eq "$?" "0" "cpm dir on CP/M disk must succeed" || return 1
}


## Pomocná funkce: vytvoří CP/M disk s jedním souborem pro testy get --mzf
## Soubor je vždy pojmenován "TEST.COM"; volající ho použije přímo.
setup_cpm_disk_with_file() {
    local dsk="$1"
    local payload="$TEST_TMPDIR/payload.bin"

    "$MZDSK_CREATE" "$dsk" --format-cpm 80 >/dev/null 2>&1 || return 1
    dd if=/dev/urandom of="$payload" bs=512 count=1 2>/dev/null
    "$MZDSK_CPM" "$dsk" put "$payload" "TEST.COM" >/dev/null 2>&1 || return 1
    return 0
}


## Pomocná funkce: vytvoří CP/M disk s více soubory v user 0 pro get --all testy.
## Volání: create_cpm_with_files <dsk> <name1> [name2] [name3]
## Každý soubor je 128B náhodných dat.
create_cpm_with_files() {
    local dsk="$1"
    shift
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 --overwrite >/dev/null 2>&1 || return 1
    local i=0
    for fname in "$@"; do
        local payload="$TEST_TMPDIR/cpm_ga_p$i.bin"
        dd if=/dev/urandom of="$payload" bs=128 count=1 2>/dev/null
        "$MZDSK_CPM" "$dsk" put "$payload" "$fname" >/dev/null 2>&1 || return 1
        i=$((i+1))
    done
    return 0
}


## --exec-addr zapisuje hodnotu do pole `fexec`, `fstrt` zůstane na defaultu 0x0100
test_cpm_get_mzf_exec_addr() {
    local dsk="$TEST_TMPDIR/cpm_exec_addr.dsk"
    setup_cpm_disk_with_file "$dsk" || return 1

    local out="$TEST_TMPDIR/exec_addr.mzf"

    # --exec-addr 0x4000: fexec = 0x4000, fstrt = 0x0100 (default strt)
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf TEST.COM "$out" --exec-addr 0x4000 >/dev/null 2>&1
    assert_eq "$?" "0" "get --mzf --exec-addr should succeed" || return 1

    # fstrt (offset 0x14, LE) = 0x0100 (default load address)
    local fstrt_hex=$(xxd -s 0x14 -l 2 -p "$out")
    assert_eq "$fstrt_hex" "0001" "fstrt must be 0x0100 (default load address)" || return 1

    # fexec (offset 0x16, LE) = 0x4000
    local fexec_hex=$(xxd -s 0x16 -l 2 -p "$out")
    assert_eq "$fexec_hex" "0040" "fexec must be 0x4000 (exec address)" || return 1
}


## Výchozí exec-addr je 0x0100 (standardní CP/M TPA)
test_cpm_get_mzf_default_exec_addr() {
    local dsk="$TEST_TMPDIR/cpm_exec_def.dsk"
    setup_cpm_disk_with_file "$dsk" || return 1

    local out="$TEST_TMPDIR/exec_def.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf TEST.COM "$out" >/dev/null 2>&1
    assert_eq "$?" "0" "get --mzf without --exec-addr should succeed" || return 1

    # fexec výchozí 0x0100
    local fexec_hex=$(xxd -s 0x16 -l 2 -p "$out")
    assert_eq "$fexec_hex" "0001" "default fexec must be 0x0100" || return 1
}


## Alias --addr stále funguje pro zpětnou kompatibilitu
test_cpm_get_mzf_addr_alias() {
    local dsk="$TEST_TMPDIR/cpm_addr_alias.dsk"
    setup_cpm_disk_with_file "$dsk" || return 1

    local out="$TEST_TMPDIR/addr_alias.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf TEST.COM "$out" --addr 0x8000 >/dev/null 2>&1
    assert_eq "$?" "0" "get --mzf --addr (alias) should succeed" || return 1

    local fexec_hex=$(xxd -s 0x16 -l 2 -p "$out")
    assert_eq "$fexec_hex" "0080" "--addr alias must write to fexec" || return 1
}


## get --mzf --ftype HH nastaví ftype v MZF hlavičce
test_cpm_get_mzf_ftype() {
    local dsk="$TEST_TMPDIR/cpm_ftype.dsk"
    setup_cpm_disk_with_file "$dsk" || return 1

    local out="$TEST_TMPDIR/ftype.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf TEST.COM "$out" --ftype 0x01 >/dev/null 2>&1
    assert_eq "$?" "0" "get --mzf --ftype should succeed" || return 1

    # ftype (offset 0x00)
    local ftype_hex=$(xxd -s 0x00 -l 1 -p "$out")
    assert_eq "$ftype_hex" "01" "ftype must be 0x01" || return 1
}


## --strt-addr zapíše adresu do fstrt - platí pro libovolný ftype včetně 0x22
test_cpm_get_mzf_strt_addr() {
    local dsk="$TEST_TMPDIR/cpm_strt.dsk"
    setup_cpm_disk_with_file "$dsk" || return 1

    # ftype=0x05 + strt=0x1200
    local out_a="$TEST_TMPDIR/strt_a.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf TEST.COM "$out_a" --ftype 0x05 --strt-addr 0x1200 >/dev/null 2>&1
    assert_eq "$?" "0" "ftype=0x05 --strt-addr should succeed" || return 1
    local fstrt_a=$(xxd -s 0x14 -l 2 -p "$out_a")
    assert_eq "$fstrt_a" "0012" "ftype=0x05: fstrt=0x1200" || return 1

    # ftype=0x22 (default) + strt=0x0200 - také se musí uplatnit
    local out_b="$TEST_TMPDIR/strt_b.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf TEST.COM "$out_b" --strt-addr 0x0200 >/dev/null 2>&1
    assert_eq "$?" "0" "ftype=0x22 --strt-addr should succeed" || return 1
    local fstrt_b=$(xxd -s 0x14 -l 2 -p "$out_b")
    assert_eq "$fstrt_b" "0002" "ftype=0x22: fstrt=0x0200 (no ignore)" || return 1
}


## get --mzf --no-attrs vynechá kódování R/O, SYS, ARC do bitů 7 fname
test_cpm_get_mzf_no_attrs() {
    local dsk="$TEST_TMPDIR/cpm_noattrs.dsk"
    setup_cpm_disk_with_file "$dsk" || return 1

    # Nastav SYS na TEST.COM (bit 7 druhého znaku přípony)
    "$MZDSK_CPM" "$dsk" attr TEST.COM +S >/dev/null 2>&1 || return 1

    # Kontrola: s default (encode_attrs) je 'O' na offsetu 0x0B = 0xCF (O | 0x80)
    local out_def="$TEST_TMPDIR/attrs_default.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf TEST.COM "$out_def" >/dev/null 2>&1
    assert_eq "$?" "0" "get --mzf (default) should succeed" || return 1
    local byte_def=$(xxd -s 0x0B -l 1 -p "$out_def")
    assert_eq "$byte_def" "cf" "default: fname[10] must be 0xCF ('O' | SYS bit)" || return 1

    # S --no-attrs je 'O' na offsetu 0x0B = 0x4F (bez bit 7)
    local out_no="$TEST_TMPDIR/attrs_none.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf TEST.COM "$out_no" --no-attrs >/dev/null 2>&1
    assert_eq "$?" "0" "get --mzf --no-attrs should succeed" || return 1
    local byte_no=$(xxd -s 0x0B -l 1 -p "$out_no")
    assert_eq "$byte_no" "4f" "no-attrs: fname[10] must be 0x4F ('O' without bit 7)" || return 1
}


## BUG 12: put se zkrácením jména vypíše varování
test_cpm_put_truncates_with_warning() {
    local dsk="$TEST_TMPDIR/cpm_trunc.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=512 count=1 2>/dev/null

    local out
    out=$("$MZDSK_CPM" "$dsk" put "$data" TOOLONGNAME.EXT 2>&1)
    assert_eq "$?" "0" "put should succeed" || return 1
    assert_contains "$out" "Warning" "warning about truncation printed" || return 1
    assert_contains "$out" "truncated to" "warning mentions truncation target" || return 1
    assert_contains "$out" "TOOLONGN.EXT" "warning shows final name" || return 1
    # 'Written' hláška musí ukázat skutečné on-disk jméno, ne původní vstup
    assert_contains "$out" "'TOOLONGN.EXT' (512 bytes)" "Written shows actual stored name" || return 1

    # Ověření v dir
    local dir
    dir=$("$MZDSK_CPM" "$dsk" dir 2>/dev/null)
    assert_contains "$dir" "TOOLONGN" "dir contains truncated name" || return 1
}


## BUG 17: --output/-o funguje i za subpříkazem
test_cpm_output_after_subcmd() {
    fixture_cpm_sample || return 1
    local dsk="$FIXTURE_CPM_DSK"

    local out1
    out1=$("$MZDSK_CPM" "$dsk" dir -o json 2>/dev/null)
    assert_contains "$out1" '"filesystem": "cpm"' "-o za subcmd" || return 1

    local out2
    out2=$("$MZDSK_CPM" "$dsk" dir --output=csv 2>/dev/null)
    assert_contains "$out2" "user,name,ext" "--output=VAL za subcmd (CSV header)" || return 1

    local out3
    out3=$("$MZDSK_CPM" "$dsk" dir --output csv 2>/dev/null)
    assert_contains "$out3" "user,name,ext" "--output VAL za subcmd" || return 1
}


## BUG 12: put s jménem, které po zkrácení způsobí duplicitu, selže
test_cpm_put_truncation_duplicate_fails() {
    local dsk="$TEST_TMPDIR/cpm_dupt.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=512 count=1 2>/dev/null

    "$MZDSK_CPM" "$dsk" put "$data" TOOLONGNAMEA.EXT >/dev/null 2>&1 || return 1

    local out
    out=$("$MZDSK_CPM" "$dsk" put "$data" TOOLONGNAMEB.EXT 2>&1)
    assert_neq "$?" "0" "duplicate after truncation must fail" || return 1
    assert_contains "$out" "Warning" "warning still printed before failure" || return 1
}


## Atomicita era - smazání neexistujícího souboru nesmí změnit DSK (memory driver)
test_cpm_era_atomicity() {
    local dsk="$TEST_TMPDIR/cpm_atom_era.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_CPM" "$dsk" era NONE.EXT >/dev/null 2>&1
    assert_neq "$?" "0" "era nonexistent must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after era failure" || return 1
}

## Atomicita put - put neexistujícího zdrojového souboru nesmí změnit DSK
test_cpm_put_atomicity() {
    local dsk="$TEST_TMPDIR/cpm_atom_put.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_CPM" "$dsk" put /does/not/exist >/dev/null 2>&1
    assert_neq "$?" "0" "put nonexistent must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after put failure" || return 1
}


## --name strict: validní jméno bez varování projde
test_cpm_put_strict_name_ok() {
    local dsk="$TEST_TMPDIR/cpm_strict_ok.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null

    local out
    out=$("$MZDSK_CPM" "$dsk" put "$data" --name HELLO.TXT 2>&1)
    assert_eq "$?" "0" "strict --name HELLO.TXT should succeed" || return 1
    # Žádné "Warning" (striktní mód nesmí tiše zkracovat)
    if echo "$out" | grep -q "Warning"; then
        echo "  unexpected warning in strict mode: $out"
        return 1
    fi
    local dir
    dir=$("$MZDSK_CPM" "$dsk" dir 2>/dev/null)
    assert_contains "$dir" "HELLO" "dir contains stored name" || return 1
}


## --name strict: dlouhé jméno musí selhat bez truncation
test_cpm_put_strict_name_too_long_fails() {
    local dsk="$TEST_TMPDIR/cpm_strict_long.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    local out
    out=$("$MZDSK_CPM" "$dsk" put "$data" --name TOOLONGNAME.EXT 2>&1)
    assert_neq "$?" "0" "strict --name too long must fail" || return 1
    assert_contains "$out" "exceeds 8.3 limit" "error mentions 8.3 limit" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after strict fail" || return 1
}


## --name strict: zakázaný znak musí selhat
test_cpm_put_strict_name_forbidden_char_fails() {
    local dsk="$TEST_TMPDIR/cpm_strict_char.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null

    local out
    out=$("$MZDSK_CPM" "$dsk" put "$data" --name 'A*B.TXT' 2>&1)
    assert_neq "$?" "0" "strict --name with '*' must fail" || return 1
    assert_contains "$out" "forbidden character" "error mentions forbidden character" || return 1
}


## --name strict: konflikt s pozičním argumentem
test_cpm_put_strict_name_conflict_with_positional() {
    local dsk="$TEST_TMPDIR/cpm_strict_conf.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null

    local out
    out=$("$MZDSK_CPM" "$dsk" put "$data" HELLO.TXT --name OTHER.TXT 2>&1)
    assert_neq "$?" "0" "positional + --name must fail" || return 1
    assert_contains "$out" "conflicts with positional" "error about conflict" || return 1
}


## --name strict u put --mzf: validní override projde
test_cpm_put_mzf_strict_name_ok() {
    local dsk="$TEST_TMPDIR/cpm_mzf_strict.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null

    # Uložit soubor, vyextrahovat MZF, znovu naformátovat disk
    "$MZDSK_CPM" "$dsk" put "$data" --name SRC.BIN >/dev/null 2>&1 || return 1
    local mzf="$TEST_TMPDIR/src.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf SRC.BIN "$mzf" >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 --overwrite 2>/dev/null || return 1

    # put --mzf s --name OTHER.DAT -> na disku musí být OTHER, ne SRC
    "$MZDSK_CPM" "$dsk" put --mzf "$mzf" --name OTHER.DAT >/dev/null 2>&1
    assert_eq "$?" "0" "strict put --mzf --name should succeed" || return 1
    local dir
    dir=$("$MZDSK_CPM" "$dsk" dir 2>/dev/null)
    assert_contains "$dir" "OTHER" "dir contains overridden name" || return 1
    if echo "$dir" | grep -q "SRC"; then
        echo "  MZF header name leaked into dir: $dir"
        return 1
    fi
}


## --name strict u put --mzf: dlouhé jméno selže
test_cpm_put_mzf_strict_name_fails() {
    local dsk="$TEST_TMPDIR/cpm_mzf_strict_fail.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null
    "$MZDSK_CPM" "$dsk" put "$data" --name SRC.BIN >/dev/null 2>&1 || return 1
    local mzf="$TEST_TMPDIR/src.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf SRC.BIN "$mzf" >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 --overwrite 2>/dev/null || return 1

    local out
    out=$("$MZDSK_CPM" "$dsk" put --mzf "$mzf" --name TOOLONGNAME.XYZ 2>&1)
    assert_neq "$?" "0" "strict --mzf --name too long must fail" || return 1
    assert_contains "$out" "exceeds 8.3 limit" "error mentions 8.3 limit" || return 1
}


## BUG B2 / KNOWN-CPM-001: put --mzf se Sharp EU velkými písmeny v fname
## musí zachovat detekci CP/M (dříve masking bit 7 produkoval non-printable
## bajty v directory a detekce vrátila "FSMZ boot track only").
test_cpm_put_mzf_preserves_detection_flappy() {
    local dsk="$TEST_TMPDIR/cpm_bug_b2_flappy.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd --sides 2 --overwrite >/dev/null 2>&1 || return 1

    # Sestav MZF s fname "FLAPPY " + AB 92 9D (Sharp EU 'v','e','r') + " 1.0A" + CR
    # a dopad fsize=4 aby byl minimální datový blok.
    local mzf="$TEST_TMPDIR/flappy_sim.mzf"
    # ftype=0x01 (OBJ), fname bajty, pak padding do 128B. fsize=4 na offsetu 18.
    printf '\x01FLAPPY \xAB\x92\x9D 1.0A\x0D' > "$mzf"
    # Dopaddovat na 18 B (ftype 1 + fname 16 = 17), pak fsize=4 LE, fstrt=0, fexec=0,
    # cmnt 104B = 0. Celková hlavička 128B, pak 4 B dat.
    # Stávající obsah: 1 + 16 = 17 B, chybí 1 B do offsetu 18:
    printf '\x00' >> "$mzf"            # padding na offset 18
    printf '\x04\x00\x00\x00\x00\x00' >> "$mzf"  # fsize=4, fstrt=0, fexec=0
    # cmnt 104 B:
    dd if=/dev/zero of="$mzf" bs=1 count=104 seek=24 conv=notrunc 2>/dev/null
    # Data (4 B):
    printf '\xDE\xAD\xBE\xEF' >> "$mzf"

    "$MZDSK_CPM" "$dsk" put --mzf "$mzf" >/dev/null 2>&1
    assert_eq "$?" "0" "put --mzf Sharp EU fname must succeed" || return 1

    # Detekce musí zůstat CP/M SD (před opravou: "FSMZ boot track only").
    local out
    out=$("$MZDSK_INFO" "$dsk" --map 2>&1)
    assert_contains "$out" "CP/M SD" "detection preserved after Sharp EU put" || return 1
    assert_not_contains "$out" "FSMZ boot track only" "detection not regressed to boot-only" || return 1
}


## BUG B2 / KNOWN-CPM-001: put --mzf s leading space + CR v fname.
## Odvozené jméno musí být sanitizováno - detekce CP/M zachována.
test_cpm_put_mzf_preserves_detection_leading_space() {
    local dsk="$TEST_TMPDIR/cpm_bug_b2_bomb.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd --sides 2 --overwrite >/dev/null 2>&1 || return 1

    # Sestav MZF s fname " F1200" + CR - dříve ext[0] = 0x0D (Group Separator)
    local mzf="$TEST_TMPDIR/bomber_sim.mzf"
    printf '\x01 F1200\x0D' > "$mzf"
    # Po těchto 8 bajtech máme offset 8, potřebujeme offset 18 pro fsize:
    dd if=/dev/zero of="$mzf" bs=1 count=10 seek=8 conv=notrunc 2>/dev/null
    printf '\x04\x00\x00\x00\x00\x00' >> "$mzf"
    dd if=/dev/zero of="$mzf" bs=1 count=104 seek=24 conv=notrunc 2>/dev/null
    printf '\xDE\xAD\xBE\xEF' >> "$mzf"

    "$MZDSK_CPM" "$dsk" put --mzf "$mzf" >/dev/null 2>&1
    assert_eq "$?" "0" "put --mzf with leading-space fname must succeed" || return 1

    local out
    out=$("$MZDSK_INFO" "$dsk" --map 2>&1)
    assert_contains "$out" "CP/M SD" "detection preserved after leading-space put" || return 1
    assert_not_contains "$out" "FSMZ boot track only" "detection not regressed to boot-only" || return 1
}


## --charset jp: neznámá hodnota musí selhat s informativní chybou
test_cpm_put_mzf_charset_invalid() {
    local dsk="$TEST_TMPDIR/cpm_charset_inv.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd --sides 2 --overwrite >/dev/null 2>&1 || return 1

    local mzf="$TEST_TMPDIR/dummy.mzf"
    printf '\x01TEST\x0D' > "$mzf"
    dd if=/dev/zero of="$mzf" bs=1 count=123 seek=5 conv=notrunc 2>/dev/null
    printf '\x00\x00\x00\x00\x00\x00\x00' >> "$mzf"

    local out
    out=$("$MZDSK_CPM" "$dsk" put --mzf "$mzf" --charset klingon 2>&1)
    assert_neq "$?" "0" "unknown charset must fail" || return 1
    assert_contains "$out" "Unknown --charset" "error mentions unknown charset" || return 1
}


## --force-charset: ftype==0x22 MZF se Sharp-kódovaným fname dekódovat
## přes --charset místo CPM-IC ASCII masky.
## Default chování masking bit 7 produkuje "_" na místě non-printable
## bajtů; --force-charset interpretuje fname přes Sharp EU a dostane
## čitelné jméno včetně písmen 'v','e','r' (Sharp EU 0xAB,0x92,0x9D).
test_cpm_put_mzf_force_charset() {
    local dsk="$TEST_TMPDIR/cpm_force_charset.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd --sides 2 --overwrite >/dev/null 2>&1 || return 1

    # MZF ftype=0x22, fname name "FOO" + Sharp EU "ver" (0xAB,0x92,0x9D) + "YY"
    # fname[0..7] = 46 4F 4F AB 92 9D 59 59
    # fname[8] = 0x2E '.'
    # fname[9..11] = "BIN" (0x42 0x49 0x4E, žádný bit 7)
    # fname[12..15] = 0x00 padding
    # fname[16] = 0x0D terminator
    # Pak fsize=4, fstrt=0, fexec=0, cmnt 104x0, data 4B.
    local mzf="$TEST_TMPDIR/force_cs.mzf"
    printf '\x22FOO\xAB\x92\x9DYY.BIN\x00\x00\x00\x00\x0D' > "$mzf"
    printf '\x04\x00\x00\x00\x00\x00' >> "$mzf"
    dd if=/dev/zero of="$mzf" bs=1 count=104 seek=24 conv=notrunc 2>/dev/null
    printf '\xDE\xAD\xBE\xEF' >> "$mzf"

    # Default put (CPM-IC masking): bajty AB,92,9D → 2B,12,1D → sanitized "+__"
    "$MZDSK_CPM" "$dsk" put --mzf "$mzf" >/dev/null 2>&1 || return 1
    local dir_default
    dir_default=$("$MZDSK_CPM" "$dsk" dir 2>&1)
    assert_contains "$dir_default" "__" "default path contains underscores from masked non-printables" || return 1

    # Reset disk
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd --sides 2 --overwrite >/dev/null 2>&1 || return 1

    # --force-charset --charset eu: Sharp EU dekóduje na "FOOverYY.BIN"
    # (CP/M directory ukládá uppercase, takže v dir výpisu je FOOVERYY).
    "$MZDSK_CPM" "$dsk" put --mzf "$mzf" --force-charset --charset eu >/dev/null 2>&1 || return 1
    local dir_forced
    dir_forced=$("$MZDSK_CPM" "$dsk" dir 2>&1)
    assert_contains "$dir_forced" "VER" "force-charset produces Sharp-decoded 'ver' in name" || return 1
    assert_not_contains "$dir_forced" "__" "force-charset path avoids underscores from masking" || return 1
}


## --no-attrs pro put --mzf: nedekódovat atributy z bitu 7 přípony.
## Default chování by pro ftype==0x22 s bitem 7 v ext bajtech nastavilo
## R/O + SYS + ARC; --no-attrs musí atributy ponechat na 0.
test_cpm_put_mzf_no_attrs() {
    local dsk="$TEST_TMPDIR/cpm_no_attrs.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd --sides 2 --overwrite >/dev/null 2>&1 || return 1

    # MZF ftype=0x22, fname = "TEST    .TXT" s bity 7 nastavenými v ext:
    # ext bajty "TXT" = 54 58 54, s bitem 7: D4 D8 D4
    # fname[0..7] = "TEST    " (T=54,E=45,S=53,T=54, 4x space=20)
    # fname[8] = '.' = 0x2E
    # fname[9..11] = D4 D8 D4 (TXT | 0x80)
    # fname[12..15] = 0x00 padding
    # fname[16] = 0x0D terminator
    local mzf="$TEST_TMPDIR/no_attrs.mzf"
    printf '\x22TEST    .\xD4\xD8\xD4\x00\x00\x00\x00\x0D' > "$mzf"
    printf '\x04\x00\x00\x00\x00\x00' >> "$mzf"
    dd if=/dev/zero of="$mzf" bs=1 count=104 seek=24 conv=notrunc 2>/dev/null
    printf '\xDE\xAD\xBE\xEF' >> "$mzf"

    # Default put: atributy se dekódují z bitů 7 → R/O + SYS + ARC
    "$MZDSK_CPM" "$dsk" put --mzf "$mzf" >/dev/null 2>&1 || return 1
    local attrs_default
    attrs_default=$("$MZDSK_CPM" "$dsk" attr TEST.TXT 2>&1)
    assert_contains "$attrs_default" "R" "default path sets R/O attribute" || return 1
    assert_contains "$attrs_default" "S" "default path sets SYS attribute" || return 1
    assert_contains "$attrs_default" "A" "default path sets ARC attribute" || return 1

    # Reset
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd --sides 2 --overwrite >/dev/null 2>&1 || return 1

    # --no-attrs: atributy zůstanou 0, i když bit 7 je nastaven
    "$MZDSK_CPM" "$dsk" put --mzf "$mzf" --no-attrs >/dev/null 2>&1 || return 1
    local attrs_no
    attrs_no=$("$MZDSK_CPM" "$dsk" attr TEST.TXT 2>&1)
    # S --no-attrs by žádný z R/S/A neměl být nastaven.
    assert_not_contains "$attrs_no" "+R" "no-attrs suppresses R/O attribute" || return 1
    assert_not_contains "$attrs_no" "+S" "no-attrs suppresses SYS attribute" || return 1
    assert_not_contains "$attrs_no" "+A" "no-attrs suppresses ARC attribute" || return 1
}


## --force-charset a --no-attrs bez --mzf musí selhat (smysluplné jen pro MZF import).
test_cpm_put_mzf_flags_without_mzf_fail() {
    local dsk="$TEST_TMPDIR/cpm_flags_nomzf.dsk"
    "$MZDSK_CREATE" "$dsk" --preset cpm-sd --sides 2 --overwrite >/dev/null 2>&1 || return 1

    local data="$TEST_TMPDIR/raw.bin"
    printf 'hello' > "$data"

    local out
    out=$("$MZDSK_CPM" "$dsk" put --force-charset "$data" SRC.BIN 2>&1)
    assert_neq "$?" "0" "put --force-charset without --mzf must fail" || return 1
    assert_contains "$out" "require --mzf" "error message mentions --mzf" || return 1

    out=$("$MZDSK_CPM" "$dsk" put --no-attrs "$data" SRC.BIN 2>&1)
    assert_neq "$?" "0" "put --no-attrs without --mzf must fail" || return 1
    assert_contains "$out" "require --mzf" "error message mentions --mzf" || return 1
}


## put --mzf poziční override stále používá tolerantní truncation (backward compat)
test_cpm_put_mzf_positional_still_lax() {
    local dsk="$TEST_TMPDIR/cpm_mzf_lax.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/data.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null
    "$MZDSK_CPM" "$dsk" put "$data" --name SRC.BIN >/dev/null 2>&1 || return 1
    local mzf="$TEST_TMPDIR/src.mzf"
    "$MZDSK_CPM" --overwrite "$dsk" get --mzf SRC.BIN "$mzf" >/dev/null 2>&1 || return 1
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 --overwrite 2>/dev/null || return 1

    local out
    out=$("$MZDSK_CPM" "$dsk" put --mzf "$mzf" TOOLONGNAME.EXT 2>&1)
    assert_eq "$?" "0" "positional lax must succeed with truncation" || return 1
    assert_contains "$out" "Warning" "tolerant path still warns" || return 1
    assert_contains "$out" "truncated to" "warning mentions truncation" || return 1
}


## get --all: raw export všech souborů
test_cpm_get_all_raw() {
    local dsk="$TEST_TMPDIR/cpm_ga_raw.dsk"
    create_cpm_with_files "$dsk" "AA.DAT" "BB.BIN" "CC.TXT" || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_raw_out"
    "$MZDSK_CPM" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "cpm get --all raw should succeed" || return 1

    assert_file_exists "$outdir/AA.DAT" "AA.DAT exported" || return 1
    assert_file_exists "$outdir/BB.BIN" "BB.BIN exported" || return 1
    assert_file_exists "$outdir/CC.TXT" "CC.TXT exported" || return 1

    # CP/M zarovnává na 128B, proto soubor má velikost 128 B
    local size=$(wc -c < "$outdir/AA.DAT" | tr -d ' ')
    assert_eq "$size" "128" "raw file size = 128 (CP/M record align)" || return 1
}


## get --all --mzf: MZF export všech souborů
test_cpm_get_all_mzf() {
    local dsk="$TEST_TMPDIR/cpm_ga_mzf.dsk"
    create_cpm_with_files "$dsk" "DD.DAT" "EE.BIN" || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_mzf_out"
    "$MZDSK_CPM" --overwrite "$dsk" get --all --mzf "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "cpm get --all --mzf should succeed" || return 1

    assert_file_exists "$outdir/DD.DAT.mzf" "DD.DAT.mzf exported" || return 1
    assert_file_exists "$outdir/EE.BIN.mzf" "EE.BIN.mzf exported" || return 1

    # MZF hlavička 128 B + data 128 B = 256 B
    local size=$(wc -c < "$outdir/DD.DAT.mzf" | tr -d ' ')
    assert_eq "$size" "256" "MZF file size = 128+128" || return 1
}


## get --all --mzf --ftype: override MZF ftype
test_cpm_get_all_mzf_ftype() {
    local dsk="$TEST_TMPDIR/cpm_ga_ft.dsk"
    create_cpm_with_files "$dsk" "FF.DAT" || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_ft_out"
    "$MZDSK_CPM" --overwrite "$dsk" get --all --mzf --ftype 0x05 "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "cpm get --all --mzf --ftype should succeed" || return 1

    # ftype na offsetu 0
    local ftype
    ftype=$(xxd -s 0 -l 1 -p "$outdir/FF.DAT.mzf")
    assert_eq "$ftype" "05" "MZF ftype = 0x05" || return 1
}


## get --all --on-duplicate rename: přejmenuje kolidující soubory na ~2
test_cpm_get_all_dup_rename() {
    local dsk="$TEST_TMPDIR/cpm_ga_rn.dsk"
    create_cpm_with_files "$dsk" "GG.DAT" "HH.TXT" || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_rn_out"
    # 1. export
    "$MZDSK_CPM" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1 || return 1
    # 2. export s rename: soubory se přejmenují na ~2
    "$MZDSK_CPM" --overwrite "$dsk" get --all --on-duplicate rename "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "second rename export should succeed" || return 1

    assert_file_exists "$outdir/GG~2.DAT" "GG~2.DAT created" || return 1
    assert_file_exists "$outdir/HH~2.TXT" "HH~2.TXT created" || return 1
}


## get --all --on-duplicate skip: přeskočí existující soubory
test_cpm_get_all_dup_skip() {
    local dsk="$TEST_TMPDIR/cpm_ga_sk.dsk"
    create_cpm_with_files "$dsk" "II.DAT" || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_sk_out"
    "$MZDSK_CPM" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1 || return 1

    local sha_before=$(sha1sum "$outdir/II.DAT" | awk '{print $1}')

    "$MZDSK_CPM" --overwrite "$dsk" get --all --on-duplicate skip "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "skip export should succeed" || return 1

    local sha_after=$(sha1sum "$outdir/II.DAT" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "file unchanged after skip" || return 1
}


## get --all --on-duplicate overwrite: přepíše existující soubory
test_cpm_get_all_dup_overwrite() {
    local dsk="$TEST_TMPDIR/cpm_ga_ovw.dsk"
    create_cpm_with_files "$dsk" "JJ.DAT" || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_ovw_out"
    "$MZDSK_CPM" --overwrite "$dsk" get --all "$outdir" >/dev/null 2>&1 || return 1

    "$MZDSK_CPM" --overwrite "$dsk" get --all --on-duplicate overwrite "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "overwrite export should succeed" || return 1
    assert_file_exists "$outdir/JJ.DAT" "JJ.DAT still exists after overwrite" || return 1
}


## get s --ftype bez --mzf: odmítne nekompatibilní kombinaci
## (sjednocení s mzdsk-mrs: nekompatibilní volby už nejsou tiše ignorovány)
test_cpm_get_rejects_ftype_without_mzf() {
    local dsk="$TEST_TMPDIR/cpm_rej_ft.dsk"
    create_cpm_with_files "$dsk" "KK.DAT" || return 1

    local out
    # --all + --ftype bez --mzf
    out=$("$MZDSK_CPM" --overwrite "$dsk" get --all "$TEST_TMPDIR/cpm_rej_ft_out" --ftype 0x22 2>&1)
    assert_neq "$?" "0" "get --all --ftype without --mzf must fail" || return 1
    assert_contains "$out" "require --mzf" "error mentions --mzf requirement" || return 1

    # single-file + --exec-addr bez --mzf
    out=$("$MZDSK_CPM" --overwrite "$dsk" get KK.DAT "$TEST_TMPDIR/cpm_rej_ft.out" --exec-addr 0x1000 2>&1)
    assert_neq "$?" "0" "single-file get --exec-addr without --mzf must fail" || return 1

    # --strt-addr bez --mzf
    out=$("$MZDSK_CPM" --overwrite "$dsk" get KK.DAT "$TEST_TMPDIR/cpm_rej_ft.out" --strt-addr 0x2000 2>&1)
    assert_neq "$?" "0" "--strt-addr without --mzf must fail" || return 1

    # --no-attrs bez --mzf
    out=$("$MZDSK_CPM" --overwrite "$dsk" get KK.DAT "$TEST_TMPDIR/cpm_rej_ft.out" --no-attrs 2>&1)
    assert_neq "$?" "0" "--no-attrs without --mzf must fail" || return 1
}


## get --by-user bez --all: nemá smysl, musí selhat
test_cpm_get_rejects_by_user_without_all() {
    local dsk="$TEST_TMPDIR/cpm_rej_byu.dsk"
    create_cpm_with_files "$dsk" "BU.DAT" || return 1

    local out
    out=$("$MZDSK_CPM" --overwrite "$dsk" get BU.DAT "$TEST_TMPDIR/cpm_rej_byu.out" --by-user 2>&1)
    assert_neq "$?" "0" "--by-user without --all must fail" || return 1
    assert_contains "$out" "by-user" "error mentions --by-user" || return 1
}


## get --on-duplicate bez --all: nemá smysl, musí selhat
test_cpm_get_rejects_on_duplicate_without_all() {
    local dsk="$TEST_TMPDIR/cpm_rej_dup.dsk"
    create_cpm_with_files "$dsk" "OD.DAT" || return 1

    local out
    out=$("$MZDSK_CPM" --overwrite "$dsk" get OD.DAT "$TEST_TMPDIR/cpm_rej_dup.out" --on-duplicate skip 2>&1)
    assert_neq "$?" "0" "--on-duplicate without --all must fail" || return 1
    assert_contains "$out" "on-duplicate" "error mentions --on-duplicate" || return 1
}


## get --all --on-duplicate <invalid>: odmítne neplatný režim
test_cpm_get_all_dup_invalid_mode() {
    local dsk="$TEST_TMPDIR/cpm_ga_inv.dsk"
    create_cpm_with_files "$dsk" "KK.DAT" || return 1

    local out
    out=$("$MZDSK_CPM" --overwrite "$dsk" get --all --on-duplicate nonsense "$TEST_TMPDIR/cpm_ga_inv_out" 2>&1)
    assert_neq "$?" "0" "invalid --on-duplicate mode must fail" || return 1
    assert_contains "$out" "on-duplicate" "error mentions on-duplicate" || return 1
}


## get --all z prázdného disku
test_cpm_get_all_empty() {
    local dsk="$TEST_TMPDIR/cpm_ga_empty.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 --overwrite >/dev/null 2>&1 || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_empty_out"
    local out
    out=$("$MZDSK_CPM" --overwrite "$dsk" get --all "$outdir" 2>&1)
    assert_eq "$?" "0" "get --all from empty disk should succeed" || return 1
    assert_contains "$out" "No files" "output mentions no files" || return 1
}


## get --all --exec-addr a --strt-addr override u MZF exportu
test_cpm_get_all_addr_override() {
    local dsk="$TEST_TMPDIR/cpm_ga_addr.dsk"
    create_cpm_with_files "$dsk" "LL.DAT" || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_addr_out"
    "$MZDSK_CPM" --overwrite "$dsk" get --all --mzf --exec-addr 0x4000 --strt-addr 0x2000 "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "cpm get --all addr override should succeed" || return 1

    # fstrt na offsetu 0x14, fexec na offsetu 0x16 (LE)
    local fstrt fexec
    fstrt=$(xxd -s 0x14 -l 2 -p "$outdir/LL.DAT.mzf")
    fexec=$(xxd -s 0x16 -l 2 -p "$outdir/LL.DAT.mzf")
    assert_eq "$fstrt" "0020" "MZF fstrt = 0x2000 (LE)" || return 1
    assert_eq "$fexec" "0040" "MZF fexec = 0x4000 (LE)" || return 1
}


## get --all --by-user: rozdělí export do podadresářů user00/, user01/
## CP/M specifická funkce - MRS a FSMZ nemají user areas.
test_cpm_get_all_by_user() {
    local dsk="$TEST_TMPDIR/cpm_ga_byu.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 --overwrite >/dev/null 2>&1 || return 1

    # Put do user 0 (default)
    local p0="$TEST_TMPDIR/cpm_ga_byu_p0.bin"
    dd if=/dev/urandom of="$p0" bs=128 count=1 2>/dev/null
    "$MZDSK_CPM" "$dsk" put "$p0" "USR0.DAT" >/dev/null 2>&1 || return 1

    # Put do user 1 (přes --user N)
    local p1="$TEST_TMPDIR/cpm_ga_byu_p1.bin"
    dd if=/dev/urandom of="$p1" bs=128 count=1 2>/dev/null
    "$MZDSK_CPM" --user 1 "$dsk" put "$p1" "USR1.DAT" >/dev/null 2>&1 || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_byu_out"
    "$MZDSK_CPM" --overwrite "$dsk" get --all --by-user "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "cpm get --all --by-user should succeed" || return 1

    assert_file_exists "$outdir/user00/USR0.DAT" "user00/USR0.DAT exported" || return 1
    assert_file_exists "$outdir/user01/USR1.DAT" "user01/USR1.DAT exported" || return 1
}


## Global --user N + get --all: exportuje jen soubory daného uživatele
test_cpm_get_all_user_filter() {
    local dsk="$TEST_TMPDIR/cpm_ga_uf.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 --overwrite >/dev/null 2>&1 || return 1

    local p0="$TEST_TMPDIR/cpm_ga_uf_p0.bin"
    local p1="$TEST_TMPDIR/cpm_ga_uf_p1.bin"
    dd if=/dev/urandom of="$p0" bs=128 count=1 2>/dev/null
    dd if=/dev/urandom of="$p1" bs=128 count=1 2>/dev/null

    "$MZDSK_CPM" "$dsk" put "$p0" "U0.DAT" >/dev/null 2>&1 || return 1
    "$MZDSK_CPM" --user 1 "$dsk" put "$p1" "U1.DAT" >/dev/null 2>&1 || return 1

    local outdir="$TEST_TMPDIR/cpm_ga_uf_out"
    # filter na user 1: jen U1.DAT se má exportovat
    "$MZDSK_CPM" --user 1 "$dsk" get --all "$outdir" >/dev/null 2>&1
    assert_eq "$?" "0" "cpm --user 1 get --all should succeed" || return 1

    assert_file_exists "$outdir/U1.DAT" "U1.DAT exported (user 1)" || return 1
    # U0.DAT nesmí být v adresáři
    if [ -f "$outdir/U0.DAT" ]; then
        assert_eq "0" "1" "U0.DAT must NOT be exported (user 0 filtered out)" || return 1
    fi
}


## put --user N: soubor se uloží do zadané user oblasti
test_cpm_put_user_target() {
    local dsk="$TEST_TMPDIR/cpm_put_user.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/put_u_data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null

    # uložení do user 5 (globální --user N je target user pro put)
    "$MZDSK_CPM" --user 5 "$dsk" put "$data" "HI.TXT" >/dev/null 2>&1
    assert_eq "$?" "0" "put --user 5 should succeed" || return 1

    # user 5 musí obsahovat soubor
    local out
    out=$("$MZDSK_CPM" --user 5 "$dsk" dir 2>&1)
    assert_contains "$out" "HI" "HI.TXT must be in user 5" || return 1

    # user 0 musí být prázdný
    out=$("$MZDSK_CPM" --user 0 "$dsk" dir 2>&1)
    assert_not_contains "$out" "HI" "HI.TXT must not be in user 0" || return 1
}


## put --user N: stejné jméno v různých user oblastech je povoleno (CP/M 2.2)
test_cpm_put_user_same_name_different_users() {
    local dsk="$TEST_TMPDIR/cpm_put_user_dup.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    # CP/M zarovnává na 128B záznamy - použijeme násobek 128
    local d1="$TEST_TMPDIR/put_u_d1.bin"
    local d2="$TEST_TMPDIR/put_u_d2.bin"
    dd if=/dev/urandom of="$d1" bs=128 count=1 2>/dev/null
    dd if=/dev/urandom of="$d2" bs=128 count=1 2>/dev/null

    # stejné jméno ve dvou různých user oblastech musí projít
    "$MZDSK_CPM" --user 0 "$dsk" put "$d1" "SAME.TXT" >/dev/null 2>&1
    assert_eq "$?" "0" "put SAME.TXT user 0 should succeed" || return 1

    "$MZDSK_CPM" --user 3 "$dsk" put "$d2" "SAME.TXT" >/dev/null 2>&1
    assert_eq "$?" "0" "put SAME.TXT user 3 should succeed (different namespace)" || return 1

    # Každá user oblast má vlastní obsah - ověříme získáním zpět.
    # Porovnáváme prvních 128B (CP/M zaokrouhluje nahoru na celý záznam).
    local out1="$TEST_TMPDIR/put_u_out1.bin"
    local out3="$TEST_TMPDIR/put_u_out3.bin"
    "$MZDSK_CPM" --user 0 "$dsk" get SAME.TXT "$out1" >/dev/null 2>&1
    "$MZDSK_CPM" --user 3 "$dsk" get SAME.TXT "$out3" >/dev/null 2>&1

    local sha_d1=$(dd if="$d1" bs=128 count=1 2>/dev/null | sha1sum | awk '{print $1}')
    local sha_d3=$(dd if="$d2" bs=128 count=1 2>/dev/null | sha1sum | awk '{print $1}')
    local sha_o1=$(dd if="$out1" bs=128 count=1 2>/dev/null | sha1sum | awk '{print $1}')
    local sha_o3=$(dd if="$out3" bs=128 count=1 2>/dev/null | sha1sum | awk '{print $1}')
    assert_eq "$sha_o1" "$sha_d1" "user 0 content preserved" || return 1
    assert_eq "$sha_o3" "$sha_d3" "user 3 content preserved" || return 1
    # a obsahy v user 0 a user 3 se musí lišit (různá náhodná data)
    assert_neq "$sha_o1" "$sha_o3" "user 0 and user 3 content must differ" || return 1
}


## put --user N: duplicita v rámci stejného usera musí selhat
test_cpm_put_user_duplicate_in_same_user() {
    local dsk="$TEST_TMPDIR/cpm_put_user_same.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/put_u_same.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null

    "$MZDSK_CPM" --user 2 "$dsk" put "$data" "DUP.TXT" >/dev/null 2>&1
    assert_eq "$?" "0" "first put should succeed" || return 1

    # druhý put se stejným jménem v user 2 musí selhat
    local out
    out=$("$MZDSK_CPM" --user 2 "$dsk" put "$data" "DUP.TXT" 2>&1)
    assert_neq "$?" "0" "second put in same user must fail" || return 1
    assert_contains "$out" "File exists" "error should mention File exists" || return 1
}


## chuser: změna user number existujícího souboru (basic case)
test_cpm_chuser_basic() {
    local dsk="$TEST_TMPDIR/cpm_chuser_basic.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/chu_data.bin"
    dd if=/dev/urandom of="$data" bs=128 count=1 2>/dev/null

    # vložíme soubor do user 0, pak přepneme na user 3
    "$MZDSK_CPM" "$dsk" put "$data" "FOO.BAR" >/dev/null 2>&1 || return 1

    local out
    out=$("$MZDSK_CPM" "$dsk" chuser FOO.BAR 3 2>&1)
    assert_eq "$?" "0" "chuser 0 -> 3 should succeed" || return 1
    assert_contains "$out" "0 -> 3" "chuser should report new user" || return 1

    # user 0 - soubor nesmí být, user 3 - soubor musí být
    out=$("$MZDSK_CPM" --user 0 "$dsk" dir 2>&1)
    assert_not_contains "$out" "FOO" "FOO must not be in user 0 after chuser" || return 1

    out=$("$MZDSK_CPM" --user 3 "$dsk" dir 2>&1)
    assert_contains "$out" "FOO" "FOO must be in user 3 after chuser" || return 1
}


## chuser: neexistující soubor musí selhat
test_cpm_chuser_not_found() {
    local dsk="$TEST_TMPDIR/cpm_chuser_notfound.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local sha_before=$(sha1sum "$dsk" | awk '{print $1}')
    "$MZDSK_CPM" "$dsk" chuser XYZ.COM 5 >/dev/null 2>&1
    assert_neq "$?" "0" "chuser of non-existent file must fail" || return 1
    local sha_after=$(sha1sum "$dsk" | awk '{print $1}')
    assert_eq "$sha_after" "$sha_before" "disk unchanged after chuser failure" || return 1
}


## chuser: nový user mimo rozsah 0-15 musí selhat
test_cpm_chuser_invalid_user() {
    local dsk="$TEST_TMPDIR/cpm_chuser_badusr.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local data="$TEST_TMPDIR/chu_bad.bin"
    dd if=/dev/urandom of="$data" bs=64 count=1 2>/dev/null
    "$MZDSK_CPM" "$dsk" put "$data" "AA.BB" >/dev/null 2>&1 || return 1

    # mimo rozsah
    "$MZDSK_CPM" "$dsk" chuser AA.BB 16 >/dev/null 2>&1
    assert_neq "$?" "0" "chuser user=16 must fail" || return 1

    # ne-číslo
    "$MZDSK_CPM" "$dsk" chuser AA.BB abc >/dev/null 2>&1
    assert_neq "$?" "0" "chuser user=abc must fail" || return 1
}


## chuser: kolize s existujícím souborem v cílové user oblasti
test_cpm_chuser_collision() {
    local dsk="$TEST_TMPDIR/cpm_chuser_col.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    local d1="$TEST_TMPDIR/chu_c1.bin"
    local d2="$TEST_TMPDIR/chu_c2.bin"
    dd if=/dev/urandom of="$d1" bs=128 count=1 2>/dev/null
    dd if=/dev/urandom of="$d2" bs=128 count=1 2>/dev/null

    # stejné jméno ve dvou různých user oblastech je validní
    "$MZDSK_CPM" --user 0 "$dsk" put "$d1" "DUP.DAT" >/dev/null 2>&1 || return 1
    "$MZDSK_CPM" --user 5 "$dsk" put "$d2" "DUP.DAT" >/dev/null 2>&1 || return 1

    # chuser 0 -> 5 musí selhat kvůli kolizi v user 5
    local out
    out=$("$MZDSK_CPM" "$dsk" chuser DUP.DAT 5 2>&1)
    assert_neq "$?" "0" "chuser must fail on namespace collision" || return 1
    assert_contains "$out" "File exists" "error should mention File exists" || return 1
}


## chuser: multi-extent soubor - všechny extenty musí být přepsány
test_cpm_chuser_multi_extent() {
    local dsk="$TEST_TMPDIR/cpm_chuser_multi.dsk"
    "$MZDSK_CREATE" "$dsk" --format-cpm 80 2>/dev/null || return 1

    # 40 kB soubor = cca 3 extenty (extent = 16 kB)
    local data="$TEST_TMPDIR/chu_big.bin"
    dd if=/dev/urandom of="$data" bs=1024 count=40 2>/dev/null

    "$MZDSK_CPM" "$dsk" put "$data" "BIG.DAT" >/dev/null 2>&1 || return 1
    "$MZDSK_CPM" "$dsk" chuser BIG.DAT 7 >/dev/null 2>&1
    assert_eq "$?" "0" "chuser on multi-extent file should succeed" || return 1

    # obsah v user 7 musí být shodný s původním (všechny extenty převedeny)
    local out="$TEST_TMPDIR/chu_big_out.bin"
    "$MZDSK_CPM" --user 7 "$dsk" get BIG.DAT "$out" >/dev/null 2>&1
    assert_eq "$?" "0" "get from user 7 after chuser should succeed" || return 1

    local sha_in=$(sha1sum "$data" | awk '{print $1}')
    local sha_out=$(sha1sum "$out" | awk '{print $1}')
    assert_eq "$sha_out" "$sha_in" "file content preserved across chuser" || return 1
}


# --- Spuštění ---

run_test test_cpm_rejects_fsmz_disk
run_test test_cpm_rejects_mrs_disk
run_test test_cpm_force_bypasses_detection
run_test test_cpm_accepts_cpm_disk
run_test test_cpm_get_mzf_exec_addr
run_test test_cpm_get_mzf_default_exec_addr
run_test test_cpm_get_mzf_addr_alias
run_test test_cpm_get_mzf_ftype
run_test test_cpm_get_mzf_strt_addr
run_test test_cpm_get_mzf_no_attrs
run_test test_cpm_put_truncates_with_warning
run_test test_cpm_put_truncation_duplicate_fails
run_test test_cpm_output_after_subcmd
run_test test_cpm_era_atomicity
run_test test_cpm_put_atomicity
run_test test_cpm_put_strict_name_ok
run_test test_cpm_put_strict_name_too_long_fails
run_test test_cpm_put_strict_name_forbidden_char_fails
run_test test_cpm_put_strict_name_conflict_with_positional
run_test test_cpm_put_mzf_strict_name_ok
run_test test_cpm_put_mzf_strict_name_fails
run_test test_cpm_put_mzf_preserves_detection_flappy
run_test test_cpm_put_mzf_preserves_detection_leading_space
run_test test_cpm_put_mzf_charset_invalid
run_test test_cpm_put_mzf_force_charset
run_test test_cpm_put_mzf_no_attrs
run_test test_cpm_put_mzf_flags_without_mzf_fail
run_test test_cpm_put_mzf_positional_still_lax

run_test test_cpm_get_all_raw
run_test test_cpm_get_all_mzf
run_test test_cpm_get_all_mzf_ftype
run_test test_cpm_get_all_dup_rename
run_test test_cpm_get_all_dup_skip
run_test test_cpm_get_all_dup_overwrite
run_test test_cpm_get_rejects_ftype_without_mzf
run_test test_cpm_get_rejects_by_user_without_all
run_test test_cpm_get_rejects_on_duplicate_without_all
run_test test_cpm_get_all_dup_invalid_mode
run_test test_cpm_get_all_empty
run_test test_cpm_get_all_addr_override
run_test test_cpm_get_all_by_user
run_test test_cpm_get_all_user_filter

run_test test_cpm_put_user_target
run_test test_cpm_put_user_same_name_different_users
run_test test_cpm_put_user_duplicate_in_same_user

run_test test_cpm_chuser_basic
run_test test_cpm_chuser_not_found
run_test test_cpm_chuser_invalid_user
run_test test_cpm_chuser_collision
run_test test_cpm_chuser_multi_extent

test_summary
