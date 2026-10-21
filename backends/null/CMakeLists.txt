# Not packaged: headless backend for tests and CI.
cmake_minimum_required(VERSION 3.25)
project(anyui-null LANGUAGES CXX)

if(NOT TARGET anyui::headers)
    find_package(anyui REQUIRED)
endif()

add_library(anyui_null MODULE null_backend.cpp)
target_link_libraries(anyui_null PRIVATE anyui::headers)
set_target_properties(anyui_null PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
