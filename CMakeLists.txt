# anyui umbrella build: core plus every backend whose toolkit is found.
#
# The tree mirrors packaging. Each directory is one package and also
# builds standalone against an installed anyui (distro split builds):
#   core/             libanyui, libanyui-dev  (Recommends the backends)
#   backends/gtk3/    libanyui-gtk3           (Depends: libanyui, GTK 3)
#   backends/gtk4/    libanyui-gtk4           (Depends: libanyui, GTK 4)
#   backends/null/    not packaged; headless backend for tests
#   backends/gtk-common/  shared source compiled into both GTK backends
cmake_minimum_required(VERSION 3.25)
project(anyui VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

option(ANYUI_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
# Static: the core and each backend become static libraries
# (anyui::anyui, anyui::backend_<name>) to link into one executable, which
# registers its backends with ANYUI_BUILTIN_BACKEND. Default: shared core,
# backends as dlopened plugins.
option(ANYUI_STATIC "Build static libraries instead of a shared core and plugins" OFF)

add_subdirectory(core)

# Backends skip themselves (with a status message) when their toolkit is
# missing; set ANYUI_REQUIRE_BACKENDS to make that an error instead.
foreach(backend null gtk3 gtk4)
    string(TOUPPER ${backend} upper)
    option(ANYUI_BACKEND_${upper} "Build the ${backend} backend" ON)
    if(ANYUI_BACKEND_${upper})
        add_subdirectory(backends/${backend})
    endif()
endforeach()

# One program can hold only one GTK: gtk3 and gtk4 both bring in libgtk
# (and GTK's own type names would clash), so a static build may link one.
if(ANYUI_STATIC AND TARGET anyui_gtk3 AND TARGET anyui_gtk4)
    message(FATAL_ERROR "ANYUI_STATIC links backends into one program, and GTK 3 and GTK 4 "
                        "can't coexist there. Pick one: -DANYUI_BACKEND_GTK3=OFF or "
                        "-DANYUI_BACKEND_GTK4=OFF.")
endif()

if(ANYUI_EXAMPLES)
    add_subdirectory(examples)
endif()
