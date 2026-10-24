# Not packaged: headless backend for tests and CI.
cmake_minimum_required(VERSION 3.25)
project(rapport-null LANGUAGES CXX)

if(NOT TARGET rapport::headers)
    find_package(rapport REQUIRED)
endif()

if(RAPPORT_STATIC)
    add_library(rapport_null STATIC null_backend.cpp)
else()
    add_library(rapport_null MODULE null_backend.cpp)
endif()
add_library(rapport::backend_null ALIAS rapport_null)
# Linked into one executable, a backend needs the core after it on the link
# line (its strings, from rapport::i18n). A module finds it loaded already,
# except on Windows, where a DLL resolves every symbol when it's linked.
if(RAPPORT_STATIC OR WIN32)
    target_link_libraries(rapport_null PRIVATE rapport::rapport)
endif()
target_link_libraries(rapport_null PRIVATE rapport::headers)
set_target_properties(rapport_null PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
