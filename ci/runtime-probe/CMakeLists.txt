# A shared library under rapport_require_shared_runtime, given the static
# runtime in ways configure can't see (#136): CI builds each case and expects
# the build to fail, at the post-build import check or at the link. "none"
# must build.
#   genex      /MT in a Release-only generator expression in its options
#   interface  /MT from an INTERFACE library it links
#   parent     its MSVC_RUNTIME_LIBRARY set by the parent after add_subdirectory
#   mixed      one /MT object linked into it (the linker's LNK2038)
cmake_minimum_required(VERSION 3.25)
project(rapport-runtime-probe LANGUAGES CXX)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../core/cmake/RapportRuntime.cmake)
set(RAPPORT_PROBE_CASE none CACHE STRING "none, genex, interface, parent or mixed")

if(RAPPORT_PROBE_CASE STREQUAL "parent")
    add_subdirectory(child)
    set_target_properties(probe PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
    return()
endif()
add_library(probe SHARED probe.cpp)
rapport_require_shared_runtime(probe)
if(RAPPORT_PROBE_CASE STREQUAL "genex")
    target_compile_options(probe PRIVATE "$<$<CONFIG:Release>:/MT>")
elseif(RAPPORT_PROBE_CASE STREQUAL "interface")
    add_library(static_runtime INTERFACE)
    target_compile_options(static_runtime INTERFACE /MT)
    target_link_libraries(probe PRIVATE static_runtime)
elseif(RAPPORT_PROBE_CASE STREQUAL "mixed")
    add_library(static_part OBJECT static_part.cpp)
    set_target_properties(static_part PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
    target_sources(probe PRIVATE $<TARGET_OBJECTS:static_part>)
elseif(NOT RAPPORT_PROBE_CASE STREQUAL "none")
    message(FATAL_ERROR "RAPPORT_PROBE_CASE: none, genex, interface, parent or mixed")
endif()
