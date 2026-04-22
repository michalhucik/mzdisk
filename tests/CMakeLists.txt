# ===========================================================================
#  Automatizované testy mzdsk projektu
#
#  Adresářová struktura:
#    tests/libs/  - testy knihoven z src/libs/
#    tests/gui/   - testy GUI logiky z src/mzdisk/ (bez ImGui/SDL3)
#    tests/cli/   - CLI integrační testy (bash skripty)
#
#  C testy se kompilují do build-tests/ a linkují .a z build-libs/.
# ===========================================================================

cmake_minimum_required(VERSION 3.16)
project(mzdsk-tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -pipe)

add_compile_definitions(GENERIC_DRIVER_NO_BASEUI)
add_compile_definitions(GENERIC_DRIVER_FILE_CB)
add_compile_definitions(GENERIC_DRIVER_MEMORY_CB)

# Include cesty:
#   src/          - pro "libs/dsk/dsk.h" atd.
#   src/mzdisk/   - pro "config.h", "panels/..." (GUI testy)
#   tests/        - pro "test_framework.h"
set(MZDSK_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)
include_directories(${MZDSK_SRC_DIR})
include_directories(${MZDSK_SRC_DIR}/mzdisk)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Předkompilované knihovny z build-libs/
set(MZDSK_LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../build-libs)

set(MZDSK_ALL_LIBS
    -Wl,--start-group
    ${MZDSK_LIBS_DIR}/libmzdsk_detect.a
    ${MZDSK_LIBS_DIR}/libmzdsk_global.a
    ${MZDSK_LIBS_DIR}/libmzdsk_ipldisk.a
    ${MZDSK_LIBS_DIR}/libmzdsk_cpm.a
    ${MZDSK_LIBS_DIR}/libmzdsk_mrs.a
    ${MZDSK_LIBS_DIR}/libmzdsk_hexdump.a
    ${MZDSK_LIBS_DIR}/libmzdsk_tools.a
    ${MZDSK_LIBS_DIR}/liboutput_format.a
    ${MZDSK_LIBS_DIR}/libdsk.a
    ${MZDSK_LIBS_DIR}/libmzf.a
    ${MZDSK_LIBS_DIR}/libgeneric_driver.a
    ${MZDSK_LIBS_DIR}/libendianity.a
    ${MZDSK_LIBS_DIR}/libsharpmz_ascii.a
    -Wl,--end-group
)

# Na Windows (MSYS2/MinGW) je gettext v separátní libintl.dll,
# na Linuxu je součástí glibc - není potřeba explicitně linkovat.
if(WIN32)
    list(APPEND MZDSK_ALL_LIBS intl)
endif()

# memory_driver.c - standalone verze s realloc podporou
set(MEMORY_DRIVER_SRC ${MZDSK_SRC_DIR}/libs/generic_driver/memory_driver.c)

# GUI zdrojáky
set(MZDSK_GUI_SRC_DIR ${MZDSK_SRC_DIR}/mzdisk)


# =====================================================================
#  Knihovní testy (tests/libs/)
# =====================================================================

add_executable(test_dsk_tools libs/test_dsk_tools.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_dsk_tools ${MZDSK_ALL_LIBS})

add_executable(test_memory_driver libs/test_memory_driver.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_memory_driver ${MZDSK_ALL_LIBS})

add_executable(test_detect libs/test_detect.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_detect ${MZDSK_ALL_LIBS})

add_executable(test_cpm libs/test_cpm.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_cpm ${MZDSK_ALL_LIBS})

add_executable(test_mrs libs/test_mrs.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_mrs ${MZDSK_ALL_LIBS})

add_executable(test_fsmz libs/test_fsmz.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_fsmz ${MZDSK_ALL_LIBS})

add_executable(test_sharpmz_ascii libs/test_sharpmz_ascii.c)
target_link_libraries(test_sharpmz_ascii ${MZDSK_ALL_LIBS})

add_executable(test_mzf libs/test_mzf.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_mzf ${MZDSK_ALL_LIBS})

add_executable(test_endianity libs/test_endianity.c)
target_link_libraries(test_endianity ${MZDSK_ALL_LIBS})

add_executable(test_output_format libs/test_output_format.c)
target_link_libraries(test_output_format ${MZDSK_ALL_LIBS})

add_executable(test_mzdsk_global libs/test_mzdsk_global.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_mzdsk_global ${MZDSK_ALL_LIBS})

add_executable(test_mzdsk_tools libs/test_mzdsk_tools.c ${MEMORY_DRIVER_SRC})
target_link_libraries(test_mzdsk_tools ${MZDSK_ALL_LIBS})

add_executable(test_mzdsk_hexdump libs/test_mzdsk_hexdump.c)
target_link_libraries(test_mzdsk_hexdump ${MZDSK_ALL_LIBS})


# =====================================================================
#  GUI testy (tests/gui/) - panel .c soubory bez ImGui závislostí
# =====================================================================

add_executable(test_gui_config gui/test_config.c ${MZDSK_GUI_SRC_DIR}/config.c)
target_link_libraries(test_gui_config ${MZDSK_ALL_LIBS})

add_executable(test_gui_panel_geometry gui/test_panel_geometry.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_geometry.c
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_panel_geometry ${MZDSK_ALL_LIBS})

add_executable(test_gui_panel_create gui/test_panel_create.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_create.c
    ${MZDSK_GUI_SRC_DIR}/config.c
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_panel_create ${MZDSK_ALL_LIBS})

add_executable(test_gui_panel_boot gui/test_panel_boot.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_boot.c
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_panel_boot ${MZDSK_ALL_LIBS})

add_executable(test_gui_panel_hexdump gui/test_panel_hexdump.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_hexdump.c
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_panel_hexdump ${MZDSK_ALL_LIBS})

add_executable(test_gui_panel_fsmz gui/test_panel_fsmz.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_fsmz.c
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_panel_fsmz ${MZDSK_ALL_LIBS})

add_executable(test_gui_panel_cpm gui/test_panel_cpm.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_cpm.c
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_panel_cpm ${MZDSK_ALL_LIBS})

add_executable(test_gui_panel_mrs gui/test_panel_mrs.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_mrs.c
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_panel_mrs ${MZDSK_ALL_LIBS})

add_executable(test_gui_panel_raw_io gui/test_panel_raw_io.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_raw_io.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_hexdump.c
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_panel_raw_io ${MZDSK_ALL_LIBS})

# Session test - vyžaduje kompilaci všech panel .c souborů
set(MZDSK_ALL_PANELS
    ${MZDSK_GUI_SRC_DIR}/disk_session.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_info.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_map.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_hexdump.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_geometry.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_boot.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_fsmz.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_cpm.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_mrs.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_raw_io.c
    ${MZDSK_GUI_SRC_DIR}/panels/panel_settings.c
)

add_executable(test_gui_session gui/test_session.c
    ${MZDSK_ALL_PANELS}
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_session ${MZDSK_ALL_LIBS})

add_executable(test_gui_dragdrop gui/test_dragdrop.c
    ${MZDSK_GUI_SRC_DIR}/dragdrop.c
    ${MZDSK_ALL_PANELS}
    ${MEMORY_DRIVER_SRC})
target_link_libraries(test_gui_dragdrop ${MZDSK_ALL_LIBS})
