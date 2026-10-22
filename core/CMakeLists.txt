# Package: libanyui (runtime) + libanyui-dev (headers, CMake config).
# Links no toolkit.
cmake_minimum_required(VERSION 3.25)
project(anyui-core VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(anyui_headers INTERFACE)
add_library(anyui::headers ALIAS anyui_headers)
target_include_directories(anyui_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(anyui_headers INTERFACE cxx_std_23)

if(ANYUI_STATIC)
    add_library(anyui STATIC src/anyui.cpp src/sys.cpp)
else()
    add_library(anyui SHARED src/anyui.cpp src/sys.cpp)
endif()
add_library(anyui::anyui ALIAS anyui)
target_link_libraries(anyui PUBLIC anyui_headers PRIVATE ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(anyui PRIVATE shell32 ole32 uuid)
endif()
target_compile_definitions(anyui PRIVATE
    ANYUI_INSTALL_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/anyui")
set_target_properties(anyui PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET default)

install(TARGETS anyui EXPORT anyuiTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT core NAMELINK_COMPONENT dev)
install(TARGETS anyui_headers EXPORT anyuiTargets)
install(DIRECTORY include/anyui DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev)
install(EXPORT anyuiTargets NAMESPACE anyui::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anyui COMPONENT dev)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/anyuiConfig.cmake
    "include(\${CMAKE_CURRENT_LIST_DIR}/anyuiTargets.cmake)\n"
    "set(ANYUI_PLUGIN_DIR \"${CMAKE_INSTALL_LIBDIR}/anyui\")\n")
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/anyuiConfigVersion.cmake
    COMPATIBILITY ExactVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/anyuiConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/anyuiConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anyui COMPONENT dev)
