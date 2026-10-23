# Package: librapport-shell-dev. File-manager extensions (context menus,
# icon badges): static libraries linked into each app's own extension module,
# which the file manager loads. Links no toolkit and not the rapport core.
cmake_minimum_required(VERSION 3.25)
project(rapport-shell VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

include(cmake/RapportShell.cmake)

# What every extension and its probe share; also what an app links to
# register its extension (rap::shell::install).
add_library(rapport_shell STATIC src/shell.cpp src/install.cpp)
add_library(rapport::shell ALIAS rapport_shell)
target_include_directories(rapport_shell PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(rapport_shell PUBLIC cxx_std_23)
set_target_properties(rapport_shell PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(rapport_shell PUBLIC ${CMAKE_DL_LIBS})

# The probe's main(): a provider's menu, printed.
add_library(rapport_shell_probe STATIC src/probe_main.cpp)
add_library(rapport::shell_probe ALIAS rapport_shell_probe)
target_link_libraries(rapport_shell_probe PUBLIC rapport_shell)

if(WIN32)
    add_library(rapport_shell_explorer STATIC src/win32/explorer_menu.cpp)
    add_library(rapport::shell_explorer ALIAS rapport_shell_explorer)
    target_link_libraries(rapport_shell_explorer PUBLIC rapport_shell ole32 shell32 shlwapi uuid
                                                        windowscodecs advapi32)
    set(RAPPORT_SHELL_EXPLORER_DEF ${CMAKE_CURRENT_SOURCE_DIR}/src/win32/explorer.def
        CACHE INTERNAL "")

    add_executable(rapport-explorer-probe tools/explorer_probe.cpp)
    target_link_libraries(rapport-explorer-probe PRIVATE ole32 shell32 uuid)
    if(MINGW)
        target_link_options(rapport-explorer-probe PRIVATE -municode)
    endif()
    rapport_shell_static_runtime(rapport-explorer-probe)
endif()

install(TARGETS rapport_shell rapport_shell_probe EXPORT rapportShellTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev)
install(FILES include/rapport/shell.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rapport COMPONENT dev)
install(FILES cmake/RapportShell.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rapport COMPONENT dev)

if(MSVC)
    foreach(lib rapport_shell rapport_shell_probe rapport_shell_explorer)
        if(TARGET ${lib})
            rapport_shell_static_runtime(${lib})
        endif()
    endforeach()
endif()
