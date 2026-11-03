# WinUI 3 spike: can rapport's CMake and MSVC build a WinUI 3 window, with no
# MSBuild, no vcxproj and no XAML compiler? Not part of rapport's build; see
# NOTES.md for what it found.
#
# From a VS 2022 x64 developer prompt, with the packages fetched by
# fetch_packages.ps1:
#   cmake -G Ninja -S spikes/winui -B build-winui -DCMAKE_BUILD_TYPE=Release
#         -DWINAPPSDK_PACKAGES=C:/somewhere/packages
#   cmake --build build-winui
cmake_minimum_required(VERSION 3.25)
project(rapport-winui-spike LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "The WinUI spike needs MSVC (C++/WinRT, the Windows App SDK's import libraries)")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(WINAPPSDK_PACKAGES "" CACHE PATH "Folder of NuGet packages from fetch_packages.ps1")
set(WINAPPSDK_VERSION "2.6.4" CACHE STRING "Microsoft.WindowsAppSDK version")
set(CPPWINRT_VERSION "3.0.260818.1" CACHE STRING "Microsoft.Windows.CppWinRT version")
# Self-contained: the runtime's DLLs are copied beside the exe and found
# through its manifest (registration-free WinRT), so nothing is installed.
# Off: framework-dependent, found at run time by the bootstrapper, which needs
# the Windows App Runtime's framework package installed.
option(WINUI_SELF_CONTAINED "Carry the Windows App SDK runtime beside the exe" ON)

if(NOT IS_DIRECTORY "${WINAPPSDK_PACKAGES}")
    message(FATAL_ERROR "Set WINAPPSDK_PACKAGES to the folder fetch_packages.ps1 filled")
endif()
set(sdk_version "$ENV{WindowsSDKVersion}")
string(REGEX REPLACE "[\\/]$" "" sdk_version "${sdk_version}")
if(sdk_version STREQUAL "")
    message(FATAL_ERROR "Run from a VS developer prompt (vcvars64.bat): WindowsSDKVersion isn't set")
endif()

macro(package var name)
    set(${var} "${WINAPPSDK_PACKAGES}/${name}")
    if(NOT IS_DIRECTORY "${${var}}")
        message(FATAL_ERROR "Missing package ${name} in ${WINAPPSDK_PACKAGES}")
    endif()
endmacro()
package(cppwinrt Microsoft.Windows.CppWinRT.${CPPWINRT_VERSION})
package(foundation Microsoft.WindowsAppSDK.Foundation.${WINAPPSDK_VERSION})
package(interactive Microsoft.WindowsAppSDK.InteractiveExperiences.${WINAPPSDK_VERSION})
package(winui Microsoft.WindowsAppSDK.WinUI.${WINAPPSDK_VERSION})
package(dwrite Microsoft.WindowsAppSDK.DWrite.${WINAPPSDK_VERSION})
package(runtime Microsoft.WindowsAppSDK.Runtime.${WINAPPSDK_VERSION})
file(GLOB webview2 LIST_DIRECTORIES true "${WINAPPSDK_PACKAGES}/Microsoft.Web.WebView2.*")

# The C++/WinRT projection, generated once: the Windows SDK's metadata and the
# Windows App SDK's. WebView2's is only referenced (Microsoft.UI.Xaml names it).
set(projection "${CMAKE_BINARY_DIR}/projection")
if(NOT EXISTS "${projection}/winrt/base.h")
    file(GLOB winmds
        "${foundation}/metadata/*.winmd"
        "${interactive}/metadata/10.0.18362.0/*.winmd"
        "${winui}/metadata/*.winmd")
    set(args -input ${sdk_version})
    foreach(winmd IN LISTS winmds)
        list(APPEND args -input "${winmd}")
    endforeach()
    # -base: its own base.h, not the older one in the Windows SDK's include path.
    list(APPEND args -input "${webview2}/lib/Microsoft.Web.WebView2.Core.winmd" -base -output "${projection}")
    message(STATUS "Generating the C++/WinRT projection (once)")
    execute_process(COMMAND "${cppwinrt}/bin/cppwinrt.exe" ${args} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "cppwinrt failed: ${result}")
    endif()
endif()

add_executable(winui_spike WIN32 main.cpp app.manifest)
target_include_directories(winui_spike PRIVATE
    "${projection}" "${foundation}/include" "${interactive}/include" "${winui}/include" "${runtime}/include")
