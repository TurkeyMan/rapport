# Package: librapport (runtime) + librapport-dev (headers, CMake config).
# Links no toolkit.
cmake_minimum_required(VERSION 3.25)
project(rapport-core VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(cmake/RapportShaders.cmake)

add_library(rapport_headers INTERFACE)
add_library(rapport::headers ALIAS rapport_headers)
target_include_directories(rapport_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(rapport_headers INTERFACE cxx_std_23)
# rapport's strings are UTF-8, in its sources and in its users': MSVC would
# read them in the ANSI code page. And __cplusplus as the standard says.
target_compile_options(rapport_headers INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /Zc:__cplusplus>)

if(RAPPORT_STATIC)
    add_library(rapport STATIC src/rapport.cpp src/sys.cpp src/http.cpp src/watch.cpp src/image.cpp src/i18n.cpp src/gpu.cpp)
else()
    add_library(rapport SHARED src/rapport.cpp src/sys.cpp src/http.cpp src/watch.cpp src/image.cpp src/i18n.cpp src/gpu.cpp)
endif()
add_library(rapport::rapport ALIAS rapport)
target_link_libraries(rapport PUBLIC rapport_headers PRIVATE ${CMAKE_DL_LIBS})
if(WIN32)
    # rap::gpu on Direct3D 11 (d3dcompiler is loaded at run time, for HLSL source).
    target_sources(rapport PRIVATE src/gpu_d3d11.cpp)
    target_link_libraries(rapport PRIVATE shell32 ole32 uuid winhttp windowscodecs d3d11 dxgi)
endif()
# rap::gpu on Vulkan elsewhere: only the headers are needed, the loader is
# opened at run time.
if(NOT WIN32)
    find_path(RAPPORT_VULKAN_INCLUDE vulkan/vulkan.h PATHS ENV PKG_CONFIG_SYSROOT_DIR)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RAPPORT_VULKAN QUIET vulkan)
    endif()
    if(RAPPORT_VULKAN_FOUND OR RAPPORT_VULKAN_INCLUDE)
        target_sources(rapport PRIVATE src/gpu_vulkan.cpp)
        target_compile_definitions(rapport PRIVATE RAPPORT_GPU_VULKAN=1)
        target_include_directories(rapport PRIVATE ${RAPPORT_VULKAN_INCLUDE_DIRS})
        message(STATUS "rapport: GPU: Vulkan")
    else()
        message(STATUS "rapport: GPU: none (no Vulkan headers)")
    endif()
endif()

target_compile_definitions(rapport PRIVATE
    RAPPORT_INSTALL_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/rapport")
set_target_properties(rapport PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET default
    # The core's whole interface is exported, as GCC does by default; MSVC
    # exports nothing from a DLL unless told.
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

install(TARGETS rapport EXPORT rapportTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT core NAMELINK_COMPONENT dev)
install(TARGETS rapport_headers EXPORT rapportTargets)
install(DIRECTORY include/rapport DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev)
install(EXPORT rapportTargets NAMESPACE rapport::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rapport COMPONENT dev)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/rapportConfig.cmake
    "include(\${CMAKE_CURRENT_LIST_DIR}/rapportTargets.cmake)\n"
    "include(\${CMAKE_CURRENT_LIST_DIR}/RapportShaders.cmake)\n"
    "set(RAPPORT_PLUGIN_DIR \"${CMAKE_INSTALL_LIBDIR}/rapport\")\n")
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/rapportConfigVersion.cmake
    COMPATIBILITY ExactVersion)
install(FILES cmake/RapportShaders.cmake cmake/RapportShaderEmbed.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rapport COMPONENT dev)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rapportConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/rapportConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rapport COMPONENT dev)
