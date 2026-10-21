# Package: libanyui-gtk4. Depends: libanyui (exact version), GTK 4.
cmake_minimum_required(VERSION 3.25)
project(anyui-gtk4 VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT TARGET anyui::headers)
    find_package(anyui REQUIRED)
endif()

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
endif()
if(NOT GTK4_FOUND)
    if(ANYUI_REQUIRE_BACKENDS OR PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "anyui-gtk4: gtk4 not found")
    endif()
    message(STATUS "anyui: skipping gtk4 backend (gtk4 not found)")
    return()
endif()

if(ANYUI_STATIC)
    add_library(anyui_gtk4 STATIC gtk4_backend.cpp)
else()
    add_library(anyui_gtk4 MODULE gtk4_backend.cpp)
endif()
add_library(anyui::backend_gtk4 ALIAS anyui_gtk4)
target_include_directories(anyui_gtk4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gtk-common)
target_link_libraries(anyui_gtk4 PRIVATE anyui::headers PkgConfig::GTK4)
set_target_properties(anyui_gtk4 PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

install(TARGETS anyui_gtk4 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/anyui COMPONENT gtk4)
