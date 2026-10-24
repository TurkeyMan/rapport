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
# line (its strings, from rapport::i18n); a module finds it loaded already.
if(RAPPORT_STATIC)
    target_link_libraries(rapport_gtk3 PRIVATE rapport::rapport)
endif()
target_include_directories(rapport_gtk3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gtk-common
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(rapport_gtk3 PRIVATE rapport::headers PkgConfig::GTK3)
set_target_properties(rapport_gtk3 PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

install(TARGETS rapport_gtk3 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/rapport COMPONENT gtk3)
