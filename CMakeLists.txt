# rapport umbrella build: core plus every backend whose toolkit is found.
#
# The tree mirrors packaging. Each directory is one package and also
# builds standalone against an installed rapport (distro split builds):
#   core/             librapport, librapport-dev  (Recommends the backends)
#   backends/gtk3/    librapport-gtk3             (Depends: librapport, GTK 3)
#   backends/gtk4/    librapport-gtk4             (Depends: librapport, GTK 4)
#   backends/qt6/     librapport-qt6              (Depends: librapport, Qt 6 Widgets)
#   backends/null/    not packaged; headless backend for tests
#   backends/gtk-common/  shared source compiled into both GTK backends
#   backends/common/      shared source for any backend (drawn icons)
#   shell/            librapport-shell-dev        (file-manager extensions; no toolkit)
cmake_minimum_required(VERSION 3.25)
project(rapport VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

option(RAPPORT_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
# Static: the core and each backend become static libraries
# (rapport::rapport, rapport::backend_<name>) to link into one executable, which
# registers its backends with RAPPORT_BUILTIN_BACKEND. Default: shared core,
# backends as dlopened plugins.
option(RAPPORT_STATIC "Build static libraries instead of a shared core and plugins" OFF)
option(RAPPORT_TESTS "Build the core's tests" ${PROJECT_IS_TOP_LEVEL})
# A plugin build passes std:: types (std::function, std::string,
# std::vector, ...) between the app, the core and each backend DLL, one
# module freeing what another allocated: with MSVC they must all share the
# one C++ runtime DLL (/MD). The static runtime (/MT) is for RAPPORT_STATIC,
# one executable with one runtime in it (#136). The shell extension
# libraries set their own static runtime: they never link the core.
if(MSVC AND NOT RAPPORT_STATIC)
    if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
        message(FATAL_ERROR "rapport: a plugin build (RAPPORT_STATIC=OFF) needs the shared C++ "
                            "runtime, but CMAKE_MSVC_RUNTIME_LIBRARY is "
                            "'${CMAKE_MSVC_RUNTIME_LIBRARY}'. std:: types cross the plugin DLLs, "
                            "so each must use the same runtime DLL: use MultiThreaded"
                            "$<$<CONFIG:Debug>:Debug>DLL, or -DRAPPORT_STATIC=ON for the static "
                            "runtime.")
    endif()
    foreach(flags CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                  CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if("${${flags}}" MATCHES "(^| )[/-]MTd?( |$)")
            message(FATAL_ERROR "rapport: a plugin build (RAPPORT_STATIC=OFF) needs the shared C++ "
                                "runtime, but ${flags} has /MT. std:: types cross the plugin "
                                "DLLs, so each must use the same runtime DLL: drop it, or use "
                                "-DRAPPORT_STATIC=ON for the static runtime.")
        endif()
    endforeach()
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()
# AddressSanitizer through everything built here (CI's asan jobs): a read or
# write of freed memory fails the program that did it. GCC and Clang
# -fsanitize=address; MSVC /fsanitize=address, whose runtime DLL (beside
# cl.exe) is copied beside the programs so they run without it on PATH.
option(RAPPORT_ASAN "Build with AddressSanitizer" OFF)
if(RAPPORT_ASAN)
    if(MSVC)
        # ASan's instrumentation takes win32_backend.cpp past the default
        # object format's section limit (C1128): /bigobj.
        add_compile_options(/fsanitize=address /Zi /bigobj)
        add_link_options(/DEBUG /INCREMENTAL:NO)
        # /RTC (Debug's default) can't be combined with /fsanitize=address.
        foreach(flags CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG)
            string(REGEX REPLACE "/RTC[1csu]*" "" ${flags} "${${flags}}")
        endforeach()
        get_filename_component(rapport_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(rapport_asan_dll "${rapport_cl_dir}/clang_rt.asan_dynamic-x86_64.dll")
        if(NOT EXISTS "${rapport_asan_dll}")
            message(FATAL_ERROR "RAPPORT_ASAN: no ASan runtime at ${rapport_asan_dll}")
        endif()
        if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
            if(CMAKE_CONFIGURATION_TYPES)
                foreach(config ${CMAKE_CONFIGURATION_TYPES})
                    file(COPY "${rapport_asan_dll}" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
                endforeach()
            else()
                file(COPY "${rapport_asan_dll}" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
            endif()
        endif()
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

add_subdirectory(core)

option(RAPPORT_SHELL "Build the file-manager extension libraries" ON)
if(RAPPORT_SHELL)
    add_subdirectory(shell)
endif()

# Backends skip themselves (with a status message) when their toolkit is
# missing; set RAPPORT_REQUIRE_BACKENDS to make that an error instead.
foreach(backend null gtk3 gtk4 qt6 win32)
    string(TOUPPER ${backend} upper)
    option(RAPPORT_BACKEND_${upper} "Build the ${backend} backend" ON)
    if(RAPPORT_BACKEND_${upper})
        add_subdirectory(backends/${backend})
    endif()
endforeach()

# One program can hold only one GTK: gtk3 and gtk4 both bring in libgtk
# (and GTK's own type names would clash), so a static build may link one.
if(RAPPORT_STATIC AND TARGET rapport_gtk3 AND TARGET rapport_gtk4)
    message(FATAL_ERROR "RAPPORT_STATIC links backends into one program, and GTK 3 and GTK 4 "
                        "can't coexist there. Pick one: -DRAPPORT_BACKEND_GTK3=OFF or "
                        "-DRAPPORT_BACKEND_GTK4=OFF.")
endif()

if(RAPPORT_EXAMPLES)
    add_subdirectory(examples)
endif()
if(RAPPORT_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
