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
target_link_libraries(rapport_null PRIVATE rapport::headers)
set_target_properties(rapport_null PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
