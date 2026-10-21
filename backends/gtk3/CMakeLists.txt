# Package: libanyui-gtk3. Depends: libanyui (exact version), GTK 3.
cmake_minimum_required(VERSION 3.25)
project(anyui-gtk3 VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT TARGET anyui::headers)
    find_package(anyui REQUIRED)
endif()

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)
endif()
if(NOT GTK3_FOUND)
    if(ANYUI_REQUIRE_BACKENDS OR PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "anyui-gtk3: gtk+-3.0 not found")
    endif()
    message(STATUS "anyui: skipping gtk3 backend (gtk+-3.0 not found)")
    return()
endif()

add_library(anyui_gtk3 MODULE gtk3_backend.cpp)
target_include_directories(anyui_gtk3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gtk-common)
target_link_libraries(anyui_gtk3 PRIVATE anyui::headers PkgConfig::GTK3)
set_target_properties(anyui_gtk3 PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

install(TARGETS anyui_gtk3 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/anyui COMPONENT gtk3)
