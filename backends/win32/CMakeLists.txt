# Package: librapport-win32 (Windows only). Native controls, Direct2D canvases.
cmake_minimum_required(VERSION 3.25)
project(rapport-win32 LANGUAGES CXX)

if(NOT WIN32)
    message(STATUS "rapport: skipping win32 backend (not a Windows build)")
    return()
endif()
if(NOT TARGET rapport::headers)
    find_package(rapport REQUIRED)
endif()

set(sources win32_backend.cpp win32_painter.cpp)
if(RAPPORT_STATIC)
    add_library(rapport_win32 STATIC ${sources})
else()
    add_library(rapport_win32 MODULE ${sources})
endif()
add_library(rapport::backend_win32 ALIAS rapport_win32)
# Linked into one executable, a backend needs the core after it on the link
# line (its strings, from rapport::i18n). A module finds it loaded already,
# except on Windows, where a DLL resolves every symbol when it's linked.
if(RAPPORT_STATIC OR WIN32)
    target_link_libraries(rapport_win32 PRIVATE rapport::rapport)
endif()
target_link_libraries(rapport_win32 PRIVATE rapport::headers)
target_include_directories(rapport_win32 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(rapport_win32 PUBLIC user32 gdi32 comctl32 comdlg32 winspool d2d1 d3d11 dxgi dcomp dwrite windowscodecs ole32 uuid dwmapi shell32 uxtheme)
target_compile_definitions(rapport_win32 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
set_target_properties(rapport_win32 PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
