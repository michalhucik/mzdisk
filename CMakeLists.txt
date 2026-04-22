cmake_minimum_required(VERSION 3.16)

# MSYS2/MinGW: GCC linker (collect2) vyzaduje zapisovatelny TEMP adresar.
# cc.exe je nativni Windows binary - pouziva systemovy TEMP z Windows registru,
# ktery nemusi byt zapisovatelny. MSYS2 cesta "/tmp" se nepropaguje.
#
# Reseni: buildujte pres "cmake --build build --preset default"
# nebo rucne: TEMP=$(cygpath -w /tmp) TMP=$(cygpath -w /tmp) make -j$(nproc)

option(BUILD_GUI "Build mzdisk GUI application (SDL3 + ImGui)" OFF)

if(BUILD_GUI)
    project(mzdisk C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    project(mzdisk C)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Varovani + -pipe (vyhne se TEMP problemum na Windows)
add_compile_options(-Wall -Wextra -Wpedantic -pipe)
add_link_options(-pipe)

# Standalone build - bez baseui z mz800emu
add_compile_definitions(GENERIC_DRIVER_NO_BASEUI)
add_compile_definitions(GENERIC_DRIVER_FILE_CB)
add_compile_definitions(GENERIC_DRIVER_MEMORY_CB)

# Include path - "libs/..." cesty vyzaduji src/ jako koren
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# ===========================================================================
#  Knihovny - kopie z tmz-format
# ===========================================================================

# generic_driver
add_library(generic_driver STATIC
    src/libs/generic_driver/generic_driver.c
    src/libs/generic_driver/memory_driver.c
)

# endianity
add_library(endianity STATIC
    src/libs/endianity/endianity.c
)

# sharpmz_ascii
add_library(sharpmz_ascii STATIC
    src/libs/sharpmz_ascii/sharpmz_ascii.c
    src/libs/sharpmz_ascii/sharpmz_utf8.c
)

# mzf
add_library(mzf STATIC
    src/libs/mzf/mzf.c
    src/libs/mzf/mzf_tools.c
)
target_link_libraries(mzf PRIVATE generic_driver endianity sharpmz_ascii)

# ===========================================================================
#  Knihovny - kopie z mz800new
# ===========================================================================

# dsk
add_library(dsk STATIC
    src/libs/dsk/dsk.c
    src/libs/dsk/dsk_tools.c
)
target_link_libraries(dsk PRIVATE generic_driver)

# ===========================================================================
#  Nove knihovny - mzdsk_*
# ===========================================================================

# mzdsk_global
add_library(mzdsk_global STATIC
    src/libs/mzdsk_global/mzdsk_global.c
)
target_link_libraries(mzdsk_global PRIVATE dsk generic_driver)

# mzdsk_ipldisk
add_library(mzdsk_ipldisk STATIC
    src/libs/mzdsk_ipldisk/mzdsk_ipldisk.c
    src/libs/mzdsk_ipldisk/mzdsk_ipldisk_tools.c
)
target_link_libraries(mzdsk_ipldisk PRIVATE mzdsk_global dsk endianity sharpmz_ascii mzf generic_driver)

# mzdsk_mrs
add_library(mzdsk_mrs STATIC
    src/libs/mzdsk_mrs/mzdsk_mrs.c
)
target_link_libraries(mzdsk_mrs PRIVATE mzdsk_global dsk generic_driver)

# mzdsk_cpm
add_library(mzdsk_cpm STATIC
    src/libs/mzdsk_cpm/mzdsk_cpm.c
    src/libs/mzdsk_cpm/mzdsk_cpm_mzf.c
)
target_link_libraries(mzdsk_cpm PRIVATE mzdsk_global dsk mzf generic_driver)

# mzdsk_hexdump
add_library(mzdsk_hexdump STATIC
    src/libs/mzdsk_hexdump/mzdsk_hexdump.c
)
target_link_libraries(mzdsk_hexdump PRIVATE sharpmz_ascii)

# mzdsk_detect
add_library(mzdsk_detect STATIC
    src/libs/mzdsk_detect/mzdsk_detect.c
)
target_link_libraries(mzdsk_detect PRIVATE mzdsk_cpm mzdsk_mrs mzdsk_global dsk generic_driver)

# output_format
add_library(output_format STATIC
    src/libs/output_format/output_format.c
)

# mzdsk_tools
add_library(mzdsk_tools STATIC
    src/libs/mzdsk_tools/mzdsk_tools.c
)
target_link_libraries(mzdsk_tools PRIVATE dsk mzdsk_global mzdsk_ipldisk generic_driver)

# ===========================================================================
#  Nastroje (CLI) — jen pokud NEbuildujeme GUI
# ===========================================================================

if(NOT BUILD_GUI)

# mzdsk-create
add_executable(mzdsk-create
    src/tools/mzdsk-create/mzdsk_create.c
)
target_link_libraries(mzdsk-create PRIVATE mzdsk_tools mzdsk_cpm mzdsk_mrs mzdsk_ipldisk mzdsk_global dsk generic_driver)

# mzdsk-info
add_executable(mzdsk-info
    src/tools/mzdsk-info/mzdsk_info.c
)
target_link_libraries(mzdsk-info PRIVATE mzdsk_detect mzdsk_cpm mzdsk_mrs mzdsk_ipldisk mzdsk_global mzdsk_hexdump output_format dsk endianity sharpmz_ascii generic_driver)

# mzdsk-raw
add_executable(mzdsk-raw
    src/tools/mzdsk-raw/mzdsk_raw.c
)
target_link_libraries(mzdsk-raw PRIVATE
    mzdsk_global
    mzdsk_ipldisk
    mzdsk_tools
    dsk
    sharpmz_ascii
    endianity
    generic_driver
)

# mzdsk-fsmz
add_executable(mzdsk-fsmz
    src/tools/mzdsk-fsmz/mzdsk_fsmz.c
)
target_link_libraries(mzdsk-fsmz PRIVATE
    mzdsk_ipldisk
    mzdsk_global
    mzdsk_hexdump
    output_format
    mzf
    dsk
    sharpmz_ascii
    endianity
    generic_driver
)

# mzdsk-cpm
add_executable(mzdsk-cpm
    src/tools/mzdsk-cpm/mzdsk_cpm.c
)
target_link_libraries(mzdsk-cpm PRIVATE
    mzdsk_cpm
    mzdsk_global
    mzdsk_hexdump
    output_format
    mzf
    dsk
    endianity
    generic_driver
)

# mzdsk-mrs
add_executable(mzdsk-mrs
    src/tools/mzdsk-mrs/mzdsk_mrs.c
)
target_link_libraries(mzdsk-mrs PRIVATE
    mzdsk_mrs
    mzdsk_global
    mzdsk_hexdump
    output_format
    mzf
    dsk
    endianity
    sharpmz_ascii
    generic_driver
)

# mzdsk-dsk
add_executable(mzdsk-dsk
    src/tools/mzdsk-dsk/mzdsk_dsk.c
)
target_link_libraries(mzdsk-dsk PRIVATE
    dsk
    generic_driver
)

endif() # NOT BUILD_GUI

# ===========================================================================
#  GUI aplikace (BUILD_GUI=ON)
# ===========================================================================

if(BUILD_GUI)
    # binárka mzdisk.exe přímo do kořene build adresáře
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_subdirectory(src/mzdisk)
endif()
