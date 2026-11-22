# rapport_require_shared_runtime(<target>): a shared rapport library (the
# core, a backend plugin) built with MSVC must use the shared C++ runtime.
# std:: types (std::function, std::string, std::vector, ...) cross between the
# app, the core and each backend DLL, one module freeing what another
# allocated, so they must all use one runtime DLL (/MD). The static runtime
# (/MT) is for RAPPORT_STATIC builds, one executable with one runtime (#136).
#
# What guarantees it is the built module: once it's linked, a post-build step
# (RapportCheckRuntime.cmake, through the toolset's dumpbin) fails the build
# unless it imports the shared runtime. That holds however its flags were set
# (generator expressions, an interface library's options, a parent's override
# after add_subdirectory). And the linker refuses objects built for different
# runtimes in one module: LNK2038 where they have CRT headers, and a default
# library conflict (LNK4098, made an error here) where they don't.
#
# Before that, configure stops early, with a clearer message, for the
# documented ways of asking for the static runtime: the target's runtime, as
# it is once its directory is processed, must be unset (it's then given
# MultiThreaded$<$<CONFIG:Debug>:Debug>DLL) or exactly one of the three below;
# and /MT or /MTd in the C++ flags or the target's own compile options is
# refused. That's a convenience, not the guarantee.
set(RAPPORT_CHECK_RUNTIME_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/RapportCheckRuntime.cmake)

function(rapport_require_shared_runtime target)
    if(NOT MSVC)
        return()
    endif()
    # A deferred call's arguments are expanded when it runs: the name's
    # written into it now.
    cmake_language(EVAL CODE "cmake_language(DEFER CALL rapport_check_shared_runtime [[${target}]])")

    # dumpbin is beside the toolset's link.exe and cl.exe.
    get_filename_component(linker_dir "${CMAKE_LINKER}" DIRECTORY)
    get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(RAPPORT_DUMPBIN dumpbin HINTS "${linker_dir}" "${compiler_dir}")
    if(NOT RAPPORT_DUMPBIN)
        message(FATAL_ERROR "rapport: no dumpbin beside ${CMAKE_LINKER} or ${CMAKE_CXX_COMPILER}, "
                            "to check that ${target} imports the shared C++ runtime (#136). Set "
                            "RAPPORT_DUMPBIN to the toolset's dumpbin.exe.")
    endif()
    # A default library conflict (LNK4098: an object, or a third-party
    # static library, built for the static runtime with no CRT headers to
    # trip LNK2038) is an error here: the linker could otherwise take CRT
    # pieces from either runtime. link.exe's own option; lld-link's differs.
    if(NOT CMAKE_LINKER MATCHES "lld")
        target_link_options(${target} PRIVATE /WX:4098)
    endif()
    get_property(script GLOBAL PROPERTY rapport_check_runtime_script)
    if(NOT script)
        set(script "${RAPPORT_CHECK_RUNTIME_SCRIPT}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DDUMPBIN=${RAPPORT_DUMPBIN}" "-DFILE=$<TARGET_FILE:${target}>"
                -P "${script}"
        COMMENT "Checking that ${target} imports the shared C++ runtime"
        VERBATIM)
endfunction()
# The script's path, wherever the function's called from.
set_property(GLOBAL PROPERTY rapport_check_runtime_script "${RAPPORT_CHECK_RUNTIME_SCRIPT}")

function(rapport_check_shared_runtime target)
    set(why "std:: types cross the plugin DLLs, so each must use the same runtime DLL (#136)")
    set(rapport_shared_runtimes MultiThreadedDLL MultiThreadedDebugDLL
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    get_target_property(runtime ${target} MSVC_RUNTIME_LIBRARY)
    if(runtime)
        if(NOT runtime IN_LIST rapport_shared_runtimes)
            message(FATAL_ERROR "rapport: ${target}, a shared rapport library, needs the shared C++ "
                                "runtime, but its MSVC runtime library is '${runtime}': ${why}. "
                                "Leave CMAKE_MSVC_RUNTIME_LIBRARY unset, or use one of "
                                "MultiThreadedDLL, MultiThreadedDebugDLL or "
                                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL; for the static runtime, "
                                "-DRAPPORT_STATIC=ON.")
        endif()
    else()
        set_target_properties(${target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    get_target_property(options ${target} COMPILE_OPTIONS)
    if(NOT options)
        set(options "")
    endif()
    foreach(flags CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                  CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL options)
        string(REPLACE ";" " " text "${${flags}}")
        if(text MATCHES "(^| )[/-]MTd?( |$)")
            message(FATAL_ERROR "rapport: ${target}, a shared rapport library, needs the shared C++ "
                                "runtime, but ${flags} has /MT: ${why}. Drop it, or use "
                                "-DRAPPORT_STATIC=ON for the static runtime.")
        endif()
    endforeach()
endfunction()
