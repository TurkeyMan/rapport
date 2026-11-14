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
# AddressSanitizer through everything built here (CI's asan jobs): a read or
# write of freed memory fails the program that did it. GCC and Clang
# -fsanitize=address; MSVC /fsanitize=address, whose runtime DLL (beside
# cl.exe) is copied beside the programs so they run without it on PATH.
option(RAPPORT_ASAN "Build with AddressSanitizer" OFF)
if(RAPPORT_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/DEBUG /INCREMENTAL:NO)
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
