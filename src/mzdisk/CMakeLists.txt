# ===========================================================================
#  mzdisk GUI - SDL3 + OpenGL + ImGui (docking + multi-viewport)
#
#  Linkuje proti předkompilovaným mzdsk knihovnám z build-libs/.
#  Volá se přes "cmake --preset gui".
# ===========================================================================

cmake_minimum_required(VERSION 3.16)
project(mzdisk-gui C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -pipe)
add_link_options(-pipe)

add_compile_definitions(GENERIC_DRIVER_NO_BASEUI)
add_compile_definitions(GENERIC_DRIVER_FILE_CB)
add_compile_definitions(GENERIC_DRIVER_MEMORY_CB)

# Include path pro hlavičky mzdsk knihoven
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

# Cesta k předkompilovaným mzdsk .a souborům
set(MZDSK_LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../build-libs)

# Všechny mzdsk knihovny jako link group (řeší vzájemné závislosti statických .a)
set(MZDSK_ALL_LIBS
    -Wl,--start-group
    ${MZDSK_LIBS_DIR}/libmzdsk_detect.a
    ${MZDSK_LIBS_DIR}/libmzdsk_cpm.a
    ${MZDSK_LIBS_DIR}/libmzdsk_mrs.a
    ${MZDSK_LIBS_DIR}/libmzdsk_ipldisk.a
    ${MZDSK_LIBS_DIR}/libmzdsk_global.a
    ${MZDSK_LIBS_DIR}/libmzdsk_hexdump.a
    ${MZDSK_LIBS_DIR}/libmzdsk_tools.a
    ${MZDSK_LIBS_DIR}/liboutput_format.a
    ${MZDSK_LIBS_DIR}/libdsk.a
    ${MZDSK_LIBS_DIR}/libmzf.a
    ${MZDSK_LIBS_DIR}/libendianity.a
    ${MZDSK_LIBS_DIR}/libsharpmz_ascii.a
    ${MZDSK_LIBS_DIR}/libgeneric_driver.a
    -Wl,--end-group
)

# Binárka přímo do kořene build adresáře
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# --- Závislosti ---

find_package(SDL3 REQUIRED)
find_package(OpenGL REQUIRED)

find_library(INTL_LIBRARY intl)
if(NOT INTL_LIBRARY)
    message(WARNING "libintl not found, i18n will not work")
    set(INTL_LIBRARY "")
endif()

# --- ImGui (statická knihovna) ---

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3 OpenGL::GL)
target_compile_options(imgui PRIVATE -w)

# --- ImGuiFileDialog (statická knihovna) ---

set(IGFD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/igfd)

add_library(igfd STATIC
    ${IGFD_DIR}/ImGuiFileDialog.cpp
)
target_include_directories(igfd PUBLIC ${IGFD_DIR})
target_include_directories(igfd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(igfd PUBLIC imgui)
target_compile_options(igfd PRIVATE -w)

# --- ImWidgets (statická knihovna) ---

set(IMWIDGETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/imwidgets)

add_library(imwidgets STATIC
    ${IMWIDGETS_DIR}/ImWidgets.cpp
)
target_include_directories(imwidgets PUBLIC ${IMWIDGETS_DIR})
target_include_directories(imwidgets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(imwidgets PUBLIC imgui)
target_compile_options(imwidgets PRIVATE -w)

# --- mzdisk GUI binárka ---

# --- Windows ikona (.rc -> .o přes windres) ---

if(WIN32)
    enable_language(RC)
    set(MZDISK_RC mzdisk.rc)
endif()

add_executable(mzdisk
    main.c
    ${MZDISK_RC}
    disk_session.c
    panels/panel_info.c
    panels/panel_info_imgui.cpp
    panels/panel_map.c
    panels/panel_map_imgui.cpp
    panels/panel_hexdump.c
    panels/panel_hexdump_imgui.cpp
    panels/panel_fsmz.c
    panels/panel_fsmz_imgui.cpp
    panels/panel_cpm.c
    panels/panel_cpm_imgui.cpp
    panels/panel_mrs.c
    panels/panel_mrs_imgui.cpp
    panels/panel_geometry.c
    panels/panel_geometry_imgui.cpp
    panels/panel_boot.c
    panels/panel_boot_imgui.cpp
    panels/panel_create.c
    panels/panel_create_imgui.cpp
    panels/panel_raw_io.c
    panels/panel_raw_io_imgui.cpp
    panels/panel_settings.c
    panels/panel_settings_imgui.cpp
    config.c
    dragdrop.c
    texture_utils.cpp
    app_imgui.cpp
    libs/mzglyphs/mzglyphs.c
    libs/mz_vcode/mz_vcode.c
)

target_include_directories(mzdisk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui/backends
)

target_link_libraries(mzdisk PRIVATE
    imgui
    igfd
    imwidgets
    # mzdsk knihovny (předkompilované v build-libs/)
    ${MZDSK_ALL_LIBS}
    # systémové
    SDL3::SDL3
    OpenGL::GL
    ${INTL_LIBRARY}
)

if(WIN32)
    target_link_options(mzdisk PRIVATE -mwindows)
endif()
