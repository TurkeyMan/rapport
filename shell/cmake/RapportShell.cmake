# rapport_add_shell_extension(<target>
#     NAME <shown name> ID <reverse-dns id> [WINDOWS_CLSID {GUID}]
#     SOURCES <provider sources...>)
#
# Builds the app's provider (defined with RAPPORT_SHELL_PROVIDER) into:
#   <target>        the file manager's extension: on Windows a COM DLL for
#                   Explorer's context menu (register it with
#                   rap::shell::install); elsewhere nothing yet
#   <target>-probe  prints the provider's menu for some paths, for testing
# Link the provider's own dependencies to <target>_provider.
function(rapport_add_shell_extension target)
    cmake_parse_arguments(ARG "" "NAME;ID;WINDOWS_CLSID" "SOURCES" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_ID OR NOT ARG_SOURCES)
        message(FATAL_ERROR "rapport_add_shell_extension(${target}): NAME, ID and SOURCES are required")
    endif()
    if(WIN32 AND NOT ARG_WINDOWS_CLSID MATCHES "^{[0-9A-Fa-f-]+}$")
        message(FATAL_ERROR "rapport_add_shell_extension(${target}): WINDOWS_CLSID {GUID} is required on Windows")
    endif()

    set(info ${CMAKE_CURRENT_BINARY_DIR}/${target}_info.cpp)
    file(CONFIGURE OUTPUT ${info} @ONLY CONTENT
        "#include <rapport/shell.h>\nconst rap::shell::extension_info rap::shell::this_extension{\"@ARG_NAME@\", \"@ARG_ID@\", \"@ARG_WINDOWS_CLSID@\"};\n")

    add_library(${target}_provider STATIC ${ARG_SOURCES} ${info})
    target_link_libraries(${target}_provider PUBLIC rapport::shell)
    set_target_properties(${target}_provider PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_executable(${target}-probe $<TARGET_OBJECTS:rapport_shell_probe>)
    target_link_libraries(${target}-probe PRIVATE ${target}_provider)

    if(WIN32)
        add_library(${target} MODULE ${RAPPORT_SHELL_EXPLORER_DEF})
        # The provider's objects whole: nothing in the module refers to them.
        target_link_libraries(${target} PRIVATE rapport::shell_explorer
                              $<LINK_LIBRARY:WHOLE_ARCHIVE,${target}_provider>)
        set_target_properties(${target} PROPERTIES PREFIX "")
        # Inside explorer.exe: no runtime DLLs of ours to load or clash.
        rapport_shell_static_runtime(${target})
        rapport_shell_static_runtime(${target}-probe)
    endif()
endfunction()

# Links the C++ runtime into <target>, so it needs no runtime DLLs.
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
