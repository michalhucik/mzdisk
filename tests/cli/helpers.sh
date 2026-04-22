#!/bin/bash
# Sdílené pomocné funkce pro CLI testy.
# Source: source tests/cli/helpers.sh

# Odpojit stdin od případného TTY - CLI nástroje (mzdsk-fsmz/cpm/mrs) při
# destruktivních operacích (format/defrag/init) interaktivně potvrzují
# operaci, pokud je stdin TTY. V testech to způsobí zatuhnutí čekáním
# na uživatelský vstup. Non-TTY stdin => confirm_destructive_op() automaticky
# pokračuje bez dotazu.
#
# POZOR: Na MSYS2/MinGW je exec </dev/null nedostatečné - MinGW isatty()
# vidí stále konzolový Windows handle. Proto destruktivní operace v testech
# používají navíc flag -y/--yes (audit M-17).
exec </dev/null

# Cesty k binárám
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CLI_DIR="$PROJECT_DIR/build-cli"
REFDATA_DIR="$PROJECT_DIR/refdata/dsk"

MZDSK_INFO="$CLI_DIR/mzdsk-info"
MZDSK_CREATE="$CLI_DIR/mzdsk-create"
MZDSK_FSMZ="$CLI_DIR/mzdsk-fsmz"
MZDSK_CPM="$CLI_DIR/mzdsk-cpm"
MZDSK_MRS="$CLI_DIR/mzdsk-mrs"
MZDSK_RAW="$CLI_DIR/mzdsk-raw"
MZDSK_DSK="$CLI_DIR/mzdsk-dsk"

# Temp adresář pro testy
TEST_TMPDIR=$(mktemp -d)
trap "rm -rf $TEST_TMPDIR" EXIT

# Počítadla
_PASSED=0
_FAILED=0
_TOTAL=0

# Barvy (pokud terminál podporuje)
if [ -t 1 ]; then
    _GREEN='\033[0;32m'
    _RED='\033[0;31m'
    _NC='\033[0m'
else
    _GREEN=''
    _RED=''
    _NC=''
fi

## assert_eq "actual" "expected" "message"
assert_eq() {
    if [ "$1" = "$2" ]; then
        return 0
    else
        echo -e "    ${_RED}FAIL${_NC}: $3"
        echo "      expected: '$2'"
        echo "      actual:   '$1'"
        return 1
    fi
}

## assert_neq "actual" "unexpected" "message"
assert_neq() {
    if [ "$1" != "$2" ]; then
        return 0
    else
        echo -e "    ${_RED}FAIL${_NC}: $3"
        echo "      expected NOT: '$2'"
        echo "      actual:       '$1'"
        return 1
    fi
}

## assert_contains "string" "substring" "message"
assert_contains() {
    if echo "$1" | grep -q "$2"; then
        return 0
    else
        echo -e "    ${_RED}FAIL${_NC}: $3"
        echo "      string: '$1'"
        echo "      missing: '$2'"
        return 1
    fi
}

## assert_not_contains "string" "substring" "message"
assert_not_contains() {
    if echo "$1" | grep -q "$2"; then
        echo -e "    ${_RED}FAIL${_NC}: $3"
        echo "      string: '$1'"
        echo "      unexpected: '$2'"
        return 1
    else
        return 0
    fi
}

## assert_file_exists "path" "message"
assert_file_exists() {
    if [ -f "$1" ]; then
        return 0
    else
        echo -e "    ${_RED}FAIL${_NC}: $2 - file not found: $1"
        return 1
    fi
}

## json_get "json_string" "key" - jednoduché grep parsování JSON
## Podporuje: "key": value, "key": "value"
json_get() {
    local val
    # Zkusit string hodnotu
    val=$(echo "$1" | grep -o "\"$2\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" | head -1 | sed "s/\"$2\"[[:space:]]*:[[:space:]]*\"//" | sed 's/"$//')
    if [ -n "$val" ]; then
        echo "$val"
        return
    fi
    # Zkusit numerickou hodnotu
    val=$(echo "$1" | grep -o "\"$2\"[[:space:]]*:[[:space:]]*[0-9]*" | head -1 | sed "s/\"$2\"[[:space:]]*:[[:space:]]*//" )
    echo "$val"
}

