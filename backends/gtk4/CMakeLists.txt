# Package: librapport-gtk4. Depends: librapport (exact version), GTK 4.
cmake_minimum_required(VERSION 3.25)
project(rapport-gtk4 VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT TARGET rapport::headers)
    find_package(rapport REQUIRED)
endif()

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
endif()
if(NOT GTK4_FOUND)
    if(RAPPORT_REQUIRE_BACKENDS OR PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "rapport-gtk4: gtk4 not found")
    endif()
    message(STATUS "rapport: skipping gtk4 backend (gtk4 not found)")
    return()
endif()

if(RAPPORT_STATIC)
    add_library(rapport_gtk4 STATIC gtk4_backend.cpp)
else()
    add_library(rapport_gtk4 MODULE gtk4_backend.cpp)
endif()
add_library(rapport::backend_gtk4 ALIAS rapport_gtk4)
# Linked into one executable, a backend needs the core after it on the link
# line (its strings, from rapport::i18n); a module finds it loaded already.
if(RAPPORT_STATIC)
    target_link_libraries(rapport_gtk4 PRIVATE rapport::rapport)
endif()
target_include_directories(rapport_gtk4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gtk-common
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(rapport_gtk4 PRIVATE rapport::headers PkgConfig::GTK4)
set_target_properties(rapport_gtk4 PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

install(TARGETS rapport_gtk4 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/rapport COMPONENT gtk4)
