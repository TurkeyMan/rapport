# Package: librapport-qt6. Depends: librapport (exact version), Qt 6 Widgets.
cmake_minimum_required(VERSION 3.25)
project(rapport-qt6 VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT TARGET rapport::headers)
    find_package(rapport REQUIRED)
endif()

find_package(Qt6 QUIET COMPONENTS Widgets)
if(NOT Qt6Widgets_FOUND)
    if(RAPPORT_REQUIRE_BACKENDS OR PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "rapport-qt6: Qt 6 Widgets not found")
    endif()
    message(STATUS "rapport: skipping qt6 backend (Qt 6 Widgets not found)")
    return()
endif()

# No moc: signals connect to lambdas, and event handling is by overrides
# and event filters, so no class here declares Q_OBJECT.
if(RAPPORT_STATIC)
    add_library(rapport_qt6 STATIC qt6_backend.cpp)
else()
    add_library(rapport_qt6 MODULE qt6_backend.cpp)
endif()
add_library(rapport::backend_qt6 ALIAS rapport_qt6)
# Linked into one executable, a backend needs the core after it on the link
# line (its strings, from rapport::i18n). A module finds it loaded already,
# except on Windows, where a DLL resolves every symbol when it's linked.
if(RAPPORT_STATIC OR WIN32)
    target_link_libraries(rapport_qt6 PRIVATE rapport::rapport)
endif()
target_link_libraries(rapport_qt6 PRIVATE rapport::headers Qt6::Widgets)
# Printing, where Qt's print support is installed (it comes with qtbase).
find_package(Qt6 QUIET COMPONENTS PrintSupport)
if(Qt6PrintSupport_FOUND)
    target_link_libraries(rapport_qt6 PRIVATE Qt6::PrintSupport)
    target_compile_definitions(rapport_qt6 PRIVATE RAPPORT_QT_PRINT=1)
endif()
target_include_directories(rapport_qt6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set_target_properties(rapport_qt6 PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

install(TARGETS rapport_qt6 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/rapport COMPONENT qt6)