## assert_json_eq "json_string" "key" "expected_value" "message"
assert_json_eq() {
    local actual
    actual=$(json_get "$1" "$2")
    assert_eq "$actual" "$3" "${4:-JSON $2 == $3}"
}

## run_test "test_function_name"
run_test() {
    _TOTAL=$((_TOTAL + 1))
    printf "  %-55s" "$1"
    local _output
    _output=$($1 2>&1)
    if [ $? -eq 0 ]; then
        echo -e "${_GREEN}OK${_NC}"
        _PASSED=$((_PASSED + 1))
    else
        echo -e "${_RED}FAIL${_NC}"
        echo "$_output" | grep -E "(FAIL|Error)" | head -5
        _FAILED=$((_FAILED + 1))
    fi
}

## test_summary - výpis souhrnu a návratový kód
test_summary() {
    echo ""
    echo "--- $_PASSED passed, $_FAILED failed, $_TOTAL total ---"
    if [ $_FAILED -ne 0 ]; then
        return 1
    fi
    return 0
}


# ============================================================================
# Fixture disky: syntetické (default) nebo refdata (USE_REFDATA=1).
#
# Historicky testy četly přímo z refdata/dsk/*.dsk. Nyní si syntetické verze
# umí vyrobit CLI nástroji za běhu. Refdata zůstávají dostupná přes
# USE_REFDATA=1 (spouštěno z `make test-cli-refdata`).
#
# Každá fixture funkce vrací (echo) cestu k disku a nastavuje metadata
# proměnné (počty, typ inkonzistence) - testy se podle nich přizpůsobí.
# ============================================================================

## make_mzf <out> <name> <size> [ftype]
## Vytvoří minimální validní MZF soubor s náhodným obsahem dané velikosti.
make_mzf() {
    local out="$1"
    local name="$2"
    local size="$3"
    local ftype="${4:-0x01}"

    dd if=/dev/zero bs=128 count=1 of="$out" 2>/dev/null
    printf "\\x$(printf '%02x' $((ftype)))" | dd of="$out" bs=1 seek=0 conv=notrunc 2>/dev/null
    printf '%s\x0d' "$name" | dd of="$out" bs=1 seek=1 conv=notrunc 2>/dev/null
    printf "\\x$(printf '%02x' $((size & 0xFF)))\\x$(printf '%02x' $(((size >> 8) & 0xFF)))" \
        | dd of="$out" bs=1 seek=18 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$out" bs=1 seek=20 conv=notrunc 2>/dev/null
    printf '\x00\x12' | dd of="$out" bs=1 seek=22 conv=notrunc 2>/dev/null
    dd if=/dev/urandom bs="$size" count=1 2>/dev/null >> "$out"
}


## _build_fsmz_dups_fixture <out>
## FSMZ disk se 6 soubory, 2 páry duplicitních jmen (celkem 2 kolize).
## Duplicita se vytvoří nepřímo: po `put` se raw edituje jmeno v adresáři
## (CLI odmítá `put`/`ren` s existujícím jménem, protože FS normálně unikátní).
_build_fsmz_dups_fixture() {
    local out="$1"

    "$MZDSK_CREATE" "$out" --format-basic 80 >/dev/null 2>&1 || return 1

    # 6 unikátních souborů v pořadí FA, FB, FC, FD, FE, FF
    local name
    for name in FA FB FC FD FE FF; do
        local mzf="$TEST_TMPDIR/_fx_fsmz_${name}.mzf"
        make_mzf "$mzf" "$name" 256
        "$MZDSK_FSMZ" "$out" put "$mzf" >/dev/null 2>&1 || return 1
    done

    # Adresář je na track 0, sector 1. Sloty po 32 B, fname na offsetu +1 (17 B).
    # Slot 0 = rezervovaný, slot 1 = první soubor (FA), ..., slot 6 = FF.
    # Přepíšeme fname slotu 2 (FB -> FA) a slotu 4 (FD -> FC) => 2 páry duplicit.
    local sec="$TEST_TMPDIR/_fx_fsmz_dir.bin"
    "$MZDSK_RAW" --noinv "$out" get "$sec" --track 0 --sector 1 --sectors 1 \
        >/dev/null 2>&1 || return 1
    printf 'FA\x0d' | dd of="$sec" bs=1 seek=$((2*32 + 1)) count=3 conv=notrunc 2>/dev/null
    printf 'FC\x0d' | dd of="$sec" bs=1 seek=$((4*32 + 1)) count=3 conv=notrunc 2>/dev/null
    "$MZDSK_RAW" --noinv "$out" put "$sec" --track 0 --sector 1 --sectors 1 \
        >/dev/null 2>&1 || return 1
    rm -f "$sec"
    return 0
}


