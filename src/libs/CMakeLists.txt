# ===========================================================================
#  Sdílené knihovny mzdsk projektu
#
#  Kompilují se jednou do build-libs/ a používají je CLI i GUI.
#  Samostatný CMake projekt - volá se přes "cmake --preset libs".
# ===========================================================================

cmake_minimum_required(VERSION 3.16)
project(mzdsk-libs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -pipe)

# Standalone build - bez baseui z mz800emu
add_compile_definitions(GENERIC_DRIVER_NO_BASEUI)
add_compile_definitions(GENERIC_DRIVER_FILE_CB)
add_compile_definitions(GENERIC_DRIVER_MEMORY_CB)

# Include path - "libs/..." cesty vyžadují src/ jako kořen
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

# .a soubory přímo do kořene build adresáře (build-libs/)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# --- Knihovny ---

add_library(generic_driver STATIC
    generic_driver/generic_driver.c
    generic_driver/memory_driver.c
)

add_library(endianity STATIC
    endianity/endianity.c
)

add_library(sharpmz_ascii STATIC
    sharpmz_ascii/sharpmz_ascii.c
)

add_library(mzf STATIC
    mzf/mzf.c
    mzf/mzf_tools.c
)

add_library(dsk STATIC
    dsk/dsk.c
    dsk/dsk_tools.c
)

add_library(mzdsk_global STATIC
    mzdsk_global/mzdsk_global.c
)

add_library(mzdsk_ipldisk STATIC
    mzdsk_ipldisk/mzdsk_ipldisk.c
    mzdsk_ipldisk/mzdsk_ipldisk_tools.c
)

add_library(mzdsk_mrs STATIC
    mzdsk_mrs/mzdsk_mrs.c
)

add_library(mzdsk_cpm STATIC
    mzdsk_cpm/mzdsk_cpm.c
    mzdsk_cpm/mzdsk_cpm_mzf.c
)

add_library(mzdsk_hexdump STATIC
    mzdsk_hexdump/mzdsk_hexdump.c
)

add_library(mzdsk_detect STATIC
    mzdsk_detect/mzdsk_detect.c
)

add_library(output_format STATIC
    output_format/output_format.c
)

add_library(mzdsk_tools STATIC
    mzdsk_tools/mzdsk_tools.c
)
