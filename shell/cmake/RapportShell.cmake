# rapport_add_shell_extension(<target>
#     NAME <shown name> ID <reverse-dns id> [WINDOWS_CLSID {GUID}]
#     [RESOURCE_DIR <absolute dir>] SOURCES <provider sources...>)
#
# Builds the app's provider (defined with RAPPORT_SHELL_PROVIDER) into:
#   <target>        on Windows, a COM DLL for Explorer's context menu
#                   (register it with rap::shell::install)
#   <target>-<fm>   on Linux, an extension for each file manager whose SDK
#                   is installed (nautilus, nemo, caja, thunar, dolphin),
#                   installed into its extension directory as the install
#                   component <target>-<fm>; RAPPORT_SHELL_FILE_MANAGERS
#                   lists them. RESOURCE_DIR is where the app installs the
#                   menu's icons (rap::shell::resource_dir()).
#   <target>-probe  prints the provider's menu for some paths, for testing
# Link the provider's own dependencies to <target>_provider.
function(rapport_add_shell_extension target)
    cmake_parse_arguments(ARG "" "NAME;ID;WINDOWS_CLSID;RESOURCE_DIR" "SOURCES" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_ID OR NOT ARG_SOURCES)
        message(FATAL_ERROR "rapport_add_shell_extension(${target}): NAME, ID and SOURCES are required")
    endif()
    if(WIN32 AND NOT ARG_WINDOWS_CLSID MATCHES "^{[0-9A-Fa-f-]+}$")
        message(FATAL_ERROR "rapport_add_shell_extension(${target}): WINDOWS_CLSID {GUID} is required on Windows")
    endif()

    set(info ${CMAKE_CURRENT_BINARY_DIR}/${target}_info.cpp)
    file(CONFIGURE OUTPUT ${info} @ONLY CONTENT
        "#include <rapport/shell.h>\nconst rap::shell::extension_info rap::shell::this_extension{\"@ARG_NAME@\", \"@ARG_ID@\", \"@ARG_WINDOWS_CLSID@\", \"@ARG_RESOURCE_DIR@\"};\n")

    add_library(${target}_provider STATIC ${ARG_SOURCES})
    target_link_libraries(${target}_provider PUBLIC rapport::shell)
    if(MSVC)
        rapport_shell_static_runtime(${target}_provider)
    endif()
    set_target_properties(${target}_provider PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # The identity is each module's own source: Visual Studio skips linking a
    # module with no sources of its own.
    add_executable(${target}-probe $<TARGET_OBJECTS:rapport_shell_probe> ${info})
    target_link_libraries(${target}-probe PRIVATE ${target}_provider)

    if(WIN32)
        add_library(${target} MODULE ${info} ${RAPPORT_SHELL_EXPLORER_DEF})
        # The provider's objects whole: nothing in the module refers to them.
        target_link_libraries(${target} PRIVATE rapport::shell_explorer
                              $<LINK_LIBRARY:WHOLE_ARCHIVE,${target}_provider>)
        set_target_properties(${target} PROPERTIES PREFIX "")
        # Inside explorer.exe: no runtime DLLs of ours to load or clash.
        rapport_shell_static_runtime(${target})
        rapport_shell_static_runtime(${target}-probe)
    endif()

    foreach(fm IN LISTS RAPPORT_SHELL_FILE_MANAGERS)
        add_library(${target}-${fm} MODULE ${info})
        if(fm STREQUAL dolphin)
            # The plugin's metadata, which moc builds into its class.
            set(meta ${CMAKE_CURRENT_BINARY_DIR}/${target}-dolphin-meta)
            file(CONFIGURE OUTPUT ${meta}/rapport-shell-dolphin.json @ONLY CONTENT [=[
{
    "KPlugin": {
        "Id": "@ARG_ID@",
        "Name": "@ARG_NAME@",
        "MimeTypes": ["application/octet-stream", "inode/directory"]
    }
}
]=])
            # Found here as well: imported targets are seen only where found.
            find_package(KF6KIO REQUIRED)
            find_package(Qt6 REQUIRED COMPONENTS Widgets)
            target_sources(${target}-${fm} PRIVATE ${RAPPORT_SHELL_DOLPHIN_SOURCE})
            target_include_directories(${target}-${fm} PRIVATE ${meta})
            target_link_libraries(${target}-${fm} PRIVATE rapport_shell_linux KF6::KIOWidgets
                                  Qt6::Widgets)
            set_target_properties(${target}-${fm} PROPERTIES AUTOMOC ON)
        else()
            target_link_libraries(${target}-${fm} PRIVATE
                                  $<LINK_LIBRARY:WHOLE_ARCHIVE,rapport::shell_${fm}>)
        endif()
        # The provider whole: only the file manager calls in.
        target_link_libraries(${target}-${fm} PRIVATE
                              $<LINK_LIBRARY:WHOLE_ARCHIVE,${target}_provider>)
        target_link_options(${target}-${fm} PRIVATE
                            -Wl,--version-script=${RAPPORT_SHELL_EXTENSION_MAP} -Wl,--no-undefined)
        set_target_properties(${target}-${fm} PROPERTIES
            PREFIX "" CXX_VISIBILITY_PRESET hidden
            LINK_DEPENDS ${RAPPORT_SHELL_EXTENSION_MAP})
        install(TARGETS ${target}-${fm} LIBRARY DESTINATION ${RAPPORT_SHELL_DIR_${fm}}
                COMPONENT ${target}-${fm})
    endforeach()
endfunction()

# Links the C++ runtime into <target>, so it needs no runtime DLLs. With MSVC
# every object in a link must agree, so the shell libraries themselves use
# the static runtime, and so must an app that links rapport::shell (set
# CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreaded$<$<CONFIG:Debug>:Debug>).
function(rapport_shell_static_runtime target)
    if(MINGW)
        # libstdc++ and winpthread come in here, statically, ahead of g++'s
        # own -lpthread: -static-libstdc++ ends its libstdc++ with -Bdynamic,
        # which would take winpthread from its DLL. (-static doesn't survive
        # an earlier -Wl,-Bdynamic in the caller's flags.)
        target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
        target_link_libraries(${target} PRIVATE -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic)
    elseif(MSVC)
        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