## _build_mrs_inconsistent_fixture <out>
## MRS disk se 3 soubory (každý 2 bloky). U M2 je v adresáři snížená bsize
## z 2 na 1, takže dir_bsize_sum=5 vs file_blocks=6 (FAT nezměněn) => BUG 18
## varování se zobrazí. Defrag scénář opraví (všechny soubory jsou čitelné).
_build_mrs_inconsistent_fixture() {
    local out="$1"

    "$MZDSK_CREATE" "$out" --preset mrs >/dev/null 2>&1 || return 1
    "$MZDSK_MRS" -y "$out" init >/dev/null 2>&1 || return 1

    # 3 soubory po 1024 B (= 2 bloky každý) v pořadí M1, M2, M3.
    # Po init začínají adresářové položky v bloku 39 (blok 38 je rezervovaný).
    local name
    for name in M1 M2 M3; do
        local mzf="$TEST_TMPDIR/_fx_mrs_${name}.mzf"
        make_mzf "$mzf" "$name" 1024
        "$MZDSK_MRS" "$out" put --mzf "$mzf" >/dev/null 2>&1 || return 1
    done

    # Adresář je v bloku 39, položky po 32 B (MRS DIR_ITEM).
    # bsize leží na offsetu 14 uvnitř položky (fname 8 + ext 3 + file_id 1 + fstrt 2).
    # M2 je slot 1 (offset 32), bsize na 32+14 = 46. Snížíme z 0x0002 na 0x0001.
    local dir="$TEST_TMPDIR/_fx_mrs_dir.bin"
    "$MZDSK_MRS" "$out" get-block 39 "$dir" >/dev/null 2>&1 || return 1
    printf '\x01\x00' | dd of="$dir" bs=1 seek=46 count=2 conv=notrunc 2>/dev/null
    "$MZDSK_MRS" "$out" put-block 39 "$dir" >/dev/null 2>&1 || return 1
    rm -f "$dir"
    return 0
}


## _build_mrs_read_errors_fixture <out>
## MRS disk se 3 soubory, u 2 z nich jsou FAT záznamy vynulované. V adresáři
## zůstává bsize=1, ale FAT říká 0 bloků => fsmrs_read_file selže s Format
## error. Použito pro test, že `get --all` vrací nenulový exit code
## při read errorech, ale čitelné soubory extrahuje OK.
_build_mrs_read_errors_fixture() {
    local out="$1"

    "$MZDSK_CREATE" "$out" --preset mrs >/dev/null 2>&1 || return 1
    "$MZDSK_MRS" -y "$out" init >/dev/null 2>&1 || return 1

    local name
    for name in M1 M2 M3; do
        local mzf="$TEST_TMPDIR/_fx_mrse_${name}.mzf"
        make_mzf "$mzf" "$name" 512
        "$MZDSK_MRS" "$out" put --mzf "$mzf" >/dev/null 2>&1 || return 1
    done

    # FAT na bloku 36. Po 3x put mají offsety 0x2D/0x2E/0x2F file_id 1/2/3.
    # Vynulujeme záznamy pro M2 a M3 => dir má bsize=1, FAT má 0 bloků
    # => fsmrs_read_file vrátí MZDSK_RES_FORMAT_ERROR.
    local fat="$TEST_TMPDIR/_fx_mrse_fat.bin"
    "$MZDSK_MRS" "$out" get-block 36 "$fat" >/dev/null 2>&1 || return 1
    printf '\x00\x00' | dd of="$fat" bs=1 seek=46 count=2 conv=notrunc 2>/dev/null
    "$MZDSK_MRS" "$out" put-block 36 "$fat" >/dev/null 2>&1 || return 1
    rm -f "$fat"
    return 0
}


