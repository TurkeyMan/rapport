# Package: libanyui-qt6. Depends: libanyui (exact version), Qt 6 Widgets.
cmake_minimum_required(VERSION 3.25)
project(anyui-qt6 VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT TARGET anyui::headers)
    find_package(anyui REQUIRED)
endif()

find_package(Qt6 QUIET COMPONENTS Widgets)
if(NOT Qt6Widgets_FOUND)
    if(ANYUI_REQUIRE_BACKENDS OR PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "anyui-qt6: Qt 6 Widgets not found")
    endif()
    message(STATUS "anyui: skipping qt6 backend (Qt 6 Widgets not found)")
    return()
endif()

# No moc: signals connect to lambdas, and event handling is by overrides
# and event filters, so no class here declares Q_OBJECT.
if(ANYUI_STATIC)
    add_library(anyui_qt6 STATIC qt6_backend.cpp)
else()
    add_library(anyui_qt6 MODULE qt6_backend.cpp)
endif()
add_library(anyui::backend_qt6 ALIAS anyui_qt6)
target_link_libraries(anyui_qt6 PRIVATE anyui::headers Qt6::Widgets)
target_include_directories(anyui_qt6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set_target_properties(anyui_qt6 PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

install(TARGETS anyui_qt6 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/anyui COMPONENT qt6)
