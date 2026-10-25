# Package: librapport-gtk3. Depends: librapport (exact version), GTK 3.
cmake_minimum_required(VERSION 3.25)
project(rapport-gtk3 VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT TARGET rapport::headers)
    find_package(rapport REQUIRED)
endif()

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)
endif()
if(NOT GTK3_FOUND)
    if(RAPPORT_REQUIRE_BACKENDS OR PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "rapport-gtk3: gtk+-3.0 not found")
    endif()
    message(STATUS "rapport: skipping gtk3 backend (gtk+-3.0 not found)")
    return()
endif()

if(RAPPORT_STATIC)
    add_library(rapport_gtk3 STATIC gtk3_backend.cpp)
else()
    add_library(rapport_gtk3 MODULE gtk3_backend.cpp)
endif()
add_library(rapport::backend_gtk3 ALIAS rapport_gtk3)
# Linked into one executable, a backend needs the core after it on the link
# line (its strings, from rapport::i18n). A module finds it loaded already,
# except on Windows, where a DLL resolves every symbol when it's linked.
if(RAPPORT_STATIC OR WIN32)
    target_link_libraries(rapport_gtk3 PRIVATE rapport::rapport)
endif()
target_include_directories(rapport_gtk3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gtk-common
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(rapport_gtk3 PRIVATE rapport::headers PkgConfig::GTK3)
set_target_properties(rapport_gtk3 PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# The true fractional scale on Wayland (GTK 3 knows only whole ones), from
# the compositor's fractional-scale protocol. Optional: without its pieces
# GTK 3 reports the whole scale, as GTK 3 itself does; packages require them.
pkg_check_modules(GTK3_WAYLAND QUIET IMPORTED_TARGET gtk+-wayland-3.0 wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner)
set(fractional_xml ${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml)
set(missing)
if(NOT GTK3_WAYLAND_FOUND)
    list(APPEND missing "gtk+-wayland-3.0/wayland-client")
endif()
if(NOT WAYLAND_PROTOCOLS_FOUND OR NOT EXISTS ${fractional_xml})
    list(APPEND missing "wayland-protocols")
endif()
if(NOT WAYLAND_SCANNER)
    list(APPEND missing "wayland-scanner")
endif()
if(missing)
    list(JOIN missing ", " missing)
    message(STATUS "rapport: GTK 3 fractional scale: off (missing ${missing})")
else()
    message(STATUS "rapport: GTK 3 fractional scale: on")
    enable_language(C)
    set(gen ${CMAKE_CURRENT_BINARY_DIR}/protocols)
    file(MAKE_DIRECTORY ${gen})
    add_custom_command(
        OUTPUT ${gen}/fractional-scale-v1-client-protocol.h ${gen}/fractional-scale-v1-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${fractional_xml} ${gen}/fractional-scale-v1-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${fractional_xml} ${gen}/fractional-scale-v1-protocol.c
        DEPENDS ${fractional_xml})
    target_sources(rapport_gtk3 PRIVATE ${gen}/fractional-scale-v1-client-protocol.h
                                        ${gen}/fractional-scale-v1-protocol.c)
    target_include_directories(rapport_gtk3 PRIVATE ${gen})
    target_compile_definitions(rapport_gtk3 PRIVATE RAPPORT_GTK3_FRACTIONAL=1)
    target_link_libraries(rapport_gtk3 PRIVATE PkgConfig::GTK3_WAYLAND)
endif()

install(TARGETS rapport_gtk3 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/rapport COMPONENT gtk3)