## _build_cpm_sample_fixture <out>
## Jednoduchý CP/M SD disk s několika soubory - stačí pro ověření,
## že `dir -o json/csv` produkuje validní výstup (test BUG 17).
_build_cpm_sample_fixture() {
    local out="$1"

    "$MZDSK_CREATE" "$out" --format-cpm 80 >/dev/null 2>&1 || return 1

    local data="$TEST_TMPDIR/_fx_cpm_data.bin"
    dd if=/dev/urandom of="$data" bs=512 count=1 2>/dev/null
    local name
    for name in C1.COM C2.DAT C3.TXT; do
        "$MZDSK_CPM" "$out" put "$data" "$name" >/dev/null 2>&1 || return 1
    done
    rm -f "$data"
    return 0
}


## fixture_fsmz_with_dups
## FSMZ disk s duplicitními jmény. Fixture funkce NASTAVUJE globální proměnné
## (nevrací hodnoty přes stdout, aby šly proměnné použít mimo subshell):
##   FIXTURE_FSMZ_DSK         - cesta k disku
##   FIXTURE_FSMZ_TOTAL       - celkový počet souborů
##   FIXTURE_FSMZ_DUP_PAIRS   - počet duplicitních dvojic (= `~2` po `get --all`)
fixture_fsmz_with_dups() {
    if [ "${USE_REFDATA:-0}" = "1" ]; then
        FIXTURE_FSMZ_DSK="$REFDATA_DIR/basic.dsk"
        FIXTURE_FSMZ_TOTAL=52
        FIXTURE_FSMZ_DUP_PAIRS=2
        return 0
    fi
    local out="$TEST_TMPDIR/_fixture_fsmz_dups.dsk"
    if [ ! -f "$out" ]; then
        _build_fsmz_dups_fixture "$out" || return 1
    fi
    FIXTURE_FSMZ_DSK="$out"
    FIXTURE_FSMZ_TOTAL=6
    FIXTURE_FSMZ_DUP_PAIRS=2
    return 0
}


## fixture_mrs_inconsistent
## MRS disk s FAT vs. dir bsize inkonzistencí, kterou defrag dokáže opravit.
## V syntetické variantě je inkonzistence vyrobená snížením bsize v adresáři.
## V refdata má disk navíc scénář s "skrytým 3. FAT sektorem" (viz fsmrs_defrag),
## díky čemuž defrag opraví i soubory, které by jinak vypadaly jako unreadable.
## Nastavuje globál:
##   FIXTURE_MRS_DSK  - cesta k disku
fixture_mrs_inconsistent() {
    if [ "${USE_REFDATA:-0}" = "1" ]; then
        FIXTURE_MRS_DSK="$REFDATA_DIR/mrs.dsk"
        return 0
    fi
    local out="$TEST_TMPDIR/_fixture_mrs_inconsistent.dsk"
    if [ ! -f "$out" ]; then
        _build_mrs_inconsistent_fixture "$out" || return 1
    fi
    FIXTURE_MRS_DSK="$out"
    return 0
}


## fixture_mrs_with_read_errors
## MRS disk s několika neporušenými soubory a několika nečitelnými
## (bsize=1, ale FAT říká 0 bloků - `fsmrs_read_file` selže).
## Refdata sdílí tuto vlastnost se základním mrs.dsk.
## Nastavuje globál:
##   FIXTURE_MRS_ERR_DSK  - cesta k disku
fixture_mrs_with_read_errors() {
    if [ "${USE_REFDATA:-0}" = "1" ]; then
        FIXTURE_MRS_ERR_DSK="$REFDATA_DIR/mrs.dsk"
        return 0
    fi
    local out="$TEST_TMPDIR/_fixture_mrs_read_errors.dsk"
    if [ ! -f "$out" ]; then
        _build_mrs_read_errors_fixture "$out" || return 1
    fi
    FIXTURE_MRS_ERR_DSK="$out"
    return 0
}


## fixture_cpm_sample
## CP/M SD disk s několika soubory. Nastavuje globál:
##   FIXTURE_CPM_DSK  - cesta k disku
fixture_cpm_sample() {
    if [ "${USE_REFDATA:-0}" = "1" ]; then
        FIXTURE_CPM_DSK="$REFDATA_DIR/cpmsd.dsk"
        return 0
    fi
    local out="$TEST_TMPDIR/_fixture_cpm_sample.dsk"
    if [ ! -f "$out" ]; then
        _build_cpm_sample_fixture "$out" || return 1
    fi
    FIXTURE_CPM_DSK="$out"
    return 0
}
