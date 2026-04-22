# ===========================================================================
#  CLI nástroje mzdsk-*
#
#  Linkují proti předkompilovaným knihovnám z build-libs/.
#  Volá se přes "make cli" (závisí na "make libs").
# ===========================================================================

cmake_minimum_required(VERSION 3.16)
project(mzdsk-tools C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -pipe)
add_link_options(-pipe)

add_compile_definitions(GENERIC_DRIVER_NO_BASEUI)
add_compile_definitions(GENERIC_DRIVER_FILE_CB)
add_compile_definitions(GENERIC_DRIVER_MEMORY_CB)

# Include path pro hlavičky knihoven
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

# Binárky přímo do kořene build adresáře
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Cesta k předkompilovaným .a souborům
set(LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../build-libs)

# Všechny mzdsk knihovny jako link group (řeší vzájemné závislosti statických .a)
set(MZDSK_ALL_LIBS
    -Wl,--start-group
    ${LIBS_DIR}/libmzdsk_detect.a
    ${LIBS_DIR}/libmzdsk_cpm.a
    ${LIBS_DIR}/libmzdsk_mrs.a
    ${LIBS_DIR}/libmzdsk_ipldisk.a
    ${LIBS_DIR}/libmzdsk_global.a
    ${LIBS_DIR}/libmzdsk_hexdump.a
    ${LIBS_DIR}/libmzdsk_tools.a
    ${LIBS_DIR}/liboutput_format.a
    ${LIBS_DIR}/libdsk.a
    ${LIBS_DIR}/libmzf.a
    ${LIBS_DIR}/libendianity.a
    ${LIBS_DIR}/libsharpmz_ascii.a
    ${LIBS_DIR}/libgeneric_driver.a
    -Wl,--end-group
)

# --- CLI nástroje ---

add_executable(mzdsk-create mzdsk-create/mzdsk_create.c)
target_link_libraries(mzdsk-create PRIVATE ${MZDSK_ALL_LIBS})

add_executable(mzdsk-info mzdsk-info/mzdsk_info.c)
target_link_libraries(mzdsk-info PRIVATE ${MZDSK_ALL_LIBS})

add_executable(mzdsk-raw mzdsk-raw/mzdsk_raw.c)
target_link_libraries(mzdsk-raw PRIVATE ${MZDSK_ALL_LIBS})

add_executable(mzdsk-fsmz mzdsk-fsmz/mzdsk_fsmz.c)
target_link_libraries(mzdsk-fsmz PRIVATE ${MZDSK_ALL_LIBS})

add_executable(mzdsk-cpm mzdsk-cpm/mzdsk_cpm.c)
target_link_libraries(mzdsk-cpm PRIVATE ${MZDSK_ALL_LIBS})

add_executable(mzdsk-mrs mzdsk-mrs/mzdsk_mrs.c)
target_link_libraries(mzdsk-mrs PRIVATE ${MZDSK_ALL_LIBS})

add_executable(mzdsk-dsk mzdsk-dsk/mzdsk_dsk.c)
target_link_libraries(mzdsk-dsk PRIVATE ${MZDSK_ALL_LIBS})
