# Package: librapport-shell-dev. File-manager extensions (context menus,
# icon badges): static libraries linked into each app's own extension module,
# which the file manager loads. Links no toolkit and not the rapport core.
cmake_minimum_required(VERSION 3.25)
project(rapport-shell VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

include(cmake/RapportShell.cmake)

# What every extension and its probe share; also what an app links to
# register its extension (rap::shell::install).
add_library(rapport_shell STATIC src/shell.cpp src/install.cpp src/resource_dir.cpp)
add_library(rapport::shell ALIAS rapport_shell)
target_include_directories(rapport_shell PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(rapport_shell PUBLIC cxx_std_23)
target_compile_options(rapport_shell PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /Zc:__cplusplus>)
set_target_properties(rapport_shell PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(rapport_shell PUBLIC ${CMAKE_DL_LIBS})

# The probe's main(): a provider's menu, printed.
add_library(rapport_shell_probe STATIC src/probe_main.cpp)
add_library(rapport::shell_probe ALIAS rapport_shell_probe)
target_link_libraries(rapport_shell_probe PUBLIC rapport_shell)

if(WIN32)
    add_library(rapport_shell_explorer STATIC src/win32/explorer_menu.cpp
        src/win32/explorer_badges.cpp)
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

# Linux: an adapter for each file manager whose extension SDK is installed,
# and the directory it loads extensions from, under the install prefix.
# Packagers set RAPPORT_REQUIRE_SHELL_EXTENSIONS to make a missing SDK an
# error rather than a skipped extension.
option(RAPPORT_REQUIRE_SHELL_EXTENSIONS "Fail if a file manager's extension SDK is missing" OFF)
if(RAPPORT_REQUIRE_SHELL_EXTENSIONS)
    set(skip_level FATAL_ERROR)
else()
    set(skip_level STATUS)
endif()
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    set(file_managers)
    if(PKG_CONFIG_FOUND OR RAPPORT_REQUIRE_SHELL_EXTENSIONS)
        add_library(rapport_shell_linux STATIC src/linux/adapter.cpp)
        target_link_libraries(rapport_shell_linux PUBLIC rapport_shell)
        set_target_properties(rapport_shell_linux PROPERTIES
            POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
        # file manager, its SDK's pkg-config module, and the variable naming
        # its extension directory
        foreach(spec nautilus:libnautilus-extension-4:extensiondir
                     nemo:libnemo-extension:extensiondir
                     caja:libcaja-extension:extensiondir
                     thunar:thunarx-3:extensionsdir)
            string(REPLACE ":" ";" spec ${spec})
            list(GET spec 0 fm)
            list(GET spec 1 module)
            list(GET spec 2 variable)
            pkg_check_modules(RAPPORT_SHELL_SDK_${fm} QUIET IMPORTED_TARGET GLOBAL ${module})
            if(NOT RAPPORT_SHELL_SDK_${fm}_FOUND)
                message(${skip_level} "rapport: skipping the ${fm} extension (${module} not found)")
                continue()
            endif()
            execute_process(
                COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=${variable}
                        --define-variable=prefix=${CMAKE_INSTALL_PREFIX} ${module}
                OUTPUT_VARIABLE dir OUTPUT_STRIP_TRAILING_WHITESPACE)
            cmake_path(IS_PREFIX CMAKE_INSTALL_PREFIX "${dir}" NORMALIZE inside)
            if(inside)
                cmake_path(RELATIVE_PATH dir BASE_DIRECTORY ${CMAKE_INSTALL_PREFIX})
            endif()
            set(RAPPORT_SHELL_DIR_${fm} ${dir} CACHE INTERNAL "")
            add_library(rapport_shell_${fm} STATIC src/linux/${fm}.cpp)
            add_library(rapport::shell_${fm} ALIAS rapport_shell_${fm})
            target_link_libraries(rapport_shell_${fm} PUBLIC rapport_shell_linux
                                                             PkgConfig::RAPPORT_SHELL_SDK_${fm})
            set_target_properties(rapport_shell_${fm} PROPERTIES
                POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
            list(APPEND file_managers ${fm})
        endforeach()
    endif()
    # Dolphin's plugin is compiled into each extension (its metadata names
    # the app), so only its toolkit is found here.
    find_package(KF6KIO QUIET)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(KF6KIO_FOUND AND Qt6Widgets_FOUND)
        set(RAPPORT_SHELL_DIR_dolphin ${QT6_INSTALL_PLUGINS}/kf6/kfileitemaction CACHE INTERNAL "")
        set(RAPPORT_SHELL_DOLPHIN_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/linux/dolphin.cpp
            CACHE INTERNAL "")
        list(APPEND file_managers dolphin)
    else()
        message(${skip_level} "rapport: skipping the dolphin extension (KF6 KIO not found)")
    endif()
    set(RAPPORT_SHELL_FILE_MANAGERS ${file_managers} CACHE INTERNAL "")
    set(RAPPORT_SHELL_EXTENSION_MAP ${CMAKE_CURRENT_SOURCE_DIR}/src/linux/extension.map
        CACHE INTERNAL "")
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