target_compile_definitions(winui_spike PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(winui_spike PRIVATE /bigobj /permissive- /Zc:__cplusplus /EHsc /utf-8)
target_link_libraries(winui_spike PRIVATE WindowsApp d3d11 dxgi wer
    "${foundation}/lib/native/x64/Microsoft.WindowsAppRuntime.lib")

# Each runtime component's DLLs, resources and metadata, as the Windows App
# SDK's MSBuild targets copy them for a self-contained app.
set(components "${foundation}" "${interactive}" "${winui}" "${dwrite}")

if(WINUI_SELF_CONTAINED)
    target_compile_definitions(winui_spike PRIVATE WINUI_SPIKE_SELF_CONTAINED=1)
    # The activatable classes each component's DLLs serve, from the fragment
    # its framework package would have registered, as a manifest the linker
    # merges with app.manifest (what the MSBuild task
    # GenerateAppManifestFromAppxFragments writes).
    set(manifest "<?xml version='1.0' encoding='utf-8' standalone='yes'?>\n<assembly manifestVersion='1.0' xmlns:asmv3='urn:schemas-microsoft-com:asm.v3' xmlns:winrtv1='urn:schemas-microsoft-com:winrt.v1' xmlns='urn:schemas-microsoft-com:asm.v1'>\n")
    foreach(component IN LISTS components)
        set(fragment "${component}/runtimes-framework/package.appxfragment")
        if(NOT EXISTS "${fragment}")
            continue()
        endif()
        file(READ "${fragment}" xml)
        string(REPLACE ";" "" xml "${xml}")
        string(REPLACE "</InProcessServer>" ";" servers "${xml}")
        foreach(server IN LISTS servers)
            if(NOT server MATCHES "<InProcessServer>")
                continue()
            endif()
            string(REGEX MATCH "<Path>([^<]+)</Path>" _ "${server}")
            set(dll "${CMAKE_MATCH_1}")
            if(NOT EXISTS "${component}/runtimes-framework/win-x64/native/${dll}")
                continue()  # WebView2's, which comes from its own package
            endif()
            string(APPEND manifest "  <asmv3:file name='${dll}'>\n")
            string(REGEX MATCHALL "ActivatableClassId=\"[^\"]+\"" classes "${server}")
            foreach(class IN LISTS classes)
                string(REGEX REPLACE "ActivatableClassId=\"([^\"]+)\"" "\\1" class "${class}")
                string(APPEND manifest "    <winrtv1:activatableClass name='${class}' threadingModel='both'/>\n")
            endforeach()
            string(APPEND manifest "  </asmv3:file>\n")
        endforeach()
    endforeach()
    string(APPEND manifest "</assembly>\n")
    file(WRITE "${CMAKE_BINARY_DIR}/WindowsAppSDK.manifest" "${manifest}")
    target_sources(winui_spike PRIVATE "${CMAKE_BINARY_DIR}/WindowsAppSDK.manifest")

    foreach(component IN LISTS components)
        add_custom_command(TARGET winui_spike POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${component}/runtimes-framework/win-x64/native" "$<TARGET_FILE_DIR:winui_spike>")
    endforeach()
    # resources.pri: WinUI finds its themes (ms-appx:///Microsoft.UI.Xaml/...)
    # through the app's resource index, which must take in the runtime's.
    # Without it XamlControlsResources fails ("Cannot locate resource").
    set(pri_stage "${CMAKE_BINARY_DIR}/pri")
    file(GLOB runtime_pris "${foundation}/runtimes-framework/win-x64/native/*.pri"
                           "${interactive}/runtimes-framework/win-x64/native/*.pri"
                           "${winui}/runtimes-framework/win-x64/native/*.pri")
    file(COPY ${runtime_pris} DESTINATION "${pri_stage}")
    set(resfiles "")
    foreach(pri IN LISTS runtime_pris)
        get_filename_component(name "${pri}" NAME)
        string(APPEND resfiles "${name}
")
    endforeach()
    file(WRITE "${pri_stage}/pri.resfiles" "${resfiles}")
    find_program(MAKEPRI makepri HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/resources.pri"
        COMMAND "${MAKEPRI}" new /pr "${pri_stage}" /cf "${CMAKE_CURRENT_SOURCE_DIR}/priconfig.xml"
                /of "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/resources.pri" /in rapport.winui.spike /o
        DEPENDS priconfig.xml ${runtime_pris}
        COMMENT "Merging the runtime's resources into resources.pri")
    add_custom_target(winui_spike_pri ALL DEPENDS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/resources.pri")

    # And the metadata, which registration-free activation reads.
    file(GLOB winmds "${foundation}/metadata/*.winmd" "${interactive}/metadata/10.0.18362.0/*.winmd"
                     "${winui}/metadata/*.winmd")
    add_custom_command(TARGET winui_spike POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${winmds} "$<TARGET_FILE_DIR:winui_spike>")
else()
    # The bootstrapper finds the installed framework package and adds it to
    # the process's package graph.
    target_link_libraries(winui_spike PRIVATE
        "${foundation}/lib/native/x64/Microsoft.WindowsAppRuntime.Bootstrap.lib")
    add_custom_command(TARGET winui_spike POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${foundation}/runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll"
            "$<TARGET_FILE_DIR:winui_spike>")
endif()

# Runs a program on a desktop of its own, which nothing shows, and waits for
# it: for measuring start-up without a window on the screen.
add_executable(hidden_run hidden_run.cpp)
target_compile_definitions(hidden_run PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
